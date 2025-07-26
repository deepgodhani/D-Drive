#ifndef GDRIVE_HANDLER_H
#define GDRIVE_HANDLER_H

#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

// Define a type for our progress callback function to match CPR's signature
using ProgressCallback = std::function<bool(cpr::cpr_off_t downloadTotal, cpr::cpr_off_t downloadNow, cpr::cpr_off_t uploadTotal, cpr::cpr_off_t uploadNow, intptr_t userdata)>;

class GDriveHandler {
public:
    GDriveHandler(const std::string& token_path, const std::string& credentials_path);
    void ensureAuthenticated();

    // --- Cloud Metadata Functions ---
    std::string findFileOrFolder(const std::string& name, const std::string& parent_id = "root");
    std::string createFolder(const std::string& name, const std::string& parent_id = "root");
    std::string uploadNewFile(const std::string& content, const std::string& remote_name, const std::string& parent_id = "root");
    void updateFileContent(const std::string& file_id, const std::string& content);
    std::string downloadFileContent(const std::string& file_id);

    // --- Chunk Transfer Functions (Now with Progress) ---
    std::string uploadChunk(const std::string& local_file_path, const std::string& remote_file_name, const std::string& parentFolderId, const ProgressCallback& progress_callback = nullptr);
    void downloadChunk(const std::string& file_id, const std::string& save_path, const ProgressCallback& progress_callback = nullptr);

private:
    void performAuthentication();
    bool refreshAccessToken();
    void saveTokens();
    bool loadTokens();

    std::string m_token_path;
    nlohmann::json m_credentials;
    nlohmann::json m_tokens;
};

#endif // GDRIVE_HANDLER_H
