# Deferred Publishing Implementation Plan

## Executive Summary

This document describes the implementation plan for **deferred publishing** - a solution to the schema version synchronization problem where multi-part operations publish to scheme board before all sibling parts have converged on final versions.

**Goal:** Ensure all paths are published with their final, converged versions after ALL operation parts complete.

---

## Part 1: Problem Statement

### 1.1 Current Behavior

```
Timeline (Current - Problematic):

  t0: Part A starts HandleReply
  t1: Part A bumps implTable.AlterVersion = 2
  t2: Part A calls PublishToSchemeBoard(implTable)  ← Queued for publish
  t3: ApplyOnComplete runs
  t4: DoPublishToSchemeBoard() sends implTable with version=2 to scheme board

  t5: Part B starts HandleReply (AFTER Part A's publish!)
  t6: Part B syncs index.AlterVersion = 2
  t7: Part B bumps mainTable.AlterVersion = 2
  t8: Part B calls PublishToSchemeBoard(index, mainTable)
  t9: ApplyOnComplete runs
  t10: DoPublishToSchemeBoard() sends index, mainTable

  PROBLEM: At t4, scheme board received implTable publish
           but mainTable's TIndexDescription.SchemaVersion was still 1!
           KQP query between t4 and t10 sees mismatch.
```

### 1.2 Root Cause

Publishing happens during `ApplyOnComplete()` of **each individual part**, not after all parts have finished. This means:

1. Early parts publish before later parts have updated related versions
2. Scheme cache receives partial/inconsistent state
3. KQP queries may see version mismatches

### 1.3 Affected Operations

Operations with multiple parts that modify related resources:
- `CreateConsistentCopyTables` (backup with indexes)
- `CreateCdcStream` on indexed tables
- `IncrementalRestoreFinalize`
- Any multi-part operation with parent-child version dependencies

---

## Part 2: Solution Design

### 2.1 Core Concept

**Defer publishing of version-sensitive paths until ALL operation parts complete.**

```
Timeline (Deferred - Correct):

  t0: Part A starts HandleReply
  t1: Part A bumps implTable.AlterVersion = 2
  t2: Part A calls DeferPublishToSchemeBoard(implTable)  ← Queued, NOT published
  t3: ApplyOnComplete runs (no publish for deferred paths)

  t4: Part B starts HandleReply
  t5: Part B syncs index.AlterVersion = 2
  t6: Part B bumps mainTable.AlterVersion = 2
  t7: Part B calls DeferPublishToSchemeBoard(index, mainTable)
  t8: ApplyOnComplete runs (no publish for deferred paths)

  t9: ALL parts complete → DoDoneTransactions()
  t10: Publish ALL deferred paths with FINAL versions
       - implTable with version=2
       - index with version=2
       - mainTable with version=2 (TIndexDescription.SchemaVersion=2)

  RESULT: All paths published atomically with consistent versions
```

### 2.2 Design Principles

1. **Backward Compatible**: Existing `PublishToSchemeBoard()` continues to work for non-problematic operations
2. **Opt-In**: Only version-sensitive paths use deferred publishing
3. **Atomic Publish**: All deferred paths published together in `DoDoneTransactions()`
4. **Persistent**: Deferred paths persisted for crash recovery

### 2.3 API Changes

```cpp
// NEW: Defer publish until operation completion
void TSideEffects::DeferPublishToSchemeBoard(TOperationId opId, TPathId pathId);

// EXISTING: Immediate publish (unchanged for backward compatibility)
void TSideEffects::PublishToSchemeBoard(TOperationId opId, TPathId pathId);
```

---

## Part 3: Implementation Details

### 3.1 Data Structures

#### 3.1.1 TSideEffects Changes

```cpp
// schemeshard__operation_side_effects.h

class TSideEffects {
private:
    // EXISTING: Immediate publish paths
    TPublications PublishPaths;

    // NEW: Deferred publish paths (published in DoDoneTransactions)
    TPublications DeferredPublishPaths;

public:
    // NEW METHOD
    void DeferPublishToSchemeBoard(TOperationId opId, TPathId pathId);

private:
    // NEW: Persist deferred paths
    void DoPersistDeferredPublishPaths(TSchemeShard* ss,
                                        NTabletFlatExecutor::TTransactionContext& txc,
                                        const TActorContext& ctx);

    // NEW: Actual publish of deferred paths (called from DoDoneTransactions)
    void DoPublishDeferredPaths(TSchemeShard* ss,
                                TTxId txId,
                                const TActorContext& ctx);
};
```

#### 3.1.2 TOperation Changes

```cpp
// schemeshard__operation.h

class TOperation {
public:
    // NEW: Track deferred paths per operation
    THashSet<TPathId> DeferredPublishPaths;

    // NEW: Add path to deferred set
    bool AddDeferredPublishPath(TPathId pathId);

    // NEW: Get all deferred paths
    const THashSet<TPathId>& GetDeferredPublishPaths() const;
};
```

#### 3.1.3 Schema Table for Persistence

```cpp
// schemeshard_schema.h

// NEW TABLE: Persist deferred publish paths for crash recovery
struct DeferredPublishPaths : Table<131> {
    struct TxId : Column<1, NScheme::NTypeIds::Uint64> { using Type = TTxId; };
    struct PathOwnerId : Column<2, NScheme::NTypeIds::Uint64> { using Type = TOwnerId; };
    struct PathLocalId : Column<3, NScheme::NTypeIds::Uint64> { using Type = TLocalPathId; };

    using TKey = TableKey<TxId, PathOwnerId, PathLocalId>;
    using TColumns = TableColumns<TxId, PathOwnerId, PathLocalId>;
};
```

### 3.2 Implementation Flow

#### 3.2.1 DeferPublishToSchemeBoard Implementation

```cpp
// schemeshard__operation_side_effects.cpp

void TSideEffects::DeferPublishToSchemeBoard(TOperationId opId, TPathId pathId) {
    // Add to deferred map (keyed by TxId, like PublishPaths)
    DeferredPublishPaths[opId.GetTxId()].push_back(pathId);
}
```

#### 3.2.2 Persistence in ApplyOnExecute

```cpp
// schemeshard__operation_side_effects.cpp

void TSideEffects::ApplyOnExecute(...) {
    // ... existing code ...

    DoPersistPublishPaths(ss, txc, ctx);      // Existing
    DoPersistDeferredPublishPaths(ss, txc, ctx);  // NEW

    // ... rest of existing code ...
}

void TSideEffects::DoPersistDeferredPublishPaths(TSchemeShard* ss,
                                                  NTabletFlatExecutor::TTransactionContext& txc,
                                                  const TActorContext& ctx) {
    NIceDb::TNiceDb db(txc.DB);

    for (const auto& [txId, paths] : DeferredPublishPaths) {
        if (!ss->Operations.contains(txId)) {
            continue;
        }

        TOperation::TPtr operation = ss->Operations.at(txId);

        for (TPathId pathId : paths) {
            if (operation->AddDeferredPublishPath(pathId)) {
                // Persist for crash recovery
                db.Table<Schema::DeferredPublishPaths>()
                    .Key(txId, pathId.OwnerId, pathId.LocalPathId)
                    .Update();
            }
        }
    }
}
```

#### 3.2.3 Actual Publishing in DoDoneTransactions

```cpp
// schemeshard__operation_side_effects.cpp

void TSideEffects::DoDoneTransactions(TSchemeShard* ss,
                                       NTabletFlatExecutor::TTransactionContext& txc,
                                       const TActorContext& ctx) {
    NIceDb::TNiceDb db(txc.DB);

    for (auto& txId : DoneTransactions) {
        if (!ss->Operations.contains(txId)) {
            continue;
        }

        TOperation::TPtr operation = ss->Operations.at(txId);
        if (!operation->IsReadyToDone(ctx)) {
            continue;
        }

        // ... existing path state release code (lines 952-959) ...

        // ... existing dependency handling code (lines 961-984) ...

        // ... existing pipe tracker cleanup (lines 986-990) ...

        // ... existing RemoveTx calls (lines 992-997) ...

        // EXISTING: Version registry cleanup (lines 999-1006)
        if (auto* claimedPaths = ss->VersionRegistry.GetClaimedPaths(txId)) {
            for (const TPathId& pathId : *claimedPaths) {
                ss->PersistRemovePendingVersionChange(db, pathId);
            }
            ss->VersionRegistry.MarkApplied(txId);
            ss->VersionRegistry.RemoveTransaction(txId);
        }

        // NEW: Publish all deferred paths with FINAL versions
        const auto& deferredPaths = operation->GetDeferredPublishPaths();
        if (!deferredPaths.empty()) {
            LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "Publishing deferred paths for completed operation"
                           << ", txId: " << txId
                           << ", pathCount: " << deferredPaths.size());

            TDeque<TPathId> pathsToPublish;
            for (const TPathId& pathId : deferredPaths) {
                if (ss->PathsById.contains(pathId)) {
                    pathsToPublish.push_back(pathId);

                    // Remove from persistence
                    db.Table<Schema::DeferredPublishPaths>()
                        .Key(txId, pathId.OwnerId, pathId.LocalPathId)
                        .Delete();
                }
            }

            // Publish with final versions
            if (!pathsToPublish.empty()) {
                ss->PublishToSchemeBoard(txId, std::move(pathsToPublish), ctx);
            }
        }

        // ... existing publication tracking code (lines 1008-1027) ...

        ss->Operations.erase(txId);
    }
}
```

#### 3.2.4 Loading on Startup

```cpp
// schemeshard__init.cpp

// In TTxInit::ReadEverything()

// NEW: Load deferred publish paths
{
    auto rowset = db.Table<Schema::DeferredPublishPaths>().Range().Select();
    if (!rowset.IsReady()) {
        return false;
    }

    while (!rowset.EndOfSet()) {
        TTxId txId = rowset.GetValue<Schema::DeferredPublishPaths::TxId>();
        TOwnerId ownerId = rowset.GetValue<Schema::DeferredPublishPaths::PathOwnerId>();
        TLocalPathId localId = rowset.GetValue<Schema::DeferredPublishPaths::PathLocalId>();
        TPathId pathId(ownerId, localId);

        if (Self->Operations.contains(txId)) {
            Self->Operations.at(txId)->AddDeferredPublishPath(pathId);
        }

        if (!rowset.Next()) {
            return false;
        }
    }
}
```

### 3.3 Migration of Callers

#### 3.3.1 Copy Table Operation

```cpp
// schemeshard__operation_copy_table.cpp
// In TCopyTable::TPropose::HandleReply()

// BEFORE (around line 190):
context.OnComplete.PublishToSchemeBoard(OperationId, srcPathId);

// AFTER:
// Use deferred publish for source table (has version sync issues)
context.OnComplete.DeferPublishToSchemeBoard(OperationId, srcPathId);

// For child indexes that were synced:
for (const auto& [childName, childPathId] : srcPath.Base()->GetChildren()) {
    if (childPath->IsTableIndex()) {
        // Deferred publish for indexes (linked to impl table version)
        context.OnComplete.DeferPublishToSchemeBoard(OperationId, childPathId);
    }
}
```

#### 3.3.2 CDC Stream Operation

```cpp
// schemeshard__operation_common_cdc_stream.cpp
// In TProposeAtTable::HandleReply()

// BEFORE (various locations):
context.OnComplete.PublishToSchemeBoard(OperationId, tablePathId);
context.OnComplete.PublishToSchemeBoard(OperationId, indexPathId);
context.OnComplete.PublishToSchemeBoard(OperationId, mainTablePathId);

// AFTER:
// Deferred publish for version-sensitive paths
context.OnComplete.DeferPublishToSchemeBoard(OperationId, tablePathId);
context.OnComplete.DeferPublishToSchemeBoard(OperationId, indexPathId);
context.OnComplete.DeferPublishToSchemeBoard(OperationId, mainTablePathId);
```

#### 3.3.3 Alter Table Operation

```cpp
// schemeshard__operation_alter_table.cpp
// In TAlterTable::TPropose::HandleReply()

// For impl table changes that sync parent index:
if (parentIsIndex) {
    // Deferred publish for parent index and grandparent table
    context.OnComplete.DeferPublishToSchemeBoard(OperationId, indexPathId);
    context.OnComplete.DeferPublishToSchemeBoard(OperationId, mainTablePathId);
}
```

#### 3.3.4 Incremental Restore Finalize

```cpp
// schemeshard__operation_incremental_restore_finalize.cpp
// In HandleReply()

// Deferred publish for all version-synced paths
context.OnComplete.DeferPublishToSchemeBoard(OperationId, indexPathId);
context.OnComplete.DeferPublishToSchemeBoard(OperationId, tablePathId);
```

---

## Part 4: Files to Modify

### 4.1 Core Infrastructure

| File | Changes |
|------|---------|
| `schemeshard__operation_side_effects.h` | Add `DeferredPublishPaths` member, `DeferPublishToSchemeBoard()` method |
| `schemeshard__operation_side_effects.cpp` | Implement deferred publish logic, modify `DoDoneTransactions()` |
| `schemeshard__operation.h` | Add `DeferredPublishPaths` to `TOperation` |
| `schemeshard__operation.cpp` | Implement `AddDeferredPublishPath()`, `GetDeferredPublishPaths()` |
| `schemeshard_schema.h` | Add `DeferredPublishPaths` table (ID 131) |
| `schemeshard__init.cpp` | Load deferred paths on startup |

### 4.2 Operation Migrations

| File | Changes |
|------|---------|
| `schemeshard__operation_copy_table.cpp` | Use `DeferPublishToSchemeBoard()` for source paths |
| `schemeshard__operation_common_cdc_stream.cpp` | Use `DeferPublishToSchemeBoard()` for version-synced paths |
| `schemeshard__operation_alter_table.cpp` | Use `DeferPublishToSchemeBoard()` for parent index sync |
| `schemeshard__operation_incremental_restore_finalize.cpp` | Use `DeferPublishToSchemeBoard()` for version-synced paths |

### 4.3 Build Configuration

| File | Changes |
|------|---------|
| `ya.make` | No changes needed (existing files modified) |

---

## Part 5: Detailed Code Changes

### 5.1 schemeshard__operation_side_effects.h

```cpp
// Add after line 47 (after TPublications PublishPaths;)
TPublications DeferredPublishPaths;

// Add after line 91 (after RePublishToSchemeBoard declaration)
void DeferPublishToSchemeBoard(TOperationId opId, TPathId pathId);

// Add after line 146 (in private section)
void DoPersistDeferredPublishPaths(TSchemeShard* ss,
                                    NTabletFlatExecutor::TTransactionContext& txc,
                                    const TActorContext& ctx);
```

### 5.2 schemeshard__operation_side_effects.cpp

```cpp
// Add new method after PublishToSchemeBoard (around line 145)
void TSideEffects::DeferPublishToSchemeBoard(TOperationId opId, TPathId pathId) {
    DeferredPublishPaths[opId.GetTxId()].push_back(pathId);
}

// Add after DoPersistPublishPaths (around line 617)
void TSideEffects::DoPersistDeferredPublishPaths(TSchemeShard* ss,
                                                  NTabletFlatExecutor::TTransactionContext& txc,
                                                  const TActorContext& ctx) {
    NIceDb::TNiceDb db(txc.DB);

    for (const auto& [txId, paths] : DeferredPublishPaths) {
        if (!ss->Operations.contains(txId)) {
            LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        "Cannot defer publish paths for unknown operation id#" << txId);
            continue;
        }

        TOperation::TPtr operation = ss->Operations.at(txId);

        for (TPathId pathId : paths) {
            if (!ss->PathsById.contains(pathId)) {
                continue;
            }

            if (operation->AddDeferredPublishPath(pathId)) {
                LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                            "Deferring publish for path"
                                << ", txId: " << txId
                                << ", pathId: " << pathId);

                db.Table<Schema::DeferredPublishPaths>()
                    .Key(txId, pathId.OwnerId, pathId.LocalPathId)
                    .Update();
            }
        }
    }
}

// Modify ApplyOnExecute (around line 180) - add call to DoPersistDeferredPublishPaths
void TSideEffects::ApplyOnExecute(...) {
    // ... existing code until line 189 ...

    DoPersistPublishPaths(ss, txc, ctx);
    DoPersistDeferredPublishPaths(ss, txc, ctx);  // NEW

    // ... rest of existing code ...
}

// Modify DoDoneTransactions (around line 999) - add deferred publish
// After version registry cleanup block, before publication tracking:

        // NEW: Publish all deferred paths with final versions
        const auto& deferredPaths = operation->GetDeferredPublishPaths();
        if (!deferredPaths.empty()) {
            LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "Publishing " << deferredPaths.size()
                           << " deferred paths for txId: " << txId);

            TDeque<TPathId> pathsToPublish;
            for (const TPathId& pathId : deferredPaths) {
                if (ss->PathsById.contains(pathId)) {
                    pathsToPublish.push_back(pathId);

                    db.Table<Schema::DeferredPublishPaths>()
                        .Key(txId, pathId.OwnerId, pathId.LocalPathId)
                        .Delete();

                    LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                                "Deferred publish path"
                                    << ", txId: " << txId
                                    << ", pathId: " << pathId
                                    << ", version: " << ss->GetPathVersion(
                                        TPath::Init(pathId, ss)).GetGeneralVersion());
                }
            }

            if (!pathsToPublish.empty()) {
                ss->PublishToSchemeBoard(txId, std::move(pathsToPublish), ctx);
            }
        }
```

### 5.3 schemeshard__operation.h

```cpp
// Add to TOperation class (around line 90, after Publications member)
THashSet<TPathId> DeferredPublishPaths;

// Add methods
bool AddDeferredPublishPath(TPathId pathId) {
    return DeferredPublishPaths.insert(pathId).second;
}

const THashSet<TPathId>& GetDeferredPublishPaths() const {
    return DeferredPublishPaths;
}
```

### 5.4 schemeshard_schema.h

```cpp
// Add after PendingVersionChanges table (around line 2650)
struct DeferredPublishPaths : Table<131> {
    struct TxId : Column<1, NScheme::NTypeIds::Uint64> { using Type = TTxId; };
    struct PathOwnerId : Column<2, NScheme::NTypeIds::Uint64> { using Type = TOwnerId; };
    struct PathLocalId : Column<3, NScheme::NTypeIds::Uint64> { using Type = TLocalPathId; };

    using TKey = TableKey<TxId, PathOwnerId, PathLocalId>;
    using TColumns = TableColumns<TxId, PathOwnerId, PathLocalId>;
};

// Add to SchemaTables list
using SchemaTables = SchemaTables<
    // ... existing tables ...
    DeferredPublishPaths
>;
```

### 5.5 schemeshard__init.cpp

```cpp
// Add to ReadEverything() after loading PendingVersionChanges

// Load deferred publish paths
{
    auto rowset = db.Table<Schema::DeferredPublishPaths>().Range().Select();
    if (!rowset.IsReady()) {
        return false;
    }

    ui64 count = 0;
    while (!rowset.EndOfSet()) {
        TTxId txId = rowset.GetValue<Schema::DeferredPublishPaths::TxId>();
        TOwnerId ownerId = rowset.GetValue<Schema::DeferredPublishPaths::PathOwnerId>();
        TLocalPathId localId = rowset.GetValue<Schema::DeferredPublishPaths::PathLocalId>();
        TPathId pathId(ownerId, localId);

        if (Self->Operations.contains(txId)) {
            Self->Operations.at(txId)->AddDeferredPublishPath(pathId);
            ++count;
        }

        if (!rowset.Next()) {
            return false;
        }
    }

    LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                 "Loaded " << count << " deferred publish paths");
}
```

---

## Part 6: Testing Strategy

### 6.1 Unit Tests

```cpp
// New test file: ut_deferred_publish.cpp

Y_UNIT_TEST_SUITE(TDeferredPublishTest) {

    Y_UNIT_TEST(DeferredPathsNotPublishedImmediately) {
        // Setup operation with deferred publish
        // Verify paths NOT in scheme board after ApplyOnComplete
        // Verify paths ARE in scheme board after DoDoneTransactions
    }

    Y_UNIT_TEST(DeferredPathsPublishedWithFinalVersions) {
        // Setup multi-part operation
        // Part A bumps version to 2
        // Part B bumps version to 3
        // Verify published version is 3 (final)
    }

    Y_UNIT_TEST(DeferredPathsSurviveRestart) {
        // Setup operation with deferred paths
        // Simulate crash
        // Restart and verify deferred paths loaded
        // Complete operation and verify publish
    }

    Y_UNIT_TEST(MixedImmediateAndDeferredPublish) {
        // Some paths use PublishToSchemeBoard (immediate)
        // Some paths use DeferPublishToSchemeBoard (deferred)
        // Verify immediate paths published immediately
        // Verify deferred paths published in DoDoneTransactions
    }
}
```

### 6.2 Integration Tests

```cpp
// Extend ut_parts_permutation.cpp

Y_UNIT_TEST(IndexedTableBackupAllPermutations_DeferredPublish) {
    TTestWithPartsPermutations test;
    test.Config.ExpectedPartCount = 3;
    test.Config.MaxPermutations = 6;  // 3! = 6

    test.Run([](TTestActorRuntime& runtime, TTestEnv& env,
                TOperationPartsBlocker& blocker, const TVector<ui32>& permutation) {

        // Create indexed table
        CreateIndexedTable(runtime, env);

        // Start backup (CreateConsistentCopyTables)
        auto txId = StartBackup(runtime, env);

        // Release parts in permutation order
        blocker.ReleaseByPermutationIndices(txId, permutation);

        // Wait for operation completion
        WaitForOperationCompletion(runtime, env, txId);

        // Query table - must NOT get version mismatch
        auto result = ExecuteQuery(runtime, env, "SELECT * FROM Table");
        UNIT_ASSERT_C(result.IsSuccess(),
                      "Query failed with: " << result.GetError());
    });
}
```

### 6.3 Manual Test Scenarios

1. **Basic deferred publish**
   - Create indexed table
   - Run backup
   - Query immediately after backup completes
   - Verify no version mismatch

2. **Crash recovery**
   - Create indexed table
   - Start backup
   - Kill schemeshard mid-operation
   - Restart schemeshard
   - Verify operation completes correctly
   - Query table - no version mismatch

3. **Concurrent queries during backup**
   - Create indexed table
   - Start backup
   - Run queries continuously during backup
   - Verify no version mismatches

---

## Part 7: Rollout Plan

### Phase 1: Infrastructure (Week 1)

1. Add `DeferredPublishPaths` to `TSideEffects`
2. Add `DeferPublishToSchemeBoard()` method
3. Add persistence (schema table + load on startup)
4. Add publish logic in `DoDoneTransactions()`
5. Unit tests for new infrastructure

### Phase 2: Migration - Copy Table (Week 2)

1. Migrate `schemeshard__operation_copy_table.cpp`
2. Run parts permutation tests
3. Verify no regression in existing tests

### Phase 3: Migration - CDC Stream (Week 2)

1. Migrate `schemeshard__operation_common_cdc_stream.cpp`
2. Run CDC-related tests
3. Verify no regression

### Phase 4: Migration - Alter Table & Restore (Week 3)

1. Migrate `schemeshard__operation_alter_table.cpp`
2. Migrate `schemeshard__operation_incremental_restore_finalize.cpp`
3. Run full test suite

### Phase 5: Validation (Week 3)

1. Run all parts permutation tests
2. Run stress tests
3. Monitor for version mismatch errors
4. Performance benchmarking

---

## Part 8: Risks and Mitigations

### Risk 1: Increased Latency

**Risk:** Schema updates not visible until all parts complete.

**Mitigation:**
- Only defer version-sensitive paths (source tables with indexes)
- Destination tables can still use immediate publish
- Monitor publish latency metrics

### Risk 2: Memory Usage

**Risk:** Deferred paths accumulate in memory for long operations.

**Mitigation:**
- Persist deferred paths to DB (already planned)
- Monitor memory usage
- Set limits on deferred path count if needed

### Risk 3: Regression in Existing Functionality

**Risk:** Deferred publish breaks existing operations.

**Mitigation:**
- Extensive test coverage
- Phased rollout
- Easy rollback (can revert to immediate publish)

### Risk 4: Crash Recovery Complexity

**Risk:** Complex state to recover after crash.

**Mitigation:**
- Persist deferred paths immediately
- Load on startup
- Test crash scenarios thoroughly

---

## Part 9: Success Criteria

1. **No version mismatch errors** in any parts permutation test
2. **All existing tests pass** (no regression)
3. **Crash recovery works** - deferred paths published after restart
4. **Performance neutral** - no significant latency increase for typical operations
5. **Memory usage bounded** - no memory leaks from deferred paths

---

## Part 10: Alternatives Considered

### Alternative 1: KQP Retry on Mismatch

**Pros:** Simple, no schemeshard changes
**Cons:** Doesn't fix root cause, adds query latency, masks issues

**Decision:** Can be added as defense-in-depth, but not primary solution

### Alternative 2: Two-Phase Commit for Publishes

**Pros:** Strong consistency guarantees
**Cons:** Complex, significant performance impact, overkill

**Decision:** Too complex for the problem at hand

### Alternative 3: Version Vectors

**Pros:** Mathematically sound convergence
**Cons:** Major architectural change, complex implementation

**Decision:** Too invasive, deferred publish is simpler

---

## Appendix A: Sequence Diagrams

### A.1 Current Flow (Problematic)

```
┌────────┐     ┌────────┐     ┌─────────────┐     ┌─────────────┐
│ Part A │     │ Part B │     │ SchemeBoard │     │ SchemeCache │
└───┬────┘     └───┬────┘     └──────┬──────┘     └──────┬──────┘
    │              │                 │                   │
    │ HandleReply  │                 │                   │
    │──────────────│                 │                   │
    │ Bump version │                 │                   │
    │ Publish ─────│─────────────────>                   │
    │              │                 │ Update cache      │
    │              │                 │──────────────────>│
    │              │                 │                   │
    │              │ HandleReply     │                   │
    │              │─────────────────│                   │
    │              │ Sync version    │                   │
    │              │ Publish ────────>                   │
    │              │                 │                   │
    │              │                 │    KQP Query      │
    │              │                 │<──────────────────│
    │              │                 │    STALE DATA!    │
    │              │                 │──────────────────>│
    │              │                 │                   │
    └──────────────┴─────────────────┴───────────────────┘
```

### A.2 Deferred Flow (Correct)

```
┌────────┐     ┌────────┐     ┌─────────────┐     ┌─────────────┐
│ Part A │     │ Part B │     │ SchemeBoard │     │ SchemeCache │
└───┬────┘     └───┬────┘     └──────┬──────┘     └──────┬──────┘
    │              │                 │                   │
    │ HandleReply  │                 │                   │
    │──────────────│                 │                   │
    │ Bump version │                 │                   │
    │ DEFER publish│                 │                   │
    │              │                 │                   │
    │              │ HandleReply     │                   │
    │              │─────────────────│                   │
    │              │ Sync version    │                   │
    │              │ DEFER publish   │                   │
    │              │                 │                   │
    │   ALL PARTS COMPLETE           │                   │
    │──────────────┬─────────────────│                   │
    │ DoDoneTransactions             │                   │
    │ Publish ALL ─┴─────────────────>                   │
    │ (final versions)               │ Update cache      │
    │                                │──────────────────>│
    │                                │                   │
    │                                │    KQP Query      │
    │                                │<──────────────────│
    │                                │  CONSISTENT DATA  │
    │                                │──────────────────>│
    │                                │                   │
    └────────────────────────────────┴───────────────────┘
```

---

## Appendix B: File Locations Quick Reference

| Component | File Path |
|-----------|-----------|
| Side Effects Header | `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.h` |
| Side Effects Impl | `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp` |
| Operation Header | `ydb/core/tx/schemeshard/schemeshard__operation.h` |
| Schema Definition | `ydb/core/tx/schemeshard/schemeshard_schema.h` |
| Initialization | `ydb/core/tx/schemeshard/schemeshard__init.cpp` |
| Copy Table Op | `ydb/core/tx/schemeshard/schemeshard__operation_copy_table.cpp` |
| CDC Stream Op | `ydb/core/tx/schemeshard/schemeshard__operation_common_cdc_stream.cpp` |
| Alter Table Op | `ydb/core/tx/schemeshard/schemeshard__operation_alter_table.cpp` |
| Restore Finalize | `ydb/core/tx/schemeshard/schemeshard__operation_incremental_restore_finalize.cpp` |
| Parts Permutation Tests | `ydb/core/tx/schemeshard/ut_parts_permutation/ut_parts_permutation.cpp` |
