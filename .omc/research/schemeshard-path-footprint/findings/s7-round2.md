# S7 round 2 findings

## S7a/S7b

### S7a — the seven missing path fields (plan §8.3)

Commit: `558efc6d6ee` — `feat: footprint covers copy sources, sequences, replication and backup dst paths`

| § 8.3 field | verdict | what the code actually does |
|---|---|---|
| `CreateTable.CopyFromTable` | added, `Absolute`, `Source` | `TPath::Resolve` at `schemeshard__operation_copy_table.cpp:568` and `:978`; WorkingDir is never joined. `ESchemeOpCreateTable` is dispatched to the copy-table factory when the field is set (`schemeshard__operation.cpp`), so the existing `ESchemeOpCreateTable` case emits it under `HasCopyFromTable()`. |
| `CreateColumnTable.CopyFromTable` | added, `Absolute`, `Source` | It *is* resolved by a Propose, contrary to the plan's doubt: `ESchemeOpCreateColumnTable` with the field set is dispatched to `TReadOnlyCopyColumnTable` (`schemeshard__operation.cpp:1433`), whose Propose calls `TPath::Resolve(opDescr.GetCopyFromTable())` (`olap/operations/read_only_copy_table.cpp:401,425`). |
| `CopySequence.CopyFrom` | added, `Absolute`, `Source` | `TPath::Resolve` at `schemeshard__operation_copy_sequence.cpp:579`. The part carrying it has op type `ESchemeOpCreateSequence` and is only ever produced by `CreateConsistentCopyTables` (`schemeshard__operation_consistent_copy_tables.cpp:428`); the top-level `ESchemeOpCreateSequence` dispatch goes to `CreateNewSequence`. |
| AlterTable column `DefaultFromSequence` | added, `Absolute` or `LeafUnderSibling`, `Dependency` | `schemeshard__operation_alter_table.cpp:665`: absolute when the value starts with `/`, else `path.Child(value)` where `path` is the altered table. Because the table may be addressed by `PathId`/`Id_Deprecated`, the base cannot always be written as a string, so `LeafUnderSibling` gained an anchor-index form (`TRefSink::SiblingOf`) whose base is another entry of the same footprint. **CreateTable does not resolve it at all** — `schemeshard__operation.cpp:1530` and `schemeshard_info_types.cpp:538,728` only compare the raw name against a `localSequences` set — so nothing was added there. |
| Replication `Target[].DstPath`, `DirectoryPath` | added, `Absolute`, `Dependency`, in three fields, not the guessed shape | Only the **transfer** strategy resolves paths. `Replication.Config.TransferSpecific.Target` is a *singular* `Target`, and its `DstPath` and `DirectoryPath` are resolved at `schemeshard__operation_create_replication.cpp:80,91`. Alter carries its own `AlterReplication.AlterTransfer.DirectoryPath` (`schemeshard__operation_alter_replication.cpp:57`). The repeated form the plan guessed is `Config.Specific.Targets[i].DstPath`, which belongs to plain async replication and is **not resolved by any Propose** (`TReplicationStrategy::Validate` touches no `TPath`); it is emitted anyway because it is an absolute local path the operation intends to write to, with a comment saying so. `SrcPath`, `SrcStreamName`, `DstPathLambda` and `ConsumerName` are never emitted. |
| `AlterPersQueueGroup.OffloadConfig.IncrementalBackup.DstPath` | added, `Absolute`, `Dependency` | Full proto path is `AlterPersQueueGroup.PQTabletConfig.OffloadConfig.IncrementalBackup.DstPath`; `TPath::Resolve` at `schemeshard__operation_alter_pq.cpp:328`, gated on `HasIncrementalBackup()` (the other oneof arm, `IncrementalRestore`, carries only a path id). |
| `AlterContinuousBackup.TakeIncrementalBackup.DstPath` | added, `PathUnderWorkingDirSplit`, `Target` | `workingDirPath.Child(DstPath, TSplitChildTag{})` at `schemeshard__operation_alter_continuous_backup.cpp:128`. Emitted only for the `TakeIncrementalBackup` action; `:128` runs before the action switch, so a `Stop` request resolves an empty child, which is not worth reporting. |

Two fields beyond the seven, both found while reading the cited code:

- `AlterContinuousBackup.TakeIncrementalBackup.DstStreamPath` — the new cdc stream leaf under the table (`:161` into `NCdc::DoNewStreamPathChecks`). Emitted only when present; otherwise the name is generated from `Now()`.
- `AlterContinuousBackup.TableName` was extracted as `PathUnderWorkingDir` but `:86` resolves it with `TSplitChildTag`. Corrected to `PathUnderWorkingDirSplit`.

Known extractor mismatch left alone, for a later fix stage: `ESchemeOpCreateCdcStream` and `ESchemeOpCreateContinuousBackup` both resolve `TableName` through `NCdc::DoNewStreamPathChecks`, which uses `TSplitChildTag`, while the extractor uses `PathUnderWorkingDir` and `LeafUnderWorkingDir` respectively. The two differ only for a `TableName` that starts with `/`.

### S7b — descriptor-walk completeness (plan §8.4)

Commit: `057ca390e9e` — `test: descriptor walk enforces path field classification in footprint`

`KnownPathFieldNames()` was added to `schemeshard_path_footprint.{h,cpp}`, directly under the `ExtractPathRefs` switch with a "keep in sync" comment. It lists 86 fully-qualified proto field names.

The test walks `NKikimrSchemeOp::TModifyScheme::descriptor()` recursively with a visited set, recursing through every message and group field (map fields included: recursing into the synthesized entry message reaches the map's value type). It collects every `TYPE_STRING` field whose name contains `Path`, `Name`, `Dir`, `Table`, `From`, `Src`, `Dst` or `Prefix`, case-sensitive.

Package policy: messages in `Ydb.*`, `NKikimrProto`, `NActorsProto`, `google.protobuf` and `NYql*` are walked into but not classified. They are transport, actor and public-API types that SchemeShard never resolves as a path in its own tree. Every other package reached is classified, which is how `NKikimrPQ`, `NKikimrSubDomains`, `NKikimrReplication`, `NKikimrIndexBuilder`, `NKikimrStoragePool`, `NKikimrBlobDepot`, `NKikimrBlockStore`, `NKikimrArrowAccessorProto` and `NLoginProto` entries end up on the lists below.

The test also asserts the reverse direction: every name in `KnownPathFieldNames()` must be reachable from the walk, so a renamed or misspelled entry fails instead of silently shrinking the known set.

Each collected field must be in one of four sets or the test fails: the known set, `ReportedOutsideTheRefList`, `NotAPath`, or `Unclassified` (tolerated, but printed on every run).

The first run reported 109 unclassified fields. Their final classification:

| bucket | count |
|---|---|
| added to the extractor, so now in `KnownPathFieldNames()` | 1 |
| `ReportedOutsideTheRefList` (`TModifyScheme.WorkingDir`) | 1 |
| `NotAPath` | 100 |
| `Unclassified` | 7 |

`KnownPathFieldNames()` holds 86 entries in total; the walk reaches all of them,
which the test asserts.

#### NotAPath (100)


**Column, family and key names inside a table or column-table schema.**

- `NKikimrIndexBuilder.TColumnBuildSetting.ColumnName`
- `NKikimrPQ.TPQTabletConfig.TKeyComponentSchema.Name`
- `NKikimrSchemeOp.TColumnDataLifeCycle.TTtl.ColumnName`
- `NKikimrSchemeOp.TColumnDescription.FamilyName`
- `NKikimrSchemeOp.TColumnDescription.Name`
- `NKikimrSchemeOp.TColumnTableSchema.KeyColumnNames`
- `NKikimrSchemeOp.TDefaultExpressionColumnDescription.DependencyColumnNames`
- `NKikimrSchemeOp.TFamilyDescription.Name`
- `NKikimrSchemeOp.TIndexAlteringConfig.DataColumnNames`
- `NKikimrSchemeOp.TIndexAlteringConfig.KeyColumnNames`
- `NKikimrSchemeOp.TIndexCreationConfig.DataColumnNames`
- `NKikimrSchemeOp.TIndexCreationConfig.KeyColumnNames`
- `NKikimrSchemeOp.TIndexDataExtractor.TSubColumn.SubColumnName`
- `NKikimrSchemeOp.TIndexDescription.DataColumnNames`
- `NKikimrSchemeOp.TIndexDescription.KeyColumnNames`
- `NKikimrSchemeOp.TMultiColumnStatisticsDescription.ColumnNames`
- `NKikimrSchemeOp.TOlapColumnDescription.ColumnFamilyName`
- `NKikimrSchemeOp.TOlapColumnDescription.Name`
- `NKikimrSchemeOp.TOlapColumnDiff.ColumnFamilyName`
- `NKikimrSchemeOp.TOlapColumnDiff.Name`
- `NKikimrSchemeOp.TRequestedBloomFilter.ColumnNames`
- `NKikimrSchemeOp.TRequestedBloomNGrammFilter.ColumnName`
- `NKikimrSchemeOp.TRequestedCountMinSketch.ColumnNames`
- `NKikimrSchemeOp.TRequestedMaxIndex.ColumnName`
- `NKikimrSchemeOp.TRequestedMinMaxIndex.ColumnName`
- `NKikimrSchemeOp.TTTLSettings.TEnabled.ColumnName`
- `NKikimrSchemeOp.TTableDescription.KeyColumnNames`

**Objects named inside a column table (presets, olap indexes, statistics).**


**They live in the table's schema, not as children in the scheme tree.**

- `NKikimrSchemeOp.TAlterColumnTable.AlterSchemaPresetName`
- `NKikimrSchemeOp.TAlterColumnTable.RESERVED_AlterTtlSettingsPresetName`
- `NKikimrSchemeOp.TAlterColumnTableSchemaPreset.Name`
- `NKikimrSchemeOp.TAlterColumnTableTtlSettingsPreset.Name`
- `NKikimrSchemeOp.TColumnTableDescription.SchemaPresetName`
- `NKikimrSchemeOp.TColumnTableSchemaPreset.Name`
- `NKikimrSchemeOp.TColumnTableTtlSettingsPreset.Name`
- `NKikimrSchemeOp.TIndexDescription.Name`
- `NKikimrSchemeOp.TMultiColumnStatisticsDescription.Name`
- `NKikimrSchemeOp.TOlapIndexDescription.Name`
- `NKikimrSchemeOp.TOlapIndexRequested.Name`
- `NKikimrSchemeOp.TOlapMoveIndex.DestinationName`
- `NKikimrSchemeOp.TOlapMoveIndex.SourceName`
- `NKikimrSchemeOp.TRemoveColumnTableSchemaPreset.Name`
- `NKikimrSchemeOp.TRemoveColumnTableTtlSettingsPreset.Name`

**Registered C++ class, policy and logic names looked up in a factory.**

- `NKikimrArrowAccessorProto.TConstructor.ClassName`
- `NKikimrArrowAccessorProto.TDataExtractor.ClassName`
- `NKikimrArrowAccessorProto.TRequestedConstructor.ClassName`
- `NKikimrSchemeOp.TColumnTableRequestedOptions.ScanReaderPolicyName`
- `NKikimrSchemeOp.TColumnTableSchemeOptions.ScanReaderPolicyName`
- `NKikimrSchemeOp.TCompactionLevelConstructorContainer.ClassName`
- `NKikimrSchemeOp.TCompactionLevelConstructorContainer.DefaultSelectorName`
- `NKikimrSchemeOp.TCompactionPlannerConstructorContainer.ClassName`
- `NKikimrSchemeOp.TCompactionPlannerConstructorContainer.TSOptimizer.LogicName`
- `NKikimrSchemeOp.TCompactionSelectorConstructorContainer.ClassName`
- `NKikimrSchemeOp.TCompactionSelectorConstructorContainer.Name`
- `NKikimrSchemeOp.TIndexDataExtractor.ClassName`
- `NKikimrSchemeOp.TMetadataManagerConstructorContainer.ClassName`
- `NKikimrSchemeOp.TOlapColumn.TSerializer.ClassName`
- `NKikimrSchemeOp.TOlapIndexDescription.ClassName`
- `NKikimrSchemeOp.TOlapIndexRequested.ClassName`
- `NKikimrSchemeOp.TPartitionConfig.NamedCompactionPolicy`
- `NKikimrSchemeOp.TSkipIndexBitSetStorage.ClassName`

**Storage pools and channel profiles: BS group selectors, not scheme paths.**

- `NKikimrBlobDepot.TBlobDepotConfig.Name`
- `NKikimrBlobDepot.TChannelProfile.StoragePoolName`
- `NKikimrBlockStore.TVolumeConfig.StoragePoolName`
- `NKikimrStoragePool.TChannelBind.StoragePoolName`
- `NKikimrStoragePool.TStoragePool.Name`

**Credentials and secret references. A secret is resolved by the secrets**


**subsystem by name, not as a TPath by any of these operations.**

- `NKikimrReplication.TOAuthToken.TokenSecretName`
- `NKikimrReplication.TStaticCredentials.PasswordSecretName`
- `NKikimrSchemeOp.TAws.AwsAccessKeyIdSecretName`
- `NKikimrSchemeOp.TAws.AwsSecretAccessKeySecretName`
- `NKikimrSchemeOp.TBasic.PasswordSecretName`
- `NKikimrSchemeOp.TIamImpersonate.InitialTokenSecretName`
- `NKikimrSchemeOp.TMdbBasic.PasswordSecretName`
- `NKikimrSchemeOp.TMdbBasic.ServiceAccountSecretName`
- `NKikimrSchemeOp.TSecretSchemaOp.ValueParamName`
- `NKikimrSchemeOp.TServiceAccountAuth.SecretName`
- `NKikimrSchemeOp.TToken.TokenSecretName`

**Locations outside this scheme tree: the remote replication cluster, a**


**filesystem or YT export target, an SQS queue.**

- `NKikimrPQ.TPQTabletConfig.SqsAccountName`
- `NKikimrPQ.TPQTabletConfig.SqsQueueName`
- `NKikimrPQ.TPQTabletConfig.TConsumer.Name`
- `NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.SrcPath`
- `NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.SrcStreamName`
- `NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.ConsumerName`
- `NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DstPathLambda`
- `NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.SrcPath`
- `NKikimrSchemeOp.TFSSettings.BasePath`
- `NKikimrSchemeOp.TFSSettings.Path`
- `NKikimrSchemeOp.TYTSettings.TablePattern`

**Filled in by SchemeShard from an already resolved path, or returned by**


**Describe. Never read from the request, so never a path to extract.**

- `NKikimrPQ.TPQTabletConfig.TopicName`
- `NKikimrPQ.TPQTabletConfig.TopicPath`
- `NKikimrPQ.TPQTabletConfig.YdbDatabasePath`
- `NKikimrReplication.TReplicationLocationConfig.Path`
- `NKikimrSchemeOp.TDirEntry.Name`
- `NKikimrSchemeOp.TExternalTableReferences.TReference.Path`
- `NKikimrSchemeOp.TSecretDescription.Name`
- `NKikimrSchemeOp.TSolomonVolumeDescription.Name`
- `NKikimrSchemeOp.TTableDescription.Path`
- `NKikimrSchemeOp.TTestShardSetDescription.Name`

**Login objects and a character set: neither is addressed by path.**

- `NKikimrSchemeOp.TLoginRenameGroup.NewName`
- `NKikimrSubDomains.TSchemeLimits.ExtraPathSymbolsAllowed`
- `NLoginProto.TSid.Name`

#### Unclassified (7)


**Path components joined into the derived parts' own path fields, which the**


**footprint does extract; the component itself is never resolved alone.**

- `NKikimrSchemeOp.TBackupBackupCollection.TargetDir`
- `NKikimrSchemeOp.TBackupCollectionDescription.Prefix`

**Absolute paths that only later execution states resolve. The hook runs at**


**Propose, so they are outside the footprint by construction (plan §8.2.4).**

- `NKikimrReplication.TReplicationConfig.TTargetEverything.DstPrefix`
- `NKikimrSchemeOp.TIncrementalRestoreFinalize.BackupTablePaths`
- `NKikimrSchemeOp.TIncrementalRestoreFinalize.TargetTablePaths`

**TModifyScheme submessages that no EOperationType dispatches to, so no**


**Propose reads them at all.**

- `NKikimrSchemeOp.TPersQueueGroupAllocate.Name`
- `NKikimrSchemeOp.TPersQueueGroupDeallocate.Name`

### Test results

Both commits end with the whole suite green:

```
{"type": "summary", "exit_code": 0, "tests": {"OK": 41}}
```

The unclassified list is printed on every run:

```
PathFootprint: 7 tolerated unclassified path-like fields:
        "NKikimrReplication.TReplicationConfig.TTargetEverything.DstPrefix",
        "NKikimrSchemeOp.TBackupBackupCollection.TargetDir",
        "NKikimrSchemeOp.TBackupCollectionDescription.Prefix",
        "NKikimrSchemeOp.TIncrementalRestoreFinalize.BackupTablePaths",
        "NKikimrSchemeOp.TIncrementalRestoreFinalize.TargetTablePaths",
        "NKikimrSchemeOp.TPersQueueGroupAllocate.Name",
        "NKikimrSchemeOp.TPersQueueGroupDeallocate.Name",
```

New pure `ExtractPathRefs` tests, one per added field:
`CreateTableCopyFromTableIsAnAbsoluteSource`,
`CreateColumnTableCopyFromTableIsAnAbsoluteSource`,
`CopySequenceCopyFromIsAnAbsoluteSource`,
`AlterTableDefaultFromSequence`,
`TransferAndReplicationDestinationPaths`,
`AlterPersQueueGroupOffloadDstPath`,
`AlterContinuousBackupTakeIncrementalBackup`,
`CreateContinuousBackupStreamName`,
plus `TSchemeShardPathFootprintProtoCoverage::EveryPathLikeFieldIsClassified`.

### Commits

| commit | subject |
|---|---|
| `558efc6d6ee` | feat: footprint covers copy sources, sequences, replication and backup dst paths |
| `057ca390e9e` | test: descriptor walk enforces path field classification in footprint |

Both pushed to `enjection/feat/schemeshard-path-footprint`.


---

## S7c / S7d

Branch `feat/schemeshard-path-footprint`, on top of `a25be8bd5a9`.

| commit | subject |
|---|---|
| `b855fe53f00` | feat: record propose-time write set and publications in path footprint |
| `e713ee51807` | feat: attribute part footprints to the originating request transaction |

Both pushed to `enjection/feat/schemeshard-path-footprint`.

### S7c — what changed

`schemeshard__operation_memory_changes.h:19-56` adds `TUndoStack<T>`, a
`TVector`-backed stack with the same `emplace` / `top` / `pop` / `size` /
`operator bool` surface as the `TStack` it replaces, plus `begin()` / `end()`.
All 24 undo-log declarations (`:58-141`) switched to it. `UnDo()` is untouched:
it still pops from the back, which is still the last pushed element.

`schemeshard__operation_memory_changes.h:143-181` adds `TMemoryChanges::TMark`
(19 `size_t`s, one per TPathId-keyed stack) plus `Mark()` and
`CollectPathIdsSince()`, implemented at
`schemeshard__operation_memory_changes.cpp:223-288`. The collector dedupes
against whatever is already in `out`. Skipped, as the design notes recommend:
`Shards` and `TxStates` (not path-keyed), `LongIncrementalRestoreOps`
(TOperationId), `IncrementalBackups` / `FullBackups` (ui64), and `SubDomains`
(only ever the database path, which the footprint already carries as
`DatabasePathId`).

`schemeshard__operation_side_effects.h:101-103` /
`schemeshard__operation_side_effects.cpp:158-174` add `PublishedCount(txId)`
and `CollectPublishedSince(txId, mark, out)` beside `PublishToSchemeBoard`.
`PublishPaths` is keyed by txId, so a part's own publications are an index
range into the deque shared by the whole request.

`schemeshard_path_footprint.h:101-115` adds `WriteSet`, `Published` and
`WriteSetMayBeIncomplete` to `TPathFootprint`;
`schemeshard_path_footprint.cpp:140-183` adds `JoinPathIds`, the shared
`FormatPathFootprintPrefix` (now carrying `writeSet#`, `published#`,
`incomplete#`) and `FormatPathFootprintWriteSetLine`, which emits one extra
line per part with `fieldPath# <writeSet>` and the ids as `owner:local`.
The hook is `schemeshard__operation.cpp:116-145`: both marks before
`Propose()`, both collections plus the incompleteness bit after it.

### S7c — deviations from the design notes

1. **No version on `Published`.** The notes proposed
   `TVector<pair<TPathId, ui64>>` filled from
   `GetPathVersion(...).GetGeneralVersion()`. The brief specified a plain
   `TVector<TPathId>` and that is what shipped. It is also the better answer:
   the version at Propose time is not the version that gets persisted, so for
   any multi-part request the recorded number would be wrong by construction.
2. **No `BecameUndoUnsafe`.** Only the conservative
   `WriteSetMayBeIncomplete = !context.IsUndoChangesSafe()` is recorded, per
   the brief. Since `IsUndoChangesSafe()` is monotone within one request this
   is exactly the notes' `(safeBefore && !safeAfter) || !safeBefore`.
3. **`CollectPathIdsSince` dedupes.** The notes left duplicates to the caller;
   the brief asked for dedup, so the collector does it.
4. `#include <util/generic/stack.h>` was left in the memory-changes header even
   though nothing in it uses `TStack` any more, to avoid breaking transitive
   includes elsewhere.

### S7c — finding: CreateTable's write set is empty by construction

`TCreateTable::Propose` never calls `context.MemChanges.Grab*`. It writes
straight through `NIceDb::TNiceDb db(context.GetDB())`
(`schemeshard__operation_create_table.cpp:730`). So the CreateTable part
reports `writeSet# 0, incomplete# 1`, and the new table's path id appears in no
write set of the request. The generated `MkDir` parts do go through
`TMemoryChanges` and report an exact two-entry write set (the new directory and
the parent whose child list changed), with `incomplete# 0` because they run
before any direct db write.

This contradicts the brief's expected assertion ("CreateTable's write set
contains the new table path id and its parent dir"). The test asserts what is
actually true, in both directions, and explains why. It is the clearest
demonstration available of what the incompleteness bit is for.

Dropping an indexed table *does* show the cascade. `TDropTable::Propose` grabs
the dropped path and its parent (`schemeshard__operation_drop_table.cpp:572`),
so the union of the request's part write sets contains the main table, its
parent, the index and the index impl table, none of which except the table
itself is named anywhere in the request proto.

### S7d — what changed

`schemeshard__operation.h:18-30` adds `TOperation::RequestFootprints`.
`schemeshard_path_footprint.h:101-106` adds `TPathFootprint::OriginalTxIndex`.
`FormatPathFootprintLine` gained a `prefix` parameter (default
`"PathFootprint"`); the shared prefix builder now prints
`partId# <request>` when `PartId == InvalidSubTxId`, and `originalTxIndex#` on
every line of both layers.

`schemeshard__operation.cpp:279-299` is the H1 pass, placed between Phase Zero
and Phase One and logged with the `PathFootprint request` prefix; the comment
there documents the three early returns above it that yield no request
footprint (duplicate txId, quota failure, rewrite failure) and the Phase One
split failure that leaves `RequestFootprints` populated with `PathFootprints`
still empty. Phase One (`:302-331`) builds the two parallel origin vectors;
Phases Two and Three (`:338-360`) are indexed loops passing the origin into
`ProcessOperationParts`, which gained a `ui32 originalTxIndex` parameter after
`prevProposeUndoSafe` (`schemeshard_impl.h:496-507`).

### S7d — deviation

The brief asked for a default argument on the new parameter "so other callers
compile". There are no other callers: `ProcessOperationParts` is private to
`TSchemeShard` and both call sites are in `IgniteOperation`. A default in the
middle of the signature is not legal C++ either, so the parameter is required
and both call sites were updated.

### Tests

`ut_path_footprint/ut_path_footprint.cpp` gained the write-set helpers
(`SplitPathIds`, `PathIdOf` via `DescribePrivatePath`, `RequireWriteSetLine`,
`AllWriteSetPathIds`) and a layer split in the log parser
(`ParseFootprintLogLayer`, `ParseFootprintLog`, `ParseRequestFootprintLog`).
The layer split is required, not cosmetic: without it the request-layer line
for a rejected `CreateTable` would be the first match in
`RejectedCreateTableStillProducesFootprint` and would report the default
`StatusSuccess`.

- `CreateTableWithIntermediateDirs` extended: every part carries
  `originalTxIndex 0`; exactly one request footprint, `partId# <request>`,
  `CreateTable.Name` / `LeafUnderWorkingDir` / `relToDb a/b/Table`; the MkDir
  write sets cover `/MyRoot`, `/MyRoot/a`, `/MyRoot/a/b` exactly;
  the CreateTable part is `writeSet# 0, incomplete# 1` and the new table id is
  absent from the request's write set.
- `RejectedCreateTableStillProducesFootprint` extended: empty write set, no
  publications, `incomplete# 0`.
- `DropIndexedTableWriteSetCoversTheCascade` (new): the cascade case.
- `TwoTransactionsGetDistinctOriginalTxIndexes` (new): a hand-built two-
  transaction `TEvModifySchemeTransaction`; two request footprints with
  indexes 0 and 1, and the generated `MkDir` for `second` plus the `MkDir` for
  `nested` both pointing back at index 1.

### Test results

| target | summary |
|---|---|
| `ydb/core/tx/schemeshard/ut_path_footprint` | `{"exit_code": 0, "tests": {"OK": 43}}` |
| `ydb/core/tx/schemeshard/ut_base -F '*TSchemeShardTest*'` | `{"exit_code": 0, "tests": {"OK": 171}}` |
| `ydb/core/tx/schemeshard/ut_cdc_stream` | `{"exit_code": 0, "tests": {"OK": 44}}` |

The `ut_path_footprint` run at the S7c commit alone was
`{"exit_code": 0, "tests": {"OK": 42}}`.

### Diffstat vs `main`, pre-existing files only

```
 schemeshard__operation.cpp                  |  79 +++++++++++--
 schemeshard__operation.h                    |  14 +++
 schemeshard__operation_memory_changes.cpp   |  65 +++++++++++
 schemeshard__operation_memory_changes.h     | 122 +++++++++++++++++----
 schemeshard__operation_side_effects.cpp     |  15 +++
 schemeshard__operation_side_effects.h       |   7 ++
 schemeshard_impl.h                          |   3 +
 7 files changed, 273 insertions(+), 32 deletions(-)
```

96 of the 122 lines in the memory-changes header are the mechanical
`TStack` -> `TUndoStack` rename plus the new type and mark struct; no existing
statement changed behaviour.

## S7f — compile-time path field identity, allocation-free extraction

Plan §8.5, first half: field identity moves from a `TString FieldPath` built
per ref into a compile-time enum plus a static table. No observable behavior
changed: the same field-path strings, kinds, roles and values reach the log and
the tests, and all 136 op types still resolve to the same refs.

### The table

`SCHEMESHARD_PATH_FIELDS(X)` lives in `schemeshard_path_footprint.h`, right
above `enum class EPathField : ui16`. **144 enumerators**, one per distinct
protobuf field the extractor reads plus one synthetic row per Implicit marker
and per id-valued field. Five columns:

1. enumerator (`<Submessage>_<Field>`, e.g. `CopyTables_Item_IndexImplCdc_StreamName`);
2. the field-path template, with `{i}` / `{j}` / `{key}` placeholders;
3. the fully qualified protobuf field name, `""` for a synthetic/id row;
4. default `EPathRefKind`; 5. default `EPathRefRole`.

Generated from it in `schemeshard_path_footprint.cpp`: the enum itself, four
`constexpr` arrays (`FieldTemplates`, `FieldProtoNames`, `FieldKinds`,
`FieldRoles`) with `static_assert`s tying their size to `EPathField::Count`,
the accessors `PathFieldName` / `PathFieldProtoName` / `PathFieldDefaultKind` /
`PathFieldDefaultRole`, and `KnownPathFieldNames()` (the non-empty proto column,
`SortUnique`d — the hand-maintained 86-entry list is gone).

The proto-name column reproduces the old `KnownPathFieldNames()` set exactly:
86 names, verified by set-diff against `HEAD~2`. Two prefixes share a proto
field in several places (Replication vs AlterReplication, Create vs Alter for
FileStore/Secret/BackupCollection), which is why generation deduplicates.

### Two indices, not one

The plan sketched a single `Index`. Two CopyTableDescriptions field paths need
two positions at once
(`...[{i}].DropSrcCdcStream.StreamName[{j}]`, and
`...[{i}].IndexImplTableDropCdcStreams[{key}].StreamName[{j}]` needs all three),
so `TPathRef` carries `Index`, `SubIndex` and `MapKey`.

### Lifetime

`TPathRef::Value` / `BasePath` / `MapKey` are now `TStringBuf` into the request
proto. The one base path that is *computed* rather than read
(`JoinPath({SrcPath, indexImplTable})` for index-impl-table cdc streams) is
interned in `TPathRefs::Owned`, a `TDeque<TString>` returned alongside the refs;
a deque never relocates, so the views stay valid. `ExtractPathRefs` therefore
returns `TPathRefs` (a `TVector<TPathRef>` plus that arena, with
`size`/`operator[]`/`begin`/`end` so call sites read unchanged) instead of a
bare vector.

A footprint outlives its `TModifyScheme` (`operation->PathFootprints`, and the
request layer resolves from a local `rewrittenTransactions` vector), so
`TPathFootprintEntry::Ref` is now a `TPathRefOwned` with `TString` members and a
`FieldPath` materialized once, in `ResolvePathFootprint`. That keeps
`FormatPathFootprintLine` and the log format byte-identical.

### What stayed hand-written, and why

The 136-case op-type switch. Per plan instruction this stage moves field
identity only; folding the switch into per-op X-macro lists (the `RewritePaths`
idea) is §8.5's second half. The switch still has no `default:`, so a new
`EOperationType` is still a `-Wswitch` error — that caught a dropped
`ESchemeOpDropBlockStoreVolume` case during this refactor.

Sink helpers now take an `EPathField`: `Add` (table defaults), `AddAs` (per-op
override), `Sibling`, `SiblingOf`, `ById`, `Implicit`, each with an optional
`TRefAt{Index, SubIndex, Key}`. **Ten call sites** use `AddAs` because the same
protobuf field resolves differently depending on the operation carrying it —
more than the three the brief expected: the four CDC `TableName` fields
(Create/Alter/Drop/Rotate) under their AtTable parts, the four stream-name
fields under their Impl parts, `DropIndex.TableName` under
`DropTableIndexAtMainTable`, and `AlterTable.Columns[].DefaultFromSequence` when
the value starts with a slash (that one varies per *value*, not per op).

### Tests

`ut_path_footprint`: **45 OK** (43 before, 2 added). Existing tests changed only
where they read the removed `TPathRef::FieldPath` member — `FieldPaths()`,
`CheckRef()` and four assertions now call the free `FieldPath(ref)`. No expected
string changed.

- `EveryPathFieldRendersAndIsListedOnce` — every enumerator has a non-empty,
  unique template; rendering leaves no `{`/`}` and substitutes `{i}`/`{j}`/
  `{key}`; `KnownPathFieldNames()` is exactly the non-empty proto column,
  deduplicated, sorted, with no empty entry.
- `ExtractedValuesPointIntoTheRequest` — `refs[0].Value.data() ==
  tx.GetMoveTable().GetSrcPath().data()` for MoveTable src/dst, and a sibling
  `BasePath` points at `MoveIndex.TablePath` in the request.

Regression: `ut_cdc_stream` + `ut_auditsettings`, **49 OK**.

### Gotcha worth recording

A nested struct's default member initializers cannot be used in the enclosing
class's default arguments (`void Add(..., TAt at = {})` with `struct TAt` inside
`TRefSink`). Clang rejects it with a cascade of misleading errors, including
"non-const lvalue reference cannot bind to a temporary". `TRefAt` had to move to
namespace scope.

### Commits

- `1157126707` refactor: compile-time path field identities in footprint extractor
- `e901047118` test: pin the path field table and allocation-free extraction

Behavior change: none.

---

## S7e — canonicalize by id → by name, and relocate a database

Plan §8.6. Two pure rewriters over a `TModifyScheme` plus the `TPathFootprint`
that `ResolvePathFootprint` produced *from that same request on the schemeshard
that owns the paths*. Both are layer 3: no `TSchemeShard`, no actor runtime, no
`TPath`. The footprint is the whole reason they can work without one — it has
already turned every path id into a path string and every raw value into an
absolute path.

### API added (`schemeshard_path_footprint.h`)

```cpp
struct TCanonicalizeResult { bool Changed; TVector<EPathField> Untransformable; };
TCanonicalizeResult CanonicalizeToPaths(NKikimrSchemeOp::TModifyScheme&, const TPathFootprint&);

struct TRelocation { TString OldDatabasePath; TString NewDatabasePath; };
struct TRelocateResult { bool Changed; TVector<EPathField> Skipped; };
TRelocateResult RelocatePaths(NKikimrSchemeOp::TModifyScheme&, const TPathFootprint&,
    const TRelocation&);

bool CanRelocatePathField(EPathField);
TVector<EPathField> StripSourceLocalPreconditions(NKikimrSchemeOp::TModifyScheme&);
```

Three supporting changes to existing declarations, all additive:

- `TPathFootprint::WorkingDirCanon` — the resolved working dir, which is what
  every `AbsPath` is built from. `ResolvePathFootprint` computed it at :936 and
  threw it away; a prefix test against the raw `WorkingDir` silently fails on a
  non-canonical client string, which is exactly the case relocation must get
  right. One line to fill in, as S7e's design note recommended.
- `TPathRefOwned::Index` — S7f dropped the repeated-field position after
  rendering it into `FieldPath`, but a setter has to address the same element
  again to write to it. `SubIndex`/`MapKey` are deliberately not kept: no field
  carrying one of those is ever rewritten (they are all `LeafUnderSibling`
  stream names).
- one new field-table row, `ApplyIf_PathId`, marked in the table as not emitted
  by the extractor. It exists so `StripSourceLocalPreconditions` can name what
  it removed; `ApplyIf` is a precondition, not a path the operation touches.

### Canonicalization

All 7 id fields from the design note, 6 name forms, exactly as E1 specified.
The id field is always cleared: in 6 of the 7 `Propose()` ternaries the id form
wins, so writing a name beside a live `PathId` would change nothing.
`AlterTable` clears both `PathId` and `Id_Deprecated` whichever one was
extracted, because `alter_table.cpp:607` takes either over the name.
`SplitMergeTablePartitions` keeps an absolute `TablePath` and leaves
`WorkingDir` alone — `split_merge.cpp:849` never joins it. Transfer needs no
rule of its own: it is `TAlterReplication` read by a different strategy.

The working dir and leaf come from string surgery on the entry's canonical
`AbsPath`, using `RelPathToParent` (which is `TPath::LeafName()`) as the split
hint and a last-slash scan as the fallback. The design note suggested a second
`TPath::Resolve().Parent()`, which would have dragged a `TSchemeShard*` into a
pure function for no gain: `AbsPath` is already canonical.

An id that did not resolve (`AbsPath` empty) is reported in `Untransformable`
and nothing is written. Such a request is rejected by the source schemeshard
anyway, and inventing a name would invent a target.

### Relocation

Rules per `EPathRefKind`, straight from E3:

| kind | rewritten? |
|---|---|
| `Absolute` | always, when the resolved path is at or under the old database |
| `PathUnderWorkingDir` | only when the *raw* value starts with `/` |
| `LeafUnderWorkingDir`, `PathUnderWorkingDirSplit`, `LeafUnderSibling` | never — they hang off the working dir or a base field that moves on its own; rewriting both would double-apply the move |
| `Implicit` | never — no field behind it |
| `ById` | never; recorded in `Skipped`. Canonicalize first |

Plus `WorkingDir := NewDb + "/" + <working dir relative to OldDb>`, done last
so the per-entry decisions read the value the footprint was resolved against.

Never touched, by construction rather than by a special case: replication
`SrcPath` (a path on a remote cluster). The extractor never emits it, and the
setter table has a row only for fields the extractor emits, so there is no way
to write it. A test pins this with a transfer whose `SrcPath` looks exactly
like a local path under the database being moved.

### The setter table

`SCHEMESHARD_PATH_FIELD_SETTERS(X)` in the `.cpp`, next to the code that uses
it rather than in the header, because a row needs protobuf accessors the header
deliberately does not reach for. **42 rows**: 31 `Absolute` fields, 10
`PathUnderWorkingDir` fields, and `AlterTable.Columns[].DefaultFromSequence`,
whose table default is `LeafUnderSibling` but which the extractor promotes to
`Absolute` when its value starts with a slash. `WorkingDirItself` is exempt: it
is synthetic, and the working-dir rewrite covers it. A row is a statement, not
an expression, so the 9 indexed rows bail out through
`SS_PATH_SETTER_GUARD_INDEX` on a footprint whose indexes no longer match the
request.

**Deviation from the brief: the setter does not key on `OperationType`.** The
brief expected `(OperationType, field)` for the three fields whose kind depends
on the operation carrying them. It is unnecessary: `AddAs` already recorded the
*effective* kind in the ref, `Materialize` copies it into
`TPathFootprintEntry::Ref::Kind`, and the rewriter switches on that. The op
type only ever changed the kind, never which protobuf field to write.
`RelocateFollowsThePerOperationKindOfTheSameField` pins this by running the
same `DropCdcStream.TableName` through `ESchemeOpDropCdcStream` (rewritten) and
`ESchemeOpDropCdcStreamAtTable` (left alone).

Second deviation: the rewritten value is `NewDb + "/" + RelativeTo(AbsPath,
OldDb)` rather than `NewDb + "/" + entry.RelPathToDatabase`. The two agree
whenever `OldDatabasePath` is the entry's own database, and only the former is
correct when a request names a path in another database.

### `StripSourceLocalPreconditions`

Clears `TModifyScheme.ApplyIf` and returns one `ApplyIf_PathId` per entry
removed. Documented as **policy, not a semantic no-op**: the request loses its
optimistic-concurrency check and may succeed where the original would have been
rejected. `ApplyIf` has no name form (path id, path version, lock tx id), so it
cannot be canonicalized, only stripped or re-derived against the target state.

### Tests

`ut_path_footprint`: **64 OK** (45 before, 19 added in a new
`TSchemeShardPathFootprintRewrite` suite).

Pure (16). Every one builds its footprint by running the **real** extractor and
faking only the resolution (`FakeResolve`, ~60 lines), so the fields, kinds and
values under test are the production ones:

- canonicalize `DropTable` by id, `AlterTable` by `PathId` (both id forms
  cleared, columns untouched), `SplitMerge` by owner/local id (absolute
  `TablePath`, `WorkingDir` untouched);
- an unresolved id yields `Untransformable == {Drop_Id}` and a byte-identical
  proto; a by-name request is left alone;
- relocate `MoveTable` (both paths + working dir), `CreateTable` (working dir
  only), `CreateConsistentCopyTables` (4 paths across 2 items),
  `AlterUserAttributes` (absolute rewritten, relative not), a request outside
  the old database (nothing changes), a by-id request (`Skipped`, then
  relocatable after canonicalization), a transfer `SrcPath` (never), a split
  child with a leading slash (never), the same field under two op types;
- `EveryRelocatableFieldHasASetter` walks all 145 enumerators and asserts the
  setter table is exactly the relocatable ones, in both directions;
- `StripApplyIf`.

Propose-level (3), against a real schemeshard in a `TTestEnv`, with the request
footprint read back out of the log the hook emits (`FootprintFromLog`, plus
`FillRawValuesFrom` for the raw values the log does not carry):

- `CanonicalizedDropByIdEqualsTheByNameRequest` — a by-id `DropTable` is run
  through the schemeshard, its request footprint captured, a copy canonicalized,
  and the result compared byte for byte against the by-name request the ut
  helper builds for the same table. That is the proof that canonicalization
  matches `Propose()` semantics on real state, not just on a hand-built
  footprint.
- `CanonicalizedAlterTableByPathIdIsAcceptedByName` — same for `AlterTable` by
  `PathId`, and the canonicalized form is then **sent** and accepted, with the
  describe showing both columns.
- `RelocateDrivenByASchemeShardResolvedFootprint` — `MoveTable` relocated using
  the schemeshard-resolved footprint.

Regression: `ut_auditsettings`, **5 OK**.

### Commit

- `96aa3c022f` `feat: canonicalize by-id requests to paths and relocate footprint paths`

Behavior change: none. The two rewriters are new entry points; nothing on the
Propose path calls them yet.

---

## S7i — observer channel, log demoted to DEBUG

Commit `bc83a9c48ad` "feat: path footprint observer channel, log demoted to debug".

### Seams chosen

| seam | choice | why |
|---|---|---|
| production channel | `TAppData::PathFootprintObserver` (`ydb/core/base/appdata_fwd.h:213`), forward-declared beside `NSchemeShard::IOperationFactory` | the notes' §I1 answer. `TSchemeShard` has one two-argument constructor and `CreateFlatTxSchemeShard` forwards exactly those two, so there is no constructor seam; `TTestEnv::TSchemeShardFactory` would be test-only and gives production nothing. One raw pointer, non-const because an observer accumulates. No include added to `appdata_fwd.h`. |
| test installation | `TTestEnvOptions::PathFootprintObserver`, published to every node's `TAppData` right after the `InitYdbDriver` block and before `BootSchemeShard` | mirrors the `YdbDriver` precedent at the same spot; per-node loop mirrors `SetupSchemeCache`. Bootstrap parts (the ~20 `ESchemeOpCreateSysView`) are therefore observed, which the tests filter exactly as they filtered them out of the log before. |
| interface | `IPathFootprintObserver` in `schemeshard_path_footprint.h` with `OnRequestFootprint(TTxId, const TPathFootprint&)` and `OnPartFootprint(TTxId, const TPathFootprint&)` | `TTxId` is passed, not stored on `TPathFootprint`: the struct is per-part state, the tx id is context. |

`IPathResolutionObserver` was declared in the same commit (used by S7h) with a
`class TPath;` forward declaration, so the footprint header still does not pull
`schemeshard_path.h`.

### Gating and cost

`ProcessOperationParts` and the `IgniteOperation` request loop both hoist

```cpp
auto* const footprintObserver = AppData()->PathFootprintObserver;
const bool logFootprints = IS_CTX_LOG_PRIORITY_ENABLED(context.Ctx,
    NActors::NLog::PRI_DEBUG, NKikimrServices::FLAT_TX_SCHEMESHARD, 0ull);
```

out of their loops. With neither, `ResolvePathFootprint` is not called, the
`TMemoryChanges::Mark()` / `PublishedCount()` marks are not taken, and
`TOperation::PathFootprints` / `RequestFootprints` stay empty. Cost in that
configuration is one pointer load plus one `TSettings` lookup per request, not
per part. All five log sites went `LOG_NOTICE_S` -> `LOG_DEBUG_S`.

`schemeshard__operation.h` documents that both vectors are now conditional.
No other consumer exists in the tree (`grep -rn 'PathFootprints|RequestFootprints'`
finds only the hook, the header and this suite), so S7k must not assume they
are populated — the notes' §K3 recommendation stands and is now enforced by
construction.

### Correction to the notes (§I5)

The notes say `TTestEnv::SetupLogging` leaves `FLAT_TX_SCHEMESHARD` at
`PRI_NOTICE` and raises it to DEBUG only when `ENABLE_SCHEMESHARD_LOG` is set.
`ENABLE_SCHEMESHARD_LOG` is initialised to **`true`**
(`ut_helpers/test_env.cpp:31`), so every schemeshard unit test already logs
`FLAT_TX_SCHEMESHARD` at DEBUG. Consequences:

- demoting the log does *not* silence the existing tests, and does not reduce
  test-time footprint cost either — only production cost;
- a test that wants the production default has to ask for it:
  `runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_NOTICE)`
  after `TTestEnv` construction. This cost one build cycle
  (`NoObserverAndNoDebugLogMeansNoFootprint` failed on the first run).

### Test migration

`TLogRecordCollector` + `ParseFootprintLog` + `TFootprintLine` +
`FindLine`/`RequireLine`/`AbsPaths`/`RequireLineByAbsPath`/`SplitPathIds`/
`RequireWriteSetLine` are gone. Replaced by `TFootprintCollector`
(a `TDeque` of `{TTxId, TPathFootprint}` per layer, deque so references stay
valid) plus `Flatten` / `FindEntry` / `RequireEntry` / `RequireEntryByAbsPath` /
`AbsPaths` / `RequirePart` / `AllWriteSetPathIds` over the structs. Assertions
are now typed: `entry.Exists` is a `bool`, `PathId` a `TPathId`, `WriteSet` a
`TVector<TPathId>` compared directly instead of through the log's
`owner:local` joining.

Also removed: `ParseFieldPath`, `ParseKind`, `FootprintFromLog`,
`FillRawValuesFrom` (95 lines). The three propose-level rewrite tests
(`CanonicalizedDropByIdEqualsTheByNameRequest`,
`CanonicalizedAlterTableByPathIdIsAcceptedByName`,
`RelocateDrivenByASchemeShardResolvedFootprint`) used to reconstruct a lossy
`TPathFootprint` out of log text; they now take
`collector.Requests[mark].Footprint` directly, which is the real one.

`TLogRecordCollector` survives for two new tests that pin the other half:

- `DebugLogStillRendersTheFootprint` — with DEBUG on, the part line, the
  request line and the write-set line all still render, with the same field
  grammar;
- `NoObserverAndNoDebugLogMeansNoFootprint` — with the priority lowered to
  NOTICE and no observer, no `PathFootprint` line is emitted at all.

## S7j — ResolveWithInactive for the Move* family

Commit `0f3d692d12e` "fix: footprint resolves move destinations with inactive-aware lookup".

`ResolvePathFootprint(tx, ss, TOperationId opId = InvalidOperationId)`. The
sentinel is `InvalidOperationId`, **not** `{}`: `TOperationId` is a
`std::pair<TTxId, TSubTxId>` whose `explicit operator bool` makes a
value-initialised `(0, 0)` truthy (notes §J3). The guard is `bool(opId)`.

`ResolvesTargetWithInactive()` returns true for exactly
`ESchemeOpMoveTable`, `ESchemeOpMoveTableIndex`, `ESchemeOpMoveSequence` — the
three ops whose `Propose()` calls `TPath::ResolveWithInactive`.
`ESchemeOpMoveIndex` is excluded (its paths are `LeafUnderSibling` under
`MoveIndex.TablePath`, and it expands into `MoveTableIndex` parts that are
covered). `ESchemeOpCreateColumnTable` is excluded per notes §J4: the
operation's own `ResolveWithInactive` call in `read_only_copy_table.cpp:502`
passes a bare leaf name, so its head-match can never fire — left alone, not
fixed here.

Only `Role == Target` refs of those ops take the new branch. Sources keep the
plain resolver, which is what `move_table.cpp` / `move_sequence.cpp` do.
`schemeshard__operation.cpp` passes `part->GetOperationId()`; the
`IgniteOperation` request loop keeps the two-argument form.

### Gap, stated honestly

The new branch is exercised by `MoveIndexedTableResolvesEveryMoveDestination`
(MoveTable + derived MoveTableIndex parts, plus the request-level footprint
still on the plain resolver), but that test pins **no behaviour flip**: it
proves the inactive-aware path is taken and returns the same correct answers.

A request where the two resolvers actually disagree needs
`TSchemeShard::AttachChild` to *reject* linking the new element into its
parent's `Children` map, so that `TPath::Resolve` by name still finds the old
element while `TPath::Init(TargetPathId)` finds the new one — plus the head
operation's `TargetPathId` being exactly the destination's parent
(`headPathNameParts.size() + 1 == pathParts.size()`,
`schemeshard_path.cpp:1517`). Neither `TestMoveTable` nor a two-transaction
`TEvModifySchemeTransaction` built from the existing helpers can produce that
shape: a plain MkDir-then-move leaves the intermediate directory properly
attached, so both resolvers agree. The change is monotone (falling through to
`Resolve` is today's behaviour), so this is a correctness alignment with
`Propose()` rather than a demonstrated bug fix. `ut_move` stays green.

## S7h — read-set recorder

Commit `a8340bfbf25` "feat: read-set recorder for path footprints behind an observer hook".

### Seams chosen

- `TSchemeShard::PathResolutionObserver` (`schemeshard_impl.h`, beside
  `RootPathElements`, which is already in the public member block). Raw
  pointer, not `std::function`: `TPath::Dive` runs ~87k times per six-suite
  run and sits under everything `Propose()` does. Forward-declared
  `class IPathResolutionObserver;` in `schemeshard_impl.h` rather than
  including the footprint header there.
- Choke points are exactly `TPath::Dive` and `TPath::Init`, as the notes
  established. `Dive` had five returns, so it was split into a private
  `DiveImpl` (the unchanged body, `return;` instead of `return *this;`) plus a
  three-line `Dive` that calls it and notifies once. `Init` has two returns and
  notifies at both, including the unknown-path-id one.
- Hot-path cost: one `Y_UNLIKELY` load-and-branch off `SS`, which `Dive`
  already dereferences. Null in production; armed only inside
  `ProcessOperationParts`.

### Deviation from the notes

The notes proposed `IPathFootprintObserver::PathResolutionObserver()` returning
a caller-supplied recorder. That was replaced by
`virtual bool WantReadSet() const { return false; }`, and the hook owns a
`TPathReadSetRecorder` bound to the current footprint's `ReadSet`. Reason: with
a caller-supplied recorder the observer would need a second call to learn which
part it is recording for; with the footprint-owned recorder the association is
structural and the read set lands where every consumer already looks. One
mechanism instead of two.

### Arming

`TPathReadSetRecorder recorder(footprint.ReadSet);` inside the `else` branch,
installed only when `wantReadSet`, with a `Y_DEFER` that nulls the pointer
whether `Propose()` returns or throws. It covers `part->Propose(...)` and
nothing else — deliberately not the footprint's own `ResolvePathFootprint`
above it, nor the `IgniteOperation` request loop, either of which would make
the coverage assertion vacuous.

### Collapse

`TPathReadSetRecorder::OnPathResolved` keeps only the maximal path of a `Dive`
chain: a name step whose path strictly extends the entry recorded immediately
before it replaces that entry. One `TPath::Resolve("/MyRoot/a/b/T")` yields one
read, not four. Id lookups never collapse into or out of a name chain.

### Coverage predicate and result

`ReadSetStaysInsideTheFootprint` drives the whole propose-level op mix in one
run (multi-segment CreateTable, CreateIndexedTable, CreateCdcStream, MoveTable
of an indexed table, DropTable, a rejected CreateTable) with the env bootstrap
parts observed too, and asserts every recorded read is covered by:

1. a path id in the part's `WriteSet` or `Published` (compared by id, since the
   write set carries ids, not strings);
2. an entry's `AbsPath`, or an ancestor of one (segment-wise prefix);
3. inside the subtree anchored by an `Implicit` entry;
4. `WorkingDirCanon` or an ancestor of it, which covers the root and the
   domain path;
5. the root or an empty path, which say nothing.

**Zero violations on the first run.** No allowlist was needed and none was
added. The test also asserts the recorder is not vacuous: the read set must
mention `/MyRoot`, `/MyRoot/a/b/Table`,
`/MyRoot/Indexed/byValue/indexImplTable` and `/MyRoot/a/b/Table/Stream` — the
last two are paths no proto field of their requests names.

`WantReadSet()` is false in the default `TFootprintCollector`, so only this one
test pays the per-`Dive` virtual call; the rest of the suite is unaffected.

### Diffstat vs main, pre-existing files only

```
 ydb/core/base/appdata_fwd.h                        |   4 +
 ydb/core/tx/schemeshard/schemeshard__operation.cpp | 125 ++++++++++++++++---
 ydb/core/tx/schemeshard/schemeshard__operation.h   |  19 +++
 ydb/core/tx/schemeshard/schemeshard_impl.h         |  13 ++
 ydb/core/tx/schemeshard/schemeshard_path.cpp       |  30 +++--
 ydb/core/tx/schemeshard/schemeshard_path.h         |   3 +
 ydb/core/tx/schemeshard/ut_helpers/test_env.cpp    |   8 ++
 ydb/core/tx/schemeshard/ut_helpers/test_env.h      |   4 +
 8 files changed, 191 insertions(+), 15 deletions(-)
```

`schemeshard_path.cpp`'s 30 lines are almost entirely the `Dive`/`DiveImpl`
split; the logic is byte-identical apart from `return *this;` -> `return;`.

### Test summaries

```
ut_path_footprint                          {"type": "summary", "exit_code": 0, "tests": {"OK": 68}}
ut_cdc_stream + ut_auditsettings + ut_move {"type": "summary", "exit_code": 0, "tests": {"OK": 90}}
```

`ut_path_footprint` went 64 -> 68 tests: `DebugLogStillRendersTheFootprint`,
`NoObserverAndNoDebugLogMeansNoFootprint` (S7i),
`MoveIndexedTableResolvesEveryMoveDestination` (S7j),
`ReadSetStaysInsideTheFootprint` (S7h). `ya` emits only aggregate summaries
when every suite passes, so there is no per-suite line to paste for the three
regression suites; the run covered all three in one invocation.

## S7g — replay experiment as a test

Commit `218487d1847` "test: replay relocated scheme requests into a second
database and diff describe trees". New file
`ydb/core/tx/schemeshard/ut_path_footprint/ut_replay.cpp` (~690 lines), added to
the ut's `SRCS`. **No production code changed.** `ut_path_footprint`: **71 OK**
(68 before, 3 added).

### Setup: one runtime, two real subdomains

The design note's G1/G2 recommendation held without modification. One
`TTestBasicRuntime`, one `TTestEnv`, one schemeshard;
`/MyRoot/dbA` and `/MyRoot/dir/dbB` created with `TestCreateSubDomain` and
`StoragePools { Name: "pool-1" Kind: "pool-kind-1" }` — the pool `TTestEnv`
registers by default (`DefaultPoolKinds`, `tablet_helpers.cpp:657`). Both
subdomains name the same pool and the schemeshard accepts that. dbB is nested
one directory deeper on purpose: relocation then *adds* a segment, so a
suffix-only rewrite would be caught.

`SendAndWait` is three lines over `AsyncSend` + `GrabEdgeEvent`, returning the
status instead of asserting it — the experiment has to be able to *record*
"accepted on A, rejected on B". The observer is a request-only
`IPathFootprintObserver` (`OnPartFootprint` ignored): part footprints describe
operations the schemeshard derived for itself, and replaying those would double
apply, per §8.7.

### The sequence and the per-request result

Every request is hand-built in the test, applied to dbA, then a copy is put
through `StripSourceLocalPreconditions` → `CanonicalizeToPaths` →
`RelocatePaths({"/MyRoot/dbA", "/MyRoot/dir/dbB"})` driven by *the footprint the
schemeshard resolved for that very request*, and sent to dbB.

| # | request | status A | status B | changed | untransformable | skipped |
|---|---|---|---|---|---|---|
| 1 | `MkDir dir` | Accepted | Accepted | yes | - | - |
| 2 | `MkDir a/b` | Accepted | Accepted | yes | - | - |
| 3 | `CreateTable t` | Accepted | Accepted | yes | - | - |
| 4 | `AlterTable t` add column | Accepted | Accepted | yes | - | - |
| 5 | `CreateIndexedTable it` (+`by_value`) | Accepted | Accepted | yes | - | - |
| 6 | `CreateCdcStream Stream` on `t` | Accepted | Accepted | yes | - | - |
| 7 | `CreateTable copy CopyFromTable <db>/dir/t` | Accepted | Accepted | yes | - | - |
| 8 | `MoveTable copy -> moved` | Accepted | Accepted | yes | - | - |
| 9 | `DropTable moved` by name | Accepted | Accepted | yes | - | - |
| 10 | `CreateTable tmp` | Accepted | Accepted | yes | - | - |
| 11 | `DropTable tmp` by id | Accepted | Accepted | yes | - | `Drop.Id` |

11/11 accepted on both sides. Nothing untransformable. The single `Skipped`
entry is not a failure: `RelocatePaths` walks the footprint of the *original*
request, so it still sees the `ById` ref that `CanonicalizeToPaths` has just
removed from the proto, and reports it. The test asserts exactly that shape
(`Drop.Id` for the by-id step, empty for the other ten) rather than asserting
`Skipped` empty.

`CopyFromTable` from a table that already carries a cdc stream (step 7 after
step 6, as G7 prescribed) was accepted on both sides and produced identical
trees — the anticipated copy-semantics divergence did not materialise.

### Finding: the two rewriters do not compose over one footprint

**This cost a build cycle and is the one non-obvious result.** The first run
failed on step 11 with `StatusPathDoesNotExist` on B. Cause:

- `CanonicalizeToPaths` turns `Drop.Id` into `WorkingDir` + `Drop.Name`,
  deriving the working dir from the entry's `AbsPath`;
- `RelocatePaths` rewrites the working dir from `TPathFootprint::WorkingDirCanon`
  (`schemeshard_path_footprint.cpp:1651`), which still describes the request
  **as submitted**;
- a by-id request typically carries no `WorkingDir` at all, so `WorkingDirCanon`
  is not under the old database, the rewrite is skipped, and the working dir
  canonicalization just invented — pointing at **dbA** — survives into the
  replayed request. The schemeshard then re-drops in the source database.

The pure S7e test `RelocateSkipsAByIdRequest` does not hit this because it
re-runs `FakeResolve` on the canonicalized request before relocating. That
re-resolution is a real requirement of the API, not test scaffolding.

Two ways out for a consumer, both now pinned by
`CanonicalizeThenRelocateNeedsTheWorkingDir` (propose-level, two tables, one
probe each):

1. re-resolve the footprint of the canonicalized request on the source
   schemeshard before relocating (correct, costs a second `ResolvePathFootprint`);
2. make sure the by-id request carries a working dir under the database being
   moved (what the experiment does; a by-id `Propose()` ignores it, so setting
   it is free).

Classification against §4 of `thoughts-replay-completeness.md`: this is not one
of the seven classes. It is a **defect in the layer-3 API contract**, and the
cheapest fix is a doc line on `RelocatePaths` plus, optionally, having
`CanonicalizeToPaths` return the working dir it wrote so a caller can patch the
footprint. Recommend adding it to §9 as S7m.

### Masking and the tree diff

No recursive-describe helper exists in the tree, as the notes said, so the test
has one (~25 lines) using `TDescribeOptionsBuilder().SetShowPrivateTable(true)`.
Two corrections to G5/G6 found empirically:

- **`GetChildren()` is empty for a table.** A table's describe sets
  `ChildrenExist: true` but returns no `Children`; its indexes and cdc streams
  are only listed inside `TTableDescription.TableIndexes` / `.CdcStreams`. Walking
  `Children` alone visited **5** paths and missed every derived path the
  experiment exists to check. `ChildNames()` unions the three sources.
- Children are sorted by name before recursing *and* the `Children` repeated
  field is sorted in place, because `TPathElement` insertion order differs
  between the two trees.

Masking is one recursive reflection pass over `google::protobuf::Message` keyed
on `field->full_name()` (~50 lines), plus a prefix rewrite of every `TYPE_STRING`
value from `<database path>` to `<db>` (so `TTableDescription.Path`,
`TEvDescribeSchemeResult.Path` and friends are normalised rather than dropped).
`MaskedFieldsExist` walks the descriptor graph from `TEvDescribeSchemeResult` and
fails if any masked key is unreachable, so a renamed field cannot silently stop
being masked.

Masked (all target-decided, none named by any request):

- `TEvDescribeSchemeResult`: `PathId`, `PathOwnerId`, `DEPRECATED_PathOwner`,
  `LastExistedPrefixPathId`;
- `TDirEntry`: `PathId`, `SchemeshardId`, `CreateTxId`, `CreateStep`,
  `ParentPathId`, `PathVersion`, `Version`, `ACL`, `EffectiveACL`
  (`EffectiveACL` inherits from the domain, so it differs by construction);
- `TPathDescription`: `TablePartitions`, `TableStats`, `TabletMetrics`,
  `TablePartitionStats`, `TablePartitionMetrics`, `AbandonedTenantsSchemeShards`,
  `BackupProgress`, `LastBackupResult`, `DomainDescription`;
- `TTableDescription`: `Id_Deprecated`, `PathId`, `TableSchemaVersion`,
  `CoordinatedSchemaVersion`, `PartitionConfig`, `UniformPartitionsCount`,
  `SplitBoundary`, `PartitionRangeBegin/End`;
- `TIndexDescription`: `LocalPathId`, `PathOwnerId`, `SchemaVersion`, `DataSize`;
  `TCdcStreamDescription`: `PathId`, `SchemaVersion`;
  `TSequenceDescription.PathId`;
- `TPersQueueGroupDescription`: `PathId`, `Partitions`, `BalancerTabletID`,
  `AlterVersion`, `NextPartitionId`, `PQTabletConfig` (the topic config carries
  the owning path ids, the database path and the per-partition tablet map).

Plus one explicit exception: the **root node's `Self.Name`** is cleared. The two
databases are deliberately named differently and the replay is not supposed to
reproduce the database's own name.

Deviation from G6: `TTableDescription.Path` and `KeyColumnIds` are **not**
masked. `Path` is normalised by the prefix rewrite instead, and `KeyColumnIds`
turned out identical in both trees, so it stays as a real assertion.

### Result

After the walk fix, the two trees are **byte-identical under masking** at all
ten paths:

```
<db>
<db>/a
<db>/a/b
<db>/dir
<db>/dir/it
<db>/dir/it/by_value
<db>/dir/it/by_value/indexImplTable
<db>/dir/t
<db>/dir/t/Stream
<db>/dir/t/Stream/streamImpl
```

The path list is asserted verbatim, so a future change that stops reaching
`indexImplTable` or `streamImpl` fails rather than silently shrinking the diff.

**Zero divergences to bucket into the §4 classes.** That is a real but narrow
result: the sequence was chosen (per §8.7 and G7) to avoid every known
non-determinism — no `BackupBackupCollection` (`TargetDir` stamped with `Now()`),
no continuous backup (timestamped stream names), no index build (data-dependent),
no split/merge (internal), no ACLs, no principals, no data. What the experiment
establishes is the *positive* half of the §8.7 claim: for requests outside those
classes, canonicalize + relocate + replay does reproduce the same logical tree,
including the derived children (index impl table, cdc stream PQ group) that no
request ever names. The §4 classes remain untested by construction, and their
sizes are still unquantified.

### Follow-ups

- **S7m (new)**: fix the canonicalize→relocate composition, either by
  documenting the re-resolution requirement on `RelocatePaths` or by having
  `CanonicalizeToPaths` report the working dir it wrote.
- Extending the experiment into the §4 classes needs a completion filter (record
  at Propose, publish at Done) first; §8.7's "accepted ≠ committed" is not
  exercised here because every request in the sequence completes.

---

## S7m — the canonicalize/relocate contract

Commit `e1052ae143f` `fix: canonicalized requests carry an updated footprint for relocation`.

### The defect, restated

S7g found it: `CanonicalizeToPaths` writes a working dir derived from the
entry's `AbsPath`, `RelocatePaths` rewrites `TPathFootprint::WorkingDirCanon`,
and the footprint between them still described the request **as submitted**. A
by-id request normally carries no `WorkingDir` at all — `Propose()` ignores it —
so `WorkingDirCanon` was not under the old database, the working-dir rewrite was
skipped, and the dir canonicalization had just invented (pointing at the source
database) survived into the replayed request.

### API change

```cpp
-TCanonicalizeResult CanonicalizeToPaths(TModifyScheme& tx, const TPathFootprint& fp);
+TCanonicalizeResult CanonicalizeToPaths(TModifyScheme& tx, TPathFootprint& fp);
```

The footprint is now patched in place to describe the rewritten request, which
is exactly what `RelocatePaths` needs:

| field | new value |
|---|---|
| `WorkingDir`, `WorkingDirCanon` | the parent directory cut out of the entry's `AbsPath`; canonical already, so both forms are the same string |
| `WorkingDirRelToDb` | that dir relative to the entry's own database |
| `DatabasePathId` | the entry's `DatabasePathId`, not the one the submitted working dir resolved to |
| every entry's `RelPathToWorkingDir` | recomputed against the new working dir |
| the rewritten entry's `Ref` | `Field` → the name form (`Drop_Name`, `AlterTable_Name`, `AlterPersQueueGroup_Name`, `AlterBlockStoreVolume_Name`, `AlterReplication_Name`, `SplitMergeTablePartitions_TablePath`), `FieldPath` re-rendered, `Kind` → `LeafUnderWorkingDir` (`Absolute` for SplitMerge), `Value` → the leaf (the absolute path for SplitMerge), `OwnerId`/`LocalPathId` cleared |

`SplitMergeTablePartitions` still does not touch the working dir: its
`TablePath` is absolute.

Retargeting the entry is not cosmetic. Without the `Field` rewrite the setter
table would still be asked for `SplitMergeTablePartitions.TableLocalId`, which
has no setter, so a canonicalized SplitMerge would land in `Skipped` and never
be relocated. With it, `RelocatePaths` writes the new `TablePath`.

Two supporting helpers in the .cpp: `DatabasePathOfEntry` (recovers the database
path as `AbsPath` minus `RelPathToDatabase`; empty when the two do not line up,
which is what a footprint records for an unresolved database) and
`MoveFootprintWorkingDir`.

Documented precondition, not enforced: canonicalization moves the request's
working dir, so a request that also carried a working-dir-relative field would
change meaning. No operation combines a by-id field with one — `AlterTable`'s
`DefaultFromSequence` hangs off the altered table, not off the working dir — so
there is a header comment rather than a check.

`RelocatePaths` gained a doc paragraph: `fp` must be the footprint of `tx` as it
stands now; after `CanonicalizeToPaths` pass the footprint it patched, after any
other edit re-resolve.

### Tests

`ut_path_footprint`: **72 OK** (71 before).

- New pure test `CanonicalizeThenRelocateWithoutAWorkingDir`: a by-id
  `DropTable` with an empty `WorkingDir`, canonicalized and then relocated with
  the same footprint, ends with `WorkingDir` = `/MyRoot2/x/db2/dir` and
  `Drop.Name` = `T`. Before the fix the working dir stayed at `/MyRoot/db1/dir`.
- `CanonicalizeDropTableById` and `CanonicalizeSplitMergeKeepsTheAbsoluteTablePath`
  now also assert the patched footprint (working dir, entry field, kind, value,
  `RelPathToWorkingDir`).
- `RelocateSkipsAByIdRequest` drops its `FakeResolve` re-resolution and reuses
  the patched footprint.
- `ut_replay.cpp`: the workaround is gone. Step 11 sends the by-id drop **with
  no working dir at all**, which is how such a request normally arrives, and the
  report now asserts `Skipped` empty for every step including that one — the
  entry is no longer `ById` by the time relocation walks it. 11/11 still
  accepted on both databases, trees still byte-identical under masking.
- `CanonicalizeThenRelocateNeedsTheWorkingDir` became
  `CanonicalizeInventsTheWorkingDirRelocationMoves`: both probes (no working
  dir, and a working dir the client sent) land in dbB.

---

## S7k — the audit log's paths come from the extractor

Commit `588dc033306` `feat: audit log paths come from the path footprint extractor`.

### What was implemented

`JoinPathRef(workingDir, ref, joined)` in `schemeshard_path_footprint.{h,cpp}`:
one ref joined into a path string out of the request alone, mirroring
`ResolvePathFootprint`'s kind switch minus `TPath`. `joined` holds the strings
already produced for earlier refs, which is how a `LeafUnderSibling` whose base
is an anchor (a by-id base, or a split child) resolves. `ById` and `Implicit`
return empty.

One deliberate difference from `NKikimr::JoinPath`: an empty leaf yields the
directory itself rather than a trailing slash. That keeps `CreateFullBackupOp`
(a `WorkingDirItself` ref with an empty value) printing its working dir exactly
as before, and it is what turns the id-bypass bug into an omitted field rather
than `"/MyRoot/"`.

`ExtractChangingPaths` shrank from 415 lines and 136 arms to a 20-line filter
keeping refs whose role is `Target` or `Source`. Three includes the switch
needed (`base/path.h`, `index_builder.pb.h`, `subdomains.pb.h`) went with it.

One exception survives: `ESchemeOpAlterLogin` returns `{WorkingDir}`. It
resolves no `TPath`, so the extractor emits nothing for it, but the record has
always shown the working dir and for a login operation that is the whole of what
it touches. Dropping it would have been a silent regression in the one family
whose records nobody else emits.

### ById: not resolved, and why

The notes offered threading `TOperation::RequestFootprints` into
`AuditLogModifySchemeTransaction`. Not done, because of what S7i created:
footprints are computed **only** when an `IPathFootprintObserver` is installed or
`FLAT_TX_SCHEMESHARD` admits DEBUG. An audit consumer reading them would get
resolved paths in one configuration and nothing in another — a worse contract
than consistently omitting the field. K3's other two reasons stand:
`Self->Operations[txId]` may already be erased by `Complete`, and by-id requests
are roughly 1 in 400 (S3 §4). The code says so in a comment.

### Audit output, old → new

Everything below is client-submittable. Derived `*Impl`/`*AtTable` parts are
unreachable from `AuditLogModifySchemeTransaction`, which iterates the client's
transactions; several of them do gain their stream leaves, which nothing reads.

**Corrected (the buggy families).**

| family | old | new |
|---|---|---|
| `CreateResourcePool`, `AlterResourcePool` | `MyResourcePool` | `/MyRoot/.metadata/workload_manager/pools/MyResourcePool` |
| `DropResourcePool` | `MyResourcePool` | same, absolute |
| `CreateStreamingQuery`, `AlterStreamingQuery` | `MyStreamingQuery` | `/MyRoot/MyStreamingQuery` |
| `DropStreamingQuery` | `MyStreamingQuery` | `/MyRoot/MyStreamingQuery` |
| `TruncateTable` | `Table` | `/MyRoot/Table` |
| `SplitMergeTablePartitions` by `TablePath` | `/MyRoot//MyRoot/Table` | `/MyRoot/Table` |
| `AlterSequence` | *(no field)* | `/MyRoot/seq` |
| `AlterReplication`, `AlterTransfer` by name | *(no field)* | `/MyRoot/repl` |
| `AlterExternalTable` | *(no field)* | `/MyRoot/et` |
| `AlterExternalDataSource` | *(no field)* | `/MyRoot/ds` |
| `CreateColumnTable` | `/MyRoot/` — it read `AlterColumnTable.Name` | `/MyRoot/ct` |
| `AlterColumnTable` without the `AlterColumnTable` submessage | `/MyRoot/` | `/MyRoot/` + `AlterTable.Name` |
| the id bypass: `Drop.Id` (~22 op types), `AlterTable.PathId`/`Id_Deprecated`, `AlterPersQueueGroup.PathId`, `AlterBlockStoreVolume.PathId`, `AlterReplication.PathId`, `SplitMerge.TableLocalId` | `/MyRoot/` | *(field omitted)* |
| any `PathUnderWorkingDir` field whose value is absolute (`AlterUserAttributes.PathName`, `DropIndex.TableName`, `ChangePathState.Path`, `TruncateTable.TableName`, the CDC `TableName`s, `IncrementalRestoreLockTargets` paths) | `/MyRoot//abs/path` | `/abs/path` |

**Added (families that were not buggy, but that name a path the old switch
dropped).** These follow from the `Target || Source` filter the design note
recommended. The alternative, `Target` only, would have *removed*
`MoveTable.SrcPath`, `RotateCdcStream.OldStreamName` and
`RestoreMultipleIncrementalBackups.SrcTablePaths`, which today's output already
carries — a bigger diff than these four additions.

| family | old | new |
|---|---|---|
| `CreateTable` with `CopyFromTable` | `/MyRoot/dst` | `/MyRoot/dst`, `/MyRoot/src` |
| `CreateConsistentCopyTables` | `dst` per item | `src`, `dst` per item |
| `CreateContinuousBackup` with an explicit `StreamName` | `/MyRoot/T` | `/MyRoot/T`, `/MyRoot/T/S` |
| `AlterContinuousBackup` with `TakeIncrementalBackup` | `/MyRoot/T` | `/MyRoot/T`, `/MyRoot/bak`, `/MyRoot/T/S` |

Without an explicit stream name the continuous-backup families are unchanged:
the schemeshard generates the name from the clock, so the request does not spell
it out and there is nothing to report.

**Verified byte-identical**, asserted one by one in the new suite: `MkDir`,
`CreateTable`, `DropTable`/`AlterTable`/`ModifyACL` by name, `CreateSubDomain`,
`AlterUserAttributes` (relative), `MoveTable`, `MoveIndex`, `DropIndex`,
`CreateCdcStream`, `DropCdcStream` (several streams), `RotateCdcStream`,
`CreateIndexBuild`, `CreateIndexedTable`, `AlterLogin`, `CreateFullBackupOp`,
`IncrementalRestoreLockTargets`, `DropContinuousBackup`. No dedupe was added:
the old implementation had none, and the new one produces no duplicate a family
did not already produce.

### Tests

- New pure suite `TSchemeShardAuditLogPaths` in `ut_path_footprint.cpp`
  (8 tests, ~50 assertions), driven through `MakeAuditLogFragment(tx).Paths` so
  it pins the public entry point rather than a file-static helper. No `TTestEnv`.
- `ut_path_footprint`: **80 OK**. `ut_auditsettings`: **5 OK**, no golden
  change, exactly as K2 predicted — it tests the `TAuditSettings` proto field and
  never inspects an audit line.
- One crash on the first run, and it was the test's fault, not the code's:
  `DefineUserOperationName` aborts on an `ESchemeOpAlterLogin` whose `AlterLogin`
  oneof is unset, so the AlterLogin case now sets `CreateGroup`.
- No C++ suite asserts on `paths=`. `grep -rn '"paths"' ydb/ --include=*.cpp`
  finds the emitter, this new suite, and `audit_log_service_ut.cpp`, which builds
  its parts literally.

### Left undone, deliberately

`ydb/tests/functional/audit/canondata/result.json` references its audit blobs by
`file://` sandbox URI, so regenerating it needs the canonical-data workflow
(`ya make -Z`) and cannot be done from this tree. Two tests reference schemeshard
`ModifyScheme` records: `test_canonical_records.test_create_drop_and_alter_database`
(5 blobs) and `test_create_drop_and_alter_table` (3 blobs). The table test
exercises `CreateTable`/`AlterTable`/`DropTable` by name and the topic test
`CreatePersQueueGroup`/`AlterPersQueueGroup` by name, all in the unchanged set.
The database test also captures tenant initialization, which creates the default
resource pool, so its schemeshard blobs are expected to change exactly the way
the resource-pool row above says. Regenerate them before this reaches a release
branch.
