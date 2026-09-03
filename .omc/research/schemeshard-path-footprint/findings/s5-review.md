# S5 part 3 — code review of the path-footprint prototype

Reviewed at the frozen tree (md5s in `s5-verification.md` §1). No code was
changed by S5. Line numbers are of the current files.

## A. Correctness

### A1. It did not compile as delivered — `schemeshard_path_footprint.cpp:509`

See `s5-verification.md` §1. `const int moveSrcIndex = out.Last();` under an
unbraced `case ESchemeOpMoveTable:` made 20 later case labels illegal jumps. The
braces are present now. **Add `-Werror`-clean CI for this file before review**;
this class of error is invisible in a diff read.

### A2. No `Y_ABORT` is reachable in `ResolvePathFootprint`, including with hostile input

I traced every `TPath` accessor the normalizer calls against its preconditions
in `schemeshard_path.cpp`:

| accessor | precondition | guard in the prototype |
|---|---|---|
| `LeafName()` (:1944) | `Y_ABORT_UNLESS(!IsEmpty())` | `path.IsEmpty()` ternary, :807 |
| `GetPathIdForDomain()` (:1408) | `!IsEmpty()` + `Elements.size()` | `ancestor.IsResolved()`, :752 and :814 |
| `GetDomainPathString()` (:1396) | same (delegates) | same |
| `IsDeleted()` (:1578) | `Y_ABORT_UNLESS(IsResolved())` | `IsResolved() && !IsDeleted()` short-circuit, :808 |
| `Base()` | resolved | `IsResolved()`, :810 / :815 / :823 |
| `TPath::Resolve` (:1481) | `Y_ABORT_UNLESS(ss)` | `ss` is `context.SS`, never null at the hook |

`TPath::Dive` (:1427) contains no abort and never dereferences an empty
`Elements`. `TPath::Init` (:1542) returns an empty `TPath` for an unknown
`TPathId` instead of aborting. So a client that sends `Drop.Id = 2^63`, an
`AlterTable.PathId` with a foreign `OwnerId`, an empty `Name`, or a `Name` of
`"////"` produces an unresolved entry, not a crash. `Child("")` yields
`NameParts = {""}`, `PathString() == "/"` — ugly, not fatal. **This is the
single most important safety property of the design and it holds.**

### A3. `Absolute` refs are silently joined with `WorkingDir` — `:721-726`, `:787-790`

`ResolveRelativeOrAbsolute` falls back to `JoinPath({WorkingDir, value})`
whenever the value has no leading `/`. That is right for
`PathUnderWorkingDir`, but `Absolute` refs go through the same helper. Propose
does not: `schemeshard__operation_move_table.cpp:818` is
`TPath::Resolve(srcPathStr, context.SS)` with no `WorkingDir` anywhere, and
`SplitPath` (`ydb/core/base/path.cpp:10`) deliberately does **not** require a
leading `/`, so `Resolve("Table")` means `/Table`. A client sending
`MoveTable.SrcPath = "Table"` with `WorkingDir = "/MyRoot"` therefore gets
`/MyRoot/Table` from the footprint and `/Table` from Propose. Fix: give
`EPathRefKind::Absolute` its own branch that calls `TPath::Resolve(value, ss)`
unconditionally.

### A4. `RelPathToWorkingDir` compares a canonized path to a raw proto string — `:827-829`

`entry.AbsPath` comes from `PathString()`, i.e. `CanonizePath(NameParts)`.
`footprint.WorkingDir` is `tx.GetWorkingDir()` verbatim. `StripPrefix` (:728) is
a literal prefix compare, so any non-canonical `WorkingDir` — a trailing slash,
a doubled slash, no leading slash — makes `StripPrefix` fall through to its
`return abs;` and `RelPathToWorkingDir` silently becomes the **absolute** path.
Same latent issue at `:756` for `WorkingDirRelToDb`, though that one happens to
compare two canonized strings today. Fix: canonize once,
`const TString wdCanon = TPath::Resolve(WorkingDir, ss).PathString();`, and
strip against that.

### A5. `RelPathToDatabase` falls back to an absolute path — `:818-820`

When `FirstExistedParent()` resolves to nothing (a path whose root segment is
not this schemeshard's root), the else branch assigns the absolute path to a
field named "relative to database". A consumer cannot tell the two apart. Either
leave it empty or add an explicit `DatabaseResolved` flag.

### A6. `FieldPath` is not always a proto field path — `:655`

`ESchemeOpCreateFullBackupOp` emits `out.Path("<WorkingDir>", TString())`. The
§0 contract says `ProtoRef` points back at the field of the request that carried
the path; `<WorkingDir>` is a sentinel. It is defensible (the op genuinely has
no name field) but it should be a distinct kind, not a fake field path, or the
consumer has to string-match `<...>`.

### A7. Footprints are resolved against partially-applied transaction state

`ResolvePathFootprint` runs inside the `for (auto& part : parts)` loop
(`schemeshard__operation.cpp:117`), so part N sees the in-memory effects of
parts 0..N-1's `Propose`. That is correct and is what makes auto-generated
`MkDir` chains resolve sensibly. Worth stating explicitly in the header, because
it means a footprint is **not** reproducible from the request proto alone.

Two second-order consequences to document: `AbortOperationPropose` rolls the
memory changes back afterwards, so logged footprints of a rejected tx describe
state that was un-applied; and `operation->PathFootprints` is destroyed along
with the `TOperation`, so the in-memory channel is unusable for rejected
proposals (S4 already found this and it is why the log exists).

### A8. Minor: `ById` resolution happens before any ACL check

The hook resolves `Drop.Id` / `AlterTable.PathId` to an absolute path and writes
it to the log before `Propose` performs authorization. A caller who may not read
a path can make its name appear in the server log by guessing a path id, and —
once the observer callback replaces the log — can make it appear in the
consumer's stream. Low severity (the name is not returned to the caller), but
the observer contract should say the footprint is pre-authorization.

## B. Cost on the Propose hot path

`ResolvePathFootprint` is called **unconditionally** for every part
(`schemeshard__operation.cpp:117`). The two `LOG_NOTICE_S` calls at `:132` and
`:136` are level-gated by `IS_CTX_LOG_PRIORITY_ENABLED`
(`ydb/library/actors/core/log.h:74-80`), so string formatting is conditional —
but the resolution work is not.

Per part the normalizer does:

| work | where | count |
|---|---|---|
| `TPath::Resolve(WorkingDir)` | :750 | 1 |
| `TPath::Resolve(WorkingDir)` again | :784, and inside `ResolveRelativeOrAbsolute` :725 | once **per entry** |
| `FirstExistedParent()` | :751, :813 | 1 + per entry |
| `GetDomainPathString()` | :754, :817 | 1 + per entry |

`GetDomainPathString()` is the expensive one: it calls `TPath::Init(domainId)`,
which walks parent pointers to the root building a `TVector<TPathElement::TPtr>`,
then `CanonizePath`. The YDB source itself flags this —
`schemeshard_path.cpp:1396` carries the comment *"not effective because of
creating vectors in Init() method"*. Each `TPath::Resolve` is one `Dive` (one
`FindChild` hash lookup) per segment.

Concretely, for the `ut_base` stress case S3 observed (one
`TEvModifySchemeTransaction` carrying 100+ `CreateTable` sub-transactions), this
adds roughly 100 × (2 `Resolve` + 2 `FirstExistedParent` + 2
`GetDomainPathString`) — a few hundred path walks and vector allocations that
did not exist before, on the tablet's transaction thread. It is the same order
as what `Propose` itself already does, so it is a constant-factor regression,
not an algorithmic one — but it is pure overhead when nothing consumes it.

Three fixes, all easy:

1. **Hoist the working-dir `TPath`.** Resolve it once at `:750` and pass the
   `TPath` into the per-entry branches instead of re-resolving at `:784`/`:725`.
   Removes one full path walk per entry.
2. **Cache the domain path.** `dbPath` is already computed at `:754`; the
   per-entry `ancestor.GetDomainPathString()` at `:817` is the same string for
   every entry in all realistic cases. Compute per footprint, not per entry.
3. **Gate the whole thing.** Skip when no observer is registered and the log
   level is below NOTICE, e.g. an early `if (!ss->PathFootprintEnabled) return
   {};`. Right now a production cluster pays for a feature only schema-CDC wants.

### Log volume in production

`FLAT_TX_SCHEMESHARD` at NOTICE is a normal production level — the adjacent
`IgniteOperation` line (`:141`) is also NOTICE. The hook turns *one* NOTICE line
per part into *1 + N* lines, where N is the number of footprint entries (1 for
`MkDir`, 2-4 for the CDC and index families, 4+ per item for
`ConsistentCopyTables`). The worst case is not the interesting ops: S3 measured
`ESchemeOpCreateSysView` as the single largest bucket (95,928 path touches),
because every subdomain bootstrap creates ~20 system views, and each one is now
a part with its own footprint line. **Recommendation: emit at
`LOG_DEBUG`/`TRACE` and make the observer callback the production channel.** The
log is the right choice for the prototype (it is the only way to see rejected
parts) and the wrong one for production.

## C. Could it be simpler?

I agree with S4's §6 conclusions and add two.

- **Keep the hand-written switch.** The reflection-walker alternative from plan
  §1b/1a is more code and loses `-Wswitch`. One caveat S4 does not mention: the
  old `ExtractChangingPaths` uses `Y_ABORT` on an unhandled op type, which also
  catches a *runtime* enum value from a newer peer; the switch only catches
  *compile-time* additions and silently returns an empty vector at runtime. For
  a footprint that is the better failure mode, but it should be a deliberate,
  commented decision rather than an accident.
- **`EPathRefRole` is unused by the machinery.** Nothing in layer 2, the
  formatter, or the hook branches on it. It is one enum and a parameter, so the
  cost of keeping it is near zero, and the schema-CDC consumer plausibly wants
  Source vs Target. Keep it, but stop treating "which role is this" as a
  question the extractor must answer correctly for all 136 ops — it is currently
  unverified by any test.
- **`Implicit` entries earn their place.** They are the honest answer to plan
  §3.4 and the coverage measurement in `s5-coverage.md` depends on them to
  distinguish "we know we cannot see this" from "we missed it". Do not drop them
  for a per-op boolean flag.
- **`AnchorIndex` is sound but fragile.** It is an index into the
  `ExtractPathRefs` result reinterpreted as an index into `footprint.Entries`
  (`:767-776`). That only works because entries are pushed 1:1 in order. It is
  bounds-checked, so a mistake degrades to an empty anchor rather than UB, but a
  comment stating the invariant would help.

## D. Concrete change list

| # | file:line | change |
|---|---|---|
| 1 | `schemeshard_path_footprint.cpp:507` | keep the braces; add the file to a `-Werror` build in CI |
| 2 | `schemeshard_path_footprint.cpp:787-790` | give `Absolute` its own `TPath::Resolve(value, ss)` branch, no `WorkingDir` join |
| 3 | `schemeshard_path_footprint.cpp:756, 829` | canonize `WorkingDir` once and strip against the canonized form |
| 4 | `schemeshard_path_footprint.cpp:750, 784, 725` | resolve the working-dir `TPath` once per footprint and reuse it |
| 5 | `schemeshard_path_footprint.cpp:754, 817` | compute the domain path string once per footprint, not per entry |
| 6 | `schemeshard_path_footprint.cpp:818-820` | do not put an absolute path in `RelPathToDatabase`; leave it empty or add a flag |
| 7 | `schemeshard_path_footprint.cpp:655` | replace the `<WorkingDir>` sentinel field path with a real kind |
| 8 | `schemeshard__operation.cpp:117` | gate the call on an observer/flag so production does not pay for it |
| 9 | `schemeshard__operation.cpp:132, 136` | demote to `LOG_DEBUG_S`; make the observer the production channel |
| 10 | `schemeshard_path_footprint.h:88` | document that resolution reflects partially-applied tx state and is pre-authorization |
