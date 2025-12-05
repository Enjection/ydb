# Version Sync Analysis - Root Cause Investigation

## CRITICAL FINDING: My InitializeBuildIndex/FinalizeBuildIndex Fixes Are Wrong!

**Problem:** These operations target the MAIN TABLE, not the impl table!
- `InitializeBuildIndex` targets `tablePathId` = base table (e.g., PathId 2)
- `FinalizeBuildIndex` targets `tablePathId` = base table
- My fix checks `if (parent.IsTableIndex())` which will NEVER be true for base tables

**Action Required:** Revert or remove these ineffective fixes.

---

## Current Understanding

### The Error
```
schema version mismatch during metadata loading for: /Root/TableWithIndex/idx/indexImplTable expected 1 got 2
```

### KQP Invariant (from kqp_metadata_loader.cpp:963)
```cpp
if (entry.TableId.SchemaVersion != expectedSchemaVersion) {
    // ERROR
}
```
Where:
- `expectedSchemaVersion` = `TIndexDescription.SchemaVersion` (from parent table's index list)
- `entry.TableId.SchemaVersion` = impl table's actual version from scheme cache

**The invariant:** `TIndexDescription.SchemaVersion == implTable.AlterVersion`

### Source of TIndexDescription.SchemaVersion
From schemeshard_path_describer.cpp:1439:
```cpp
entry.SetSchemaVersion(indexInfo->AlterVersion);
```

So: `TIndexDescription.SchemaVersion` ← `TTableIndexInfo::AlterVersion`

## Problem: InitializeBuildIndex/FinalizeBuildIndex Fixes Are WRONG

### What I thought:
- InitializeBuildIndex bumps impl table version
- FinalizeBuildIndex bumps impl table version
- Need to sync parent index in both

### What's actually true:
InitializeBuildIndex and FinalizeBuildIndex target the **MAIN TABLE**, not the impl table!

```cpp
// schemeshard__operation_initiate_build_index.cpp:421
TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxInitializeBuildIndex, tablePathId);
// tablePathId = main table (e.g., /Root/TableWithIndex), NOT impl table!

// schemeshard__operation_finalize_build_index.cpp:430
TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxFinalizeBuildIndex, tablePathId);
// Same - targets main table
```

So my fix that checks "if parent is index" will NEVER execute for these operations!

## Complete Operation Flow for Backup

### CreateConsistentCopyTables creates these sub-operations (same TxId):

1. **CreateCopyTable** - Copy main table (with CDC stream embedded)
2. **CreateNewTableIndex** - Create index structure at destination
3. **CreateCopyTable** - Copy impl table (with CDC stream embedded)
   - **This bumps SOURCE impl table version!**
   - **This syncs parent index version!**
   - **This bumps main table (grandparent) version!**

### Key Insight: CDC Streams Are Embedded, Not Separate Operations!

The CDC streams are NOT created as separate TxCreateCdcStreamAtTable operations. They are EMBEDDED in the CreateCopyTable via `CreateSrcCdcStream` field!

So the flow is:
1. CreateCopyTable copies impl table
2. As part of copying, it also creates CDC stream on source
3. The source impl table version is bumped ONCE, not twice

### So Where Does Version 3 Come From?

Looking at the log more carefully:
- The impl table goes 1→3 during **CreateIndexedTable** (TxId 281474976710657), NOT during backup!
- This happens BEFORE any backup operations run

### Operations in CreateIndexedTable for Sync Index:

1. SubTxId 0: CreateTable (base) → base table version = 1
2. SubTxId 1: CreateTableIndex (index) → index version = 1
3. SubTxId 2: CreateTable (impl) → impl table version = 1

For sync indexes, there should NOT be InitializeBuildIndex/FinalizeBuildIndex. Those are for async index building only.

### The Mystery: Why Does Impl Table Go 1→3 During CreateIndexedTable?

Possible explanations:
1. There ARE additional build index operations for sync indexes (need to verify)
2. Something in the CreateTable Done phase bumps version more than once
3. The log shows intermediate states we're misinterpreting

## Correct Fixes (Applied)

1. ✅ **schemeshard__operation_copy_table.cpp** - Syncs parent index when copying impl table
2. ✅ **schemeshard__operation_common_cdc_stream.cpp** - Syncs parent index for CDC on impl tables
3. ✅ **schemeshard__operation_alter_table.cpp** - Syncs parent index when altering impl table (including FinalizeBuildIndexImplTable)

## Reverted Incorrect Fixes

1. ~~**schemeshard__operation_initiate_build_index.cpp**~~ - REVERTED: Target is main table, not impl table
2. ~~**schemeshard__operation_finalize_build_index.cpp**~~ - REVERTED: Target is main table, not impl table

These fixes incorrectly checked `if (parent.IsTableIndex())` which would NEVER be true for the operations' target paths (main table).

## Sibling Conflict Analysis

For CreateConsistentCopyTables:
- All sub-operations share the same TxId
- Each PathId gets its own registry claim
- First sibling to claim a PathId wins
- Subsequent siblings get Joined and use the first claim's target version

This is CORRECT behavior - no conflicts expected.

## Root Cause Hypothesis

The error "expected 1 got 2" suggests:
- `TIndexDescription.SchemaVersion` = 1 (index's AlterVersion when parent table metadata was cached)
- `entry.TableId.SchemaVersion` = 2 (impl table's actual version)

This is a **caching issue**:
1. KQP loads parent table metadata (which has TIndexDescription with SchemaVersion from index)
2. KQP then loads impl table (which has its own version)
3. If these are loaded at different times, they can be inconsistent

## Fix Locations (All Addressed)

All operations that bump impl table version now sync parent index:
1. ✅ `schemeshard__operation_apply_build_index.cpp` - FinalizeIndexImplTable → handled by alter_table.cpp fix
2. ✅ `schemeshard__operation_alter_table.cpp` - TAlterTable::TPropose::HandleReply syncs parent index
3. ✅ `schemeshard__operation_copy_table.cpp` - Syncs parent index when copying impl table
4. ✅ `schemeshard__operation_common_cdc_stream.cpp` - Syncs parent index for CDC on impl tables

All fixes use TVersionRegistry for sibling coordination to handle concurrent sub-operations.
