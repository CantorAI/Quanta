#ifndef HNSW_VDB_H
#define HNSW_VDB_H
#include "VectorDatabase.h"
#include "hnswlib/hnswlib.h" // Adjust the include path as needed
#include <string>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace Quanta {

    // Derived class implementing ANN lookup using hnswlib.
    // This example assumes that hnswlib is integrated into your project.
    class HnswVdb : public VectorDatabase {
    public:
        hnswlib::HierarchicalNSW<float>* appr_alg;
        // HNSW parameters:
        int M;              // Maximum number of neighbors.
        int efConstruction; // Controls index construction quality.
        int efSearch;       // Controls search quality.
        size_t maxElements; // Capacity for the index.
        // The base filename for saving/loading the hnsw index.
        std::string indexFileName;

        // Constructor requires capacity and other parameters.
        HnswVdb(int dimension, size_t maxElements_, int maxM = 16, int efC = 200, int efS = 50)
            : VectorDatabase(dimension), M(maxM), efConstruction(efC), efSearch(efS), maxElements(maxElements_)
        {
            // Create a space (using L2 space here)
            hnswlib::L2Space* l2space = new hnswlib::L2Space(dimension);
            appr_alg = new hnswlib::HierarchicalNSW<float>(l2space, maxElements, M, efConstruction);
            appr_alg->setEf(efSearch);
        }

        virtual ~HnswVdb() {
            if (appr_alg) {
                delete appr_alg;
                appr_alg = nullptr;
            }
        }

        // Override AddVector: add to base storage then add to hnswlib index.
        virtual void AddVector(uint64_t id, const std::vector<float>& vec) override {
            VectorDatabase::AddVector(id, vec);
            appr_alg->addPoint(vec.data(), static_cast<size_t>(ids.size() - 1));
        }

        // We do not override Save() and Load() from the base.
        // Instead, we override SaveMore and LoadMore.

        static std::string NormalizeIndexFilename(const std::string& fname) {
            size_t pos = fname.find_last_of('.');
            if (pos != std::string::npos) {
                return fname.substr(0, pos);
            }
            return fname;
        }

        virtual void SaveMore(std::ofstream& ofs) const override {
            // Write extra HNSW parameters.
            ofs.write(reinterpret_cast<const char*>(&maxElements), sizeof(maxElements));
            ofs.write(reinterpret_cast<const char*>(&M), sizeof(M));
            ofs.write(reinterpret_cast<const char*>(&efConstruction), sizeof(efConstruction));
            ofs.write(reinterpret_cast<const char*>(&efSearch), sizeof(efSearch));

            // Save the hnsw index to a separate file.
            if (!indexFileName.empty()) {
                std::string baseName = NormalizeIndexFilename(indexFileName);
                std::string idxName = baseName + ".hnsw";
                appr_alg->saveIndex(idxName.c_str());
            }
        }

        virtual void LoadMore(std::ifstream& ifs) override {
            ifs.read(reinterpret_cast<char*>(&maxElements), sizeof(maxElements));
            ifs.read(reinterpret_cast<char*>(&M), sizeof(M));
            ifs.read(reinterpret_cast<char*>(&efConstruction), sizeof(efConstruction));
            ifs.read(reinterpret_cast<char*>(&efSearch), sizeof(efSearch));

            // Re-create the hnswlib index using the loaded parameters.
            hnswlib::L2Space* l2space = new hnswlib::L2Space(D);
            if (appr_alg) {
                delete appr_alg;
            }
            appr_alg = new hnswlib::HierarchicalNSW<float>(l2space, maxElements, M, efConstruction);
            appr_alg->setEf(efSearch);
            if (!indexFileName.empty()) {
                std::string baseName = NormalizeIndexFilename(indexFileName);
                std::string idxName = baseName + ".hnsw";
                appr_alg->loadIndex(idxName.c_str(), l2space, maxElements);
            }
        }

        // Lookup using hnswlib.
        virtual std::vector<std::pair<uint64_t, float>> Lookup(const std::vector<float>& query, int topK) override {
            std::priority_queue<std::pair<float, hnswlib::labeltype>> result =
                appr_alg->searchKnn(query.data(), static_cast<size_t>(topK));
            std::vector<std::pair<uint64_t, float>> output;
            while (!result.empty()) {
                auto elem = result.top();
                result.pop();
                // Convert distance to similarity (e.g., similarity = 1/(1+distance)).
                output.push_back({ ids[elem.second], 1.0f / (1.0f + elem.first) });
            }
            std::sort(output.begin(), output.end(),
                [](const std::pair<uint64_t, float>& a, const std::pair<uint64_t, float>& b) {
                    return a.second > b.second;
                });
            return output;
        }
    };

} // namespace Quanta

#endif // HNSW_VDB_H
