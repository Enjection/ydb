# Implementation Plan: Incremental Restore for Indexes

## Overview

This document describes the implementation plan for adding incremental restore support for indexes in YDB, extending the existing incremental backup/restore functionality implemented in branch `feature/incr-backup/indexes-support-003`.

## Current State Analysis

### Backup Implementation (Already Done)
- When backing up tables, the system now also backs up global indexes (if `OmitIndexes` is false)
- For each table, it discovers child indexes of type `EIndexTypeGlobal`
- Creates `AlterContinuousBackup` operations for index implementation tables
- Stores index backups at: `{backup_collection}/{timestamp}_inc/__ydb_backup_meta/indexes/{table_path}/{index_name}`
- **Location**: `ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp:240-311`

### Restore Implementation (Tables Only - Current Gap)
- Currently restores only tables, not indexes
- Uses `ESchemeOpRestoreMultipleIncrementalBackups` operation
- Creates separate restore transactions for each table
- **Location**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp:512-599`

## Architecture Design

### Key Principles

1. **Parallel Execution**: Tables and indexes restore data simultaneously
   - Multiple `ESchemeOpRestoreMultipleIncrementalBackups` operations created
   - Each operation targets different shards (table shards vs index impl table shards)
   - No artificial dependencies or sequencing between tables and indexes

2. **Unified Tracking**: Same tracking mechanism for both tables and indexes
   - Both added to `InProgressOperations`
   - Both tracked in `TableOperations` map
   - Both contribute to completion check

3. **Atomic Activation**: Finalization happens together
   - `AreAllCurrentOperationsComplete()` waits for ALL operations (tables + indexes)
   - `FinalizeIncrementalRestoreOperation` activates everything atomically
   - No partial visibility (either all visible or none)

## Implementation Phases

### Phase 1: Parallel Discovery & Operation Creation

**File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`

#### Task 1.1: Extend `CreateIncrementalRestoreOperation`

Add index discovery after table operation creation (after line ~596):

```cpp
void TSchemeShard::CreateIncrementalRestoreOperation(
    const TPathId& backupCollectionPathId,
    ui64 operationId,
    const TString& backupName,
    const TActorContext& ctx) {

    // ... existing table discovery and operation creation ...

    // NEW: Discover and create index restore operations IN PARALLEL
    DiscoverAndCreateIndexRestoreOperations(
        backupCollectionPathId,
        operationId,
        backupName,
        bcPath,
        backupCollectionInfo,
        ctx
    );
}
```

#### Task 1.2: Implement index discovery helper

```cpp
void TSchemeShard::DiscoverAndCreateIndexRestoreOperations(
    const TPathId& backupCollectionPathId,
    ui64 operationId,
    const TString& backupName,
    const TPath& bcPath,
    const TBackupCollectionInfo::TPtr& backupCollectionInfo,
    const TActorContext& ctx);
```

**Logic**:
1. Check if indexes were backed up (`OmitIndexes` flag)
2. Resolve path to index metadata: `{backup}/__ydb_backup_meta/indexes`
3. Iterate through table directories under `indexes/`
4. For each index in each table directory:
   - Find corresponding target table path
   - Validate index exists on target table
   - Create restore operation

#### Task 1.3: Create individual index restore operations

```cpp
void TSchemeShard::CreateSingleIndexRestoreOperation(
    ui64 operationId,
    const TString& backupName,
    const TPath& bcPath,
    const TString& relativeTablePath,
    const TString& indexName,
    const TString& targetTablePath,
    const TActorContext& ctx);
```

**Logic**:
1. Validate target table exists and has the specified index
2. Find index implementation table (child of index path)
3. Construct source path: `{backup}/__ydb_backup_meta/indexes/{table}/{index}`
4. Construct destination path: `{table}/{index}/indexImplTable`
5. Create `ESchemeOpRestoreMultipleIncrementalBackups` operation
6. Track operation in `InProgressOperations` (same as table operations)
7. Track expected shards for index impl table
8. Send restore request

### Phase 2: Helper Functions

**File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`

#### Task 2.1: Add target table path mapper

```cpp
TString TSchemeShard::FindTargetTablePath(
    const TBackupCollectionInfo::TPtr& backupCollectionInfo,
    const TString& relativeTablePath);
```

**Logic**:
- Map backup relative path to restore target path
- Use backup collection's `ExplicitEntryList` to find matching table

### Phase 3: Unified Progress Tracking

**File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`

#### Task 3.1: Verify existing tracking works

**No changes needed!** The existing `TTxProgressIncrementalRestore` already handles this:

```cpp
// Existing code at line ~75:
if (state.AreAllCurrentOperationsComplete()) {
    LOG_I("All operations for current incremental backup completed, moving to next");
    // This includes BOTH table and index operations!
    state.MarkCurrentIncrementalComplete();
    state.MoveToNextIncremental();
}
```

Both table and index operations are tracked in `InProgressOperations` and complete independently.

### Phase 4: Synchronized Finalization

**File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`

#### Task 4.1: Verify atomic activation

Review `FinalizeIncrementalRestoreOperation` to ensure it atomically activates both tables and indexes by sending the `ESchemeOpIncrementalRestoreFinalize` operation.

The finalization should:
1. Wait for ALL operations (tables + indexes) to complete
2. Atomically change path states for all involved paths
3. Make everything visible/active in a single transaction

### Phase 5: Header Declarations

**File**: `ydb/core/tx/schemeshard/schemeshard_impl.h`

#### Task 5.1: Add method declarations

```cpp
void DiscoverAndCreateIndexRestoreOperations(
    const TPathId& backupCollectionPathId,
    ui64 operationId,
    const TString& backupName,
    const TPath& bcPath,
    const TBackupCollectionInfo::TPtr& backupCollectionInfo,
    const TActorContext& ctx);

void CreateSingleIndexRestoreOperation(
    ui64 operationId,
    const TString& backupName,
    const TPath& bcPath,
    const TString& relativeTablePath,
    const TString& indexName,
    const TString& targetTablePath,
    const TActorContext& ctx);

TString FindTargetTablePath(
    const TBackupCollectionInfo::TPtr& backupCollectionInfo,
    const TString& relativeTablePath);
```

#### Task 5.2: Optional tracking enhancement

Add flag to distinguish index vs table operations (for debugging):

```cpp
struct TTableOperationState {
    TOperationId OperationId;
    THashSet<TShardIdx> ExpectedShards;
    THashSet<TShardIdx> CompletedShards;
    bool IsIndexOperation = false;  // NEW: for logging/debugging
};
```

### Phase 6: Testing

**File**: `ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`

#### Test Scenarios

1. **Basic index restore**: Backup and restore table with one global index
2. **Multiple indexes**: Table with multiple global indexes
3. **Mixed scenario**: Multiple tables, each with different number of indexes
4. **Index data verification**: Verify index data is correctly restored
5. **Parallel completion**: Verify indexes and tables complete in parallel
6. **Incremental sequences**: Multiple incremental backups with index changes
7. **OmitIndexes flag**: Verify restore works gracefully when indexes weren't backed up
8. **Missing index**: Index in backup but not on target table (should warn and skip)

## Files to Modify

### Core Implementation
1. **`ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`**
   - Add `DiscoverAndCreateIndexRestoreOperations()`
   - Add `CreateSingleIndexRestoreOperation()`
   - Add `FindTargetTablePath()` helper
   - Call from `CreateIncrementalRestoreOperation()`

2. **`ydb/core/tx/schemeshard/schemeshard_impl.h`**
   - Add method declarations
   - Optional: Add `IsIndexOperation` flag to tracking structure

### Testing
3. **`ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`**
   - Add comprehensive test suite for index restore scenarios

### No Changes Needed
- **Proto definitions**: Reuse existing `TRestoreMultipleIncrementalBackups`
- **Progress tracking**: Already handles mixed operations
- **Finalization**: Should already be atomic

## Key Implementation Details

### Path Structure

**Backup structure**:
```
{backup_collection}/
  {timestamp}_incremental/
    table1/              # Main table data
    table2/
    __ydb_backup_meta/
      indexes/
        table1/
          index1/        # Index implementation table data
          index2/
        table2/
          index1/
```

**Restore mapping**:
- Source: `{backup}/__ydb_backup_meta/indexes/{table}/{index}`
- Target: `{table_path}/{index_name}/indexImplTable`

### Index Types

Only `EIndexTypeGlobal` indexes are backed up and restored (sync indexes). Async indexes (vector indexes, etc.) are excluded.

### Error Handling

1. **Missing indexes on target**: Log warning, skip that index, continue with others
2. **OmitIndexes=true in backup**: Skip index restore entirely, log info message
3. **Schema mismatches**: Let the restore operation fail naturally (same as table restore)
4. **Index metadata directory missing**: Log info, continue (valid case if no indexes backed up)

### Performance Considerations

- Index restores run **in parallel** with table restores
- Each targets different shards, no contention
- Completion tracked together, finalization is atomic
- No artificial sequencing or dependencies

## Estimated Complexity

- **Phase 1 (Parallel Discovery)**: ~400-500 lines
- **Phase 2 (Helpers)**: ~100-150 lines
- **Phase 3 (Tracking)**: ~0 lines (already works!)
- **Phase 4 (Finalization)**: ~50-100 lines (verification/minor tweaks)
- **Phase 5 (Headers)**: ~50 lines
- **Phase 6 (Testing)**: ~500-800 lines

**Total estimated effort**: 1-2 weeks

## Success Criteria

1. ✅ Index backups are discovered during restore
2. ✅ Index restore operations created in parallel with table operations
3. ✅ All operations (tables + indexes) tracked together
4. ✅ Atomic activation when all complete
5. ✅ Data integrity: index data matches expected state after restore
6. ✅ Tests pass for all scenarios
7. ✅ Graceful handling of edge cases (missing indexes, OmitIndexes flag, etc.)

## References

- **Base branch**: `Enjection:feature/incr-backup/indexes-support-003`
- **Backup implementation**: `schemeshard__operation_backup_incremental_backup_collection.cpp:240-311`
- **Table restore**: `schemeshard_incremental_restore_scan.cpp:512-599`
- **Progress tracking**: `schemeshard_incremental_restore_scan.cpp:30-200`
