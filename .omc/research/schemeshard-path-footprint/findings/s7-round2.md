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

