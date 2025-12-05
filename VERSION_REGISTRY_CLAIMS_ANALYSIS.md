# Why Version Registry Claims Don't Work for Child Index Sync Version Bump

## Problem Statement

The test `SimpleBackupRestoreWithIndex` fails with:
```
schema version mismatch during metadata loading for: /Root/TableWithIndex/idx/indexImplTable expected 1 got 2
```

This occurs because the scheme cache has stale `TIndexDescription::SchemaVersion = 1` while the actual impl table has `AlterVersion = 2`.

## The Real Root Cause: Premature Publishing

The fundamental issue is that **source tables are published during the Propose phase, BEFORE version syncs happen in HandleReply**.

### Why Do We Publish Early?

Looking at the logs, the publish flow is:

```
11:43:20.999 - Propose phases complete, "Activate send" for all sub-operations
11:43:21.000 - TTxPublishToSchemeBoard DoExecute - BATCH PUBLISH starts
             - Publishes pathId 2 (main table) with index SchemaVersion = 1
             - Publishes pathId 3 (index)
             - Publishes pathId 4 (impl table)
             - Publishes pathIds 10-22 (backup collection paths)
11:43:21.231 - HandleReply runs, syncs index version 1 → 2
11:43:21.239 - Another publish batch with correct SchemaVersion = 2
```

The early publish at 11:43:21.000 includes the SOURCE tables (2, 3, 4) even though their versions will be modified in HandleReply 231ms later.

### The Correct Design

**You're absolutely right** - we should only publish the final "converged" version. The current design violates this:

1. Each sub-operation's Propose phase schedules publishes for paths it touches
2. All scheduled publishes are batched and executed together
3. This includes source tables that haven't had their version syncs yet
4. HandleReply then does the real work (version syncs) and publishes again

The scheme cache may see the early publish and cache stale data. Even though a later publish with correct data arrives, there may be race conditions or caching behavior that causes the stale data to be returned.

## Timeline Analysis

From the logs, here's the sequence of events:

1. **11:43:21.000** - Main table (pathId 2) published with **GeneralVersion 3**
   - At this point, index version = 1
   - `TIndexDescription::SchemaVersion = 1` in this publish
   - **THIS IS THE PROBLEM - publishing before version sync**

2. **11:43:21.231** - HandleReply runs and syncs child index
   - Index version updated: 1 → 2
   - Log: `CopyTable SYNCED child index version, oldVersion: 1, newVersion: 2`

3. **11:43:21.239** - DescribeTableIndex correctly sets SchemaVersion = 2

4. **11:43:21.250** - Main table published with **GeneralVersion 6**
   - This publish has `TIndexDescription::SchemaVersion = 2`

5. **11:43:22.533** - Query fails
   - KQP sees `TIndexDescription::SchemaVersion = 1` (from stale cache)
   - Impl table has `AlterVersion = 2`
   - **Mismatch!**

## Secondary Issue: Version Registry Claim Behavior

*Note: This is a secondary issue. The PRIMARY issue is premature publishing during Propose phase.*

### How Claims Work

The `TVersionRegistry::ClaimVersionChange` function has this behavior:

```cpp
EClaimResult ClaimVersionChange(
    TOperationId opId,
    TPathId pathId,
    ui64 currentVersion,
    ui64 targetVersion,
    ETxType opType,
    TString debugInfo);
```

Returns:
- `Claimed` - First to claim, registered new claim
- `Joined` - Same TxId already claimed this pathId, joined existing claim
- `Conflict` - Different TxId already claimed

### The Key Design Decision

From `IMPLEMENTATION_PROGRESS.md`:

> **Fix 1: Remove Max() when joining claims**
> - Removed `Max()` logic when joining an existing claim
> - The old logic caused ClaimedVersion to drift when second sibling read already-updated version
> - **First sibling's target version is now authoritative for the entire TxId**

This means: **When a sibling joins an existing claim, the `ClaimedVersion` is NOT updated to the new target version.**

### Why This Breaks Version Bump After Child Index Sync

Here's the version bump flow in `TCopyTable::TPropose::HandleReply`:

```
Step 1: CDC Version Sync (lines 232-269)
========================================
- oldSrcVersion = srcTable->AlterVersion = 1
- newSrcVersion = oldSrcVersion + 1 = 2
- ClaimVersionChange(pathId=2, current=1, target=2) → returns Claimed
- Registry now has: pathId 2 → ClaimedVersion = 2
- srcTable->AlterVersion = 2

Step 2: Child Index Sync (lines 275-322)
========================================
- Iterates over child indexes
- Syncs index version to match table's effective version
- childIndex->AlterVersion = 2
- childIndexSynced = true

Step 3: Re-publish After Index Sync (lines 328-359)
===================================================
- GOAL: Bump table version to force GeneralVersion increase
- currentTableVersion = srcTable->AlterVersion = 2
- targetBumpVersion = currentTableVersion + 1 = 3
- ClaimVersionChange(pathId=2, current=2, target=3)
  → Returns **Joined** (not Claimed!)
  → ClaimedVersion stays at 2 (not updated to 3)
- GetEffectiveVersion(pathId=2) returns 2 (the original claim)
- srcTable->AlterVersion stays at 2 (no update since effective == current)
```

### Why This Is Secondary

The registry behavior prevents us from bumping the version higher. BUT even if we could bump higher, **the early publish during Propose already sent stale data to the scheme cache**.

The correct fix is to NOT publish early, not to fight with the registry's "join without update" behavior.

## Why Version Bumping Alone Won't Fix This

The original code tried to bump the version after child index sync:
```cpp
++srcTable->AlterVersion;
```

But even if this worked perfectly, **the early publish during Propose already sent stale data**.

The scheme cache received:
1. **Early publish** (Propose phase): SchemaVersion = 1, GeneralVersion = X
2. **Later publish** (HandleReply): SchemaVersion = 2, GeneralVersion = X+N

Even though the later publish has a higher GeneralVersion, there may be race conditions or caching behavior where the scheme cache returns the stale data from the early publish.

**The only reliable fix is to NOT publish early.**

## Correct Solution: Don't Publish Until Convergence

The correct fix is to **not publish source tables during Propose phase** when their versions will be modified in HandleReply. Publish only the final "converged" version.

### Proposed Fix: Skip Source Table Publish in Propose

For operations that will sync versions in HandleReply (CDC stream creation, CopyTable with CDC):

1. **In Propose phase**: Only publish NEW paths (backup copies, streams), NOT source tables
2. **In HandleReply**: Do all version syncs, THEN publish source tables once with final versions

This ensures:
- Source tables are published exactly ONCE with correct data
- No stale intermediate states in scheme cache
- The "converged" version is the only version published

### Implementation Details

The key changes would be in:

1. **CDC Stream Creation** (`schemeshard__operation_create_cdc_stream.cpp`):
   - In TPropose: Don't schedule PublishToSchemeBoard for target table
   - In TProposeAtTable::HandleReply: Publish target table after version sync

2. **CopyTable** (`schemeshard__operation_copy_table.cpp`):
   - In TPropose: Don't schedule PublishToSchemeBoard for source table
   - In TPropose::HandleReply: Publish source table after all version syncs

3. **Consistent Copy Tables** (`schemeshard__operation_consistent_copy_tables.cpp`):
   - Review which paths are published during Propose
   - Defer source table publishes to HandleReply phases

### Why Version Registry "Bump Higher" Won't Work

Even if we try to bump the version higher after child index sync using the registry:

```cpp
if (childIndexSynced) {
    // This returns "Joined" - doesn't update ClaimedVersion
    auto result = VersionRegistry.ClaimVersionChange(
        OperationId, srcPathId, currentVersion, currentVersion + 1, ...);
}
```

The problem is:
1. An earlier claim exists for the same pathId from CDC version sync
2. ClaimVersionChange returns `Joined` (same TxId already claimed)
3. Registry design: "First sibling's claim is authoritative" - doesn't update ClaimedVersion
4. The version bump is effectively ignored

**This is a secondary issue.** The PRIMARY issue is publishing early with stale data.

## Summary

| Problem | Cause | Solution |
|---------|-------|----------|
| Stale SchemaVersion in cache | Source table published BEFORE version sync | Don't publish source tables in Propose |
| Version registry can't bump higher | "First claim wins" design | N/A if we don't publish early |
| Multiple publishes with different data | Publish in Propose AND HandleReply | Single publish in HandleReply only |

## Key Insight

The version registry was designed for **sibling coordination** - ensuring multiple sub-operations (same TxId) see consistent versions. It's NOT designed for:
- Updating a claimed version to a higher value
- Sequential version bumps within a single operation

But **this doesn't matter** if we simply don't publish until we have the final converged version. The registry can continue doing its job (sibling coordination) while we fix the premature publishing issue.

---

## Implementation Progress (Dec 5, 2025)

### Fix 1: Skip Early Publish in TProposeAtTable (CDC Stream)

**File:** `schemeshard__operation_common_cdc_stream.cpp`

The `TProposeAtTable::HandleReply` was publishing the table during Propose phase. We added a check to skip this publish when there's a sibling CopyTable operation that will handle the final publish:

```cpp
bool hasSiblingCopyTable = false;
if (versionCtx.IsContinuousBackupStream || versionCtx.IsPartOfContinuousBackup) {
    // Check if there's a sibling CopyTable operation in the same TxId
    if (context.SS->Operations.contains(txId)) {
        auto operation = context.SS->Operations.at(txId);
        for (const auto& part : operation->Parts) {
            if (siblingTxState->TxType == TTxState::TxCopyTable) {
                hasSiblingCopyTable = true;
                break;
            }
        }
    }
}
if (!hasSiblingCopyTable) {
    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
}
```

**Result:** Fixed other CDC stream tests that were breaking. But `SimpleBackupRestoreWithIndex` still failed because the backup flow uses `DoCreateStreamImpl` (not `TNewCdcStreamAtTable`).

### Fix 2: Found the Real Early Publish Source - IncAliveChildrenSafeWithUndo

**Root Cause Discovery:**

When `TNewCdcStream` creates a CDC stream as the **first child** of a table, `IncAliveChildrenSafeWithUndo` publishes the **grandparent**:

```cpp
void IncAliveChildrenSafeWithUndo(const TOperationId& opId, const TPath& parentPath, ...) {
    parentPath.Base()->IncAliveChildrenPrivate(isBackup);
    if (parentPath.Base()->GetAliveChildren() == 1 && !parentPath.Base()->IsDomainRoot()) {
        auto grandParent = parentPath.Parent();
        if (grandParent.IsActive()) {
            context.SS->ClearDescribePathCaches(grandParent.Base());
            context.OnComplete.PublishToSchemeBoard(opId, grandParent->PathId);  // ← EARLY PUBLISH!
        }
    }
}
```

**For the impl table CDC stream:**
- CDC stream (pathId 17) is created under impl table (pathId 4)
- Impl table has no children before → GetAliveChildren() becomes 1
- Grandparent = main table (pathId 2)
- **Main table is published with stale TIndexDescription.SchemaVersion!**

### Fix 3: TNewCdcStream - Skip Grandparent Publish for Continuous Backup

**File:** `schemeshard__operation_create_cdc_stream.cpp`

```cpp
const bool isContinuousBackup = streamName.EndsWith("_continuousBackupImpl");
if (isContinuousBackup) {
    // Just increment children count without publishing grandparent
    tablePath.Base()->IncAliveChildrenPrivate(false);
    if (tablePath.Base()->GetAliveChildren() == 1 && !tablePath.Base()->IsDomainRoot()) {
        auto grandParent = tablePath.Parent();
        if (grandParent.Base()->IsLikeDirectory()) {
            ++grandParent.Base()->DirAlterVersion;
            context.MemChanges.GrabPath(context.SS, grandParent.Base()->PathId);
            context.DbChanges.PersistPath(grandParent.Base()->PathId);
        }
        // Skip grandparent publish - CopyTable HandleReply will handle it
    }
} else {
    IncAliveChildrenSafeWithUndo(OperationId, tablePath, context);
}
```

### Fix 4: TCreatePQ - Skip Grandparent Publish for Continuous Backup

**File:** `schemeshard__operation_create_pq.cpp`

Same issue: When PQ (PersQueue) is created under a CDC stream, it's the first child, so grandparent (the table) is published early.

```cpp
const bool isContinuousBackupCdc = parentPath.Base()->IsCdcStream() && 
                                   parentPath.Base()->Name.EndsWith("_continuousBackupImpl");
if (isContinuousBackupCdc) {
    // Just increment children count without publishing grandparent
    parentPath.Base()->IncAliveChildrenPrivate(false);
    // ... (same pattern as above)
    // Skip grandparent publish - CopyTable HandleReply will handle it
} else {
    IncAliveChildrenSafeWithUndo(OperationId, parentPath, context);
}
```

### Current Status

After Fixes 3 and 4:
- **Verified:** Debug logs show `isContinuousBackup: 1` and `isContinuousBackupCdc: 1` - detection works
- **Verified:** PathId 2 is NO LONGER in the early Propose phase publish batch
- **Verified:** PathId 2 IS published in HandleReply phase at the correct time
- **Verified:** Version 6 is published (correct GeneralVersion after syncs)

**But test still fails with "expected 1 got 2"**

### Current Investigation: Describe Time vs Sync Time

The mystery: Even though the early publish is removed, the scheme cache still sees `SchemaVersion = 1`.

Possible causes being investigated:
1. Multiple publishes of pathId 2 in HandleReply (we see 3 DescribePath calls for pathId 2)
2. The index version might not be visible when the table is described
3. Transaction boundary issue between HandleReply and TTxPublishToSchemeBoard

Added debug logging to `DescribeTableIndex` to trace what `SchemaVersion` is being set during describe:
```cpp
LOG_DEBUG_S(..., "DescribeTableIndex setting SchemaVersion"
            << ", indexPathId: " << pathId
            << ", schemaVersion: " << indexInfo->AlterVersion);
```

### Finding: SchemaVersion IS Correct at Describe Time!

With the new logging, we confirmed:
- **At describe time (18:51:34.083):** SchemaVersion = 2 (CORRECT!)
- **AckPublish (18:51:34.095):** PathId 2 version 6 acknowledged
- **Query fails (18:51:35.378):** "expected 1 got 2" - 1.28 seconds LATER

The schemeshard is describing and publishing the correct data. But 1.28 seconds later, KQP still sees SchemaVersion=1.

### How KQP Checks Schema Version

In `kqp_metadata_loader.cpp`:
1. KQP loads main table metadata from scheme cache
2. Gets `TIndexDescription` which includes `SchemaVersion` (from `indexInfo->AlterVersion`)
3. Uses `TIndexId{..., SchemaVersion=X}` to load impl table
4. Compares `expectedSchemaVersion` (from TIndexDescription) with `implTable.TableId.SchemaVersion`
5. If mismatch → ERROR

The comparison:
- `expectedSchemaVersion` = TIndexDescription.SchemaVersion (should be 2 if cache is fresh)
- `entry.TableId.SchemaVersion` = impl table's actual version (2)
- Error says "expected 1" → Scheme cache returned stale TIndexDescription with SchemaVersion=1

### Root Cause Hypothesis: Scheme Cache Propagation Issue

The issue is NOT in the schemeshard (it publishes correctly). The issue is that:
1. The scheme board received version 6 correctly
2. The scheme board should notify the scheme cache
3. The scheme cache should invalidate its entry for pathId 2
4. KQP queries scheme cache 1.28 seconds later
5. **Scheme cache still returns old data**

Possible causes:
1. Scheme cache subscription isn't picking up the update
2. Test runtime doesn't properly propagate scheme board notifications
3. There's a separate caching layer that isn't being invalidated

### Potential Solutions

1. **Force scheme cache refresh** - Add explicit invalidation or wait for cache to update
2. **Add SyncVersion flag** - When querying scheme cache, request sync version validation
3. **Fix scheme cache subscription** - Ensure notifications are properly delivered

### Scheme Cache Behavior Discovery

In `ydb/core/tx/scheme_board/cache.cpp`, `HandleEntry` (lines 2449-2455):

```cpp
if (cacheItem->IsFilled() && !entry.SyncVersion) {
    cacheItem->FillEntry(context.Get(), entry);  // Return cached data immediately
}

if (entry.SyncVersion) {
    cacheItem->AddInFlight(context, index, true);  // Wait for sync
}
```

**Key Finding:** When `SyncVersion = false` (default), the scheme cache returns cached data WITHOUT checking for updates. If the notification from scheme board hasn't been processed yet, stale data is returned.

### How Scheme Board Notifications Work

1. Schemeshard publishes to scheme board via `TTxPublishToSchemeBoard`
2. Scheme board populator sends `TEvUpdate` to replica
3. Replica sends `TEvNotifyUpdate` to all subscribers (scheme caches)
4. Scheme cache's `HandleNotify` processes the notification
5. `cacheItem->Fill(notify)` updates the cached data

### The Race Condition

The issue appears to be a race between:
1. KQP's navigate request (with `SyncVersion=false`) arrives at scheme cache
2. Scheme board's `TEvNotifyUpdate` notification arrives at scheme cache

If (1) happens before (2) completes, KQP gets stale data.

Even though there's a 5-second sleep in the test, the scheme board notification might not be processed correctly in the test runtime, or there's a path/pathId mismatch causing the update to go to a different cache item.

### Potential Fixes

**Option 1: Force SyncVersion for tables with indexes**
- When KQP loads a table with indexes, set `SyncVersion=true` to force fresh data
- Pros: Guaranteed consistency
- Cons: May increase latency

**Option 2: Ensure notification delivery in test runtime**
- Debug why notifications aren't reaching the scheme cache
- May be test-runtime specific

**Option 3: Invalidate cache more aggressively**
- When version bumps happen, ensure cache is invalidated before returning
- Complex to implement

### Sync Version Deep Dive

Even with `SyncVersion = true` (which the KQP metadata loader uses), there's still a race condition:

1. **KQP requests with SyncVersion=true**
2. **Scheme cache calls `SendSyncRequest()` to subscriber**
3. **Subscriber forwards `TEvSyncVersionRequest` to replica**
4. **Replica responds with its current data**

**The Race**: If step 4 happens BEFORE the scheme board populator has sent the update to the replica, the sync response contains stale data!

### Flow of Updates (vs Sync Requests)

**Update flow (from schemeshard):**
```
Schemeshard → Populator → Replica → Subscriber → Scheme Cache
```

**Sync request flow (from KQP):**
```
KQP → Scheme Cache → Subscriber → Replica → Response
```

These are **independent event streams**. If KQP's sync request arrives at the replica before the populator's update, KQP gets stale data.

### Why the Test Fails

1. **18:51:34.083** - Schemeshard describes pathId 2 with SchemaVersion=2
2. **18:51:34.095** - AckPublish confirms scheme board received the data
3. But this only means the **schemeshard** received the ack from **scheme board populator**
4. The update still needs to propagate: Populator → Replica → Subscriber → Cache
5. **18:51:35.378** - KQP query runs, sync request goes to replica
6. If replica hasn't received the update yet, it returns SchemaVersion=1

### Potential Fixes

**Option 1: Wait for scheme board replication (complex)**
- Schemeshard waits for ALL replicas to confirm receipt before completing operation
- Ensures consistency but adds latency

**Option 2: Retry on version mismatch (pragmatic)**
- When KQP sees "schema version mismatch", retry with fresh scheme cache request
- Already partially implemented (compilation retry on ABORTED)

**Option 3: Force scheme cache invalidation (targeted)**
- After backup operation completes, send `TEvInvalidateTable` for affected tables
- Forces next query to fetch fresh data from schemeshard

**Option 4: Test-specific fix**
- Ensure test runtime properly processes all pending events
- `SimulateSleep` might not dispatch scheme board notifications

### Recommendation

The cleanest fix is **Option 3**: Add explicit scheme cache invalidation for the source table after the backup operation completes. This ensures any subsequent queries see the correct SchemaVersion.

Alternatively, investigate if the test's event dispatch is the issue - the 5-second sleep should be enough for real propagation, but test runtime might not process events correctly.

