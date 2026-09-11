# S2 — Compound/derived-op map for SchemeShard `TModifyScheme`

Scope: every operation type whose single client request becomes more than one
`ISubOperation` part (or generates extra transactions via
`TOperation::SplitIntoTransactions`), with the exact formula for each derived
part's `WorkingDir` and name/path field(s), and whether that formula needs
SchemeShard state beyond the request.

All line numbers verified against the tree on 2026-09-02. `TransactionTemplate`
is `ydb/core/tx/schemeshard/schemeshard__operation_part.h:804`:

```cpp
inline NKikimrSchemeOp::TModifyScheme TransactionTemplate(const TString& workingDir, NKikimrSchemeOp::EOperationType type) {
    NKikimrSchemeOp::TModifyScheme tx;
    tx.SetWorkingDir(workingDir);
    tx.SetOperationType(type);
    return tx;
}
```

A part built this way starts with only `WorkingDir`+`OperationType` set; the
caller then fills its own sub-message (`Name`, `Drop.Name`, etc). Two
recurring exceptions to "derived proto" are worth flagging up front because
they break the "per-part proto is a fresh object" assumption:

- Several compound ops push the **original, unmodified `tx`** as one of their
  own parts (`CreateDropTable(nextId, tx)` in `CreateDropIndexedTable`,
  `CreateMoveColumnTableLocalIndex(NextPartId(...), tx)` in
  `CreateConsistentMoveLocalIndex`). For those parts, `part->GetTransaction()`
  *is* the client request, byte for byte — attribution is trivial (part 0 ==
  whole original proto).
- Several CDC parts don't build a fresh sub-message; they
  `outTx.MutableCreateCdcStream()->CopyFrom(op)` (or `MutableDropCdcStream`,
  `MutableAlterCdcStream`, `MutableRotateCdcStream`) — i.e. the derived part
  carries a **full copy of the original op's payload**, just under a new
  `WorkingDir` and a new `OperationType`. `ProtoRef` for such a part is "same
  fields as the original request's `CreateCdcStream`/etc., just re-homed".

---

## 1. `SplitIntoTransactions` — auto-generated `MkDir`s (pre-`MakeOperationParts`)

`ydb/core/tx/schemeshard/schemeshard__operation.cpp:908-1042`, helper
`CreateDirs` at `:845`, per-op-type gating via `schemeshard__op_traits.h`
(`TSchemeTxTraits<opType>::CreateDirsFromName` / `CreateAdditionalDirs`).

This runs **before** `MakeOperationParts`/`ConstructParts` and mutates the
*original* transaction's `WorkingDir`/name field before the per-op-type
switch ever sees it — so it's a layer-0 rewrite, not a `MakeOperationParts`
case, but it does turn one client request into N+1 transactions.

Two independent mechanisms, both string-decomposition of a path, no schemeshard
state needed for the **formula** (state is only needed to decide *how many*
intermediate dirs are missing — i.e. whether to stop early):

### 1a. `CreateDirsFromName` (most `Create*` op types: MkDir, CreateTable,
CreatePersQueueGroup, CreateSubDomain/ExtSubDomain, CreateRtmrVolume,
CreateBlockStoreVolume, CreateFileStore, CreateKesus, CreateSolomonVolume,
CreateIndexedTable, CreateColumnStore, CreateColumnTable, CreateExternalTable,
CreateExternalDataSource, CreateView, CreateResourcePool, CreateBackupCollection,
CreateSequence, CreateReplication, CreateTransfer, CreateSecret,
CreateStreamingQuery, CreateTestShardSet — full list at
`schemeshard__op_traits.h:29-217`)

Given `WorkingDir = /Root/some_dir`, `ObjectName = other_dir/another_dir/object_name`
(a multi-segment name — `GetTargetName`/`SetName` per-trait accessors read/write
the op-type-specific name field, e.g. `CreateTable.Name`,
`CreateIndexedTable.TableDescription.Name`):

- Splits `ObjectName` on `/`. For every segment except the last, if that path
  doesn't already resolve to an existing directory, emit
  `MkDir{WorkingDir: <accumulated prefix>, MkDir.Name: <segment>}`
  (`schemeshard__operation.cpp:889-898`, inside `CreateDirs`).
- Rewrites the *original* transaction's `WorkingDir` to the full accumulated
  prefix and its name field to the last segment only (`:975-988`).
- **Formula is 100% derivable from the request's `WorkingDir`+name field
  alone** (pure path decomposition); SchemeShard state (`TPath::Resolve`) is
  only consulted to short-circuit already-existing prefixes and to reject
  invalid parents.

### 1b. `CreateAdditionalDirs` (`BackupBackupCollection`,
`BackupIncrementalBackupCollection`, `RestoreBackupCollection` —
`schemeshard__op_traits.h:157-176`)

Calls the op-type's `NOperation::GetRequiredPaths` specialization
(`schemeshard__op_traits.h:249-256`), which for these three ops is
`NBackup::GetBackupRequiredPaths`/`GetRestoreRequiredPaths`
(`schemeshard__backup_collection_common.cpp:100`, `:213`) — see §7 below.
**This is state-derived**: the set of directories needed comes from the
backup collection's *stored* `ExplicitEntryList` (schema state), not from any
repeated field on the request. Each `(parentPathStr, {segment...})` pair from
that map becomes one or more `MkDir`s via the same `CreateDirs` walk as 1a.

---

## 2. `CreateConsistentCopyTables` (ESchemeOpCreateConsistentCopyTables)

`ydb/core/tx/schemeshard/schemeshard__operation_consistent_copy_tables.cpp:130-386`
(bool overload) / `:388-390` (vector wrapper). **The single most complex
derived-parts case** — also the entry point used internally by
`BackupBackupCollection` and `RestoreBackupCollection`.

Request: `CreateConsistentCopyTables.CopyTableDescriptions[]` (repeated
`TCopyTableConfig`), each with absolute `SrcPath`/`DstPath` plus optional
`CreateSrcCdcStream`, `DropSrcCdcStream`, `IndexImplTableCdcStreams` (map
`"IndexName/ImplTableName" -> TCreateCdcStream`),
`IndexImplTableDropCdcStreams` (same key shape), `OmitIndexes`,
`TargetPathTargetState`.

For **each** `descr` in `CopyTableDescriptions[i]` (repeated-index `i`):

| # | Part OpType | WorkingDir | Name/path fields | Source | Conditional |
|---|---|---|---|---|---|
| 1 | `CreateTable` or `CreateColumnTable` (`CopyAnyTableTask`, `:22-51`) | `dstPath.Parent()` | `Name = dstPath.LeafName()`, `CopyFromTable = srcPath` (absolute) | `CopyTableDescriptions[i].SrcPath`/`DstPath` (request, resolved) | table type (row vs column) picked by `srcPath->IsTable()`, i.e. schemeshard state |
| 1a | embedded in part 1's own proto, not a separate part | — | `CreateTable.CreateCdcStream = descr.CreateSrcCdcStream` (verbatim copy) | `CopyTableDescriptions[i].CreateSrcCdcStream` | only if `descr.HasCreateSrcCdcStream()` |
| 1b | embedded in part 1 | — | `CreateTable.DropSrcCdcStream = descr.DropSrcCdcStream` | `CopyTableDescriptions[i].DropSrcCdcStream` | only if `HasDropSrcCdcStream()` |
| 1c | embedded in part 1 | — | `CreateTable.PathState = descr.TargetPathTargetState` | `CopyTableDescriptions[i].TargetPathTargetState` | only if present |
| 2 | `CreateTableIndex` or `CreateTableIndexLocal` (`CreateIndexTask`, `:53-116`) | `dstIndexPath.Parent()` = `dstPath` | `Name = dstIndexPath.LeafName()` = source child name `name` (mirrored) | **state**: `srcPath.Base()->GetChildren()` filtered to `IsTableIndex()` and `Ready` | once per **existing, Ready** index child of `srcPath` (schemeshard state, not request); local index (`IsLocalIndex`) → `continue`, no impl-table fan-out |
| 3 | `CreateTable` (impl table copy) | `dstImplTable.Parent()` = `dstIndexPath` | `Name = srcImplTableName` (mirrored), `CopyFromTable = srcImplTable.PathString()` | **state**: `srcIndexPath.Base()->GetChildren()` | once per impl-table child of a non-local index (state); embeds `CreateSrcCdcStream`/`DropSrcCdcStream` looked up from `descr.IndexImplTableCdcStreams[key]` / `IndexImplTableDropCdcStreams[key]` where `key = IndexName + "/" + ImplTableName` (map-key attribution) |
| 4 | `CreateSequence` (`AddCopySequences`, `:387-405`) | `dstPath` (of table or impl table) | `Sequence = <copy of sequence proto>`, `CopySequence.CopyFrom = srcTable/sequenceName` | **state**: sequence children of `srcPath` / `srcImplTable` (`GetLocalSequences`, `:377-386`) | once per sequence child, for both the main table (after the index loop) and each index impl table |

Attribution rule for this op: `(CopyTableDescriptions[i], derived-part-kind)`.
Parts 1/1a/1b/1c map cleanly to `CopyTableDescriptions[i].*`. Parts 2-4 have
**no field in the request that names them** — the index/impl-table/sequence
*set* is entirely schemeshard state (children of `srcPath`); the only
request-derived piece is the map lookup key for CDC embedding. `ProtoRef` for
parts 2-4 should be reported as `Implicit(children of CopyTableDescriptions[i].SrcPath)`,
with map-derived CDC embedding cross-referenced to
`CopyTableDescriptions[i].IndexImplTableCdcStreams["<indexName>/<implTableName>"]`
where applicable.

---

## 3. `CreateNewCdcStream` / `CreateAlterCdcStream` / `CreateDropCdcStream` /
`CreateRotateCdcStream` (CDC family)

All four share the same two-part "Impl + AtTable" skeleton, and Create/Rotate
add a PQ-group part.

### 3a. `CreateNewCdcStream` (ESchemeOpCreateCdcStream)

`schemeshard__operation_create_cdc_stream.cpp:939-1032` builds, in this
order:

| # | Part OpType | WorkingDir | Name/path | Conditional |
|---|---|---|---|---|
| 1 | `CreateLock` (`DoCreateLock`, `:643-648`) | `tx.WorkingDir` | `LockConfig.Name = tablePath.LeafName()` (= `op.TableName`) | only if `initialScan` (`streamDesc.GetState() == ECdcStreamStateScan`) |
| 2 | `AlterTableIndex` | `workingDirPath.Parent()` | `AlterTableIndex.Name = workingDirPath.LeafName()`, `State = Ready` | only if `workingDirPath.IsTableIndex()` and stream name doesn't end `_continuousBackupImpl` |
| 3 | `CreateCdcStreamImpl` (`DoCreateStreamImpl`, `:1063-1075`) | `tablePath` = `WorkingDir + "/" + op.TableName` | `CreateCdcStream = op` (full `CopyFrom`) | always |
| 4 | `CreateCdcStreamAtTable` (`DoCreateStream`, `:1077-1092`) | `workingDirPath` = `tx.WorkingDir` | `CreateCdcStream = op` (full `CopyFrom`) | always |
| 5 | `CreatePersQueueGroup` (`DoCreatePqPart`, `:672-763`) | `streamPath` = `tablePath/op.StreamDescription.Name` | `Name = "streamImpl"` (**constant, not from request**); `TopicName = op.StreamDescription.Name`; partition boundaries from **table state** (`table->GetPartitions()`) or `op.TopicPartitions` | always |

`tablePath`/`streamPath`/`workingDirPath` are all computed once via
`DoNewStreamPathChecks` (`:895-916`) by resolving
`tx.WorkingDir / op.TableName / op.StreamDescription.Name` — purely
request-derived (no state needed for the *formula*, only for validation).

### 3b. `CreateAlterCdcStream` (ESchemeOpAlterCdcStream)

`schemeshard__operation_alter_cdc_stream.cpp:551-575`. Two parts, symmetric
to 3.3/3.4 above: `AlterCdcStreamImpl` at `tablePath`, `AlterCdcStreamAtTable`
at `workingDirPath`; both `CopyFrom(op)` the full `AlterCdcStream` payload
(plus `LockGuard.OwnerTxId = op.GetReady().LockTxId` if `HasGetReady()`).

### 3c. `CreateDropCdcStream` (ESchemeOpDropCdcStream)

`schemeshard__operation_drop_cdc_stream.cpp:557-739`. Supports **multiple
stream names in one request** (`op.StreamName` repeated field — note: newer
than the single-name field the plan doc assumed).

| # | Part OpType | WorkingDir | Name/path | Source | Conditional |
|---|---|---|---|---|---|
| 1 | `DropCdcStreamAtTable` | `workingDirPath` | `DropCdcStream = op` (full copy, all stream names preserved) | request | always |
| 2 | `DropLock` | `workingDirPath` | `LockConfig.Name = tablePath.LeafName()` | request (table name) | only if `lockTxId != Invalid` (derived from stream state: `stream->State == ECdcStreamStateScan`) |
| 3 | `AlterTableIndex` | `workingDirPath.Parent()` | `Name = workingDirPath.LeafName()`, `State = Ready` | request path shape | only if `workingDirPath.IsTableIndex()` and none of the dropped streams is `_continuousBackupImpl` |
| 4 | `DropCdcStreamImpl` (one per stream) | `tablePath` | `Drop.Name = op.StreamName[i]` | `op.StreamName[i]` — **repeated-index attribution** | once per entry in `op.StreamName` |
| 5 | `DropPersQueueGroup` (one per PQ child) | `streamPath` (= `tablePath/streamName`) | `Drop.Name = <child name>` (state: `streamPath.Base()->GetChildren()`, typically `"streamImpl"`) | **state**, not request | once per non-dropped PQ-group child of each stream in `streamPaths` |

### 3d. `CreateRotateCdcStream` (ESchemeOpRotateCdcStream)

`schemeshard__operation_rotate_cdc_stream.cpp:680-826`. Two parts, same
skeleton as Alter: `RotateCdcStreamImpl` at `tablePath`,
`RotateCdcStreamAtTable` at `workingDirPath`, both `CopyFrom(op)` the full
`RotateCdcStream` (which itself embeds `OldStreamName` + `NewStream`, a
nested `CreateCdcStream`).

### 3e. `CreateNewContinuousBackup` / `AlterContinuousBackup` / `DropContinuousBackup`

Thin wrappers that build a `CreateCdcStream`/`AlterCdcStream`/`DropCdcStream`
transaction internally (stream name suffixed `_continuousBackupImpl`) and
delegate to 3a-3c; `CreateAlterContinuousBackup` additionally returns the
created stream's `TPathId` by out-param (used by
`CreateBackupIncrementalBackupCollection`, §8). Same derived-part shape as
their CDC counterparts; the extra field is a fixed stream-name suffix, not
schemeshard state.

**Propose reads more than WorkingDir+Name**: `CreateCdcStreamAtTable`,
`AlterCdcStreamAtTable`, `DropCdcStreamAtTable`, `RotateCdcStreamAtTable` all
read the *entire* embedded `CreateCdcStream`/`DropCdcStream`/etc. proto
(table name, stream mode, retention, partitioning, `LockGuard`), not just
`WorkingDir`+one name field — these derived protos are **not** flattened to
"one path", they carry the whole original payload re-addressed.

---

## 4. `CreateConsistentAlterTable` (ESchemeOpAlterTable) — sequences, bloom
indexes, index-impl-table alters, alter-on-index-table

`schemeshard__operation_alter_table.cpp:1052-1133`, helpers
`AppendOwnedSequenceDrops` (`:830-876`), `AppendIndexImplTableMetricsAlters`
(`:885-926`), `DropLocalBloomIndexesOnFilterDisable` (`:953-976`),
`AddLocalBloomIndexes` (`:996-1048`).

Dispatch is a chain of early returns, not a fixed set — at most one branch
below fires per alter:

| Branch | Trigger | Derived parts |
|---|---|---|
| Plain alter | no name/pathId, or path not resolved/not a table | `{CreateAlterTable(tx)}` (single, original tx) |
| Bloom-filter drop | `PartitionConfig.EnableFilterByKey == false` and table has bloom-filter index children (**state**) | `AlterTable(tx)` + one `DropIndex`-cascade (`AddDropIndex`, see §6) per bloom index name (state: `CollectLocalBloomIndexNames`) + `AppendIndexImplTableMetricsAlters` |
| Bloom-filter add | `alter.TableIndexes` non-empty | optional base `AlterTable` (if non-index fields also changed) + one `CreateTableIndex` part per `alter.TableIndexes[i]`, `WorkingDir = path.PathString()` (the table), `Name` embedded in `ToIndexCreationConfig(indexDesc)` — **repeated-index attribution on `AlterTable.TableIndexes[i]`** |
| Common-sense path (plain table alter) | default case | `AlterTable(tx)` + `AppendOwnedSequenceDrops` + `AppendIndexImplTableMetricsAlters` |
| Index-impl-table alter | `path.Parent().IsTableIndex()` and caller passes admin/allowed-fields check | `AlterTableIndex` part (`WorkingDir = parent.Parent()`, i.e. the table; `Name = parent.LeafName()`, i.e. the index; `State = Ready`) + `AlterTable(tx)` (original tx, unmodified) |

Sub-details:

- **`AppendOwnedSequenceDrops`** (`:830-876`): for each `alter.DropColumns[i]`
  whose column's `DefaultKind == FromSequence`, resolves the backing
  sequence's leaf name from **table state**
  (`TTableInfo::Columns[...].DefaultValue`, possibly a full path — last
  segment taken), then emits `DropSequence{WorkingDir: tablePath,
  Drop.Name: seqLeaf}` **only if** that child actually resolves to a live
  sequence under the table (owned-child check). Request field
  `AlterTable.DropColumns[i].Name` selects *which* column, but the sequence
  name and its very existence are state-derived.
- **`AppendIndexImplTableMetricsAlters`** (`:885-926`): fires only if
  `alter.HasDetailedMetricsSettings()`. For every **live** index child of the
  table (state) and every **live** impl-table child of that index (state),
  emits `AlterTable{WorkingDir: indexPath, Name: implTableName,
  DetailedMetricsSettings: alter.DetailedMetricsSettings}` (copied verbatim
  from the request, but the *targets* are 100% state).
- **Bloom-filter add** (`AddLocalBloomIndexes`): `WorkingDir` for each
  generated `CreateTableIndex` part is `path.PathString()` (the altered
  table itself, request-derived); `Name` comes from
  `alter.TableIndexes[i].Name` embedded inside the copied
  `TIndexCreationConfig` rather than a separate scheme field — attribution
  still resolves to `AlterTable.TableIndexes[i]`.

---

## 5. Column-table-with-local-indexes family (`CreateColumnTableWithLocalIndexes`,
`AlterColumnTableWithLocalIndexes`, `DropColumnTableWithLocalIndexes`)

`ydb/core/tx/schemeshard/olap/operations/{create,alter,drop}_table_with_local_indexes.cpp`.
Dispatched from `ESchemeOpCreateColumnTable` (no `CopyFromTable`),
`ESchemeOpAlterColumnTable`, `ESchemeOpDropColumnTable`.

### 5a. Create (`create_table_with_local_indexes.cpp:7-66`)

1. `CreateNewColumnTable` — **original `tx`**, unmodified (part 0).
2. For each index in the (validated, normalized) inline schema
   (`createDescription.GetSchema().GetIndexes()[i]`, only if no
   `SchemaPresetName` and feature flag on): `CreateTableIndexLocal` part,
   `WorkingDir = workingDir + "/" + tableName`, index `Name` embedded inside
   the converted `TIndexCreationConfig` (`= indexProto.GetName()`) —
   repeated-index attribution on `CreateColumnTable.Schema.Indexes[i]`.

### 5b. Alter (`alter_table_with_local_indexes.cpp:9-300`)

Only fires when `AlterColumnTable.AlterSchema` has index changes
(`UpsertIndexes`/`DropIndexes`/`MoveIndex`) **and** the feature flag is on;
otherwise falls back to a single plain `AlterColumnTable(tx)`.

| Source field | Derived part(s) | WorkingDir | Name/path |
|---|---|---|---|
| `AlterSchema.UpsertIndexes[i]`, name **exists** (state: `existingIndexNames`) | `AlterTableIndexLocal` | `parentPathStr + "/" + tableName` | `AlterTableIndex = <converted config>` |
| `AlterSchema.UpsertIndexes[i]`, name new | `CreateTableIndexLocal` | same | `CreateTableIndex = <converted config>` |
| `AlterSchema.DropIndexes[i]` | `DropTableIndexLocal` | same | `Drop.Name = dropIdx` |
| `AlterSchema.MoveIndex[i]` (rename), `ReplaceDestination` and dest exists (state) | `DropTableIndexLocal` (drop dest first) | same | `Drop.Name = destinationName` |
| `AlterSchema.MoveIndex[i]` | `MoveColumnTableLocalIndex` (op type `ESchemeOpMoveIndex`) | same | `MoveIndex{TablePath: parentPathStr+"/"+tableName, SrcPath: sourceName, DstPath: destinationName}` — leaf names, not paths |

Base `AlterColumnTable(tx)` part is always pushed first (part 0), carrying
the original request (including any non-index fields).

### 5c. Drop (`drop_table_with_local_indexes.cpp:9-51`)

1. `DropColumnTable(tx)` — original tx.
2. For each **local**-index child of the table (state:
   `tablePath.Base()->GetChildren()` filtered by `IsTableIndex()` +
   `TTableIndexInfo::IsLocalIndex(Type)`): `DropTableIndexLocal` part,
   `WorkingDir = parentPathStr + "/" + tableName`, `Drop.Name = childName`.
   Entirely state-derived; nothing in the request names these children.

---

## 6. Index family (`CreateIndexedTable`, `CreateDropIndexedTable`,
`CreateBuildIndex`, `ApplyBuildIndex`, `CancelBuildIndex`, `CreateDropIndex`)

`ydb/core/tx/schemeshard/index/`.

### 6a. `CreateIndexedTable` (ESchemeOpCreateIndexedTable)

`operation_create_indexed_table.cpp:107-580`. Most complex *create*-side
fan-out in the tree. Before anything else,
**`MaybeProvisionFulltextRowId` (`:44-101`) can mutate the in-memory request**
— injecting a `__ydb_row_id` column + backing sequence + a
`__ydb_unique_row_id` unique index into `indexedTable` — none of which exist
in the client's original bytes. Any footprint entry for these injected
objects has **no `ProtoRef` into the client's request at all**; it must be
tagged `Synthesized(fulltext-rowid-provisioning)`.

Parts, in order:

| # | Part OpType | WorkingDir | Name/path | Source |
|---|---|---|---|---|
| 1 | `CreateTable` | `tx.WorkingDir` | `CreateTable = indexedTable.TableDescription` (copy) | `CreateIndexedTable.TableDescription` |
| 2..N | `CreateTableIndex` (one per `indexedTable.IndexDescription[i]`) | `tx.WorkingDir + "/" + baseTableDescription.Name` | `CreateTableIndex = indexDescription` (copy) | `CreateIndexedTable.IndexDescription[i]` — repeated-index attribution |
| impl tables | `CreateTable` (0-4 per index depending on index type: Global/Async/Unique → 1; VectorKmeansTree → 2-3 + a `CreateSequence`; JsonCompact/FulltextCompact(Relevance) → 1-3 + a `CreateSequence`; Json/FulltextPlain → 1; FulltextRelevance → 4) | `.../TableName/IndexName` (+ `/PrefixTable` etc. for the sequence) | `Name` fixed per position (`NTableIndex::NKMeans::LevelTablePosition` etc, constants), descriptions computed by `Calc*ImplTableDesc(...)` helpers from base table columns | derived from `IndexDescription[i]`'s type + base table description; **impl-table shape is a fixed function of index type, not itself named in the request** |
| N+1.. | `CreateSequence` (one per `indexedTable.SequenceDescription[i]`) | `tx.WorkingDir + "/" + baseTableDescription.Name` | `Sequence = sequenceDescription` (copy) | `CreateIndexedTable.SequenceDescription[i]` — repeated-index attribution |

Attribution: parts 1, 2..N, and the trailing sequence loop map cleanly to
request fields by repeated index. The impl-table parts do not — they are a
deterministic function of `IndexDescription[i].Type`, tag them
`Derived(IndexDescription[i], impl-table-<position>)`.

### 6b. `CreateDropIndexedTable` (ESchemeOpDropTable)

`operation_drop_indexed_table.cpp:393-450`. `dropOperation.HasId()` ?
`TPath::Init(TPathId(..., dropOperation.Id))` : resolve
`WorkingDir/Drop.Name` — the plan's `Drop.Id` vs `Drop.Name` ambiguity,
confirmed here verbatim. If the target is a column table, delegates whole to
`CreateDropColumnTable(tx)` (single part). Otherwise:

1. `DropTable(tx)` — **original tx**, unmodified (so `ProtoRef` is trivially
   the whole request; the `Id`-vs-`Name` resolution must still happen to know
   *which* path this refers to for the footprint's `AbsPath`).
2. `CascadeDropTableChildren(result, nextId, table)` —
   **fully state-derived**, see §6-shared below.

### 6c. `AddDropIndex` / `CascadeDropTableChildren` (shared cascade helper)

`schemeshard__operation_part.cpp:117-217` (`CascadeDropTableChildren`),
`index/operation_drop_index.cpp:552-577` (`AddDropIndex`). Used by 6b, by
`CreateDropIndex` (6e), and by `MoveIndex`'s destination-overwrite path
(§2/move family).

`CascadeDropTableChildren(table)`: walks `table.Base()->GetChildren()`
(state). For each child:

- sequence → `DropSequence{WorkingDir: table, Drop.Name: childName}`
- table index → `DropTableIndex{WorkingDir: table, Drop.Name: childName}`
- cdc stream → `DropCdcStreamImpl{WorkingDir: table, Drop.Name: childName}`
- then, for **that child's own children** (impl tables / PQ groups, state):
  table → `DropTable{WorkingDir: child, Drop.Name: implName}` + recurse
  `CascadeDropTableChildren(implPath)`; PQ group →
  `DropPersQueueGroup{WorkingDir: child, Drop.Name: implName}`

`AddDropIndex(indexPath)`: `DropTableIndex{WorkingDir: indexPath.Parent(),
Drop.Name: indexPath.LeafName()}` + for each impl-table child (state):
`DropTable{WorkingDir: indexPath, Drop.Name: childName}` +
`CascadeDropTableChildren(child)`.

**None of this is derivable from the request.** Every part here must be
`Implicit(children of <root path>)` in the footprint model; the only
request-anchored fact is the root table/index identity itself.

### 6d. `CreateBuildIndex` (ESchemeOpCreateIndexBuild)

`operation_create_build_index.cpp:55-270`. `op.IsRebuild` branches the first
part; impl-table fan-out mirrors 6a's index-type switch closely (same
`Calc*ImplTableDesc` helpers).

| # | Part OpType | WorkingDir | Name/path | Conditional |
|---|---|---|---|---|
| 1 (rebuild) | `AlterTableIndex` | `table.PathString()` | `Name = index.LeafName()` (= `op.Index.Name`), `State = WriteOnly`, `KeyColumnNames`/`DataColumnNames` from `op.Index` | `isRebuild` |
| 1 (new) | `CreateTableIndex` | `table.PathString()` | `CreateTableIndex = op.Index` (copy), `State = WriteOnly` | `!isRebuild` |
| 2 | `InitiateBuildIndexMainTable` | `table.Parent()` | `InitiateBuildIndexMainTable.TableName = table.LeafName()` | always |
| 3..N | `InitiateBuildIndexImplTable` (`CreateTable` variant, 0-4 depending on index type, same fan-out shape as 6a) | `index.PathString()` (= `table/op.Index.Name`) | impl-table descriptions computed the same way as 6a | skipped entirely when `isRebuild` (existing impl tables reused) |

`op.Index` (repeated-field-free here — one index per `CreateIndexBuild`
request) is the sole request anchor; impl-table shape is again a function of
`op.Index.Type`, not itself named.

### 6e. `ApplyBuildIndex` (ESchemeOpApplyIndexBuild) / `CancelBuildIndex`
(ESchemeOpCancelIndexBuild)

`operation_apply_build_index.cpp:89-285`. Both keyed off
`config.TablePath` (absolute) + `config.IndexName` (leaf, optional —
distinguishes plain build-column jobs from index builds).

`ApplyBuildIndex`:

1. `FinalizeBuildIndexMainTable` — `WorkingDir = table.Parent()`,
   `TableName = table.LeafName()`, `SnapshotTxId = config.SnapshotTxId`,
   `BuildIndexId = config.BuildIndexId`, plus (if `!indexName.empty()`)
   `Outcome.Apply.IndexPathId = index.PathId` (**state**: resolved path id).
2. (if index build) `AlterTableIndex` — `WorkingDir = table`,
   `Name = index.LeafName()`, `State = Ready`,
   `VectorIndexKmeansTreeDescription = config.VectorIndexKmeansTreeDescription`
   if present.
3. (if index build) for each child of `index` (**state**,
   `index.Base()->GetChildren()`): either `DropTable` (impl table — via
   `DropIndexImplTable` helper) if `NTableIndex::IsBuildImplTable(name)`, or
   another `FinalizeBuildIndexMainTable` (if the child has a live snapshot in
   `context.SS->TablesWithSnapshots`, **state**) with `TableName = <child
   name>`, `SnapshotTxId` from that state map, or
   `FinalizeBuildIndexImplTable` (`AlterTable` variant, via
   `FinalizeIndexImplTable`) otherwise, `Name = <child name>`.

`CancelBuildIndex` is the mirror: `FinalizeBuildIndexMainTable` (with
`Outcome.Cancel.IndexPathId`) + `DropTableIndex{WorkingDir: table,
Drop.Name: index.Name}` + one `DropIndexImplTable` per child of `index`
(state) — no finalize-with-snapshot branch here (everything is being torn
down).

Both: `config.TablePath`/`config.IndexName` are the only request fields; the
child fan-out is entirely `index.Base()->GetChildren()` state.

### 6f. `CreateDropIndex` (ESchemeOpDropIndex)

`operation_drop_index.cpp:397-576`. `dropOperation.TableName` (leaf, under
`WorkingDir`) + `dropOperation.IndexName` (leaf, under the table) are the
only request fields.

1. Either `AlterTable{WorkingDir: workingDirPath, Name: mainTablePath.Leaf,
   PartitionConfig.DropByKeyFilterPrefixLengths += droppedPrefixLen}` (if the
   index is a local bloom filter — **state**-determined branch, prefix length
   from index state) **or** `DropTableIndexAtMainTable{WorkingDir:
   workingDirPath, DropIndex.TableName/IndexName: mirrored leaf names}`
   (generic global index).
2. `AddDropIndex(indexPath)` cascade (§6c) — `DropTableIndex` + per-impl-table
   `DropTable` + recursive `CascadeDropTableChildren`.

There's also a request-independent **reject-only** guard: dropping a
`__ydb_unique_row_id`-shaped unique index is blocked if a Ready fulltext
index still depends on it and no other Ready row-id-unique index remains
(state check, no derived part).

---

## 7. `.backups/collections` family

`schemeshard__backup_collection_common.cpp` (shared helpers),
`schemeshard__operation_backup_backup_collection.cpp`,
`schemeshard__operation_backup_incremental_backup_collection.cpp`,
`schemeshard__operation_restore_backup_collection.cpp`,
`schemeshard__operation_drop_backup_collection.cpp`,
`schemeshard__operation_create_restore_incremental_backup.cpp`.

Shared path formula (`ResolveBackupCollectionPaths`, `:19-97`):
`<domain>/.backups/collections/<CollectionName>` — `CollectionName` may
itself be an absolute path under that directory; validated to actually live
there. This is the only piece truly derivable from the request
(`BackupBackupCollection.Name`, etc.); everything below it is state.

### 7a. `CreateBackupBackupCollection` (ESchemeOpBackupBackupCollection)

`schemeshard__operation_backup_backup_collection.cpp:33-168`. Builds one
big `CreateConsistentCopyTables` transaction (delegated to §2, *not* pushed
as a separate outer part — this function *is* effectively
`CreateConsistentCopyTables`'s caller, constructing its
`CopyTableDescriptions[]` from **state**):

- For each entry in `bc->Description.GetExplicitEntryList().Entries`
  (**state** — the collection's stored, not requested, member list):
  `SrcPath = entry.Path` (existing live table), `DstPath =
  <domain>/.backups/collections/<Name>/<TargetDir>/<relativeItemPath>` where
  `TargetDir` is set by `NOperation::Rewrite<TTag>` (`:26-29`) to
  `NBackup::FullBackupDirName(now)` — a **timestamp generated during
  Propose**, not present in the client's request at all (`NeedRewrite = true`
  trait, `schemeshard__op_traits.h:157-162`).
  If the entry is a table with **live index children whose impl tables are
  not omitted** (state, `bc->Description.OmitIndexes` false), sets
  `IndexImplTableCdcStreams`/entries similarly derived — actual CDC stream
  creation embedding happens inside `CreateConsistentCopyTables` itself
  (§2, row 3).
- `GetRequiredPaths<TTag>` (`:16-22`, feeds `SplitIntoTransactions`'s
  `CreateAdditionalDirs`, §1b) computes the mkdir set from the same entry
  list plus, if `bc->Description.HasIncrementalBackupConfig()` and indexes
  aren't omitted, extra `__ydb_backup_meta/indexes/<relPath>/<indexName>`
  directories per index child (state).

`ProtoRef` for every generated `CopyTableDescriptions[i]` here is
`Implicit(BackupCollections[bcPathId].Description.ExplicitEntryList.Entries[i])`
— **not** derivable from the client's `BackupBackupCollection` request at
all beyond the collection name.

### 7b. `CreateBackupIncrementalBackupCollection`
(ESchemeOpBackupIncrementalBackupCollection)

`schemeshard__operation_backup_incremental_backup_collection.cpp:155-297`.
For each entry in `bc->Description.ExplicitEntryList.Entries` (state):
`AlterContinuousBackup{WorkingDir: tx.WorkingDir,
AlterContinuousBackup.TableName: <relative path>,
TakeIncrementalBackup.DstPath: Name/TargetDir/relPath}` (delegates to §3e).
Additionally, unless `IncrementalBackupConfig.OmitIndexes`, for each live
index child + impl-table child of each entry's table (state, two nested
loops), another `AlterContinuousBackup` targeting
`<relPath>/<indexName>/<implTableName>` with a
`__ydb_backup_meta/indexes/...` destination. Finishes with
`CreateLongIncrementalBackupOp` (control-plane part,
`WorkingDir = bcPath`, carries the collected stream `TPathId`s from all the
`AlterContinuousBackup` calls — state, not request).

### 7c. `CreateRestoreBackupCollection` (ESchemeOpRestoreBackupCollection)

`schemeshard__operation_restore_backup_collection.cpp:332-441`. Picks
`lastFullBackupName` = lexicographically-last child ending
`FullBackupSuffix`, and `incrBackupNames` = children ending
`IncrementalBackupSuffix` **after** it (state: `bcPath.Base()->GetChildren()`
is a sorted map, relied on via `static_assert`). Builds a synthetic
`CreateConsistentCopyTables` tx: for each `bc->Description.ExplicitEntryList`
entry (state), `SrcPath = <collection>/<lastFullBackupName>/<relItemPath>`,
`DstPath = entry.Path` (restore back to the live location), delegates to §2.
If there are incremental backups, additionally emits one `ChangePathState`
part per `(incrBackupName, entry)` pair (state × state) targeting
`AwaitingOutgoingIncrementalRestore`, and finally one
`CreateLongIncrementalRestoreOp` control-plane part
(`WorkingDir = bcPath`). Everything here traces to
`RestoreBackupCollection.Name` (which collection) but the entry/backup set is
100% state.

### 7d. `CreateDropBackupCollectionCascade` (ESchemeOpDropBackupCollection)

`schemeshard__operation_drop_backup_collection.cpp:537-628`. Part 0:
`TDropBackupCollection` sub-op with the **original tx**. Then
`CollectExternalObjects(context, dstPath)` (state — walks the collection
subtree) yields `CdcStreamsByTable` (grouped) and `BackupTables`; for each,
builds a `DropCdcStream` (via `CreateDropCdcStream`, §3c) or `DropTable`
transaction and appends its parts. **Fully `Implicit`** past the root
collection identity.

### 7e. `CreateRestoreMultipleIncrementalBackups`
(ESchemeOpRestoreMultipleIncrementalBackups)

`schemeshard__operation_create_restore_incremental_backup.cpp:17-38`. **Now
always rejects** — "schema-op dispatch has been retired; the incremental
restore orchestrator now uses the request/response channel
(`TEvIncrementalRestoreSrcCreateRequest`) instead". No derived parts to model;
flag as dead/no-op in the footprint design (still present in the
`MakeOperationParts` switch, still reachable, always a single `Reject`).

---

## 8. Move family (recap incorporating request/direct-client distinction)

Already partly covered under earlier headings' "Propose reads more than
WorkingDir+Name" callouts; consolidated here since the plan flagged it as an
offender.

### 8a. `CreateConsistentMoveTable` (ESchemeOpMoveTable)

`schemeshard__operation_move_tables.cpp:14-133`. `MoveTable.SrcPath`/`DstPath`
absolute (request). Rejects outright if the source table has any cdc-stream
children (state check, no cascade support). Parts:

1. `MoveTable{WorkingDir: dstPath.Parent(), MoveTable.SrcPath/DstPath:
   srcPath/dstPath}` (`MoveTableTask`, `schemeshard__operation_common.cpp:1364`)
   — request-derived.
2. For each non-sequence, non-deleted child of `srcPath` (**state**): if a
   local index → `MoveColumnTableLocalIndex` (column table) or
   `MoveTableIndex` (`MoveTableIndexTask`, absolute Src/Dst) targeting
   `dstPath.Child(name)` where `name` is the **state**-derived child name,
   mirrored 1:1 to the destination.
3. For each impl-table child of that index child (**state**): another
   `MoveTable` part (impl table mirror) + `AddMoveSequences` for that impl
   table's own sequence children (state).
4. `AddMoveSequences(srcPath, dstPath)` for the main table's own sequence
   children (state) — `MoveSequence{WorkingDir: dstPath,
   SrcPath: srcTable/seqName, DstPath: dstPath/seqName}`.

Attribution: part 1 → `MoveTable.SrcPath`/`DstPath` directly. Parts 2-4 are
`Implicit(children of MoveTable.SrcPath)`, mirrored by construction (dest
child name == src child name), so `AbsPath` is computable but `ProtoRef`
points at the parent request field, not a specific child field.

### 8b. `CreateConsistentMoveIndex` (row tables) / `CreateConsistentMoveLocalIndex`
(column tables) — ESchemeOpMoveIndex

`index/operation_move_index.cpp:462-591` (row),
`olap/operations/move_local_index.cpp:400-476` (column). Both dispatched
from the same request shape:
`MoveIndex.TablePath` (absolute, parent table), `MoveIndex.SrcPath`/`DstPath`
(**leaf names relative to `TablePath`**, confirmed exactly as the plan
states), `AllowOverwrite` (row-table variant only).

Row-table (`CreateConsistentMoveIndex`):

1. `TUpdateMainTableOnIndexMove` (custom `AlterTable`-flavored sub-op) —
   `WorkingDir = mainTablePath.Parent()`, `Name = mainTablePath.LeafName()`
   — re-derivation of `MoveIndex.TablePath`, decomposed.
2. (conditional, only if `dstIndexPath` already resolved and
   `AllowOverwrite`) `AddDropIndex(dstIndexPath)` cascade (§6c) — **state**,
   drops whatever currently occupies the destination name.
3. `MoveTableIndex{WorkingDir: mainTablePath, SrcPath: srcIndexPath
   (absolute), DstPath: dstIndexPath (absolute)}` (`MoveTableIndexTask`) —
   both absolute paths reconstructed by joining `TablePath` (request) with
   `SrcPath`/`DstPath` (request, leaf).
4. For each impl-table child of `srcIndexPath` (**state**): `MoveTable` part
   (impl table mirror) + `AddMoveSequences` (state) for that impl table's
   sequences.

Column-table variant is much thinner: part 1 = `AlterColumnTable` with an
**empty** `AlterSchema` at `WorkingDir = mainTablePath.Parent()`, `Name =
mainTablePath.LeafName()` (puts the table "under operation" without
otherwise changing it); part 2 = `MoveColumnTableLocalIndex(tx)` — **the
original tx, unmodified** (no impl-table cascade — local indexes have none).

### 8c. `CreateMoveTableIndex` (ESchemeOpMoveTableIndex, direct client op)

Dispatch switch: `{CreateMoveTableIndex(op.NextPartId(), tx)}` — **always a
single part**, original tx unmodified. Only becomes a *derived* part
(constructed via `MoveTableIndexTask`, absolute `Src`/`Dst`) when produced
internally by 8a/8b's cascades; as a directly-issued client op it is not
compound and needs no special attribution handling.

---

## Summary table

| OriginalOpType | Derived parts (typical count) | Derivable from request alone? | Attribution rule |
|---|---|---|---|
| Any `Create*` with multi-segment name (§1a) | 1 + (missing-dir count) `MkDir`s | Yes (path decomposition; state only picks count) | Prefix decomposition of `WorkingDir + <name field>` |
| `BackupBackupCollection`/`BackupIncrementalBackupCollection`/`RestoreBackupCollection` auto-mkdir (§1b) | N `MkDir`s | No — needs collection's stored entry list | `Implicit(BackupCollections[id].ExplicitEntryList)` |
| `CreateConsistentCopyTables` (§2) | 1 + ~3-6 per copy item (indexes/impl tables/sequences) | Root copy pair yes; index/impl/sequence fan-out no | `CopyTableDescriptions[i]` for row 1; `Implicit(children of SrcPath[i])` for the rest; map key `IndexName/ImplTableName` for CDC embedding |
| `CreateCdcStream` (§3a) | 2-5 (Lock?, AlterTableIndex?, Impl, AtTable, PQ) | Mostly yes (path join); PQ boundaries/const name partly state | Impl/AtTable → whole op copy; PQ → `op.StreamDescription.Name` + state (table partitions) |
| `AlterCdcStream` / `RotateCdcStream` (§3b/3d) | 2 | Yes | Whole op copy, re-homed |
| `DropCdcStream` (§3c) | 3 + 1/stream + 1/PQ-child | Stream-name parts yes (repeated index); PQ-drop no | `op.StreamName[i]` for impl drops; `Implicit(children of streamPath)` for PQ drops |
| `ContinuousBackup` Create/Alter/Drop (§3e) | same as CDC + fixed suffix | Yes | Same as CDC, name suffix `_continuousBackupImpl` is constant |
| `AlterTable` (sequences/bloom/index-impl, §4) | 0-many, branch-dependent | Sequence drops: no (owned-child state); bloom add: yes (repeated field); metrics-alert fan-out: no | `AlterTable.DropColumns[i]` (name only) / `AlterTable.TableIndexes[i]` / `Implicit(index+impl children)` |
| `CreateColumnTable` w/ inline indexes (§5a) | 1 + N | Yes | `CreateColumnTable.Schema.Indexes[i]` |
| `AlterColumnTable` w/ index ops (§5b) | 0-many | Yes | `AlterSchema.UpsertIndexes[i]` / `DropIndexes[i]` / `MoveIndex[i]` |
| `DropColumnTable` (§5c) | 1 + N | No | `Implicit(local-index children)` |
| `CreateIndexedTable` (§6a) | 1 (table) + N (indexes) + M (impl tables) + K (sequences), M/K a function of index type | Table/indexes/sequences yes (repeated fields); impl tables no; fulltext-rowid provisioning: **synthesized, not in request at all** | `IndexDescription[i]`/`SequenceDescription[i]`; impl tables `Derived(IndexDescription[i], position)`; provisioned rowid objects `Synthesized` |
| `DropTable` → `CreateDropIndexedTable` (§6b) | 1 + cascade | Root part yes (`Drop.Id`/`Drop.Name`); cascade no | `Implicit(children of table)` |
| `CreateIndexBuild` (§6d) | 2 + M impl tables (0 if rebuild) | Index desc yes; impl tables no | `op.Index` (single, not repeated) |
| `ApplyIndexBuild`/`CancelIndexBuild` (§6e) | 2 + K (children of index) | Root yes (`TablePath`/`IndexName`); children no | `Implicit(children of index)` |
| `DropIndex` (§6f) | 2 + cascade | Root yes (`TableName`/`IndexName`); cascade no | `Implicit(children of index)` |
| `BackupBackupCollection` (§7a) | 1 CreateConsistentCopyTables tx built entirely from state | No (beyond collection name) | `Implicit(BackupCollections[id].ExplicitEntryList[i])` |
| `BackupIncrementalBackupCollection` (§7b) | N `AlterContinuousBackup` + 1 control part | No | `Implicit(...)`, same as above |
| `RestoreBackupCollection` (§7c) | 1 CreateConsistentCopyTables + N ChangePathState + 1 control part | No (beyond collection name) | `Implicit(...)` |
| `DropBackupCollection` (§7d) | 1 + N (cdc/table drops) | No | `Implicit(CollectExternalObjects(dstPath))` |
| `RestoreMultipleIncrementalBackups` (§7e) | 0 (always rejects) | N/A | dead code path |
| `MoveTable` (§8a) | 1 + cascade | Root yes; cascade no (mirrored names) | `MoveTable.SrcPath`/`DstPath`; `Implicit(children)` |
| `MoveIndex` (§8b) | 2-3 + cascade | Root yes (`TablePath`+leaf `SrcPath`/`DstPath`); cascade no | `MoveIndex.TablePath/SrcPath/DstPath`; `Implicit(children of index)` |
| `MoveTableIndex` (§8c, direct) | 1 (not compound as a client op) | Yes | Whole request |

---

## Simplest attribution strategy for H2

Given the shapes above, a viable `(originalTransactionIndex, partIndex,
partOpType, part proto) -> field(s) of the original request` rule is a
**three-tier lookup**, evaluated in this order:

1. **Byte-identical part.** If `part->GetTransaction()` is the exact original
   `tx` (a few ops push it unmodified — `DropTable` in
   `CreateDropIndexedTable`, `MoveColumnTableLocalIndex` in
   `CreateConsistentMoveLocalIndex`, the base `AlterTable`/`AlterColumnTable`
   in §4/§5b): attribute the whole part to the whole original request.
   Detect via pointer/structural equality against the transaction
   `TOperation::SplitIntoTransactions` recorded per `originalTransactionIndex`
   — cheap and exact.

2. **Static field table** (Layer 1a from the plan, `THashMap<EOperationType,
   TVector<TFieldSpec>>`), extended with a **derived-op variant**: for each
   op type that can appear as a *derived* `OperationType` (e.g.
   `CreateCdcStreamAtTable`, `MoveTableIndex`, `FinalizeBuildIndexMainTable`,
   `AlterTableIndex`, `DropTableIndex`, `CreateSequence` when emitted as a
   cascade part, `AlterContinuousBackup`), record which *originating*
   `EOperationType`(s) can produce it and a **field-path template** relative
   to the *originating* request, parameterized by:
   - a repeated index, when the derived part corresponds 1:1 to a repeated
     field entry of the original request (CDC's none — always whole-copy;
     `CreateIndexedTable.IndexDescription[i]`;
     `CreateConsistentCopyTables.CopyTableDescriptions[i]`;
     `AlterTable.TableIndexes[i]`; `DropCdcStream.StreamName[i]`); or
   - a literal `Implicit(<anchor>)` marker, when the part's target set comes
     from `TPathElement::GetChildren()` of a resolved anchor path (itself
     attributable via the table above), with no further request field to
     point at. This is the majority case for cascading drops/moves/build-index
     impl tables — the plan's finding 3 ("indexed by anchor, not by field")
     generalizes to essentially every non-CDC, non-CreateIndexedTable/
     CreateConsistentCopyTables compound op.

3. **Runtime-derived fallback** (`Synthesized`/state-sourced, no `ProtoRef`
   possible): reserved for `MaybeProvisionFulltextRowId`'s injected
   column/sequence/index, the `.backups/collections` family's entire
   `CopyTableDescriptions[]`/`AlterContinuousBackup` fan-out (sourced from
   the *stored* backup-collection entry list, not the triggering request),
   and `BackupBackupCollection`'s `Rewrite`-generated `TargetDir` timestamp.
   These need the post-Propose `TTxState`/`TMemoryChanges` layer (plan
   finding 4) as their only source of truth; the static extractor should
   emit `Implicit(<schemeshard-state-anchor>)` and rely on layer 3 to fill
   in `AbsPath` after Propose.

Practically: **tier 1 and tier 2's repeated-index cases cover every op the
plan's §1a table calls "level 1" (request-named paths + generated parts)**.
Tier 2's `Implicit(anchor)` cases and tier 3 are exactly the "level 2, full
cascaded subtree" gap the plan already scoped as a measured gap report
(§3, open question 1) rather than a product guarantee — this survey confirms
that scoping is correct: cascade fan-out (indexes, impl tables, cdc streams,
PQ groups, sequences) is *never* named in the triggering request for any op
in this tree; it is uniformly `TPathElement::GetChildren()` state. The one
partial exception is `CreateConsistentCopyTables`'s CDC-stream embedding via
`IndexImplTableCdcStreams["IndexName/ImplTableName"]`, which *is* a request
map lookup once the impl-table's existence is already known from state — so
even there, the map key itself needs the state-derived impl-table name
before it becomes a valid `ProtoRef`.
