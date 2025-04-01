#pragma once
#include "xpackage.h"
#include <string>
#include <vector>
#include "value.h"
#include "quantadb.h"
#include "FilePathIndex.h"

namespace Quanta
{
    class DfsEngine
    {
    private:
        std::string m_lastRootFolder;
        bool m_isScanning;

        FilePathIndex m_filePathIndex;
        // Internal helper methods
        std::string GenerateMetadata(const std::string& filePath, long long fileSize, const std::string& lastModified);
        bool Scan(std::string rootFolder, const std::vector<std::string>& excludeFolders = {}, bool skipHidden = true);
        void ScanFolder(const std::string& folderPath, const std::string& nodeId,
            const std::vector<std::string>& excludeFolders, bool skipHidden);
    public:
        BEGIN_PACKAGE(DfsEngine)
            APISET().SetPackageContentProc([](void* pContextObj)
                {
                    return ((DfsEngine*)pContextObj)->GetContentSize();
                },
                [](void* pContextObj, X::XLStream* pStream)
                {
                    return ((DfsEngine*)pContextObj)->ToBytes(pStream);
                },
                [](void* pContextObj, X::XLStream* pStream)
                {
                    return ((DfsEngine*)pContextObj)->FromBytes(pStream);
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

        // Content serialization methods
        inline long long GetContentSize()
        {
            return m_lastRootFolder.size();
        }

        inline bool ToBytes(X::XLStream* pStream)
        {
            X::Value lastFolder(m_lastRootFolder);
            bool bOK = X::g_pXHost->ConvertToBytes(lastFolder, pStream);
            return bOK;
        }

        inline bool FromBytes(X::XLStream* pStream)
        {
            X::Value lastFolder;
            bool bOK = X::g_pXHost->ConvertFromBytes(lastFolder, pStream);
            m_lastRootFolder = lastFolder.ToString();
            return bOK;
        }
    };
}