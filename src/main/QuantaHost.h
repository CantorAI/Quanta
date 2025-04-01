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

namespace Quanta
{
    class QuantaHost :
        public Singleton<QuantaHost>
    {
        X::Runtime* m_defaultRuntime = nullptr;
        std::string m_strQuantaPath;
        std::string m_strLibName;
        X::Value m_cantor;
        X::Value m_log;
        UID m_nodeId;

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
            APISET().AddClass<2, Vdb>("vdb");
            APISET().AddFunc<1>("SetCantor", &QuantaHost::SetCantor);
            APISET().AddPropL("cantor",
                [](auto* pThis, X::Value v)
                {
                    pThis->SetCantor(v);
                },
                [](auto* pThis) {return pThis->m_cantor; });
            APISET().AddFunc<0>("Test", &QuantaHost::Test);
        END_PACKAGE

        inline long long GetContentSize()
        {
            return sizeof(m_cantor);
        }
        void Test();
        inline X::Runtime& RT() { return *m_defaultRuntime; }

        QuantaHost()
        {
            m_defaultRuntime = new X::Runtime();
        }
		~QuantaHost()
		{
            if (m_defaultRuntime)
            {
                delete m_defaultRuntime;
            }
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