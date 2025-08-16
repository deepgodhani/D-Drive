#include "Shell.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <future>
#include <iterator>
#include <algorithm>
#include <chrono>
#include <indicators/progress_bar.hpp>
#include <indicators/cursor_control.hpp>

namespace fs = std::filesystem;

Shell::Shell()
    : m_creds_path("data/credentials/credentials.json"),
      m_metadata_changed(false),
      m_upload_slots(8)
{
    initializeState();

    m_commands = {
        {"add-account", {"Add a new Google Drive account", [this](const auto& args) { addAccount(args); }}},
        {"upload", {"Upload a file", [this](const auto& args) { uploadFile(args); }}},
        {"download", {"Download a file", [this](const auto& args) { downloadFile(args); }}},
        {"list", {"List uploaded files", [this](const auto& args) { listFiles(args); }}},
        {"accounts", {"List connected accounts", [this](const auto& args) { listAccounts(args); }}},
        {"help", {"Show help", [this](const auto& args) { showHelp(args); }}},
        {"exit", {"Exit the application", [this](const auto& args) { saveMetadataOnExit(); exit(0); }}},
        {"delete", {"Delete a file from D-Drive", [this](const auto& args) { deleteFile(args); }}}
    };
}

void Shell::run() {
    std::cout << "Welcome to D-Drive CLI\nType 'help' to list available commands\n";
    std::string input;
    while (true) {
        std::cout << ">> ";
        std::getline(std::cin, input);
        auto tokens = parseCommand(input);
        if (tokens.empty()) continue;
        std::string cmd = tokens[0];
        if (cmd == "exit") break;
        auto it = m_commands.find(cmd);
        if (it != m_commands.end()) {
            try {
                it->second.handler(tokens);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "Unknown command: " << cmd << std::endl;
        }
    }
    saveMetadataOnExit();
}

void Shell::uploadFile(const std::vector<std::string>& args) {
    if (args.size() < 2)
        throw std::runtime_error("Usage: upload <file_path>");
    uploadFile(args[1]);
}

void Shell::uploadFile(const std::string& localFilePath) {
    std::ifstream file(localFilePath, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open file: " + localFilePath);

    std::streamsize fileSize = file.tellg();
    file.seekg(0);
    const int64_t chunkSize = 50 * 1024 * 1024;
    int totalChunks = (fileSize + chunkSize - 1) / chunkSize;
// In Shell::uploadFile...
std::string fileName = fs::path(localFilePath).filename().string();
std::string metadataKey = fileName;

if (m_local_accounts.empty()) {
    throw std::runtime_error("No linked accounts. Use 'add-account' first.");
}

// Set initial metadata
m_metadata["files"][metadataKey]["total_size"] = fileSize;


    std::mutex meta_mutex;
    std::mutex progress_mutex;
    std::atomic<long long> uploaded_bytes = 0;
    std::atomic<int> successful_chunks = 0;
    std::vector<std::future<void>> futures;

    indicators::ProgressBar bar{
        indicators::option::BarWidth{50},
        indicators::option::Start{"["},
        indicators::option::Fill{"="},
        indicators::option::Lead{">"},
        indicators::option::Remainder{" "},
        indicators::option::End{"]"},
        indicators::option::MaxProgress{static_cast<size_t>(fileSize)},
        indicators::option::ShowPercentage{true},
        indicators::option::ShowElapsedTime{true},
        indicators::option::ShowRemainingTime{true}
    };
    
    indicators::show_console_cursor(false);
    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < totalChunks; ++i) {
        int64_t thisChunkSize = std::min(chunkSize, fileSize - (i * chunkSize));
        std::vector<char> buffer(thisChunkSize);
        file.read(buffer.data(), thisChunkSize);

        std::string chunkFilePath = "data/" + fileName + ".part" + std::to_string(i);
        std::ofstream chunkFile(chunkFilePath, std::ios::binary);
        chunkFile.write(buffer.data(), thisChunkSize);
        chunkFile.close();

        auto account_it = std::next(m_local_accounts.begin(), i % m_local_accounts.size());
        std::string account = account_it->first;
        std::string tokenPath = account_it->second;

        m_upload_slots.acquire();
       // In the for loop inside Shell::uploadFile...
futures.emplace_back(std::async(std::launch::async, [this, tokenPath, chunkFilePath, fileName, i, &bar, &uploaded_bytes, &meta_mutex, &progress_mutex, &successful_chunks, account, metadataKey, &start_time] {
    try {
        GDriveHandler gdrive(tokenPath, m_creds_path);
        gdrive.ensureAuthenticated();
        
        // --- NEW LOGIC: Get or create the chunk folder in THIS account ---
        const std::string CHUNK_FOLDER_NAME = "D-Drive Chunks";
        std::string chunkFolderId = gdrive.findFileOrFolder(CHUNK_FOLDER_NAME, "root");

        if (chunkFolderId.empty()) {
            // This should ideally never happen if account setup was successful
            throw std::runtime_error("Critical: 'D-Drive Chunks' folder not found for account " + account + ". Please try re-adding the account.");
        }

        cpr::cpr_off_t chunk_uploaded = 0;

        std::string fileId = gdrive.uploadChunk(
            chunkFilePath,
            fileName + ".part" + std::to_string(i),
            chunkFolderId, // Use the account-specific folder ID
            [&](cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t now_ul, intptr_t) -> bool {
                // ... (the progress callback logic remains exactly the same)
                cpr::cpr_off_t delta = now_ul - chunk_uploaded;
                chunk_uploaded = now_ul;
                uploaded_bytes += delta;

                std::lock_guard<std::mutex> lock(progress_mutex);
                auto now = std::chrono::steady_clock::now();
                auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                if (elapsed_seconds > 0) {
                    double speed = (double)uploaded_bytes / elapsed_seconds;
                    std::stringstream speed_ss;
                    if (speed < 1024 * 1024) {
                        speed_ss << std::fixed << std::setprecision(1) << (speed / 1024.0) << " KB/s";
                    } else {
                        speed_ss << std::fixed << std::setprecision(1) << (speed / (1024.0 * 1024.0)) << " MB/s";
                    }
                    bar.set_option(indicators::option::PostfixText{speed_ss.str()});
                }
                bar.set_progress(uploaded_bytes);
                return true;
            });

        {
            std::lock_guard<std::mutex> lock(meta_mutex);
            m_metadata["files"][metadataKey]["chunks"].push_back({
                {"part", i},
                {"account", account},
                {"drive_file_id", fileId}
            });
        }
        successful_chunks++;
        fs::remove(chunkFilePath);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(progress_mutex);
        std::cerr << "\nError uploading chunk " << i << ": " << e.what() << std::endl;
    }
    m_upload_slots.release();
}));
    }

    for (auto& f : futures) {
        f.get();
    }
    
    indicators::show_console_cursor(true);
    
    // --- FIX FOR PROGRESS BAR SPAM ---
    // Only update and finalize the bar once, after all threads are done.
    if (successful_chunks == totalChunks) {
        bar.set_option(indicators::option::PostfixText{"Upload Complete!"});
        if(!bar.is_completed()) bar.mark_as_completed();
        std::cout << "\nFile uploaded successfully. Metadata saved." << std::endl;
    } else {
        bar.set_option(indicators::option::PostfixText{"Upload Failed!"});
        if(!bar.is_completed()) bar.mark_as_completed();
        std::cerr << "\nUpload failed. " << successful_chunks << "/" << totalChunks << " chunks were successful." << std::endl;
    }
    // --- END FIX ---
    
    std::ofstream out("data/metadata.json");
    out << m_metadata.dump(4);
}

void Shell::downloadFile(const std::vector<std::string>& args) {
    if (args.size() < 3)
        throw std::runtime_error("Usage: download <remote_file_name> <save_as_path>");

    std::string remoteFileName = args[1];
    std::string savePath = args[2];

    if (!m_metadata["files"].contains(remoteFileName))
        throw std::runtime_error("No metadata found for: " + remoteFileName);

    const auto& fileMeta = m_metadata["files"][remoteFileName];
    const auto& chunks = fileMeta["chunks"];

    std::string tempDir = "./temp_chunks_download";
    if (fs::exists(tempDir)) fs::remove_all(tempDir);
    fs::create_directories(tempDir);

    std::vector<std::thread> threads;
    for (const auto& chunk : chunks) {
        threads.emplace_back([&, chunk]() {
            std::string account = chunk["account"];
            std::string file_id = chunk["drive_file_id"];
            int part = chunk["part"];
            std::string token_path = m_local_accounts[account];
            std::string chunkPath = (fs::path(tempDir) / (remoteFileName + ".part" + std::to_string(part))).string();

            GDriveHandler gdrive(token_path, m_creds_path);
            gdrive.downloadChunk(file_id, chunkPath, nullptr);
        });
    }

    for (auto& t : threads) t.join();

    std::ofstream out(savePath, std::ios::binary);
    for (size_t i = 0; i < chunks.size(); ++i) {
        std::string partPath = (fs::path(tempDir) / (remoteFileName + ".part" + std::to_string(i))).string();
        std::ifstream in(partPath, std::ios::binary);
        out << in.rdbuf();
        in.close();
    }
    out.close();
    fs::remove_all(tempDir);

    std::cout << "✅ Download completed to: " << savePath << std::endl;
}

void Shell::listFiles(const std::vector<std::string>&) {
    std::cout << "--- Uploaded Files ---\n";
    for (const auto& [key, value] : m_metadata["files"].items()) {
        std::cout << key << " (" << value["chunks"].size() << " chunks)\n";
    }
}

void Shell::initializeState() {
    if (fs::exists("data/metadata.json")) {
        std::ifstream in("data/metadata.json");
        in >> m_metadata;
    }
    for (const auto& file : fs::directory_iterator("data/tokens")) {
        std::string email = file.path().stem().string();
        m_local_accounts[email] = file.path().string();
    }
   
}

void Shell::saveMetadataOnExit() {
    if (m_metadata_changed) {
        std::ofstream out("data/metadata.json");
        out << m_metadata.dump(4);
    }
}

std::vector<std::string> Shell::parseCommand(const std::string& input) {
    std::istringstream iss(input);
    return {std::istream_iterator<std::string>{iss}, std::istream_iterator<std::string>{}};
}

void Shell::addAccount(const std::vector<std::string>&) {
    std::cout << "Adding new account..." << std::endl;
    // This handler is temporary, just for the auth flow
    GDriveHandler auth_handler("", m_creds_path);
    std::string email = auth_handler.authenticateNewAccount("data/tokens");

    std::string token_path = "data/tokens/" + email + ".json";
    m_local_accounts[email] = token_path;
    std::cout << "Account for " << email << " added locally." << std::endl;

    // --- NEW LOGIC: SETUP FOLDER FOR THE NEW ACCOUNT ---
    try {
        std::cout << "Setting up storage folder in " << email << "'s Drive..." << std::endl;
        // Create a handler for the newly added account
        GDriveHandler new_account_gdrive(token_path, m_creds_path);

        const std::string CHUNK_FOLDER_NAME = "D-Drive Chunks";
        // Find or create the folder in the root of the new account's drive
        std::string chunkFolderId = new_account_gdrive.findFileOrFolder(CHUNK_FOLDER_NAME, "root");
        if (chunkFolderId.empty()) {
            new_account_gdrive.createFolder(CHUNK_FOLDER_NAME, "root");
            std::cout << "Created 'D-Drive Chunks' folder." << std::endl;
        } else {
            std::cout << "'D-Drive Chunks' folder already exists." << std::endl;
        }
        std::cout << "Setup complete for " << email << "!" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error during folder setup for " << email << ": " << e.what() << std::endl;
        // Optional: decide if you want to remove the account if setup fails
    }
}

void Shell::listAccounts(const std::vector<std::string>&) {
    std::cout << "Connected accounts:\n";
    for (const auto& [email, _] : m_local_accounts)
        std::cout << "- " << email << std::endl;
}

void Shell::showHelp(const std::vector<std::string>&) {
    std::cout << "Available commands:\n";
    for (const auto& [cmd, info] : m_commands)
        std::cout << std::setw(12) << std::left << cmd << " : " << info.description << std::endl;
    std::cout << "Type 'exit' to quit.\n";
}

void Shell::deleteFile(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        throw std::runtime_error("Usage: delete <remote_file_name>");
    }
    std::string remoteFileName = args[1];

    if (!m_metadata["files"].contains(remoteFileName)) {
        throw std::runtime_error("File not found in metadata: " + remoteFileName);
    }

    const auto& chunks = m_metadata["files"][remoteFileName]["chunks"];
    std::cout << "Deleting " << remoteFileName << " (" << chunks.size() << " chunks)..." << std::endl;

    std::vector<std::future<void>> futures;
    std::atomic<int> successful_deletes = 0;

    for (const auto& chunk_info : chunks) {
        std::string account_email = chunk_info["account"];
        std::string file_id = chunk_info["drive_file_id"];
        std::string token_path = m_local_accounts[account_email];

        futures.emplace_back(std::async(std::launch::async, [this, token_path, file_id, &successful_deletes]() {
            try {
                // Create a handler for the specific account for this chunk
                GDriveHandler gdrive(token_path, m_creds_path);
                gdrive.deleteFileById(file_id);
                successful_deletes++;
            } catch (const std::exception& e) {
                // It's often okay if deletion fails (e.g., file already gone), so we just print a warning.
                std::cerr << "\nWarning: Could not delete chunk " << file_id << ". Reason: " << e.what() << std::endl;
            }
        }));
    }

    // Wait for all delete operations to complete
    for (auto& f : futures) {
        f.get();
    }

    // After deleting all chunks, remove the file from our metadata
    m_metadata["files"].erase(remoteFileName);
    m_metadata_changed = true; // Mark metadata for saving on exit
    saveMetadataOnExit(); // Save immediately for data consistency

    std::cout << "Successfully deleted '" << remoteFileName << "' from D-Drive." << std::endl;
    std::cout << successful_deletes << "/" << chunks.size() << " chunks deleted from Google Drive." << std::endl;
}