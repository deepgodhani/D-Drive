#include "metadata_handler.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

MetadataHandler::MetadataHandler(const std::string& metadata_path) : m_path(metadata_path) {
    // Ensure the directory for the metadata file exists
    fs::path p(m_path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    load();
}

bool MetadataHandler::load() {
    std::ifstream file(m_path);
    if (file.is_open()) {
        file >> m_data;
        file.close();
        return true;
    }
    // If file doesn't exist, create a default structure
    initializeNewMetadata();
    return false;
}

void MetadataHandler::save() {
    std::ofstream file(m_path);
    file << m_data.dump(4); // Pretty-print with 4-space indent
    file.close();
}

void MetadataHandler::addAccount(const std::string& email, const std::string& token_path) {
    m_data["accounts"][email] = {
        {"token_path", token_path},
        {"total_space", 15 * 1024 * 1024 * 1024LL}, // Assume 15 GB, LL for long long
        {"used_space", 0}
    };
}

json& MetadataHandler::getMetadata() {
    return m_data;
}

void MetadataHandler::initializeNewMetadata() {
    m_data = {
        {"accounts", json::object()},
        {"files", json::object()}
    };
}
