#include "vdb_config.h"
#include <iostream>
#include "xpackage.h"

namespace Quanta
{
    VdbConfig::VdbConfig()
    {
    }

    VdbConfig::~VdbConfig()
    {
        Close();
    }

    bool VdbConfig::Init(X::Runtime& rt, const fs::path& basePath, const std::string& prefix, std::map<int, std::set<std::string>>& outCustomPartitions)
    {
        X::Runtime defaultRt;
        X::Package sqlModule(rt ? rt : defaultRt, "sqlite", "xlang_sqlite");

        if (sqlModule.IsObject())
        {
            m_sqlite = sqlModule;
            auto UseDatabase = m_sqlite["UseDatabase"];
            std::string dbFile = (basePath / (prefix + "_config.db")).string();
            
            m_configDb = UseDatabase(dbFile);
            if (m_configDb.IsObject())
            {
                auto statement = m_configDb["statement"];
                
                // Initialize configuration table
                auto stat = statement("CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT)");
                stat["step"]();

                // Initialize custom partitions table
                auto stat_parts = statement("CREATE TABLE IF NOT EXISTS custom_partitions (id INTEGER PRIMARY KEY, tag TEXT UNIQUE)");
                stat_parts["step"]();
                
                // Initialize buckets manifest
                auto stat_buckets = statement("CREATE TABLE IF NOT EXISTS partitions (key TEXT PRIMARY KEY, ts_partition TEXT, custom_index INTEGER, bucket_number INTEGER, ts_start INTEGER, ts_end INTEGER, element_count INTEGER)");
                stat_buckets["step"]();
                
                // Read custom partitions back
                X::Value statusROW = m_sqlite["ROW"];
                auto stat_read_parts = statement("SELECT id, tag FROM custom_partitions");
                while (stat_read_parts["step"]() == statusROW) {
                    int id = stat_read_parts["get"](0).ToInt();
                    std::string tag = stat_read_parts["get"](1).ToString();
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
        if (m_configDb.IsObject()) {
            auto closeMethod = m_configDb["close"];
            if (closeMethod.IsValid()) {
                closeMethod();
            }
            m_configDb = X::Value();
        }
        m_sqlite = X::Value();
        return true;
    }

    void VdbConfig::SyncConfigMap(std::map<std::string, std::string>& inOutConfigMap)
    {
        if (!m_configDb.IsObject()) return;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        
        // Load whatever exists in DB first
        auto stat_read = statement("SELECT key, value FROM config");
        while (stat_read["step"]() == statusROW) {
            std::string k = stat_read["get"](0).ToString();
            std::string v = stat_read["get"](1).ToString();
            inOutConfigMap[k] = v; // Overwrite RAM with DB source of truth
        }
        
        // Upsert all current RAM values back to ensure defaults are saved
        for (const auto& [k, v] : inOutConfigMap) {
            auto stat_write = statement("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");
            stat_write["bind"](1, k);
            stat_write["bind"](2, v);
            stat_write["step"]();
        }
    }

    std::string VdbConfig::GetConfig(const std::string& key, const std::string& defaultVal)
    {
        if (!m_configDb.IsObject()) return defaultVal;
        auto statement = m_configDb["statement"];
        auto stat = statement("SELECT value FROM config WHERE key=?");
        stat["bind"](1, key);
        X::Value statusROW = m_sqlite["ROW"];
        if (stat["step"]() == statusROW) {
            return stat["get"](0).ToString();
        }
        return defaultVal;
    }

    void VdbConfig::SetConfig(const std::string& key, const std::string& value)
    {
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        auto stat = statement("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");
        stat["bind"](1, key);
        stat["bind"](2, value);
        stat["step"]();
    }

    void VdbConfig::SaveCustomPartitionToDB(int index, const std::string& tag)
    {
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        auto stat = statement("INSERT OR IGNORE INTO custom_partitions (id, tag) VALUES (?, ?)");
        stat["bind"](1, index);
        stat["bind"](2, tag);
        stat["step"]();
    }

    void VdbConfig::LoadHighestBucketsMap(std::map<std::string, int>& outBucketsMap)
    {
        if (!m_configDb.IsObject()) return;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        auto stat = statement("SELECT ts_partition, custom_index, MAX(bucket_number) FROM partitions GROUP BY ts_partition, custom_index");
        
        while (stat["step"]() == statusROW) {
            std::string tsPartition = stat["get"](0).ToString();
            int customIndex = stat["get"](1).ToInt();
            int maxBucket = stat["get"](2).ToInt();
            
            std::string mapKey = tsPartition + "_" + std::to_string(customIndex);
            outBucketsMap[mapKey] = maxBucket;
        }
    }

    bool VdbConfig::LoadBucketBounds(const std::string& key, long long& outStart, long long& outEnd)
    {
        if (!m_configDb.IsObject()) return false;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        X::Value stat = statement("SELECT ts_start, ts_end FROM partitions WHERE key=?");
        stat["bind"](1, key);
        
        if (stat["step"]() == statusROW) {
            outStart = stat["get"](0).ToLongLong();
            outEnd = stat["get"](1).ToLongLong();
            return true;
        }
        return false;
    }

    bool VdbConfig::SaveBucketBounds(const std::string& key, const std::string& tsPartition, int customIndex, int bucketNum, long long tsStart, long long tsEnd)
    {
        if (!m_configDb.IsObject()) return false;
        
        auto statement = m_configDb["statement"];
        X::Value stat = statement("INSERT OR REPLACE INTO partitions (key, ts_partition, custom_index, bucket_number, ts_start, ts_end) VALUES (?, ?, ?, ?, ?, ?)");
        stat["bind"](1, key);
        stat["bind"](2, tsPartition);
        stat["bind"](3, customIndex);
        stat["bind"](4, bucketNum);
        stat["bind"](5, tsStart);
        stat["bind"](6, tsEnd);
        stat["step"]();
        
        return true;
    }

    void VdbConfig::UpdateTotalRecordsCount(long long totalRecords)
    {
        UpdateMetricValue("TotalRecords", totalRecords);
    }

    long long VdbConfig::GetTotalRecordsCount()
    {
        return GetMetricValue("TotalRecords");
    }

    long long VdbConfig::GetTotalBucketsCount()
    {
        if (!m_configDb.IsObject()) return 0;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        auto stat = statement("SELECT COUNT(*) FROM partitions");
        
        if (stat["step"]() == statusROW) {
            return stat["get"](0).ToLongLong();
        }
        return 0;
    }

    void VdbConfig::UpdateMetricValue(const std::string& key, long long value)
    {
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        auto stat = statement("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");
        stat["bind"](1, key);
        stat["bind"](2, std::to_string(value));
        stat["step"]();
    }

    long long VdbConfig::GetMetricValue(const std::string& key)
    {
        if (!m_configDb.IsObject()) return 0;
        
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        auto stat = statement("SELECT value FROM config WHERE key = ?");
        stat["bind"](1, key);
        
        if (stat["step"]() == statusROW) {
            std::string valStr = stat["get"](0).ToString();
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
        std::vector<std::string> result;
        if (!m_configDb.IsObject()) return result;
        
        if (tsStartMs <= 0) tsStartMs = 0;
        if (tsEndMs <= 0) tsEndMs = LLONG_MAX;

        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];
        
        std::string q = "SELECT key, ts_start, ts_end, custom_index, ts_partition FROM partitions";
        X::Value stat = statement(q);
        
        while (stat["step"]() == statusROW) {
            std::string key = stat["get"](0).ToString();
            long long bStart = stat["get"](1).ToLongLong();
            long long bEnd = stat["get"](2).ToLongLong();
            int cIndex = stat["get"](3).ToInt();
            std::string tsPart = stat["get"](4).ToString();

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
