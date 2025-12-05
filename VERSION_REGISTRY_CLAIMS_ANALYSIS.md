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

