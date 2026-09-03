# S7c / S7d / S7e / S7g — design notes for the executors

Read-only scouting, 2026-09-03. Every file:line below was read in the tree at
commit `66947081430` plus the uncommitted S7a/S7b work. Paths are relative to
`/home/innokentii/ydbwork3/ydb/`.

Note taken during scouting: `schemeshard_path_footprint.h` already gained
`const TVector<TStringBuf>& KnownPathFieldNames();` (S7b landed while this was
being written). S7e must extend that list when it adds no new fields — it adds
none, so no change there; S7f will replace it with the enum.

---

## S7c — Propose-time write set from `TMemoryChanges` + `PublishPaths`

### C1. What is in `TMemoryChanges`, and what each entry is keyed by

`ydb/core/tx/schemeshard/schemeshard__operation_memory_changes.h:18-98`. All
members are **private**; there is no accessor of any kind today.

| member | line | element type | key | path-keyed? |
|---|---|---|---|---|
| `Paths` | :20 | `pair<TPathId, TPathElement::TPtr>` | `TPathId` | yes |
| `Indexes` | :26 | `pair<TPathId, TTableIndexInfo::TPtr>` | `TPathId` | yes |
| `CdcStreams` | :26 | `pair<TPathId, TCdcStreamInfo::TPtr>` | `TPathId` | yes |
| `TablesWithSnapshots` | :29 | `pair<TPathId, TTxId>` | `TPathId` | yes |
| `LockedPaths` | :32 | `pair<TPathId, TTxId>` | `TPathId` | yes |
| `Tables` | :35 | `pair<TPathId, TTableInfo::TPtr>` | `TPathId` | yes |
| `ColumnTables` | :38 | `pair<TPathId, TColumnTableInfo::TPtr>` | `TPathId` | yes |
| `Sequences` | :41 | `pair<TPathId, TSequenceInfo::TPtr>` | `TPathId` | yes |
| `Shards` | :44 | `pair<TShardIdx, THolder<TShardInfo>>` | `TShardIdx` | **no** |
| `SubDomains` | :51 | `THashMap<TPathId, TSubDomainInfo::TPtr>` | `TPathId` | yes (a map, not a stack) |
| `TxStates` | :54 | `pair<TOperationId, THolder<TTxState>>` | `TOperationId` | **no** |
| `ExternalTables` | :57 | `pair<TPathId, ...>` | `TPathId` | yes |
| `ExternalDataSources` | :60 | `pair<TPathId, ...>` | `TPathId` | yes |
| `Views` | :63 | `pair<TPathId, ...>` | `TPathId` | yes |
| `ResourcePools` | :66 | `pair<TPathId, ...>` | `TPathId` | yes |
| `BackupCollections` | :69 | `pair<TPathId, ...>` | `TPathId` | yes |
| `SysViews` | :72 | `pair<TPathId, ...>` | `TPathId` | yes |
| `LongIncrementalRestoreOps` | :75 | `pair<TOperationId, optional<...>>` | `TOperationId` | **no** |
| `IncrementalBackups` | :78 | `pair<ui64, TIncrementalBackupInfo::TPtr>` | `ui64` | **no** |
| `FullBackups` | :82 | `pair<ui64, TFullBackupInfo::TPtr>` | `ui64` | **no** |
| `BCPathToFullBackup` | :86 | `pair<TPathId, optional<ui64>>` | `TPathId` | yes |
| `Secrets` | :89 | `pair<TPathId, ...>` | `TPathId` | yes |
| `StreamingQueries` | :92 | `pair<TPathId, ...>` | `TPathId` | yes |
| `SharedShardEntries` | :95 | `tuple<TShardIdx, TPathId, optional<TTxId>>` | pair | yes (element 1) |
| `TestShardSets` | :98 | `pair<TPathId, TTestShardSetInfo::TPtr>` | `TPathId` | yes |

`GrabNewX(...)` means "this key did not exist and the operation creates it"
(the undo action is `erase`); `GrabX(...)` means "the key existed and the
operation is about to mutate it" (the undo action is `restore the copy`). Both
implementations are the two templates at
`schemeshard__operation_memory_changes.cpp:8-17`, but nine `Grab*` functions
bypass them (`GrabColumnTable` :44, `GrabShard` :53, `GrabDomain` :60,
`GrabNewTableSnapshot` :94, `GrabNewLongLock` :99, `GrabLongLock` :104,
`GrabNewLongIncrementalRestoreOp` :158, `GrabNewSharedShard` :199,
`GrabSharedShard` :207). So *centralising a journal inside the two templates
is not sufficient* — do not take that shortcut.

### C2. Are the stacks accessible / iterable?

No, twice over.

- They are **private** members with no accessor.
- `TStack` is not iterable. `util/generic/stack.h`:

  ```cpp
  template <class T, class S>
  class TStack: public std::stack<T, S> {
      using TBase = std::stack<T, S>;
  public:
      using TBase::TBase;
      inline explicit operator bool() const noexcept { return !this->empty(); }
  };
  ```

  It inherits `size()`, `top()`, `pop()`, `empty()` from `std::stack`; the
  underlying container `c` is `protected`, so even a member of
  `TMemoryChanges` cannot walk it. Reaching `c` requires the
  "derive-and-steal-the-pointer-to-member" hack — **do not use it here**, this
  is a core SchemeShard type.

`size()` *is* public, so the "sizes before" half needs no change at all. Only
the "enumerate new entries after" half does.

### C3. Minimal accessor to add (recommended)

Two edits, both inside
`ydb/core/tx/schemeshard/schemeshard__operation_memory_changes.{h,cpp}`.

**Edit 1 — make the undo stacks iterable.** Replace `TStack<T>` with a
23-line local type placed above the class in the header. Only four operations
are used by the file (`emplace`, `top`, `pop`, `operator bool`), so the swap is
mechanical and behaviour-preserving; `UnDo()` pops from the back, and
`Items.back()` is the last pushed element, exactly as before.

```cpp
// Undo log slot: a stack for UnDo(), a forward range for observers.
template <class T>
class TUndoStack {
    TVector<T> Items;
public:
    template <class... TArgs>
    void emplace(TArgs&&... args) { Items.emplace_back(std::forward<TArgs>(args)...); }
    const T& top() const { return Items.back(); }
    void pop() { Items.pop_back(); }
    size_t size() const { return Items.size(); }
    explicit operator bool() const noexcept { return !Items.empty(); }
    auto begin() const { return Items.begin(); }
    auto end() const { return Items.end(); }
};
```

then `s/TStack</TUndoStack</` on the 24 declarations at
`schemeshard__operation_memory_changes.h:20-98` (drop the second template
argument). `TVector` supports move-only elements, so `THolder<TShardInfo>` and
`THolder<TTxState>` are fine.

**Edit 2 — a mark type plus one collector.** Public section of
`TMemoryChanges`:

```cpp
    // Number of entries on every path-keyed undo stack. Entries pushed after
    // a mark are exactly the in-memory writes of the code that ran since.
    struct TMark {
        size_t Paths = 0, Indexes = 0, CdcStreams = 0, TablesWithSnapshots = 0;
        size_t LockedPaths = 0, Tables = 0, ColumnTables = 0, Sequences = 0;
        size_t ExternalTables = 0, ExternalDataSources = 0, Views = 0;
        size_t ResourcePools = 0, BackupCollections = 0, SysViews = 0;
        size_t BCPathToFullBackup = 0, Secrets = 0, StreamingQueries = 0;
        size_t SharedShardEntries = 0, TestShardSets = 0, SubDomains = 0;
    };

    TMark Mark() const;
    // Appends, in grab order, every TPathId grabbed after `mark`. Duplicates
    // are possible (a path grabbed twice) and are the caller's problem.
    void CollectPathIdsSince(const TMark& mark, TVector<TPathId>& out) const;
```

`Mark()` is 20 assignments; `CollectPathIdsSince` is 19 loops of the shape

```cpp
    for (auto it = Paths.begin() + mark.Paths; it != Paths.end(); ++it) {
        out.push_back(it->first);
    }
```

plus `SharedShardEntries` (`std::get<1>(*it)`) and `SubDomains` (a `THashMap`;
mark its `size()` and, since `GrabDomain` is idempotent per pathId
(:65 `if (!SubDomains.contains(pathId))`), just re-emit every key whose count
grew — or simply skip `SubDomains`, since the domain is never the write target
in the sense the footprint cares about. **Recommendation: skip `SubDomains`**
and say so in a comment; it adds only the database path id, which the footprint
already carries as `DatabasePathId`.

Do **not** try to also derive path ids from `Shards`/`TxStates`: a `TShardIdx`
is not a path and the `TOperationId` in `TxStates` is the part's own id.

Optional refinement if the consumer wants create-vs-update: make the collector
emit `std::pair<TPathId, bool /*isNew*/>` using the null second element as the
"new" marker (`GrabNew*` stores `nullptr`/`std::nullopt`/`InvalidTxId`). That
is free, but it is not uniform across all stacks (`LockedPaths` uses
`InvalidTxId`, `BCPathToFullBackup` uses `nullopt`), so it costs a per-stack
predicate. Ship the plain `TVector<TPathId>` first.

### C4. `TSideEffects::PublishPaths`

`ydb/core/tx/schemeshard/schemeshard__operation_side_effects.h:47`:

```cpp
    using TPublications = THashMap<TTxId, TDeque<TPathId>>;
    ...
    TPublications PublishPaths;
    TPublications RePublishPaths; // only for UpgradeSubDomain
```

It is keyed by **`TTxId`, not `TOperationId`** —
`schemeshard__operation_side_effects.cpp:154-156`:

```cpp
void TSideEffects::PublishToSchemeBoard(TOperationId opId, TPathId pathId) {
    PublishPaths[opId.GetTxId()].push_back(pathId);
}
```

so all parts of one request share one deque, appended in part order. Pulling
"the entries added by this part" is therefore an index range, not a key lookup.
`PublishPaths` is private; add two lines to the public section:

```cpp
    size_t PublishedCount(TTxId txId) const;                       // 0 when absent
    void CollectPublishedSince(TTxId txId, size_t mark, TVector<TPathId>& out) const;
```

Versions: `TSideEffects` does not carry one. The version is computed later, at
`ApplyOnExecute` time, in `DoPersistPublishPaths`
(`schemeshard__operation_side_effects.cpp:626`):

```cpp
    const ui64 version = ss->GetPathVersion(TPath::Init(pathId, ss)).GetGeneralVersion();
```

`TSchemeShard::GetPathVersion` is declared at `schemeshard_impl.h:712`:
`NKikimrSchemeOp::TPathVersion GetPathVersion(const TPath& pathEl) const;`.
Calling it in the hook, right after the part's `Propose()`, gives the version
**as of this part**; a later part of the same request may bump it again before
`ApplyOnExecute` persists the final value. Document that in the field comment.
Do not chase the final value from the Propose hook — it does not exist yet.

Ignore `RePublishPaths` (only `UpgradeSubDomain` writes it).

### C5. `IsUndoChangesSafe()` — where it lives and how to read it per part

`ydb/core/tx/schemeshard/schemeshard__operation_part.h:153`:

```cpp
    bool IsUndoChangesSafe() const {
        return !DirectAccessGranted;
    }
```

`DirectAccessGranted` (:110) is a member of `TOperationContext`, set to `true`
by the first `GetDB()` call (:145) and **never reset**. So it is monotone and
cumulative across all parts of one `IgniteOperation`. There is already a
before/after comparison in the driver at
`schemeshard__operation.cpp:190` using the `prevProposeUndoSafe` parameter.

Record two bits, not one:

- `WriteSetMayBeIncomplete = !context.IsUndoChangesSafe()` *after* Propose —
  conservative: once anything in this request wrote directly to the DB, no
  later part's `TMemoryChanges` diff can be trusted as complete.
- `BecameUndoUnsafe = undoSafeBefore && !context.IsUndoChangesSafe()` —
  precise attribution of *which* part did it. This is exactly the condition
  `schemeshard__operation.cpp:190` already logs.

### C6. Struct additions to `TPathFootprint`

`ydb/core/tx/schemeshard/schemeshard_path_footprint.h`, inside
`struct TPathFootprint` after `TSubTxId PartId`:

```cpp
    // Filled by the ProcessOperationParts hook from the TMemoryChanges undo
    // log diffed across this part's Propose(). In-memory writes only, in grab
    // order, duplicates possible. Cascades (subtree drops, moved children)
    // appear here even though no proto field names them.
    TVector<TPathId> WriteSet;
    // Paths this part asked SchemeBoard to republish, with the general version
    // as of the end of this part's Propose(). A later part of the same request
    // may bump the version again before it is persisted.
    TVector<std::pair<TPathId, ui64>> Published;
    // The operation wrote through TOperationContext::GetDB() instead of
    // TMemoryChanges/TStorageChanges, so WriteSet is a lower bound.
    bool WriteSetMayBeIncomplete = false;
    // This part is the one that made the request undo-unsafe.
    bool BecameUndoUnsafe = false;
```

### C7. The hook (12 added lines)

In `ydb/core/tx/schemeshard/schemeshard__operation.cpp`, inside the existing
`for (auto& part : parts)` at **:113**. Marked lines are new; the rest is
context that is already there.

```cpp
    for (auto& part : parts) {
        auto footprint = ResolvePathFootprint(part->GetTransaction(), context.SS);
        const auto memMark = context.MemChanges.Mark();                          // +1
        const size_t pubMark = context.OnComplete.PublishedCount(txId);          // +2
        const bool undoSafeBefore = context.IsUndoChangesSafe();                 // +3

        TString errStr;
        if (!context.SS->CheckInFlightLimit(...)) {   // :121, unchanged
            ...
        } else {
            response = part->Propose(owner, context);                    // :124
        }

        Y_ABORT_UNLESS(response);

        footprint.ProposeStatus = response->Record.GetStatus();          // :129
        footprint.PartId = part->GetOperationId().GetSubTxId();          // :130
        context.MemChanges.CollectPathIdsSince(memMark, footprint.WriteSet);     // +4
        {                                                                        // +5
            TVector<TPathId> published;                                          // +6
            context.OnComplete.CollectPublishedSince(txId, pubMark, published);   // +7
            for (const TPathId pathId : published) {                             // +8
                footprint.Published.emplace_back(pathId, context.SS->GetPathVersion(  // +9
                    TPath::Init(pathId, context.SS)).GetGeneralVersion());        // +10
            }                                                                     // +11
        }                                                                         // +12
        footprint.WriteSetMayBeIncomplete = !context.IsUndoChangesSafe();         // +13
        footprint.BecameUndoUnsafe = undoSafeBefore && !context.IsUndoChangesSafe(); // +14
        ... existing logging at :131-138 ...
        operation->PathFootprints.push_back(std::move(footprint));       // :139
```

Two ordering facts that make this correct:

- `ResolvePathFootprint` must stay **before** `Propose()` (it resolves against
  pre-Propose state; the comment at :114-116 says so) while the marks must be
  taken at the same point, and the collection **after**.
- On the reject path the loop `return false`s at :185 after
  `AbortOperationPropose`. The recording above happens at :129-140, i.e.
  *before* that branch, so rejected parts keep their footprint. `WriteSet` for
  a rejected part is whatever it grabbed before failing, which is the honest
  answer; `AbortOperationPropose` → `context.MemChanges.UnDo` at :344 runs
  afterwards and does not touch the recorded vector.

Extend `FormatPathFootprintLine`
(`schemeshard_path_footprint.cpp:140-163`) with `writeSet#` (comma-joined path
ids) and `published#` so `ut_path_footprint`'s existing log-scraping channel
(`ut_path_footprint/ut_path_footprint.cpp:50-95`) can assert on it without a
new seam. Keep the `", "` separator convention — the parser splits on it
(:83), so join path ids with `,` and no space.

---

## S7d — H1 pass: footprint of each original request transaction

### D1. Where the request transactions are iterated

`ydb/core/tx/schemeshard/schemeshard__operation.cpp`, all inside
`TSchemeShard::IgniteOperation` (**:209**):

| line | code | note |
|---|---|---|
| :228 | `for (const auto& transaction : record.GetTransaction())` | quota only; early `return` on failure |
| :237-246 | `for (auto& transaction : *record.MutableTransaction())` | inherits `FailOnExist` into each `TModifyScheme` |
| :248 | `TVector<TTxTransaction> rewrittenTransactions;` | |
| :254 | `for (auto tx : record.GetTransaction()) {` | **Phase Zero**, by value |
| :255 | `if (DispatchOp(tx, [&](auto traits){ return traits.NeedRewrite && !Rewrite(traits, tx); }))` | `Rewrite` mutates `tx` in place; only two backup ops set `NeedRewrite` |
| :261 | `rewrittenTransactions.push_back(std::move(tx));` | **1:1 with `record.GetTransaction(i)`** — no filtering, no fan-out; the only exit is a whole-request `return` at :259 |
| :266-267 | `TVector<TTxTransaction> transactions; TVector<TTxTransaction> generatedTransactions;` | |
| :272 | `for (const auto& transaction : rewrittenTransactions)` | **Phase One** |
| :273 | `auto splitResult = operation->SplitIntoTransactions(transaction, context);` | |
| :280 | `std::move(splitResult.Transactions.begin(), ..., back_inserter(generatedTransactions));` | 0..N generated `MkDir`s |
| :281-283 | `if (splitResult.Transaction) { transactions.push_back(*splitResult.Transaction); }` | the (possibly modified) main tx |
| :288 | `Operations[txId] = operation;` | |
| :295-306 | Phase Two: `generatedTransactions` → `ConstructParts` → `ProcessOperationParts` | |
| :307-318 | Phase Three: `transactions` → `ConstructParts` → `ProcessOperationParts` | |

`TSplitTransactionsResult` (`schemeshard__operation.h:72-77`):

```cpp
    struct TSplitTransactionsResult {
        NKikimrScheme::EStatus Status = NKikimrScheme::StatusSuccess;
        TString Reason;
        TVector<TTxTransaction> Transactions;      // generated MkDirs
        std::optional<TTxTransaction> Transaction; // the main tx, possibly rewritten
    };
```

`SplitIntoTransactions` (:925) either returns `result.Transaction = tx`
unchanged (several bail-out branches, :948/:953/:970/:993/:1040/:1056) or a
**copy with `WorkingDir` deepened and the name shortened**
(:1000-1011: `TTxTransaction create(tx); create.SetWorkingDir(path.PathString());
... SetName(traits, create, name); result.Transaction = create;`), plus the
`MkDir`s built at :910-916 and reversed at :919. There is also a second,
independent generator for `traits.CreateAdditionalDirs` at :1024-1053. Both
generators only ever *append* to `result.Transactions`; `result.Transaction`
stays a single optional. So **each original transaction maps to at most one
entry in `transactions` and 0..N entries in `generatedTransactions`**, and the
mapping is recoverable by carrying the index.

### D2. Concrete code shape for `OriginalTxIndex`

Parallel index vectors, built where the transactions are (Phase One):

```cpp
    TVector<TTxTransaction> transactions;
    TVector<ui32> transactionOrigins;                                  // NEW
    TVector<TTxTransaction> generatedTransactions;
    TVector<ui32> generatedOrigins;                                    // NEW

    for (ui32 originalTxIndex = 0; originalTxIndex < rewrittenTransactions.size(); ++originalTxIndex) {
        const auto& transaction = rewrittenTransactions[originalTxIndex];
        auto splitResult = operation->SplitIntoTransactions(transaction, context);
        if (splitResult.Status != NKikimrScheme::StatusSuccess) {
            ... unchanged ...
        }
        for (auto& generated : splitResult.Transactions) {             // replaces :280
            generatedTransactions.push_back(std::move(generated));
            generatedOrigins.push_back(originalTxIndex);               // NEW
        }
        if (splitResult.Transaction) {
            transactions.push_back(*splitResult.Transaction);
            transactionOrigins.push_back(originalTxIndex);             // NEW
        }
    }
```

Phases Two and Three become indexed loops that pass the origin down:

```cpp
    for (size_t i = 0; i < generatedTransactions.size(); ++i) {
        auto parts = operation->ConstructParts(generatedTransactions[i], context);
        operation->PreparedParts += parts.size();
        if (!ProcessOperationParts(parts, txId, record, prevProposeUndoSafe,
                generatedOrigins[i], operation, response, context)) {   // + one arg
            return response;
        }
    }
```

`ProcessOperationParts` gains one parameter, `ui32 originalTxIndex`, in both
the definition (`schemeshard__operation.cpp:96-103`) and the declaration
(`schemeshard_impl.h:496-503`); insert it after `prevProposeUndoSafe`. In the
body it is a single assignment beside the existing two:

```cpp
        footprint.OriginalTxIndex = originalTxIndex;   // beside :129-130
```

`TPathFootprint` gains, next to `PartId`:

```cpp
    // Index into TEvModifySchemeTransaction.Transaction of the request
    // transaction this part descends from. Max<ui32>() when unknown.
    ui32 OriginalTxIndex = Max<ui32>();
```

### D3. Where the H1 pass goes, and where the request footprints live

Put the pass **after Phase Zero and before Phase One**, i.e. between :262 and
:266. Reasons:

- `rewrittenTransactions[i]` is 1:1 with `record.GetTransaction(i)`, so the
  index is the origin index without bookkeeping.
- SchemeShard state has not been mutated yet (no `Propose()` has run), so
  resolution sees the same world every part footprint's pre-Propose resolution
  sees for the first part.
- `Rewrite` has been applied, so the footprint describes what SchemeShard will
  actually do. Only two backup ops set `NeedRewrite`
  (`schemeshard__op_traits.h`), and neither rewrite touches `WorkingDir` or the
  relative-to-database forms that relocation needs, so streaming the *client's*
  literal proto and relocating it with these footprints stays sound. Say so in
  a comment rather than storing two copies.

Code (8 lines):

```cpp
    // # H1: request-level footprint, one per client transaction, before any
    // part is constructed. This is the layer relocation rewrites against; the
    // per-part footprints below describe what SchemeShard derived from it.
    operation->RequestFootprints.reserve(rewrittenTransactions.size());
    for (ui32 i = 0; i < rewrittenTransactions.size(); ++i) {
        auto footprint = ResolvePathFootprint(rewrittenTransactions[i], context.SS);
        footprint.OriginalTxIndex = i;
        operation->RequestFootprints.push_back(std::move(footprint));
    }
```

Storage on `TOperation`, beside the existing `PathFootprints`
(`schemeshard__operation.h:18-20`):

```cpp
    // H1: one footprint per transaction of the client request, indexed by
    // OriginalTxIndex. PathFootprints below are the derived per-part view and
    // point back here through the same field.
    TVector<TPathFootprint> RequestFootprints;
```

Caveats to write into the commit message: `IgniteOperation` returns early on a
duplicate txId (:217), a quota failure (:229) and a rewrite failure (:256)
**before** this point, so those requests get no footprint at all; a Phase One
split failure (:274) leaves `RequestFootprints` populated but
`PathFootprints` empty. `PartId`/`ProposeStatus`/`PartOpType` are meaningless
on a request footprint — leave them at their defaults and have
`FormatPathFootprintLine` print `partId# <request>` when
`PartId == InvalidSubTxId`.

---

## S7e — canonicalize by id → by name, and relocate

### E1. The 7 id fields, their name form, and the proof each is equivalent

Run on the owning SchemeShard, on a **copy** of the `TModifyScheme`
(`Propose()` must keep seeing the client's bytes). Every rule below was read
from the `Propose()` that consumes the field.

| # | id field(s) | proto decl | name form to write | `Propose()` site |
|---|---|---|---|---|
| 1 | `Drop.Id` | `flat_scheme_op.proto:54` `optional uint64 Id = 3;` (`Name = 1` :52) | `tx.SetWorkingDir(parent.PathString()); tx.MutableDrop()->SetName(leaf); tx.MutableDrop()->ClearId();` | `schemeshard__operation_rmdir.cpp:32` |
| 2 | `AlterTable.PathId` | `TTableDescription` field 34, `optional NKikimrProto.TPathID PathId = 34;` | `tx.SetWorkingDir(parent); tx.MutableAlterTable()->SetName(leaf); ->ClearPathId(); ->ClearId_Deprecated();` | `schemeshard__operation_alter_table.cpp:607-610` |
| 3 | `AlterTable.Id_Deprecated` | `TTableDescription` field 2, `optional uint64 Id_Deprecated = 2;` | same as 2 | same |
| 4 | `AlterPersQueueGroup.PathId` | `TPersQueueGroupDescription` field 2, `optional uint64 PathId = 2;` | `tx.SetWorkingDir(parent); tx.MutableAlterPersQueueGroup()->SetName(leaf); ->ClearPathId();` | `schemeshard__operation_alter_pq.cpp:631,651` |
| 5 | `AlterBlockStoreVolume.PathId` | `TBlockStoreVolumeDescription` field 2, `optional uint64 PathId = 2;` | `tx.SetWorkingDir(parent); tx.MutableAlterBlockStoreVolume()->SetName(leaf); ->ClearPathId();` | `schemeshard__operation_alter_bsv.cpp:396,420` |
| 6 | `AlterReplication.PathId` | `TReplicationDescription` field 3, `optional NKikimrProto.TPathID PathId = 3;` | `tx.SetWorkingDir(parent); tx.MutableAlterReplication()->SetName(leaf); ->ClearPathId();` | `schemeshard__operation_alter_replication.cpp:368-386` |
| 7 | `SplitMergeTablePartitions.TableOwnerId` + `.TableLocalId` | :1813 / :1809 | `->SetTablePath(abs); ->ClearTableOwnerId(); ->ClearTableLocalId();` (leave `WorkingDir` alone — it is not consulted) | `schemeshard__operation_split_merge.cpp:819-823,855-857` |

The equivalence proofs, quoted:

```cpp
// rmdir.cpp:32
        TPath path = drop.HasId()
            ? TPath::Init(context.SS->MakeLocalId(drop.GetId()), context.SS)
            : TPath::Resolve(parentPathStr, context.SS).Dive(name);
```
```cpp
// alter_table.cpp:607
        if (alter.HasId_Deprecated() || alter.HasPathId()) {
            pathId = alter.HasPathId()
                ? TPathId::FromProto(alter.GetPathId())
                : context.SS->MakeLocalId(alter.GetId_Deprecated());
        }
```
```cpp
// alter_pq.cpp:645
        if (!alter.HasName() && !alter.HasPathId()) { ...InvalidParameter... }
        TPath path = alter.HasPathId()
            ? TPath::Init(pathId, context.SS)
            : TPath::Resolve(parentPathStr, context.SS).Dive(name);
```
```cpp
// alter_bsv.cpp:420 — byte-identical shape to alter_pq
// alter_replication.cpp:384
        const auto path = pathId
            ? TPath::Init(pathId, context.SS)
            : TPath::Resolve(workingDir, context.SS).Dive(name);
```
```cpp
// split_merge.cpp:849
        if (!info.HasTablePath() && !info.HasTableLocalId()) { ...InvalidParameter... }
        TPath path = pathId
            ? TPath::Init(pathId, context.SS)
            : TPath::Resolve(info.GetTablePath(), context.SS);
```

Two consequences for the writer:

- In cases 2-6 the id form **wins** when present, so `ClearPathId()` is
  mandatory, not cosmetic; writing a `Name` beside a live `PathId` changes
  nothing.
- Case 7 uses `TPath::Resolve(TablePath)` with **no** `WorkingDir` join, so
  `TablePath` must be the absolute path. That matches the extractor, which
  emits it as `Absolute` (`schemeshard_path_footprint.cpp:308`).

Transfer sharing: `AlterTransfer` is not a separate submessage. Transfer ops go
through the same `TAlterReplication::Propose` with a different `IStrategy`
(`schemeshard__operation_alter_replication.cpp:32-69`) and read
`Transaction.GetAlterReplication()` (:365). One rule covers both.

`ApplyIf` has **no name form**:

```proto
// flat_scheme_op.proto:1891
message TApplyIf {
    optional uint64 PathId = 1;
    optional uint64 PathVersion = 2;
    optional uint64 LockedTxId = 3;
    optional bool CheckEntityVersion = 4 [default = false];
    repeated EPathType PathTypes = 5;
}
```

`repeated TApplyIf ApplyIf = 20;` on `TModifyScheme`. Strip it on
canonicalize and record that fact in the result; re-deriving it is a consumer
decision (§8.6 of the plan).

Unknown id (`TPath::Init` does not resolve): the operation would be rejected
anyway. Emit an untransformable marker instead of guessing — a `bool
Untransformable` plus the offending `FieldPath` on the canonicalize result.

The path id → name lookup: the footprint already did it.
`TPathFootprintEntry::AbsPath` and `RelPathToParent` for the `ById` entry are
filled by `ResolvePathFootprint` (`schemeshard_path_footprint.cpp:1011-1023`),
so the canonicalizer needs the footprint plus the copy, not a second
`TSchemeShard` walk. The parent working dir is `AbsPath` minus
`RelPathToParent`; do it with `TPath::Resolve(AbsPath, ss).Parent().PathString()`
rather than string surgery, so a root-level target is handled.

### E2. Setting `WorkingDir`

`optional string WorkingDir = 1;` on `TModifyScheme` (`flat_scheme_op.proto`
:2003, first field of the message at :2002). Accessors:
`tx.SetWorkingDir(v)`, `tx.GetWorkingDir()`, `tx.HasWorkingDir()`,
`tx.ClearWorkingDir()`. `OperationType` is field 2, `Internal` field 36,
`FailOnExist` field 50.

`ResolvePathFootprint` canonicalises `WorkingDir` through
`TPath::Resolve(...).PathString()` (:932-936) precisely because the raw proto
string may be non-canonical; the canonicaliser should write the canonical form
(`footprint.WorkingDir` is the raw one — use the resolved string, or add
`TString WorkingDirCanon` to `TPathFootprint`; it is computed at :936 and
currently thrown away. **Recommend adding it**, one line, and S7e needs it).

### E3. `RelocatePaths` rules per kind

Inputs: the request footprint (H1, §S7d), the old database path
(`footprint.DatabasePathId` → its path string) and the new database path
`newDb`. Rules, one per `EPathRefKind`
(`schemeshard_path_footprint.h:18-38`), derived from how
`ResolvePathFootprint` resolves each kind (`schemeshard_path_footprint.cpp`
:971-1020):

| kind | resolution at :971-1020 | relocation rule |
|---|---|---|
| `LeafUnderWorkingDir` | `workingDirPath.Child(Value)` | **leave the value alone.** Moving `WorkingDir` moves it. |
| `PathUnderWorkingDirSplit` | `workingDirPath.Child(Value, TSplitChildTag{})` — a leading slash does **not** escape | **leave alone**, same reason. Never treat a leading slash here as absolute. |
| `PathUnderWorkingDir` | `ResolveRelativeOrAbsolute` (:904): absolute iff `Value.StartsWith('/')` | rewrite **only when `Value.StartsWith('/')`**: `Value := newDb + "/" + entry.RelPathToDatabase`. Otherwise leave alone. |
| `Absolute` | `TPath::Resolve(Value)` unconditionally, `WorkingDir` never joined (:988-995) | always rewrite: `Value := newDb + "/" + entry.RelPathToDatabase`. |
| `LeafUnderSibling` | `base.Child(Value)` where base is `BasePath` or `Entries[AnchorIndex].AbsPath` (:997-1009) | **leave the leaf alone**; the base entry is rewritten by its own rule. Never rewrite both. |
| `ById` | `TPath::Init(pathId)` (:1011-1017) | not relocatable as an id — it must have been canonicalised to a name form first (§E1), after which it is a `LeafUnderWorkingDir`. If canonicalisation failed, mark the whole request untransformable. |
| `Implicit` | mirrors its anchor (:952-967), `Value` empty | nothing to write. |

Plus one request-level rewrite:
`tx.SetWorkingDir(newDb + "/" + footprint.WorkingDirRelToDb)`, with the
degenerate case `WorkingDirRelToDb.empty()` → `tx.SetWorkingDir(newDb)`.
`StripPrefix` (:911-922) returns `""` when the path equals the prefix, so that
case is real and must be handled.

Two values that must **never** be rewritten, both already correct in the
extractor: replication `SrcPath` (a path on a *remote* cluster — the extractor
deliberately never emits it, see the comment at
`schemeshard_path_footprint.cpp:188-190`), and any value whose entry did not
resolve (`entry.AbsPath.empty()`), where `RelPathToDatabase` falls back to
`entry.AbsPath` (:1040) and is meaningless.

### E4. The setter switch, keyed by `FieldPath` string (S7f replaces this)

`FieldPath` strings are built by `TRefSink` (`schemeshard_path_footprint.cpp`
:18-93) plus two formatters:

```cpp
// :95
TString Indexed(TStringBuf prefix, size_t i, TStringBuf suffix) {
    return TStringBuilder() << prefix << "[" << i << "]" << suffix;
}
// :99
TString Keyed(TStringBuf prefix, TStringBuf key, TStringBuf suffix) {
    return TStringBuilder() << prefix << "[" << key << "]" << suffix;
}
```

So a repeated element is `Prefix[<decimal>]Suffix` and a map element is
`Prefix[<key>]Suffix`. Parse back by scanning for the **last** `[` before the
matching `]` in each dot-separated segment; a decimal-only body is a repeated
index, anything else is a map key. Map keys in this tree are index names, which
never contain `.` `[` `]`, so a naive scan is safe today — assert it.
Map iteration is sorted for stability (`SortedByKey`, :104-113), so the
`[key]` form is deterministic but its *position* among siblings is not tied to
the proto's internal order; always look the key up, never the position.

**Complete list of `Absolute` FieldPath strings** (`out.Abs`, 29 sites). These
are the ones `RelocatePaths` must be able to write:

| FieldPath | site | setter |
|---|---|---|
| `Replication.Config.TransferSpecific.Target.DstPath` | :198 (prefix `"Replication"`, :686) | `tx.MutableReplication()->MutableConfig()->MutableTransferSpecific()->MutableTarget()->SetDstPath(v)` |
| `AlterReplication.Config.TransferSpecific.Target.DstPath` | :198 (prefix `"AlterReplication"`, :697) | `tx.MutableAlterReplication()->…->SetDstPath(v)` |
| `Replication.Config.TransferSpecific.Target.DirectoryPath` | :202 | `…MutableTarget()->SetDirectoryPath(v)` |
| `AlterReplication.Config.TransferSpecific.Target.DirectoryPath` | :202 | idem |
| `Replication.Config.Specific.Targets[i].DstPath` | :214 | `…MutableConfig()->MutableSpecific()->MutableTargets(i)->SetDstPath(v)` |
| `AlterReplication.Config.Specific.Targets[i].DstPath` | :214 | idem |
| `Replication.AlterTransfer.DirectoryPath` | :220 | `tx.MutableReplication()->MutableAlterTransfer()->SetDirectoryPath(v)` |
| `AlterReplication.AlterTransfer.DirectoryPath` | :220 | idem |
| `CreateTable.CopyFromTable` | :236 | `tx.MutableCreateTable()->SetCopyFromTable(v)` |
| `AlterPersQueueGroup.PQTabletConfig.OffloadConfig.IncrementalBackup.DstPath` | :290 | `tx.MutableAlterPersQueueGroup()->MutablePQTabletConfig()->MutableOffloadConfig()->MutableIncrementalBackup()->SetDstPath(v)` |
| `SplitMergeTablePartitions.TablePath` | :308 | `tx.MutableSplitMergeTablePartitions()->SetTablePath(v)` |
| `CreateConsistentCopyTables.CopyTableDescriptions[i].SrcPath` | :395 | `tx.MutableCreateConsistentCopyTables()->MutableCopyTableDescriptions(i)->SetSrcPath(v)` |
| `CreateConsistentCopyTables.CopyTableDescriptions[i].DstPath` | :397 | idem, `SetDstPath` |
| `InitiateIndexBuild.Table` | :448 | `tx.MutableInitiateIndexBuild()->SetTable(v)` |
| `ApplyIndexBuild.TablePath` | :465 | `tx.MutableApplyIndexBuild()->SetTablePath(v)` |
| `CancelIndexBuild.TablePath` | :501 | `tx.MutableCancelIndexBuild()->SetTablePath(v)` |
| `CreateColumnTable.CopyFromTable` | :536 | `tx.MutableCreateColumnTable()->SetCopyFromTable(v)` |
| `MoveTable.SrcPath` / `MoveTable.DstPath` | :647 / :649 | `tx.MutableMoveTable()->SetSrcPath/SetDstPath(v)` |
| `MoveTableIndex.SrcPath` / `MoveTableIndex.DstPath` | :655 / :657 | `tx.MutableMoveTableIndex()->…` |
| `MoveSequence.SrcPath` / `MoveSequence.DstPath` | :665 / :666 | `tx.MutableMoveSequence()->…` |
| `CopySequence.CopyFrom` | :676 | `tx.MutableCopySequence()->SetCopyFrom(v)` |
| `MoveIndex.TablePath` | :715 | `tx.MutableMoveIndex()->SetTablePath(v)` |
| `CreateExternalTable.DataSourcePath` | :725 | `tx.MutableCreateExternalTable()->SetDataSourcePath(v)` |
| `InitiateColumnBuild.Table` | :745 | `tx.MutableInitiateColumnBuild()->SetTable(v)` |
| `DropColumnBuild.Settings.Table` | :748 | `tx.MutableDropColumnBuild()->MutableSettings()->SetTable(v)` |
| `RestoreMultipleIncrementalBackups.SrcTablePaths[i]` | :800 | `tx.MutableRestoreMultipleIncrementalBackups()->SetSrcTablePaths(i, v)` |
| `RestoreMultipleIncrementalBackups.DstTablePath` | :803 | `…->SetDstTablePath(v)` |
| `CreateBackupCollection.ExplicitEntryList.Entries[i].Path` | :814 | `tx.MutableCreateBackupCollection()->MutableExplicitEntryList()->MutableEntries(i)->SetPath(v)` |
| `AlterTable.Columns[i].DefaultFromSequence` | :269-272 | `tx.MutableAlterTable()->MutableColumns(i)->SetDefaultFromSequence(v)` — **conditional**: emitted as `Absolute` only when the value starts with `/`, else as `LeafUnderSibling` (:274). Rewrite only the `Absolute` case. |

**Complete list of `PathUnderWorkingDir` FieldPath strings** (`out.Path`, 11
sites). Rewrite only when the value starts with `/`:

| FieldPath | site | setter |
|---|---|---|
| `CreateCdcStream.TableName` | :183 (prefix `"CreateCdcStream"`, :556) | `tx.MutableCreateCdcStream()->SetTableName(v)` |
| `AlterUserAttributes.PathName` | :365 | `tx.MutableAlterUserAttributes()->SetPathName(v)` |
| `DropIndex.TableName` | :486 | `tx.MutableDropIndex()->SetTableName(v)` |
| `AlterCdcStream.TableName` | :577 | `tx.MutableAlterCdcStream()->SetTableName(v)` |
| `DropCdcStream.TableName` | :595 | `tx.MutableDropCdcStream()->SetTableName(v)` |
| `RotateCdcStream.TableName` | :619 | `tx.MutableRotateCdcStream()->SetTableName(v)` |
| `ChangePathState.Path` | :853 | `tx.MutableChangePathState()->SetPath(v)` |
| `IncrementalRestoreLockTargets.DstPaths[i]` | :859 | `tx.MutableIncrementalRestoreLockTargets()->SetDstPaths(i, v)` |
| `IncrementalRestoreLockTargets.SrcPaths[i]` | :863 | `…->SetSrcPaths(i, v)` |
| `TruncateTable.TableName` | :888 | `tx.MutableTruncateTable()->SetTableName(v)` |
| `<WorkingDir>` | :838 | **not a field.** Synthetic entry for `CreateFullBackupOp`, whose target is the working dir itself. The setter switch must recognise it and do nothing (the `WorkingDir` rewrite already covers it). |

Note `DropIndex.TableName` and `AlterCdcStream.TableName` / `RotateCdcStream.TableName`
appear under **two different kinds** depending on the op type (:486 vs :495,
:577 vs :589) — this is finding D7 in the report, the same field resolved
differently per op. The switch must therefore key on `(FieldPath, Kind)` or,
better, on `(OperationType, FieldPath)`. **Do not key on `FieldPath` alone.**

`LeafUnderWorkingDir`, `PathUnderWorkingDirSplit`, `LeafUnderSibling` and
`Implicit` sites need no setter at all under the §E3 rules, so the switch has
40 arms, not 130.

---

## S7g — replay experiment as a test

### G1. Two environments in one test

`TTestEnv` constructors (`ut_helpers/test_env.h:127-130`):

```cpp
        TTestEnv(TTestActorRuntime& runtime, ui32 nchannels = 4, bool enablePipeRetries = true,
            TSchemeShardFactory ssFactory = &CreateFlatTxSchemeShard);
        TTestEnv(TTestActorRuntime& runtime, const TTestEnvOptions& opts,
            TSchemeShardFactory ssFactory = &CreateFlatTxSchemeShard,
            std::shared_ptr<NKikimr::NDataShard::IExportFactory> dsExportFactory = {});
```

`TTestEnvOptions` is a builder of ~60 feature-flag options
(`test_env.h:29-101`); the only global is
`bool TTestEnv::ENABLE_SCHEMESHARD_LOG` (`test_env.cpp:31`), a static toggle,
plus four file-scope `static const bool ENABLE_*_LOG` at :32-37. No port
manager, no `Singleton<>`, no process-wide registry appears in `test_env.cpp`.
`TTestBasicRuntime` (`ydb/core/testlib/basics/runtime.h:9`) only binds real
sockets when `UseRealInterconnect` is set (`runtime.cpp:34,74`), which the
schemeshard tests do not set.

**But**: I found no test anywhere under `ydb/` that instantiates two
`TTestBasicRuntime`s in one test body (grepped for the obvious naming). The
absence of a precedent is the strongest signal here. Treat "two runtimes
coexist" as unproven.

**Recommendation: do not use two runtimes.** Use one `TTestBasicRuntime` +
one `TTestEnv` + one SchemeShard, and make env A and env B two **subdomains**
of the same schemeshard: `/MyRoot/dbA` and `/MyRoot/dbB`. This tests exactly
what relocation is for (rewrite a request from database X to database Y),
costs nothing in fidelity for the §8.7 question, and dodges the runtime
question entirely. Path ids and tablet ids still differ between the two trees,
so the masking work in G4 is unchanged. If a future variant genuinely needs
two schemeshards, `TTestTxConfig::SchemeShard` is a constant and every helper
has a `ui64 schemeShardId` overload, so a second tablet in the same runtime is
the next step up, not a second runtime.

### G2. A plain directory is **not** enough — use a real subdomain

Relocation is defined against `RelPathToDatabase` and `WorkingDirRelToDb`,
which `ResolvePathFootprint` computes from
`TPath::GetDomainPathString()` (`schemeshard_path_footprint.cpp:943`):

```cpp
// schemeshard_path.cpp:1396
TString TPath::GetDomainPathString() const {
    return Init(GetPathIdForDomain(), SS).PathString();
}
```

`GetPathIdForDomain` (:1408) walks to the nearest **domain** element. For
`/MyRoot/dirA/t` where `dirA` is a plain `MkDir`, the domain is `/MyRoot`, so
`RelPathToDatabase == "dirA/t"` and `newDb + relToDb` would produce
`/MyRoot/dirA/t` again — the relocation is a no-op. Create real subdomains:

```cpp
    TestCreateSubDomain(runtime, ++txId, "/MyRoot", R"(
        Name: "dbA"
        PlanResolution: 50
        Coordinators: 1
        Mediators: 1
        TimeCastBucketsPerMediator: 2
        StoragePools { Name: "pool-1" Kind: "pool-kind-1" }
    )");
    env.TestWaitNotification(runtime, txId);
```

That is the shape used at `ut_subdomain/ut_subdomain.cpp:928-938`.
`TestCreateSubDomain` comes from `GENERIC_WITH_ATTRS_HELPERS(CreateSubDomain)`
(`ut_helpers/helpers.h:176`), which expands to
`ui64 TestCreateSubDomain(TTestActorRuntime&, ui64 txId, const TString& parentPath,
const TString& scheme, const TVector<TExpectedResult>& = {{StatusAccepted}},
const NKikimrSchemeOp::TAlterUserAttributes& = {}, const TApplyIf& = {});`
(macro at :161-172). The `TTestEnvOptions().NStoragePools(2)` default
(`test_env.h:85`) already provides pools; match the pool kind the env
registers or the create is rejected.

### G3. Sending a hand-built `TModifyScheme`

`ut_helpers/helpers.h:79` exports the alias
`using TEvTx = TEvSchemeShard::TEvModifySchemeTransaction;`.
`CreateRequest(ui64 schemeShardId, ui64 txId, NKikimrSchemeOp::TModifyScheme&& tx)`
exists at `helpers.cpp:864` but is **not declared in helpers.h** — the test
must write its own three lines, which is exactly what `CreateRequest` does:

```cpp
    ui64 SendModify(TTestActorRuntime& runtime, ui64 txId,
            const NKikimrSchemeOp::TModifyScheme& tx,
            ui64 schemeShardId = TTestTxConfig::SchemeShard) {
        auto* ev = new TEvTx(txId, schemeShardId);
        *ev->Record.AddTransaction() = tx;
        AsyncSend(runtime, schemeShardId, ev);
        return TestModificationResults(runtime, txId, {{NKikimrScheme::StatusAccepted}});
    }
```

using
`void AsyncSend(TTestActorRuntime&, ui64 targetTabletId, IEventBase* ev, ui32 nodeIndex = 0, TActorId sender = TActorId());`
(`helpers.h:150`, impl at `helpers.cpp:3085` — it forwards to the tablet via
an edge actor) and
`ui64 TestModificationResults(TTestActorRuntime&, ui64 txId, const TVector<TExpectedResult>& expectedResults);`
(`helpers.h:146`, impl at `helpers.cpp:221`, which grabs
`TEvModifySchemeTransactionResult` until `txId` matches).

### G4. Getting the original request protos

**Build them in the test; do not capture.** Every `Test<Name>` helper is
`Async<Name>` + `TestModificationResults`, and every `Async<Name>` is
`AsyncSend(<Name>Request(...))` (macro at `helpers.cpp:871-900`). The
`<Name>Request(txId, ...)` form returns a `TEvTx*` whose
`Record.GetTransaction(0)` is the finished `TModifyScheme`. So the test can:

```cpp
    TVector<NKikimrSchemeOp::TModifyScheme> script;
    auto run = [&](TEvTx* req) {                       // takes ownership
        script.push_back(req->Record.GetTransaction(0));
        AsyncSend(runtime, TTestTxConfig::SchemeShard, req);
        TestModificationResults(runtime, txId, {{NKikimrScheme::StatusAccepted}});
        env.TestWaitNotification(runtime, txId);
    };
    run(MkDirRequest(++txId, "/MyRoot/dbA", "dir"));
```

`CreateRequest` returns a raw `TEvTx*` and `AsyncSend` consumes it, so read
the record **before** the send. Capturing via a runtime observer would also
work but would pick up the internally generated requests that §8.7 says must
never be replayed.

### G5. `DescribePath` and the recursive walk

`ut_helpers/helpers.h:107-114`:

```cpp
    NKikimrScheme::TEvDescribeSchemeResult DescribePath(TTestActorRuntime& runtime, ui64 schemeShard, const TString& path, const NKikimrSchemeOp::TDescribeOptions& opts);
    NKikimrScheme::TEvDescribeSchemeResult DescribePath(TTestActorRuntime& runtime, const TString& path, const NKikimrSchemeOp::TDescribeOptions& opts);
    NKikimrScheme::TEvDescribeSchemeResult DescribePrivatePath(TTestActorRuntime& runtime, ui64 schemeShard, const TString& path, bool returnPartitioning = false, bool returnBoundaries = false);
    NKikimrScheme::TEvDescribeSchemeResult DescribePath(TTestActorRuntime& runtime, ui64 schemeShard, const TString& path, bool returnPartitioning = false, bool returnBoundaries = false, bool showPrivate = false, bool returnBackups = false);
    TPathVersion ExtractPathVersion(const NKikimrScheme::TEvDescribeSchemeResult& describe);
    TPathVersion TestDescribeResult(const NKikimrScheme::TEvDescribeSchemeResult& describe, TVector<NLs::TCheckFunc> checks = {});
```

with the options builder at :98-104
(`SetReturnPartitioningInfo`, `SetReturnPartitionConfig`, `SetBackupInfo`,
`SetReturnBoundaries`, `SetShowPrivateTable`). Use
`TDescribeOptionsBuilder().SetShowPrivateTable(true)` so index impl tables and
CDC-stream PQ groups appear — otherwise the two trees differ for reasons that
have nothing to do with relocation.

There is **no** recursive-describe helper in the tree. `ls_checks.cpp:173` and
:879 iterate `record.GetPathDescription().GetChildren()` for single-level
checks only. Write a ~15-line walker in the test:

```cpp
    void Walk(TTestActorRuntime& runtime, const TString& path,
            TVector<std::pair<TString, NKikimrScheme::TEvDescribeSchemeResult>>& out) {
        auto d = DescribePath(runtime, path,
            TDescribeOptionsBuilder().SetShowPrivateTable(true));
        out.emplace_back(path, d);
        for (const auto& child : d.GetPathDescription().GetChildren()) {
            Walk(runtime, path + "/" + child.GetName(), out);
        }
    }
```

then compare `outA[i]` to `outB[i]` pairwise after (a) stripping the database
prefix from the key and (b) masking per G6. Sort children by name before
recursing — `GetChildren()` order is `TPathElement` insertion order and is not
guaranteed to match across two trees.

### G6. Fields to mask

`TEvDescribeSchemeResult` (`flat_tx_scheme.proto:112-125`): mask `PathId`,
`PathOwnerId`, `DEPRECATED_PathOwner`, `LastExistedPrefixPathId`; rewrite
`Path` and `LastExistedPrefixPath` by prefix substitution instead of masking.

`TDirEntry` (`flat_scheme_op.proto:2283-2302`) — mask `PathId` (2),
`SchemeshardId` (3), `CreateTxId` (6), `CreateStep` (7), `ParentPathId` (8),
`PathVersion` (13), `Version` (15, the whole `TPathVersion`). Keep `Name` (1),
`PathType` (4), `CreateFinished` (5), `PathState` (9), `Owner` (10),
`PathSubType` (14), `ChildrenExist` (16). `ACL`/`EffectiveACL` (11/12) are
serialised `NACLibProto.TDiffACL` blobs; keep them only if the test sets no
ACLs, otherwise mask — `EffectiveACL` inherits from the domain, so it differs
by construction.

`TPathVersion` (:2244-2274) is 30 counters (`GeneralVersion`, `ACLVersion`,
`TableSchemaVersion`, `TablePartitionVersion`, …). Mask the whole message; the
counts legitimately differ because env B replays the requests as a fresh
sequence.

`TPathDescription` (:2339-2375) — mask `TablePartitions` (4, each
`TTablePartition.DatashardId` at :2309 is a tablet id), `TableStats` (8),
`TabletMetrics` (9), `TablePartitionStats` (15), `TablePartitionMetrics` (18),
`AbandonedTenantsSchemeShards` (19), `BackupProgress` (6),
`LastBackupResult` (7), and `DomainDescription` (10) in full (it carries
coordinator/mediator tablet ids and pool bindings). Keep `Self`, `Children`,
`Table`, `UserAttributes`, `TableIndex`, and the typed `*Description` messages.

`TTableDescription` — mask `Id_Deprecated` (2), `PathId` (34),
`TableSchemaVersion` (33), `PartitionConfig` (7),
`UniformPartitionsCount` (6), `SplitBoundary` (31), `PartitionRangeBegin/End`
(20/21), `CoordinatedSchemaVersion` (49), `Path` (9). Keep `Name`, `Columns`,
`KeyColumnNames`, `TTLSettings`, `IsBackup`, `Temporary`, `PathState`.
`KeyColumnIds` (5) are assigned per table and are stable for identical column
order — keep them, and if they turn out to drift, mask.

Inside the typed descriptions: `TIndexDescription.LocalPathId` (field 2) and
`.PathOwnerId` (field 7); `TCdcStreamDescription.PathId` (field 3);
`TSequenceDescription.PathId` (field 2); `TPartitionConfig.FollowerCount` (3),
`.CrossDataCenterFollowerCount` (8), `.ChannelProfileId` (9) — all masked by
the blanket `PartitionConfig` mask above.

Implement masking as one recursive reflection pass over
`google::protobuf::Message` driven by a `THashSet<TString>` of
`"<FullMessageName>.<FieldName>"` keys, then compare
`maskedA.DebugString()` with `maskedB.DebugString()` so a failure prints a
readable diff. That is ~40 lines and it is the only part of this test that is
not glue.

### G7. Minimal deterministic op sequence

Everything below is single-shard, no data, no timestamps, so it is
reproducible. Run against `/MyRoot/dbA`, replay relocated into `/MyRoot/dbB`.

1. `MkDir /MyRoot/dbA` → `dir` — the trivial `LeafUnderWorkingDir` case.
2. `MkDir /MyRoot/dbA` → `a/b` — exercises the auto-generated `MkDir`s from
   `SplitIntoTransactions` (`schemeshard__operation.cpp:910-916`) and proves
   the H1 footprint, not the derived parts, is what gets rewritten.
3. `CreateTable /MyRoot/dbA/dir` → `t` (2 columns, 1 key, `UniformPartitionsCount`
   unset so exactly one shard).
4. `AlterTable /MyRoot/dbA/dir` → add one nullable column. Then repeat it in a
   second request addressed **by `PathId`**, to exercise §E1 case 2.
5. `CreateIndexedTable /MyRoot/dbA/dir` → `it` with one global sync index —
   the compound case whose children are never named in the request.
6. `CreateCdcStream` on `/MyRoot/dbA/dir/t` — derived PQ group under the
   stream; needs `SetShowPrivateTable(true)` to appear in the describe.
7. `CreateTable /MyRoot/dbA/dir` → `copy` with `CopyFromTable: "/MyRoot/dbA/dir/t"`
   — the `Absolute` source, and the one S7a added.
8. `MoveTable /MyRoot/dbA/dir/copy` → `/MyRoot/dbA/dir/moved` — two `Absolute`
   values in one request, both rewritten.
9. `DropTable` by name on `/MyRoot/dbA/dir/moved`.
10. `RmDir` **by id** on `/MyRoot/dbA/a/b` — §E1 case 1, and the reason the
    canonicaliser has to run on the source schemeshard.

Skip anything in §8.7's non-determinism list: no `BackupBackupCollection`
(its `TargetDir` is stamped with `Now()`), no continuous backup (timestamped
stream names), no index build (data-dependent), no `SplitMergeTablePartitions`
(internal). Note in the test that step 5's `CreateIndexedTable` mutates the
request in place for fulltext rowid provisioning — capture the proto from the
`*Request` helper *before* sending, which the G4 shape already does.

Expected failures to bucket rather than fix: step 6's PQ group carries
partition/tablet identity (masked), and the index impl table of step 5 gets its
own path id (masked). If anything else diverges, that divergence is the
finding.

---

## Summary

1. **S7c**: `TMemoryChanges` has 24 private undo stacks, 19 keyed by `TPathId`;
   `TStack` exposes `size()` but is not iterable, so swap it for a ~20-line
   `TUndoStack<T>` over `TVector` and add `Mark()` + `CollectPathIdsSince()`.
2. `TSideEffects::PublishPaths` is `THashMap<TTxId, TDeque<TPathId>>` keyed by
   **txId not opId**, so a part's publications are an index range; versions
   come from `TSchemeShard::GetPathVersion(...).GetGeneralVersion()`, the same
   call `DoPersistPublishPaths` makes later.
3. `IsUndoChangesSafe()` lives on `TOperationContext`
   (`schemeshard__operation_part.h:153`) and is monotone, so record both a
   conservative `WriteSetMayBeIncomplete` and a precise `BecameUndoUnsafe`.
   The whole S7c hook is 14 added lines inside the existing part loop at
   `schemeshard__operation.cpp:113`.
4. **S7d**: Phase Zero (`schemeshard__operation.cpp:254-262`) is 1:1 with
   `record.GetTransaction()`, so carrying two parallel `TVector<ui32>` origin
   vectors through Phase One (:272-284) and one extra parameter on
   `ProcessOperationParts` is the whole mechanism.
5. The H1 pass goes between :262 and :266, 8 lines, storing into a new
   `TOperation::RequestFootprints`; three early returns above it mean some
   requests get no footprint, which must be documented.
6. **S7e**: 7 id fields, 6 name forms, each proved equivalent by the quoted
   `Propose()` ternary; `ClearPathId()` is mandatory because the id form wins.
   `ApplyIf` has no name form and gets stripped. Transfer shares
   `AlterReplication` entirely.
7. Relocation rewrites `Absolute` always, `PathUnderWorkingDir` only on a
   leading slash, and nothing else; `LeafUnderWorkingDir`,
   `PathUnderWorkingDirSplit` and `LeafUnderSibling` ride along on the
   `WorkingDir` rewrite. Replication `SrcPath` must never be touched.
8. The setter switch needs 29 `Absolute` + 11 `PathUnderWorkingDir` arms, all
   enumerated above with their `Mutable…Set…` expressions, and must key on
   `(OperationType, FieldPath)` because three fields change kind by op type.
9. **S7g**: no in-tree precedent for two `TTestBasicRuntime`s, so use one
   runtime with two real **subdomains** (`/MyRoot/dbA`, `/MyRoot/dbB`); a plain
   `MkDir` is not a database and would make relocation a no-op.
10. There is no recursive-describe or describe-diff helper in the tree; the
    test needs a ~15-line child walker, a ~40-line reflection masker over the
    listed physical fields, and its own 5-line `SendModify` because
    `CreateRequest` is `.cpp`-local.

File: `/home/innokentii/ydbwork3/ydb/.omc/research/schemeshard-path-footprint/findings/s7-design-notes.md`
