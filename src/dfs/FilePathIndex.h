#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <omp.h>
#include <random>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <set>
//#include <cstddef>

// Define these before including any Windows headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifdef byte
#undef byte
#endif


using namespace std;
using namespace std::chrono;

namespace Quanta {
    // Helper functions to read a 4-byte unsigned integer from a char array.
    inline bool is_aligned(const void* ptr, size_t alignment) {
        return (reinterpret_cast<uintptr_t>(ptr) % alignment) == 0;
    }

    inline uint32_t read_uint32(const char* ptr) {
        if (is_aligned(ptr, 4)) {
            return *(reinterpret_cast<const uint32_t*>(ptr));
        }
        else {
            return static_cast<uint32_t>(static_cast<unsigned char>(ptr[0])) |
                (static_cast<uint32_t>(static_cast<unsigned char>(ptr[1])) << 8) |
                (static_cast<uint32_t>(static_cast<unsigned char>(ptr[2])) << 16) |
                (static_cast<uint32_t>(static_cast<unsigned char>(ptr[3])) << 24);
        }
    }

    class FilePathIndex {
    private:
        // Data layout for each record:
        //   [4 bytes ID][4 bytes file-path length][file-path characters]
        vector<char> data;
        // Offsets for each record in 'data'.
        vector<int> offsets;
        // Next record ID.
        uint32_t nextID;

        // Inlined Boyer¨CMoore¨CHorspool substring search.
        inline bool BMH_Search(const char* text, int text_len,
            const char* pattern, int pat_len,
            const int skip[256]) const {
            if (pat_len > text_len) return false;
            int i = 0;
            while (i <= text_len - pat_len) {
                int j = pat_len - 1;
                while (j >= 0 && pattern[j] == text[i + j])
                    j--;
                if (j < 0)
                    return true; // Found a match.
                i += skip[(unsigned char)text[i + pat_len - 1]];
            }
            return false;
        }

        // Fill in the skip table for the given pattern into the provided array.
        void createSkipTable(const string& pattern, int skip[256]) const {
            int pat_len = pattern.size();
            for (int i = 0; i < 256; i++)
                skip[i] = pat_len;
            for (int i = 0; i < pat_len - 1; i++) {
                skip[(unsigned char)pattern[i]] = pat_len - i - 1;
            }
        }

        // Internal function: partial match search.
        // Returns a vector of record indices (into offsets) for active records where
        // BMH_Search returns true (regardless of whether it's an exact full match).
        vector<int> searchMatchesPartial(const char* pattern, int pat_len, const int skip[256]) const {
            int numRecords = offsets.size();
            int numThreads = omp_get_max_threads();
            vector<vector<int>> threadResults(numThreads);

#pragma omp parallel
            {
                int tid = omp_get_thread_num();
                vector<int>& localMatches = threadResults[tid];
#pragma omp for schedule(static)
                for (int i = 0; i < numRecords; i++) {
                    const char* record = data.data() + offsets[i];
                    uint32_t id = read_uint32(record);
                    if (id == 0) continue; // Skip removed record.
                    int filePathLen = static_cast<int>(read_uint32(record + sizeof(uint32_t)));
                    const char* filepath = record + sizeof(uint32_t) + sizeof(uint32_t);
                    if (BMH_Search(filepath, filePathLen, pattern, pat_len, skip)) {
                        localMatches.push_back(i);
                    }
                }
            }
            vector<int> result;
            for (int t = 0; t < numThreads; t++) {
                result.insert(result.end(), threadResults[t].begin(), threadResults[t].end());
            }
            return result;
        }

        // Internal function: full match search.
        // Returns a vector of record indices for active records where
        // BMH_Search returns true and the file-path length equals the pattern length.
        vector<int> searchMatchesFull(const char* pattern, int pat_len, const int skip[256]) const {
            int numRecords = offsets.size();
            int numThreads = omp_get_max_threads();
            vector<vector<int>> threadResults(numThreads);

#pragma omp parallel
            {
                int tid = omp_get_thread_num();
                vector<int>& localMatches = threadResults[tid];
#pragma omp for schedule(static)
                for (int i = 0; i < numRecords; i++) {
                    const char* record = data.data() + offsets[i];
                    uint32_t id = read_uint32(record);
                    if (id == 0) continue; // Skip removed record.
                    int filePathLen = static_cast<int>(read_uint32(record + sizeof(uint32_t)));
                    const char* filepath = record + sizeof(uint32_t) + sizeof(uint32_t);
                    if (BMH_Search(filepath, filePathLen, pattern, pat_len, skip) && (filePathLen == pat_len)) {
                        localMatches.push_back(i);
                    }
                }
            }
            vector<int> result;
            for (int t = 0; t < numThreads; t++) {
                result.insert(result.end(), threadResults[t].begin(), threadResults[t].end());
            }
            return result;
        }

    public:
        FilePathIndex() : nextID(1) {}

        // SingleMatch: searches for the given substring (partial match) in all active records.
        // Returns a vector of record IDs (uint32_t).
        vector<uint32_t> SingleMatch(const string& pattern) const {
            int pat_len = pattern.size();
            int skip[256];
            createSkipTable(pattern, skip);
            vector<int> indices = searchMatchesPartial(pattern.c_str(), pat_len, skip);
            vector<uint32_t> result;
            result.reserve(indices.size());
            for (int idx : indices) {
                uint32_t id = read_uint32(data.data() + offsets[idx]);
                result.push_back(id);
            }
            return result;
        }

        // MatchAll: for multiple patterns separated by '|',
        // returns the common (intersection) set of record IDs (active records) that match all patterns (partial match).
        // This implementation uses OpenMP to parallelize the intersection over the candidate set.
        vector<uint32_t> MatchAll(const string& patterns) const {
            // Split patterns by '|'.
            vector<string> parts;
            {
                istringstream iss(patterns);
                string token;
                while (getline(iss, token, '|')) {
                    if (!token.empty())
                        parts.push_back(token);
                }
            }
            if (parts.empty()) return {};

            // For each pattern, compute a sorted vector of matching record indices.
            vector<vector<int>> allMatches;
            allMatches.reserve(parts.size());
            for (const auto& pat : parts) {
                int skip[256];
                createSkipTable(pat, skip);
                vector<int> matches = searchMatchesPartial(pat.c_str(), pat.size(), skip);
                sort(matches.begin(), matches.end());
                allMatches.push_back(move(matches));
            }
            // Use the candidate from the first pattern.
            vector<int>& candidate = allMatches[0];
            int candidateSize = candidate.size();
            vector<vector<int>> threadCandidates(omp_get_max_threads());

#pragma omp parallel for schedule(static)
            for (int i = 0; i < candidateSize; i++) {
                int idx = candidate[i];
                bool foundInAll = true;
                for (size_t j = 1; j < allMatches.size(); j++) {
                    if (!binary_search(allMatches[j].begin(), allMatches[j].end(), idx)) {
                        foundInAll = false;
                        break;
                    }
                }
                if (foundInAll) {
                    int tid = omp_get_thread_num();
                    threadCandidates[tid].push_back(idx);
                }
            }
            vector<int> intersection;
            for (auto& vec : threadCandidates)
                intersection.insert(intersection.end(), vec.begin(), vec.end());

            vector<uint32_t> result;
            result.reserve(intersection.size());
            for (int idx : intersection) {
                uint32_t id = read_uint32(data.data() + offsets[idx]);
                result.push_back(id);
            }
            return result;
        }

        // AddFile: Adds a new file-path record.
        // If MatchFirst is true, performs a full-match search (using searchMatchesFull)
        // and if any active record fully matches the file path, the file is not added.
        bool AddFile(const string& filePath, bool MatchFirst) {
            if (MatchFirst) {
                int skip[256];
                createSkipTable(filePath, skip);
                vector<int> matches = searchMatchesFull(filePath.c_str(), filePath.size(), skip);
                if (!matches.empty())
                    return false; // File already exists.
            }
            int offset = data.size();
            offsets.push_back(offset);
            uint32_t id = nextID++;
            // Write 4-byte ID.
            for (int b = 0; b < 4; b++) {
                data.push_back(static_cast<char>((id >> (8 * b)) & 0xFF));
            }
            // Write 4-byte file-path length.
            int len = filePath.size();
            for (int b = 0; b < 4; b++) {
                data.push_back(static_cast<char>((len >> (8 * b)) & 0xFF));
            }
            // Append file-path characters.
            data.insert(data.end(), filePath.begin(), filePath.end());
            return true;
        }

        // RemoveFile: Searches for an exact file-path full match among active records.
        // If found, marks the record as removed (by setting its 4-byte ID to 0) and returns true.
        bool RemoveFile(const string& filePath) {
            int skip[256];
            createSkipTable(filePath, skip);
            vector<int> matches = searchMatchesFull(filePath.c_str(), filePath.size(), skip);
            if (!matches.empty()) {
                char* record = data.data() + offsets[matches[0]];
                for (int b = 0; b < 4; b++) {
                    record[b] = 0;
                }
                return true;
            }
            return false;
        }

        // Save: Writes the entire index (data block, offsets, nextID) to a binary file.
        bool Save(const string& indexFileName) const {
            ofstream ofs(indexFileName, ios::binary);
            if (!ofs) return false;
            int numRecords = offsets.size();
            ofs.write(reinterpret_cast<const char*>(&numRecords), sizeof(int));
            ofs.write(reinterpret_cast<const char*>(&nextID), sizeof(uint32_t));
            int offsetsSize = offsets.size();
            ofs.write(reinterpret_cast<const char*>(&offsetsSize), sizeof(int));
            ofs.write(reinterpret_cast<const char*>(offsets.data()), offsetsSize * sizeof(int));
            int dataSize = data.size();
            ofs.write(reinterpret_cast<const char*>(&dataSize), sizeof(int));
            ofs.write(reinterpret_cast<const char*>(data.data()), dataSize * sizeof(char));
            ofs.close();
            return true;
        }

        // Load: Reads the index (data block, offsets, nextID) from a binary file.
        bool Load(const string& indexFileName) {
            ifstream ifs(indexFileName, ios::binary);
            if (!ifs) return false;
            int numRecords;
            ifs.read(reinterpret_cast<char*>(&numRecords), sizeof(int));
            ifs.read(reinterpret_cast<char*>(&nextID), sizeof(uint32_t));
            int offsetsSize;
            ifs.read(reinterpret_cast<char*>(&offsetsSize), sizeof(int));
            offsets.resize(offsetsSize);
            ifs.read(reinterpret_cast<char*>(offsets.data()), offsetsSize * sizeof(int));
            int dataSize;
            ifs.read(reinterpret_cast<char*>(&dataSize), sizeof(int));
            data.resize(dataSize);
            ifs.read(reinterpret_cast<char*>(data.data()), dataSize * sizeof(char));
            ifs.close();
            return true;
        }
    };

    //////////////////////
    // Example usage. //
    //////////////////////

    inline int test() {
        FilePathIndex index;

        // Sample file paths.
        vector<string> sampleFiles = {
            "C:/Documents/Report1_final.docx",
            "C:/Pictures/Vacation2021.jpg",
            "D:/Music/rock/classic.mp3",
            "E:/Work/Project_pattern_plan.pdf",
            "F:/Downloads/pattern_sample.txt"
        };

        // Add files.
        for (const auto& file : sampleFiles) {
            if (index.AddFile(file, true))
                cout << "Added: " << file << endl;
            else
                cout << "Already exists: " << file << endl;
        }

        // Perform a single-match search.
        string searchPattern = "pattern";
        vector<uint32_t> matches = index.SingleMatch(searchPattern);
        cout << "SingleMatch for \"" << searchPattern << "\" found " << matches.size() << " records:" << endl;
        for (uint32_t id : matches) {
            cout << "  Record ID: " << id << endl;
        }

        // Perform a multi-match search.
        string multiPattern = "pattern|plan";
        vector<uint32_t> commonMatches = index.MatchAll(multiPattern);
        cout << "MatchAll for \"" << multiPattern << "\" found " << commonMatches.size() << " records:" << endl;
        for (uint32_t id : commonMatches) {
            cout << "  Record ID: " << id << endl;
        }

        // Remove a file.
        string removeFile = "E:/Work/Project_pattern_plan.pdf";
        if (index.RemoveFile(removeFile))
            cout << "Removed file: " << removeFile << endl;
        else
            cout << "File not found for removal: " << removeFile << endl;

        // Save the index.
        string filename = "FilePathIndex.bin";
        if (index.Save(filename))
            cout << "Index saved to " << filename << endl;
        else
            cout << "Error saving index." << endl;

        // Load the index into a new instance.
        FilePathIndex loadedIndex;
        if (loadedIndex.Load(filename))
            cout << "Index loaded from " << filename << endl;
        else
            cout << "Error loading index." << endl;

        return 0;
    }
}