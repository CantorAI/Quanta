#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include "partitioned_vdb.h" // Needed for Partition and WalRecordHeader structs

namespace fs = std::filesystem;

namespace Quanta
{
    class BucketStorage
    {
    public:
        // Core Resolvers
        static fs::path GetHnswPath(const fs::path& basePath, const std::string& prefix, const std::string& tsPartition, int customIndex, const std::string& bucketStr);
        static fs::path GetVdbPath(const fs::path& basePath, const std::string& prefix, const std::string& tsPartition, int customIndex, const std::string& bucketStr);
        
        // WAL Methods
        static bool AppendWalRecord(const fs::path& basePath, std::shared_ptr<Partition> p, const std::vector<unsigned long long>& extIds, const std::vector<std::string>& chunkTexts, long long timestampMs, const float* vectors, size_t count, int dimension);
            
        static bool ReadWalFile(const fs::path& walPath, std::vector<char>& outBuffer);
        
        // Final Physical Bucket Methods
        static bool SavePhysicalBucket(const fs::path& basePath, const std::string& prefix, std::shared_ptr<Partition> p, const std::string& key);
    };
}
