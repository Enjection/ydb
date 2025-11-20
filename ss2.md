## Table of Contents
1. [Operation Parts Vector](#operation-parts-vector)
2. [Critical Enums - DO NOT REORDER](#critical-enums)
3. [DbChanges and MemChanges Mechanics](#dbchanges-and-memchanges)

---

## Operation Parts Vector

### Overview

Operations in SchemeShard are split into **Parts** (sub-operations). Each part is an independent unit that can progress through states independently. The `Parts` vector is the core mechanism for managing multi-step, multi-object operations.

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation.h:10-15`

```cpp
struct TOperation: TSimpleRefCount<TOperation> {
    using TPtr = TIntrusivePtr<TOperation>;

    const TTxId TxId;
    ui32 PreparedParts = 0;
    TVector<ISubOperation::TPtr> Parts;  // ← The Parts vector
    // ...
};
```

---

### Why Parts Are Needed

#### Problem: Complex Operations Span Multiple Objects

Consider **DropIndexedTable**:
1. Drop table's indexes (multiple sub-operations)
2. Wait for all indexes to drop (barrier)
3. Drop the main table

Without parts, this would be:
- A monolithic state machine tracking multiple objects
- Complex coordination logic scattered everywhere
- Difficult crash recovery

#### Solution: Split Into Parts

Each part is a **separate sub-operation** with its own:
- Operation ID: `TOperationId(TxId, SubTxId)`
- State machine
- Message handlers
- Database state tracking

**Benefits:**
- **Modularity:** Each part is self-contained
- **Reusability:** Drop index logic is same everywhere
- **Independent Progress:** Parts progress at their own pace
- **Clear Coordination:** Barriers synchronize parts explicitly

---

### How Parts Are Created

#### Step 1: ConstructParts Factory

`ydb/core/tx/schemeshard/schemeshard__operation.cpp:1653-1655`

```cpp
TVector<ISubOperation::TPtr> TOperation::ConstructParts(const TTxTransaction& tx,
                                                       TOperationContext& context) const {
    return AppData()->SchemeOperationFactory->MakeOperationParts(*this, tx, context);
}
```

The factory delegates to operation-specific logic based on `tx.GetOperationType()`.

#### Step 2: Operation-Specific Part Construction

**Example: Drop Indexed Table**

```cpp
// Creates multiple parts:
TVector<ISubOperation::TPtr> parts;

for (auto& index : table->Indexes) {
    // Part 0, 1, 2, ... - Drop each index
    parts.push_back(CreateDropTableIndex(opId, txState));
}

// Last part - Drop main table (after barrier)
parts.push_back(CreateDropTable(opId, txState));

return parts;
```

#### Step 3: Adding Parts to Operation

`ydb/core/tx/schemeshard/schemeshard__operation.cpp:277-295`

```cpp
// For all initial transactions parts are constructed and proposed
for (const auto& transaction : transactions) {
    auto parts = operation->ConstructParts(transaction, context);
    operation->PreparedParts += parts.size();

    if (!ProcessOperationParts(parts, txId, record, prevProposeUndoSafe,
                               operation, response, context)) {
        return std::move(response);
    }
}
```

**ProcessOperationParts:** `ydb/core/tx/schemeshard/schemeshard__operation.cpp:120-188`

```cpp
for (auto& part : parts) {
    auto response = part->Propose(owner, context);

    if (response->IsAccepted()) {
        operation->AddPart(part);  // ← Add to Parts vector
    } else if (response->IsDone()) {
        operation->AddPart(part);
        context.OnComplete.DoneOperation(part->GetOperationId());
    } else {
        // Abort entire operation if any part fails
        AbortOperationPropose(txId, context);
        return false;
    }
}
```

**AddPart Implementation:** `ydb/core/tx/schemeshard/schemeshard__operation.cpp:1657-1659`

```cpp
void TOperation::AddPart(ISubOperation::TPtr part) {
    Parts.push_back(part);
}
```

---

### Context Loss on Restoration

#### The Problem

When SchemeShard crashes and restarts:
1. **All in-memory state is lost**
2. Only database state survives
3. Operations must be reconstructed from scratch

**What's lost:**
- `TOperation` object
- `Parts` vector
- In-flight messages
- Runtime state

**What's saved:**
- `TTxState` in database (Type, State, TargetPathId, Shards, etc.)
- Path metadata
- Shard mappings

#### The Solution: Stateless Restoration

**Key Insight:** All context needed to continue must be in `TTxState` (saved to DB).

**Restoration Flow:**

`ydb/core/tx/schemeshard/schemeshard__init.cpp:3734`

```cpp
// During TTxInit, for each TxState loaded from DB:
ISubOperation::TPtr part = operation->RestorePart(txState.TxType, txState.State, context);
```

**RestorePart Implementation:** `ydb/core/tx/schemeshard/schemeshard__operation.cpp:1031-1130`

```cpp
ISubOperation::TPtr TOperation::RestorePart(TTxState::ETxType txType,
                                           TTxState::ETxState txState,
                                           TOperationContext& context) const {
    switch (txType) {
    case TTxState::TxCreateTable:
        return CreateNewTable(NextPartId(), txState);
    case TTxState::TxAlterTable:
        return CreateAlterTable(NextPartId(), txState);
    case TTxState::TxDropTable:
        return CreateDropTable(NextPartId(), txState);
    // ... all operation types
    }
}
```

**CreateNewTable with state:** Creates operation object, then selects state handler:

```cpp
TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
    switch (state) {
    case TTxState::CreateParts:
        return MakeHolder<TCreateParts>(OperationId);
    case TTxState::ConfigureParts:
        return MakeHolder<TConfigureParts>(OperationId);
    case TTxState::Propose:
        return MakeHolder<TPropose>(OperationId);
    case TTxState::ProposedWaitParts:
        return MakeHolder<NTableState::TProposedWaitParts>(OperationId);
    case TTxState::Done:
        return MakeHolder<TDone>(OperationId);
    }
}
```

#### Critical Requirement: TTxState Must Be Complete

**BAD Example (DON'T DO THIS):**

```cpp
// Store important info only in memory
struct TConfigureParts {
    TVector<TString> specialSettings;  // ← LOST on crash!

    bool ProgressState() {
        for (auto& setting : specialSettings) {  // ← Empty after restore!
            // ...
        }
    }
};
```

**GOOD Example (DO THIS):**

```cpp
// Store everything in TTxState (persisted to DB)
TTxState txState;
txState.TargetPathId = pathId;
txState.SourcePathId = sourcePathId;
txState.Shards = shardList;
txState.MinStep = minStep;
// ... all needed info

// On restore, read from txState
TConfigureParts::ProgressState() {
    TTxState* txState = context.SS->FindTx(OperationId);
    for (auto& shard : txState->Shards) {  // ← From DB!
        // ...
    }
}
```

#### Parts Vector After Restore

**IMPORTANT:** The `Parts` vector is **NOT restored** to its original contents!

**Before crash:**
```
Operation[TxId=100]
  Parts = [
    DropIndex(100:0),  // SubTxId=0
    DropIndex(100:1),  // SubTxId=1
    DropIndex(100:2),  // SubTxId=2
    DropTable(100:3)   // SubTxId=3
  ]
```

**After restore:**
```
Operation[TxId=100]
  Parts = []  // Empty!
```

Only **active** parts (not yet Done) are restored:

```cpp
for each TTxState in DB where State != Done {
    auto part = operation->RestorePart(txState.TxType, txState.State, context);
    operation->AddPart(part);
}
```

**Why this works:**
- Each part has its own `TxState` entry in DB
- Parts in `Done` state are already persisted and don't need restoration
- Only in-flight parts need to continue

---

## Critical Enums - DO NOT REORDER

### Overview

Several enums in SchemeShard have **strict ordering requirements**. Reordering them **breaks upgrade/downgrade** between YDB versions because:
- Enum values are **persisted to database** as integers
- Old data uses old integer values
- Changing mapping = data corruption

### ETxType - Transaction Type

**Location:** `ydb/core/tx/schemeshard/schemeshard_subop_types.h:14-154`

```cpp
// WARNING: DO NOT REORDER this constants
// reordering breaks update
#define TX_STATE_TYPE_ENUM(item) \
    item(TxInvalid, 0) \
    item(TxMkDir, 1) \
    item(TxCreateTable, 2) \
    item(TxCreatePQGroup, 3) \
    item(TxAlterPQGroup, 4) \
    item(TxAlterTable, 5) \
    item(TxDropTable, 6) \
    item(TxDropPQGroup, 7) \
    item(TxModifyACL, 8) \
    item(TxRmDir, 9) \
    item(TxCopyTable, 10) \
    item(TxSplitTablePartition, 11) \
    item(TxBackup, 12) \
    item(TxCreateSubDomain, 13) \
    item(TxDropSubDomain, 14) \
    // ... continues to 116

enum ETxType {
    TX_STATE_TYPE_ENUM(TX_STATE_DECLARE_ENUM)
};
```

**Why critical:**
- Saved to `Schema::TxInFlightV2::TxType` column
- Used in `RestorePart()` switch statement
- Changing `TxCreateTable = 2` to `TxCreateTable = 99` → existing operations become invalid

**Adding new types:**
✅ **SAFE:** Append to end
```cpp
    item(TxCreateNewFeature, 117) \  // ← Safe, new value
```

❌ **UNSAFE:** Insert in middle
```cpp
    item(TxCreateTable, 2) \
    item(TxNewFeature, 3) \          // ← Breaks! Shifts all below
    item(TxCreatePQGroup, 4) \       // ← Was 3, now 4!
```

### ETxState - Transaction State

**Location:** `ydb/core/tx/schemeshard/schemeshard_subop_state_types.h:7-38`

```cpp
// WARNING: DO NOT REORDER this constants
// reordering breaks update
enum ETxState {
    Invalid = 0,
    Waiting = 1,
    CreateParts = 2,
    ConfigureParts = 3,
    DropParts = 4,
    DeleteParts = 5,
    // ...
    Propose = 128,
    ProposedWaitParts = 129,
    // ...
    Done = 240,
    Aborted = 250,
};
```

**Why critical:**
- Saved to `Schema::TxInFlightV2::State` column
- Used in `SelectStateFunc()` to determine which handler to use
- On crash/restart, SchemeShard reads state from DB and must map to correct handler

**Example scenario:**
1. V1: `ConfigureParts = 3`
2. Operation saved to DB with `State = 3`
3. Upgrade to V2 where `ConfigureParts = 5` (someone inserted states)
4. Restore reads `State = 3` → wrong handler!

---

## DbChanges and MemChanges

### Overview

SchemeShard uses a **two-phase change tracking** system:
1. **MemChanges:** Track in-memory changes (can be rolled back)
2. **DbChanges:** Track database writes (committed atomically)

This enables **safe operation abort** if validation fails mid-operation.

### Architecture

```
┌─────────────────────────────────────────────────┐
│           TOperationContext                     │
├─────────────────────────────────────────────────┤
│  TMemoryChanges MemChanges;  // Stack-based     │
│  TStorageChanges DbChanges;   // Queue-based    │
└─────────────────────────────────────────────────┘
                 │
                 v
       ┌─────────────────┐
       │  ApplyOnExecute │  Execute phase
       └────────┬────────┘
                │
         ┌──────┴──────┐
         │             │
    Success?        Failure?
         │             │
         v             v
    ┌────────┐    ┌─────────┐
    │ Commit │    │ UnDo()  │
    └────────┘    └─────────┘
         │             │
         v             v
   ┌────────────┐  ┌──────────────┐
   │  Persisted │  │ Memory       │
   │  to DB     │  │ Restored     │
   └────────────┘  └──────────────┘
```

---

### TMemoryChanges - Rollback Support

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_memory_changes.h:16-137`

```cpp
class TMemoryChanges: public TSimpleRefCount<TMemoryChanges> {
    using TPathState = std::pair<TPathId, TPathElement::TPtr>;
    TStack<TPathState> Paths;  // ← Stack for LIFO restore

    using TTableState = std::pair<TPathId, TTableInfo::TPtr>;
    TStack<TTableState> Tables;

    using TShardState = std::pair<TShardIdx, THolder<TShardInfo>>;
    TStack<TShardState> Shards;

    using TTxState = std::pair<TOperationId, THolder<TTxState>>;
    TStack<TTxState> TxStates;

    THashMap<TPathId, TSubDomainInfo::TPtr> SubDomains;
    // ... more containers for all object types

public:
    void GrabNewPath(TSchemeShard* ss, const TPathId& pathId);
    void GrabPath(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewTable(TSchemeShard* ss, const TPathId& pathId);
    void GrabTable(TSchemeShard* ss, const TPathId& pathId);

    void UnDo(TSchemeShard* ss);  // ← Rollback all changes
};
```

#### Key Concept: "Grab" Before Modify

**Pattern:** Before modifying any SchemeShard in-memory structure, **grab** (save) its current state.

**Implementation:** `ydb/core/tx/schemeshard/schemeshard__operation_memory_changes.cpp:7-17`

```cpp
// For NEW objects (don't exist yet)
template <typename I, typename C, typename H>
static void GrabNew(const I& id, const C& cont, H& holder) {
    Y_ABORT_UNLESS(!cont.contains(id));
    holder.emplace(id, nullptr);  // ← Save "null" = object should NOT exist
}

// For EXISTING objects (already exist)
template <typename T, typename I, typename C, typename H>
static void Grab(const I& id, const C& cont, H& holder) {
    Y_ABORT_UNLESS(cont.contains(id));
    holder.emplace(id, new T(*cont.at(id)));  // ← Deep copy current state
}
```

#### Usage Example

**Scenario:** Create a new table

```cpp
// In operation's Propose():

// 1. Grab path (will be created)
context.MemChanges.GrabNewPath(ss, pathId);

// 2. Create path
TPathElement::TPtr path = new TPathElement(...);
ss->PathsById[pathId] = path;  // ← Modify in-memory state

// 3. Grab table (will be created)
context.MemChanges.GrabNewTable(ss, pathId);

// 4. Create table
TTableInfo::TPtr table = new TTableInfo(...);
ss->Tables[pathId] = table;  // ← Modify in-memory state

// Later: if validation fails...
// context.MemChanges.UnDo(ss);  ← Restores everything
```

#### UnDo Implementation

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_memory_changes.cpp:163-363`

```cpp
void TMemoryChanges::UnDo(TSchemeShard* ss) {
    // be aware of the order of grab & undo ops
    // stack is the best way to manage it right

    while (Paths) {
        const auto& [id, elem] = Paths.top();
        if (elem) {
            ss->PathsById[id] = elem;  // ← Restore old value
        } else {
            ss->PathsById.erase(id);   // ← Delete (was new)
        }
        Paths.pop();
    }

    while (Tables) {
        const auto& [id, elem] = Tables.top();
        if (elem) {
            ss->Tables[id] = elem;  // ← Restore old value
        } else {
            ss->Tables.erase(id);   // ← Delete (was new)
        }
        Tables.pop();
    }

    // ... same for all object types
}
```

**Why Stack?**
- Operations modify objects in order: A → B → C
- Rollback must restore in reverse: C → B → A
- Stack guarantees LIFO (Last In, First Out)

#### When UnDo Is Called

`ydb/core/tx/schemeshard/schemeshard__operation.cpp:302-319`

```cpp
void TSchemeShard::AbortOperationPropose(const TTxId txId, TOperationContext& context) {
    Y_ABORT_UNLESS(Operations.contains(txId));
    TOperation::TPtr operation = Operations.at(txId);

    // Drop operation side effects, undo memory changes
    context.OnComplete = {};
    context.DbChanges = {};

    for (auto& i : operation->Parts) {
        i->AbortPropose(context);
    }

    context.MemChanges.UnDo(context.SS);  // ← Rollback all memory changes

    // And remove aborted operation from existence
    Operations.erase(txId);
}
```

**Abort Scenario:**
1. User creates table
2. Validation passes initial checks
3. Quota check fails later
4. `AbortOperationPropose()` called
5. `UnDo()` removes path, table, shards from memory
6. DB transaction rolled back
7. No trace of failed operation remains

---

### TStorageChanges - Database Persistence

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_db_changes.h:15-168`

```cpp
class TStorageChanges: public TSimpleRefCount<TStorageChanges> {
    TDeque<TPathId> Paths;
    TDeque<TPathId> Tables;
    TDeque<TShardIdx> Shards;
    TDeque<TOperationId> TxStates;
    TDeque<TPathId> AlterUserAttrs;
    // ... more queues for all persistent operations

public:
    void PersistPath(const TPathId& pathId) {
        Paths.push_back(pathId);
    }

    void PersistTable(const TPathId& pathId) {
        Tables.push_back(pathId);
    }

    void PersistTxState(const TOperationId& opId) {
        TxStates.push_back(opId);
    }

    void Apply(TSchemeShard* ss, NTabletFlatExecutor::TTransactionContext &txc,
               const TActorContext &ctx);
};
```

#### Key Difference from MemChanges

| Feature | TMemoryChanges | TStorageChanges |
|---------|----------------|-----------------|
| **Container** | `TStack` | `TDeque` |
| **Purpose** | Rollback support | Track DB writes |
| **When applied** | Immediately | At transaction commit |
| **Can rollback** | Yes (UnDo) | No (DB rollback) |
| **Stores** | Full object copies | Just IDs |

#### Usage Pattern

```cpp
// In operation's Propose():

// 1. Modify memory
context.MemChanges.GrabNewPath(ss, pathId);
TPathElement::TPtr path = new TPathElement(...);
ss->PathsById[pathId] = path;

// 2. Schedule DB write
context.DbChanges.PersistPath(pathId);
```

**NOTE:** `DbChanges` only stores **which objects to persist**, not the objects themselves. The actual write happens in `Apply()`.

#### Apply Implementation

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_db_changes.cpp:7-131`

```cpp
void TStorageChanges::Apply(TSchemeShard* ss,
                           NTabletFlatExecutor::TTransactionContext& txc,
                           const TActorContext&) {
    NIceDb::TNiceDb db(txc.DB);

    for (const auto& pId : Paths) {
        ss->PersistPath(db, pId);  // ← Write path to DB
    }

    for (const auto& pId : Tables) {
        ss->PersistTable(db, pId);  // ← Write table to DB
    }

    for (const auto& shardIdx : Shards) {
        const TShardInfo& shardInfo = ss->ShardInfos.at(shardIdx);
        ss->PersistShardMapping(db, shardIdx, shardInfo.TabletID, ...);
    }

    for (const auto& opId : TxStates) {
        ss->PersistTxState(db, opId);  // ← Write TxState to DB
    }

    // ... persist all tracked changes
}
```

**When Apply Is Called:**

During transaction execution, in `ApplyOnExecute` phase:

```cpp
void TSchemeShard::Execute(TTxOperationProgress* tx, const TActorContext& ctx) {
    // ... operation logic

    // Commit phase
    context.DbChanges.Apply(this, txc, ctx);  // ← Write to DB

    // If DB commit succeeds → changes are permanent
    // If DB commit fails → transaction rolled back, UnDo called
}
```

---

### The Complete Flow

#### Happy Path (Success)

```cpp
// 1. PROPOSE PHASE
THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) {
    // Grab current state
    context.MemChanges.GrabNewPath(ss, pathId);

    // Modify memory
    TPathElement::TPtr path = new TPathElement(...);
    ss->PathsById[pathId] = path;

    // Schedule DB write
    context.DbChanges.PersistPath(pathId);

    // Create TxState
    TTxState& txState = ss->CreateTx(opId, TTxState::TxCreateTable, pathId);
    context.DbChanges.PersistTxState(opId);

    return StatusAccepted;
}

// 2. EXECUTE PHASE (in transaction executor)
void Execute() {
    // Apply DB changes
    context.DbChanges.Apply(ss, txc, ctx);

    // Commit transaction
    txc.DB.Commit();  // ← Success!

    // Memory changes already applied, keep them
    // MemChanges destructor does nothing
}
```

#### Failure Path (Abort)

```cpp
// 1. PROPOSE PHASE
THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) {
    // Grab current state
    context.MemChanges.GrabNewPath(ss, pathId);

    // Modify memory
    TPathElement::TPtr path = new TPathElement(...);
    ss->PathsById[pathId] = path;

    // Validation fails!
    if (quotaExceeded) {
        return StatusResourceExhausted;
    }
}

// 2. ABORT HANDLING (in IgniteOperation)
if (!response->IsAccepted()) {
    // Rollback DB transaction
    context.GetTxc().DB.RollbackChanges();

    // Undo memory changes
    context.MemChanges.UnDo(context.SS);  // ← Restore to before Propose

    // Memory and DB are consistent again!
}
```

---

### Why This Design?

#### Problem 1: Partial Failure

Without change tracking:
```cpp
// Bad: direct modification
ss->PathsById[pathId] = newPath;
ss->Tables[pathId] = newTable;

// Validation fails here!
if (quotaExceeded) {
    // How to undo? We don't remember old state!
    // Memory is corrupted now
}
```

With change tracking:
```cpp
// Good: tracked modification
context.MemChanges.GrabNewPath(ss, pathId);
ss->PathsById[pathId] = newPath;

context.MemChanges.GrabNewTable(ss, pathId);
ss->Tables[pathId] = newTable;

// Validation fails here!
if (quotaExceeded) {
    context.MemChanges.UnDo(ss);  // ← Clean rollback
}
```

#### Problem 2: Crash During Propose

If SchemeShard crashes **during** `Propose()`:
- Memory changes are lost (process died)
- DB changes are rolled back (transaction not committed)
- On restart: old state restored from DB
- **No corruption!**

#### Problem 3: Consistency

Change tracking ensures:
- **Memory and DB match** (same changes tracked in both)
- **Atomic commits** (all DB writes in single transaction)
- **Clean rollback** (UnDo reverses all memory changes)

---

### Best Practices

#### DO: Grab Before Modify

```cpp
✅ CORRECT:
context.MemChanges.GrabPath(ss, pathId);
ss->PathsById[pathId]->SomeField = newValue;
```

```cpp
❌ WRONG:
ss->PathsById[pathId]->SomeField = newValue;
// No way to undo!
```

#### DO: Persist After Modify

```cpp
✅ CORRECT:
ss->PathsById[pathId] = newPath;
context.DbChanges.PersistPath(pathId);
```

```cpp
❌ WRONG:
context.DbChanges.PersistPath(pathId);
ss->PathsById[pathId] = newPath;  // Changes not captured!
```

#### DO: Use Appropriate Grab

```cpp
✅ For NEW objects:
context.MemChanges.GrabNewPath(ss, pathId);
ss->PathsById[pathId] = new TPathElement(...);
```

```cpp
✅ For EXISTING objects:
context.MemChanges.GrabPath(ss, pathId);
ss->PathsById[pathId]->DirAlterVersion++;
```

```cpp
❌ WRONG:
context.MemChanges.GrabNewPath(ss, pathId);  // Says "new"
ss->PathsById[pathId]->DirAlterVersion++;     // But modifying existing!
// UnDo will DELETE the path!
```

---

## Summary

### Operation Parts Vector

1. **Purpose:** Split complex operations into independent, reusable sub-operations
2. **Creation:** `ConstructParts()` → factory creates parts → `AddPart()` to vector
3. **Restoration:** Parts NOT restored from DB; only active `TTxState` entries restored
4. **Critical Rule:** All context must be in `TTxState` (saved to DB), not just in memory

### Critical Enums

1. **ETxType:** Transaction types, values 0-116+
2. **ETxState:** Transaction states (CreateParts=2, ConfigureParts=3, etc.)
3. **Rule:** NEVER reorder, only append new values at end
4. **Reason:** Integer values persisted to DB, changing breaks upgrade

### DbChanges and MemChanges

1. **MemChanges:**
   - Stack-based tracking of in-memory changes
   - Supports rollback via `UnDo()`
   - Must "Grab" before modifying

2. **DbChanges:**
   - Queue-based tracking of DB writes
   - Applied atomically in `Apply()`
   - No rollback (DB transaction handles it)

3. **Flow:**
   - Grab → Modify Memory → Persist to DB queue
   - Success: Commit DB, keep memory
   - Failure: Rollback DB, UnDo memory

This architecture enables **safe, crash-tolerant schema operations** with clean failure handling and consistent state across memory and disk.
