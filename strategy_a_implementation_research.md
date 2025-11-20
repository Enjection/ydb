# Strategy A Implementation Research: Barrier-Based Coordination for CDC Version Sync

## Executive Summary

### Problem Recap

When creating CDC streams for tables with indexes during incremental backup/restore operations, parallel execution of CDC creation parts causes race conditions that desynchronize `AlterVersion` across Table, Index entity, and indexImplTable objects. This violates query engine invariants and can cause SCHEME_CHANGED errors.

**Root Cause:**
- Multiple CDC stream operations execute in parallel as separate operation parts
- Each part independently reads, modifies, and writes schema versions
- No coordination mechanism exists between parallel parts
- Classic race condition: read-modify-write without synchronization

### Strategy A Overview

**Strategy A: Barrier-Based Coordination** uses the existing SchemeShard barrier mechanism to coordinate version synchronization after all CDC streams are created.

**Core Concept:**
1. CDC streams for a table and its indexes are created in parallel (preserving performance)
2. Each CDC part registers at a barrier instead of syncing versions immediately
3. When all CDC parts complete, a dedicated version sync part executes
4. The sync part atomically reads all current versions and sets them to a consistent value

**Advantages:**
- Uses battle-tested barrier infrastructure
- Atomic version sync after all CDC operations complete
- Clean separation of concerns (CDC creation vs version sync)
- Easy to understand, debug, and maintain

**Trade-offs:**
- Requires creating a new operation part type (CdcVersionSync)
- Adds latency: barrier wait time + sync execution time
- More complex operation structure
- Must pass barrier context through CDC creation flow

### Implementation Scope

This research document provides:
1. **Deep analysis** of barrier mechanism internals
2. **Detailed implementation guide** with code examples
3. **State machine modifications** for CDC operations
4. **Testing strategies** and validation approaches
5. **Risk mitigation** and rollback plans

---

## 1. Barrier Mechanism Deep Dive

### 1.1 What Are Barriers?

Barriers are a coordination primitive in SchemeShard operations that block a set of operation parts from completing until all parts reach the barrier point.

**Use Case:** When operation parts have dependencies (e.g., "drop all indexes before dropping table"), barriers ensure proper ordering without losing parallelism benefits.

### 1.2 Barrier Data Structures

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation.h:119-146`

```cpp
struct TOperation: TSimpleRefCount<TOperation> {
    const TTxId TxId;
    TVector<ISubOperation::TPtr> Parts;           // All operation parts
    TSet<TSubTxId> DoneParts;                     // Completed parts
    THashMap<TString, TSet<TSubTxId>> Barriers;   // Barrier name → blocked parts
    
    void RegisterBarrier(TSubTxId partId, const TString& name) {
        Barriers[name].insert(partId);
        Y_ABORT_UNLESS(Barriers.size() == 1);  // Only ONE barrier at a time!
    }
    
    bool IsDoneBarrier() const {
        for (const auto& [_, subTxIds] : Barriers) {
            for (const auto blocked : subTxIds) {
                Y_VERIFY_S(!DoneParts.contains(blocked), 
                          "part is blocked and done: " << blocked);
            }
            // Barrier complete when: blocked parts + done parts = total parts
            return subTxIds.size() + DoneParts.size() == Parts.size();
        }
        return false;
    }
    
    void DropBarrier(const TString& name) {
        Y_ABORT_UNLESS(IsDoneBarrier());
        Barriers.erase(name);
    }
};
```

**Key Constraints:**
1. **Only one barrier per operation at a time** - This is enforced by `Y_ABORT_UNLESS(Barriers.size() == 1)`
2. **Barrier holds part IDs** - Parts register themselves by SubTxId
3. **Completion check** - Barrier is done when `blocked + done = total`

### 1.3 Barrier Lifecycle

#### Phase 1: Registration

Parts register at barrier during their state progression:

```cpp
// In TSubOperationState::ProgressState()
bool ProgressState(TOperationContext& context) override {
    // Do work...
    
    // Register at barrier instead of completing
    context.OnComplete.Barrier(OperationId, "barrier_name");
    
    return false;  // Don't progress further
}
```

**TSideEffects::Barrier implementation:**

```cpp
void Barrier(const TOperationId& opId, const TString& name) {
    Barriers.push_back({opId, name});
}
```

This queues barrier registration for processing after transaction commit.

#### Phase 2: Registration Processing

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:DoRegisterBarriers`

```cpp
void TSideEffects::DoRegisterBarriers(TSchemeShard* ss) {
    for (auto& [opId, name] : Barriers) {
        auto operation = ss->Operations.at(opId.GetTxId());
        operation->RegisterBarrier(opId.GetSubTxId(), name);
    }
}
```

Parts are added to `operation->Barriers[name]` set.

#### Phase 3: Completion Detection

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:1086-1141`

```cpp
void TSideEffects::DoCheckBarriers(TSchemeShard* ss, 
                                   NTabletFlatExecutor::TTransactionContext& txc,
                                   const TActorContext& ctx) {
    TSet<TTxId> touchedOperations;
    
    // Collect operations that registered barriers or completed parts
    for (auto& [opId, name] : Barriers) {
        touchedOperations.insert(opId.GetTxId());
    }
    for (auto& opId : DoneOperations) {
        touchedOperations.insert(opId.GetTxId());
    }
    
    // Check each operation's barrier status
    for (auto& txId : touchedOperations) {
        auto& operation = ss->Operations.at(txId);
        
        if (!operation->HasBarrier() || !operation->IsDoneBarrier()) {
            continue;  // Not ready yet
        }
        
        // Barrier is complete! Notify all blocked parts
        auto name = operation->Barriers.begin()->first;
        const auto& blockedParts = operation->Barriers.begin()->second;
        
        LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "All parts have reached barrier"
                     << ", tx: " << txId
                     << ", done: " << operation->DoneParts.size()
                     << ", blocked: " << blockedParts.size());
        
        // Create TEvCompleteBarrier event
        THolder<TEvPrivate::TEvCompleteBarrier> msg = 
            MakeHolder<TEvPrivate::TEvCompleteBarrier>(txId, name);
        TEvPrivate::TEvCompleteBarrier::TPtr personalEv = 
            (TEventHandle<TEvPrivate::TEvCompleteBarrier>*) 
            new IEventHandle(ss->SelfId(), ss->SelfId(), msg.Release());
        
        // Send to all blocked parts
        for (auto& partId : blockedParts) {
            operation->Parts.at(partId)->HandleReply(personalEv, context);
        }
        
        operation->DropBarrier(name);
    }
}
```

**Timeline for CDC Version Sync - CORRECTED**:

```
T1: Part0 (CDC) registers → Barriers["cdc_sync_table1"] = {0}
T2: Part1 (CDC) registers → Barriers["cdc_sync_table1"] = {0, 1}
T3: Part2 (CDC) registers → Barriers["cdc_sync_table1"] = {0, 1, 2}
T4: DoCheckBarriers() runs:
    - IsDoneBarrier(): blocked={0,1,2}, done={0,1,2,4}, total=5
    - 3 + 2 == 5 → TRUE (barrier complete!)
    - Creates TEvCompleteBarrier("cdc_sync_table1")
    - Sends to Parts[0], Parts[1], Parts[2]
    - DropBarrier("cdc_sync_table1")
```

**KEY INSIGHT**: Parts 0,1,2 (CDC) will receive the event but don't handle it. Part 3 (CdcVersionSync) should also register so it receives the event. The sync part is what actually performs the version synchronization.

#### Phase 4: Barrier Completion Handling - SYNC PART ONLY

Only the `CdcVersionSync` part implements `HandleReply(TEvCompleteBarrier)`:

```cpp
bool TWaitBarrier::HandleReply(TEvPrivate::TEvCompleteBarrier::TPtr& ev, 
                                TOperationContext& context) override {
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
               DebugHint() << " HandleReply TEvCompleteBarrier");
    
    NIceDb::TNiceDb db(context.GetDB());
    
    // All CDC parts have completed - now safe to sync versions atomically
    TVector<TPathId> affectedPaths;
    CollectAffectedPaths(TablePathId, context, affectedPaths);
    
    ui64 maxVersion = FindMaxVersion(affectedPaths, context);
    
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
               DebugHint() << " Syncing all versions to max"
               << ", maxVersion: " << maxVersion);
    
    SyncAllVersions(affectedPaths, maxVersion, context, db);
    
    // Mark as done
    context.OnComplete.DoneOperation(OperationId);
    return true;
}
```

**CRITICAL DESIGN DETAIL**: The sync part should also register at the barrier so it's guaranteed to be notified. This ensures it only performs sync after all CDC parts complete.

### 1.4 Real-World Example: Drop Indexed Table

**Location:** `ydb/core/tx/schemeshard/schemeshard__operation_drop_indexed_table.cpp:187-241`

```cpp
class TDeletePathBarrier: public TSubOperationState {
    TOperationId OperationId;
    
    bool HandleReply(TEvPrivate::TEvCompleteBarrier::TPtr& ev, 
                     TOperationContext& context) override {
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TDeletePathBarrier HandleReply TEvCompleteBarrier");
        
        NIceDb::TNiceDb db(context.GetDB());
        TTxState* txState = context.SS->FindTx(OperationId);
        TPath path = TPath::Init(txState->TargetPathId, context.SS);
        
        // Now safe to drop main table path
        DropPath(db, context, OperationId, *txState, path);
        
        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }
    
    bool ProgressState(TOperationContext& context) override {
        // Register at barrier
        context.OnComplete.Barrier(OperationId, "RenamePathBarrier");
        return false;
    }
};
```

**Operation structure:**

```
Operation[DropIndexedTable]
  Parts = [
    DropIndex(SubTxId=0),    // Registers barrier, completes
    DropIndex(SubTxId=1),    // Registers barrier, completes
    DropIndex(SubTxId=2),    // Registers barrier, completes
    DeletePath(SubTxId=3)    // Waits for TEvCompleteBarrier, then drops table
  ]
```

---

## 2. Current CDC Creation Flow Analysis

### 2.1 Entry Point

**File:** `ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp`  
**Function:** `CreateBackupIncrementalBackupCollection` (lines 155-302)

This function is the top-level coordinator for creating CDC streams during incremental backup.

### 2.2 Flow Breakdown

#### Step 1: Create CDC for Main Tables

**Lines 186-224:**

```cpp
TVector<ISubOperation::TPtr> result;
TVector<TPathId> streams;

// Process each table in backup collection
for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
    const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
    
    // Build AlterContinuousBackup request
    NKikimrSchemeOp::TModifyScheme modifyScheme;
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup);
    auto& cb = *modifyScheme.MutableAlterContinuousBackup();
    cb.SetTableName(relativeItemPath);
    auto& ib = *cb.MutableTakeIncrementalBackup();
    ib.SetDstPath(destinationPath);
    
    // Create CDC stream operation part
    TPathId stream;
    if (!CreateAlterContinuousBackup(opId, modifyScheme, context, result, stream)) {
        return result;  // Error
    }
    streams.push_back(stream);
}
```

**Key Point:** `CreateAlterContinuousBackup` adds operation parts to `result` vector. Each CDC creation is a separate part.

#### Step 2: Create CDC for Index Impl Tables

**Lines 226-297:**

```cpp
bool omitIndexes = bc->Description.GetIncrementalBackupConfig().GetOmitIndexes();
if (!omitIndexes) {
    for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
        const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
        
        // Iterate through table's children
        for (const auto& [childName, childPathId] : tablePath.Base()->GetChildren()) {
            auto childPath = context.SS->PathsById.at(childPathId);
            
            // Find indexes
            if (childPath->PathType != NKikimrSchemeOp::EPathTypeTableIndex) {
                continue;
            }
            if (childPath->Dropped()) {
                continue;
            }
            
            // Filter for global indexes
            auto indexInfo = context.SS->Indexes.at(childPathId);
            if (indexInfo->Type != NKikimrSchemeOp::EIndexTypeGlobal) {
                continue;
            }
            
            // Get index impl table (single child of index entity)
            auto indexPath = TPath::Init(childPathId, context.SS);
            Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
            auto [implTableName, implTablePathId] = 
                *indexPath.Base()->GetChildren().begin();
            
            // Build CDC request for impl table
            TString indexImplTableRelPath = 
                JoinPath({relativeItemPath, childName, implTableName});
            
            NKikimrSchemeOp::TModifyScheme modifyScheme;
            // ... same CDC creation as main tables ...
            
            TPathId stream;
            if (!CreateAlterContinuousBackup(opId, modifyScheme, context, 
                                            result, stream)) {
                return result;
            }
            streams.push_back(stream);
        }
    }
}
```

**Key Point:** For each index, a separate CDC creation part is added. All parts execute in parallel.

#### Step 3: Result

**Line 299:**

```cpp
CreateLongIncrementalBackupOp(opId, bcPath, result, streams);
return result;
```

The `result` vector now contains:
- CDC creation parts for main tables
- CDC creation parts for each index impl table
- A final "long backup" tracking part

**Example for table with 2 indexes:**

```
result = [
    CreateCdcStream(table1),           // Part 0
    CreateCdcStream(table1/index1),    // Part 1
    CreateCdcStream(table1/index2),    // Part 2
    LongIncrementalBackup(tracker)     // Part 3
]
```

### 2.3 CDC Stream Creation State Machine

**File:** `ydb/core/tx/schemeshard/schemeshard__operation_create_cdc_stream.cpp`  
**Class:** `TNewCdcStreamAtTable`

**State progression:**

```
CreateParts (if needed) → ConfigureParts → Propose → ProposedWaitParts → Done
```

Most CDC creations skip `CreateParts` since datashards already exist.

**Typical flow:**

```
ConfigureParts:
  - Send TEvProposeTransaction to datashards with CDC config
  - Wait for TEvProposeTransactionResult
  → Advance to Propose

Propose:
  - Propose to coordinator to get global plan step
  - Wait for TEvOperationPlan
  - **THIS IS WHERE VERSION INCREMENT HAPPENS** ←←←
  → Advance to ProposedWaitParts

ProposedWaitParts:
  - Wait for TEvSchemaChanged from datashards
  → Advance to Done
```

### 2.4 Version Increment Location

**File:** `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp`  
**Function:** `TProposeAtTable::HandleReply` (lines 447-479)

**VERIFIED CORRECT** - Barrier mechanism entry point confirmed.

```cpp
bool TProposeAtTable::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, 
                                  TOperationContext& context) {
    const auto* txState = context.SS->FindTx(OperationId);
    const auto& pathId = txState->TargetPathId;
    
    auto path = context.SS->PathsById.at(pathId);
    auto table = context.SS->Tables.at(pathId);
    
    NIceDb::TNiceDb db(context.GetDB());
    
    // *** VERSION INCREMENT HAPPENS HERE ***
    auto versionCtx = BuildTableVersionContext(*txState, path, context);
    UpdateTableVersion(versionCtx, table, OperationId, context, db);
    
    // Additional sync for main tables with indexes
    if (versionCtx.IsContinuousBackupStream && !versionCtx.IsIndexImplTable) {
        NCdcStreamState::SyncChildIndexes(path, table->AlterVersion, 
                                         OperationId, context, db);
    }
    
    context.SS->PersistTableAlterVersion(db, pathId, table);
    context.SS->ClearDescribePathCaches(path);
    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
    
    context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
    return true;
}
```

**UpdateTableVersion logic** (lines 175-248) - **IMPORTANT CHANGE FROM EARLIER ANALYSIS**:

The actual current code has been refactored from what we initially described. It now handles multiple cases:

```cpp
void UpdateTableVersion(const TTableVersionContext& versionCtx,
                       TTableInfo::TPtr& table,
                       TOperationId operationId,
                       TOperationContext& context,
                       NIceDb::TNiceDb& db) {
    if (versionCtx.IsPartOfContinuousBackup && versionCtx.IsIndexImplTable && 
        versionCtx.GrandParentPathId && context.SS->Tables.contains(versionCtx.GrandParentPathId)) {
        
        // Index impl table path - syncs with parent
        SyncImplTableVersion(versionCtx, table, operationId, context, db);
        SyncIndexEntityVersion(versionCtx.ParentPathId, table->AlterVersion, 
                              operationId, context, db);
        
        // Sync sibling indexes to maintain consistency
        auto grandParentPath = context.SS->PathsById.at(versionCtx.GrandParentPathId);
        SyncChildIndexes(grandParentPath, table->AlterVersion, operationId, context, db);
    } else {
        // For main tables: simple increment
        table->AlterVersion += 1;
        
        // For main tables with continuous backup: sync child indexes
        if (!versionCtx.IsIndexImplTable && context.SS->PathsById.contains(versionCtx.PathId)) {
            auto path = context.SS->PathsById.at(versionCtx.PathId);
            if (HasParentContinuousBackup(versionCtx.PathId, context)) {
                SyncChildIndexes(path, table->AlterVersion, operationId, context, db);
            }
        }
    }
}
```

**KEY INSIGHT**: The current `SyncChildIndexes()` implementation (lines 318-368) only syncs the index ENTITY, NOT the impl table version:

```cpp
void SyncChildIndexes(...) {
    for (const auto& [childName, childPathId] : parentPath->GetChildren()) {
        // ... filter logic ...
        
        // Only syncs the index entity
        NCdcStreamState::SyncIndexEntityVersion(childPathId, targetVersion, ...);
        
        // NOTE: Intentionally does NOT sync the index impl table version
        // because bumping AlterVersion without TX_KIND_SCHEME causes SCHEME_CHANGED errors
    }
}
```

### 2.5 Race Condition Timeline - VERIFIED FROM ACTUAL CODE

**Scenario:** Table with 2 indexes, CDC created in parallel

The race condition happens because both CDC parts call `TProposeAtTable::HandleReply()` in parallel, each executing `SyncChildIndexes()` independently:

```
Time    Part0(Index1Impl CDC)              Part1(Index2Impl CDC)
====    ==========================         ==========================
T0      Table.version = 10                 Table.version = 10
        Index1.version = 10                Index2.version = 10
        Index1Impl.version = 10            Index2Impl.version = 10

T1      HandleReply(TEvOperationPlan)      
        UpdateTableVersion():
          SyncImplTableVersion()
          Index1Impl.version = 10 (no inc)

T2      SyncIndexEntityVersion()
        Index1.version = 10

T3                                         HandleReply(TEvOperationPlan)
                                           UpdateTableVersion():
                                             SyncImplTableVersion()
                                             Index2Impl.version = 10

T4      SyncChildIndexes(table):           
        Read: Index1.version = 10          
        Read: Index2.version = 10          
        targetVersion = 10
        
T5                                         SyncIndexEntityVersion()
                                           Index2.version = 10

T6      Write Index1 = 10                  SyncChildIndexes(table):
                                           Read: Index1.version = 10
T7      Write Index2 = 10                  Read: Index2.version = 10
        (DoneOperation)                    targetVersion = 10

T8                                         Write Index1 = 10 ❌ (race!)
                                           Write Index2 = 10

Result: Versions become inconsistent due to concurrent writes!
        Part1 overwrites Part0's writes at T8
```

**Why the race condition occurs:**
1. Both CDC parts independently increment their version in the database
2. Each part calls `UpdateTableVersion()` which reads ALL indexes
3. Each part calculates what target version siblings should have
4. Multiple threads performing unsynchronized "read all → calculate → write all"
5. Last write wins, causing inconsistency
6. **No atomic operation** spanning all related objects

**CRITICAL FINDING**: The current code path in `UpdateTableVersion()` does NOT fully prevent this race. Strategy A's barrier-based coordination solves this by ensuring only ONE part performs the final version sync.

---

## 3. Strategy A Architecture

### 3.1 High-Level Design

**Goal:** Coordinate version synchronization after all CDC streams are created.

**Approach:** Add a barrier and a dedicated version sync part.

```
┌─────────────────────────────────────────────────────────────┐
│ Operation: CreateBackupIncrementalBackupCollection          │
├─────────────────────────────────────────────────────────────┤
│ Parts (parallel execution):                                 │
│                                                             │
│  Part 0: CreateCdcStream(table1)                           │
│          ├─ ConfigureParts                                 │
│          ├─ Propose (increment version locally)            │
│          └─ Register barrier "cdc_version_sync_table1"     │
│                                                             │
│  Part 1: CreateCdcStream(table1/index1/implTable)          │
│          ├─ ConfigureParts                                 │
│          ├─ Propose (increment version locally)            │
│          └─ Register barrier "cdc_version_sync_table1"     │
│                                                             │
│  Part 2: CreateCdcStream(table1/index2/implTable)          │
│          ├─ ConfigureParts                                 │
│          ├─ Propose (increment version locally)            │
│          └─ Register barrier "cdc_version_sync_table1"     │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ BARRIER: All CDC parts registered                    │  │
│  │ IsDoneBarrier() = true                              │  │
│  │ Send TEvCompleteBarrier to Part 3                   │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                             │
│  Part 3: CdcVersionSync(table1)                            │
│          └─ HandleReply(TEvCompleteBarrier)                │
│             ├─ Read all versions                           │
│             ├─ Calculate max                               │
│             ├─ Write all to max (atomic in DB txn)        │
│             └─ Done                                        │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Component Interactions

```
┌──────────────────────────────┐
│ CreateBackupIncremental...   │
│ BackupCollection()           │
└──────────┬───────────────────┘
           │ Creates parts
           v
┌──────────────────────────────┐
│ CDC Parts (0, 1, 2)          │ ──┐
│ - Each increments own version│   │ Register
│ - Register at barrier        │   │ barrier
└──────────────────────────────┘   │
           │                        │
           v                        v
┌──────────────────────────────────────┐
│ TSideEffects::DoCheckBarriers        │
│ - Detects all parts at barrier       │
│ - Creates TEvCompleteBarrier         │
└──────────┬───────────────────────────┘
           │ Send event
           v
┌──────────────────────────────────────┐
│ CdcVersionSync Part (3)              │
│ HandleReply(TEvCompleteBarrier)      │
│ - CollectAffectedPaths()             │
│ - FindMaxVersion()                   │
│ - SyncAllVersions(max)               │
│ - PersistToDb()                      │
└──────────────────────────────────────┘
```

### 3.3 Data Flow

**Phase 1: CDC Creation**

```
Part0: Index1Impl.version = 10 → 11 (local increment)
Part1: Index2Impl.version = 10 → 11 (local increment)
Part2: Index3Impl.version = 10 → 11 (local increment)

All parts: Register barrier "cdc_version_sync_table1"
```

**Phase 2: Barrier Complete**

```
DoCheckBarriers():
  IsDoneBarrier("cdc_version_sync_table1"):
    blocked parts: 3
    done parts: 0
    total parts: 4 (3 CDC + 1 sync)
    3 + 0 < 4 → FALSE, wait
    
  (Later, after CDC parts marked done in other states)
  
  IsDoneBarrier("cdc_version_sync_table1"):
    blocked parts: 3
    done parts: 0
    total parts: 4
    Still FALSE...
```

**IMPORTANT REALIZATION:** Blocked parts shouldn't be counted in `DoneParts`. The barrier completion logic is:

```cpp
return subTxIds.size() + DoneParts.size() == Parts.size();
// blocked_count + done_count == total_count
```

When CDC parts register at barrier, they DON'T mark as done yet. When barrier completes:
- 3 CDC parts are blocked
- 1 sync part is NOT blocked (hasn't registered)
- 3 blocked + 1 not-blocked = 4 total → Barrier done!

**Phase 3: Sync Execution**

```
CdcVersionSync::HandleReply(TEvCompleteBarrier):
  affectedPaths = [
    table1 (pathId=100),
    index1 (pathId=101),
    index1Impl (pathId=102),
    index2 (pathId=103),
    index2Impl (pathId=104),
    index3 (pathId=105),
    index3Impl (pathId=106)
  ]
  
  Read versions:
    index1Impl.version = 11
    index2Impl.version = 11
    index3Impl.version = 11
    index1.version = 10 (not yet synced)
    index2.version = 10
    index3.version = 10
    table1.version = 10
  
  maxVersion = 11
  
  Write all:
    index1.version = 11
    index2.version = 11
    index3.version = 11
    index1Impl.version = 11 (already 11)
    index2Impl.version = 11
    index3Impl.version = 11
    table1.version = 10 (don't increment main table for index CDC)
  
  PersistToDb (atomic transaction)
```

### 3.4 State Machine Modifications

#### Modification 1: TProposeAtTable (CDC Propose State) - REPLACE OLD SYNC LOGIC

**Current behavior (WILL BE REPLACED):**

```
Propose HandleReply:
  - Increment version via UpdateTableVersion()
  - Call SyncChildIndexes() immediately
  - Advance to ProposedWaitParts
  ⚠️ RACE CONDITION: Multiple parts do this in parallel
```

**New behavior with barrier (REPLACES above):**

```
Propose HandleReply:
  - Check if part of coordinated CDC backup operation
  - If YES (part of multi-stream CDC backup):
      - Increment only THIS part's version locally
      - SKIP SyncChildIndexes() call
      - Register at barrier instead
      - Advance to ProposedWaitParts (blocked by barrier)
  - If NO (standalone CDC operation):
      - Keep existing behavior for backward compatibility
      - Increment version via UpdateTableVersion()
      - Call SyncChildIndexes() as before
      - Advance to ProposedWaitParts
```

**Key Change**: When part of a multi-stream CDC backup, skip the synchronization work and let the dedicated `CdcVersionSync` part handle it atomically.

#### Modification 2: New State - CdcVersionSync - REPLACES OLD SYNC LOGIC

**New operation part type:**

```
CdcVersionSync:
  Initial state: WaitBarrier
  
  WaitBarrier:
    - MUST register at barrier (same barrier as CDC parts)
    - When all CDC parts done → barrier triggers
    → HandleReply(TEvCompleteBarrier):
      1. Collect all affected paths (table + all indexes + all impl tables)
      2. Read current version of each
      3. Find maximum version across all
      4. Write maximum to all affected paths (atomic in DB transaction)
      5. Mark as done
    → Operation complete
```

**Why sync part must register at barrier**:
- If sync part doesn't register, it can't be properly notified
- Pattern from drop-indexed-table shows both parts register at same barrier
- Ensures ordering: CDC parts complete → barrier fires → sync executes

---

## 3.5 Implementation Simplification - Replace vs Extend

**KEY DECISION**: We are REPLACING the old `UpdateTableVersion()` and `SyncChildIndexes()` logic, not extending it.

**Why replacement is better than extension**:

1. **Old logic has race conditions** - multiple parts call `SyncChildIndexes()` in parallel
2. **Old logic is complex** - handles many different scenarios (impl tables, main tables, etc.)
3. **New approach is simpler** - barrier ensures single execution point for all sync
4. **Better maintainability** - all version sync logic in one place (CdcVersionSync part)

**What gets replaced**:
- `UpdateTableVersion()` calls in `TProposeAtTable::HandleReply` → simple increment only
- `SyncChildIndexes()` calls in CDC flow → moved to CdcVersionSync part
- Complex version context logic → simplified to just increment

**What's preserved**:
- Non-CDC operations continue using existing logic
- Backward compatibility for standalone CDC operations
- Barrier infrastructure for other operations (drop table, etc.)

---

## 4. Detailed Implementation Steps

### 4.1 Step 1: Modify Backup Collection Creation

**File:** `ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp`

**Goal:** Group CDC parts by table and add version sync parts.

#### Implementation

```cpp
TVector<ISubOperation::TPtr> CreateBackupIncrementalBackupCollection(
    TOperationId opId, const TTxTransaction& tx, TOperationContext& context) {
    
    TVector<ISubOperation::TPtr> result;
    
    // ... validation code (unchanged) ...
    
    // Group CDC operations by table
    THashMap<TPathId, TVector<TPathId>> cdcStreamsByTable;
    THashMap<TPathId, TVector<ISubOperation::TPtr>> cdcPartsByTable;
    
    // Create CDC for main tables
    for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
        const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
        TPathId tablePathId = tablePath.Base()->PathId;
        
        // ... build modifyScheme (unchanged) ...
        
        TPathId stream;
        TVector<ISubOperation::TPtr> tempResult;
        if (!CreateAlterContinuousBackup(opId, modifyScheme, context, 
                                        tempResult, stream)) {
            return tempResult;  // Error
        }
        
        // Store CDC part for this table
        cdcStreamsByTable[tablePathId].push_back(stream);
        cdcPartsByTable[tablePathId].insert(
            cdcPartsByTable[tablePathId].end(),
            tempResult.begin(), tempResult.end()
        );
    }
    
    // Create CDC for index impl tables
    bool omitIndexes = bc->Description.GetIncrementalBackupConfig().GetOmitIndexes();
    if (!omitIndexes) {
        for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
            const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
            TPathId tablePathId = tablePath.Base()->PathId;
            
            // Iterate indexes (same as before)
            for (const auto& [childName, childPathId] : tablePath.Base()->GetChildren()) {
                // ... filter for indexes (unchanged) ...
                
                TPathId stream;
                TVector<ISubOperation::TPtr> tempResult;
                if (!CreateAlterContinuousBackup(opId, modifyScheme, context,
                                                tempResult, stream)) {
                    return tempResult;
                }
                
                cdcStreamsByTable[tablePathId].push_back(stream);
                cdcPartsByTable[tablePathId].insert(
                    cdcPartsByTable[tablePathId].end(),
                    tempResult.begin(), tempResult.end()
                );
            }
        }
    }
    
    // Add CDC parts and version sync parts per table
    TVector<TPathId> allStreams;
    for (auto& [tablePathId, cdcParts] : cdcPartsByTable) {
        // Add CDC parts to result
        result.insert(result.end(), cdcParts.begin(), cdcParts.end());
        
        // Track streams
        allStreams.insert(allStreams.end(), 
                         cdcStreamsByTable[tablePathId].begin(),
                         cdcStreamsByTable[tablePathId].end());
        
        // Add version sync part if we have multiple CDC streams for this table
        if (cdcParts.size() > 1) {
            TString barrierName = TStringBuilder() 
                << "cdc_version_sync_" << tablePathId;
            
            result.push_back(CreateCdcVersionSync(
                NextPartId(opId, result),
                tablePathId,
                barrierName
            ));
        }
    }
    
    // Add long backup tracker
    CreateLongIncrementalBackupOp(opId, bcPath, result, allStreams);
    
    return result;
}
```

**Helper function:**

```cpp
TOperationId NextPartId(TOperationId baseOpId, 
                       const TVector<ISubOperation::TPtr>& parts) {
    return TOperationId(baseOpId.GetTxId(), parts.size());
}
```

### 4.2 Step 2: Pass Barrier Context to CDC Parts

**Challenge:** CDC parts need to know:
1. Whether they're part of coordinated sync
2. What barrier name to register at

**Solution 1: Via TTxState** (Recommended)

Add fields to `TTxState`:

```cpp
struct TTxState {
    // ... existing fields ...
    
    // Barrier coordination
    bool UseBarrierCoordination = false;
    TString BarrierName;
};
```

Modify `CreateAlterContinuousBackup`:

```cpp
bool CreateAlterContinuousBackup(
    TOperationId opId,
    const NKikimrSchemeOp::TModifyScheme& modifyScheme,
    TOperationContext& context,
    TVector<ISubOperation::TPtr>& result,
    TPathId& streamPathId,
    const TString& barrierName = "") {  // New parameter
    
    // ... existing logic ...
    
    // Store barrier info in TTxState
    if (barrierName) {
        txState.UseBarrierCoordination = true;
        txState.BarrierName = barrierName;
        
        NIceDb::TNiceDb db(context.GetDB());
        // Persist barrier info
        db.Table<Schema::TxInFlightV2>()
          .Key(opId.GetTxId(), opId.GetSubTxId())
          .Update(
              NIceDb::TUpdate<Schema::TxInFlightV2::UseBarrierCoordination>(true),
              NIceDb::TUpdate<Schema::TxInFlightV2::BarrierName>(barrierName)
          );
    }
    
    // ... rest of logic ...
}
```

**Solution 2: Via Operation Context** (Alternative)

Store barrier mapping in operation object:

```cpp
struct TOperation {
    // ... existing fields ...
    
    THashMap<TSubTxId, TString> PartBarriers;  // part → barrier name
};
```

Set in backup collection creation:

```cpp
for (auto& [tablePathId, cdcParts] : cdcPartsByTable) {
    TString barrierName = TStringBuilder() << "cdc_version_sync_" << tablePathId;
    
    for (size_t i = 0; i < cdcParts.size(); ++i) {
        TSubTxId partId = result.size() + i;
        operation->PartBarriers[partId] = barrierName;
    }
    
    result.insert(result.end(), cdcParts.begin(), cdcParts.end());
}
```

**Recommendation:** Use Solution 1 (TTxState) for crash recovery support.

### 4.3 Step 3: Modify TProposeAtTable::HandleReply - REPLACE SYNC LOGIC

**File:** `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp`

**Goal:** Check for barrier coordination. If coordinated, skip sync and register at barrier. Otherwise, use simplified version increment (no SyncChildIndexes).

**CHANGE FROM OLD APPROACH**: We're REPLACING the old `UpdateTableVersion()` and `SyncChildIndexes()` calls with a simpler approach. The dedicated sync part will handle all version synchronization.

#### Implementation

```cpp
bool TProposeAtTable::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev,
                                  TOperationContext& context) {
    const auto* txState = context.SS->FindTx(OperationId);
    Y_ABORT_UNLESS(txState);
    
    const auto& pathId = txState->TargetPathId;
    auto path = context.SS->PathsById.at(pathId);
    auto table = context.SS->Tables.at(pathId);
    
    NIceDb::TNiceDb db(context.GetDB());
    
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
               DebugHint() << " HandleReply TEvOperationPlan"
                           << ", pathId: " << pathId
                           << ", operationId: " << OperationId);
    
    // Check if using barrier coordination (part of multi-stream CDC backup)
    if (txState->UseBarrierCoordination && !txState->BarrierName.empty()) {
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " Using barrier coordination"
                   << ", barrier: " << txState->BarrierName);
        
        // REPLACE old UpdateTableVersion() + SyncChildIndexes() logic
        // Just increment this part's version locally
        table->AlterVersion += 1;
        
        context.SS->PersistTableAlterVersion(db, pathId, table);
        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
        
        // Register at barrier - don't do sync yet
        context.OnComplete.Barrier(OperationId, txState->BarrierName);
        
        // Advance to ProposedWaitParts (will be blocked at barrier)
        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
        
        return true;
    }
    
    // Non-coordinated path: standalone CDC operations (backward compatible)
    // Still use simple version increment
    table->AlterVersion += 1;
    
    context.SS->PersistTableAlterVersion(db, pathId, table);
    context.SS->ClearDescribePathCaches(path);
    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
    
    context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
    return true;
}
```

**Key Changes from Old Code**:
1. **Removed** `BuildTableVersionContext()` and `UpdateTableVersion()` calls for barrier-coordinated CDC
2. **Removed** `SyncChildIndexes()` call from the main flow
3. **Simple increment** only: `table->AlterVersion += 1`
4. **Delegated synchronization** to dedicated `CdcVersionSync` part that runs after barrier completion
5. **Backward compatible**: Non-coordinated CDC still works with simple increment

### 4.4 Step 4: Create CdcVersionSync Operation

**New files needed:**
1. `ydb/core/tx/schemeshard/schemeshard__operation_cdc_version_sync.h`
2. `ydb/core/tx/schemeshard/schemeshard__operation_cdc_version_sync.cpp`

#### Header File

```cpp
#pragma once

#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_path_element.h"

namespace NKikimr::NSchemeShard {

ISubOperation::TPtr CreateCdcVersionSync(
    TOperationId id,
    TPathId tablePathId,
    const TString& barrierName
);

}  // namespace NKikimr::NSchemeShard
```

#### Implementation File

```cpp
#include "schemeshard__operation_cdc_version_sync.h"
#include "schemeshard_impl.h"
#include "schemeshard_path.h"

namespace NKikimr::NSchemeShard {

namespace {

class TWaitBarrier: public TSubOperationState {
private:
    TOperationId OperationId;
    TPathId TablePathId;
    TString BarrierName;
    bool BarrierRegistered = false;
    
    TString DebugHint() const override {
        return TStringBuilder()
            << "CdcVersionSync TWaitBarrier"
            << " operationId: " << OperationId
            << " tablePathId: " << TablePathId
            << " barrier: " << BarrierName;
    }
    
public:
    TWaitBarrier(TOperationId id, TPathId tablePathId, const TString& barrierName)
        : OperationId(id)
        , TablePathId(tablePathId)
        , BarrierName(barrierName)
    {
        // Ignore messages we don't care about
        IgnoreMessages(DebugHint(), {
            TEvHive::TEvCreateTabletReply::EventType,
            TEvDataShard::TEvProposeTransactionResult::EventType,
            TEvPrivate::TEvOperationPlan::EventType
        });
    }
    
    bool HandleReply(TEvPrivate::TEvCompleteBarrier::TPtr& ev,
                     TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     DebugHint() << " HandleReply TEvCompleteBarrier");
        
        Y_ABORT_UNLESS(ev->Get()->TxId == OperationId.GetTxId());
        Y_ABORT_UNLESS(ev->Get()->Name == BarrierName);
        
        NIceDb::TNiceDb db(context.GetDB());
        
        // All CDC parts have reached barrier and completed
        // Now perform atomic version synchronization
        TVector<TPathId> affectedPaths;
        CollectAffectedPaths(TablePathId, context, affectedPaths);
        
        ui64 maxVersion = FindMaxVersion(affectedPaths, context);
        
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " Performing atomic version sync"
                   << ", affectedPaths: " << affectedPaths.size()
                   << ", maxVersion: " << maxVersion);
        
        // Sync ALL paths to max version in single DB transaction
        SyncAllVersions(affectedPaths, maxVersion, context, db);
        
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     DebugHint() << " Version sync complete");
        
        // Mark operation as done
        context.OnComplete.DoneOperation(OperationId);
        return true;
    }
    
    bool ProgressState(TOperationContext& context) override {
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState");
        
        if (!BarrierRegistered) {
            // Register this sync part at the SAME barrier as CDC parts
            // This ensures we're notified when all CDC parts complete
            context.OnComplete.Barrier(OperationId, BarrierName);
            BarrierRegistered = true;
            
            LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                       DebugHint() << " Registered at barrier");
        }
        
        // Wait for TEvCompleteBarrier callback
        return false;
    }
    
private:
    void CollectAffectedPaths(TPathId tablePathId,
                             TOperationContext& context,
                             TVector<TPathId>& out) const {
        // Add main table
        if (context.SS->Tables.contains(tablePathId)) {
            out.push_back(tablePathId);
        }
        
        // Find all indexes and their impl tables
        if (!context.SS->PathsById.contains(tablePathId)) {
            return;
        }
        
        auto tablePath = context.SS->PathsById[tablePathId];
        for (const auto& [childName, childPathId] : tablePath->GetChildren()) {
            auto childPath = context.SS->PathsById.at(childPathId);
            
            // Skip non-indexes
            if (!childPath->IsTableIndex()) {
                continue;
            }
            
            // Skip dropped
            if (childPath->Dropped()) {
                continue;
            }
            
            // Add index entity
            out.push_back(childPathId);
            
            // Add impl table
            Y_ABORT_UNLESS(childPath->GetChildren().size() == 1);
            auto implTablePathId = childPath->GetChildren().begin()->second;
            out.push_back(implTablePathId);
        }
        
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " Collected affected paths: " << out.size());
    }
    
    ui64 FindMaxVersion(const TVector<TPathId>& paths,
                       TOperationContext& context) const {
        ui64 maxVersion = 0;
        
        for (auto pathId : paths) {
            if (context.SS->Tables.contains(pathId)) {
                auto table = context.SS->Tables[pathId];
                maxVersion = Max(maxVersion, table->AlterVersion);
                
                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                           DebugHint() << " Table version"
                           << ", pathId: " << pathId
                           << ", version: " << table->AlterVersion);
            }
            
            if (context.SS->Indexes.contains(pathId)) {
                auto index = context.SS->Indexes[pathId];
                maxVersion = Max(maxVersion, index->AlterVersion);
                
                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                           DebugHint() << " Index version"
                           << ", pathId: " << pathId
                           << ", version: " << index->AlterVersion);
            }
        }
        
        return maxVersion;
    }
    
    void SyncAllVersions(const TVector<TPathId>& paths,
                        ui64 targetVersion,
                        TOperationContext& context,
                        NIceDb::TNiceDb& db) const {
        for (auto pathId : paths) {
            if (context.SS->Tables.contains(pathId)) {
                auto table = context.SS->Tables[pathId];
                ui64 oldVersion = table->AlterVersion;
                
                if (table->AlterVersion != targetVersion) {
                    table->AlterVersion = targetVersion;
                    context.SS->PersistTableAlterVersion(db, pathId, table);
                    
                    auto path = context.SS->PathsById[pathId];
                    context.SS->ClearDescribePathCaches(path);
                    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
                    
                    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                              DebugHint() << " Synced table version"
                              << ", pathId: " << pathId
                              << ", oldVersion: " << oldVersion
                              << ", newVersion: " << targetVersion);
                }
            }
            
            if (context.SS->Indexes.contains(pathId)) {
                auto index = context.SS->Indexes[pathId];
                ui64 oldVersion = index->AlterVersion;
                
                if (index->AlterVersion != targetVersion) {
                    index->AlterVersion = targetVersion;
                    context.SS->PersistTableIndexAlterVersion(db, pathId, index);
                    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
                    
                    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                              DebugHint() << " Synced index version"
                              << ", pathId: " << pathId
                              << ", oldVersion: " << oldVersion
                              << ", newVersion: " << targetVersion);
                }
            }
        }
    }
};

class TCdcVersionSync: public TSubOperation {
private:
    TPathId TablePathId;
    TString BarrierName;
    
    static TTxState::ETxState NextState() {
        return TTxState::Done;
    }
    
    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
            return NextState();
        default:
            return TTxState::Invalid;
        }
    }
    
    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
            return MakeHolder<TWaitBarrier>(OperationId, TablePathId, BarrierName);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }
    
public:
    TCdcVersionSync(TOperationId id, TPathId tablePathId, const TString& barrierName)
        : TSubOperation(id)
        , TablePathId(tablePathId)
        , BarrierName(barrierName)
    {
    }
    
    TCdcVersionSync(TOperationId id, TTxState::ETxState state)
        : TSubOperation(id)
    {
        SetState(SelectStateFunc(state));
    }
    
    THolder<TProposeResponse> Propose(const TString& owner,
                                     TOperationContext& context) override {
        const auto* table = context.SS->Tables.FindPtr(TablePathId);
        
        if (!table) {
            return MakeHolder<TEvSchemeShard::TEvModifySchemeTransactionResult>(
                NKikimrScheme::StatusPathDoesNotExist,
                TStringBuilder() << "Table not found: " << TablePathId
            );
        }
        
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "CdcVersionSync Propose"
                     << ", operationId: " << OperationId
                     << ", tablePathId: " << TablePathId
                     << ", barrier: " << BarrierName);
        
        // Create TxState
        NIceDb::TNiceDb db(context.GetDB());
        
        auto& txState = context.SS->CreateTx(
            OperationId,
            TTxState::TxCdcVersionSync,  // New tx type
            TablePathId
        );
        txState.State = TTxState::Waiting;
        txState.MinStep = TStepId(0);
        
        context.SS->PersistTxState(db, OperationId);
        
        // Don't activate yet - will activate after barrier
        context.OnComplete.RouteByTabletsFromOperation(OperationId);
        
        SetState(SelectStateFunc(TTxState::Waiting));
        
        return MakeHolder<TProposeResponse>(
            NKikimrScheme::StatusAccepted,
            ui64(OperationId.GetTxId()),
            ui64(context.SS->SelfTabletId())
        );
    }
    
    void AbortPropose(TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "CdcVersionSync AbortPropose"
                     << ", operationId: " << OperationId);
    }
    
    void AbortUnsafe(TTxId txId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "CdcVersionSync AbortUnsafe"
                     << ", operationId: " << OperationId);
        
        context.OnComplete.DoneOperation(OperationId);
    }
};

}  // namespace anonymous

ISubOperation::TPtr CreateCdcVersionSync(TOperationId id,
                                        TPathId tablePathId,
                                        const TString& barrierName) {
    return MakeSubOperation<TCdcVersionSync>(id, tablePathId, barrierName);
}

ISubOperation::TPtr CreateCdcVersionSync(TOperationId id,
                                        TTxState::ETxState state) {
    return MakeSubOperation<TCdcVersionSync>(id, state);
}

}  // namespace NKikimr::NSchemeShard
```

### 4.5 Step 5: Register New Transaction Type

**File:** `ydb/core/tx/schemeshard/schemeshard_subop_types.h`

Add to enum:

```cpp
#define TX_STATE_TYPE_ENUM(item) \
    item(TxInvalid, 0) \
    // ... existing types ...
    item(TxCdcVersionSync, 117) \  // ← New type (append to end!)
```

**File:** `ydb/core/tx/schemeshard/schemeshard__operation.cpp`

Add to `RestorePart`:

```cpp
ISubOperation::TPtr TOperation::RestorePart(TTxState::ETxType txType,
                                           TTxState::ETxState txState,
                                           TOperationContext& context) const {
    switch (txType) {
    // ... existing cases ...
    case TTxState::TxCdcVersionSync:
        return CreateCdcVersionSync(NextPartId(), txState);
    }
}
```

### 4.6 Step 6: Database Schema Changes

**File:** `ydb/core/tx/schemeshard/schemeshard_schema.h`

Add columns to `TxInFlightV2` table:

```cpp
struct TxInFlightV2 : NIceDb::Schema::Table<100> {
    // ... existing columns ...
    
    struct UseBarrierCoordination : Column<25, NScheme::NTypeIds::Bool> {};
    struct BarrierName : Column<26, NScheme::NTypeIds::Utf8> {};
    
    using TKey = TableKey<TxId, SubTxId>;
    using TColumns = TableColumns<
        TxId, SubTxId, TxType, State, TargetPathId, // ... existing ...
        UseBarrierCoordination, BarrierName  // ← New
    >;
};
```

**File:** `ydb/core/tx/schemeshard/schemeshard_impl.cpp`

Persist new fields:

```cpp
void TSchemeShard::PersistTxState(NIceDb::TNiceDb& db, const TOperationId& opId) {
    const TTxState* txState = FindTx(opId);
    Y_ABORT_UNLESS(txState);
    
    db.Table<Schema::TxInFlightV2>()
      .Key(opId.GetTxId(), opId.GetSubTxId())
      .Update(
          NIceDb::TUpdate<Schema::TxInFlightV2::TxType>(txState->TxType),
          NIceDb::TUpdate<Schema::TxInFlightV2::State>(txState->State),
          // ... existing fields ...
          NIceDb::TUpdate<Schema::TxInFlightV2::UseBarrierCoordination>(
              txState->UseBarrierCoordination),
          NIceDb::TUpdate<Schema::TxInFlightV2::BarrierName>(
              txState->BarrierName)
      );
}
```

Load during restore:

```cpp
void TSchemeShard::TTxInit::LoadTxInFlightV2() {
    // ... existing load logic ...
    
    if (rowset.HaveValue<Schema::TxInFlightV2::UseBarrierCoordination>()) {
        txState.UseBarrierCoordination = 
            rowset.GetValue<Schema::TxInFlightV2::UseBarrierCoordination>();
    }
    
    if (rowset.HaveValue<Schema::TxInFlightV2::BarrierName>()) {
        txState.BarrierName = 
            rowset.GetValue<Schema::TxInFlightV2::BarrierName>();
    }
}
```

---

## 5. Testing Strategy

### 5.1 Unit Tests

**Test File:** `ydb/core/tx/schemeshard/ut_cdc_version_sync/ut_cdc_version_sync.cpp`

#### Test 1: Single Table, No Indexes

```cpp
Y_UNIT_TEST(SingleTableNoIndexes) {
    // Verify no barrier created when no indexes exist
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    CreateTable(env, "/MyRoot/table1");
    
    // Create backup collection
    auto opId = CreateIncrementalBackup(env, "/MyRoot/table1");
    
    // Verify operation completes without barrier
    auto operation = env.GetSchemeShard()->Operations.at(opId);
    UNIT_ASSERT_VALUES_EQUAL(operation->Parts.size(), 2); // CDC + tracker, no sync
    UNIT_ASSERT(!operation->HasBarrier());
}
```

#### Test 2: Table with 1 Index

```cpp
Y_UNIT_TEST(TableWithOneIndex) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    CreateTableWithIndex(env, "/MyRoot/table1", "index1");
    
    // Capture initial versions
    auto table = GetTable(env, "/MyRoot/table1");
    auto index = GetIndex(env, "/MyRoot/table1/index1");
    auto implTable = GetTable(env, "/MyRoot/table1/index1/indexImplTable");
    
    ui64 initialVersion = table->AlterVersion;
    
    // Create incremental backup
    auto opId = CreateIncrementalBackup(env, "/MyRoot/table1");
    
    // Verify operation has CDC parts + sync part
    auto operation = env.GetSchemeShard()->Operations.at(opId);
    UNIT_ASSERT_VALUES_EQUAL(operation->Parts.size(), 4);
    // Part 0: CDC for table
    // Part 1: CDC for indexImplTable
    // Part 2: CdcVersionSync
    // Part 3: LongBackupTracker
    
    // Wait for completion
    WaitForOperation(env, opId);
    
    // Verify all versions synchronized
    table = GetTable(env, "/MyRoot/table1");
    index = GetIndex(env, "/MyRoot/table1/index1");
    implTable = GetTable(env, "/MyRoot/table1/index1/indexImplTable");
    
    UNIT_ASSERT_VALUES_EQUAL(index->AlterVersion, implTable->AlterVersion);
    UNIT_ASSERT_C(index->AlterVersion > initialVersion, 
                 "Version should have incremented");
}
```

#### Test 3: Table with Multiple Indexes

```cpp
Y_UNIT_TEST(TableWithMultipleIndexes) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    CreateTableWithIndexes(env, "/MyRoot/table1", {"index1", "index2", "index3"});
    
    auto opId = CreateIncrementalBackup(env, "/MyRoot/table1");
    WaitForOperation(env, opId);
    
    // Verify all indexes and impl tables have same version
    auto index1 = GetIndex(env, "/MyRoot/table1/index1");
    auto index2 = GetIndex(env, "/MyRoot/table1/index2");
    auto index3 = GetIndex(env, "/MyRoot/table1/index3");
    
    auto impl1 = GetTable(env, "/MyRoot/table1/index1/indexImplTable");
    auto impl2 = GetTable(env, "/MyRoot/table1/index2/indexImplTable");
    auto impl3 = GetTable(env, "/MyRoot/table1/index3/indexImplTable");
    
    ui64 expectedVersion = index1->AlterVersion;
    
    UNIT_ASSERT_VALUES_EQUAL(index2->AlterVersion, expectedVersion);
    UNIT_ASSERT_VALUES_EQUAL(index3->AlterVersion, expectedVersion);
    UNIT_ASSERT_VALUES_EQUAL(impl1->AlterVersion, expectedVersion);
    UNIT_ASSERT_VALUES_EQUAL(impl2->AlterVersion, expectedVersion);
    UNIT_ASSERT_VALUES_EQUAL(impl3->AlterVersion, expectedVersion);
}
```

#### Test 4: Concurrent CDC Operations

```cpp
Y_UNIT_TEST(ConcurrentCdcOperations) {
    // Test that multiple tables can have barriers simultaneously
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    CreateTableWithIndexes(env, "/MyRoot/table1", {"index1", "index2"});
    CreateTableWithIndexes(env, "/MyRoot/table2", {"index1", "index2"});
    
    // Start both backups concurrently
    auto opId1 = CreateIncrementalBackup(env, "/MyRoot/table1", false); // no wait
    auto opId2 = CreateIncrementalBackup(env, "/MyRoot/table2", false);
    
    // Wait for both
    WaitForOperation(env, opId1);
    WaitForOperation(env, opId2);
    
    // Verify both tables' indexes are synced
    VerifyIndexVersionsSync(env, "/MyRoot/table1");
    VerifyIndexVersionsSync(env, "/MyRoot/table2");
}
```

#### Test 5: Crash Recovery

```cpp
Y_UNIT_TEST(CrashDuringBarrier) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    CreateTableWithIndexes(env, "/MyRoot/table1", {"index1", "index2"});
    
    // Start backup
    auto opId = CreateIncrementalBackup(env, "/MyRoot/table1", false);
    
    // Wait for CDC parts to register at barrier
    WaitForBarrierRegistration(env, opId);
    
    // Simulate crash
    RestartSchemeShard(env);
    
    // Verify operation continues after restore
    WaitForOperation(env, opId);
    
    // Verify versions still synchronized correctly
    VerifyIndexVersionsSync(env, "/MyRoot/table1");
}
```

### 5.2 Integration Tests

**Test File:** `ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`

#### Test 6: Full Backup/Restore Cycle

```cpp
Y_UNIT_TEST(BackupRestoreWithIndexes) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    // Create table with data
    CreateTableWithIndexes(env, "/MyRoot/table1", {"index1", "index2"});
    InsertRows(env, "/MyRoot/table1", 1000);
    
    // Take incremental backup
    auto backupPath = CreateIncrementalBackup(env, "/MyRoot/table1");
    
    // Verify backup includes index data
    VerifyBackupIncludes(backupPath, {"table1", "index1", "index2"});
    
    // Drop original table
    DropTable(env, "/MyRoot/table1");
    
    // Restore from backup
    RestoreFromBackup(env, backupPath, "/MyRoot/table1_restored");
    
    // Verify restored table
    auto table = GetTable(env, "/MyRoot/table1_restored");
    auto index1 = GetIndex(env, "/MyRoot/table1_restored/index1");
    auto index2 = GetIndex(env, "/MyRoot/table1_restored/index2");
    
    // Check versions synchronized
    VerifyIndexVersionsSync(env, "/MyRoot/table1_restored");
    
    // Verify data integrity
    VerifyRowCount(env, "/MyRoot/table1_restored", 1000);
    VerifyIndexData(env, "/MyRoot/table1_restored/index1");
}
```

### 5.3 Validation Functions

```cpp
void VerifyIndexVersionsSync(TTestEnv& env, const TString& tablePath) {
    auto table = GetTable(env, tablePath);
    auto tableName = TPath::Parse(tablePath).back();
    
    TVector<TString> indexes = GetIndexNames(env, tablePath);
    
    ui64 baseVersion = 0;
    bool first = true;
    
    for (const auto& indexName : indexes) {
        auto indexPath = tablePath + "/" + indexName;
        auto index = GetIndex(env, indexPath);
        auto implTable = GetTable(env, indexPath + "/indexImplTable");
        
        // Verify index and impl table match
        UNIT_ASSERT_VALUES_EQUAL_C(
            index->AlterVersion, implTable->AlterVersion,
            "Index and impl table versions must match: " << indexPath
        );
        
        // Verify all indexes have same version
        if (first) {
            baseVersion = index->AlterVersion;
            first = false;
        } else {
            UNIT_ASSERT_VALUES_EQUAL_C(
                index->AlterVersion, baseVersion,
                "All indexes must have same version: " << indexPath
            );
        }
    }
}
```

### 5.4 Performance Tests

#### Test 7: Many Indexes Performance

```cpp
Y_UNIT_TEST(ManyIndexesPerformance) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    
    // Create table with 10 indexes
    TVector<TString> indexNames;
    for (int i = 0; i < 10; ++i) {
        indexNames.push_back(TStringBuilder() << "index" << i);
    }
    CreateTableWithIndexes(env, "/MyRoot/table1", indexNames);
    
    auto startTime = TInstant::Now();
    
    // Take incremental backup
    auto opId = CreateIncrementalBackup(env, "/MyRoot/table1");
    WaitForOperation(env, opId);
    
    auto duration = TInstant::Now() - startTime;
    
    Cerr << "Backup with 10 indexes took: " << duration << Endl;
    
    // Verify versions
    VerifyIndexVersionsSync(env, "/MyRoot/table1");
    
    // Ensure reasonable performance (adjust threshold as needed)
    UNIT_ASSERT_C(duration < TDuration::Seconds(30),
                 "Backup took too long: " << duration);
}
```

---

## 6. Risk Analysis and Mitigation

### 6.1 Implementation Risks

#### Risk 1: Barrier Constraint Violation

**Description:** Only one barrier allowed per operation. If CDC operations already use barriers elsewhere, adding another will fail.

**Likelihood:** Low (current CDC operations don't use barriers)

**Impact:** High (operation will abort)

**Mitigation:**
1. Audit all CDC-related operations for existing barrier usage
2. Add assertions in development build to catch violations early
3. If barrier conflict detected, fall back to sequential sync

#### Risk 2: Database Schema Migration

**Description:** Adding new columns to `TxInFlightV2` requires database migration during upgrade.

**Likelihood:** Certain

**Impact:** Medium (requires careful upgrade handling)

**Mitigation:**
1. Make new columns optional (nullable)
2. Check column existence before reading: `rowset.HaveValue<Column>()`
3. Default to `UseBarrierCoordination = false` for old operations
4. Test upgrade from previous version

#### Risk 3: Increased Latency

**Description:** Barrier adds synchronization overhead, potentially slowing backup operations.

**Likelihood:** High

**Impact:** Low-Medium (acceptable trade-off for correctness)

**Mitigation:**
1. Measure latency impact in performance tests
2. Optimize CdcVersionSync to execute quickly
3. Consider batching multiple version updates
4. Document expected latency increase

#### Risk 4: Partial Failure During Sync

**Description:** If CdcVersionSync crashes mid-execution, versions might be partially synced.

**Likelihood:** Low (database transaction ensures atomicity)

**Impact:** Medium (temporary inconsistency until retry)

**Mitigation:**
1. All version updates in single database transaction
2. Operation will retry from `Waiting` state on crash
3. Version sync is idempotent (safe to re-execute)
4. Add extensive logging for debugging

#### Risk 5: Version Sync Part Completion Before CDC Parts

**Description:** If version sync part completes before CDC parts register at barrier, barrier logic fails.

**Likelihood:** Very Low (sync part waits for `TEvCompleteBarrier`)

**Impact:** High (operation stuck)

**Mitigation:**
1. Version sync part doesn't register at barrier itself
2. Only handles `TEvCompleteBarrier` event
3. Add timeout detection for stuck barriers
4. Log warning if sync part activated before barrier complete

### 6.2 Operational Risks

#### Risk 6: Increased Operation Complexity

**Description:** More parts per operation makes debugging harder.

**Likelihood:** Certain

**Impact:** Low (manageable with good logging)

**Mitigation:**
1. Add detailed logging at each stage
2. Include barrier name and state in all log messages
3. Create debugging guide for operations team
4. Add metrics for barrier wait time

#### Risk 7: Barrier Memory Usage

**Description:** Barriers hold part IDs in memory, could grow large with many indexes.

**Likelihood:** Low (typical tables have <10 indexes)

**Impact:** Low (minimal memory overhead)

**Mitigation:**
1. Use `TSet<TSubTxId>` (efficient storage)
2. Clean up barriers immediately after completion
3. Add monitoring for barrier size

### 6.3 Rollback Plan

If Strategy A implementation causes issues in production:

**Step 1: Quick Disable**

```cpp
// Add feature flag
bool UseBarrierCoordinationForCdc() {
    return AppData()->FeatureFlags.GetEnableCdcBarrierCoordination();
}

// In backup collection creation:
if (UseBarrierCoordinationForCdc() && cdcParts.size() > 1) {
    // Add barrier coordination
} else {
    // Fall back to old behavior
}
```

**Step 2: Partial Rollback**

Keep barrier code but disable for specific scenarios:
- Only use for tables with >N indexes
- Only use for restore operations (not backup)
- Only use when explicitly requested

**Step 3: Full Rollback**

1. Remove `UseBarrierCoordination` checks
2. Remove `CdcVersionSync` part creation
3. Revert to old `TProposeAtTable::HandleReply` logic
4. Database schema columns remain (unused but harmless)

---

## 7. Alternative Implementations Within Strategy A

### 7.1 Variation 1: Single Barrier for All Tables

**Current Design:** One barrier per table.

**Alternative:** One global barrier for entire backup operation.

**Pros:**
- Simpler coordination
- One sync part for all tables
- Less barrier overhead

**Cons:**
- Tables without indexes wait unnecessarily
- Less parallelism
- All-or-nothing synchronization

**Recommendation:** Keep per-table barriers for better parallelism.

### 7.2 Variation 2: Sync Part Registers at Barrier

**Current Design:** Sync part waits for `TEvCompleteBarrier`, doesn't register.

**Alternative:** Sync part also registers at barrier.

**Implementation:**

```cpp
bool TWaitBarrier::ProgressState(TOperationContext& context) override {
    // Register at barrier too
    context.OnComplete.Barrier(OperationId, BarrierName);
    return false;
}

bool TWaitBarrier::HandleReply(TEvPrivate::TEvCompleteBarrier::TPtr& ev,
                               TOperationContext& context) override {
    // Now sync part is also blocked
    // Do version sync
    // Complete
}
```

**Barrier completion logic:**

```cpp
// blocked = 4 (3 CDC + 1 sync)
// done = 0
// total = 4
// 4 + 0 == 4 → Barrier complete
```

**Pros:**
- Explicit participation in barrier
- Clearer barrier membership

**Cons:**
- More complex (sync part in barrier set)
- Same functional outcome

**Recommendation:** Current design (sync part as observer) is cleaner.

### 7.3 Variation 3: Two-Phase Barrier

**Concept:** Two barriers:
1. Phase 1: CDC parts register, sync part waits
2. Phase 2: After sync, all parts wait before completing

**Benefits:**
- Additional synchronization point
- Can add post-sync validation

**Drawbacks:**
- Violates "one barrier at a time" constraint
- More complexity for little benefit

**Recommendation:** Not needed for version sync use case.

### 7.4 Variation 4: Optimistic Sync Without Barrier

**Concept:** Each CDC part syncs like before, but uses compare-and-swap logic.

**Implementation:**

```cpp
do {
    ui64 currentMax = ReadMaxVersionAcrossAllIndexes();
    ui64 newMax = currentMax + 1;
    
    success = AtomicCompareAndSwap(allVersions, currentMax, newMax);
} while (!success);
```

**Pros:**
- No barrier needed
- Lock-free approach

**Cons:**
- Complex implementation in SchemeShard
- Database doesn't support compare-and-swap primitives
- Retry loops could thrash

**Recommendation:** Barrier approach is more reliable.

---

## 8. Detailed Code Walkthrough

### 8.1 Operation Lifecycle with Barrier

Let's trace a complete operation from start to finish.

#### Initial State

```
Table: /MyRoot/table1
  AlterVersion = 10
  Indexes:
    index1 → indexImplTable1 (version = 10)
    index2 → indexImplTable2 (version = 10)
```

#### Step 1: User Initiates Backup

```
User → SchemeShard: CreateIncrementalBackup("/MyRoot/table1")
```

#### Step 2: Operation Construction

```cpp
CreateBackupIncrementalBackupCollection():
  cdcPartsByTable[table1] = [
    CreateCdcStream(table1),         // Part 0
    CreateCdcStream(indexImpl1),     // Part 1  
    CreateCdcStream(indexImpl2)      // Part 2
  ]
  
  result.push_back(CreateCdcVersionSync(
    NextPartId(opId, result),        // Part 3
    table1,
    "cdc_version_sync_100"           // barrier name
  ))
  
  result.push_back(CreateLongBackupTracker(...))  // Part 4
  
  return result
```

**Operation Structure:**

```
TOperation[TxId=1000]
  Parts = [
    Part 0: CreateCdcStream(table1)
    Part 1: CreateCdcStream(indexImpl1)
    Part 2: CreateCdcStream(indexImpl2)
    Part 3: CdcVersionSync(table1, "cdc_version_sync_100")
    Part 4: LongBackupTracker
  ]
  Barriers = {}
  DoneParts = {}
```

#### Step 3: Part Proposal

Each part's `Propose()` called:

```
Part 0 Propose():
  - Create TTxState(type=TxAlterContinuousBackup, state=ConfigureParts)
  - Set txState.UseBarrierCoordination = true
  - Set txState.BarrierName = "cdc_version_sync_100"
  - Persist to DB
  - Return StatusAccepted

Part 1 Propose(): (same)
Part 2 Propose(): (same)

Part 3 Propose():
  - Create TTxState(type=TxCdcVersionSync, state=Waiting)
  - Set barrier name in txState (will register at barrier in ProgressState)
  - Return StatusAccepted

Part 4 Propose(): (long backup logic)
```

#### Step 4: Activation

```
TEvProgressOperation sent to all parts
```

#### Step 5: CDC Parts Execute (Parallel)

**Part 0 (table1 CDC):**

```
ConfigureParts:
  - Send TEvProposeTransaction to datashards
  - Wait for TEvProposeTransactionResult
  → Advance to Propose

Propose:
  - Send TEvProposeTransaction to coordinator
  - Wait for TEvOperationPlan
  → HandleReply(TEvOperationPlan):
      - table1.AlterVersion = 10 → 11
      - context.OnComplete.Barrier(Part0, "cdc_version_sync_100")
      - Advance to ProposedWaitParts
```

**Part 1 (indexImpl1 CDC) - parallel:**

```
Propose HandleReply:
  - indexImpl1.AlterVersion = 10 → 11
  - context.OnComplete.Barrier(Part1, "cdc_version_sync_100")
  - Advance to ProposedWaitParts
```

**Part 2 (indexImpl2 CDC) - parallel:**

```
Propose HandleReply:
  - indexImpl2.AlterVersion = 10 → 11
  - context.OnComplete.Barrier(Part2, "cdc_version_sync_100")
  - Advance to ProposedWaitParts
```

#### Step 6: Barrier Registration

```
TSideEffects::DoRegisterBarriers():
  operation->RegisterBarrier(0, "cdc_version_sync_100")
  operation->RegisterBarrier(1, "cdc_version_sync_100")
  operation->RegisterBarrier(2, "cdc_version_sync_100")

TOperation[TxId=1000]:
  Barriers = {
    "cdc_version_sync_100": {0, 1, 2}
  }
  DoneParts = {}
```

#### Step 7: CDC Parts Continue (Not Blocked Yet)

CDC parts continue to `ProposedWaitParts` state:

```
Part 0 ProposedWaitParts:
  - Wait for TEvSchemaChanged from datashards
  → HandleReply(TEvSchemaChanged):
      - context.OnComplete.DoneOperation(Part0)

(Same for Part 1, Part 2)
```

**Key Point:** Parts don't block immediately upon barrier registration. They continue their workflow and eventually mark as done.

#### Step 8: Barrier Completion Detection

When all CDC parts reach done:

```
TSideEffects::DoCheckBarriers():
  touchedOperations = {1000}
  
  operation = Operations[1000]
  
  operation->IsDoneBarrier():
    blockedParts = {0, 1, 2}
    DoneParts = {0, 1, 2, 4}  // CDC parts + long backup
    total = 5
    blockedParts.size() + DoneParts.size() = 3 + 2 = 5 ✓
    → TRUE
  
  Create TEvCompleteBarrier("cdc_version_sync_100")
  
  For partId in {0, 1, 2}:
    operation->Parts[partId]->HandleReply(TEvCompleteBarrier, context)
```

#### Barrier Notification - Important Clarification

**Initial Question**: Parts 0, 1, 2 are sent `TEvCompleteBarrier` but they're CDC parts - do they handle this event?

**Answer**: No - CDC parts do NOT implement `HandleReply(TEvCompleteBarrier)`. The event is sent to all parts in the barrier set by `DoCheckBarriers()`, but only parts that have implemented the handler will respond.

**Key Insight**: This is why the sync part (Part 3) must ALSO register at the same barrier. When it registers:
- Part 3 gets added to the barrier set: `Barriers["cdc_sync"] = {0, 1, 2, 3}`
- When all parts complete: `blocked={3}, done={0,1,2,4}, total=5 → 1+4=5 ✓`
- Part 3 receives `TEvCompleteBarrier` and handles it

**Pattern Verification**: This is exactly how drop-indexed-table works:
- Multiple drop-index parts register at barrier
- TDeletePathBarrier part also registers at same barrier
- When complete, TDeletePathBarrier::HandleReply(TEvCompleteBarrier) executes

```
Part 0 (CDC): Register barrier → ProposedWaitParts → Done
Part 1 (CDC): Register barrier → ProposedWaitParts → Done  
Part 2 (CDC): Register barrier → ProposedWaitParts → Done
Part 3 (Sync): Register barrier → WaitBarrier (waiting)

Barrier state during execution:
  T1-T3: Barriers["cdc_sync"] = {0, 1, 2, 3}  (all parts registered)
  T4-T6: Parts 0,1,2 → ProposedWaitParts → DoneParts
  
When Parts 0, 1, 2 reach done:
  blocked = {3}           (Part 3 still registered, not done)
  done = {0, 1, 2, 4}     (CDC parts + tracker done)
  total = 5
  
  IsDoneBarrier(): 1 + 4 = 5 ✓ TRUE
  
  DoCheckBarriers() sends TEvCompleteBarrier to all blocked parts {3}
  
Part 3 receives TEvCompleteBarrier:
  Part 3 HandleReply(TEvCompleteBarrier):
    - Collect all affected paths (table + indexes + impl tables)
    - Find maximum version across all
    - Sync ALL to maximum in single DB transaction
    - Mark as done
    
Final state: All versions consistent ✓
```

**Design Benefits**:
- Sync part is explicit participant in barrier (not an observer)
- Guaranteed ordering: CDC parts → barrier complete → sync part notified
- Follows proven pattern from drop-indexed-table operation
- Clear separation: CDC parts increment, sync part synchronizes

---

## 9. Implementation Checklist

### 9.1 Code Changes

- [ ] **File 1:** `schemeshard__operation_backup_incremental_backup_collection.cpp`
  - [ ] Group CDC parts by table
  - [ ] Add `CreateCdcVersionSync` call for tables with indexes
  - [ ] Pass barrier name to CDC creation

- [ ] **File 2:** `schemeshard__operation_common_cdc_stream.cpp`
  - [ ] Modify `TProposeAtTable::HandleReply`
  - [ ] Check `UseBarrierCoordination` flag
  - [ ] Register at barrier instead of syncing

- [ ] **File 3:** `schemeshard__operation_cdc_version_sync.h` (new)
  - [ ] Declare `CreateCdcVersionSync` function
  - [ ] Add necessary includes

- [ ] **File 4:** `schemeshard__operation_cdc_version_sync.cpp` (new)
  - [ ] Implement `TWaitBarrier` state
  - [ ] Implement `TCdcVersionSync` operation
  - [ ] Implement `CollectAffectedPaths`
  - [ ] Implement `FindMaxVersion`
  - [ ] Implement `SyncAllVersions`

- [ ] **File 5:** `schemeshard_subop_types.h`
  - [ ] Add `TxCdcVersionSync` to enum (append to end!)

- [ ] **File 6:** `schemeshard__operation.cpp`
  - [ ] Add case for `TxCdcVersionSync` in `RestorePart`

- [ ] **File 7:** `schemeshard_schema.h`
  - [ ] Add `UseBarrierCoordination` column to `TxInFlightV2`
  - [ ] Add `BarrierName` column to `TxInFlightV2`

- [ ] **File 8:** `schemeshard_impl.cpp`
  - [ ] Update `PersistTxState` to save new fields
  - [ ] Update `LoadTxInFlightV2` to load new fields

- [ ] **File 9:** `schemeshard_tx_infly.h`
  - [ ] Add `UseBarrierCoordination` field to `TTxState`
  - [ ] Add `BarrierName` field to `TTxState`

### 9.2 Testing

- [ ] **Unit Tests:** `ut_cdc_version_sync.cpp` (new)
  - [ ] Test: Single table, no indexes
  - [ ] Test: Table with 1 index
  - [ ] Test: Table with 3 indexes
  - [ ] Test: Table with 10 indexes
  - [ ] Test: Concurrent operations on multiple tables
  - [ ] Test: Crash recovery during barrier

- [ ] **Integration Tests:** `datashard_ut_incremental_backup.cpp`
  - [ ] Test: Full backup/restore cycle with indexes
  - [ ] Test: Version verification after backup
  - [ ] Test: Query execution after restore

- [ ] **Performance Tests:**
  - [ ] Measure latency with vs without barrier
  - [ ] Test with varying number of indexes (1, 3, 10, 20)
  - [ ] Benchmark version sync execution time

### 9.3 Documentation

- [ ] Update contributor docs with barrier pattern
- [ ] Document new transaction type
- [ ] Add troubleshooting guide for barrier issues
- [ ] Update operations manual

### 9.4 Deployment

- [ ] Feature flag for gradual rollout
- [ ] Monitoring dashboards for barrier metrics
- [ ] Alert rules for stuck barriers
- [ ] Rollback procedure documented

---

## 10. References and Appendices

### 10.1 Key File Locations

**Barrier Infrastructure:**
- `ydb/core/tx/schemeshard/schemeshard__operation.h:119-146` - Barrier methods
- `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:1086-1141` - DoCheckBarriers
- `ydb/core/tx/schemeshard/schemeshard_private.h:60-70` - TEvCompleteBarrier

**CDC Creation:**
- `ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp:155-302`
- `ydb/core/tx/schemeshard/schemeshard__operation_create_cdc_stream.cpp`
- `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp:447-479`

**Version Sync Logic:**
- `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp:94-248`
  - BuildTableVersionContext (94-113)
  - SyncImplTableVersion (115-173)
  - UpdateTableVersion (175-248)

**Example Barrier Usage:**
- `ydb/core/tx/schemeshard/schemeshard__operation_drop_indexed_table.cpp:187-241`

### 10.2 Data Structure Definitions

**TOperation:**
```cpp
struct TOperation {
    const TTxId TxId;
    TVector<ISubOperation::TPtr> Parts;
    TSet<TSubTxId> DoneParts;
    THashMap<TString, TSet<TSubTxId>> Barriers;
    
    void RegisterBarrier(TSubTxId, const TString&);
    bool IsDoneBarrier() const;
    void DropBarrier(const TString&);
};
```

**TTxState:**
```cpp
struct TTxState {
    ETxType TxType;
    ETxState State;
    TPathId TargetPathId;
    TVector<TShard> Shards;
    ui64 MinStep;
    ui64 PlanStep;
    
    // New fields for barrier coordination:
    bool UseBarrierCoordination;
    TString BarrierName;
};
```

### 10.3 State Machine Diagrams

**CDC Stream Creation (without barrier):**
```
ConfigureParts → Propose → ProposedWaitParts → Done
                    ↓
              [Increment version]
              [Sync immediately]
```

**CDC Stream Creation (with barrier):**
```
ConfigureParts → Propose → ProposedWaitParts → Done
                    ↓
              [Increment version]
              [Register barrier]
                    ↓
              [Wait for siblings]
```

**Version Sync Part:**
```
Waiting → HandleReply(TEvCompleteBarrier) → Done
            ↓
    [Collect paths]
    [Find max version]
    [Sync all to max]
```

### 10.4 Example Timeline

**Table with 2 indexes, Strategy A:**

```
Time  Part0(Table)      Part1(Index1)     Part2(Index2)     Part3(Sync)
====  ==============    ==============    ==============    ==============
T0    ConfigureParts    ConfigureParts    ConfigureParts    Waiting
T1    Propose           Propose           Propose           [idle]
T2    version=10→11     version=10→11     version=10→11     [idle]
T3    Barrier("sync")   Barrier("sync")   Barrier("sync")   [idle]
T4    ProposedWait      ProposedWait      ProposedWait      [idle]
T5    Done              Done              Done              [idle]
T6    ─────────────────── Barrier Complete ───────────────→ Activated
T7                                                          ReadVersions:
                                                              table=11
                                                              index1=10
                                                              index1Impl=11
                                                              index2=10
                                                              index2Impl=11
                                                            maxVersion=11
T8                                                          SyncAll:
                                                              index1=11
                                                              index1Impl=11
                                                              index2=11
                                                              index2Impl=11
T9                                                          Done

Result: All versions = 11 ✓
```

### 10.5 Comparison: Current vs Strategy A

| Aspect | Current (Broken) | Strategy A (Barrier) |
|--------|------------------|----------------------|
| **Parallelism** | Full (all CDC parts parallel) | Full (CDC parallel, sync sequential) |
| **Coordination** | None (race conditions) | Barrier-based |
| **Consistency** | ❌ Inconsistent | ✅ Consistent |
| **Latency** | Fast | +5-10% (barrier overhead) |
| **Complexity** | Low | Medium |
| **Crash Recovery** | ✅ Supported | ✅ Supported |
| **Code Changes** | N/A | ~500 lines |

### 10.6 Glossary

- **AlterVersion:** Schema version number for tables and indexes
- **Barrier:** Coordination primitive blocking operation parts until all reach barrier
- **CDC Stream:** Change Data Capture stream for incremental backup
- **Index Entity:** Metadata object (TTableIndexInfo) representing an index
- **Index Impl Table:** Physical table storing index data (indexImplTable)
- **Operation Part:** Sub-operation with independent state machine
- **TEvCompleteBarrier:** Event sent when all parts reach barrier
- **TTxState:** Database-persisted transaction state
- **SubTxId:** Part identifier within an operation

---

## 11. Conclusion

### 11.1 Summary

Strategy A (Barrier-Based Coordination) provides a robust solution to the CDC stream version synchronization problem by:

1. **Leveraging existing infrastructure:** Uses battle-tested barrier mechanism
2. **Preserving parallelism:** CDC streams still created concurrently
3. **Ensuring consistency:** Atomic version sync after all CDC operations complete
4. **Supporting recovery:** All state persisted to database for crash recovery

### 11.2 Key Advantages

- ✅ **Correctness:** Guarantees consistent schema versions
- ✅ **Reliability:** Uses proven barrier pattern from drop indexed table
- ✅ **Maintainability:** Clean separation of concerns (CDC vs sync)
- ✅ **Debuggability:** Clear coordination points with detailed logging
- ✅ **Testability:** Easy to write unit and integration tests

### 11.3 Trade-offs

- ⚠️ **Latency:** ~5-10% increase due to barrier synchronization
- ⚠️ **Complexity:** Additional operation part and state management
- ⚠️ **Code Size:** ~500 lines of new code

### 11.4 Recommended Next Steps

1. **Phase 1: Prototype** (1-2 weeks)
   - Implement basic barrier coordination
   - Test with single index case
   - Validate correctness

2. **Phase 2: Complete Implementation** (2-3 weeks)
   - Handle multiple indexes
   - Add crash recovery
   - Comprehensive testing

3. **Phase 3: Integration** (1-2 weeks)
   - Performance benchmarking
   - Documentation
   - Code review

4. **Phase 4: Deployment** (2-4 weeks)
   - Feature flag rollout
   - Monitoring setup
   - Gradual production deployment

### 11.5 Success Criteria

Implementation is successful when:

1. ✅ All unit tests pass
2. ✅ Integration tests show correct version synchronization
3. ✅ Performance overhead < 15%
4. ✅ Zero schema version inconsistencies in test runs
5. ✅ Crash recovery works correctly
6. ✅ Production deployment stable for 2 weeks

### 11.6 Fallback Options

If Strategy A encounters issues:

1. **Quick fix:** Feature flag to disable barrier coordination
2. **Alternative:** Switch to Strategy E (Lock-Free Helping)
3. **Conservative:** Fall back to sequential CDC creation

---

## Appendix A: Detailed State Diagrams

### A.1 Complete Operation Flow with Barrier

```
┌──────────────────────────────────────────────────────────────────┐
│ User Request: CreateIncrementalBackup("/MyRoot/table1")         │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       v
┌──────────────────────────────────────────────────────────────────┐
│ CreateBackupIncrementalBackupCollection()                        │
│ ┌──────────────────────────────────────────────────────────────┐ │
│ │ Group CDC parts by table:                                    │ │
│ │   cdcPartsByTable[table1] = [CDC1, CDC2, CDC3]              │ │
│ │                                                              │ │
│ │ Create sync part:                                            │ │
│ │   syncPart = CreateCdcVersionSync(table1, "barrier_name")   │ │
│ │                                                              │ │
│ │ Result = [CDC1, CDC2, CDC3, SyncPart, Tracker]             │ │
│ └──────────────────────────────────────────────────────────────┘ │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       v
┌──────────────────────────────────────────────────────────────────┐
│ Propose Phase: All parts validate and persist initial state     │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       v
┌──────────────────────────────────────────────────────────────────┐
│ Activation: TEvProgressOperation sent to all parts              │
└──────────────────────┬───────────────────────────────────────────┘
                       │
       ┌───────────────┼───────────────┐
       │               │               │
       v               v               v
┌────────────┐  ┌────────────┐  ┌────────────┐
│ CDC Part 1 │  │ CDC Part 2 │  │ CDC Part 3 │
│ Configure  │  │ Configure  │  │ Configure  │
│ Propose    │  │ Propose    │  │ Propose    │
│ version++  │  │ version++  │  │ version++  │
│ Barrier()  │  │ Barrier()  │  │ Barrier()  │
│ Wait       │  │ Wait       │  │ Wait       │
│ Done       │  │ Done       │  │ Done       │
└─────┬──────┘  └─────┬──────┘  └─────┬──────┘
      │               │               │
      └───────────────┼───────────────┘
                      │
                      v
        ┌─────────────────────────────┐
        │ Barrier Complete Detection  │
        │ DoCheckBarriers():          │
        │   blocked: 3 parts          │
        │   done: 3 parts + others    │
        │   → Send TEvCompleteBarrier │
        └─────────────┬───────────────┘
                      │
                      v
        ┌─────────────────────────────┐
        │ Sync Part Activated         │
        │ HandleReply(CompleteBarrier)│
        │   - CollectAffectedPaths()  │
        │   - FindMaxVersion()        │
        │   - SyncAllVersions()       │
        │   - Done                    │
        └─────────────┬───────────────┘
                      │
                      v
        ┌─────────────────────────────┐
        │ Operation Complete          │
        │ All versions synchronized ✓ │
        └─────────────────────────────┘
```

### A.2 Error Handling Flow

```
┌───────────────────────────────────────┐
│ Error During CDC Creation             │
└──────────────┬────────────────────────┘
               │
        ┌──────┴───────┐
        │              │
        v              v
┌─────────────┐  ┌─────────────┐
│ Transient   │  │ Permanent   │
│ Error       │  │ Error       │
└──────┬──────┘  └──────┬──────┘
       │                │
       v                v
┌─────────────┐  ┌─────────────┐
│ Retry CDC   │  │ Abort       │
│ Operation   │  │ Operation   │
└──────┬──────┘  └──────┬──────┘
       │                │
       v                v
┌─────────────┐  ┌─────────────┐
│ Success     │  │ Rollback    │
│ Continue    │  │ - UnDo()    │
│             │  │ - Clean DB  │
└─────────────┘  └─────────────┘
```

---

## Appendix B: Database Schema Details

### B.1 New Columns in TxInFlightV2

```cpp
struct TxInFlightV2 : NIceDb::Schema::Table<100> {
    struct TxId : Column<1, NScheme::NTypeIds::Uint64> {};
    struct SubTxId : Column<2, NScheme::NTypeIds::Uint32> {};
    struct TxType : Column<3, NScheme::NTypeIds::Uint32> {};
    struct State : Column<4, NScheme::NTypeIds::Uint32> {};
    // ... existing columns 5-24 ...
    
    // New columns for barrier coordination:
    struct UseBarrierCoordination : Column<25, NScheme::NTypeIds::Bool> {
        static constexpr bool Default = false;
    };
    struct BarrierName : Column<26, NScheme::NTypeIds::Utf8> {
        static constexpr const char* Default = "";
    };
    
    using TKey = TableKey<TxId, SubTxId>;
    using TColumns = TableColumns<
        TxId, SubTxId, TxType, State, /* ... existing ... */
        UseBarrierCoordination, BarrierName
    >;
};
```

### B.2 Migration Strategy

**Backward Compatibility:**

```cpp
// Old version reading new data:
if (rowset.HaveValue<Schema::TxInFlightV2::UseBarrierCoordination>()) {
    txState.UseBarrierCoordination = 
        rowset.GetValue<Schema::TxInFlightV2::UseBarrierCoordination>();
} else {
    txState.UseBarrierCoordination = false;  // Default for old operations
}
```

**Forward Compatibility:**

New code always writes both columns. Old code ignores unknown columns (safe).

---

## Appendix C: Performance Analysis

### C.1 Expected Latency Impact

**Baseline (no barrier):**
```
CDC Creation: 100ms per stream
Parallel execution with 3 streams: ~100ms total
Total: 100ms
```

**With barrier (Strategy A):**
```
CDC Creation: 100ms per stream (parallel)
Barrier wait: ~5-10ms (coordination overhead)
Version sync: ~10-20ms (read + write versions)
Total: ~115-130ms
```

**Overhead: 15-30% increase in latency**

### C.2 Scalability Analysis

**Number of indexes vs latency:**

| Indexes | CDC Creation | Barrier Overhead | Sync Time | Total  |
|---------|--------------|------------------|-----------|--------|
| 1       | 100ms        | 5ms              | 5ms       | 110ms  |
| 3       | 100ms        | 5ms              | 15ms      | 120ms  |
| 10      | 100ms        | 10ms             | 30ms      | 140ms  |
| 20      | 100ms        | 15ms             | 50ms      | 165ms  |

**Conclusion:** Overhead grows linearly with number of indexes but remains acceptable.

---

## 12. Verification and Findings Summary

### 12.1 Code Verification Results

**Verified Correct**:
- ✅ Barrier mechanism exists and works as documented (schemeshard__operation.h:119-146)
- ✅ TEvCompleteBarrier event exists with correct structure (schemeshard_private.h:238-245)
- ✅ CDC stream creation is parallel (schemeshard__operation_backup_incremental_backup_collection.cpp)
- ✅ Barrier pattern used in drop-indexed-table operation (schemeshard__operation_drop_indexed_table.cpp)
- ✅ Race condition exists in current code (parallel SyncChildIndexes calls)
- ✅ Index structure assumption verified (one index → one impl table)

**Implementation Gaps** (will be created):
- ❌ New columns in TxInFlightV2 (UseBarrierCoordination, BarrierName) - don't exist
- ❌ TxCdcVersionSync transaction type - doesn't exist (next available: 117)
- ❌ CdcVersionSync operation files - don't exist

### 12.2 Key Changes from Original Document

1. **Version Sync Logic is Outdated**
   - Original document described one approach to UpdateTableVersion()
   - Actual code has been refactored with more sophisticated logic
   - Document updated to match current code patterns

2. **SyncChildIndexes Doesn't Update Impl Tables**
   - Current code intentionally skips impl table syncing (lines 349-352)
   - Comment: "bumping AlterVersion without TX_KIND_SCHEME causes SCHEME_CHANGED errors"
   - Our strategy replaces this entirely

3. **Sync Part Must Register at Barrier**
   - Initial design idea was sync part as observer
   - Corrected: sync part should also register at barrier (same as drop-indexed-table pattern)
   - Ensures proper sequencing and notification

### 12.3 Design Improvements

1. **Simplified TProposeAtTable Logic**
   - Old: Complex UpdateTableVersion() with multiple scenarios
   - New: Simple increment only, skip SyncChildIndexes entirely
   - Barrier coordination handled by dedicated sync part

2. **Centralized Version Sync**
   - Old: Scattered throughout CDC flow
   - New: All version sync in CdcVersionSync::HandleReply()
   - Single atomic operation in database transaction

3. **Backward Compatibility**
   - Non-coordinated CDC operations get simple behavior
   - Existing code paths preserved for non-barrier operations
   - Feature flag can control rollout

### 12.4 Constraint Verification

- ✅ Only one barrier per operation enforced: `Y_ABORT_UNLESS(Barriers.size() == 1)`
- ✅ Barrier completion check correct: `subTxIds.size() + DoneParts.size() == Parts.size()`
- ✅ Event routing verified in DoCheckBarriers (schemeshard__operation_side_effects.cpp:1086-1141)

---

*Document Version: 1.1*  
*Date: 2025-01-20*  
*Author: Strategy A Implementation Research*  
*Status: Updated with Verification Findings*  
*Last Update: Incorporated codebase verification, corrected barrier coordination, simplified implementation strategy to REPLACE old sync logic*