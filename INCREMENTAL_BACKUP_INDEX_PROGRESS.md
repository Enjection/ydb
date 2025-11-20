# Incremental Backup with Global Indexes - Progress Log

## Problem Statement
Incremental backups fail when tables have global indexes. The error is:
```
path is under operation (EPathStateCopying)
```

This occurs because the backup operation tries to copy index implementation tables, but they're already marked as "under operation" when the index structure is being created.

## Root Cause Analysis

### Issue #1: Index Implementation Tables Not Handled
- Index impl tables are children of index objects, not direct children of the main table
- Backup operation wasn't aware of these impl tables when creating CDC streams
- CreateConsistentCopyTables creates index structures, but CDC streams need to be created for impl tables too

### Issue #2: Duplicate Operations
- Both CreateConsistentCopyTables AND CreateCopyTable were processing indexes
- This caused duplicate "CreateTable" operations for index impl tables
- Led to "path is under operation" errors

### Issue #3: OmitIndexes Flag Behavior
- Setting OmitIndexes=true prevented ALL index processing, including structure creation
- Resulted in "No global indexes for table" error after restore
- Needed to separate "skip impl table copies" from "skip index structure creation"

### Issue #4: Schema Version Mismatch
- Creating CDC streams on source tables increments their AlterVersion (e.g., from 1 to 2)
- Backup copies tables at their current version (version 1)
- Restore creates tables with version 1
- Client metadata queries expect version 2 (the source version after CDC creation)
- Result: "schema version mismatch during metadata loading for: /Root/TableWithIndex/idx/indexImplTable expected 1 got 2"

## Solution Attempts

### Attempt #1: Three-Phase CDC Creation (Lines 100-279 in backup_backup_collection.cpp)
**Approach:** Create CDC streams in three phases:
1. StreamImpl - Create CDC metadata
2. AtTable - Notify datashard to start tracking changes
3. PQ - Create persistent queue infrastructure

**Implementation:**
- Extended protobuf with `IndexImplTableCdcStreams` map to pass CDC info through CreateConsistentCopyTables
- Created CDC StreamImpl for both main table and index impl tables during backup Propose phase
- Passed CDC info through descriptor to CreateConsistentCopyTables
- Created PQ parts after backup copy operations

**Result:** Failed - Schema version mismatch error during restore

**Root cause:** `NCdcStreamAtTable::FillNotice` sets `TableSchemaVersion = table->AlterVersion + 1` when creating CDC streams. This increments the version on source tables, but backups copy at the old version.

### Attempt #2: Skip AlterVersion Increment Flag
**Approach:** Add flag to prevent CDC streams from incrementing AlterVersion during backup

**Changes made:**
1. Added `SkipAlterVersionIncrement` field to `TCreateCdcStream` protobuf (flat_scheme_op.proto:1066)
2. Added `SkipCdcAlterVersionIncrement` field to `TTxState` (schemeshard_tx_infly.h:89)
3. Modified `NCdcStreamAtTable::FillNotice` to check flag and skip version increment (schemeshard_cdc_stream_common.cpp:20)
4. Propagated flag through copy_table.cpp, schemeshard_impl.cpp, schemeshard__init.cpp
5. Set flag to true in backup_backup_collection.cpp when creating CDC streams

**Result:** Failed - Datashard VERIFY panic

**Error:**
```
VERIFY failed: pathId [OwnerId: 72057594046644480, LocalPathId: 2] old version 1 new version 1
AlterTableSchemaVersion(): requirement oldTableInfo->GetTableSchemaVersion() < newTableInfo->GetTableSchemaVersion() failed
```

**Root cause:** Datashard enforces strict invariant that new schema version MUST be greater than old version. Skipping the increment violates this invariant.

## Next Steps (To Be Implemented)

### Proposed Solution: Version Synchronization
Instead of skipping version increments, capture and restore schema versions:

1. **Capture versions during backup:**
   - Store source table schema versions in backup metadata
   - Include both main table and index impl table versions
   - Persist this info in backup descriptor

2. **Restore with correct versions:**
   - When restoring tables, set their initial schema versions to match captured source versions
   - This ensures restored metadata matches what clients expect
   - Prevents "expected X got Y" mismatches

3. **CDC creation remains unchanged:**
   - CDC streams increment versions as normal (maintaining datashard invariants)
   - Backup copies reflect the pre-CDC version
   - Restore synchronizes to the post-CDC version

### Files to Modify (Version Sync Approach)
- `schemeshard__operation_backup_backup_collection.cpp` - Capture schema versions
- `schemeshard__operation_restore_backup_collection.cpp` - Restore with captured versions
- Protobuf definitions - Add version capture fields if needed

## Current Status
- Reverted Attempt #2 (SkipAlterVersionIncrement flag)
- Completed Phase 1 research: Added diagnostic logging and collected version information
- **Phase 2 Analysis In Progress**: Deep investigation of version synchronization mechanism

## Detailed Investigation Findings

### Phase 2: Version Synchronization Analysis

#### Test Log Analysis (Lines 1476-3413)

**After CDC StreamImpl Creation (Schemeshard logs):**
```
Line 1476: MainTable AlterVersion: 1
Line 1477: Index AlterVersion: 1  
Line 1478: IndexImplTable AlterVersion: 1
```

**After Backup Operation (Test diagnostics):**
```
Line 3409: Main table SchemaVersion: 2
Line 3413: Index impl table SchemaVersion: 2
```

**Error at Line 3408:**
```
schema version mismatch during metadata loading for: /Root/TableWithIndex/idx/indexImplTable 
expected 1 got 2
```

#### Timeline Reconstruction

1. **DoCreateStreamImpl** (backup_backup_collection.cpp line 66-178)
   - Creates CDC StreamImpl sub-operations for main table and index impl tables
   - Diagnostic logging shows all versions at 1
   - Returns immediately without waiting for sub-operations

2. **Backup Operation Completes** 
   - Returns to test
   - CDC sub-operations continue running asynchronously

3. **CDC AtTable Phase Executes** (async, after backup returns)
   - TProposeAtTable::HandleReply (schemeshard__operation_common_cdc_stream.cpp line 376)
   - Calls UpdateTableVersion (line 391) - increments AlterVersion to 2
   - Calls SyncChildIndexes (lines 399, 404) - synchronizes index metadata
   - Calls ClearDescribePathCaches and PublishToSchemeBoard (lines 397-398)

4. **Test Queries Table**
   - Main table SchemaVersion: 2 ✓
   - Index impl table SchemaVersion: 2 ✓
   - But KQP expects version 1 ✗

#### Code Path Analysis

**CDC Stream Creation Flow:**
```
DoCreateStreamImpl (StreamImpl phase)
  → CreateNewCdcStreamImpl (returns sub-operation)
    → TNewCdcStreamImpl state machine: Propose → Done
  
DoCreateStream (AtTable phase) 
  → CreateNewCdcStreamAtTable (returns sub-operation)
    → TNewCdcStreamAtTable state machine:
       ConfigureParts → Propose → ProposedWaitParts → Done
       
       In Propose state (TProposeAtTable::HandleReply):
         → UpdateTableVersion (increments version, syncs indexes)
         → ClearDescribePathCaches (invalidates cache)
         → PublishToSchemeBoard (notifies subscribers)
```

**Version Synchronization Functions:**

1. **UpdateTableVersion** (schemeshard__operation_common_cdc_stream.cpp line 248):
   - Increments table's AlterVersion
   - Checks if it's an index impl table with continuous backup
   - Calls SyncChildIndexes if needed

2. **SyncChildIndexes** (line 182):
   - For each index of the table:
     - Gets the index impl table
     - Calls SyncIndexEntityVersion to sync the **index's** AlterVersion
     - Updates impl table's AlterVersion
     - Clears caches and publishes changes

3. **SyncIndexEntityVersion** (line 154):
   ```cpp
   index->AlterVersion = targetVersion;  // Line 175
   context.SS->PersistTableIndex(operationId, indexPathId);
   ```
   - **This updates the parent index's AlterVersion to match the impl table!**

#### KQP Metadata Loading Flow

**Where "expected 1" comes from:**

1. **kqp_metadata_loader.cpp line 790**:
   ```cpp
   TIndexId(ownerId, index.LocalPathId, index.SchemaVersion)
   ```
   - Creates TIndexId with SchemaVersion from table's index metadata

2. **line 920**:
   ```cpp
   expectedSchemaVersion = GetExpectedVersion(entityName)
   ```
   - Gets expected version from TIndexId

3. **line 92**:
   ```cpp
   return pathId.first.SchemaVersion;
   ```
   - Returns the cached SchemaVersion from TIndexId

4. **line 968**:
   ```cpp
   if (expectedSchemaVersion && *expectedSchemaVersion != navigateEntry.TableId->SchemaVersion)
   ```
   - Compares expected (1) with actual (2) → ERROR

**Where index.SchemaVersion comes from:**

1. **schemeshard_path_describer.cpp line 1438**:
   ```cpp
   entry.SetSchemaVersion(indexInfo->AlterVersion);
   ```
   - Sets index SchemaVersion from the **index's AlterVersion** (not impl table's!)

2. **TIndexDescription constructor (kqp_table_settings.h line 91)**:
   ```cpp
   SchemaVersion(index.GetSchemaVersion())
   ```
   - Stores the SchemaVersion in index metadata

#### The Core Question

The synchronization code exists and should work:
- `SyncIndexEntityVersion` (line 175) updates `index->AlterVersion`
- This is called from `SyncChildIndexes` (line 211)
- Which is called from CDC AtTable phase (lines 399, 404)

**But why is the error still occurring?**

Possible reasons:
1. **Timing**: SyncChildIndexes runs async; maybe not complete before query?
2. **Code path**: Maybe SyncChildIndexes isn't being called for backup CDC streams?
3. **Cache**: Maybe KQP's cache isn't being invalidated for the index?
4. **Wrong target**: Maybe sync is updating wrong index or wrong version?

#### Next Investigation Steps

Need to verify if SyncChildIndexes is actually executing:
1. Add logging to SyncChildIndexes entry/exit
2. Add logging to SyncIndexEntityVersion entry/exit  
3. Check if the backup CDC streams trigger the continuous backup path
4. Verify the index->AlterVersion is actually being updated to 2
5. Check if PublishToSchemeBoard is called for the index path

## Key Learnings
1. Datashard schema version invariants are strict and cannot be bypassed
2. CDC streams inherently modify source table metadata (version increment)
3. Backup/restore must account for metadata changes that occur during backup process
4. Index impl tables are separate entities that need explicit CDC handling
