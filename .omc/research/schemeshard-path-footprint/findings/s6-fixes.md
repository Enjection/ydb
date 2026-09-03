# S6 — the cheap fixes, applied and measured

All ten items from `report.md` §6.1 and §6.6 (the three correctness/performance
cleanups) are in. **Nothing was committed and nothing was staged.** The diff
against `main` in the three pre-existing files is byte-for-byte what §2 of the
report describes: still 23 added lines in `schemeshard__operation.{h,cpp}` and
`ya.make`, still 0 deleted and 0 modified. Every change below is inside the two
new files and the new test.

Each table fix was checked against the real `Propose()` or factory code before
editing; the `file:line` of that check is in the code comment beside the fix.

---

## 1. Layer 1 — the eight table bugs

`ydb/core/tx/schemeshard/schemeshard_path_footprint.cpp`

| # | fix | new line | verified against |
|---|---|--:|---|
| a | `CreateCdcStreamAtTable` now emits the stream leaf as `LeafUnderSibling` of `CreateCdcStream.TableName` | 479-490 | `schemeshard__operation_create_cdc_stream.cpp:541,596` — `Propose` resolves `tablePath.Child(streamName)` to fill `txState.CdcPathId` |
| b | `AlterCdcStreamAtTable` emits `AlterCdcStream.StreamName` under the table | 500-509 | `schemeshard__operation_alter_cdc_stream.cpp:375,404` |
| c | `DropCdcStreamAtTable` emits every `DropCdcStream.StreamName[i]` under the table | 521-533 | `schemeshard__operation_drop_cdc_stream.cpp:361,388` — one `tablePath.Child(name)` per repeated entry |
| d | `RotateCdcStreamAtTable` emits `OldStreamName` (Source) and `NewStream.StreamDescription.Name` (Target) under the table | 550-562 | `schemeshard__operation_rotate_cdc_stream.cpp:543,572,591` |
| e | `DropTableIndexAtMainTable` emits `DropIndex.IndexName` under `DropIndex.TableName` | 414-421 | `index/operation_drop_index.cpp:278,311` |
| f | `CreateColumnTable` reads `tx.GetCreateColumnTable().GetName()`; the case is split from `AlterColumnTable` | 450-455 | `olap/operations/create_table.cpp:570,643,882` |
| g | `CreateBackupCollection` emits each `ExplicitEntryList.Entries[i].Path` as `Absolute` / `Dependency` | 695-708 | `schemeshard_impl.cpp:3920` `RegisterBackupCollectionTables()` calls `TPath::Resolve(entry.GetPath())` with **no** `WorkingDir` join, which is why the kind is `Absolute` and not `PathUnderWorkingDir` |
| h | `MoveTableIndex` emits `MoveTableIndex.<indexImplTables,sequences>`, anchored on `SrcPath` | 570-579 | `schemeshard__operation_move_tables.cpp:110` enumerates the children of the **source**, same as `MoveTable` and `MoveIndex` |

Deliberate non-changes:

- The `AtTable` table refs keep `Role::Target`, not `Parent`. The `AtTable`
  half genuinely alters the table (`PathState = EPathStateAlter`), so `Target`
  is right; the whole-operation `ESchemeOpCreateCdcStream` case keeps `Parent`.
- `ESchemeOpAlterColumnTable`'s missing `AlterTable.Name` fallback was noted
  here as out of scope in the first round. It came back as defect D6 and **is
  now fixed** — see §6.

## 2. Layer 2 — `ResolvePathFootprint`

| # | fix | new line |
|---|---|--:|
| 2a | `Absolute` has its own branch: `TPath::Resolve(ref.Value, ss)`, never joined with `WorkingDir`, even with no leading slash. `PathUnderWorkingDir` keeps the old leading-slash rule. | 877-884 |
| 2b | The working-dir `TPath` is resolved once per footprint (`workingDirPath`) and reused by every entry, instead of `TPath::Resolve(WorkingDir)` per entry | 820-827, 861-884 |
| 2c | `RelPathToWorkingDir` strips against `workingDirPath.PathString()`, the canonized form the entries' `AbsPath` is built from, instead of the raw proto string | 926-928 |
| 2d | The domain path string is computed once per footprint and reused whenever the entry's `DatabasePathId` equals the footprint's, which is every case observed | 908-916 |

On 2d: the report asked for "once per footprint" flat. A ref can in principle
resolve into another domain, so the cached string is used only when the ids
match and `GetDomainPathString()` is still called otherwise. That keeps the
saving in the common case without inventing a wrong `RelPathToDatabase`.

One build error was hit and fixed on the way: `TPath`'s copy assignment is
deleted (it declares a move assignment), so `cond ? constTPathLvalue : prvalue`
yields a `const TPath` prvalue that cannot be assigned. The two `Absolute` /
`PathUnderWorkingDir` branches use `if`/`else` with `TPath(workingDirPath)`.

## 3. Tests

`ydb/core/tx/schemeshard/ut_path_footprint/ut_path_footprint.cpp`, 667 -> 995 lines.
Line numbers below are the final ones, after the D6/D7 round in §7.

Pure `ExtractPathRefs` tests, one per fix, asserting the exact `FieldPath`
strings, kinds, roles, `BasePath`, and anchor indexes:

| test | line | fix |
|---|--:|---|
| `CreateCdcStreamAtTableReportsTheStreamLeaf` | 324 | a |
| `AlterCdcStreamAtTableReportsTheStreamLeaf` | 341 | b |
| `DropCdcStreamAtTableReportsEveryStreamLeaf` | 354 | c |
| `RotateCdcStreamAtTableReportsBothStreamLeaves` | 372 | d |
| `DropTableIndexAtMainTableReportsTheIndexLeaf` | 392 | e |
| `CreateColumnTableReadsItsOwnSubmessage` | 411 | f |
| `CreateBackupCollectionReportsExplicitEntries` | 489 | g |
| `MoveTableIndexMarksItsCascade` | 510 | h |

`CreateColumnTableReadsItsOwnSubmessage` sets **both** `CreateColumnTable.Name`
and `AlterColumnTable.Name`, to different values, and asserts the extracted
value is the create one and is non-empty.

Two more:

- **`OpVerbMatchesTheSubmessageItReads`** (line 621) is the general guard the
  report asked for. `EveryOperationTypeIsCovered` can only see that *something*
  was extracted, and an op reading the wrong submessage still yields one entry
  with an empty name — which is exactly how bug (f) survived. The new test walks
  every `EOperationType`, takes each ref's `FieldPath` prefix, and fails when a
  `Create*`/`Alter*`/`Drop*`/`Move*`/`Rotate*` operation reads a submessage with
  a different verb, unless the pair is in an explicit allowlist. The allowlist
  is four real cross-verb reads: `AlterExternalTable`, `AlterExternalDataSource`,
  `AlterResourcePool` and `AlterStreamingQuery` all carry their payload in the
  matching `Create*` submessage. Pre-fix, `ESchemeOpCreateColumnTable /
  AlterColumnTable` would have failed it.
- **`BackupCollectionEntriesAreAbsoluteNotWorkingDirRelative`** (line 926) is
  the layer-2 test for fix 2a. It creates a collection under working dir
  `/MyRoot/.backups/collections/` with two entries, `"/MyRoot/Table1"` and the
  slash-less `"Table1"`, and asserts the first resolves to `/MyRoot/Table1`
  (`exists 1`) and the second to `/Table1` (`exists 0`) rather than to
  `/MyRoot/.backups/collections/Table1`. This matches what
  `RegisterBackupCollectionTables()` itself would resolve.
  (`TestSplitTable` was tried first and does not work: its helper overwrites
  `TablePath` from its own argument and leaves `WorkingDir` empty, so it cannot
  express the case.)
- The existing `TSchemeShardPathFootprintPropose::CreateCdcStream` (line 725)
  gained three assertions that the `AtTable` part now reports
  `/MyRoot/Table/Stream` as `LeafUnderSibling`.

## 4. Test evidence

Exact summary lines, all run after the final edit:

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_path_footprint
{"type": "summary", "ts": 1788394921.556, "exit_code": 0, "tests": {"OK": 31}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_auditsettings
{"type": "summary", "ts": 1788395227.068, "exit_code": 0, "tests": {"OK": 5}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_cdc_stream
{"type": "summary", "ts": 1788395428.686, "exit_code": 0, "tests": {"OK": 44}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_olap -F '*CreateDropStandaloneTable*'
{"type": "summary", "ts": 1788394975.838, "exit_code": 0, "tests": {"OK": 2}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_cdc_stream \
  ydb/core/tx/schemeshard/ut_auditsettings ydb/core/tx/schemeshard/ut_backup_collection \
  ydb/core/tx/schemeshard/ut_move
{"type": "summary", "ts": 1788395140.631, "exit_code": 0, "tests": {"OK": 156}}
```

`ut_path_footprint` was 21 tests before, 31 now, all green. No suite lost a
test. The `-F '*CreateTable*'` filter the brief suggested for `ut_olap` was
replaced with `'*CreateDropStandaloneTable*'`, which is the pair of tests that
actually exercises a standalone `ESchemeOpCreateColumnTable` part.

### Measured effect on the real footprint stream

Harvested from the freshly re-run suites (`test-results` is a symlink, so
`grep -R`). Before the fix each of these ops emitted exactly one line, the
parent table, and **zero** stream leaves.

`ut_cdc_stream`, 6,903 `PathFootprint` lines:

| part op type | `LeafUnderSibling` stream leaves | total lines |
|---|--:|--:|
| `CreateCdcStreamAtTable` | 165 | 330 |
| `AlterCdcStreamAtTable` | 36 | 72 |
| `DropCdcStreamAtTable` | 27 | 51 |

`DropCdcStreamAtTable`'s 51 is 24 table lines plus 27 stream lines, because
some drops carry more than one `StreamName`.

`RotateCdcStreamAtTable` is not exercised by `ut_cdc_stream` at all; it appears
in `ut_backup_collection` via continuous-backup rotation, 93 parts, each now
with all three fields:

```
93 RotateCdcStream.NewStream.StreamDescription.Name
93 RotateCdcStream.OldStreamName
93 RotateCdcStream.TableName
```

A representative new line:

```
partOpType# ESchemeOpCreateCdcStreamAtTable ... fieldPath# CreateCdcStream.StreamDescription.Name,
kind# LeafUnderSibling, role# Target, absPath# /MyRoot/Table/Index/indexImplTable/Stream,
pathId# [OwnerId: 72057594046678944, LocalPathId: 39], exists# 1, relToParent# Stream,
relToDb# Table/Index/indexImplTable/Stream, relToWorkingDir# indexImplTable/Stream
```

The other four fixes, same harvest:

| fix | evidence |
|---|---|
| e | `ut_*`: 63 `DropIndex.IndexName` lines beside 63 `DropIndex.TableName` (was 0 and 63) |
| f | `ut_olap`: `fieldPath# CreateColumnTable.Name, kind# LeafUnderWorkingDir, role# Target, absPath# /MyRoot/MyDir/ColumnTable` — a real path, where the op previously reported only the working dir |
| g | `ut_backup_collection`: 195 `Entries[0].Path` lines plus 9/3/3 for indexes 1-4, e.g. `kind# Absolute, role# Dependency, absPath# /MyRoot/CoverTable` |
| h | `ut_move`: 156 `MoveTableIndex.<indexImplTables,sequences>, kind# Implicit, role# Dependency` lines, matching the 156 `SrcPath`/`DstPath` pairs |

## 5. What is still open

- Gap class **(a) implicit children**, 8 shapes. `MoveTableIndex`'s 6 are now
  *marked* rather than silent, but an `Implicit` entry still contributes zero
  concrete paths (report §4.5 is unchanged by this work). `CreateTable`'s 2
  continuous-backup-stream shapes are likewise still only reachable through
  `TTxState` after Propose.
- Gap class **(b) runtime-derived**, 24 shapes, all `AlterPersQueueGroup` under
  `.backups/collections/...`. Unchanged and not statically fixable.
- Report §6 items 2 to 5 — observer callback instead of the `LOG_NOTICE`,
  rewiring `ExtractChangingPaths`, passing `OperationId` for
  `ResolveWithInactive`, and the H1/H2 join — are untouched.
- §6.6 item 6 (`RelPathToDatabase` falling back to the absolute path when
  nothing resolves) and item 7 (the `<WorkingDir>` sentinel field path) were
  left alone: both are interface decisions rather than bugs, and the existing
  `RejectedCreateTableStillProducesFootprint` test pins the current behaviour.


## 6. D6 and D7 — the two extra defects

Both were reported by the S4 author. **D6 is confirmed and fixed. D7 is
confirmed in substance but its description was wrong in one detail**, and the
correction matters, because acting on the description as written injects a new
defect into two operations that do not have it.

### D6 — `AlterColumnTable` missing the `AlterTable.Name` fallback: confirmed

`olap/operations/alter_table.cpp:278` reads:

```cpp
const TString& name = Transaction.HasAlterColumnTable() ? Transaction.GetAlterColumnTable().GetName() : Transaction.GetAlterTable().GetName();
```

and `:291` then resolves `TPath::Resolve(WorkingDir).Dive(name)`. So when the
request carries no `AlterColumnTable` submessage the name genuinely comes from
`AlterTable.Name`, and the extractor previously reported an empty leaf. Fixed at
`schemeshard_path_footprint.cpp:456-464`, mirroring the same `Has` check. The
plain `Dive(name)` confirms `LeafUnderWorkingDir` is the right kind.

Covered by `CreateColumnTableReadsItsOwnSubmessage` (line 411), which now also
builds an `ESchemeOpAlterColumnTable` carrying only `AlterTable.Name` and
asserts the ref is `AlterTable.Name` with that value. No suite in the tree
exercises this branch, so the unit test is the only coverage.

### D7 — `TSplitChildTag`: confirmed for Alter and Rotate, refuted for the stream child and for Create and Drop

D7 said the Alter/Rotate `AtTable` `Propose` "resolves **the stream child** with
`TPath::Child(name, TSplitChildTag)` semantics". The tag is on the **table
name**, not the stream. Both stream resolutions are plain `Child`:

| op | AtTable `Propose` table resolution | stream resolution | splits? |
|---|---|---|---|
| Create | `create_cdc_stream.cpp:541` `workingDirPath.Child(tableName)` | `:596` `tablePath.Child(streamName)` | **no** |
| Alter | `alter_cdc_stream.cpp:375` `.Child(tableName, TSplitChildTag{})` | `:404` `tablePath.Child(streamName)` | **yes** |
| Drop | `drop_cdc_stream.cpp:361` `TPath::Resolve(workingDir).Dive(tableName)` | `:388` `tablePath.Child(name)` | **no** |
| Rotate | `rotate_cdc_stream.cpp:543` `.Child(tableName, TSplitChildTag{})` | `:572`, `:591` `tablePath.Child()` | **yes** |

Do not confuse these with the compound-factory lines (`:906`, `:505`, `:500`,
`:739`), which build the derived parts and follow yet another split/no-split
pattern.

**Does it affect `AbsPath`?** No. `Dive(name)` pushes `name` into `NameParts`
verbatim (`schemeshard_path.cpp:1427`) and `PathString()` is
`CanonizePath(NameParts)` (`:1338`), so `Child("a/b")` and
`Child("a/b", TSplitChildTag{})` both render `/wd/a/b`. What differs is
everything else: the plain form looks up a child literally named `"a/b"`, finds
none, and reports `Exists 0` with no `PathId`, and its `LeafName()` is `"a/b"`
rather than `"b"`, so `RelPathToParent` is wrong too.

**Fix.** A new kind, `EPathRefKind::PathUnderWorkingDirSplit`
(`schemeshard_path_footprint.h:27`), resolved in layer 2 at
`schemeshard_path_footprint.cpp:864-869` as
`workingDirPath.Child(value, TPath::TSplitChildTag{})`. `PathUnderWorkingDir`
was not reused for this: it treats a leading slash as absolute and resolves from
the root, whereas `TSplitChildTag` stays under the working dir. This also
settles the open question in `report.md` §6.7, which asked for either a new kind
or documented acceptance.

Each of the four `AtTable` cases now carries the kind matching its own
`Propose`: `Leaf` for Create (`:479`) and Drop (`:521`), `SplitChild` for Alter
(`:500`) and Rotate (`:550`). Verified in the live log after a re-run:

| part op type | table-ref kind emitted | lines |
|---|---|--:|
| `CreateCdcStreamAtTable` | `LeafUnderWorkingDir` | 177 |
| `AlterCdcStreamAtTable` | `PathUnderWorkingDirSplit` | 36 |
| `DropCdcStreamAtTable` | `LeafUnderWorkingDir` | 54 |
| `RotateCdcStreamAtTable` | `PathUnderWorkingDirSplit` | 93 |

`OnlyAlterAndRotateAtTableSplitTheTableName` (line 449) pins all four against a
multi-segment `"Table/Index/indexImplTable"`.

**Residual divergence, documented not fixed.** The stream refs are
`LeafUnderSibling` on the raw `TableName`, and that base still resolves with the
relative-or-absolute rule. A `TableName` beginning with `/` would therefore send
the stream entry to the root while `Propose` keeps it under the working dir.
Reaching it requires a client to send a leading-slash `TableName`; changing the
sibling rule would affect the genuinely-absolute bases (`MoveIndex.TablePath`,
`ApplyIndexBuild.TablePath`) and was left alone.

### One more instance found, not changed

`ESchemeOpDropTableIndexAtMainTable` reports `DropIndex.TableName` as
`PathUnderWorkingDir`, but `index/operation_drop_index.cpp:278` resolves it with
a plain `Dive(mainTableName)`. By the rule established above that should be
`LeafUnderWorkingDir`. It predates this session's work, was not part of D6 or
D7, and has no measured impact, so it is flagged here rather than changed.

### Concurrent edits to the same files

Between the S6 round and this one, another agent edited
`schemeshard_path_footprint.cpp` and `ut_path_footprint.cpp`, despite the
exclusive-ownership note. Their changes: the D6 fallback (kept as-is, it is
correct), all four `AtTable` table refs switched from `Leaf` to `Path`, and a
test `CdcStreamAtTableSplitsMultiSegmentTableName` asserting
`PathUnderWorkingDir` on `ESchemeOpCreateCdcStreamAtTable`.

That last part is wrong: Create and Drop do not split, so `Path` claims a
resolution `Propose` will not perform. Their test was replaced by
`OnlyAlterAndRotateAtTableSplitTheTableName`, which covers all four ops, and
their stale expectation in `CreateCdcStreamAtTableReportsTheStreamLeaf` was
corrected back to `LeafUnderWorkingDir` — that mismatch was the one failure in
the first build of this round (32 tests: 31 GOOD, 1 FAIL).

### Evidence

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_path_footprint
{"type": "summary", "ts": 1788396270.506, "exit_code": 0, "tests": {"OK": 32}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_cdc_stream \
  ydb/core/tx/schemeshard/ut_auditsettings ydb/core/tx/schemeshard/ut_backup_collection \
  ydb/core/tx/schemeshard/ut_move
{"type": "summary", "ts": 1788396565.646, "exit_code": 0, "tests": {"OK": 156}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_olap -F '*CreateDropStandaloneTable*'
{"type": "summary", "ts": 1788396333.043, "exit_code": 0, "tests": {"OK": 2}}
```

The §4 numbers above are from the pre-D6/D7 round; the suites were re-run after
these fixes with the same results plus one extra test.

## 7. Tree state

```
$ git status --short
 M ydb/core/tx/schemeshard/schemeshard__operation.cpp
 M ydb/core/tx/schemeshard/schemeshard__operation.h
 M ydb/core/tx/schemeshard/ya.make
?? .omc/
?? ydb/core/tx/schemeshard/schemeshard_path_footprint.cpp
?? ydb/core/tx/schemeshard/schemeshard_path_footprint.h
?? ydb/core/tx/schemeshard/ut_path_footprint/

$ git diff --stat main
 ydb/core/tx/schemeshard/schemeshard__operation.cpp | 17 +++++++++++++++++
 ydb/core/tx/schemeshard/schemeshard__operation.h   |  5 +++++
 ydb/core/tx/schemeshard/ya.make                    |  1 +
 3 files changed, 23 insertions(+)
```

Nothing committed, nothing staged; the diff in pre-existing files is untouched.
New-file sizes: `schemeshard_path_footprint.cpp` 838 -> 936,
`schemeshard_path_footprint.h` 99 -> 104 (the new kind),
`ut_path_footprint/ut_path_footprint.cpp` 667 -> 995.
