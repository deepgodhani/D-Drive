#include "Shell.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <iomanip>
#include <indicators/progress_bar.hpp>
#include <indicators/cursor_control.hpp>

namespace fs = std::filesystem;
using namespace indicators;

// =============================================================================
// UTILITY FUNCTIONS (MOVED TO THE TOP)
// =============================================================================

// This function's full body must be defined before it is used because it returns 'auto'.
auto create_progress_callback(ProgressBar& bar) {
    auto start_time = std::chrono::high_resolution_clock::now();
    return [&](cpr::cpr_off_t total, cpr::cpr_off_t now, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
        if (total == 0) return true;
        auto now_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_time - start_time).count();
        float speed_mbps = (duration_ms > 500) ? (static_cast<float>(now) / (1024 * 1024)) / (static_cast<float>(duration_ms) / 1000.0f) : 0.0f;
        float percentage = (static_cast<float>(now) / static_cast<float>(total)) * 100.0f;
        bar.set_progress(static_cast<size_t>(percentage));

        std::stringstream postfix_ss;
        postfix_ss << std::to_string(now / (1024 * 1024)) << "/" << std::to_string(total / (1024 * 1024)) << " MB | ";
        postfix_ss << std::fixed << std::setprecision(2) << speed_mbps << " MB/s | ";
        if (speed_mbps > 0.01) {
            float eta_seconds = (static_cast<float>(total - now)) / (speed_mbps * 1024 * 1024);
            postfix_ss << "ETA: " << std::fixed << std::setprecision(1) << eta_seconds << "s";
        } else {
            postfix_ss << "ETA: inf";
        }
        bar.set_option(option::PostfixText{postfix_ss.str()});
        return true;
    };
}

std::string getOrCreateFolder(GDriveHandler& gdrive, const std::string& folderName, const std::string& parentId = "root") {
    std::string folderId = gdrive.findFileOrFolder(folderName, parentId);
    if (folderId.empty()) {
        std::cout << "Folder '" << folderName << "' not found, creating..." << std::endl;
        folderId = gdrive.createFolder(folderName, parentId);
    }
    return folderId;
}

int getPartNumber(const fs::path& path) {
    std::string filename = path.filename().string();
    size_t pos = filename.rfind(".part");
    if (pos != std::string::npos) {
        try {
            return std::stoi(filename.substr(pos + 5));
        } catch (...) {}
    }
    return -1;
}

void splitFile(const std::string& inputFilePath, const std::string& outputDir, long long chunkSize) {
    std::ifstream inputFile(inputFilePath, std::ios::binary);
    if (!inputFile) throw std::runtime_error("Cannot open input file for splitting: " + inputFilePath);
    
    std::vector<char> buffer(chunkSize);
    int chunkNumber = 1;
    while (inputFile) {
        inputFile.read(buffer.data(), chunkSize);
        std::streamsize bytesRead = inputFile.gcount();
        if (bytesRead > 0) {
            std::ofstream outputFile(
                fs::path(outputDir) / (fs::path(inputFilePath).filename().string() + ".part" + std::to_string(chunkNumber++)),
                std::ios::binary
            );
            outputFile.write(buffer.data(), bytesRead);
        }
    }
}

void mergeFiles(const std::string& inputDir, const std::string& outputFilePath) {
    std::vector<fs::path> chunkFiles;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file()) chunkFiles.push_back(entry.path());
    }
    std::sort(chunkFiles.begin(), chunkFiles.end(), [](const fs::path& a, const fs::path& b) {
        return getPartNumber(a) < getPartNumber(b);
    });

    std::ofstream outputFile(outputFilePath, std::ios::binary);
    if (!outputFile) throw std::runtime_error("Cannot open output file for merging: " + outputFilePath);

    std::vector<char> buffer(4 * 1024 * 1024);
    for (const auto& chunkPath : chunkFiles) {
        std::ifstream inputFile(chunkPath, std::ios::binary);
        while (inputFile) {
            inputFile.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = inputFile.gcount();
            if (bytesRead > 0) outputFile.write(buffer.data(), bytesRead);
        }
    }
}

// =============================================================================
// SHELL CLASS IMPLEMENTATION
// =============================================================================

Shell::Shell() : m_metadata_changed(false) {
    m_commands["add-account"] = {"Link a new Google Drive account.", [this](const auto& args){ this->addAccount(args); }};
    m_commands["upload"] = {"Uploads and splits a file.", [this](const auto& args){ this->uploadFile(args); }};
    m_commands["download"] = {"Downloads and merges a file.", [this](const auto& args){ this->downloadFile(args); }};
    m_commands["ls"] = {"List all managed files.", [this](const auto& args){ this->listFiles(args); }};
    m_commands["list-files"] = {"(Alias for ls)", [this](const auto& args){ this->listFiles(args); }};
    m_commands["list-accounts"] = {"List linked accounts and usage.", [this](const auto& args){ this->listAccounts(args); }};
    m_commands["help"] = {"Show this help message.", [this](const auto& args){ this->showHelp(args); }};
    
    m_creds_path = "data/credentials/credentials.json";
}

void Shell::initializeState() {
    const std::string metadataFilePath = "data/metadata.json";

    // Load metadata from local file
    if (fs::exists(metadataFilePath)) {
        std::ifstream metadataFile(metadataFilePath);
        if (metadataFile.is_open()) {
            m_metadata = json::parse(metadataFile);
            std::cout << "Loaded metadata from local storage." << std::endl;
        } else {
            throw std::runtime_error("Failed to open metadata file: " + metadataFilePath);
        }
    } else {
        std::cout << "No local metadata found. Initializing new metadata." << std::endl;
        m_metadata = {{"accounts", json::object()}, {"files", json::object()}};
    }

    // Load local accounts
    if (fs::exists("data/tokens")) {
        for (const auto& entry : fs::directory_iterator("data/tokens")) {
            if (entry.is_regular_file()) {
                m_local_accounts[entry.path().stem().string()] = entry.path().string();
            }
        }
    }

    // Ensure a primary account exists
    if (m_local_accounts.empty()) {
        std::cout << "No accounts found. Please add a primary account to begin." << std::endl;
        std::cout << "Use the command: add-account <email>" << std::endl;

        std::string line;
        while (true) {
            std::cout << "\ngdrive > ";
            if (!std::getline(std::cin, line) || line == "exit" || line == "quit") {
                throw std::runtime_error("Application cannot proceed without a primary account.");
            }

            auto args = parseCommand(line);
            if (args.empty() || args[0] != "add-account") {
                std::cerr << "Invalid command. Please use 'add-account <email>' to add a primary account." << std::endl;
                continue;
            }

            try {
                addAccount(args);
                break; // Exit the loop once the primary account is added
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
    }
}

void Shell::run() {
    initializeState();
    std::string line;
    std::cout << "\n--- GDrive Splitter Shell (v2.0) ---" << std::endl;
    showHelp({});

    while (true) {
        std::cout << "\ngdrive > ";
        if (!std::getline(std::cin, line) || line == "exit" || line == "quit") break;
        
        auto args = parseCommand(line);
        if (args.empty()) continue;

        auto const& command_name = args[0];
        auto const it = m_commands.find(command_name);

        if (it == m_commands.end()) {
            std::cerr << "Unknown command: '" << command_name << "'. Type 'help' for a list of commands." << std::endl;
        } else {
            try {
                it->second.handler(args);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
                // Clean up temp directories in case of error during upload/download
                if (fs::exists("./temp_chunks_upload")) fs::remove_all("./temp_chunks_upload");
                if (fs::exists("./temp_chunks_download")) fs::remove_all("./temp_chunks_download");
            }
        }
    }
    
    saveMetadataOnExit();
    std::cout << "Exiting." << std::endl;
}

void Shell::saveMetadataOnExit() {
    if (m_metadata_changed && m_primary_gdrive) {
        std::cout << "Saving updated metadata to primary drive..." << std::endl;
        std::string metadataFileId = m_primary_gdrive->findFileOrFolder("gdrive_splitter_metadata.json");
        if (metadataFileId.empty()) {
            m_primary_gdrive->uploadNewFile(m_metadata.dump(4), "gdrive_splitter_metadata.json");
        } else {
            m_primary_gdrive->updateFileContent(metadataFileId, m_metadata.dump(4));
        }
        std::cout << "Metadata saved successfully." << std::endl;
    }
}

std::vector<std::string> Shell::parseCommand(const std::string& input) {
    std::stringstream ss(input);
    std::string segment;
    std::vector<std::string> seglist;
    while (ss >> segment) seglist.push_back(segment);
    return seglist;
}

// --- COMMAND HANDLER IMPLEMENTATIONS ---

void Shell::showHelp(const std::vector<std::string>& args) {
    std::cout << "Available commands:" << std::endl;
    for (const auto& pair : m_commands) {
        if (pair.second.description.find("Alias") == std::string::npos) {
             std::cout << "  " << std::left << std::setw(20) << pair.first << pair.second.description << std::endl;
        }
    }
}

void Shell::addAccount(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::runtime_error("Usage: add-account <user_email>");
    std::string email = args[1];
    std::string tokenPath = "data/tokens/" + email + ".json";

    GDriveHandler gdrive(tokenPath, m_creds_path);
    gdrive.ensureAuthenticated();
    std::cout << "Successfully added account: " << email << std::endl;

    if (!m_metadata["accounts"].contains(email)) {
        m_metadata["accounts"][email] = {
            {"token_path", tokenPath},
            {"total_space", 15LL * 1024 * 1024 * 1024},
            {"used_space", 0}
        };
        m_metadata_changed = true;
        std::cout << "Account added to local metadata." << std::endl;
    }
}

void Shell::uploadFile(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::runtime_error("Usage: upload <path_to_file>");

    std::string localFilePath = args[1];
    std::string remoteFileName = fs::path(localFilePath).filename().string();
    if (m_metadata["files"].contains(remoteFileName)) {
        throw std::runtime_error("File with this name already exists in metadata. Please choose a different name or delete the existing one first.");
    }

    const std::string tempChunkDir = "./temp_chunks_upload";
    if (fs::exists(tempChunkDir)) fs::remove_all(tempChunkDir);
    fs::create_directories(tempChunkDir);

    splitFile(localFilePath, tempChunkDir, 50 * 1024 * 1024);

    std::vector<fs::path> chunks;
    for (const auto& entry : fs::directory_iterator(tempChunkDir)) chunks.push_back(entry.path());
    std::sort(chunks.begin(), chunks.end(), [](const fs::path& a, const fs::path& b) {
        return getPartNumber(a) < getPartNumber(b);
    });

    int part = 1;
    for (const auto& chunk_path : chunks) {
        bool uploaded = false;
        for (auto& acc : m_metadata["accounts"].items()) {
            if ((acc.value()["total_space"].get<long long>() - acc.value()["used_space"].get<long long>()) > fs::file_size(chunk_path)) {
                GDriveHandler gdrive(acc.value()["token_path"], m_creds_path);

                std::string fileId = gdrive.uploadChunk(chunk_path.string(), chunk_path.filename().string(), "root");

                m_metadata["files"][remoteFileName]["chunks"].push_back({
                    {"part", part}, {"account", acc.key()}, {"drive_file_id", fileId}
                });
                acc.value()["used_space"] = acc.value()["used_space"].get<long long>() + fs::file_size(chunk_path);
                uploaded = true;
                break;
            }
        }
        if (!uploaded) {
            throw std::runtime_error("Upload failed! Not enough space on any account for chunk " + std::to_string(part));
        }
        part++;
    }

    m_metadata["files"][remoteFileName]["total_size"] = fs::file_size(localFilePath);
    fs::remove_all(tempChunkDir);
    std::cout << "File upload finished." << std::endl;
    m_metadata_changed = true;
}


void Shell::downloadFile(const std::vector<std::string>& args) {
    if (args.size() < 3) throw std::runtime_error("Usage: download <remote_file_name> <save_as_path>");

    std::string remoteFileName = args[1];
    std::string savePath = args[2];
    if (!m_metadata["files"].contains(remoteFileName)) throw std::runtime_error("File not found in metadata.");

    const std::string tempChunkDir = "./temp_chunks_download";
    if (fs::exists(tempChunkDir)) fs::remove_all(tempChunkDir);
    fs::create_directories(tempChunkDir);

    int total_chunks = m_metadata["files"][remoteFileName]["chunks"].size();
    std::vector<std::thread> threads;

    for (const auto& chunk : m_metadata["files"][remoteFileName]["chunks"]) {
        threads.emplace_back([&, chunk]() {
            std::string account = chunk["account"];
            std::string file_id = chunk["drive_file_id"];
            std::string chunk_save_path = (fs::path(tempChunkDir) / (remoteFileName + ".part" + std::to_string(chunk["part"].get<int>()))).string();

            GDriveHandler gdrive(m_metadata["accounts"][account]["token_path"], m_creds_path);

            gdrive.downloadChunk(file_id, chunk_save_path);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    mergeFiles(tempChunkDir, savePath);
    fs::remove_all(tempChunkDir);
    std::cout << "File download completed! Saved to " << savePath << std::endl;
}

void Shell::listFiles(const std::vector<std::string>& args) {
    if (!m_primary_gdrive) throw std::runtime_error("No primary account set. Please add one first.");
    std::cout << "--- Managed Files ---" << std::endl;
    if (m_metadata.value("files", json::object()).empty()) {
        std::cout << "No files are currently managed." << std::endl;
    } else {
        for (const auto& file : m_metadata.value("files", json::object()).items()) {
            long long size_mb = file.value()["total_size"].get<long long>() / (1024 * 1024);
            std::cout << "- " << file.key() << " (" << size_mb << " MB)" << std::endl;
        }
    }
}

void Shell::listAccounts(const std::vector<std::string>& args) {
    std::cout << "--- Linked Accounts ---" << std::endl;
    if (m_metadata["accounts"].empty()) {
        std::cout << "No accounts are currently linked." << std::endl;
    } else {
        for (const auto& acc : m_metadata["accounts"].items()) {
            long long used_mb = acc.value()["used_space"].get<long long>() / (1024 * 1024);
            long long total_mb = acc.value()["total_space"].get<long long>() / (1024 * 1024);
            std::cout << "- " << acc.key() << " | " << used_mb << " / " << total_mb << " MB used." << std::endl;
        }
    }
}