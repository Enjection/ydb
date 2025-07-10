#include "schemeshard_impl.h"
#include "schemeshard_utils.h"

#include <ydb/core/tx/tx_proxy/proxy.h>

#if defined LOG_D || \
    defined LOG_W || \
    defined LOG_N || \
    defined LOG_I || \
    defined LOG_E
#error lvoid TSchemeShard::CreateIncrementalRestoreOperation(
    const TPathId& backupCollectionPathId, 
    ui64 operationId, 
    const TString& backupName,
    const TActorContext& ctx) {
    
    LOG_I("[IncrementalRestore] CreateIncrementalRestoreOperation START for backup: " << backupName 
          << " operationId: " << operationId
          << " backupCollectionPathId: " << backupCollectionPathId);redefinition
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

        // Debug: Check what states exist
        LOG_I("IncrementalRestoreStates contains " << Self->IncrementalRestoreStates.size() << " entries");
        for (const auto& [key, value] : Self->IncrementalRestoreStates) {
            LOG_I("  State key: " << key << " (comparing with " << OperationId << ")");
        }

        // Find the incremental restore state for this operation
        LOG_I("Looking up state for operation: " << OperationId << " (type: ui64)");
        auto stateIt = Self->IncrementalRestoreStates.find(OperationId);
        if (stateIt == Self->IncrementalRestoreStates.end()) {
            LOG_W("No incremental restore state found for operation: " << OperationId);
            LOG_I("Available states:");
            for (const auto& [key, value] : Self->IncrementalRestoreStates) {
                LOG_I("  Key: " << key);
            }
            return true;
        }

        auto& state = stateIt->second;
        
        LOG_I("Found state with " << state.IncrementalBackups.size() << " incremental backups, current index: " << state.CurrentIncrementalIdx);
        
        // Check for completed operations by seeing if they're still in the Operations map
        CheckForCompletedOperations(state, ctx);
        
        // Check if all operations for current incremental backup are complete
        if (state.AreAllCurrentOperationsComplete()) {
            LOG_I("All operations for current incremental backup completed, moving to next");
            state.MarkCurrentIncrementalComplete();
            state.MoveToNextIncremental();
            
            if (state.AllIncrementsProcessed()) {
                LOG_I("All incremental backups processed, cleaning up");
                Self->IncrementalRestoreStates.erase(OperationId);
                return true;
            }
            
            // Start processing next incremental backup
            ProcessNextIncrementalBackup(state, ctx);
        } else if (!state.InProgressOperations.empty()) {
            // Still have operations in progress, schedule another check
            auto progressEvent = MakeHolder<TEvPrivate::TEvProgressIncrementalRestore>(OperationId);
            Self->Schedule(TDuration::Seconds(1), progressEvent.Release());
        } else {
            // No operations in progress, start the first incremental backup
            LOG_I("No operations in progress, starting first incremental backup");
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
    
    void CheckForCompletedOperations(TIncrementalRestoreState& state, const TActorContext& ctx) {
        // Check if any in-progress operations have completed
        THashSet<TOperationId> stillInProgress;
        
        LOG_I("CheckForCompletedOperations: checking " << state.InProgressOperations.size() << " operations");
        
        for (const auto& opId : state.InProgressOperations) {
            TTxId txId = opId.GetTxId();
            LOG_I("CheckForCompletedOperations: checking operation " << opId << " (txId: " << txId << ")");
            
            if (Self->Operations.contains(txId)) {
                // Operation is still running
                stillInProgress.insert(opId);
                LOG_I("Operation " << opId << " still in progress");
            } else {
                // Operation completed
                state.CompletedOperations.insert(opId);
                LOG_I("Operation " << opId << " completed for incremental restore " << OperationId);
            }
        }
        
        LOG_I("CheckForCompletedOperations: " << stillInProgress.size() << " still in progress, " << state.CompletedOperations.size() << " completed");
        
        state.InProgressOperations = std::move(stillInProgress);
    }
    
    void ProcessNextIncrementalBackup(TIncrementalRestoreState& state, const TActorContext& ctx) {
        const auto* currentIncremental = state.GetCurrentIncremental();
        if (!currentIncremental) {
            LOG_I("No more incremental backups to process");
            return;
        }
        
        LOG_I("Processing incremental backup #" << state.CurrentIncrementalIdx + 1 
            << " path: " << currentIncremental->BackupPath
            << " timestamp: " << currentIncremental->Timestamp);
        
        LOG_I("[IncrementalRestore] About to call CreateIncrementalRestoreOperation");
        
        // Create MultiIncrementalRestore operation for this backup
        Self->CreateIncrementalRestoreOperation(
            state.BackupCollectionPathId,
            OperationId,
            currentIncremental->BackupPath,
            ctx
        );
        
        LOG_I("[IncrementalRestore] Finished calling CreateIncrementalRestoreOperation");
        
        // Initialize tracking for this incremental backup
        state.InProgressOperations.clear();
        state.CompletedOperations.clear();
        state.CurrentIncrementalStarted = true;
        
        // Schedule a progress check to detect when operations complete
        auto progressEvent = MakeHolder<TEvPrivate::TEvProgressIncrementalRestore>(OperationId);
        Self->Schedule(TDuration::Seconds(1), progressEvent.Release());
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
    
    // Debug: print all incremental backup names
    for (size_t i = 0; i < incrementalBackupNames.size(); ++i) {
        LOG_I("Handle(TEvRunIncrementalRestore) incrementalBackupNames[" << i << "]: '" << incrementalBackupNames[i] << "'");
    }

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

// Handler for DataShard completion notifications (currently unused - using operation completion instead)
void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx) {
    const auto& record = ev->Get()->Record;
    
    LOG_I("Handle(TEvIncrementalRestoreResponse)"
        << " operationId: " << record.GetOperationId()
        << " shardIdx: " << record.GetShardIdx()
        << " incrementalIdx: " << record.GetIncrementalIdx()
        << " status: " << (int)record.GetRestoreStatus()
        << " tablet: " << TabletID());

    // Currently using operation completion detection instead of shard-level responses
    // This handler is kept for future enhancement but not actively used
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
    
    // Process each table in the backup collection - create separate operation for each table
    for (const auto& item : backupCollectionInfo->Description.GetExplicitEntryList().GetEntries()) {
        std::pair<TString, TString> paths;
        TString err;
        if (!TrySplitPathByDb(item.GetPath(), bcPath.GetDomainPathString(), paths, err)) {
            LOG_E("Failed to split path: " << err);
            continue;
        }
        
        auto& relativeItemPath = paths.second;
        
        // Check if the incremental backup path exists (with _incremental suffix)
        TString incrBackupPathStr = JoinPath({bcPath.PathString(), backupName + "_incremental", relativeItemPath});
        const TPath& incrBackupPath = TPath::Resolve(incrBackupPathStr, this);
        
        if (incrBackupPath.IsResolved()) {
            LOG_I("Creating separate restore operation for table: " << incrBackupPathStr << " -> " << item.GetPath());
            
            // Create a separate MultiIncrementalRestore operation for this table
            auto tableRequest = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>();
            auto& tableRecord = tableRequest->Record;
            
            TTxId tableTxId = GetCachedTxId(ctx);
            tableRecord.SetTxId(ui64(tableTxId));
            
            auto& tableTx = *tableRecord.AddTransaction();
            tableTx.SetOperationType(NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups);
            tableTx.SetInternal(true);
            tableTx.SetWorkingDir(bcPath.PathString());

            auto& tableRestore = *tableTx.MutableRestoreMultipleIncrementalBackups();
            
            // Add single source path for this table
            tableRestore.AddSrcTablePaths(incrBackupPathStr);
            
            // Set destination path for this table
            tableRestore.SetDstTablePath(item.GetPath());
            
            // Track this operation for completion handling
            TOperationId tableRestoreOpId(tableTxId, 0);
            IncrementalRestoreOperationToState[tableRestoreOpId] = operationId;
            
            // Add to current incremental operations tracking
            auto stateIt = IncrementalRestoreStates.find(operationId);
            if (stateIt != IncrementalRestoreStates.end()) {
                stateIt->second.InProgressOperations.insert(tableRestoreOpId);
                LOG_I("Tracking operation " << tableRestoreOpId << " for incremental restore " << operationId);
            }
            
            LOG_I("Sending MultiIncrementalRestore operation for table: " << item.GetPath());
            Send(SelfId(), tableRequest.Release());
        } else {
            LOG_W("Incremental backup path not found: " << incrBackupPathStr);
        }
    }
    
    LOG_I("Created separate restore operations for incremental backup: " << backupName);
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
