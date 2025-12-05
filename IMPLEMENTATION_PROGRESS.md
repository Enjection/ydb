# Deferred Publishing Implementation Progress

## Status: PHASE 1 - IN PROGRESS

## Goal
Implement deferred publishing to fix schema version sync issues by ensuring all paths are published with final, converged versions after ALL operation parts complete.

---

## Phase 1: Infrastructure

| # | Task | Status | Notes |
|---|------|--------|-------|
| 1 | Add `DeferredPublishPaths` to `TSideEffects` | ⬜ Pending | New member variable |
| 2 | Add `DeferPublishToSchemeBoard()` method | ⬜ Pending | New API |
| 3 | Add `DeferredPublishPaths` to `TOperation` | ⬜ Pending | Track per operation |
| 4 | Add schema table `DeferredPublishPaths` (ID 131) | ⬜ Pending | Persistence |
| 5 | Add `DoPersistDeferredPublishPaths()` | ⬜ Pending | Persist in ApplyOnExecute |
| 6 | Modify `DoDoneTransactions()` to publish deferred | ⬜ Pending | Actual publish logic |
| 7 | Add loading in `schemeshard__init.cpp` | ⬜ Pending | Crash recovery |
| 8 | Unit tests for infrastructure | ⬜ Pending | |

## Phase 2: Operation Migration

| # | Task | Status | Notes |
|---|------|--------|-------|
| 9 | Migrate `schemeshard__operation_copy_table.cpp` | ⬜ Pending | Source table + indexes |
| 10 | Migrate `schemeshard__operation_common_cdc_stream.cpp` | ⬜ Pending | CDC version sync paths |
| 11 | Migrate `schemeshard__operation_alter_table.cpp` | ⬜ Pending | Parent index sync |
| 12 | Migrate `schemeshard__operation_incremental_restore_finalize.cpp` | ⬜ Pending | Restore paths |

## Phase 3: Testing & Validation

| # | Task | Status | Notes |
|---|------|--------|-------|
| 13 | Run parts permutation tests | ⬜ Pending | All permutations must pass |
| 14 | Test crash recovery | ⬜ Pending | Deferred paths survive restart |
| 15 | Run full test suite | ⬜ Pending | No regression |

---

## Files to Create

- [ ] None (all modifications to existing files)

## Files to Modify

### Infrastructure
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.h`
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp`
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation.h`
- [ ] `ydb/core/tx/schemeshard/schemeshard_schema.h`
- [ ] `ydb/core/tx/schemeshard/schemeshard__init.cpp`

### Operations
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation_copy_table.cpp`
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp`
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation_alter_table.cpp`
- [ ] `ydb/core/tx/schemeshard/schemeshard__operation_incremental_restore_finalize.cpp`

---

## Implementation Log

### 2025-12-05
- Created branch `deferred-publishing` from commit `3cd0d03901d`
- Added documentation and implementation plan
- Starting Phase 1 implementation

---

## Key Design Decisions

1. **Opt-in approach**: Only version-sensitive paths use `DeferPublishToSchemeBoard()`
2. **Backward compatible**: Existing `PublishToSchemeBoard()` unchanged
3. **Persistent**: Deferred paths stored in DB for crash recovery
4. **Publish in DoDoneTransactions**: When ALL parts complete, publish with final versions

---

## Quick Reference

### New API
```cpp
// Defer publish until operation completion
void TSideEffects::DeferPublishToSchemeBoard(TOperationId opId, TPathId pathId);
```

### Schema Table
```cpp
struct DeferredPublishPaths : Table<131> {
    struct TxId : Column<1, NScheme::NTypeIds::Uint64> { using Type = TTxId; };
    struct PathOwnerId : Column<2, NScheme::NTypeIds::Uint64> { using Type = TOwnerId; };
    struct PathLocalId : Column<3, NScheme::NTypeIds::Uint64> { using Type = TLocalPathId; };
    using TKey = TableKey<TxId, PathOwnerId, PathLocalId>;
};
```

### Publish Location
```cpp
// In DoDoneTransactions(), after version registry cleanup:
if (!operation->DeferredPublishPaths.empty()) {
    ss->PublishToSchemeBoard(txId, deferredPaths, ctx);
}
```
