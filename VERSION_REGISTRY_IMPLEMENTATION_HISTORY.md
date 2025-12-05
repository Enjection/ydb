# PendingVersionChanges Registry Implementation Progress

## Status: PHASE 2 COMPLETE

## Tasks

| # | Task | Status | Notes |
|---|------|--------|-------|
| 1 | Explore schema table IDs | ✅ Done | Use table ID 130 |
| 2 | Explore TSchemeShard structure | ✅ Done | Add member after line 303 |
| 3 | Explore initialization patterns | ✅ Done | Use LoadXxx pattern with tuples |
| 4 | Explore ya.make structure | ✅ Done | Add after line 315 |
| 5 | Create schemeshard_version_registry.h | ✅ Done | EClaimResult, TPendingVersionChange, TVersionRegistry |
| 6 | Create schemeshard_version_registry.cpp | ✅ Done | Full implementation |
| 7 | Add schema table | ✅ Done | PendingVersionChanges Table<130> |
| 8 | Integrate into TSchemeShard | ✅ Done | VersionRegistry member + Persist methods |
| 9 | Add loading in init | ✅ Done | LoadPendingVersionChanges in ReadEverything |
| 10 | Update ya.make | ✅ Done | Added .cpp and .h files |
| 11 | Add operation lifecycle hooks | ✅ Done | DoDoneTransactions cleanup, AbortOperationPropose rollback |
| 12 | Migrate incremental restore finalize | ✅ Done | SyncIndexSchemaVersions now uses registry |
| 13 | Migrate CDC stream operations | ✅ Done | TProposeAtTable now uses registry |
| 14 | Migrate copy table operations | ✅ Done | Copy table now uses registry |
| 15 | Remove old sync functions | ✅ Done | HelpSyncSiblingVersions, SyncIndexEntityVersion, SyncChildIndexes removed |

## Investigation Results

### Schema Table IDs
- Highest table ID: 129 (StreamingQueryState)
- **Used table ID 130 for PendingVersionChanges**

### TSchemeShard Structure
- Added TVersionRegistry member after line 303 (after ShardDeletionSubscribers)
- Added PersistPendingVersionChange and PersistRemovePendingVersionChange methods

### Initialization Patterns
- Added TPendingVersionChangeRec tuple type
- Created LoadPendingVersionChanges function
- Loading added after SystemShardsToDelete in ReadEverything

### ya.make Structure
- Added schemeshard_version_registry.cpp and .h after schemeshard_validate_ttl.cpp

## Files Created/Modified

- [x] `ydb/core/tx/schemeshard/schemeshard_version_registry.h` - NEW
- [x] `ydb/core/tx/schemeshard/schemeshard_version_registry.cpp` - NEW
- [x] `ydb/core/tx/schemeshard/schemeshard_schema.h` - MODIFY (added PendingVersionChanges table)
- [x] `ydb/core/tx/schemeshard/schemeshard_impl.h` - MODIFY (added include, VersionRegistry member, Persist methods)
- [x] `ydb/core/tx/schemeshard/schemeshard_impl.cpp` - MODIFY (added Persist implementations)
- [x] `ydb/core/tx/schemeshard/schemeshard__init.cpp` - MODIFY (added loading)
- [x] `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp` - MODIFY (added cleanup hooks)
- [x] `ydb/core/tx/schemeshard/schemeshard__operation.cpp` - MODIFY (added rollback in AbortOperationPropose)
- [x] `ydb/core/tx/schemeshard/schemeshard__operation_incremental_restore_finalize.cpp` - MODIFY (migrated to use registry)
- [x] `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp` - MODIFY (migrated to use registry, removed old functions)
- [x] `ydb/core/tx/schemeshard/schemeshard__operation_copy_table.cpp` - MODIFY (migrated to use registry, added parent index sync)
- [x] `ydb/core/tx/schemeshard/schemeshard_cdc_stream_common.h` - MODIFY (removed old function declarations)
- [x] `ydb/core/tx/schemeshard/ya.make` - MODIFY (added new files)
- [x] `ydb/core/tx/schemeshard/schemeshard__operation_alter_table.cpp` - MODIFY (added parent index sync for impl tables)

## Key Implementation Details

### TVersionRegistry API
```cpp
EClaimResult ClaimVersionChange(opId, pathId, currentVersion, targetVersion, opType, debugInfo);
bool HasPendingChange(pathId);
ui64 GetEffectiveVersion(pathId, currentVersion);
void MarkApplied(txId);
void RollbackChanges(txId);
void RemoveTransaction(txId);
void LoadChange(change);
```

### EClaimResult
- `Claimed` - First sibling to claim, update in-memory state
- `Joined` - Another sibling already claimed, use GetEffectiveVersion
- `Conflict` - Different TxId claimed (should not happen in normal flow)

### Sibling Convergence
- Claims are at TxId level (not SubTxId)
- Multiple siblings can join the same claim
- **First sibling's claim is authoritative** - subsequent siblings join without updating ClaimedVersion
- When `Claimed`: caller updates in-memory state and persists
- When `Joined`: caller does NOT update (in-memory already correct from first sibling)
- All siblings use GetEffectiveVersion() for consistent values

### Version Increment Responsibility
- **Who increments**: The first sibling to run HandleReply gets `Claimed` and increments `table->AlterVersion`
- Subsequent siblings get `Joined` and skip the in-memory update (it's already done)
- All siblings call `PersistTableAlterVersion()`, which persists the current in-memory value (same for all)

## Bug Fixes (2025-12-05)

### Fix 1: Remove Max() when joining claims
- Removed `Max()` logic when joining an existing claim
- The old logic caused ClaimedVersion to drift when second sibling read already-updated version
- First sibling's target version is now authoritative for the entire TxId

### Fix 2: Restore index-to-implTable version sync (CDC stream)
- For index impl tables with CDC, index version must equal impl table version (not just increment)
- Changed from `newIndexVersion = oldIndexVersion + 1` to `newIndexVersion = effectiveTableVersion`
- This preserves the original sync semantics: `index->AlterVersion = implTable->AlterVersion`

### Fix 3: Restore index-to-implTable version sync (incremental restore finalize)
- Same issue in restore finalize - index was being incremented instead of synced to table version
- Changed from `newVersion = oldVersion + 1` to `tableVersion = table->AlterVersion` (after FinishAlter)
- Index version now correctly syncs to impl table version

### Fix 4: Always sync index even when AlterData is null
- When multiple operation parts run, one might finalize the table (clearing AlterData)
- The old code skipped index sync when AlterData was null (via `continue`)
- Now: always sync index version, even if AlterData was already cleared by another part
- Also publish main table to scheme board to ensure metadata consistency

### Fix 5: Bump main table version when index version changes
- KQP loads table metadata which includes `TIndexDescription::SchemaVersion`
- This SchemaVersion is compared against the impl table's actual version
- If only the index is updated but not the main table, scheme cache may serve stale metadata
- Now: when syncing index version, also bump the parent table's AlterVersion
- This forces scheme cache to refresh and pick up the new index SchemaVersion

### Fix 6: Sync parent index version in copy table for impl tables
- **Root cause**: When copying an index impl table with CDC, the copy table operation bumps the impl table version
- But the parent INDEX version (TTableIndexInfo::AlterVersion) was NOT synced
- KQP compares TIndexDescription::SchemaVersion (from parent table metadata) with impl table's actual version
- If they don't match: "schema version mismatch during metadata loading for: /path/to/indexImplTable"
- **The fix**: In TCopyTable::TPropose::HandleReply, when the source is an impl table:
  - Sync the parent index version to match the impl table version
  - Also bump the main table (grandparent) to refresh scheme cache
- This ensures TIndexDescription::SchemaVersion == implTable->AlterVersion

### Fix 7: REVERTED - InitializeBuildIndex/FinalizeBuildIndex fixes were wrong
- **Problem discovered**: These operations target the MAIN TABLE, not the impl table!
  - `InitializeBuildIndex` uses `tablePathId` = main table
  - `FinalizeBuildIndex` uses `tablePathId` = main table
- The fix that checked `if (parent.IsTableIndex())` would NEVER execute because main table's parent is a directory
- **Action taken**: Reverted these ineffective fixes
- **Files reverted**:
  - `schemeshard__operation_initiate_build_index.cpp` - Removed ineffective code
  - `schemeshard__operation_finalize_build_index.cpp` - Removed ineffective code

### Fix 8: Sync parent index version in TAlterTable for impl tables
- **Root cause**: When AlterTable runs on an impl table (including FinalizeBuildIndexImplTable during ApplyBuildIndex), it bumps the impl table version but doesn't sync the parent index
- ApplyBuildIndex flow:
  1. FinalizeBuildIndexMainTable - bumps main table version
  2. AlterTableIndex - changes index state to Ready
  3. FinalizeBuildIndexImplTable (via TAlterTable) - bumps impl table version ← PROBLEM
- The impl table version bump wasn't synced to parent index, causing version mismatch
- **Error**: "schema version mismatch during metadata loading for: indexImplTable expected 1 got 2"
- **The fix**: In TAlterTable::TPropose::HandleReply (schemeshard__operation_alter_table.cpp):
  - After `table->FinishAlter()`, check if parent is an index
  - If so, sync parent index's AlterVersion to match the impl table version using TVersionRegistry
  - Also bump the grandparent (main table) to refresh scheme cache
- **Files modified**:
  - `schemeshard__operation_alter_table.cpp` - Added parent index sync after FinishAlter()

## Next Steps (Phase 3)

1. Build and test the changes
2. Add unit tests for TVersionRegistry
3. Run parts permutation tests to verify fix

## Removed Functions

The following old version synchronization functions have been removed and replaced with TVersionRegistry:

- `HelpSyncSiblingVersions` - Was in anonymous namespace in CDC stream file
- `SyncIndexEntityVersion` - Was in NCdcStreamState namespace
- `SyncChildIndexes` - Was in NCdcStreamState namespace

## Timeline

- Started: 2025-12-05
- Phase 1 Complete: 2025-12-05
- Phase 2 Complete: 2025-12-05
