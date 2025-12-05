# Comprehensive Plan: PendingVersionChanges Registry

## Executive Summary

This document outlines a plan to replace the existing fragmented version synchronization mechanisms (`SyncIndexEntityVersion`, `SyncChildIndexes`, `HelpSyncSiblingVersions`, `SyncIndexSchemaVersions`) with a unified **PendingVersionChanges** registry that provides:

1. **Idempotent version updates** - safe to call multiple times
2. **Operation-scoped tracking** - versions tied to specific operations
3. **Conflict detection** - detect when sibling operations modify the same resource
4. **Automatic cleanup** - versions cleaned up on operation completion/abort

---

## Problem Analysis

### Current State: Fragmented Version Synchronization

The current codebase has multiple overlapping mechanisms added incrementally:

| Mechanism | Location | Problem |
|-----------|----------|---------|
| `SyncIndexEntityVersion` | `schemeshard__operation_common_cdc_stream.cpp:265` | Idempotent but requires knowing target version |
| `SyncChildIndexes` | `schemeshard__operation_common_cdc_stream.cpp:330` | Iterates all children, can cause sibling conflicts |
| `HelpSyncSiblingVersions` | `schemeshard__operation_common_cdc_stream.cpp:117` | Complex lock-free helping, hard to reason about |
| `SyncIndexSchemaVersions` | `schemeshard__operation_incremental_restore_finalize.cpp:274` | Uses `+= 1` which is NOT idempotent |

### Root Cause of Failures

The test failure `schema version mismatch for idx2 expected 3 got 4` occurs because:

1. **Non-idempotent increments**: Code uses `AlterVersion += 1` without guards
2. **No coordination between siblings**: Multiple parts can increment the same resource
3. **Race conditions**: Parts see side effects of siblings in unpredictable order
4. **No single source of truth**: Version state scattered across multiple mechanisms

### Git History of Problematic Changes

| Date | Commit | Change | Issue |
|------|--------|--------|-------|
| Nov 8, 2025 | `f0a04ef9032` | Added SyncIndexEntityVersion, SyncChildIndexes | Initial attempt, works for single-table |
| Nov 20, 2025 | `8245aee0ddc` | Added HelpSyncSiblingVersions | Lock-free helping, complex |
| Dec 3, 2025 | `82a9f2245c3` | Added SyncIndexSchemaVersions | Non-idempotent `+= 1` |
| Dec 5, 2025 | `7648d680b70` | Added THashSet deduplication | Band-aid fix, doesn't solve root cause |

---

## Solution Design: PendingVersionChanges Registry

### Core Concept

Instead of each operation independently incrementing versions, we introduce a **centralized registry** that:

1. **Claims** version changes at operation start
2. **Detects conflicts** when multiple operations claim the same resource
3. **Applies** version changes atomically on operation completion
4. **Rolls back** on operation abort

### Sibling Convergence Model

**The Core Problem:** Multiple sibling parts (same TxId, different SubTxIds) may both need to increment the same resource's version. For example:
- Sibling A: processing Table1's CDC, wants `idx.AlterVersion: 3 → 4`
- Sibling B: processing Table2's CDC, also wants `idx.AlterVersion: 3 → 4`

**Requirements:**
1. Both must converge to the same version (4)
2. Increment happens exactly once in SchemeShard state
3. Both siblings send the SAME version to their shards
4. Works regardless of execution order

**Solution:** Claims are at **TxId level, not SubTxId level**. Siblings share claims via "join" semantics.

```
Timeline A (Sibling A runs first):
  t0: Sibling A claims idx: 3→4  → Claimed
  t1: Sibling A updates in-memory: idx.AlterVersion = 4
  t2: Sibling A sends version 4 to shards
  t3: Sibling B claims idx: 3→4  → Joined (sees existing claim)
  t4: Sibling B queries effective version → 4
  t5: Sibling B sends version 4 to shards (same as A!)
  t6: TxId completes, ApplyChanges() persists version 4 (once)

Timeline B (Sibling B runs first):
  t0: Sibling B claims idx: 3→4  → Claimed
  t1: Sibling B updates in-memory: idx.AlterVersion = 4
  t2: Sibling B sends version 4 to shards
  t3: Sibling A claims idx: 3→4  → Joined
  t4: Sibling A queries effective version → 4
  t5: Sibling A sends version 4 to shards (same as B!)
  t6: TxId completes, ApplyChanges() persists version 4 (once)

Both timelines produce identical results!
```

### Data Structures

#### 1. TPendingVersionChange Structure

```cpp
// In schemeshard_info_types.h or new file schemeshard_version_registry.h

struct TPendingVersionChange {
    TPathId PathId;                          // Resource being modified
    ui64 OriginalVersion;                    // Version when first claim made
    ui64 ClaimedVersion;                     // Target version (may increase via Max)
    TTxId ClaimingTxId;                      // Shared by all sibling parts
    THashSet<TSubTxId> Contributors;         // Which parts contributed to this claim
    TTxState::ETxType OperationType;         // Type of operation
    TInstant ClaimTime;                      // When the first claim was made
    bool Applied = false;                    // Whether change was applied

    // For debugging/auditing
    TString DebugInfo;                       // Human-readable description
};

enum class EClaimResult {
    Claimed,      // First to claim, registered new claim
    Joined,       // Sibling already claimed, joined existing claim
    Conflict,     // Different TxId already claimed - real conflict
};
```

#### 2. TVersionRegistry Class

```cpp
// New file: schemeshard_version_registry.h

class TVersionRegistry {
public:
    // Claim a version change for an operation
    // Returns: Claimed (first), Joined (sibling), or Conflict (different TxId)
    EClaimResult ClaimVersionChange(
        TOperationId opId,        // TxId + SubTxId
        TPathId pathId,
        ui64 currentVersion,
        ui64 targetVersion,
        TTxState::ETxType opType,
        TString debugInfo = "");

    // Check if a path has a pending version change
    bool HasPendingChange(TPathId pathId) const;

    // Get the effective version for a path (claimed version if pending, else current)
    // CRITICAL: All siblings call this to get the SAME version for shard notifications
    ui64 GetEffectiveVersion(TPathId pathId, ui64 currentVersion) const;

    // Get the TxId that claimed a path
    TMaybe<TTxId> GetClaimingTxId(TPathId pathId) const;

    // Check if this operation (or its sibling) claimed a path
    bool IsClaimedByTxId(TPathId pathId, TTxId txId) const;

    // Apply all pending changes for a TxId (call when ALL parts complete)
    void ApplyChanges(TTxId txId, NIceDb::TNiceDb& db);

    // Rollback all pending changes for a TxId (call on abort)
    void RollbackChanges(TTxId txId);

    // Cleanup completed transaction
    void RemoveTransaction(TTxId txId);

    // Persistence
    void PersistClaim(NIceDb::TNiceDb& db, const TPendingVersionChange& change);
    void PersistRemoveClaim(NIceDb::TNiceDb& db, TTxId txId, TPathId pathId);
    void LoadFromDb(NIceDb::TNiceDb& db);

private:
    // Primary index: PathId -> pending change
    THashMap<TPathId, TPendingVersionChange> PendingByPath_;

    // Secondary index: TxId -> claimed paths (shared by all siblings)
    THashMap<TTxId, THashSet<TPathId>> ClaimsByTxId_;
};
```

#### 3. ClaimVersionChange Implementation (Sibling Join Semantics)

```cpp
EClaimResult TVersionRegistry::ClaimVersionChange(
    TOperationId opId,
    TPathId pathId,
    ui64 currentVersion,
    ui64 targetVersion,
    TTxState::ETxType opType,
    TString debugInfo)
{
    TTxId txId = opId.GetTxId();
    TSubTxId subTxId = opId.GetSubTxId();

    if (auto* existing = PendingByPath_.FindPtr(pathId)) {
        if (existing->ClaimingTxId == txId) {
            // SIBLING CLAIM - join existing
            // Take max version (in case siblings computed different targets)
            existing->ClaimedVersion = Max(existing->ClaimedVersion, targetVersion);
            existing->Contributors.insert(subTxId);

            LOG_DEBUG("Sibling " << opId << " joined claim for " << pathId
                      << ", version now " << existing->ClaimedVersion);
            return EClaimResult::Joined;
        }

        // Different TxId - actual conflict between different operations
        LOG_WARN("Conflict: " << pathId << " claimed by TxId "
                 << existing->ClaimingTxId << ", rejected " << txId);
        return EClaimResult::Conflict;
    }

    // First claim - register new
    PendingByPath_[pathId] = {
        .PathId = pathId,
        .OriginalVersion = currentVersion,
        .ClaimedVersion = targetVersion,
        .ClaimingTxId = txId,
        .Contributors = {subTxId},
        .OperationType = opType,
        .ClaimTime = TInstant::Now(),
        .DebugInfo = debugInfo,
    };
    ClaimsByTxId_[txId].insert(pathId);

    LOG_DEBUG("Claimed " << pathId << " version " << currentVersion
              << " -> " << targetVersion << " by " << opId);
    return EClaimResult::Claimed;
}
```

#### 4. GetEffectiveVersion Implementation

```cpp
ui64 TVersionRegistry::GetEffectiveVersion(TPathId pathId, ui64 currentVersion) const {
    if (auto* pending = PendingByPath_.FindPtr(pathId)) {
        // Return the claimed version - all siblings see the same value
        return pending->ClaimedVersion;
    }
    return currentVersion;
}
```

#### 5. ApplyChanges Implementation

```cpp
void TVersionRegistry::ApplyChanges(TTxId txId, NIceDb::TNiceDb& db) {
    auto* claims = ClaimsByTxId_.FindPtr(txId);
    if (!claims) return;

    for (const TPathId& pathId : *claims) {
        auto& change = PendingByPath_[pathId];

        if (change.Applied) {
            continue;  // Already applied (idempotent)
        }

        // Persist to database
        // Note: In-memory state was already updated by the first claiming sibling
        PersistVersionChange(db, pathId, change.ClaimedVersion);
        change.Applied = true;

        LOG_INFO("Applied version change for " << pathId
                 << ": " << change.OriginalVersion << " -> " << change.ClaimedVersion
                 << ", contributors: " << change.Contributors.size());
    }
}
```

#### 6. Schema for Persistence

```cpp
// In schemeshard_schema.h, add new table

struct PendingVersionChanges : Table<127> {
    struct TxId : Column<1, NScheme::NTypeIds::Uint64> {};
    struct SubTxId : Column<2, NScheme::NTypeIds::Uint32> {};
    struct PathLocalId : Column<3, NScheme::NTypeIds::Uint64> {};
    struct PathOwnerId : Column<4, NScheme::NTypeIds::Uint64> {};
    struct OriginalVersion : Column<5, NScheme::NTypeIds::Uint64> {};
    struct ClaimedVersion : Column<6, NScheme::NTypeIds::Uint64> {};
    struct OperationType : Column<7, NScheme::NTypeIds::Uint32> {};
    struct ClaimTime : Column<8, NScheme::NTypeIds::Uint64> {};
    struct DebugInfo : Column<9, NScheme::NTypeIds::Utf8> {};

    using TKey = TableKey<TxId, SubTxId, PathLocalId, PathOwnerId>;
    using TColumns = TableColumns<
        TxId, SubTxId, PathLocalId, PathOwnerId,
        OriginalVersion, ClaimedVersion, OperationType, ClaimTime, DebugInfo
    >;
};
```

#### 7. Integration with TSchemeShard

```cpp
// In schemeshard_impl.h, add to TSchemeShard class (after line 297)

// Version change tracking registry
TVersionRegistry VersionRegistry;
```

---

## Migration Plan

### Phase 1: Add Infrastructure (Non-Breaking)

**Files to create:**
- `ydb/core/tx/schemeshard/schemeshard_version_registry.h`
- `ydb/core/tx/schemeshard/schemeshard_version_registry.cpp`

**Files to modify:**
- `ydb/core/tx/schemeshard/schemeshard_schema.h` - Add PendingVersionChanges table
- `ydb/core/tx/schemeshard/schemeshard_impl.h` - Add TVersionRegistry member
- `ydb/core/tx/schemeshard/schemeshard__init.cpp` - Load registry on startup
- `ydb/core/tx/schemeshard/ya.make` - Add new files

**Changes:**
1. Create `TVersionRegistry` class with full implementation
2. Add schema table for persistence
3. Integrate into `TSchemeShard` initialization
4. Add tests for registry in isolation

### Phase 2: Create Wrapper Functions

Create new idempotent version update functions that use the registry:

```cpp
// In schemeshard_version_registry.h

// Replace SyncIndexEntityVersion with:
bool ClaimIndexVersionChange(
    TVersionRegistry& registry,
    TOperationId opId,
    TPathId indexPathId,
    ui64 targetVersion,
    TOperationContext& context,
    NIceDb::TNiceDb& db);

// Replace SyncChildIndexes with:
bool ClaimChildIndexVersionChanges(
    TVersionRegistry& registry,
    TOperationId opId,
    TPathElement::TPtr parentPath,
    ui64 targetVersion,
    TOperationContext& context,
    NIceDb::TNiceDb& db);

// Replace HelpSyncSiblingVersions with:
bool ClaimSiblingVersionChanges(
    TVersionRegistry& registry,
    TOperationId opId,
    TPathId myImplTablePathId,
    TPathId parentTablePathId,
    TOperationContext& context,
    NIceDb::TNiceDb& db);

// Replace SyncIndexSchemaVersions with:
bool ClaimRestoreVersionChanges(
    TVersionRegistry& registry,
    TOperationId opId,
    const NKikimrSchemeOp::TIncrementalRestoreFinalize& finalize,
    TOperationContext& context,
    NIceDb::TNiceDb& db);
```

### Phase 3: Migrate Callers

#### 3.1 Migrate CDC Stream Operations

**File:** `schemeshard__operation_common_cdc_stream.cpp`

**Before (lines 483-499):**
```cpp
if (isIndexImplTableCdc) {
    table->AlterVersion += 1;
    HelpSyncSiblingVersions(pathId, versionCtx.ParentPathId,
                            versionCtx.GrandParentPathId,
                            table->AlterVersion, OperationId, context, db);
}
```

**After:**
```cpp
if (isIndexImplTableCdc) {
    ui64 newVersion = table->AlterVersion + 1;
    auto result = context.SS->VersionRegistry.ClaimVersionChange(
        OperationId, pathId, table->AlterVersion, newVersion,
        TTxState::TxCreateCdcStream, "CDC stream on index impl table");

    switch (result) {
        case EClaimResult::Claimed:
            // First sibling - update in-memory state
            table->AlterVersion = newVersion;
            break;

        case EClaimResult::Joined:
            // Sibling already claimed - nothing to do for in-memory
            // (first sibling already updated or will update)
            break;

        case EClaimResult::Conflict:
            // Different operation - this shouldn't happen for CDC on same table
            LOG_ERROR("Unexpected conflict for CDC operation");
            break;
    }

    // ALL siblings use the same effective version for shard notifications
    ui64 effectiveVersion = context.SS->VersionRegistry.GetEffectiveVersion(
        pathId, table->AlterVersion);
    notice.SetTableSchemaVersion(effectiveVersion);
}
```

#### 3.2 Migrate Copy Table Operations

**File:** `schemeshard__operation_copy_table.cpp`

**Before (line 225-230):**
```cpp
srcTable->AlterVersion += 1;
NCdcStreamState::SyncChildIndexes(srcPath, srcTable->AlterVersion,
                                   OperationId, context, db);
```

**After:**
```cpp
ui64 newVersion = srcTable->AlterVersion + 1;
auto result = context.SS->VersionRegistry.ClaimVersionChange(
    OperationId, srcPathId, srcTable->AlterVersion, newVersion,
    TTxState::TxCopyTable, "Copy table source version bump");

if (result == EClaimResult::Claimed) {
    srcTable->AlterVersion = newVersion;
}

// Claim child index versions (only if we're the first to claim parent)
if (result != EClaimResult::Conflict) {
    for (const auto& [childName, childPathId] : srcPath.Base()->GetChildren()) {
        auto childPath = context.SS->PathsById.at(childPathId);
        if (!childPath->IsTableIndex() || childPath->Dropped()) {
            continue;
        }

        // Claim index version change
        auto indexResult = context.SS->VersionRegistry.ClaimVersionChange(
            OperationId, childPathId,
            context.SS->Indexes[childPathId]->AlterVersion, newVersion,
            TTxState::TxCopyTable, "Copy table child index sync");

        if (indexResult == EClaimResult::Claimed) {
            context.SS->Indexes[childPathId]->AlterVersion = newVersion;
        }
    }
}
```

#### 3.3 Migrate Incremental Restore Finalization

**File:** `schemeshard__operation_incremental_restore_finalize.cpp`

**Before (lines 349-358):**
```cpp
if (context.SS->Indexes.contains(indexPathId)) {
    auto oldVersion = context.SS->Indexes[indexPathId]->AlterVersion;
    context.SS->Indexes[indexPathId]->AlterVersion += 1;
    context.SS->PersistTableIndexAlterVersion(db, indexPathId,
                                               context.SS->Indexes[indexPathId]);
}
```

**After:**
```cpp
if (context.SS->Indexes.contains(indexPathId)) {
    auto index = context.SS->Indexes[indexPathId];
    ui64 newVersion = index->AlterVersion + 1;

    auto result = context.SS->VersionRegistry.ClaimVersionChange(
        OperationId, indexPathId, index->AlterVersion, newVersion,
        TTxState::TxIncrementalRestoreFinalize, "Restore finalization");

    switch (result) {
        case EClaimResult::Claimed:
            index->AlterVersion = newVersion;
            context.SS->PersistTableIndexAlterVersion(db, indexPathId, index);
            context.OnComplete.PublishToSchemeBoard(OperationId, indexPathId);
            break;

        case EClaimResult::Joined:
            // Sibling finalization part already claimed - skip
            // Version will be persisted when TxId completes
            break;

        case EClaimResult::Conflict:
            // Different restore operation? Should not happen.
            LOG_ERROR("Conflict during restore finalization for " << indexPathId);
            break;
    }
}
```

### Phase 4: Remove Old Functions

After all callers are migrated, remove:

1. **Delete `HelpSyncSiblingVersions`** from `schemeshard__operation_common_cdc_stream.cpp`
2. **Simplify `SyncIndexEntityVersion`** - keep as thin wrapper over registry or delete
3. **Simplify `SyncChildIndexes`** - keep as thin wrapper over registry or delete
4. **Refactor `SyncIndexSchemaVersions`** - use registry claims

### Phase 5: Add Operation Lifecycle Hooks

Integrate registry with operation completion/abort. Note that we use **TTxId** (not TOperationId) because claims are shared across all sibling parts.

**In `schemeshard__operation_side_effects.cpp`:**

```cpp
// In DoDoneTransactions() - after line 938
// This is called when ALL parts of a TxId have completed
void TSideEffects::DoDoneTransactions(TSchemeShard* ss, ...) {
    for (const TTxId& txId : DoneTransactions) {
        // Apply all claimed version changes for this TxId
        // This persists versions that were updated in-memory by individual parts
        ss->VersionRegistry.ApplyChanges(txId, db);

        // ... existing completion logic ...

        // Cleanup registry for this TxId
        ss->VersionRegistry.RemoveTransaction(txId);
    }
}

// In AbortOperationPropose() - around line 302
void TSchemeShard::AbortOperationPropose(const TTxId txId, TOperationContext& context) {
    // Rollback all claimed version changes for this TxId
    // This affects ALL sibling parts at once
    VersionRegistry.RollbackChanges(txId);

    // ... existing abort logic ...
}
```

**Key Insight:** Because claims are at TxId level:
- `ApplyChanges(txId)` is called ONCE when all parts complete
- `RollbackChanges(txId)` reverts ALL sibling claims at once
- No need to track individual part completion for version changes

---

## Conflict Resolution Strategy

The registry distinguishes between **sibling claims** (same TxId) and **conflicting claims** (different TxId):

### Sibling Claims (Same TxId) - JOIN

Siblings are parts of the same logical operation. They share claims:

```cpp
if (existing->ClaimingTxId == txId) {
    // SIBLING - join existing claim
    existing->ClaimedVersion = Max(existing->ClaimedVersion, targetVersion);
    existing->Contributors.insert(subTxId);
    return EClaimResult::Joined;
}
```

**Behavior:**
- First sibling to claim registers the change
- Subsequent siblings JOIN the existing claim
- Version converges to MAX of all sibling requests
- All siblings query `GetEffectiveVersion()` → get same value
- Applied ONCE when TxId completes (all parts done)

### Conflicting Claims (Different TxId) - REJECT

Different operations cannot claim the same path simultaneously:

```cpp
if (existing->ClaimingTxId != txId) {
    // CONFLICT - different operation
    LOG_WARN("Conflict: " << pathId << " claimed by TxId "
             << existing->ClaimingTxId << ", rejected " << txId);
    return EClaimResult::Conflict;
}
```

**Behavior:**
- Conflicting operation receives `EClaimResult::Conflict`
- Caller can:
  - Wait for the claiming operation to complete, then retry
  - Proceed without modifying this resource
  - Abort with error

### Handling EClaimResult in Callers

```cpp
auto result = registry.ClaimVersionChange(opId, pathId, oldVer, newVer, ...);

switch (result) {
    case EClaimResult::Claimed:
        // We're the first - update in-memory state now
        resource->AlterVersion = newVer;
        break;

    case EClaimResult::Joined:
        // Sibling already claimed - get the effective version
        // Don't update in-memory (sibling already did or will)
        break;

    case EClaimResult::Conflict:
        // Different operation owns this - handle gracefully
        // Option 1: Skip this resource
        // Option 2: Wait and retry
        // Option 3: Abort
        break;
}

// ALL callers use GetEffectiveVersion for shard notifications
ui64 versionForShards = registry.GetEffectiveVersion(pathId, resource->AlterVersion);
```

### Why This Model Works

| Guarantee | Mechanism |
|-----------|-----------|
| **Siblings converge** | `Max(existing, new)` ensures highest version wins |
| **Exactly-once update** | First sibling updates in-memory; `Applied` flag prevents re-persist |
| **Same version to shards** | All siblings call `GetEffectiveVersion()` → same result |
| **Order-independent** | Whether A or B runs first, final version is the same |
| **Conflict detection** | Different TxId → immediate rejection with logging |

---

## Testing Strategy

### Unit Tests for TVersionRegistry

```cpp
Y_UNIT_TEST_SUITE(TVersionRegistryTest) {
    Y_UNIT_TEST(ClaimAndApply) {
        TVersionRegistry registry;
        TOperationId op1(1, 0);
        TPathId path1(100, 1);

        auto result = registry.ClaimVersionChange(op1, path1, 1, 2, ...);
        UNIT_ASSERT_EQUAL(result, EClaimResult::Claimed);
        UNIT_ASSERT_EQUAL(registry.GetEffectiveVersion(path1, 1), 2);

        registry.ApplyChanges(TTxId(1), db);
        UNIT_ASSERT(!registry.HasPendingChange(path1));
    }

    Y_UNIT_TEST(SiblingJoinClaim) {
        TVersionRegistry registry;
        // Same TxId (1), different SubTxIds (0 and 1) = siblings
        TOperationId siblingA(1, 0);
        TOperationId siblingB(1, 1);
        TPathId path1(100, 1);

        // Sibling A claims first
        auto resultA = registry.ClaimVersionChange(siblingA, path1, 3, 4, ...);
        UNIT_ASSERT_EQUAL(resultA, EClaimResult::Claimed);

        // Sibling B joins existing claim
        auto resultB = registry.ClaimVersionChange(siblingB, path1, 3, 4, ...);
        UNIT_ASSERT_EQUAL(resultB, EClaimResult::Joined);

        // Both see the same effective version
        UNIT_ASSERT_EQUAL(registry.GetEffectiveVersion(path1, 3), 4);

        // Contributors includes both
        auto* pending = registry.GetPendingChange(path1);
        UNIT_ASSERT(pending->Contributors.contains(0));
        UNIT_ASSERT(pending->Contributors.contains(1));
    }

    Y_UNIT_TEST(SiblingVersionConvergence) {
        TVersionRegistry registry;
        TOperationId siblingA(1, 0);
        TOperationId siblingB(1, 1);
        TPathId path1(100, 1);

        // Sibling A sees version 3, wants 4
        auto resultA = registry.ClaimVersionChange(siblingA, path1, 3, 4, ...);
        UNIT_ASSERT_EQUAL(resultA, EClaimResult::Claimed);

        // Sibling B sees version 4 (A already updated in-memory), wants 5
        auto resultB = registry.ClaimVersionChange(siblingB, path1, 4, 5, ...);
        UNIT_ASSERT_EQUAL(resultB, EClaimResult::Joined);

        // Version converges to MAX (5)
        UNIT_ASSERT_EQUAL(registry.GetEffectiveVersion(path1, 3), 5);
    }

    Y_UNIT_TEST(ConflictBetweenDifferentOperations) {
        TVersionRegistry registry;
        // Different TxIds = different operations
        TOperationId op1(1, 0);
        TOperationId op2(2, 0);
        TPathId path1(100, 1);

        auto result1 = registry.ClaimVersionChange(op1, path1, 1, 2, ...);
        UNIT_ASSERT_EQUAL(result1, EClaimResult::Claimed);

        auto result2 = registry.ClaimVersionChange(op2, path1, 1, 3, ...);
        UNIT_ASSERT_EQUAL(result2, EClaimResult::Conflict);
    }

    Y_UNIT_TEST(RollbackOnAbort) {
        TVersionRegistry registry;
        TOperationId op1(1, 0);
        TPathId path1(100, 1);

        registry.ClaimVersionChange(op1, path1, 1, 2, ...);
        registry.RollbackChanges(TTxId(1));
        UNIT_ASSERT(!registry.HasPendingChange(path1));
    }

    Y_UNIT_TEST(IdempotentClaimSamePart) {
        TVersionRegistry registry;
        TOperationId op1(1, 0);
        TPathId path1(100, 1);

        auto result1 = registry.ClaimVersionChange(op1, path1, 1, 2, ...);
        UNIT_ASSERT_EQUAL(result1, EClaimResult::Claimed);

        // Same part claims again (retry scenario)
        auto result2 = registry.ClaimVersionChange(op1, path1, 1, 2, ...);
        UNIT_ASSERT_EQUAL(result2, EClaimResult::Joined);  // Treated as join

        UNIT_ASSERT_EQUAL(registry.GetEffectiveVersion(path1, 1), 2);
    }

    Y_UNIT_TEST(ApplyOnlyOnce) {
        TVersionRegistry registry;
        TOperationId siblingA(1, 0);
        TOperationId siblingB(1, 1);
        TPathId path1(100, 1);

        registry.ClaimVersionChange(siblingA, path1, 3, 4, ...);
        registry.ClaimVersionChange(siblingB, path1, 3, 4, ...);

        // First apply
        registry.ApplyChanges(TTxId(1), db);
        auto* pending = registry.GetPendingChange(path1);
        UNIT_ASSERT(pending->Applied);

        // Second apply is no-op
        registry.ApplyChanges(TTxId(1), db);
        // No crash, no double-persist
    }
}
```

### Integration Tests

Use the existing parts permutation framework to verify:

1. **All permutations pass** - No version mismatches regardless of part order
2. **Conflict logging** - Conflicts are logged but don't cause failures
3. **Crash recovery** - Registry state survives SchemeShard restart

---

## Rollout Plan

### Step 1: Feature Flag (Week 1)

Add feature flag to enable/disable new registry:

```cpp
// In schemeshard_impl.h
bool UseVersionRegistry = false;  // Default off

// In version claiming code:
if (SS->UseVersionRegistry) {
    // New path with registry
} else {
    // Old path with direct increments
}
```

### Step 2: Shadow Mode (Week 2)

Run both paths, compare results:

```cpp
if (SS->UseVersionRegistry) {
    bool registryResult = ClaimVersionChange(...);
    bool oldResult = OldSyncFunction(...);

    if (registryResult != oldResult) {
        LOG_ERROR("Registry mismatch!");
    }
}
```

### Step 3: Gradual Rollout (Week 3-4)

1. Enable for new incremental restore operations only
2. Enable for CDC operations
3. Enable for all operations

### Step 4: Cleanup (Week 5)

1. Remove feature flag
2. Remove old sync functions
3. Update documentation

---

## Files Changed Summary

| File | Change Type | Description |
|------|-------------|-------------|
| `schemeshard_version_registry.h` | **NEW** | TVersionRegistry class definition |
| `schemeshard_version_registry.cpp` | **NEW** | TVersionRegistry implementation |
| `schemeshard_schema.h` | MODIFY | Add PendingVersionChanges table |
| `schemeshard_impl.h` | MODIFY | Add TVersionRegistry member |
| `schemeshard__init.cpp` | MODIFY | Load registry on startup |
| `schemeshard__operation_side_effects.cpp` | MODIFY | Hook Apply/Rollback |
| `schemeshard__operation_common_cdc_stream.cpp` | MODIFY | Use registry instead of HelpSync |
| `schemeshard__operation_copy_table.cpp` | MODIFY | Use registry |
| `schemeshard__operation_incremental_restore_finalize.cpp` | MODIFY | Use registry |
| `schemeshard_cdc_stream_common.h` | MODIFY | Remove old function declarations |
| `ya.make` | MODIFY | Add new source files |

---

## Success Criteria

1. **All parts permutation tests pass** - Including the failing [3, 0, 1, 2] case
2. **No "schema version mismatch" errors** - In any operation order
3. **Idempotent operations** - Same operation can be called multiple times safely
4. **Conflict detection** - Conflicting operations are detected and logged
5. **Clean crash recovery** - Registry state persisted and restored correctly
6. **Performance neutral** - No measurable performance regression

---

## Appendix: Code to Remove

### A. Functions to Delete

```cpp
// schemeshard__operation_common_cdc_stream.cpp

// DELETE: Lines 117-260
void HelpSyncSiblingVersions(...)

// SIMPLIFY OR DELETE: Lines 265-328
void SyncIndexEntityVersion(...)

// SIMPLIFY OR DELETE: Lines 330-380
void SyncChildIndexes(...)
```

### B. Headers to Clean

```cpp
// schemeshard_cdc_stream_common.h

// REMOVE declarations for:
void SyncIndexEntityVersion(...);
void SyncChildIndexes(...);
// HelpSyncSiblingVersions is already internal (anonymous namespace)
```

### C. Test Updates

Update tests that directly call old functions to use registry API:
- `ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`
- `ydb/core/tx/schemeshard/ut_restore/ut_restore.cpp`
