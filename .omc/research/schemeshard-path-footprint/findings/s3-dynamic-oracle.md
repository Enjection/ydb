# S3 — Dynamic oracle: what SchemeShard actually resolves during Propose

Status: DONE (2026-09-02). Experiment run in an isolated git worktree
(`ydb/.claude/worktrees/agent-a57d9011bfeb2d816`); no source changes were
committed — the worktree was reverted at the end (`git status --short` under
`ydb/` is clean).

## 1. Method

Two files were temporarily instrumented (diff: 41 lines in
`schemeshard__operation.cpp`, 109 lines in `schemeshard_path.cpp`, both
additive, no existing logic touched):

1. **`ydb/core/tx/schemeshard/schemeshard_path.cpp`** — a process-global
   `TString g_FootprintTag` plus a small `TFootprintRecorder` singleton
   (mutex-guarded `TFile`, opened lazily, silently disabled if the target
   directory can't be created). Every return path of `TPath::Dive`,
   `TPath::Child` (both overloads), `TPath::Resolve` (both overloads, the
   `TString` entry point plus the internal one — tagged as `Resolve`),
   `TPath::ResolveWithInactive`, and `TPath::Init` appends one TSV line:
   ```
   <tag>\t<method>\t<PathString()>\t<IsResolved 0/1>\t<PathId or empty>
   ```
   `IsResolved()`/`Base()` are only dereferenced when resolved (`PathString()`
   itself is safe unresolved — it just joins `NameParts`). Logging is a no-op
   (single `TString::empty()` check) whenever `g_FootprintTag` is empty, so it
   costs nothing outside of Propose.

2. **`ydb/core/tx/schemeshard/schemeshard__operation.cpp`** —
   - `TTxOperationPropose::Execute` sets `g_FootprintRequestTag =
     "REQUEST=<opType1>[+opType2...],txId=<txId>"` from `record.GetTransaction()`
     right before calling `Self->IgniteOperation(...)`, and clears both the
     request tag and any leftover part tag right after it returns.
   - `TSchemeShard::ProcessOperationParts` (the single loop that calls
     `part->Propose(owner, context)` for every constructed part, for both the
     auto-generated-`MkDir` pass and the main pass) sets
     `g_FootprintTag = "<requestTag>|part=<PartOpTypeName>#<counter>"` right
     before `Propose`, and right after (once `response` is known non-null)
     appends a `STATUS` line with `NKikimrScheme::EStatus_Name(...)`, then
     clears `g_FootprintTag` so nothing outside that one `Propose` call is
     attributed to it.

   This exactly matches finding 2 in the research plan ("this loop is the
   single natural hook point").

3. Ran, in one `hya make -T --build=relwithdebinfo -j128 -ttt ...` invocation
   (see §8), all six requested UT directories: `ut_base`, `ut_cdc_stream`,
   `ut_move`, `ut_consistent_copy_tables`, `ut_index_build`,
   `ut_backup_collection`. Build+test took ~6 minutes wall clock once
   configure finished; `exit_code=0`, `tests: {"OK": 561}` — instrumentation
   did not crash or fail anything.

4. Every test worker process wrote its own file
   (`footprint.<pid>.tsv`, one file per UT process/fork — 41 files, 172,691
   lines, 26 MB total). Aggregated with a small Python script
   (`analyze_footprint.py`, logic described in §3) into
   `footprint_summary.json` + a text report.

5. Reverted the worktree's instrumented files back to HEAD (`schemeshard_path.cpp`,
   `schemeshard__operation.cpp`); `git status --short` under `ydb/` is clean
   (only the pre-existing untracked `.omc/` remains).

### Line-kind breakdown (172,691 total)

| method | count |
|---|---|
| Dive | 86,813 |
| Resolve | 28,020 |
| STATUS | 27,382 |
| Child | 24,593 |
| Init | 5,663 |
| ResolveWithInactive | 220 |

`Dive` dominates because every `Resolve`/`Child(..., TSplitChildTag)` call
internally walks one `Dive` per path segment, so it is expected noise, not a
separate "resolution attempt" — the per-op-type tables in §4 report the
**outer** `Resolve`/`Child`/`Init`/`ResolveWithInactive` calls as the
meaningful "touch" count and list `Dive` only in the method histogram for
context.

## 2. Coverage: which of the 136 `EOperationType`s were exercised

`NKikimrSchemeOp::EOperationType` (`ydb/core/protos/schemeshard/operations.proto`)
has exactly **136** values.

- **68 op types** were observed as a **part** actually proposed
  (`part->Propose(...)` called with that `GetTransaction().GetOperationType()`).
- **59 op types** were observed as a **top-level request** op type
  (`record.GetTransaction()` entries feeding `TTxOperationPropose::Execute`).
- **82 of 136 (60%)** were observed in *either* role.
- **54 of 136 (40%)** were never exercised by this test selection at all —
  expected, since `ut_view`, `ut_external_table`, `ut_replication`,
  `ut_login`, `ut_filestore`, `ut_column_store`, `ut_streaming_query`,
  `ut_resource_pool`, `ut_secret`, `ut_transfer`, `ut_blob_depot`,
  `ut_rtmr`, etc. were intentionally out of scope for this run. Not tested:
  `ESchemeOpAlterBackupCollection`, `ESchemeOpAlterBlobDepot`,
  `ESchemeOpAlterColumnStore`, `ESchemeOpAlterContinuousBackup`,
  `ESchemeOpAlterExternalDataSource`, `ESchemeOpAlterExternalTable`,
  `ESchemeOpAlterFileStore`, `ESchemeOpAlterLogin`, `ESchemeOpAlterReplication`,
  `ESchemeOpAlterResourcePool`, `ESchemeOpAlterSecret`,
  `ESchemeOpAlterStreamingQuery`, `ESchemeOpAlterTransfer`, `ESchemeOpAlterView`,
  `ESchemeOpCreateBlobDepot`, `ESchemeOpCreateColumnStore`,
  `ESchemeOpCreateContinuousBackup`, `ESchemeOpCreateExternalTable`,
  `ESchemeOpCreateFileStore`, `ESchemeOpCreateReplication`,
  `ESchemeOpCreateResourcePool`, `ESchemeOpCreateRtmrVolume`,
  `ESchemeOpCreateSecret`, `ESchemeOpCreateStreamingQuery`,
  `ESchemeOpCreateTestShardSet`, `ESchemeOpCreateTransfer`, `ESchemeOpCreateView`,
  `ESchemeOpDropBlobDepot`, `ESchemeOpDropColumnBuild`,
  `ESchemeOpDropColumnStore`, `ESchemeOpDropContinuousBackup`,
  `ESchemeOpDropExternalDataSource`, `ESchemeOpDropExternalTable`,
  `ESchemeOpDropFileStore`, `ESchemeOpDropReplication`,
  `ESchemeOpDropReplicationCascade`, `ESchemeOpDropResourcePool`,
  `ESchemeOpDropSecret`, `ESchemeOpDropStreamingQuery`, `ESchemeOpDropSubDomain`,
  `ESchemeOpDropSysView`, `ESchemeOpDropTestShardSet`, `ESchemeOpDropTransfer`,
  `ESchemeOpDropTransferCascade`, `ESchemeOpDropView`,
  `ESchemeOpForceDropExtSubDomain`, `ESchemeOpForceDropSubDomain`,
  `ESchemeOpIncrementalRestoreLockTargets`,
  `ESchemeOpIncrementalRestoreUnlockTargets`,
  `ESchemeOpRestoreIncrementalBackupAtTable`,
  `ESchemeOpRestoreMultipleIncrementalBackups`, `ESchemeOpRotateCdcStream`,
  `ESchemeOpTruncateTable`, `ESchemeOp_DEPRECATED_35`.

**Important architectural confirmation**: the op types that were a
*request* but never a *part* — e.g. `ESchemeOpCreateCdcStream`,
`ESchemeOpCreateConsistentCopyTables`, `ESchemeOpCreateIndexedTable`,
`ESchemeOpCreateIndexBuild`, `ESchemeOpApplyIndexBuild`,
`ESchemeOpCancelIndexBuild`, `ESchemeOpBackupBackupCollection`,
`ESchemeOpBackupIncrementalBackupCollection`, `ESchemeOpRestoreBackupCollection`,
`ESchemeOpCreateColumnBuild` — are exactly the **compound/driver** op types.
They never themselves reach `part->Propose`; they only exist to be split
into derived parts by their own `ConstructParts`/`DoPropose` logic (finding 3
in the research plan). This is ground truth for "the top-level op type as
requested by the client is not what `TPath` resolution ever sees" — the
static extractor (S4/layer 1) must key its per-part table on the **derived**
part op types, not the request op type, for these families.

## 3. Post-processing script

`analyze_footprint.py` (copied to `findings/s3-raw/analyze_footprint.py` for
reproducibility) parses every `footprint.*.tsv`, decodes the tag
(`REQUEST=<opTypes>,txId=<id>|part=<PartOpType>#<counter>`), and produces:

- **Per part op type**: distinct path *shapes* touched (concrete segments
  replaced by placeholders — `<table>`, `<index>`, `<stream>`, `<dir>`, a
  positional `<segN>` fallback for anything else — root/database segment and
  known system dirs `.sys`, `.backups`, `collections`, `indexImplTable` kept
  literal), with occurrence counts, one example concrete path per shape, the
  method histogram (`Dive`/`Resolve`/`Child`/`Init`/`ResolveWithInactive`),
  the resolved fraction, and the `Propose` status distribution.
- **Per request op type**: the set of derived part op types actually
  proposed, with counts — this is the dynamic cross-check for S2's static
  derived-parts map.

Full machine-readable output: `findings/s3-raw/footprint_summary.json`.
Full text report: `findings/s3-raw/footprint_report.txt`.

## 4. Per-part-op-type path shapes (selected — see full JSON for all 68)

Format: `shape` (placeholder path) — `count` — one concrete `example`.
`resolved_fraction` is the fraction of *outer* `Resolve`/`Child`/`Init`/
`ResolveWithInactive` calls (not raw `Dive`) where `IsResolved()` was true —
low fractions are expected and healthy: they mostly mean "checking a name
does not yet exist" (create-side) or "the leaf under WorkingDir is the new
name being resolved before it exists".

### `ESchemeOpCreateTable` (11,820 path-touch lines, part of `CreateTable`,
`CreateIndexedTable`, `CreateConsistentCopyTables`, `BackupBackupCollection`, `RestoreBackupCollection`, ...)

| shape | count | example |
|---|---|---|
| `/MyRoot` | high | `/MyRoot` |
| `/MyRoot/<seg1>` | high | `/MyRoot/Table` (leaf under WorkingDir, unresolved) |
| `/MyRoot/<seg1>/<seg2>` | med | nested table under a subdir |
| `/MyRoot/.backups/collections/<seg3>/<seg4>/<seg5>` | med | table materialized under a backup-collection incremental/full dir |
| `/MyRoot/<seg1>/<seg2>/<index>` | med | table inside an index's impl-table slot |

### `ESchemeOpDropTable` (2,115 lines; methods `Dive`/`Resolve`/**`Init`**)

| shape | count | example |
|---|---|---|
| `/MyRoot` | 473 | root |
| `/MyRoot/<seg1>` | 277 | direct table under root |
| `/MyRoot/<seg1>/<table>/<index>` | 202 | index cascaded from parent table drop |
| `/MyRoot/<seg1>/<seg2>/<index>` | 87 | impl table of a nested index |
| `/MyRoot/.backups/collections/<seg3>/<seg4>` | 71 | backup-collection item dropped as part of collection teardown |

Confirms offender **"Drop by id"** from plan §1a: one `DropTable` part in
this run resolved via `TPath::Init` from `Drop.Id` directly (no
`WorkingDir`/`Name` in the trace at all for that entry):
```
REQUEST=ESchemeOpDropTable,txId=1002|part=ESchemeOpDropTable#658  Init  /MyRoot/Table  1  [OwnerId: 72057594046678944, LocalPathId: 36]
```
By-id drops are rare relative to by-name drops in the exercised UTs (1 of
415 accepted `DropTable` parts), but they are real and must be a distinct
`EKind::ById` in the layer-1 table, not inferred from `WorkingDir+Name`.

### `ESchemeOpMoveTable` / `ESchemeOpMoveTableIndex` (1,120 / 712 lines;
uses **`ResolveWithInactive`**, 159 and 54 calls respectively — all 220
`ResolveWithInactive` calls in the whole run come from `Move*` family)

Confirms offender **"Move* dst may be a just-dropped/inactive name"**:
```
REQUEST=ESchemeOpMoveTable,txId=102|part=ESchemeOpMoveTable#78   ResolveWithInactive  /MyRoot/TableMove                    0
REQUEST=ESchemeOpMoveTable,txId=102|part=ESchemeOpMoveTable#80   ResolveWithInactive  /MyRoot/TableMove/index/indexImplTable  0
```
Shapes include `/MyRoot/<seg1>/<seg2>/indexImplTable` — i.e. `MoveTable`
resolves impl-table children of the source under the *destination*'s
would-be name via the inactive-aware resolver, exactly the cascade the plan
flagged.

### `ESchemeOpCreateCdcStreamImpl` (728 lines; part of `CreateCdcStream`)

Shapes touch the table, the new stream leaf under the table
(`/MyRoot/<table>/<stream>`), the PQ group that backs it
(cross-checked against `ESchemeOpCreatePersQueueGroup`, 1,489 lines, which is
always proposed alongside — see §5 request→parts table), index-impl-table
variants (`/MyRoot/<table>/<index>/indexImplTable/<stream>` shows up in
`ESchemeOpAlterCdcStreamAtTable`'s shape list), and the auto-generated
`19700101000000Z_continuousBackupImpl` stream name used by the
continuous-backup/incremental-restore family.

### Backup-collection family (`ESchemeOpCreateFullBackupOp` 260 lines,
`ESchemeOpCreateLongIncrementalBackupOp` 115, `ESchemeOpCreateLongIncrementalRestoreOp` 60,
all **`resolved_fraction=1.0`**)

All shapes are anchored at `/MyRoot/.backups/collections/<seg3>` — confirms
the `<domain>/.backups/collections/<name>/...` layout from §1a, and that (at
Propose time, for these particular part op types) the collection directory
itself is always already resolved — the runtime-derived item list
(`Implicit` entries in the plan's `EKind`) is not visible as raw `TPath`
calls in these specific parts; it shows up instead in the *derived*
`CreateTable`/`CreatePersQueueGroup`/`CreateTableIndex` parts that
`BackupBackupCollection` fans out into (see §5).

### `ESchemeOpInitiateBuildIndexImplTable` (3,890 lines — a rejected-proposal
example)

```
REQUEST=ESchemeOpInitiateBuildIndexImplTable,txId=...|part=ESchemeOpInitiateBuildIndexImplTable#950
    Child  /MyRoot/Table/idx_global/indexImplPostingTable0build  0
STATUS StatusResourceExhausted
```
This is a clean instance of the plan's open question 3 ("rejected
proposals: is 'resolved as far as possible' enough?") — the trace shows
exactly the intended leaf (`indexImplPostingTable0build`) was looked up,
found unresolved (parent chain existed, leaf didn't — expected, it's a
brand-new impl table), and the propose was then rejected for an unrelated
reason (resource exhaustion), not a path problem. A `TPath`-level trace
alone cannot distinguish "rejected because of this path" from "rejected for
an unrelated reason after touching this path" — that requires reading the
`STATUS` line's status name alongside the path shapes, which this format
already provides.

### `ESchemeOpCreateSysView` (95,928 lines — the single largest bucket)

This is test-harness noise, not signal: `TTestEnv` creates ~10-20 system
views (`auth_permissions`, `auth_effective_permissions`, ...) on every test's
DB bootstrap, and with hundreds of test cases across 6 UT dirs this dwarfs
everything else. Path shapes are trivial (`/MyRoot/.sys/<seg1>`), all
resolved-or-not exactly as expected. Excluded from the "top by call volume"
table below its own row for that reason.

## 5. Request → derived parts (dynamic cross-check for S2)

Selected compound ops (full table for all 59 observed request op types is in
`footprint_report.txt` / `footprint_summary.json["per_request"]`):

| REQUEST op type | derived PART op types actually proposed |
|---|---|
| `ESchemeOpCreateCdcStream` | `CreateCdcStreamImpl`, `CreateCdcStreamAtTable`, `CreatePersQueueGroup`, `MkDir`, `CreateLock`, `AlterTableIndex` |
| `ESchemeOpCreateConsistentCopyTables` | `CreateTable`, `CreateTableIndex`, `CreateSequence`, `MkDir`, `CreateColumnTable` |
| `ESchemeOpCreateIndexedTable` | `CreateTable`, `CreateTableIndex`, `MkDir`, `CreateSequence` |
| `ESchemeOpCreateIndexBuild` | `InitiateBuildIndexImplTable`, `InitiateBuildIndexMainTable`, `CreateTableIndex`, `CreateSequence`, `AlterTableIndex` |
| `ESchemeOpApplyIndexBuild` | `FinalizeBuildIndexMainTable`, `FinalizeBuildIndexImplTable`, `AlterTableIndex`, `DropTable`, `MkDir` |
| `ESchemeOpCancelIndexBuild` | `DropTable`, `FinalizeBuildIndexMainTable`, `DropTableIndex` |
| `ESchemeOpBackupBackupCollection` | `MkDir`, `CreateTable`, `CreateCdcStreamImpl`, `CreatePersQueueGroup`, `CreateFullBackupOp`, `CreateTableIndex`, `CreateSequence` |
| `ESchemeOpBackupIncrementalBackupCollection` | `MkDir`, `RotateCdcStreamImpl`, `RotateCdcStreamAtTable`, `CreateTable`, `AlterPersQueueGroup`, `CreatePersQueueGroup`, `CreateLongIncrementalBackupOp` |
| `ESchemeOpRestoreBackupCollection` | `CreateTable`, `CreateLongIncrementalRestoreOp`, `ChangePathState`, `CreateTableIndex`, `CreateSequence` |
| `ESchemeOpDropBackupCollection` | `DropTable`, `DropBackupCollection`, `MkDir`, `DropCdcStreamImpl`, `DropPersQueueGroup`, `DropCdcStreamAtTable` |
| `ESchemeOpDropTable` | `DropTable`, `DropTableIndex`, `DropCdcStreamImpl`, `DropPersQueueGroup`, `MkDir`, `DropSequence` |
| `ESchemeOpDropIndex` | `DropTable`, `DropTableIndex`, `DropTableIndexAtMainTable`, `MkDir`, `DropSequence`, `AlterTable`, `DropCdcStreamImpl`, `DropPersQueueGroup` |
| `ESchemeOpMoveTable` | `MoveTable`, `MoveTableIndex`, `MoveSequence`, `MoveIndex`, `MkDir` |
| `ESchemeOpMoveIndex` | `MoveTable`, `AlterTable`, `MoveTableIndex`, `DropTable`, `MkDir`, `DropTableIndex`, `MoveSequence`, `DropSequence` |
| `ESchemeOpAlterTable` | `AlterTable`, `CreateTableIndex`, `DropSequence`, `DropTableIndex`, `AlterTableIndex`, `MkDir` |
| `ESchemeOpAlterCdcStream` | `AlterCdcStreamImpl`, `AlterCdcStreamAtTable`, `DropLock`, `MkDir`, `AlterTableIndex` |
| `ESchemeOpDropCdcStream` | `DropCdcStreamImpl`, `DropPersQueueGroup`, `DropCdcStreamAtTable`, `MkDir`, `AlterTableIndex`, `DropLock` |
| `ESchemeOpCreateColumnBuild` | `InitiateBuildIndexMainTable` |

This matches the plan's §1a "Known offenders" table closely: every one of the
"many-to-many"/cascading offenders it named
(`ConsistentCopyTables`→src CDC/index-impl fan-out, `MoveTable`→index/sequence
cascade, `DropTable`/`DropIndex`→CDC+PQ+sequence+impl-table cascade,
`BackupBackupCollection`/`RestoreBackupCollection`→table+CDC+PQ+index fan-out
under `.backups/collections`) is visible directly in the dynamic trace, with
concrete counts, not just plausible from reading the source.

Also confirms the "Auto-generated MkDirs" §1a offender:
`ESchemeOpMkDir` is a derived part of a **majority** of the compound ops
above — it appears as a derived part of 15 of the 18 request op types listed
here.

One artifact of the tag design worth noting for anyone reusing this data:
when a single `TEvModifySchemeTransaction` carries N repeated identical-type
sub-transactions (observed for `CreateTable` x100+ in one `ut_base` stress
test, and `DropTable`/`DropLock`/`MoveTable`/`InitiateBuildIndexImplTable`
x2-3 in a few `ut_move`/`ut_index_build` cases), the request tag naively
joins all N names with `+`, producing a very long, repetitive `REQUEST=`
string. It is still a valid unique-enough key (only 9 of the parsed request
tags have a `+` at all) but a real (non-experimental) implementation should
dedupe/count instead of concatenating.

## 6. Status distribution (evidence for open question 3 — rejected proposals)

27,382 `STATUS` lines were recorded. Non-`StatusAccepted`/`StatusSuccess`
statuses seen across the run include `StatusMultipleModifications`,
`StatusPathDoesNotExist`, `StatusSchemeError`, `StatusAlreadyExists`,
`StatusNameConflict`, `StatusPathIsNotDirectory`, `StatusInvalidParameter`,
`StatusResourceExhausted`, `StatusPreconditionFailed` — i.e. the existing
test suites do exercise a reasonable spread of rejection paths, and the
per-part-op-type `status_distribution` in `footprint_summary.json` gives an
exact count per op type. Two useful patterns observed:

- **Some rejections have zero `TPath` calls for that part at all** (e.g.
  `REQUEST=ESchemeOpMoveIndex,txId=103|part=ESchemeOpMkDir#193  STATUS
  StatusPathDoesNotExist` with no preceding `Dive`/`Resolve`/`Child` line for
  that tag) — the rejection happened before any `TPath` resolution for that
  particular part (e.g. quota/limit check, or the part never reached a
  point where it resolves a path). A footprint extractor that only hooks
  `TPath` calls will report **no touched paths** for these, which is
  arguably correct (nothing was touched) but worth flagging explicitly in the
  product's schema rather than silently omitting the entry.
- **Some rejections have an unresolved leaf right before them** (the
  `InitiateBuildIndexImplTable`/`StatusResourceExhausted` example in §4) —
  "resolved as far as possible" (nearest existing ancestor + intended leaf)
  is a faithful description of what actually happens at the `TPath` layer for
  these.

## 7. Raw data

- Raw per-process TSV dumps (41 files, 172,691 lines, 26 MB):
  `.omc/research/schemeshard-path-footprint/findings/s3-raw/footprint.*.tsv`
- Post-processing script: `.omc/research/schemeshard-path-footprint/findings/s3-raw/analyze_footprint.py`
- Machine-readable aggregate: `.omc/research/schemeshard-path-footprint/findings/s3-raw/footprint_summary.json`
- Full text report (all 68 part op types x shapes, all 59 request op types x derived parts): `.omc/research/schemeshard-path-footprint/findings/s3-raw/footprint_report.txt`

## 8. Build/test commands used

Build (from repo root, per project rules -- hya, -T streaming, relwithdebinfo, -ttt to run tests):

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_base ydb/core/tx/schemeshard/ut_cdc_stream ydb/core/tx/schemeshard/ut_move ydb/core/tx/schemeshard/ut_consistent_copy_tables ydb/core/tx/schemeshard/ut_index_build ydb/core/tx/schemeshard/ut_backup_collection
```

Result: exit_code 0, tests: {"OK": 561}, build+test wall time ~6 minutes
after configure (configure ~19s).

Revert: the two instrumented files (`schemeshard_path.cpp`,
`schemeshard__operation.cpp`) were restored to their pre-instrumentation
content in the worktree; the worktree's tracked-file status under `ydb/` is
clean (only the pre-existing untracked `.omc/` research directory remains).
