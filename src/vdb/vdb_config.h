#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <tuple>
#include <filesystem>
#include <mutex>
#include "xpackage.h"

namespace Quanta
{
    namespace fs = std::filesystem;
    class VdbConfig
    {
    public:
        VdbConfig();
        ~VdbConfig();

        // One-time SQLite init
        bool Init(X::Runtime& rt, const fs::path& basePath, const std::string& prefix, std::map<int, std::set<std::string>>& outCustomPartitions);
        bool Close();

        // Config Key/Value Settings
        std::string GetConfig(const std::string& key, const std::string& defaultVal = "");
        void SetConfig(const std::string& key, const std::string& value);
        void SyncConfigMap(std::map<std::string, std::string>& inOutConfigMap);

        // Custom Layout Management
        void SaveCustomPartitionToDB(int index, const std::string& tag);
        
        // Metadata / Bounding Box Queries
        void LoadHighestBucketsMap(std::map<std::string, int>& outBucketsMap);
        
        // Returns: ts_start, ts_end
        bool LoadBucketBounds(const std::string& key, long long& outStart, long long& outEnd);
        bool SaveBucketBounds(const std::string& key, const std::string& tsPartition, int customIndex, int bucketNum, long long tsStart, long long tsEnd);

        long long GetTotalRecordsCount();
        void UpdateTotalRecordsCount(long long totalRecords);
        
        long long GetTotalBucketsCount();
        long long GetMetricValue(const std::string& key);
        void UpdateMetricValue(const std::string& key, long long value);

        // Returns bucket keys overlapping the time window mathematically
        std::vector<std::string> ScanMatchingBuckets(long long tsStartMs, long long tsEndMs, const std::set<int>& customIndices);

    private:
        X::Value m_sqlite;
        X::Value m_configDb;
    };
}
