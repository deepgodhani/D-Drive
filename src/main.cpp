#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <numeric>
#include <fstream> 

#include "gdrive_handler.h"
#include "metadata_handler.h"

namespace fs = std::filesystem;

void splitFile(const std::string& inputFilePath, const std::string& outputDir, long long chunkSize);
void mergeFiles(const std::string& inputDir, const std::string& outputFilePath);
int getPartNumber(const fs::path& path);

void printUsage(const char* progName) {
    std::cerr << "\n--- GDrive Splitter ---"
              << "\nUsage:\n"
              << "  " << progName << " add-account <user_email>\n"
              << "    (Links a new Google account to the tool)\n\n"
              << "  " << progName << " upload <path_to_file>\n"
              << "    (Splits and uploads a large file across all linked accounts)\n\n"
              << "  " << progName << " download <remote_file_name> <save_as_path>\n"
              << "    (Downloads and merges a file from Google Drive)\n\n"
              << "  " << progName << " list-files\n"
              << "    (Lists files managed by the tool)\n\n"
              << "  " << progName << " list-accounts\n"
              << "    (Lists all linked Google accounts)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string credsPath = "data/credentials/credentials.json";
    const std::string metadataPath = "data/metadata.json";
    MetadataHandler metadata(metadataPath);

    std::string operation = argv[1];

    try {
        if (operation == "add-account") {
            if (argc < 3) throw std::runtime_error("Missing user email for add-account.");
            std::string email = argv[2];
            std::string tokenPath = "data/tokens/" + email + ".json";
            fs::create_directories("data/tokens");

            GDriveHandler gdrive(tokenPath, credsPath);
            gdrive.ensureAuthenticated(); 

            metadata.addAccount(email, tokenPath);
            metadata.save();
            std::cout << "Successfully added and authenticated account: " << email << std::endl;

        } else if (operation == "upload") {
            if (argc < 3) throw std::runtime_error("Missing file path for upload.");
            std::string localFilePath = argv[2];
            long long fileSize = fs::file_size(localFilePath);
            std::string remoteFileName = fs::path(localFilePath).filename().string();
            
            auto& md = metadata.getMetadata();
            if (md["files"].contains(remoteFileName)) {
                throw std::runtime_error("File with this name already exists in metadata. Please remove it first.");
            }

            const std::string tempChunkDir = "./temp_chunks_upload";
            fs::create_directories(tempChunkDir);
            long long chunkSize = 50 * 1024 * 1024;
            splitFile(localFilePath, tempChunkDir, chunkSize);

            int part = 1;
            for (const auto& entry : fs::directory_iterator(tempChunkDir)) {
                bool uploaded = false;
                for (auto& acc : md["accounts"].items()) {
                    long long used = acc.value()["used_space"].get<long long>();
                    long long total = acc.value()["total_space"].get<long long>();
                    long long chunk_size_on_disk = fs::file_size(entry.path());

                    if ((total - used) > chunk_size_on_disk) {
                        std::cout << "Uploading part " << part << " using account " << acc.key() << "..." << std::endl;
                        GDriveHandler gdrive(acc.value()["token_path"], credsPath);
                        std::string fileId = gdrive.uploadChunk(entry.path().string(), entry.path().filename().string());

                        md["files"][remoteFileName]["chunks"].push_back({
                            {"part", part},
                            {"account", acc.key()},
                            {"drive_file_id", fileId}
                        });
                        acc.value()["used_space"] = used + chunk_size_on_disk;
                        uploaded = true;
                        break;
                    }
                }
                if (!uploaded) {
                    fs::remove_all(tempChunkDir);
                    throw std::runtime_error("Upload failed! Not enough space on any account for chunk " + std::to_string(part));
                }
                part++;
            }

            md["files"][remoteFileName]["total_size"] = fileSize;
            metadata.save();
            fs::remove_all(tempChunkDir);
            std::cout << "File upload completed successfully!" << std::endl;


        } else if (operation == "download") {
            if (argc < 4) throw std::runtime_error("Missing remote file name or save path for download.");
            std::string remoteFileName = argv[2];
            std::string savePath = argv[3];
            
            auto& md = metadata.getMetadata();
            if (!md["files"].contains(remoteFileName)) {
                throw std::runtime_error("File not found in metadata.");
            }

            const std::string tempChunkDir = "./temp_chunks_download";
            fs::create_directories(tempChunkDir);

            for (const auto& chunk_info : md["files"][remoteFileName]["chunks"]) {
                std::string account = chunk_info["account"];
                std::string file_id = chunk_info["drive_file_id"];
                int part_num = chunk_info["part"];
                
                // FIX: Explicitly call .string() on the path object before assigning to std::string
                std::string chunk_save_path = (fs::path(tempChunkDir) / (remoteFileName + ".part" + std::to_string(part_num))).string();
                
                std::string token_path = md["accounts"][account]["token_path"];
                GDriveHandler gdrive(token_path, credsPath);
                gdrive.downloadChunk(file_id, chunk_save_path);
            }

            mergeFiles(tempChunkDir, savePath);
            fs::remove_all(tempChunkDir);
            std::cout << "File download completed successfully! Saved to " << savePath << std::endl;

        } else if (operation == "list-files") {
            std::cout << "--- Managed Files ---\n";
            for (const auto& file : metadata.getMetadata()["files"].items()) {
                std::cout << file.key() << " (" << file.value()["total_size"].get<long long>() / (1024*1024) << " MB)" << std::endl;
            }
        } else if (operation == "list-accounts") {
            std::cout << "--- Linked Accounts ---\n";
            for (const auto& acc : metadata.getMetadata()["accounts"].items()) {
                long long used = acc.value()["used_space"].get<long long>();
                long long total = acc.value()["total_space"].get<long long>();
                std::cout << acc.key() << " - " << used / (1024*1024) << " / " << total / (1024*1024) << " MB used." << std::endl;
            }
        }
        else {
            printUsage(argv[0]);
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "\nAn error occurred: " << e.what() << std::endl;
        if(fs::exists("./temp_chunks_upload")) fs::remove_all("./temp_chunks_upload");
        if(fs::exists("./temp_chunks_download")) fs::remove_all("./temp_chunks_download");
        return 1;
    }

    return 0;
}

void splitFile(const std::string& inputFilePath, const std::string& outputDir, long long chunkSize) {
    if (!fs::exists(inputFilePath) || fs::is_directory(inputFilePath)) throw std::runtime_error("Input file does not exist or is a directory.");
    fs::create_directories(outputDir);
    std::ifstream inputFile(inputFilePath, std::ios::binary);
    if (!inputFile.is_open()) throw std::runtime_error("Could not open input file.");
    char* buffer = new char[chunkSize];
    int chunkNumber = 1;
    while (inputFile) {
        inputFile.read(buffer, chunkSize);
        std::streamsize bytesRead = inputFile.gcount();
        if (bytesRead > 0) {
            std::string chunkFileName = fs::path(inputFilePath).filename().string() + ".part" + std::to_string(chunkNumber);
            std::ofstream outputFile(fs::path(outputDir) / chunkFileName, std::ios::binary);
            outputFile.write(buffer, bytesRead);
            chunkNumber++;
        }
    }
    delete[] buffer;
}

void mergeFiles(const std::string& inputDir, const std::string& outputFilePath) {
    if (!fs::exists(inputDir) || !fs::is_directory(inputDir)) throw std::runtime_error("Input directory does not exist.");
    std::vector<fs::path> chunkFiles;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file() && getPartNumber(entry.path()) != -1) {
            chunkFiles.push_back(entry.path());
        }
    }
    if (chunkFiles.empty()) throw std::runtime_error("No chunk files found.");
    std::sort(chunkFiles.begin(), chunkFiles.end(), [](const fs::path& a, const fs::path& b) {
        return getPartNumber(a) < getPartNumber(b);
    });
    std::ofstream outputFile(outputFilePath, std::ios::binary);
    if (!outputFile.is_open()) throw std::runtime_error("Could not create output file.");
    char* buffer = new char[4 * 1024 * 1024];
    for (const auto& chunkPath : chunkFiles) {
        std::ifstream inputFile(chunkPath, std::ios::binary);
        while (inputFile) {
            inputFile.read(buffer, 4 * 1024 * 1024);
            std::streamsize bytesRead = inputFile.gcount();
            if (bytesRead > 0) outputFile.write(buffer, bytesRead);
        }
    }
    delete[] buffer;
}

int getPartNumber(const fs::path& path) {
    std::string filename = path.filename().string();
    size_t pos = filename.rfind(".part");
    if (pos != std::string::npos) {
        try { return std::stoi(filename.substr(pos + 5)); }
        catch (...) { return -1; }
    }
    return -1;
}
