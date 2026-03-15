#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include "xpackage.h"

namespace fs = std::filesystem;

namespace Quanta
{
    // WAL Binary Record Format
    #pragma pack(push, 1)
    struct WalRecordHeader {
        unsigned long long external_id;
        unsigned long long timestamp_ms;
        unsigned int chunk_text_length;
    };
    #pragma pack(pop)
    class BucketStorage
    {
    public:
        // Core Resolvers
        static fs::path GetHnswPath(const fs::path& basePath, const std::string& prefix, const std::string& tsPartition, int customIndex, const std::string& bucketStr);
        static fs::path GetVdbPath(const fs::path& basePath, const std::string& prefix, const std::string& tsPartition, int customIndex, const std::string& bucketStr);
        
        // WAL Methods
        static bool AppendWalRecord(const fs::path& basePath, const std::string& active_wal_filename, const std::vector<unsigned long long>& extIds, const std::vector<std::string>& chunkTexts, long long timestampMs, const float* vectors, size_t count, int dimension);
            
        static bool ReadWalFile(const fs::path& walPath, std::vector<char>& outBuffer);
    };
}
