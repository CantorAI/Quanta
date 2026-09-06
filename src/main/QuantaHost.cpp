#include "QuantaHost.h"
#include <filesystem>

namespace Quanta {
QuantaHost::~QuantaHost() {
    std::map<std::string, X::Value> instances;
    {
        std::lock_guard<std::mutex> lock(cache_->mutex);
        instances.swap(cache_->instances);
    }
    for (auto& entry : instances) {
        if (auto* database = entry.second.NativeData<PartitionedVdb>()) {
            try { database->Close(); } catch (...) {}
        }
    }
}
bool QuantaHost::SetCantor(X::Value value) {
    std::lock_guard<std::mutex> lock(context_->mutex);
    context_->cantor = std::move(value);
    return true;
}
X::Value QuantaHost::NewDfs(const X::ARGS& args, const X::KWARGS& kwargs) {
    if (!args.empty() || !kwargs.empty()) throw X::Error("dfs expects no arguments");
    return __xlang3_package_->CreateInstance("DfsEngine", new DfsEngine());
}
X::Value QuantaHost::NewVdb(const X::ARGS& args, const X::KWARGS& kwargs) {
    auto* object = new Vdb();
    auto instance = __xlang3_package_->CreateInstance("Vdb", object);
    if (!instance.IsValid()) throw X::Error("cannot create Vdb");
    object->Init(args, kwargs);
    return instance;
}
X::Value QuantaHost::GetPartitionedVdb(const X::ARGS& args, const X::KWARGS& kwargs) {
    auto pathArg = Keyword(kwargs, "path");
    auto prefixArg = Keyword(kwargs, "prefix");
    const auto path = pathArg ? pathArg->ToString() : args.size()>1 ? args[1].ToString() : ".";
    const auto prefix = prefixArg ? prefixArg->ToString() : !args.empty() ? args[0].ToString() : "vdb";
    const auto key = std::filesystem::absolute(path).lexically_normal().make_preferred().string() + "|" + prefix;
    std::unique_lock<std::mutex> lock(cache_->mutex);
    auto it = cache_->instances.find(key);
    if (it != cache_->instances.end()) return it->second;
    auto* object = new PartitionedVdb(context_, cache_, key);
    auto instance = __xlang3_package_->CreateInstance("PartitionedVdb", object);
    if (!instance.IsValid()) throw X::Error("cannot create PartitionedVdb");
    try {
        object->Init(args, kwargs);
        cache_->instances.emplace(key, instance);
    } catch (...) {
        lock.unlock();
        throw;
    }
    return instance;
}
}
