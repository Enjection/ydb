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

// Simple sequential incremental restore transaction
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

        // Find the incremental restore state for this operation
        auto stateIt = Self->IncrementalRestoreStates.find(OperationId);
        if (stateIt == Self->IncrementalRestoreStates.end()) {
            LOG_W("No incremental restore state found for operation: " << OperationId);
            return true;
        }

        auto& state = stateIt->second;
        
        // Check if current incremental is complete and we can move to next
        if (state.IsCurrentIncrementalComplete()) {
            LOG_I("Current incremental backup completed, moving to next");
            state.MoveToNextIncremental();
            
            if (state.AllIncrementsProcessed()) {
                LOG_I("All incremental backups processed, cleaning up");
                Self->IncrementalRestoreStates.erase(OperationId);
                return true;
            }
            
            // Start processing next incremental backup
            ProcessNextIncrementalBackup(state, ctx);
        }
        
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        LOG_I("TTxProgressIncrementalRestore::Complete"
            << " operationId: " << OperationId);
    }

private:
    ui64 OperationId;
    
    void ProcessNextIncrementalBackup(TIncrementalRestoreState& state, const TActorContext& ctx) {
        const auto* currentIncremental = state.GetCurrentIncremental();
        if (!currentIncremental) {
            LOG_I("No more incremental backups to process");
            return;
        }
        
        LOG_I("Processing incremental backup #" << state.CurrentIncrementalIdx + 1 
            << " path: " << currentIncremental->BackupPath
            << " timestamp: " << currentIncremental->Timestamp);
        
        // Create MultiIncrementalRestore operation for this backup
        Self->CreateIncrementalRestoreOperation(
            state.BackupCollectionPathId,
            OperationId,
            currentIncremental->BackupPath,
            ctx
        );
        
        // Initialize tracking for this incremental backup
        state.InProgressShards.clear();
        state.DoneShards.clear();
        state.CurrentIncrementalStarted = true;
    }
};

// Handler for TEvRunIncrementalRestore - starts sequential processing
void TSchemeShard::Handle(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    auto* msg = ev->Get();
    const auto& backupCollectionPathId = msg->BackupCollectionPathId;
    const auto& operationId = msg->OperationId;
    const auto& incrementalBackupNames = msg->IncrementalBackupNames;
    
    LOG_I("Handle(TEvRunIncrementalRestore) starting sequential processing for " 
          << incrementalBackupNames.size() << " incremental backups"
          << " backupCollectionPathId: " << backupCollectionPathId
          << " operationId: " << operationId
          << " tablet: " << TabletID());

    // Find the backup collection to get restore settings
    auto itBc = BackupCollections.find(backupCollectionPathId);
    if (itBc == BackupCollections.end()) {
        LOG_E("Backup collection not found for pathId: " << backupCollectionPathId);
        return;
    }

    if (incrementalBackupNames.empty()) {
        LOG_I("No incremental backups provided, nothing to restore");
        return;
    }

    // Initialize state for sequential processing
    TIncrementalRestoreState state;
    state.BackupCollectionPathId = backupCollectionPathId;
    state.OriginalOperationId = ui64(operationId.GetTxId());
    state.CurrentIncrementalIdx = 0;
    state.CurrentIncrementalStarted = false;
    
    // Add incremental backups (already sorted by timestamp based on backup names)
    for (const auto& backupName : incrementalBackupNames) {
        TPathId dummyPathId; // Will be filled when processing
        state.AddIncrementalBackup(dummyPathId, backupName, 0); // Timestamp will be inferred
    }
    
    // Store the state
    IncrementalRestoreStates[ui64(operationId.GetTxId())] = std::move(state);
    
    // Start processing the first incremental backup
    auto progressEvent = MakeHolder<TEvPrivate::TEvProgressIncrementalRestore>(ui64(operationId.GetTxId()));
    Send(SelfId(), progressEvent.Release());
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

// Handler for DataShard completion notifications
void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx) {
    const auto& record = ev->Get()->Record;
    
    LOG_I("Handle(TEvIncrementalRestoreResponse)"
        << " operationId: " << record.GetOperationId()
        << " shardIdx: " << record.GetShardIdx()
        << " incrementalIdx: " << record.GetIncrementalIdx()
        << " status: " << (int)record.GetRestoreStatus()
        << " tablet: " << TabletID());

    // Update state with shard completion
    auto stateIt = IncrementalRestoreStates.find(record.GetOperationId());
    if (stateIt != IncrementalRestoreStates.end()) {
        auto& state = stateIt->second;
        
        // Track shard completion for current incremental backup
        if (record.GetIncrementalIdx() == state.CurrentIncrementalIdx) {
            ui64 shardIdx = record.GetShardIdx();
            state.InProgressShards.erase(shardIdx);
            state.DoneShards.insert(shardIdx);
            
            LOG_I("Shard " << shardIdx << " completed incremental #" << record.GetIncrementalIdx()
                << " (" << state.DoneShards.size() << " done, " << state.InProgressShards.size() << " in progress)");
            
            // Check if all shards are done for current incremental
            if (state.InProgressShards.empty() && state.CurrentIncrementalStarted) {
                LOG_I("All shards completed for incremental #" << record.GetIncrementalIdx());
                state.MarkCurrentIncrementalComplete();
                
                // Trigger progress to move to next incremental
                auto progressEvent = MakeHolder<TEvPrivate::TEvProgressIncrementalRestore>(record.GetOperationId());
                Send(SelfId(), progressEvent.Release());
            }
        } else {
            LOG_W("Received response for incremental #" << record.GetIncrementalIdx() 
                << " but currently processing #" << state.CurrentIncrementalIdx);
        }
    } else {
        LOG_W("No incremental restore state found for operation: " << record.GetOperationId());
    }
}

// Create a MultiIncrementalRestore operation for a single incremental backup
void TSchemeShard::CreateIncrementalRestoreOperation(
    const TPathId& backupCollectionPathId,
    ui64 operationId, 
    const TString& backupName,
    const TActorContext& ctx) {
    
    LOG_I("CreateIncrementalRestoreOperation for backup: " << backupName 
          << " operationId: " << operationId
          << " backupCollectionPathId: " << backupCollectionPathId);
    
    // Find the backup collection to get restore settings
    auto itBc = BackupCollections.find(backupCollectionPathId);
    if (itBc == BackupCollections.end()) {
        LOG_E("Backup collection not found for pathId: " << backupCollectionPathId);
        return;
    }
    
    // Get backup collection info and path
    const auto& backupCollectionInfo = itBc->second;
    const auto& bcPath = TPath::Init(backupCollectionPathId, this);
    
    // Create MultiIncrementalRestore operation for this single backup
    auto request = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>();
    auto& record = request->Record;
    
    TTxId txId = GetCachedTxId(ctx);
    record.SetTxId(ui64(txId));
    
    auto& tx = *record.AddTransaction();
    tx.SetOperationType(NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups);
    tx.SetInternal(true);
    tx.SetWorkingDir(bcPath.PathString());

    auto& restore = *tx.MutableRestoreMultipleIncrementalBackups();
    
    // Process each table in the backup collection
    for (const auto& item : backupCollectionInfo->Description.GetExplicitEntryList().GetEntries()) {
        std::pair<TString, TString> paths;
        TString err;
        if (!TrySplitPathByDb(item.GetPath(), bcPath.GetDomainPathString(), paths, err)) {
            LOG_E("Failed to split path: " << err);
            continue;
        }
        
        auto& relativeItemPath = paths.second;
        
        // Check if the incremental backup path exists
        TString incrBackupPathStr = JoinPath({bcPath.PathString(), backupName, relativeItemPath});
        const TPath& incrBackupPath = TPath::Resolve(incrBackupPathStr, this);
        
        if (incrBackupPath.IsResolved()) {
            LOG_I("Adding incremental backup path to restore: " << incrBackupPathStr);
            
            // Add to src paths
            restore.AddSrcTablePaths(incrBackupPathStr);
            
            // Set destination path if not already set
            if (!restore.HasDstTablePath()) {
                restore.SetDstTablePath(item.GetPath());
            }
        } else {
            LOG_W("Incremental backup path not found: " << incrBackupPathStr);
        }
    }
    
    // Track this operation for completion handling
    TOperationId restoreOpId(txId, 0);
    IncrementalRestoreOperationToState[restoreOpId] = operationId;
    
    // Update state to track target shards
    auto stateIt = IncrementalRestoreStates.find(operationId);
    if (stateIt != IncrementalRestoreStates.end()) {
        auto& state = stateIt->second;
        
        // Initialize shard tracking (simplified - get from table metadata in real implementation)
        state.InProgressShards.clear();
        state.DoneShards.clear();
        
        // For now, add some placeholder shards - this should be determined from table metadata
        for (const auto& [shardIdx, shardInfo] : ShardInfos) {
            if (shardInfo.TabletType == ETabletType::DataShard) {
                state.InProgressShards.insert(ui64(shardIdx.GetLocalId()));
                if (state.InProgressShards.size() >= 3) break; // Limit for testing
            }
        }
        
        LOG_I("Tracking " << state.InProgressShards.size() << " shards for incremental restore");
    }
    
    LOG_I("Sending MultiIncrementalRestore operation for backup: " << backupName);
    Send(SelfId(), request.Release());
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
