#include "vdb_config.h"
#include <iostream>
#include <climits>
#include "quanta_runtime.h"

namespace Quanta
{
    VdbConfig::VdbConfig()
    {
    }

    VdbConfig::~VdbConfig()
    {
        try { Close(); } catch (...) {}
    }

    bool VdbConfig::Init(X3PackageHost* host, const fs::path& basePath, const std::string& prefix, std::map<int, std::set<std::string>>& outCustomPartitions)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto sqlModule = Import(host, "xlang_sqlite3", "sqlite");

        if (sqlModule.IsObject())
        {
            m_sqlite = sqlModule;
            auto UseDatabase = m_sqlite["Database"];
            std::string dbFile = (basePath / (prefix + "_config.db")).string();
            
            m_configDb = Invoke(UseDatabase, dbFile);
            if (m_configDb.IsObject())
            {
                auto statement = m_configDb["statement"];
                
                // Initialize configuration table
                auto stat = Invoke(statement, "CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT)");
                Invoke(stat["step"]);

                // Initialize custom partitions table
                auto stat_parts = Invoke(statement, "CREATE TABLE IF NOT EXISTS custom_partitions (id INTEGER PRIMARY KEY, tag TEXT UNIQUE)");
                Invoke(stat_parts["step"]);
                auto aliases = Invoke(statement, "CREATE TABLE IF NOT EXISTS custom_partition_tags (id INTEGER NOT NULL, tag TEXT UNIQUE, PRIMARY KEY(id, tag))");
                Invoke(aliases["step"]);
                auto migrate = Invoke(statement, "INSERT OR IGNORE INTO custom_partition_tags SELECT id, tag FROM custom_partitions");
                Invoke(migrate["step"]);
                
                // Initialize buckets manifest
                auto stat_buckets = Invoke(statement, "CREATE TABLE IF NOT EXISTS partitions (key TEXT PRIMARY KEY, ts_partition TEXT, custom_index INTEGER, bucket_number INTEGER, ts_start INTEGER, ts_end INTEGER, element_count INTEGER)");
                Invoke(stat_buckets["step"]);
                
                // Read custom partitions back
                X::Value statusROW = m_sqlite["ROW"];
                auto stat_read_parts = Invoke(statement, "SELECT id, tag FROM custom_partition_tags");
                while (Invoke(stat_read_parts["step"]) == statusROW) {
                    int id = Invoke(stat_read_parts["get"], 0).ToLongLong();
                    std::string tag = Invoke(stat_read_parts["get"], 1).ToString();
                    outCustomPartitions[id].insert(tag);
                }
                
                return true;
            }
        }
        else {
            // failed to load
        }
        return false;
    }

    bool VdbConfig::Close()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (m_configDb.IsObject()) {
            auto closeMethod = m_configDb["close"];
            if (closeMethod.IsValid()) {
                Invoke(closeMethod);
            }
            m_configDb = X::Value();
        }
        m_sqlite = X::Value();
        return true;
    }

    void VdbConfig::LoadConfigToMap(std::map<std::string, std::string>& outConfigMap)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        
        auto stat_read = Invoke(statement, "SELECT key, value FROM config");
        while (Invoke(stat_read["step"]) == statusROW) {
            std::string k = Invoke(stat_read["get"], 0).ToString();
            std::string v = Invoke(stat_read["get"], 1).ToString();
            outConfigMap[k] = v;
        }
    }

    void VdbConfig::SaveConfigFromMap(const std::map<std::string, std::string>& inConfigMap)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return;
        
        auto statement = m_configDb["statement"];
        for (const auto& [k, v] : inConfigMap) {
            auto stat_write = Invoke(statement, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");
            Invoke(stat_write["bind"], 1, k);
            Invoke(stat_write["bind"], 2, v);
            Invoke(stat_write["step"]);
        }
    }

    std::string VdbConfig::GetConfig(const std::string& key, const std::string& defaultVal)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return defaultVal;
        auto statement = m_configDb["statement"];
        auto stat = Invoke(statement, "SELECT value FROM config WHERE key=?");
        Invoke(stat["bind"], 1, key);
        X::Value statusROW = m_sqlite["ROW"];
        if (Invoke(stat["step"]) == statusROW) {
            return Invoke(stat["get"], 0).ToString();
        }
        return defaultVal;
    }

    void VdbConfig::SetConfig(const std::string& key, const std::string& value)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        auto stat = Invoke(statement, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");
        Invoke(stat["bind"], 1, key);
        Invoke(stat["bind"], 2, value);
        Invoke(stat["step"]);
    }

    void VdbConfig::SaveCustomPartitionToDB(int index, const std::string& tag)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        auto stat = Invoke(statement, "INSERT OR IGNORE INTO custom_partition_tags (id, tag) VALUES (?, ?)");
        Invoke(stat["bind"], 1, index);
        Invoke(stat["bind"], 2, tag);
        Invoke(stat["step"]);
    }

    void VdbConfig::LoadHighestBucketsMap(std::map<std::string, int>& outBucketsMap)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        auto stat = Invoke(statement, "SELECT ts_partition, custom_index, MAX(bucket_number) FROM partitions GROUP BY ts_partition, custom_index");
        
        while (Invoke(stat["step"]) == statusROW) {
            std::string tsPartition = Invoke(stat["get"], 0).ToString();
            int customIndex = Invoke(stat["get"], 1).ToLongLong();
            int maxBucket = Invoke(stat["get"], 2).ToLongLong();
            
            std::string mapKey = tsPartition + "_" + std::to_string(customIndex);
            outBucketsMap[mapKey] = maxBucket;
        }
    }

    bool VdbConfig::LoadBucketBounds(const std::string& key, long long& outStart, long long& outEnd)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return false;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        X::Value stat = Invoke(statement, "SELECT ts_start, ts_end FROM partitions WHERE key=?");
        Invoke(stat["bind"], 1, key);
        
        if (Invoke(stat["step"]) == statusROW) {
            outStart = Invoke(stat["get"], 0).ToLongLong();
            outEnd = Invoke(stat["get"], 1).ToLongLong();
            return true;
        }
        return false;
    }

    bool VdbConfig::SaveBucketBounds(const std::string& key, const std::string& tsPartition, int customIndex, int bucketNum, long long tsStart, long long tsEnd)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return false;
        
        auto statement = m_configDb["statement"];
        X::Value stat = Invoke(statement, "INSERT OR REPLACE INTO partitions (key, ts_partition, custom_index, bucket_number, ts_start, ts_end) VALUES (?, ?, ?, ?, ?, ?)");
        Invoke(stat["bind"], 1, key);
        Invoke(stat["bind"], 2, tsPartition);
        Invoke(stat["bind"], 3, customIndex);
        Invoke(stat["bind"], 4, bucketNum);
        Invoke(stat["bind"], 5, tsStart);
        Invoke(stat["bind"], 6, tsEnd);
        Invoke(stat["step"]);
        
        return true;
    }

    void VdbConfig::UpdateTotalRecordsCount(long long totalRecords)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        UpdateMetricValue("TotalRecords", totalRecords);
    }

    long long VdbConfig::GetTotalRecordsCount()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return GetMetricValue("TotalRecords");
    }

    long long VdbConfig::GetTotalBucketsCount()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return 0;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        auto stat = Invoke(statement, "SELECT COUNT(*) FROM partitions");
        
        if (Invoke(stat["step"]) == statusROW) {
            return Invoke(stat["get"], 0).ToLongLong();
        }
        return 0;
    }

    void VdbConfig::UpdateMetricValue(const std::string& key, long long value)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        auto stat = Invoke(statement, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");
        Invoke(stat["bind"], 1, key);
        Invoke(stat["bind"], 2, std::to_string(value));
        Invoke(stat["step"]);
    }

    long long VdbConfig::GetMetricValue(const std::string& key)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!m_configDb.IsObject()) return 0;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        auto stat = Invoke(statement, "SELECT value FROM config WHERE key = ?");
        Invoke(stat["bind"], 1, key);
        
        if (Invoke(stat["step"]) == statusROW) {
            std::string valStr = Invoke(stat["get"], 0).ToString();
            try {
                return std::stoll(valStr);
            } catch (...) {
                return 0;
            }
        }
        return 0;
    }

    std::vector<std::string> VdbConfig::ScanMatchingBuckets(long long tsStartMs, long long tsEndMs, const std::set<int>& customIndices)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::vector<std::string> result;
        if (!m_configDb.IsObject()) return result;
        
        if (tsStartMs <= 0) tsStartMs = 0;
        if (tsEndMs <= 0) tsEndMs = LLONG_MAX;

        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        
        std::string q = "SELECT key, ts_start, ts_end, custom_index, ts_partition FROM partitions";
        X::Value stat = Invoke(statement, q);
        
        while (Invoke(stat["step"]) == statusROW) {
            std::string key = Invoke(stat["get"], 0).ToString();
            long long bStart = Invoke(stat["get"], 1).ToLongLong();
            long long bEnd = Invoke(stat["get"], 2).ToLongLong();
            int cIndex = Invoke(stat["get"], 3).ToLongLong();
            std::string tsPart = Invoke(stat["get"], 4).ToString();

            if (!customIndices.empty() && customIndices.find(cIndex) == customIndices.end()) continue;
            
            if (bStart > 0 && bEnd > 0) {
                if (bStart > tsEndMs || bEnd < tsStartMs) continue;
            } else {
                // Approximate from tsPartition name if bounds missing in DB (legacy)
                long long partStartMs = 0;
                long long partEndMs = LLONG_MAX;
                // Assuming standard naming convention like 2024-03-14-12 (hour resolution)
                // If it fails to parse, it just includes it naturally
                if (bStart <= 0 && bEnd <= 0) {
                    // Pass
                }
            }
            
            result.push_back(key);
        }
        return result;
    }
}
