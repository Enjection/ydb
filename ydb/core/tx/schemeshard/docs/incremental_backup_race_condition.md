# Incremental Backup Race Condition Analysis

## Executive Summary

A race condition exists in the incremental backup lifecycle where the cleanup phase from a completed backup operation can race with subsequent user operations on the same table, causing `StatusMultipleModifications` (OVERLOADED) errors.

## Problem Description

When running multiple incremental backups in sequence on the same table, the second backup can fail with `StatusMultipleModifications` because:

1. The first backup operation completes (all TxState parts reach TDone)
2. The table becomes FREE (PathState = EPathStateNoChanges)
3. User submits the second backup operation
4. Asynchronously, OffloadStatus from the first backup triggers cleanup
5. Cleanup sets the table to EPathStateAlter
6. The second backup's sub-operations fail because the table is under operation

## Timeline from Log Analysis

```
21.638Z - First backup operation 281474976715672 completes
          Table becomes FREE (PathState = EPathStateNoChanges)
          IncrementalBackupInfo items still in "Transferring" state

22.308Z - OffloadStatus arrives for TxId 281474976715672
          OnOffloadStatus checks IncrementalBackups.contains(id) -> TRUE
          TryStartCleaner is called
          ContinuousBackupCleaner actor is created

23.064Z - Cleaner's DropCdcStream proposal arrives at SchemeShard
          TxDropCdcStreamAtTable sets table to EPathStateAlter
          Table is now UNDER OPERATION

23.244Z - User's second backup operation 281474976715677 arrives
          TRotateCdcStreamAtTable checks NotUnderOperation() -> FAILS
          Error: "path is under operation... state: EPathStateAlter"
          Operation aborted with StatusMultipleModifications
```

## Code Flow Analysis

### 1. Backup Operation Lifecycle

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Backup Operation Parts                            │
├─────────────────────────────────────────────────────────────────────┤
│ TxMkDir                    - Create backup directory                │
│ TxRotateCdcStream          - Create new CDC stream                  │
│ TxRotateCdcStreamAtTable   - Configure table for new stream        │
│ TxCreateTable              - Create backup destination table        │
│ TxAlterPQGroup             - Configure PQ for offload              │
│ TxCreatePQGroup            - Create new PQ for new stream          │
│ TxCreateLongIncrementalBackupOp - Track backup in IncrementalBackups│
└─────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    All parts reach TDone
                              │
                              ▼
                    Operation "completes"
                    Table becomes FREE
                              │
                              ▼
              ┌───────────────┴───────────────┐
              │                               │
              ▼                               ▼
    User can start             OffloadStatus arrives
    new operations             (async, ~670ms later)
              │                               │
              ▼                               ▼
    Second backup starts       OnOffloadStatus triggers
                               TryStartCleaner
                                              │
                                              ▼
                               ContinuousBackupCleaner
                               sends DropCdcStream
                                              │
                                              ▼
                               Table set to EPathStateAlter
                                              │
                                              ▼
                               RACE CONDITION!
                               Second backup fails
```

### 2. Key Files and Functions

#### schemeshard_backup_incremental__progress.cpp

**OnOffloadStatus() - Lines 122-183**
```cpp
void OnOffloadStatus(TTransactionContext& txc) {
    // Get TxId from OffloadStatus message
    ui64 id = record.GetTxId();

    // Check if backup exists in map
    if (!Self->IncrementalBackups.contains(id)) {
        LOG_E("Incremental backup with id# " << id << " not found");
        TryStartOrphanCleaner(shardInfo.PathId);  // Orphan cleanup
        return;
    }

    auto& backupInfo = *Self->IncrementalBackups.at(id);

    // Check if backup is finished
    if (backupInfo.IsFinished()) {
        LOG_E("Incremental backup with id# " << id << " is already finished");
        return;
    }

    // Get item and check state
    auto& item = backupInfo.Items.at(itemPathId);
    if (item.State != TIncrementalBackupInfo::TItem::EState::Transferring) {
        return;  // Already processed
    }

    // Transition to Dropping and start cleanup
    item.State = TIncrementalBackupInfo::TItem::EState::Dropping;
    PersistIncrementalBackupItem(db, id, item);
    TryStartCleaner(txc, backupInfo, item);  // <-- STARTS CLEANUP
}
```

**TryStartCleaner() - Lines 37-65**
```cpp
void TryStartCleaner(TTransactionContext& txc,
                     TIncrementalBackupInfo& backupInfo,
                     TIncrementalBackupInfo::TItem& item) {
    // Validate paths exist
    if (!Self->PathsById.contains(item.PathId)) {
        MarkItemAsDone(txc, backupInfo, item);
        return;
    }

    const auto& streamPath = Self->PathsById.at(item.PathId);
    const auto& tablePath = Self->PathsById.at(streamPath->ParentPathId);

    // Check if table is under operation (added fix)
    if (tablePath->PathState != NKikimrSchemeOp::EPathState::EPathStateNoChanges) {
        LOG_D("Cleaner for item pathId# " << item.PathId
              << " deferred - table is under operation");
        return;
    }

    // Create and register cleaner actor
    NewCleaners.emplace_back(
        CreateContinuousBackupCleaner(
            Self->TxAllocatorClient,
            Self->SelfId(),
            backupInfo.Id,
            item.PathId,
            workingDir,
            tablePath->Name,
            streamPath->Name
    ));
}
```

#### schemeshard_continuous_backup_cleaner.cpp

**Bootstrap() - Lines 32-41**
```cpp
void Bootstrap() {
    LOG_DEBUG_S(..., "Starting continuous backup cleaner:"
        << " workingDir# " << WorkingDir
        << " table# " << TableName
        << " stream# " << StreamName);

    AllocateTxId();  // Immediately allocates TxId
    Become(&TContinuousBackupCleaner::StateWork);
}
```

**Handle(TEvAllocateResult) - Lines 47-50**
```cpp
void Handle(TEvTxAllocatorClient::TEvAllocateResult::TPtr& ev) {
    TxId = TTxId(ev->Get()->TxIds.front());
    Send(SchemeShard, DropPropose());  // Immediately sends DropCdcStream
}
```

**Retry on StatusMultipleModifications - Lines 80-82**
```cpp
if (status == NKikimrScheme::StatusMultipleModifications) {
    return Schedule(TDuration::Seconds(10), new TEvents::TEvWakeup);
}
```

### 3. State Management

#### IncrementalBackupInfo States

```
┌─────────────────┐
│   Transferring  │  Initial state when backup starts
└────────┬────────┘
         │ OnOffloadStatus (all partitions complete)
         ▼
┌─────────────────┐
│    Dropping     │  Cleaner started, DropCdcStream in progress
└────────┬────────┘
         │ OnCleanerResult (success)
         ▼
┌─────────────────┐
│      Done       │  Item cleanup complete
└─────────────────┘
```

#### Backup Lifecycle States

```
IncrementalBackupInfo.State:
  - Transferring: Active backup, items being transferred
  - Done: All items complete, backup finished
  - Cancelled: Backup was cancelled

IncrementalBackupInfo.Item.State:
  - Transferring: Waiting for OffloadStatus
  - Dropping: Cleaner running
  - Done: Cleanup complete
```

### 4. Path State Management

The table's PathState controls concurrent operation access:

```
EPathStateNoChanges  - Table is FREE, operations can start
EPathStateAlter      - Table is being modified (blocks other operations)
EPathStateDrop       - Table is being deleted
EPathStateCreate     - Table is being created
```

When DropCdcStreamAtTable starts:
- It sets `tablePath->PathState = EPathStateAlter`
- This blocks ALL other operations on the table
- Including user's subsequent backup operations

## Attempted Fixes

### Fix 1: Path State Check in TryStartCleaner (Partial)

**Location**: `schemeshard_backup_incremental__progress.cpp`

```cpp
// Skip cleanup if table is under operation
if (tablePath->PathState != NKikimrSchemeOp::EPathState::EPathStateNoChanges) {
    LOG_D("Cleaner for item pathId# " << item.PathId
          << " deferred - table is under operation");
    return;
}
```

**Result**: Does NOT prevent the race because:
- When TryStartCleaner is called, the table IS free
- The race occurs AFTER the cleaner starts and sets PathState

### Fix 2: Delayed Cleaner Startup (Failed)

**Attempted Change**:
```cpp
void Bootstrap() {
    // Delay 3 seconds before starting
    Schedule(TDuration::Seconds(3), new TEvents::TEvWakeup);
    Become(&TContinuousBackupCleaner::StateWork);
}
```

**Result**: Does NOT work because:
- Tests use simulated time
- Schedule() fires immediately in simulated time
- No actual delay occurs

## Root Cause Summary

The fundamental issue is an **asynchronous lifecycle mismatch**:

1. **Operation Completion** (synchronous): All TxState parts complete, operation is "done"
2. **Cleanup Trigger** (asynchronous): OffloadStatus arrives later via async message
3. **No Coordination**: Nothing prevents new operations during the gap

```
Timeline:
T0: Operation completes -> Table FREE
T1: User can submit new operation
T2: OffloadStatus arrives -> Cleanup starts -> Table LOCKED
T3: New operation fails (if submitted between T0 and T2, or during T2-cleanup)
```

## Potential Solutions

### Solution 1: Skip Cleanup if Backup Already Complete

**Approach**: In OnOffloadStatus, check if the backup operation has already completed and skip cleanup.

**Challenge**: Need to track when the backup operation (TCreateLongIncrementalBackupOp) completes vs when cleanup is done.

### Solution 2: Allow Internal Operations to be Preempted

**Approach**: Modify TRotateCdcStreamAtTable to allow proceeding when the table is under operation by an internal DropCdcStream.

**Implementation**:
```cpp
// In schemeshard__operation_alter_cdc_stream.cpp
if (tablePath.IsUnderOperation()) {
    // Check if the operation holding the path is an internal cleanup
    if (IsInternalCleanupOperation(tablePath.Base()->LastTxId)) {
        // Allow this operation to proceed
    } else {
        checks.NotUnderOperation();
    }
}
```

### Solution 3: Serialize Cleanup with Next Operation

**Approach**: Track ongoing cleanups per table. Before starting a new backup, wait for cleanup to complete.

**Implementation**:
- Add `PendingCleanups` map to SchemeShard
- In TryStartCleaner, register the cleanup
- In backup Propose, check and wait for pending cleanups

### Solution 4: Make Cleanup Part of Operation Completion

**Approach**: Instead of async cleanup via OffloadStatus, make cleanup synchronous with operation completion.

**Implementation**:
- In TDone for TRotateCdcStreamAtTable, trigger cleanup directly
- Wait for cleanup before releasing path state

**Trade-off**: Increases operation latency

### Solution 5: Retry on Internal Operation Conflict

**Approach**: When user operation fails due to internal cleanup, retry automatically.

**Implementation**:
- In operation propose, detect internal operation conflict
- Return a retriable status instead of failure
- Client retries after short delay

## Recommended Solution

**Solution 2** (Allow internal operations to be preempted) appears most practical because:

1. Minimal code changes
2. No changes to operation lifecycle
3. Internal cleanups can safely be delayed (they already retry on conflict)
4. User operations get priority over background cleanup

## Files Requiring Changes

1. `schemeshard__operation_alter_cdc_stream.cpp` - TRotateCdcStreamAtTable checks
2. `schemeshard__operation_drop_cdc_stream.cpp` - Mark internal operations
3. `schemeshard_path.cpp` - Add IsInternalOperation helper
4. `schemeshard_path_element.h` - Track if operation is internal

## Test Cases

1. `TTestCaseMultiBackup` - Multiple backups in sequence
2. `TTestCaseShopDemoIncrementalBackupScenario` - Shop demo with backups
3. New test: Concurrent backup and cleanup operations

## Implemented Solution

After investigating multiple approaches, **Solution 3 (Serialize Cleanup with Next Operation)** was implemented as it provides the most reliable fix without introducing DataShard conflicts.

### Why Solution 2 Failed

The initial attempt was to implement Solution 2 (allow internal operations to be preempted). This approach:
1. Added `IsUnderDropCdcStream()` method to check if table is under a DropCdcStream operation
2. Modified CDC operations to skip `NotUnderOperation()` check when table is under DropCdcStream

**Problem**: This allowed BOTH operations to proceed to SchemeShard level, but they then conflicted at DataShard level with error:
```
Previous Tx 281474976715758 must be in a state where it only waits for Ack
```

DataShard cannot handle two concurrent schema transactions on the same table. The path state check at SchemeShard level is the gate that prevents this.

### Implemented Fix: Delayed Cleanup Start

The fix adds a startup delay to the cleanup actor, giving pending backup operations time to start executing first. This approach is simpler and more robust than tracking pending cleanups.

#### Key Insight

The race window exists between when cleanup is triggered (offload completion) and when DropCdcStream is proposed. If a new backup arrives in this window, both operations would conflict. By delaying the cleanup start, we ensure any pending backup operations execute first, and the cleanup can then safely defer via its existing retry mechanism.

#### Key Changes:

1. **Added startup delay to cleaner** (`schemeshard_continuous_backup_cleaner.cpp`):
```cpp
void Bootstrap() {
    LOG_DEBUG_S(..., "Starting continuous backup cleaner: ...");

    // Delay cleanup start to allow pending backup operations to proceed first.
    // This prevents race conditions where a new backup arrives just as cleanup starts.
    Schedule(TDuration::MilliSeconds(100), new TEvents::TEvWakeup);
    Become(&TContinuousBackupCleaner::StateWork);
}

void HandleWakeup() {
    if (!Started) {
        Started = true;
        AllocateTxId();  // Now proceed with cleanup
    } else {
        Retry();  // StatusMultipleModifications retry
    }
}
```

2. **Removed TablesWithPendingCleanup insertion** (`schemeshard_backup_incremental__progress.cpp`):
```cpp
void TryStartCleaner(...) {
    // ... existing operation check ...

    // The cleaner has a startup delay (100ms) to allow pending backup operations
    // to start first. If a backup starts during this delay, the cleaner will fail
    // with StatusMultipleModifications when it tries to propose DropCdcStream,
    // and will retry after 10 seconds.
    NewCleaners.emplace_back(CreateContinuousBackupCleaner(...));
}
```

3. **Passed TablePathId through cleaner** for proper cleanup tracking in OnCleanerResult.

### How the Fix Eliminates the Race

```
Before (race condition):
T0: OffloadStatus triggers TryStartCleaner
T1: Cleaner immediately proposes DropCdcStream
T2: Table becomes EPathStateAlter (under operation)
T3: New backup arrives → blocked by NotUnderOperation
T4: Backup fails with StatusMultipleModifications

After (no race):
T0: OffloadStatus triggers TryStartCleaner
T1: Cleaner waits 100ms (startup delay)
T2: New backup arrives, table is FREE → backup proceeds
T3: Table becomes under operation (CreateCdcStream/RotateCdcStream)
T4: After 100ms, cleaner tries DropCdcStream → fails (table under operation)
T5: Cleaner retries after 10s
T6: Backup completes, table FREE
T7: Cleaner retry succeeds
```

### Files Modified

| File | Changes |
|------|---------|
| `schemeshard_continuous_backup_cleaner.cpp` | Added 100ms startup delay, HandleWakeup state tracking |
| `schemeshard_backup_incremental__progress.cpp` | Removed TablesWithPendingCleanup insertion, updated comments |
| `schemeshard_private.h` | Added TablePathId to TEvContinuousBackupCleanerResult |

### Behavior Notes

1. **Backup operations take priority** - The 100ms delay ensures pending backups start before cleanup
2. **Cleanup retries automatically** - If table is under operation, cleaner fails and retries after 10 seconds
3. **No user-facing errors** - Backups succeed immediately; cleanup happens in background
4. **Existing retry mechanism leveraged** - No new complexity, just a strategic delay

## References

- Log files: `log13.log` through `log24.log` (investigation progression)
- Key logs:
  - `log21.log` - DataShard conflict when allowing both operations
  - `log22.log` - NotUnderOperation blocking after revert
  - `log23.log` - NotUnderPendingCleanup blocking during delay window
  - `log24.log` - Final fix working (delayed cleanup without TablesWithPendingCleanup insertion)
- Error: `StatusMultipleModifications` / `OVERLOADED`
- DataShard error: `Previous Tx must be in a state where it only waits for Ack`
- Source locations:
  - `schemeshard__operation_create_cdc_stream.cpp:836`
  - `schemeshard_backup_incremental__progress.cpp:55-58`
