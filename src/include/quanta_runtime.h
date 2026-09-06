#pragma once
#include "xlang3/xlang3.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace Quanta {
template<class... Args> X::Value Invoke(const X::Value& function, Args&&... args) {
    auto result = function(std::forward<Args>(args)...);
    if (!result.IsValid()) {
        const char* error = function.runtime() ? x3_runtime_last_error(function.runtime()) : nullptr;
        throw X::Error(error && *error ? error : "Quanta dependency call failed");
    }
    return result;
}
inline bool IsMapping(const X::Value& value) {
    return value.IsDict() || (x3_value_object_kind(value.raw()) == X3_OBJECT_KIND_INSTANCE && value["keys"].IsValid());
}
inline bool IsSequence(const X::Value& value) {
    const auto kind = x3_value_object_kind(value.raw());
    if (kind == X3_OBJECT_KIND_LIST || kind == X3_OBJECT_KIND_TUPLE) return true;
    if (kind != X3_OBJECT_KIND_INSTANCE || IsMapping(value)) return false;
    uint64_t length = 0;
    return x3_len(value.runtime(), value.raw(), &length) == X3_STATUS_OK &&
        (!length || value.Get(uint64_t(0)).IsValid());
}
inline const X::Value* Keyword(const X::KWARGS& args, const std::string& name) {
    for (const auto& item : args) if (item.first == name) return &item.second;
    return nullptr;
}
inline X::Value Import(X3PackageHost* host, const char* library, const char* name) {
    X3Value value = x3_value_invalid();
    if (x3_runtime_import_module(host->runtime, library, name, &value) != X3_STATUS_OK)
        throw X::Error(host->runtime_last_error(host->runtime));
    return X::Value(host, value, false);
}
struct QuantaContext {
    X::Value tracker_class;
    X::Value metric_class;
    X3PackageHost* host;
    mutable std::mutex mutex;
    X::Value cantor;
    explicit QuantaContext(X3PackageHost* h) : host(h) {}
    X::Value Cantor() const { std::lock_guard<std::mutex> lock(mutex); return cantor; }
};
struct VdbCache {
    std::mutex mutex;
    std::map<std::string, X::Value> instances;
};
}
