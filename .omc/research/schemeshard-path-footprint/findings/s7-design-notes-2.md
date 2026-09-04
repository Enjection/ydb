# S7h / S7i / S7j / S7k — design notes for the executors

Read-only scouting, 2026-09-03. Every `file:line` below was read in the tree at
commit `a25be8bd5a9` **plus the uncommitted S7c/S7d work** that is live in the
working tree right now. Paths are relative to `/home/innokentii/ydbwork3/ydb/`.

State of the tree as I read it (`git status --short`), because three of these
four stages build directly on it:

| file | what S7c/S7d already added |
|---|---|
| `schemeshard_path_footprint.h` | `TPathFootprint::WriteSet`, `::Published`, `::WriteSetMayBeIncomplete`, `::OriginalTxIndex`; `FormatPathFootprintWriteSetLine` |
| `schemeshard__operation.h` | `TOperation::PathFootprints` **and** `TOperation::RequestFootprints` |
| `schemeshard__operation.cpp` | `ProcessOperationParts` gained a `ui32 originalTxIndex` parameter (:96-104); `IgniteOperation` resolves one footprint per client transaction after Phase Zero (:280-301) |
| `schemeshard__operation_memory_changes.{h,cpp}` | `Mark()` / `CollectPathIdsSince` |
| `schemeshard__operation_side_effects.{h,cpp}` | `PublishedCount` / `CollectPublishedSince` |

Line numbers in `schemeshard__operation.cpp` below are from that dirty tree and
will move again; the anchors I quote are stable text, not just line numbers.

---

## S7h — `TPath` read-set recorder behind a test hook

### H1. The choke points, exactly

`ydb/core/tx/schemeshard/schemeshard_path.cpp`:

| method | line | what it does |
|---|---|---|
| `TPath::Dive(const TString&)` | :1427 | the **only** name→child step. Appends to `NameParts`, and appends to `Elements` iff `last->FindChild(name)` hits |
| `TPath::Child(name)` | :1464 | `copy; result.Dive(name)` |
| `TPath::Child(name, TSplitChildTag)` | :1470 | `SplitPath(name)` then one `Dive` per segment |
| `TPath::Resolve(TString, ss)` | :1481 | `TPath nullPrefix{ss}` then the overload below |
| `TPath::Resolve(prefix, parts)` | :1488 | one `Dive` per segment |
| `TPath::ResolveWithInactive(opId, path, ss)` | :1498 | walks earlier parts' `TTxState::TargetPathId` via `Init`, then either `headOpPath.Child(last)` or falls back to `Resolve` |
| `TPath::Init(pathId, ss)` | :1542 | id→path. Walks `ss->PathsById` up to the root and builds the element vector directly. **Does not call `Dive`** |
| `TPath::DiveByPathId(pathId)` | :2168 | private; only reached from `MaterializeImpl` (:2141-2166), i.e. path *creation*, not resolution |

So the complete instrumentation surface is **`Dive` + `Init`**. Everything
name-shaped funnels through `Dive`; everything id-shaped funnels through `Init`.
`ResolveWithInactive` needs nothing of its own: its two exits are `Child` (→
`Dive`) and `Resolve` (→ `Dive`), and its head-walk uses `Init`.

The S3 oracle (`findings/s3-dynamic-oracle.md` §1) instrumented all seven of the
outer entry points as well and found `Dive` = 86,813 lines vs `Resolve` 28,020 +
`Child` 24,593 + `Init` 5,663. It also concluded (§4 preamble) that the *outer*
call is the meaningful "touch" and `Dive` is per-segment noise. That argues for a
different split than the oracle used:

- record at `Dive` and `Init` (2 sites, complete by construction);
- have the recorder collapse per-segment noise itself, by keeping only the
  **maximal** recorded path of each `Dive` chain. Concretely: a `Dive` that
  extends the path recorded immediately before it (same `SS`, `NameParts` is a
  one-element extension) replaces that entry instead of appending. One
  `Resolve("/MyRoot/a/b/T")` then yields one entry, not four.

That collapse is a recorder-side detail, not a `TPath` change, so it stays out
of the hot path.

### H2. The hook itself — recommendation

`TPath` holds exactly one back-pointer, `TSchemeShard* SS`
(`schemeshard_path.h:22`), and `schemeshard_path.cpp` already includes
`schemeshard_impl.h` (:4). `TSchemeShard`'s member block from
`schemeshard_impl.h:222` onward is **public** (`RootPathElements` :281,
`PathsById` :284, `Operations` :336 all live there). So a member on
`TSchemeShard` costs one pointer load off an already-hot pointer.

Add to `schemeshard_path_footprint.h`:

```cpp
// Records every path resolution performed while it is installed. Installed
// only around a single part's Propose(); null in production.
class IPathResolutionObserver {
public:
    virtual ~IPathResolutionObserver() = default;
    // path is the TPath as it stands after the step. resolved==false means the
    // name chain exists but no TPathElement backs the leaf.
    virtual void OnPathResolved(const TPath& path, bool byPathId) = 0;
};
```

and to `schemeshard_impl.h`, beside `RootPathElements` (:281):

```cpp
    // Test-only observation seam for TPath resolutions. Null in production;
    // checked on the hot path of TPath::Dive/Init, so it must stay a plain
    // pointer, not a std::function.
    IPathResolutionObserver* PathResolutionObserver = nullptr;
```

Then two call sites, one per choke point:

```cpp
// TPath::Dive, at each of its four returns (schemeshard_path.cpp:1427-1462)
if (Y_UNLIKELY(SS->PathResolutionObserver)) {
    SS->PathResolutionObserver->OnPathResolved(*this, /*byPathId*/ false);
}
```

```cpp
// TPath::Init, before each of its two returns (:1542-1562)
if (Y_UNLIKELY(ss->PathResolutionObserver)) {
    ss->PathResolutionObserver->OnPathResolved(result, /*byPathId*/ true);
}
```

`Dive` has five returns (:1430, :1443, :1448, :1457, :1461). Rather
than five call sites, restructure it as a private `DiveImpl` plus a two-line
public `Dive` that calls it and then notifies once. That keeps the notification
exactly-once per `Dive` and leaves the resolution logic untouched.

**Why not a process-global.** The oracle used `TString g_FootprintTag` plus a
mutex-guarded singleton (`findings/s3-dynamic-oracle.md` §1.1) because it was a
throwaway. In-tree it is wrong twice: `TTestBasicRuntime` runs several
schemeshard tablets in one process (root + tenant schemeshards are routine in
`ut_base`/`ut_subdomain`), and unit-test binaries fork per test but share a
process across cases in the same fork. A global cannot attribute a resolution to
a schemeshard, and it needs a mutex that the member does not.

**Why not `std::function`.** `Dive` is called ~87k times per six-suite run and
sits under `TPath::Resolve`, which the whole of `Propose` is built on. A raw
pointer compare is one predictable branch; a `std::function` is an indirect call
plus a possible heap-allocated closure. Cost of the recommended form: one load
(the pointer is on the same cache line region as `PathsById`, which `Dive`
already dereferences at :1452) plus one never-taken branch.

### H3. Arming it per part

The recorder must cover **only** `part->Propose(owner, context)`. Two things
inside `ProcessOperationParts` resolve paths on their own and must not be
recorded:

- `ResolvePathFootprint(part->GetTransaction(), context.SS)` at
  `schemeshard__operation.cpp:118` — layer 2 is built entirely out of
  `TPath::Resolve`/`Child`/`Init` (`schemeshard_path_footprint.cpp:1064-1160`).
  Recording it would make the coverage assertion vacuously true.
- the `IgniteOperation` request-footprint loop
  (`schemeshard__operation.cpp:280-301`), for the same reason.

So arm strictly around the `Propose` call, with an RAII guard so an exception or
an early `return false` (the abort path at :185) cannot leave it installed:

```cpp
// in ProcessOperationParts, after the footprint is resolved and the
// MemChanges/Published marks are taken:
struct TObserverGuard {
    TSchemeShard* SS;
    ~TObserverGuard() { SS->PathResolutionObserver = nullptr; }
};

TString errStr;
if (!context.SS->CheckInFlightLimit(...)) {
    ...
} else {
    if (auto* obs = context.SS->PathResolutionObserver) { /* already null */ }
    context.SS->PathResolutionObserver = recorder;   // recorder is per-part
    TObserverGuard guard{context.SS};
    response = part->Propose(owner, context);
}
```

The "per-part context" is just: the recorder object is told, before arming, the
`(txId, partId, partOpType)` triple it is about to collect for. In the test that
is one `THashMap<TOperationId, TVector<TRecorded>>` keyed by
`part->GetOperationId()` (available before `Propose` — it is set in the
`TSubOperationBase` constructor, `schemeshard__operation_part.h:239-245`).

Where does `recorder` come from? The same channel S7i introduces:
`AppData()->PathFootprintObserver`. Give `IPathFootprintObserver` an optional
`virtual IPathResolutionObserver* PathResolutionObserver() { return nullptr; }`,
so one installed test object provides both. That way S7h adds **no** second
injection mechanism.

**Effect on production `TPath` users outside Propose.** `TPath::Init`/`Resolve`
have 536 call sites under `ydb/core/tx/schemeshard/` (18 of them in
`schemeshard__describe.cpp` + `schemeshard_impl.cpp` alone): Describe, ACL
checks, background compaction, export/import, scheme-board publication
(`DoPersistPublishPaths` at `schemeshard__operation_side_effects.cpp:626` calls
`TPath::Init`). None of them are affected, because the pointer is null except
inside the `TObserverGuard` scope, and that scope is entered only from
`ProcessOperationParts`. Describe runs in a different tablet transaction and
cannot interleave with `Propose`: both are `TTransactionBase::Execute` bodies on
the same tablet, which the executor serializes. There is no reentrancy concern —
`Propose` never re-enters `ProcessOperationParts`.

### H4. The coverage predicate, stated precisely

For one part `P` with footprint `F` (a `TPathFootprint`), define the covered set
`C(F)` over canonical absolute path strings:

1. `F.WorkingDir` after canonization (`TPath::Resolve(F.WorkingDir, ss).PathString()`,
   which layer 2 already computes as `workingDirCanon`,
   `schemeshard_path_footprint.cpp:1046`), **and every prefix of it**.
2. For every `e` in `F.Entries` with `e.AbsPath` non-empty: `e.AbsPath` **and
   every proper prefix of `e.AbsPath`**. Ancestors are covered because
   `TPath::Resolve` necessarily dives through them, and because the `TChecker`
   chain walks up for ACL/limit checks.
3. For every `e` in `F.Entries` with `e.Ref.Kind == Implicit`: every path having
   `e.AbsPath` as a **path prefix** (the anchored subtree). This is what makes
   force-drop cascades, `MoveTable`'s index/impl-table/cdc children and the
   backup-collection fan-out pass without enumerating them.
4. The domain path: `TPath::Init(F.DatabasePathId, ss).PathString()` and all its
   prefixes, plus the root path (`TPath::Root(ss).PathString()`). `GetDomainPathString`
   (`schemeshard_path.cpp:1396`) is itself an `Init`, so it *will* show up in the
   recording, from `ResolveDomainInfo`/limit checks and from
   `TSchemeShard::GetPathVersion`.
5. For every `pathId` in `F.WriteSet` and `F.Published` (S7c): the path
   `TPath::Init(pathId, ss).PathString()` and all its prefixes. Resolve these
   **after** `Propose`, because a just-materialized path only exists then.

Prefix means path-segment prefix, not string prefix: `/MyRoot/Tab` is not a
prefix of `/MyRoot/Table`. Compare `SplitPath` vectors, or compare strings with
an explicit `'/'` boundary check — `StripPrefix` in
`schemeshard_path_footprint.cpp:1052-1062` already does exactly that boundary
check and is the right thing to mirror.

The assertion is then:

> For every part `P` proposed during the test, every recorded resolution `r`
> (after the `Dive`-chain collapse of §H1) satisfies `r.PathString() ∈ C(F_P)`.

Deliberate non-goals of this predicate, which the test comment should state:

- It does **not** assert the converse (every footprint entry was resolved). A
  `Propose` that rejects early legitimately never touches its later fields; the
  `ESchemeOpInitiateBuildIndexImplTable` / `StatusResourceExhausted` trace in
  `findings/s3-dynamic-oracle.md` §4 is the canonical example.
- It does **not** distinguish "resolved because of this field" from "resolved
  incidentally". Prefix coverage is intentionally generous; the value is that a
  *new* resolution site landing on a path that is neither an ancestor, a
  declared entry, nor inside an `Implicit` anchor fails the build.

### H5. Rollout: do not turn it on for all 136 op types at once

The S7b descriptor-walk test already established the pattern to copy
(`findings/s7-round2.md`, S7b): hard-fail for a curated set, print-and-tolerate
for the rest. Do the same here:

- an `EnforcedOpTypes` set, seeded with the op types `ut_path_footprint`'s
  `TSchemeShardPathFootprintPropose` suite already drives (CreateTable,
  CreateIndexedTable, MkDir, CreateTableIndex, ...);
- everything else accumulates into an `uncovered` report printed via `Cerr`
  exactly like `ut_path_footprint.cpp:1462` does for `unclassified`.

Expect real, legitimate misses on first run in at least these shapes, all
visible in the oracle data: `ESchemeOpCreateSysView` (95,928 lines — test-harness
bootstrap noise, `/MyRoot/.sys/<name>`, its own parts so it should self-cover);
`ESchemeOpCreateCdcStreamImpl`'s PQ group beside the stream; and the
backup-collection family, whose item list is genuinely runtime-derived and is
only covered through rule 3.

---

## S7i — observer callback as the production channel

### I1. How SchemeShard receives injectable dependencies today

There is exactly one established pattern, and it is a very close analogue.

`ydb/core/base/appdata_fwd.h:211`:

```cpp
    const NSchemeShard::IOperationFactory *SchemeOperationFactory = nullptr;
```

- Interface: `ydb/core/tx/schemeshard/schemeshard_operation_factory.h:16-31` —
  a pure-virtual class plus a `DefaultOperationFactory()` free function.
- Production implementation: `TDefaultOperationFactory`,
  `schemeshard__operation.cpp:1371-1381`.
- Consumed: `schemeshard__operation.cpp:1743`,
  `AppData()->SchemeOperationFactory->MakeOperationParts(...)` — one raw pointer
  deref per call, no null check, no ownership.
- Owned in tests by `TAppPrepare::TMine::SchemeOperationFactory`
  (`ydb/core/testlib/basics/appdata.h:64`), constructed at
  `ydb/core/testlib/basics/appdata.cpp:23` and published to `TAppData` at
  `appdata.cpp:42`.

The other candidate seams are worse:

- `TSchemeShard` has a single constructor, `TSchemeShard(const TActorId&,
  TTabletStorageInfo*)` (`schemeshard_impl.h:2253`, defined
  `schemeshard_impl.cpp:5579`), and the tablet factory `CreateFlatTxSchemeShard`
  (`schemeshard.h:737`, `schemeshard.cpp:49`) just forwards those two arguments.
  No room for a dependency without changing both signatures.
- `TTestEnv` *does* accept a `TSchemeShardFactory`
  (`ut_helpers/test_env.h:107`, `std::function<IActor*(const TActorId&,
  TTabletStorageInfo*)>`, defaulted to `&CreateFlatTxSchemeShard`), so a test
  could `new TSchemeShard(...)`, poke a public member and return it. That works
  and needs zero production plumbing, but it gives production no channel at all,
  which is the whole point of report §6.2. Keep it in mind as the fallback if
  the `TAppData` edit is contentious.

### I2. The interface and where it lives

In `schemeshard_path_footprint.h`, under the `TPathFootprint` definition:

```cpp
// Production observation channel for path footprints. Registered on TAppData,
// like NSchemeShard::IOperationFactory. Called synchronously from inside the
// schemeshard's Propose transaction, on the tablet's actor thread: an
// implementation must not block, must not Send from a foreign thread, and must
// not outlive the TAppData that publishes it.
class IPathFootprintObserver {
public:
    virtual ~IPathFootprintObserver() = default;

    // One call per transaction of the client request, from IgniteOperation,
    // before any part is constructed. footprint.OriginalTxIndex is its index
    // in TEvModifySchemeTransaction.Transaction.
    virtual void OnRequestFootprint(TTxId txId, const TPathFootprint& footprint) = 0;

    // One call per constructed part, from ProcessOperationParts, after that
    // part's Propose() returned. Covers rejected parts.
    virtual void OnPartFootprint(TTxId txId, const TPathFootprint& footprint) = 0;

    // S7h: non-null to also record every TPath resolution during Propose.
    virtual IPathResolutionObserver* PathResolutionObserver() { return nullptr; }
};
```

`TTxId` is passed explicitly because `TPathFootprint` carries `PartId` and
`OriginalTxIndex` but not the tx id — the log line takes it as a parameter today
(`FormatPathFootprintLine(..., ui64 txId)`, `schemeshard_path_footprint.h:126`).
Do not add a `TxId` field to `TPathFootprint` just for this; the struct is
per-part state, the tx id is context.

Storage, in `appdata_fwd.h` next to :211:

```cpp
    NSchemeShard::IPathFootprintObserver *PathFootprintObserver = nullptr;
```

Non-const, because an observer accumulates. Forward-declare
`NSchemeShard::IPathFootprintObserver` in `appdata_fwd.h` beside the existing
`NSchemeShard::IOperationFactory` forward declaration — do **not** include
`schemeshard_path_footprint.h` from `appdata_fwd.h`; it pulls
`flat_scheme_op.pb.h`, and `appdata_fwd.h` is included nearly everywhere.

### I3. Gating

Both emission sites become:

```cpp
auto* observer = AppData()->PathFootprintObserver;
const bool wantLog = IS_CTX_LOG_PRIORITY_ENABLED(context.Ctx,
    NActors::NLog::PRI_DEBUG, NKikimrServices::FLAT_TX_SCHEMESHARD, 0ull);
if (!observer && !wantLog) {
    // ... skip ResolvePathFootprint entirely; skip the MemChanges/Published
    // marks too, they are only inputs to it
}
```

`IS_CTX_LOG_PRIORITY_ENABLED` is `ydb/library/actors/core/log.h:39-44`; the
`sampleBy` argument is mandatory in the macro, pass `0ull` (existing uses:
`ydb/core/tablet/tablet_req_rebuildhistory.cpp:353`,
`ydb/core/blobstorage/nodewarden/distconf_binding.cpp:48`).

Structure it so the skip is genuinely free: hoist the `observer`/`wantLog` pair
out of the `for (auto& part : parts)` loop in `ProcessOperationParts`
(`schemeshard__operation.cpp:114`), since neither can change mid-loop. In
`IgniteOperation` the same two locals guard the whole `RequestFootprints` loop
(:293-301).

Careful: `operation->PathFootprints` / `operation->RequestFootprints` become
empty when gated off. Anything that later reads them — including S7k's audit
consumer — must handle that. This is the one real coupling between S7i and S7k,
and it is why I recommend S7k use the *string-only* path for the common cases
(see §K3) rather than depending on a footprint that may not have been computed.

### I4. Demote the log

`LOG_NOTICE_S` → `LOG_DEBUG_S` at all five current sites:
`schemeshard__operation.cpp:143` (write-set line), :145 and :149 (per-part),
:295 and :299 (per-request). Report §6.2's argument stands: every subdomain
bootstrap creates ~20 system views, each its own part, each currently `1 + N`
NOTICE lines. The S3 oracle measured `ESchemeOpCreateSysView` at 95,928 of
172,691 recorded lines — 56% of the whole corpus was that one bootstrap shape.

### I5. Installing it from a test, and migrating `ut_path_footprint`

`TTestEnv`'s constructor order (`ut_helpers/test_env.cpp:619-818`) is:

1. build `TAppPrepare app` (:631);
2. ~70 lines of `app.*` configuration;
3. `SetupTabletServices(runtime, &app, ...)` at :757 — this is where `TAppData`
   is created and published per node;
4. **:769-772** — `runtime.GetAppData().YdbDriver = YdbDriver.Get();` — proof
   that mutating `TAppData` after `SetupTabletServices` and before boot is an
   accepted pattern in this file;
5. `BootSchemeShard(runtime, schemeRoot)` at :782;
6. `WaitForSysViewsRosterUpdate` at :789 (system-view creation happens here).

So step 4 is the seam. Add one `TTestEnvOptions` entry beside the others
(`ut_helpers/test_env.h:38-97`, all generated by the `OPTION(type, name, default)`
macro):

```cpp
        OPTION(NKikimr::NSchemeShard::IPathFootprintObserver*, PathFootprintObserver, nullptr);
```

and in `test_env.cpp`, immediately after the `YdbDriver` block:

```cpp
    if (opts.PathFootprintObserver_) {
        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
            runtime.GetAppData(node).PathFootprintObserver = opts.PathFootprintObserver_;
        }
    }
```

The per-node loop matters: `TTestActorRuntime::GetAppData(ui32 nodeIndex = 0)`
(`ydb/core/testlib/actors/test_runtime.h:118`) is per node, and multi-node
schemeshard tests exist. The `SetupSchemeCache` loop at `test_env.cpp:753-755`
is the local precedent for iterating nodes.

Placing it before `BootSchemeShard` means the ~20 `ESchemeOpCreateSysView` parts
of bootstrap **are** observed. Tests that do not want them must filter, exactly
as `ut_path_footprint.cpp:903-909` already filters `/MyRoot/.sys` prefixes out
of the MkDir list today.

**Migration of the existing tests.** `ut_path_footprint.cpp` currently observes
through a log backend: `TLogRecordCollector : TLogBackend` (:50-66), installed as
`runtime.SetLogBackend(new TLogRecordCollector(&log))` *before* `TTestEnv env(runtime)`
(:884-886), then parsed by `ParseFootprintLog` (:76-104) into
`THashMap<TString,TString>` per line, and queried by `FindLine`/`RequireLine`/
`AbsPaths`/`RequireLineByAbsPath` (:106-155).

Note the trap: `TTestEnv::SetupLogging` sets `FLAT_TX_SCHEMESHARD` to
`PRI_NOTICE` (`ut_helpers/test_env.cpp:848`), and raises it to `PRI_DEBUG`
(:852) only when the `ENABLE_SCHEMESHARD_LOG` static is set. **Demoting to
DEBUG silently empties every existing `TSchemeShardPathFootprintPropose` test.**
They will not fail loudly on the log level; they will fail on `UNIT_FAIL("no
PathFootprint line for ...")`, which is at least legible.

Migrate rather than bump the priority. Replace the backend with:

```cpp
class TFootprintCollector: public IPathFootprintObserver {
public:
    void OnRequestFootprint(TTxId txId, const TPathFootprint& f) override {
        Requests.emplace_back(txId, f);
    }
    void OnPartFootprint(TTxId txId, const TPathFootprint& f) override {
        Parts.emplace_back(txId, f);
    }
    TVector<std::pair<TTxId, TPathFootprint>> Requests;
    TVector<std::pair<TTxId, TPathFootprint>> Parts;
};
```

and rewrite `FindLine`/`RequireLine`/`AbsPaths`/`RequireLineByAbsPath` against
`TPathFootprint`/`TPathFootprintEntry` directly. This is strictly better:

- the assertions become typed (`entry.Exists` is a `bool`, not `"0"`; `PathId`
  is a `TPathId`, not a formatted string);
- `WriteSet`/`Published` become directly assertable instead of going through
  `FormatPathFootprintWriteSetLine`'s `owner:local` joining
  (`schemeshard_path_footprint.cpp:141-152`);
- the tests stop depending on the exact log text, which is the thing S7i is
  about to change anyway.

Keep `FormatPathFootprintLine` and its `TestFormat`-style coverage: it is still
the production DEBUG rendering, and one small direct unit test over it costs
nothing. Do not keep `ParseFootprintLog`.

The layer-1 suite (`TSchemeShardPathFootprintExtract`, :160-879) and the
descriptor-walk suite (:1279+) call `ExtractPathRefs` directly and need no
change at all.

---

## S7j — `ResolveWithInactive` for the `Move*` family

### J1. What `ResolveWithInactive` actually does

`schemeshard_path.cpp:1498-1540`. Given `opId`, it walks `subTxId-1` down to `0`,
and for each `headOpId` with a live `ss->FindTx(headOpId)` builds
`TPath::Init(txState->TargetPathId, ss)`. If that head path's name parts (with
`ss->RootPathElements` spliced in front, replacing the head path's own root
element) are exactly `pathParts` minus its last element, it returns
`headOpPath.Child(pathParts.back())`. Otherwise it falls through to plain
`Resolve`.

Two properties matter for us:

1. **It is safe to call from the footprint hook.** It logs via
   `LOG_DEBUG_S(TlsActivationContext->AsActorContext(), ...)` at :1518 and :1535.
   `ResolvePathFootprint` is called from `ProcessOperationParts`
   (`schemeshard__operation.cpp:118`) and from `IgniteOperation` (:294), both
   inside a tablet transaction on the actor thread, so `TlsActivationContext` is
   non-null. It is *not* safe from a pure unit test that calls
   `ResolvePathFootprint` with no actor context — which is fine, because the
   layer-1 tests never construct a `TSchemeShard`.
2. **It only ever helps.** Falling through to `Resolve` is the current behaviour,
   so the change is monotone: paths that resolve today still resolve.

### J2. Which refs should use it

Only refs the operation itself resolves that way. Grepping the whole of
`ydb/core/tx/schemeshard/` for `ResolveWithInactive` gives exactly five sites:

| site | condition | what it resolves |
|---|---|---|
| `schemeshard__operation_move_table.cpp:857` | unconditional | `MoveTable.DstPath` |
| `schemeshard__operation_move_table.cpp:904` | `if (dstParent.IsUnderOperation())` | same, re-resolved |
| `schemeshard__operation_move_sequence.cpp:894` | after a plain `Resolve` at :840 | `MoveSequence.DstPath` |
| `index/operation_move_table_index.cpp:425` | `if (dstParentPath.IsUnderOperation())`, else `Y_ABORT("NONO")` | `MoveTableIndex.DstPath` |
| `olap/operations/read_only_copy_table.cpp:502` | `if (dstParent.IsUnderOperation())` | `CreateColumnTable.Name` (**see J4**) |

`MoveIndex` (`index/operation_move_index.cpp`, dispatched at
`schemeshard__operation.cpp:1587`) does **not** call it: it is a top-level op
that expands into `MoveTableIndex` parts (`schemeshard__operation_common.cpp:1394`),
and those parts do.

So the rule is: for `Absolute` refs with `Role == Target` whose part op type is
`ESchemeOpMoveTable`, `ESchemeOpMoveTableIndex` or `ESchemeOpMoveSequence`, use
`ResolveWithInactive`. Source refs (`Move*.SrcPath`) must **not** — the source is
resolved with plain `TPath::Resolve` at
`schemeshard__operation_move_sequence.cpp:787` and the equivalent in
`move_table.cpp`, and it is meant to be the still-active original.

`ESchemeOpMoveIndex` carries `MoveIndex.SrcPath`/`.DstPath` as
`LeafUnderSibling` under `MoveIndex.TablePath`
(`schemeshard_path_footprint.cpp:745`), not `Absolute`, so it is outside the
rule — correctly, because its Propose does plain `Resolve(TablePath).Child(...)`.

### J3. The signature change

```cpp
// schemeshard_path_footprint.h
// opId: the sub-operation this footprint belongs to, when it is known. Move*
// destination paths are then resolved with TPath::ResolveWithInactive, which is
// what those Propose() implementations do, so a destination whose name was
// freed by an earlier part of the same transaction resolves instead of
// reporting exists=0. InvalidOperationId (the default) means "request-level
// footprint, no part context".
TPathFootprint ResolvePathFootprint(const NKikimrSchemeOp::TModifyScheme& tx,
                                    TSchemeShard* ss,
                                    TOperationId opId = InvalidOperationId);
```

**Do not write `TOperationId opId = {}`.** `TOperationId` derives from
`std::pair<TTxId, TSubTxId>` (`schemeshard_identificators.h:63`) and its
`explicit operator bool` (:80) is `GetTxId() != InvalidTxId && GetSubTxId() !=
InvalidSubTxId`. A value-initialized `{}` is `(TTxId(0), 0)`, which is
**truthy**. `InvalidOperationId` (:112) is the only correct sentinel, and the
guard in the resolver must be `if (opId) { ... }`.

Call sites:

- `schemeshard__operation.cpp:118` →
  `ResolvePathFootprint(part->GetTransaction(), context.SS, part->GetOperationId())`.
  `GetOperationId()` is valid before `Propose`: `TSubOperationBase` stores it in
  its constructor (`schemeshard__operation_part.h:239-256`) and it is `const`.
- `schemeshard__operation.cpp:294` (the `IgniteOperation` request loop) → leave
  as the two-argument form. There is no part yet, and the request-level footprint
  should describe the request as submitted, not as it will resolve mid-transaction.

Implementation inside `ResolvePathFootprint`
(`schemeshard_path_footprint.cpp:1128-1137`, the `case EPathRefKind::Absolute`):

```cpp
        case EPathRefKind::Absolute:
            if (ref.Value.empty()) {
                path = TPath(workingDirPath);
            } else if (opId && ref.Role == EPathRefRole::Target && isMoveOp) {
                path = TPath::ResolveWithInactive(opId, ref.Value, ss);
            } else {
                path = TPath::Resolve(ref.Value, ss);
            }
            break;
```

with `isMoveOp` computed once from `footprint.PartOpType` above the loop.

### J4. Two things to record while you are in there

- `read_only_copy_table.cpp:502` passes `dstPathStr = opDescr.GetName()` (:402) —
  a **leaf name** — to `ResolveWithInactive`, which `SplitPath`s it and treats it
  as absolute. With a one-segment `pathParts`, the head-match condition
  `headPathNameParts.size() + 1 == pathParts.size()` requires
  `headPathNameParts` to be empty, but it always starts with
  `ss->RootPathElements` (non-empty on a configured schemeshard), so the match
  can never fire and it silently degrades to `Resolve("Name")` — resolving a
  bare name from the null prefix. That looks like a real production bug, not a
  footprint concern. **Do not fix it in S7j**; note it and let it be its own
  change. It also means `ESchemeOpCreateColumnTable` must stay out of the S7j
  rule, since the operation's own use of the inactive resolver is broken.
- The S3 oracle counted all 220 `ResolveWithInactive` calls in a six-suite run as
  coming from the `Move*` family (`findings/s3-dynamic-oracle.md` §4,
  `ESchemeOpMoveTable` 159 + `ESchemeOpMoveTableIndex` 54). The observed shapes
  include `/MyRoot/<seg1>/<seg2>/indexImplTable`, i.e. the impl-table children
  resolved under the destination's would-be name. Those are `Implicit` entries in
  layer 1, so S7j does not have to reach them; it fixes the top-level `DstPath`
  `exists` flag only.

`ut_move` is the suite that exercises this (`ydb/core/tx/schemeshard/ut_move`).
A regression test belongs in `ut_path_footprint`, driving a
`ESchemeOpMoveTable` whose destination name was freed by an earlier part of the
same transaction, and asserting `exists == 1` on `MoveTable.DstPath` where it is
`0` today.

---

## S7k — rewire `ExtractChangingPaths`

### K1. What the audit path is, exactly

- `ExtractChangingPaths(const TModifyScheme&)` —
  `schemeshard_audit_log_fragment.cpp:327-745`. A 136-arm switch returning
  `TVector<TString>`; it is `static`-scoped to that translation unit (no
  declaration in the header) and has exactly one caller.
- `MakeAuditLogFragment` — `:891-908`, fills `TAuditLogFragment::Paths` from it.
  `TAuditLogFragment` is `schemeshard_audit_log_fragment.h:14-26`.
- Rendered as one audit field — `schemeshard_audit_log.cpp:122`:
  `AUDIT_PART("paths", RenderList(logEntry.Paths), !logEntry.Paths.empty())`.
- Callers of `MakeAuditLogFragment`: `schemeshard_audit_log.cpp:108`
  (`AuditLogModifySchemeOperation`) and `:186` (the deprecated common-log
  duplicate). `AuditLogModifySchemeOperation` in turn has two callers:
  `AuditLogModifySchemeTransaction` (:176) and
  `schemeshard__operation_alter_login.cpp:213`.

**Scope reducer, and it is a large one.** `AuditLogModifySchemeTransaction`
(`schemeshard_audit_log.cpp:163-180`) iterates `request.GetTransaction()` — the
**client's** transactions, not the constructed parts. So `ExtractChangingPaths`
is only ever called on op types a client can actually submit. Every derived-part
op type — `*Impl`, `*AtTable`, `MoveTableIndex`, `InitiateBuildIndexImplTable`,
`FinalizeBuildIndex*`, `CreateFullBackupOp`, `CreateLongIncrementalRestoreOp` —
is unreachable from this path, however many switch arms it has for them. That
is exactly `TOperation::RequestFootprints` (S7d), not `PathFootprints`.

`NKikimr::JoinPath` (`ydb/core/base/path.cpp:35-49`) is plain `'/'`-separated
concatenation with no absolute-path detection, which is why
`JoinPath({"/MyRoot/dir", "/MyRoot/dir/T"})` yields `/MyRoot/dir//MyRoot/dir/T`.

### K2. Which tests assert on this output — the answer is "none in C++"

I checked this specifically, because report §6.3 and plan §9 both say "behind
updated `ut_auditsettings` goldens", and that is **wrong**.

- `ydb/core/tx/schemeshard/ut_auditsettings/ut_auditsettings.cpp` tests the
  `NKikimrSubDomains::TAuditSettings` **proto field** on subdomain
  create/alter/describe. It never installs an audit log backend and never
  inspects an audit line. Grep for `audit` in it returns only
  `AuditSettingsCompare` and `AuditSettings { ... }` proto fragments. **It needs
  no golden update.**
- The C++ tests that do capture audit lines are
  `ut_export/ut_export.cpp` (:442, :3101, :3186), `ut_restore/ut_restore.cpp`
  (:4943, :5515, :5610) and `ut_login/ut_login.cpp` (includes the helper), via
  `CreateTestAuditLogBackends` (`ydb/core/testlib/audit_helpers/audit_helper.h:23`).
  Every assertion in them is on `operation=EXPORT START|END` /
  `IMPORT START|END` lines, which come from a different emitter, plus
  `component=`, `id=`, `remote_address=`, `subject=`, `database=`, `status=`,
  `detailed_status=`, `start_time=`, `end_time=`. **No `paths=` assertion
  anywhere in the C++ suites** — `grep -rn '"paths"' ydb/ --include=*.cpp` finds
  only the emitter itself and `ydb/core/audit/ut/audit_log_service_ut.cpp:195,226,239`,
  which constructs its parts literally and never calls `ExtractChangingPaths`.
- The real golden files are Python canondata:
  `ydb/tests/functional/audit/test_canonical_records.py` +
  `ydb/tests/functional/audit/canondata/result.json`. That file references
  per-test `audit_log*.json` blobs by `file://` URI (sandbox resources, not
  checked into the tree). The covered tests whose records include schemeshard
  `ModifyScheme` lines are `test_create_drop_and_alter_database` (5 blobs) and
  `test_create_drop_and_alter_table` (3 blobs). Those **are** the goldens to
  regenerate.
- `ydb/tests/functional/audit/test_auditlog.py` has no `paths` assertion.

Net: the C++ side needs no golden churn; the Python canondata for those two
canonical-record tests does, and regenerating it needs the canonical-data
workflow (`-Z`), not an edit.

### K3. Implementation — string-only for everything except `ById`

Two halves, and I recommend keeping them apart.

**Half 1 (the bulk): a pure, state-free string join over `ExtractPathRefs`.**
Add to `schemeshard_path_footprint.{h,cpp}`:

```cpp
// Joins one extracted ref into an absolute path string, using nothing but the
// request. Mirrors ResolvePathFootprint's kind switch, minus TPath: no
// schemeshard state, no canonization, no existence. Returns an empty string for
// ById and Implicit, which cannot be answered from the request alone.
TString JoinPathRef(const TString& workingDir, const TPathRef& ref);
```

with the arms mapping 1:1 onto `ResolvePathFootprint`'s switch
(`schemeshard_path_footprint.cpp:1113-1160`):

| kind | join |
|---|---|
| `LeafUnderWorkingDir` | `JoinPath({workingDir, ref.Value})` |
| `PathUnderWorkingDir` | `ref.Value.StartsWith('/') ? ref.Value : JoinPath({workingDir, ref.Value})` |
| `PathUnderWorkingDirSplit` | `JoinPath({workingDir} + SplitPath(ref.Value))` |
| `Absolute` | `ref.Value` |
| `LeafUnderSibling` | `JoinPath({<base as above>, ref.Value})`, where the base is `ref.BasePath` or, when `ref.AnchorIndex >= 0`, the already-joined string of that anchor |
| `ById`, `Implicit` | `TString()` |

`ExtractChangingPaths` then becomes roughly twenty lines:

```cpp
TVector<TString> ExtractChangingPaths(const NKikimrSchemeOp::TModifyScheme& tx) {
    TVector<TString> result;
    TVector<TString> joined;                    // parallel, for anchor lookup
    for (const auto& ref : ExtractPathRefs(tx)) {
        joined.push_back(JoinPathRef(tx.GetWorkingDir(), ref, joined));
        if (joined.back().empty()) {
            continue;                            // ById / Implicit
        }
        if (ref.Role == EPathRefRole::Target || ref.Role == EPathRefRole::Source) {
            result.push_back(joined.back());
        }
    }
    return result;
}
```

The `Target || Source` filter is deliberate and load-bearing: it keeps "changing
paths" meaning changing paths, and it keeps the diff bounded. Without it, every
`Parent` ref (`CreateCdcStream.TableName`, `DropCdcStream.TableName`,
`AlterCdcStream.TableName`, `RotateCdcStream.TableName`) and every `Dependency`
ref (replication `DstPath`/`DirectoryPath`,
`ExternalTable.DataSourcePath`, `AlterTable` column `DefaultFromSequence`,
`AlterPersQueueGroup` incremental-backup `DstPath`) starts appearing in audit
output, which is a much larger behavioural change than the eight bug fixes.
Verified against the extractor: for the CDC family the stream leaf is `Target`
and the table is `Parent` (`schemeshard_path_footprint.cpp:594-680`), which
reproduces today's audit output exactly.

**Half 2 (`ById`): decide, and I recommend deferring.**

`ExtractChangingPaths` is pure and takes only a `TModifyScheme`. Resolving an id
needs `TSchemeShard`. The call site does have one:
`AuditLogModifySchemeTransaction(record, Response->Record, Self, PeerName,
UserSID, SanitizedToken)` at `schemeshard__operation.cpp:491`, inside
`TTxOperationPropose::Complete`, and `Self` is the `TSchemeShard*`. So it is
*possible*: thread `TSchemeShard*` (or the already-computed
`TOperation::RequestFootprints`, reachable as
`Self->Operations[txId]->RequestFootprints`) through
`AuditLogModifySchemeOperation` → `MakeAuditLogFragment` → `ExtractChangingPaths`.

Three reasons not to, in this stage:

1. `Self->Operations` is erased at `ApplyOnExecute` when all parts finish at
   Propose (`schemeshard__operation.cpp:333` comment), so by `Complete` the
   `TOperation` may be gone. You would have to snapshot the footprints into the
   `TTxOperationPropose` object during `Execute`. Doable, but it is a second
   lifetime concern layered onto an audit change.
2. S7i makes footprint computation conditional on an observer or DEBUG logging.
   An audit consumer that reads `RequestFootprints` would silently lose its
   `ById` paths in the default production configuration — or would force the
   footprint to be computed unconditionally, undoing S7i.
3. `ById` requests are rare in practice. The S3 oracle found 1 of 415 accepted
   `DropTable` parts used the id branch (`findings/s3-dynamic-oracle.md` §4).

**Recommendation: keep `ById` producing no path entry**, and say so in a comment.
That is already a strict improvement over today, where an id-addressed request
logs `JoinPath(WorkingDir, "")` — a bare working dir masquerading as the target.
An empty `Paths` vector makes `AUDIT_PART("paths", ...)` omit the field entirely
(`schemeshard_audit_log.cpp:122` passes `!logEntry.Paths.empty()` as the
condition), which is honest. If someone wants the resolved path later, it is
`TSchemeShard*` + `TPath::Init` at the audit call site and it can be its own
change.

### K4. Old vs new, per affected family

Working dirs below are taken from real tests, so they are not invented:
`/MyRoot/.metadata/workload_manager/pools` from
`ut_resource_pool/ut_resource_pool.cpp:12-21`, `/MyRoot` from
`ut_streaming_query/ut_streaming_query.cpp:12`.

| # | family | representative request | audit today | audit after |
|---|---|---|---|---|
| 1 | `CreateResourcePool`, `AlterResourcePool` (`fragment.cpp:648,654`) | WorkingDir `/MyRoot/.metadata/workload_manager/pools`, `CreateResourcePool.Name = "MyResourcePool"` | `MyResourcePool` | `/MyRoot/.metadata/workload_manager/pools/MyResourcePool` |
| 2 | `DropResourcePool` (`:651`) | same WorkingDir, `Drop.Name = "MyResourcePool"` | `MyResourcePool` | `/MyRoot/.metadata/workload_manager/pools/MyResourcePool`; with `Drop.Id` set instead: today `""` → **field omitted** |
| 3 | `CreateStreamingQuery`, `AlterStreamingQuery` (`:726,732`) | WorkingDir `/MyRoot`, `CreateStreamingQuery.Name = "MyStreamingQuery"` | `MyStreamingQuery` | `/MyRoot/MyStreamingQuery` |
| 4 | `DropStreamingQuery` (`:729`) | WorkingDir `/MyRoot`, `Drop.Name = "MyStreamingQuery"` | `MyStreamingQuery` | `/MyRoot/MyStreamingQuery`; id branch → field omitted |
| 5 | `TruncateTable` (`:735`) | WorkingDir `/MyRoot`, `TruncateTable.TableName = "Table"` | `Table` | `/MyRoot/Table` |
| 6 | `SplitMergeTablePartitions` (`:358`) | WorkingDir `/MyRoot`, `TablePath = "/MyRoot/Table"` | `/MyRoot//MyRoot/Table` (double join; `JoinPath` does no absolute detection) | `/MyRoot/Table`. With `TableOwnerId`/`TableLocalId` instead: today `/MyRoot/` → field omitted |
| 7 | `AlterSequence` (`fragment.cpp:576`, empty body) | WorkingDir `/MyRoot`, `Sequence.Name = "seq"` | *(no `paths` field)* | `/MyRoot/seq` |
| 8 | `AlterReplication` / `AlterTransfer` (`:585`, empty body) | WorkingDir `/MyRoot`, `AlterReplication.Name = "repl"` | *(no `paths` field)* | `/MyRoot/repl`; with `AlterReplication.PathId` → still no field (`ById`) |
| 9 | the `TDrop` id bypass, ~22 op types sharing `genericDrop` (`schemeshard_path_footprint.cpp:209-218`) | `DropTable` with `Drop.Id = 36`, `WorkingDir = "/MyRoot"`, `Drop.Name` empty | `/MyRoot/` | field omitted |
| 10 | the `Alter*` id bypass: `AlterTable.PathId`/`Id_Deprecated`, `AlterPersQueueGroup.PathId`, `AlterBlockStoreVolume.PathId` | `AlterTable` by `PathId`, name empty | `/MyRoot/` | field omitted |

Rows 9 and 10 are the same bug in two switch shapes; s1 §3 counts them as one
"`TDrop`-family id bypass" plus the analogous `Alter*` branches, which is how the
"8 families" number in the plan is reached. Unchanged families that I explicitly
verified reproduce byte-for-byte under the new implementation: `MkDir`,
`CreateTable`, `DropTable` (by name), `AlterTable` (by name), `ModifyACL`,
`MoveTable`/`MoveSequence` (`Absolute` refs, no join, as today),
`MoveIndex` (`LeafUnderSibling` under `TablePath` → `JoinPath({TablePath, Src})`
and `JoinPath({TablePath, Dst})`, identical to `fragment.cpp:601-604`),
`CreateConsistentCopyTables` (today `DstPath` only; new adds `SrcPath` because it
is `Role::Source` — **this one does change**, see the caveat below), the whole
CDC family, `CreateIndexBuild`/`ApplyIndexBuild`/`CancelIndexBuild`,
`CreateColumnBuild`/`DropColumnBuild`, and the backup-collection family.

Caveat to decide before implementing: including `Role::Source` adds
`CreateConsistentCopyTables.CopyTableDescriptions[i].SrcPath`,
`CreateTable.CopyFromTable`, `MoveTable.SrcPath` (already present today),
`RotateCdcStream.OldStreamName` (already present) and
`RestoreMultipleIncrementalBackups.SrcTablePaths` (already present). Only the
first two are genuinely new. Both are arguably correct for an audit record — a
copy reads the source — but if the goal is a minimal diff, restrict the filter to
`Role::Target` and note the three source paths that today's implementation
already emits as pre-existing behaviour to preserve by an explicit exception.
**My recommendation: `Target || Source`, and accept the two new source entries.**

### K5. What must change in tests

- **Nothing in `ut_auditsettings`.** See K2.
- **Nothing in `ut_export` / `ut_restore` / `ut_login`.** Their audit assertions
  are on export/import lines and never on `paths=`.
- **`ydb/tests/functional/audit/canondata`** for
  `test_canonical_records.test_create_drop_and_alter_database` (5 audit_log blobs)
  and `test_canonical_records.test_create_drop_and_alter_table` (3 blobs) —
  regenerate. Those exercise `CREATE/ALTER/DROP TABLE` and database ops, whose
  audit paths are all in the unchanged set above, so the diff may well turn out
  empty; confirm rather than assume, because the tests also touch column
  statistics (`extra_feature_flags=['enable_column_statistics']`,
  `test_canonical_records.py:44`).
- **New coverage in `ut_path_footprint`**: a pure `JoinPathRef` suite asserting
  the ten rows of K4 directly against `ExtractPathRefs` output, with no
  `TTestEnv` at all. That is the cheapest place to pin the fixed strings, and it
  keeps `ExtractChangingPaths` itself untested-but-trivial (a filter over two
  already-tested functions).

---

## Cross-stage ordering

S7i and S7j are independent of each other. S7h depends on S7i only for the
injection mechanism (§H3), so do S7i first even though the plan lists S7h first.
S7k depends on nothing here — its only coupling to S7i is the reason it should
*not* consume `RequestFootprints` (§K3). Suggested order: **S7i → S7h → S7j → S7k**.
