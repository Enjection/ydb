# 📋 Incremental Restore Implementation Plan - VERIFIED AND ALIGNED ✅

## 🎯 MAIN GOAL: Fix DataShard Completion Notifications for Sequential Incremental Restore

### 🔑 KEY FINDINGS - VERIFIED ✅:
Based on analysis of the diff, test failures, and code investigation:

1. **✅ Working Foundation**: `MultiIncrementalRestore` operations already exist and work
2. **✅ Existing Mechanism**: DataShard already has `IncrementalRestoreScan` implementation
3. **❌ Missing Piece**: DataShard doesn't notify SchemeShard when scan completes
4. **🎯 Required Fix**: 
   - DataShard's `IncrementalRestoreScan` needs to send completion notification
   - SchemeShard already has handler for `TEvIncrementalRestoreResponse`
   - Track completion of source DataShards (backup scanners), not target DataShards (appliers)
   - Use DataShard approach for fine-grained progress tracking

### ✅ **PLAN VERIFICATION STATUS**: 
**PERFECTLY ALIGNED** with corrected understanding - both plans identify the same root cause, solution approach, and implementation location.

### 🚨 CRITICAL ARCHITECTURAL INSIGHT - CONFIRMED ✅:
**DataShard-Driven Completion with IncrementalRestoreScan**:
- SchemeShard sends `MultiIncrementalRestore` scheme operation to **source DataShards** (backup tables)
- Source DataShards start `IncrementalRestoreScan` actor to scan backup data
- Scan actor sends data to **target DataShards** for application
- **Missing piece**: When scan completes, source DataShard needs to notify SchemeShard
- SchemeShard tracks when ALL source DataShards complete scanning before starting next incremental

### 🔍 CURRENT FLOW ANALYSIS - VERIFIED ✅:
```
✅ SchemeShard creates MultiIncrementalRestore operation
✅ DataShard receives scheme operation
✅ DataShard starts IncrementalRestoreScan actor (already implemented)
✅ Scan reads backup data and sends to target DataShards
❌ Scan completes but doesn't notify SchemeShard (IDENTIFIED FIX LOCATION)
❌ SchemeShard never knows when to start next incremental backup
```

**CODE EVIDENCE - COMPLETION POINT FOUND**:
```cpp
// In incr_restore_scan.cpp - Finish() method is the completion point
TAutoPtr<IDestructable> Finish(EStatus status) override {
    LOG_D("Finish " << status);
    if (status != EStatus::Done) {
        // TODO: https://github.com/ydb-platform/ydb/issues/18797
    }
    Send(Parent, new TEvIncrementalRestoreScan::TEvFinished(TxId));
    PassAway();
    return nullptr;
}
```

## 🔧 IMPLEMENTATION PLAN:

### ✅ Step 1: Foundation Already Working - VERIFIED
The existing implementation correctly:
- Uses `ESchemeOpRestoreMultipleIncrementalBackups` operations  
- Creates `MultiIncrementalRestore` scheme operations sent to DataShards
- DataShards start `IncrementalRestoreScan` actors (verified in `incr_restore_scan.cpp`)
- SchemeShard has handler for `TEvIncrementalRestoreResponse`

### ❌ Step 2: Missing DataShard Completion Notification - NEEDS IMPLEMENTATION
**Problem**: `IncrementalRestoreScan` completes but doesn't notify SchemeShard

**Solution**: Add completion notification in `incr_restore_scan.cpp`:
```cpp
// In TIncrementalRestoreScan::Complete() or similar completion method
void NotifySchemeShard() {
    auto response = MakeHolder<TEvDataShard::TEvIncrementalRestoreResponse>();
    response->Record.SetTabletId(DataShard->TabletID());
    response->Record.SetStatus(Success ? SUCCESS : ERROR);
    response->Record.SetTxId(TxId);
    response->Record.SetTableId(TableId);
    
    Send(SchemeShardId, response.Release());
}
```

### ✅ Step 3: SchemeShard Completion Tracking - ALREADY IMPLEMENTED
The existing handler correctly tracks when all source DataShards complete:
```cpp
// In Handle(TEvIncrementalRestoreResponse)
state.InProgressShards.erase(shardIdx);
state.DoneShards.insert(shardIdx);

if (state.InProgressShards.empty() && state.CurrentIncrementalStarted) {
    // All source DataShards done scanning, move to next incremental
    state.MarkCurrentIncrementalComplete();
}
```

## 🚀 IMMEDIATE NEXT STEPS:

### Step 4: Find and Fix IncrementalRestoreScan Completion
**Action**: Locate the completion point in `incr_restore_scan.cpp` and add SchemeShard notification:

1. **Study `TIncrementalRestoreScan` class** - understand its lifecycle and completion methods
2. **Find completion points** - both success and error cases where scan finishes
3. **Add notification code** - send `TEvIncrementalRestoreResponse` to SchemeShard
4. **Ensure SchemeShard ActorId** is available to the scan actor for notification

### Step 5: Debug and Verify Flow
**Action**: Add comprehensive logging to track the complete flow:

1. **DataShard side**: Log when scan starts and completes
2. **SchemeShard side**: Log when responses are received and next incremental starts  
3. **Test with multiple incrementals** - verify sequential processing works correctly

### Step 6: Handle Edge Cases
**Action**: Robust error handling and edge case management:

1. **Scan failures**: Proper error reporting from DataShard to SchemeShard
2. **Timeout handling**: What if DataShard doesn't respond
3. **State persistence**: Ensure state survives restarts
4. **Retry logic**: Handle transient failures appropriately

---

## 🎯 TARGET OUTCOME:
With this fix, the incremental restore will work as follows:
1. ✅ Start first incremental backup restore → DataShards scan backup #1
2. ✅ Wait for ALL source DataShards to complete scanning  
3. ✅ Start second incremental backup restore → DataShards scan backup #2
4. ✅ Continue until all incremental backups are processed sequentially
5. ✅ Test shows updated values and deleted rows from all incremental backups

