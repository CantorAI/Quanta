#pragma once
#include "quanta_runtime.h"
#include <string>
#include <vector>
#include "QuantaDb.h"
#include "FilePathIndex.h"

namespace Quanta
{
    class DfsEngine
    {
    private:
        std::string m_lastRootFolder;
        bool m_isScanning;

        FilePathIndex m_filePathIndex;
        std::unique_ptr<QuantaDb> database_;
        QuantaDb& Database();
        // Internal helper methods
        std::string GenerateMetadata(const std::string& filePath, long long fileSize, const std::string& lastModified);
        bool Scan(std::string rootFolder, const std::vector<std::string>& excludeFolders = {}, bool skipHidden = true);
        void ScanFolder(const std::string& folderPath, const std::string& nodeId,
            const std::vector<std::string>& excludeFolders, bool skipHidden);
    public:
        BEGIN_PACKAGE(DfsEngine)
            APISET().AddSerializer("quanta.dfs", 1,
                [](DfsEngine* self) { return X::Value::String(self->Host(), self->m_lastRootFolder); },
                [](DfsEngine* self, const X::Value& state) {
                    if (!state.IsString()) throw X::Error("invalid DFS state");
                    self->m_lastRootFolder = state.ToString();
                });
            APISET().AddFunc<1>("Scan", &DfsEngine::ScanAPI);
            APISET().AddFunc<1>("Query", &DfsEngine::Query);
            APISET().AddFunc<1>("BuildIndex", &DfsEngine::BuildIndex);
            APISET().AddFunc<1>("LoadIndex", &DfsEngine::LoadIndex);
        END_PACKAGE

        DfsEngine() : m_isScanning(false) {}
        ~DfsEngine() {}

        void BuildIndex(std::string indexFile);
        void LoadIndex(std::string indexFile);
        // Main API methods
        bool ScanAPI(std::string rootFolder)
        {
			std::vector<std::string> excludeFolders; // Default to empty, can be customized
			return Scan(rootFolder, excludeFolders, true);
        }
        X::Value Query(std::string filePattern);

    };
}
