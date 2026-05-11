#pragma once

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard__op_traits.h>

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NKikimr::NSchemeShard::NSsTool {

struct TOpRow {
    TString Name;
    NKikimrSchemeOp::EOperationType Type{};
    TOpDescriptor Desc;
};

TStringBuf ClassName(EOperationClass c);
TOpRow CollectRow(NKikimrSchemeOp::EOperationType opType);
TVector<TOpRow> AllOps();

bool MethodKnown(const TString& name);
bool HasMethod(const TOpDescriptor& d, const TString& name);

} // namespace NKikimr::NSchemeShard::NSsTool
