#pragma once
#include "xpackage.h"
#include <map>
#include <set>
#include <numeric>
#include <memory>
#include <filesystem>

namespace Quanta
{
    namespace fs = std::filesystem;
	class HnswVdb;
	class VectorDatabase;

    // Single partition: index + database
    struct Partition {
        std::unique_ptr<HnswVdb> index;
        std::unique_ptr<VectorDatabase> vdb;
        size_t count = 0;
    };

    class PartitionedVdb
    {
        X::Value m_sqlite;
        X::Runtime m_rt;
        X::Value m_configDb;
        // Config stored as map, synced to SQLite
        std::map<std::string, std::string> config_;

        // Paths
        fs::path basePath_;
        std::string prefix_;

        // Cached config values
        std::string tsGranularity_ = "monthly";
        int dimension_ = 512;
        std::string spaceName_ = "l2";
        size_t maxElements_ = 100000;
        int M_ = 16;
        int efConstruction_ = 200;
        int efSearch_ = 50;

        // Custom partitions: index ¡ú tags
        std::map<int, std::set<std::string>> customPartitionTags_;
        std::map<std::string, int> tagToIndex_;
        int nextCustomIndex_ = 1;  // 0 = "default"

        // Loaded partitions: "2024-01_0" ¡ú Partition
        std::map<std::string, std::unique_ptr<Partition>> partitions_;

    public:
        BEGIN_PACKAGE(PartitionedVdb)
            APISET().AddVarFunc("Init", &PartitionedVdb::Init);
        APISET().AddFunc<1>("Save", &PartitionedVdb::Save);
        APISET().AddFunc<1>("Load", &PartitionedVdb::Load);
        APISET().AddVarFunc("AddVectors", &PartitionedVdb::AddVectors);
        APISET().AddVarFunc("Lookup", &PartitionedVdb::Lookup);
        APISET().AddFunc<2>("AddPartitionTag", &PartitionedVdb::AddPartitionTag);
        APISET().AddFunc<0>("ListPartitions", &PartitionedVdb::ListPartitions);
        APISET().AddVarFunc("QueryLabelByID", &PartitionedVdb::QueryLabelByID);
        APISET().AddFunc<1>("GetPartitionInfo", &PartitionedVdb::GetPartitionInfo);
        END_PACKAGE

            PartitionedVdb(X::ARGS& params, X::KWARGS& kwParams);
        virtual ~PartitionedVdb();

        bool Init(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);

        bool Save(const std::string& path);
        bool Load(const std::string& path);

        void AddVectors(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        bool QueryLabelByID(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        void Lookup(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);

        bool AddPartitionTag(int index, const std::string& tag);
        X::Value ListPartitions();
        X::Value GetPartitionInfo(const std::string& tag);

    private:
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

        Partition* GetOrCreatePartition(const std::string& tsPartition, int customIndex);
        Partition* LoadPartition(const std::string& key);

        // File path helpers
        fs::path GetDbPath();
        fs::path GetHnswPath(const std::string& tsPartition, int customIndex);
        fs::path GetVdbPath(const std::string& tsPartition, int customIndex);
    };
}