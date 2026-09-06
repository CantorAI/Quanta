#pragma once
#include "quanta_runtime.h"
#include "DfsEng.h"
#include "vdb.h"
#include "partitioned_vdb.h"

namespace Quanta {
class QuantaHost {
    std::shared_ptr<QuantaContext> context_;
    std::shared_ptr<VdbCache> cache_ = std::make_shared<VdbCache>();
public:
    explicit QuantaHost(X3PackageHost* host) : context_(std::make_shared<QuantaContext>(host)) {}
    ~QuantaHost();
    void OnPackageCreated(X::Package<QuantaHost>* package) {
        context_->tracker_class = package->GetValue("SceneTracker");
        context_->metric_class = package->GetValue("_Metric");
    }
    BEGIN_PACKAGE(QuantaHost)
        APISET().AddClass<0, DfsEngine>("DfsEngine");
        APISET().AddClass<0, Vdb>("Vdb");
        APISET().AddClass<0, PartitionedVdb>("PartitionedVdb");
        APISET().AddClass<0, SceneTracker>("SceneTracker");
        APISET().AddClass<0, VdbMetric>("_Metric");
        APISET().AddVarFunc("dfs", &QuantaHost::NewDfs);
        APISET().AddVarFunc("vdb", &QuantaHost::NewVdb);
        APISET().AddVarFunc("partitioned_vdb", &QuantaHost::GetPartitionedVdb);
        APISET().AddVarFunc("GetPartitionedVdb", &QuantaHost::GetPartitionedVdb);
        APISET().AddFunc<1>("SetCantor", &QuantaHost::SetCantor);
        APISET().AddPropL("cantor", [](QuantaHost* self, X::Value value) { self->SetCantor(value); },
            [](QuantaHost* self) { return self->context_->Cantor(); });
    END_PACKAGE
    X::Value NewDfs(const X::ARGS&, const X::KWARGS&);
    X::Value NewVdb(const X::ARGS&, const X::KWARGS&);
    X::Value GetPartitionedVdb(const X::ARGS&, const X::KWARGS&);
    bool SetCantor(X::Value value);
};
}
