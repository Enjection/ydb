# S1 — Field inventory per SchemeShard operation type

Scope: for every `NKikimrSchemeOp::EOperationType` (136 values, enumerated from
the `switch` in `ExtractChangingPaths`, `ydb/core/tx/schemeshard/schemeshard_audit_log_fragment.cpp:327-744`),
identify which `TModifyScheme` proto field(s) carry the path(s) the operation
touches, how each is interpreted, and whether `ExtractChangingPaths` computes
the same thing the actual sub-operation `Propose()` code computes.

Method: read `ExtractChangingPaths` in full (136 cases, all extracted below),
then cross-checked ~75 of the 136 op types directly against their `Propose()`
/ factory source (grep + read on `schemeshard__operation_*.cpp`,
`ydb/core/tx/schemeshard/index/*.cpp`, `ydb/core/tx/schemeshard/olap/operations/*.cpp`).
The remaining ~55 rows — almost all sharing the generic `TDrop`
(`WorkingDir + Drop.Name`, with a `HasId()` numeric-id branch) or a plain
`WorkingDir + <SubMessage>.Name` shape that is the dominant SchemeShard
convention — are classified by that verified convention; rows not opened
individually are marked "(convention, not opened)" in the parity column
rather than asserted as directly confirmed. `ConstructParts`/
`MakeOperationParts` dispatch (`schemeshard__operation.cpp:1370-1722`) was
read in full to get the factory function and to see which op types are
**internal-only** (the dispatch switch does `Y_ABORT("multipart operations
are handled before...")` or `Y_ABORT("TODO: implement")` for them — such an
op type can never be the *direct* op type of a client-submitted
`TModifyScheme`; it only appears as an internally-synthesized part).

Kind vocabulary (per the plan): `LeafUnderWorkingDir`, `PathUnderWorkingDir`
(field may itself contain `/`), `Absolute`, `LeafUnderSibling(field)`,
`ById(field)` (numeric id branch, bypasses Name/WorkingDir), `Implicit`
(touched path not present in the proto at all).

---

## 1. Full per-op-type table

| # | OpType | Source file:line | Field path(s) | Kind | Role | ExtractChangingPaths parity |
|---|---|---|---|---|---|---|
| 1 | ESchemeOpMkDir | schemeshard__operation_mkdir.cpp:134 | `MkDir.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 2 | ESchemeOpCreateTable | schemeshard__operation_create_table.cpp:444-508 | `CreateTable.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 3 | ESchemeOpCreatePersQueueGroup | schemeshard__operation_create_pq.cpp:299-342 | `CreatePersQueueGroup.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 4 | ESchemeOpDropTable | index/operation_drop_indexed_table.cpp:400-402 | `Drop.Name` / `Drop.Id` (ById) | LeafUnderWorkingDir / ById | Target | **Mismatch: `Id` branch not reflected** (verified) |
| 5 | ESchemeOpDropPersQueueGroup | schemeshard__operation_drop_pq.cpp:325-341 | `Drop.Name` / `Drop.Id` | LeafUnderWorkingDir / ById | Target | **Mismatch: `Id` branch not reflected** (verified) |
| 6 | ESchemeOpAlterTable | schemeshard__operation_alter_table.cpp:604-629 | `AlterTable.Name` / `AlterTable.PathId` / `Id_Deprecated` (ById) | LeafUnderWorkingDir / ById | Target | **Mismatch: id branch not reflected** (verified) |
| 7 | ESchemeOpAlterPersQueueGroup | schemeshard__operation_alter_pq.cpp:629-653 | `AlterPersQueueGroup.Name` (+ `HasId()` ById, see cols below) | LeafUnderWorkingDir / ById | Target | **Mismatch: id branch not reflected** (verified, ternary at line 653) |
| 8 | ESchemeOpModifyACL | schemeshard__operation_modify_acl.cpp:37 | `ModifyACL.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 9 | ESchemeOpRmDir | schemeshard__operation_rmdir.cpp:29-34 | `Drop.Name` / `Drop.Id` | LeafUnderWorkingDir / ById | Target | **Mismatch: `Id` branch not reflected** (verified) |
| 10 | ESchemeOpSplitMergeTablePartitions | schemeshard__operation_split_merge.cpp:817-857 | `SplitMergeTablePartitions.TablePath` (absolute) / `TableLocalId`+`TableOwnerId` (ById) | **Absolute** / ById | Target | **Mismatch (real bug candidate): audit does `JoinPath(WorkingDir, TablePath)` but `Propose()` resolves `TablePath` directly with no `WorkingDir` join at all; the id-addressed branch is also not reflected** (verified) |
| 11 | ESchemeOpBackup | schemeshard__operation_backup_restore_common.h:716-748 | `Backup.TableName` | LeafUnderWorkingDir | Target | OK (verified, shared helper w/ Restore) |
| 12 | ESchemeOpCreateSubDomain | schemeshard__operation_create_subdomain.cpp | `SubDomain.Name` | LeafUnderWorkingDir | Target | OK (convention; sibling AlterSubDomain/ExtSubDomain verified) |
| 13 | ESchemeOpDropSubDomain | schemeshard__operation_drop_subdomain.cpp | `Drop.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 14 | ESchemeOpCreateRtmrVolume | schemeshard__operation_create_rtmr.cpp | `CreateRtmrVolume.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 15 | ESchemeOpCreateBlockStoreVolume | schemeshard__operation_create_bsv.cpp | `CreateBlockStoreVolume.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 16 | ESchemeOpAlterBlockStoreVolume | schemeshard__operation_alter_bsv.cpp:394-422 | `AlterBlockStoreVolume.Name` / id (ById) | LeafUnderWorkingDir / ById | Target | **Mismatch: id branch not reflected** (verified, ternary at line 422) |
| 17 | ESchemeOpAssignBlockStoreVolume | schemeshard__operation_assign_bsv.cpp | `AssignBlockStoreVolume.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 18 | ESchemeOpDropBlockStoreVolume | schemeshard__operation_drop_bsv.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not individually opened) |
| 19 | ESchemeOpCreateKesus | schemeshard__operation_create_kesus.cpp | `Kesus.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 20 | ESchemeOpDropKesus | schemeshard__operation_drop_kesus.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 21 | ESchemeOpForceDropSubDomain | schemeshard__operation_upgrade_subdomain.cpp:1514 (`CreateCompatibleSubdomainDrop`) | `Drop.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 22 | ESchemeOpCreateSolomonVolume | schemeshard__operation_create_solomon.cpp:246-287 | `CreateSolomonVolume.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 23 | ESchemeOpDropSolomonVolume | schemeshard__operation_drop_solomon.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 24 | ESchemeOpAlterKesus | schemeshard__operation_alter_kesus.cpp | `Kesus.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 25 | ESchemeOpAlterSubDomain | schemeshard__operation_alter_subdomain.cpp:82-107 | `SubDomain.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 26 | ESchemeOpAlterUserAttributes | schemeshard__operation_alter_user_attrs.cpp:40 | `AlterUserAttributes.PathName` | PathUnderWorkingDir | Target | OK (verified; field name is `PathName`, may contain `/`) |
| 27 | ESchemeOpForceDropUnsafe | schemeshard__operation_drop_unsafe.cpp:170-172 | `Drop.Name` / `Drop.Id` | LeafUnderWorkingDir / ById | Target | **Mismatch: `Id` branch not reflected** (verified) |
| 28 | ESchemeOpCreateIndexedTable | index/operation_create_indexed_table.cpp:142-159,320-513 | `CreateIndexedTable.TableDescription.Name` (Target); index/impl-table/sequence/prefix children computed as `WorkingDir/BaseTable/IndexName[/...]` but **not in the request as separate fields** | PathUnderWorkingDir (base table) + Implicit (children) | Target + Dependency(children) | Gap: audit captures only the base-table name; index/impl-table/sequence children are Implicit (verified, matches plan §1a) |
| 29 | ESchemeOpCreateTableIndex | index/operation_create_index.cpp:112-133 | `CreateTableIndex.Name` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only**: main dispatch does `Y_ABORT("is handled as part of ESchemeOpCreateIndexedTable")` |
| 30 | ESchemeOpCreateConsistentCopyTables | schemeshard__operation_consistent_copy_tables.cpp:185-210 | `CopyTableDescriptions[i].SrcPath` (absolute) / `.DstPath` (absolute); nested `CreateSrcCdcStream`/`IndexImplTableCdcStreams` (map, per-item) | Absolute (Src/Dst) + Implicit (CDC children) | Source(Src) + Target(Dst) + Dependency(implicit CDC) | **Mismatch: audit emits only `DstPath` per item — `SrcPath` and every implicit CDC-stream/impl-table child are dropped entirely** (verified, matches plan §1a) |
| 31 | ESchemeOpDropTableIndex | index/operation_drop_index.cpp | `Drop.Name` | LeafUnderWorkingDir | Target | OK (convention). **Internal-only**: `Y_ABORT("is handled as part of ESchemeOpDropTable")` |
| 32 | ESchemeOpCreateExtSubDomain | schemeshard__operation_create_extsubdomain.cpp:51-99 | `SubDomain.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 33 | ESchemeOpAlterExtSubDomain | schemeshard__operation_alter_extsubdomain.cpp | `SubDomain.Name` | LeafUnderWorkingDir | Target | OK (convention, shares field w/ #32/#34) |
| 34 | ESchemeOpAlterExtSubDomainCreateHive | (derived from AlterExtSubDomain) | `SubDomain.Name` | LeafUnderWorkingDir | Target | OK (convention). **Internal-only**: `Y_ABORT("multipart operations are handled before")` |
| 35 | ESchemeOpForceDropExtSubDomain | schemeshard__operation_drop_extsubdomain.cpp | `Drop.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 36 | ESchemeOp_DEPRECATED_35 | n/a | none | Implicit | n/a | OK — dead enum value; dispatch does `Y_ABORT("impossible")`; audit case body is empty (both agree) |
| 37 | ESchemeOpUpgradeSubDomain | schemeshard__operation_upgrade_subdomain.cpp:1127-1140 | `UpgradeSubDomain.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 38 | ESchemeOpUpgradeSubDomainDecision | schemeshard__operation_upgrade_subdomain.cpp:1378-1390 | `UpgradeSubDomain.Name` | LeafUnderWorkingDir | Target | OK (verified, shares field w/ #37) |
| 39 | ESchemeOpCreateIndexBuild | index/operation_create_build_index.cpp:23,109 | `InitiateIndexBuild.Table` (absolute) + `.Index.Name` (leaf under Table) | Absolute + LeafUnderSibling(Table) | Dependency(Table) + Target(Index) | OK (verified; audit's raw `JoinPath({Table, Index.Name})` matches) |
| 40 | ESchemeOpInitiateBuildIndexMainTable | index/operation_initiate_build_index.cpp:303-313 | `InitiateBuildIndexMainTable.TableName` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only** |
| 41 | ESchemeOpPrepareIndexValidation | index/operation_prepare_index_validation.cpp:280-290 | `PrepareIndexValidation.TableName` | LeafUnderWorkingDir | Target | OK (verified) |
| 42 | ESchemeOpCreateLock | schemeshard__operation_create_lock.cpp:96-135 | `LockConfig.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 43 | ESchemeOpApplyIndexBuild | index/operation_apply_build_index.cpp:93-96,181-184 | `ApplyIndexBuild.TablePath` (absolute) + `.IndexName` (leaf under TablePath) | Absolute + LeafUnderSibling(TablePath) | Dependency(Table) + Target(Index) | OK (verified) |
| 44 | ESchemeOpFinalizeBuildIndexMainTable | index/operation_finalize_build_index.cpp:305-315 | `FinalizeBuildIndexMainTable.TableName` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only** |
| 45 | ESchemeOpAlterTableIndex | index/operation_alter_index.cpp:112-133 | `AlterTableIndex.Name` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only**: `Y_ABORT("multipart operations are handled before")` |
| 46 | ESchemeOpAlterSolomonVolume | schemeshard__operation_alter_solomon.cpp | `AlterSolomonVolume.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 47 | ESchemeOpDropLock | schemeshard__operation_drop_lock.cpp | `LockConfig.Name` | LeafUnderWorkingDir | Target | OK (convention, shares field w/ CreateLock) |
| 48 | ESchemeOpFinalizeBuildIndexImplTable | schemeshard__operation.cpp:1466 area | `AlterTable.Name` (submessage reused) | LeafUnderWorkingDir | Target | OK (convention — audit deliberately reuses `AlterTable`, matches how the derived part's proto is built). **Internal-only** |
| 49 | ESchemeOpInitiateBuildIndexImplTable | schemeshard__operation_create_table.cpp:870 | `CreateTable.Name` (submessage reused) | LeafUnderWorkingDir | Target | OK (convention). **Internal-only** |
| 50 | ESchemeOpDropIndex | index/operation_drop_index.cpp:256-278 | `DropIndex.TableName` (Parent) + `.IndexName` (leaf under TableName) | PathUnderWorkingDir + LeafUnderSibling(TableName) | Parent + Target | OK (verified) |
| 51 | ESchemeOpDropTableIndexAtMainTable | index/operation_drop_index.cpp | `DropIndex.TableName` | PathUnderWorkingDir | Target | OK (verified). **Internal-only** |
| 52 | ESchemeOpCancelIndexBuild | index/operation_apply_build_index.cpp:177-184 | `CancelIndexBuild.TablePath` (absolute) + `.IndexName` | Absolute + LeafUnderSibling(TablePath) | Dependency + Target | OK (verified) |
| 53 | ESchemeOpCreateFileStore | schemeshard__operation_create_fs.cpp | `CreateFileStore.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 54 | ESchemeOpAlterFileStore | schemeshard__operation_alter_fs.cpp | `AlterFileStore.Name` | LeafUnderWorkingDir | Target | OK (convention, not opened) |
| 55 | ESchemeOpDropFileStore | schemeshard__operation_drop_fs.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 56 | ESchemeOpRestore | schemeshard__operation_backup_restore_common.h:716-748 | `Restore.TableName` | LeafUnderWorkingDir | Target | OK (verified, shares helper w/ Backup) |
| 57 | ESchemeOpCreateColumnStore | olap/operations/create_store.cpp:324-373 | `CreateColumnStore.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 58 | ESchemeOpAlterColumnStore | olap/operations/alter_store.cpp:458-498 | `AlterColumnStore.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 59 | ESchemeOpDropColumnStore | olap/operations/drop_store.cpp:263-277 | `Drop.Name` / `Drop.Id` | LeafUnderWorkingDir / ById | Target | **Mismatch: `Id` branch not reflected** (verified) |
| 60 | ESchemeOpCreateColumnTable | olap/operations/create_table_with_local_indexes.cpp:11-12 | **`AlterColumnTable.Name`** (not `CreateColumnTable`!) | LeafUnderWorkingDir | Target | OK — audit deliberately reads the same (reused) submessage; verified this quirk is intentional and matches |
| 61 | ESchemeOpAlterColumnTable | olap/operations/alter_table_with_local_indexes.cpp:19-27 | `AlterColumnTable.Name` (fallback `AlterTable.Name`) | LeafUnderWorkingDir | Target | OK (verified) |
| 62 | ESchemeOpDropColumnTable | olap/operations/drop_table_with_local_indexes.cpp:12-15 | `Drop.Name` (plain `.Dive()`, no `HasId()` seen in this file) | LeafUnderWorkingDir | Target | OK (verified — this one, unlike most Drop-family ops, has no id branch in the code actually read) |
| 63 | ESchemeOpAlterLogin | schemeshard__operation_alter_login.cpp:28 | none — only checks `WorkingDir == LoginProvider.Audience` | Implicit (no path touched) | n/a | OK — audit emits `WorkingDir` itself as a placeholder; verified `Propose()` never resolves/touches a `TPath` |
| 64 | ESchemeOpCreateCdcStream | schemeshard__operation_create_cdc_stream.cpp:522-533 | `CreateCdcStream.TableName` (Parent) + `.StreamDescription.Name` (Target) | PathUnderWorkingDir + LeafUnderSibling(TableName) | Parent + Target | OK (verified) |
| 65 | ESchemeOpCreateCdcStreamImpl | derived, schemeshard__operation_create_cdc_stream.cpp:771 | `StreamDescription.Name` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only** |
| 66 | ESchemeOpCreateCdcStreamAtTable | derived, schemeshard__operation_create_cdc_stream.cpp:788 | `TableName` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only** |
| 67 | ESchemeOpAlterCdcStream | schemeshard__operation_alter_cdc_stream.cpp | `AlterCdcStream.TableName` + `.StreamName` | PathUnderWorkingDir + LeafUnderSibling | Parent + Target | OK (convention, mirrors verified CreateCdcStream structure, not individually opened) |
| 68 | ESchemeOpAlterCdcStreamImpl | derived | `StreamName` | LeafUnderWorkingDir | Target | OK (convention). **Internal-only** |
| 69 | ESchemeOpAlterCdcStreamAtTable | derived | `TableName` | LeafUnderWorkingDir | Target | OK (convention). **Internal-only** |
| 70 | ESchemeOpDropCdcStream | schemeshard__operation_drop_cdc_stream.cpp:338-361 | `DropCdcStream.TableName` (Parent) + `.StreamName[]` (Target, cascade loop) | PathUnderWorkingDir + LeafUnderSibling (repeated) | Parent + Target(×N) | OK (verified) |
| 71 | ESchemeOpDropCdcStreamImpl | derived, schemeshard__operation_drop_cdc_stream.cpp:110-122 | `Drop.Name` | LeafUnderWorkingDir | Target | OK (convention). **Internal-only** |
| 72 | ESchemeOpDropCdcStreamAtTable | derived | `TableName` | LeafUnderWorkingDir | Target | OK (verified). **Internal-only** |
| 73 | ESchemeOpRotateCdcStream | schemeshard__operation_rotate_cdc_stream.cpp:517-543 | `RotateCdcStream.TableName` (Parent) + `.OldStreamName` (Source) + `.NewStream.StreamDescription.Name` (Target) | PathUnderWorkingDir + LeafUnderSibling(×2) | Parent + Source + Target | OK (verified) |
| 74 | ESchemeOpRotateCdcStreamImpl | derived, schemeshard__operation_rotate_cdc_stream.cpp:121-141 | `OldStreamName` + `NewStream...Name` | LeafUnderWorkingDir | Source + Target | OK (verified). **Internal-only** |
| 75 | ESchemeOpRotateCdcStreamAtTable | derived | `TableName` | LeafUnderWorkingDir | Target | OK (convention). **Internal-only** |
| 76 | ESchemeOpMoveTable | schemeshard__operation_move_table.cpp:802-857 | `MoveTable.SrcPath` (absolute) + `.DstPath` (absolute, via `ResolveWithInactive`) | Absolute | Source + Target | OK (verified — matches plan §1a "known offender", confirmed genuinely absolute) |
| 77 | ESchemeOpMoveTableIndex | schemeshard__operation_common.cpp:1377 (`MoveTableIndexTask`) + index/operation_move_table_index.cpp:345-425 | `MoveTableIndex.SrcPath` + `.DstPath`, both already-resolved absolute path strings | Absolute | Source + Target | OK (verified). **Clarifies plan §1a**: the plan's note "leaf names relative to parent table" describes `MoveIndex` (#93), not `MoveTableIndex` — `MoveTableIndex.Src/DstPath` are full absolute paths |
| 78 | ESchemeOpMoveSequence | schemeshard__operation_move_sequence.cpp:771-840 | `MoveSequence.SrcPath` + `.DstPath`, absolute | Absolute | Source + Target | OK (verified) |
| 79 | ESchemeOpCreateSequence | schemeshard__operation_create_sequence.cpp:370-440 | `Sequence.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 80 | ESchemeOpAlterSequence | schemeshard__operation_alter_sequence.cpp:411-461 | `AlterSequence.Name` | LeafUnderWorkingDir | Target | **Mismatch (real bug candidate): audit's case body is empty — no path emitted at all — but `Propose()` resolves `WorkingDir/Name` via `.Child()`** (verified) |
| 81 | ESchemeOpDropSequence | schemeshard__operation_drop_sequence.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 82 | ESchemeOpCreateReplication | schemeshard__operation_create_replication.cpp:365-401 | `Replication.Name` | LeafUnderWorkingDir | Target | OK (verified; `Replication.Target[].DstPath`/`.DirectoryPath` resolved absolutely as a Dependency but not captured by audit — minor gap) |
| 83 | ESchemeOpCreateTransfer | same file as #82 (fallthrough case) | `Replication.Name` | LeafUnderWorkingDir | Target | OK (verified, shares field+case w/ #82) |
| 84 | ESchemeOpAlterReplication | schemeshard__operation_alter_replication.cpp:364-385 | `AlterReplication.Name` (leaf) / `.PathId` (ById) | LeafUnderWorkingDir / ById | Target | **Mismatch (real bug candidate): audit's case body is empty, but `Propose()` clearly resolves `WorkingDir/Name` or `PathId`** (verified) |
| 85 | ESchemeOpAlterTransfer | same file as #84 (fallthrough case) | same as #84 | LeafUnderWorkingDir / ById | Target | **Mismatch, same as #84** (verified, shared case) |
| 86 | ESchemeOpDropReplication | schemeshard__operation_drop_replication.cpp:276-288 | `Drop.Name` / `Drop.Id` (`op.HasId()`) | LeafUnderWorkingDir / ById | Target | **Mismatch: `Id` branch not reflected** (verified) |
| 87 | ESchemeOpDropReplicationCascade | same file (fallthrough) | same as #86 | LeafUnderWorkingDir / ById | Target | **Mismatch, same as #86** (verified, shared case) |
| 88 | ESchemeOpDropTransfer | same file (fallthrough) | same as #86 | LeafUnderWorkingDir / ById | Target | **Mismatch, same as #86** (verified, shared case) |
| 89 | ESchemeOpDropTransferCascade | same file (fallthrough) | same as #86 | LeafUnderWorkingDir / ById | Target | **Mismatch, same as #86** (verified, shared case) |
| 90 | ESchemeOpCreateBlobDepot | schemeshard__operation_blob_depot.cpp:241-267 | `BlobDepot.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 91 | ESchemeOpAlterBlobDepot | schemeshard__operation_blob_depot.cpp:389-393 (`ProposeAlter`, a no-op stub) | none at Propose time | Implicit | n/a | OK — verified `ProposeAlter` touches no `TPath` at all (`(void)owner, (void)context; SetState(...)`); audit also emits nothing, consistent |
| 92 | ESchemeOpDropBlobDepot | schemeshard__operation_blob_depot.cpp:395-399 (`ProposeDrop`, a no-op stub) | none at Propose time | Implicit | n/a | OK — same as #91, verified |
| 93 | ESchemeOpMoveIndex | index/operation_move_index.cpp:473-486 | `MoveIndex.TablePath` (absolute, Parent) + `.SrcPath`/`.DstPath` (leaf names under TablePath) | Absolute(TablePath) + LeafUnderSibling(TablePath) | Parent + Source + Target | OK (verified — this is the op the plan's §1a note actually describes) |
| 94 | ESchemeOpCreateExternalTable | schemeshard__operation_create_external_table.cpp:233-255 | `CreateExternalTable.Name` | LeafUnderWorkingDir | Target | OK (verified; `.DataSourcePath` resolved absolutely as a Dependency, not captured — minor gap) |
| 95 | ESchemeOpDropExternalTable | schemeshard__operation_drop_external_table.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 96 | ESchemeOpAlterExternalTable | schemeshard__operation_alter_external_table.cpp:233-260 | `externalTableDescription.Name` (i.e. `AlterExternalTable`-equivalent submessage) | LeafUnderWorkingDir | Target | OK (verified — audit's `// TODO: unimplemented` and the dispatch's `Y_ABORT("TODO: implement")` agree: this op type is unreachable as a *direct* client op; the `TAlterExternalTable` `Propose()` code exists and is correct but is only invoked internally from `CreateNewExternalTable` |
| 97 | ESchemeOpCreateExternalDataSource | schemeshard__operation_create_external_data_source.cpp:164-188 | `CreateExternalDataSource.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 98 | ESchemeOpDropExternalDataSource | schemeshard__operation_drop_external_data_source.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 99 | ESchemeOpAlterExternalDataSource | schemeshard__operation_alter_external_data_source.cpp:156-166 | `externalDataSourceDescription.Name` | LeafUnderWorkingDir | Target | OK (verified — same situation as #96: dispatch aborts for direct submission, real code only reachable as internal derived part) |
| 100 | ESchemeOpCreateColumnBuild | index/operation_create_build_index.cpp:23 | `InitiateColumnBuild.Table` | Absolute | Target | OK (verified — raw field matches raw audit expression) |
| 101 | ESchemeOpDropColumnBuild | index/operation_apply_build_index.cpp:251 | `DropColumnBuild.Settings.Table` | Absolute | Target | OK (verified) |
| 102 | ESchemeOpCreateView | schemeshard__operation_create_view.cpp:110-149 | `CreateView.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 103 | ESchemeOpDropView | schemeshard__operation_drop_view.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 104 | ESchemeOpAlterView | n/a — genuinely unimplemented | none | Implicit | n/a | OK — verified: no `TAlterView`/Propose class exists anywhere in `schemeshard__operation_create_view.cpp`; dispatch does `Y_ABORT("TODO: implement")`; audit comment agrees (`// TODO: implement`) |
| 105 | ESchemeOpCreateContinuousBackup | schemeshard__operation_create_continuous_backup.cpp:24 | `CreateContinuousBackup.TableName` | LeafUnderWorkingDir | Target | OK (verified) |
| 106 | ESchemeOpAlterContinuousBackup | schemeshard__operation_alter_continuous_backup.cpp:84-128 | `AlterContinuousBackup.TableName` (Target/Parent) + `.TakeIncrementalBackup.DstPath` (Dependency child under WorkingDir/TableName) | PathUnderWorkingDir + Implicit(backup table) | Target + Dependency | OK for the captured field (verified); the derived incremental-backup table path is Implicit and not captured |
| 107 | ESchemeOpDropContinuousBackup | schemeshard__operation_drop_continuous_backup.cpp | `DropContinuousBackup.TableName` (a distinct field, not `Drop.Name`) | LeafUnderWorkingDir | Target | OK (convention, matches audit exactly; not opened) |
| 108 | ESchemeOpCreateResourcePool | schemeshard__operation_create_resource_pool.cpp:129-145 | `CreateResourcePool.Name` | LeafUnderWorkingDir | Target | **Mismatch (real bug): audit emits the bare `Name` with no `WorkingDir` join at all; `Propose()` resolves `WorkingDir/Name` via `.Child()`** (verified) |
| 109 | ESchemeOpDropResourcePool | schemeshard__operation_drop_resource_pool.cpp:164-175 | `Drop.Name` / `Drop.Id` (`HasId()`) | LeafUnderWorkingDir / ById | Target | **Mismatch (real bug, double issue): audit emits bare `Drop.Name`, missing both the `WorkingDir` join and the `Id` branch** (verified) |
| 110 | ESchemeOpAlterResourcePool | schemeshard__operation_alter_resource_pool.cpp:120-136 | `CreateResourcePool.Name` (submessage reused) | LeafUnderWorkingDir | Target | **Mismatch: same bug as #108** (verified) |
| 111 | ESchemeOpRestoreMultipleIncrementalBackups | schemeshard__operation_create_restore_incremental_backup.cpp:17-30 | `RestoreMultipleIncrementalBackups.SrcTablePaths[]` / `.DstTablePath` (audit-side expression only) | n/a — **op type is retired** | n/a | Finding, not a path bug: `CreateRestoreMultipleIncrementalBackups(...)` **always** returns `CreateReject(..., "schema-op dispatch has been retired; the incremental restore orchestrator now uses the request/response channel")`. The op can never reach `Propose()`; the audit case is dead code for this op type today (verified) |
| 112 | ESchemeOpRestoreIncrementalBackupAtTable | schemeshard__operation_create_restore_incremental_backup.cpp:6-9 | shares audit case with #111 | n/a — **retired** | n/a | Same finding as #111: `CreateRestoreIncrementalBackupAtTable(...)` always returns `CreateReject(..., "no longer supported; use the request/response channel")` (verified) |
| 113 | ESchemeOpCreateBackupCollection | schemeshard__operation_create_backup_collection.cpp:118-120 | `CreateBackupCollection.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 114 | ESchemeOpAlterBackupCollection | dispatch: `Y_ABORT("TODO: implement")` | `AlterBackupCollection.Name` | LeafUnderWorkingDir | Target | OK — audit and dispatch agree it's unimplemented; genuinely dead for direct client use today (verified via dispatch switch) |
| 115 | ESchemeOpDropBackupCollection | schemeshard__operation_drop_backup_collection.cpp:351-353,541-550 | `DropBackupCollection.Name` | LeafUnderWorkingDir (via `NBackup::ResolveBackupCollectionPaths`) | Target | OK for the collection path (verified); cascade to the backed-up tables/streams is Implicit, not captured (matches plan §1a) |
| 116 | ESchemeOpBackupBackupCollection | schemeshard__operation_backup_backup_collection.cpp:43-45 | `BackupBackupCollection.Name` | LeafUnderWorkingDir | Target | OK for the collection path (verified); the concrete table/CDC set from `NBackup::GetBackupRequiredPaths` is Implicit |
| 117 | ESchemeOpBackupIncrementalBackupCollection | schemeshard__operation_backup_incremental_backup_collection.cpp:158-160 | `BackupIncrementalBackupCollection.Name` | LeafUnderWorkingDir | Target | OK for the collection path (verified); table set is Implicit |
| 118 | ESchemeOpCreateLongIncrementalBackupOp | derived from #117 | `BackupIncrementalBackupCollection.Name` (reused, per audit) | LeafUnderWorkingDir | Target | OK (convention). **Internal-only**: `Y_ABORT("multipart operations are handled before")` |
| 119 | ESchemeOpCreateFullBackupOp | schemeshard__operation_create_full_backup_op.cpp:48-60 | none — uses `WorkingDir` directly (caller already set it to the collection path) | Absolute (=WorkingDir itself) | Target | OK (verified; comment in dispatch confirms "internal control op only — not valid for direct user submission") |
| 120 | ESchemeOpRestoreBackupCollection | schemeshard__operation_restore_backup_collection.cpp:224-226 | `RestoreBackupCollection.Name` | LeafUnderWorkingDir | Target | OK for the collection path (verified); restored-table set is Implicit |
| 121 | ESchemeOpCreateLongIncrementalRestoreOp | schemeshard__operation.cpp:1668 | `RestoreBackupCollection.Name` (reused, per audit) | LeafUnderWorkingDir | Target | OK (convention); control-plane op, not opened in detail |
| 122 | ESchemeOpCreateSysView | schemeshard__operation_create_sysview.cpp:109-147 | `CreateSysView.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 123 | ESchemeOpDropSysView | schemeshard__operation_drop_sysview.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 124 | ESchemeOpChangePathState | schemeshard__operation_change_path_state.cpp:57 | `ChangePathState.Path` | PathUnderWorkingDir | Target | OK (verified) |
| 125 | ESchemeOpIncrementalRestoreLockTargets | schemeshard__operation_incr_restore_lock_targets.cpp:27 | `IncrementalRestoreLockTargets.DstPaths[]` (Target) / `.SrcPaths[]` (Source) | PathUnderWorkingDir (each) | Target + Source | OK (verified) |
| 126 | ESchemeOpIncrementalRestoreUnlockTargets | same file as #125 | same fields | PathUnderWorkingDir (each) | Target + Source | OK (convention, shares file/shape w/ #125, not opened) |
| 127 | ESchemeOpIncrementalRestoreFinalize | schemeshard__operation_incremental_restore_finalize.cpp:178,260,351,365 | none in the proto — `tablePath`/`backupTablePath` locals are computed from persisted incremental-restore state | Implicit | n/a | OK — audit's comment ("operates on paths determined at runtime") matches; genuinely runtime-only (verified) |
| 128 | ESchemeOpCreateSecret | schemeshard__operation_create_secret.cpp:151-192 | `CreateSecret.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 129 | ESchemeOpAlterSecret | schemeshard__operation_alter_secret.cpp:107-123 | `AlterSecret.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 130 | ESchemeOpDropSecret | schemeshard__operation_drop_secret.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |
| 131 | ESchemeOpCreateStreamingQuery | schemeshard__operation_create_streaming_query.cpp:235-243 | `CreateStreamingQuery.Name` | LeafUnderWorkingDir | Target | **Mismatch (real bug): audit emits the bare `Name`, no `WorkingDir` join** (verified) |
| 132 | ESchemeOpDropStreamingQuery | schemeshard__operation_drop_streaming_query.cpp:186-197 | `Drop.Name` / `Drop.Id` (`HasId()`) | LeafUnderWorkingDir / ById | Target | **Mismatch (real bug, double issue): missing `WorkingDir` join and `Id` branch** (verified) |
| 133 | ESchemeOpAlterStreamingQuery | schemeshard__operation_alter_streaming_query.cpp:214-223 | `streamingQueryDescription.Name` (audit reuses `CreateStreamingQuery` submessage) | LeafUnderWorkingDir | Target | **Mismatch: audit emits bare `Name`, no `WorkingDir` join** (verified) |
| 134 | ESchemeOpTruncateTable | schemeshard__operation_truncate_table.cpp:232-233 | `TruncateTable.TableName` | LeafUnderWorkingDir | Target | **Mismatch (real bug): audit emits bare `TableName`; `Propose()` does `JoinPath({WorkingDir, TableName})`** (verified) |
| 135 | ESchemeOpCreateTestShardSet | schemeshard__operation_create_test_shard_set.cpp:248-288 | `CreateTestShardSet.Name` | LeafUnderWorkingDir | Target | OK (verified) |
| 136 | ESchemeOpDropTestShardSet | schemeshard__operation_drop_test_shard_set.cpp | `Drop.Name` (+ likely `Drop.Id`) | LeafUnderWorkingDir / ById | Target | Likely mismatch (Drop-family convention, not opened) |

---

## 2. Shapes — minimal set of extraction rules

Grouping by (proto shape × interpretation), the 136 op types collapse into
**9 shapes**. A table-driven extractor needs roughly this many distinct
handlers, not 136:

1. **`WorkingDir + <SubMessage>.Name` (plain leaf)** — the overwhelming
   majority (~80 op types): Mk/Rm-Dir, all the simple Create/Alter ops
   (Table\*, PQ, RTMR, BSV, Kesus, Solomon, FileStore, ColumnStore/Table,
   Sequence, Replication/Transfer(create), BlobDepot(create),
   ExternalTable/DataSource, View, Secret, SysView, TestShardSet, Lock,
   ResourcePool, StreamingQuery, all *derived-only* CDC/Index/BackupCollection
   parts). One `TPath::Resolve(WorkingDir).Dive(name)` / `.Child(name)` call.
2. **`WorkingDir + Drop.Name`, with `Drop.HasId()` → `ById(Drop.Id)`
   bypass** — the `TDrop` shape, shared by ~22 op types (all `Drop*`/`RmDir`/
   `ForceDrop*` variants, plus `AlterTable`/`AlterBlockStoreVolume`/
   `AlterPersQueueGroup`/`AlterReplication`/`AlterTransfer` which reuse the
   same id-or-name pattern via their own submessage's `HasPathId()`/
   `HasId_Deprecated()`/`HasId()`/`HasPathId()`). This is the single biggest
   audit gap: **every** op in this shape silently drops the id-addressed case.
3. **Absolute path field(s), no `WorkingDir` involved** — `MoveTable`,
   `MoveTableIndex`, `MoveSequence` (`SrcPath`/`DstPath`), `CreateColumnBuild`
   /`DropColumnBuild` (`.Table`), `ConsistentCopyTables`
   (`CopyTableDescriptions[].Src/DstPath`), `SplitMergeTablePartitions`
   (`.TablePath`, **but the audit wrongly treats this one as
   WorkingDir-relative — see Findings**).
4. **Leaf/child under a sibling field, not `WorkingDir`** — the whole index
   family: `CreateIndexBuild`/`ApplyIndexBuild`/`CancelIndexBuild`
   (`Index(Name)` under `Table`/`TablePath`), `MoveIndex`
   (`Src/DstPath` leaf under `TablePath`).
5. **`WorkingDir + Parent.Field + Child.Field`, cascading/repeated** — the
   CDC-stream family at the top level (`CreateCdcStream`, `AlterCdcStream`,
   `DropCdcStream` with a repeated `StreamName`, `RotateCdcStream`), and
   `DropIndex`/`DropTableIndexAtMainTable`.
6. **`WorkingDir + repeated Src/DstPaths[]`** — `IncrementalRestoreLock/
   UnlockTargets`.
7. **`WorkingDir + free-form relative-or-absolute Path field`** —
   `AlterUserAttributes.PathName`, `ChangePathState.Path`.
8. **No path in the proto at all (Implicit / runtime-derived)** —
   `AlterLogin`, `IncrementalRestoreFinalize`, `AlterBlobDepot`/
   `DropBlobDepot` (Propose-time), `AlterView` (unimplemented),
   `ESchemeOp_DEPRECATED_35`, and the now-retired
   `RestoreMultipleIncrementalBackups`/`RestoreIncrementalBackupAtTable`.
9. **Backup-collection family: `WorkingDir + Collection.Name`, with an
   Implicit runtime-derived table/stream set** — `CreateBackupCollection`,
   `DropBackupCollection`, `BackupBackupCollection`,
   `BackupIncrementalBackupCollection`, `CreateLongIncrementalBackupOp`,
   `CreateFullBackupOp` (degenerate: `WorkingDir` itself is the target),
   `RestoreBackupCollection`, `CreateLongIncrementalRestoreOp`.

Shape 2 (Drop-family id bypass) alone covers roughly a sixth of all op types
and is the highest-value single fix for the audit extractor.

---

## 3. Findings

**Confirmed audit bugs (not just gaps) — these produce an actively wrong
path string today, or omit a path the operation demonstrably touches:**

- **`SplitMergeTablePartitions` (#10)**: `ExtractChangingPaths` computes
  `JoinPath({WorkingDir, SplitMergeTablePartitions.TablePath})`. The real
  `Propose()` (`schemeshard__operation_split_merge.cpp:857`) resolves
  `TablePath` **directly**, with no `WorkingDir` join, and also supports a
  `TableOwnerId`/`TableLocalId` id-addressed branch the audit never sees.
  If `TablePath` is (as the code implies) always absolute, `JoinPath`
  produces a malformed double-path string (`NKikimr::JoinPath` is plain
  string concatenation with `/`; it does not detect that the second argument
  is already absolute).
- **`CreateResourcePool`/`DropResourcePool`/`AlterResourcePool` (#108-110)**:
  the resource-pool family resolves `WorkingDir/Name` (verified in all
  three files), but the audit pushes the bare `Name` with **no `WorkingDir`
  join at all** — for any op outside the root, the audit's path is simply
  wrong (missing the parent directory). `DropResourcePool` additionally
  ignores its `Drop.HasId()` branch.
- **`CreateStreamingQuery`/`AlterStreamingQuery`/`DropStreamingQuery`
  (#131-133)**: same class of bug as ResourcePool — `Propose()` resolves
  `WorkingDir/Name`, audit emits the bare name. `DropStreamingQuery` also
  ignores its id branch.
- **`TruncateTable` (#134)**: `Propose()` does
  `JoinPath({WorkingDir, TableName})`; audit emits the bare `TableName`.
- **`AlterSequence` (#80)** and **`AlterReplication`/`AlterTransfer`
  (#84-85)**: the audit case bodies are **empty** (no `result.emplace_back`
  at all), but both `Propose()` implementations clearly resolve a path
  (`WorkingDir/Name` or, for Alter\*plication, an id branch). These three op
  types currently produce **zero** audit-log path entries despite genuinely
  touching a path.
- **The `TDrop`-family id bypass** (Shape 2, ~22 op types, directly verified
  for `DropTable`, `DropPersQueueGroup`, `RmDir`, `ForceDropUnsafe`,
  `DropColumnStore`, `DropReplication`(+Cascade)/`DropTransfer`(+Cascade),
  `DropResourcePool`, `DropStreamingQuery`, and the analogous
  `AlterTable`/`AlterBlockStoreVolume`/`AlterPersQueueGroup`/
  `AlterReplication`/`AlterTransfer` id branches): whenever the client
  addresses the target **by numeric id** (`Drop.HasId()`,
  `Alter*.HasPathId()`/`HasId_Deprecated()`), `Propose()` resolves via
  `TPath::Init(MakeLocalId(id))` and the `Name`/`WorkingDir` fields may be
  empty or stale. `ExtractChangingPaths` always computes
  `JoinPath(WorkingDir, X.Name)` regardless, so an id-addressed request can
  log a nonsensical or empty leaf.

**Confirmed gaps (extractor is incomplete but not actively wrong):**

- **`ConsistentCopyTables` (#30)**: only `DstPath` is emitted per item;
  `SrcPath` (Source role) and every implicit `CreateSrcCdcStream`/
  `IndexImplTableCdcStreams` child are dropped.
- **`CreateIndexedTable` (#28)**: only the base table name is emitted; index,
  impl-table, sequence, and KMeans-prefix children (all derivable from the
  same request, just not walked) are Implicit.
- **Backup-collection family (#113-121)**: only the collection path is
  emitted; the concrete table/stream set is genuinely runtime-derived
  (depends on the collection's stored entry list), matches plan §1a.
- Minor: `CreateReplication`/`CreateTransfer`'s `Target[].DstPath`/
  `.DirectoryPath` and `CreateExternalTable`'s `.DataSourcePath` are
  Dependency paths resolved absolutely in `Propose()` but never surfaced by
  the audit at all (low priority — these are references to *other* existing
  objects, not paths the operation creates/drops).

**Dead / unreachable op types still carried in both switches:**

- **`RestoreMultipleIncrementalBackups` / `RestoreIncrementalBackupAtTable`
  (#111-112)**: both factory functions unconditionally `CreateReject(...)`
  with a message saying the schema-op dispatch "has been retired" in favor
  of a request/response channel (`TEvIncrementalRestoreSrcCreateRequest`).
  The audit case for these op types is effectively dead code today.
- **`AlterExternalTable`/`AlterExternalDataSource` (#96, #99)** and
  **`AlterView` (#104)**: the top-level `MakeOperationParts` dispatch
  `Y_ABORT`s ("TODO: implement") if a client submits these op types
  directly. `AlterExternalTable`/`AlterExternalDataSource` *do* have working
  `Propose()` code, but it is only reachable when internally synthesized as
  a derived part of `CreateExternalTable`/`CreateExternalDataSource` (an
  "or replace"-style path). `AlterView` has no implementation anywhere.
- **`AlterBackupCollection` (#114)**: same `Y_ABORT("TODO: implement")`
  situation — genuinely unimplemented for direct client use.
- **`ESchemeOp_DEPRECATED_35` (#36)**: dispatch `Y_ABORT("impossible")`;
  correctly a no-op in the audit switch.

**Ops with no path or a runtime-only path** (confirmed, matches plan §1a):
`AlterLogin` (only validates `WorkingDir == LoginProvider.Audience`),
`IncrementalRestoreFinalize` (resolves paths from persisted incremental-
restore state, not the request), `AlterBlobDepot`/`DropBlobDepot` (the
`Propose()` handlers for these two are no-op stubs — whatever path
resolution happens for BlobDepot alter/drop must happen earlier, at
`ConstructParts`/construction time, not at `Propose()`; not traced further
in this pass).

**Correction to plan §1a**: the "MoveTable / MoveIndex / MoveSequence" row
of the known-offenders table describes `MoveTableIndex.SrcPath/DstPath` as
"leaf names relative to `TablePath`". That description is actually correct
for `MoveIndex` (#93: `TPath::Resolve(TablePath).Child(SrcPath/DstPath)`,
i.e. leaf-under-sibling) but **not** for `MoveTableIndex` (#77): its
`Src/DstPath` are constructed as full absolute path strings
(`src.PathString()`/`dst.PathString()`) by `MoveTableIndexTask()` before the
derived part is created, and consumed as plain `TPath::Resolve(absolute)` —
i.e. Shape 3 (Absolute), the same as `MoveTable`/`MoveSequence`, not Shape 4.

**23-raw-cases re-count**: re-deriving the plan's "23 cases push the raw
field without `JoinPath(WorkingDir, ...)`" claim directly from the switch
body gives **19** op types with a non-empty raw (non-`WorkingDir`-joined)
result (`CreateConsistentCopyTables`, `CreateIndexBuild`, `ApplyIndexBuild`,
`CancelIndexBuild`, `MoveTable`, `MoveTableIndex`, `MoveSequence`,
`MoveIndex`, `CreateColumnBuild`, `DropColumnBuild`, `CreateResourcePool`,
`DropResourcePool`, `AlterResourcePool`,
`RestoreMultipleIncrementalBackups`+`RestoreIncrementalBackupAtTable`
(one case, two op types), `CreateStreamingQuery`, `DropStreamingQuery`,
`AlterStreamingQuery`, `TruncateTable`) plus 2 more that push `WorkingDir`
itself with no join at all (`AlterLogin`, `CreateFullBackupOp`) — 21 if those
are included, still short of 23. Of these 19-21, **8 are genuinely wrong**
(ResourcePool×3, StreamingQuery×3, TruncateTable, SplitMergeTablePartitions
— counted separately above since it's nominally a "WD" case in the switch
body's structure but is semantically raw/absolute) and the rest (Move family,
ColumnBuild family, IndexBuild family, ConsistentCopyTables partially) are
legitimately raw/absolute and match the real code.

---

## 4. Answer to plan §3 open question about op-type count vs. shapes

136 op types reduce to **9 extraction shapes** (§2). A table-driven
extractor (`static const THashMap<EOperationType, TFieldSpec>`, design 1a
from the plan) needs roughly one `TFieldSpec` entry per op type but the
*resolution logic* behind each `Kind` is one of only 9 small functions, so
the new `schemeshard_path_footprint.{h,cpp}` implementation surface is small
regardless of the 136-entry table size.
