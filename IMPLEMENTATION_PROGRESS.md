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
- [x] `ydb/core/tx/schemeshard/schemeshard__operation_copy_table.cpp` - MODIFY (migrated to use registry)
- [x] `ydb/core/tx/schemeshard/schemeshard_cdc_stream_common.h` - MODIFY (removed old function declarations)
- [x] `ydb/core/tx/schemeshard/ya.make` - MODIFY (added new files)

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
- Version converges to Max() of all sibling requests
- All siblings use GetEffectiveVersion() for consistent values

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
