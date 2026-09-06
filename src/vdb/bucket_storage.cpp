#include "bucket_storage.h"
#include <fstream>
#include "HnswVdb.h"
#include "VectorDatabase.h"

namespace Quanta
{
    fs::path BucketStorage::GetHnswPath(const fs::path& basePath, const std::string& prefix, const std::string& tsPartition, int customIndex, const std::string& bucketStr)
    {
        return basePath / (prefix + "_" + tsPartition + "_" + std::to_string(customIndex) + "_" + bucketStr + ".hnsw");
    }

    fs::path BucketStorage::GetVdbPath(const fs::path& basePath, const std::string& prefix, const std::string& tsPartition, int customIndex, const std::string& bucketStr)
    {
        return basePath / (prefix + "_" + tsPartition + "_" + std::to_string(customIndex) + "_" + bucketStr + ".vdb");
    }

    bool BucketStorage::AppendWalRecord(const fs::path& basePath, const std::string& active_wal_filename, const std::vector<unsigned long long>& extIds, const std::vector<std::string>& chunkTexts, long long timestampMs, const float* vectors, size_t count, int dimension, size_t first)
    {
        fs::path walPath = basePath / active_wal_filename;
        std::ofstream walFile(walPath, std::ios::binary | std::ios::app);
        if (!walFile.is_open()) return false;

        for (size_t i = 0; i < count; ++i) {
            WalRecordHeader header;
            header.external_id = extIds[first + i];
            header.timestamp_ms = static_cast<unsigned long long>(timestampMs);
            header.chunk_text_length = static_cast<unsigned int>(chunkTexts[first + i].size());

            walFile.write(reinterpret_cast<const char*>(&header), sizeof(WalRecordHeader));
            walFile.write(reinterpret_cast<const char*>(vectors + (i * dimension)), dimension * sizeof(float));
            
            if (header.chunk_text_length > 0) {
                walFile.write(chunkTexts[first + i].data(), header.chunk_text_length);
            }
        }
        
        // Flush the C++ stream. This is not an OS fsync/power-loss guarantee.
        walFile.flush(); 
        if (!walFile.good()) return false;

        return true;
    }

    bool BucketStorage::ReadWalFile(const fs::path& walPath, std::vector<char>& outBuffer)
    {
        std::ifstream walFile(walPath, std::ios::binary | std::ios::ate);
        if (!walFile.is_open()) return false;

        std::streamsize fileSize = walFile.tellg();
        walFile.seekg(0, std::ios::beg);

        if (fileSize > 0) {
            outBuffer.resize(static_cast<size_t>(fileSize));
            if (!walFile.read(outBuffer.data(), fileSize)) {
                return false;
            }
        }
        return true;
    }
}
