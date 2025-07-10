#include "datashard_impl.h"

namespace NKikimr {
namespace NDataShard {

///
/// TTxIncrementalRestoreRequest
///

class TTxIncrementalRestoreRequest : public TTransactionBase<TDataShard> {
public:
    TTxIncrementalRestoreRequest(TDataShard* ds, TEvDataShard::TEvIncrementalRestoreRequest::TPtr ev)
        : TTransactionBase(ds)
        , Ev(std::move(ev))
    {}

    TTxType GetTxType() const override { return TXTYPE_INCREMENTAL_RESTORE_REQUEST; }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "TTxIncrementalRestoreRequest::Execute at " << Self->TabletID());

        const auto& record = Ev->Get()->Record;
        const ui64 operationId = record.GetOperationId();
        const ui64 tableId = record.GetTableId();
        const TShardIdx shardIdx = TShardIdx(record.GetShardIdx());
        const ui64 backupCollectionPathId = record.GetBackupCollectionPathId();
        const ui64 sourcePathId = record.GetSourcePathId();

        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, 
            "Processing incremental restore request: operationId=" << operationId 
            << ", tableId=" << tableId 
            << ", shardIdx=" << shardIdx
            << ", backupCollectionPathId=" << backupCollectionPathId
            << ", sourcePathId=" << sourcePathId);

        // Create the response
        Response = MakeHolder<TEvDataShard::TEvIncrementalRestoreResponse>();
        Response->Record.SetOperationId(operationId);
        Response->Record.SetShardIdx(ui64(shardIdx));

        try {
            // Process the incremental restore
            ProcessIncrementalRestore(txc, ctx, record);
            
            // Success
            Response->Record.SetRestoreStatus(NKikimrTxDataShard::TEvIncrementalRestoreResponse::SUCCESS);
            LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, 
                "Incremental restore request processed successfully: operationId=" << operationId);
                
        } catch (const std::exception& ex) {
            // Error
            Response->Record.SetRestoreStatus(NKikimrTxDataShard::TEvIncrementalRestoreResponse::ERROR);
            Response->Record.SetError(TStringBuilder() << "Error processing incremental restore: " << ex.what());
            LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD, 
                "Error processing incremental restore request: operationId=" << operationId 
                << ", error=" << ex.what());
        }

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "TTxIncrementalRestoreRequest::Complete at " << Self->TabletID());
        
        // Send response back to SchemeShard
        ctx.Send(Ev->Sender, Response.Release());
    }

private:
    void ProcessIncrementalRestore(TTransactionContext& txc, const TActorContext& ctx, 
                                   const NKikimrTxDataShard::TEvIncrementalRestoreRequest& record) {
        // This is where the actual incremental restore logic would go
        // For now, we'll implement a basic version that validates the request
        
        const ui64 tableId = record.GetTableId();
        
        // Validate that the table exists
        auto tableInfoPtr = Self->TableInfos.FindPtr(tableId);
        if (!tableInfoPtr) {
            throw yexception() << "Table not found: " << tableId;
        }
        
        const auto& tableInfo = *tableInfoPtr;
        if (!tableInfo) {
            throw yexception() << "Table info is null for table: " << tableId;
        }
        
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, 
            "Validated table for incremental restore: tableId=" << tableId 
            << ", tableName=" << tableInfo->Name);
        
        // TODO: Implement actual incremental restore logic here
        // This would involve:
        // 1. Reading the incremental backup data from the source path
        // 2. Applying the incremental changes to the target table
        // 3. Updating progress and status
        
        // For now, we'll just mark it as completed
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, 
            "Incremental restore processing completed (placeholder implementation)");
    }

private:
    TEvDataShard::TEvIncrementalRestoreRequest::TPtr Ev;
    THolder<TEvDataShard::TEvIncrementalRestoreResponse> Response;
};

///
/// Handler implementation
///

void TDataShard::Handle(TEvDataShard::TEvIncrementalRestoreRequest::TPtr& ev, const TActorContext& ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Handle TEvIncrementalRestoreRequest at " << TabletID());
    
    Executor()->Execute(new TTxIncrementalRestoreRequest(this, ev), ctx);
}

} // namespace NDataShard
} // namespace NKikimr
