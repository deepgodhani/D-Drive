#include "gdrive_handler.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <windows.h>
#include <sstream>
#include <vector>


void open_url_in_browser(const std::string &url) {
#if defined(_WIN32)
  std::string command = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  std::string command = "open \"" + url + "\"";
#else
  std::string command = "xdg-open \"" + url + "\"";
#endif
  std::system(command.c_str());
}

std::string decode_base64url(const std::string& input) {
  static const std::string base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

  std::string base64 = input;
  std::replace(base64.begin(), base64.end(), '-', '+');
  std::replace(base64.begin(), base64.end(), '_', '/');
  while (base64.length() % 4 != 0)
      base64 += '=';

  auto is_base64 = [](unsigned char c) {
      return std::isalnum(c) || c == '+' || c == '/';
  };

  std::vector<unsigned char> bytes;
  int val = 0, valb = -8;
  for (unsigned char c : base64) {
      if (!is_base64(c)) break;
      val = (val << 6) + base64_chars.find(c);
      valb += 6;
      if (valb >= 0) {
          bytes.push_back(char((val >> valb) & 0xFF));
          valb -= 8;
      }
  }

  return std::string(bytes.begin(), bytes.end());
}
std::vector<std::string> splitString(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}
// --- All authentication, find, create, update functions are correct and remain
// unchanged. ---
GDriveHandler::GDriveHandler(const std::string &token_path,
                             const std::string &credentials_path)
    : m_token_path(token_path) {
  std::ifstream credentials_file(credentials_path);
  if (!credentials_file.is_open()) {
    throw std::runtime_error("FATAL: Could not open credentials file: " +
                             credentials_path);
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
    std::cout << "No existing session found. Starting authentication..."
              << std::endl;
    performAuthentication();
    return;
  }
  if (!refreshAccessToken()) {
    std::cout << "Could not refresh session. Please authenticate again."
              << std::endl;
    performAuthentication();
  }
}
std::string GDriveHandler::getAccessToken() const {
  return m_tokens["access_token"].get<std::string>();
}


std::string GDriveHandler::authenticateNewAccount(const std::string& token_directory) {
  performAuthentication(); // Triggers OAuth2.0 flow

  if (!m_tokens.contains("id_token"))
      throw std::runtime_error("ID token not found after authentication.");

  std::string id_token = m_tokens["id_token"];
  auto parts = splitString(id_token, '.'); // You'll need to implement or already have splitString()

  if (parts.size() != 3)
      throw std::runtime_error("Invalid ID token format.");

  std::string payload_json = decode_base64url(parts[1]);
  nlohmann::json payload = nlohmann::json::parse(payload_json);
  std::string email = payload["email"];

  std::filesystem::create_directories(token_directory);
  std::string token_path = (std::filesystem::path(token_directory) / (email + ".json")).string();

  std::ofstream out(token_path);
  out << m_tokens.dump(4);
  out.close();

  m_token_path = token_path;

  return email;
}

bool GDriveHandler::refreshAccessToken() {
  cpr::Response r = cpr::Post(
      cpr::Url{m_credentials["installed"]["token_uri"].get<std::string>()},
      cpr::Payload{
          {"refresh_token", m_tokens["refresh_token"].get<std::string>()},
          {"client_id",
           m_credentials["installed"]["client_id"].get<std::string>()},
          {"client_secret",
           m_credentials["installed"]["client_secret"].get<std::string>()},
          {"grant_type", "refresh_token"}});
  if (r.status_code == 200) {
    nlohmann::json new_token_data = nlohmann::json::parse(r.text);
    m_tokens["access_token"] = new_token_data["access_token"];
    saveTokens();
    return true;
  }
  return false;
}
std::string GDriveHandler::findFileOrFolder(const std::string &name,
                                            const std::string &parent_id) {
  ensureAuthenticated();
  std::string query = "name = '" + name + "' and '" + parent_id +
                      "' in parents and trashed = false";
  cpr::Response r = cpr::Get(
      cpr::Url{"https://www.googleapis.com/drive/v3/files"},
      cpr::Header{{"Authorization",
                   "Bearer " + m_tokens["access_token"].get<std::string>()}},
      cpr::Parameters{{"q", query}, {"fields", "files(id, name)"}});
  if (r.status_code == 200) {
    auto json_response = nlohmann::json::parse(r.text);
    if (!json_response["files"].empty()) {
      return json_response["files"][0]["id"];
    }
  }
  return "";
}
std::string GDriveHandler::createFolder(const std::string &name,
                                        const std::string &parent_id) {
  ensureAuthenticated();
  nlohmann::json metadata = {{"name", name},
                             {"mimeType", "application/vnd.google-apps.folder"},
                             {"parents", {parent_id}}};
  cpr::Response r = cpr::Post(
      cpr::Url{"https://www.googleapis.com/drive/v3/files"},
      cpr::Header{{"Authorization",
                   "Bearer " + m_tokens["access_token"].get<std::string>()},
                  {"Content-Type", "application/json"}},
      cpr::Body{metadata.dump()});
  if (r.status_code == 200) {
    return nlohmann::json::parse(r.text)["id"];
  }
  throw std::runtime_error("Failed to create folder. Response: " + r.text);
}

void GDriveHandler::shareFileOrFolder(const std::string& fileId, const std::string& emailAddress) {
  ensureAuthenticated();
  nlohmann::json permission = {
      {"type", "user"},
      {"role", "writer"},
      {"emailAddress", emailAddress}
  };

  cpr::Response r = cpr::Post(
      cpr::Url{"https://www.googleapis.com/drive/v3/files/" + fileId + "/permissions"},
      cpr::Header{
          {"Authorization", "Bearer " + m_tokens["access_token"].get<std::string>()},
          {"Content-Type", "application/json"}
      },
      cpr::Parameters{{"sendNotificationEmail", "false"}},
      cpr::Body{permission.dump()}
  );

  if (r.status_code != 200) {
      // It's better not to throw an error here, just warn the user.
      // The upload might still succeed.
      std::cerr << "\nWarning: Failed to share folder " << fileId << " with " << emailAddress 
                << ". Response: " << r.text << std::endl;
  }
}

std::string GDriveHandler::uploadNewFile(const std::string &content,
                                         const std::string &remote_name,
                                         const std::string &parent_id) {
  ensureAuthenticated();
  nlohmann::json metadata = {{"name", remote_name}, {"parents", {parent_id}}};
  cpr::Buffer file_buffer(content.begin(), content.end(),
                          std::filesystem::path(remote_name));
  cpr::Response r = cpr::Post(
      cpr::Url{"https://www.googleapis.com/upload/drive/v3/"
               "files?uploadType=multipart"},
      cpr::Header{{"Authorization",
                   "Bearer " + m_tokens["access_token"].get<std::string>()}},
      cpr::Multipart{
          cpr::Part{"metadata", metadata.dump(),
                    "application/json; charset=UTF-8"},
          cpr::Part{"file", file_buffer, "application/octet-stream"}});
  if (r.status_code == 200) {
    return nlohmann::json::parse(r.text)["id"];
  } else {
    throw std::runtime_error("Upload new file failed. Response: " + r.text);
  }
}
void GDriveHandler::updateFileContent(const std::string &file_id,
                                      const std::string &content) {
  ensureAuthenticated();
  cpr::Response r = cpr::Patch(
      cpr::Url{"https://www.googleapis.com/upload/drive/v3/files/" + file_id +
               "?uploadType=media"},
      cpr::Header{{"Authorization",
                   "Bearer " + m_tokens["access_token"].get<std::string>()}},
      cpr::Body{content});
  if (r.status_code != 200) {
    throw std::runtime_error("Failed to update file content. Response: " +
                             r.text);
  }
}
std::string GDriveHandler::downloadFileContent(const std::string &file_id) {
  ensureAuthenticated();
  cpr::Response r = cpr::Get(
      cpr::Url{"https://www.googleapis.com/drive/v3/files/" + file_id +
               "?alt=media"},
      cpr::Header{{"Authorization",
                   "Bearer " + m_tokens["access_token"].get<std::string>()}});
  if (r.status_code == 200) {
    return r.text;
  }
  throw std::runtime_error("Failed to download file content. Response: " +
                           r.text);
}
void GDriveHandler::performAuthentication() {
  // A promise will hold the authorization code we get from the server
  std::promise<std::string> code_promise;
  std::future<std::string> code_future = code_promise.get_future();

  // Create and configure the local HTTP server
  httplib::Server svr;

  // This is the endpoint Google will redirect to.
  svr.Get("/", [&](const httplib::Request &req, httplib::Response &res) {
    // Check if the 'code' parameter exists in the request URL
    if (req.has_param("code")) {
      std::string auth_code = req.get_param_value("code");

      // Send a success message back to the user's browser
      res.set_content("<h1>Authentication Successful!</h1><p>You can now close "
                      "this browser tab.</p>",
                      "text/html");

      // Fulfill the promise with the authorization code
      code_promise.set_value(auth_code);
    } else {
      // Handle the case where the user denies access
      std::string error_msg =
          req.has_param("error") ? req.get_param_value("error") : "unknown";
      res.set_content("<h1>Authentication Failed</h1><p>Error: " + error_msg +
                          ". Please try again.</p>",
                      "text/html");
      code_promise.set_exception(std::make_exception_ptr(std::runtime_error(
          "User denied access or an error occurred: " + error_msg)));
    }
  });

  // Start the server in a separate thread so it doesn't block our main
  // application
  std::thread server_thread([&]() {
    // Listen on port 8080. This MUST match the URI in your Google Cloud
    // Console.
    if (!svr.listen("localhost", 8080)) {
      code_promise.set_exception(std::make_exception_ptr(
          std::runtime_error("Failed to start local server on port 8080.")));
    }
  });
  // Detach the thread to let it run independently. It will be stopped later.
  server_thread.detach();

  std::string scope = "https://www.googleapis.com/auth/drive openid email";

  std::string client_id =
      m_credentials["installed"]["client_id"].get<std::string>();
  std::string redirect_uri = "http://localhost:8080"; // This must match

  std::string auth_url =
      std::string("https://accounts.google.com/o/oauth2/v2/auth?") +
      "scope=" + scope + "&" + "response_type=code&" +
      "redirect_uri=" + redirect_uri + "&" + "client_id=" + client_id;

  std::cout << "\nYour browser is opening for authentication. Please follow "
               "the instructions..."
            << std::endl;
  open_url_in_browser(auth_url);

  // Wait for the future to be fulfilled by the server thread.
  // This will block until the user finishes authentication in the browser.
  std::cout
      << "Waiting for you to complete the sign-in process in your browser..."
      << std::endl;
  std::string auth_code = code_future.get(); // This line will wait
  svr.stop(); // Stop the server now that we have the code

  std::cout << "Authorization code received. Exchanging for tokens..."
            << std::endl;

  cpr::Response r = cpr::Post(
    cpr::Url{m_credentials["installed"]["token_uri"].get<std::string>()},
    cpr::Payload{
        {"code", auth_code},
        {"client_id", client_id},
        {"client_secret",
         m_credentials["installed"]["client_secret"].get<std::string>()},
        {"redirect_uri", redirect_uri},
        {"grant_type", "authorization_code"}});
std::cout << "Response: " << r.text << std::endl; // Debugging output
          

  if (r.status_code == 200) {
    m_tokens = nlohmann::json::parse(r.text);
    saveTokens();
    std::cout << "Authentication successful!" << std::endl;
  } else {
    throw std::runtime_error("Token exchange failed. Response: " + r.text);
  }
}

std::string GDriveHandler::extractUploadedFileId(const cpr::Response& response) {
  auto json_response = nlohmann::json::parse(response.text);
  if (json_response.contains("id")) {
      return json_response["id"];
  }
  return "";
}



std::string
GDriveHandler::uploadChunk(const std::string &local_file_path,
                           const std::string &remote_file_name,
                           const std::string &parentFolderId,
                           const ProgressCallback &progress_callback) {
  ensureAuthenticated();
  std::ifstream file(local_file_path, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("Could not open chunk file for upload: " +
                             local_file_path);
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  file.close();

  nlohmann::json metadata = {{"name", remote_file_name},
                             {"parents", {parentFolderId}}};
  cpr::Buffer file_buffer(content.begin(), content.end(),
                          std::filesystem::path(remote_file_name));

  cpr::Session session;
  session.SetUrl(cpr::Url{
      "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart"});
  session.SetHeader(
      {{"Authorization",
        "Bearer " + m_tokens["access_token"].get<std::string>()}});
  session.SetMultipart(
      {cpr::Part{"metadata", metadata.dump(),
                 "application/json; charset=UTF-8"},
       cpr::Part{"file", file_buffer, "application/octet-stream"}});

  if (progress_callback) {
    session.SetProgressCallback(progress_callback);
  }

  cpr::Response r = session.Post();
  if (r.status_code == 200) {
    return nlohmann::json::parse(r.text)["id"];
  } else {
    throw std::runtime_error("Upload failed. Response: " + r.text);
  }
}

void GDriveHandler::downloadChunk(const std::string &file_id,
                                  const std::string &save_path,
                                  const ProgressCallback &progress_callback) {
  ensureAuthenticated();
  std::ofstream of(save_path, std::ios::binary);
  if (!of.is_open()) {
    throw std::runtime_error("Could not open file for writing download: " +
                             save_path);
  }

  cpr::Session session;
  session.SetUrl(cpr::Url{"https://www.googleapis.com/drive/v3/files/" +
                          file_id + "?alt=media"});
  session.SetHeader(
      {{"Authorization",
        "Bearer " + m_tokens["access_token"].get<std::string>()}});

  // Use a WriteCallback to stream the download to the file
  session.SetWriteCallback(
      cpr::WriteCallback([&](const std::string_view &data, intptr_t) {
        of.write(data.data(), data.size());
        return true; // Return true to continue, false to abort
      }));

  if (progress_callback) {
    session.SetProgressCallback(progress_callback);
  }

  cpr::Response r = session.Get();
  if (r.status_code != 200) {
    throw std::runtime_error("Download failed for file ID " + file_id +
                             ". Status: " + std::to_string(r.status_code));
  }
}