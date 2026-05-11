#include "op_inspect.h"

#include <ydb/core/tx/schemeshard/schemeshard__dispatch_op.h>

#include <util/generic/hash_set.h>

namespace NKikimr::NSchemeShard::NSsTool {

TStringBuf ClassName(EOperationClass c) {
    switch (c) {
        case EOperationClass::Create:  return "Create";
        case EOperationClass::Alter:   return "Alter";
        case EOperationClass::Drop:    return "Drop";
        case EOperationClass::Other:   return "Other";
        case EOperationClass::Unknown: return "Unknown";
    }
    return "?";
}

TOpRow CollectRow(NKikimrSchemeOp::EOperationType opType) {
    TOpRow row;
    row.Type = opType;
    row.Name = NKikimrSchemeOp::EOperationType_Name(opType);

    NKikimrSchemeOp::TModifyScheme tx;
    tx.SetOperationType(opType);
    DispatchOp(tx, [&](auto traits) {
        row.Desc = Describe<decltype(traits)>();
    });

    return row;
}

TVector<TOpRow> AllOps() {
    TVector<TOpRow> result;
    THashSet<int> seen; // proto enum may carry aliases sharing one number
    const auto* d = NKikimrSchemeOp::EOperationType_descriptor();
    for (int i = 0; i < d->value_count(); ++i) {
        const auto* v = d->value(i);
        if (!seen.insert(v->number()).second) {
            continue;
        }
        result.push_back(CollectRow(static_cast<NKikimrSchemeOp::EOperationType>(v->number())));
    }
    return result;
}

bool MethodKnown(const TString& name) {
    return name == "MakeOperationParts" || name == "CollectChangingPaths";
}

bool HasMethod(const TOpDescriptor& d, const TString& name) {
    if (name == "MakeOperationParts")   return d.HasMakeOperationParts;
    if (name == "CollectChangingPaths") return d.HasCollectChangingPaths;
    return false;
}

} // namespace NKikimr::NSchemeShard::NSsTool
