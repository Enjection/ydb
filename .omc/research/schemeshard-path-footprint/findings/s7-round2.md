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
