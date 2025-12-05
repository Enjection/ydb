# Schema Version Sync - Comprehensive Research & Ideas

## Executive Summary

This document provides a comprehensive analysis of the schema version synchronization issue, current implementation status, identified root causes, and potential solutions to try.

**The Core Problem:** Schema version mismatches occur when multi-part operations (siblings) execute in different orders, causing `TIndexDescription::SchemaVersion` (from parent table metadata) to diverge from the actual impl table's `AlterVersion`.

**Error Manifestation:**
```
schema version mismatch during metadata loading for: /Root/TableWithIndex/idx/indexImplTable
expected 1 got 2
```

---

## Part 1: Current Architecture Understanding

### 1.1 Schema Versioning System

**Version Registry (`TVersionRegistry`)** - Central mechanism for coordinating version changes:

```
Location: ydb/core/tx/schemeshard/schemeshard_version_registry.{h,cpp}

Key Components:
- TPendingVersionChange: Tracks pending version changes per PathId
- ClaimVersionChange(): Claims a version change (returns Claimed/Joined/Conflict)
- GetEffectiveVersion(): Returns the agreed-upon version for all siblings
- MarkApplied()/RollbackChanges(): Lifecycle management
```

**The KQP Invariant** (from `kqp_metadata_loader.cpp:963`):
```cpp
// This MUST hold:
TIndexDescription.SchemaVersion == implTable.AlterVersion

// Where:
// - TIndexDescription.SchemaVersion comes from parent table's index metadata
// - implTable.AlterVersion is the actual impl table version
```

### 1.2 Schemeboard Publishing Flow

```
Schema Modification Flow:
1. Operation HandleReply phase → Version incremented
2. PublishToSchemeBoard() called → Path registered for publishing
3. TTxPublishToSchemeBoard → Descriptions sent to Populator
4. Populator → Updates sent to Replicas
5. Replicas acknowledge → Operation notified
6. Operation completion → Notifications sent

Critical Timing:
- Publishing happens DURING HandleReply phase (potentially BEFORE sibling convergence)
- Multiple siblings may publish at different times with different versions
```

### 1.3 Multi-Part Operation Structure

**CreateConsistentCopyTables** (typical indexed table backup):
```
TxId: 123
├── SubTxId 0: CreateCopyTable (main table + embedded CDC)
├── SubTxId 1: CreateNewTableIndex
└── SubTxId 2: CreateCopyTable (impl table + embedded CDC)
    └── This bumps source impl table version
    └── Triggers parent index sync
    └── May trigger grandparent table bump
```

**Sibling Execution Order Problem:**
- Parts can execute in ANY order due to operation parts permutation
- Each part may see different in-memory state
- Version claims may arrive in different orders

---

## Part 2: Identified Root Causes

### 2.1 ROOT CAUSE #1: Premature Publishing

**Problem:** Source tables are published to scheme board DURING the Propose phase, before all sibling version increments converge.

**Timing Diagram:**
```
Timeline (Problematic):
  t0: Part A starts HandleReply
  t1: Part A publishes table with version=1 to scheme board  ← PREMATURE!
  t2: Part B starts HandleReply
  t3: Part B increments impl table version to 2
  t4: Part B syncs parent index to version 2
  t5: Part B publishes index with version=2 to scheme board
  t6: KQP query arrives
  t7: KQP loads parent table from cache (has TIndexDescription.SchemaVersion=1)  ← STALE!
  t8: KQP loads impl table (has version=2)
  t9: MISMATCH: expected 1 got 2
```

**Locations with early publish:**
- `schemeshard__operation_create_cdc_stream.cpp` - TNewCdcStream
- `schemeshard__operation_common_cdc_stream.cpp` - CDC stream creation
- `schemeshard__operation_copy_table.cpp` - During copy

### 2.2 ROOT CAUSE #2: Version Registry "First Claim Wins" Limitations

**Problem:** The registry's "first claim wins" design may not handle all sibling scenarios correctly when siblings see different in-memory states.

**Scenario:**
```
Sibling A: claims idx: 1→2, gets "Claimed", updates idx.AlterVersion=2
Sibling B: sees idx.AlterVersion=2 (A already updated), claims idx: 2→3
Registry: B "Joins" A's claim but ClaimedVersion doesn't update to 3

Result: Registry has ClaimedVersion=2, but B thinks it should be 3
```

**Current Fix Attempt:** Added `Max()` logic when joining:
```cpp
if (targetVersion > existing->ClaimedVersion) {
    existing->ClaimedVersion = targetVersion;
}
```

**Issue:** This creates version drift - the version keeps increasing when it shouldn't.

### 2.3 ROOT CAUSE #3: Missing Parent Table Re-publish

**Problem:** When child index version changes, parent table's scheme cache entry is NOT invalidated.

**Flow:**
```
1. CopyTable syncs child index: index.AlterVersion = 2
2. Child index published to scheme board ✓
3. Parent table NOT re-published ✗
4. KQP loads parent from cache: TIndexDescription.SchemaVersion = 1 (stale!)
5. KQP loads impl table: SchemaVersion = 2
6. MISMATCH!
```

**Current Fix Attempt:** Bump parent's `DirAlterVersion` and republish
**Status:** Incomplete - scheme cache may still serve stale data

### 2.4 ROOT CAUSE #4: Scheme Cache Staleness Window

**Problem:** Even with correct publishing, there's a window where scheme cache has stale data.

**Race Condition:**
```
1. Schemeshard publishes new version to scheme board
2. KQP sends query with SyncVersion=false (default)
3. Scheme cache returns cached value (stale!)
4. Scheme board notification arrives AFTER cache response
5. Query fails with version mismatch
```

### 2.5 ROOT CAUSE #5: Sibling In-Memory State Visibility

**Problem:** When sibling A updates in-memory state, sibling B may or may not see it depending on execution timing.

**Scenarios:**
```
Scenario A (B sees A's update):
  t0: A updates table.AlterVersion = 2
  t1: B reads table.AlterVersion → 2
  t2: B claims version 2→3 (increments further!)

Scenario B (B doesn't see A's update):
  t0: B reads table.AlterVersion → 1
  t1: A updates table.AlterVersion = 2
  t2: B claims version 1→2 (correct!)
```

The non-determinism depends on which sibling's HandleReply runs first.

---

## Part 3: Current Implementation Status

### 3.1 Fixes Applied (Phase 2 Complete)

| Fix | Location | Status | Notes |
|-----|----------|--------|-------|
| TVersionRegistry created | schemeshard_version_registry.* | ✅ Done | Central coordination |
| Schema table 130 | schemeshard_schema.h | ✅ Done | Persistence |
| CDC stream uses registry | operation_common_cdc_stream.cpp | ✅ Done | Migrated |
| Copy table uses registry | operation_copy_table.cpp | ✅ Done | Migrated |
| Alter table syncs parent index | operation_alter_table.cpp | ✅ Done | For impl tables |
| Restore finalize uses registry | operation_incremental_restore_finalize.cpp | ✅ Done | Migrated |
| Sync semantics (not increment) | Multiple files | ✅ Done | index = implTable |
| Parent table bump for cache | Multiple files | ✅ Done | Forces refresh |

### 3.2 Known Issues with Current Fixes

1. **Premature publishing NOT fully addressed** - Still publishes before convergence
2. **MAX() logic causes version drift** - Can escalate versions unexpectedly
3. **Scheme cache staleness window** - No retry mechanism
4. **No automatic retry on mismatch** - KQP fails immediately

---

## Part 4: Ideas to Try

### IDEA 1: Delay All Publishing Until Operation Completion

**Concept:** Don't publish any paths until ALL sibling parts have completed their HandleReply phase.

**Implementation:**
```cpp
// In operation HandleReply:
// Instead of:
context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

// Use:
context.OnComplete.DeferPublishToSchemeBoard(OperationId, pathId);

// In DoDoneTransactions (when ALL parts complete):
for (pathId in DeferredPublishPaths) {
    // Now publish with final, converged versions
    PublishToSchemeBoard(pathId);
}
```

**Pros:**
- Ensures all versions are finalized before any publish
- Eliminates race between sibling version increments and publishes

**Cons:**
- Requires new deferred publish mechanism
- May delay visibility of schema changes
- Complex to implement across all operation types

**Files to modify:**
- `schemeshard__operation.h` - Add DeferredPublishPaths
- `schemeshard__operation_side_effects.cpp` - Process deferred publishes in DoDoneTransactions
- All operation HandleReply methods - Use deferred publish

### IDEA 2: Two-Phase Publishing with Barrier

**Concept:** Use a barrier to ensure all siblings reach a sync point before any publishing occurs.

**Implementation:**
```cpp
// Phase 1: All siblings claim versions and register for barrier
auto result = registry.ClaimVersionChange(...);
operation.SetBarrier("version_convergence", OperationId.GetSubTxId());

// Phase 2: When ALL siblings hit barrier, release and publish
if (operation.AllSiblingsReachedBarrier("version_convergence")) {
    for (pathId : ClaimedPaths) {
        PublishToSchemeBoard(pathId);
    }
}
```

**Pros:**
- Explicit synchronization point
- Clear semantics for when publishing should occur

**Cons:**
- Adds latency waiting for slowest sibling
- Barrier mechanism already exists but may need extension

**Files to modify:**
- `schemeshard__operation.h` - Extend barrier mechanism
- Operation HandleReply methods - Add barrier points

### IDEA 3: Version Claim at Propose Phase (Earlier Coordination)

**Concept:** Claim versions during the Propose phase BEFORE any in-memory modifications, ensuring deterministic version assignment.

**Implementation:**
```cpp
// In Propose phase (before HandleReply):
void TPropose::DoPropose(context) {
    // Pre-claim all versions that will be needed
    for (pathId : PathsToModify) {
        registry.PreClaimVersion(OperationId, pathId);
    }
}

// In HandleReply:
void HandleReply(context) {
    // Use pre-claimed version, not current in-memory version
    ui64 targetVersion = registry.GetPreClaimedVersion(pathId);
    table.AlterVersion = targetVersion;
}
```

**Pros:**
- Removes in-memory state visibility problem
- Version assignment is deterministic

**Cons:**
- Requires knowing all affected paths upfront
- May not work for operations that discover paths dynamically
- Significant refactoring

### IDEA 4: Invalidate-Then-Publish Protocol

**Concept:** Before publishing new version, first send invalidation, wait for ack, then publish.

**Implementation:**
```cpp
// In PublishToSchemeBoard:
void PublishWithInvalidation(pathId, newVersion) {
    // Step 1: Send invalidation to scheme cache
    SendInvalidation(pathId);

    // Step 2: Wait for invalidation acknowledgment
    WaitForInvalidationAck(pathId);

    // Step 3: Now publish new version
    PublishNewVersion(pathId, newVersion);
}
```

**Pros:**
- Ensures cache doesn't have stale data when publish arrives
- Clean separation of concerns

**Cons:**
- Adds round-trip latency
- Complex distributed coordination
- May need new protocol messages

**Files to modify:**
- `schemeshard__publish_to_scheme_board.cpp` - Add invalidation step
- `scheme_board/cache.cpp` - Handle invalidation properly
- New protocol messages in `scheme_board.proto`

### IDEA 5: KQP Automatic Retry on Version Mismatch

**Concept:** Instead of failing, KQP should invalidate cache and retry when version mismatch is detected.

**Implementation:**
```cpp
// In kqp_metadata_loader.cpp:
if (entry.TableId.SchemaVersion != expectedSchemaVersion) {
    if (retryCount < MaxRetries) {
        // Invalidate cache entry
        SendCacheInvalidation(pathId);

        // Retry after short delay
        Schedule(RetryDelay, new TEvRetryLoad(pathId));
        return;
    }
    // Only fail after retries exhausted
    return Error("schema version mismatch after retries");
}
```

**Pros:**
- Handles transient staleness gracefully
- Doesn't require schemeshard changes
- Defense in depth

**Cons:**
- Adds query latency on retry
- Doesn't fix root cause
- May mask other issues

**Files to modify:**
- `ydb/core/kqp/gateway/kqp_metadata_loader.cpp` - Add retry logic
- `ydb/core/kqp/session_actor/kqp_query_state.cpp` - Handle retry

### IDEA 6: Authoritative Version Source (Single Writer)

**Concept:** Designate ONE sibling as the "authoritative" version writer for each path. Other siblings read but don't write.

**Implementation:**
```cpp
// Determine authoritative sibling (e.g., lowest SubTxId)
bool IsAuthoritativeSibling(pathId) {
    return registry.GetFirstClaimingSubTxId(pathId) == OperationId.GetSubTxId();
}

// Only authoritative sibling updates version
if (IsAuthoritativeSibling(pathId)) {
    table.AlterVersion = newVersion;
    PersistVersion(table);
    PublishToSchemeBoard(pathId);
}

// Other siblings just read the effective version
ui64 effectiveVersion = registry.GetEffectiveVersion(pathId);
```

**Pros:**
- Clear ownership of version writes
- No version drift from multiple writers
- Deterministic behavior

**Cons:**
- Requires tracking which sibling is authoritative
- May create bottleneck on authoritative sibling
- Complex coordination

### IDEA 7: Optimistic Locking with Version Vectors

**Concept:** Instead of single version number, use version vector to track which sibling contributed what.

**Implementation:**
```cpp
struct TVersionVector {
    THashMap<TSubTxId, ui64> Contributions;

    ui64 GetMergedVersion() {
        return MaxElement(Contributions.values());
    }
};

// Each sibling contributes its expected version
registry.ContributeVersion(OperationId, pathId, myExpectedVersion);

// Final version is merge of all contributions
ui64 finalVersion = registry.GetMergedVersion(pathId);
```

**Pros:**
- Handles arbitrary sibling orderings
- Clear audit trail of contributions
- Mathematically sound convergence

**Cons:**
- More complex data structure
- Higher storage overhead
- May be overkill

### IDEA 8: Publish Ordering Guarantees via Sequence Numbers

**Concept:** Assign sequence numbers to publishes and ensure replicas process them in order.

**Implementation:**
```cpp
// In PublishToSchemeBoard:
ui64 publishSeqNo = registry.GetNextPublishSeqNo(pathId);
message.SetSequenceNumber(publishSeqNo);

// In scheme board replica:
if (msg.SequenceNumber <= lastProcessedSeqNo[pathId]) {
    // Skip stale publish
    return;
}
processPublish(msg);
lastProcessedSeqNo[pathId] = msg.SequenceNumber;
```

**Pros:**
- Guarantees ordering of publishes
- Prevents stale data from overwriting newer data

**Cons:**
- Doesn't help with sibling convergence
- Only addresses publish ordering, not version assignment
- Adds complexity to scheme board protocol

### IDEA 9: Synchronous Sibling Communication

**Concept:** Siblings explicitly communicate their version claims before any persists.

**Implementation:**
```cpp
// Step 1: Each sibling broadcasts its version intent
BroadcastVersionIntent(OperationId, pathId, myTargetVersion);

// Step 2: Wait for all sibling intents
WaitForAllSiblingIntents(TxId, pathId);

// Step 3: Compute final version as max of all intents
ui64 finalVersion = Max(allSiblingIntents);

// Step 4: All siblings use same final version
table.AlterVersion = finalVersion;
```

**Pros:**
- Explicit coordination
- Guaranteed convergence
- Clear semantics

**Cons:**
- Requires sibling discovery mechanism
- Adds latency for synchronization
- Complex distributed coordination

### IDEA 10: Decouple Version from Schema Content

**Concept:** Use content-based versioning instead of monotonic counters.

**Implementation:**
```cpp
// Instead of:
ui64 AlterVersion;  // Monotonic counter

// Use:
TString ContentHash;  // Hash of schema content

// Version mismatch check becomes:
if (cachedHash != actualHash) {
    // Refresh needed
}
```

**Pros:**
- Version is deterministic based on content
- No ordering issues
- Idempotent by definition

**Cons:**
- Major architectural change
- Hash collisions (rare but possible)
- Breaks existing version comparison logic

---

## Part 5: Recommended Approach

Based on analysis, I recommend a **layered approach** combining multiple ideas:

### Layer 1: Fix Root Cause (IDEA 1 + IDEA 6)

**Deferred Publishing with Authoritative Sibling:**

1. Designate first-claiming sibling as authoritative for each path
2. Only authoritative sibling persists version changes
3. All siblings defer publishing until operation completion
4. When all parts complete, publish all paths with final versions

**Implementation Priority: HIGH**

### Layer 2: Defense in Depth (IDEA 5)

**KQP Retry on Mismatch:**

1. On version mismatch, invalidate cache entry
2. Retry query after short delay (100ms)
3. Max 3 retries before failing
4. Log metrics for retry frequency

**Implementation Priority: MEDIUM**

### Layer 3: Monitoring & Debugging

**Add Comprehensive Logging:**

1. Log all version claims with sibling info
2. Log publish timing relative to sibling completion
3. Add metrics for version mismatch frequency
4. Add test coverage for all sibling orderings

**Implementation Priority: HIGH**

---

## Part 6: Implementation Plan

### Phase 1: Immediate Fixes (This Sprint)

1. **Add version claim logging** - Understand current behavior
2. **Add retry logic in KQP** - Mitigate immediate failures
3. **Review and fix premature publishes** - Identify all early publish locations

### Phase 2: Architecture Improvements (Next Sprint)

1. **Implement deferred publishing** - Core fix for publish ordering
2. **Implement authoritative sibling** - Core fix for version assignment
3. **Add comprehensive tests** - Cover all sibling orderings

### Phase 3: Hardening (Following Sprint)

1. **Add monitoring dashboards** - Track version mismatch frequency
2. **Performance testing** - Ensure no regression from deferred publishing
3. **Documentation** - Update design docs

---

## Part 7: Files to Investigate/Modify

### Critical Files

| File | Purpose | Potential Changes |
|------|---------|-------------------|
| `schemeshard_version_registry.cpp` | Version coordination | Add authoritative sibling tracking |
| `schemeshard__operation_side_effects.cpp` | Operation lifecycle | Add deferred publish processing |
| `schemeshard__operation.h` | Operation state | Add DeferredPublishPaths |
| `schemeshard__publish_to_scheme_board.cpp` | Publishing | Add deferred publish support |
| `kqp_metadata_loader.cpp` | KQP metadata loading | Add retry logic |

### Operation Files

| File | Current Behavior | Needed Changes |
|------|------------------|----------------|
| `schemeshard__operation_copy_table.cpp` | Publishes in HandleReply | Defer publish |
| `schemeshard__operation_common_cdc_stream.cpp` | Publishes in HandleReply | Defer publish |
| `schemeshard__operation_alter_table.cpp` | Publishes in HandleReply | Defer publish |
| `schemeshard__operation_incremental_restore_finalize.cpp` | Publishes in HandleReply | Defer publish |

### Test Files

| File | Purpose |
|------|---------|
| `ut_parts_permutation.cpp` | Test sibling orderings |
| `ut_restore.cpp` | Restore operation tests |
| `datashard_ut_incremental_backup.cpp` | Backup tests |

---

## Part 8: Key Questions to Answer

1. **Why does the test pass sometimes but fail other times?**
   - Answer: Sibling execution order is non-deterministic

2. **Why does MAX() in registry cause issues?**
   - Answer: If sibling B sees A's update, B may claim a higher version, causing drift

3. **Why doesn't publishing after version update work?**
   - Answer: Other siblings may publish with different versions before convergence

4. **Why doesn't scheme cache invalidation help?**
   - Answer: Race between invalidation and query arrival

5. **What guarantees convergence?**
   - Answer: Need: (1) single authoritative writer OR (2) deferred publish until all complete

---

## Part 9: Test Scenarios to Cover

```
Scenario 1: Basic indexed table backup
- Create indexed table
- Run backup (CreateConsistentCopyTables)
- Query table immediately after
- Verify no version mismatch

Scenario 2: All permutations of 3-part operation
- [0, 1, 2], [0, 2, 1], [1, 0, 2], [1, 2, 0], [2, 0, 1], [2, 1, 0]
- All must pass without version mismatch

Scenario 3: Concurrent operations on same table
- Two backup operations on same table
- Verify conflict detection
- Verify no version mismatch

Scenario 4: Crash recovery
- Start backup operation
- Kill schemeshard mid-operation
- Restart and verify consistent state

Scenario 5: High load scenario
- Many concurrent queries during backup
- Verify no version mismatches
```

---

## Conclusion

The schema version sync issue is fundamentally a **distributed coordination problem** where multiple sibling operations must converge on the same version before publishing to the scheme cache.

**Root causes:**
1. Premature publishing before sibling convergence
2. Non-deterministic in-memory state visibility between siblings
3. Scheme cache staleness window

**Recommended solution:**
1. Deferred publishing until operation completion (fix root cause)
2. Authoritative sibling for version writes (ensure convergence)
3. KQP retry on mismatch (defense in depth)

The combination of these approaches should eliminate version mismatch errors while maintaining system performance and reliability.
