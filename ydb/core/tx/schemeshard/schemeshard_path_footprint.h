#pragma once

#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard_identificators.h>

#include <util/generic/deque.h>
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
    // all (runtime/state derived). Value is empty; the field path describes
    // what.
    Implicit,
};

enum class EPathRefRole {
    Target,
    Source,
    Parent,
    Dependency,
};

////////////////////////////////////////////////////////////////////////////////
// The path-field table.
//
// One row per distinct protobuf field that ExtractPathRefs reads as a path,
// plus one synthetic row per Implicit marker. Columns:
//
//   1. enumerator of EPathField, named <Submessage>_<Field>;
//   2. the field-path template rendered into log lines and asserted by tests.
//      "{i}" and "{j}" expand to TPathRef::Index and TPathRef::SubIndex,
//      "{key}" to TPathRef::MapKey;
//   3. the fully qualified protobuf field name, consumed by the descriptor-walk
//      completeness test through KnownPathFieldNames(). Empty for a synthetic
//      marker and for an id-valued field: the walk only classifies string
//      fields. Several rows may name the same protobuf field (the same
//      submessage read under two different prefixes); KnownPathFieldNames()
//      deduplicates;
//   4. the default EPathRefKind and 5. the default EPathRefRole. A handful of
//      fields are resolved differently depending on which operation type
//      carries them (the CDC-stream AtTable/Impl parts, DropIndex.TableName,
//      AlterTable.Columns[].DefaultFromSequence); those call sites override
//      both columns explicitly.
//
// Keep this list in sync with the ExtractPathRefs switch: the switch names an
// enumerator per emitted ref, so a row that no case uses is dead, and a case
// needs a row before it can emit anything.
#define SCHEMESHARD_PATH_FIELDS(X)                                                                 \
    /* The generic TDrop submessage, shared by ~22 op types. */                                     \
    X(Drop_Id, "Drop.Id", "", ById, Target)                                                         \
    X(Drop_Name, "Drop.Name", "NKikimrSchemeOp.TDrop.Name", LeafUnderWorkingDir, Target)             \
    X(MkDir_Name, "MkDir.Name", "NKikimrSchemeOp.TMkDir.Name", LeafUnderWorkingDir, Target)          \
    X(CreateTable_Name, "CreateTable.Name", "NKikimrSchemeOp.TTableDescription.Name",                \
        LeafUnderWorkingDir, Target)                                                                \
    X(CreateTable_CopyFromTable, "CreateTable.CopyFromTable",                                       \
        "NKikimrSchemeOp.TTableDescription.CopyFromTable", Absolute, Source)                        \
    X(CreatePersQueueGroup_Name, "CreatePersQueueGroup.Name",                                       \
        "NKikimrSchemeOp.TPersQueueGroupDescription.Name", LeafUnderWorkingDir, Target)             \
    X(Implicit_DropTable_Children, "DropTable.<indexes,cdcStreams,implTables>", "",                 \
        Implicit, Dependency)                                                                       \
    X(AlterTable_PathId, "AlterTable.PathId", "", ById, Target)                                      \
    X(AlterTable_Id_Deprecated, "AlterTable.Id_Deprecated", "", ById, Target)                        \
    X(AlterTable_Name, "AlterTable.Name", "NKikimrSchemeOp.TTableDescription.Name",                 \
        LeafUnderWorkingDir, Target)                                                                \
    X(AlterTable_Column_DefaultFromSequence, "AlterTable.Columns[{i}].DefaultFromSequence",         \
        "NKikimrSchemeOp.TColumnDescription.DefaultFromSequence", LeafUnderSibling, Dependency)      \
    X(AlterPersQueueGroup_PathId, "AlterPersQueueGroup.PathId", "", ById, Target)                    \
    X(AlterPersQueueGroup_Name, "AlterPersQueueGroup.Name",                                         \
        "NKikimrSchemeOp.TPersQueueGroupDescription.Name", LeafUnderWorkingDir, Target)             \
    X(AlterPersQueueGroup_IncrementalBackup_DstPath,                                                \
        "AlterPersQueueGroup.PQTabletConfig.OffloadConfig.IncrementalBackup.DstPath",               \
        "NKikimrPQ.TOffloadConfig.TIncrementalBackup.DstPath", Absolute, Dependency)                \
    X(ModifyACL_Name, "ModifyACL.Name", "NKikimrSchemeOp.TModifyACL.Name",                          \
        LeafUnderWorkingDir, Target)                                                                \
    X(SplitMergeTablePartitions_TableLocalId, "SplitMergeTablePartitions.TableLocalId", "",         \
        ById, Target)                                                                               \
    X(SplitMergeTablePartitions_TablePath, "SplitMergeTablePartitions.TablePath",                   \
        "NKikimrSchemeOp.TSplitMergeTablePartitions.TablePath", Absolute, Target)                   \
    X(Backup_TableName, "Backup.TableName", "NKikimrSchemeOp.TBackupTask.TableName",                \
        LeafUnderWorkingDir, Target)                                                                \
    X(SubDomain_Name, "SubDomain.Name", "NKikimrSubDomains.TSubDomainSettings.Name",                \
        LeafUnderWorkingDir, Target)                                                                \
    X(CreateRtmrVolume_Name, "CreateRtmrVolume.Name",                                               \
        "NKikimrSchemeOp.TRtmrVolumeDescription.Name", LeafUnderWorkingDir, Target)                 \
    X(CreateBlockStoreVolume_Name, "CreateBlockStoreVolume.Name",                                   \
        "NKikimrSchemeOp.TBlockStoreVolumeDescription.Name", LeafUnderWorkingDir, Target)           \
    X(AlterBlockStoreVolume_PathId, "AlterBlockStoreVolume.PathId", "", ById, Target)                \
    X(AlterBlockStoreVolume_Name, "AlterBlockStoreVolume.Name",                                     \
        "NKikimrSchemeOp.TBlockStoreVolumeDescription.Name", LeafUnderWorkingDir, Target)           \
    X(AssignBlockStoreVolume_Name, "AssignBlockStoreVolume.Name",                                   \
        "NKikimrSchemeOp.TBlockStoreAssignOp.Name", LeafUnderWorkingDir, Target)                    \
    X(Kesus_Name, "Kesus.Name", "NKikimrSchemeOp.TKesusDescription.Name",                           \
        LeafUnderWorkingDir, Target)                                                                \
    X(Implicit_ForceDropSubDomain_Subtree, "ForceDropSubDomain.<subtree>", "",                      \
        Implicit, Dependency)                                                                       \
    X(CreateSolomonVolume_Name, "CreateSolomonVolume.Name",                                         \
        "NKikimrSchemeOp.TCreateSolomonVolume.Name", LeafUnderWorkingDir, Target)                   \
    X(AlterSolomonVolume_Name, "AlterSolomonVolume.Name",                                           \
        "NKikimrSchemeOp.TAlterSolomonVolume.Name", LeafUnderWorkingDir, Target)                    \
    X(AlterUserAttributes_PathName, "AlterUserAttributes.PathName",                                 \
        "NKikimrSchemeOp.TAlterUserAttributes.PathName", PathUnderWorkingDir, Target)               \
    X(Implicit_ForceDropExtSubDomain_Subtree, "ForceDropExtSubDomain.<subtree>", "",                 \
        Implicit, Dependency)                                                                       \
    X(Implicit_ForceDropUnsafe_Subtree, "ForceDropUnsafe.<subtree>", "", Implicit, Dependency)       \
    X(CreateIndexedTable_TableDescription_Name, "CreateIndexedTable.TableDescription.Name",         \
        "NKikimrSchemeOp.TTableDescription.Name", LeafUnderWorkingDir, Target)                      \
    X(CreateIndexedTable_IndexDescription_Name, "CreateIndexedTable.IndexDescription[{i}].Name",    \
        "NKikimrSchemeOp.TIndexCreationConfig.Name", LeafUnderSibling, Dependency)                  \
    X(CreateIndexedTable_SequenceDescription_Name,                                                  \
        "CreateIndexedTable.SequenceDescription[{i}].Name",                                         \
        "NKikimrSchemeOp.TSequenceDescription.Name", LeafUnderSibling, Dependency)                  \
    X(Implicit_CreateIndexedTable_IndexImplTables, "CreateIndexedTable.<indexImplTables>", "",      \
        Implicit, Dependency)                                                                       \
    X(CreateTableIndex_Name, "CreateTableIndex.Name",                                               \
        "NKikimrSchemeOp.TIndexCreationConfig.Name", LeafUnderWorkingDir, Target)                   \
    /* CreateConsistentCopyTables: one group per repeated CopyTableDescriptions item. */             \
    X(CopyTables_Item_SrcPath, "CreateConsistentCopyTables.CopyTableDescriptions[{i}].SrcPath",     \
        "NKikimrSchemeOp.TCopyTableConfig.SrcPath", Absolute, Source)                               \
    X(CopyTables_Item_DstPath, "CreateConsistentCopyTables.CopyTableDescriptions[{i}].DstPath",     \
        "NKikimrSchemeOp.TCopyTableConfig.DstPath", Absolute, Target)                               \
    X(CopyTables_Item_CreateSrcCdc_StreamName,                                                      \
        "CreateConsistentCopyTables.CopyTableDescriptions[{i}]"                                     \
            ".CreateSrcCdcStream.StreamDescription.Name",                                           \
        "NKikimrSchemeOp.TCdcStreamDescription.Name", LeafUnderSibling, Dependency)                 \
    X(CopyTables_Item_DropSrcCdc_StreamName,                                                        \
        "CreateConsistentCopyTables.CopyTableDescriptions[{i}].DropSrcCdcStream.StreamName[{j}]",   \
        "NKikimrSchemeOp.TDropCdcStream.StreamName", LeafUnderSibling, Dependency)                  \
    X(CopyTables_Item_IndexImplCdc_StreamName,                                                      \
        "CreateConsistentCopyTables.CopyTableDescriptions[{i}]"                                     \
            ".IndexImplTableCdcStreams[{key}].StreamDescription.Name",                              \
        "NKikimrSchemeOp.TCdcStreamDescription.Name", LeafUnderSibling, Dependency)                 \
    X(CopyTables_Item_IndexImplDropCdc_StreamName,                                                  \
        "CreateConsistentCopyTables.CopyTableDescriptions[{i}]"                                     \
            ".IndexImplTableDropCdcStreams[{key}].StreamName[{j}]",                                 \
        "NKikimrSchemeOp.TDropCdcStream.StreamName", LeafUnderSibling, Dependency)                  \
    X(Implicit_CopyTables_Item_Children,                                                            \
        "CreateConsistentCopyTables.CopyTableDescriptions[{i}].<indexes,implTables,sequences>", "", \
        Implicit, Dependency)                                                                       \
    X(UpgradeSubDomain_Name, "UpgradeSubDomain.Name",                                               \
        "NKikimrSchemeOp.TUpgradeSubDomain.Name", LeafUnderWorkingDir, Target)                      \
    X(InitiateIndexBuild_Table, "InitiateIndexBuild.Table",                                         \
        "NKikimrSchemeOp.TIndexBuildConfig.Table", Absolute, Parent)                                \
    X(InitiateIndexBuild_Index_Name, "InitiateIndexBuild.Index.Name",                               \
        "NKikimrSchemeOp.TIndexCreationConfig.Name", LeafUnderSibling, Target)                      \
    X(Implicit_InitiateIndexBuild_IndexImplTables, "InitiateIndexBuild.<indexImplTables>", "",      \
        Implicit, Dependency)                                                                       \
    X(InitiateBuildIndexMainTable_TableName, "InitiateBuildIndexMainTable.TableName",               \
        "NKikimrSchemeOp.TInitiateBuildIndexMainTable.TableName", LeafUnderWorkingDir, Target)      \
    X(PrepareIndexValidation_TableName, "PrepareIndexValidation.TableName",                         \
        "NKikimrSchemeOp.TPrepareIndexValidation.TableName", LeafUnderWorkingDir, Target)           \
    X(LockConfig_Name, "LockConfig.Name", "NKikimrSchemeOp.TLockConfig.Name",                       \
        LeafUnderWorkingDir, Target)                                                                \
    X(ApplyIndexBuild_TablePath, "ApplyIndexBuild.TablePath",                                       \
        "NKikimrSchemeOp.TIndexBuildControl.TablePath", Absolute, Parent)                           \
    X(ApplyIndexBuild_IndexName, "ApplyIndexBuild.IndexName",                                       \
        "NKikimrSchemeOp.TIndexBuildControl.IndexName", LeafUnderSibling, Target)                   \
    X(FinalizeBuildIndexMainTable_TableName, "FinalizeBuildIndexMainTable.TableName",               \
        "NKikimrSchemeOp.TFinalizeBuildIndexMainTable.TableName", LeafUnderWorkingDir, Target)      \
    X(AlterTableIndex_Name, "AlterTableIndex.Name",                                                 \
        "NKikimrSchemeOp.TIndexAlteringConfig.Name", LeafUnderWorkingDir, Target)                   \
    X(DropIndex_TableName, "DropIndex.TableName", "NKikimrSchemeOp.TDropIndex.TableName",           \
        PathUnderWorkingDir, Parent)                                                                \
    X(DropIndex_IndexName, "DropIndex.IndexName", "NKikimrSchemeOp.TDropIndex.IndexName",           \
        LeafUnderSibling, Target)                                                                   \
    X(Implicit_DropIndex_IndexImplTables, "DropIndex.<indexImplTables>", "",                        \
        Implicit, Dependency)                                                                       \
    X(CancelIndexBuild_TablePath, "CancelIndexBuild.TablePath",                                     \
        "NKikimrSchemeOp.TIndexBuildControl.TablePath", Absolute, Parent)                           \
    X(CancelIndexBuild_IndexName, "CancelIndexBuild.IndexName",                                     \
        "NKikimrSchemeOp.TIndexBuildControl.IndexName", LeafUnderSibling, Target)                   \
    X(CreateFileStore_Name, "CreateFileStore.Name",                                                 \
        "NKikimrSchemeOp.TFileStoreDescription.Name", LeafUnderWorkingDir, Target)                  \
    X(AlterFileStore_Name, "AlterFileStore.Name",                                                   \
        "NKikimrSchemeOp.TFileStoreDescription.Name", LeafUnderWorkingDir, Target)                  \
    X(Restore_TableName, "Restore.TableName", "NKikimrSchemeOp.TRestoreTask.TableName",             \
        LeafUnderWorkingDir, Target)                                                                \
    X(CreateColumnStore_Name, "CreateColumnStore.Name",                                             \
        "NKikimrSchemeOp.TColumnStoreDescription.Name", LeafUnderWorkingDir, Target)                \
    X(AlterColumnStore_Name, "AlterColumnStore.Name",                                               \
        "NKikimrSchemeOp.TAlterColumnStore.Name", LeafUnderWorkingDir, Target)                      \
    X(Implicit_DropColumnStore_ColumnTables, "DropColumnStore.<columnTables>", "",                  \
        Implicit, Dependency)                                                                       \
    X(CreateColumnTable_Name, "CreateColumnTable.Name",                                             \
        "NKikimrSchemeOp.TColumnTableDescription.Name", LeafUnderWorkingDir, Target)                \
    X(CreateColumnTable_CopyFromTable, "CreateColumnTable.CopyFromTable",                           \
        "NKikimrSchemeOp.TColumnTableDescription.CopyFromTable", Absolute, Source)                  \
    X(AlterColumnTable_Name, "AlterColumnTable.Name",                                               \
        "NKikimrSchemeOp.TAlterColumnTable.Name", LeafUnderWorkingDir, Target)                      \
    /* CDC streams. The AtTable and Impl parts read the same submessage with a  */                   \
    /* different resolution rule, and override the defaults below.              */                   \
    X(CreateCdcStream_TableName, "CreateCdcStream.TableName",                                       \
        "NKikimrSchemeOp.TCreateCdcStream.TableName", PathUnderWorkingDir, Parent)                  \
    X(CreateCdcStream_StreamDescription_Name, "CreateCdcStream.StreamDescription.Name",             \
        "NKikimrSchemeOp.TCdcStreamDescription.Name", LeafUnderSibling, Target)                     \
    X(Implicit_CreateCdcStream_PqGroupUnderStream, "CreateCdcStream.<pqGroupUnderStream>", "",      \
        Implicit, Dependency)                                                                       \
    X(AlterCdcStream_TableName, "AlterCdcStream.TableName",                                         \
        "NKikimrSchemeOp.TAlterCdcStream.TableName", PathUnderWorkingDir, Parent)                   \
    X(AlterCdcStream_StreamName, "AlterCdcStream.StreamName",                                       \
        "NKikimrSchemeOp.TAlterCdcStream.StreamName", LeafUnderSibling, Target)                     \
    X(DropCdcStream_TableName, "DropCdcStream.TableName",                                           \
        "NKikimrSchemeOp.TDropCdcStream.TableName", PathUnderWorkingDir, Parent)                    \
    X(DropCdcStream_StreamName, "DropCdcStream.StreamName[{i}]",                                    \
        "NKikimrSchemeOp.TDropCdcStream.StreamName", LeafUnderSibling, Target)                      \
    X(RotateCdcStream_TableName, "RotateCdcStream.TableName",                                       \
        "NKikimrSchemeOp.TRotateCdcStream.TableName", PathUnderWorkingDir, Parent)                  \
    X(RotateCdcStream_OldStreamName, "RotateCdcStream.OldStreamName",                               \
        "NKikimrSchemeOp.TRotateCdcStream.OldStreamName", LeafUnderSibling, Source)                 \
    X(RotateCdcStream_NewStream_Name, "RotateCdcStream.NewStream.StreamDescription.Name",           \
        "NKikimrSchemeOp.TCdcStreamDescription.Name", LeafUnderSibling, Target)                     \
    X(MoveTable_SrcPath, "MoveTable.SrcPath", "NKikimrSchemeOp.TMove.SrcPath", Absolute, Source)     \
    X(MoveTable_DstPath, "MoveTable.DstPath", "NKikimrSchemeOp.TMove.DstPath", Absolute, Target)     \
    X(Implicit_MoveTable_Children, "MoveTable.<indexes,implTables,cdcStreams>", "",                 \
        Implicit, Dependency)                                                                       \
    X(MoveTableIndex_SrcPath, "MoveTableIndex.SrcPath", "NKikimrSchemeOp.TMove.SrcPath",            \
        Absolute, Source)                                                                           \
    X(MoveTableIndex_DstPath, "MoveTableIndex.DstPath", "NKikimrSchemeOp.TMove.DstPath",            \
        Absolute, Target)                                                                           \
    X(Implicit_MoveTableIndex_Children, "MoveTableIndex.<indexImplTables,sequences>", "",           \
        Implicit, Dependency)                                                                       \
    X(MoveSequence_SrcPath, "MoveSequence.SrcPath", "NKikimrSchemeOp.TMove.SrcPath",                \
        Absolute, Source)                                                                           \
    X(MoveSequence_DstPath, "MoveSequence.DstPath", "NKikimrSchemeOp.TMove.DstPath",                \
        Absolute, Target)                                                                           \
    X(Sequence_Name, "Sequence.Name", "NKikimrSchemeOp.TSequenceDescription.Name",                  \
        LeafUnderWorkingDir, Target)                                                                \
    X(CopySequence_CopyFrom, "CopySequence.CopyFrom", "NKikimrSchemeOp.TCopySequence.CopyFrom",     \
        Absolute, Source)                                                                           \
    X(Replication_Name, "Replication.Name", "NKikimrSchemeOp.TReplicationDescription.Name",         \
        LeafUnderWorkingDir, Target)                                                                \
    X(Replication_TransferTarget_DstPath, "Replication.Config.TransferSpecific.Target.DstPath",     \
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DstPath",                  \
        Absolute, Dependency)                                                                       \
    X(Replication_TransferTarget_DirectoryPath,                                                     \
        "Replication.Config.TransferSpecific.Target.DirectoryPath",                                 \
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DirectoryPath",            \
        Absolute, Dependency)                                                                       \
    X(Replication_SpecificTarget_DstPath, "Replication.Config.Specific.Targets[{i}].DstPath",       \
        "NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.DstPath",                    \
        Absolute, Dependency)                                                                       \
    X(Replication_AlterTransfer_DirectoryPath, "Replication.AlterTransfer.DirectoryPath",           \
        "NKikimrSchemeOp.TReplicationDescription.TAlterTransfer.DirectoryPath",                     \
        Absolute, Dependency)                                                                       \
    X(AlterReplication_PathId, "AlterReplication.PathId", "", ById, Target)                          \
    X(AlterReplication_Name, "AlterReplication.Name",                                               \
        "NKikimrSchemeOp.TReplicationDescription.Name", LeafUnderWorkingDir, Target)                \
    X(AlterReplication_TransferTarget_DstPath,                                                      \
        "AlterReplication.Config.TransferSpecific.Target.DstPath",                                  \
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DstPath",                  \
        Absolute, Dependency)                                                                       \
    X(AlterReplication_TransferTarget_DirectoryPath,                                                \
        "AlterReplication.Config.TransferSpecific.Target.DirectoryPath",                            \
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DirectoryPath",            \
        Absolute, Dependency)                                                                       \
    X(AlterReplication_SpecificTarget_DstPath,                                                      \
        "AlterReplication.Config.Specific.Targets[{i}].DstPath",                                    \
        "NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.DstPath",                    \
        Absolute, Dependency)                                                                       \
    X(AlterReplication_AlterTransfer_DirectoryPath,                                                 \
        "AlterReplication.AlterTransfer.DirectoryPath",                                             \
        "NKikimrSchemeOp.TReplicationDescription.TAlterTransfer.DirectoryPath",                     \
        Absolute, Dependency)                                                                       \
    X(BlobDepot_Name, "BlobDepot.Name", "NKikimrSchemeOp.TBlobDepotDescription.Name",               \
        LeafUnderWorkingDir, Target)                                                                \
    X(MoveIndex_TablePath, "MoveIndex.TablePath", "NKikimrSchemeOp.TMoveIndex.TablePath",           \
        Absolute, Parent)                                                                           \
    X(MoveIndex_SrcPath, "MoveIndex.SrcPath", "NKikimrSchemeOp.TMoveIndex.SrcPath",                 \
        LeafUnderSibling, Source)                                                                   \
    X(MoveIndex_DstPath, "MoveIndex.DstPath", "NKikimrSchemeOp.TMoveIndex.DstPath",                 \
        LeafUnderSibling, Target)                                                                   \
    X(Implicit_MoveIndex_IndexImplTables, "MoveIndex.<indexImplTables>", "",                        \
        Implicit, Dependency)                                                                       \
    X(CreateExternalTable_Name, "CreateExternalTable.Name",                                         \
        "NKikimrSchemeOp.TExternalTableDescription.Name", LeafUnderWorkingDir, Target)              \
    X(CreateExternalTable_DataSourcePath, "CreateExternalTable.DataSourcePath",                     \
        "NKikimrSchemeOp.TExternalTableDescription.DataSourcePath", Absolute, Dependency)           \
    X(CreateExternalDataSource_Name, "CreateExternalDataSource.Name",                               \
        "NKikimrSchemeOp.TExternalDataSourceDescription.Name", LeafUnderWorkingDir, Target)         \
    X(InitiateColumnBuild_Table, "InitiateColumnBuild.Table",                                       \
        "NKikimrIndexBuilder.TColumnBuildSettings.Table", Absolute, Target)                         \
    X(DropColumnBuild_Settings_Table, "DropColumnBuild.Settings.Table",                             \
        "NKikimrIndexBuilder.TColumnBuildSettings.Table", Absolute, Target)                         \
    X(CreateView_Name, "CreateView.Name", "NKikimrSchemeOp.TViewDescription.Name",                  \
        LeafUnderWorkingDir, Target)                                                                \
    X(CreateContinuousBackup_TableName, "CreateContinuousBackup.TableName",                         \
        "NKikimrSchemeOp.TCreateContinuousBackup.TableName", LeafUnderWorkingDir, Target)           \
    X(CreateContinuousBackup_StreamName,                                                            \
        "CreateContinuousBackup.ContinuousBackupDescription.StreamName",                            \
        "NKikimrSchemeOp.TContinuousBackupDescription.StreamName", LeafUnderSibling, Target)        \
    X(Implicit_CreateContinuousBackup_CdcStream, "CreateContinuousBackup.<cdcStream>", "",          \
        Implicit, Dependency)                                                                       \
    X(AlterContinuousBackup_TableName, "AlterContinuousBackup.TableName",                           \
        "NKikimrSchemeOp.TAlterContinuousBackup.TableName", PathUnderWorkingDirSplit, Target)       \
    X(AlterContinuousBackup_TakeIncrementalBackup_DstPath,                                          \
        "AlterContinuousBackup.TakeIncrementalBackup.DstPath",                                      \
        "NKikimrSchemeOp.TAlterContinuousBackup.TTakeIncrementalBackup.DstPath",                    \
        PathUnderWorkingDirSplit, Target)                                                           \
    X(AlterContinuousBackup_TakeIncrementalBackup_DstStreamPath,                                    \
        "AlterContinuousBackup.TakeIncrementalBackup.DstStreamPath",                                \
        "NKikimrSchemeOp.TAlterContinuousBackup.TTakeIncrementalBackup.DstStreamPath",              \
        LeafUnderSibling, Target)                                                                   \
    X(Implicit_AlterContinuousBackup_IncrementalBackupTable,                                        \
        "AlterContinuousBackup.<incrementalBackupTable>", "", Implicit, Dependency)                 \
    X(DropContinuousBackup_TableName, "DropContinuousBackup.TableName",                             \
        "NKikimrSchemeOp.TDropContinuousBackup.TableName", LeafUnderWorkingDir, Target)             \
    X(CreateResourcePool_Name, "CreateResourcePool.Name",                                           \
        "NKikimrSchemeOp.TResourcePoolDescription.Name", LeafUnderWorkingDir, Target)               \
    X(RestoreMultipleIncrementalBackups_SrcTablePaths,                                              \
        "RestoreMultipleIncrementalBackups.SrcTablePaths[{i}]",                                     \
        "NKikimrSchemeOp.TRestoreMultipleIncrementalBackups.SrcTablePaths", Absolute, Source)       \
    X(RestoreMultipleIncrementalBackups_DstTablePath,                                               \
        "RestoreMultipleIncrementalBackups.DstTablePath",                                           \
        "NKikimrSchemeOp.TRestoreMultipleIncrementalBackups.DstTablePath", Absolute, Target)        \
    X(CreateBackupCollection_Name, "CreateBackupCollection.Name",                                   \
        "NKikimrSchemeOp.TBackupCollectionDescription.Name", LeafUnderWorkingDir, Target)           \
    X(CreateBackupCollection_Entry_Path,                                                            \
        "CreateBackupCollection.ExplicitEntryList.Entries[{i}].Path",                               \
        "NKikimrSchemeOp.TBackupCollectionDescription.TBackupEntry.Path", Absolute, Dependency)     \
    X(AlterBackupCollection_Name, "AlterBackupCollection.Name",                                     \
        "NKikimrSchemeOp.TBackupCollectionDescription.Name", LeafUnderWorkingDir, Target)           \
    X(DropBackupCollection_Name, "DropBackupCollection.Name",                                       \
        "NKikimrSchemeOp.TBackupCollectionDescription.Name", LeafUnderWorkingDir, Target)           \
    X(Implicit_DropBackupCollection_Entries, "DropBackupCollection.<collectionEntries>", "",        \
        Implicit, Dependency)                                                                       \
    X(BackupBackupCollection_Name, "BackupBackupCollection.Name",                                   \
        "NKikimrSchemeOp.TBackupBackupCollection.Name", LeafUnderWorkingDir, Target)                \
    X(Implicit_BackupBackupCollection_Entries, "BackupBackupCollection.<collectionEntries>", "",    \
        Implicit, Dependency)                                                                       \
    X(BackupIncrementalBackupCollection_Name, "BackupIncrementalBackupCollection.Name",             \
        "NKikimrSchemeOp.TBackupBackupCollection.Name", LeafUnderWorkingDir, Target)                \
    X(Implicit_BackupIncrementalBackupCollection_Entries,                                           \
        "BackupIncrementalBackupCollection.<collectionEntries>", "", Implicit, Dependency)          \
    /* CreateFullBackupOp: WorkingDir already points at the backup collection.  */                   \
    /* Reported as an entry, unlike the WorkingDir of every other request.      */                   \
    X(WorkingDirItself, "<WorkingDir>", "", PathUnderWorkingDir, Target)                             \
    X(Implicit_CreateFullBackupOp_Entries, "CreateFullBackupOp.<collectionEntries>", "",            \
        Implicit, Dependency)                                                                       \
    X(RestoreBackupCollection_Name, "RestoreBackupCollection.Name",                                 \
        "NKikimrSchemeOp.TBackupBackupCollection.Name", LeafUnderWorkingDir, Target)                \
    X(Implicit_RestoreBackupCollection_Entries, "RestoreBackupCollection.<collectionEntries>", "",  \
        Implicit, Dependency)                                                                       \
    X(CreateSysView_Name, "CreateSysView.Name", "NKikimrSchemeOp.TSysViewDescription.Name",         \
        LeafUnderWorkingDir, Target)                                                                \
    X(ChangePathState_Path, "ChangePathState.Path", "NKikimrSchemeOp.TChangePathState.Path",        \
        PathUnderWorkingDir, Target)                                                                \
    X(IncrementalRestoreLockTargets_DstPaths, "IncrementalRestoreLockTargets.DstPaths[{i}]",        \
        "NKikimrSchemeOp.TIncrementalRestoreLockTargets.DstPaths", PathUnderWorkingDir, Target)     \
    X(IncrementalRestoreLockTargets_SrcPaths, "IncrementalRestoreLockTargets.SrcPaths[{i}]",        \
        "NKikimrSchemeOp.TIncrementalRestoreLockTargets.SrcPaths", PathUnderWorkingDir, Source)     \
    X(Implicit_IncrementalRestoreFinalize_PersistedState,                                           \
        "IncrementalRestoreFinalize.<persistedRestoreState>", "", Implicit, Target)                 \
    X(CreateSecret_Name, "CreateSecret.Name", "NKikimrSchemeOp.TSecretSchemaOp.Name",               \
        LeafUnderWorkingDir, Target)                                                                \
    X(AlterSecret_Name, "AlterSecret.Name", "NKikimrSchemeOp.TSecretSchemaOp.Name",                 \
        LeafUnderWorkingDir, Target)                                                                \
    X(CreateStreamingQuery_Name, "CreateStreamingQuery.Name",                                       \
        "NKikimrSchemeOp.TStreamingQueryDescription.Name", LeafUnderWorkingDir, Target)             \
    X(TruncateTable_TableName, "TruncateTable.TableName",                                           \
        "NKikimrSchemeOp.TTruncateTable.TableName", PathUnderWorkingDir, Target)                    \
    X(CreateTestShardSet_Name, "CreateTestShardSet.Name",                                           \
        "NKikimrSchemeOp.TCreateTestShardSet.Name", LeafUnderWorkingDir, Target)                    \
    /* Not emitted by ExtractPathRefs: TModifyScheme.ApplyIf is a precondition on                   \
       the source schemeshard's path ids and versions, not a path the operation                     \
       touches. The row exists so StripSourceLocalPreconditions can name what it                    \
       removed. */                                                                                  \
    X(ApplyIf_PathId, "ApplyIf[{i}].PathId", "", ById, Dependency)

// Compile-time identity of one path-carrying field. Replaces the field-path
// string that TPathRef used to carry: rendering it is now a resolve-time
// concern, not an extraction-time allocation.
enum class EPathField : ui16 {
#define SCHEMESHARD_PATH_FIELD_ENUMERATOR(name, tpl, proto, kind, role) name,
    SCHEMESHARD_PATH_FIELDS(SCHEMESHARD_PATH_FIELD_ENUMERATOR)
#undef SCHEMESHARD_PATH_FIELD_ENUMERATOR
    Count
};

// The field-path template of a field, with "{i}", "{j}" and "{key}" still
// unexpanded. Equal to the rendered field path for a field that has no
// placeholder, which is most of them.
TStringBuf PathFieldName(EPathField field);

TStringBuf PathFieldProtoName(EPathField field);
EPathRefKind PathFieldDefaultKind(EPathField field);
EPathRefRole PathFieldDefaultRole(EPathField field);

// One path-carrying field of one TModifyScheme, before any resolution.
//
// Lifetime: MapKey, Value and BasePath are views into the TModifyScheme that
// ExtractPathRefs was given, or into the TPathRefs that owns this ref. Both
// must outlive the ref. ResolvePathFootprint copies everything it keeps into
// TPathFootprintEntry, which owns its strings.
struct TPathRef {
    EPathField Field = EPathField::Count;
    // Repeated-field positions rendered into the field path as "{i}"/"{j}".
    ui32 Index = Max<ui32>();
    ui32 SubIndex = Max<ui32>();
    // Map key rendered into the field path as "{key}".
    TStringBuf MapKey;
    TStringBuf Value;
    // ById only. OwnerId == 0 means "this schemeshard" (a local path id).
    ui64 OwnerId = 0;
    ui64 LocalPathId = 0;
    EPathRefKind Kind = EPathRefKind::LeafUnderWorkingDir;
    EPathRefRole Role = EPathRefRole::Target;
    // LeafUnderSibling only: the raw value of the sibling field Value hangs off.
    TStringBuf BasePath;
    // Index, within the same ExtractPathRefs result, of the ref this one hangs
    // off; -1 when there is none. For Implicit it is the anchor of the
    // runtime-derived set (the path whose children the operation will actually
    // touch). For LeafUnderSibling it is the base, used instead of BasePath
    // when the base cannot be written as a raw string (the base field is
    // addressed by path id, or is itself resolved with TSplitChildTag).
    int AnchorIndex = -1;
};

// The field path of a ref: its template with "{i}", "{j}" and "{key}" replaced
// by Index, SubIndex and MapKey. Allocates; call it once per ref, at resolve
// time, not inside the extractor.
TString FieldPath(const TPathRef& ref);

// The result of ExtractPathRefs: the refs plus stable storage for the few base
// paths that are computed rather than read straight out of the request. Behaves
// like a const TVector<TPathRef>.
struct TPathRefs {
    TVector<TPathRef> Refs;
    // A TDeque never relocates its elements, so a TStringBuf into one stays
    // valid however many more refs are appended.
    TDeque<TString> Owned;

    size_t size() const { return Refs.size(); }
    bool empty() const { return Refs.empty(); }
    const TPathRef& operator[](size_t i) const { return Refs[i]; }
    TVector<TPathRef>::const_iterator begin() const { return Refs.begin(); }
    TVector<TPathRef>::const_iterator end() const { return Refs.end(); }
};

// Layer 1: pure, state-free extraction. Covers every EOperationType. Allocates
// nothing per string: every value is a view into tx or into the result itself.
TPathRefs ExtractPathRefs(const NKikimrSchemeOp::TModifyScheme& tx);

// Every protobuf field ExtractPathRefs reads as a path, fully qualified, e.g.
// "NKikimrSchemeOp.TMove.SrcPath". Generated from SCHEMESHARD_PATH_FIELDS,
// deduplicated and sorted. Consumed by the descriptor-walk completeness test,
// which fails when a path-like field of TModifyScheme is neither listed here
// nor explicitly classified as not-a-path.
const TVector<TStringBuf>& KnownPathFieldNames();

// An owning snapshot of the TPathRef an entry came from. A TPathRef points into
// the request proto; a footprint outlives it.
struct TPathRefOwned {
    EPathField Field = EPathField::Count;
    // Rendered once, here, from the field's template.
    TString FieldPath;
    // The repeated-field position, kept beside the rendered field path because
    // RelocatePaths has to address the same element again to write to it.
    // SubIndex and MapKey are not kept: no field that carries one of those is
    // ever rewritten.
    ui32 Index = Max<ui32>();
    TString Value;
    ui64 OwnerId = 0;
    ui64 LocalPathId = 0;
    EPathRefKind Kind = EPathRefKind::LeafUnderWorkingDir;
    EPathRefRole Role = EPathRefRole::Target;
    TString BasePath;
    int AnchorIndex = -1;
};

struct TPathFootprintEntry {
    TPathRefOwned Ref;
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
    // The request's WorkingDir exactly as the client spelled it.
    TString WorkingDir;
    // The same directory after TPath::Resolve, which is what every AbsPath
    // below is built from. The raw string may be non-canonical, so a prefix
    // test against it can silently fail; test against this one.
    TString WorkingDirCanon;
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

////////////////////////////////////////////////////////////////////////////////
// Layer 3: rewriting a request.
//
// Both rewriters take a TModifyScheme and the TPathFootprint that
// ResolvePathFootprint produced *from that same request*, on the schemeshard
// that owns the paths. The footprint is what turns a path id into a path
// string and a raw value into an absolute one; without it neither rewrite is
// possible without a second schemeshard walk.
//
// Never mutate a part's Transaction. TOperation parts hold references into the
// bytes the client sent and Propose() is entitled to keep seeing them; every
// caller must rewrite a copy.

// What CanonicalizeToPaths could not rewrite: one entry per by-id field whose
// path id did not resolve on this schemeshard. Such a request would be
// rejected anyway, so the caller decides whether to drop it or keep it as-is.
struct TCanonicalizeResult {
    bool Changed = false;
    TVector<EPathField> Untransformable;
};

// Rewrite by-id addressing into the equivalent by-name form: WorkingDir plus a
// leaf name for the Drop/Alter* families, an absolute TablePath for SplitMerge.
//
// The id form wins over the name form in every Propose() that accepts both, so
// the id field is always cleared; writing a name beside a live path id would
// change nothing.
TCanonicalizeResult CanonicalizeToPaths(NKikimrSchemeOp::TModifyScheme& tx, const TPathFootprint& fp);

// Where the request's database is moving. Both paths are absolute and
// canonical, e.g. "/MyRoot/db1" -> "/MyRoot2/dir/db2".
struct TRelocation {
    TString OldDatabasePath;
    TString NewDatabasePath;
};

struct TRelocateResult {
    bool Changed = false;
    // By-id fields found in the footprint. An id means nothing in the new
    // database, so the caller must run CanonicalizeToPaths first; anything
    // listed here was left untouched.
    TVector<EPathField> Skipped;
};

// Rewrite every path the request spells out so that it points into
// r.NewDatabasePath instead of r.OldDatabasePath.
//
// Only values that name a path outside the working dir are rewritten:
// Absolute always, PathUnderWorkingDir only when the raw value starts with a
// slash. Leaf names, split children and sibling leaves ride along on the
// WorkingDir rewrite and are deliberately left alone; rewriting both a base
// and its leaf would double-apply the move. Paths that do not live under
// r.OldDatabasePath are left alone too, which is what keeps a replication
// SrcPath (a path on a remote cluster) safe even before noting that the
// extractor never emits it: only fields the extractor emitted are ever
// written, and the setter table has a row for none but those.
TRelocateResult RelocatePaths(NKikimrSchemeOp::TModifyScheme& tx, const TPathFootprint& fp,
    const TRelocation& r);

// Whether RelocatePaths knows how to write a new value into this field. True
// for exactly the fields that can carry a relocatable path; asserted field by
// field by the tests.
bool CanRelocatePathField(EPathField field);

// Drop the preconditions that only mean something on the schemeshard the
// request was proposed to, and return one entry per precondition removed.
//
// Today that is TModifyScheme.ApplyIf, which pins path ids, path versions and
// lock ids of the source schemeshard. It has no name form, so it cannot be
// canonicalized, only stripped. This is policy, not a semantic no-op: the
// request loses its optimistic-concurrency check and may succeed where the
// original would have been rejected. A consumer that needs the check back has
// to re-derive it against the target state.
TVector<EPathField> StripSourceLocalPreconditions(NKikimrSchemeOp::TModifyScheme& tx);

}  // namespace NKikimr::NSchemeShard
