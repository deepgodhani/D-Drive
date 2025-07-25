#include "gdrive_handler.h"
#include <fstream>
#include <iostream>
#include <cpr/cpr.h>
#include <filesystem> // Required for the fix

GDriveHandler::GDriveHandler(const std::string& token_path, const std::string& credentials_path)
    : m_token_path(token_path) {
    std::ifstream credentials_file(credentials_path);
    if (!credentials_file.is_open()) {
        throw std::runtime_error("FATAL: Could not open credentials file: " + credentials_path);
    }
    credentials_file >> m_credentials;
    credentials_file.close();
    loadTokens();
}

bool GDriveHandler::loadTokens() {
    std::ifstream token_file(m_token_path);
    if (token_file.is_open()) {
        token_file >> m_tokens;
        token_file.close();
        return m_tokens.contains("refresh_token");
    }
    return false;
}

void GDriveHandler::saveTokens() {
    std::ofstream token_file(m_token_path);
    token_file << m_tokens.dump(4);
    token_file.close();
}

void GDriveHandler::ensureAuthenticated() {
    if (!m_tokens.contains("refresh_token")) {
        std::cout << "No existing session found. Starting authentication..." << std::endl;
        performAuthentication();
        return;
    }
    if (refreshAccessToken()) {
        std::cout << "Session refreshed successfully." << std::endl;
    } else {
        std::cout << "Could not refresh session. Please authenticate again." << std::endl;
        performAuthentication();
    }
}

void GDriveHandler::performAuthentication() {
    std::string scope = "https://www.googleapis.com/auth/drive";
    std::string client_id = m_credentials["installed"]["client_id"].get<std::string>();
    std::string redirect_uri = "http://127.0.0.1";
    
    std::string auth_url = std::string("https://accounts.google.com/o/oauth2/v2/auth?") +
        "scope=" + scope + "&" +
        "response_type=code&" +
        "redirect_uri=" + redirect_uri + "&" +
        "client_id=" + client_id;

    std::cout << "\nPlease open this URL in your browser:\n\n" << auth_url << std::endl;
    std::cout << "\nAfter authorizing, paste the code from your browser's address bar here: ";
    std::string auth_code;
    std::cin >> auth_code;

    cpr::Response r = cpr::Post(cpr::Url{m_credentials["installed"]["token_uri"].get<std::string>()},
        cpr::Payload{
            {"code", auth_code},
            {"client_id", client_id},
            {"client_secret", m_credentials["installed"]["client_secret"].get<std::string>()},
            {"redirect_uri", redirect_uri},
            {"grant_type", "authorization_code"}
        });

    if (r.status_code == 200) {
        m_tokens = nlohmann::json::parse(r.text);
        saveTokens();
        std::cout << "Authentication successful!" << std::endl;
    } else {
        throw std::runtime_error("Authentication failed. Response: " + r.text);
    }
}

bool GDriveHandler::refreshAccessToken() {
    std::cout << "Refreshing access token..." << std::endl;
    cpr::Response r = cpr::Post(cpr::Url{m_credentials["installed"]["token_uri"].get<std::string>()},
        cpr::Payload{
            {"refresh_token", m_tokens["refresh_token"].get<std::string>()},
            {"client_id", m_credentials["installed"]["client_id"].get<std::string>()},
            {"client_secret", m_credentials["installed"]["client_secret"].get<std::string>()},
            {"grant_type", "refresh_token"}
        });

    if (r.status_code == 200) {
        nlohmann::json new_token_data = nlohmann::json::parse(r.text);
        m_tokens["access_token"] = new_token_data["access_token"];
        saveTokens();
        return true;
    } else {
        std::cerr << "Failed to refresh access token. Response: " << r.text << std::endl;
        return false;
    }
}

std::string GDriveHandler::uploadChunk(const std::string& local_file_path, const std::string& remote_file_name) {
    ensureAuthenticated();
    std::ifstream file(local_file_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open chunk file for upload: " + local_file_path);
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    nlohmann::json metadata = { {"name", remote_file_name} };
    std::string metadata_str = metadata.dump();

    // FINAL FIX: Use the iterator-based constructor for cpr::Buffer that MSVC expects.
    cpr::Buffer file_buffer(
        content.begin(), 
        content.end(),
        std::filesystem::path(remote_file_name)
    );

    cpr::Response r = cpr::Post(cpr::Url{"https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart"},
        cpr::Header{{"Authorization", "Bearer " + m_tokens["access_token"].get<std::string>()}},
        cpr::Multipart{
            cpr::Part{"metadata", metadata_str, "application/json; charset=UTF-8"},
            cpr::Part{"file", file_buffer, "application/octet-stream"}
        });

    if (r.status_code == 200) {
        nlohmann::json response_json = nlohmann::json::parse(r.text);
        std::cout << "Successfully uploaded '" << remote_file_name << "', File ID: " << response_json["id"].get<std::string>() << std::endl;
        return response_json["id"].get<std::string>();
    } else {
        throw std::runtime_error("Upload failed. Response: " + r.text);
    }
}

void GDriveHandler::downloadChunk(const std::string& file_id, const std::string& save_path) {
    ensureAuthenticated();

    std::ofstream of(save_path, std::ios::binary);
    if (!of.is_open()) {
        throw std::runtime_error("Could not open file for writing download: " + save_path);
    }

    cpr::Response r = cpr::Download(of,
        cpr::Url{"https://www.googleapis.com/drive/v3/files/" + file_id + "?alt=media"},
        cpr::Header{{"Authorization", "Bearer " + m_tokens["access_token"].get<std::string>()}}
    );

    if (r.status_code != 200) {
        throw std::runtime_error("Download failed for file ID " + file_id + ". Status: " + std::to_string(r.status_code) + " Body: " + r.text);
    }
    std::cout << "Successfully downloaded chunk to: " << save_path << std::endl;
}
