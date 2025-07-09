# Incremental Restore Progress Tracking Plan

This document outlines the implementation plan for adding progress tracking to the incremental restore functionality in SchemeShard, based on patterns from the build_index implementation.

## 1. State Management for TIncrementalRestoreContext

Add state tracking to the context structure:

```cpp
struct TIncrementalRestoreContext {
    TPathId DestinationTablePathId;
    TString DestinationTablePath;
    ui64 OriginalOperationId;
    TPathId BackupCollectionPathId;
    
    // New fields for progress tracking
    enum EState {
        Invalid,
        Allocating,
        Proposing,
        Waiting,
        Applying,
        Done,
        Failed
    };
    
    EState State = Invalid;
    THashSet<TShardIdx> InProgressShards;
    THashSet<TShardIdx> DoneShards;
    TVector<TShardIdx> ToProcessShards;
    
    // Track individual incremental backup progress
    THashMap<TPathId, bool> IncrementalBackupStatus; // PathId -> Completed
    
    // Tracking and transaction management
    TTxId CurrentTxId = InvalidTxId;
    
    bool AllIncrementsProcessed() const {
        for (const auto& [pathId, completed] : IncrementalBackupStatus) {
            if (!completed) return false;
        }
        return !IncrementalBackupStatus.empty();
    }
};
```

## 2. Progress Helper Function

Add a function to trigger progress updates:

```cpp
// Add to schemeshard_impl.h
void ProgressIncrementalRestore(ui64 operationId);

// Implementation in schemeshard_incremental_restore_scan.cpp
void TSchemeShard::ProgressIncrementalRestore(ui64 operationId) {
    auto ctx = ActorContext();
    ctx.Send(SelfId(), new TEvPrivate::TEvProgressIncrementalRestore(operationId));
}
```

## 3. New Event for Progress Updates

Add a new event type:

```cpp
// In schemeshard__events.h
struct TEvProgressIncrementalRestore : public TEventLocal<TEvProgressIncrementalRestore, EvProgressIncrementalRestore> {
    ui64 OperationId;
    
    explicit TEvProgressIncrementalRestore(ui64 operationId)
        : OperationId(operationId)
    {}
};
```

## 4. State Machine in TTxProgress

Implement a state machine in the `TTxProgress` class:

```cpp
bool TTxProgress::Execute(TTransactionContext& txc, const TActorContext& ctx) {
    if (ProgressIncrementalRestore) {
        return OnProgressIncrementalRestore(txc, ctx);
    }
    // ... existing conditions
}

bool TTxProgress::OnProgressIncrementalRestore(TTransactionContext& txc, const TActorContext& ctx) {
    const ui64 operationId = ProgressIncrementalRestore->Get()->OperationId;
    
    if (!Self->IncrementalRestoreContexts.contains(operationId)) {
        LOG_W("Progress event for unknown operation: " << operationId);
        return true;
    }
    
    auto& context = Self->IncrementalRestoreContexts[operationId];
    
    switch (context.State) {
        case TIncrementalRestoreContext::Invalid:
            return HandleInvalidState(txc, ctx, operationId, context);
            
        case TIncrementalRestoreContext::Allocating:
            return HandleAllocatingState(txc, ctx, operationId, context);
            
        case TIncrementalRestoreContext::Proposing:
            return HandleProposingState(txc, ctx, operationId, context);
            
        case TIncrementalRestoreContext::Waiting:
            return HandleWaitingState(txc, ctx, operationId, context);
            
        case TIncrementalRestoreContext::Applying:
            return HandleApplyingState(txc, ctx, operationId, context);
            
        case TIncrementalRestoreContext::Done:
        case TIncrementalRestoreContext::Failed:
            return HandleFinalState(txc, ctx, operationId, context);
    }
    
    return true;
}
```

## 5. Shard Progress Tracking

Track progress per shard:

```cpp
bool TTxProgress::HandleWaitingState(TTransactionContext& txc, const TActorContext& ctx, 
                                    ui64 operationId, TIncrementalRestoreContext& context) {
    NIceDb::TNiceDb db(txc.DB);
    
    // Check if all shards completed
    if (context.InProgressShards.empty() && context.ToProcessShards.empty()) {
        if (context.AllIncrementsProcessed()) {
            // All done, move to applying state
            context.State = TIncrementalRestoreContext::Applying;
            db.Table<Schema::IncrementalRestoreState>()
                .Key(operationId)
                .Update<Schema::IncrementalRestoreState::State>(context.State);
            
            Self->ProgressIncrementalRestore(operationId);
            return true;
        }
        
        // Start next incremental backup
        StartNextIncrementalBackup(txc, ctx, operationId, context);
    }
    
    // Send work to shards
    const size_t MaxInProgressShards = 10; // Configure appropriate limit
    while (!context.ToProcessShards.empty() && 
           context.InProgressShards.size() < MaxInProgressShards) {
        auto shardIdx = context.ToProcessShards.back();
        context.ToProcessShards.pop_back();
        context.InProgressShards.insert(shardIdx);
        
        SendRestoreRequestToShard(ctx, operationId, shardIdx, context);
    }
    
    return true;
}
```

## 6. Shard Response Handling

Add a transaction to handle shard responses:

```cpp
// New transaction type for shard responses
struct TTxShardResponse : public TTxBase {
    TEvDataShard::TEvIncrementalRestoreResponse::TPtr Response;
    
    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        auto& record = Response->Get()->Record;
        ui64 operationId = record.GetOperationId();
        TShardIdx shardIdx = Self->GetShardIdx(TTabletId(record.GetTabletId()));
        
        if (!Self->IncrementalRestoreContexts.contains(operationId)) {
            return true;
        }
        
        auto& context = Self->IncrementalRestoreContexts[operationId];
        NIceDb::TNiceDb db(txc.DB);
        
        switch (record.GetStatus()) {
            case NKikimrIndexBuilder::EBuildStatus::DONE:
                context.InProgressShards.erase(shardIdx);
                context.DoneShards.insert(shardIdx);
                
                // Persist shard progress
                db.Table<Schema::IncrementalRestoreShardProgress>()
                    .Key(operationId, shardIdx)
                    .Update<Schema::IncrementalRestoreShardProgress::Status>(DONE);
                
                // Trigger next progress
                Self->ProgressIncrementalRestore(operationId);
                break;
                
            case NKikimrIndexBuilder::EBuildStatus::ABORTED:
                // Retry shard
                context.InProgressShards.erase(shardIdx);
                context.ToProcessShards.push_back(shardIdx);
                Self->ProgressIncrementalRestore(operationId);
                break;
                
            case NKikimrIndexBuilder::EBuildStatus::BUILD_ERROR:
                // Handle error
                context.State = TIncrementalRestoreContext::Failed;
                db.Table<Schema::IncrementalRestoreState>()
                    .Key(operationId)
                    .Update<Schema::IncrementalRestoreState::State>(context.State);
                break;
        }
        
        return true;
    }
};
```

## 7. Persistence Schema

Add schema tables to persist state:

```cpp
// In schemeshard__init.h
struct IncrementalRestoreState : Table<128> {
    struct OperationId : Column<1, NScheme::NTypeIds::Uint64> {};
    struct State : Column<2, NScheme::NTypeIds::Uint32> {};
    struct CurrentIncrementalIdx : Column<3, NScheme::NTypeIds::Uint32> {};
    
    using TKey = TableKey<OperationId>;
    using TColumns = TableColumns<OperationId, State, CurrentIncrementalIdx>;
};

struct IncrementalRestoreShardProgress : Table<129> {
    struct OperationId : Column<1, NScheme::NTypeIds::Uint64> {};
    struct ShardIdx : Column<2, NScheme::NTypeIds::Uint64> {};
    struct Status : Column<3, NScheme::NTypeIds::Uint32> {};
    struct LastKey : Column<4, NScheme::NTypeIds::String> {};
    
    using TKey = TableKey<OperationId, ShardIdx>;
    using TColumns = TableColumns<OperationId, ShardIdx, Status, LastKey>;
};
```

## 8. Operation Initialization and Cleanup

Initialize the context properly:

```cpp
bool TTxProgress::OnRunIncrementalRestore(TTransactionContext& txc, const TActorContext& ctx) {
    // ... existing code to find operation
    
    // Initialize context with proper state
    TSchemeShard::TIncrementalRestoreContext context;
    context.DestinationTablePathId = tablePathId;
    context.DestinationTablePath = tablePath.PathString();
    context.OriginalOperationId = ui64(operationId.GetTxId());
    context.BackupCollectionPathId = pathId;
    context.State = TIncrementalRestoreContext::Allocating;
    
    // Collect all incremental backups
    for (const auto& entry : incrementalBackupEntries) {
        context.IncrementalBackupStatus[entry.second] = false;
    }
    
    // Generate a new unique operation ID
    const ui64 newOperationId = Self->NextIncrementalRestoreId++;
    Self->IncrementalRestoreContexts[newOperationId] = context;
    
    // Persist initial state
    NIceDb::TNiceDb db(txc.DB);
    db.Table<Schema::IncrementalRestoreState>()
        .Key(newOperationId)
        .Update<Schema::IncrementalRestoreState::State>(context.State);
    
    // Request transaction allocation
    ctx.Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate(), 0, newOperationId);
    
    return true;
}
```

## 9. Event Handler Registration

Wire up the event handlers:

```cpp
// In schemeshard_impl.h
void Handle(TEvPrivate::TEvProgressIncrementalRestore::TPtr& ev, const TActorContext& ctx);
void Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx);

// In schemeshard.cpp
void TSchemeShard::Handle(TEvPrivate::TEvProgressIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxProgressIncrementalRestore(ev), ctx);
}

void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxIncrementalRestoreShardResponse(ev), ctx);
}
```

## 10. Cleanup on Completion

Properly clean up resources when an operation completes:

```cpp
bool TTxProgress::HandleFinalState(TTransactionContext& txc, const TActorContext& ctx, 
                                  ui64 operationId, const TIncrementalRestoreContext& context) {
    NIceDb::TNiceDb db(txc.DB);
    
    // Clean up persistent state
    db.Table<Schema::IncrementalRestoreState>()
        .Key(operationId)
        .Delete();
    
    // Clean up shard progress
    for (const auto& shardIdx : context.DoneShards) {
        db.Table<Schema::IncrementalRestoreShardProgress>()
            .Key(operationId, shardIdx)
            .Delete();
    }
    
    // Remove from memory
    Self->IncrementalRestoreContexts.erase(operationId);
    Self->TxIdToIncrementalRestore.erase(context.CurrentTxId);
    
    // Notify completion to original operation
    if (context.State == TIncrementalRestoreContext::Done) {
        LOG_I("Incremental restore completed successfully: " << operationId);
    } else {
        LOG_E("Incremental restore failed: " << operationId);
    }
    
    return true;
}
```

## Implementation Notes

1. The implementation follows similar patterns to the build_index subsystem to maintain consistency
2. All state is persisted to survive tablet restarts
3. Tracking happens at both the operation level and individual shard level
4. Each incremental backup is processed sequentially, but within each backup, shards can be processed in parallel
5. State transitions follow a clear pattern: Allocating → Proposing → Waiting → Applying → Done/Failed
6. Need to add appropriate maps in TSchemeShard to track operations by ID and transaction ID

## Required Changes

1. Add new event types in schemeshard__events.h
2. Update TIncrementalRestoreContext in schemeshard_impl.h
3. Add persistence schema in schemeshard__init.h
4. Update TTxProgress in schemeshard_incremental_restore_scan.cpp
5. Add event handlers in schemeshard.cpp
6. Add tracking maps and cleanup in TSchemeShard
7. Implement the state machine and handlers
8. Add DataShard response handling logic
