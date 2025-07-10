#include "schemeshard_impl.h"
#include "schemeshard_utils.h"

#include <ydb/core/tx/tx_proxy/proxy.h>

#if defined LOG_D || \
    defined LOG_W || \
    defined LOG_N || \
    defined LOG_I || \
    defined LOG_E
#error log macro redefinition
#endif

#define LOG_D(stream) LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[IncrementalRestore] " << stream)
#define LOG_I(stream) LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[IncrementalRestore] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[IncrementalRestore] " << stream)
#define LOG_W(stream) LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[IncrementalRestore] " << stream)
#define LOG_E(stream) LOG_ERROR_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[IncrementalRestore] " << stream)

namespace NKikimr::NSchemeShard {

// Enhanced TTxProgressIncrementalRestore implementation with state machine
class TSchemeShard::TTxProgressIncrementalRestore : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
public:
    TTxProgressIncrementalRestore(TSchemeShard* self, ui64 operationId)
        : TBase(self)
        , OperationId(operationId)
    {}

    bool Execute(NTabletFlatExecutor::TTransactionContext&, const TActorContext& ctx) override {
        LOG_I("TTxProgressIncrementalRestore::Execute"
            << " operationId: " << OperationId
            << " tablet: " << Self->TabletID());

        // Find the incremental restore context for this operation
        auto contextIt = Self->IncrementalRestoreContexts.find(OperationId);
        if (contextIt == Self->IncrementalRestoreContexts.end()) {
            LOG_W("No incremental restore context found for operation: " << OperationId);
            return true;
        }

        auto& context = contextIt->second;
        
        // State machine logic
        switch (context.State) {
            case TIncrementalRestoreContext::EState::Invalid:
                LOG_E("Invalid state for operation: " << OperationId);
                return true;
            case TIncrementalRestoreContext::EState::Allocating:
                return HandleAllocatingState(context, ctx);
            case TIncrementalRestoreContext::EState::Proposing:
                // For now, move directly to Applying state
                context.State = TIncrementalRestoreContext::EState::Applying;
                return HandleApplyingState(context, ctx);
            case TIncrementalRestoreContext::EState::Applying:
                return HandleApplyingState(context, ctx);
            case TIncrementalRestoreContext::EState::Waiting:
                return HandleWaitingState(context, ctx);
            case TIncrementalRestoreContext::EState::NextIncremental:
                return HandleNextIncrementalState(context, ctx);
            case TIncrementalRestoreContext::EState::Done:
                return HandleDoneState(context, ctx);
            case TIncrementalRestoreContext::EState::Failed:
                LOG_E("Failed state for operation: " << OperationId);
                return true;
        }
        
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        LOG_I("TTxProgressIncrementalRestore::Complete"
            << " operationId: " << OperationId);
    }

private:
    ui64 OperationId;
    
    bool HandleAllocatingState(TIncrementalRestoreContext& context, const TActorContext& ctx) {
        LOG_I("HandleAllocatingState for operation: " << OperationId);
        
        const auto* currentIncremental = context.GetCurrentIncremental();
        if (!currentIncremental) {
            LOG_I("No more incremental backups to process, moving to Done state");
            context.State = TIncrementalRestoreContext::EState::Done;
            return true;
        }
        
        LOG_I("Starting incremental backup #" << context.CurrentIncrementalIdx + 1 
            << " path: " << currentIncremental->BackupPath
            << " timestamp: " << currentIncremental->Timestamp);
        
        // Move to applying state
        context.State = TIncrementalRestoreContext::EState::Applying;
        return true;
    }
    
    bool HandleApplyingState(TIncrementalRestoreContext& context, const TActorContext& ctx) {
        LOG_I("HandleApplyingState for operation: " << OperationId);
        
        // For now, just move to waiting state
        // In a full implementation, we would send requests to DataShards here
        context.State = TIncrementalRestoreContext::EState::Waiting;
        return true;
    }
    
    bool HandleWaitingState(TIncrementalRestoreContext& context, const TActorContext& ctx) {
        LOG_I("HandleWaitingState for operation: " << OperationId);
        
        // Check if current incremental is complete
        if (context.IsCurrentIncrementalComplete()) {
            LOG_I("Current incremental backup completed, moving to next");
            context.State = TIncrementalRestoreContext::EState::NextIncremental;
        }
        
        return true;
    }
    
    bool HandleNextIncrementalState(TIncrementalRestoreContext& context, const TActorContext& ctx) {
        LOG_I("HandleNextIncrementalState for operation: " << OperationId);
        
        // Mark current incremental as completed and move to next
        if (context.CurrentIncrementalIdx < context.IncrementalBackups.size()) {
            context.IncrementalBackups[context.CurrentIncrementalIdx].Completed = true;
        }
        
        context.MoveToNextIncremental();
        
        if (context.AllIncrementsProcessed()) {
            LOG_I("All incremental backups processed, moving to Done state");
            context.State = TIncrementalRestoreContext::EState::Done;
        } else {
            LOG_I("Moving to next incremental backup");
            context.State = TIncrementalRestoreContext::EState::Allocating;
        }
        
        return true;
    }
    
    bool HandleDoneState(TIncrementalRestoreContext& context, const TActorContext& ctx) {
        Y_UNUSED(context); // Suppress unused parameter warning
        LOG_I("HandleDoneState for operation: " << OperationId);
        
        // Clean up context
        Self->IncrementalRestoreContexts.erase(OperationId);
        
        return true;
    }
};

// Enhanced handler for TEvRunIncrementalRestore
void TSchemeShard::Handle(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    const auto& backupCollectionPathId = ev->Get()->BackupCollectionPathId;
    
    LOG_I("Handle(TEvRunIncrementalRestore)"
        << " backupCollectionPathId: " << backupCollectionPathId
        << " tablet: " << TabletID());

    // Create a new incremental restore context
    ui64 operationId = backupCollectionPathId.LocalPathId;
    auto& context = IncrementalRestoreContexts[operationId];
    
    // TODO: Load incremental backup list from backup collection metadata
    // For now, create a dummy incremental backup for testing
    context.AddIncrementalBackup(backupCollectionPathId, "test_backup_path", 1000);
    
    LOG_I("Created incremental restore context with " << context.IncrementalBackups.size() << " backups");
    
    // Start the state machine
    Execute(new TTxProgressIncrementalRestore(this, operationId), ctx);
}

// Enhanced handler for TEvProgressIncrementalRestore  
void TSchemeShard::Handle(TEvPrivate::TEvProgressIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    ui64 operationId = ev->Get()->OperationId;
    
    LOG_I("Handle(TEvProgressIncrementalRestore)"
        << " operationId: " << operationId
        << " tablet: " << TabletID());

    // Execute progress transaction
    Execute(new TTxProgressIncrementalRestore(this, operationId), ctx);
}

// Enhanced handler for DataShard response
void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx) {
    const auto& record = ev->Get()->Record;
    
    LOG_I("Handle(TEvIncrementalRestoreResponse)"
        << " operationId: " << record.GetOperationId()
        << " shardIdx: " << record.GetShardIdx()
        << " incrementalIdx: " << record.GetIncrementalIdx()
        << " status: " << (int)record.GetRestoreStatus()
        << " tablet: " << TabletID());

    // Update context with shard completion
    auto contextIt = IncrementalRestoreContexts.find(record.GetOperationId());
    if (contextIt != IncrementalRestoreContexts.end()) {
        auto& context = contextIt->second;
        
        // Track shard completion for current incremental backup
        if (record.GetIncrementalIdx() == context.CurrentIncrementalIdx) {
            ui64 shardIdx = record.GetShardIdx();
            context.InProgressShards.erase(shardIdx);
            context.DoneShards.insert(shardIdx);
            
            LOG_I("Shard " << shardIdx << " completed incremental #" << record.GetIncrementalIdx()
                << " (" << context.DoneShards.size() << "/" << (context.DoneShards.size() + context.InProgressShards.size()) << " done)");
            
            // Check if all shards are done for current incremental
            if (context.InProgressShards.empty()) {
                LOG_I("All shards completed for incremental #" << record.GetIncrementalIdx());
                if (context.CurrentIncrementalIdx < context.IncrementalBackups.size()) {
                    context.IncrementalBackupStatus[context.IncrementalBackups[context.CurrentIncrementalIdx].BackupPathId] = true;
                }
            }
        } else {
            LOG_W("Received response for incremental #" << record.GetIncrementalIdx() 
                << " but currently processing #" << context.CurrentIncrementalIdx);
        }
    }

    // Send progress update
    auto progressEvent = MakeHolder<TEvPrivate::TEvProgressIncrementalRestore>(record.GetOperationId());
    Send(SelfId(), progressEvent.Release());
}

// Helper function to create TTxProgressIncrementalRestore
NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(ui64 operationId) {
    return new TTxProgressIncrementalRestore(this, operationId);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev) {
    return new TTxProgressIncrementalRestore(this, ev->Get()->BackupCollectionPathId.LocalPathId);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvPrivate::TEvProgressIncrementalRestore::TPtr& ev) {
    return new TTxProgressIncrementalRestore(this, ev->Get()->OperationId);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvTxAllocatorClient::TEvAllocateResult::TPtr& ev) {
    // For simplified implementation, use the first TxId if available
    const auto& txIds = ev->Get()->TxIds;
    ui64 operationId = txIds.empty() ? 0 : txIds[0];
    return new TTxProgressIncrementalRestore(this, operationId);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr& ev) {
    // For simplified implementation, use TxId from the event
    ui64 operationId = ev->Get()->Record.GetTxId();
    return new TTxProgressIncrementalRestore(this, operationId);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TTxId txId) {
    // For simplified implementation, convert TTxId to ui64
    ui64 operationId = ui64(txId);
    return new TTxProgressIncrementalRestore(this, operationId);
}

} // namespace NKikimr::NSchemeShard
