#ifndef GDRIVE_HANDLER_H
#define GDRIVE_HANDLER_H

#include <string>
#include <nlohmann/json.hpp>

class GDriveHandler {
public:
    GDriveHandler(const std::string& token_path, const std::string& credentials_path);
    void ensureAuthenticated();
    std::string uploadChunk(const std::string& local_file_path, const std::string& remote_file_name);
    
    // New function for downloading
    void downloadChunk(const std::string& file_id, const std::string& save_path);

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
