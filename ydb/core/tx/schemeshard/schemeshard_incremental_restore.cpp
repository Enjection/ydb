#include "schemeshard_billing_helpers.h"
#include "schemeshard_impl.h"

#include <ydb/core/metering/metering.h>
#include <ydb/core/tx/tx_proxy/proxy.h>

#include <util/generic/deque.h>

#if defined LOG_D || \
    defined LOG_W || \
    defined LOG_E
#error log macro redefinition
#endif

#define LOG_D(stream) LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[RestoreScan] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[RestoreScan] " << stream)
#define LOG_W(stream) LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[RestoreScan] " << stream)
#define LOG_E(stream) LOG_ERROR_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[RestoreScan] " << stream)

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

class TRestoreScanFinalizer: public TActorBootstrapped<TRestoreScanFinalizer> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::SCHEMESHARD_CDC_STREAM_SCAN_FINALIZER;
    }

    explicit TRestoreScanFinalizer(const TActorId& ssActorId, THolder<TEvSchemeShard::TEvModifySchemeTransaction>&& req)
        : SSActorId(ssActorId)
        , Request(std::move(req)) // template without txId
    {
    }

    void Bootstrap() {
        AllocateTxId();
        Become(&TRestoreScanFinalizer::StateWork);
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, Handle)
            sFunc(TEvents::TEvPoison, PassAway);
        }
    }

private:
    void AllocateTxId() {
        Send(MakeTxProxyID(), new TEvTxUserProxy::TEvAllocateTxId);
    }

    void Handle(TEvTxUserProxy::TEvAllocateTxIdResult::TPtr& ev) {
        Request->Record.SetTxId(ev->Get()->TxId);
        Send(SSActorId, Request.Release());
        TActorBootstrapped::PassAway();
    }

private:
    const TActorId SSActorId;
    THolder<TEvSchemeShard::TEvModifySchemeTransaction> Request;

}; // TRestoreScanFinalizer

struct TSchemeShard::TRestoreScan::TTxProgress: public TTransactionBase<TSchemeShard> {
    // params
    TEvPrivate::TEvRunRestoreScan::TPtr RunRestoreScan = nullptr;
    TEvDataShard::TEvRestoreScanResponse::TPtr RestoreScanResponse = nullptr;
    struct {
        TPathId StreamPathId;
        TTabletId TabletId;
        explicit operator bool() const { return StreamPathId && TabletId; }
    } PipeRetry;

    // side effects
    TDeque<std::tuple<TPathId, TTabletId, THolder<IEventBase>>> ScanRequests;
    TPathId StreamToProgress;
    THolder<NMetering::TEvMetering::TEvWriteMeteringJson> Metering;
    THolder<TEvSchemeShard::TEvModifySchemeTransaction> Finalize;

public:
    explicit TTxProgress(TSelf* self, TEvPrivate::TEvRunRestoreScan::TPtr& ev)
        : TBase(self)
        , RunRestoreScan(ev)
    {
    }

    explicit TTxProgress(TSelf* self, TEvDataShard::TEvRestoreScanResponse::TPtr& ev)
        : TBase(self)
        , RestoreScanResponse(ev)
    {
    }

    explicit TTxProgress(TSelf* self, const TPathId& streamPathId, TTabletId tabletId)
        : TBase(self)
        , PipeRetry({streamPathId, tabletId})
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_CDC_STREAM_SCAN_PROGRESS;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        if (RunRestoreScan) {
            return OnRunRestoreScan(txc, ctx);
        } else if (RestoreScanResponse) {
            return OnRestoreScanResponse(txc, ctx);
        } else if (PipeRetry) {
            return OnPipeRetry(txc, ctx);
        } else {
            Y_ABORT("unreachable");
        }
    }

    void Complete(const TActorContext& ctx) override {
        for (auto& [streamPathId, tabletId, ev] : ScanRequests) {
            Self->RestoreScanPipes.Send(streamPathId, tabletId, std::move(ev), ctx);
        }

        if (StreamToProgress) {
            ctx.Send(ctx.SelfID, new TEvPrivate::TEvRunRestoreScan(StreamToProgress));
        }

        if (Metering) {
            ctx.Send(NMetering::MakeMeteringServiceID(), Metering.Release());
        }

        if (Finalize) {
            ctx.Register(new TRestoreScanFinalizer(ctx.SelfID, std::move(Finalize)));
        }
    }

private:
    bool OnRunRestoreScan(TTransactionContext& txc, const TActorContext& ctx) {
        const auto& streamPathId = RunRestoreScan->Get()->StreamPathId;

        LOG_D("Run"
            << ": streamPathId# " << streamPathId);

        if (!Self->CdcStreams.contains(streamPathId)) {
            LOG_W("Cannot run"
                << ": streamPathId# " << streamPathId
                << ", reason# " << "stream doesn't exist");
            return true;
        }

        auto streamInfo = Self->CdcStreams.at(streamPathId);
        if (streamInfo->State != TCdcStreamInfo::EState::ECdcStreamStateScan) {
            LOG_W("Cannot run"
                << ": streamPathId# " << streamPathId
                << ", reason# " << "unexpected state");
            return true;
        }

        Y_ABORT_UNLESS(Self->PathsById.contains(streamPathId));
        auto streamPath = Self->PathsById.at(streamPathId);

        Y_ABORT_UNLESS(Self->PathsById.contains(streamPathId));
        const auto& tablePathId = Self->PathsById.at(streamPathId)->ParentPathId;

        Y_ABORT_UNLESS(Self->Tables.contains(tablePathId));
        auto table = Self->Tables.at(tablePathId);

        if (streamInfo->ScanShards.empty()) {
            NIceDb::TNiceDb db(txc.DB);
            for (const auto& shard : table->GetPartitions()) {
                // const auto status = TCdcStreamInfo::TShardStatus(NKikimrTxDataShard::TEvRestoreScanResponse::PENDING);
                const auto status = TCdcStreamInfo::TShardStatus(NKikimrTxDataShard::TEvCdcStreamScanResponse::PENDING);
                streamInfo->ScanShards.emplace(shard.ShardIdx, status);
                streamInfo->PendingShards.insert(shard.ShardIdx);
                Self->PersistRestoreScanShardStatus(db, streamPathId, shard.ShardIdx, status);
            }
        }

        while (!streamInfo->PendingShards.empty()) {
            if (streamInfo->InProgressShards.size() >= Self->MaxCdcInitialScanShardsInFlight) {
                break;
            }

            auto it = streamInfo->PendingShards.begin();

            Y_ABORT_UNLESS(Self->ShardInfos.contains(*it));
            const auto tabletId = Self->ShardInfos.at(*it).TabletID;

            streamInfo->InProgressShards.insert(*it);
            streamInfo->PendingShards.erase(it);

            auto ev = MakeHolder<TEvDataShard::TEvRestoreScanRequest>();
            tablePathId.ToProto(ev->Record.MutableTablePathId());
            ev->Record.SetTableSchemaVersion(table->AlterVersion);
            streamPathId.ToProto(ev->Record.MutableStreamPathId());
            ev->Record.SetSnapshotStep(ui64(streamPath->StepCreated));
            ev->Record.SetSnapshotTxId(ui64(streamPath->CreateTxId));
            ScanRequests.emplace_back(streamPathId, tabletId, std::move(ev));
        }

        if (streamInfo->DoneShards.size() == streamInfo->ScanShards.size()) {
            const auto path = TPath::Init(streamPathId, Self);

            Finalize = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>();
            auto& tx = *Finalize->Record.AddTransaction();
            tx.SetOperationType(NKikimrSchemeOp::ESchemeOpAlterCdcStream);
            tx.SetWorkingDir(path.Parent().Parent().PathString()); // stream -> table -> working dir
            tx.SetFailOnExist(false);

            auto& op = *tx.MutableAlterCdcStream();
            op.SetTableName(path.Parent().LeafName());
            op.SetStreamName(path.LeafName());
            op.MutableGetReady()->SetLockTxId(ui64(streamPath->CreateTxId));
            tx.MutableLockGuard()->SetOwnerTxId(ui64(streamPath->CreateTxId));
        }

        return true;
    }

    bool OnRestoreScanResponse(TTransactionContext& txc, const TActorContext& ctx) {
        const auto& record = RestoreScanResponse->Get()->Record;

        LOG_D("Response"
            << ": ev# " << record.ShortDebugString());

        // FIXME

        Y_UNUSED(txc, ctx, record);

        // const auto streamPathId = TPathId::FromProto(record.GetStreamPathId());
        // if (!Self->CdcStreams.contains(streamPathId)) {
        //     LOG_W("Cannot process response"
        //         << ": streamPathId# " << streamPathId
        //         << ", reason# " << "stream doesn't exist");
        //     return true;
        // }

        // auto streamInfo = Self->CdcStreams.at(streamPathId);
        // if (streamInfo->State != TCdcStreamInfo::EState::ECdcStreamStateScan) {
        //     LOG_W("Cannot process response"
        //         << ": streamPathId# " << streamPathId
        //         << ", reason# " << "unexpected state");
        //     return true;
        // }

        // const auto tabletId = TTabletId(record.GetTabletId());
        // const auto shardIdx = Self->GetShardIdx(tabletId);
        // if (shardIdx == InvalidShardIdx) {
        //     LOG_E("Cannot process response"
        //         << ": streamPathId# " << streamPathId
        //         << ", tabletId# " << tabletId
        //         << ", reason# " << "tablet not found");
        //     return true;
        // }

        // auto it = streamInfo->ScanShards.find(shardIdx);
        // if (it == streamInfo->ScanShards.end()) {
        //     LOG_E("Cannot process response"
        //         << ": streamPathId# " << streamPathId
        //         << ", shardIdx# " << shardIdx
        //         << ", reason# " << "shard not found");
        //     return true;
        // }

        // auto& status = it->second;
        // if (!streamInfo->InProgressShards.contains(shardIdx)) {
        //     LOG_W("Shard status mismatch"
        //         << ": streamPathId# " << streamPathId
        //         << ", shardIdx# " << shardIdx
        //         << ", got# " << record.GetStatus()
        //         << ", current# " << status.Status);
        //     return true;
        // }

        // switch (record.GetStatus()) {
        // case NKikimrTxDataShard::TEvRestoreScanResponse::ACCEPTED:
        // case NKikimrTxDataShard::TEvRestoreScanResponse::IN_PROGRESS:
        //     break;

        // case NKikimrTxDataShard::TEvRestoreScanResponse::DONE:
        //     status.Status = record.GetStatus();
        //     streamInfo->DoneShards.insert(shardIdx);
        //     streamInfo->InProgressShards.erase(shardIdx);
        //     Self->RestoreScanPipes.Close(streamPathId, tabletId, ctx);
        //     StreamToProgress = streamPathId;
        //     Bill(streamPathId, shardIdx, TRUCalculator::ReadTable(record.GetStats().GetBytesProcessed()), ctx);
        //     break;

        // case NKikimrTxDataShard::TEvRestoreScanResponse::OVERLOADED:
        // case NKikimrTxDataShard::TEvRestoreScanResponse::ABORTED:
        //     streamInfo->PendingShards.insert(shardIdx);
        //     streamInfo->InProgressShards.erase(shardIdx);
        //     Self->RestoreScanPipes.Close(streamPathId, tabletId, ctx);
        //     StreamToProgress = streamPathId;
        //     break;

        // case NKikimrTxDataShard::TEvRestoreScanResponse::BAD_REQUEST:
        // case NKikimrTxDataShard::TEvRestoreScanResponse::SCHEME_ERROR:
        //     Y_ABORT("unreachable");

        // default:
        //     LOG_E("Unexpected response status"
        //         << ": status# " << static_cast<int>(record.GetStatus())
        //         << ", error# " << record.GetErrorDescription());
        //     return true;
        // }

        // NIceDb::TNiceDb db(txc.DB);
        // Self->PersistRestoreScanShardStatus(db, streamPathId, shardIdx, status);

        // if (streamInfo->DoneShards.size() == streamInfo->ScanShards.size()) {
        //     StreamToProgress = streamPathId;
        // }

        return true;
    }

    bool OnPipeRetry(TTransactionContext&, const TActorContext& ctx) {
        const auto& streamPathId = PipeRetry.StreamPathId;
        const auto& tabletId = PipeRetry.TabletId;

        LOG_D("Pipe retry"
            << ": streamPathId# " << streamPathId
            << ", tabletId# " << tabletId);

        if (!Self->CdcStreams.contains(streamPathId)) {
            LOG_W("Cannot retry"
                << ": streamPathId# " << streamPathId
                << ", reason# " << "stream doesn't exist");
            return true;
        }

        auto streamInfo = Self->CdcStreams.at(streamPathId);
        if (streamInfo->State != TCdcStreamInfo::EState::ECdcStreamStateScan) {
            LOG_W("Cannot retry"
                << ": streamPathId# " << streamPathId
                << ", reason# " << "unexpected state");
            return true;
        }

        const auto shardIdx = Self->GetShardIdx(tabletId);
        if (shardIdx == InvalidShardIdx) {
            LOG_E("Cannot retry"
                << ": streamPathId# " << streamPathId
                << ", tabletId# " << tabletId
                << ", reason# " << "tablet not found");
            return true;
        }

        auto it = streamInfo->InProgressShards.find(shardIdx);
        if (it == streamInfo->InProgressShards.end()) {
            LOG_E("Cannot retry"
                << ": streamPathId# " << streamPathId
                << ", shardIdx# " << shardIdx
                << ", reason# " << "shard not found");
            return true;
        }

        streamInfo->PendingShards.insert(*it);
        streamInfo->InProgressShards.erase(it);
        Self->RestoreScanPipes.Close(streamPathId, tabletId, ctx);
        StreamToProgress = streamPathId;

        return true;
    }
};

ITransaction* TSchemeShard::CreateTxProgressRestoreScan(TEvPrivate::TEvRunRestoreScan::TPtr& ev) {
    return new TRestoreScan::TTxProgress(this, ev);
}

ITransaction* TSchemeShard::CreateTxProgressRestoreScan(TEvDataShard::TEvRestoreScanResponse::TPtr& ev) {
    return new TRestoreScan::TTxProgress(this, ev);
}

ITransaction* TSchemeShard::CreatePipeRetry(TTabletId tabletId) {
    // TODO
    return nullptr;
    // return new TRestoreScan::TTxProgress(this, tabletId);
}

void TSchemeShard::Handle(TEvPrivate::TEvRunRestoreScan::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxProgressRestoreScan(ev), ctx);
}

void TSchemeShard::Handle(TEvDataShard::TEvRestoreScanResponse::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxProgressRestoreScan(ev), ctx);
}

void TSchemeShard::ResumeRestoreScans(const TVector<TPathId>& ids, const TActorContext& ctx) {
    for (const auto& id : ids) {
        Send(ctx.SelfID, new TEvPrivate::TEvRunRestoreScan(id));
    }
}

void TSchemeShard::PersistRestoreScanShardStatus(NIceDb::TNiceDb& db, const TPathId& streamPathId,
        const TShardIdx& shardIdx, const TCdcStreamInfo::TShardStatus& status)
{
    // FIXME

    Y_UNUSED(db, streamPathId, shardIdx, status);

    // db.Table<Schema::RestoreScanShardStatus>()
    //     .Key(streamPathId.OwnerId, streamPathId.LocalPathId, shardIdx.GetOwnerId(), shardIdx.GetLocalId())
    //     .Update(
    //         NIceDb::TUpdate<Schema::RestoreScanShardStatus::Status>(status.Status)
    //     );
}

void TSchemeShard::RemoveRestoreScanShardStatus(NIceDb::TNiceDb& db, const TPathId& streamPathId, const TShardIdx& shardIdx) {
    db.Table<Schema::RestoreScanShardStatus>()
        .Key(streamPathId.OwnerId, streamPathId.LocalPathId, shardIdx.GetOwnerId(), shardIdx.GetLocalId())
        .Delete();
}

}
