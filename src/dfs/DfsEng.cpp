#include "DfsEng.h"
#include "port.h"
#include <filesystem>
#include <chrono>
#include <ctime>
#include <regex>
#include "QuantaDb.h"
#include "log.h"

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

    bool DfsEngine::Scan(std::string rootFolder, 
        const std::vector<std::string>& excludeFolders, 
        bool skipHidden)
    {
        if (m_isScanning) {
            return false; // Already scanning
        }

        m_isScanning = true;

        // Normalize root folder path (fixing the const issue)
        std::filesystem::path normalizedRoot = std::filesystem::path(rootFolder);
        normalizedRoot.make_preferred();
        m_lastRootFolder = normalizedRoot.string();

        // Default excluded folders if not specified
        std::vector<std::string> foldersToExclude = excludeFolders;
        if (foldersToExclude.empty()) {
            foldersToExclude = { ".git", "$RECYCLE.BIN", "System Volume Information" };
        }

        // Get the local node ID
        std::string nodeId = "local"; // Replace with actual node ID retrieval

        // Start recursive scanning
        ScanFolder(normalizedRoot.string(), nodeId, foldersToExclude, skipHidden);

        m_isScanning = false;
        return true;
    }

    void DfsEngine::ScanFolder(const std::string& folderPath, const std::string& nodeId,
        const std::vector<std::string>& excludeFolders, bool skipHidden)
    {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
                try {
                    // Create a normalized path (can't use make_preferred on const path)
                    std::filesystem::path normalizedPath = entry.path();
                    normalizedPath.make_preferred();
                    std::string entryPath = normalizedPath.string();
                    std::string entryFilename = normalizedPath.filename().string();

                    // Check if this is a hidden file/folder
                    bool isHidden = false;
                    if (skipHidden) {
#ifdef _WIN32
                        DWORD attributes = GetFileAttributesA(entryPath.c_str());
                        isHidden = (attributes != INVALID_FILE_ATTRIBUTES) &&
                            (attributes & FILE_ATTRIBUTE_HIDDEN);
#else
                        isHidden = !entryFilename.empty() && entryFilename[0] == '.';
#endif
                    }

                    if (isHidden) {
                        continue; // Skip hidden files/folders
                    }

                    // Handle directory
                    if (entry.is_directory()) {
                        // Check if this folder should be excluded
                        bool shouldExclude = false;
                        for (const auto& excludeFolder : excludeFolders) {
                            if (entryFilename == excludeFolder) {
                                shouldExclude = true;
                                break;
                            }
                        }

                        if (!shouldExclude) {
                            // Recursively scan this directory
                            ScanFolder(entryPath, nodeId, excludeFolders, skipHidden);
                        }
                    }
                    // Handle file
                    else if (entry.is_regular_file()) {
                        std::error_code ec;
                        long long fileSize = static_cast<long long>(std::filesystem::file_size(normalizedPath, ec));

                        if (ec) {
                            // Handle file size error, but continue scanning
                            continue;
                        }

                        // Get last modified time
                        auto lastModified = std::filesystem::last_write_time(normalizedPath, ec);
                        if (ec) {
                            // Handle last modified time error, but continue scanning
                            continue;
                        }

                        // Convert time_point to string
                        auto timePoint = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            lastModified - std::filesystem::file_time_type::clock::now() +
                            std::chrono::system_clock::now());
                        auto timeT = std::chrono::system_clock::to_time_t(timePoint);
                        std::string timeStr = std::ctime(&timeT);
                        if (!timeStr.empty() && timeStr.back() == '\n') {
                            timeStr.pop_back(); // Remove trailing newline
                        }

                        // Generate metadata and add file to database
                        std::string metadata = GenerateMetadata(entryPath, fileSize, timeStr);
                        // Replace backslashes with forward slashes on Windows
                        std::string standardizedPath = entryPath;
#ifdef _WIN32
                        std::replace(standardizedPath.begin(), standardizedPath.end(), '\\', '/');
#endif
                        LOG << "File:" << standardizedPath << ", size: " << fileSize << LINE_END;
                        QuantaDb::I().AddFile(standardizedPath, fileSize, nodeId, metadata);
                    }
                }
                catch (const std::exception& ex) {
                    // Log the exception for this entry, but continue with next entry
                    // Consider adding more detailed logging here
                }
            }
        }
        catch (const std::exception& ex) {
            // Log the exception for this folder, but don't abort the entire scan
            // Consider adding more detailed logging here
        }
    }

    X::Value DfsEngine::Query(std::string filePattern)
    {
        // Use the database to query files based on the pattern
        return QuantaDb::I().QueryFilesByPath(filePattern);
    }
}