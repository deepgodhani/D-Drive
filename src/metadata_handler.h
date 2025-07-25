#ifndef METADATA_HANDLER_H
#define METADATA_HANDLER_H

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class MetadataHandler {
public:
    MetadataHandler(const std::string& metadata_path);

    // Load metadata from the file
    bool load();

    // Save the current metadata to the file
    void save();

    // Add a new account to the metadata
    void addAccount(const std::string& email, const std::string& token_path);

    // Get the entire metadata object
    json& getMetadata();

private:
    std::string m_path;
    json m_data;

    // Initialize a new metadata file if one doesn't exist
    void initializeNewMetadata();
};

#endif // METADATA_HANDLER_H
