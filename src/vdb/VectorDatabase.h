#ifndef VECTOR_DATABASE_H
#define VECTOR_DATABASE_H

#include <vector>
#include <cstdint>
#include <string>
#include <utility>
#include <fstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <omp.h>
#include <string>

namespace Quanta {

    using std::vector;
    using std::uint64_t;
    using std::string;
    using std::pair;
    using std::cerr;
    using std::ofstream;
    using std::ifstream;

    // --------------------------
    // Base Class: VectorDatabase
    // --------------------------
    class VectorDatabase {
    public:
        int D;                     // Dimension of each vector.
        vector<uint64_t> ids;      // Record IDs.
        // All vectors concatenated: record i occupies data[i * D ... (i+1)*D-1].
        vector<float> data;

        VectorDatabase(int dimension) : D(dimension) {}
        virtual ~VectorDatabase() {}

        // Add a new record.
        virtual void AddVector(uint64_t id, const vector<float>& vec) {
            if (vec.size() != static_cast<size_t>(D)) {
                cerr << "Dimension mismatch. Expected dimension: " << D << "\n";
                return;
            }
            ids.push_back(id);
            data.insert(data.end(), vec.begin(), vec.end());
        }

        // Remove a record by index (0-based).
        virtual void RemoveVector(int index) {
            int numRecords = ids.size();
            if (index < 0 || index >= numRecords) {
                cerr << "Invalid index for removal.\n";
                return;
            }
            // Swap with the last record and remove.
            ids[index] = ids.back();
            ids.pop_back();
            int lastPos = (numRecords - 1) * D;
            int removePos = index * D;
            for (int i = 0; i < D; i++) {
                data[removePos + i] = data[lastPos + i];
            }
            data.erase(data.end() - D, data.end());
        }

        // Base Save: store D, number of records, ids and data.
        virtual void Save(const string& filename) const {
            ofstream ofs(filename, std::ios::binary);
            if (!ofs) {
                cerr << "Failed to open file for saving.\n";
                return;
            }
            ofs.write(reinterpret_cast<const char*>(&D), sizeof(D));
            size_t numRecords = ids.size();
            ofs.write(reinterpret_cast<const char*>(&numRecords), sizeof(numRecords));
            ofs.write(reinterpret_cast<const char*>(ids.data()), sizeof(uint64_t) * numRecords);
            size_t dataSize = data.size();
            ofs.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
            ofs.write(reinterpret_cast<const char*>(data.data()), sizeof(float) * dataSize);

            // Call virtual function to allow derived classes to save their own members.
            SaveMore(ofs, filename);
            ofs.close();
        }

        // Base Load: load D, number of records, ids and data.
        virtual void Load(const string& filename) {
            ifstream ifs(filename, std::ios::binary);
            if (!ifs) {
                cerr << "Failed to open file for loading.\n";
                return;
            }
            int fileD;
            ifs.read(reinterpret_cast<char*>(&fileD), sizeof(fileD));
            if (fileD != D) {
                cerr << "Dimension mismatch in loaded file.\n";
                return;
            }
            size_t numRecords;
            ifs.read(reinterpret_cast<char*>(&numRecords), sizeof(numRecords));
            ids.resize(numRecords);
            ifs.read(reinterpret_cast<char*>(ids.data()), sizeof(uint64_t) * numRecords);
            size_t dataSize;
            ifs.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));
            data.resize(dataSize);
            ifs.read(reinterpret_cast<char*>(data.data()), sizeof(float) * dataSize);

            // Call virtual function for derived class members.
            LoadMore(ifs,filename);
            ifs.close();
        }

        // Virtual function for derived classes to save additional members.
        virtual void SaveMore(ofstream& ofs,const std::string& filename) const {}
        // Virtual function for derived classes to load additional members.
        virtual void LoadMore(ifstream& ifs,const std::string& filename) {}

        // Pure virtual Lookup function.
        virtual vector<pair<uint64_t, float>> Lookup(const vector<float>& query, int topK) = 0;

        // Helper: cosine similarity between two vectors.
        float cosine_similarity(const float* a, const float* b) const {
            float dot = 0.0f, normA = 0.0f, normB = 0.0f;
            for (int i = 0; i < D; i++) {
                dot += a[i] * b[i];
                normA += a[i] * a[i];
                normB += b[i] * b[i];
            }
            if (normA == 0 || normB == 0) return 0;
            return dot / (std::sqrt(normA) * std::sqrt(normB));
        }
    };

} // namespace Quanta

#endif // VECTOR_DATABASE_H
