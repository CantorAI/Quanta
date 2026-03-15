#pragma once
#include "xpackage.h"
#include "scene_tracker.h"
#include <map>
#include <set>
#include <numeric>
#include <memory>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>




namespace Quanta
{
    namespace fs = std::filesystem;
    class HnswVdb;
    class VectorDatabase;

    // WAL Binary Record Format
    // We write [WalRecordHeader] + [vector_floats...] + [chunk_text_bytes]
    #pragma pack(push, 1)
    struct WalRecordHeader {
        unsigned long long external_id;
        unsigned long long timestamp_ms;
        unsigned int chunk_text_length;
    };
    #pragma pack(pop)

    // Single partition: index + database
    struct Partition {
        std::unique_ptr<HnswVdb> index;
        std::unique_ptr<VectorDatabase> vdb;
        size_t count = 0;
        std::atomic<long long> last_access_ms_{0};
        std::atomic<long long> last_save_ms_{0};
        std::string key_;
        std::atomic<bool> is_dirty_{false};
        std::atomic<bool> is_historical_read_{false};
        std::atomic<long long> ts_start_{0};
        std::atomic<long long> ts_end_{0};
        
        // WAL Tracking per Partition
        std::string active_wal_filename_;       // Current file being appended to
        int active_wal_record_count_ = 0;       // Number of vectors in the active file
    };

    class PartitionedVdb
    {
        friend class SceneTracker;
        X::Value m_sqlite;
        X::Value m_configDb;
        // Config stored as map, synced to SQLite
        std::map<std::string, std::string> config_;

        // Paths
        fs::path basePath_;
        std::string prefix_;

        // Cached config values
        std::string tsGranularity_ = "hourly";
        int dimension_ = 512;
        std::string spaceName_ = "l2";
        float maxMemoryGb_ = 1.0f;
        size_t maxElements_ = 500000;       // Dynamically calculated during Init
        int M_ = 16;
        int efConstruction_ = 200;
        int efSearch_ = 50;

        // Maintenance and TTL configuration
        long long ttl_minutes_ = 60;
        long long auto_save_seconds_ = 300;
        int max_loaded_read_only_partitions_ = 50;

        // Concurrency
        mutable std::recursive_mutex partitions_mutex_;
        std::thread maintenance_thread_;
        std::atomic<bool> stop_thread_{false};

        // WAL Maintenance Concurrency
        std::queue<std::string> pending_wals_;
        std::mutex wals_mutex_;
        std::condition_variable wals_cv_;
        int wal_rotation_threshold_ = 100; // Micro-batch size

        // Metrics Tracking
        std::atomic<long long> total_lookups_{0};
        std::atomic<long long> total_add_vectors_{0};
        std::atomic<long long> total_grouping_{0};
        std::atomic<long long> total_buckets_{0};
        std::atomic<long long> total_wals_processed_{0};
        std::atomic<long long> total_wal_vectors_merged_{0};
        // Custom partitions: index -> tags
        std::map<int, std::set<std::string>> customPartitionTags_;
        std::map<std::string, int> tagToIndex_;
        int nextCustomIndex_ = 1;  // 0 = "default"

        // Loaded partitions: "2024-01_0" -> Partition
        std::map<std::string, std::shared_ptr<Partition>> partitions_;

        // Type aliases used by the Grouping helpers
        struct GroupingItem {
            unsigned long long id;
            std::set<int> sources;   // which source-list indices this ID came from
            std::vector<float> vector;
        };
        using GroupingItemMap = std::map<unsigned long long, GroupingItem>;

    public:
        BEGIN_PACKAGE(PartitionedVdb)
            APISET().AddVarFunc("Init", &PartitionedVdb::Init);
        APISET().AddFunc<0>("Close", &PartitionedVdb::Close);
        APISET().AddFunc<1>("Load", &PartitionedVdb::Load);
        APISET().AddVarFunc("AddVectors", &PartitionedVdb::AddVectors);
        APISET().AddVarFunc("Lookup", &PartitionedVdb::Lookup);
        APISET().AddFunc<2>("AddPartitionTag", &PartitionedVdb::AddPartitionTag);
        APISET().AddFunc<0>("ListPartitions", &PartitionedVdb::ListPartitions);
        APISET().AddVarFunc("QueryLabelByID", &PartitionedVdb::QueryLabelByID);
        APISET().AddVarFunc("Grouping", &PartitionedVdb::Grouping);
        APISET().AddFunc<1>("GetPartitionInfo", &PartitionedVdb::GetPartitionInfo);
        APISET().AddVarClass<SceneTracker, PartitionedVdb>("CreateTracker");
        END_PACKAGE

            PartitionedVdb(X::ARGS& params, X::KWARGS& kwParams);
        virtual ~PartitionedVdb();

        bool Init(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);

        bool Close();
        bool Save(const std::string& path);
        bool Load(const std::string& path);

        void AddVectors(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        bool QueryLabelByID(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        void Lookup(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        void Grouping(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        bool AddPartitionTag(int index, const std::string& tag);
        X::Value ListPartitions();
        X::Value GetPartitionInfo(const std::string& tag);

        // Accessors for SceneTracker
        int GetDimension() const { return dimension_; }

    private:
        void MaintenanceLoop();
        void StartMaintenanceThread();
        void ProcessWalFile(const std::string& unmerged_wal);
        void ProcessWalFileBuffer(const std::string& target_key, const std::vector<char>& buffer);

        void InitDatabase();
        void SyncConfigToDB();
        void LoadConfigFromDB();
        void LoadCustomPartitionsFromDB();
        void SaveCustomPartitionToDB(int index, const std::string& tag);

        // Config helpers
        void SetConfig(const std::string& key, const std::string& value);
        std::string GetConfig(const std::string& key, const std::string& defaultVal = "");
        void ApplyConfigToMembers();
        void UpdateConfigFromMembers();

        // Timestamp helpers (all timestamps in milliseconds)
        std::string TimestampToPartitionName(long long timestampMs);
        std::pair<long long, long long> PartitionNameToTimeRange(const std::string& tsPartition);
        int CountChar(const std::string& s, char c);

        // Partition helpers
        int GetOrCreateCustomIndex(const std::string& tag);
        int GetCustomIndex(const std::string& tag);
        std::set<int> ResolveTagsToIndices(const std::vector<std::string>& tags);

        std::vector<std::string> ScanMatchingPartitions(
            long long tsStartMs, long long tsEndMs,
            const std::set<int>& customIndices);

        std::shared_ptr<Partition> GetOrCreatePartition(const std::string& tsPartition, int customIndex);
        std::shared_ptr<Partition> LoadPartition(const std::string& key);
        bool SavePartition(std::shared_ptr<Partition> p, const std::string& key);

        // File path helpers
        fs::path GetDbPath();

        // Grouping helpers
        std::string ResolveItemPartitionKey(
            X::Dict& dict,
            const std::string& fullPartitionKey,
            const std::string& partitionKey,
            const std::string& timestampKey,
            bool& customIndexUnknown);

        std::vector<float> FetchVectorForItem(
            unsigned long long id,
            const std::string& fullKey,
            bool customIndexUnknown);

        void CollectGroupingItems(
            X::List& itemsList,
            const std::string& idKey,
            const std::string& partitionKey,
            const std::string& timestampKey,
            const std::string& fullPartitionKey,
            GroupingItemMap& allItems);

        std::map<size_t, std::vector<size_t>> RunCentroidClustering(
            const std::vector<GroupingItem*>& items,
            float threshold);

        X::Value BuildGroupingResult(
            const std::vector<GroupingItem*>& items,
            const std::map<size_t, std::vector<size_t>>& groups);
    };
}