#include "DfsEng.h"
#include "port.h"
#include <filesystem>
#include <chrono>
#include <ctime>
#include <regex>
#include "QuantaDb.h"

namespace Quanta
{
    std::string DfsEngine::GenerateMetadata(const std::string& filePath, long long fileSize, const std::string& lastModified)
    {
        std::filesystem::path path(filePath);

        // Create metadata using X::Dict
        X::Dict metadata;
        metadata->Set("name", path.filename().string());
        metadata->Set("extension", path.extension().string());
        metadata->Set("size", fileSize);
        metadata->Set("lastModified", lastModified);
        metadata->Set("parentDir", path.parent_path().string());

        // Convert to string for storage
        return metadata.ToString();
    }

    bool DfsEngine::Scan(std::string rootFolder)
    {
        if (m_isScanning) {
            return false; // Already scanning
        }

        m_isScanning = true;
        m_lastRootFolder = rootFolder;

        try {
            // Get the local node ID (assuming it's stored somewhere or can be retrieved)
            std::string nodeId = "local"; // Replace with actual node ID retrieval

            // Recursively scan the directory
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                rootFolder, std::filesystem::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    std::string filePath = entry.path().string();
                    std::error_code ec;
                    long long fileSize = static_cast<long long>(std::filesystem::file_size(entry.path(), ec));

                    if (ec) {
                        // Handle file size error
                        continue;
                    }

                    // Get last modified time
                    auto lastModified = std::filesystem::last_write_time(entry.path(), ec);
                    if (ec) {
                        // Handle last modified time error
                        continue;
                    }

                    // Convert time_point to string
                    auto timePoint = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        lastModified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                    auto timeT = std::chrono::system_clock::to_time_t(timePoint);
                    std::string timeStr = std::ctime(&timeT);
                    if (!timeStr.empty() && timeStr.back() == '\n') {
                        timeStr.pop_back(); // Remove trailing newline
                    }

                    // Generate metadata and add file to database
                    std::string metadata = GenerateMetadata(filePath, fileSize, timeStr);
                    QuantaDb::I().AddFile(filePath, fileSize, nodeId, metadata);
                }
            }
        }
        catch (const std::exception& ex) {
            // Log error
            m_isScanning = false;
            return false;
        }

        m_isScanning = false;
        return true;
    }

    X::Value DfsEngine::Query(std::string filePattern)
    {
        // Use the database to query files based on the pattern
        return QuantaDb::I().QueryFilesByPath(filePattern);
    }
}