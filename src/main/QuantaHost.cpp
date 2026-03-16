#include "QuantaHost.h"

extern int vdb_test();
namespace Quanta
{
    void QuantaHost::Test()
    {
        //vdb_test();
    }

    bool QuantaHost::GetPartitionedVdb(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        std::string path = "";
        std::string prefix = "vdb";

        if (auto it = kwParams.find("path"); it) {
            path = it->val.ToString();
            // Normalize path for consistent caching
            if (!path.empty()) {
                path = std::filesystem::absolute(std::filesystem::path(path)).make_preferred().string();
            }
        }
        if (auto it = kwParams.find("prefix"); it) {
            prefix = it->val.ToString();
        }

        std::string cacheKey = path + "|" + prefix;
        {
            std::lock_guard<std::mutex> lock(m_vdbMutex);
            auto it = m_vdbInstances.find(cacheKey);
            if (it != m_vdbInstances.end()) {
                retValue = it->second;
                return true;
            }
        }

        // It's a new path. Create it manually via native XPackage instantiation.
        X::XPackageValue<PartitionedVdb> PVDB;
        X::Value dummyRet;
        bool res = PVDB->Init(rt, pContext, params, kwParams, dummyRet);
        // Optionally log result
        X::Value varPvdb = PVDB;

        m_vdbInstances[cacheKey] = varPvdb;
        retValue = varPvdb;
        return true;
    }
}
