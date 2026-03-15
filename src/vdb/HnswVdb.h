#ifndef HNSW_VDB_H
#define HNSW_VDB_H

#include "hnswlib/hnswlib.h"   // Adjust include path as needed
#include <vector>
#include <string>
#include <thread>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <queue>
#include <omp.h>
#include <filesystem>

namespace Quanta {

    class HnswVdb {
    public:
        /// Construct with space = "l2", "ip", or "cosine"
        HnswVdb(const std::string& space_name,
            int dimension,
            size_t max_elements,
            int M = 16,
            int ef_construction = 200,
            int ef_search = 50)
            : space_name_(space_name),
            dim_(dimension),
            M_(M),
            ef_construction_(ef_construction),
            ef_search_(ef_search),
            max_elements_(max_elements),
            normalize_(false),
            space_(nullptr),
            appr_alg_(nullptr)
        {
            // select metric space
            if (space_name_ == "l2") {
                space_ = new hnswlib::L2Space(dim_);
            }
            else if (space_name_ == "ip") {
                space_ = new hnswlib::InnerProductSpace(dim_);
            }
            else if (space_name_ == "cosine") {
                space_ = new hnswlib::InnerProductSpace(dim_);
                normalize_ = true;
            }
            else {
                throw std::runtime_error("Space name must be \"l2\", \"ip\", or \"cosine\".");
            }

            // build index
            appr_alg_ = new hnswlib::HierarchicalNSW<float>(
                space_, max_elements_, M_, ef_construction_);
            appr_alg_->setEf(ef_search_);

            num_threads_default_ = std::thread::hardware_concurrency();
        }

        ~HnswVdb() {
            delete appr_alg_;
            delete space_;
        }
        /// Get vector by label/ID - returns empty vector if not found
        std::vector<float> GetVectorById(unsigned long long label) const {
            try {
                return appr_alg_->getDataByLabel<float>(
                    static_cast<hnswlib::labeltype>(label));
            }
            catch (...) {
                return std::vector<float>();
            }
        }

        /// Add a batch of vectors (each of length dim_). Labels.size() == vectors.size() / dim_
        void AddVectors(const std::vector<unsigned long long>& labels,
			const float* vectors, unsigned long long vec_n,
            int num_threads = -1)
        {
            size_t n = labels.size();
            if (vec_n != n * static_cast<size_t>(dim_)) {
                throw std::runtime_error(
                    "AddVectors: expected " +
                    std::to_string(n * dim_) +
                    " floats, got " +
                    std::to_string(vec_n)
                );
            }

            // determine thread count
            int threads = (num_threads < 1
                ? static_cast<int>(num_threads_default_)
                : num_threads);
            if (n <= static_cast<size_t>(threads) * 4) {
                threads = 1;
            }

            // worker lambda
            auto worker = [&](size_t i) {
                const float* ptr = vectors + i * dim_;
                // optionally normalize each vector once
                if (normalize_) {
                    std::vector<float> tmp(dim_);
                    normalize_vector(ptr, tmp.data());
                    appr_alg_->addPoint((void*)tmp.data(),
                        static_cast<hnswlib::labeltype>(labels[i]));
                }
                else {
                    appr_alg_->addPoint((void*)ptr,
                        static_cast<hnswlib::labeltype>(labels[i]));
                }
                };

            if (threads == 1) {
                for (size_t i = 0; i < n; ++i) worker(i);
            }
            else {
#pragma omp parallel for num_threads(threads)
                for (long long i = 0; i < static_cast<long long>(n); ++i) {
                    worker(i);
                }
            }
        }

        /// Query topK similar items; returns (label, similarity)
        std::vector<std::pair<unsigned long long, float>>
            Lookup(const std::vector<float>& query, int topK)
        {
            if ((int)query.size() != dim_) {
                throw std::runtime_error("Lookup: query size mismatch.");
            }

            // optionally normalize query
            std::vector<float> qbuf;
            const float* qptr = query.data();
            if (normalize_) {
                qbuf.resize(dim_);
                normalize_vector(qptr, qbuf.data());
                qptr = qbuf.data();
            }

            // search
            auto result = appr_alg_->searchKnn(qptr, static_cast<size_t>(topK));

            // collect and convert to (label, similarity)
            std::vector<std::pair<unsigned long long, float>> out;
            while (!result.empty()) {
                auto& p = result.top();
                float dist = p.first;
                float sim;
                // distance -> similarity
                if (std::isnan(dist) || std::isinf(dist) || dist < 0.0f) {
                    sim = 0.0f;  // treat invalid distances as zero similarity
                    // optionally log warning here
                }
                else {
                    sim = 1.0f / (1.0f + dist);
                }
                out.emplace_back(
                    static_cast<unsigned long long>(p.second),
                    sim
                );
                result.pop();
            }
            // reverse so highest sim first
            std::reverse(out.begin(), out.end());
            return out;
        }

        /// Persist index to disk atomically using .new and .old
        void Save(const std::string& filename) {
            std::string base = NormalizeFilename(filename);
            std::string finalPath = base + ".hnsw";
            std::string newPath = base + ".hnsw.new";
            std::string oldPath = base + ".hnsw.old";
            
            std::error_code ec;
            // Clean up any stale .new file just in case
            if (std::filesystem::exists(newPath, ec)) {
                std::filesystem::remove(newPath, ec);
            }

            // 1. Write to .new file
            appr_alg_->saveIndex(newPath.c_str());
            
            // 2. Rename existing .hnsw to .old (if exists)
            if (std::filesystem::exists(finalPath, ec)) {
                if (std::filesystem::exists(oldPath, ec)) {
                    std::filesystem::remove(oldPath, ec);
                }
                std::filesystem::rename(finalPath, oldPath, ec);
            }
            
            // 3. Rename .new to .hnsw
            std::filesystem::rename(newPath, finalPath, ec);
            
            // 4. Delete .old
            if (std::filesystem::exists(oldPath, ec)) {
                std::filesystem::remove(oldPath, ec);
            }
        }

        /// Load index from disk (expects .hnsw)
        void Load(const std::string& filename) {
            std::string base = NormalizeFilename(filename);
            std::string finalPath = base + ".hnsw";
            std::string newPath = base + ".hnsw.new";
            std::string oldPath = base + ".hnsw.old";
            
            std::error_code ec;
            // Recover from interrupted save
            if (!std::filesystem::exists(finalPath, ec)) {
                if (std::filesystem::exists(newPath, ec)) {
                    std::filesystem::rename(newPath, finalPath, ec);
                } else if (std::filesystem::exists(oldPath, ec)) {
                    std::filesystem::rename(oldPath, finalPath, ec);
                }
            }

            // rebuild new index object
            delete appr_alg_;
            appr_alg_ = new hnswlib::HierarchicalNSW<float>(
                space_, max_elements_, M_, ef_construction_);
            appr_alg_->setEf(ef_search_);
            
            try {
                if (std::filesystem::exists(finalPath, ec)) {
                    appr_alg_->loadIndex(finalPath.c_str(),
                        space_,
                        max_elements_);
                }
            } catch (const std::exception& e) {
                std::cerr << "[HnswVdb] WARNING: Failed to load index from " << finalPath << " (" << e.what() << "). Resetting to empty index.\n";
                // Destroy the corrupted in-memory instance and recreate clean
                delete appr_alg_;
                appr_alg_ = new hnswlib::HierarchicalNSW<float>(
                    space_, max_elements_, M_, ef_construction_);
                appr_alg_->setEf(ef_search_);
            } catch (...) {
                std::cerr << "[HnswVdb] WARNING: Unknown fatal error loading " << finalPath << ". Resetting to empty index.\n";
                delete appr_alg_;
                appr_alg_ = new hnswlib::HierarchicalNSW<float>(
                    space_, max_elements_, M_, ef_construction_);
                appr_alg_->setEf(ef_search_);
            }
        }

        /// Dynamically resize the maximum allowed elements in the graph
        void Resize(size_t new_max_elements) {
            if (new_max_elements > max_elements_) {
                appr_alg_->resizeIndex(new_max_elements);
                max_elements_ = new_max_elements;
            }
        }

        size_t GetMaxElements() const {
            return max_elements_;
        }

        size_t GetCurrentCount() const {
            return appr_alg_->cur_element_count;
        }

    private:
        std::string space_name_;
        int         dim_;
        int         M_;
        int         ef_construction_;
        int         ef_search_;
        size_t      max_elements_;
        bool        normalize_;
        size_t      num_threads_default_;

        hnswlib::SpaceInterface<float>* space_;
        hnswlib::HierarchicalNSW<float>* appr_alg_;

        static std::string NormalizeFilename(const std::string& f) {
            auto pos = f.find_last_of('.');
            return (pos == std::string::npos ? f : f.substr(0, pos));
        }

        static void normalize_vector(const float* data, float* out) {
            float norm = 0.0f;
            for (int i = 0; i < (int)std::distance(data, data + 1); ++i) {
                norm += data[i] * data[i];
            }
            norm = 1.0f / (std::sqrt(norm) + 1e-30f);
            for (int i = 0; i < (int)std::distance(data, data + 1); ++i) {
                out[i] = data[i] * norm;
            }
        }
    };

} // namespace Quanta

#endif // HNSW_VDB_H
