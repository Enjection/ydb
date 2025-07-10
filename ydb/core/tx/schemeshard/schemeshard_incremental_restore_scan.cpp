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

// Simplified TTxProgressIncrementalRestore implementation
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

        // Simple progress tracking - just acknowledge the operation
        // The actual work is done by MultiIncrementalRestore operation
        LOG_I("Incremental restore progress acknowledged: " << OperationId);
        
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        LOG_I("TTxProgressIncrementalRestore::Complete"
            << " operationId: " << OperationId);
    }

private:
    ui64 OperationId;
};

// Handler for TEvRunIncrementalRestore
void TSchemeShard::Handle(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    const auto& backupCollectionPathId = ev->Get()->BackupCollectionPathId;
    
    LOG_I("Handle(TEvRunIncrementalRestore)"
        << " backupCollectionPathId: " << backupCollectionPathId
        << " tablet: " << TabletID());

    // Simple implementation - just acknowledge the run request
    // The actual work is done by MultiIncrementalRestore operation
    LOG_I("Incremental restore run acknowledged for path: " << backupCollectionPathId);
}

// Handler for TEvProgressIncrementalRestore  
void TSchemeShard::Handle(TEvPrivate::TEvProgressIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    ui64 operationId = ev->Get()->OperationId;
    
    LOG_I("Handle(TEvProgressIncrementalRestore)"
        << " operationId: " << operationId
        << " tablet: " << TabletID());

    // Execute progress transaction
    Execute(new TTxProgressIncrementalRestore(this, operationId), ctx);
}

// Handler for DataShard response
void TSchemeShard::Handle(TEvDataShard::TEvIncrementalRestoreResponse::TPtr& ev, const TActorContext& ctx) {
    const auto& record = ev->Get()->Record;
    
    LOG_I("Handle(TEvIncrementalRestoreResponse)"
        << " operationId: " << record.GetOperationId()
        << " shardIdx: " << record.GetShardIdx()
        << " status: " << (int)record.GetRestoreStatus()
        << " tablet: " << TabletID());

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
