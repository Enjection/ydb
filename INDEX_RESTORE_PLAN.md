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

---

## Implementation Summary - November 14, 2025

### What Was Attempted

An implementation was attempted following the plan outlined above to add incremental restore support for global indexes. The approach involved:

1. **Index Discovery**: Added code to discover index backups from the `__ydb_backup_meta/indexes/` directory structure during incremental restore
2. **Parallel Restore Operations**: Created separate `ESchemeOpRestoreMultipleIncrementalBackups` operations for each index implementation table
3. **Unified Tracking**: Tracked index restore operations alongside table operations in the existing `InProgressOperations` and `TableOperations` structures
4. **Finalization Enhancement**: Modified the finalization path collection to include index implementation table paths

### Critical Discovery: Fundamental Architecture Mismatch

During implementation and testing, a **fundamental architectural misunderstanding** was uncovered that invalidates the entire approach:

#### The Problem

**Incremental backups store ONLY data changes, not schema definitions.**

The restore flow is actually:
1. **Full backup restore** creates the entire table structure including ALL indexes (schema + implementation tables)
2. **Incremental backup restore** applies only DATA changes to existing tables and their index implementation tables

#### What Was Wrong With Our Approach

Our implementation attempted to:
- Discover index metadata from incremental backup directories
- Create/restore index structures during incremental restore
- Map index backups to target table indexes

This is **fundamentally incorrect** because:

1. **Indexes already exist**: When incremental restore runs, the table and all its indexes have already been created by the full backup restore
2. **No schema in incremental backups**: The `__ydb_backup_meta/indexes/` directory contains only index implementation table DATA, not schema
3. **Wrong operation type**: We were trying to use restore operations to create structures that should already exist

#### Evidence From Test Logs

The test `MultipleIndexesIncrementalRestore` failed with:
```
Index index1 not found on target table: /Root/MultiIndexTable
```

This was because:
- The test drops the table (including indexes)
- Runs RESTORE which should recreate everything from the full backup
- The full backup restore is responsible for recreating the table WITH indexes
- Incremental restore should only be applying data changes to existing structures

Looking at the initial table creation logs, we can see indexes are created as:
```
/Root/MultiIndexTable/index1/indexImplTable
/Root/MultiIndexTable/index2/indexImplTable
/Root/MultiIndexTable/index3/indexImplTable
```

These are created during the **table creation** operation (`ESchemeOpCreateIndexedTable`), not during restore discovery.

### The Correct Architecture

The proper incremental backup/restore flow for indexes is:

#### Full Backup
1. Copies table schema (including index definitions)
2. Backs up table data
3. Backs up index implementation table data to `__ydb_backup_meta/indexes/{table}/{index}/`

#### Full Restore
1. Recreates table WITH all index structures from backup schema
2. Restores table data
3. Restores index implementation table data (indexes already exist)

#### Incremental Backup
1. Captures CDC stream changes for table
2. Captures CDC stream changes for index implementation tables
3. Stores changes in `__ydb_backup_meta/indexes/{table}/{index}/` structure

#### Incremental Restore
1. **Assumes indexes already exist** (created by full restore)
2. Applies table data changes
3. Applies index implementation table data changes to **existing** indexes

### What Actually Needs to Be Fixed

The real issue is NOT in incremental restore - it's in **full backup restore**:

1. **Full backup** needs to properly store table schema including index definitions
2. **Full restore** needs to recreate tables WITH their indexes from the stored schema
3. Incremental restore should work as-is once indexes exist

The current incremental restore code path (`ESchemeOpRestoreMultipleIncrementalBackups`) likely already handles restoring data to index implementation tables - we just need to ensure the full restore creates them first.

### Code Changes Made (Now Understood to Be Incorrect)

Files modified on branch `bugfix/incr-restore/indexes-restore-2`:

1. **`schemeshard_incremental_restore_scan.cpp`**:
   - Added `DiscoverAndCreateIndexRestoreOperations()` - **WRONG APPROACH**
   - Added `CreateSingleIndexRestoreOperation()` - **WRONG APPROACH**
   - Added `FindTargetTablePath()` helper - **WRONG APPROACH**
   - Modified `CollectTargetTablePaths()` to include index implementation tables - **CORRECT** (for finalization)

2. **Compilation fixes**:
   - Fixed `TPath` member access (`.` → `->`)
   - Added `Y_UNUSED` for unused parameters

### Correct Path Forward

To properly support incremental restore for indexes:

#### Option 1: Verify Full Restore Works (Recommended)
1. Verify that full backup stores complete table schema including indexes
2. Verify that full restore recreates tables with all their indexes
3. Test that incremental restore already works with existing indexes
4. If full restore doesn't handle indexes, fix THAT, not incremental restore

#### Option 2: Comprehensive Schema Restoration
If full backup/restore doesn't handle indexes:
1. Enhance full backup to serialize complete table schema (including indexes)
2. Enhance full restore to deserialize and recreate index structures
3. Keep incremental restore focused on data-only operations

### Lessons Learned

1. **Understand the full architecture**: Before implementing features, understand the complete backup/restore lifecycle
2. **Data vs Schema**: Incremental backups are about data changes, not schema changes
3. **Test-driven understanding**: The test failure revealed the architectural truth
4. **Read the source**: The table creation logs showed indexes are created during `ESchemeOpCreateIndexedTable`, not during restore discovery

### Recommendation

**Do not merge branch `bugfix/incr-restore/indexes-restore-2`**

The changes on this branch are based on a fundamental misunderstanding of the backup/restore architecture. The correct fix requires:

1. Investigation of full backup/restore to understand index handling
2. If needed, enhancement of full restore to properly recreate indexes
3. Verification that incremental restore already works for index data once structures exist

The only potentially useful change from this branch is the modification to `CollectTargetTablePaths()` in the finalization logic to ensure index implementation table paths are properly normalized after restore completes.

### Next Steps

1. **Investigate full backup**: Check how indexes are stored in full backup metadata
2. **Investigate full restore**: Check if indexes are recreated from full backup
3. **Run simpler test**: Test full backup → full restore with indexes (no incremental)
4. **Only then**: If that works, test full backup → full restore → incremental restore
5. **Document findings**: Update this plan with correct architecture understanding

### Conclusion

This was a valuable learning experience that revealed the true architecture of YDB's backup/restore system. The initial plan was well-structured but based on incorrect assumptions about where index schema lives and when it's restored. The proper implementation requires understanding and potentially fixing the full backup/restore cycle, not adding schema discovery to incremental restore.

**Status**: Implementation abandoned pending full backup/restore investigation.
**Branch**: `bugfix/incr-restore/indexes-restore-2` should be closed or repurposed.
**Estimated effort for correct fix**: Unknown until full restore is investigated.
