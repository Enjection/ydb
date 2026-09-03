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

class TRefSink {
public:
    explicit TRefSink(TVector<TPathRef>& out)
        : Out(out)
    {}

    void Add(TString field, TString value, EKind kind, ERole role, TString base = {}) {
        TPathRef ref;
        ref.FieldPath = std::move(field);
        ref.Value = std::move(value);
        ref.Kind = kind;
        ref.Role = role;
        ref.BasePath = std::move(base);
        Out.push_back(std::move(ref));
    }

    // Shape 1: WorkingDir + <SubMessage>.Name
    void Leaf(TString field, TString value, ERole role = ERole::Target) {
        Add(std::move(field), std::move(value), EKind::LeafUnderWorkingDir, role);
    }

    // Shape 7: WorkingDir + a free-form relative-or-absolute path field
    void Path(TString field, TString value, ERole role = ERole::Target) {
        Add(std::move(field), std::move(value), EKind::PathUnderWorkingDir, role);
    }

    // Shape 7b: TPath::Child(value, TSplitChildTag{}) — always under WorkingDir,
    // split into segments, and a leading slash does not escape to the root.
    void SplitChild(TString field, TString value, ERole role = ERole::Target) {
        Add(std::move(field), std::move(value), EKind::PathUnderWorkingDirSplit, role);
    }

    // Shape 3: absolute path field, WorkingDir not involved
    void Abs(TString field, TString value, ERole role = ERole::Target) {
        Add(std::move(field), std::move(value), EKind::Absolute, role);
    }

    // Shape 4/5: a leaf name under another field of the same request
    void Sibling(TString field, TString value, TString base, ERole role = ERole::Target) {
        Add(std::move(field), std::move(value), EKind::LeafUnderSibling, role, std::move(base));
    }

    // Shape 4/5 when the base cannot be written as a raw string: a leaf under
    // the path of an already-emitted ref. Needed when the base field may be
    // addressed by path id, or is itself resolved with TSplitChildTag.
    void SiblingOf(TString field, TString value, int anchorIndex, ERole role = ERole::Target) {
        Add(std::move(field), std::move(value), EKind::LeafUnderSibling, role, {});
        Out.back().AnchorIndex = anchorIndex;
    }

    // Shape 2: numeric-id addressing, bypasses WorkingDir/Name
    void ById(TString field, ui64 ownerId, ui64 localPathId, ERole role = ERole::Target) {
        TPathRef ref;
        ref.FieldPath = std::move(field);
        ref.OwnerId = ownerId;
        ref.LocalPathId = localPathId;
        ref.Kind = EKind::ById;
        ref.Role = role;
        Out.push_back(std::move(ref));
    }

    // Index of the most recently added ref; use as an Implicit anchor.
    int Last() const {
        return static_cast<int>(Out.size()) - 1;
    }

    // Shape 8/9: touched paths that the request does not name at all. The set
    // is enumerated at Propose/Execute time from the children of the anchor.
    void Implicit(TString what, int anchorIndex, ERole role = ERole::Dependency) {
        Add(std::move(what), TString(), EKind::Implicit, role, {});
        Out.back().AnchorIndex = anchorIndex;
    }

private:
    TVector<TPathRef>& Out;
};

TString Indexed(TStringBuf prefix, size_t i, TStringBuf suffix) {
    return TStringBuilder() << prefix << "[" << i << "]" << suffix;
}

TString Keyed(TStringBuf prefix, TStringBuf key, TStringBuf suffix) {
    return TStringBuilder() << prefix << "[" << key << "]" << suffix;
}

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

TStringBuilder FormatPathFootprintPrefix(const TPathFootprint& footprint, ui64 txId) {
    TStringBuilder line;
    line << "PathFootprint"
         << " txId# " << txId
         << ", partId# " << ui32(footprint.PartId)
         << ", partOpType# " << NKikimrSchemeOp::EOperationType_Name(footprint.PartOpType)
         << ", proposeStatus# " << NKikimrScheme::EStatus_Name(footprint.ProposeStatus)
         << ", writeSet# " << footprint.WriteSet.size()
         << ", published# " << footprint.Published.size()
         << ", incomplete# " << (footprint.WriteSetMayBeIncomplete ? 1 : 0);
    return line;
}

}  // namespace

TString FormatPathFootprintWriteSetLine(const TPathFootprint& footprint, ui64 txId) {
    TStringBuilder line = FormatPathFootprintPrefix(footprint, txId);
    return line << ", fieldPath# <writeSet>"
                << ", writeSetPaths# " << JoinPathIds(footprint.WriteSet)
                << ", publishedPaths# " << JoinPathIds(footprint.Published);
}

TString FormatPathFootprintLine(const TPathFootprint& footprint,
        const TPathFootprintEntry* entry, ui64 txId) {
    TStringBuilder line = FormatPathFootprintPrefix(footprint, txId);
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

TVector<TPathRef> ExtractPathRefs(const NKikimrSchemeOp::TModifyScheme& tx) {
    TVector<TPathRef> result;
    TRefSink out(result);

    // Shape 2: the generic TDrop submessage, shared by ~22 op types.
    const auto genericDrop = [&]() {
        const auto& drop = tx.GetDrop();
        if (drop.HasId()) {
            // Propose() resolves TPath::Init(MakeLocalId(Id)) and ignores Name.
            out.ById("Drop.Id", 0, drop.GetId());
        } else {
            out.Leaf("Drop.Name", drop.GetName());
        }
    };

    // The CDC-stream family: WorkingDir + TableName (parent) + stream leaf.
    const auto createCdcStream = [&](TStringBuf prefix) {
        const auto& op = tx.GetCreateCdcStream();
        out.Path(TStringBuilder() << prefix << ".TableName", op.GetTableName(), ERole::Parent);
        out.Sibling(TStringBuilder() << prefix << ".StreamDescription.Name",
            op.GetStreamDescription().GetName(), op.GetTableName(), ERole::Target);
    };

    // Local paths carried by a replication/transfer description. SrcPath is a
    // path on the *remote* cluster and is deliberately never emitted.
    const auto replicationPaths = [&](const TString& prefix,
            const NKikimrSchemeOp::TReplicationDescription& desc) {
        const auto& config = desc.GetConfig();
        if (config.HasTransferSpecific()) {
            // TTransferStrategy::Validate resolves both of these absolutely
            // (schemeshard__operation_create_replication.cpp:80,91).
            const auto& target = config.GetTransferSpecific().GetTarget();
            if (target.HasDstPath()) {
                out.Abs(prefix + ".Config.TransferSpecific.Target.DstPath",
                    target.GetDstPath(), ERole::Dependency);
            }
            if (target.HasDirectoryPath()) {
                out.Abs(prefix + ".Config.TransferSpecific.Target.DirectoryPath",
                    target.GetDirectoryPath(), ERole::Dependency);
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
                out.Abs(Indexed(prefix + ".Config.Specific.Targets", i, ".DstPath"),
                    target.GetDstPath(), ERole::Dependency);
            }
        }
        if (desc.HasAlterTransfer() && desc.GetAlterTransfer().HasDirectoryPath()) {
            // schemeshard__operation_alter_replication.cpp:57, absolute.
            out.Abs(prefix + ".AlterTransfer.DirectoryPath",
                desc.GetAlterTransfer().GetDirectoryPath(), ERole::Dependency);
        }
    };

    switch (tx.GetOperationType()) {
    case NKikimrSchemeOp::ESchemeOpMkDir:
        out.Leaf("MkDir.Name", tx.GetMkDir().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateTable:
        out.Leaf("CreateTable.Name", tx.GetCreateTable().GetName());
        // A CreateTable that carries CopyFromTable is dispatched to the
        // copy-table factory (schemeshard__operation.cpp), whose Propose
        // resolves the source absolutely, without WorkingDir
        // (schemeshard__operation_copy_table.cpp:568,978).
        if (tx.GetCreateTable().HasCopyFromTable()) {
            out.Abs("CreateTable.CopyFromTable",
                tx.GetCreateTable().GetCopyFromTable(), ERole::Source);
        }
        break;
    case NKikimrSchemeOp::ESchemeOpCreatePersQueueGroup:
        out.Leaf("CreatePersQueueGroup.Name", tx.GetCreatePersQueueGroup().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropTable:
        genericDrop();
        out.Implicit("DropTable.<indexes,cdcStreams,implTables>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpDropPersQueueGroup:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterTable: {
        const auto& alter = tx.GetAlterTable();
        if (alter.HasPathId()) {
            const auto pathId = TPathId::FromProto(alter.GetPathId());
            out.ById("AlterTable.PathId", pathId.OwnerId, pathId.LocalPathId);
        } else if (alter.HasId_Deprecated()) {
            out.ById("AlterTable.Id_Deprecated", 0, alter.GetId_Deprecated());
        } else {
            out.Leaf("AlterTable.Name", alter.GetName());
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
            const TString field = Indexed("AlterTable.Columns", i, ".DefaultFromSequence");
            const TString& value = column.GetDefaultFromSequence();
            if (value.StartsWith('/')) {
                out.Abs(field, value, ERole::Dependency);
            } else {
                out.SiblingOf(field, value, alterTableIndex, ERole::Dependency);
            }
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterPersQueueGroup: {
        const auto& alter = tx.GetAlterPersQueueGroup();
        if (alter.HasPathId()) {
            out.ById("AlterPersQueueGroup.PathId", 0, alter.GetPathId());
        } else {
            out.Leaf("AlterPersQueueGroup.Name", alter.GetName());
        }
        // schemeshard__operation_alter_pq.cpp:328 resolves the incremental
        // backup destination absolutely while building the alter data.
        const auto& offload = alter.GetPQTabletConfig().GetOffloadConfig();
        if (offload.HasIncrementalBackup()) {
            out.Abs("AlterPersQueueGroup.PQTabletConfig.OffloadConfig.IncrementalBackup.DstPath",
                offload.GetIncrementalBackup().GetDstPath(), ERole::Dependency);
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpModifyACL:
        out.Leaf("ModifyACL.Name", tx.GetModifyACL().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpRmDir:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions: {
        const auto& info = tx.GetSplitMergeTablePartitions();
        if (info.HasTableLocalId()) {
            out.ById("SplitMergeTablePartitions.TableLocalId",
                info.GetTableOwnerId(), info.GetTableLocalId());
        } else {
            // Propose() resolves TablePath directly, WITHOUT joining WorkingDir.
            out.Abs("SplitMergeTablePartitions.TablePath", info.GetTablePath());
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpBackup:
        out.Leaf("Backup.TableName", tx.GetBackup().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSubDomain:
        out.Leaf("SubDomain.Name", tx.GetSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSubDomain:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateRtmrVolume:
        out.Leaf("CreateRtmrVolume.Name", tx.GetCreateRtmrVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateBlockStoreVolume:
        out.Leaf("CreateBlockStoreVolume.Name", tx.GetCreateBlockStoreVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterBlockStoreVolume: {
        const auto& alter = tx.GetAlterBlockStoreVolume();
        if (alter.HasPathId()) {
            out.ById("AlterBlockStoreVolume.PathId", 0, alter.GetPathId());
        } else {
            out.Leaf("AlterBlockStoreVolume.Name", alter.GetName());
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAssignBlockStoreVolume:
        out.Leaf("AssignBlockStoreVolume.Name", tx.GetAssignBlockStoreVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropBlockStoreVolume:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateKesus:
        out.Leaf("Kesus.Name", tx.GetKesus().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropKesus:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpForceDropSubDomain:
        genericDrop();
        out.Implicit("ForceDropSubDomain.<subtree>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSolomonVolume:
        out.Leaf("CreateSolomonVolume.Name", tx.GetCreateSolomonVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSolomonVolume:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterKesus:
        out.Leaf("Kesus.Name", tx.GetKesus().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterSubDomain:
        out.Leaf("SubDomain.Name", tx.GetSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterUserAttributes:
        out.Path("AlterUserAttributes.PathName", tx.GetAlterUserAttributes().GetPathName());
        break;
    case NKikimrSchemeOp::ESchemeOpForceDropUnsafe:
        genericDrop();
        out.Implicit("ForceDropUnsafe.<subtree>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateIndexedTable: {
        const auto& cfg = tx.GetCreateIndexedTable();
        const TString& base = cfg.GetTableDescription().GetName();
        out.Leaf("CreateIndexedTable.TableDescription.Name", base);
        const int baseIndex = out.Last();
        for (size_t i = 0; i < cfg.IndexDescriptionSize(); ++i) {
            out.Sibling(Indexed("CreateIndexedTable.IndexDescription", i, ".Name"),
                cfg.GetIndexDescription(i).GetName(), base, ERole::Dependency);
        }
        for (size_t i = 0; i < cfg.SequenceDescriptionSize(); ++i) {
            out.Sibling(Indexed("CreateIndexedTable.SequenceDescription", i, ".Name"),
                cfg.GetSequenceDescription(i).GetName(), base, ERole::Dependency);
        }
        out.Implicit("CreateIndexedTable.<indexImplTables>", baseIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateTableIndex:
        out.Leaf("CreateTableIndex.Name", tx.GetCreateTableIndex().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables: {
        const auto& cfg = tx.GetCreateConsistentCopyTables();
        for (size_t i = 0; i < cfg.CopyTableDescriptionsSize(); ++i) {
            const auto& item = cfg.GetCopyTableDescriptions(i);
            const TString prefix = Indexed("CreateConsistentCopyTables.CopyTableDescriptions", i, "");
            out.Abs(prefix + ".SrcPath", item.GetSrcPath(), ERole::Source);
            const int srcIndex = out.Last();
            out.Abs(prefix + ".DstPath", item.GetDstPath(), ERole::Target);
            if (item.HasCreateSrcCdcStream()) {
                out.Sibling(prefix + ".CreateSrcCdcStream.StreamDescription.Name",
                    item.GetCreateSrcCdcStream().GetStreamDescription().GetName(),
                    item.GetSrcPath(), ERole::Dependency);
            }
            if (item.HasDropSrcCdcStream()) {
                const auto& drop = item.GetDropSrcCdcStream();
                for (size_t j = 0; j < drop.StreamNameSize(); ++j) {
                    out.Sibling(Indexed(prefix + ".DropSrcCdcStream.StreamName", j, ""),
                        drop.GetStreamName(j), item.GetSrcPath(), ERole::Dependency);
                }
            }
            for (const auto* kv : SortedByKey(item.GetIndexImplTableCdcStreams())) {
                out.Sibling(
                    Keyed(prefix + ".IndexImplTableCdcStreams", kv->first, ".StreamDescription.Name"),
                    kv->second.GetStreamDescription().GetName(),
                    JoinPath({item.GetSrcPath(), kv->first}), ERole::Dependency);
            }
            for (const auto* kv : SortedByKey(item.GetIndexImplTableDropCdcStreams())) {
                for (size_t j = 0; j < kv->second.StreamNameSize(); ++j) {
                    out.Sibling(
                        Indexed(Keyed(prefix + ".IndexImplTableDropCdcStreams", kv->first, ".StreamName"), j, ""),
                        kv->second.GetStreamName(j),
                        JoinPath({item.GetSrcPath(), kv->first}), ERole::Dependency);
                }
            }
            out.Implicit(prefix + ".<indexes,implTables,sequences>", srcIndex);
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropTableIndex:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateExtSubDomain:
    case NKikimrSchemeOp::ESchemeOpAlterExtSubDomain:
    case NKikimrSchemeOp::ESchemeOpAlterExtSubDomainCreateHive:
        out.Leaf("SubDomain.Name", tx.GetSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpForceDropExtSubDomain:
        genericDrop();
        out.Implicit("ForceDropExtSubDomain.<subtree>", out.Last());
        break;
    case NKikimrSchemeOp::EOperationType::ESchemeOp_DEPRECATED_35:
        break;
    case NKikimrSchemeOp::ESchemeOpUpgradeSubDomain:
    case NKikimrSchemeOp::ESchemeOpUpgradeSubDomainDecision:
        out.Leaf("UpgradeSubDomain.Name", tx.GetUpgradeSubDomain().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateIndexBuild: {
        const auto& cfg = tx.GetInitiateIndexBuild();
        out.Abs("InitiateIndexBuild.Table", cfg.GetTable(), ERole::Parent);
        out.Sibling("InitiateIndexBuild.Index.Name", cfg.GetIndex().GetName(), cfg.GetTable());
        out.Implicit("InitiateIndexBuild.<indexImplTables>", out.Last());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpInitiateBuildIndexMainTable:
        out.Leaf("InitiateBuildIndexMainTable.TableName", tx.GetInitiateBuildIndexMainTable().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpPrepareIndexValidation:
        out.Leaf("PrepareIndexValidation.TableName", tx.GetPrepareIndexValidation().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateLock:
    case NKikimrSchemeOp::ESchemeOpDropLock:
        out.Leaf("LockConfig.Name", tx.GetLockConfig().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpApplyIndexBuild: {
        const auto& cfg = tx.GetApplyIndexBuild();
        out.Abs("ApplyIndexBuild.TablePath", cfg.GetTablePath(), ERole::Parent);
        out.Sibling("ApplyIndexBuild.IndexName", cfg.GetIndexName(), cfg.GetTablePath());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpFinalizeBuildIndexMainTable:
        out.Leaf("FinalizeBuildIndexMainTable.TableName", tx.GetFinalizeBuildIndexMainTable().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterTableIndex:
        out.Leaf("AlterTableIndex.Name", tx.GetAlterTableIndex().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterSolomonVolume:
        out.Leaf("AlterSolomonVolume.Name", tx.GetAlterSolomonVolume().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpFinalizeBuildIndexImplTable:
        out.Leaf("AlterTable.Name", tx.GetAlterTable().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpInitiateBuildIndexImplTable:
        out.Leaf("CreateTable.Name", tx.GetCreateTable().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropIndex: {
        const auto& cfg = tx.GetDropIndex();
        out.Path("DropIndex.TableName", cfg.GetTableName(), ERole::Parent);
        out.Sibling("DropIndex.IndexName", cfg.GetIndexName(), cfg.GetTableName());
        out.Implicit("DropIndex.<indexImplTables>", out.Last());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropTableIndexAtMainTable: {
        // index/operation_drop_index.cpp:278,311 resolves WorkingDir/TableName
        // and then tablePath.Child(IndexName).
        const auto& cfg = tx.GetDropIndex();
        out.Leaf("DropIndex.TableName", cfg.GetTableName());  // plain Dive, no split
        out.Sibling("DropIndex.IndexName", cfg.GetIndexName(), cfg.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCancelIndexBuild: {
        const auto& cfg = tx.GetCancelIndexBuild();
        out.Abs("CancelIndexBuild.TablePath", cfg.GetTablePath(), ERole::Parent);
        out.Sibling("CancelIndexBuild.IndexName", cfg.GetIndexName(), cfg.GetTablePath());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateFileStore:
        out.Leaf("CreateFileStore.Name", tx.GetCreateFileStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterFileStore:
        out.Leaf("AlterFileStore.Name", tx.GetAlterFileStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropFileStore:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpRestore:
        out.Leaf("Restore.TableName", tx.GetRestore().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateColumnStore:
        out.Leaf("CreateColumnStore.Name", tx.GetCreateColumnStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterColumnStore:
        out.Leaf("AlterColumnStore.Name", tx.GetAlterColumnStore().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropColumnStore:
        genericDrop();
        out.Implicit("DropColumnStore.<columnTables>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateColumnTable:
        // CreateColumnTable is a *separate* TModifyScheme field from
        // AlterColumnTable; olap/operations/create_table.cpp:570,643 resolves
        // WorkingDir.Child(CreateColumnTable.Name).
        out.Leaf("CreateColumnTable.Name", tx.GetCreateColumnTable().GetName());
        // With CopyFromTable set the op is dispatched to TReadOnlyCopyColumnTable
        // (schemeshard__operation.cpp:1433), whose Propose resolves the source
        // absolutely (olap/operations/read_only_copy_table.cpp:401,425).
        if (tx.GetCreateColumnTable().HasCopyFromTable()) {
            out.Abs("CreateColumnTable.CopyFromTable",
                tx.GetCreateColumnTable().GetCopyFromTable(), ERole::Source);
        }
        break;
    case NKikimrSchemeOp::ESchemeOpAlterColumnTable:
        // olap/operations/alter_table.cpp:278 falls back to AlterTable.Name
        // when the AlterColumnTable submessage is absent.
        if (tx.HasAlterColumnTable()) {
            out.Leaf("AlterColumnTable.Name", tx.GetAlterColumnTable().GetName());
        } else {
            out.Leaf("AlterTable.Name", tx.GetAlterTable().GetName());
        }
        break;
    case NKikimrSchemeOp::ESchemeOpDropColumnTable:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterLogin:
        // Touches no TPath at all; only validates WorkingDir == Audience.
        break;
    case NKikimrSchemeOp::ESchemeOpCreateCdcStream:
        createCdcStream("CreateCdcStream");
        out.Implicit("CreateCdcStream.<pqGroupUnderStream>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateCdcStreamImpl:
        out.Leaf("CreateCdcStream.StreamDescription.Name",
            tx.GetCreateCdcStream().GetStreamDescription().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateCdcStreamAtTable: {
        // The AtTable half alters the table, and also resolves the stream leaf
        // to fill txState.CdcPathId
        // (schemeshard__operation_create_cdc_stream.cpp:541,596).
        const auto& op = tx.GetCreateCdcStream();
        // :541 is a *plain* workingDirPath.Child(tableName) -- no
        // TSplitChildTag, unlike the Alter and Rotate AtTable parts below.
        out.Leaf("CreateCdcStream.TableName", op.GetTableName());
        out.Sibling("CreateCdcStream.StreamDescription.Name",
            op.GetStreamDescription().GetName(), op.GetTableName(), ERole::Target);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterCdcStream: {
        const auto& op = tx.GetAlterCdcStream();
        out.Path("AlterCdcStream.TableName", op.GetTableName(), ERole::Parent);
        out.Sibling("AlterCdcStream.StreamName", op.GetStreamName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterCdcStreamImpl:
        out.Leaf("AlterCdcStream.StreamName", tx.GetAlterCdcStream().GetStreamName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterCdcStreamAtTable: {
        // :375 resolves the table with Child(TableName, TSplitChildTag{}), so a
        // multi-segment TableName is dived segment by segment under WorkingDir;
        // :404 then takes a plain tablePath.Child(StreamName).
        const auto& op = tx.GetAlterCdcStream();
        out.SplitChild("AlterCdcStream.TableName", op.GetTableName());
        out.Sibling("AlterCdcStream.StreamName", op.GetStreamName(), op.GetTableName());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropCdcStream: {
        const auto& op = tx.GetDropCdcStream();
        out.Path("DropCdcStream.TableName", op.GetTableName(), ERole::Parent);
        for (size_t i = 0; i < op.StreamNameSize(); ++i) {
            out.Sibling(Indexed("DropCdcStream.StreamName", i, ""),
                op.GetStreamName(i), op.GetTableName());
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
        out.Leaf("DropCdcStream.TableName", op.GetTableName());
        for (size_t i = 0; i < op.StreamNameSize(); ++i) {
            out.Sibling(Indexed("DropCdcStream.StreamName", i, ""),
                op.GetStreamName(i), op.GetTableName());
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpRotateCdcStream: {
        const auto& op = tx.GetRotateCdcStream();
        out.Path("RotateCdcStream.TableName", op.GetTableName(), ERole::Parent);
        out.Sibling("RotateCdcStream.OldStreamName", op.GetOldStreamName(),
            op.GetTableName(), ERole::Source);
        out.Sibling("RotateCdcStream.NewStream.StreamDescription.Name",
            op.GetNewStream().GetStreamDescription().GetName(),
            op.GetTableName(), ERole::Target);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpRotateCdcStreamImpl: {
        const auto& op = tx.GetRotateCdcStream();
        out.Leaf("RotateCdcStream.OldStreamName", op.GetOldStreamName(), ERole::Source);
        out.Leaf("RotateCdcStream.NewStream.StreamDescription.Name",
            op.GetNewStream().GetStreamDescription().GetName(), ERole::Target);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpRotateCdcStreamAtTable: {
        // :543 resolves the table with Child(TableName, TSplitChildTag{}); :572
        // and :591 then take plain tablePath.Child() for the old and new stream.
        const auto& op = tx.GetRotateCdcStream();
        out.SplitChild("RotateCdcStream.TableName", op.GetTableName());
        out.Sibling("RotateCdcStream.OldStreamName", op.GetOldStreamName(),
            op.GetTableName(), ERole::Source);
        out.Sibling("RotateCdcStream.NewStream.StreamDescription.Name",
            op.GetNewStream().GetStreamDescription().GetName(),
            op.GetTableName(), ERole::Target);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpMoveTable: {
        out.Abs("MoveTable.SrcPath", tx.GetMoveTable().GetSrcPath(), ERole::Source);
        const int moveSrcIndex = out.Last();
        out.Abs("MoveTable.DstPath", tx.GetMoveTable().GetDstPath(), ERole::Target);
        // The cascade is enumerated from the children of the *source*.
        out.Implicit("MoveTable.<indexes,implTables,cdcStreams>", moveSrcIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpMoveTableIndex: {
        out.Abs("MoveTableIndex.SrcPath", tx.GetMoveTableIndex().GetSrcPath(), ERole::Source);
        const int moveTableIndexSrcIndex = out.Last();
        out.Abs("MoveTableIndex.DstPath", tx.GetMoveTableIndex().GetDstPath(), ERole::Target);
        // Impl tables and sequences are enumerated from the children of the
        // *source* (schemeshard__operation_move_tables.cpp:110), exactly as for
        // MoveTable and MoveIndex.
        out.Implicit("MoveTableIndex.<indexImplTables,sequences>", moveTableIndexSrcIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpMoveSequence:
        out.Abs("MoveSequence.SrcPath", tx.GetMoveSequence().GetSrcPath(), ERole::Source);
        out.Abs("MoveSequence.DstPath", tx.GetMoveSequence().GetDstPath(), ERole::Target);
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSequence:
    case NKikimrSchemeOp::ESchemeOpAlterSequence:
        // Both resolve WorkingDir/Sequence.Name.
        out.Leaf("Sequence.Name", tx.GetSequence().GetName());
        // A CreateSequence part emitted by CreateConsistentCopyTables carries
        // the source sequence here; TCopySequence::Propose resolves it
        // absolutely (schemeshard__operation_copy_sequence.cpp:579).
        if (tx.HasCopySequence()) {
            out.Abs("CopySequence.CopyFrom",
                tx.GetCopySequence().GetCopyFrom(), ERole::Source);
        }
        break;
    case NKikimrSchemeOp::ESchemeOpDropSequence:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateReplication:
    case NKikimrSchemeOp::ESchemeOpCreateTransfer:
        out.Leaf("Replication.Name", tx.GetReplication().GetName());
        replicationPaths("Replication", tx.GetReplication());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterReplication:
    case NKikimrSchemeOp::ESchemeOpAlterTransfer: {
        const auto& op = tx.GetAlterReplication();
        if (op.HasPathId()) {
            const auto pathId = TPathId::FromProto(op.GetPathId());
            out.ById("AlterReplication.PathId", pathId.OwnerId, pathId.LocalPathId);
        } else {
            out.Leaf("AlterReplication.Name", op.GetName());
        }
        replicationPaths("AlterReplication", op);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropReplication:
    case NKikimrSchemeOp::ESchemeOpDropReplicationCascade:
    case NKikimrSchemeOp::ESchemeOpDropTransfer:
    case NKikimrSchemeOp::ESchemeOpDropTransferCascade:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateBlobDepot:
        out.Leaf("BlobDepot.Name", tx.GetBlobDepot().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterBlobDepot:
    case NKikimrSchemeOp::ESchemeOpDropBlobDepot:
        // Propose() for these two is a no-op stub; no TPath is touched.
        break;
    case NKikimrSchemeOp::ESchemeOpMoveIndex: {
        const auto& op = tx.GetMoveIndex();
        out.Abs("MoveIndex.TablePath", op.GetTablePath(), ERole::Parent);
        out.Sibling("MoveIndex.SrcPath", op.GetSrcPath(), op.GetTablePath(), ERole::Source);
        const int moveIndexSrcIndex = out.Last();
        out.Sibling("MoveIndex.DstPath", op.GetDstPath(), op.GetTablePath(), ERole::Target);
        out.Implicit("MoveIndex.<indexImplTables>", moveIndexSrcIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateExternalTable:
        out.Leaf("CreateExternalTable.Name", tx.GetCreateExternalTable().GetName());
        if (tx.GetCreateExternalTable().HasDataSourcePath()) {
            out.Abs("CreateExternalTable.DataSourcePath",
                tx.GetCreateExternalTable().GetDataSourcePath(), ERole::Dependency);
        }
        break;
    case NKikimrSchemeOp::ESchemeOpDropExternalTable:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterExternalTable:
        out.Leaf("CreateExternalTable.Name", tx.GetCreateExternalTable().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateExternalDataSource:
        out.Leaf("CreateExternalDataSource.Name", tx.GetCreateExternalDataSource().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropExternalDataSource:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterExternalDataSource:
        out.Leaf("CreateExternalDataSource.Name", tx.GetCreateExternalDataSource().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateColumnBuild:
        out.Abs("InitiateColumnBuild.Table", tx.GetInitiateColumnBuild().GetTable());
        break;
    case NKikimrSchemeOp::ESchemeOpDropColumnBuild:
        out.Abs("DropColumnBuild.Settings.Table", tx.GetDropColumnBuild().GetSettings().GetTable());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateView:
        out.Leaf("CreateView.Name", tx.GetCreateView().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropView:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpAlterView:
        // Unimplemented in the tree; no Propose() exists.
        break;
    case NKikimrSchemeOp::ESchemeOpCreateContinuousBackup: {
        const auto& op = tx.GetCreateContinuousBackup();
        out.Leaf("CreateContinuousBackup.TableName", op.GetTableName());
        const int cbTableIndex = out.Last();
        // NCdc::DoNewStreamPathChecks resolves tablePath.Child(streamName)
        // (schemeshard__operation_create_continuous_backup.cpp:35). When the
        // field is absent the name is generated from the current time, so
        // there is nothing to report and the Implicit marker below stands in.
        if (op.GetContinuousBackupDescription().HasStreamName()) {
            out.SiblingOf("CreateContinuousBackup.ContinuousBackupDescription.StreamName",
                op.GetContinuousBackupDescription().GetStreamName(), cbTableIndex);
        }
        out.Implicit("CreateContinuousBackup.<cdcStream>", cbTableIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterContinuousBackup: {
        const auto& op = tx.GetAlterContinuousBackup();
        // :86 resolves the table with Child(TableName, TSplitChildTag{}), so a
        // leading slash does not escape the working dir.
        out.SplitChild("AlterContinuousBackup.TableName", op.GetTableName());
        const int cbTableIndex = out.Last();
        if (op.HasTakeIncrementalBackup()) {
            const auto& take = op.GetTakeIncrementalBackup();
            // :128 workingDirPath.Child(DstPath, TSplitChildTag{}).
            out.SplitChild("AlterContinuousBackup.TakeIncrementalBackup.DstPath",
                take.GetDstPath());
            if (take.HasDstStreamPath()) {
                // :161 the new stream is a leaf under the table. When the field
                // is absent the name is generated from the current time, so
                // there is nothing to report.
                out.SiblingOf("AlterContinuousBackup.TakeIncrementalBackup.DstStreamPath",
                    take.GetDstStreamPath(), cbTableIndex);
            }
        }
        out.Implicit("AlterContinuousBackup.<incrementalBackupTable>", cbTableIndex);
        break;
    }
    case NKikimrSchemeOp::ESchemeOpDropContinuousBackup:
        out.Leaf("DropContinuousBackup.TableName", tx.GetDropContinuousBackup().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateResourcePool:
    case NKikimrSchemeOp::ESchemeOpAlterResourcePool:
        out.Leaf("CreateResourcePool.Name", tx.GetCreateResourcePool().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropResourcePool:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups:
    case NKikimrSchemeOp::ESchemeOpRestoreIncrementalBackupAtTable: {
        // Retired: the factory always rejects. Kept for completeness.
        const auto& op = tx.GetRestoreMultipleIncrementalBackups();
        for (size_t i = 0; i < op.SrcTablePathsSize(); ++i) {
            out.Abs(Indexed("RestoreMultipleIncrementalBackups.SrcTablePaths", i, ""),
                op.GetSrcTablePaths(i), ERole::Source);
        }
        out.Abs("RestoreMultipleIncrementalBackups.DstTablePath", op.GetDstTablePath());
        break;
    }
    case NKikimrSchemeOp::ESchemeOpCreateBackupCollection: {
        const auto& op = tx.GetCreateBackupCollection();
        out.Leaf("CreateBackupCollection.Name", op.GetName());
        // Propose() -> RegisterBackupCollectionTables() resolves every entry
        // with TPath::Resolve(entry.GetPath()) — absolute, no WorkingDir join
        // (schemeshard_impl.cpp:3920).
        const auto& entryList = op.GetExplicitEntryList();
        for (size_t i = 0; i < entryList.EntriesSize(); ++i) {
            out.Abs(Indexed("CreateBackupCollection.ExplicitEntryList.Entries", i, ".Path"),
                entryList.GetEntries(i).GetPath(), ERole::Dependency);
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpAlterBackupCollection:
        out.Leaf("AlterBackupCollection.Name", tx.GetAlterBackupCollection().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropBackupCollection:
        out.Leaf("DropBackupCollection.Name", tx.GetDropBackupCollection().GetName());
        out.Implicit("DropBackupCollection.<collectionEntries>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpBackupBackupCollection:
        out.Leaf("BackupBackupCollection.Name", tx.GetBackupBackupCollection().GetName());
        out.Implicit("BackupBackupCollection.<collectionEntries>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpBackupIncrementalBackupCollection:
    case NKikimrSchemeOp::ESchemeOpCreateLongIncrementalBackupOp:
        out.Leaf("BackupIncrementalBackupCollection.Name",
            tx.GetBackupIncrementalBackupCollection().GetName());
        out.Implicit("BackupIncrementalBackupCollection.<collectionEntries>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateFullBackupOp:
        // WorkingDir already points at the backup collection; no name field.
        out.Path("<WorkingDir>", TString());
        out.Implicit("CreateFullBackupOp.<collectionEntries>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpRestoreBackupCollection:
    case NKikimrSchemeOp::ESchemeOpCreateLongIncrementalRestoreOp:
        out.Leaf("RestoreBackupCollection.Name", tx.GetRestoreBackupCollection().GetName());
        out.Implicit("RestoreBackupCollection.<collectionEntries>", out.Last());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSysView:
        out.Leaf("CreateSysView.Name", tx.GetCreateSysView().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSysView:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpChangePathState:
        out.Path("ChangePathState.Path", tx.GetChangePathState().GetPath());
        break;
    case NKikimrSchemeOp::ESchemeOpIncrementalRestoreLockTargets:
    case NKikimrSchemeOp::ESchemeOpIncrementalRestoreUnlockTargets: {
        const auto& op = tx.GetIncrementalRestoreLockTargets();
        for (size_t i = 0; i < op.DstPathsSize(); ++i) {
            out.Path(Indexed("IncrementalRestoreLockTargets.DstPaths", i, ""),
                op.GetDstPaths(i), ERole::Target);
        }
        for (size_t i = 0; i < op.SrcPathsSize(); ++i) {
            out.Path(Indexed("IncrementalRestoreLockTargets.SrcPaths", i, ""),
                op.GetSrcPaths(i), ERole::Source);
        }
        break;
    }
    case NKikimrSchemeOp::ESchemeOpIncrementalRestoreFinalize:
        out.Implicit("IncrementalRestoreFinalize.<persistedRestoreState>", -1, ERole::Target);
        break;
    case NKikimrSchemeOp::ESchemeOpCreateSecret:
        out.Leaf("CreateSecret.Name", tx.GetCreateSecret().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpAlterSecret:
        out.Leaf("AlterSecret.Name", tx.GetAlterSecret().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropSecret:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpCreateStreamingQuery:
    case NKikimrSchemeOp::ESchemeOpAlterStreamingQuery:
        out.Leaf("CreateStreamingQuery.Name", tx.GetCreateStreamingQuery().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropStreamingQuery:
        genericDrop();
        break;
    case NKikimrSchemeOp::ESchemeOpTruncateTable:
        out.Path("TruncateTable.TableName", tx.GetTruncateTable().GetTableName());
        break;
    case NKikimrSchemeOp::ESchemeOpCreateTestShardSet:
        out.Leaf("CreateTestShardSet.Name", tx.GetCreateTestShardSet().GetName());
        break;
    case NKikimrSchemeOp::ESchemeOpDropTestShardSet:
        genericDrop();
        break;
    }

    return result;
}

// Keep this in sync with the switch above: every protobuf field it reads as a
// path must appear here, fully qualified, and nothing else. The descriptor-walk
// test in ut_path_footprint uses it to decide whether a path-like field of
// TModifyScheme is covered, so a new case without a new entry fails that test.
// Id-valued fields (TDrop.Id, TTableDescription.PathId, ...) are not listed:
// the walk only classifies string fields.
const TVector<TStringBuf>& KnownPathFieldNames() {
    static const TVector<TStringBuf> names = {
        "NKikimrSchemeOp.TAlterCdcStream.StreamName",
        "NKikimrSchemeOp.TAlterCdcStream.TableName",
        "NKikimrSchemeOp.TAlterColumnStore.Name",
        "NKikimrSchemeOp.TAlterColumnTable.Name",
        "NKikimrSchemeOp.TAlterContinuousBackup.TTakeIncrementalBackup.DstPath",
        "NKikimrSchemeOp.TAlterContinuousBackup.TTakeIncrementalBackup.DstStreamPath",
        "NKikimrSchemeOp.TAlterContinuousBackup.TableName",
        "NKikimrSchemeOp.TAlterSolomonVolume.Name",
        "NKikimrSchemeOp.TAlterUserAttributes.PathName",
        "NKikimrSchemeOp.TBackupBackupCollection.Name",
        "NKikimrSchemeOp.TBackupCollectionDescription.Name",
        "NKikimrSchemeOp.TBackupCollectionDescription.TBackupEntry.Path",
        "NKikimrSchemeOp.TBackupTask.TableName",
        "NKikimrSchemeOp.TBlobDepotDescription.Name",
        "NKikimrSchemeOp.TBlockStoreAssignOp.Name",
        "NKikimrSchemeOp.TBlockStoreVolumeDescription.Name",
        "NKikimrSchemeOp.TCdcStreamDescription.Name",
        "NKikimrSchemeOp.TChangePathState.Path",
        "NKikimrSchemeOp.TColumnDescription.DefaultFromSequence",
        "NKikimrSchemeOp.TColumnStoreDescription.Name",
        "NKikimrSchemeOp.TColumnTableDescription.CopyFromTable",
        "NKikimrSchemeOp.TColumnTableDescription.Name",
        "NKikimrSchemeOp.TContinuousBackupDescription.StreamName",
        "NKikimrSchemeOp.TCopySequence.CopyFrom",
        "NKikimrSchemeOp.TCopyTableConfig.DstPath",
        "NKikimrSchemeOp.TCopyTableConfig.SrcPath",
        "NKikimrSchemeOp.TCreateCdcStream.TableName",
        "NKikimrSchemeOp.TCreateContinuousBackup.TableName",
        "NKikimrSchemeOp.TCreateSolomonVolume.Name",
        "NKikimrSchemeOp.TCreateTestShardSet.Name",
        "NKikimrSchemeOp.TDrop.Name",
        "NKikimrSchemeOp.TDropCdcStream.StreamName",
        "NKikimrSchemeOp.TDropCdcStream.TableName",
        "NKikimrSchemeOp.TDropContinuousBackup.TableName",
        "NKikimrSchemeOp.TDropIndex.IndexName",
        "NKikimrSchemeOp.TDropIndex.TableName",
        "NKikimrSchemeOp.TExternalDataSourceDescription.Name",
        "NKikimrSchemeOp.TExternalTableDescription.DataSourcePath",
        "NKikimrSchemeOp.TExternalTableDescription.Name",
        "NKikimrSchemeOp.TFileStoreDescription.Name",
        "NKikimrSchemeOp.TFinalizeBuildIndexMainTable.TableName",
        "NKikimrSchemeOp.TIncrementalRestoreLockTargets.DstPaths",
        "NKikimrSchemeOp.TIncrementalRestoreLockTargets.SrcPaths",
        "NKikimrSchemeOp.TIndexAlteringConfig.Name",
        "NKikimrSchemeOp.TIndexBuildConfig.Table",
        "NKikimrSchemeOp.TIndexBuildControl.IndexName",
        "NKikimrSchemeOp.TIndexBuildControl.TablePath",
        "NKikimrSchemeOp.TIndexCreationConfig.Name",
        "NKikimrSchemeOp.TInitiateBuildIndexMainTable.TableName",
        "NKikimrSchemeOp.TKesusDescription.Name",
        "NKikimrSchemeOp.TLockConfig.Name",
        "NKikimrSchemeOp.TModifyACL.Name",
        "NKikimrSchemeOp.TMkDir.Name",
        "NKikimrSchemeOp.TMove.DstPath",
        "NKikimrSchemeOp.TMove.SrcPath",
        "NKikimrSchemeOp.TMoveIndex.DstPath",
        "NKikimrSchemeOp.TMoveIndex.SrcPath",
        "NKikimrSchemeOp.TMoveIndex.TablePath",
        "NKikimrSchemeOp.TPersQueueGroupDescription.Name",
        "NKikimrSchemeOp.TPrepareIndexValidation.TableName",
        "NKikimrSchemeOp.TReplicationDescription.Name",
        "NKikimrSchemeOp.TReplicationDescription.TAlterTransfer.DirectoryPath",
        "NKikimrSchemeOp.TResourcePoolDescription.Name",
        "NKikimrSchemeOp.TRestoreMultipleIncrementalBackups.DstTablePath",
        "NKikimrSchemeOp.TRestoreMultipleIncrementalBackups.SrcTablePaths",
        "NKikimrSchemeOp.TRestoreTask.TableName",
        "NKikimrSchemeOp.TRotateCdcStream.OldStreamName",
        "NKikimrSchemeOp.TRotateCdcStream.TableName",
        "NKikimrSchemeOp.TRtmrVolumeDescription.Name",
        "NKikimrSchemeOp.TSecretSchemaOp.Name",
        "NKikimrSchemeOp.TSequenceDescription.Name",
        "NKikimrSchemeOp.TSplitMergeTablePartitions.TablePath",
        "NKikimrSchemeOp.TStreamingQueryDescription.Name",
        "NKikimrSchemeOp.TSysViewDescription.Name",
        "NKikimrSchemeOp.TTableDescription.CopyFromTable",
        "NKikimrSchemeOp.TTableDescription.Name",
        "NKikimrSchemeOp.TTruncateTable.TableName",
        "NKikimrSchemeOp.TUpgradeSubDomain.Name",
        "NKikimrSchemeOp.TViewDescription.Name",
        // Path-carrying fields owned by other packages.
        "NKikimrIndexBuilder.TColumnBuildSettings.Table",
        "NKikimrPQ.TOffloadConfig.TIncrementalBackup.DstPath",
        "NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.DstPath",
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DirectoryPath",
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DstPath",
        "NKikimrSubDomains.TSubDomainSettings.Name",
    };
    return names;
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

}  // namespace

TPathFootprint ResolvePathFootprint(const NKikimrSchemeOp::TModifyScheme& tx, TSchemeShard* ss) {
    TPathFootprint footprint;
    footprint.WorkingDir = tx.GetWorkingDir();
    footprint.PartOpType = tx.GetOperationType();

    // Resolved once per footprint and reused by every entry below.
    const TPath workingDirPath = TPath::Resolve(footprint.WorkingDir, ss);
    // The canonized working dir, which is what the entries' AbsPath is built
    // from. Stripping against tx.GetWorkingDir() would silently fail whenever
    // the raw proto string is not already canonical.
    const TString workingDirCanon = workingDirPath.PathString();

    TString dbPath;
    {
        const TPath existing = workingDirPath.FirstExistedParent();
        if (existing.IsResolved()) {
            footprint.DatabasePathId = existing.GetPathIdForDomain();
            dbPath = existing.GetDomainPathString();
        }
        footprint.WorkingDirRelToDb = StripPrefix(workingDirCanon, dbPath);
    }

    for (auto& ref : ExtractPathRefs(tx)) {
        TPathFootprintEntry entry;
        entry.Ref = ref;

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
            ? ref.Value
            : StripPrefix(entry.AbsPath, workingDirCanon);

        footprint.Entries.push_back(std::move(entry));
    }

    return footprint;
}

}  // namespace NKikimr::NSchemeShard
