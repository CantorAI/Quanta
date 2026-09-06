#ifndef VECTOR_DATABASE_H
#define VECTOR_DATABASE_H

#include <vector>
#include <string>
#include <map>
#include <utility>
#include <cmath>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <variant>
#include <cstdint>
#include <type_traits>

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
        // Add a new record with label text and optional timestamp. Returns the index in `ids`.
        unsigned long long AddLabel(unsigned long long id, const std::string& text,
            unsigned long long timestampMs = 0) {
            std::lock_guard<std::mutex> lock(mtx);
            ids.push_back(id);
            chunkMap[id] = text;
            if (timestampMs > 0) {
                timestampMap_[id] = timestampMs;
            }
            return static_cast<unsigned long long>(ids.size() - 1);
        }

        // Add multiple records with sequential IDs starting from startId.
        // Returns vector of indices in `ids`.
        std::vector<unsigned long long> AddLabels(unsigned long long startId,
            const std::vector<std::string>& texts,
            unsigned long long timestampMs = 0) {
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<unsigned long long> indices;
            indices.reserve(texts.size());
            for (size_t i = 0; i < texts.size(); ++i) {
                unsigned long long id = startId + static_cast<unsigned long long>(i);
                ids.push_back(id);
                chunkMap[id] = texts[i];
                if (timestampMs > 0) {
                    timestampMap_[id] = timestampMs;
                }
                indices.push_back(static_cast<unsigned long long>(ids.size() - 1));
            }
            return indices;
        }
        std::vector<unsigned long long> AddLabels(const std::vector<unsigned long long>& ids0,
            const std::vector<std::string>& texts,
            unsigned long long timestampMs = 0) {
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
                if (timestampMs > 0) {
                    timestampMap_[id] = timestampMs;
                }
                indices.push_back(static_cast<unsigned long long>(ids.size() - 1));
            }
            return indices;
        }

        // Set timestamps for multiple IDs at once
        void SetTimestamps(const std::vector<unsigned long long>& ids0,
            unsigned long long timestampMs) {
            if (timestampMs == 0) return;
            std::lock_guard<std::mutex> lock(mtx);
            for (auto id : ids0) {
                timestampMap_[id] = timestampMs;
            }
        }

        // Get timestamp for a specific ID (returns 0 if not found)
        unsigned long long GetTimestampById(unsigned long long id) const {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = timestampMap_.find(id);
            return (it != timestampMap_.end()) ? it->second : 0;
        }

        // Add or update a parameter.
        using Parameter = std::variant<int64_t, double, std::string>;
        template<class T> void AddParameter(const std::string& key, const T& val) {
            std::lock_guard<std::mutex> lock(mtx);
            if constexpr (std::is_integral_v<T>) paramMap[key] = static_cast<int64_t>(val);
            else paramMap[key] = val;
        }

        // Retrieve a parameter value by key, with a default.
        template<class T> T GetParam(const char* key, T defaultVal) const {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = paramMap.find(key);
            if (it != paramMap.end()) {
                if constexpr (std::is_integral_v<T>) {
                    if (auto value = std::get_if<int64_t>(&it->second)) return static_cast<T>(*value);
                } else if (auto value = std::get_if<T>(&it->second)) return *value;
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
            std::string newPath = filename + ".new";
            std::string oldPath = filename + ".old";
            
            std::error_code ec;
            if (std::filesystem::exists(newPath, ec)) {
                std::filesystem::remove(newPath, ec);
            }

            std::ofstream ofs(newPath, std::ios::binary);
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
                const Parameter& val = kv.second;
                uint8_t typeCode = 255;
                if (std::holds_alternative<int64_t>(val)) {
                    typeCode = 0;
                    int64_t v = std::get<int64_t>(val);
                    ofs.write(reinterpret_cast<const char*>(&typeCode), sizeof(typeCode));
                    ofs.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }
                else if (std::holds_alternative<double>(val)) {
                    typeCode = 1;
                    double v = std::get<double>(val);
                    ofs.write(reinterpret_cast<const char*>(&typeCode), sizeof(typeCode));
                    ofs.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }
                else if (std::holds_alternative<std::string>(val)) {
                    typeCode = 2;
                    const std::string& s = std::get<std::string>(val);
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

            // Write timestamps (new section, backward-compatible)
            size_t numTimestamps = timestampMap_.size();
            ofs.write(reinterpret_cast<const char*>(&numTimestamps), sizeof(numTimestamps));
            for (const auto& kv : timestampMap_) {
                unsigned long long id = kv.first;
                unsigned long long ts = kv.second;
                ofs.write(reinterpret_cast<const char*>(&id), sizeof(id));
                ofs.write(reinterpret_cast<const char*>(&ts), sizeof(ts));
            }
            
            ofs.close();
            
            if (std::filesystem::exists(filename, ec)) {
                if (std::filesystem::exists(oldPath, ec)) {
                    std::filesystem::remove(oldPath, ec);
                }
                std::filesystem::rename(filename, oldPath, ec);
            }
            std::filesystem::rename(newPath, filename, ec);
            
            if (std::filesystem::exists(oldPath, ec)) {
                std::filesystem::remove(oldPath, ec);
            }
            
            return true;
        }

        // Load database state (thread‑safe)
        bool Load(const std::string& filename) {
            std::lock_guard<std::mutex> lock(mtx);
            
            std::string newPath = filename + ".new";
            std::string oldPath = filename + ".old";
            std::error_code ec;
            if (!std::filesystem::exists(filename, ec)) {
                if (std::filesystem::exists(newPath, ec)) {
                    std::filesystem::rename(newPath, filename, ec);
                } else if (std::filesystem::exists(oldPath, ec)) {
                    std::filesystem::rename(oldPath, filename, ec);
                }
            }

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
                Parameter val;
                switch (typeCode) {
                case 0: {
                    long long v;
                    ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
                    val = static_cast<int64_t>(v);
                    break;
                }
                case 1: {
                    double v;
                    ifs.read(reinterpret_cast<char*>(&v), sizeof(v));
                    val = v;
                    break;
                }
                case 2: {
                    size_t len;
                    ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
                    std::string s(len, '\0');
                    ifs.read(&s[0], len);
                    val = std::move(s);
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

            // Read timestamps (backward-compatible: only read if data remains)
            timestampMap_.clear();
            if (ifs.peek() != EOF) {
                size_t numTimestamps;
                ifs.read(reinterpret_cast<char*>(&numTimestamps), sizeof(numTimestamps));
                if (ifs.good()) {
                    for (size_t i = 0; i < numTimestamps; ++i) {
                        unsigned long long id;
                        unsigned long long ts;
                        ifs.read(reinterpret_cast<char*>(&id), sizeof(id));
                        ifs.read(reinterpret_cast<char*>(&ts), sizeof(ts));
                        if (ifs.good()) {
                            timestampMap_[id] = ts;
                        }
                    }
                }
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
        inline long long GetIndexById(unsigned long long id) const {
			std::lock_guard<std::mutex> lock(mtx);
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == id) {
                    return i;
                }
            }
			return -1;
		}
		inline std::string GetTextById(unsigned long long id) const {
			std::lock_guard<std::mutex> lock(mtx);
			auto it = chunkMap.find(id);
			if (it != chunkMap.end()) {
				return it->second;
			}
			return std::string();
		}   
        inline size_t GetSize() const {
            std::lock_guard<std::mutex> lock(mtx);
            return ids.size();
        }
    private:
        int D;  // Dimension of each vector.
        std::vector<unsigned long long> ids;                  // Record IDs.
        std::map<std::string, Parameter> paramMap;
        std::map<unsigned long long, std::string> chunkMap;  // ID-to-text mapping.
        std::map<unsigned long long, unsigned long long> timestampMap_;  // ID-to-timestamp (ms).
        mutable std::mutex mtx;  // Protects all member data for thread safety
    };

} // namespace Quanta

#endif // VECTOR_DATABASE_H
