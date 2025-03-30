#pragma once
#include "xpackage.h"
#include <string>
#include <vector>
#include "value.h"
#include "quantadb.h"

namespace Quanta
{
    class DfsEngine
    {
    private:
        std::string m_lastRootFolder;
        bool m_isScanning;

        // Internal helper methods
        std::string GenerateMetadata(const std::string& filePath, long long fileSize, const std::string& lastModified);

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
        APISET().AddFunc<1>("Scan", &DfsEngine::Scan);
        APISET().AddFunc<1>("Query", &DfsEngine::Query);
        END_PACKAGE

        DfsEngine() : m_isScanning(false) {}
        ~DfsEngine() {}

        // Main API methods
        bool Scan(std::string rootFolder);
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