#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <filesystem>
#include "xpackage.h"

namespace Quanta {
    class VectorDatabase;
    class HnswVdb;

    class VdbBucket {
    public:
        // Initialization
        VdbBucket(int dimension, const std::string& spaceName, size_t maxElements, int M, int efConstruction, int efSearch, const std::string& key);
        ~VdbBucket();

        // Core encapsulated functions
        bool Load(const std::filesystem::path& vdbPath, const std::filesystem::path& hnswPath);
        bool Save(const std::filesystem::path& basePath, const std::string& prefix);
        
        bool AddVector(unsigned long long extId, const float* vectorData, const std::string& chunkText);
        bool AddVectorsBatch(const std::vector<unsigned long long>& extIds, 
                           const std::vector<float>& vectors, 
                           const std::vector<std::string>& chunkTexts, unsigned long long maxTs = 0);
                           
        std::vector<std::pair<unsigned long long, float>> Lookup(const std::vector<float>& query, int topK);
        
        std::vector<float> GetVectorById(unsigned long long internalId);
        unsigned long long GetIdByIndex(unsigned long long internalIdx);
        std::string GetTextById(unsigned long long extId);
        unsigned long long GetTimestampById(unsigned long long extId);

        // Accessors for Map Router
        const std::string& GetKey() const { return key_; }
        size_t GetCount() const { return count_; }
        bool IsDirty() const { return is_dirty_; }
        void ClearDirty() { is_dirty_ = false; }
        bool IsHistoricalRead() const { return is_historical_read_; }
        void SetHistoricalRead(bool val) { is_historical_read_ = val; }
        
        long long GetLastAccessMs() const { return last_access_ms_; }
        void UpdateAccessTime(long long now_ms) { last_access_ms_ = now_ms; }
        
        long long GetLastSaveMs() const { return last_save_ms_; }
        void UpdateSaveTime(long long now_ms) { last_save_ms_ = now_ms; }

        // Time bounding box
        long long GetTsStart() const { return ts_start_; }
        long long GetTsEnd() const { return ts_end_; }
        void SetBounds(long long start, long long end) { ts_start_ = start; ts_end_ = end; }

        // WAL Active State
        std::string active_wal_filename_ = "";
        std::atomic<int> active_wal_record_count_{0};
        std::atomic<long long> last_wal_append_ms_{0};
        std::atomic<size_t> total_inserted_count_{0};

        size_t GetTotalInsertedCount() const { return total_inserted_count_; }
        void IncrementTotalInsertedCount(size_t n) { total_inserted_count_ += n; }

        // Exposed temporarily for direct BucketStorage access if necessary, 
        // but prefer using encapsulated Load/Save methods
        std::mutex& GetLock() { return bucket_lock_; }

    private:
        std::unique_ptr<VectorDatabase> vdb_;
        std::unique_ptr<HnswVdb> index_;
        
        std::string key_;
        size_t count_ = 0;
        int dimension_;
        std::string spaceName_;
        size_t maxElements_;
        int M_;
        int efConstruction_;
        int efSearch_;

        // Tier 3: Time Bounding Box
        std::atomic<long long> ts_start_{0};
        std::atomic<long long> ts_end_{0};

        // Dynamic memory eviction metadata
        std::atomic<bool> is_dirty_{false};
        std::atomic<bool> is_historical_read_{false};
        std::atomic<long long> last_access_ms_{0};
        std::atomic<long long> last_save_ms_{0};

        // Fine-grained lock for isolated physical disk I/O and vector additions
        mutable std::mutex bucket_lock_;
    };
}
