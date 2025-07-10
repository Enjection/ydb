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

### Step 1: Fix Operation Type
Replace `ESchemeOpChangePathState` with `ESchemeOpRestoreMultipleIncrementalBackups`:

```cpp
void TSchemeShard::CreateIncrementalRestoreOperation(
    const TPathId& backupCollectionPathId,
    const TOperationId& operationId, 
    const TString& backupName,
    const TActorContext& ctx) {
    
    auto request = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>();
    auto& record = request->Record;
    
    record.SetTxId(ui64(GetCachedTxId(ctx)));
    
    auto& tx = *record.AddTransaction();
    tx.SetOperationType(NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups);
    tx.SetInternal(true);
    
    // Process ONLY current incremental backup
    auto& restore = *tx.MutableRestoreMultipleIncrementalBackups();
    // Add current backup path for each table
    
    Send(SelfId(), request.Release());
}
```

### Step 2: Implement Sequential State Tracking
Track which incremental backup is currently being processed:

```cpp
struct TIncrementalRestoreState {
    TOperationId OperationId;
    TPathId BackupCollectionPathId;
    TVector<TString> IncrementalBackupNames;
    ui32 CurrentIndex = 0;
    TOperationId CurrentRestoreOpId;
    
    // Track DataShard progress (following build_index pattern)
    THashSet<TShardIdx> ShardsInProgress;
    THashSet<TShardIdx> CompletedShards;
    THashMap<TShardIdx, TString> ShardErrors;
};
```

### Step 3: Handle DataShard Completion Notifications
Following build_index pattern, wait for DataShard completion before starting next backup:

```cpp
void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResult::TPtr& ev, const TActorContext& ctx) {
    // Track shard completion
    // When all shards complete current backup, start next backup
    // If any shard fails, handle error appropriately
}

void TSchemeShard::OnCurrentIncrementalRestoreComplete(const TOperationId& operationId, const TActorContext& ctx) {
    // Move to next incremental backup
    // If all backups processed, mark operation complete
}
```

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

