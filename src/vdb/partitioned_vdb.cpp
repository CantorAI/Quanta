#include "partitioned_vdb.h"
#include <ctime>
#include <algorithm>
#include <climits>
#include <functional>
#include "HnswVdb.h"
#include "VectorDatabase.h"

namespace Quanta
{

    // Helper: cosine similarity between two vectors.
    float quanta_cosine_similarity(int dimension, const float* a, const float* b)
    {
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

            const float* vec_i = vectors[i].data();

            for (long long jj = 0; jj < ii; ++jj) {
                size_t j = sortedIdx[jj];
                if (removed[j] || vectors[j].empty()) continue;

                const float* vec_j = vectors[j].data();
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
        // If params/kwargs provided, call Init
        if (params.size() > 0 || kwParams.size() > 0) {
            X::Value retValue;
            Init(nullptr, nullptr, params, kwParams, retValue);
        }

        X::Runtime rt;
        m_rt = rt;
        X::Package sqlite(rt, "sqlite", "xlang_sqlite");
        m_sqlite = sqlite;

        // Initialize default partition (index 0)
        customPartitionTags_[0].insert("default");
        tagToIndex_["default"] = 0;
    }

    PartitionedVdb::~PartitionedVdb()
    {
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
        tsGranularity_ = GetConfig("granularity", "monthly");
        dimension_ = std::stoi(GetConfig("dimension", "512"));
        spaceName_ = GetConfig("space", "l2");
        maxElements_ = std::stoul(GetConfig("max_elements", "100000"));
        M_ = std::stoi(GetConfig("M", "16"));
        efConstruction_ = std::stoi(GetConfig("ef_construction", "200"));
        efSearch_ = std::stoi(GetConfig("ef_search", "50"));
        nextCustomIndex_ = std::stoi(GetConfig("next_custom_index", "1"));
    }

    void PartitionedVdb::UpdateConfigFromMembers()
    {
        SetConfig("prefix", prefix_);
        SetConfig("granularity", tsGranularity_);
        SetConfig("dimension", std::to_string(dimension_));
        SetConfig("space", spaceName_);
        SetConfig("max_elements", std::to_string(maxElements_));
        SetConfig("M", std::to_string(M_));
        SetConfig("ef_construction", std::to_string(efConstruction_));
        SetConfig("ef_search", std::to_string(efSearch_));
        SetConfig("next_custom_index", std::to_string(nextCustomIndex_));
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

    fs::path PartitionedVdb::GetHnswPath(const std::string& tsPartition, int customIndex)
    {
        std::string filename = prefix_ + "_" + tsPartition + "_" + std::to_string(customIndex) + ".hnsw";
        return basePath_ / filename;
    }

    fs::path PartitionedVdb::GetVdbPath(const std::string& tsPartition, int customIndex)
    {
        std::string filename = prefix_ + "_" + tsPartition + "_" + std::to_string(customIndex) + ".vdb";
        return basePath_ / filename;
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

        // Note: Need rt to persist - this API might need adjustment
        // For now, caller should call Save() to persist
        return true;
    }

    Partition* PartitionedVdb::GetOrCreatePartition(const std::string& tsPartition, int customIndex)
    {
        std::string key = tsPartition + "_" + std::to_string(customIndex);

        auto it = partitions_.find(key);
        if (it != partitions_.end()) {
            return it->second.get();
        }

        auto partition = std::make_unique<Partition>();
        partition->vdb = std::make_unique<VectorDatabase>(dimension_);
        partition->index = std::make_unique<HnswVdb>(
            spaceName_, dimension_, maxElements_, M_, efConstruction_, efSearch_);

        Partition* ptr = partition.get();
        partitions_[key] = std::move(partition);
        return ptr;
    }

    Partition* PartitionedVdb::LoadPartition(const std::string& key)
    {
        auto it = partitions_.find(key);
        if (it != partitions_.end()) {
            return it->second.get();
        }

        size_t pos = key.rfind('_');
        if (pos == std::string::npos) return nullptr;

        std::string tsPartition = key.substr(0, pos);
        int customIndex = std::stoi(key.substr(pos + 1));

        fs::path hnswPath = GetHnswPath(tsPartition, customIndex);
        fs::path vdbPath = GetVdbPath(tsPartition, customIndex);

        if (!fs::exists(hnswPath)) {
            return nullptr;
        }

        auto partition = std::make_unique<Partition>();
        partition->vdb = std::make_unique<VectorDatabase>(dimension_);
        partition->vdb->Load(vdbPath.string());

        partition->index = std::make_unique<HnswVdb>(
            spaceName_, dimension_, maxElements_, M_, efConstruction_, efSearch_);
        partition->index->Load(hnswPath.string());

        Partition* ptr = partition.get();
        partitions_[key] = std::move(partition);
        return ptr;
    }

    std::vector<std::string> PartitionedVdb::ScanMatchingPartitions(
        long long tsStartMs, long long tsEndMs,
        const std::set<int>& customIndices)
    {
        std::vector<std::string> result;

        if (tsStartMs <= 0) tsStartMs = 0;
        if (tsEndMs <= 0) tsEndMs = LLONG_MAX;

        if (!fs::exists(basePath_)) {
            return result;
        }

        std::string pattern = prefix_ + "_";

        for (const auto& entry : fs::directory_iterator(basePath_)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();

            // Must start with prefix_ and end with .hnsw
            if (filename.find(pattern) != 0) continue;
            if (filename.size() < 5 || filename.substr(filename.size() - 5) != ".hnsw") continue;

            // Extract key: prefix_tsPartition_index.hnsw ¡ú tsPartition_index
            std::string key = filename.substr(pattern.size());
            key = key.substr(0, key.size() - 5);  // Remove .hnsw

            size_t pos = key.rfind('_');
            if (pos == std::string::npos) continue;

            std::string tsPartition = key.substr(0, pos);
            int customIndex = std::stoi(key.substr(pos + 1));

            // Filter by custom index
            if (!customIndices.empty() && customIndices.find(customIndex) == customIndices.end()) {
                continue;
            }

            // Filter by time range
            auto [partStartMs, partEndMs] = PartitionNameToTimeRange(tsPartition);
            if (partEndMs < tsStartMs || partStartMs > tsEndMs) {
                continue;
            }

            result.push_back(key);
        }

        // Include in-memory partitions not yet saved
        for (const auto& [key, partition] : partitions_) {
            if (std::find(result.begin(), result.end(), key) != result.end()) continue;

            size_t pos = key.rfind('_');
            if (pos == std::string::npos) continue;

            std::string tsPartition = key.substr(0, pos);
            int customIndex = std::stoi(key.substr(pos + 1));

            if (!customIndices.empty() && customIndices.find(customIndex) == customIndices.end()) {
                continue;
            }

            auto [partStartMs, partEndMs] = PartitionNameToTimeRange(tsPartition);
            if (partEndMs < tsStartMs || partStartMs > tsEndMs) {
                continue;
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
        X::Runtime xrt(rt);

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

        // Optional parameters with defaults
        parseParam("dimension", "512");
        parseParam("granularity", "monthly");
        parseParam("space", "l2");
        parseParam("max_elements", "100000");
        parseParam("M", "16");
        parseParam("ef_construction", "200");
        parseParam("ef_search", "50");
        SetConfig("next_custom_index", "1");

        // Handle dim as alias for dimension
        if (auto it = kwParams.find("dim"); it) {
            SetConfig("dimension", it->val.ToString());
        }

        // Apply config to member variables
        ApplyConfigToMembers();

        // Create directory and initialize database
        fs::create_directories(basePath_);
        InitDatabase();
        SyncConfigToDB();

        retValue = X::Value(true);
        return true;
    }

    // ============================================================================
    // Public API: AddVectors
    // ============================================================================

    void PartitionedVdb::AddVectors(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        X::Runtime xrt(rt);

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

        Partition* partition = GetOrCreatePartition(tsPartition, customIndex);
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
            for (auto& item : *list) {
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
                    for (auto& item : *list) {
                        chunkTexts[i++] = item.ToString();
                    }
                }
            }
            else if (chunksVal.IsString()) {
                std::fill(chunkTexts.begin(), chunkTexts.end(), chunksVal.ToString());
            }
        }

        // Add to partition
        std::vector<unsigned long long> recIdx = partition->vdb->AddLabels(extIds, chunkTexts);
        partition->index->AddVectors(recIdx, rawPtr, totalCount, numThreads);
        partition->count += n;

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
            std::string tsPartition = TimestampToPartitionName(timestampMs);
            int customIndex = GetCustomIndex(partitionTag);
            if (customIndex < 0) {
                retValue = X::Value();
                return false;
            }

            std::string key = tsPartition + "_" + std::to_string(customIndex);
            Partition* partition = partitions_.count(key) ?
                partitions_[key].get() : LoadPartition(key);

            if (partition) {
                std::string label = partition->vdb->GetTextById(id);
                if (!label.empty()) {
                    retValue = label;
                    return true;
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
                Partition* partition = partitions_.count(key) ?
                    partitions_[key].get() : LoadPartition(key);

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
                size_t pos = key.rfind('_');
                if (pos != std::string::npos && key.substr(0, pos) == tsPartition) {
                    Partition* partition = partitions_.count(key) ?
                        partitions_[key].get() : LoadPartition(key);

                    if (partition) {
                        std::string label = partition->vdb->GetTextById(id);
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
            Partition* partition = partitions_.count(key) ?
                partitions_[key].get() : LoadPartition(key);

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

    // ============================================================================
    // Public API: Lookup
    // ============================================================================

    void PartitionedVdb::Lookup(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
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
                for (auto& item : *list) {
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

        // Query each partition
        std::vector<std::tuple<unsigned long long, float, std::string, std::string>> allResults;

        for (const auto& key : matchingKeys) {
            Partition* partition = partitions_.count(key) ?
                partitions_[key].get() : LoadPartition(key);

            if (!partition) continue;

            auto results = partition->index->Lookup(query, topK);

            // Dedup within this partition if enabled
            if (dedupThreshold > 0.0f && results.size() > 1) {
                // Fetch vectors for all results in parallel
                std::vector<std::vector<float>> vectors(results.size());

#pragma omp parallel for
                for (long long i = 0; i < static_cast<long long>(results.size()); ++i) {
                    vectors[i] = partition->index->GetVectorById(results[i].first);
                }

                // Get deduplicated indices
                std::vector<size_t> keptIndices = dedup_results(
                    results, vectors, dimension_, dedupThreshold);

                // Add only kept results
                for (size_t idx : keptIndices) {
                    unsigned long long extId = partition->vdb->GetIdByIndex(results[idx].first);
                    std::string text = partition->vdb->GetTextById(extId);
                    allResults.emplace_back(extId, results[idx].second, text, key);
                }
            }
            else {
                // No dedup - add all results
                for (const auto& [internalIdx, score] : results) {
                    unsigned long long extId = partition->vdb->GetIdByIndex(internalIdx);
                    std::string text = partition->vdb->GetTextById(extId);
                    allResults.emplace_back(extId, score, text, key);
                }
            }
        }

        // Sort by score descending
        std::sort(allResults.begin(), allResults.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a) > std::get<1>(b);
            });

        // Return top K
        X::List resultList;
        size_t count = std::min(static_cast<size_t>(topK), allResults.size());
        for (size_t i = 0; i < count; ++i) {
            auto& r = allResults[i];
            X::List item;
            item += static_cast<long long>(std::get<0>(r));
            item += std::get<1>(r);
            item += std::get<2>(r);
            item += std::get<3>(r);
            resultList->AddItem(item);
        }

        retValue = resultList;
    }

    void PartitionedVdb::Grouping(X::XRuntime* rt,
        X::XObj* pContext, X::ARGS& params,
        X::KWARGS& kwParams, X::Value& retValue)
    {
        // Parse kwParams for key names
        std::string idKey = "image_id";
        std::string partitionKey = "device_id";      // partition tag key (for SQL)
        std::string timestampKey = "timestamp";
        std::string fullPartitionKey = "";           // full partition key like "2024-01_0" (for Seek)

        if (auto it = kwParams.find("id_key"); it) {
            idKey = it->val.ToString();
        }
        if (auto it = kwParams.find("partition_key"); it) {
            partitionKey = it->val.ToString();
        }
        if (auto it = kwParams.find("timestamp_key"); it) {
            timestampKey = it->val.ToString();
        }
        if (auto it = kwParams.find("full_partition_key"); it) {
            fullPartitionKey = it->val.ToString();
        }

        // Last param is threshold, all others are dict lists
        if (params.size() < 2) {
            retValue = X::List();
            return;
        }

        float threshold = (float)params[params.size() - 1];

        // Collect all items from all sources with their vectors
        // Structure: (id, source_indices, vector)
        struct ItemInfo {
            unsigned long long id;
            std::set<int> sources;  // which param indices this ID came from
            std::vector<float> vector;
        };

        std::map<unsigned long long, ItemInfo> allItems;  // id -> info

        // Process each dict list (source)
        for (size_t srcIdx = 0; srcIdx < params.size() - 1; ++srcIdx) {
            X::Value paramVal = params[srcIdx];
            if (!paramVal.IsList()) continue;

            X::List dictList(paramVal);
            for (auto& item : *dictList) {
                if (!item.IsDict()) continue;

                X::Dict dict(item);

                // Get ID
                X::Value idVal = dict->Get(idKey);
                if (!idVal.IsValid()) continue;
                unsigned long long id = static_cast<unsigned long long>(idVal.ToLongLong());

                // Get partition info to locate vector
                std::string fullKey;

                // Try full_partition_key first (for Seek results)
                if (!fullPartitionKey.empty()) {
                    X::Value fpkVal = dict->Get(fullPartitionKey);
                    if (fpkVal.IsValid()) {
                        fullKey = fpkVal.ToString();
                    }
                }

                // If no full key, construct from timestamp and partition tag
                if (fullKey.empty()) {
                    long long timestampMs = 0;
                    X::Value tsVal = dict->Get(timestampKey);
                    if (tsVal.IsValid()) {
                        timestampMs = tsVal.ToLongLong();
                    }

                    std::string partitionTag = "default";
                    X::Value ptVal = dict->Get(partitionKey);
                    if (ptVal.IsValid()) {
                        partitionTag = ptVal.ToString();
                    }

                    std::string tsPartition = TimestampToPartitionName(timestampMs);
                    int customIndex = GetCustomIndex(partitionTag);
                    if (customIndex < 0) customIndex = 0;

                    fullKey = tsPartition + "_" + std::to_string(customIndex);
                }

                // Add source index to this ID
                auto& info = allItems[id];
                info.id = id;
                info.sources.insert(static_cast<int>(srcIdx));

                // Fetch vector if not already fetched
                if (info.vector.empty()) {
                    Partition* partition = partitions_.count(fullKey) ?
                        partitions_[fullKey].get() : LoadPartition(fullKey);

                    if (partition && partition->vdb) {
                        info.vector = partition->index->GetVectorById(id);
                    }
                }
            }
        }
        // Convert to vector for processing
        std::vector<ItemInfo*> items;
        items.reserve(allItems.size());
        for (auto& [id, info] : allItems) {
            if (!info.vector.empty()) {
                items.push_back(&info);
            }
        }

        if (items.empty()) {
            retValue = X::List();
            return;
        }

        // Union-Find for grouping
        std::vector<size_t> parent(items.size());
        std::iota(parent.begin(), parent.end(), 0);

        std::function<size_t(size_t)> find = [&](size_t x) -> size_t {
            if (parent[x] != x) {
                parent[x] = find(parent[x]);
            }
            return parent[x];
            };

        auto unite = [&](size_t x, size_t y) {
            size_t px = find(x);
            size_t py = find(y);
            if (px != py) {
                parent[px] = py;
            }
            };

        // Compare all pairs and group similar ones
        size_t n = items.size();
#pragma omp parallel for schedule(dynamic)
        for (long long i = 0; i < static_cast<long long>(n); ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                float sim = quanta_cosine_similarity(
                    dimension_,
                    items[i]->vector.data(),
                    items[j]->vector.data()
                );

                if (sim >= threshold) {
#pragma omp critical
                    {
                        unite(i, j);
                    }
                }
            }
        }

        // Collect groups
        std::map<size_t, std::vector<size_t>> groups;  // root -> member indices
        for (size_t i = 0; i < n; ++i) {
            groups[find(i)].push_back(i);
        }

        // Build result: List of groups, each group is list of [id, source] pairs
        X::List result;

        for (auto& [root, members] : groups) {
            X::List group;

            for (size_t idx : members) {
                ItemInfo* info = items[idx];
                X::List pair;

                // ID
                pair += static_cast<long long>(info->id);

                // Source(s)
                if (info->sources.size() == 1) {
                    // Single source - just the index
                    pair += *info->sources.begin();
                }
                else {
                    // Multiple sources - list of indices
                    X::List sourceList;
                    for (int src : info->sources) {
                        sourceList += src;
                    }
                    pair += sourceList;
                }

                group += pair;
            }

            result += group;
        }

        retValue = result;
    }

    // ============================================================================
    // Public API: Save / Load
    // ============================================================================

    bool PartitionedVdb::Save(const std::string& path)
    {
        if (!path.empty()) {
            basePath_ = path;
        }

        fs::create_directories(basePath_);

        for (auto& [key, partition] : partitions_) {
            size_t pos = key.rfind('_');
            std::string tsPartition = key.substr(0, pos);
            int customIndex = std::stoi(key.substr(pos + 1));

            partition->index->Save(GetHnswPath(tsPartition, customIndex).string());
            partition->vdb->Save(GetVdbPath(tsPartition, customIndex).string());
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

        std::vector<std::string> allKeys = ScanMatchingPartitions(0, LLONG_MAX, {});

        for (const auto& key : allKeys) {
            X::Dict info;
            info->Set("key", key);

            size_t pos = key.rfind('_');
            std::string tsPartition = key.substr(0, pos);
            int customIndex = std::stoi(key.substr(pos + 1));

            info->Set("ts_partition", tsPartition);
            info->Set("custom_index", customIndex);

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
        info["index"] = idx;

        X::List tags;
        if (customPartitionTags_.count(idx)) {
            for (const auto& t : customPartitionTags_[idx]) {
                tags += t;
            }
        }
        info["tags"] = tags;

        return info;
    }

} // namespace Quanta