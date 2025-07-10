#pragma once
#include "datashard_impl.h"

namespace NKikimr {
namespace NDataShard {

class TDataShard::TTxIncrementalRestore : public NTabletFlatExecutor::TTransactionBase<TDataShard> {
public:
    TTxIncrementalRestore(TDataShard* self, TEvDataShard::TEvIncrementalRestoreRequest::TPtr& ev)
        : TBase(self)
        , Event(ev)
    {}

    bool Execute(TTransactionContext&, const TActorContext& ctx) override {
        const auto& record = Event->Get()->Record;
        
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD,
            "TTxIncrementalRestore at tablet " << Self->TabletID()
            << " operationId: " << record.GetOperationId()
            << " shardIdx: " << record.GetShardIdx());

        // DataShard just acknowledges the request
        // Actual incremental restore work happens via change senders
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        auto response = MakeHolder<TEvDataShard::TEvIncrementalRestoreResponse>();
        const auto& record = Event->Get()->Record;
        
        response->Record.SetTxId(record.GetTxId());
        response->Record.SetTableId(record.GetTableId());
        response->Record.SetOperationId(record.GetOperationId());
        response->Record.SetIncrementalIdx(record.GetIncrementalIdx());
        response->Record.SetShardIdx(record.GetShardIdx());
        response->Record.SetRestoreStatus(NKikimrTxDataShard::TEvIncrementalRestoreResponse::SUCCESS);
        
        ctx.Send(Event->Sender, response.Release());
    }

private:
    TEvDataShard::TEvIncrementalRestoreRequest::TPtr Event;
};

} // namespace NDataShard
} // namespace NKikimr
