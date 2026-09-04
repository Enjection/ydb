# S5 part 2 — measured coverage: prototype vs the S3 dynamic oracle

## 1. How this was measured

The six suites S3 used were re-run on `feat/schemeshard-path-footprint`:

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_base \
  ydb/core/tx/schemeshard/ut_cdc_stream ydb/core/tx/schemeshard/ut_move \
  ydb/core/tx/schemeshard/ut_consistent_copy_tables \
  ydb/core/tx/schemeshard/ut_index_build ydb/core/tx/schemeshard/ut_backup_collection
{"type": "summary", "ts": 1788393500.88, "exit_code": 0, "tests": {"OK": 561}}
```

Zero `"status": "FAILED"` records. **561 OK is exactly the count S3 measured on
uninstrumented `main`, so the hook changes no test outcome.**

The prototype's `PathFootprint` lines were harvested from the per-test stderr
that `ya` collects (`<ut>/test-results/unittest/chunk*/testing_out_stuff/*.err`;
note `test-results` is a symlink, so `grep -R`, not `grep -r`):

| suite | PathFootprint lines |
|---|--:|
| ut_base | 30,111 |
| ut_index_build | 25,920 |
| ut_backup_collection | 10,758 |
| ut_move | 7,176 |
| ut_cdc_stream | 6,903 |
| ut_consistent_copy_tables | 4,818 |
| **total** | **85,686** |

Raw lines: `findings/s5-raw/pathfootprint.lines` (post-fix, ~43 MB); the
pre-fix harvest is kept as `pathfootprint.v1-prefix.lines`.
Comparison script: `findings/s5-raw/compare_coverage.py`.
Machine-readable result: `findings/s5-raw/coverage.json`.

Both sides are normalized with **the same** `normalize_path_shape()` function
S3 used — the script imports it verbatim out of
`findings/s3-raw/analyze_footprint.py` — so the two shape vocabularies are
identical by construction rather than by eyeballing.

`ESchemeOpCreateSysView` is excluded (S3 called it test-harness noise: ~96k of
its path touches come from `TTestEnv` creating ~20 system views per DB
bootstrap). Shapes at depth <= 1 (`/MyRoot`, the domain root) and anything under
`/MyRoot/.sys/` are dropped as noise.

**Part op types match exactly: 68 observed by the oracle, 68 emitting footprint
lines, same set.** Nothing the oracle saw proposed is invisible to the hook.

## 2. Column meanings, and one honest limitation

- **covered** — an S3 shape that a non-`Implicit` footprint entry also produced.
- **parent-walk** — an S3 shape that is a strict ancestor of a shape the
  prototype does report. These are `TPath::Dive` intermediates: the oracle logs
  one line per path segment walked, so every resolution of `/a/b/c` also emits
  `/a` and `/a/b`. They are not paths the operation "touches" in any meaningful
  sense, and are scored separately rather than counted as gaps.
- **missing** — an S3 shape with no counterpart at all. This is the real gap.

Limitation to keep in mind: **S3 shapes are heuristic placeholders**
(`<table>`, `<index>`, `<seg3>`) derived from UT fixture naming, so
`/MyRoot/Table` and `/MyRoot/TableWithIndex` can normalize to different shapes
(`/MyRoot/<table>` vs `/MyRoot/<index>`) while being the same structural thing.
That inflates the shape count on both sides and produces some spurious
"missing" rows where the prototype emitted the same path under a different
placeholder. I checked every row with a non-zero missing count by hand against
the concrete examples and the extractor source; the classifications in §4 are
based on that reading, not on the placeholder strings.

## 3. Coverage table (post-fix)

Measured against `schemeshard_path_footprint.cpp` md5 `190f8401...` (913 lines),
hash-gated before and after the run. The pre-fix table is preserved in
`s5-raw/coverage.v1-prefix.json`.

| part op type | S3 shapes | covered | parent-walk | missing | gap class |
|---|--:|--:|--:|--:|---|
| `AlterPersQueueGroup` | 43 | 13 | 6 | 24 | b runtime-derived (backup collection) |
| `MoveTableIndex` | 11 | 3 | 2 | 6 | a implicit child |
| `CreateTable` | 76 | 44 | 30 | 2 | a implicit child (cb stream) |
| `AlterBlockStoreVolume` | 1 | 1 | 0 | 0 |  |
| `AlterCdcStreamAtTable` | 8 | 6 | 2 | 0 |  |
| `AlterCdcStreamImpl` | 8 | 3 | 5 | 0 |  |
| `AlterExtSubDomain` | 1 | 1 | 0 | 0 |  |
| `AlterExtSubDomainCreateHive` | 1 | 1 | 0 | 0 |  |
| `AlterKesus` | 1 | 1 | 0 | 0 |  |
| `AlterSequence` | 12 | 3 | 9 | 0 |  |
| `AlterSolomonVolume` | 1 | 1 | 0 | 0 |  |
| `AlterSubDomain` | 1 | 1 | 0 | 0 |  |
| `AlterTable` | 10 | 8 | 2 | 0 |  |
| `AlterTableIndex` | 9 | 6 | 3 | 0 |  |
| `AlterUserAttributes` | 2 | 2 | 0 | 0 |  |
| `AssignBlockStoreVolume` | 1 | 1 | 0 | 0 |  |
| `Backup` | 7 | 4 | 3 | 0 |  |
| `ChangePathState` | 6 | 2 | 4 | 0 |  |
| `CreateBackupCollection` | 12 | 9 | 3 | 0 |  |
| `CreateBlockStoreVolume` | 2 | 2 | 0 | 0 |  |
| `CreateCdcStreamAtTable` | 9 | 7 | 2 | 0 |  |
| `CreateCdcStreamImpl` | 26 | 12 | 14 | 0 |  |
| `CreateColumnTable` | 3 | 3 | 0 | 0 |  |
| `CreateExtSubDomain` | 1 | 1 | 0 | 0 |  |
| `CreateExternalDataSource` | 1 | 1 | 0 | 0 |  |
| `CreateFullBackupOp` | 4 | 2 | 2 | 0 |  |
| `CreateKesus` | 1 | 1 | 0 | 0 |  |
| `CreateLock` | 17 | 12 | 5 | 0 |  |
| `CreateLongIncrementalBackupOp` | 3 | 1 | 2 | 0 |  |
| `CreateLongIncrementalRestoreOp` | 3 | 1 | 2 | 0 |  |
| `CreatePersQueueGroup` | 43 | 15 | 28 | 0 |  |
| `CreateSequence` | 34 | 11 | 23 | 0 |  |
| `CreateSolomonVolume` | 1 | 1 | 0 | 0 |  |
| `CreateSubDomain` | 1 | 1 | 0 | 0 |  |
| `CreateTableIndex` | 34 | 17 | 17 | 0 |  |
| `DropBackupCollection` | 3 | 1 | 2 | 0 |  |
| `DropBlockStoreVolume` | 1 | 1 | 0 | 0 |  |
| `DropCdcStreamAtTable` | 6 | 5 | 1 | 0 |  |
| `DropCdcStreamImpl` | 15 | 7 | 8 | 0 |  |
| `DropColumnTable` | 1 | 1 | 0 | 0 |  |
| `DropKesus` | 1 | 1 | 0 | 0 |  |
| `DropLock` | 13 | 8 | 5 | 0 |  |
| `DropPersQueueGroup` | 29 | 10 | 19 | 0 |  |
| `DropSequence` | 18 | 6 | 12 | 0 |  |
| `DropSolomonVolume` | 1 | 1 | 0 | 0 |  |
| `DropTable` | 35 | 23 | 12 | 0 |  |
| `DropTableIndex` | 12 | 8 | 4 | 0 |  |
| `DropTableIndexAtMainTable` | 8 | 8 | 0 | 0 |  |
| `FinalizeBuildIndexImplTable` | 19 | 10 | 9 | 0 |  |
| `FinalizeBuildIndexMainTable` | 13 | 8 | 5 | 0 |  |
| `ForceDropUnsafe` | 4 | 4 | 0 | 0 |  |
| `IncrementalRestoreFinalize` | 0 | 0 | 0 | 0 |  |
| `InitiateBuildIndexImplTable` | 20 | 11 | 9 | 0 |  |
| `InitiateBuildIndexMainTable` | 3 | 3 | 0 | 0 |  |
| `MkDir` | 54 | 51 | 3 | 0 |  |
| `ModifyACL` | 2 | 2 | 0 | 0 |  |
| `MoveIndex` | 2 | 2 | 0 | 0 |  |
| `MoveSequence` | 9 | 4 | 5 | 0 |  |
| `MoveTable` | 10 | 8 | 2 | 0 |  |
| `PrepareIndexValidation` | 13 | 5 | 8 | 0 |  |
| `Restore` | 9 | 6 | 3 | 0 |  |
| `RmDir` | 4 | 4 | 0 | 0 |  |
| `RotateCdcStreamAtTable` | 23 | 18 | 5 | 0 |  |
| `RotateCdcStreamImpl` | 23 | 9 | 14 | 0 |  |
| `SplitMergeTablePartitions` | 9 | 5 | 4 | 0 |  |
| `UpgradeSubDomain` | 1 | 1 | 0 | 0 |  |
| `UpgradeSubDomainDecision` | 1 | 1 | 0 | 0 |  |
| **total (67 part op types)** | **756** | **430** | **294** | **32** | |

Of 756 scored oracle shapes, **430 are covered**, **294 are parent-walk noise**,
and **32 are real gaps**, in **3 of 67 part operation types**. The other 64 have
none.

Before the layer-1 fixes the same measurement gave 399 covered and 62 gaps
across 10 op types. Every closed gap was a table bug; the 32 that remain are the
structural classes (a) and (b) below, and no fix moved a class (a) or (b) shape.
That is the cleanest evidence that the fixes were correct and narrowly scoped.

## 4. Gap classification

### (c) Table bugs — 30 shapes, ALL NOW FIXED AND RE-MEASURED

These were the gaps in the first measurement. Each has since been fixed in the
layer-1 table and the fix verified by re-running the full comparison: all seven
rows below now report **zero** missing shapes. The table is kept because it
records what the cross-validation actually caught.

These are paths the part's **own proto already carries** and the extractor
simply does not emit. Each is a one- to four-line fix in the `switch`.

| part op type | missing | what is dropped | fix |
|---|--:|---|---|
| `CreateCdcStreamAtTable` | 4 | the stream leaf under the table, e.g. `/MyRoot/Table/Stream` | `schemeshard_path_footprint.cpp:457` emits only `CreateCdcStream.TableName`; `TCreateCdcStream.StreamDescription.Name` (`flat_scheme_op.proto:1193`) is right there |
| `AlterCdcStreamAtTable` | 3 | same, from `AlterCdcStream.StreamName` | `:469` |
| `DropCdcStreamAtTable` | 3 | same, from `DropCdcStream.StreamName[]` | `:484` |
| `RotateCdcStreamAtTable` | 8 | same, from `RotateCdcStream.OldStreamName` and `NewStream.StreamDescription.Name` | `:504` |
| `DropTableIndexAtMainTable` | 5 | the index leaf, e.g. `/MyRoot/Table/Index1` | `:407` emits only `DropIndex.TableName` and drops `DropIndex.IndexName`, which the sibling case at `:400` does use |
| `CreateColumnTable` | 3 | **everything** — the prototype reports only `/MyRoot` | `:438-441` reads `tx.GetAlterColumnTable().GetName()` for both create and alter, but `TModifyScheme.CreateColumnTable` is a *separate* field (`flat_scheme_op.proto:2047`, type `TColumnTableDescription`) and `olap/operations/create_table.cpp:882` literally returns `tx.GetCreateColumnTable().GetName()`. On a create the extractor reads an unset submessage and produces an empty name. |
| `CreateBackupCollection` | 4 (116 occurrences) | the collection's entry paths, e.g. `/MyRoot/Table1` | `:633` emits only `.Name`; `TBackupCollectionDescription.ExplicitEntryList.Entries[].Path` (`flat_scheme_op.proto:2540-2550`) is in the request |

The four `*CdcStreamAtTable` rows are one bug *shape* repeated: the `AtTable`
half of every CDC operation is modelled as touching only the parent table. That
is the single most valuable finding of this comparison, because CDC streams are
exactly what the requesting colleague's schema-CDC consumer cares about.

It is not, however, one fix applied four times. Verified against `Propose()`:

| op | resolution site | ref(s) needed |
|---|---|---|
| `CreateCdcStreamAtTable` | `..._create_cdc_stream.cpp:596`, sets `txState.CdcPathId` | `CreateCdcStream.StreamDescription.Name` |
| `AlterCdcStreamAtTable` | `..._alter_cdc_stream.cpp:404` | `AlterCdcStream.StreamName` |
| `DropCdcStreamAtTable` | `..._drop_cdc_stream.cpp:338,388`, **per stream** | `DropCdcStream.StreamName[]`, repeated |
| `RotateCdcStreamAtTable` | `..._rotate_cdc_stream.cpp:572,591` | `OldStreamName` **and** `NewStream.StreamDescription.Name`, two refs |

### Two further defects in the same table, and a hole in the test

Found while re-verifying the seven above; both confirmed in source.

- **`AlterColumnTable` misses its fallback.** `olap/operations/alter_table.cpp:278`
  reads `HasAlterColumnTable() ? AlterColumnTable.Name : AlterTable.Name`; the
  extractor emits only the first arm.
- **`TSplitChildTag` divergence.** `Alter`/`Rotate` `AtTable` resolve the table
  with `Child(tableName, TPath::TSplitChildTag{})`
  (`..._alter_cdc_stream.cpp:375`, `..._rotate_cdc_stream.cpp:543`), splitting a
  `/`-containing `TableName`. Layer 2 mirrors plain `Child` and keeps it as one
  unresolvable leaf: same `AbsPath` string, different `Exists`/`PathId`. Needs
  either a new kind or acceptance as a documented divergence.

The `CreateColumnTable` row also deserves a sharper reading than "wrong
submessage". `CreateColumnTableWithLocalIndexes` pushes the **unmodified client
tx** (`create_table_with_local_indexes.cpp:52`) and
`TCreateColumnTable::Propose` reads `Transaction.GetCreateColumnTable()`
(`olap/operations/create_table.cpp:566`, traits getter at `:882`). So this is an
audit bug the prototype **reproduced** rather than fixed, because S1 recorded the
quirk as intentional and it was carried forward unrechecked.

The most useful conclusion is about the test rather than the table:
`EveryOperationTypeIsCovered` asserts only that the result is non-empty, and an
unset field still yields one ref. It validates the *shape* of the output and
never that a value came from a field that is actually set — which is exactly how
a case that emits nothing but the working directory passed a green suite.

The `CreateColumnTable` row is the one the static tests could not have caught:
`EveryOperationTypeIsCovered` only asserts the result is non-empty, and an empty
name still yields one entry.

### (a) Implicit children — 8 missing shapes, by design (unchanged by the fixes)

| part op type | missing | note |
|---|--:|---|
| `MoveTableIndex` | 6 | `<src>/indexImplTable`, `indexImplPostingTable`, `__ydb_id_sequence`. Enumerated from the source's children at Propose time. **The extractor emits no `Implicit` marker for this op** (`:514-517`), unlike `MoveTable` (`:512`) and `MoveIndex` (`:564`) — an inconsistency worth fixing so the gap is visible. |
| `CreateTable` | 2 | `.../<table>/19700101000000Z_continuousBackupImpl` — the continuous-backup stream created beside a table. |

### (b) Runtime-derived — 24 missing shapes, cannot be static (unchanged by the fixes)

`AlterPersQueueGroup` accounts for all 24, and every one is under
`/MyRoot/.backups/collections/<name>/<timestamp>/...`. These come from the
backup-collection fan-out, where the concrete item set is read from the
collection's stored entry list rather than from the part's proto — the class the
plan predicted. One caveat on attribution: the oracle tags every `TPath` call
made during `part->Propose` to that part, so some of these may be resolutions
the enclosing `BackupIncrementalBackupCollection` driver performs; I could not
separate the two from the dump alone.

## 5. The `Implicit` marker contributes zero measured coverage

Every row's "implicit-only" count is 0. That is structural, not a bug: an
`Implicit` entry copies its anchor's resolved path (`:767-776`), so its shape is
always a shape the anchor already reported.

To be unambiguous about which version this measures: it is the **anchored**
`Implicit` implementation, the one added in S4's second edit round, where
`AnchorIndex` names the *source* for `MoveTable`/`MoveIndex` and for each
`CopyTableDescriptions[i]` rather than the last-emitted ref. The comparison
does not assume `Implicit` entries have an empty `absPath`; it normalizes
whatever path the log line carries and buckets `Implicit` entries separately.
The zero column is a consequence of anchoring, not of a stale assumption. `Implicit` entries therefore
**annotate** ("more paths exist below this one") and never **add** a path.

That is the right behaviour, but it means the answer to plan §3.4 is: the
`Implicit` marker tells a consumer where to go looking; it does not tell it what
it will find. Anything that needs the concrete set has to read `TTxState` after
Propose. It also means the four `*CdcStreamAtTable` bugs above were *not*
masked by an `Implicit` marker — those ops emit none — so they show up honestly
as missing.
