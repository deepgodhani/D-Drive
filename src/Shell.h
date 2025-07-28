#ifndef SHELL_H
#define SHELL_H

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <condition_variable>
#include "gdrive_handler.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Semaphore {
public:
    Semaphore(int count) : count_(count) {}

    void acquire() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return count_ > 0; });
        --count_;
    }

    void release() {
        std::lock_guard<std::mutex> lock(mtx_);
        ++count_;
        cv_.notify_one();
    }

private:
    int count_;
    std::mutex mtx_;
    std::condition_variable cv_;
};


class Shell {
public:
    Shell();
    void run();

private:
    // --- State Variables ---
    std::string m_creds_path;
    std::map<std::string, std::string> m_local_accounts;
    std::unique_ptr<GDriveHandler> m_primary_gdrive;
    json m_metadata;
    bool m_metadata_changed;
    Semaphore m_upload_slots; // Semaphore as a class member

    // --- Command Handling ---
    struct Command {
        std::string description;
        std::function<void(const std::vector<std::string>&)> handler;
    };

    std::map<std::string, Command> m_commands;

    // --- Private Methods ---
    void initializeState();
    void saveMetadataOnExit();
    std::vector<std::string> parseCommand(const std::string& input);

    // --- Command Handler Functions ---
    void addAccount(const std::vector<std::string>& args);
    void uploadFile(const std::vector<std::string>& args);
    void uploadFile(const std::string& localFilePath);
    void downloadFile(const std::vector<std::string>& args);
    void listFiles(const std::vector<std::string>& args);
    void listAccounts(const std::vector<std::string>& args);
    void showHelp(const std::vector<std::string>& args);
};

#endif // SHELL_H