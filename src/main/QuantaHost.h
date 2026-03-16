#pragma once
#include "singleton.h"
#include "xpackage.h"
#include "xlang.h"
#include <iostream>
#include <unordered_map>
#include <vector>
#include "Locker.h"
#include "help_func.h"
#include "DfsEng.h"
#include "log.h"
#include "vdb.h"
#include "partitioned_vdb.h"

namespace Quanta
{
    class QuantaHost :
        public Singleton<QuantaHost>
    {
        X::XRuntime* m_defaultRuntime = nullptr;
        std::string m_strQuantaPath;
        std::string m_strLibName;
        X::Value m_cantor;
        X::Value m_log;
        UID m_nodeId;

        std::mutex m_vdbMutex;
        std::map<std::string, X::Value> m_vdbInstances;

        std::string GetNodeIdString()
        {
            if (!m_cantor.IsObject())
            {
                return "";
            }
            auto host = m_cantor["Host"]();
            return host["NodeId"]().ToString();
        }
    public:
        BEGIN_PACKAGE(QuantaHost)
            APISET().SetPackageContentProc([](void* pContextObj)
                {
                    return ((QuantaHost*)pContextObj)->GetContentSize();
                },
                [](void* pContextObj, X::XLStream* pStream)
                {
                    return ((QuantaHost*)pContextObj)->ToBytes(pStream);
                },
                [](void* pContextObj, X::XLStream* pStream)
                {
                    return ((QuantaHost*)pContextObj)->FromBytes(pStream);
                });
            APISET().AddClass<0, DfsEngine>("dfs");
            APISET().AddVarClass<Vdb>("vdb");
            APISET().AddVarClass<PartitionedVdb>("partitioned_vdb");
            APISET().AddVarFunc("GetPartitionedVdb", &QuantaHost::GetPartitionedVdb);
            APISET().AddFunc<1>("SetCantor", &QuantaHost::SetCantor);
            APISET().AddPropL("cantor",
                [](auto* pThis, X::Value v)
                {
                    pThis->SetCantor(v);
                },
                [](auto* pThis) {return pThis->m_cantor; });
            APISET().AddFunc<0>("Test", &QuantaHost::Test);
        END_PACKAGE


        bool HasPartitionedVdb(const std::string& path, const std::string& prefix) {
            std::lock_guard<std::mutex> lock(m_vdbMutex);
            std::string cacheKey = path + "|" + prefix;
            return m_vdbInstances.find(cacheKey) != m_vdbInstances.end();
        }

        void RegisterPartitionedVdb(const std::string& path, const std::string& prefix, X::Value& pvdb) {
            std::lock_guard<std::mutex> lock(m_vdbMutex);
            std::string cacheKey = path + "|" + prefix;
            m_vdbInstances[cacheKey] = pvdb;
        }

        void UnregisterPartitionedVdb(const std::string& path, const std::string& prefix) {
            std::lock_guard<std::mutex> lock(m_vdbMutex);
            std::string cacheKey = path + "|" + prefix;
            m_vdbInstances.erase(cacheKey);
        }

        inline X::Value GetCantor() { return m_cantor; }

        inline long long GetContentSize()
        {
            return sizeof(m_cantor);
        }
        void Test();
        bool GetPartitionedVdb(X::XRuntime* rt, X::XObj* pContext,
            X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
        inline X::XRuntime* RT() { return m_defaultRuntime; }

		inline void SetDefaultRuntime(X::XRuntime* rt)
		{
			m_defaultRuntime = rt;
		}
        QuantaHost()
        {
        }
		~QuantaHost()
		{
		}
        inline void SetPath(std::string& path, std::string& libName)
        {
            m_strQuantaPath = path;
            m_strLibName = libName;
        }
        bool SetCantor(X::Value cantor)
        {
            if (m_cantor.IsValid())
            {
                return true;
            }
            m_cantor = cantor;
            m_log = cantor["log_nolineend"];
            InitLog(m_log);

            std::string strNodeId = GetNodeIdString();
            m_nodeId = UIDFromString(strNodeId);
            OnCantorSet();
            return true;
        }

        void OnCantorSet()
        {
            // Implementation here
        }

        inline bool ToBytes(X::XLStream* pStream)
        {
            bool bOK = X::g_pXHost->ConvertToBytes(m_cantor, pStream);
            return bOK;
        }

        inline bool FromBytes(X::XLStream* pStream)
        {
            bool bOK = X::g_pXHost->ConvertFromBytes(m_cantor, pStream);
            return bOK;
        }
    };
}