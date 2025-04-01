#ifndef HNSW_VDB_H
#define HNSW_VDB_H

#include "VectorDatabase.h"
#include "hnswlib/hnswlib.h" // Adjust the include path as needed
#include <string>
#include <cstdlib>

namespace Quanta {

    // Derived class implementing ANN lookup using hnswlib.
    // This example assumes that you have integrated hnswlib into your project.
    class HnswVdb : public VectorDatabase {
    public:
        hnswlib::HierarchicalNSW<float>* appr_alg;
        // Parameters for hnswlib.
        int M;              // Maximum number of neighbors.
        int efConstruction; // Controls index construction quality.
        int efSearch;       // Controls search quality.
        size_t maxElements; // Capacity for the index.

        // Updated constructor: require a capacity for the index.
        HnswVdb(int dimension, size_t maxElements_, int maxM = 16, int efC = 200, int efS = 50)
            : VectorDatabase(dimension), M(maxM), efConstruction(efC), efSearch(efS), maxElements(maxElements_)
        {
            // Create a space (using L2 space here; adjust if needed)
            hnswlib::L2Space* l2space = new hnswlib::L2Space(dimension);
            // Use maxElements_ instead of 0.
            appr_alg = new hnswlib::HierarchicalNSW<float>(l2space, maxElements, M, efConstruction);
            appr_alg->setEf(efSearch);
        }

        virtual ~HnswVdb() {
            if (appr_alg) {
                delete appr_alg;
                appr_alg = nullptr;
            }
        }

        // Override AddVector: add to base and then to the hnswlib index.
        virtual void AddVector(uint64_t id, const std::vector<float>& vec) override {
            VectorDatabase::AddVector(id, vec);
            // Now that the index has a valid capacity, addPoint should work.
            appr_alg->addPoint(vec.data(), static_cast<size_t>(ids.size() - 1));
        }

        // Save and Load remain the same as before, with the change below.
        virtual void Save(const std::string& filename) const override {
            VectorDatabase::Save(filename + ".base");
            appr_alg->saveIndex((filename + ".hnsw").c_str());
        }

        virtual void Load(const std::string& filename) override {
            VectorDatabase::Load(filename + ".base");
            hnswlib::L2Space* l2space = new hnswlib::L2Space(D);
            if (appr_alg) {
                delete appr_alg;
            }
            // Use the same maxElements value we stored.
            appr_alg = new hnswlib::HierarchicalNSW<float>(l2space, maxElements, M, efConstruction);
            appr_alg->setEf(efSearch);
            appr_alg->loadIndex((filename + ".hnsw").c_str(), l2space, maxElements);
        }

        virtual std::vector<std::pair<uint64_t, float>> Lookup(const std::vector<float>& query, int topK) override {
            std::priority_queue<std::pair<float, hnswlib::labeltype>> result =
                appr_alg->searchKnn(query.data(), static_cast<size_t>(topK));
            std::vector<std::pair<uint64_t, float>> output;
            while (!result.empty()) {
                auto elem = result.top();
                result.pop();
                output.push_back({ ids[elem.second], 1.0f / (1.0f + elem.first) });
            }
            std::sort(output.begin(), output.end(),
                [](const std::pair<uint64_t, float>& a, const std::pair<uint64_t, float>& b) {
                    return a.second > b.second;
                });
            return output;
        }

        virtual void SaveMore(std::ofstream& ofs) const override {}
        virtual void LoadMore(std::ifstream& ifs) override {}
    };

} // namespace Quanta

#endif // HNSW_VDB_H
