#include "vdb_bucket.h"
#include "VectorDatabase.h"
#include "HnswVdb.h"
#include "bucket_storage.h"
#include <iostream>

namespace Quanta {

    VdbBucket::VdbBucket(int dimension, const std::string& spaceName, size_t maxElements, int M, int efConstruction, int efSearch, const std::string& key)
        : dimension_(dimension), spaceName_(spaceName), maxElements_(maxElements), M_(M), efConstruction_(efConstruction), efSearch_(efSearch), key_(key)
    {
        vdb_ = std::make_unique<VectorDatabase>(dimension_);
        size_t initialCapacity = (maxElements_ < 1000) ? maxElements_ : 1000;
        index_ = std::make_unique<HnswVdb>(spaceName_, dimension_, initialCapacity, M_, efConstruction_, efSearch_);
        
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        last_access_ms_ = now_ms;
        last_save_ms_ = now_ms;
    }

    VdbBucket::~VdbBucket() {
        // Pointers automatically cleaned up by unique_ptr
    }

    bool VdbBucket::Load(const std::filesystem::path& vdbPath, const std::filesystem::path& hnswPath) {
        // NO LOCK REQUIRED HERE - This is called during initialization before the pointer is exposed to the map
        if (!std::filesystem::exists(vdbPath) || !std::filesystem::exists(hnswPath)) {
            return false;
        }

        vdb_->Load(vdbPath.string());
        count_ = vdb_->GetSize();
        total_inserted_count_ = count_.load();

        index_->Load(hnswPath.string());
        is_historical_read_ = true;

        return true;
    }

    bool VdbBucket::Save(const std::filesystem::path& basePath, const std::string& prefix) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        
        std::filesystem::path hnswPath = basePath / (prefix + "_" + key_ + ".hnsw");
        std::filesystem::path vdbPath = basePath / (prefix + "_" + key_ + ".vdb");
        vdb_->Save(vdbPath.string());
        index_->Save(hnswPath.string());
        is_dirty_ = false;

        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        last_save_ms_ = now_ms;

        return true;
    }

    bool VdbBucket::AddVector(unsigned long long extId, const float* vectorData, const std::string& chunkText) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        
        std::vector<unsigned long long> extIds = { extId };
        std::vector<std::string> chunkTexts = { chunkText };
        std::vector<unsigned long long> internalIndices = vdb_->AddLabels(extIds, chunkTexts, 0);
        
        size_t new_total = vdb_->GetSize();
        if (new_total > index_->GetMaxElements()) {
            size_t new_max = index_->GetMaxElements() * 2;
            if (new_max < new_total) new_max = new_total + 1000;
            if (new_max > maxElements_) new_max = maxElements_;
            index_->Resize(new_max);
        }
        
        index_->AddVectors(internalIndices, vectorData, dimension_, 1);
        count_ = new_total;
        is_dirty_ = true;
        is_historical_read_ = false;

        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        last_access_ms_ = now_ms;
        
        // Update Time Bounding Box
        if (ts_start_ == 0 || ts_start_ > now_ms) ts_start_ = now_ms;
        if (ts_end_ < now_ms) ts_end_ = now_ms;

        return count_ < maxElements_; // return false if bucket is full after adding
    }

    bool VdbBucket::AddVectorsBatch(const std::vector<unsigned long long>& extIds, 
                                  const std::vector<float>& vectors, 
                                  const std::vector<std::string>& chunkTexts,
                                  unsigned long long maxTs) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        
        std::vector<unsigned long long> internalIndices = vdb_->AddLabels(extIds, chunkTexts, maxTs);
        
        size_t new_total = vdb_->GetSize();
        if (new_total > index_->GetMaxElements()) {
            size_t new_max = index_->GetMaxElements() * 2;
            if (new_max < new_total) new_max = new_total + 1000;
            if (new_max > maxElements_) new_max = maxElements_;
            index_->Resize(new_max);
        }
        
        index_->AddVectors(internalIndices, vectors.data(), vectors.size(), 1);
        count_ = new_total;
        is_dirty_ = true;
        is_historical_read_ = false;

        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        last_access_ms_ = now_ms;
        
        if (ts_start_ == 0 || ts_start_ > now_ms) ts_start_ = now_ms;
        if (ts_end_ < now_ms) ts_end_ = now_ms;

        return count_ < maxElements_;
    }

    std::vector<std::pair<unsigned long long, float>> VdbBucket::Lookup(const std::vector<float>& query, int topK) {
        // For search, we can use the HnswVdb's own thread-safe lookup.  However, 
        // to prevent data structures from migrating during active searching, we should lock.
        // HNSW itself allows concurrent search and adding, but our VectorDatabase mapping might not if resizing. Note: Quanta VectorDatabase reserves ahead of time, but to be completely safe against bucket rotation races, we grab our bucket_lock_.
        std::lock_guard<std::mutex> plock(bucket_lock_);
        
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        last_access_ms_ = now_ms;
        
        return index_->Lookup(query, topK);
    }

    std::vector<float> VdbBucket::GetVectorById(unsigned long long internalId) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        return index_->GetVectorById(internalId);
    }

    std::vector<float> VdbBucket::GetVectorByExternalId(unsigned long long externalId) {
        std::lock_guard<std::mutex> lock(bucket_lock_);
        const auto index = vdb_->GetIndexById(externalId);
        return index < 0 ? std::vector<float>{} : index_->GetVectorById(static_cast<unsigned long long>(index));
    }

    unsigned long long VdbBucket::GetIdByIndex(unsigned long long internalIdx) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        return vdb_->GetIdByIndex(internalIdx);
    }

    std::string VdbBucket::GetTextById(unsigned long long extId) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        return vdb_->GetTextById(extId);
    }

    unsigned long long VdbBucket::GetTimestampById(unsigned long long extId) {
        std::lock_guard<std::mutex> plock(bucket_lock_);
        return vdb_->GetTimestampById(extId);
    }
}
