#ifndef SIMPLE_VDB_H
#define SIMPLE_VDB_H

#include "VectorDatabase.h"
#include <queue>
#include <omp.h>
#include <algorithm>

namespace Quanta {

    // Derived class implementing a naive lookup using exact cosine similarity.
    class SimpleVdb : public VectorDatabase {
    public:
        SimpleVdb(int dimension) : VectorDatabase(dimension) {}
        virtual ~SimpleVdb() {}

        // Lookup: compute cosine similarity between query and every record.
        virtual vector<pair<uint64_t, float>> Lookup(const vector<float>& query, int topK) override {
            int numRecords = ids.size();
            vector<pair<uint64_t, float>> results(numRecords);
#pragma omp parallel for
            for (int i = 0; i < numRecords; i++) {
                const float* vec = &data[i * D];
                float sim = cosine_similarity(query.data(), vec);
                results[i] = { ids[i], sim };
            }
            std::sort(results.begin(), results.end(),
                [](const pair<uint64_t, float>& a, const pair<uint64_t, float>& b) {
                    return a.second > b.second;
                });
            if (results.size() > static_cast<size_t>(topK))
                results.resize(topK);
            return results;
        }

        // No additional members to save/load.
        virtual void SaveMore(ofstream& ofs) const override {}
        virtual void LoadMore(ifstream& ifs) override {}
    };

} // namespace Quanta

#endif // SIMPLE_VDB_H
