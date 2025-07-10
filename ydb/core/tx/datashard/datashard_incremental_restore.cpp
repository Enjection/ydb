#include "datashard_incremental_restore.h"
#include "datashard_impl.h"

namespace NKikimr {
namespace NDataShard {

TDataShard::TTxIncrementalRestore::TTxIncrementalRestore(TDataShard* self, TEvDataShard::TEvIncrementalRestoreRequest::TPtr& ev)
    : TBase(self)
    , Event(ev)
{}

bool TDataShard::TTxIncrementalRestore::Execute(TTransactionContext&, const TActorContext& ctx) {
    const auto& record = Event->Get()->Record;
    
    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD,
        "TTxIncrementalRestore at tablet " << Self->TabletID()
        << " operationId: " << record.GetOperationId()
        << " shardIdx: " << record.GetShardIdx());

    // DataShard just acknowledges the request
    // Actual incremental restore work happens via change senders
    return true;
}

void TDataShard::TTxIncrementalRestore::Complete(const TActorContext& ctx) {
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

} // namespace NDataShard
} // namespace NKikimr
