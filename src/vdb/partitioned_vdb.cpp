#include "partitioned_vdb.h"
#include <ctime>
#include <algorithm>
#include <fstream> // Added for file operations
#include <climits>
#include <functional>
#include "HnswVdb.h"
#include "VectorDatabase.h"
#include <chrono>
#include <chrono>
#include "QuantaHost.h"
#include "bucket_storage.h"

namespace Quanta
{

    float quanta_cosine_similarity(int dimension, const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() < static_cast<size_t>(dimension) || b.size() < static_cast<size_t>(dimension)) {
            return 0.0f;
        }

        float dot = 0.0f, normA = 0.0f, normB = 0.0f;
        for (int i = 0; i < dimension; ++i) {
            dot += a[i] * b[i];
            normA += a[i] * a[i];
            normB += b[i] * b[i];
        }
        if (normA == 0.0f || normB == 0.0f) return 0.0f;
        return dot / (std::sqrt(normA) * std::sqrt(normB));
    }

    // Dedup results within a partition using parallel processing
    // Returns indices to keep (sorted by score descending)
    std::vector<size_t> dedup_results(
        const std::vector<std::pair<unsigned long long, float>>& results,
        const std::vector<std::vector<float>>& vectors,
        int dimension,
        float threshold)
    {
        size_t n = results.size();
        if (n <= 1) {
            std::vector<size_t> indices(n);
            std::iota(indices.begin(), indices.end(), 0);
            return indices;
        }

        // Create sorted indices by score (descending)
        std::vector<size_t> sortedIdx(n);
        std::iota(sortedIdx.begin(), sortedIdx.end(), 0);
        std::sort(sortedIdx.begin(), sortedIdx.end(),
            [&results](size_t a, size_t b) {
                return results[a].second > results[b].second;
            });

        // Mark removed items
        std::vector<bool> removed(n, false);

        // Compare each item against higher-scored items
#pragma omp parallel for schedule(dynamic)
        for (long long ii = 1; ii < static_cast<long long>(n); ++ii) {
            size_t i = sortedIdx[ii];
            if (removed[i] || vectors[i].empty()) continue;

            const auto& vec_i = vectors[i];

            for (long long jj = 0; jj < ii; ++jj) {
                size_t j = sortedIdx[jj];
                if (removed[j] || vectors[j].empty()) continue;

                const auto& vec_j = vectors[j];
                float sim = quanta_cosine_similarity(dimension, vec_i, vec_j);

                if (sim >= threshold) {
                    removed[i] = true;
                    break;
                }
            }
        }

        // Collect kept indices (in sorted order)
        std::vector<size_t> kept;
        kept.reserve(n);
        for (size_t idx : sortedIdx) {
            if (!removed[idx]) {
                kept.push_back(idx);
            }
        }
        return kept;
    }


    // ============================================================================
    // Constructor / Destructor
    // ============================================================================

    PartitionedVdb::PartitionedVdb(X::ARGS& params, X::KWARGS& kwParams)
    {
        // Load SQLite package FIRST (required by Init -> InitDatabase)
        X::Runtime rt;
        X::Package sqlite(rt, "sqlite", "xlang_sqlite");
        m_sqlite = sqlite;

        // If params/kwargs provided, call Init
        if (params.size() > 0 || kwParams.size() > 0) {
            X::Value retValue;
            Init(nullptr, nullptr, params, kwParams, retValue);
        }

        // Initialize default partition (index 0)
        customPartitionTags_[0].insert("default");
        tagToIndex_["default"] = 0;
    }

    PartitionedVdb::~PartitionedVdb()
    {
        Close();
        
        std::lock_guard<std::recursive_mutex> lock(partitions_mutex_);
        partitions_.clear();
    }

    // ============================================================================
    // Config Helpers
    // ============================================================================

    void PartitionedVdb::SetConfig(const std::string& key, const std::string& value)
    {
        config_[key] = value;
    }

    std::string PartitionedVdb::GetConfig(const std::string& key, const std::string& defaultVal)
    {
        auto it = config_.find(key);
        return (it != config_.end()) ? it->second : defaultVal;
    }

    void PartitionedVdb::ApplyConfigToMembers()
    {
        prefix_ = GetConfig("prefix", "vdb");
        tsGranularity_ = GetConfig("granularity", "daily");
        dimension_ = std::stoi(GetConfig("dimension", "512"));
        spaceName_ = GetConfig("space", "l2");
        maxMemoryGb_ = std::stof(GetConfig("max_memory_gb", "1.0"));
        M_ = std::stoi(GetConfig("M", "16"));
        efConstruction_ = std::stoi(GetConfig("ef_construction", "200"));
        efSearch_ = std::stoi(GetConfig("ef_search", "50"));
        nextCustomIndex_ = std::stoi(GetConfig("next_custom_index", "1"));
        ttl_minutes_ = std::stoll(GetConfig("ttl_minutes", "60"));
        auto_save_seconds_ = std::stoll(GetConfig("auto_save_seconds", "300"));
        max_loaded_read_only_partitions_ = std::stoi(GetConfig("max_loaded_read_only_partitions", "50"));
        wal_cooling_time_seconds_ = std::stoll(GetConfig("wal_cooling_time_seconds", "60"));
    }

    void PartitionedVdb::UpdateConfigFromMembers()
    {
        SetConfig("prefix", prefix_);
        SetConfig("granularity", tsGranularity_);
        SetConfig("dimension", std::to_string(dimension_));
        SetConfig("space", spaceName_);
        SetConfig("max_memory_gb", std::to_string(maxMemoryGb_));
        SetConfig("M", std::to_string(M_));
        SetConfig("ef_construction", std::to_string(efConstruction_));
        SetConfig("ef_search", std::to_string(efSearch_));
        SetConfig("next_custom_index", std::to_string(nextCustomIndex_));
        SetConfig("ttl_minutes", std::to_string(ttl_minutes_));
        SetConfig("auto_save_seconds", std::to_string(auto_save_seconds_));
        SetConfig("max_loaded_read_only_partitions", std::to_string(max_loaded_read_only_partitions_));
    }

    // ============================================================================
    // SQLite Operations
    // ============================================================================

    void PartitionedVdb::InitDatabase()
    {
        std::string dbPath = GetDbPath().string();
        X::Value db = m_sqlite["Database"](dbPath);
        m_configDb = db;
        auto execSQL = db["exec"];
        auto statement = db["statement"];
        X::Value statusROW = m_sqlite["ROW"];

        // Create config table
        X::Value checkStat = statement("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='config'");
        bool hasConfigTable = false;
        if (checkStat["step"]() == statusROW) {
            hasConfigTable = (checkStat["get"](0).ToInt() > 0);
        }

        if (!hasConfigTable) {
            execSQL("CREATE TABLE config (key TEXT PRIMARY KEY, value TEXT)");
        }

        // Create custom_partitions table
        checkStat = statement("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='custom_partitions'");
        bool hasPartitionsTable = false;
        if (checkStat["step"]() == statusROW) {
            hasPartitionsTable = (checkStat["get"](0).ToInt() > 0);
        }

        if (!hasPartitionsTable) {
            execSQL("CREATE TABLE custom_partitions (partition_index INTEGER, tag TEXT PRIMARY KEY)");
            execSQL("INSERT INTO custom_partitions (partition_index, tag) VALUES (0, 'default')");
        }

        // Create buckets table for Tier 3 spilling
        checkStat = statement("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='buckets'");
        bool hasBucketsTable = false;
        if (checkStat["step"]() == statusROW) {
            hasBucketsTable = (checkStat["get"](0).ToInt() > 0);
        }

        if (!hasBucketsTable) {
            execSQL("CREATE TABLE IF NOT EXISTS buckets (key TEXT PRIMARY KEY, ts_partition TEXT, custom_index INTEGER, bucket_number INTEGER, ts_start INTEGER, ts_end INTEGER)");
        } else {
            X::Value bucketCounterStat = statement("SELECT COUNT(*) FROM buckets");
            if (bucketCounterStat["step"]() == statusROW) {
                total_buckets_ = bucketCounterStat["get"](0).ToLongLong();
            }
        }
    }

    void PartitionedVdb::SyncConfigToDB()
    {
        auto statement = m_configDb["statement"];

        X::Value stat = statement("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)");

        for (const auto& [key, value] : config_) {
            stat["bind"](1, key);
            stat["bind"](2, value);
            stat["step"]();
            stat["reset"]();
        }
    }

    void PartitionedVdb::LoadConfigFromDB()
    {
        if (!m_configDb.IsObject()) return;
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];

        config_.clear();

        X::Value stat = statement("SELECT key, value FROM config");
        while (stat["step"]() == statusROW) {
            std::string key = stat["get"](0).ToString();
            std::string value = stat["get"](1).ToString();
            config_[key] = value;
        }

        ApplyConfigToMembers();
    }

    void PartitionedVdb::LoadCustomPartitionsFromDB()
    {
        auto statement = m_configDb["statement"];
        X::Value statusROW = m_sqlite["ROW"];

        customPartitionTags_.clear();
        tagToIndex_.clear();

        X::Value stat = statement("SELECT partition_index, tag FROM custom_partitions");
        while (stat["step"]() == statusROW) {
            int idx = stat["get"](0).ToInt();
            std::string tag = stat["get"](1).ToString();
            customPartitionTags_[idx].insert(tag);
            tagToIndex_[tag] = idx;
        }

        // Ensure default exists
        if (customPartitionTags_.find(0) == customPartitionTags_.end()) {
            customPartitionTags_[0].insert("default");
            tagToIndex_["default"] = 0;
        }
    }

    void PartitionedVdb::SaveCustomPartitionToDB(int index, const std::string& tag)
    {
        auto statement = m_configDb["statement"];

        X::Value stat = statement("INSERT OR REPLACE INTO custom_partitions (partition_index, tag) VALUES (?, ?)");
        stat["bind"](1, index);
        stat["bind"](2, tag);
        stat["step"]();
    }

    // ============================================================================
    // File Path Helpers
    // ============================================================================

    fs::path PartitionedVdb::GetDbPath()
    {
        return basePath_ / (prefix_ + "_manifest.db");
    }

    // ============================================================================
    // Timestamp Helpers (all timestamps in milliseconds)
    // ============================================================================

    int PartitionedVdb::CountChar(const std::string& s, char c)
    {
        return static_cast<int>(std::count(s.begin(), s.end(), c));
    }

    std::string PartitionedVdb::TimestampToPartitionName(long long timestampMs)
    {
        if (timestampMs <= 0) return "default";

        // Convert milliseconds to seconds
        time_t t = static_cast<time_t>(timestampMs / 1000);
        struct tm tm_info;

#ifdef _WIN32
        gmtime_s(&tm_info, &t);
#else
        gmtime_r(&t, &tm_info);
#endif

        char buf[32];

        if (tsGranularity_ == "hourly") {
            // Format: 2024-01-15-14 (3 dashes)
            strftime(buf, sizeof(buf), "%Y-%m-%d-%H", &tm_info);
        }
        else if (tsGranularity_ == "daily") {
            // Format: 2024-01-15 (2 dashes)
            strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_info);
        }
        else if (tsGranularity_ == "weekly") {
            // Format: 2024-W03 (1 dash, has 'W')
            strftime(buf, sizeof(buf), "%Y-W%W", &tm_info);
        }
        else if (tsGranularity_ == "monthly") {
            // Format: 2024-01 (1 dash, no 'W')
            strftime(buf, sizeof(buf), "%Y-%m", &tm_info);
        }
        else {
            // yearly: 2024 (0 dashes)
            strftime(buf, sizeof(buf), "%Y", &tm_info);
        }

        return std::string(buf);
    }

    std::pair<long long, long long> PartitionedVdb::PartitionNameToTimeRange(const std::string& tsPartition)
    {
        if (tsPartition == "default") {
            return { 0, LLONG_MAX };
        }

        int dashCount = CountChar(tsPartition, '-');
        bool hasW = (tsPartition.find('W') != std::string::npos);

        struct tm tm_start = {};
        struct tm tm_end = {};
        tm_start.tm_mday = 1;
        tm_end.tm_mday = 1;

        if (dashCount == 3) {
            // Hourly: 2024-01-15-14
            int year, month, day, hour;
            sscanf(tsPartition.c_str(), "%d-%d-%d-%d", &year, &month, &day, &hour);
            tm_start.tm_year = year - 1900;
            tm_start.tm_mon = month - 1;
            tm_start.tm_mday = day;
            tm_start.tm_hour = hour;
            tm_end = tm_start;
            tm_end.tm_hour += 1;
        }
        else if (dashCount == 2) {
            // Daily: 2024-01-15
            int year, month, day;
            sscanf(tsPartition.c_str(), "%d-%d-%d", &year, &month, &day);
            tm_start.tm_year = year - 1900;
            tm_start.tm_mon = month - 1;
            tm_start.tm_mday = day;
            tm_end = tm_start;
            tm_end.tm_mday += 1;
        }
        else if (dashCount == 1 && hasW) {
            // Weekly: 2024-W03
            int year, week;
            sscanf(tsPartition.c_str(), "%d-W%d", &year, &week);
            tm_start.tm_year = year - 1900;
            tm_start.tm_mon = 0;
            tm_start.tm_mday = 1 + (week * 7);
            tm_end = tm_start;
            tm_end.tm_mday += 7;
        }
        else if (dashCount == 1) {
            // Monthly: 2024-01
            int year, month;
            sscanf(tsPartition.c_str(), "%d-%d", &year, &month);
            tm_start.tm_year = year - 1900;
            tm_start.tm_mon = month - 1;
            tm_end = tm_start;
            tm_end.tm_mon += 1;
        }
        else {
            // Yearly: 2024
            int year;
            sscanf(tsPartition.c_str(), "%d", &year);
            tm_start.tm_year = year - 1900;
            tm_start.tm_mon = 0;
            tm_end = tm_start;
            tm_end.tm_year += 1;
        }

#ifdef _WIN32
        long long startSec = static_cast<long long>(_mkgmtime(&tm_start));
        long long endSec = static_cast<long long>(_mkgmtime(&tm_end));
#else
        long long startSec = static_cast<long long>(timegm(&tm_start));
        long long endSec = static_cast<long long>(timegm(&tm_end));
#endif

        // Return in milliseconds
        return { startSec * 1000, endSec * 1000 };
    }

    // ============================================================================
    // Partition Helpers
    // ============================================================================

    int PartitionedVdb::GetOrCreateCustomIndex(const std::string& tag)
    {
        if (tag.empty() || tag == "default") {
            return 0;
        }

        auto it = tagToIndex_.find(tag);
        if (it != tagToIndex_.end()) {
            return it->second;
        }

        int idx = nextCustomIndex_++;
        customPartitionTags_[idx].insert(tag);
        tagToIndex_[tag] = idx;

        // Persist to DB
        SaveCustomPartitionToDB(idx, tag);
        SetConfig("next_custom_index", std::to_string(nextCustomIndex_));
        SyncConfigToDB();

        return idx;
    }

    int PartitionedVdb::GetCustomIndex(const std::string& tag)
    {
        if (tag.empty() || tag == "default") return 0;
        auto it = tagToIndex_.find(tag);
        return (it != tagToIndex_.end()) ? it->second : -1;
    }

    std::set<int> PartitionedVdb::ResolveTagsToIndices(const std::vector<std::string>& tags)
    {
        std::set<int> indices;
        for (const auto& tag : tags) {
            int idx = GetCustomIndex(tag);
            if (idx >= 0) {
                indices.insert(idx);
            }
        }
        return indices;
    }

    bool PartitionedVdb::AddPartitionTag(int index, const std::string& tag)
    {
        if (customPartitionTags_.find(index) == customPartitionTags_.end()) {
            return false;
        }
        if (tagToIndex_.find(tag) != tagToIndex_.end()) {
            return false;
        }

        customPartitionTags_[index].insert(tag);
        tagToIndex_[tag] = index;

        SaveCustomPartitionToDB(index, tag);
        return true;
    }

    std::shared_ptr<Partition> PartitionedVdb::GetOrCreatePartition(const std::string& tsPartition, int customIndex)
    {
        // 1. Find the highest bucket_number for this tsPartition and customIndex in SQLite.
        int activeBucketNum = 0;
        if (m_configDb.IsObject()) {
            auto statement = m_configDb["statement"];
            X::Value statusROW = m_sqlite["ROW"];
            X::Value stat = statement("SELECT MAX(bucket_number) FROM buckets WHERE ts_partition=? AND custom_index=?");
            stat["bind"](1, tsPartition);
            stat["bind"](2, customIndex);
            if (stat["step"]() == statusROW) {
                X::Value val = stat["get"](0);
                if (val.IsValid() && val.IsObject()) {
                    activeBucketNum = val.ToInt();
                }
            }
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%04d", activeBucketNum);
        std::string bucketStr = buf;

        std::string key = tsPartition + "_" + std::to_string(customIndex) + "_" + bucketStr;

        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::lock_guard<std::recursive_mutex> lock(partitions_mutex_);
        
        while (true) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d", activeBucketNum);
            std::string bucketStr = buf;
            std::string key = tsPartition + "_" + std::to_string(customIndex) + "_" + bucketStr;

            auto it = partitions_.find(key);
            if (it != partitions_.end()) {
                if (it->second->count >= maxElements_) {
                    if (!it->second->is_historical_read_) {
                        SavePartition(it->second, key);
                        it->second->is_historical_read_ = true; 
                        // DO NOT ERASE from RAM yet! Allow TTL MaintenanceLoop to organically prune it 
                        // after the WAL thread merges and updates the physical disk size.
                    }
                    activeBucketNum++;
                    continue; 
                } else {
                    it->second->last_access_ms_ = now_ms;
                    return it->second;
                }
            }

            fs::path hnswPath = BucketStorage::GetHnswPath(basePath_, prefix_, tsPartition, customIndex, bucketStr);
            fs::path vdbPath = BucketStorage::GetVdbPath(basePath_, prefix_, tsPartition, customIndex, bucketStr);

            if (fs::exists(hnswPath) && fs::exists(vdbPath)) {
                auto partition = std::make_shared<Partition>();
                partition->vdb = std::make_unique<VectorDatabase>(dimension_);
                partition->vdb->Load(vdbPath.string());
                partition->count = partition->vdb->GetSize();
                
                if (partition->count >= maxElements_) {
                     activeBucketNum++;
                     continue;
                }
                
                partition->index = std::make_unique<HnswVdb>(
                    spaceName_, dimension_, maxElements_, M_, efConstruction_, efSearch_);
                partition->index->Load(hnswPath.string());
                
                if (m_configDb.IsObject()) {
                    auto statement = m_configDb["statement"];
                    X::Value statusROW = m_sqlite["ROW"];
                    X::Value stat = statement("SELECT ts_start, ts_end FROM buckets WHERE key=?");
                    stat["bind"](1, key);
                    if (stat["step"]() == statusROW) {
                        partition->ts_start_ = stat["get"](0).ToLongLong();
                        partition->ts_end_ = stat["get"](1).ToLongLong();
                    }
                }
                partition->is_historical_read_ = false; 

                partition->key_ = key; // Added this line
                partition->is_dirty_ = true;
                partition->last_access_ms_ = now_ms;
                partition->last_save_ms_ = now_ms;
                partitions_[key] = std::move(partition);
                return partitions_[key];
            }

            total_buckets_++;
            total_buckets_++;
            auto partition = std::make_unique<Partition>();
            partition->vdb = std::make_unique<VectorDatabase>(dimension_);
            partition->index = std::make_unique<HnswVdb>(
                spaceName_, dimension_, maxElements_, M_, efConstruction_, efSearch_);
            partition->is_historical_read_ = false;

            partition->key_ = key;
            partition->is_dirty_ = true;
            partition->last_access_ms_ = now_ms;
            partition->last_save_ms_ = now_ms;
            partitions_[key] = std::move(partition);
            return partitions_[key];
        }
    }

    std::shared_ptr<Partition> PartitionedVdb::LoadPartition(const std::string& key)
    {

        // Parse key: tsPartition_customIndex_bucketNum
        size_t last_under = key.rfind('_');
        size_t first_under = key.find('_');
        if (first_under == std::string::npos || last_under == std::string::npos || first_under == last_under) return nullptr;

        std::string tsPartition = key.substr(0, last_under); // But wait, it could be `2024-03-14-12_1_0000`, so tsPartition has internal dashes. The first '_' is after tsPartition!
        tsPartition = key.substr(0, first_under);
        
        int customIndex = 0;
        try {
            customIndex = std::stoi(key.substr(first_under + 1, last_under - first_under - 1));
        } catch (const std::exception& e) {

            return nullptr;
        }
        
        std::string bucketStr = key.substr(last_under + 1);

        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::unique_lock<std::recursive_mutex> lock(partitions_mutex_);

        auto it = partitions_.find(key);
        if (it != partitions_.end()) {
            it->second->last_access_ms_ = now_ms;
            return it->second;
        }

        // LRU Read-Only Throttling: Enforce capacity to prevent deep historical queries from bursting RAM boundaries
        int readOnlyCount = 0;
        std::string oldestKey = "";
        long long oldestAccess = LLONG_MAX;
        

        for (const auto& [k, p] : partitions_) {
            if (!p) {

                continue;
            }
            if (p->is_historical_read_) {
                readOnlyCount++;
                if (p->last_access_ms_ < oldestAccess) {
                    oldestAccess = p->last_access_ms_;
                    oldestKey = k;
                }
            }
        }

        if (readOnlyCount >= max_loaded_read_only_partitions_ && !oldestKey.empty()) {

            // We've hit the exact quota for static lookups. Drop the oldest historical bucket out of RAM.
            partitions_.erase(oldestKey);
        }

        fs::path hnswPath = BucketStorage::GetHnswPath(basePath_, prefix_, tsPartition, customIndex, bucketStr);
        fs::path vdbPath = BucketStorage::GetVdbPath(basePath_, prefix_, tsPartition, customIndex, bucketStr);


        if (!fs::exists(hnswPath) || !fs::exists(vdbPath)) {
            return nullptr;
        }

        // Pre-allocate the partition and insert it to prevent concurrent loads
        auto partition = std::make_shared<Partition>();
        partition->vdb = std::make_unique<VectorDatabase>(dimension_);
        partition->index = std::make_unique<HnswVdb>(
            spaceName_, dimension_, maxElements_, M_, efConstruction_, efSearch_);
            
        partition->key_ = key;
        partition->last_access_ms_ = now_ms;
        partition->last_save_ms_ = now_ms;
        
        // Temporarily store empty partition in map
        partitions_[key] = partition;

        // ==========================================================
        // RELEASE GLOBAL MUTEX DURING MASSIVE DISK I/O
        // ==========================================================
        lock.unlock();

        partition->vdb->Load(vdbPath.string());
        partition->index->Load(hnswPath.string());
        
        lock.lock();
        // ==========================================================
        // REAQUIRE MUTEX FOR METADATA FINALIZATION
        // ==========================================================

        partition->count = partition->vdb->GetSize();
        partition->is_historical_read_ = true; // explicitly mark as read-only historical
        
        // Try getting bounds from DB to cache them in RAM
        if (m_configDb.IsObject()) {
            auto statement = m_configDb["statement"];
            X::Value statusROW = m_sqlite["ROW"];
            X::Value stat = statement("SELECT ts_start, ts_end FROM buckets WHERE key=?");
            stat["bind"](1, key);
            if (stat["step"]() == statusROW) {
                partition->ts_start_ = stat["get"](0).ToLongLong();
                partition->ts_end_ = stat["get"](1).ToLongLong();
            }
        }

        return partitions_[key];
    }

    std::vector<std::string> PartitionedVdb::ScanMatchingPartitions(
        long long tsStartMs, long long tsEndMs,
        const std::set<int>& customIndices)
    {
        std::vector<std::string> result;

        if (tsStartMs <= 0) tsStartMs = 0;
        if (tsEndMs <= 0) tsEndMs = LLONG_MAX;

        // Query SQLite manifest for saved buckets that overlap the time window
        if (m_configDb.IsObject()) {
            auto statement = m_configDb["statement"];
            X::Value statusROW = m_sqlite["ROW"];
            
            std::string q = "SELECT key, ts_start, ts_end, custom_index, ts_partition FROM buckets";
            X::Value stat = statement(q);
            
            while (stat["step"]() == statusROW) {
                std::string key = stat["get"](0).ToString();
                long long bStart = stat["get"](1).ToLongLong();
                long long bEnd = stat["get"](2).ToLongLong();
                int cIndex = stat["get"](3).ToInt();
                std::string tsPartition = stat["get"](4).ToString();

                if (!customIndices.empty() && customIndices.find(cIndex) == customIndices.end()) continue;
                
                if (bStart > 0 && bEnd > 0) {
                    if (bStart > tsEndMs || bEnd < tsStartMs) continue;
                } else {
                    auto [partStartMs, partEndMs] = PartitionNameToTimeRange(tsPartition);
                    if (partEndMs < tsStartMs || partStartMs > tsEndMs) continue;
                }
                
                result.push_back(key);
            }
        }

        // Include in-memory partitions not yet saved or actively being written
        for (const auto& [key, partition] : partitions_) {
            if (std::find(result.begin(), result.end(), key) != result.end()) continue;

            // Parse key: tsPartition_customIndex_bucketNum
            size_t last_under = key.rfind('_');
            size_t first_under = key.find('_');
            if (first_under == std::string::npos || last_under == std::string::npos || first_under == last_under) continue;
            
            int customIndex = std::stoi(key.substr(first_under + 1, last_under - first_under - 1));

            if (!customIndices.empty() && customIndices.find(customIndex) == customIndices.end()) continue;

            long long pStart = partition->ts_start_.load();
            long long pEnd = partition->ts_end_.load();

            if (pStart > 0 && pEnd > 0) {
                if (pStart > tsEndMs || pEnd < tsStartMs) continue;
            } else {
                std::string tsPartition = key.substr(0, first_under);
                auto [partStartMs, partEndMs] = PartitionNameToTimeRange(tsPartition);
                if (partEndMs < tsStartMs || partStartMs > tsEndMs) continue;
            }

            result.push_back(key);
        }

        return result;
    }

    // ============================================================================
    // Public API: Init
    // ============================================================================

    bool PartitionedVdb::Init(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        // Parse parameters into config map
        auto parseParam = [&](const std::string& key, const std::string& defaultVal) {
            if (auto it = kwParams.find(key.c_str()); it) {
                SetConfig(key, it->val.ToString());
            }
            else {
                SetConfig(key, defaultVal);
            }
            };

        // Required: prefix
        if (auto it = kwParams.find("prefix"); it) {
            prefix_ = it->val.ToString();
        }
        else if (params.size() >= 1) {
            prefix_ = params[0].ToString();
        }
        else {
            prefix_ = "vdb";
        }
        SetConfig("prefix", prefix_);

        // Required: path
        if (auto it = kwParams.find("path"); it) {
            basePath_ = it->val.ToString();
        }
        else if (params.size() >= 2) {
            basePath_ = params[1].ToString();
        }
        else {
            basePath_ = ".";
        }

        // Create directory immediately so SQLite can bind to the file
        fs::create_directories(basePath_);

        // Optional parameters with defaults
        parseParam("dimension", "512");
        parseParam("granularity", "hourly");
        parseParam("space", "l2");
        parseParam("max_memory_gb", "1.0");
        parseParam("max_loaded_read_only_partitions", "50");
        parseParam("M", "16");
        parseParam("ef_construction", "200");
        parseParam("ef_search", "50");
        parseParam("ttl_minutes", "60");
        parseParam("auto_save_seconds", "300");
        parseParam("wal_cooling_time_seconds", "60");
        SetConfig("next_custom_index", "1");

        // Handle dim as alias for dimension
        if (auto it = kwParams.find("dim"); it) {
            SetConfig("dimension", it->val.ToString());
        }

        // Apply config to member variables
        ApplyConfigToMembers();

        // Calculate safe bucket max_elements_ from user's max_memory_gb_
        size_t bytes_per_vector = dimension_ * sizeof(float);
        size_t graph_overhead = M_ * sizeof(int) * 2; 
        size_t metadata_overhead = 256; 
        size_t total_bytes_per_vector = bytes_per_vector + graph_overhead + metadata_overhead;
        size_t bytes_limit = static_cast<size_t>(maxMemoryGb_ * 1024.0f * 1024.0f * 1024.0f);
        maxElements_ = bytes_limit / total_bytes_per_vector;
        if (maxElements_ < 1000) maxElements_ = 1000; // Hard minimum safety

        // Initialize database
        InitDatabase();

        // If the DB already has config data (i.e. this is an existing VDB),
        // load from it instead of overwriting with fresh defaults.
        {
            auto statement = m_configDb["statement"];
            X::Value statusROW = m_sqlite["ROW"];
            X::Value countStat = statement("SELECT COUNT(*) FROM config");
            int configCount = 0;
            if (countStat["step"]() == statusROW) {
                configCount = countStat["get"](0).ToInt();
            }
            if (configCount > 0) {
                // Existing DB: load config and partitions from it
                LoadConfigFromDB();
                LoadCustomPartitionsFromDB();
            }
            else {
                // Fresh DB: sync our in-memory config to it
                SyncConfigToDB();
            }
        }

        // ==========================================
        // WAL Crash Recovery: Asynchronous Hydration
        // ==========================================
        std::vector<std::string> orphaned_wals;
        for (const auto& entry : fs::directory_iterator(basePath_)) {
            if (entry.is_regular_file()) {
                std::string fname = entry.path().filename().string();
                if (fname.find(".wal_") != std::string::npos) {
                    orphaned_wals.push_back(fname);
                }
            }
        }
        if (!orphaned_wals.empty()) {
            std::sort(orphaned_wals.begin(), orphaned_wals.end()); // Chronological by timestamp postfix
            {
                std::lock_guard<std::mutex> clock(wals_mutex_);
                for (const auto& wal : orphaned_wals) {
                    pending_wals_.push(wal);
                }
            }
            wals_cv_.notify_one();
        }

        StartMaintenanceThread();

        retValue = X::Value(true);
        return true;
    }

    // ============================================================================
    // Public API: AddVectors
    // ============================================================================

    void PartitionedVdb::AddVectors(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        if (params.size() < 2) {
            retValue = X::Value(false);
            return;
        }

        // Parse kwParams
        long long timestampMs = 0;
        std::string partitionTag = "default";
        int numThreads = -1;

        if (auto it = kwParams.find("timestamp"); it) {
            timestampMs = it->val.ToLongLong();
        }
        if (auto it = kwParams.find("partition"); it) {
            partitionTag = it->val.ToString();
        }
        if (auto it = kwParams.find("num_threads"); it) {
            numThreads = it->val.ToInt();
        }

        // Determine partition
        std::string tsPartition = TimestampToPartitionName(timestampMs);
        int customIndex = GetOrCreateCustomIndex(partitionTag);

        std::shared_ptr<Partition> partition = GetOrCreatePartition(tsPartition, customIndex);
        if (!partition) {
            retValue = X::Value(false);
            return;
        }

        // Get vectors
        X::Value vecVal = params[1];
        if (!vecVal.IsTensor()) {
            retValue = X::Value(false);
            return;
        }

        X::Tensor vecT0(vecVal);
        X::Value vecValCont = vecT0->ToType(X::TensorDataType::FLOAT);
        X::Tensor vecT(vecValCont);
        long long totalCount = vecT->GetCount();

        if (totalCount == 0 || dimension_ <= 0 || totalCount % dimension_ != 0) {
            retValue = X::Value(false);
            return;
        }

        size_t n = totalCount / dimension_;
        total_add_vectors_ += n;
        
        const float* rawPtr = reinterpret_cast<const float*>(vecT->GetData());

        // Build external IDs
        std::vector<unsigned long long> extIds(n);
        X::Value idsVal = params[0];

        if (idsVal.IsList()) {
            X::List list(idsVal);
            if (list->Size() != n) {
                retValue = X::Value(false);
                return;
            }
            size_t i = 0;
            for (auto item : *list) {
                extIds[i++] = item.ToLongLong();
            }
        }
        else {
            unsigned long long start = idsVal.ToLongLong();
            for (size_t i = 0; i < n; ++i) {
                extIds[i] = start + i;
            }
        }

        // Collect chunk texts
        std::vector<std::string> chunkTexts(n);
        if (auto it = kwParams.find("chunks"); it) {
            X::Value chunksVal = it->val;
            if (chunksVal.IsList()) {
                X::List list(chunksVal);
                if (list->Size() == n) {
                    size_t i = 0;
                    for (auto item : *list) {
                        chunkTexts[i++] = item.ToString();
                    }
                }
            }
            else if (chunksVal.IsString()) {
                std::fill(chunkTexts.begin(), chunkTexts.end(), chunksVal.ToString());
            }
        }

        // Add to partition (with timestamp)
        // partition->vdb->AddLabels(extIds, chunkTexts, static_cast<unsigned long long>(timestampMs));
        // partition->index->AddVectors(recIdx, rawPtr, totalCount, numThreads);
        // partition->count += n;
        // partition->is_dirty_ = true;

        if (timestampMs > 0) {
            long long current_start = partition->ts_start_.load();
            long long current_end = partition->ts_end_.load();
            if (current_start == 0 || timestampMs < current_start) partition->ts_start_ = timestampMs;
            if (current_end == 0 || timestampMs > current_end) partition->ts_end_ = timestampMs;
        }

        // Update logical partition count so bucket cascades trigger dynamically in GetOrCreatePartition
        partition->count += n;

        // ==========================================
        // WAL (Write-Ahead Log) Synchronous Append
        // ==========================================
        {
            std::lock_guard<std::recursive_mutex> lock(partitions_mutex_);
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            if (partition->active_wal_filename_.empty()) {
                partition->active_wal_filename_ = partition->key_ + ".wal_" + std::to_string(now_ms);
            }

            BucketStorage::AppendWalRecord(basePath_, partition, extIds, chunkTexts, timestampMs, rawPtr, n, dimension_);

            partition->active_wal_record_count_ += n;
            partition->last_wal_append_ms_ = now_ms;
            
            if (partition->active_wal_record_count_ >= wal_rotation_threshold_) {
                {
                    std::lock_guard<std::mutex> wlock(wals_mutex_);
                    pending_wals_.push(partition->active_wal_filename_);
                }
                wals_cv_.notify_one();
                
                partition->active_wal_filename_.clear();
                partition->active_wal_record_count_ = 0;
            }
        }

        retValue = X::Value(static_cast<long long>(extIds[n - 1]));
    }

    bool PartitionedVdb::QueryLabelByID(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        if (params.size() < 1)
        {
            return false;
        }
        unsigned long long id = (unsigned long long)params[0];

        // Check if timestamp and/or partition are specified
        bool hasTimestamp = false;
        bool hasPartition = false;
        long long timestampMs = 0;
        std::string partitionTag;

        if (auto it = kwParams.find("timestamp"); it) {
            timestampMs = it->val.ToLongLong();
            hasTimestamp = true;
        }
        if (auto it = kwParams.find("partition"); it) {
            partitionTag = it->val.ToString();
            hasPartition = true;
        }

        // If both timestamp and partition are specified, search only that specific partition
        if (hasTimestamp && hasPartition) {
            int customIndex = GetCustomIndex(partitionTag);
            if (customIndex < 0) {
                retValue = X::Value();
                return false;
            }

            std::set<int> indices = { customIndex };
            std::vector<std::string> matchingKeys = ScanMatchingPartitions(timestampMs, timestampMs, indices);

            for (const auto& key : matchingKeys) {
                std::shared_ptr<Quanta::Partition> pPart = partitions_.count(key) ?
                    partitions_[key] : LoadPartition(key);

                if (pPart) {
                    std::string label = pPart->vdb->GetTextById(id);
                    if (!label.empty()) {
                        retValue = label;
                        return true;
                    }
                }
            }
            retValue = X::Value();
            return false;
        }

        // If only partition is specified, search all time partitions for that custom index
        if (hasPartition && !hasTimestamp) {
            int customIndex = GetCustomIndex(partitionTag);
            if (customIndex < 0) {
                retValue = X::Value();
                return false;
            }

            std::set<int> indices = { customIndex };
            std::vector<std::string> matchingKeys = ScanMatchingPartitions(0, LLONG_MAX, indices);

            for (const auto& key : matchingKeys) {
                std::shared_ptr<Quanta::Partition> partition = partitions_.count(key) ?
                    partitions_[key] : LoadPartition(key);

                if (partition) {
                    std::string label = partition->vdb->GetTextById(id);
                    if (!label.empty()) {
                        retValue = label;
                        return true;
                    }
                }
            }
            retValue = X::Value();
            return false;
        }

        // If only timestamp is specified, search all custom indices for that time partition
        if (hasTimestamp && !hasPartition) {
            std::string tsPartition = TimestampToPartitionName(timestampMs);

            // Search all custom indices for this time partition
            std::set<int> emptySet;  // Empty set means all indices
            std::vector<std::string> matchingKeys = ScanMatchingPartitions(timestampMs, timestampMs, emptySet);

            for (const auto& key : matchingKeys) {
                // Only check keys that match our time partition
                size_t first_under = key.find('_');
                if (first_under != std::string::npos && key.substr(0, first_under) == tsPartition) {
                    std::shared_ptr<Quanta::Partition> pPart = partitions_.count(key) ?
                        partitions_[key] : LoadPartition(key);

                    if (pPart) {
                        std::string label = pPart->vdb->GetTextById(id);
                        if (!label.empty()) {
                            retValue = label;
                            return true;
                        }
                    }
                }
            }
            retValue = X::Value();
            return false;
        }

        // Neither timestamp nor partition specified - search ALL partitions
        std::set<int> emptySet;  // Empty set means all indices
        std::vector<std::string> allKeys = ScanMatchingPartitions(0, LLONG_MAX, emptySet);

        for (const auto& key : allKeys) {
            std::shared_ptr<Quanta::Partition> pPart = partitions_.count(key) ?
                partitions_[key] : LoadPartition(key);

            if (pPart) {
                std::string label = pPart->vdb->GetTextById(id);
                if (!label.empty()) {
                    retValue = label;
                    return true;
                }
            }
        }

        retValue = X::Value();
        return false;
    }

    // ============================================================================
    // Public API: Lookup
    // ============================================================================

    void PartitionedVdb::Lookup(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        total_lookups_++;
        if (params.size() < 2) {
            retValue = X::Value();
            return;
        }

        X::Value vecVal = params[0];
        int topK = params[1].ToInt();

        if (!vecVal.IsTensor()) {
            retValue = X::Value();
            return;
        }

        X::Tensor vecT0(vecVal);
        X::Value vecValCont = vecT0->ToType(X::TensorDataType::FLOAT);
        X::Tensor vecT(vecValCont);
        std::vector<float> query(vecT->GetCount());
        memcpy(query.data(), vecT->GetData(), vecT->GetCount() * sizeof(float));

        // Parse dedup threshold
        float dedupThreshold = -1.0f;
        if (auto it = kwParams.find("dedup"); it) {
            dedupThreshold = static_cast<float>(it->val.ToDouble());
        }

        // Parse time range filters (milliseconds)
        long long tsStartMs = 0;
        long long tsEndMs = 0;

        if (auto it = kwParams.find("ts_start"); it) {
            tsStartMs = it->val.ToLongLong();
        }
        if (auto it = kwParams.find("ts_end"); it) {
            tsEndMs = it->val.ToLongLong();
        }

        // Parse partition filters
        std::set<int> customIndices;
        if (auto it = kwParams.find("partitions"); it) {
            X::Value partVal = it->val;
            if (partVal.IsList()) {
                X::List list(partVal);
                std::vector<std::string> tags;
                for (auto item : *list) {
                    tags.push_back(item.ToString());
                }
                customIndices = ResolveTagsToIndices(tags);
            }
            else if (partVal.IsString()) {
                int idx = GetCustomIndex(partVal.ToString());
                if (idx >= 0) customIndices.insert(idx);
            }
        }
        if (auto it = kwParams.find("partition"); it) {
            int idx = GetCustomIndex(it->val.ToString());
            if (idx >= 0) customIndices.insert(idx);
        }

        // Find matching partitions
        std::vector<std::string> matchingKeys = ScanMatchingPartitions(tsStartMs, tsEndMs, customIndices);

        if (matchingKeys.empty()) {
            retValue = X::List();
            return;
        }

        // Results: id, score, chunk_text, partition_key, timestamp_ms
        std::vector<std::tuple<unsigned long long, float, std::string, std::string, unsigned long long>> allResults;

        // 1. Sequentially load partitions to prevent massive I/O disk thrashing
        std::vector<std::pair<std::string, std::shared_ptr<Quanta::Partition>>> activePartitions;
        activePartitions.reserve(matchingKeys.size());

        for (const auto& key : matchingKeys) {
            std::shared_ptr<Quanta::Partition> pPart = nullptr;
            {
                std::unique_lock<std::recursive_mutex> glock(partitions_mutex_);
                pPart = partitions_.count(key) ? partitions_[key] : nullptr;
            }
            if (!pPart) {
                pPart = LoadPartition(key);
            }
            if (pPart) {
                activePartitions.push_back({key, pPart});
            }
        }

        // 2. Query each active partition concurrently utilizing OpenMP 
        std::mutex results_mutex;

#pragma omp parallel for
        for (long long i = 0; i < static_cast<long long>(activePartitions.size()); ++i) {
            const auto& key = activePartitions[i].first;
            auto pPart = activePartitions[i].second;

            auto results = pPart->index->Lookup(query, topK);

            // Dedup within this partition if enabled
            if (dedupThreshold > 0.0f && results.size() > 1) {

                // Fetch vectors for all results (parallel natively inside dedup)
                std::vector<std::vector<float>> vectors(results.size());

                for (long long j = 0; j < static_cast<long long>(results.size()); ++j) {
                    vectors[j] = pPart->index->GetVectorById(results[j].first);
                }
                
                // Get deduplicated indices
                std::vector<size_t> keptIndices = dedup_results(
                    results, vectors, dimension_, dedupThreshold);

                // Add only kept results
                std::lock_guard<std::mutex> rLock(results_mutex);
                for (size_t idx : keptIndices) {
                    unsigned long long extId = pPart->vdb->GetIdByIndex(results[idx].first);
                    std::string text = pPart->vdb->GetTextById(extId);
                    unsigned long long tsMs = pPart->vdb->GetTimestampById(extId);
                    allResults.emplace_back(extId, results[idx].second, text, key, tsMs);
                }
            }
            else {
                // No dedup - add all results
                std::lock_guard<std::mutex> rLock(results_mutex);
                for (const auto& [internalIdx, score] : results) {
                    unsigned long long extId = pPart->vdb->GetIdByIndex(internalIdx);
                    std::string text = pPart->vdb->GetTextById(extId);
                    unsigned long long tsMs = pPart->vdb->GetTimestampById(extId);
                    allResults.emplace_back(extId, score, text, key, tsMs);
                }
            }
        }

        // Precise time-range post-filter (ms-level)
        // This filters individual results by their exact stored timestamp,
        // beyond the coarse partition-level pre-filter above.
        if (tsStartMs > 0 || tsEndMs > 0) {
            long long filterStart = (tsStartMs > 0) ? tsStartMs : 0;
            long long filterEnd = (tsEndMs > 0) ? tsEndMs : LLONG_MAX;
            allResults.erase(
                std::remove_if(allResults.begin(), allResults.end(),
                    [filterStart, filterEnd](const auto& r) {
                        long long ts = static_cast<long long>(std::get<4>(r));
                        // Keep items with ts==0 (no stored timestamp) to avoid
                        // dropping legacy data that predates timestamp storage.
                        if (ts == 0) return false;
                        return ts < filterStart || ts > filterEnd;
                    }),
                allResults.end());
        }

        // Sort by score descending
        std::sort(allResults.begin(), allResults.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a) > std::get<1>(b);
            });

        // Return top K (5-element tuples: id, score, chunk, key, timestamp_ms)
        X::List resultList;
        size_t count = static_cast<size_t>(topK);
        if (allResults.size() < count) {
            count = allResults.size();
        }
        for (size_t i = 0; i < count; ++i) {
            auto& r = allResults[i];
            X::List item;
            item += static_cast<long long>(std::get<0>(r));
            item += std::get<1>(r);
            item += std::get<2>(r);
            item += std::get<3>(r);
            item += static_cast<long long>(std::get<4>(r));
            resultList->AddItem(item);
        }

        retValue = resultList;
    }

    // ============================================================================
    // Grouping Helpers
    // ============================================================================

    // Resolve partition key from a dict item, using either a pre-built full key
    // (e.g. from Lookup/Seek results) or constructing it from timestamp + partition tag.
    // Returns the resolved partition key string (e.g. "2024-01_2") and sets
    // customIndexUnknown=true when the tag wasn't found in the registry.
    std::string PartitionedVdb::ResolveItemPartitionKey(
        X::Dict& dict,
        const std::string& fullPartitionKey,
        const std::string& partitionKey,
        const std::string& timestampKey,
        bool& customIndexUnknown)
    {
        customIndexUnknown = false;

        // Prefer a pre-built full key (e.g. "partition" field from Lookup results)
        if (!fullPartitionKey.empty()) {
            X::Value fpkVal = dict->Get(fullPartitionKey);
            if (fpkVal.IsValid()) {
                return fpkVal.ToString();
            }
        }

        // Fall back: construct from timestamp + partition tag
        long long timestampMs = 0;
        X::Value tsVal = dict->Get(timestampKey);
        if (tsVal.IsValid()) {
            timestampMs = tsVal.ToLongLong();
        }

        std::string tag = "default";
        X::Value ptVal = dict->Get(partitionKey);
        if (ptVal.IsValid()) {
            tag = ptVal.ToString();
        }

        std::string tsPartition = TimestampToPartitionName(timestampMs);
        int customIndex = GetCustomIndex(tag);
        if (customIndex < 0) {
            customIndexUnknown = true;
            customIndex = 0;
        }

        return tsPartition + "_" + std::to_string(customIndex) + "_0000";
    }

    // Fetch the embedding vector for a given item ID.
    // Searches the primary key first, then scans partitions with the same tsPartition
    // prefix (both in-memory and on-disk) when the custom index is unknown.
    std::vector<float> PartitionedVdb::FetchVectorForItem(
        unsigned long long id,
        const std::string& fullKey,
        bool customIndexUnknown)
    {
        // Extract tsPartition prefix from the key (e.g. "2024-01")
        std::string tsPartition;
        {
            size_t first_under = fullKey.find('_');
            if (first_under != std::string::npos) {
                tsPartition = fullKey.substr(0, first_under);
            }
        }

        // Try loading from one specific partition key; returns the vector or empty.
        auto tryKey = [&](const std::string& k) -> std::vector<float> {
            std::shared_ptr<Quanta::Partition> pPart = partitions_.count(k) ?
                partitions_[k] : LoadPartition(k);
            if (!pPart || !pPart->index) return {};
            auto idx = pPart->vdb->GetIndexById(id);
            if (idx < 0) return {};
            return pPart->index->GetVectorById(idx);
        };

        // 1. Try the primary key directly.
        if (!fullKey.empty()) {
            auto v = tryKey(fullKey);
            if (!v.empty()) return v;
        }

        // 2. Extract key prefix for scanning multiple capacity buckets (Tier 3)
        // If customIndex is known, prefix is "tsPartition_Index_" (e.g. "2024-01_1_")
        // If unknown, prefix is just "tsPartition_" 
        std::string keyPrefix;
        int targetCustomIndex = -1;
        if (!customIndexUnknown && !fullKey.empty()) {
            size_t last_under = fullKey.rfind('_');
            if (last_under != std::string::npos) {
                keyPrefix = fullKey.substr(0, last_under + 1);
                size_t first_under = fullKey.find('_');
                if (first_under != std::string::npos && first_under != last_under) {
                     targetCustomIndex = std::stoi(fullKey.substr(first_under + 1, last_under - first_under - 1));
                }
            }
        }
        if (keyPrefix.empty() && !tsPartition.empty()) {
            keyPrefix = tsPartition + "_";
        }

        // 3. Scan all relevant partitions for this tsPartition (and customIndex if known)
        if (!keyPrefix.empty()) {
            std::vector<std::string> keys;

            // In-memory partitions
            for (const auto& pkv : partitions_) {
                if (pkv.first.rfind(keyPrefix, 0) == 0 && pkv.first != fullKey) {
                    keys.push_back(pkv.first);
                }
            }

            // On-disk partitions 
            auto [tsStart, tsEnd] = PartitionNameToTimeRange(tsPartition);
            std::set<int> searchIndices;
            if (targetCustomIndex >= 0) {
                searchIndices.insert(targetCustomIndex);
            }
            
            for (const auto& k : ScanMatchingPartitions(tsStart, tsEnd, searchIndices)) {
                if (k.rfind(keyPrefix, 0) == 0 && k != fullKey &&
                    std::find(keys.begin(), keys.end(), k) == keys.end()) {
                    keys.push_back(k);
                }
            }

            // Sort ascending: _0000, _0001
            std::sort(keys.begin(), keys.end());
            for (const auto& k : keys) {
                auto v = tryKey(k);
                if (!v.empty()) return v;
            }
        }

        // 3. Last resort: the default sub-partition and initial bucket for this time slot.
        if (!tsPartition.empty()) {
            auto v = tryKey(tsPartition + "_0_0000");
            if (!v.empty()) return v;
        }

        return {};
    }

    // Walk the combined dict-list and populate allItems with id -> {vector}.
    void PartitionedVdb::CollectGroupingItems(
        X::List& itemsList,
        const std::string& idKey,
        const std::string& partitionKey,
        const std::string& timestampKey,
        const std::string& fullPartitionKey,
        GroupingItemMap& allItems)
    {
        for (auto item : *itemsList) {
            if (!item.IsDict()) continue;
            X::Dict dict(item);

            X::Value idVal = dict->Get(idKey);
            if (!idVal.IsValid()) continue;
            unsigned long long id = static_cast<unsigned long long>(idVal.ToLongLong());

            bool customIndexUnknown = false;
            std::string fullKey = ResolveItemPartitionKey(
                dict, fullPartitionKey, partitionKey, timestampKey, customIndexUnknown);

            auto& info = allItems[id];
            info.id = id;
            info.sources.insert(0);   // all items are "source 0" (combined pool)

            if (info.vector.empty()) {
                info.vector = FetchVectorForItem(id, fullKey, customIndexUnknown);
            }
        }
    }

    // Greedy centroid (leader) clustering.
    //
    // WHY NOT Union-Find / single-linkage?
    //   Single-linkage merges A and C if A~B and B~C, even when A and C are far apart.
    //   This "chaining" produces long, thin clusters (a line), not compact ones (a circle).
    //
    // THIS ALGORITHM guarantees that every member is within `threshold` similarity
    // of its group CENTROID, giving the round/compact clusters the user wants:
    //   For each item: find the closest centroid; join if >= threshold, else new group.
    //
    // EFFICIENCY:
    //   - Vectors are pre-normalized to unit length ONCE (parallel).
    //     After that, cosine_similarity = dot product only  (no sqrt in the hot path).
    //   - Centroids are kept as normalized unit vectors (normalized mean direction).
    //     Centroid update: normalize(old_centroid * count + new_vec).
    //   - The inner centroid-search loop is read-only per item 鈫?safe for OMP reduction.
    //     The outer item loop stays serial (adding new centroids changes shared state).
    //   - Complexity: O(N 脳 G) where G = number of groups.
    std::map<size_t, std::vector<size_t>> PartitionedVdb::RunCentroidClustering(
        const std::vector<GroupingItem*>& items,
        float threshold)
    {
        const size_t n   = items.size();
        const size_t dim = static_cast<size_t>(dimension_);

        // -----------------------------------------------------------------------
        // Step 1: Pre-normalize all valid vectors to unit length (parallel).
        //   After this, cosine_sim(a, b) = dot(a, b)  鈥?no sqrt in the hot path.
        // -----------------------------------------------------------------------
        std::vector<std::vector<float>> unitVecs(n);  // unitVecs[i] is empty if invalid

#pragma omp parallel for schedule(static)
        for (long long i = 0; i < static_cast<long long>(n); ++i) {
            const auto& src = items[i]->vector;
            if (src.size() != dim) continue;

            float norm = 0.0f;
            for (size_t d = 0; d < dim; ++d) norm += src[d] * src[d];
            if (norm <= 0.0f) continue;

            norm = std::sqrt(norm);
            unitVecs[i].resize(dim);
            for (size_t d = 0; d < dim; ++d) unitVecs[i][d] = src[d] / norm;
        }

        // -----------------------------------------------------------------------
        // Step 2: Greedy centroid assignment (serial outer loop).
        //   centroids[g]     = unit-length centroid of group g (normalized mean direction)
        //   centroidCounts[g] = number of members (for weighted update)
        //   groupMembers[g]  = ordered item indices
        // -----------------------------------------------------------------------
        std::vector<std::vector<float>> centroids;
        std::vector<size_t>             centroidCounts;
        std::vector<std::vector<size_t>> groupMembers;

        for (size_t i = 0; i < n; ++i) {
            if (unitVecs[i].empty()) continue;  // invalid / wrong dimension
            const float* vec = unitVecs[i].data();

            // Find the best matching centroid.
            // Inner scan is read-only 鈫?parallelizable with OMP reduction.
            int   bestGroup = -1;
            float bestSim   = -2.0f;  // cosine 鈭?[-1, 1]

            const size_t G = centroids.size();

            if (G > 0) {
                // For small G the overhead of OMP outweighs the benefit.
                // Only go parallel when there are enough groups to amortize launch cost.
                if (G >= 16) {
#pragma omp parallel for schedule(static) reduction(max: bestSim)
                    for (long long g = 0; g < static_cast<long long>(G); ++g) {
                        float sim = 0.0f;
                        const float* c = centroids[g].data();
                        for (size_t d = 0; d < dim; ++d) sim += vec[d] * c[d];
                        if (sim > bestSim) {
                            bestSim  = sim;
                            // NOTE: bestGroup update is not safe inside OMP max reduction.
                            // We re-scan serially below in that case.
                        }
                    }
                    // Re-scan serially to find which group achieved bestSim
                    // (OMP reduction only tracks the max value, not the index).
                    for (size_t g = 0; g < G; ++g) {
                        float sim = 0.0f;
                        const float* c = centroids[g].data();
                        for (size_t d = 0; d < dim; ++d) sim += vec[d] * c[d];
                        if (sim >= bestSim) { bestGroup = static_cast<int>(g); break; }
                    }
                }
                else {
                    // Serial scan (faster for small G)
                    for (size_t g = 0; g < G; ++g) {
                        float sim = 0.0f;
                        const float* c = centroids[g].data();
                        for (size_t d = 0; d < dim; ++d) sim += vec[d] * c[d];
                        if (sim > bestSim) { bestSim = sim; bestGroup = static_cast<int>(g); }
                    }
                }
            }

            if (bestGroup >= 0 && bestSim >= threshold) {
                // Join existing group and update centroid (normalized mean direction).
                groupMembers[bestGroup].push_back(i);
                size_t cnt = ++centroidCounts[bestGroup];
                auto& centroid = centroids[bestGroup];

                // new_centroid_unnorm = old_centroid * (cnt-1) + vec
                // then normalize 鈥?we store the normalized direction so future
                // dot products remain in [-1,1].
                float norm = 0.0f;
                for (size_t d = 0; d < dim; ++d) {
                    centroid[d] = centroid[d] * static_cast<float>(cnt - 1) + vec[d];
                    norm += centroid[d] * centroid[d];
                }
                norm = std::sqrt(norm);
                if (norm > 0.0f)
                    for (size_t d = 0; d < dim; ++d) centroid[d] /= norm;
            }
            else {
                // Start a new group; the first member's unit vector IS the centroid.
                groupMembers.push_back({ i });
                centroids.push_back(unitVecs[i]);
                centroidCounts.push_back(1);
            }
        }

        // -----------------------------------------------------------------------
        // Step 3: Convert to map<root, members> for BuildGroupingResult.
        // -----------------------------------------------------------------------
        std::map<size_t, std::vector<size_t>> groups;
        for (size_t g = 0; g < groupMembers.size(); ++g) {
            if (!groupMembers[g].empty()) {
                size_t root = groupMembers[g].front();
                groups[root] = std::move(groupMembers[g]);
            }
        }
        return groups;
    }

    // Convert the clustering result into the XLang return list.
    // Each group is a flat list of IDs: [[ID1, ID2, ...], [ID3, ...], ...]
    X::Value PartitionedVdb::BuildGroupingResult(
        const std::vector<GroupingItem*>& items,
        const std::map<size_t, std::vector<size_t>>& groups)
    {
        X::List result;
        for (auto& [root, members] : groups) {
            X::List group;
            for (size_t idx : members) {
                group += static_cast<long long>(items[idx]->id);
            }
            result->append(group);
        }
        return result;
    }

    // ============================================================================
    // Public API: Grouping
    // ============================================================================

    void PartitionedVdb::Grouping(X::XRuntime* rt,
        X::XObj* pContext, X::ARGS& params,
        X::KWARGS& kwParams, X::Value& retValue)
    {
        total_grouping_++;
        // params[0] = combined list of dicts (all candidates, VDB + SQL merged)
        // params[1] = threshold (float)
        if (params.size() < 2) {
            retValue = X::List();
            return;
        }

        if (!params[0].IsList()) {
            retValue = X::List();
            return;
        }

        // Parse field-name overrides from keyword args
        std::string idKey          = "image_id";
        std::string partitionKey   = "device_id";
        std::string timestampKey   = "timestamp";
        std::string fullPartKey    = "";   // e.g. "full_partition_key" from Lookup results

        if (auto it = kwParams.find("id_key"); it)             idKey        = it->val.ToString();
        if (auto it = kwParams.find("partition_key"); it)      partitionKey = it->val.ToString();
        if (auto it = kwParams.find("timestamp_key"); it)      timestampKey = it->val.ToString();
        if (auto it = kwParams.find("full_partition_key"); it) fullPartKey  = it->val.ToString();

        float threshold = static_cast<float>(params[1]);

        // Step 1: collect all items and their vectors from the combined list
        GroupingItemMap allItems;
        X::List combined(params[0]);
        CollectGroupingItems(combined, idKey, partitionKey, timestampKey, fullPartKey, allItems);

        // Step 2: build a flat pointer array (only items whose vectors were resolved)
        std::vector<GroupingItem*> items;
        items.reserve(allItems.size());
        for (auto& [id, info] : allItems) {
            if (!info.vector.empty()) items.push_back(&info);
        }

        if (items.empty()) {
            retValue = X::List();
            return;
        }

        // Step 3: cluster into compact groups using centroid similarity
        auto groups = RunCentroidClustering(items, threshold);

        // Step 4: serialize groups into an XLang list
        retValue = BuildGroupingResult(items, groups);
    }

    // ============================================================================
    // Public API: Save / Load
    // ============================================================================

    bool PartitionedVdb::Close()
    {
        stop_thread_ = true;
        if (maintenance_thread_.joinable()) {
            maintenance_thread_.join();
        }

        // Persist config and custom partitions to the manifest DB
        UpdateConfigFromMembers();
        SyncConfigToDB();

        std::lock_guard<std::recursive_mutex> lock(partitions_mutex_);
        for (auto& [key, partition] : partitions_) {
            if (partition->is_dirty_) {
                SavePartition(partition, key);
            }
        }
        return true;
    }

    bool PartitionedVdb::Save(const std::string& path)
    {
        if (!path.empty()) {
            basePath_ = path;
            fs::create_directories(basePath_);
        }
        return Close();
    }

    bool PartitionedVdb::SavePartition(std::shared_ptr<Partition> p, const std::string& key)
    {
        if (!p) return false;
        size_t last_under = key.rfind('_');
        size_t first_under = key.find('_');
        if (first_under == std::string::npos || last_under == std::string::npos || first_under == last_under) return false;

        std::string tsPartition = key.substr(0, first_under);
        int customIndex = std::stoi(key.substr(first_under + 1, last_under - first_under - 1));
        std::string bucketStr = key.substr(last_under + 1);

        BucketStorage::SavePhysicalBucket(basePath_, prefix_, p, key);
        p->is_dirty_ = false;

        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        p->last_save_ms_ = now_ms;

        // Upsert bounding manifest to SQLite
        if (m_configDb.IsObject()) {
            auto statement = m_configDb["statement"];
            X::Value stat = statement("INSERT OR REPLACE INTO buckets (key, ts_partition, custom_index, bucket_number, ts_start, ts_end) VALUES (?, ?, ?, ?, ?, ?)");
            stat["bind"](1, key);
            stat["bind"](2, tsPartition);
            stat["bind"](3, customIndex);
            stat["bind"](4, std::stoi(bucketStr));
            stat["bind"](5, (long long)p->ts_start_.load());
            stat["bind"](6, (long long)p->ts_end_.load());
            stat["step"]();
        }

        return true;
    }

    bool PartitionedVdb::Load(const std::string& path)
    {
        basePath_ = path;

        InitDatabase();
        LoadConfigFromDB();
        LoadCustomPartitionsFromDB();

        return true;
    }

    // ============================================================================
    // Public API: List / Info
    // ============================================================================

    X::Value PartitionedVdb::ListPartitions()
    {
        X::List result;

        std::set<int> emptySet;
        std::vector<std::string> allKeys = ScanMatchingPartitions(0, LLONG_MAX, emptySet);

        for (const auto& key : allKeys) {
            X::Dict info;
            info->Set("key", key);

            size_t last_under = key.rfind('_');
            size_t first_under = key.find('_');
            if (first_under == std::string::npos || last_under == std::string::npos || first_under == last_under) continue;

            std::string tsPartition = key.substr(0, first_under);
            int customIndex = std::stoi(key.substr(first_under + 1, last_under - first_under - 1));
            int bucketNum = std::stoi(key.substr(last_under + 1));

            info->Set("ts_partition", tsPartition);
            info->Set("custom_index", customIndex);
            info->Set("bucket_num", bucketNum);

            X::List tags;
            if (customPartitionTags_.count(customIndex)) {
                for (const auto& tag : customPartitionTags_[customIndex]) {
                    tags += tag;
                }
            }
            info->Set("tags", tags);

            bool loaded = partitions_.count(key) > 0;
            info->Set("loaded", loaded);
            if (loaded) {
                info->Set("count", static_cast<long long>(partitions_[key]->count));
            }

            result->AddItem(info);
        }

        return result;
    }

    X::Value PartitionedVdb::GetPartitionInfo(const std::string& tag)
    {
        int idx = GetCustomIndex(tag);
        if (idx < 0) return X::Value();

        X::Dict info;
        info->Set("customIndex", idx);

        X::List tags;
        if (customPartitionTags_.count(idx)) {
            for (const auto& t : customPartitionTags_[idx]) {
                tags += t;
            }
        }
        info->Set("tags", tags);

        return info;
    }

    // ============================================================================
    // Maintenance Thread Logic
    // ============================================================================

    void PartitionedVdb::StartMaintenanceThread()
    {
        maintenance_thread_ = std::thread(&PartitionedVdb::MaintenanceLoop, this);
    }

    void PartitionedVdb::MaintenanceLoop()
    {
        X::Value cantor = QuantaHost::I().GetCantor();
        while (!stop_thread_) {
            size_t peak_queue_size = 0;
            {
                std::unique_lock<std::mutex> wlock(wals_mutex_);
                wals_cv_.wait_for(wlock, std::chrono::milliseconds(5000), [this]() {
                    return !pending_wals_.empty() || stop_thread_;
                });
                peak_queue_size = pending_wals_.size();
            }
            if (stop_thread_) break;

            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            long long loaded_partitions = 0;
            long long memory_vectors = 0;
            long long dirty_partitions = 0;

            {
                std::lock_guard<std::recursive_mutex> lock(partitions_mutex_);
                for (auto it = partitions_.begin(); it != partitions_.end(); ) {
                    auto& partition = it->second;
                    bool erased = false;
                    
                    size_t last_under = it->first.rfind('_');
                    size_t first_under = it->first.find('_');
                    if (first_under == std::string::npos || last_under == std::string::npos || first_under == last_under) {
                        ++it;
                        continue;
                    }
                    std::string tsPartition = it->first.substr(0, first_under);
                    int customIndex = std::stoi(it->first.substr(first_under + 1, last_under - first_under - 1));

                    // 1. Time-To-Live Eviction: Unload from RAM if idle
                    long long idle_time_ms = now_ms - partition->last_access_ms_;
                    if (ttl_minutes_ > 0 && idle_time_ms > (ttl_minutes_ * 60000LL)) {
                        // Save first before dropping
                        if (partition->is_dirty_) {
                            SavePartition(partition, it->first);
                        }
                        it = partitions_.erase(it);
                        erased = true;
                    }
                    
                    // 2. Auto-save if it's dirty and has exceeded the auto-save threshold
                    if (!erased && partition->is_dirty_ && auto_save_seconds_ > 0) {
                        long long dirty_age_ms = now_ms - partition->last_save_ms_;
                        if (dirty_age_ms > (auto_save_seconds_ * 1000LL)) {
                            SavePartition(partition, it->first);
                        }
                    }

                    // 3. WAL Cooling Time Flush (Force merge if idle)
                    if (!erased && wal_cooling_time_seconds_ > 0 && !partition->active_wal_filename_.empty()) {
                        long long idle_wal_ms = now_ms - partition->last_wal_append_ms_;
                        if (idle_wal_ms > (wal_cooling_time_seconds_ * 1000LL)) {
                            {
                                std::lock_guard<std::mutex> wlock(wals_mutex_);
                                pending_wals_.push(partition->active_wal_filename_);
                            }
                            wals_cv_.notify_one();
                            
                            partition->active_wal_filename_.clear();
                            partition->active_wal_record_count_ = 0;
                        }
                    }

                    if (!erased) {
                        loaded_partitions++;
                        if (partition) {
                            memory_vectors += partition->count;
                            if (partition->is_dirty_) {
                                dirty_partitions++;
                            }
                        }
                        ++it;
                    }
                }
            }
            
            // 3. Process Pending WAL Micro-Batches
            while (!stop_thread_) {
                std::string unmerged_wal;
                size_t pending_count = 0;
                {
                    std::unique_lock<std::mutex> wlock(wals_mutex_);
                    if (!pending_wals_.empty()) {
                        unmerged_wal = pending_wals_.front();
                        pending_wals_.pop();
                    }
                    pending_count = pending_wals_.size();
                }

                if (!unmerged_wal.empty()) {
                    ProcessWalFile(unmerged_wal);
                } else {
                    break; // Queue empty, exit 
                }
            }

            // 4. Metrics Reporting
            X::Value cantor = QuantaHost::I().GetCantor();
            if (cantor.IsObject()) {
                X::Value metrics = cantor["Metrics"]();
                X::Value SetWordBook = metrics["SetWordBook"];
                std::string namespace_name = "PartitionedVdb_" + basePath_.filename().string();
                
                SetWordBook(namespace_name, "LoadedPartitions", X::Value(loaded_partitions));
                SetWordBook(namespace_name, "MemoryVectors", X::Value(memory_vectors));
                SetWordBook(namespace_name, "DirtyPartitions", X::Value(dirty_partitions));
                SetWordBook(namespace_name, "PendingWals", X::Value((long long)peak_queue_size));
                
                SetWordBook(namespace_name, "TotalBuckets", X::Value((long long)total_buckets_));
                SetWordBook(namespace_name, "TotalWalsProcessed", X::Value((long long)total_wals_processed_));
                SetWordBook(namespace_name, "TotalWalVectorsMerged", X::Value((long long)total_wal_vectors_merged_));
                
                SetWordBook(namespace_name, "TotalLookups", X::Value((long long)total_lookups_));
                SetWordBook(namespace_name, "TotalAddVectors", X::Value((long long)total_add_vectors_));
                SetWordBook(namespace_name, "TotalGroupings", X::Value((long long)total_grouping_));
            }

        }
    }

    void PartitionedVdb::ProcessWalFileBuffer(const std::string& target_key, const std::vector<char>& buffer)
    {
        // Wait for lock to modify
        std::lock_guard<std::recursive_mutex> plock(partitions_mutex_);
        std::shared_ptr<Partition> pPart = partitions_.count(target_key) ? partitions_[target_key] : LoadPartition(target_key);
        if (!pPart) {
            pPart = std::make_shared<Partition>();
            pPart->vdb = std::make_unique<VectorDatabase>(dimension_);
            pPart->index = std::make_unique<HnswVdb>(
                spaceName_, dimension_, maxElements_, M_, efConstruction_, efSearch_);
            pPart->is_historical_read_ = false;
            pPart->key_ = target_key;
            pPart->is_dirty_ = true;
            
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            pPart->last_access_ms_ = now_ms;
            pPart->last_save_ms_ = now_ms;
            
            partitions_[target_key] = pPart;
            total_buckets_++;
        }

        std::vector<unsigned long long> extIds;
        std::vector<std::string> chunkTexts;
        std::vector<float> allVectors;
        unsigned long long maxTs = 0;

        size_t offset = 0;
        size_t total_size = buffer.size();

        while (offset + sizeof(WalRecordHeader) <= total_size) {
            WalRecordHeader header;
            std::memcpy(&header, buffer.data() + offset, sizeof(WalRecordHeader));
            offset += sizeof(WalRecordHeader);

            size_t vector_bytes = dimension_ * sizeof(float);
            if (offset + vector_bytes > total_size) break; // Incomplete write

            extIds.push_back(header.external_id);
            if (header.timestamp_ms > maxTs) maxTs = header.timestamp_ms;

            std::vector<float> vec(dimension_);
            std::memcpy(vec.data(), buffer.data() + offset, vector_bytes);
            allVectors.insert(allVectors.end(), vec.begin(), vec.end());
            offset += vector_bytes;

            std::string chunkText;
            if (header.chunk_text_length > 0) {
                if (offset + header.chunk_text_length > total_size) break; // Incomplete string
                chunkText.resize(header.chunk_text_length);
                std::memcpy(&chunkText[0], buffer.data() + offset, header.chunk_text_length);
                offset += header.chunk_text_length;
            }
            chunkTexts.push_back(chunkText);
        }

        // Merge organically into active physical layers natively (bypass RAM duplication)
        if (!extIds.empty()) {
            std::vector<unsigned long long> recIdx = pPart->vdb->AddLabels(extIds, chunkTexts, maxTs);
            
            size_t new_total = pPart->vdb->GetSize();
            if (new_total > pPart->index->GetMaxElements()) {
                pPart->index->Resize(new_total + 1000);
            }
            
            pPart->index->AddVectors(recIdx, allVectors.data(), allVectors.size(), 1);
            
            // Re-sync logical count with physical count (fixes crash recovery without double-counting runtime)
            pPart->count = pPart->vdb->GetSize();
            SavePartition(pPart, target_key);
            
            total_wals_processed_++;
            total_wal_vectors_merged_ += extIds.size();
        }
    }

    void PartitionedVdb::ProcessWalFile(const std::string& unmerged_wal)
    {
        size_t wal_pos = unmerged_wal.find(".wal_");
        if (wal_pos != std::string::npos) {
            std::string target_key = unmerged_wal.substr(0, wal_pos);
            fs::path walPath = basePath_ / unmerged_wal;

            std::vector<char> buffer;
            if (BucketStorage::ReadWalFile(walPath, buffer)) {
                ProcessWalFileBuffer(target_key, buffer);
            }

            std::error_code ec;
            fs::remove(walPath, ec);
        }
    }
} // namespace Quanta
