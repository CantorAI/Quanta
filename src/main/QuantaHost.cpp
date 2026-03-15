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
        }
        if (auto it = kwParams.find("prefix"); it) {
            prefix = it->val.ToString();
        }

        std::string cacheKey = path + "|" + prefix;

        std::lock_guard<std::mutex> lock(m_vdbMutex);
        if (m_vdbInstances.find(cacheKey) != m_vdbInstances.end()) {
            retValue = m_vdbInstances[cacheKey];
            return true;
        }

        // It's a new path. Create it manually via native XPackage instantiation.
        X::XPackageValue<PartitionedVdb> PVDB(params, kwParams);
        X::Value varPvdb = PVDB;

        m_vdbInstances[cacheKey] = varPvdb;
        retValue = varPvdb;
        return true;
    }
}
