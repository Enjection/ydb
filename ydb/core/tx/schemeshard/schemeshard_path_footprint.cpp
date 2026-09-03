#include "schemeshard_path_footprint.h"

#include "schemeshard_impl.h"
#include "schemeshard_path.h"

#include <ydb/core/base/path.h>

#include <util/generic/algorithm.h>
#include <util/string/builder.h>

namespace NKikimr::NSchemeShard {

namespace {

using EKind = EPathRefKind;
using ERole = EPathRefRole;

////////////////////////////////////////////////////////////////////////////////
// The static columns of SCHEMESHARD_PATH_FIELDS, indexed by EPathField.

#define SCHEMESHARD_PATH_FIELD_TEMPLATE(name, tpl, proto, kind, role) TStringBuf(tpl),
#define SCHEMESHARD_PATH_FIELD_PROTO(name, tpl, proto, kind, role) TStringBuf(proto),
#define SCHEMESHARD_PATH_FIELD_KIND(name, tpl, proto, kind, role) EKind::kind,
#define SCHEMESHARD_PATH_FIELD_ROLE(name, tpl, proto, kind, role) ERole::role,

constexpr TStringBuf FieldTemplates[] = {
    SCHEMESHARD_PATH_FIELDS(SCHEMESHARD_PATH_FIELD_TEMPLATE)
};
constexpr TStringBuf FieldProtoNames[] = {
    SCHEMESHARD_PATH_FIELDS(SCHEMESHARD_PATH_FIELD_PROTO)
};
constexpr EKind FieldKinds[] = {
    SCHEMESHARD_PATH_FIELDS(SCHEMESHARD_PATH_FIELD_KIND)
};
constexpr ERole FieldRoles[] = {
    SCHEMESHARD_PATH_FIELDS(SCHEMESHARD_PATH_FIELD_ROLE)
};

#undef SCHEMESHARD_PATH_FIELD_TEMPLATE
#undef SCHEMESHARD_PATH_FIELD_PROTO
#undef SCHEMESHARD_PATH_FIELD_KIND
#undef SCHEMESHARD_PATH_FIELD_ROLE

constexpr size_t PathFieldCount = static_cast<size_t>(EPathField::Count);
static_assert(std::size(FieldTemplates) == PathFieldCount);
static_assert(std::size(FieldProtoNames) == PathFieldCount);
static_assert(std::size(FieldKinds) == PathFieldCount);
static_assert(std::size(FieldRoles) == PathFieldCount);

size_t FieldIndex(EPathField field) {
    const size_t index = static_cast<size_t>(field);
    Y_DEBUG_ABORT_UNLESS(index < PathFieldCount);
    return index < PathFieldCount ? index : 0;
}

}  // namespace

TStringBuf PathFieldName(EPathField field) {
    return FieldTemplates[FieldIndex(field)];
}

TStringBuf PathFieldProtoName(EPathField field) {
    return FieldProtoNames[FieldIndex(field)];
}

EPathRefKind PathFieldDefaultKind(EPathField field) {
    return FieldKinds[FieldIndex(field)];
}

EPathRefRole PathFieldDefaultRole(EPathField field) {
    return FieldRoles[FieldIndex(field)];
}

TString FieldPath(const TPathRef& ref) {
    const TStringBuf tpl = PathFieldName(ref.Field);
    if (tpl.find('{') == TStringBuf::npos) {
        return TString(tpl);
    }
    TStringBuilder rendered;
    size_t pos = 0;
    while (pos < tpl.size()) {
        const size_t open = tpl.find('{', pos);
        if (open == TStringBuf::npos) {
            rendered << tpl.substr(pos);
            break;
        }
        rendered << tpl.substr(pos, open - pos);
        const size_t close = tpl.find('}', open + 1);
        if (close == TStringBuf::npos) {
            rendered << tpl.substr(open);
            break;
        }
        const TStringBuf placeholder = tpl.substr(open + 1, close - open - 1);
        if (placeholder == "i") {
            rendered << ref.Index;
        } else if (placeholder == "j") {
            rendered << ref.SubIndex;
        } else {
            rendered << ref.MapKey;
        }
        pos = close + 1;
    }
    return rendered;
}

const TVector<TStringBuf>& KnownPathFieldNames() {
    static const TVector<TStringBuf> names = [] {
        TVector<TStringBuf> collected;
        collected.reserve(PathFieldCount);
        for (const TStringBuf name : FieldProtoNames) {
            // Empty for a synthetic Implicit marker, for the working dir, and
            // for an id-valued field: the descriptor walk classifies string
            // fields only.
            if (!name.empty()) {
                collected.push_back(name);
            }
        }
        // Several fields share one protobuf field (the same submessage read
        // under two prefixes, e.g. Replication and AlterReplication).
        SortUnique(collected);
        return collected;
    }();
    return names;
}

namespace {

// Where a ref sits inside a repeated field or a map, rendered into the field
// path through the "{i}", "{j}" and "{key}" placeholders. Kept outside TRefSink
// because a nested class's default member initializers are not usable in the
// enclosing class's default arguments.
struct TRefAt {
    ui32 Index = Max<ui32>();
    ui32 SubIndex = Max<ui32>();
    TStringBuf Key;
};

class TRefSink {
public:
    using TAt = TRefAt;

    explicit TRefSink(TPathRefs& out)
        : Out(out)
    {}

    // The ordinary case: kind and role are the field's table defaults.
    void Add(EPathField field, TStringBuf value, TAt at = {}) {
        Emit(field, value, PathFieldDefaultKind(field), PathFieldDefaultRole(field), {}, at);
    }

    // The same protobuf field resolved differently because of the operation
    // type carrying it: the CDC-stream AtTable/Impl parts, DropIndex.TableName
    // under DropTableIndexAtMainTable, and an absolute DefaultFromSequence.
    void AddAs(EPathField field, TStringBuf value, EKind kind, ERole role, TAt at = {}) {
        Emit(field, value, kind, role, {}, at);
    }

    // Shape 4/5: a leaf name under another field of the same request.
    void Sibling(EPathField field, TStringBuf value, TStringBuf base, TAt at = {}) {
        Emit(field, value, PathFieldDefaultKind(field), PathFieldDefaultRole(field), base, at);
    }

    // Shape 4/5 when the base cannot be written as a raw string: a leaf under
    // the path of an already-emitted ref. Needed when the base field may be
    // addressed by path id, or is itself resolved with TSplitChildTag.
    void SiblingOf(EPathField field, TStringBuf value, int anchorIndex, TAt at = {}) {
        Emit(field, value, PathFieldDefaultKind(field), PathFieldDefaultRole(field), {}, at)
            .AnchorIndex = anchorIndex;
    }

    // Shape 2: numeric-id addressing, bypasses WorkingDir/Name.
    void ById(EPathField field, ui64 ownerId, ui64 localPathId) {
        TPathRef& ref = Emit(field, {}, PathFieldDefaultKind(field),
            PathFieldDefaultRole(field), {}, {});
        ref.OwnerId = ownerId;
        ref.LocalPathId = localPathId;
    }

    // Shape 8/9: touched paths that the request does not name at all. The set
    // is enumerated at Propose/Execute time from the children of the anchor.
    void Implicit(EPathField field, int anchorIndex, TAt at = {}) {
        Emit(field, {}, PathFieldDefaultKind(field), PathFieldDefaultRole(field), {}, at)
            .AnchorIndex = anchorIndex;
    }

    // Index of the most recently added ref; use as an anchor.
    int Last() const {
        return static_cast<int>(Out.Refs.size()) - 1;
    }

    // Stable storage for a base path that has to be computed rather than read
    // straight out of the request. The only such case is an index impl table
    // under a copied table's source path.
    TStringBuf Own(TString value) {
        Out.Owned.push_back(std::move(value));
        return Out.Owned.back();
    }

private:
    TPathRef& Emit(EPathField field, TStringBuf value, EKind kind, ERole role,
            TStringBuf base, TAt at) {
        TPathRef ref;
        ref.Field = field;
        ref.Index = at.Index;
        ref.SubIndex = at.SubIndex;
        ref.MapKey = at.Key;
        ref.Value = value;
        ref.Kind = kind;
        ref.Role = role;
        ref.BasePath = base;
        Out.Refs.push_back(ref);
        return Out.Refs.back();
    }

    TPathRefs& Out;
};

// Protobuf map iteration order is unspecified; sort so the footprint is stable.
template <class TMap>
TVector<const typename TMap::value_type*> SortedByKey(const TMap& m) {
    TVector<const typename TMap::value_type*> items;
    items.reserve(m.size());
    for (const auto& kv : m) {
        items.push_back(&kv);
    }
    Sort(items, [](const auto* l, const auto* r) { return l->first < r->first; });
    return items;
}

}  // namespace

TStringBuf PathRefKindName(EPathRefKind kind) {
    switch (kind) {
    case EPathRefKind::LeafUnderWorkingDir: return "LeafUnderWorkingDir";
    case EPathRefKind::PathUnderWorkingDir: return "PathUnderWorkingDir";
    case EPathRefKind::PathUnderWorkingDirSplit: return "PathUnderWorkingDirSplit";
    case EPathRefKind::Absolute: return "Absolute";
    case EPathRefKind::LeafUnderSibling: return "LeafUnderSibling";
    case EPathRefKind::ById: return "ById";
    case EPathRefKind::Implicit: return "Implicit";
    }
    return "Unknown";
}

TStringBuf PathRefRoleName(EPathRefRole role) {
    switch (role) {
    case EPathRefRole::Target: return "Target";
    case EPathRefRole::Source: return "Source";
    case EPathRefRole::Parent: return "Parent";
    case EPathRefRole::Dependency: return "Dependency";
    }
    return "Unknown";
}

namespace {

// Compact, separator-free rendering: TPathId::Out() emits ", " inside itself,
// which the log line format cannot carry.
TString JoinPathIds(const TVector<TPathId>& pathIds) {
    TStringBuilder joined;
    for (size_t i = 0; i < pathIds.size(); ++i) {
        if (i) {
            joined << ',';
        }
        joined << pathIds[i].OwnerId << ':' << pathIds[i].LocalPathId;
    }
    return joined;
}

TStringBuilder FormatPathFootprintPrefix(const TPathFootprint& footprint, ui64 txId,
        TStringBuf prefix) {
    TStringBuilder line;
    line << prefix
         << " txId# " << txId
         << ", partId# ";
    if (footprint.PartId == InvalidSubTxId) {
        line << "<request>";
    } else {
        line << ui32(footprint.PartId);
    }
    line << ", originalTxIndex# " << footprint.OriginalTxIndex
         << ", partOpType# " << NKikimrSchemeOp::EOperationType_Name(footprint.PartOpType)
         << ", proposeStatus# " << NKikimrScheme::EStatus_Name(footprint.ProposeStatus)
         << ", writeSet# " << footprint.WriteSet.size()
         << ", published# " << footprint.Published.size()
         << ", incomplete# " << (footprint.WriteSetMayBeIncomplete ? 1 : 0);
    return line;
}

}  // namespace

TString FormatPathFootprintWriteSetLine(const TPathFootprint& footprint, ui64 txId) {
    TStringBuilder line = FormatPathFootprintPrefix(footprint, txId, "PathFootprint");
    return line << ", fieldPath# <writeSet>"
                << ", writeSetPaths# " << JoinPathIds(footprint.WriteSet)
                << ", publishedPaths# " << JoinPathIds(footprint.Published);
}

TString FormatPathFootprintLine(const TPathFootprint& footprint,
        const TPathFootprintEntry* entry, ui64 txId, TStringBuf prefix) {
    TStringBuilder line = FormatPathFootprintPrefix(footprint, txId, prefix);
    line << ", workingDir# " << footprint.WorkingDir
         << ", workingDirRelToDb# " << footprint.WorkingDirRelToDb;
    if (!entry) {
        return line << ", fieldPath# <none>";
    }
    line << ", fieldPath# " << entry->Ref.FieldPath
         << ", kind# " << PathRefKindName(entry->Ref.Kind)
         << ", role# " << PathRefRoleName(entry->Ref.Role)
         << ", absPath# " << entry->AbsPath
         << ", pathId# " << entry->PathId
         << ", exists# " << (entry->Exists ? 1 : 0)
         << ", relToParent# " << entry->RelPathToParent
         << ", relToDb# " << entry->RelPathToDatabase
         << ", relToWorkingDir# " << entry->RelPathToWorkingDir;
    return line;
}

TPathRefs ExtractPathRefs(const NKikimrSchemeOp::TModifyScheme& tx) {
    using F = EPathField;
    using TAt = TRefSink::TAt;

    TPathRefs result;
    TRefSink out(result);

    // Shape 2: the generic TDrop submessage, shared by ~22 op types.
    const auto genericDrop = [&]() {
        const auto& drop = tx.GetDrop();
        if (drop.HasId()) {
            // Propose() resolves TPath::Init(MakeLocalId(Id)) and ignores Name.
            out.ById(F::Drop_Id, 0, drop.GetId());
        } else {
            out.Add(F::Drop_Name, drop.GetName());
        }
    };

    // Every TTL tier that evicts to external storage names an external data
    // source by absolute path; Propose resolves each one and persists a
    // reference (olap/operations/create_table.cpp:820). The proto field is
    // "Storage", which no name heuristic would guess is a path.
    const auto emitTierStorages = [&](F field, const NKikimrSchemeOp::TColumnDataLifeCycle& ttl) {
        if (!ttl.HasEnabled()) {
            return;
        }
        const auto& tiers = ttl.GetEnabled().GetTiers();
        for (int i = 0; i < tiers.size(); ++i) {
            if (tiers[i].HasEvictToExternalStorage()) {
                out.Add(field, tiers[i].GetEvictToExternalStorage().GetStorage(),
                    TAt{.Index = ui32(i)});
            }
        }
    };

    // Local paths carried by a replication/transfer description. SrcPath is a
    // path on the *remote* cluster and is deliberately never emitted. The four
    // field ids differ between Replication and AlterReplication, which is the
    // only thing the two call sites disagree about.
    const auto replicationPaths = [&](F transferDstPath, F transferDirectoryPath,
            F specificTargetDstPath, F alterTransferDirectoryPath,
            const NKikimrSchemeOp::TReplicationDescription& desc) {
        const auto& config = desc.GetConfig();
        if (config.HasTransferSpecific()) {
            // TTransferStrategy::Validate resolves both of these absolutely
            // (schemeshard__operation_create_replication.cpp:80,91).
            const auto& target = config.GetTransferSpecific().GetTarget();
            if (target.HasDstPath()) {
                out.Add(transferDstPath, target.GetDstPath());
            }
            if (target.HasDirectoryPath()) {
                out.Add(transferDirectoryPath, target.GetDirectoryPath());
            }
        }
        // Plain (non-transfer) replication targets are NOT resolved by
        // Propose -- TReplicationStrategy::Validate touches no TPath and the
        // replication controller creates the destination later -- but DstPath
        // is an absolute local path this operation intends to write to.
        const auto& specific = config.GetSpecific();
        for (size_t i = 0; i < specific.TargetsSize(); ++i) {
            const auto& target = specific.GetTargets(i);
            if (target.HasDstPath()) {
                out.Add(specificTargetDstPath, target.GetDstPath(), TAt{.Index = ui32(i)});
            }
        }
        if (desc.HasAlterTransfer() && desc.GetAlterTransfer().HasDirectoryPath()) {
            // schemeshard__operation_alter_replication.cpp:57, absolute.
            out.Add(alterTransferDirectoryPath, desc.GetAlterTransfer().GetDirectoryPath());
        }
    };

    switch (tx.GetOperationType()) {
    case NKikimrSchemeOp::ESchemeOpMkDir:
        out.Add(F::MkDir_Name, tx.GetMkDir().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateTable:
        out.Add(F::CreateTable_Name, tx.GetCreateTable().GetName());
        // A CreateTable that carries CopyFromTable is dispatched to the
        // copy-table factory (schemeshard__operation.cpp), whose Propose
        // resolves the source absolutely, without WorkingDir
        // (schemeshard__operation_copy_table.cpp:568,978).
        if (tx.GetCreateTable().HasCopyFromTable()) {
            out.Add(F::CreateTable_CopyFromTable, tx.GetCreateTable().GetCopyFromTable());
        }
        break;
    case NKikimrSchemeOp::ESchemeOpCreatePersQueueGroup:
        out.Add(F::CreatePersQueueGroup_Name, tx.GetCreatePersQueueGroup().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropTable:
        genericDrop();
        out.Implicit(F::Implicit_DropTable_Children, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpDropPersQueueGroup:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterTable: {
        const auto& alter = tx.GetAlterTable();
        if (alter.HasPathId()) {
            const auto pathId = TPathId::FromProto(alter.GetPathId());
            out.ById(F::AlterTable_PathId, pathId.OwnerId, pathId.LocalPathId);
        } else if (alter.HasId_Deprecated()) {
            out.ById(F::AlterTable_Id_Deprecated, 0, alter.GetId_Deprecated());
        } else {
            out.Add(F::AlterTable_Name, alter.GetName());
        }
        const int alterTableIndex = out.Last();
        // schemeshard__operation_alter_table.cpp:665: absolute when the value
        // starts with a slash, otherwise a leaf under the altered table. The
        // table may be addressed by id, so the base is that entry, not a name.
        for (size_t i = 0; i < alter.ColumnsSize(); ++i) {
            const auto& column = alter.GetColumns(i);
            if (!column.HasDefaultFromSequence()) {
                continue;
            }
            const TAt at{.Index = ui32(i)};
            const TString& value = column.GetDefaultFromSequence();
            if (value.StartsWith('/')) {
                out.AddAs(F::AlterTable_Column_DefaultFromSequence, value,
                    EKind::Absolute, ERole::Dependency, at);
            } else {
                out.SiblingOf(F::AlterTable_Column_DefaultFromSequence, value,
                    alterTableIndex, at);
            }
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterPersQueueGroup: {
        const auto& alter = tx.GetAlterPersQueueGroup();
        if (alter.HasPathId()) {
            out.ById(F::AlterPersQueueGroup_PathId, 0, alter.GetPathId());
        } else {
            out.Add(F::AlterPersQueueGroup_Name, alter.GetName());
        }
        // schemeshard__operation_alter_pq.cpp:328 resolves the incremental
        // backup destination absolutely while building the alter data.
        const auto& offload = alter.GetPQTabletConfig().GetOffloadConfig();
        if (offload.HasIncrementalBackup()) {
            out.Add(F::AlterPersQueueGroup_IncrementalBackup_DstPath,
                offload.GetIncrementalBackup().GetDstPath());
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpModifyACL:
        out.Add(F::ModifyACL_Name, tx.GetModifyACL().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpRmDir:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions: {
        const auto& info = tx.GetSplitMergeTablePartitions();
        if (info.HasTableLocalId()) {
            out.ById(F::SplitMergeTablePartitions_TableLocalId,
                info.GetTableOwnerId(), info.GetTableLocalId());
        } else {
            // Propose() resolves TablePath directly, WITHOUT joining WorkingDir.
            out.Add(F::SplitMergeTablePartitions_TablePath, info.GetTablePath());
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpBackup:
        out.Add(F::Backup_TableName, tx.GetBackup().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSubDomain:
        out.Add(F::SubDomain_Name, tx.GetSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSubDomain:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateRtmrVolume:
        out.Add(F::CreateRtmrVolume_Name, tx.GetCreateRtmrVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateBlockStoreVolume:
        out.Add(F::CreateBlockStoreVolume_Name, tx.GetCreateBlockStoreVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterBlockStoreVolume: {
        const auto& alter = tx.GetAlterBlockStoreVolume();
        if (alter.HasPathId()) {
            out.ById(F::AlterBlockStoreVolume_PathId, 0, alter.GetPathId());
        } else {
            out.Add(F::AlterBlockStoreVolume_Name, alter.GetName());
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAssignBlockStoreVolume:
        out.Add(F::AssignBlockStoreVolume_Name, tx.GetAssignBlockStoreVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropBlockStoreVolume:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateKesus:
        out.Add(F::Kesus_Name, tx.GetKesus().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropKesus:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpForceDropSubDomain:
        genericDrop();
        out.Implicit(F::Implicit_ForceDropSubDomain_Subtree, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSolomonVolume:
        out.Add(F::CreateSolomonVolume_Name, tx.GetCreateSolomonVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSolomonVolume:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterKesus:
        out.Add(F::Kesus_Name, tx.GetKesus().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterSubDomain:
        out.Add(F::SubDomain_Name, tx.GetSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterUserAttributes:
        out.Add(F::AlterUserAttributes_PathName, tx.GetAlterUserAttributes().GetPathName());
        break;
    case NKikimrSchemeOp::ESchemeOpForceDropUnsafe:
        genericDrop();
        out.Implicit(F::Implicit_ForceDropUnsafe_Subtree, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateIndexedTable: {
        const auto& cfg = tx.GetCreateIndexedTable();
        const TString& base = cfg.GetTableDescription().GetName();
        out.Add(F::CreateIndexedTable_TableDescription_Name, base);
        const int baseIndex = out.Last();
        for (size_t i = 0; i < cfg.IndexDescriptionSize(); ++i) {
            out.Sibling(F::CreateIndexedTable_IndexDescription_Name,
                cfg.GetIndexDescription(i).GetName(), base, TAt{.Index = ui32(i)});
        }
        for (size_t i = 0; i < cfg.SequenceDescriptionSize(); ++i) {
            out.Sibling(F::CreateIndexedTable_SequenceDescription_Name,
                cfg.GetSequenceDescription(i).GetName(), base, TAt{.Index = ui32(i)});
        }
        out.Implicit(F::Implicit_CreateIndexedTable_IndexImplTables, baseIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateTableIndex:
        out.Add(F::CreateTableIndex_Name, tx.GetCreateTableIndex().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables: {
        const auto& cfg = tx.GetCreateConsistentCopyTables();
        for (size_t i = 0; i < cfg.CopyTableDescriptionsSize(); ++i) {
            const auto& item = cfg.GetCopyTableDescriptions(i);
            const TAt at{.Index = ui32(i)};
            out.Add(F::CopyTables_Item_SrcPath, item.GetSrcPath(), at);
            const int srcIndex = out.Last();
            out.Add(F::CopyTables_Item_DstPath, item.GetDstPath(), at);
            if (item.HasCreateSrcCdcStream()) {
                out.Sibling(F::CopyTables_Item_CreateSrcCdc_StreamName,
                    item.GetCreateSrcCdcStream().GetStreamDescription().GetName(),
                    item.GetSrcPath(), at);
            }
            if (item.HasDropSrcCdcStream()) {
                const auto& drop = item.GetDropSrcCdcStream();
                for (size_t j = 0; j < drop.StreamNameSize(); ++j) {
                    out.Sibling(F::CopyTables_Item_DropSrcCdc_StreamName,
                        drop.GetStreamName(j), item.GetSrcPath(),
                        TAt{.Index = ui32(i), .SubIndex = ui32(j)});
                }
            }
            for (const auto* kv : SortedByKey(item.GetIndexImplTableCdcStreams())) {
                out.Sibling(F::CopyTables_Item_IndexImplCdc_StreamName,
                    kv->second.GetStreamDescription().GetName(),
                    out.Own(JoinPath({item.GetSrcPath(), kv->first})),
                    TAt{.Index = ui32(i), .Key = kv->first});
            }
            for (const auto* kv : SortedByKey(item.GetIndexImplTableDropCdcStreams())) {
                const TStringBuf base = out.Own(JoinPath({item.GetSrcPath(), kv->first}));
                for (size_t j = 0; j < kv->second.StreamNameSize(); ++j) {
                    out.Sibling(F::CopyTables_Item_IndexImplDropCdc_StreamName,
                        kv->second.GetStreamName(j), base,
                        TAt{.Index = ui32(i), .SubIndex = ui32(j), .Key = kv->first});
                }
            }
            out.Implicit(F::Implicit_CopyTables_Item_Children, srcIndex, at);
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropTableIndex:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateExtSubDomain:
    case NKikimrSchemeOp::ESchemeOpAlterExtSubDomain:
    case NKikimrSchemeOp::ESchemeOpAlterExtSubDomainCreateHive:
        out.Add(F::SubDomain_Name, tx.GetSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpForceDropExtSubDomain:
        genericDrop();
        out.Implicit(F::Implicit_ForceDropExtSubDomain_Subtree, out.Last());
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOp_DEPRECATED_35:
        break;
    case NKikimrSchemeOp::ESchemeOpUpgradeSubDomain:
    case NKikimrSchemeOp::ESchemeOpUpgradeSubDomainDecision:
        out.Add(F::UpgradeSubDomain_Name, tx.GetUpgradeSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateIndexBuild: {
        const auto& cfg = tx.GetInitiateIndexBuild();
        out.Add(F::InitiateIndexBuild_Table, cfg.GetTable());
        out.Sibling(F::InitiateIndexBuild_Index_Name, cfg.GetIndex().GetName(), cfg.GetTable());
        out.Implicit(F::Implicit_InitiateIndexBuild_IndexImplTables, out.Last());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpInitiateBuildIndexMainTable:
        out.Add(F::InitiateBuildIndexMainTable_TableName,
            tx.GetInitiateBuildIndexMainTable().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpPrepareIndexValidation:
        out.Add(F::PrepareIndexValidation_TableName,
            tx.GetPrepareIndexValidation().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateLock:
    case NKikimrSchemeOp::ESchemeOpDropLock:
        out.Add(F::LockConfig_Name, tx.GetLockConfig().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpApplyIndexBuild: {
        const auto& cfg = tx.GetApplyIndexBuild();
        out.Add(F::ApplyIndexBuild_TablePath, cfg.GetTablePath());
        out.Sibling(F::ApplyIndexBuild_IndexName, cfg.GetIndexName(), cfg.GetTablePath());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpFinalizeBuildIndexMainTable:
        out.Add(F::FinalizeBuildIndexMainTable_TableName,
            tx.GetFinalizeBuildIndexMainTable().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterTableIndex:
        out.Add(F::AlterTableIndex_Name, tx.GetAlterTableIndex().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterSolomonVolume:
        out.Add(F::AlterSolomonVolume_Name, tx.GetAlterSolomonVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpFinalizeBuildIndexImplTable:
        out.Add(F::AlterTable_Name, tx.GetAlterTable().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpInitiateBuildIndexImplTable:
        out.Add(F::CreateTable_Name, tx.GetCreateTable().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropIndex: {
        const auto& cfg = tx.GetDropIndex();
        out.Add(F::DropIndex_TableName, cfg.GetTableName());
        out.Sibling(F::DropIndex_IndexName, cfg.GetIndexName(), cfg.GetTableName());
        out.Implicit(F::Implicit_DropIndex_IndexImplTables, out.Last());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropTableIndexAtMainTable: {
        // index/operation_drop_index.cpp:278,311 resolves WorkingDir/TableName
        // and then tablePath.Child(IndexName).
        const auto& cfg = tx.GetDropIndex();
        // Plain Dive, no split, and the table is the target here rather than
        // the parent of one: not the DropIndex_TableName table defaults.
        out.AddAs(F::DropIndex_TableName, cfg.GetTableName(),
            EKind::LeafUnderWorkingDir, ERole::Target);
        out.Sibling(F::DropIndex_IndexName, cfg.GetIndexName(), cfg.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCancelIndexBuild: {
        const auto& cfg = tx.GetCancelIndexBuild();
        out.Add(F::CancelIndexBuild_TablePath, cfg.GetTablePath());
        out.Sibling(F::CancelIndexBuild_IndexName, cfg.GetIndexName(), cfg.GetTablePath());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateFileStore:
        out.Add(F::CreateFileStore_Name, tx.GetCreateFileStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterFileStore:
        out.Add(F::AlterFileStore_Name, tx.GetAlterFileStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropFileStore:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpRestore:
        out.Add(F::Restore_TableName, tx.GetRestore().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateColumnStore:
        out.Add(F::CreateColumnStore_Name, tx.GetCreateColumnStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterColumnStore:
        out.Add(F::AlterColumnStore_Name, tx.GetAlterColumnStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropColumnStore:
        genericDrop();
        out.Implicit(F::Implicit_DropColumnStore_ColumnTables, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateColumnTable:
        // CreateColumnTable is a *separate* TModifyScheme field from
        // AlterColumnTable; olap/operations/create_table.cpp:570,643 resolves
        // WorkingDir.Child(CreateColumnTable.Name).
        out.Add(F::CreateColumnTable_Name, tx.GetCreateColumnTable().GetName());
        // With CopyFromTable set the op is dispatched to TReadOnlyCopyColumnTable
        // (schemeshard__operation.cpp:1433), whose Propose resolves the source
        // absolutely (olap/operations/read_only_copy_table.cpp:401,425).
        if (tx.GetCreateColumnTable().HasCopyFromTable()) {
            out.Add(F::CreateColumnTable_CopyFromTable,
                tx.GetCreateColumnTable().GetCopyFromTable());
        }
        emitTierStorages(F::CreateColumnTable_TierStorage,
            tx.GetCreateColumnTable().GetTtlSettings());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterColumnTable:
        // olap/operations/alter_table.cpp:278 falls back to AlterTable.Name
        // when the AlterColumnTable submessage is absent.
        if (tx.HasAlterColumnTable()) {
            out.Add(F::AlterColumnTable_Name, tx.GetAlterColumnTable().GetName());
            const int alterColumnTableIndex = out.Last();
            emitTierStorages(F::AlterColumnTable_TierStorage,
                tx.GetAlterColumnTable().GetAlterTtlSettings());
            out.Implicit(F::Implicit_AlterColumnTable_DroppedTiers, alterColumnTableIndex);
        } else {
            out.Add(F::AlterTable_Name, tx.GetAlterTable().GetName());
        }
        break;
    case NKikimrSchemeOp::ESchemeOpDropColumnTable:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterLogin:
        // Touches no TPath at all; only validates WorkingDir == Audience.
        break;
    case NKikimrSchemeOp::ESchemeOpCreateCdcStream: {
        // WorkingDir + TableName (parent) + stream leaf.
        const auto& op = tx.GetCreateCdcStream();
        out.Add(F::CreateCdcStream_TableName, op.GetTableName());
        out.Sibling(F::CreateCdcStream_StreamDescription_Name,
            op.GetStreamDescription().GetName(), op.GetTableName());
        out.Implicit(F::Implicit_CreateCdcStream_PqGroupUnderStream, out.Last());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateCdcStreamImpl:
        out.AddAs(F::CreateCdcStream_StreamDescription_Name,
            tx.GetCreateCdcStream().GetStreamDescription().GetName(),
            EKind::LeafUnderWorkingDir, ERole::Target);
        break;
    case NKikimrSchemeOp::ESchemeOpCreateCdcStreamAtTable: {
        // The AtTable half alters the table, and also resolves the stream leaf
        // to fill txState.CdcPathId
        // (schemeshard__operation_create_cdc_stream.cpp:541,596).
        const auto& op = tx.GetCreateCdcStream();
        // :541 is a *plain* workingDirPath.Child(tableName) -- no
        // TSplitChildTag, unlike the Alter and Rotate AtTable parts below.
        out.AddAs(F::CreateCdcStream_TableName, op.GetTableName(),
            EKind::LeafUnderWorkingDir, ERole::Target);
        out.Sibling(F::CreateCdcStream_StreamDescription_Name,
            op.GetStreamDescription().GetName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterCdcStream: {
        const auto& op = tx.GetAlterCdcStream();
        out.Add(F::AlterCdcStream_TableName, op.GetTableName());
        out.Sibling(F::AlterCdcStream_StreamName, op.GetStreamName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterCdcStreamImpl:
        out.AddAs(F::AlterCdcStream_StreamName, tx.GetAlterCdcStream().GetStreamName(),
            EKind::LeafUnderWorkingDir, ERole::Target);
        break;
    case NKikimrSchemeOp::ESchemeOpAlterCdcStreamAtTable: {
        // :375 resolves the table with Child(TableName, TSplitChildTag{}), so a
        // multi-segment TableName is dived segment by segment under WorkingDir;
        // :404 then takes a plain tablePath.Child(StreamName).
        const auto& op = tx.GetAlterCdcStream();
        out.AddAs(F::AlterCdcStream_TableName, op.GetTableName(),
            EKind::PathUnderWorkingDirSplit, ERole::Target);
        out.Sibling(F::AlterCdcStream_StreamName, op.GetStreamName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropCdcStream: {
        const auto& op = tx.GetDropCdcStream();
        out.Add(F::DropCdcStream_TableName, op.GetTableName());
        for (size_t i = 0; i < op.StreamNameSize(); ++i) {
            out.Sibling(F::DropCdcStream_StreamName, op.GetStreamName(i), op.GetTableName(),
                TAt{.Index = ui32(i)});
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropCdcStreamImpl:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpDropCdcStreamAtTable: {
        // schemeshard__operation_drop_cdc_stream.cpp:361,388 resolves the table
        // with a plain Dive(tableName) -- no TSplitChildTag -- and then one
        // tablePath.Child(name) per StreamName entry.
        const auto& op = tx.GetDropCdcStream();
        out.AddAs(F::DropCdcStream_TableName, op.GetTableName(),
            EKind::LeafUnderWorkingDir, ERole::Target);
        for (size_t i = 0; i < op.StreamNameSize(); ++i) {
            out.Sibling(F::DropCdcStream_StreamName, op.GetStreamName(i), op.GetTableName(),
                TAt{.Index = ui32(i)});
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpRotateCdcStream: {
        const auto& op = tx.GetRotateCdcStream();
        out.Add(F::RotateCdcStream_TableName, op.GetTableName());
        out.Sibling(F::RotateCdcStream_OldStreamName, op.GetOldStreamName(), op.GetTableName());
        out.Sibling(F::RotateCdcStream_NewStream_Name,
            op.GetNewStream().GetStreamDescription().GetName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpRotateCdcStreamImpl: {
        const auto& op = tx.GetRotateCdcStream();
        out.AddAs(F::RotateCdcStream_OldStreamName, op.GetOldStreamName(),
            EKind::LeafUnderWorkingDir, ERole::Source);
        out.AddAs(F::RotateCdcStream_NewStream_Name,
            op.GetNewStream().GetStreamDescription().GetName(),
            EKind::LeafUnderWorkingDir, ERole::Target);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpRotateCdcStreamAtTable: {
        // :543 resolves the table with Child(TableName, TSplitChildTag{}); :572
        // and :591 then take plain tablePath.Child() for the old and new stream.
        const auto& op = tx.GetRotateCdcStream();
        out.AddAs(F::RotateCdcStream_TableName, op.GetTableName(),
            EKind::PathUnderWorkingDirSplit, ERole::Target);
        out.Sibling(F::RotateCdcStream_OldStreamName, op.GetOldStreamName(), op.GetTableName());
        out.Sibling(F::RotateCdcStream_NewStream_Name,
            op.GetNewStream().GetStreamDescription().GetName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpMoveTable: {
        out.Add(F::MoveTable_SrcPath, tx.GetMoveTable().GetSrcPath());
        const int moveSrcIndex = out.Last();
        out.Add(F::MoveTable_DstPath, tx.GetMoveTable().GetDstPath());
        // The cascade is enumerated from the children of the *source*.
        out.Implicit(F::Implicit_MoveTable_Children, moveSrcIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpMoveTableIndex: {
        out.Add(F::MoveTableIndex_SrcPath, tx.GetMoveTableIndex().GetSrcPath());
        const int moveTableIndexSrcIndex = out.Last();
        out.Add(F::MoveTableIndex_DstPath, tx.GetMoveTableIndex().GetDstPath());
        // Impl tables and sequences are enumerated from the children of the
        // *source* (schemeshard__operation_move_tables.cpp:110), exactly as for
        // MoveTable and MoveIndex.
        out.Implicit(F::Implicit_MoveTableIndex_Children, moveTableIndexSrcIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpMoveSequence:
        out.Add(F::MoveSequence_SrcPath, tx.GetMoveSequence().GetSrcPath());
        out.Add(F::MoveSequence_DstPath, tx.GetMoveSequence().GetDstPath());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSequence:
    case NKikimrSchemeOp::ESchemeOpAlterSequence:
        // Both resolve WorkingDir/Sequence.Name.
        out.Add(F::Sequence_Name, tx.GetSequence().GetName());
        // A CreateSequence part emitted by CreateConsistentCopyTables carries
        // the source sequence here; TCopySequence::Propose resolves it
        // absolutely (schemeshard__operation_copy_sequence.cpp:579).
        if (tx.HasCopySequence()) {
            out.Add(F::CopySequence_CopyFrom, tx.GetCopySequence().GetCopyFrom());
        }
        break;
    case NKikimrSchemeOp::ESchemeOpDropSequence:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateReplication:
    case NKikimrSchemeOp::ESchemeOpCreateTransfer:
        out.Add(F::Replication_Name, tx.GetReplication().GetName());
        replicationPaths(F::Replication_TransferTarget_DstPath,
            F::Replication_TransferTarget_DirectoryPath,
            F::Replication_SpecificTarget_DstPath,
            F::Replication_AlterTransfer_DirectoryPath,
            tx.GetReplication());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterReplication:
    case NKikimrSchemeOp::ESchemeOpAlterTransfer: {
        const auto& op = tx.GetAlterReplication();
        if (op.HasPathId()) {
            const auto pathId = TPathId::FromProto(op.GetPathId());
            out.ById(F::AlterReplication_PathId, pathId.OwnerId, pathId.LocalPathId);
        } else {
            out.Add(F::AlterReplication_Name, op.GetName());
        }
        replicationPaths(F::AlterReplication_TransferTarget_DstPath,
            F::AlterReplication_TransferTarget_DirectoryPath,
            F::AlterReplication_SpecificTarget_DstPath,
            F::AlterReplication_AlterTransfer_DirectoryPath,
            op);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropReplication:
    case NKikimrSchemeOp::ESchemeOpDropReplicationCascade:
    case NKikimrSchemeOp::ESchemeOpDropTransfer:
    case NKikimrSchemeOp::ESchemeOpDropTransferCascade:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateBlobDepot:
        out.Add(F::BlobDepot_Name, tx.GetBlobDepot().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterBlobDepot:
    case NKikimrSchemeOp::ESchemeOpDropBlobDepot:
        // Propose() for these two is a no-op stub; no TPath is touched.
        break;
    case NKikimrSchemeOp::ESchemeOpMoveIndex: {
        const auto& op = tx.GetMoveIndex();
        out.Add(F::MoveIndex_TablePath, op.GetTablePath());
        out.Sibling(F::MoveIndex_SrcPath, op.GetSrcPath(), op.GetTablePath());
        const int moveIndexSrcIndex = out.Last();
        out.Sibling(F::MoveIndex_DstPath, op.GetDstPath(), op.GetTablePath());
        out.Implicit(F::Implicit_MoveIndex_IndexImplTables, moveIndexSrcIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateExternalTable:
        out.Add(F::CreateExternalTable_Name, tx.GetCreateExternalTable().GetName());
        if (tx.GetCreateExternalTable().HasDataSourcePath()) {
            out.Add(F::CreateExternalTable_DataSourcePath,
                tx.GetCreateExternalTable().GetDataSourcePath());
        }
        break;
    case NKikimrSchemeOp::ESchemeOpDropExternalTable:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterExternalTable:
        out.Add(F::CreateExternalTable_Name, tx.GetCreateExternalTable().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateExternalDataSource:
        out.Add(F::CreateExternalDataSource_Name, tx.GetCreateExternalDataSource().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropExternalDataSource:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterExternalDataSource:
        out.Add(F::CreateExternalDataSource_Name, tx.GetCreateExternalDataSource().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateColumnBuild:
        out.Add(F::InitiateColumnBuild_Table, tx.GetInitiateColumnBuild().GetTable());
        break;
    case NKikimrSchemeOp::ESchemeOpDropColumnBuild:
        out.Add(F::DropColumnBuild_Settings_Table,
            tx.GetDropColumnBuild().GetSettings().GetTable());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateView:
        out.Add(F::CreateView_Name, tx.GetCreateView().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropView:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterView:
        // Unimplemented in the tree; no Propose() exists.
        break;
    case NKikimrSchemeOp::ESchemeOpCreateContinuousBackup: {
        const auto& op = tx.GetCreateContinuousBackup();
        out.Add(F::CreateContinuousBackup_TableName, op.GetTableName());
        const int cbTableIndex = out.Last();
        // NCdc::DoNewStreamPathChecks resolves tablePath.Child(streamName)
        // (schemeshard__operation_create_continuous_backup.cpp:35). When the
        // field is absent the name is generated from the current time, so
        // there is nothing to report and the Implicit marker below stands in.
        if (op.GetContinuousBackupDescription().HasStreamName()) {
            out.SiblingOf(F::CreateContinuousBackup_StreamName,
                op.GetContinuousBackupDescription().GetStreamName(), cbTableIndex);
        }
        out.Implicit(F::Implicit_CreateContinuousBackup_CdcStream, cbTableIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterContinuousBackup: {
        const auto& op = tx.GetAlterContinuousBackup();
        // :86 resolves the table with Child(TableName, TSplitChildTag{}), so a
        // leading slash does not escape the working dir.
        out.Add(F::AlterContinuousBackup_TableName, op.GetTableName());
        const int cbTableIndex = out.Last();
        if (op.HasTakeIncrementalBackup()) {
            const auto& take = op.GetTakeIncrementalBackup();
            // :128 workingDirPath.Child(DstPath, TSplitChildTag{}).
            out.Add(F::AlterContinuousBackup_TakeIncrementalBackup_DstPath, take.GetDstPath());
            if (take.HasDstStreamPath()) {
                // :161 the new stream is a leaf under the table. When the field
                // is absent the name is generated from the current time, so
                // there is nothing to report.
                out.SiblingOf(F::AlterContinuousBackup_TakeIncrementalBackup_DstStreamPath,
                    take.GetDstStreamPath(), cbTableIndex);
            }
        }
        out.Implicit(F::Implicit_AlterContinuousBackup_IncrementalBackupTable, cbTableIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropContinuousBackup:
        out.Add(F::DropContinuousBackup_TableName, tx.GetDropContinuousBackup().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateResourcePool:
    case NKikimrSchemeOp::ESchemeOpAlterResourcePool:
        out.Add(F::CreateResourcePool_Name, tx.GetCreateResourcePool().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropResourcePool:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups:
    case NKikimrSchemeOp::ESchemeOpRestoreIncrementalBackupAtTable: {
        // Retired: the factory always rejects. Kept for completeness.
        const auto& op = tx.GetRestoreMultipleIncrementalBackups();
        for (size_t i = 0; i < op.SrcTablePathsSize(); ++i) {
            out.Add(F::RestoreMultipleIncrementalBackups_SrcTablePaths,
                op.GetSrcTablePaths(i), TAt{.Index = ui32(i)});
        }
        out.Add(F::RestoreMultipleIncrementalBackups_DstTablePath, op.GetDstTablePath());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateBackupCollection: {
        const auto& op = tx.GetCreateBackupCollection();
        out.Add(F::CreateBackupCollection_Name, op.GetName());
        // Propose() -> RegisterBackupCollectionTables() resolves every entry
        // with TPath::Resolve(entry.GetPath()) — absolute, no WorkingDir join
        // (schemeshard_impl.cpp:3920).
        const auto& entryList = op.GetExplicitEntryList();
        for (size_t i = 0; i < entryList.EntriesSize(); ++i) {
            out.Add(F::CreateBackupCollection_Entry_Path, entryList.GetEntries(i).GetPath(),
                TAt{.Index = ui32(i)});
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterBackupCollection:
        out.Add(F::AlterBackupCollection_Name, tx.GetAlterBackupCollection().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropBackupCollection:
        out.Add(F::DropBackupCollection_Name, tx.GetDropBackupCollection().GetName());
        out.Implicit(F::Implicit_DropBackupCollection_Entries, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpBackupBackupCollection:
        out.Add(F::BackupBackupCollection_Name, tx.GetBackupBackupCollection().GetName());
        out.Implicit(F::Implicit_BackupBackupCollection_Entries, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpBackupIncrementalBackupCollection:
    case NKikimrSchemeOp::ESchemeOpCreateLongIncrementalBackupOp:
        out.Add(F::BackupIncrementalBackupCollection_Name,
            tx.GetBackupIncrementalBackupCollection().GetName());
        out.Implicit(F::Implicit_BackupIncrementalBackupCollection_Entries, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateFullBackupOp:
        // WorkingDir already points at the backup collection; no name field.
        out.Add(F::WorkingDirItself, {});
        out.Implicit(F::Implicit_CreateFullBackupOp_Entries, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpRestoreBackupCollection:
    case NKikimrSchemeOp::ESchemeOpCreateLongIncrementalRestoreOp:
        out.Add(F::RestoreBackupCollection_Name, tx.GetRestoreBackupCollection().GetName());
        out.Implicit(F::Implicit_RestoreBackupCollection_Entries, out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSysView:
        out.Add(F::CreateSysView_Name, tx.GetCreateSysView().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSysView:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpChangePathState:
        out.Add(F::ChangePathState_Path, tx.GetChangePathState().GetPath());
        break;
    case NKikimrSchemeOp::ESchemeOpIncrementalRestoreLockTargets:
    case NKikimrSchemeOp::ESchemeOpIncrementalRestoreUnlockTargets: {
        const auto& op = tx.GetIncrementalRestoreLockTargets();
        for (size_t i = 0; i < op.DstPathsSize(); ++i) {
            out.Add(F::IncrementalRestoreLockTargets_DstPaths, op.GetDstPaths(i),
                TAt{.Index = ui32(i)});
        }
        for (size_t i = 0; i < op.SrcPathsSize(); ++i) {
            out.Add(F::IncrementalRestoreLockTargets_SrcPaths, op.GetSrcPaths(i),
                TAt{.Index = ui32(i)});
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpIncrementalRestoreFinalize:
        out.Implicit(F::Implicit_IncrementalRestoreFinalize_PersistedState, -1);
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSecret:
        out.Add(F::CreateSecret_Name, tx.GetCreateSecret().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterSecret:
        out.Add(F::AlterSecret_Name, tx.GetAlterSecret().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSecret:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateStreamingQuery:
    case NKikimrSchemeOp::ESchemeOpAlterStreamingQuery:
        out.Add(F::CreateStreamingQuery_Name, tx.GetCreateStreamingQuery().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropStreamingQuery:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpTruncateTable:
        out.Add(F::TruncateTable_TableName, tx.GetTruncateTable().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateTestShardSet:
        out.Add(F::CreateTestShardSet_Name, tx.GetCreateTestShardSet().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropTestShardSet:
        genericDrop();
        break;
    }

    return result;
}

namespace {

// NKikimr::JoinPath always inserts the separator, so joining an empty leaf
// yields a trailing slash. An empty leaf here means "the directory itself":
// that is what a PathUnderWorkingDir/Absolute ref with no value stands for
// (CreateFullBackupOp's working dir), and TPath::Child would not add a segment
// for it either.
TString JoinLeafUnder(TStringBuf dir, TStringBuf leaf) {
    if (leaf.empty()) {
        return TString(dir);
    }
    if (dir.empty()) {
        return TString(leaf);
    }
    return TStringBuilder() << dir << '/' << leaf;
}

// The string form of ResolveRelativeOrAbsolute: a base that starts with a
// slash is already absolute, anything else hangs off the working dir.
TString JoinRelativeOrAbsolute(TStringBuf workingDir, TStringBuf value) {
    if (value.StartsWith('/')) {
        return TString(value);
    }
    return JoinLeafUnder(workingDir, value);
}

}  // namespace

TString JoinPathRef(TStringBuf workingDir, const TPathRef& ref, const TVector<TString>& joined) {
    switch (ref.Kind) {
    case EPathRefKind::LeafUnderWorkingDir:
        return JoinLeafUnder(workingDir, ref.Value);
    case EPathRefKind::PathUnderWorkingDir:
        return JoinRelativeOrAbsolute(workingDir, ref.Value);
    case EPathRefKind::PathUnderWorkingDirSplit:
        // TPath::Child(value, TSplitChildTag{}) dives the value one segment at
        // a time under the working dir, so a leading slash does not escape it.
        return JoinLeafUnder(workingDir,
            ref.Value.StartsWith('/') ? ref.Value.substr(1) : ref.Value);
    case EPathRefKind::Absolute:
        // Propose() resolves these on their own; the working dir is never
        // joined in. An empty value stands for the working dir itself.
        return ref.Value.empty() ? TString(workingDir) : TString(ref.Value);
    case EPathRefKind::LeafUnderSibling: {
        const TString base = ref.BasePath.empty() && ref.AnchorIndex >= 0
                && size_t(ref.AnchorIndex) < joined.size()
            ? joined[ref.AnchorIndex]
            : JoinRelativeOrAbsolute(workingDir, ref.BasePath);
        return base.empty() ? TString() : JoinLeafUnder(base, ref.Value);
    }
    case EPathRefKind::ById:
    case EPathRefKind::Implicit:
        // A path id and a runtime-derived set both need schemeshard state.
        return TString();
    }
    return TString();
}

namespace {

// Mirrors what Propose() does for a relative-or-absolute path field.
TPath ResolveRelativeOrAbsolute(TSchemeShard* ss, const TString& workingDir, const TString& value) {
    if (value.StartsWith('/')) {
        return TPath::Resolve(value, ss);
    }
    return TPath::Resolve(JoinPath({workingDir, value}), ss);
}

TString StripPrefix(const TString& abs, const TString& prefix) {
    if (prefix.empty() || prefix == "/") {
        return abs.StartsWith('/') ? abs.substr(1) : abs;
    }
    if (abs == prefix) {
        return TString();
    }
    if (abs.StartsWith(prefix) && abs.size() > prefix.size() && abs[prefix.size()] == '/') {
        return abs.substr(prefix.size() + 1);
    }
    return abs;
}

// The ref, with everything a footprint keeps copied out of the request proto.
TPathRefOwned Materialize(const TPathRef& ref) {
    TPathRefOwned owned;
    owned.Field = ref.Field;
    owned.FieldPath = FieldPath(ref);
    owned.Index = ref.Index;
    owned.Value = TString(ref.Value);
    owned.OwnerId = ref.OwnerId;
    owned.LocalPathId = ref.LocalPathId;
    owned.Kind = ref.Kind;
    owned.Role = ref.Role;
    owned.BasePath = TString(ref.BasePath);
    owned.AnchorIndex = ref.AnchorIndex;
    return owned;
}

}  // namespace

namespace {

// The parts whose Propose() resolves its destination with
// TPath::ResolveWithInactive rather than with a plain TPath::Resolve:
// schemeshard__operation_move_table.cpp, schemeshard__operation_move_sequence.cpp
// and index/operation_move_table_index.cpp. ESchemeOpMoveIndex is not one of
// them — it is a top-level op that expands into MoveTableIndex parts, and it
// carries its paths as LeafUnderSibling, not Absolute.
bool ResolvesTargetWithInactive(NKikimrSchemeOp::EOperationType type) {
    switch (type) {
    case NKikimrSchemeOp::ESchemeOpMoveTable:
    case NKikimrSchemeOp::ESchemeOpMoveTableIndex:
    case NKikimrSchemeOp::ESchemeOpMoveSequence:
        return true;
    default:
        return false;
    }
}

}  // namespace

TPathFootprint ResolvePathFootprint(const NKikimrSchemeOp::TModifyScheme& tx, TSchemeShard* ss,
        TOperationId opId) {
    TPathFootprint footprint;
    footprint.WorkingDir = tx.GetWorkingDir();
    footprint.PartOpType = tx.GetOperationType();
    // ResolveWithInactive needs a live sub-operation to walk back from, so it
    // is only reachable from the part-level hook.
    const bool inactiveAwareTarget = bool(opId) && ResolvesTargetWithInactive(footprint.PartOpType);

    // Resolved once per footprint and reused by every entry below.
    const TPath workingDirPath = TPath::Resolve(footprint.WorkingDir, ss);
    // The canonized working dir, which is what the entries' AbsPath is built
    // from. Stripping against tx.GetWorkingDir() would silently fail whenever
    // the raw proto string is not already canonical.
    const TString workingDirCanon = workingDirPath.PathString();
    footprint.WorkingDirCanon = workingDirCanon;

    TString dbPath;
    {
        const TPath existing = workingDirPath.FirstExistedParent();
        if (existing.IsResolved()) {
            footprint.DatabasePathId = existing.GetPathIdForDomain();
            dbPath = existing.GetDomainPathString();
        }
        footprint.WorkingDirRelToDb = StripPrefix(workingDirCanon, dbPath);
    }

    for (const auto& rawRef : ExtractPathRefs(tx)) {
        TPathFootprintEntry entry;
        // Everything below reads the owned copy: the raw ref only points into
        // tx, which does not outlive the footprint.
        entry.Ref = Materialize(rawRef);
        const TPathRefOwned& ref = entry.Ref;

        if (ref.Kind == EPathRefKind::Implicit) {
            // The touched set is enumerated at Propose/Execute time from the
            // children of the anchor, so report the anchor's resolved path.
            // Exists stays false: the entry stands for paths, not one path.
            if (ref.AnchorIndex >= 0 && size_t(ref.AnchorIndex) < footprint.Entries.size()) {
                const auto& anchor = footprint.Entries[ref.AnchorIndex];
                entry.AbsPath = anchor.AbsPath;
                entry.PathId = anchor.PathId;
                entry.ParentPathId = anchor.ParentPathId;
                entry.DatabasePathId = anchor.DatabasePathId;
                entry.RelPathToParent = anchor.RelPathToParent;
                entry.RelPathToDatabase = anchor.RelPathToDatabase;
                entry.RelPathToWorkingDir = anchor.RelPathToWorkingDir;
            }
            footprint.Entries.push_back(std::move(entry));
            continue;
        }

        TPath path(ss);
        switch (ref.Kind) {
        case EPathRefKind::LeafUnderWorkingDir:
            path = workingDirPath.Child(ref.Value);
            break;
        case EPathRefKind::PathUnderWorkingDirSplit:
            // TPath::Child(value, TSplitChildTag{}). Stays under the working
            // dir even if the value happens to start with a slash, which is
            // what makes this different from PathUnderWorkingDir.
            path = workingDirPath.Child(ref.Value, TPath::TSplitChildTag{});
            break;
        case EPathRefKind::PathUnderWorkingDir:
            if (ref.Value.empty()) {
                path = TPath(workingDirPath);
            } else {
                path = ResolveRelativeOrAbsolute(ss, footprint.WorkingDir, ref.Value);
            }
            break;
        case EPathRefKind::Absolute:
            // Propose() resolves these fields on their own, so WorkingDir is
            // never joined in — not even when the value has no leading slash.
            if (ref.Value.empty()) {
                path = TPath(workingDirPath);
            } else if (inactiveAwareTarget && ref.Role == EPathRefRole::Target) {
                path = TPath::ResolveWithInactive(opId, ref.Value, ss);
            } else {
                path = TPath::Resolve(ref.Value, ss);
            }
            break;
        case EPathRefKind::LeafUnderSibling:
            if (ref.BasePath.empty() && ref.AnchorIndex >= 0
                    && size_t(ref.AnchorIndex) < footprint.Entries.size()) {
                // The base is another entry of this same footprint, used when
                // it cannot be written as a raw string (by-id addressing, or a
                // split child). An unresolvable base leaves the path empty.
                const TString& base = footprint.Entries[ref.AnchorIndex].AbsPath;
                if (!base.empty()) {
                    path = TPath::Resolve(base, ss).Child(ref.Value);
                }
            } else {
                path = ResolveRelativeOrAbsolute(ss, footprint.WorkingDir, ref.BasePath).Child(ref.Value);
            }
            break;
        case EPathRefKind::ById: {
            const TPathId pathId = ref.OwnerId
                ? TPathId(TOwnerId(ref.OwnerId), TLocalPathId(ref.LocalPathId))
                : ss->MakeLocalId(TLocalPathId(ref.LocalPathId));
            path = TPath::Init(pathId, ss);
            break;
        }
        case EPathRefKind::Implicit:
            break;
        }

        entry.AbsPath = path.IsEmpty() ? TString() : path.PathString();
        entry.RelPathToParent = path.IsEmpty() ? TString() : path.LeafName();
        entry.Exists = path.IsResolved() && !path.IsDeleted();
        if (path.IsResolved()) {
            entry.PathId = path.Base()->PathId;
        }

        TPath ancestor = path.FirstExistedParent();
        if (ancestor.IsResolved()) {
            entry.ParentPathId = ancestor.Base()->PathId;
            entry.DatabasePathId = ancestor.GetPathIdForDomain();
            // Almost always the working dir's own domain, whose path string was
            // already built above; only walk again when it genuinely differs.
            entry.RelPathToDatabase = StripPrefix(entry.AbsPath,
                entry.DatabasePathId == footprint.DatabasePathId
                    ? dbPath
                    : ancestor.GetDomainPathString());
        } else {
            entry.RelPathToDatabase = entry.AbsPath;
        }
        if (entry.Exists) {
            entry.ParentPathId = path.Parent().IsResolved()
                ? path.Parent().Base()->PathId
                : entry.ParentPathId;
        }

        entry.RelPathToWorkingDir = entry.AbsPath.empty()
            ? entry.Ref.Value
            : StripPrefix(entry.AbsPath, workingDirCanon);

        footprint.Entries.push_back(std::move(entry));
    }

    return footprint;
}

void TPathReadSetRecorder::OnPathResolved(const TPath& path, bool byPathId) {
    TPathRead read;
    read.AbsPath = path.PathString();
    read.Resolved = path.IsResolved();
    read.ByPathId = byPathId;
    if (read.Resolved) {
        read.PathId = path.Base()->PathId;
    }
    // One TPath::Resolve is a chain of Dive calls, each one segment longer than
    // the last. Keep only the longest: the shorter prefixes carry no
    // information the longest one does not.
    if (!Sink.empty() && !byPathId && !Sink.back().ByPathId
            && read.AbsPath.size() > Sink.back().AbsPath.size()
            && read.AbsPath.StartsWith(Sink.back().AbsPath))
    {
        Sink.back() = std::move(read);
        return;
    }
    Sink.push_back(std::move(read));
}

////////////////////////////////////////////////////////////////////////////////
// Layer 3: rewriting a request.

namespace {

bool IsRootPath(TStringBuf path) {
    return path.empty() || path == "/";
}

// Is `abs` the path `prefix`, or something under it? Both must be canonical,
// which every AbsPath and WorkingDirCanon in a footprint is.
bool IsAtOrUnder(TStringBuf abs, TStringBuf prefix) {
    if (abs.empty()) {
        return false;
    }
    if (IsRootPath(prefix)) {
        return abs.StartsWith('/');
    }
    if (abs == prefix) {
        return true;
    }
    return abs.size() > prefix.size() && abs.StartsWith(prefix) && abs[prefix.size()] == '/';
}

// `abs` with `prefix` removed. Empty when the two are the same path. Only
// meaningful when IsAtOrUnder(abs, prefix).
TStringBuf RelativeTo(TStringBuf abs, TStringBuf prefix) {
    if (IsRootPath(prefix)) {
        return abs.StartsWith('/') ? abs.substr(1) : abs;
    }
    if (abs == prefix) {
        return TStringBuf();
    }
    return abs.substr(prefix.size() + 1);
}

TString JoinUnder(TStringBuf base, TStringBuf rel) {
    if (rel.empty()) {
        return TString(base);
    }
    if (IsRootPath(base)) {
        return TStringBuilder() << '/' << rel;
    }
    return TStringBuilder() << base << '/' << rel;
}

// Split a canonical absolute path into the directory it lives in and its leaf
// name. `leafHint` is the entry's RelPathToParent, which TPath::LeafName()
// produced from the same path; the suffix scan is the fallback for an entry
// that has no hint.
bool SplitParentLeaf(TStringBuf abs, TStringBuf leafHint, TString& parent, TString& leaf) {
    if (abs.empty()) {
        return false;
    }
    if (!leafHint.empty() && abs.size() > leafHint.size() + 1 && abs.EndsWith(leafHint)
            && abs[abs.size() - leafHint.size() - 1] == '/') {
        parent = TString(abs.substr(0, abs.size() - leafHint.size() - 1));
        leaf = TString(leafHint);
        return true;
    }
    const size_t slash = abs.rfind('/');
    if (slash == TStringBuf::npos || slash + 1 >= abs.size()) {
        return false;
    }
    parent = slash == 0 ? TString("/") : TString(abs.substr(0, slash));
    leaf = TString(abs.substr(slash + 1));
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// The setter table: how to write a new value into each field that can carry a
// relocatable path.
//
// One row per EPathField whose kind is Absolute or PathUnderWorkingDir, in the
// table's own order, plus AlterTable.Columns[].DefaultFromSequence, which is
// Absolute only when its value starts with a slash. Kept next to the field
// table rather than inside it: a row here needs protobuf accessors, which the
// header deliberately does not reach for.
//
// A row is a statement, not an expression, so an indexed field can bail out on
// a footprint whose indexes no longer match the request.

#define SS_PATH_SETTER_GUARD_INDEX(size)                                                       \
    if (static_cast<i64>(index) >= static_cast<i64>(size)) {                                   \
        return false;                                                                          \
    }

#define SCHEMESHARD_PATH_FIELD_SETTERS(X)                                                      \
    X(CreateTable_CopyFromTable,                                                               \
        tx.MutableCreateTable()->SetCopyFromTable(value);)                                     \
    X(AlterTable_Column_DefaultFromSequence,                                                   \
        SS_PATH_SETTER_GUARD_INDEX(tx.GetAlterTable().ColumnsSize())                           \
        tx.MutableAlterTable()->MutableColumns(index)->SetDefaultFromSequence(value);)          \
    X(AlterPersQueueGroup_IncrementalBackup_DstPath,                                           \
        tx.MutableAlterPersQueueGroup()->MutablePQTabletConfig()->MutableOffloadConfig()        \
            ->MutableIncrementalBackup()->SetDstPath(value);)                                   \
    X(SplitMergeTablePartitions_TablePath,                                                     \
        tx.MutableSplitMergeTablePartitions()->SetTablePath(value);)                            \
    X(AlterUserAttributes_PathName,                                                            \
        tx.MutableAlterUserAttributes()->SetPathName(value);)                                   \
    X(CopyTables_Item_SrcPath,                                                                 \
        SS_PATH_SETTER_GUARD_INDEX(tx.GetCreateConsistentCopyTables().CopyTableDescriptionsSize()) \
        tx.MutableCreateConsistentCopyTables()->MutableCopyTableDescriptions(index)             \
            ->SetSrcPath(value);)                                                              \
    X(CopyTables_Item_DstPath,                                                                 \
        SS_PATH_SETTER_GUARD_INDEX(tx.GetCreateConsistentCopyTables().CopyTableDescriptionsSize()) \
        tx.MutableCreateConsistentCopyTables()->MutableCopyTableDescriptions(index)             \
            ->SetDstPath(value);)                                                              \
    X(InitiateIndexBuild_Table,                                                                \
        tx.MutableInitiateIndexBuild()->SetTable(value);)                                       \
    X(ApplyIndexBuild_TablePath,                                                               \
        tx.MutableApplyIndexBuild()->SetTablePath(value);)                                      \
    X(DropIndex_TableName,                                                                     \
        tx.MutableDropIndex()->SetTableName(value);)                                            \
    X(CancelIndexBuild_TablePath,                                                              \
        tx.MutableCancelIndexBuild()->SetTablePath(value);)                                     \
    X(CreateColumnTable_CopyFromTable,                                                         \
        tx.MutableCreateColumnTable()->SetCopyFromTable(value);)                                \
    X(CreateColumnTable_TierStorage,                                                           \
        SS_PATH_SETTER_GUARD_INDEX(                                                            \
            tx.GetCreateColumnTable().GetTtlSettings().GetEnabled().TiersSize())               \
        tx.MutableCreateColumnTable()->MutableTtlSettings()->MutableEnabled()                   \
            ->MutableTiers(index)->MutableEvictToExternalStorage()->SetStorage(value);)         \
    X(AlterColumnTable_TierStorage,                                                            \
        SS_PATH_SETTER_GUARD_INDEX(                                                            \
            tx.GetAlterColumnTable().GetAlterTtlSettings().GetEnabled().TiersSize())           \
        tx.MutableAlterColumnTable()->MutableAlterTtlSettings()->MutableEnabled()               \
            ->MutableTiers(index)->MutableEvictToExternalStorage()->SetStorage(value);)         \
    X(CreateBackupCollection_Name,                                                             \
        tx.MutableCreateBackupCollection()->SetName(value);)                                    \
    X(DropBackupCollection_Name,                                                               \
        tx.MutableDropBackupCollection()->SetName(value);)                                      \
    X(CreateCdcStream_TableName,                                                               \
        tx.MutableCreateCdcStream()->SetTableName(value);)                                      \
    X(AlterCdcStream_TableName,                                                                \
        tx.MutableAlterCdcStream()->SetTableName(value);)                                       \
    X(DropCdcStream_TableName,                                                                 \
        tx.MutableDropCdcStream()->SetTableName(value);)                                        \
    X(RotateCdcStream_TableName,                                                               \
        tx.MutableRotateCdcStream()->SetTableName(value);)                                      \
    X(MoveTable_SrcPath, tx.MutableMoveTable()->SetSrcPath(value);)                             \
    X(MoveTable_DstPath, tx.MutableMoveTable()->SetDstPath(value);)                             \
    X(MoveTableIndex_SrcPath, tx.MutableMoveTableIndex()->SetSrcPath(value);)                   \
    X(MoveTableIndex_DstPath, tx.MutableMoveTableIndex()->SetDstPath(value);)                   \
    X(MoveSequence_SrcPath, tx.MutableMoveSequence()->SetSrcPath(value);)                       \
    X(MoveSequence_DstPath, tx.MutableMoveSequence()->SetDstPath(value);)                       \
    X(CopySequence_CopyFrom, tx.MutableCopySequence()->SetCopyFrom(value);)                     \
    X(Replication_TransferTarget_DstPath,                                                      \
        tx.MutableReplication()->MutableConfig()->MutableTransferSpecific()->MutableTarget()    \
            ->SetDstPath(value);)                                                              \
    X(Replication_TransferTarget_DirectoryPath,                                                \
        tx.MutableReplication()->MutableConfig()->MutableTransferSpecific()->MutableTarget()    \
            ->SetDirectoryPath(value);)                                                        \
    X(Replication_SpecificTarget_DstPath,                                                      \
        SS_PATH_SETTER_GUARD_INDEX(tx.GetReplication().GetConfig().GetSpecific().TargetsSize()) \
        tx.MutableReplication()->MutableConfig()->MutableSpecific()->MutableTargets(index)      \
            ->SetDstPath(value);)                                                              \
    X(Replication_AlterTransfer_DirectoryPath,                                                 \
        tx.MutableReplication()->MutableAlterTransfer()->SetDirectoryPath(value);)              \
    X(AlterReplication_TransferTarget_DstPath,                                                 \
        tx.MutableAlterReplication()->MutableConfig()->MutableTransferSpecific()               \
            ->MutableTarget()->SetDstPath(value);)                                             \
    X(AlterReplication_TransferTarget_DirectoryPath,                                           \
        tx.MutableAlterReplication()->MutableConfig()->MutableTransferSpecific()               \
            ->MutableTarget()->SetDirectoryPath(value);)                                       \
    X(AlterReplication_SpecificTarget_DstPath,                                                 \
        SS_PATH_SETTER_GUARD_INDEX(                                                            \
            tx.GetAlterReplication().GetConfig().GetSpecific().TargetsSize())                  \
        tx.MutableAlterReplication()->MutableConfig()->MutableSpecific()->MutableTargets(index) \
            ->SetDstPath(value);)                                                              \
    X(AlterReplication_AlterTransfer_DirectoryPath,                                            \
        tx.MutableAlterReplication()->MutableAlterTransfer()->SetDirectoryPath(value);)         \
    X(MoveIndex_TablePath, tx.MutableMoveIndex()->SetTablePath(value);)                         \
    X(CreateExternalTable_DataSourcePath,                                                      \
        tx.MutableCreateExternalTable()->SetDataSourcePath(value);)                             \
    X(InitiateColumnBuild_Table, tx.MutableInitiateColumnBuild()->SetTable(value);)             \
    X(DropColumnBuild_Settings_Table,                                                          \
        tx.MutableDropColumnBuild()->MutableSettings()->SetTable(value);)                       \
    X(RestoreMultipleIncrementalBackups_SrcTablePaths,                                         \
        SS_PATH_SETTER_GUARD_INDEX(                                                            \
            tx.GetRestoreMultipleIncrementalBackups().SrcTablePathsSize())                     \
        tx.MutableRestoreMultipleIncrementalBackups()->SetSrcTablePaths(index, value);)         \
    X(RestoreMultipleIncrementalBackups_DstTablePath,                                          \
        tx.MutableRestoreMultipleIncrementalBackups()->SetDstTablePath(value);)                 \
    X(CreateBackupCollection_Entry_Path,                                                       \
        SS_PATH_SETTER_GUARD_INDEX(                                                            \
            tx.GetCreateBackupCollection().GetExplicitEntryList().EntriesSize())               \
        tx.MutableCreateBackupCollection()->MutableExplicitEntryList()                          \
            ->MutableEntries(index)->SetPath(value);)                                          \
    X(ChangePathState_Path, tx.MutableChangePathState()->SetPath(value);)                       \
    X(IncrementalRestoreLockTargets_DstPaths,                                                  \
        SS_PATH_SETTER_GUARD_INDEX(tx.GetIncrementalRestoreLockTargets().DstPathsSize())       \
        tx.MutableIncrementalRestoreLockTargets()->SetDstPaths(index, value);)                  \
    X(IncrementalRestoreLockTargets_SrcPaths,                                                  \
        SS_PATH_SETTER_GUARD_INDEX(tx.GetIncrementalRestoreLockTargets().SrcPathsSize())       \
        tx.MutableIncrementalRestoreLockTargets()->SetSrcPaths(index, value);)                  \
    X(TruncateTable_TableName, tx.MutableTruncateTable()->SetTableName(value);)

// Writes `value` into the one protobuf field `field` names. False when the
// field has no setter (it can never carry a relocatable path) or when the
// ref's index is out of range for this request.
bool SetPathField(NKikimrSchemeOp::TModifyScheme& tx, EPathField field, ui32 index,
        const TString& value) {
    switch (field) {
#define SCHEMESHARD_PATH_FIELD_SETTER_CASE(name, stmt)                                         \
    case EPathField::name:                                                                     \
        stmt                                                                                   \
        return true;
        SCHEMESHARD_PATH_FIELD_SETTERS(SCHEMESHARD_PATH_FIELD_SETTER_CASE)
#undef SCHEMESHARD_PATH_FIELD_SETTER_CASE
    default:
        return false;
    }
}

// The database an entry lives in, recovered from the entry itself: its AbsPath
// with RelPathToDatabase removed. Empty when the two do not line up, which is
// what a footprint records for a path whose database did not resolve.
TString DatabasePathOfEntry(const TPathFootprintEntry& entry) {
    const TStringBuf abs = entry.AbsPath;
    const TStringBuf rel = entry.RelPathToDatabase;
    if (abs.empty()) {
        return TString();
    }
    if (rel.empty()) {
        return TString(abs);
    }
    if (abs.size() > rel.size() && abs.EndsWith(rel) && abs[abs.size() - rel.size() - 1] == '/') {
        const size_t cut = abs.size() - rel.size() - 1;
        return cut == 0 ? TString("/") : TString(abs.substr(0, cut));
    }
    return TString();
}

// Point the footprint at the working dir canonicalization has just written.
// Both forms are set to the same string: it was cut out of an AbsPath, which
// is canonical already.
void MoveFootprintWorkingDir(TPathFootprint& fp, const TString& workingDir,
        const TString& databasePath) {
    fp.WorkingDir = workingDir;
    fp.WorkingDirCanon = workingDir;
    fp.WorkingDirRelToDb = StripPrefix(workingDir, databasePath);
    for (auto& entry : fp.Entries) {
        if (!entry.AbsPath.empty()) {
            entry.RelPathToWorkingDir = StripPrefix(entry.AbsPath, workingDir);
        }
    }
}

// The entry now describes a name-addressed field. Role is unchanged: every
// by-id field this rewrites is a Target, and so is the name form it becomes.
void RetargetEntry(TPathFootprintEntry& entry, EPathField field, EPathRefKind kind,
        const TString& value) {
    entry.Ref.Field = field;
    // None of the name forms carries a placeholder, so the template is the
    // rendered field path.
    entry.Ref.FieldPath = TString(PathFieldName(field));
    entry.Ref.Kind = kind;
    entry.Ref.Value = value;
    entry.Ref.OwnerId = 0;
    entry.Ref.LocalPathId = 0;
}

// Rewrite one by-id field into its name form, in the request and in the
// footprint entry that described it. False when the entry does not name a
// by-id field this knows about, or when its resolved path cannot be split into
// a working dir and a leaf.
bool CanonicalizeEntry(NKikimrSchemeOp::TModifyScheme& tx, TPathFootprint& fp,
        size_t entryIndex) {
    TPathFootprintEntry& entry = fp.Entries[entryIndex];
    const TString abs = entry.AbsPath;

    // SplitMerge is the one case that keeps an absolute path rather than a
    // WorkingDir plus a leaf: Propose() calls TPath::Resolve(TablePath) with
    // no WorkingDir join (schemeshard__operation_split_merge.cpp:849).
    if (entry.Ref.Field == EPathField::SplitMergeTablePartitions_TableLocalId) {
        auto& info = *tx.MutableSplitMergeTablePartitions();
        info.SetTablePath(abs);
        info.ClearTableOwnerId();
        info.ClearTableLocalId();
        RetargetEntry(entry, EPathField::SplitMergeTablePartitions_TablePath,
            EPathRefKind::Absolute, abs);
        return true;
    }

    TString parent;
    TString leaf;
    if (!SplitParentLeaf(abs, entry.RelPathToParent, parent, leaf)) {
        return false;
    }

    EPathField nameField = EPathField::Count;
    switch (entry.Ref.Field) {
    case EPathField::Drop_Id:
        tx.MutableDrop()->SetName(leaf);
        tx.MutableDrop()->ClearId();
        nameField = EPathField::Drop_Name;
        break;
    case EPathField::AlterTable_PathId:
    case EPathField::AlterTable_Id_Deprecated:
        tx.MutableAlterTable()->SetName(leaf);
        // Both id forms have to go: alter_table.cpp:607 takes either one over
        // the name.
        tx.MutableAlterTable()->ClearPathId();
        tx.MutableAlterTable()->ClearId_Deprecated();
        nameField = EPathField::AlterTable_Name;
        break;
    case EPathField::AlterPersQueueGroup_PathId:
        tx.MutableAlterPersQueueGroup()->SetName(leaf);
        tx.MutableAlterPersQueueGroup()->ClearPathId();
        nameField = EPathField::AlterPersQueueGroup_Name;
        break;
    case EPathField::AlterBlockStoreVolume_PathId:
        tx.MutableAlterBlockStoreVolume()->SetName(leaf);
        tx.MutableAlterBlockStoreVolume()->ClearPathId();
        nameField = EPathField::AlterBlockStoreVolume_Name;
        break;
    case EPathField::AlterReplication_PathId:
        // Transfer has no submessage of its own: an alter-transfer request is
        // this same TAlterReplication read by a different strategy
        // (schemeshard__operation_alter_replication.cpp:32-69,365).
        tx.MutableAlterReplication()->SetName(leaf);
        tx.MutableAlterReplication()->ClearPathId();
        nameField = EPathField::AlterReplication_Name;
        break;
    default:
        return false;
    }

    tx.SetWorkingDir(parent);
    RetargetEntry(entry, nameField, EPathRefKind::LeafUnderWorkingDir, leaf);
    // The database of the path just named, which is the database the new
    // working dir lives in; the old one was derived from a working dir the
    // request may not even have carried.
    fp.DatabasePathId = entry.DatabasePathId;
    MoveFootprintWorkingDir(fp, parent, DatabasePathOfEntry(entry));
    return true;
}

}  // namespace

bool CanRelocatePathField(EPathField field) {
    switch (field) {
#define SCHEMESHARD_PATH_FIELD_SETTER_PRESENT(name, stmt) case EPathField::name:
        SCHEMESHARD_PATH_FIELD_SETTERS(SCHEMESHARD_PATH_FIELD_SETTER_PRESENT)
#undef SCHEMESHARD_PATH_FIELD_SETTER_PRESENT
        return true;
    default:
        return false;
    }
}

TCanonicalizeResult CanonicalizeToPaths(NKikimrSchemeOp::TModifyScheme& tx, TPathFootprint& fp) {
    TCanonicalizeResult result;

    for (size_t i = 0; i < fp.Entries.size(); ++i) {
        if (fp.Entries[i].Ref.Kind != EPathRefKind::ById) {
            continue;
        }
        // An id nothing resolved to: the operation would be rejected on this
        // schemeshard anyway, and guessing a name would invent a target.
        if (fp.Entries[i].AbsPath.empty() || !CanonicalizeEntry(tx, fp, i)) {
            result.Untransformable.push_back(fp.Entries[i].Ref.Field);
            continue;
        }
        result.Changed = true;
    }

    return result;
}

TRelocateResult RelocatePaths(NKikimrSchemeOp::TModifyScheme& tx, const TPathFootprint& fp,
        const TRelocation& r) {
    TRelocateResult result;

    for (const auto& entry : fp.Entries) {
        switch (entry.Ref.Kind) {
        case EPathRefKind::ById:
            // Canonicalization has to run first: a path id says nothing about
            // where the path lives in the new database.
            result.Skipped.push_back(entry.Ref.Field);
            continue;
        case EPathRefKind::LeafUnderWorkingDir:
        case EPathRefKind::PathUnderWorkingDirSplit:
        case EPathRefKind::LeafUnderSibling:
        case EPathRefKind::Implicit:
            // All of these hang off something else -- the working dir or a
            // base field -- which is rewritten on its own. A split child stays
            // under the working dir even when it starts with a slash.
            continue;
        case EPathRefKind::PathUnderWorkingDir:
            // Relative here means "under the working dir", which moves with it.
            if (!entry.Ref.Value.StartsWith('/')) {
                continue;
            }
            break;
        case EPathRefKind::Absolute:
            break;
        }

        if (entry.Ref.Value.empty() || !IsAtOrUnder(entry.AbsPath, r.OldDatabasePath)) {
            // Outside the database being moved, or a synthetic entry with no
            // field behind it (the CreateFullBackupOp working dir).
            continue;
        }

        const TString relocated =
            JoinUnder(r.NewDatabasePath, RelativeTo(entry.AbsPath, r.OldDatabasePath));
        if (SetPathField(tx, entry.Ref.Field, entry.Ref.Index, relocated)) {
            result.Changed = true;
        } else {
            // Either the field table gained a relocatable row without a setter
            // or the footprint does not belong to this request.
            result.Skipped.push_back(entry.Ref.Field);
        }
    }

    // The working dir last, so that the PathUnderWorkingDir decisions above
    // read the value the footprint was resolved against.
    if (IsAtOrUnder(fp.WorkingDirCanon, r.OldDatabasePath)) {
        tx.SetWorkingDir(JoinUnder(r.NewDatabasePath,
            RelativeTo(fp.WorkingDirCanon, r.OldDatabasePath)));
        result.Changed = true;
    }

    return result;
}

TVector<EPathField> StripSourceLocalPreconditions(NKikimrSchemeOp::TModifyScheme& tx) {
    TVector<EPathField> stripped(tx.ApplyIfSize(), EPathField::ApplyIf_PathId);
    tx.ClearApplyIf();
    return stripped;
}

}  // namespace NKikimr::NSchemeShard
