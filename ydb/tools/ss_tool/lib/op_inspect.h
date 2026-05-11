#pragma once

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard__op_traits.h>

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NKikimr::NSchemeShard::NSsTool {

// One row in ss_tool's tables: identity (name + enum number) plus the
// trait-derived metadata. Identity is purely runtime info that does not
// belong on the trait; the rest is mirrored from TOpDescriptor in
// schemeshard__op_traits.h, which is the single source of truth for trait
// fields.
struct TOpRow {
    TString Name;
    NKikimrSchemeOp::EOperationType Type{};
    TOpDescriptor Desc;
};

// Pretty-print an operation class enum.
TStringBuf ClassName(EOperationClass c);

// Read TOpDescriptor for a single op type via the existing DispatchOp +
// Describe<>() infrastructure. Pure: same input -> same output.
TOpRow CollectRow(NKikimrSchemeOp::EOperationType opType);

// Enumerate every op type from the proto descriptor (deduplicating aliases
// that share an enum number) and collect a row for each.
TVector<TOpRow> AllOps();

// Whether NAME is one of the per-op module method names ss_tool understands
// for filtering. Used to validate --has / --missing arguments.
bool MethodKnown(const TString& name);

// Lookup the per-op-method bool on a descriptor by name. Returns false for
// unknown names (callers are expected to have validated via MethodKnown).
bool HasMethod(const TOpDescriptor& d, const TString& name);

} // namespace NKikimr::NSchemeShard::NSsTool
