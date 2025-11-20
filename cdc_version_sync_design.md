# CDC Stream Schema Version Synchronization - Design Document

## Executive Summary

This document analyzes the schema version synchronization problem during CDC stream creation for tables with indexes in YDB's incremental backup/restore operations, and proposes multiple implementation strategies.

**Problem:** When creating CDC streams for indexed tables, parallel operation parts cause race conditions that desynchronize `AlterVersion` across Table, Index entity, and indexImplTable objects, violating query engine invariants.

**Recommended Solution:** Strategy E (Lock-Free "Helping" Coordination) or Strategy A (Barrier-Based Coordination), depending on implementation complexity preferences.

---

## 1. Problem Statement

### 1.1 The Race Condition

During incremental backup/restore operations on tables with indexes:

1. **Multiple CDC streams are created in parallel** as separate operation parts:
   - One CDC stream for the main table
   - One CDC stream for each index implementation table (`Table/Index/indexImplTable`)
   
2. **Each CDC creation increments schema versions independently** in `TProposeAtTable::HandleReply`:
   ```
   File: ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp
   Lines: 447-479
   ```

3. **Race condition timeline:**
   ```
   T1: CDC for indexImplTable reads parent table version = 5
   T2: CDC for another indexImplTable reads parent table version = 5
   T1: Increments indexImplTable1 version to 6
   T1: Tries to sync parent to 6 (but parent might be at 7 already from T2)
   T2: Increments indexImplTable2 version to 6
   T2: Tries to sync parent to 6
   Result: Versions are now out of sync
   ```

### 1.2 Current Sync Attempts and Why They Fail

**File:** `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp`

**Existing sync logic** (lines 175-248):
- `UpdateTableVersion()` - tries to sync versions when CDC is created
- `SyncImplTableVersion()` - syncs impl table with parent table
- `SyncIndexEntityVersion()` - syncs index entity version
- `SyncChildIndexes()` - syncs all child indexes

**Why it fails:**
1. **Non-atomic reads and writes:** Each operation reads current versions, makes decisions, then writes - classic race condition
2. **Parallel execution:** Operations execute simultaneously in different transaction contexts
3. **No coordination mechanism:** Each part acts independently without knowing about sibling operations

**Key insight from line 348-352:**
```cpp
// NOTE: We intentionally do NOT sync the index impl table version here.
// Bumping AlterVersion without sending a TX_KIND_SCHEME transaction to datashards
// causes SCHEME_CHANGED errors because datashards still have the old version.
```

This comment reveals that version increments **must be accompanied by actual schema transactions** to datashards.

### 1.3 Impact on Query Engine

**Test evidence:**
```
File: ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp
Lines: 573-595
```

The test explicitly checks schema versions after backup, indicating the query engine expects consistency.

**Expected invariant:** 
- `Table.AlterVersion == Index.AlterVersion == indexImplTable.AlterVersion` (all in sync)

**What breaks:**
- Query planning uses schema versions to ensure consistent reads
- Mismatched versions can cause "schema changed" errors during query execution
- Index reads might see wrong schema version compared to base table

---

## 2. Current State Analysis

### 2.1 CDC Creation Flow for Indexed Tables

**Entry point:** `CreateBackupIncrementalBackupCollection`
```
File: ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp
Lines: 155-299
```

**Flow:**
1. **Lines 186-224:** Create CDC for main tables
2. **Lines 226-297:** Create CDC for index impl tables
   - Iterates through table children (line 242)
   - Finds indexes (lines 245-259)
   - Creates CDC for each indexImplTable (lines 269-294)
3. **All CDC creations are added as separate parts** to the same operation

**Key observation:** Parts array contains multiple CDC creations that execute in parallel:
```cpp
result.push_back(CreateAlterContinuousBackup(...)); // Main table CDC
result.push_back(CreateAlterContinuousBackup(...)); // Index 1 impl table CDC
result.push_back(CreateAlterContinuousBackup(...)); // Index 2 impl table CDC
// ... etc
```

### 2.2 CDC Stream Operation Lifecycle

**CDC stream creation goes through these states:**

```
File: ydb/core/tx/schemeshard/schemeshard__operation_create_cdc_stream.cpp
Lines: 462-479 (TNewCdcStreamAtTable::NextState)
```

State progression:
1. `ConfigureParts` - Send CDC creation to datashards
2. `Propose` - Get plan step from coordinator
3. `ProposedWaitParts` - Wait for datashards to confirm
4. `Done` - Complete

**Critical point:** Version increment happens in `Propose` state's `HandleReply`:
```
File: ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp
Lines: 447-479 (TProposeAtTable::HandleReply)
```

### 2.3 Version Sync Logic

**BuildTableVersionContext** (lines 94-113):
- Detects if operation is on index impl table
- Checks if it's part of continuous backup
- Builds context with parent/grandparent relationships

**UpdateTableVersion** (lines 175-248):
- **For index impl tables during backup** (lines 190-216):
  - Calls `SyncImplTableVersion` to match parent version
  - Calls `SyncIndexEntityVersion` to update index entity
  - Calls `SyncChildIndexes` to sync sibling indexes
- **For other cases** (lines 217-247):
  - Simple increment: `table->AlterVersion += 1`

**The race:** Multiple calls to `UpdateTableVersion` happen simultaneously for different indexes on the same table.

### 2.4 Why Current Sync Fails: Detailed Analysis

**Scenario:** Table with 3 indexes (Index1, Index2, Index3)

```
Initial state:
  Table.AlterVersion = 10
  Index1.AlterVersion = 10
  Index1Impl.AlterVersion = 10
  Index2.AlterVersion = 10
  Index2Impl.AlterVersion = 10
  Index3.AlterVersion = 10
  Index3Impl.AlterVersion = 10

Parallel CDC creation (3 parts execute simultaneously):

Part1 (Index1Impl CDC):
  T1: Read Table.AlterVersion = 10
  T1: Read Index1.AlterVersion = 10
  T5: Set Index1Impl.AlterVersion = 10 (sync with parent)
  T5: Set Index1.AlterVersion = 10
  T6: Try to sync siblings... but they're changing too!

Part2 (Index2Impl CDC):
  T2: Read Table.AlterVersion = 10
  T2: Read Index2.AlterVersion = 10
  T4: Set Index2Impl.AlterVersion = 10
  T4: Set Index2.AlterVersion = 10
  
Part3 (Index3Impl CDC):
  T3: Read Table.AlterVersion = 10
  T3: Read Index3.AlterVersion = 10
  T7: Set Index3Impl.AlterVersion = 10
  T7: Set Index3.AlterVersion = 10

After CDC creation (some operations "win", others "lose"):
  Table.AlterVersion = 10 (unchanged!)
  Index1.AlterVersion = 11 (from Part1's SyncChildIndexes)
  Index1Impl.AlterVersion = 11 (incremented by CDC)
  Index2.AlterVersion = 10 (overwritten by Part3)
  Index2Impl.AlterVersion = 11 (incremented by CDC)
  Index3.AlterVersion = 11 (from Part2's SyncChildIndexes)
  Index3Impl.AlterVersion = 11 (incremented by CDC)
  
Result: INCONSISTENT! Index2 has wrong version.
```

---

## 3. Invariant Verification

### 3.1 Schema Version Requirements

**From datashard perspective:**

```
File: ydb/core/tx/datashard/datashard_impl.h, datashard_write_operation.cpp
```

DataShards track `SchemaVersion` and reject operations with mismatched versions with `SCHEME_CHANGED` errors.

**From query engine perspective:**

When executing a query on an indexed table:
1. Query planner resolves table and index schemas
2. Expects consistent schema versions across related objects
3. If versions don't match, may see stale schema or incorrect query plans

**Test evidence:**
```
File: ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp
Lines: 573-595
```

After backup, test explicitly checks that SchemaVersions are reported correctly, implying they must be consistent.

### 3.2 The Required Invariant

**For a table with indexes:**

```
Invariant: Table.AlterVersion == Index1.AlterVersion == Index1Impl.AlterVersion
           == Index2.AlterVersion == Index2Impl.AlterVersion
           == ... (for all indexes)
```

**Alternative weaker invariant:**
```
For each index I:
  Table.AlterVersion >= IndexI.AlterVersion == IndexIImpl.AlterVersion
```

**Question: Should table version be incremented during CDC creation on indexes?**

Analysis:
- CDC creation on impl table **does change the table's effective schema** (adds CDC stream)
- However, CDC is created on the *impl table*, not the main table
- Main table's schema doesn't actually change

**Conclusion:** Main table version **should not** be incremented when CDC is added to impl table. Only the impl table and its parent index entity should be incremented, and they must stay in sync.

**Refined invariant:**
```
For each index I:
  IndexI.AlterVersion == IndexIImpl.AlterVersion
  
All indexes may have different versions (if CDC was created at different times),
but each Index and its Impl must match.
```

---

## 4. Barrier Pattern Analysis

### 4.1 How Barriers Work

**Definition:**
```
File: ydb/core/tx/schemeshard/schemeshard__operation.h
Lines: 119-146
```

A barrier blocks a set of operation parts from completing until all parts reach the barrier.

**Key methods:**
- `RegisterBarrier(partId, name)` - Part registers itself at barrier
- `IsDoneBarrier()` - Checks if barrier is complete: `blocked_parts + done_parts == total_parts`
- `DropBarrier(name)` - Removes barrier after completion

**Barrier flow:**

1. Parts register at barrier via `context.OnComplete.Barrier(OperationId, "barrier_name")`
2. When part is done, it's added to `DoneParts` but stays blocked if in barrier
3. When last part completes: `IsDoneBarrier()` returns true
4. `DoCheckBarriers` (lines 1086-1141 in `schemeshard__operation_side_effects.cpp`) sends `TEvCompleteBarrier` to all blocked parts
5. Parts handle `TEvCompleteBarrier` and proceed

**Example usage:**
```
File: ydb/core/tx/schemeshard/schemeshard__operation_drop_indexed_table.cpp
Lines: 187-241 (TDeletePathBarrier)
```

Drop indexed table uses barrier to ensure all index drops complete before table drop.

### 4.2 Applicability to CDC Version Sync

**Pros:**
- ✅ Existing, battle-tested mechanism
- ✅ Handles arbitrary number of parts
- ✅ Automatic coordination without manual synchronization
- ✅ Can execute sync logic after all CDC streams created

**Cons:**
- ❌ Only one barrier allowed per operation at a time (line 121: `Y_ABORT_UNLESS(Barriers.size() == 1)`)
- ❌ Requires adding extra operation part for version sync
- ❌ Adds latency (barrier wait + sync step)

**Limitation impact:** The "one barrier at a time" constraint means we can't nest barriers or have multiple concurrent barriers in the same operation.

### 4.3 Barrier Usage Pattern for CDC Sync

**Proposed flow:**

```
Operation Parts:
  Part0: Create CDC for main table
  Part1: Create CDC for Index1 impl table (register barrier "cdc_sync")
  Part2: Create CDC for Index2 impl table (register barrier "cdc_sync")
  Part3: Create CDC for Index3 impl table (register barrier "cdc_sync")
  Part4: Version sync (waits for barrier, then syncs all versions)
```

**Implementation:**
1. Each CDC stream part registers at barrier instead of syncing immediately
2. When all CDC parts done → `TEvCompleteBarrier` sent
3. Version sync part receives `TEvCompleteBarrier` and performs atomic sync
4. Sync part reads all current versions and sets them to `max(versions) + 1`

---

## 5. Implementation Strategies

### Strategy A: Barrier-Based Coordination

**Concept:** Use existing barrier mechanism to coordinate version sync after all CDC streams are created.

**Implementation:**

1. **Modify CDC creation** (`schemeshard__operation_backup_incremental_backup_collection.cpp`):
   ```cpp
   // Group CDC parts by table
   THashMap<TPathId, TVector<ISubOperation::TPtr>> cdcByTable;
   
   // Add main table CDC
   cdcByTable[tablePathId].push_back(CreateAlterContinuousBackup(...));
   
   // Add index CDC streams
   for (each index) {
       cdcByTable[tablePathId].push_back(CreateAlterContinuousBackup(...));
   }
   
   // For each table, add barrier and sync part
   for (auto& [tablePathId, cdcParts] : cdcByTable) {
       TString barrierName = TStringBuilder() << "cdc_version_sync_" << tablePathId;
       
       // Mark each CDC part to register at barrier
       for (auto& part : cdcParts) {
           // Pass barrier name via transaction context or part state
       }
       
       result.insert(result.end(), cdcParts.begin(), cdcParts.end());
       
       // Add version sync part
       result.push_back(CreateCdcVersionSync(NextPartId(opId, result), tablePathId, barrierName));
   }
   ```

2. **Modify CDC TProposeAtTable::HandleReply** (`schemeshard__operation_common_cdc_stream.cpp`):
   ```cpp
   bool TProposeAtTable::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) {
       // ... existing code ...
       
       // Check if this CDC is part of coordinated sync
       if (IsPartOfCoordinatedSync(context)) {
           // Register at barrier instead of syncing
           context.OnComplete.Barrier(OperationId, GetBarrierName(context));
           
           // Still increment the impl table version locally
           // but skip the sync logic
           table->AlterVersion += 1;
           context.SS->PersistTableAlterVersion(db, pathId, table);
       } else {
           // Normal flow: sync immediately
           auto versionCtx = BuildTableVersionContext(*txState, path, context);
           UpdateTableVersion(versionCtx, table, OperationId, context, db);
       }
       
       // ... rest of code ...
   }
   ```

3. **Create CdcVersionSync operation part** (new file):
   ```cpp
   // ydb/core/tx/schemeshard/schemeshard__operation_cdc_version_sync.cpp
   
   class TCdcVersionSync : public TSubOperationState {
       TOperationId OperationId;
       TPathId TablePathId;
       TString BarrierName;
       
       bool HandleReply(TEvPrivate::TEvCompleteBarrier::TPtr& ev, TOperationContext& context) override {
           // All CDC streams have completed
           // Now sync all versions atomically
           
           NIceDb::TNiceDb db(context.GetDB());
           
           // Find all affected objects
           TVector<TPathId> affectedPaths;
           CollectAffectedPaths(TablePathId, context, affectedPaths);
           
           // Find max version
           ui64 maxVersion = 0;
           for (auto pathId : affectedPaths) {
               if (context.SS->Tables.contains(pathId)) {
                   maxVersion = Max(maxVersion, context.SS->Tables[pathId]->AlterVersion);
               }
               if (context.SS->Indexes.contains(pathId)) {
                   maxVersion = Max(maxVersion, context.SS->Indexes[pathId]->AlterVersion);
               }
           }
           
           ui64 targetVersion = maxVersion; // Already incremented by CDC parts
           
           // Sync all to target version
           for (auto pathId : affectedPaths) {
               if (context.SS->Tables.contains(pathId)) {
                   auto table = context.SS->Tables[pathId];
                   table->AlterVersion = targetVersion;
                   context.SS->PersistTableAlterVersion(db, pathId, table);
                   context.SS->ClearDescribePathCaches(context.SS->PathsById[pathId]);
                   context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
               }
               if (context.SS->Indexes.contains(pathId)) {
                   auto index = context.SS->Indexes[pathId];
                   index->AlterVersion = targetVersion;
                   context.SS->PersistTableIndexAlterVersion(db, pathId, index);
                   context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
               }
           }
           
           return true;
       }
       
       void CollectAffectedPaths(TPathId tablePathId, TOperationContext& context, 
                                 TVector<TPathId>& out) {
           // Add table itself
           out.push_back(tablePathId);
           
           // Find all indexes
           auto tablePath = context.SS->PathsById[tablePathId];
           for (auto& [childName, childPathId] : tablePath->GetChildren()) {
               auto childPath = context.SS->PathsById[childPathId];
               if (childPath->IsTableIndex() && !childPath->Dropped()) {
                   // Add index entity
                   out.push_back(childPathId);
                   
                   // Add impl table
                   auto indexPath = context.SS->PathsById[childPathId];
                   Y_ABORT_UNLESS(indexPath->GetChildren().size() == 1);
                   auto implTablePathId = indexPath->GetChildren().begin()->second;
                   out.push_back(implTablePathId);
               }
           }
       }
   };
   ```

**Pros:**
- ✅ Uses battle-tested barrier mechanism
- ✅ Atomic version sync after all CDC streams created
- ✅ Clean separation of concerns
- ✅ Easy to understand and debug

**Cons:**
- ❌ Requires creating new operation part type
- ❌ Adds latency (wait for barrier + extra sync step)
- ❌ More complex operation structure
- ❌ Requires passing barrier context through CDC creation

**Complexity:** Medium

**Risk:** Low (uses existing patterns)

---

### Strategy B: Sequential CDC Creation

**Concept:** Create CDC streams sequentially instead of in parallel, syncing after each one.

**Implementation:**

1. **Modify backup collection creation** (`schemeshard__operation_backup_incremental_backup_collection.cpp`):
   ```cpp
   // Instead of adding all CDC parts at once:
   result.push_back(CreateAlterContinuousBackup(mainTable));
   result.push_back(CreateAlterContinuousBackup(index1));
   result.push_back(CreateAlterContinuousBackup(index2));
   
   // Create a sequential coordinator:
   result.push_back(CreateSequentialCdcCoordinator(opId, tablePathId, {mainTable, index1, index2}));
   ```

2. **Sequential coordinator:**
   ```cpp
   class TSequentialCdcCoordinator : public TSubOperationState {
       TVector<TPathId> StreamsToCreate;
       size_t CurrentIndex = 0;
       
       bool ProgressState(TOperationContext& context) override {
           if (CurrentIndex >= StreamsToCreate.size()) {
               // All done
               return true;
           }
           
           // Create next CDC stream
           auto streamPathId = StreamsToCreate[CurrentIndex];
           // ... create CDC stream operation ...
           
           // Wait for it to complete before proceeding
           return false;
       }
       
       bool HandleReply(/* CDC completion */, TOperationContext& context) override {
           // Sync versions
           SyncVersionsForStream(StreamsToCreate[CurrentIndex], context);
           
           // Move to next
           CurrentIndex++;
           context.OnComplete.ActivateTx(OperationId);
           return false;
       }
   };
   ```

**Pros:**
- ✅ Simple - no race conditions by design
- ✅ Syncs after each CDC creation
- ✅ Easy to debug

**Cons:**
- ❌ **Much slower** - N sequential operations instead of parallel
- ❌ Poor user experience (longer backup/restore times)
- ❌ Doesn't leverage parallel execution capability
- ❌ Requires rewriting backup/restore operation structure

**Complexity:** Medium

**Risk:** Low (but poor performance)

**Recommendation:** ❌ Not recommended due to performance implications

---

### Strategy C: Post-Creation Synchronization

**Concept:** Create all CDC streams in parallel as currently done, then sync versions in finalization step.

**Implementation:**

1. **Keep current CDC creation logic unchanged**

2. **Enhance `TIncrementalRestoreFinalizeOp`** (already exists):
   ```
   File: ydb/core/tx/schemeshard/schemeshard__operation_incremental_restore_finalize.cpp
   Lines: 81-202 (TConfigureParts)
   ```

   Current code already tries to sync versions, but has bugs:
   - Line 100-134: Prepares AlterData for each impl table
   - Line 232: Calls `SyncIndexSchemaVersions`
   - Line 308-337: Finalizes ALTER for tables and syncs index versions

3. **Fix the finalization logic:**
   ```cpp
   // In TIncrementalRestoreFinalizeOp::TFinalizationPropose::SyncIndexSchemaVersions
   
   void SyncIndexSchemaVersions(...) {
       // Find all tables involved in restore
       THashMap<TPathId, TVector<TPathId>> tableToIndexImpls;
       
       for (const auto& tablePath : finalize.GetTargetTablePaths()) {
           if (!tablePath.Contains("/indexImplTable")) {
               continue;
           }
           
           auto implPath = TPath::Resolve(tablePath, context.SS);
           auto indexPath = implPath.Parent();
           auto mainTablePath = indexPath.Parent();
           
           tableToIndexImpls[mainTablePath->PathId].push_back(implPath->PathId);
       }
       
       // For each main table, sync all its index versions
       for (auto& [tablePathId, implTablePathIds] : tableToIndexImpls) {
           ui64 maxVersion = context.SS->Tables[tablePathId]->AlterVersion;
           
           // Find max across all impl tables
           for (auto implPathId : implTablePathIds) {
               maxVersion = Max(maxVersion, context.SS->Tables[implPathId]->AlterVersion);
           }
           
           // Set all impl tables to max
           for (auto implPathId : implTablePathIds) {
               auto implTable = context.SS->Tables[implPathId];
               implTable->AlterVersion = maxVersion;
               context.SS->PersistTableAlterVersion(db, implPathId, implTable);
               
               // Sync parent index entity
               auto implPath = context.SS->PathsById[implPathId];
               auto indexPathId = implPath->ParentPathId;
               if (context.SS->Indexes.contains(indexPathId)) {
                   context.SS->Indexes[indexPathId]->AlterVersion = maxVersion;
                   context.SS->PersistTableIndexAlterVersion(db, indexPathId, context.SS->Indexes[indexPathId]);
               }
           }
       }
   }
   ```

**Pros:**
- ✅ Leverages existing finalization infrastructure
- ✅ CDC streams still created in parallel (performance preserved)
- ✅ Centralized sync logic
- ✅ Works for restore operations

**Cons:**
- ❌ Doesn't help backup operations (no finalization step)
- ❌ Versions stay inconsistent during backup until finalization
- ❌ May cause issues if operations fail between CDC creation and finalization
- ❌ Finalization happens much later, versions wrong in intermediate state

**Complexity:** Low (enhancement to existing code)

**Risk:** Medium (temporary inconsistency)

**Recommendation:** ⚠️ Only suitable for restore, not complete solution

---

### Strategy D: Pre-Increment Coordination

**Concept:** Calculate target version before creating any CDC streams, pass it to all CDC operations.

**Implementation:**

1. **Calculate target version before CDC creation:**
   ```cpp
   // In CreateBackupIncrementalBackupCollection
   
   THashMap<TPathId, ui64> targetVersions;
   
   // For each table with indexes
   for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
       const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
       auto table = context.SS->Tables.at(tablePath.Base()->PathId);
       
       // Find max version across table and all its indexes
       ui64 maxVersion = table->AlterVersion;
       
       for (const auto& [childName, childPathId] : tablePath.Base()->GetChildren()) {
           auto childPath = context.SS->PathsById.at(childPathId);
           if (childPath->IsTableIndex() && !childPath->Dropped()) {
               if (context.SS->Indexes.contains(childPathId)) {
                   maxVersion = Max(maxVersion, context.SS->Indexes[childPathId]->AlterVersion);
               }
               
               // Check impl table
               auto indexPath = TPath::Init(childPathId, context.SS);
               auto [implName, implPathId] = *indexPath.Base()->GetChildren().begin();
               if (context.SS->Tables.contains(implPathId)) {
                   maxVersion = Max(maxVersion, context.SS->Tables[implPathId]->AlterVersion);
               }
           }
       }
       
       // Target version for CDC creation
       targetVersions[tablePath.Base()->PathId] = maxVersion + 1;
   }
   ```

2. **Pass target version to CDC creation:**
   ```cpp
   // Modify transaction to include target version
   modifyScheme.MutableAlterContinuousBackup()->SetTargetSchemaVersion(targetVersions[tablePathId]);
   ```

3. **Use target version in CDC TProposeAtTable:**
   ```cpp
   bool TProposeAtTable::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) {
       // ... existing code ...
       
       ui64 targetVersion;
       if (HasTargetVersion(context)) {
           targetVersion = GetTargetVersion(context);
       } else {
           targetVersion = table->AlterVersion + 1;
       }
       
       table->AlterVersion = targetVersion;
       
       // Sync index entity to same target
       if (IsIndexImplTable(context)) {
           auto indexPathId = GetParentIndexPathId(context);
           if (context.SS->Indexes.contains(indexPathId)) {
               context.SS->Indexes[indexPathId]->AlterVersion = targetVersion;
           }
       }
       
       // ... persist ...
   }
   ```

**Pros:**
- ✅ Simple conceptually
- ✅ CDC streams still run in parallel
- ✅ No extra operation parts needed
- ✅ Versions coordinated from the start

**Cons:**
- ❌ **Doesn't fully solve the race** - multiple CDC operations can still write different values
- ❌ If one CDC fails and retries, it might get a different target version
- ❌ Requires modifying CDC operation interface to pass version
- ❌ Pre-calculated version might be stale by the time CDC actually runs

**Complexity:** Medium

**Risk:** Medium-High (still has race condition potential)

**Recommendation:** ⚠️ Not fully reliable without additional locking

---

### Strategy E: Lock-Free Style "Helping" Coordination

**Concept:** Each CDC creation helps its siblings by checking and syncing their versions, similar to lock-free algorithms where threads help each other.

**Implementation:**

1. **Each CDC operation checks siblings in HandleReply:**
   ```cpp
   bool TProposeAtTable::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) {
       // ... existing code ...
       
       NIceDb::TNiceDb db(context.GetDB());
       
       // Increment self
       table->AlterVersion += 1;
       ui64 myVersion = table->AlterVersion;
       
       // "Help" siblings by ensuring they're all at consistent version
       if (IsPartOfIndexedTable(context)) {
           HelpSyncSiblingVersions(pathId, myVersion, context, db);
       }
       
       context.SS->PersistTableAlterVersion(db, pathId, table);
       
       // ... rest of code ...
   }
   ```

2. **HelpSyncSiblingVersions implementation:**
   ```cpp
   void HelpSyncSiblingVersions(TPathId myPathId, ui64 myVersion, 
                                TOperationContext& context, NIceDb::TNiceDb& db) {
       // Find parent table
       TPathId parentTablePathId = GetParentTablePathId(myPathId, context);
       if (!parentTablePathId) {
           return;
       }
       
       // Collect all related objects
       TVector<TPathId> allIndexes, allImplTables;
       CollectIndexFamily(parentTablePathId, context, allIndexes, allImplTables);
       
       // Find current max version across all objects
       ui64 maxVersion = myVersion;
       for (auto pathId : allIndexes) {
           if (context.SS->Indexes.contains(pathId)) {
               maxVersion = Max(maxVersion, context.SS->Indexes[pathId]->AlterVersion);
           }
       }
       for (auto pathId : allImplTables) {
           if (context.SS->Tables.contains(pathId)) {
               maxVersion = Max(maxVersion, context.SS->Tables[pathId]->AlterVersion);
           }
       }
       
       // If someone is ahead of us, catch up
       if (maxVersion > myVersion) {
           auto myTable = context.SS->Tables[myPathId];
           myTable->AlterVersion = maxVersion;
           // Will persist below
       }
       
       // Help others catch up to us/maxVersion
       for (auto pathId : allIndexes) {
           if (context.SS->Indexes.contains(pathId)) {
               auto index = context.SS->Indexes[pathId];
               if (index->AlterVersion < maxVersion) {
                   index->AlterVersion = maxVersion;
                   context.SS->PersistTableIndexAlterVersion(db, pathId, index);
                   context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
               }
           }
       }
       for (auto pathId : allImplTables) {
           if (pathId == myPathId) continue; // Skip self
           if (context.SS->Tables.contains(pathId)) {
               auto table = context.SS->Tables[pathId];
               if (table->AlterVersion < maxVersion) {
                   table->AlterVersion = maxVersion;
                   context.SS->PersistTableAlterVersion(db, pathId, table);
                   auto tablePath = context.SS->PathsById[pathId];
                   context.SS->ClearDescribePathCaches(tablePath);
                   context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
               }
           }
       }
   }
   ```

3. **Key insight - Idempotency:**
   - Multiple operations can call `HelpSyncSiblingVersions` simultaneously
   - Each reads current max, updates to max
   - Even if interleaved, they all converge to same final state
   - Last writer wins, but all writers write the same value (max)

**Lock-free properties:**
- **Progress:** At least one operation makes progress
- **Linearizability:** All operations see monotonically increasing versions
- **Convergence:** Eventually all versions reach the same max value
- **Idempotency:** Safe to execute multiple times

**Handling races:**
```
T1: HelpSync reads max=10, prepares to write 11
T2: HelpSync reads max=10, prepares to write 11
T1: Writes Index1=11, Index2=11, Index3=11
T2: Writes Index1=11, Index2=11, Index3=11 (overwrites with same value)
Result: All at 11 ✓

Alternative race:
T1: HelpSync reads max=10, writes Impl1=11
T2: HelpSync reads max=11 (sees T1's write), writes Impl2=11
T3: HelpSync reads max=11, writes Impl3=11
Result: All at 11 ✓
```

**Pros:**
- ✅ No extra operation parts or barriers needed
- ✅ Minimal changes to existing code
- ✅ Lock-free - good parallelism
- ✅ Self-healing - operations help each other
- ✅ Works with any number of concurrent CDC creations
- ✅ Eventually consistent by design

**Cons:**
- ❌ More complex logic (harder to understand at first)
- ❌ Redundant work (multiple operations sync same objects)
- ❌ Slightly higher DB load (more writes)
- ❌ Requires careful reasoning about race conditions
- ❌ May have brief windows of inconsistency during updates

**Complexity:** Medium-High (requires careful implementation)

**Risk:** Medium (lock-free algorithms need careful validation)

**Optimization:** Add a flag to track "already synced by someone" to reduce redundant work.

**Recommendation:** ✅ **Recommended** - Good balance of simplicity and correctness

---

## 6. Code References

### 6.1 Key Files and Locations

**Barrier Implementation:**
```
File: ydb/core/tx/schemeshard/schemeshard__operation.h
Lines: 119-146 - Barrier registration and checking
Lines: 129-140 - IsDoneBarrier() logic

File: ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp
Lines: 1086-1141 - DoCheckBarriers() - barrier completion handling
Lines: 1131-1136 - TEvCompleteBarrier sending to blocked parts
```

**CDC Creation Flow:**
```
File: ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp
Lines: 155-299 - CreateBackupIncrementalBackupCollection
Lines: 186-224 - Main table CDC creation loop
Lines: 226-297 - Index impl table CDC creation loop
Lines: 241-296 - Index iteration and impl table CDC

File: ydb/core/tx/schemeshard/schemeshard__operation_create_cdc_stream.cpp
Lines: 462-503 - TNewCdcStreamAtTable state machine
Lines: 518-615 - Propose() method
```

**CDC Version Sync Logic:**
```
File: ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp
Lines: 34-42 - TTableVersionContext structure
Lines: 43-56 - DetectContinuousBackupStream
Lines: 58-71 - DetectIndexImplTable
Lines: 94-113 - BuildTableVersionContext
Lines: 115-173 - SyncImplTableVersion
Lines: 175-248 - UpdateTableVersion (main version update logic)
Lines: 253-316 - SyncIndexEntityVersion
Lines: 318-368 - SyncChildIndexes
Lines: 447-479 - TProposeAtTable::HandleReply (where version increment happens)
```

**Finalization (Restore):**
```
File: ydb/core/tx/schemeshard/schemeshard__operation_incremental_restore_finalize.cpp
Lines: 40-203 - TConfigureParts (prepares ALTER transactions)
Lines: 81-202 - CollectIndexImplTables and ALTER preparation
Lines: 205-419 - TFinalizationPropose
Lines: 270-338 - SyncIndexSchemaVersions
```

**Test Evidence:**
```
File: ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp
Lines: 524-630 - Test for table with index backup/restore
Lines: 573-595 - Version diagnostics after backup
```

### 6.2 Key Data Structures

**TOperation:**
```
File: ydb/core/tx/schemeshard/schemeshard__operation.h
Lines: 10-157

struct TOperation {
    const TTxId TxId;
    TVector<ISubOperation::TPtr> Parts;  // Operation parts (parallel execution)
    TSet<TSubTxId> DoneParts;            // Completed parts
    THashMap<TString, TSet<TSubTxId>> Barriers;  // Barrier name -> blocked parts
    // ...
};
```

**TTableInfo:**
```
File: ydb/core/tx/schemeshard/schemeshard_info_types.h

struct TTableInfo {
    ui64 AlterVersion;  // Schema version
    // ...
};
```

**TTableIndexInfo:**
```
File: ydb/core/tx/schemeshard/schemeshard_info_types.h

struct TTableIndexInfo {
    ui64 AlterVersion;  // Index entity version
    // ...
};
```

**TTxState:**
```
File: ydb/core/tx/schemeshard/schemeshard_tx_infly.h

struct TTxState {
    ETxState State;
    ETxType TxType;
    TPathId TargetPathId;
    TPathId SourcePathId;
    TPathId CdcPathId;  // CDC stream path (for continuous backup detection)
    // ...
};
```

### 6.3 Transaction State Flow

**CDC Creation States:**
```
ConfigureParts → Propose → ProposedWaitParts → Done
```

**Version increment location:**
```
Propose state's HandleReply (TEvOperationPlan)
  ↓
BuildTableVersionContext
  ↓
UpdateTableVersion
  ↓
table->AlterVersion++
```

---

## 7. Recommendation

### 7.1 Recommended Solution: Strategy E (Lock-Free Helping) with Strategy A (Barrier) fallback

**Primary recommendation: Strategy E - Lock-Free "Helping" Coordination**

**Rationale:**
1. **Minimal code changes** - Works within existing CDC operation structure
2. **Preserves parallelism** - CDC streams still execute concurrently
3. **Self-healing** - Operations automatically sync siblings
4. **No new infrastructure** - Doesn't require barrier coordination or new operation parts
5. **Mathematically sound** - Lock-free convergence guarantees eventual consistency

**Implementation priority:**
1. Implement `HelpSyncSiblingVersions()` in `schemeshard_cdc_stream_common.cpp`
2. Modify `TProposeAtTable::HandleReply` to call help function
3. Add helper functions to detect index families and collect siblings
4. Add extensive logging for debugging race conditions
5. Test with multiple indexes (3-10 indexes) and verify convergence

**Alternative: Strategy A - Barrier-Based Coordination**

If lock-free approach proves too complex or has unforeseen issues:
- Fall back to Strategy A (Barrier-Based)
- More straightforward to understand and debug
- Slightly more code but clearer control flow
- Trade-off: Extra latency for simplicity

**Why not others:**
- **Strategy B (Sequential):** ❌ Too slow, poor user experience
- **Strategy C (Post-Creation):** ❌ Only works for restore, temporary inconsistency
- **Strategy D (Pre-Increment):** ❌ Doesn't fully solve race condition

### 7.2 Implementation Complexity

**Strategy E (Lock-Free):**
- **New code:** ~200-300 lines
- **Modified code:** ~50 lines
- **Files affected:** 2-3 files
- **Estimated effort:** 2-3 days implementation + 2 days testing

**Strategy A (Barrier):**
- **New code:** ~400-500 lines (new operation part)
- **Modified code:** ~100 lines
- **Files affected:** 4-5 files
- **Estimated effort:** 4-5 days implementation + 2 days testing

### 7.3 Risk Analysis

**Strategy E Risks:**
- **Medium risk:** Lock-free algorithms need careful validation
- **Mitigation:** Extensive unit tests with concurrent operations
- **Mitigation:** Add detailed logging to trace version updates
- **Mitigation:** Add assertions to verify invariants

**Strategy A Risks:**
- **Low risk:** Uses existing proven patterns
- **Mitigation:** Reuse barrier patterns from drop indexed table
- **Potential issue:** One barrier per operation limitation

### 7.4 Testing Requirements

**Unit Tests:**
1. Table with 1 index - verify versions sync
2. Table with 3 indexes - verify all sync to same version
3. Table with 10 indexes - stress test
4. Concurrent CDC creations (parallel parts)
5. CDC creation with one part failing and retrying
6. Restore after backup with indexes

**Integration Tests:**
1. Full backup/restore cycle with indexed table
2. Multiple tables with multiple indexes
3. Incremental backup with schema changes

**Validation:**
```cpp
// After CDC creation, verify invariant:
void ValidateIndexVersions(TPathId tablePathId, TOperationContext& context) {
    auto table = context.SS->Tables[tablePathId];
    ui64 expectedVersion = table->AlterVersion;
    
    auto tablePath = context.SS->PathsById[tablePathId];
    for (auto& [childName, childPathId] : tablePath->GetChildren()) {
        auto childPath = context.SS->PathsById[childPathId];
        if (childPath->IsTableIndex() && !childPath->Dropped()) {
            // Check index entity
            Y_VERIFY_S(context.SS->Indexes[childPathId]->AlterVersion == expectedVersion,
                      "Index version mismatch");
            
            // Check impl table
            auto indexPath = context.SS->PathsById[childPathId];
            auto [implName, implPathId] = *indexPath->GetChildren().begin();
            Y_VERIFY_S(context.SS->Tables[implPathId]->AlterVersion == expectedVersion,
                      "Impl table version mismatch");
        }
    }
}
```

---

## 8. Open Questions and Future Work

### 8.1 Open Questions

1. **Should main table version be incremented when CDC is added to index impl table?**
   - Current analysis: NO - only impl table and index entity should be incremented
   - Needs validation with query engine team

2. **What happens if CDC creation partially fails?**
   - Some CDC streams created, others failed
   - Versions might be partially incremented
   - Needs recovery mechanism

3. **Performance impact of "helping" approach?**
   - Each CDC operation updates all siblings
   - May cause DB write contention
   - Needs benchmarking

### 8.2 Future Work

1. **Optimization:** Add "sync completed" flag to reduce redundant help operations
2. **Monitoring:** Add metrics for version sync conflicts and helps
3. **Generalization:** Apply helping pattern to other multi-part operations with coordination needs
4. **Documentation:** Update YDB contributor docs with version sync patterns

---

## Appendix A: Glossary

- **AlterVersion / SchemaVersion:** Version number tracking schema changes
- **Barrier:** Coordination mechanism blocking operation parts until all reach barrier
- **CDC Stream:** Change Data Capture stream for replication
- **Index Entity:** The index metadata object (TTableIndexInfo)
- **Index Impl Table:** Physical table storing index data (indexImplTable)
- **Lock-Free Algorithm:** Concurrent algorithm guaranteeing system-wide progress
- **Operation Part:** Sub-operation executing independently within an operation
- **SchemeShard:** Tablet managing schema metadata and DDL operations
- **Helping:** Lock-free pattern where operations assist each other in completion

---

## Appendix B: Example Scenarios

### B.1 Scenario: Table with 3 Indexes - Strategy E

**Initial state:**
```
Table: AlterVersion = 10
Index1: AlterVersion = 10, Impl1: AlterVersion = 10
Index2: AlterVersion = 10, Impl2: AlterVersion = 10
Index3: AlterVersion = 10, Impl3: AlterVersion = 10
```

**Backup creates 3 parallel CDC streams:**

**Timeline with lock-free helping:**
```
T1: Part1(Impl1) HandleReply
    - Increment Impl1: 10 → 11
    - HelpSync: read max=10, sees self=11, max=11
    - Help: Index1=11, Index2=10, Impl2=10, Index3=10, Impl3=10
    - Update: Index1=11, Index2=11, Index3=11, Impl2=11, Impl3=11

T2: Part2(Impl2) HandleReply (concurrent with T1)
    - Increment Impl2: 10 → 11
    - HelpSync: read max=11 (sees T1's updates), self=11, max=11
    - Help: All already at 11, no updates needed

T3: Part3(Impl3) HandleReply (concurrent with T1, T2)
    - Increment Impl3: 10 → 11
    - HelpSync: read max=11, self=11, max=11
    - Help: All already at 11, no updates needed
```

**Final state:**
```
Table: AlterVersion = 10 (unchanged - not incremented for index CDC)
Index1: AlterVersion = 11, Impl1: AlterVersion = 11 ✓
Index2: AlterVersion = 11, Impl2: AlterVersion = 11 ✓
Index3: AlterVersion = 11, Impl3: AlterVersion = 11 ✓
```

**Key insight:** First operation to complete helps all others. Subsequent operations find everything already synced.

### B.2 Scenario: Table with 3 Indexes - Strategy A (Barrier)

**Timeline with barrier:**
```
T1: Part1(Impl1) HandleReply
    - Increment Impl1: 10 → 11
    - RegisterBarrier("cdc_sync_table1")
    - State: Blocked at barrier

T2: Part2(Impl2) HandleReply
    - Increment Impl2: 10 → 11
    - RegisterBarrier("cdc_sync_table1")
    - State: Blocked at barrier

T3: Part3(Impl3) HandleReply
    - Increment Impl3: 10 → 11
    - RegisterBarrier("cdc_sync_table1")
    - State: Blocked at barrier (last one!)
    - Barrier complete: 3 blocked + 0 done = 3 total

T4: DoCheckBarriers triggers
    - Send TEvCompleteBarrier to all 3 parts

T5: VersionSyncPart HandleReply(TEvCompleteBarrier)
    - Read max: Impl1=11, Impl2=11, Impl3=11, max=11
    - Sync: Index1=11, Index2=11, Index3=11
    - All consistent
```

**Final state:** Same as Strategy E, but took extra coordination step.

---

## Appendix C: Decision Matrix

| Criterion | Strategy A (Barrier) | Strategy B (Sequential) | Strategy C (Post-Sync) | Strategy D (Pre-Increment) | Strategy E (Lock-Free) |
|-----------|---------------------|------------------------|------------------------|---------------------------|----------------------|
| **Correctness** | ✅ Guaranteed | ✅ Guaranteed | ⚠️ Eventually | ⚠️ Probabilistic | ✅ Guaranteed |
| **Performance** | ⭐⭐⭐ Good | ⭐ Poor | ⭐⭐⭐⭐ Best | ⭐⭐⭐⭐ Best | ⭐⭐⭐⭐ Best |
| **Complexity** | ⭐⭐⭐ Medium | ⭐⭐ Low | ⭐⭐ Low | ⭐⭐⭐ Medium | ⭐⭐⭐⭐ High |
| **Code Changes** | ⭐⭐ Large | ⭐⭐⭐ Medium | ⭐⭐⭐⭐ Minimal | ⭐⭐⭐ Medium | ⭐⭐⭐⭐ Minimal |
| **Risk** | ⭐⭐⭐⭐ Low | ⭐⭐⭐⭐ Low | ⭐⭐ Medium-High | ⭐⭐ Medium-High | ⭐⭐⭐ Medium |
| **Backup Support** | ✅ Yes | ✅ Yes | ❌ No | ✅ Yes | ✅ Yes |
| **Restore Support** | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |
| **Debugging** | ⭐⭐⭐⭐ Easy | ⭐⭐⭐⭐ Easy | ⭐⭐⭐ Medium | ⭐⭐⭐ Medium | ⭐⭐ Hard |
| **Maintenance** | ⭐⭐⭐⭐ Easy | ⭐⭐⭐⭐ Easy | ⭐⭐⭐ Medium | ⭐⭐⭐ Medium | ⭐⭐ Needs Care |

**Recommendation:** Strategy E (Lock-Free) for optimal performance, with Strategy A (Barrier) as fallback if complexity becomes an issue.

---

*Document Version: 1.0*  
*Date: 2025-01-20*  
*Author: Research Analysis Based on YDB Codebase*

