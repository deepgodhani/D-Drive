#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <map>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <thread>

#include "gdrive_handler.h"
#include <indicators/progress_bar.hpp>
#include <indicators/cursor_control.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace indicators;

// --- All forward declarations and helper functions are correct and remain unchanged ---
void splitFile(const std::string&, const std::string&, long long);
void mergeFiles(const std::string&, const std::string&);
int getPartNumber(const fs::path&);
void printUsage(const char*);
std::string getOrCreateFolder(GDriveHandler& gdrive, const std::string& folderName, const std::string& parentId = "root") {
    std::string folderId = gdrive.findFileOrFolder(folderName, parentId);
    if (folderId.empty()) {
        std::cout << "Folder '" << folderName << "' not found, creating..." << std::endl;
        folderId = gdrive.createFolder(folderName, parentId);
    }
    return folderId;
}

// --- CORRECTED PROGRESS CALLBACK ---
auto create_progress_callback(ProgressBar& bar) {
    auto start_time = std::chrono::high_resolution_clock::now();
    // The signature now includes an intptr_t userdata, which we don't need but must accept
    return [&](cpr::cpr_off_t total, cpr::cpr_off_t now, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
        if (total == 0) return true;
        auto now_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_time - start_time).count();
        float speed_mbps = (duration_ms > 500) ? (static_cast<float>(now) / (1024*1024)) / (static_cast<float>(duration_ms) / 1000.0f) : 0.0f;
        float percentage = (static_cast<float>(now) / static_cast<float>(total)) * 100.0f;
        bar.set_progress(static_cast<size_t>(percentage));
        
        std::stringstream postfix_ss;
        postfix_ss << std::to_string(now / (1024*1024)) << "/" << std::to_string(total / (1024*1024)) << " MB | ";
        postfix_ss << std::fixed << std::setprecision(2) << speed_mbps << " MB/s | ";
        if (speed_mbps > 0.01) {
            float eta_seconds = (static_cast<float>(total - now)) / (speed_mbps * 1024 * 1024);
            postfix_ss << "ETA: " << std::fixed << std::setprecision(1) << eta_seconds << "s";
        } else {
            postfix_ss << "ETA: inf";
        }
        
        // This is the key change for the indicators library
        bar.set_option(option::PostfixText{postfix_ss.str()});

        return true;
    };
}


int main(int argc, char* argv[]) {
    if (argc < 2) { printUsage(argv[0]); return 1; }
    
    const std::string credsPath = "data/credentials/credentials.json";
    const std::string METADATA_FILENAME = "gdrive_splitter_metadata.json";
    const std::string CHUNK_FOLDER_NAME = "GDriveSplitter_Chunks";
    std::string operation = argv[1];

    try {
        if (operation == "add-account") {
            if (argc < 3) throw std::runtime_error("Missing user email.");
            std::string email = argv[2];
            std::string tokenPath = "data/tokens/" + email + ".json";
            fs::create_directories("data/tokens");
            GDriveHandler gdrive(tokenPath, credsPath);
            gdrive.ensureAuthenticated();
            std::cout << "Successfully added account: " << email << std::endl;
            int count = 0; 
            if (fs::exists("data/tokens")) {
                for (const auto& entry : fs::directory_iterator("data/tokens")) {
                    if (entry.is_regular_file()) count++;
                }
            }
            if (count == 1) std::cout << "This is the PRIMARY account for storing metadata." << std::endl;
            return 0;
        }

        std::map<std::string, std::string> local_accounts;
        if (fs::exists("data/tokens")) {
            for (const auto& entry : fs::directory_iterator("data/tokens")) {
                if(entry.is_regular_file()) local_accounts[entry.path().stem().string()] = entry.path().string();
            }
        }
        if (local_accounts.empty()) throw std::runtime_error("No accounts found. Use 'add-account'.");

        std::string primary_email = local_accounts.begin()->first;
        std::string primary_token_path = local_accounts.begin()->second;
        std::cout << "Using primary account '" << primary_email << "' to sync metadata." << std::endl;
        GDriveHandler primary_gdrive(primary_token_path, credsPath);
        
        std::string metadataFileId = primary_gdrive.findFileOrFolder(METADATA_FILENAME);
        json metadata;
        if (metadataFileId.empty()) {
            std::cout << "No remote metadata found. Initializing." << std::endl;
            metadata = { {"accounts", json::object()}, {"files", json::object()} };
        } else {
            std::cout << "Downloading remote metadata..." << std::endl;
            metadata = json::parse(primary_gdrive.downloadFileContent(metadataFileId));
        }

        bool metadata_changed = false;
        for(const auto& pair : local_accounts) {
            if (!metadata["accounts"].contains(pair.first)) {
                std::cout << "New local account '" << pair.first << "' detected. Adding to metadata." << std::endl;
                metadata["accounts"][pair.first] = {{"token_path", pair.second}, {"total_space", 15LL*1024*1024*1024}, {"used_space", 0}};
                metadata_changed = true;
            }
        }

        if (operation == "upload") {
            if (argc < 3) throw std::runtime_error("Missing file path.");
            std::string localFilePath = argv[2];
            std::string remoteFileName = fs::path(localFilePath).filename().string();
            if (metadata["files"].contains(remoteFileName)) throw std::runtime_error("File exists in metadata.");

            const std::string tempChunkDir = "./temp_chunks_upload";
            if (fs::exists(tempChunkDir)) fs::remove_all(tempChunkDir);
            fs::create_directories(tempChunkDir);
            splitFile(localFilePath, tempChunkDir, 50 * 1024 * 1024);

            std::vector<fs::path> chunks;
            for(const auto& entry : fs::directory_iterator(tempChunkDir)) chunks.push_back(entry.path());
            std::sort(chunks.begin(), chunks.end(), [](const fs::path& a, const fs::path& b){ return getPartNumber(a) < getPartNumber(b); });

            int part = 1;
            for (const auto& chunk_path : chunks) {
                bool uploaded = false;
                for (auto& acc : metadata["accounts"].items()) {
                    if ((acc.value()["total_space"].get<long long>() - acc.value()["used_space"].get<long long>()) > fs::file_size(chunk_path)) {
                        GDriveHandler gdrive(acc.value()["token_path"], credsPath);
                        std::string chunkFolderId = getOrCreateFolder(gdrive, CHUNK_FOLDER_NAME);
                        
                        show_console_cursor(false);
                        std::string label = "Chunk " + std::to_string(part) + "/" + std::to_string(chunks.size());
                        ProgressBar bar{
                            option::BarWidth{40}, option::Start{"["}, option::Fill{"="}, option::Lead{">"}, option::Remainder{" "}, option::End{"]"},
                            option::PrefixText{label},
                            option::ShowElapsedTime{true}
                        };
                        auto callback = create_progress_callback(bar);
                        
                        std::string fileId = gdrive.uploadChunk(chunk_path.string(), chunk_path.filename().string(), chunkFolderId, callback);
                        
                        if (!bar.is_completed()) { bar.set_progress(100); bar.mark_as_completed(); }
                        show_console_cursor(true);
                        std::cout << " Done." << std::endl;

                        metadata["files"][remoteFileName]["chunks"].push_back({{"part", part}, {"account", acc.key()}, {"drive_file_id", fileId}});
                        acc.value()["used_space"] = acc.value()["used_space"].get<long long>() + fs::file_size(chunk_path);
                        uploaded = true;
                        break;
                    }
                }
                if (!uploaded) { fs::remove_all(tempChunkDir); throw std::runtime_error("Upload failed! Not enough space for chunk " + std::to_string(part)); }
                part++;
            }
            metadata["files"][remoteFileName]["total_size"] = fs::file_size(localFilePath);
            fs::remove_all(tempChunkDir);
            std::cout << "File upload finished." << std::endl;
            metadata_changed = true;

        } else if (operation == "download") {
            if (argc < 4) throw std::runtime_error("Missing remote file name or save path.");
            std::string remoteFileName = argv[2];
            std::string savePath = argv[3];
            if (!metadata["files"].contains(remoteFileName)) throw std::runtime_error("File not found in metadata.");

            const std::string tempChunkDir = "./temp_chunks_download";
            if (fs::exists(tempChunkDir)) fs::remove_all(tempChunkDir);
            fs::create_directories(tempChunkDir);

            int total_chunks = metadata["files"][remoteFileName]["chunks"].size();
            int current_chunk_num = 1;
            for (const auto& chunk : metadata["files"][remoteFileName]["chunks"]) {
                std::string account = chunk["account"];
                std::string file_id = chunk["drive_file_id"];
                std::string chunk_save_path = (fs::path(tempChunkDir) / (remoteFileName + ".part" + std::to_string(chunk["part"].get<int>()))).string();
                
                GDriveHandler gdrive(metadata["accounts"][account]["token_path"], credsPath);
                
                show_console_cursor(false);
                std::string label = "Chunk " + std::to_string(current_chunk_num) + "/" + std::to_string(total_chunks);
                ProgressBar bar{
                    option::BarWidth{40}, option::Start{"["}, option::Fill{"="}, option::Lead{">"}, option::Remainder{" "}, option::End{"]"},
                    option::PrefixText{label},
                    option::ShowElapsedTime{true}
                };
                auto callback = create_progress_callback(bar);

                gdrive.downloadChunk(file_id, chunk_save_path, callback);
                
                if (!bar.is_completed()) { bar.set_progress(100); bar.mark_as_completed(); }
                show_console_cursor(true);
                std::cout << " Done." << std::endl;
                current_chunk_num++;
            }
            mergeFiles(tempChunkDir, savePath);
            fs::remove_all(tempChunkDir);
            std::cout << "File download completed! Saved to " << savePath << std::endl;

        } else if (operation == "list-files") {
            std::cout << "--- Managed Files ---" << std::endl;
            if (metadata.value("files", json::object()).empty()) {
                std::cout << "No files are currently managed." << std::endl;
            }
            for (const auto& file : metadata.value("files", json::object()).items()) {
                std::cout << "- " << file.key() << " (" << file.value()["total_size"].get<long long>() / (1024 * 1024) << " MB)" << std::endl;
            }
        } else if (operation == "list-accounts") {
            std::cout << "--- Linked Accounts ---" << std::endl;
            for (const auto& acc : metadata["accounts"].items()) {
                long long used = acc.value()["used_space"];
                long long total = acc.value()["total_space"];
                std::cout << "- " << acc.key() << " - " << used / (1024 * 1024) << " / " << total / (1024 * 1024) << " MB used." << std::endl;
            }
        }
        else { printUsage(argv[0]); }

        if (metadata_changed) {
            std::cout << "Saving updated metadata to primary drive..." << std::endl;
            if (metadataFileId.empty()) {
                metadataFileId = primary_gdrive.uploadNewFile(metadata.dump(), METADATA_FILENAME);
            } else {
                primary_gdrive.updateFileContent(metadataFileId, metadata.dump());
            }
            std::cout << "Metadata saved successfully." << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "\nAn error occurred: " << e.what() << std::endl;
        show_console_cursor(true);
        if (fs::exists("./temp_chunks_upload")) fs::remove_all("./temp_chunks_upload");
        if (fs::exists("./temp_chunks_download")) fs::remove_all("./temp_chunks_download");
        return 1;
    }
    return 0;
}

// --- All utility functions (printUsage, splitFile, mergeFiles, getPartNumber) are correct and remain unchanged ---
void printUsage(const char* progName) {
    std::cerr << "\n--- GDrive Splitter (Cloud Version) ---\n"
              << "Usage:\n"
              << "  " << progName << " add-account <user_email>\n"
              << "  " << progName << " upload <path_to_file>\n"
              << "  " << progName << " download <remote_file_name> <save_as_path>\n"
              << "  " << progName << " list-files\n"
              << "  " << progName << " list-accounts\n"
              << std::endl;
}
void splitFile(const std::string& inputFilePath, const std::string& outputDir, long long chunkSize) {
    std::ifstream inputFile(inputFilePath, std::ios::binary);
    char* buffer = new char[chunkSize];
    int chunkNumber = 1;
    while (inputFile) {
        inputFile.read(buffer, chunkSize);
        if (inputFile.gcount() > 0) {
            std::ofstream outputFile(fs::path(outputDir) / (fs::path(inputFilePath).filename().string() + ".part" + std::to_string(chunkNumber++)), std::ios::binary);
            outputFile.write(buffer, inputFile.gcount());
        }
    }
    delete[] buffer;
}
void mergeFiles(const std::string& inputDir, const std::string& outputFilePath) {
    std::vector<fs::path> chunkFiles;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file()) chunkFiles.push_back(entry.path());
    }
    std::sort(chunkFiles.begin(), chunkFiles.end(), [](const fs::path& a, const fs::path& b) { return getPartNumber(a) < getPartNumber(b); });
    std::ofstream outputFile(outputFilePath, std::ios::binary);
    char* buffer = new char[4 * 1024 * 1024];
    for (const auto& chunkPath : chunkFiles) {
        std::ifstream inputFile(chunkPath, std::ios::binary);
        while (inputFile) {
            inputFile.read(buffer, 4 * 1024 * 1024);
            if (inputFile.gcount() > 0) outputFile.write(buffer, inputFile.gcount());
        }
    }
    delete[] buffer;
}
int getPartNumber(const fs::path& path) {
    std::string filename = path.filename().string();
    size_t pos = filename.rfind(".part");
    if (pos != std::string::npos) { try { return std::stoi(filename.substr(pos + 5)); } catch (...) {} }
    return -1;
}
