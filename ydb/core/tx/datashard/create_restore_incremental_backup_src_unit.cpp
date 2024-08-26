#include "defs.h"
#include "execution_unit_ctors.h"
#include "datashard_active_transaction.h"
#include "datashard_impl.h"

namespace NKikimr {
namespace NDataShard {

using namespace NKikimrTxDataShard;

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

    bool Run(TOperation::TPtr op, TTransactionContext& txc, const TActorContext& ctx) {
        Y_UNUSED(op, txc, ctx);
        // TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        // Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

        // Y_ABORT_UNLESS(tx->GetSchemeTx().HasBackup());
        // const auto& backup = tx->GetSchemeTx().GetBackup();

        // const ui64 tableId = backup.GetTableId();
        // Y_ABORT_UNLESS(DataShard.GetUserTables().contains(tableId));

        // const ui32 localTableId = DataShard.GetUserTables().at(tableId)->LocalTid;
        // Y_ABORT_UNLESS(txc.DB.GetScheme().GetTableInfo(localTableId));

        // auto* appData = AppData(ctx);
        // const auto& columns = DataShard.GetUserTables().at(tableId)->Columns;
        // std::shared_ptr<::NKikimr::NDataShard::IExport> exp;

        // if (backup.HasYTSettings()) {
        //     if (backup.HasCompression()) {
        //         Abort(op, ctx, "Exports to YT do not support compression");
        //         return false;
        //     }

        //     if (auto* exportFactory = appData->DataShardExportFactory) {
        //         std::shared_ptr<IExport>(exportFactory->CreateExportToYt(backup, columns)).swap(exp);
        //     } else {
        //         Abort(op, ctx, "Exports to YT are disabled");
        //         return false;
        //     }
        // } else if (backup.HasS3Settings()) {
        //     NBackupRestoreTraits::ECompressionCodec codec;
        //     if (!TryCodecFromTask(backup, codec)) {
        //         Abort(op, ctx, TStringBuilder() << "Unsupported compression codec"
        //             << ": " << backup.GetCompression().GetCodec());
        //         return false;
        //     }

        //     if (auto* exportFactory = appData->DataShardExportFactory) {
        //         std::shared_ptr<IExport>(exportFactory->CreateExportToS3(backup, columns)).swap(exp);
        //     } else {
        //         Abort(op, ctx, "Exports to S3 are disabled");
        //         return false;
        //     }
        // } else {
        //     Abort(op, ctx, "Unsupported backup task");
        //     return false;
        // }

        // auto createUploader = [self = DataShard.SelfId(), txId = op->GetTxId(), exp]() {
        //     return exp->CreateUploader(self, txId);
        // };

        // THolder<IBuffer> buffer{exp->CreateBuffer()};
        // THolder<NTable::IScan> scan{CreateExportScan(std::move(buffer), createUploader)};

        // const auto& taskName = appData->DataShardConfig.GetBackupTaskName();
        // const auto taskPrio = appData->DataShardConfig.GetBackupTaskPriority();

        // ui64 readAheadLo = appData->DataShardConfig.GetBackupReadAheadLo();
        // if (ui64 readAheadLoOverride = DataShard.GetBackupReadAheadLoOverride(); readAheadLoOverride > 0) {
        //     readAheadLo = readAheadLoOverride;
        // }

        // ui64 readAheadHi = appData->DataShardConfig.GetBackupReadAheadHi();
        // if (ui64 readAheadHiOverride = DataShard.GetBackupReadAheadHiOverride(); readAheadHiOverride > 0) {
        //     readAheadHi = readAheadHiOverride;
        // }

        // tx->SetScanTask(DataShard.QueueScan(localTableId, scan.Release(), op->GetTxId(),
        //     TScanOptions()
        //         .SetResourceBroker(taskName, taskPrio)
        //         .SetReadAhead(readAheadLo, readAheadHi)
        //         .SetReadPrio(TScanOptions::EReadPrio::Low)
        // ));

        return true;
    }

    bool HasResult(TOperation::TPtr op) const {
        return op->HasScanResult();
    }

    bool ProcessResult(TOperation::TPtr op, const TActorContext&) {
        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

        // auto* result = CheckedCast<TExportScanProduct*>(op->ScanResult().Get());
        // bool done = true;

        // switch (result->Outcome) {
        // case EExportOutcome::Success:
        // case EExportOutcome::Error:
        //     if (auto* schemeOp = DataShard.FindSchemaTx(op->GetTxId())) {
        //         schemeOp->Success = result->Outcome == EExportOutcome::Success;
        //         schemeOp->Error = std::move(result->Error);
        //         schemeOp->BytesProcessed = result->BytesRead;
        //         schemeOp->RowsProcessed = result->RowsRead;
        //     } else {
        //         Y_FAIL_S("Cannot find schema tx: " << op->GetTxId());
        //     }
        //     break;
        // case EExportOutcome::Aborted:
        //     done = false;
        //     break;
        // }

        // op->SetScanResult(nullptr);
        // tx->SetScanTask(0);

        // return done;
        return true;
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
        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

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
        Y_UNUSED(op,ctx);
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
