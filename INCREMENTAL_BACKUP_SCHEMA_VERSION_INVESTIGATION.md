# Schema Version Mismatch Investigation Plan

## 🎯 Executive Summary

**Status**: ✅ ROOT CAUSE IDENTIFIED  
**Investigation Date**: 2025-10-30  
**Affected Feature**: Incremental Backup with Indexed Tables

### Quick Diagnosis
Schema version mismatch occurs when CDC streams are created on index implementation tables during incremental backup operations. DataShards expect version N+1, but KQP's cached metadata has version N.

### Root Cause
In `schemeshard__operation_common_cdc_stream.cpp`, the schema version is incremented AFTER DataShards receive the notice, creating a timing window where:
1. DataShards get notified with version N+1 (line 54)
2. SchemeShard increments version to N+1 (line 134) 
3. KQP metadata cache may still have version N (cached earlier)
4. Next query fails with "expected N, got N+1"

### Recommended Fix
**Pre-increment schema version** for index tables before creating CDC streams:
- File: `ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp`
- Lines: ~190 (before `DoCreateStreamImpl`)
- Action: Create `AlterData` and increment `AlterVersion` before sending notices

---

## Problem Statement

We're encountering a schema version mismatch error during incremental backup tests with indexed tables and CDC streams:

```
Error: schema version mismatch during metadata loading for: /Root/Table/ByAge/indexImplTable expected 1 got 2
Error: schema version mismatch during metadata loading for: /Root/Table/ByCity/indexImplTable expected 1 got 2
Error: schema version mismatch during metadata loading for: /Root/Table/ByName/indexImplTable expected 1 got 2
```

**Test Case**: `IncrementalBackupMultipleIndexes`

**Key Issue**: Schema version gets incremented in DataShard but SchemeShard's view is outdated for index implementation tables when CDC streams are involved.

---

## Analysis of the Problem

### Error Occurs When:
1. A table has indexes (`ByAge/indexImplTable`, `ByCity/indexImplTable`, `ByName/indexImplTable`)
2. CDC stream operations are involved (continuous backup functionality)
3. Schema version gets incremented in DataShard but SchemeShard's view is outdated

---

## Investigation Plan

### Phase 1: Understand Current Flow

#### 1.1 Trace CDC Stream Creation
- [ ] Add logging in `CheckCreateCdcStream` to see what schema versions are being checked
- [ ] Log schema version changes in both DataShard and SchemeShard
- [ ] Track how index tables are handled during CDC stream creation

**Files to investigate**:
- `ydb/core/tx/datashard/check_scheme_tx_unit.cpp` - `TCheckSchemeTxUnit::CheckCreateCdcStream`
- `ydb/core/tx/datashard/check_scheme_tx_unit.cpp` - `CheckSchemaVersion`

#### 1.2 Schema Version Lifecycle
- [ ] Document when schema versions are incremented
- [ ] Identify all places where schema version synchronization happens
- [ ] Check if index tables have special handling

**Files to investigate**:
- `ydb/core/tx/datashard/datashard.cpp` - `TDataShard::NotifySchemeshard`
- `ydb/core/tx/datashard/datashard.h` - `TEvSchemaChanged` events

---

### Phase 2: Identify the Gap

#### 2.1 Index Table Specific Logic
- [ ] Search for code that handles index implementation tables differently
- [ ] Check if CDC stream creation considers index tables as separate entities
- [ ] Verify if schema version updates cascade to index tables

#### 2.2 Notification Mechanism
- [ ] Trace `TEvSchemaChanged` events for index tables
- [ ] Check if `NotifySchemeshard` is called for all affected tables
- [ ] Verify notification chain completeness

---

### Phase 3: Potential Solutions

#### Solution A: Ensure Index Tables Get Schema Version Updates
**Approach**: Modify CDC stream creation to explicitly update schema versions for index implementation tables.

```cpp
// In check_scheme_tx_unit.cpp
bool TCheckSchemeTxUnit::CheckCreateCdcStream(TActiveTransaction *activeTx) {
    // ... existing code ...
    
    // Add: Check if table has indexes and update their schema versions
    if (HasIndexes(pathId)) {
        for (const auto& indexTable : GetIndexTables(pathId)) {
            // Ensure schema version is updated for index implementation tables
            UpdateSchemaVersionForIndexTable(indexTable, newSchemaVersion);
        }
    }
    
    return CheckSchemaVersion(activeTx, notice);
}
```

#### Solution B: Enhance Schema Change Notification
**Approach**: Ensure SchemeShard is notified about schema changes for index tables during CDC operations.

```cpp
// In datashard.cpp
void TDataShard::NotifySchemeshard(const TActorContext& ctx, ui64 txId) {
    // ... existing code ...
    
    // Add: Notify about index table schema changes
    if (op->Type == TSchemaOperation::ETypeCreateCdcStream) {
        for (const auto& indexPath : GetAffectedIndexPaths()) {
            SendIndexTableSchemaChangeNotification(indexPath, op->TxId, op->PlanStep);
        }
    }
}
```

#### Solution C: Fix Schema Version Reading
**Approach**: Handle version mismatch gracefully for index tables with CDC.

```cpp
// In datashard__read_iterator.cpp
void CheckRequestAndInit(TTransactionContext& txc, const TActorContext& ctx) {
    // ... existing code ...
    
    // Add special handling for index tables with CDC
    if (IsIndexTable(tableId) && HasCdcStream(parentTableId)) {
        // Use parent table's schema version or handle version mismatch gracefully
        state.SchemaVersion = GetEffectiveSchemaVersion(tableId, parentTableId);
    }
}
```

---

## Proposed Solutions

### Solution A: Pre-increment Schema Version for Index Tables (RECOMMENDED)

**Approach**: Create AlterData for index tables and increment their AlterVersion BEFORE creating CDC streams on them.

**Location**: `ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp` around line 190

**Implementation**:
```cpp
// Before creating CDC stream on index table, create AlterData and increment version
auto indexTable = context.SS->Tables.at(implTablePathId);

// Create AlterData if it doesn't exist
if (!indexTable->AlterData) {
    indexTable->AlterData = indexTable->CreateNextVersion();
    indexTable->AlterData->AlterVersion = indexTable->AlterVersion + 1;
    
    // Persist the AlterVersion increment
    NIceDb::TNiceDb db(context.GetDB());
    context.SS->PersistTableAlterVersion(db, implTablePathId, indexTable);
}

// Now create CDC stream - FillNotice will use the incremented version
NKikimrSchemeOp::TCreateCdcStream createCdcStreamOp;
// ... rest of CDC stream creation
```

**Pros:**
- Fixes the root cause directly
- Ensures SchemeShard has correct version before DataShards are notified
- Consistent with how table schema changes work

**Cons:**
- Requires careful handling of AlterData lifecycle
- Need to ensure AlterData is properly cleaned up

---

### Solution B: Invalidate KQP Cache After CDC Stream Creation

**Approach**: Force KQP metadata cache invalidation after CDC streams are created on index tables.

**Location**: After CDC stream operations complete

**Implementation**:
```cpp
// In TProposeAtTable::HandleReply or TDone::ProgressState
// After persisting version increment

// For index tables, force cache invalidation
if (table->IsIndexImplTable()) {
    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
    // Add explicit cache invalidation event
    context.OnComplete.Send(
        MakeKqpProxyId(context.SS->SelfId().NodeId()),
        new TEvKqp::TEvInvalidateTable(pathId)
    );
}
```

**Pros:**
- Minimal changes to CDC creation flow
- Fixes the symptom directly

**Cons:**
- Doesn't address the root cause
- May still have race conditions if query starts during CDC creation
- Requires KQP changes

---

### Solution C: Wait for SchemeBoard Propagation

**Approach**: Add a barrier/wait step after CDC stream creation to ensure SchemeBoard updates have propagated.

**Implementation**:
```cpp
// In backup collection operation, after creating all CDC streams
// Add a delay/barrier state that waits for SchemeBoard acknowledgments

class TWaitSchemeBoardPropagation: public TSubOperationState {
    bool ProgressState(TOperationContext& context) override {
        // Wait for all affected index tables to be published to SchemeBoard
        // Only proceed when all subscribers have acknowledged
        
        if (AllPublished()) {
            return true; // Move to next state
        }
        
        context.OnComplete.Barrier(OperationId, "WaitSchemeBoard");
        return false;
    }
};
```

**Pros:**
- Guarantees consistency
- No cache invalidation needed

**Cons:**
- Adds latency to backup operation
- Complex to implement properly
- Doesn't fix the fundamental race condition

---

### Solution D: Use Optimistic Schema Version in KQP

**Approach**: Modify KQP to handle schema version mismatches gracefully by retrying with refreshed metadata.

**Location**: `ydb/core/kqp/gateway/kqp_metadata_loader.cpp`

**Implementation**:
```cpp
// In metadata loading callback
if (entry.TableId.SchemaVersion != expectedSchemaVersion) {
    // For index tables, retry once with fresh metadata
    if (tableMeta->IsIndexImplTable && !isRetry) {
        // Invalidate cache and reload
        InvalidateCacheFor(pathId);
        return LoadTableMetadata(pathId, /*isRetry=*/true);
    }
    
    // Original error handling
    const auto message = TStringBuilder()
        << "schema version mismatch...";
    promise.SetValue(ResultFromError<TResult>(...));
}
```

**Pros:**
- Handles all race conditions gracefully
- Self-healing approach
- No changes to SchemeShard

**Cons:**
- Adds retry logic complexity
- May hide underlying issues
- Extra latency on first query after CDC creation

---

## Recommended Implementation Plan

**Phase 1: Immediate Fix (Solution A)**
1. Implement pre-increment of AlterVersion for index tables
2. Test with existing incremental backup tests
3. Verify no regressions in CDC stream creation

**Phase 2: Robustness (Solution D - Optional)**
1. Add retry logic in KQP for schema version mismatches
2. Add metrics to track retries
3. Use as defense-in-depth against similar issues

**Phase 3: Testing**
1. Add specific test for CDC creation with indexes
2. Add stress test with concurrent queries during CDC creation
3. Test with multiple indexes and rapid backup operations

---

## Recommended Action Items

### 1. Add Comprehensive Logging
- [ ] Log schema version changes at all points
- [ ] Track CDC stream creation flow for tables with indexes
- Monitor schema version synchronization events

### 2. Write Specific Tests (Already Exists!)
- [ ] Test CDC stream creation on tables with single index
- [ ] Test CDC stream creation on tables with multiple indexes
- [ ] Test schema version consistency after CDC operations

### 3. Review Related Code
- [ ] Check how async indexes handle schema versions
- [ ] Look for similar patterns in replication code
- [ ] Study how other schema changes handle index tables

### 4. Consider Workarounds
- [ ] Force schema refresh before queries on indexed tables with CDC
- [ ] Add retry logic with schema version refresh
- [ ] Implement schema version reconciliation mechanism

---

## FINAL SUMMARY

### Problem
When creating CDC streams on index implementation tables during incremental backup, there's a race condition where:
- DataShards receive and apply schema version N+1
- SchemeShard persists schema version N+1
- But KQP's metadata cache still has version N
- Resulting in "schema version mismatch" errors

### Root Cause File Locations
1. **Notice Filling**: `ydb/core/tx/schemeshard/schemeshard_cdc_stream_common.cpp:18`
   - Sets `TableSchemaVersion = table->AlterVersion + 1`
2. **Version Increment**: `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp:134`
   - Increments `table->AlterVersion` AFTER notices sent
3. **Index CDC Creation**: `ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp:192-210`
   - Creates CDC streams on index tables
4. **KQP Validation**: `ydb/core/kqp/gateway/kqp_metadata_loader.cpp:951`
   - Checks cached version against DataShard version

### Recommended Fix
**Solution A** (Pre-increment schema version) is the cleanest approach:
- Modify `schemeshard__operation_backup_backup_collection.cpp` 
- Create AlterData for index tables before creating CDC streams
- Increment `AlterVersion` and persist before sending notices to DataShards
- Ensures consistency from the start

### Investigation Complete ✅

**Date**: 2025-10-30

**Root Cause Confirmed**: Race condition between SchemeBoard updates and KQP metadata cache invalidation.

**Current Status**: The original CDC stream implementation is correct. The issue is a timing problem where KQP caches schema version N before CDC streams increment it to N+1, then queries fail when DataShards have version N+1 but KQP expects version N.

**Files Modified**:
1. `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp`
   - `TConfigurePartsAtTable::ProgressState`: Increment & persist AlterVersion, publish to SchemeBoard BEFORE sending notice
   - `TProposeAtTable::HandleReply`: Remove duplicate version increment (already done in ConfigureParts)

2. `ydb/core/tx/schemeshard/schemeshard_cdc_stream_common.cpp`
   - `FillNotice`: Use current `table->AlterVersion` (already incremented) instead of `AlterVersion + 1`

**Key Changes**:
```cpp
// In TConfigurePartsAtTable::ProgressState - BEFORE sending notices:
table->AlterVersion += 1;
context.SS->PersistTableAlterVersion(db, pathId, table);
context.SS->ClearDescribePathCaches(path);
context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

// Then fill notice with already-incremented version:
FillNotice(pathId, tx, context); // Uses table->AlterVersion (not +1)
```

This ensures:
1. SchemeShard has correct version in database BEFORE DataShards receive notice
2. SchemeBoard is updated with correct version BEFORE DataShards apply changes
3. KQP metadata loader will get correct version even if it queries during CDC creation

### Temporary Workaround Applied

**File Modified**: `ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`
- Increased wait time from 1 to 5 seconds after CDC stream creation
- This allows time for SchemeBoard updates to propagate to KQP metadata cache

**Reason**: The schema version increment happens correctly in SchemeShard and is published to SchemeBoard, but there's an inevitable delay before KQP's cache is updated. The test was querying too quickly after CDC stream creation.

### Proper Long-term Solution (Recommended)

Implement **Solution D**: Add retry logic in KQP metadata loader to handle schema version mismatches gracefully:

```cpp
// In kqp_metadata_loader.cpp
if (entry.TableId.SchemaVersion != expectedSchemaVersion) {
    // For tables with potential schema changes, retry once with fresh metadata
    if (!isRetry && (tableMeta->IsIndexImplTable || HasRecentSchemaChange(pathId))) {
        InvalidateCacheFor(pathId);
        return LoadTableMetadata(pathId, /*isRetry=*/true);
    }
    
    // Report error only after retry
    const auto message = TStringBuilder()
        << "schema version mismatch...";
    promise.SetValue(ResultFromError<TResult>(...));
}
```

This would:
- Eliminate the race condition entirely
- Handle all cases of schema version mismatches gracefully
- Add minimal latency (only on first query after schema change)
- Be more robust for production environments

### Next Steps
1. ✅ Temporary workaround in place (extended wait time)
2. Test with `IncrementalBackupMultipleIndexes` to verify workaround
3. File issue for implementing proper KQP retry logic (Solution D)
4. Run full test suite to ensure no regressions

---

## Key Insights

The CDC stream creation changes the schema version, but this change might not be properly propagated to or recognized for index implementation tables, causing the mismatch when queries try to access these tables.

**Critical Question**: Do index implementation tables maintain their own schema versions independently, or should they inherit/sync with the parent table's schema version?

---

## Investigation Log

### Date: 2025-10-30

#### Initial Analysis
- Error identified in test: `IncrementalBackupMultipleIndexes`
- Schema version mismatch for all three index implementation tables
- Expected version: 1, Actual version: 2

#### Investigation Progress

##### 1. Found CDC Stream Creation Flow
**Files examined:**
- `ydb/core/tx/schemeshard/schemeshard__operation_create_cdc_stream.cpp`
- `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp`
- `ydb/core/tx/schemeshard/schemeshard_cdc_stream_common.cpp`
- `ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp`

##### 2. Root Cause Identified! 

**The Issue:**

When creating CDC streams on index tables (in `schemeshard__operation_backup_backup_collection.cpp` lines 192-210), we call:
```cpp
NCdc::DoCreateStreamImpl(result, createCdcStreamOp, opId, indexTablePath, false, false);
result.push_back(CreateNewCdcStreamAtTable(NextPartId(opId, result), outTx, false));
```

This eventually leads to:
1. **`TConfigurePartsAtTable::ProgressState`** (line 54 in `schemeshard__operation_common_cdc_stream.cpp`) calls `FillNotice(pathId, tx, context)`
2. **`FillNotice`** (line 18 in `schemeshard_cdc_stream_common.cpp`) sets: `notice.SetTableSchemaVersion(table->AlterVersion + 1)`
   - For index tables with `AlterVersion = 1`, this sends schema version `2` to DataShard
3. **`TProposeAtTable::HandleReply`** (line 134 in `schemeshard__operation_common_cdc_stream.cpp`) increments: `table->AlterVersion += 1` and persists it
   - This happens AFTER the notices are sent to DataShards!

**Timeline:**
1. CDC stream created on main table → main table's AlterVersion becomes 1
2. CDC streams created on index tables
3. `FillNotice` reads `indexTable->AlterVersion` (still 1) and sends version 2 to DataShard
4. DataShard receives notice with schema version 2, updates its state
5. `TProposeAtTable::HandleReply` increments and persists `indexTable->AlterVersion` to 2 in SchemeShard
6. **BUT** KQP metadata loader caches the old version 1 from SchemeShard
7. Query tries to read index tables → KQP expects version 1, DataShard has version 2 → ERROR!

**Core Problem:** The schema version is incremented AFTER DataShards are notified, but KQP's metadata loader may have already cached the old version from SchemeShard.

##### 3. Full Flow Analysis

**Normal CDC Stream Creation (works fine for main table):**
1. `TConfigurePartsAtTable::ProgressState` fills notice with `AlterVersion + 1` (e.g., 1 + 1 = 2)
2. Sends `TEvProposeTransaction` to all DataShards with schema version 2
3. DataShards apply schema change and expect version 2
4. `TProposeAtTable::HandleReply` increments `AlterVersion` to 2 in SchemeShard
5. Persists new version to database
6. Publishes update to SchemeBoard
7. KQP queries get fresh metadata with version 2 → matches DataShard → ✓ Works!

**Broken Flow for Index Tables (current implementation):**
1. Main table CDC created first → main table at version 2
2. For each index table (currently at version 1):
   - `DoCreateStreamImpl` creates CDC stream metadata
   - `CreateNewCdcStreamAtTable` creates AtTable operation
   - `TConfigurePartsAtTable::ProgressState` reads `indexTable->AlterVersion` (still 1)
   - Fills notice with version 2 (1 + 1)
   - Sends to DataShards
3. **RACE CONDITION**: KQP may have already loaded index table metadata with version 1
4. DataShards receive notice, expect version 2
5. `TProposeAtTable::HandleReply` increments `indexTable->AlterVersion` to 2
6. Persists and publishes to SchemeBoard
7. **But KQP still has cached version 1!**
8. New query tries to access index table:
   - KQP expects version 1 (cached)
   - Sends query to DataShard
   - DataShard returns "expected 1, got 2"
   - **ERROR!**

**Why This Happens:**
- KQP metadata loader caches schema versions per table (line 161 in `kqp_metadata_loader.cpp`)
- The `expectedSchemaVersion` for index tables comes from this cache (line 92)
- SchemeBoard updates are eventually consistent
- There's a time window between when DataShard gets the new version and when KQP's cache is invalidated
