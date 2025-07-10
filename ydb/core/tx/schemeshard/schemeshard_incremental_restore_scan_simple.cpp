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

    bool Execute(NTabletFlatExecutor::TTransactionContext& txc, const TActorContext& ctx) override {
        LOG_I("TTxProgressIncrementalRestore::Execute"
            << " operationId: " << OperationId
            << " tablet: " << Self->TabletID());

        // Find the operation
        auto operation = Self->FindTx(OperationId);
        if (!operation) {
            LOG_W("Operation not found: " << OperationId);
            return true;
        }

        // Check if operation is complete
        if (operation->Done()) {
            LOG_I("Operation is already done: " << OperationId);
            return true;
        }

        // Simple progress check - just mark as progressing
        LOG_I("Operation is progressing: " << OperationId);
        
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
    const auto& record = ev->Get()->Record;
    ui64 operationId = record.GetOperationId();
    
    LOG_I("Handle(TEvRunIncrementalRestore)"
        << " operationId: " << operationId
        << " tablet: " << TabletID());

    // Start progress tracking
    Execute(new TTxProgressIncrementalRestore(this, operationId), ctx);
}

// Handler for TEvProgressIncrementalRestore  
void TSchemeShard::Handle(TEvPrivate::TEvProgressIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    const auto& record = ev->Get()->Record;
    ui64 operationId = record.GetOperationId();
    
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
        << " status: " << record.GetRestoreStatus()
        << " tablet: " << TabletID());

    // Send progress update
    auto progressEvent = MakeHolder<TEvPrivate::TEvProgressIncrementalRestore>();
    progressEvent->Record.SetOperationId(record.GetOperationId());
    Send(SelfId(), progressEvent.Release());
}

// Helper function to create TTxProgressIncrementalRestore
NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(ui64 operationId) {
    return new TTxProgressIncrementalRestore(this, operationId);
}

} // namespace NKikimr::NSchemeShard
