# SchemeShard path footprint — research result

Branch `feat/schemeshard-path-footprint` (off `main` @ `66947081430`),
**uncommitted working tree**. Research session:
`.omc/research/schemeshard-path-footprint/`.

> **Measured state.** All numbers below were verified against, and only
> against, this manifest. Each run was hash-gated: the tree was hashed
> immediately before and immediately after, and results are published only
> where both agreed.
>
> ```
> 190f840105f9efa291bfca4c2895418c  schemeshard_path_footprint.cpp      (913 lines)
> dfe5b9689ac60cca2fcdf231d1c08cae  schemeshard_path_footprint.h
> 1048ae79992ad586b508f86ac413159f  ut_path_footprint/ut_path_footprint.cpp (936 lines)
> 8de4d10331ff9b3ada47553b5326b44c  ut_path_footprint/ya.make
> 43d71b9bad049836653d7a436efc6a80  schemeshard__operation.cpp
> d0095b46cd08ea4d5f70b2f909d6798f  schemeshard__operation.h
> a881d3541fece66bc605cc34e0a8186c  ya.make
> ```
>
> This is the **post-fix** prototype: the nine layer-1 table bugs found by the
> first round of cross-validation have been fixed and re-measured. The earlier
> pre-fix measurement is preserved for comparison in
> `findings/s5-raw/coverage.v1-prefix.json` and
> `findings/s5-raw/pathfootprint.v1-prefix.lines`. `findings/s5-verification.md`
> §5 records why two separate measurement rounds were needed.

**Bottom line.** A per-part hook in `ProcessOperationParts` plus two new files
gives you a normalized, proto-attributed list of the paths every scheme
operation intends to touch — accepted or rejected — for **23 added lines in 3
existing files**. Measured against a dynamic `TPath` trace of 561 tests, it
reproduces **430 of 462 non-noise path shapes**. The 32 remaining gaps fall in
3 of 67 part operation types and are all structural — implicit children and
runtime-derived path sets. Every gap that was a bug in the per-operation table
is closed.

Cross-validation earned its keep here: measuring the first prototype against
the oracle found nine defects that a green unit suite did not, they were fixed,
and re-measurement closed 30 of the 62 gaps. §3 records the build and
coordination problems along the way.

---

## 1. Recommendation: the design and the hook point

Three layers, each independently testable.

**Layer 1 — `ExtractPathRefs(const TModifyScheme&) -> TVector<TPathRef>`**, pure,
no `TSchemeShard*`. One `switch` over `EOperationType` with 136 case labels and
**no `default:`**, so a new enum value is a `-Wswitch` compile error. `TPathRef`
carries the protobuf field path
(`"CreateConsistentCopyTables.CopyTableDescriptions[1].IndexImplTableCdcStreams[idx/indexImplTable].StreamDescription.Name"`),
the raw value, a `Kind` (`LeafUnderWorkingDir`, `PathUnderWorkingDir`,
`Absolute`, `LeafUnderSibling`, `ById`, `Implicit`), a `Role`, and an
`OwnerId`/`LocalPathId` pair for id-addressed operations.

Keep the hand-written switch. The plan's table-plus-reflection-walker
alternative is *more* code, not less: the walker must handle repeated fields,
map keys, nested submessages and the id-or-name branches, and it forfeits the
compiler's completeness check. 136 case labels resolve to about 8 distinct code
paths.

**Layer 2 — `ResolvePathFootprint(tx, ss) -> TPathFootprint`**, normalization
through `TPath` only: `Resolve`, `Child`, `Init`, `PathString`, `Parent`,
`LeafName`, `FirstExistedParent`, `GetPathIdForDomain`, `GetDomainPathString`.
Nothing new was needed. It produces every field §0 asked for — `Id`, `AbsPath`,
`RelPathToParent`, `RelPathToDatabase`, `RelPathToWorkingDir`,
`WorkingDirRelToDb`, `ProtoRef` — plus `Exists` and the nearest existing parent
for paths that do not exist yet.

**Layer 3 — hook at H2, the per-part loop.** `TSchemeShard::ProcessOperationParts`
is the one place that sees, together: every part including the `MkDir`s
`SplitIntoTransactions` generates, the derived protos of compound operations,
and each part's Propose status. Two properties make it the right choice, and
both were confirmed empirically rather than assumed:

- **Derived part protos arrive already normalized.** Compound operations build
  their parts with an absolute `WorkingDir` and a leaf `Name`, so layer 2 is
  nearly trivial for them. `CreateIndexedTable` yields `CreateTable /MyRoot/Table`,
  `CreateTableIndex /MyRoot/Table/byValue`,
  `CreateTable /MyRoot/Table/byValue/indexImplTable`.
- **Rejected parts are covered.** The footprint is computed before `Propose` and
  recorded after it, so a `CreateTable` into a missing directory still reports
  `absPath /MyRoot/NoSuchDir/Table`, `exists 0`,
  `proposeStatus StatusPathDoesNotExist`.

H1 (the original request) has exact client-field attribution but cannot see
generated `MkDir`s or compound children. H3 (the factory seam) has no Propose
status. H4 (a `TPath` recorder) was built as the S3 oracle and is what §4
measures against; it is role-less and noisy and is not a product.

**Why this is the simplest thing that meets §0.** No `Propose()` implementation
was touched, none of the 233 `TPath::Resolve/Child` call sites, no `.proto`
(which would relink ~660 test modules). Storage is a `TVector<TPathFootprint>`
on `TOperation` — in memory, same lifetime, nothing persisted.

## 2. Diff against `main`

```
 ydb/core/tx/schemeshard/schemeshard__operation.cpp | 17 +++++++++++++++++
 ydb/core/tx/schemeshard/schemeshard__operation.h   |  5 +++++
 ydb/core/tx/schemeshard/ya.make                    |  1 +
 3 files changed, 23 insertions(+)
```

23 added lines, **0 deleted, 0 modified**. Every one:

| file:line | what |
|---|---|
| `schemeshard__operation.h:4` | `#include "schemeshard_path_footprint.h"` |
| `schemeshard__operation.h:18-21` | `TVector<TPathFootprint> PathFootprints;` on `TOperation`, plus its comment |
| `schemeshard__operation.cpp:114-117` | first statement of the `for (auto& part : parts)` body: `auto footprint = ResolvePathFootprint(part->GetTransaction(), context.SS);` plus its comment |
| `schemeshard__operation.cpp:129-139` | after `Y_ABORT_UNLESS(response)` and before the accept/reject classification: stamp `ProposeStatus` and `PartId`, log one line per entry, push onto `operation->PathFootprints` |
| `ya.make:248` | `schemeshard_path_footprint.cpp` |

New files, nothing else in the tree references them:

| file | lines |
|---|--:|
| `schemeshard_path_footprint.h` | 99 |
| `schemeshard_path_footprint.cpp` | 913 |
| `ut_path_footprint/ut_path_footprint.cpp` | 936 |
| `ut_path_footprint/ya.make` | 23 |

`ExtractChangingPaths` was deliberately **not** rewired — the new extractor
fixes eight audit-log bugs S1 confirmed, so a naive parity assert would fail on
exactly the cases worth fixing.

## 3. Test evidence

All three runs hash-gated against the manifest at the top.

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_path_footprint
{"type": "summary", "ts": 1788395321.99, "exit_code": 0, "tests": {"OK": 31}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_auditsettings
{"type": "summary", "ts": 1788394859.295, "exit_code": 0, "tests": {"OK": 5}}

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_base \
  ydb/core/tx/schemeshard/ut_cdc_stream ydb/core/tx/schemeshard/ut_move \
  ydb/core/tx/schemeshard/ut_consistent_copy_tables \
  ydb/core/tx/schemeshard/ut_index_build ydb/core/tx/schemeshard/ut_backup_collection
{"type": "summary", "ts": 1788395120.135, "exit_code": 0, "tests": {"OK": 561}}
```

561 is exactly the count S3 measured on uninstrumented `main`, so the hook
changes no test outcome. `ut_auditsettings` passing is a weak signal by
construction: audit still runs through the old switch, so it cannot detect a
divergence between old and new extractors.

### What it took to get these three lines

Worth knowing before you rely on the branch.

- The prototype **did not compile** when first handed over: 20 errors from a
  variable declared under an unbraced `case` label
  (`const int moveSrcIndex = out.Last();`), which makes every later case label
  an illegal jump. Add this file to a `-Werror` build in CI.
- The tree was edited **four times** after being declared frozen, twice while a
  measurement was in flight. One intermediate state ran 30 OK / 1 FAILED. The
  numbers above come from a state that was hashed before and after each run and
  agreed both times; that gate is the only reason they can be trusted.
- **Do not accept a summary line as evidence for this suite.** Two separate
  green claims in this session did not survive an independent run.

## 4. Measured coverage

85,686 `PathFootprint` log lines were harvested from the six suites and compared
against S3's per-part-op-type shapes, both normalized with the **same** function
(the comparison script imports S3's normalizer verbatim). Full table in
`findings/s5-coverage.md`.

| | before fixes | after fixes |
|---|--:|--:|
| part op types observed by the oracle | 68 | 68 |
| part op types emitting footprints | 68 | 68 |
| oracle path shapes, minus noise | 756 | 756 |
| covered by a footprint entry | 399 | **430** |
| parent-walk intermediates, not real touches | 295 | 294 |
| **real gaps** | 62 | **32** |
| part op types with gaps | 10 | **3** |

The fixes closed every table bug and nothing else moved:

| part op type | gaps before | after |
|---|--:|--:|
| `RotateCdcStreamAtTable` | 8 | 0 |
| `DropTableIndexAtMainTable` | 5 | 0 |
| `CreateCdcStreamAtTable` | 4 | 0 |
| `CreateBackupCollection` | 4 | 0 |
| `AlterCdcStreamAtTable` | 3 | 0 |
| `DropCdcStreamAtTable` | 3 | 0 |
| `CreateColumnTable` | 3 | 0 |
| `AlterPersQueueGroup` | 24 | 24 |
| `MoveTableIndex` | 6 | 6 |
| `CreateTable` | 2 | 2 |

**The 32 that remain are structural, not defects.**

- **(a) implicit child — 8 shapes.** `MoveTableIndex`'s impl tables, posting
  tables and sequences, and the continuous-backup stream created beside a table.
  Enumerated from `TPathElement::GetChildren()` at Propose time.
- **(b) runtime-derived — 24 shapes.** All `AlterPersQueueGroup`, under
  `.backups/collections/<name>/<timestamp>/...`, where the item set comes from
  the collection's stored entry list rather than the request. One caveat on
  attribution: the oracle tags every `TPath` call made during `part->Propose` to
  that part, so some may belong to the enclosing backup-collection driver.

### What cross-validation caught that the unit tests did not

This is the part worth generalizing. The prototype's own suite was green while
nine defects were live, because `EveryOperationTypeIsCovered` asserts only that
the output is **non-empty** — an unset proto field still yields one ref. It
validates the *shape* of the result, never that a value came from a field that
is set. `CreateColumnTable` read `AlterColumnTable.Name` and emitted nothing but
the working directory, and the suite passed.

Two of the nine were found only by re-verifying the other seven against
`Propose()`, and one, `CreateColumnTable`, was an audit bug the prototype
*reproduced* while its own notes claimed that class was fixed — S1 had recorded
the quirk as intentional and it was carried forward unrechecked.

One structural result unchanged by the fixes: **`Implicit` entries contribute
zero measured coverage.** An `Implicit` entry copies its anchor's resolved path,
so its shape is always one the anchor already reported. The marker tells a
consumer where to look; it never tells it what is there.

Caveat: S3's shapes are heuristic placeholders derived from test-fixture naming,
so the same structural path can normalize two ways. Every non-zero gap row was
checked by hand against concrete examples and the extractor source.

## 5. Decisions still needed from you (plan §3)

1. **Granularity.** *Recommended: level 1 — request-named paths plus generated
   parts and auto-`MkDir`s — with level 2 reported as `Implicit` markers.* The
   measurement supports this: the full cascaded subtree is never named in the
   triggering request for any operation in the tree, it is uniformly
   `TPathElement::GetChildren()` state. Promising it would mean a post-Propose
   `TTxState` layer.
2. **Is per-part attribution acceptable as "map back to the protobuf"?**
   *Recommended: yes.* At H2 a `FieldPath` points at a field of the derived
   part's proto, which is correct and useful but is not a field the client ever
   wrote. Requiring a client field is impossible for compound children — they
   are not in the request. If you want it anyway, pair H2 with H1 and join by
   originating transaction index (§6, item 5).
3. **Rejected proposals — is "resolved as far as possible" enough?**
   *Recommended: yes.* The prototype reports the intended leaf, the nearest
   existing ancestor's id, and the rejection status. S3 found rejections with no
   `TPath` resolution at all (a quota check firing first); those produce a
   footprint entry with the status and no paths, which is the honest answer.
4. **Runtime-derived operations — `Implicit` entries or leave them out?**
   *Recommended: keep the `Implicit` entries.* They cost one entry and make the
   gap visible in the output instead of silent. Given §4, be explicit with
   consumers that an `Implicit` entry is an anchor, not a path set.

## 6. Follow-ups, in priority order

1. **Finish the completeness test.** `EveryOperationTypeIsCovered` still asserts
   only a non-empty result, which is how nine defects survived a green suite.
   S6 added `OpVerbMatchesTheSubmessageItReads`, which closes the specific hole
   the `CreateColumnTable` bug went through — it fails when a `Create*` op reads
   an `Alter*` submessage, or vice versa, outside a four-entry allowlist. That
   is a proxy, not the real check. The real check is still to populate a
   distinctive value per op type and assert it surfaces in the output.
2. **Replace the log with an observer callback.** The prototype logs at
   `LOG_NOTICE`, a normal production level, and turns one line per part into
   `1 + N`. The worst case is not the interesting operations: every subdomain
   bootstrap creates ~20 system views, each now a part with its own line. Demote
   to `DEBUG` and make the callback the production channel. Gate
   `ResolvePathFootprint` itself on the callback being registered — it currently
   runs unconditionally on the Propose path.
3. **Rewire `ExtractChangingPaths`** onto layer 1 behind updated
   `ut_auditsettings` goldens, fixing the 8 audit bugs S1 found. The string half
   of layer 2 keeps audit state-free; id-addressed cases need the
   `TPathFootprint` the hook already computed, which is in scope at the audit
   call site.
4. **Pass `part->GetOperationId()` into `ResolvePathFootprint`** so the `Move*`
   family can use `TPath::ResolveWithInactive`. Today a move whose destination
   name was freed by an earlier part of the same transaction reports `exists 0`
   where `Propose` sees a resolved inactive path. One extra argument.
5. **Join H1 and H2 by originating transaction index** if question 2 above is
   answered "must be a client field". About 10 lines in `IgniteOperation`.
6. **Correctness and performance cleanups** — full list with line numbers in
   `findings/s5-review.md` §D, written against the pre-fix file, so re-check the
   line numbers. **All three of the cleanups that mattered are done** (S6 §2):
   `Absolute` no longer joins `WorkingDir` and has a regression test;
   `RelPathToWorkingDir` now strips against the canonized working dir rather
   than the raw proto string; and the working-directory `TPath` plus the domain
   path string are resolved once per footprint. Still open from that list, both
   interface decisions rather than defects: `RelPathToDatabase` falls back to
   the absolute path when nothing resolves, and `CreateFullBackupOp` uses a
   `<WorkingDir>` sentinel field path instead of a real kind. No `Y_ABORT` is
   reachable in `ResolvePathFootprint`, including with hostile input — checked
   accessor by accessor.

7. **Decide the `TSplitChildTag` divergence.** RESOLVED in S6 by the new
   `PathUnderWorkingDirSplit` kind; the note below records what it was. `Alter`/`Rotate` `AtTable`
   resolve their table with `Child(tableName, TPath::TSplitChildTag{})`, so a
   `/`-containing `TableName` is split; layer 2 mirrors plain `Child` and keeps
   it as one unresolvable leaf. Same `AbsPath` string, different `Exists` and
   `PathId`. Needs either a new kind or acceptance as documented.

## 7. How to run and inspect the branch

```bash
cd /home/innokentii/ydbwork3/ydb
git status --short          # 3 modified, 3 untracked; nothing is committed

hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_path_footprint
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_auditsettings

# see real footprints from any schemeshard suite (test-results is a symlink,
# so -R, not -r)
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_cdc_stream
grep -Rh "PathFootprint txId#" ydb/core/tx/schemeshard/ut_cdc_stream/test-results | head
```

One line per footprint entry, prefix `PathFootprint`, component
`FLAT_TX_SCHEMESHARD` at NOTICE:

```
PathFootprint txId# 281474976710658, partId# 0, partOpType# ESchemeOpCreateSysView,
proposeStatus# StatusAccepted, workingDir# /MyRoot/.sys, workingDirRelToDb# .sys,
fieldPath# CreateSysView.Name, kind# LeafUnderWorkingDir, role# Target,
absPath# /MyRoot/.sys/auth_permissions, pathId# <Invalid>, exists# 0,
relToParent# auth_permissions, relToDb# .sys/auth_permissions,
relToWorkingDir# auth_permissions
```

A test that installs its own log backend will not show these on stderr — the
`ut_path_footprint` propose tests capture them with a record-collecting
`TLogBackend` instead.

Supporting documents: `findings/s1-field-inventory.md` (per-op field table),
`findings/s2-derived-parts.md` (derived-part attribution rule),
`findings/s3-dynamic-oracle.md` (the `TPath` trace and its raw data),
`findings/s4-prototype.md` (prototype notes), `findings/s5-verification.md`,
`findings/s5-coverage.md`, `findings/s5-review.md`.

## 8. Post-fix status (S6)

All eight table bugs from §4 class (c) are fixed, each verified against the real
`Propose()`/factory before editing; details and line numbers in
`findings/s6-fixes.md`. The three layer-2 cleanups are in too: `Absolute` refs
get their own `TPath::Resolve` with no `WorkingDir` join, `RelPathToWorkingDir`
strips against the canonized working dir, and the working-dir `TPath` plus the
domain path string are resolved once per footprint rather than per entry.

The diff against `main` in the three pre-existing files is **unchanged** — still
23 added lines, 0 deleted, 0 modified. Nothing was committed or staged.

`ut_path_footprint` grew from 21 to 31 tests: one pure `ExtractPathRefs` test
per fix asserting exact field paths and kinds, a layer-2 propose test that a
slash-less backup-collection entry resolves to `/Table1` and not under the
working dir, and `OpVerbMatchesTheSubmessageItReads`, which fails when an op
reads a submessage with the wrong verb.

```
ut_path_footprint  {"exit_code": 0, "tests": {"OK": 31}}
ut_auditsettings   {"exit_code": 0, "tests": {"OK": 5}}
ut_cdc_stream      {"exit_code": 0, "tests": {"OK": 44}}
ut_olap -F '*CreateDropStandaloneTable*'  {"exit_code": 0, "tests": {"OK": 2}}
ut_cdc_stream + ut_auditsettings + ut_backup_collection + ut_move
                   {"exit_code": 0, "tests": {"OK": 156}}
```

In the live log stream the `AtTable` CDC parts now emit their stream leaf: 165
in `CreateCdcStreamAtTable`, 36 in `AlterCdcStreamAtTable`, 27 in
`DropCdcStreamAtTable` (`ut_cdc_stream`), and 93 old-plus-new pairs in
`RotateCdcStreamAtTable` (`ut_backup_collection`). All were zero before.

Remaining gaps are only classes (a) and (b): 8 implicit children and 24
runtime-derived `AlterPersQueueGroup` shapes. Neither is statically fixable.

### Two later defects (D6, D7)

**D6 confirmed and fixed.** `ESchemeOpAlterColumnTable` now mirrors
`olap/operations/alter_table.cpp:278`, which falls back to `AlterTable.Name`
when the `AlterColumnTable` submessage is absent. The extractor previously
reported an empty leaf for that request shape.

**D7 confirmed in substance, wrong in one detail.** The `TSplitChildTag` is on
the `AtTable` **table name**, not the stream child, and only two of the four
CDC `AtTable` parts use it: Alter (`:375`) and Rotate (`:543`) split, while
Create (`:541`, plain `Child`) and Drop (`:361`, plain `Dive`) do not. It does
not change `AbsPath` — `Dive("a/b")` and the split form both render `/wd/a/b` —
but the plain form looks up a child literally named `"a/b"`, so `Exists`,
`PathId` and `LeafName` all differ.

Fixed with a new kind, `PathUnderWorkingDirSplit`
(`schemeshard_path_footprint.h:27`), resolved as
`Child(value, TPath::TSplitChildTag{})`. `PathUnderWorkingDir` could not be
reused: it treats a leading slash as absolute, whereas the split tag stays
under the working dir. **This closes the open question in §6.7**, which asked
for either a new kind or documented acceptance.

`ut_path_footprint` is now 32 tests, all green; `ut_cdc_stream` +
`ut_auditsettings` + `ut_backup_collection` + `ut_move` 156 OK, `ut_olap`
filtered 2 OK. Each `AtTable` op now emits the kind matching its own `Propose`.

One further instance is flagged but **not** changed:
`ESchemeOpDropTableIndexAtMainTable` reports `DropIndex.TableName` as
`PathUnderWorkingDir` while `index/operation_drop_index.cpp:278` uses a plain
`Dive`. It predates this work and has no measured impact. Details in
`findings/s6-fixes.md` §6.

### Post-S6 touch-up (team lead)

`ESchemeOpDropTableIndexAtMainTable` reported `DropIndex.TableName` as
`PathUnderWorkingDir`, but `index/operation_drop_index.cpp:278` resolves it
with a plain `Dive` (same class as D7). Changed to `LeafUnderWorkingDir`
(`schemeshard_path_footprint.cpp:418`) and the matching test expectation.

```
ut_path_footprint  {"type": "summary", "ts": 1788397161.842, "exit_code": 0, "tests": {"OK": 32}}
```

See also `thoughts-replay-completeness.md` (2026-09-03): path completeness vs replay completeness, 7 known one-line field misses, enforcement loop, relocation design.
