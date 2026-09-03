#pragma once

#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard_identificators.h>

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NKikimr::NSchemeShard {

class TSchemeShard;

// How the raw proto value has to be interpreted to become an absolute path.
enum class EPathRefKind {
    // A single leaf name that lives directly under TModifyScheme.WorkingDir.
    LeafUnderWorkingDir,
    // A (possibly multi-segment, possibly already absolute) path resolved
    // relative to TModifyScheme.WorkingDir.
    PathUnderWorkingDir,
    // A possibly multi-segment path that is *always* resolved under
    // TModifyScheme.WorkingDir, one segment at a time: exactly
    // TPath::Child(value, TPath::TSplitChildTag{}). Unlike
    // PathUnderWorkingDir, a leading slash does not make it absolute.
    PathUnderWorkingDirSplit,
    // An absolute path; WorkingDir is not involved.
    Absolute,
    // A leaf name under another field of the same request (BasePath).
    LeafUnderSibling,
    // The path is addressed by a numeric path id, not by name.
    ById,
    // The operation touches paths that are not spelled out in the request at
    // all (runtime/state derived). Value is empty; FieldPath describes what.
    Implicit,
};

enum class EPathRefRole {
    Target,
    Source,
    Parent,
    Dependency,
};

// One path-carrying field of one TModifyScheme, before any resolution.
struct TPathRef {
    // Protobuf field path inside TModifyScheme, e.g. "CreateTable.Name",
    // "Drop.Id", "CreateConsistentCopyTables.CopyTableDescriptions[2].DstPath".
    TString FieldPath;
    TString Value;
    // ById only. OwnerId == 0 means "this schemeshard" (a local path id).
    ui64 OwnerId = 0;
    ui64 LocalPathId = 0;
    EPathRefKind Kind = EPathRefKind::LeafUnderWorkingDir;
    EPathRefRole Role = EPathRefRole::Target;
    // LeafUnderSibling only: the raw value of the sibling field Value hangs off.
    TString BasePath;
    // Index, within the same ExtractPathRefs result, of the ref this one hangs
    // off; -1 when there is none. For Implicit it is the anchor of the
    // runtime-derived set (the path whose children the operation will actually
    // touch). For LeafUnderSibling it is the base, used instead of BasePath
    // when the base cannot be written as a raw string (the base field is
    // addressed by path id, or is itself resolved with TSplitChildTag).
    int AnchorIndex = -1;
};

// Layer 1: pure, state-free extraction. Covers every EOperationType.
TVector<TPathRef> ExtractPathRefs(const NKikimrSchemeOp::TModifyScheme& tx);

// Every protobuf field ExtractPathRefs reads as a path, fully qualified, e.g.
// "NKikimrSchemeOp.TMove.SrcPath". Hand-maintained beside the ExtractPathRefs
// switch: the two must always move together. Consumed by the descriptor-walk
// completeness test, which fails when a path-like field of TModifyScheme is
// neither listed here nor explicitly classified as not-a-path.
const TVector<TStringBuf>& KnownPathFieldNames();

struct TPathFootprintEntry {
    TPathRef Ref;
    TPathId PathId;             // invalid unless Exists
    bool Exists = false;
    TString AbsPath;
    TString RelPathToParent;    // leaf name
    TString RelPathToDatabase;
    TString RelPathToWorkingDir;
    TPathId ParentPathId;       // nearest existing parent when !Exists
    TPathId DatabasePathId;
};

struct TPathFootprint {
    TString WorkingDir;
    TString WorkingDirRelToDb;
    TPathId DatabasePathId;
    TVector<TPathFootprintEntry> Entries;

    // Filled by the ProcessOperationParts hook.
    NKikimrSchemeOp::EOperationType PartOpType = NKikimrSchemeOp::EOperationType::ESchemeOp_DEPRECATED_35;
    NKikimrScheme::EStatus ProposeStatus = NKikimrScheme::StatusSuccess;
    TSubTxId PartId = InvalidSubTxId;
    // Index into TEvModifySchemeTransaction.Transaction of the request
    // transaction this footprint belongs to. Max<ui32>() when unknown. A
    // footprint of the request transaction itself keeps PartId invalid, which
    // is how the two layers are told apart in the log.
    ui32 OriginalTxIndex = Max<ui32>();

    // The in-memory writes this part's Propose() made, taken as the diff of
    // the TMemoryChanges undo log across the call: grab order, deduplicated.
    // Cascades (subtree drops, index and cdc children, moved subtrees) show up
    // here even though no proto field of the request names them.
    TVector<TPathId> WriteSet;
    // The paths this part asked SchemeBoard to republish. Versions are not
    // recorded: they are computed later, at ApplyOnExecute time, and a further
    // part of the same request may still bump them.
    TVector<TPathId> Published;
    // The operation wrote through TOperationContext::GetDB() rather than
    // through TMemoryChanges, so WriteSet is a lower bound. Cumulative for the
    // whole request: TOperationContext::DirectAccessGranted is never reset, so
    // once one part goes direct every later part is flagged too.
    bool WriteSetMayBeIncomplete = false;
};

// Layer 2: normalization through TPath only. Never aborts on bad input.
TPathFootprint ResolvePathFootprint(const NKikimrSchemeOp::TModifyScheme& tx, TSchemeShard* ss);

TStringBuf PathRefKindName(EPathRefKind kind);
TStringBuf PathRefRoleName(EPathRefRole role);

// One-line, greppable rendering of a single footprint entry. The default
// prefix is "PathFootprint"; the request layer passes "PathFootprint request"
// so the two layers can be told apart in the log. Used as the observation
// channel by tests.
TString FormatPathFootprintLine(const TPathFootprint& footprint,
    const TPathFootprintEntry* entry, ui64 txId,
    TStringBuf prefix = "PathFootprint");

// The same prefix, one extra line per footprint, listing the write set and the
// publications as "owner:local" ids. Kept out of the per-entry line because it
// belongs to the part, not to any one field.
TString FormatPathFootprintWriteSetLine(const TPathFootprint& footprint, ui64 txId);

}  // namespace NKikimr::NSchemeShard
