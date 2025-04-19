#ifndef VECTOR_DATABASE_H
#define VECTOR_DATABASE_H

#include <vector>
#include <string>
#include <map>
#include <utility>
#include <cmath>
#include <mutex>
#include <fstream>
#include "xlang.h"

namespace Quanta {

    // --------------------------------
    // Base Class: VectorDatabase
    // --------------------------------
    class VectorDatabase {
    public:
        // Constructor and destructor
        explicit VectorDatabase(int dimension) : D(dimension) {}
        virtual ~VectorDatabase() {}

		inline int GetDimension() const { return D; }
        // Add a new record with label text. Returns the index in `ids`.
        unsigned long long AddLabel(unsigned long long id, const std::string& text) {
            std::lock_guard<std::mutex> lock(mtx);
            ids.push_back(id);
            chunkMap[id] = text;
            return static_cast<unsigned long long>(ids.size() - 1);
        }

        // Add multiple records with sequential IDs starting from startId.
        // Returns vector of indices in `ids`.
        std::vector<unsigned long long> AddLabels(unsigned long long startId,
            const std::vector<std::string>& texts) {
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<unsigned long long> indices;
            indices.reserve(texts.size());
            for (size_t i = 0; i < texts.size(); ++i) {
                unsigned long long id = startId + static_cast<unsigned long long>(i);
                ids.push_back(id);
                chunkMap[id] = texts[i];
                indices.push_back(static_cast<unsigned long long>(ids.size() - 1));
            }
            return indices;
        }
        std::vector<unsigned long long> AddLabels(const std::vector<unsigned long long>& ids0,
            const std::vector<std::string>& texts) {
            if (ids0.size() != texts.size())
            {
                return std::vector<unsigned long long>();
            }
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<unsigned long long> indices;
            indices.reserve(ids0.size());
            for (size_t i = 0; i < ids0.size(); ++i) {
                unsigned long long id = ids0[i];
                ids.push_back(id);
                chunkMap[id] = texts[i];
                indices.push_back(static_cast<unsigned long long>(ids.size() - 1));
            }
            return indices;
        }

        // Add or update a parameter.
        void AddParameter(const std::string& key, const X::Value& val) {
            std::lock_guard<std::mutex> lock(mtx);
            paramMap[key] = val;
        }

        // Retrieve a parameter value by key, with a default.
        X::Value GetParam(const char* key, X::Value defaultVal) const {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = paramMap.find(key);
            if (it != paramMap.end()) {
                return it->second;
            }
            return defaultVal;
        }

        // Helper: cosine similarity between two vectors.
        float cosine_similarity(const float* a, const float* b) const {
            float dot = 0.0f, normA = 0.0f, normB = 0.0f;
            for (int i = 0; i < D; ++i) {
                dot += a[i] * b[i];
                normA += a[i] * a[i];
                normB += b[i] * b[i];
            }
            if (normA == 0.0f || normB == 0.0f) return 0.0f;
            return dot / (std::sqrt(normA) * std::sqrt(normB));
        }

        bool Save(const std::string& filename) const {
            std::lock_guard<std::mutex> lock(mtx);
            std::ofstream ofs(filename, std::ios::binary);
            if (!ofs) {
                return false;
            }
            ofs.write(reinterpret_cast<const char*>(&D), sizeof(D));
            size_t numRecords = ids.size();
            ofs.write(reinterpret_cast<const char*>(&numRecords), sizeof(numRecords));
            ofs.write(reinterpret_cast<const char*>(ids.data()), sizeof(unsigned long long) * numRecords);

            size_t numParams = paramMap.size();
            ofs.write(reinterpret_cast<const char*>(&numParams), sizeof(numParams));
            for (const auto& kv : paramMap) {
                // Key
                const std::string& key = kv.first;
                size_t keyLen = key.size();
                ofs.write(reinterpret_cast<const char*>(&keyLen), sizeof(keyLen));
                ofs.write(key.data(), keyLen);

                // Type + value
                X::Value val = kv.second;
                uint8_t typeCode = 255;
                if (val.IsLong()) {
                    typeCode = 0;
                    long long v = static_cast<long long>(val);
                    ofs.write(reinterpret_cast<const char*>(&typeCode), sizeof(typeCode));
                    ofs.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }
                else if (val.IsDouble()) {
                    typeCode = 1;
                    double v = static_cast<double>(val);
                    ofs.write(reinterpret_cast<const char*>(&typeCode), sizeof(typeCode));
                    ofs.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }
                else if (val.IsString()) {
                    typeCode = 2;
                    std::string s = val.ToString();
                    ofs.write(reinterpret_cast<const char*>(&typeCode), sizeof(typeCode));
                    size_t len = s.size();
                    ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
                    ofs.write(s.data(), len);
                }
                else {
                    ofs.write(reinterpret_cast<const char*>(&typeCode), sizeof(typeCode));
                }
            }

            // Write chunks
            size_t numChunks = chunkMap.size();
            ofs.write(reinterpret_cast<const char*>(&numChunks), sizeof(numChunks));
            for (const auto& kv : chunkMap) {
                unsigned long long id = kv.first;
                const std::string& text = kv.second;
                ofs.write(reinterpret_cast<const char*>(&id), sizeof(id));
                size_t len = text.size();
                ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
                ofs.write(text.data(), len);
            }
			return true;
        }

        // Load database state (thread‑safe)
        bool Load(const std::string& filename) {
            std::lock_guard<std::mutex> lock(mtx);
            std::ifstream ifs(filename, std::ios::binary);
            if (!ifs) {
                return false;
            }
            int fileD;
            ifs.read(reinterpret_cast<char*>(&fileD), sizeof(fileD));
            D = fileD;
            size_t numRecords;
            ifs.read(reinterpret_cast<char*>(&numRecords), sizeof(numRecords));
            ids.resize(numRecords);
            ifs.read(reinterpret_cast<char*>(ids.data()), sizeof(unsigned long long) * numRecords);

			// Read parameters
            size_t numParams;
            ifs.read(reinterpret_cast<char*>(&numParams), sizeof(numParams));
            paramMap.clear();
            for (size_t i = 0; i < numParams; ++i) {
                size_t keyLen;
                ifs.read(reinterpret_cast<char*>(&keyLen), sizeof(keyLen));
                std::string key(keyLen, '\0');
                ifs.read(&key[0], keyLen);

                uint8_t typeCode;
                ifs.read(reinterpret_cast<char*>(&typeCode), sizeof(typeCode));
                X::Value val;
                switch (typeCode) {
                case 0: {
                    long long v;
                    ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
                    val = X::Value(static_cast<int64_t>(v));
                    break;
                }
                case 1: {
                    double v;
                    ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
                    val = X::Value(v);
                    break;
                }
                case 2: {
                    size_t len;
                    ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
                    std::string s(len, '\0');
                    ifs.read(&s[0], len);
                    val = X::Value(s);
                    break;
                }
                default:
                    break;
                }
                paramMap[key] = val;
            }
			// Read chunks
            size_t numChunks;
            ifs.read(reinterpret_cast<char*>(&numChunks), sizeof(numChunks));
            chunkMap.clear();
            for (size_t i = 0; i < numChunks; ++i) {
                unsigned long long id;
                ifs.read(reinterpret_cast<char*>(&id), sizeof(id));
                size_t len;
                ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
                std::string text(len, '\0');
                ifs.read(&text[0], len);
                chunkMap[id] = text;
            }
			return true;
        }
		inline unsigned long long GetIdByIndex(unsigned long long index) const {
			std::lock_guard<std::mutex> lock(mtx);
			if (index < ids.size()) {
				return ids[index];
			}
			return 0;
		}
		inline std::string GetTextById(unsigned long long id) const {
			std::lock_guard<std::mutex> lock(mtx);
			auto it = chunkMap.find(id);
			if (it != chunkMap.end()) {
				return it->second;
			}
			return std::string();
		}   
    private:
        int D;  // Dimension of each vector.
        std::vector<unsigned long long> ids;                  // Record IDs.
        std::map<std::string, X::Value> paramMap;            // Parameters.
        std::map<unsigned long long, std::string> chunkMap;  // ID-to-text mapping.
        mutable std::mutex mtx;  // Protects all member data for thread safety
    };

} // namespace Quanta

#endif // VECTOR_DATABASE_H
