# S4 — Prototype: SchemeShard path footprint (layer 1a + layer 2 + H2 hook)

Branch: `feat/schemeshard-path-footprint` (off `main` @ `66947081430`), **uncommitted**.
Status: builds clean, 21/21 new tests green, `ut_base` + `ut_auditsettings` smoke green.

**Frozen state measured (2026-09-02T23:56, tree final, no further edits):**

| File | md5 |
|---|---|
| `schemeshard_path_footprint.cpp` | `8c5f4cbebf713da82d3729f5a001c343` |
| `schemeshard_path_footprint.h` | `dfe5b9689ac60cca2fcdf231d1c08cae` |
| `ut_path_footprint/ut_path_footprint.cpp` | `88c218d15f4d199608e3cd843eb7cc0e` |
| `ut_path_footprint/ya.make` | `8de4d10331ff9b3ada47553b5326b44c` |
| `schemeshard__operation.cpp` | `43d71b9bad049836653d7a436efc6a80` |
| `schemeshard__operation.h` | `d0095b46cd08ea4d5f70b2f909d6798f` |
| `schemeshard/ya.make` | `a881d3541fece66bc605cc34e0a8186c` |

> **Erratum — a broken intermediate state was visible to S5.** This document was
> first written against a green tree. It was then edited a second time to add
> anchored `Implicit` entries (S2 tier 2), and that edit introduced a transient
> compile error: `const int moveSrcIndex = out.Last();` was declared directly
> under an unbraced `case ESchemeOpMoveTable:`, which makes every later case
> label in the switch a "cannot jump from switch statement to this case label"
> error (20 errors, root at `schemeshard_path_footprint.cpp:509`). S5 built
> during that window and hit it. The case is now braced; last code write
> 23:50:49, `ut_path_footprint` 21/21 at 23:52:08 and again at 23:56:42,
> `ut_base` 3/3 at 23:53:38, `ut_auditsettings` 5/5 at 23:53:57 — all after the
> fix, and the `.cpp` md5 was byte-identical before and after the final run.
> The process failure is real and worth recording: I resumed editing a tree I
> had already declared final without announcing it. Any future edit round on a
> published prototype needs to re-announce before, not after.

---

## 1. What was built

### New files (nothing else in the tree defines them)

| File | Lines | Contents |
|---|---|---|
| `ydb/core/tx/schemeshard/schemeshard_path_footprint.h` | 94 | `EPathRefKind`, `EPathRefRole`, `TPathRef`, `TPathFootprintEntry`, `TPathFootprint`, `ExtractPathRefs`, `ResolvePathFootprint`, `FormatPathFootprintLine`, `PathRefKindName`/`PathRefRoleName` |
| `ydb/core/tx/schemeshard/schemeshard_path_footprint.cpp` | 812 | layer 1 (136 `case` labels, no `default:`), layer 2 (`TPath`-only normalizer), the log-line formatter |
| `ydb/core/tx/schemeshard/ut_path_footprint/ut_path_footprint.cpp` | 661 | 14 pure extraction tests + 7 propose-level integration tests |
| `ydb/core/tx/schemeshard/ut_path_footprint/ya.make` | 23 | `UNITTEST_FOR(ydb/core/tx/schemeshard)`, `SIZE(MEDIUM)` |

### Layer 1 — `ExtractPathRefs(const TModifyScheme&) -> TVector<TPathRef>`

Pure, no `TSchemeShard*`. A single `switch` over `EOperationType` with **136 case
labels and no `default:`** — a new enum value is a `-Wswitch` **compile error**,
which is a stronger completeness guarantee than the `Y_ABORT` in
`ExtractChangingPaths`. The 136 cases are served by six shared sink helpers
(`Leaf`, `Path`, `Abs`, `Sibling`, `ById`, `Implicit`) plus two shape lambdas
(`genericDrop` for the ~22-op `TDrop` family, `createCdcStream`), i.e. the S1
"9 shapes" collapse into ~8 code paths.

`TPathRef` carries `FieldPath` (`"CreateConsistentCopyTables.CopyTableDescriptions[1].IndexImplTableCdcStreams[idx/indexImplTable].StreamDescription.Name"`),
`Value`, `Kind`, `Role`, `BasePath` (for `LeafUnderSibling`),
`OwnerId`/`LocalPathId` (for `ById`), and `AnchorIndex` (for `Implicit`).

**`Implicit` entries are anchored** (S2's tier-2 `Implicit(<anchor>)` rule).
`AnchorIndex` is the index, within the same result vector, of the ref whose
resolved path is the anchor of the runtime-derived set — i.e. the path whose
`TPathElement::GetChildren()` the operation will actually walk. Layer 2 copies
the anchor's `AbsPath`/`PathId` onto the `Implicit` entry (leaving
`Exists = false`, since the entry stands for a *set*, not one path), so a
consumer can enumerate the cascade from a real `TPathId` instead of getting a
bare string marker. Anchors are the dropped/created path for the drop and
create families, and deliberately the **source** for `MoveTable`/`MoveIndex`
(the children exist under the source, not the destination) and for each
`CopyTableDescriptions[i]`'s `SrcPath`. `IncrementalRestoreFinalize` is the one
op with `AnchorIndex == -1`: it has no anchor in the request at all.

**Deviation from the task spec, deliberate:** the spec asked for a single
`ui64 LocalPathId`. `AlterTable.PathId` and `AlterReplication.PathId` are
`NKikimrProto.TPathID` (owner + local), and their `Propose()` uses
`TPathId::FromProto`, not `MakeLocalId`. So `TPathRef` has both `OwnerId` and
`LocalPathId`; `OwnerId == 0` means "local id, resolve via `ss->MakeLocalId()`".
Without this the id branch of `AlterTable`/`AlterReplication`/
`SplitMergeTablePartitions` would be silently wrong on a foreign owner.

The extractor fixes most of the S1-confirmed audit bugs rather than reproducing
them (but see **Confirmed defects** below — it reproduced one and introduced
four of its own): `SplitMergeTablePartitions.TablePath` is `Absolute` (not WorkingDir-joined),
ResourcePool / StreamingQuery / TruncateTable are WorkingDir-relative,
`AlterSequence`, `AlterReplication`/`AlterTransfer`, `AlterExternalTable`,
`AlterExternalDataSource` emit their (previously missing) path, and every
`TDrop`/`Alter*` id branch is emitted as `ById`.

### Layer 2 — `ResolvePathFootprint(tx, ss) -> TPathFootprint`

Uses only `TPath`: `Resolve`, `Child`, `Init`, `PathString`, `Parent`,
`LeafName`, `FirstExistedParent`, `GetPathIdForDomain`, `GetDomainPathString`,
`IsResolved`, `IsDeleted`. Resolution mirrors `Propose()`:

| Kind | Resolution |
|---|---|
| `LeafUnderWorkingDir` | `Resolve(WorkingDir).Child(name)` (mirrors `.Dive(name)`, so a multi-segment name stays a single unresolvable leaf, exactly as in `Propose()`) |
| `PathUnderWorkingDir` / `Absolute` | value starting with `/` → `Resolve(value)`, else `Resolve(JoinPath({WorkingDir, value}))` |
| `LeafUnderSibling` | same rule applied to `BasePath`, then `.Child(value)` |
| `ById` | `TPath::Init(OwnerId ? TPathId(OwnerId, LocalPathId) : ss->MakeLocalId(LocalPathId))` |
| `Implicit` | not resolved; entry recorded with empty `AbsPath` as an explicit gap marker |

Never aborts. Every `TPath` accessor that has a `Y_ABORT_UNLESS` precondition
(`LeafName`, `GetPathIdForDomain`, `Base`) is guarded by `IsEmpty()`/
`IsResolved()`. For an unresolved path, `AbsPath` still comes out right
(`PathString()` canonizes `NameParts`, which exist even without `Elements`), and
`ParentPathId`/`DatabasePathId`/`RelPathToDatabase` come from
`FirstExistedParent()`.

### H2 hook — the only production edits

`git diff --stat main`:

```
 ydb/core/tx/schemeshard/schemeshard__operation.cpp | 17 +++++++++++++++++
 ydb/core/tx/schemeshard/schemeshard__operation.h   |  5 +++++
 ydb/core/tx/schemeshard/ya.make                    |  1 +
 3 files changed, 23 insertions(+)
```

23 added lines, 0 deleted. Exactly:

- `schemeshard__operation.h:4` — `#include "schemeshard_path_footprint.h"`.
- `schemeshard__operation.h:18-21` — `TVector<TPathFootprint> PathFootprints;` on
  `TOperation` (in-memory, same lifetime as `TOperation`, nothing persisted).
- `schemeshard__operation.cpp:114-117` — inside `ProcessOperationParts`, first
  statement of the `for (auto& part : parts)` body:
  `auto footprint = ResolvePathFootprint(part->GetTransaction(), context.SS);`
- `schemeshard__operation.cpp:129-140` — right after `Y_ABORT_UNLESS(response)`
  (so before the accepted/rejected classification and before
  `AbortOperationPropose`): stamp `ProposeStatus` and `PartId`, emit one
  `LOG_NOTICE_S` per entry (one `fieldPath# <none>` line when the op has no
  path refs at all), push into `operation->PathFootprints`.
- `ya.make:248` — `schemeshard_path_footprint.cpp` next to
  `schemeshard_audit_log_fragment.cpp`.

No `Propose()` implementation, no `TPath` call site, no `.proto` was touched.
The `TxCancelTx` fake propose (`TTxOperationProposeCancelTx::Execute`) calls
`part->Propose` directly and does not go through `ProcessOperationParts`, so it
is not instrumented, as required.

`ExtractChangingPaths` was **not** rewired (see §5).

### Confirmed defects (found by S5's coverage run, verified by me in the source)

Seven bugs, four of them one bug. I verified every one against `Propose()`
before accepting the report; all are real. **Not fixed — the tree is frozen at
the md5s above, which S5's report cites.**

**D1–D4: the four `*CdcStreamAtTable` parts drop the stream leaf.** I emit only
the parent table, but every `AtTable` `Propose()` resolves
`tablePath.Child(streamName)`, and the stream name is in the submessage the part
already carries. The `AtTable` half is not "the table half" of the operation —
it touches both.

| Part op type | `Propose()` resolves | Field the part carries | My output |
|---|---|---|---|
| `CreateCdcStreamAtTable` | `tablePath.Child(streamName)`, sets `txState.CdcPathId` (`..._create_cdc_stream.cpp:594`) | `CreateCdcStream.StreamDescription.Name` | table only |
| `AlterCdcStreamAtTable` | `tablePath.Child(streamName)` (`..._alter_cdc_stream.cpp:100`) | `AlterCdcStream.StreamName` | table only |
| `DropCdcStreamAtTable` | `tablePath.Child(streamName)` **per stream** (`..._drop_cdc_stream.cpp:101`) | `DropCdcStream.StreamName[]` (repeated) | table only |
| `RotateCdcStreamAtTable` | `Child(oldStreamName)` and `Child(newStreamName)` (`..._rotate_cdc_stream.cpp:105,124`) | `OldStreamName` + `NewStream.StreamDescription.Name` | table only |

**D5: `CreateColumnTable` reads the wrong submessage.** I emit
`AlterColumnTable.Name`, inherited from `ExtractChangingPaths` and recorded in
S1 as a "verified intentional" quirk. It is not. `CreateColumnTableWithLocalIndexes`
pushes the **unmodified client tx** (`create_table_with_local_indexes.cpp:52`,
`CreateNewColumnTable(NextPartId(nextId, result), tx)`), and
`TCreateColumnTable::Propose` reads `Transaction.GetCreateColumnTable()`
(`olap/operations/create_table.cpp:566`; the traits getter at `:882` is
`tx.GetCreateColumnTable().GetName()`). So the part proto carries
`CreateColumnTable.Name`, my read returns empty, and the entry collapses to the
working directory. This is the one audit bug the prototype **reproduced** while
claiming to fix that class — my §1 wording was wrong and is corrected above.

`EveryOperationTypeIsCovered` cannot catch D5, as S5 notes: an empty name still
yields exactly one ref, so the "non-empty result" assertion passes. The test
asserts the *shape* of the output, never that the value came from a field that
is actually set. A stronger completeness test would populate one distinctive
value per op type and assert it appears in some entry.

**D6 (mine, not in S5's list): `AlterColumnTable` is missing its fallback.**
`olap/operations/alter_table.cpp:278` reads
`HasAlterColumnTable() ? AlterColumnTable.Name : AlterTable.Name`. I emit only
the first arm, so an alter submitted through the `AlterTable` submessage
reports an empty leaf.

**D7 (mine): `Alter`/`RotateCdcStreamAtTable` split a multi-segment table name.**
Both resolve `Child(tableName, TPath::TSplitChildTag{})`, i.e. a `/`-containing
`TableName` is split into segments. My `LeafUnderWorkingDir` kind mirrors plain
`.Child(name)` and keeps it as one unresolvable leaf. Same `AbsPath` string,
different `Exists`/`PathId`.

Fix sketch, ~12 lines in the extractor, no hook or layer-2 change: give the four
`AtTable` cases the same parent-plus-sibling treatment their non-`AtTable`
counterparts already get (with the repeated loop for `Drop`, and two refs for
`Rotate`); read `CreateColumnTable.Name` for `ESchemeOpCreateColumnTable`; add
the `AlterTable.Name` fallback for `ESchemeOpAlterColumnTable`. D7 needs a new
kind (`PathUnderWorkingDirSplit`) or is accepted as a documented divergence.

---

## 2. Observation channel

**Chosen: one `LOG_NOTICE_S` line per footprint entry, prefix `PathFootprint`,
component `FLAT_TX_SCHEMESHARD`.** Format:

```
PathFootprint txId# N, partId# M, partOpType# ESchemeOpCreateTable,
proposeStatus# StatusAccepted, workingDir# /MyRoot/a/b, workingDirRelToDb# a/b,
fieldPath# CreateTable.Name, kind# LeafUnderWorkingDir, role# Target,
absPath# /MyRoot/a/b/Table, pathId# [OwnerId: 0, LocalPathId: 0], exists# 0,
relToParent# Table, relToDb# a/b/Table, relToWorkingDir# Table
```

Why this and not a test seam:

- The in-memory `TOperation::PathFootprints` **does not survive a rejected
  propose**: `AbortOperationPropose` erases `Operations[txId]`, and
  `IgniteOperation` returns only the response. A rejected part's footprint is
  therefore only observable through the log, and rejected parts are exactly one
  of the required cases.
- `TTestEnv` already sets `FLAT_TX_SCHEMESHARD` to `PRI_NOTICE`, so no test
  needs to raise the log level; and no extra production seam (callback,
  friend class, `#ifdef`) is added just for tests.
- It is also the channel a schema-CDC consumer can read today without
  SchemeShard knowing about CDC; a real observer callback can replace it later
  without touching layers 1 and 2.

Test-side capture: `runtime.SetLogBackend(new TLogRecordCollector(&log))` before
`TTestEnv env(runtime)`, where `TLogRecordCollector` is a 10-line `TLogBackend`
that appends each record to a `TVector<TString>`.

**Gotcha worth recording:** `TStreamLogBackend` (the pattern used in
`datashard_ut_write.cpp`) writes records **without a trailing newline**, so the
whole log arrives as one concatenated blob and line-based parsing silently
yields zero matches. A record-collecting backend is required.

---

## 3. Test results

### `ydb/core/tx/schemeshard/ut_path_footprint` — 21/21 OK

```
{"type": "summary", "exit_code": 0, "tests": {"OK": 21}}
```

`TSchemeShardPathFootprintExtract` (pure, no actor runtime), one per S1 shape:
`MkDirAndCreateTable`, `DropTableByNameAndById`, `AlterTableById`,
`ConsistentCopyTables` (2 items + `CreateSrcCdcStream` +
`IndexImplTableCdcStreams`, asserting the exact `FieldPath` strings including
`[2]`-style indices and `[mapKey]`), `MoveIndexIsLeafUnderSibling`,
`MoveTableIsAbsolute`, `ApplyIndexBuild`, `CreateCdcStream`,
`CreateIndexedTable`, `AlterUserAttributesTakesAbsolutePath` (absolute value),
`AlterLoginTouchesNoPath` (empty), `CreateFullBackupOpTargetsWorkingDir`
(WorkingDir itself), `SplitMergeIsAbsoluteNotWorkingDirRelative`,
`EveryOperationTypeIsCovered`. `DropTableByNameAndById`, `ConsistentCopyTables`
and `MoveIndexIsLeafUnderSibling` also pin `AnchorIndex`, including that the
copy and move cascades anchor on the source rather than on the last-emitted ref.

`EveryOperationTypeIsCovered` walks `EOperationType_descriptor()`, builds a
`TModifyScheme` with only `OperationType` set, and asserts a non-empty result
except for a pinned allowlist of op types that legitimately extract nothing from
an empty proto: `ESchemeOp_DEPRECATED_35`, `AlterLogin`, `AlterBlobDepot`,
`DropBlobDepot`, `AlterView`, `IncrementalRestoreLockTargets`,
`IncrementalRestoreUnlockTargets`, `CreateConsistentCopyTables` (repeated-only).

`TSchemeShardPathFootprintPropose` (`TTestEnv`, observed through the log):
`CreateTableWithIntermediateDirs`, `CreateIndexedTable`, `CreateCdcStream`,
`MoveTable`, `DropTableByNameAndById`, `ConsistentCopyTables`,
`RejectedCreateTableStillProducesFootprint`.

### Smoke

```
ut_base -F 'TSchemeShardTest::MkRmDir' -F '...CreateWithIntermediateDirs' -F '...NestedDirs'
{"type": "summary", "exit_code": 0, "tests": {"OK": 3}}

ut_auditsettings
{"type": "summary", "exit_code": 0, "tests": {"OK": 5}}

hya make -T --build=relwithdebinfo -j128 ydb/core/tx/schemeshard
{"type": "summary", "exit_code": 0}
```

### What the integration tests confirm about H2

- **Auto-generated MkDirs are covered.** `CreateTable WorkingDir=/MyRoot
  Name="a/b/Table"` produces `MkDir /MyRoot a`, `MkDir /MyRoot/a b`, then
  `CreateTable /MyRoot/a/b Table` with `workingDirRelToDb# a/b`,
  `relToDb# a/b/Table`, `relToWorkingDir# Table`, `exists# 0` (nothing exists
  yet at Propose time). Note the test env itself creates `/MyRoot/.sys`, so
  MkDir footprints have to be filtered by path, not counted.
- **Compound ops arrive already normalized.** `CreateIndexedTable` yields three
  parts whose protos each have an absolute `WorkingDir` and a leaf `Name`:
  `CreateTable /MyRoot/Table`, `CreateTableIndex /MyRoot/Table/byValue`,
  `CreateTable /MyRoot/Table/byValue/indexImplTable`. The impl-table part's op
  type is **`ESchemeOpCreateTable`**, not `ESchemeOpInitiateBuildIndexImplTable`.
- **`ConsistentCopyTables` fans out to per-item `CreateTable` parts** at
  `/MyRoot/Dst0` and `/MyRoot/Dst1`; the client-level `SrcPath`/`DstPath`
  attribution only exists at layer 1 (H1), not in the per-part protos.
- **Drop by id resolves.** `Drop.Id` → `kind# ById`, `absPath# /MyRoot/ById`,
  `exists# 1`.
- **Rejected proposes produce a footprint.** `CreateTable` into a missing
  `/MyRoot/NoSuchDir` logs `absPath# /MyRoot/NoSuchDir/Table`, `exists# 0`,
  `proposeStatus# StatusPathDoesNotExist`, `relToDb# NoSuchDir/Table`.

`BackupBackupCollection` was **not** covered by an integration test: it needs
`EnableBackupService`, a created collection with an entry list, and the backup
tablets, i.e. the `ut_backup_collection` fixture rather than a bare `TTestEnv`.
Its static shape *is* covered by `EveryOperationTypeIsCovered` and by the
`Implicit` marker.

---

## 4. Gaps found

Op types where `ExtractPathRefs` **cannot** express the full touched set. Each of
these emits an explicit `Implicit` entry, so the gap is visible in the output
rather than silent:

| Gap class | Op types | Why |
|---|---|---|
| Cascaded subtree on drop/move | `DropTable`, `DropColumnStore`, `ForceDropSubDomain`, `ForceDropExtSubDomain`, `ForceDropUnsafe`, `MoveTable`, `MoveIndex`, `DropIndex` | the request names only the root; indexes, impl tables, CDC streams and PQ groups under it are enumerated from `TPathElement::GetChildren()` at Propose/Execute time |
| Derived children of compound creates | `CreateIndexedTable` (impl tables), `CreateCdcStream` (PQ group under the stream), `CreateContinuousBackup`, `AlterContinuousBackup` (incremental-backup table), `ConsistentCopyTables` (index/impl-table/sequence mirrors) | the child names come from schemeshard state, not from the request. H2 recovers most of them anyway, because the *derived part protos* name them (verified for `CreateIndexedTable`) |
| Runtime/state-derived path sets | `BackupBackupCollection`, `BackupIncrementalBackupCollection`, `CreateLongIncrementalBackupOp`, `CreateFullBackupOp`, `RestoreBackupCollection`, `CreateLongIncrementalRestoreOp`, `DropBackupCollection` | the concrete table/stream set comes from the collection's stored `ExplicitEntryList` (`NBackup::GetBackupRequiredPaths`), which is not in the request at all |
| No path in the proto | `IncrementalRestoreFinalize` | paths come from persisted incremental-restore state |
| Genuinely pathless | `AlterLogin`, `AlterBlobDepot`, `DropBlobDepot` (no-op `Propose` stubs), `AlterView` (unimplemented), `ESchemeOp_DEPRECATED_35` | nothing to extract; the hook logs a `fieldPath# <none>` line so the part is still observable |
| Retired | `RestoreMultipleIncrementalBackups`, `RestoreIncrementalBackupAtTable` | factory always `CreateReject`s; the extractor still handles them, but the case is dead |

This matches S2's independent survey: cascade fan-out (indexes, impl tables,
CDC streams, PQ groups, sequences) is *never* named in the triggering request
for any op in this tree — it is uniformly `TPathElement::GetChildren()` state.
S2's three-tier attribution rule maps onto the prototype as follows:

| S2 tier | Prototype |
|---|---|
| 1 — byte-identical original tx passthrough | needs no code: at H2 the part *is* the client proto, so `FieldPath` already names real client fields. Detecting the case would only let the output *say* "this is the original request" |
| 2 — static field table with repeated index / map key | implemented as the `switch`; repeated indices and map keys are already in `FieldPath` (`CopyTableDescriptions[i]`, `IndexDescription[i]`, `StreamName[i]`, `IndexImplTableCdcStreams[key]`) |
| 2 — `Implicit(<anchor>)` | implemented via `AnchorIndex`, resolved to the anchor's `TPathId` |
| 3 — runtime-derived fallback | the `Implicit` entries with a backup-collection anchor; filling in the concrete set still needs the post-Propose `TTxState` layer, which the prototype does not build |

Two further, smaller gaps:

- **Dependency paths to *other* objects** are emitted only for
  `CreateExternalTable.DataSourcePath`. `Replication.Target[].DstPath` /
  `.DirectoryPath` are resolved absolutely in `Propose()` but not extracted;
  adding them is a two-line change if the consumer wants them.
- **`ResolveWithInactive`.** `MoveTable`'s destination uses
  `TPath::ResolveWithInactive(opId, ...)`, which can attach to the target path
  of an earlier part of the *same* transaction. Layer 2 uses plain `Resolve`,
  since it has no `TOperationId`. Consequence: a move whose destination name was
  just freed by an earlier part of the same tx reports `exists# 0` where
  `Propose()` sees a resolved inactive path. Passing `part->GetOperationId()`
  into `ResolvePathFootprint` would close this; it costs one more argument at
  the hook and was left out of the prototype.

---

## 5. `ExtractChangingPaths` — how it could be replaced later

Not rewired here, on purpose: `ExtractPathRefs` deliberately **fixes** the eight
S1-confirmed audit bugs, so a naive parity assert would fail on exactly the cases
worth fixing. The migration is mechanical once the audit-log output change is
accepted:

```cpp
TVector<TString> ExtractChangingPaths(const TModifyScheme& tx) {
    TVector<TString> result;
    for (const auto& ref : ExtractPathRefs(tx)) {
        if (ref.Kind == EPathRefKind::Implicit || ref.Kind == EPathRefKind::ById) continue;
        result.push_back(JoinPathRef(tx.GetWorkingDir(), ref));  // string-only, no TSchemeShard
    }
    return result;
}
```

`JoinPathRef` is the string half of layer 2 (the `StartsWith('/')` rule plus
`Child`), so audit stays state-free. The id-addressed cases need `TSchemeShard*`
to produce a name, so either audit keeps dropping them (status quo, but now
explicitly) or `schemeshard_audit_log.cpp` passes the `TPathFootprint` that the
hook already computed. The second is strictly better: the audit log is emitted at
`schemeshard__operation.cpp:474+` with the final response, and
`operation->PathFootprints` is in scope there for accepted operations.
Gate the switch-over behind `ut_auditsettings` golden updates for the eight
changed op families.

---

## 6. Could the design be simpler?

Three honest simplifications, in decreasing order of value:

1. **Drop `EPathRefRole` from the prototype.** Nothing in layer 2, the log line,
   or the hook branches on it; it is pure metadata for the consumer. It costs
   nothing to keep, but if the colleague does not need Source/Parent/Dependency
   distinctions, half the per-case thinking in the switch disappears.
2. **~~Drop the `Implicit` entries.~~** Superseded. Before anchoring they
   resolved to nothing and were arguably worth deleting. Anchored, they carry
   the `TPathId` whose children *are* the touched set, which is the only thing
   that makes the level-2 cascade addressable without a post-Propose `TTxState`
   pass. Keep them.
3. **Do not go table-driven (design 1a from the plan).** The plan proposed a
   `THashMap<EOperationType, TVector<TFieldSpec>>` plus a protobuf-reflection
   walker. That is *more* code, not less: the reflection walker has to handle
   repeated fields, map keys, nested submessages, and the id-or-name branches,
   and it loses `-Wswitch` completeness enforcement. The hand-written switch is
   136 case labels but only ~8 distinct code paths, and the compiler guarantees
   it stays complete. **Recommendation: keep the switch.**

What should *not* be simplified away:

- **H2 (per-part) is the right hook**, confirmed empirically: it is the only
  place that sees auto-generated MkDirs, compound-op children, and the propose
  status together, and the derived part protos are already normalized (absolute
  `WorkingDir` + leaf `Name`), which makes layer 2 almost trivial for them.
- The **`OwnerId` + `LocalPathId` pair** in `TPathRef`; a bare local id is wrong
  for `AlterTable.PathId`.
- Splitting layer 1 from layer 2. Layer 1 is testable with no actor runtime at
  all, which is what makes the 14 pure tests cheap.

**Open question for the colleague (plan §3.2):** at H2 the `FieldPath` points at
a field of the *derived part's* proto, not of the client's request. For
`CreateIndexedTable` the impl-table part's `FieldPath` is `CreateTable.Name` with
`absPath /MyRoot/Table/byValue/indexImplTable` — correct and useful, but it is
not a field the client ever wrote. If every entry must point at a client field,
H2 has to be paired with H1 (footprint of `record.GetTransaction()[i]` before
`SplitIntoTransactions`) and the two joined by originating transaction index.
That is a further ~10 lines in `IgniteOperation`, and it is the only remaining
piece of the §0 `ProtoRef` requirement that the prototype does not fully satisfy.
