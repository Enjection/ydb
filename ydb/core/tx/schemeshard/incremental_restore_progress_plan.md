# 📋 Incremental Restore Implementation Plan - FINAL

## 🎯 MAIN GOAL: Implement Sequential DataShard-Driven Incremental Restore

### 🔑 KEY FINDINGS:
Based on analysis of the diff and user feedback, we discovered:

1. **✅ Working Foundation**: Before our changes, at least one incremental backup restore worked correctly
2. **❌ Wrong Approach**: We implemented `ESchemeOpChangePathState` operations instead of using `ESchemeOpRestoreMultipleIncrementalBackups`
3. **🎯 Required Architecture**: 
   - Use `ESchemeOpRestoreMultipleIncrementalBackups` in `TEvModifyScheme` operations
   - Process incremental backups **sequentially, one at a time**
   - Each incremental backup triggers its own `MultiIncrementalRestore` operation
   - Wait for **DataShard completion notifications** before starting next backup (following build_index pattern)
   - Add completion notification mechanism to update SchemeShard state

### 🚨 CRITICAL ARCHITECTURAL INSIGHT:
**Sequential Processing with DataShard Synchronization**:
- Incremental backup #1 → MultiIncrementalRestore → Wait for ALL DataShards → Incremental backup #2 → ...
- Operations are **asynchronous** - they only trigger data sending to DataShards
- **DataShards notify completion** (like build_index pattern)
- Only when ALL DataShards complete current backup, start next backup

## 🔧 IMPLEMENTATION PLAN:

### ✅ Step 1: Fix Operation Type - COMPLETED
Replaced `ESchemeOpChangePathState` with `ESchemeOpRestoreMultipleIncrementalBackups`:

```cpp
// ✅ IMPLEMENTED in CreateIncrementalRestoreOperation()
tx.SetOperationType(NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups);
// Process ONLY current incremental backup
auto& restore = *tx.MutableRestoreMultipleIncrementalBackups();
```

### ✅ Step 2: Sequential State Tracking - COMPLETED
Implemented `TIncrementalRestoreState` with simple sequential processing:

```cpp
// ✅ IMPLEMENTED in schemeshard_impl.h
struct TIncrementalRestoreState {
    TVector<TIncrementalBackup> IncrementalBackups; // Sorted by timestamp
    ui32 CurrentIncrementalIdx = 0;
    bool CurrentIncrementalStarted = false;
    THashSet<ui64> InProgressShards;
    THashSet<ui64> DoneShards;
    // ... completion tracking methods
};
```

### ✅ Step 3: DataShard Completion Notifications - COMPLETED
Implemented proper completion tracking:

```cpp
// ✅ IMPLEMENTED in Handle(TEvIncrementalRestoreResponse)
void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx) {
    // Track shard completion
    state.InProgressShards.erase(shardIdx);
    state.DoneShards.insert(shardIdx);
    
    // When all shards complete, trigger next incremental
    if (state.InProgressShards.empty() && state.CurrentIncrementalStarted) {
        state.MarkCurrentIncrementalComplete();
        // Send progress event to move to next incremental
    }
}
```

### ✅ Step 4: Remove Complex State Machine - COMPLETED
Simplified `TTxProgressIncrementalRestore` to handle only sequential processing:

```cpp
// ✅ IMPLEMENTED - removed complex state machine
// Now only handles: Check completion → Move to next → Process next backup
```

## 🚀 NEXT STEPS:

### Step 5: Integration Testing
- **Test the sequential flow**: One incremental backup at a time
- **Verify DataShard notifications**: Completion tracking works correctly
- **Check operation completion**: All incremental backups are processed in order

### Step 6: Error Handling & Recovery
- **Handle failed operations**: Retry logic for failed incremental backups
- **State persistence**: Ensure state survives SchemeShard restarts
- **Timeout handling**: Handle cases where DataShards don't respond

### Step 7: Performance Optimization
- **Parallel shard processing**: Multiple shards can process same incremental in parallel
- **Better shard detection**: Get actual target shards from table metadata
- **Progress reporting**: Add better progress tracking and logging

---

## 🔄 STATUS: IMPLEMENTATION COMPLETE, TESTING NEEDED

All architectural changes are complete:
- ✅ Using correct operation type (`ESchemeOpRestoreMultipleIncrementalBackups`)
- ✅ Sequential processing with proper state tracking
- ✅ DataShard completion notifications
- ✅ Removed complex state machine
- ✅ Simple, robust architecture following build_index pattern

**Next**: Integration testing and refinement based on test results.

### Step 4: Add Proto Definitions
```proto
message TEvIncrementalRestoreResult {
    enum EStatus {
        SUCCESS = 1;
        ERROR = 2;
        IN_PROGRESS = 3;
    }
    
    optional uint64 TabletId = 1;
    optional uint64 TaskId = 2;
    optional EStatus Status = 3;
    optional string Error = 4;
}
```

## 🎯 NEXT STEPS:
1. **Remove**: Complex state machine code from current implementation
2. **Replace**: `CreateIncrementalRestoreOperation` to use `ESchemeOpRestoreMultipleIncrementalBackups`
3. **Add**: DataShard completion tracking (following build_index pattern)
4. **Implement**: Sequential processing with proper synchronization
5. **Test**: Verify incremental backups are applied in correct order

