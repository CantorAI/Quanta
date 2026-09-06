#pragma once
#include "quanta_runtime.h"
#include "scene_tracker.h"
#include <map>
#include <set>
#include <unordered_set>
#include <numeric>
#include <memory>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>




#include "vdb_bucket.h"
#include "vdb_config.h"

namespace Quanta
{
    namespace fs = std::filesystem;
    class HnswVdb;
    class VectorDatabase;
    class PartitionedVdb;
    struct MetricSource {
        std::mutex mutex;
        PartitionedVdb* database = nullptr;
    };
    class VdbMetric {
        std::shared_ptr<MetricSource> source_;
        int kind_;
    public:
        VdbMetric(std::shared_ptr<MetricSource> source, int kind) : source_(std::move(source)), kind_(kind) {}
        BEGIN_PACKAGE(VdbMetric)
            APISET().AddFunc<0>("Fetch", &VdbMetric::Fetch);
        END_PACKAGE
        long long Fetch();
    };

    // WalRecordHeader moved to bucket_storage.h

    class PartitionedVdb
    {
        friend class SceneTracker;
        friend class VdbMetric;
        std::shared_ptr<QuantaContext> context_;
        std::weak_ptr<VdbCache> cache_;
        std::string cache_key_;
        std::shared_ptr<MetricSource> metric_source_ = std::make_shared<MetricSource>();
        mutable std::mutex error_mutex_;
        std::string background_error_;
        std::mutex close_mutex_;
        mutable std::recursive_mutex metadata_mutex_;
        void RecordBackgroundError(const std::string& message);
        X3Value self_{}; // Borrowed; trackers retain their parent explicitly.
        std::unique_ptr<VdbConfig> m_config;
        // Config stored as map, synced to SQLite
        std::map<std::string, std::string> config_;

        // Paths
        fs::path basePath_;
        std::string prefix_;
        uint64_t wal_sequence_ = 0;
        bool OwnsWal(const std::string& filename) const;

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
        std::atomic<long long> ttl_minutes_{3};
        long long auto_save_seconds_ = 300;
        int max_loaded_read_only_partitions_ = 50;

        // Concurrency
        mutable std::mutex partitions_mutex_;
        std::thread maintenance_thread_;
        std::atomic<bool> stop_thread_{false};

        // WAL Maintenance Concurrency
        std::queue<std::string> pending_wals_;
        std::mutex wals_mutex_;
        std::condition_variable wals_cv_;
        int wal_rotation_threshold_ = 100; // Micro-batch size
        long long wal_cooling_time_seconds_ = 60; // Time without new appends before forcing flush

        // Metrics Tracking
        std::atomic<long long> active_lookups_{0};
        std::atomic<long long> total_lookups_{0};
        std::atomic<long long> total_add_vectors_{0};
        std::atomic<long long> total_grouping_{0};
        std::atomic<long long> total_buckets_{0};
        std::atomic<long long> total_records_{0};
        std::atomic<long long> total_wals_processed_{0};
        std::atomic<long long> total_wal_vectors_merged_{0};
        // Custom partitions: index -> tags
        std::map<int, std::set<std::string>> customPartitionTags_;
        std::map<std::string, int> tagToIndex_;
        int nextCustomIndex_ = 1;  // 0 = "default"

        // Loaded partitions: "2024-01_0" -> Partition
        std::map<std::string, std::shared_ptr<VdbBucket>> partitions_;

        // Type aliases used by the Grouping helpers
        struct GroupingItem {
            unsigned long long id;
            std::set<int> sources;   // which source-list indices this ID came from
            std::vector<float> vector;
        };
        using GroupingItemMap = std::map<unsigned long long, GroupingItem>;

        struct AsyncAddTask {
            long long timestampMs;
            std::string partitionTag;
            int numThreads;
            std::vector<unsigned long long> extIds;
            std::vector<std::string> chunkTexts;
            std::vector<float> vectors;
            size_t n;
            int dimension;
        };

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
        APISET().AddFunc<0>("GetHealth", &PartitionedVdb::GetHealth);
        APISET().AddFunc<0>("GetTotalRecords", &PartitionedVdb::GetTotalRecords);
        APISET().AddFunc<0>("PerformFullScan", &PartitionedVdb::PerformFullScan);
        APISET().AddFunc<1>("SetTTL", &PartitionedVdb::SetTTL);
        APISET().AddVarFunc("CreateTracker", &PartitionedVdb::CreateTracker);
        END_PACKAGE

        PartitionedVdb(std::shared_ptr<QuantaContext> context, std::weak_ptr<VdbCache> cache, std::string key);
        virtual ~PartitionedVdb();
        void OnInstanceCreated(const X::Value& value) { self_ = value.raw(); }
        X::Value CreateTracker(const X::ARGS&, const X::KWARGS&);

        X::Value Init(const X::ARGS& params, const X::KWARGS& kwParams);

        bool Close();
        bool Save(const std::string& path);
        bool Load(const std::string& path);

        X::Value AddVectors(const X::ARGS& params, const X::KWARGS& kwParams);
        X::Value QueryLabelByID(const X::ARGS& params, const X::KWARGS& kwParams);
        X::Value Lookup(const X::ARGS& params, const X::KWARGS& kwParams);
        X::Value Grouping(const X::ARGS& params, const X::KWARGS& kwParams);
        bool AddPartitionTag(int index, const std::string& tag);
        X::Value ListPartitions();
        X::Value GetPartitionInfo(const std::string& tag);
        X::Value GetHealth();
        X::Value GetTotalRecords();
        X::Value PerformFullScan();
        bool SetTTL(long long ttlMinutes);
        void RegMetrics();

        // Accessors for SceneTracker
        int GetDimension() const { return dimension_; }
        std::string GetBasePath() const { return basePath_.string(); }

    private:
        X::Value metrics_mgr_;
        X::Value registerMetrics_;
        
        std::atomic<bool> is_closed_{true};
        // Asynchronous Ingestion
        std::queue<AsyncAddTask> ingestion_queue_;
        std::mutex ingestion_mutex_;
        std::condition_variable ingestion_cv_;
        std::thread ingestion_thread_;
        void IngestionLoop();

        void MaintenanceLoop();
        void StartMaintenanceThread();
        void ProcessWalFile(const std::string& unmerged_wal);
        void ProcessWalFileBuffer(const std::string& target_key, const std::vector<char>& buffer);

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
        std::map<std::string, int> highest_buckets_;
        std::shared_ptr<VdbBucket> GetOrCreatePartition(const std::string& tsPartition, int customIndex);
        std::shared_ptr<VdbBucket> LoadPartition(const std::string& key);
        bool SavePartition(std::shared_ptr<VdbBucket> p, const std::string& key);

        // File path helpers
        fs::path GetDbPath();

        // Grouping helpers
        std::string ResolveItemPartitionKey(
            X::Value& dict,
            const std::string& fullPartitionKey,
            const std::string& partitionKey,
            const std::string& timestampKey,
            bool& customIndexUnknown);

        std::vector<float> FetchVectorForItem(
            unsigned long long id,
            const std::string& fullKey,
            bool customIndexUnknown);

        void CollectGroupingItems(
            X::Value& itemsList,
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
