#include "QuantaHost.h"

extern "C" XLANG3_PACKAGE_EXPORT const uint32_t xlang3_package_abi_version = X3_ABI_VERSION;
extern "C" XLANG3_PACKAGE_EXPORT X3Status Load(void* raw, X3Value currentModule) {
    auto* host = static_cast<X3PackageHost*>(raw);
    if (!host || host->abi_version != X3_ABI_VERSION) return X3_STATUS_ERROR;
    Quanta::QuantaHost::BuildAPI();
    return Quanta::QuantaHost::APISET().Create(host, "quanta", currentModule);
}
