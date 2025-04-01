#ifndef LSH_VDB_H
#define LSH_VDB_H

#include "VectorDatabase.h"
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <omp.h>
#include <algorithm>

namespace Quanta {

    using std::unordered_map;
    using std::unordered_set;

    // Derived class implementing ANN lookup via a simple LSH method.
    class LshVdb : public VectorDatabase {
    public:
        int L;   // Number of hash tables.
        int k;   // Number of hyperplanes per table.
        // LSH members:
        vector<vector<vector<float>>> lsh_hyperplanes;
        vector<unordered_map<uint64_t, vector<int>>> lsh_tables;
        bool lsh_built;

        LshVdb(int dimension, int L_tables = 5, int k_hyperplanes = 10)
            : VectorDatabase(dimension), L(L_tables), k(k_hyperplanes), lsh_built(false) {
        }

        virtual ~LshVdb() {}

        // Build the LSH index.
        void build_lsh_index() {
            lsh_tables.clear();
            lsh_tables.resize(L);
            lsh_hyperplanes.clear();
            lsh_hyperplanes.resize(L, vector<vector<float>>(k, vector<float>(D, 0.0f)));

            std::random_device rd;
            std::mt19937 gen(rd());
            std::normal_distribution<float> dist(0.0f, 1.0f);

            for (int i = 0; i < L; i++) {
                for (int j = 0; j < k; j++) {
                    for (int d = 0; d < D; d++) {
                        lsh_hyperplanes[i][j][d] = dist(gen);
                    }
                }
            }
            int numRecords = ids.size();
            for (int idx = 0; idx < numRecords; idx++) {
                const float* vec = &data[idx * D];
                for (int i = 0; i < L; i++) {
                    uint64_t hash = compute_hash(vec, lsh_hyperplanes[i]);
                    lsh_tables[i][hash].push_back(idx);
                }
            }
            lsh_built = true;
        }

        // Helper: compute hash for a vector using a set of hyperplanes.
        uint64_t compute_hash(const float* vec, const vector<vector<float>>& hyperplanes) const {
            uint64_t hash = 0;
            for (int i = 0; i < k; i++) {
                float dot = 0.0f;
                for (int d = 0; d < D; d++) {
                    dot += vec[d] * hyperplanes[i][d];
                }
                if (dot > 0)
                    hash |= (1ULL << i);
            }
            return hash;
        }

        // Override Lookup using LSH-based ANN.
        virtual vector<pair<uint64_t, float>> Lookup(const vector<float>& query, int topK) override {
            if (!lsh_built) {
                build_lsh_index();
            }
            unordered_set<int> candidateSet;
            for (int i = 0; i < L; i++) {
                uint64_t hash = compute_hash(query.data(), lsh_hyperplanes[i]);
                if (lsh_tables[i].count(hash) > 0) {
                    for (int idx : lsh_tables[i][hash]) {
                        candidateSet.insert(idx);
                    }
                }
            }
            vector<pair<uint64_t, float>> candidates;
            vector<int> candIndices(candidateSet.begin(), candidateSet.end());
#pragma omp parallel for
            for (int i = 0; i < static_cast<int>(candIndices.size()); i++) {
                int idx = candIndices[i];
                const float* vec = &data[idx * D];
                float sim = cosine_similarity(query.data(), vec);
#pragma omp critical
                candidates.push_back({ ids[idx], sim });
            }
            std::sort(candidates.begin(), candidates.end(),
                [](const pair<uint64_t, float>& a, const pair<uint64_t, float>& b) {
                    return a.second > b.second;
                });
            if (candidates.size() > static_cast<size_t>(topK))
                candidates.resize(topK);
            return candidates;
        }

        // Extend SaveMore to store LSH state.
        virtual void SaveMore(ofstream& ofs) const override {
            ofs.write(reinterpret_cast<const char*>(&L), sizeof(L));
            ofs.write(reinterpret_cast<const char*>(&k), sizeof(k));
            ofs.write(reinterpret_cast<const char*>(&lsh_built), sizeof(lsh_built));
            // Save hyperplanes.
            for (int i = 0; i < L; i++) {
                for (int j = 0; j < k; j++) {
                    ofs.write(reinterpret_cast<const char*>(lsh_hyperplanes[i][j].data()), sizeof(float) * D);
                }
            }
            // Save LSH tables.
            for (int i = 0; i < L; i++) {
                size_t tableSize = lsh_tables[i].size();
                ofs.write(reinterpret_cast<const char*>(&tableSize), sizeof(tableSize));
                for (const auto& bucket : lsh_tables[i]) {
                    uint64_t hash = bucket.first;
                    ofs.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
                    size_t vecSize = bucket.second.size();
                    ofs.write(reinterpret_cast<const char*>(&vecSize), sizeof(vecSize));
                    ofs.write(reinterpret_cast<const char*>(bucket.second.data()), sizeof(int) * vecSize);
                }
            }
        }

        // Extend LoadMore to load LSH state.
        virtual void LoadMore(ifstream& ifs) override {
            ifs.read(reinterpret_cast<char*>(&L), sizeof(L));
            ifs.read(reinterpret_cast<char*>(&k), sizeof(k));
            ifs.read(reinterpret_cast<char*>(&lsh_built), sizeof(lsh_built));
            lsh_hyperplanes.clear();
            lsh_hyperplanes.resize(L, vector<vector<float>>(k, vector<float>(D, 0.0f)));
            for (int i = 0; i < L; i++) {
                for (int j = 0; j < k; j++) {
                    ifs.read(reinterpret_cast<char*>(lsh_hyperplanes[i][j].data()), sizeof(float) * D);
                }
            }
            lsh_tables.clear();
            lsh_tables.resize(L);
            for (int i = 0; i < L; i++) {
                size_t tableSize;
                ifs.read(reinterpret_cast<char*>(&tableSize), sizeof(tableSize));
                for (size_t b = 0; b < tableSize; b++) {
                    uint64_t hash;
                    ifs.read(reinterpret_cast<char*>(&hash), sizeof(hash));
                    size_t vecSize;
                    ifs.read(reinterpret_cast<char*>(&vecSize), sizeof(vecSize));
                    vector<int> indices(vecSize);
                    ifs.read(reinterpret_cast<char*>(indices.data()), sizeof(int) * vecSize);
                    lsh_tables[i][hash] = indices;
                }
            }
        }
    };

} // namespace Quanta

#endif // LSH_VDB_H
