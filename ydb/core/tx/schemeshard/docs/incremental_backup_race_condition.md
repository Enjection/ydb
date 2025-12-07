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

### Implemented Fix: Track Tables with Pending Cleanup

The fix adds a tracking mechanism that marks tables as having pending cleanup BEFORE the cleaner actor is created, eliminating the race window.

#### Key Changes:

1. **Added tracking set** (`schemeshard_impl.h`):
```cpp
// Tables that have pending CDC stream cleanup. User operations on these tables
// should fail with StatusMultipleModifications to avoid racing with cleanup.
// The cleanup actor will retry if it encounters conflicts.
THashSet<TPathId> TablesWithPendingCleanup;
```

2. **Mark table when cleanup starts** (`schemeshard_backup_incremental__progress.cpp`):
```cpp
void TryStartCleaner(...) {
    // ... existing checks ...

    // Mark table as having pending cleanup to prevent user CDC operations from racing.
    // User operations will check this set and fail with StatusMultipleModifications.
    // The set entry is cleared in OnCleanerResult when cleanup completes.
    Self->TablesWithPendingCleanup.insert(tablePath->PathId);

    NewCleaners.emplace_back(CreateContinuousBackupCleaner(...));
}
```

3. **Clear mark when cleanup completes** (`schemeshard_backup_incremental__progress.cpp`):
```cpp
void OnCleanerResult(TTransactionContext& txc) {
    // ...

    // Clear pending cleanup marker for the table.
    // itemPathId is the stream's PathId, we need to find the table's PathId.
    if (Self->PathsById.contains(itemPathId)) {
        const auto& streamPath = Self->PathsById.at(itemPathId);
        if (Self->PathsById.contains(streamPath->ParentPathId)) {
            Self->TablesWithPendingCleanup.erase(streamPath->ParentPathId);
        }
    }

    // ... rest of function
}
```

4. **Added new checker** (`schemeshard_path.cpp`):
```cpp
const TPath::TChecker& TPath::TChecker::NotUnderPendingCleanup(EStatus status) const {
    if (Failed) {
        return *this;
    }

    if (!Path.SS->TablesWithPendingCleanup.contains(Path.Base()->PathId)) {
        return *this;
    }

    return Fail(status, TStringBuilder() << "path has pending CDC stream cleanup"
        << " (" << BasicPathInfo(Path.Base()) << ")");
}
```

5. **Updated CDC operations** to check pending cleanup:
   - `schemeshard__operation_create_cdc_stream.cpp` - `RejectOnTablePathChecks()`
   - `schemeshard__operation_rotate_cdc_stream.cpp` - 3 table check locations
   - `schemeshard__operation_alter_cdc_stream.cpp` - 3 table check locations

### How the Fix Eliminates the Race

```
Before (race condition):
T0: TryStartCleaner called (table FREE)
T1: [756ms gap] ← User operation can slip in here
T2: DropCdcStream proposal locks table
T3: User operation conflicts at DataShard

After (no race):
T0: TryStartCleaner called
    - Immediately adds table to TablesWithPendingCleanup
T1: User operation checks TablesWithPendingCleanup
    - Sees pending cleanup → rejected with StatusMultipleModifications
T2: DropCdcStream runs normally
T3: Cleanup completes, removes from TablesWithPendingCleanup
T4: User operation retries → succeeds
```

### Files Modified

| File | Changes |
|------|---------|
| `schemeshard_impl.h` | Added `TablesWithPendingCleanup` set |
| `schemeshard_backup_incremental__progress.cpp` | Add to set in TryStartCleaner/TryStartOrphanCleaner, remove in OnCleanerResult |
| `schemeshard_path.h` | Added `NotUnderPendingCleanup()` declaration |
| `schemeshard_path.cpp` | Added `NotUnderPendingCleanup()` implementation |
| `schemeshard__operation_create_cdc_stream.cpp` | Added `.NotUnderPendingCleanup()` check |
| `schemeshard__operation_rotate_cdc_stream.cpp` | Added `.NotUnderPendingCleanup()` check (3 places) |
| `schemeshard__operation_alter_cdc_stream.cpp` | Added `.NotUnderPendingCleanup()` check (3 places) |

### Behavior Notes

1. **User operations get StatusMultipleModifications** when cleanup is pending - this is a retriable error
2. **Cleanup retries on its own** if it encounters conflicts (10 second delay)
3. **Set is cleared on both success and failure** in OnCleanerResult to allow user operations after cleanup attempt
4. **On SchemeShard restart**, Resume() re-adds tables with items in Dropping state to the set

## References

- Log files: `log13.log`, `log14.log`, `log15.log`, `log16.log`
- Error: `StatusMultipleModifications` / `OVERLOADED`
- DataShard error: `Previous Tx must be in a state where it only waits for Ack`
- Source locations:
  - `schemeshard__operation_create_cdc_stream.cpp:836`
  - `schemeshard_backup_incremental__progress.cpp:55-58`
