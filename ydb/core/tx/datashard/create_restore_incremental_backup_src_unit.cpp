#include "defs.h"
#include "execution_unit_ctors.h"
#include "datashard_active_transaction.h"
#include "datashard_impl.h"
#include "export_iface.h"
#include "export_scan.h"
#include <ydb/core/protos/datashard_config.pb.h>

namespace NKikimr {
namespace NDataShard {

using namespace NKikimrTxDataShard;
using namespace NExportScan;

///

class IRestoreIncrementalBackupFactory {
public:
    virtual ~IRestoreIncrementalBackupFactory() = default;
    virtual IExport* CreateRestore(const ::NKikimrSchemeOp::TRestoreIncrementalBackup, const IExport::TTableColumns& columns) const = 0;
    virtual void Shutdown() = 0;
};

class TTableExport: public IExport {
public:
    explicit TTableExport(const ::NKikimrSchemeOp::TRestoreIncrementalBackup& task, const TTableColumns& columns)
        : Task(task)
        , Columns(columns)
    {
    }

    IActor* CreateUploader(const TActorId& dataShard, ui64 txId) const override {
        // FIXME
        Y_UNUSED(dataShard, txId);
        return nullptr;
    }

    IBuffer* CreateBuffer() const override {
        // using namespace NBackupRestoreTraits;

        // const auto& scanSettings = Task.GetScanSettings();
        // const ui64 maxRows = scanSettings.GetRowsBatchSize() ? scanSettings.GetRowsBatchSize() : Max<ui64>();
        // const ui64 maxBytes = scanSettings.GetBytesBatchSize();
        // const ui64 minBytes = Task.GetS3Settings().GetLimits().GetMinWriteBatchSize();

        // switch (CodecFromTask(Task)) {
        // case ECompressionCodec::None:
        //     return CreateS3ExportBufferRaw(Columns, maxRows, maxBytes);
        // case ECompressionCodec::Zstd:
        //     return CreateS3ExportBufferZstd(Task.GetCompression().GetLevel(), Columns, maxRows, maxBytes, minBytes);
        // case ECompressionCodec::Invalid:
        //     Y_ABORT("unreachable");
        // }
        // FIXME
        return nullptr;
    }

    void Shutdown() const override {}

protected:
    const ::NKikimrSchemeOp::TRestoreIncrementalBackup Task;
    const TTableColumns Columns;
};

class TDirectReplicationScan: private NActors::IActorCallback, public NTable::IScan {
    enum EStateBits {
        ES_REGISTERED = 0, // Actor is registered
        ES_INITIALIZED, // Seek(...) was called
        ES_UPLOADER_READY,
        ES_BUFFER_SENT,
        ES_NO_MORE_DATA,

        ES_COUNT,
    };

    struct TStats: public IBuffer::TStats {
        TStats()
            : IBuffer::TStats()
        {
            auto counters = GetServiceCounters(AppData()->Counters, "tablets")->GetSubgroup("subsystem", "store_to_yt");

            MonRows = counters->GetCounter("Rows", true);
            MonBytesRead = counters->GetCounter("BytesRead", true);
            MonBytesSent = counters->GetCounter("BytesSent", true);
        }

        void Aggr(ui64 rows, ui64 bytesRead, ui64 bytesSent) {
            Rows += rows;
            BytesRead += bytesRead;
            BytesSent += bytesSent;

            *MonRows += rows;
            *MonBytesRead += bytesRead;
            *MonBytesSent += bytesSent;
        }

        void Aggr(const IBuffer::TStats& stats) {
            Aggr(stats.Rows, stats.BytesRead, stats.BytesSent);
        }

        TString ToString() const {
            return TStringBuilder()
                << "Stats { "
                    << " Rows: " << Rows
                    << " BytesRead: " << BytesRead
                    << " BytesSent: " << BytesSent
                << " }";
        }

    private:
        ::NMonitoring::TDynamicCounters::TCounterPtr MonRows;
        ::NMonitoring::TDynamicCounters::TCounterPtr MonBytesRead;
        ::NMonitoring::TDynamicCounters::TCounterPtr MonBytesSent;
    };

    bool IsReady() const {
        return State.Test(ES_REGISTERED) && State.Test(ES_INITIALIZED);
    }

    void MaybeReady() {
        if (IsReady()) {
            Send(Uploader, new TEvExportScan::TEvReady());
        }
    }

    EScan MaybeSendBuffer() {
        const bool noMoreData = State.Test(ES_NO_MORE_DATA);

        if (!noMoreData && !Buffer->IsFilled()) {
            return EScan::Feed;
        }

        if (!State.Test(ES_UPLOADER_READY) || State.Test(ES_BUFFER_SENT)) {
            Spent->Alter(false);
            return EScan::Sleep;
        }

        IBuffer::TStats stats;
        THolder<IEventBase> ev{Buffer->PrepareEvent(noMoreData, stats)};

        if (!ev) {
            Success = false;
            Error = Buffer->GetError();
            return EScan::Final;
        }

        Send(Uploader, std::move(ev));
        State.Set(ES_BUFFER_SENT);
        Stats->Aggr(stats);

        if (noMoreData) {
            Spent->Alter(false);
            return EScan::Sleep;
        }

        return EScan::Feed;
    }

    void Handle(TEvExportScan::TEvReset::TPtr&) {
        Y_ABORT_UNLESS(IsReady());

        EXPORT_LOG_D("Handle TEvExportScan::TEvReset"
            << ": self# " << SelfId());

        Stats.Reset(new TStats);
        State.Reset(ES_UPLOADER_READY).Reset(ES_BUFFER_SENT).Reset(ES_NO_MORE_DATA);
        Spent->Alter(true);
        Driver->Touch(EScan::Reset);
    }

    void Handle(TEvExportScan::TEvFeed::TPtr&) {
        Y_ABORT_UNLESS(IsReady());

        EXPORT_LOG_D("Handle TEvExportScan::TEvFeed"
            << ": self# " << SelfId());

        State.Set(ES_UPLOADER_READY).Reset(ES_BUFFER_SENT);
        Spent->Alter(true);
        if (EScan::Feed == MaybeSendBuffer()) {
            Driver->Touch(EScan::Feed);
        }
    }

    void Handle(TEvExportScan::TEvFinish::TPtr& ev) {
        Y_ABORT_UNLESS(IsReady());

        EXPORT_LOG_D("Handle TEvExportScan::TEvFinish"
            << ": self# " << SelfId()
            << ", msg# " << ev->Get()->ToString());

        Success = ev->Get()->Success;
        Error = ev->Get()->Error;
        Driver->Touch(EScan::Final);
    }

public:
    static constexpr TStringBuf LogPrefix() {
        return "scanner"sv;
    }

    explicit TDirectReplicationScan()
        : IActorCallback(static_cast<TReceiveFunc>(&TExportScan::StateWork), NKikimrServices::TActivity::EXPORT_SCAN_ACTOR)
        , Stats(new TStats)
        , Driver(nullptr)
        , Success(false)
    {
    }

    void Describe(IOutputStream& o) const noexcept override {
        o << "ExportScan { "
              << "Uploader: " << Uploader
              << Stats->ToString() << " "
              << "Success: " << Success
              << "Error: " << Error
          << " }";
    }

    IScan::TInitialState Prepare(IDriver* driver, TIntrusiveConstPtr<TScheme> scheme) noexcept override {
        TlsActivationContext->AsActorContext().RegisterWithSameMailbox(this);

        Driver = driver;
        Scheme = std::move(scheme);
        Spent = new TSpent(TAppData::TimeProvider.Get());
        Buffer->ColumnsOrder(Scheme->Tags());

        return {EScan::Feed, {}};
    }

    void Registered(TActorSystem* sys, const TActorId&) override {
        // Uploader = sys->Register(CreateUploaderFn(), TMailboxType::HTSwap, AppData()->BatchPoolId);

        State.Set(ES_REGISTERED);
        MaybeReady();
    }

    EScan Seek(TLead& lead, ui64) noexcept override {
        lead.To(Scheme->Tags(), {}, ESeek::Lower);
        Buffer->Clear();

        State.Set(ES_INITIALIZED);
        MaybeReady();

        Spent->Alter(true);
        return EScan::Feed;
    }

    EScan Feed(TArrayRef<const TCell>, const TRow& row) noexcept override {
        if (!Buffer->Collect(row)) {
            Success = false;
            Error = Buffer->GetError();
            EXPORT_LOG_E("Error read data from table: " << Error);
            return EScan::Final;
        }

        return MaybeSendBuffer();
    }

    EScan Exhausted() noexcept override {
        State.Set(ES_NO_MORE_DATA);
        return MaybeSendBuffer();
    }

    TAutoPtr<IDestructable> Finish(EAbort abort) noexcept override {
        auto outcome = EExportOutcome::Success;
        if (abort != EAbort::None) {
            outcome = EExportOutcome::Aborted;
        } else if (!Success) {
            outcome = EExportOutcome::Error;
        }

        PassAway();
        return new TExportScanProduct(outcome, Error, Stats->BytesRead, Stats->Rows);
    }

    void PassAway() override {
        if (const auto& actorId = std::exchange(Uploader, {})) {
            Send(actorId, new TEvents::TEvPoisonPill());
        }

        IActorCallback::PassAway();
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvExportScan::TEvReset, Handle);
            hFunc(TEvExportScan::TEvFeed, Handle);
            hFunc(TEvExportScan::TEvFinish, Handle);
        }
    }

private:
    TActorId Uploader;
    THolder<TStats> Stats;

    IDriver* Driver;
    TIntrusiveConstPtr<TScheme> Scheme;
    TAutoPtr<TSpent> Spent;

    TBitMap<EStateBits::ES_COUNT> State;
    bool Success;
    TString Error;

}; // TExportScan

NTable::IScan* CreateDirectReplicationScan() {
    return nullptr; // FIXME
}

///

class TRestoreIncrementalBackupSrcUnit : public TExecutionUnit {
protected:
    bool IsRelevant(TActiveTransaction* tx) const {
        return tx->GetSchemeTx().HasRestoreIncrementalBackupSrc();
    }

    bool IsWaiting(TOperation::TPtr op) const {
        return op->IsWaitingForScan() || op->IsWaitingForRestart();
    }

    void SetWaiting(TOperation::TPtr op) {
        op->SetWaitingForScanFlag();
    }

    void ResetWaiting(TOperation::TPtr op) {
        op->ResetWaitingForScanFlag();
        op->ResetWaitingForRestartFlag();
    }

    void Abort(TOperation::TPtr op, const TActorContext& ctx, const TString& error) {
        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

        LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, error);

        BuildResult(op)->AddError(NKikimrTxDataShard::TError::WRONG_SHARD_STATE, error);
        ResetWaiting(op);

        Cancel(tx, ctx);
    }

    bool Run(TOperation::TPtr op, TTransactionContext& txc, const TActorContext& ctx) {
        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

        Y_ABORT_UNLESS(tx->GetSchemeTx().HasRestoreIncrementalBackupSrc());
        const auto& restoreSrc = tx->GetSchemeTx().GetRestoreIncrementalBackupSrc();

        const ui64 tableId = restoreSrc.GetSrcPathId().GetLocalId();
        Y_ABORT_UNLESS(DataShard.GetUserTables().contains(tableId));

        const ui32 localTableId = DataShard.GetUserTables().at(tableId)->LocalTid;
        Y_ABORT_UNLESS(txc.DB.GetScheme().GetTableInfo(localTableId));

        auto* appData = AppData(ctx);
        const auto& columns = DataShard.GetUserTables().at(tableId)->Columns;
        std::shared_ptr<::NKikimr::NDataShard::IExport> exp; // TODO: decouple from export
        Y_UNUSED(exp, appData, columns);

        // if (auto* restoreFactory = appData->DataShardRestoreIncrementalBackupFactory) {
        //     std::shared_ptr<IExport>(restoreFactory->CreateRestore(restoreSrc, columns)).swap(exp);
        // } else {
        //     std::shared_ptr<IExport>(new TTableExport(restoreSrc, columns)).swap(exp);
        //     /*
        //     Abort(op, ctx, "Restore incremental backup are disabled");
        //     return false;
        //     */
        // }

        // auto createUploader = [self = DataShard.SelfId(), txId = op->GetTxId(), exp]() {
        //     return exp->CreateUploader(self, txId);
        // };

        // THolder<IBuffer> buffer{exp->CreateBuffer()};
        THolder<NTable::IScan> scan{CreateDirectReplicationScan(/* std::move(buffer), createUploader */)};

        // FIXME:

        const auto& taskName = appData->DataShardConfig.GetBackupTaskName();
        const auto taskPrio = appData->DataShardConfig.GetBackupTaskPriority();

        ui64 readAheadLo = appData->DataShardConfig.GetBackupReadAheadLo();
        if (ui64 readAheadLoOverride = DataShard.GetBackupReadAheadLoOverride(); readAheadLoOverride > 0) {
            readAheadLo = readAheadLoOverride;
        }

        ui64 readAheadHi = appData->DataShardConfig.GetBackupReadAheadHi();
        if (ui64 readAheadHiOverride = DataShard.GetBackupReadAheadHiOverride(); readAheadHiOverride > 0) {
            readAheadHi = readAheadHiOverride;
        }

        tx->SetScanTask(DataShard.QueueScan(localTableId, scan.Release(), op->GetTxId(),
            TScanOptions()
                .SetResourceBroker(taskName, taskPrio)
                .SetReadAhead(readAheadLo, readAheadHi)
                .SetReadPrio(TScanOptions::EReadPrio::Low)
        ));

        return true;
    }

    bool HasResult(TOperation::TPtr op) const {
        return op->HasScanResult();
    }

    bool ProcessResult(TOperation::TPtr op, const TActorContext&) {
        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

        auto* result = CheckedCast<TExportScanProduct*>(op->ScanResult().Get());
        bool done = true;

        switch (result->Outcome) {
        case EExportOutcome::Success:
        case EExportOutcome::Error:
            if (auto* schemeOp = DataShard.FindSchemaTx(op->GetTxId())) {
                schemeOp->Success = result->Outcome == EExportOutcome::Success;
                schemeOp->Error = std::move(result->Error);
                schemeOp->BytesProcessed = result->BytesRead;
                schemeOp->RowsProcessed = result->RowsRead;
            } else {
                Y_FAIL_S("Cannot find schema tx: " << op->GetTxId());
            }
            break;
        case EExportOutcome::Aborted:
            done = false;
            break;
        }

        op->SetScanResult(nullptr);
        tx->SetScanTask(0);

        return done;
    }

    void Cancel(TActiveTransaction* tx, const TActorContext&) {
        if (!tx->GetScanTask()) {
            return;
        }

        const ui64 tableId = tx->GetSchemeTx().GetBackup().GetTableId();

        Y_ABORT_UNLESS(DataShard.GetUserTables().contains(tableId));
        const ui32 localTableId = DataShard.GetUserTables().at(tableId)->LocalTid;

        DataShard.CancelScan(localTableId, tx->GetScanTask());
        tx->SetScanTask(0);
    }

    void PersistResult(TOperation::TPtr op, TTransactionContext& txc) {
        auto* schemeOp = DataShard.FindSchemaTx(op->GetTxId());
        Y_ABORT_UNLESS(schemeOp);

        NIceDb::TNiceDb db(txc.DB);
        DataShard.PersistSchemeTxResult(db, *schemeOp);
    }

    EExecutionStatus Execute(TOperation::TPtr op, TTransactionContext& txc, const TActorContext& ctx) override final {
        Y_ABORT_UNLESS(op->IsSchemeTx());

        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

        const TString msg = TStringBuilder() << "Got2 " << "<" << tx->IsSchemeTx() << ">" << tx->GetTxBody() << " tx";
        LOG_ERROR_S(TActivationContext::AsActorContext(), NKikimrServices::TX_DATASHARD, msg);

        if (!IsRelevant(tx)) {
            return EExecutionStatus::Executed;
        }

        if (!IsWaiting(op)) {
            LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Starting a " << GetKind() << " operation"
                << " at " << DataShard.TabletID());

            if (!Run(op, txc, ctx)) {
                return EExecutionStatus::Executed;
            }

            SetWaiting(op);
            Y_DEBUG_ABORT_UNLESS(!HasResult(op));
        }

        if (HasResult(op)) {
            LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "" << GetKind() << " complete"
                << " at " << DataShard.TabletID());

            ResetWaiting(op);
            if (ProcessResult(op, ctx)) {
                PersistResult(op, txc);
            } else {
                Y_DEBUG_ABORT_UNLESS(!HasResult(op));
                op->SetWaitingForRestartFlag();
                ctx.Schedule(TDuration::Seconds(1), new TDataShard::TEvPrivate::TEvRestartOperation(op->GetTxId()));
            }
        }

        while (op->HasPendingInputEvents()) {
            ProcessEvent(op->InputEvents().front(), op, ctx);
            op->InputEvents().pop();
        }

        if (IsWaiting(op)) {
            return EExecutionStatus::Continue;
        }

        return EExecutionStatus::Executed;
    }

    bool IsReadyToExecute(TOperation::TPtr op) const override final {
        if (!IsWaiting(op)) {
            return true;
        }

        if (HasResult(op)) {
            return true;
        }

        if (op->HasPendingInputEvents()) {
            return true;
        }

        return false;
    }

    void Complete(TOperation::TPtr, const TActorContext&) override final {
    }

    void ProcessEvent(TAutoPtr<NActors::IEventHandle>& ev, TOperation::TPtr op, const TActorContext& ctx) {
        switch (ev->GetTypeRewrite()) {
            // OHFunc(TEvCancel, Handle);
        }
        Y_UNUSED(op, ctx);
    }

public:
    TRestoreIncrementalBackupSrcUnit(TDataShard& self, TPipeline& pipeline)
        : TExecutionUnit(EExecutionUnitKind::RestoreIncrementalBackupSrc, false, self, pipeline)
    {
    }

}; // TRestoreIncrementalBackupSrcUnit

THolder<TExecutionUnit> CreateRestoreIncrementalBackupSrcUnit(TDataShard& self, TPipeline& pipeline) {
    return THolder(new TRestoreIncrementalBackupSrcUnit(self, pipeline));
}

} // NDataShard
} // NKikimr
