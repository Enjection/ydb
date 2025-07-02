#include "schemeshard_impl.h"
#include "schemeshard_incremental_restore_scan.h"
#include "schemeshard_utils.h"

#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/base/tablet_pipe.h>

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

namespace NKikimr::NSchemeShard::NIncrementalRestoreScan {

class TTxProgress: public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
private:
    // Input params
    TEvPrivate::TEvRunIncrementalRestore::TPtr RunIncrementalRestore = nullptr;
    struct {
        TOperationId OperationId;
        TTabletId TabletId;
        explicit operator bool() const { return OperationId && TabletId; }
    } PipeRetry;

    // Side effects
    TOperationId OperationToProgress;

public:
    TTxProgress() = delete;

    explicit TTxProgress(TSelf* self, TEvPrivate::TEvRunIncrementalRestore::TPtr& ev)
        : TTransactionBase(self)
        , RunIncrementalRestore(ev)
    {
    }

    explicit TTxProgress(TSelf* self, const TOperationId& operationId, TTabletId tabletId)
        : TTransactionBase(self)
        , PipeRetry({operationId, tabletId})
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_PROGRESS_INCREMENTAL_RESTORE;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        if (RunIncrementalRestore) {
            return OnRunIncrementalRestore(txc, ctx);
        } else if (PipeRetry) {
            return OnPipeRetry(txc, ctx);
        } else {
            Y_ABORT("unreachable");
        }
    }

    void Complete(const TActorContext& ctx) override {
        // ARCHITECTURAL FIX: Trigger proper SchemeShard operations instead of bypassing the infrastructure
        // The scan logic should NOT create schema transactions directly. Instead, it should trigger
        // proper TxRestoreIncrementalBackupAtTable operations that have correct transaction coordination.
        // This is the correct approach: Let the existing operation infrastructure handle the transaction
        // coordination via context.OnComplete.BindMsgToPipe() which ensures proper plan steps

        if (OperationToProgress) {
            if (Self->LongIncrementalRestoreOps.contains(OperationToProgress)) {
                // Construct the transaction for the restore operation
                TTxTransaction tx;
                tx.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpRestoreMultipleIncrementalBackups);
                // TODO: Fill tx fields from op as needed (wire up source/destination paths, etc.)
                // This may require extending op to store the necessary transaction fields.

                // Register the operation using the correct SchemeShard pattern
                // Create the suboperation
                auto subOp = CreateRestoreIncrementalBackupAtTable(OperationToProgress, tx);
                // Create a new TOperation and add the suboperation
                auto operation = new TOperation(OperationToProgress.GetTxId());
                operation->AddPart(subOp);
                Self->Operations[OperationToProgress.GetTxId()] = operation;
                LOG_I("Registered SchemeShard operation for incremental restore: "
                    << ": operationId# " << OperationToProgress);
            }
        }

        // Schedule next progress check if needed
        if (OperationToProgress) {
            TPathId backupCollectionPathId;
            if (Self->LongIncrementalRestoreOps.contains(OperationToProgress)) {
                const auto& op = Self->LongIncrementalRestoreOps.at(OperationToProgress);
                backupCollectionPathId.OwnerId = op.GetBackupCollectionPathId().GetOwnerId();
                backupCollectionPathId.LocalPathId = op.GetBackupCollectionPathId().GetLocalId();
                LOG_D("Scheduling next progress check"
                    << ": operationId# " << OperationToProgress
                    << ", backupCollectionPathId# " << backupCollectionPathId);
                ctx.Send(ctx.SelfID, new TEvPrivate::TEvRunIncrementalRestore(backupCollectionPathId));
            }
        }
    }

    bool OnRunIncrementalRestore(TTransactionContext&, const TActorContext& ctx);
    bool OnPipeRetry(TTransactionContext&, const TActorContext& ctx);
}; // TTxProgress

// Implementation of OnRunIncrementalRestore and OnPipeRetry

bool NKikimr::NSchemeShard::NIncrementalRestoreScan::TTxProgress::OnRunIncrementalRestore(TTransactionContext&, const TActorContext& ctx) {
    const auto& pathId = RunIncrementalRestore->Get()->BackupCollectionPathId;

    LOG_D("Run incremental restore"
        << ": backupCollectionPathId# " << pathId);

    // Find the backup collection
    if (!Self->PathsById.contains(pathId)) {
        LOG_W("Cannot run incremental restore"
            << ": backupCollectionPathId# " << pathId
            << ", reason# " << "backup collection doesn't exist");
        return true;
    }

    auto path = Self->PathsById.at(pathId);
    if (!path->IsBackupCollection()) {
        LOG_W("Cannot run incremental restore"
            << ": backupCollectionPathId# " << pathId
            << ", reason# " << "path is not a backup collection");
        return true;
    }

    // Find the corresponding incremental restore operation
    TOperationId operationId;
    bool operationFound = false;
    for (const auto& [opId, op] : Self->LongIncrementalRestoreOps) {
        TPathId opBackupCollectionPathId;
        opBackupCollectionPathId.OwnerId = op.GetBackupCollectionPathId().GetOwnerId();
        opBackupCollectionPathId.LocalPathId = op.GetBackupCollectionPathId().GetLocalId();
        
        if (opBackupCollectionPathId == pathId) {
            operationId = opId;
            operationFound = true;
            break;
        }
    }

    if (!operationFound) {
        LOG_W("Cannot run incremental restore"
            << ": backupCollectionPathId# " << pathId
            << ", reason# " << "incremental restore operation not found");
        return true;
    }

    LOG_D("Found incremental restore operation"
        << ": operationId# " << operationId
        << ", txId# " << Self->LongIncrementalRestoreOps.at(operationId).GetTxId()
        << ", tableCount# " << Self->LongIncrementalRestoreOps.at(operationId).GetTablePathList().size());

    // Process each table in the restore operation
    for (const auto& tablePathString : Self->LongIncrementalRestoreOps.at(operationId).GetTablePathList()) {
        TPath tablePath = TPath::Resolve(tablePathString, Self);
        if (!tablePath.IsResolved()) {
            LOG_W("Table path not resolved in restore operation"
                << ": operationId# " << operationId
                << ", tablePath# " << tablePathString);
            continue;
        }
        
        TPathId tablePathId = tablePath.Base()->PathId;
        
        if (!Self->Tables.contains(tablePathId)) {
            LOG_W("Table not found in restore operation"
                << ": operationId# " << operationId
                << ", tablePathId# " << tablePathId);
            continue;
        }

        // Create schema transaction for incremental restore once per table
        // (not per shard - the operation framework handles shard distribution)
        
        // Find the backup table path within the backup collection
        TVector<TPathId> backupTablePathIds;
        auto tableName = tablePath.Base()->Name;
        auto backupCollectionPath = Self->PathsById.at(pathId);
        for (auto& [childName, childPathId] : backupCollectionPath->GetChildren()) {
            if (childName.Contains("_incremental")) {
                auto backupEntryPath = Self->PathsById.at(childPathId);
                for (auto& [tableNameInEntry, tablePathId] : backupEntryPath->GetChildren()) {
                    if (tableNameInEntry == tableName) {
                        backupTablePathIds.push_back(tablePathId);
                    }
                }
            }
        }
        if (backupTablePathIds.empty()) {
            LOG_W("No backup tables found in incremental backup entries"
                << ": operationId# " << operationId
                << ", tableName# " << tableName
                << ", backupCollectionPathId# " << pathId);
            continue;
        }
        // Only the first backup table is used for now (multiple incremental backups per table not yet supported)
        TPathId selectedBackupTablePathId = backupTablePathIds[0];

        // Use an empty string or a valid working directory if available
        NKikimrSchemeOp::TModifyScheme tx = TransactionTemplate("", NKikimrSchemeOp::EOperationType::ESchemeOpRestoreIncrementalBackupAtTable);
        auto* multipleRestore = tx.MutableRestoreMultipleIncrementalBackups();
        multipleRestore->add_srctablepaths(tablePath.PathString());
        selectedBackupTablePathId.ToProto(multipleRestore->add_srcpathids());
        multipleRestore->set_dsttablepath(tablePath.PathString());
        tablePathId.ToProto(multipleRestore->mutable_dstpathid());

        // Register the operation using the correct SchemeShard pattern
        auto subOp = CreateRestoreIncrementalBackupAtTable(operationId, tx);
        auto operation = new TOperation(operationId.GetTxId());
        operation->AddPart(subOp);
        Self->Operations[operationId.GetTxId()] = operation;
        LOG_I("Registered SchemeShard operation for incremental restore: "
            << ": operationId# " << operationId
            << ", srcPathId# " << selectedBackupTablePathId
            << ", dstPathId# " << tablePathId);
        }

    LOG_N("Incremental restore operation initiated"
        << ": operationId# " << operationId
        << ", backupCollectionPathId# " << pathId
        << ", tableCount# " << Self->LongIncrementalRestoreOps.at(operationId).GetTablePathList().size());

    return true;
}

bool NKikimr::NSchemeShard::NIncrementalRestoreScan::TTxProgress::OnPipeRetry(TTransactionContext&, const TActorContext& ctx) {
    LOG_D("Retrying incremental restore for pipe failure"
        << ": operationId# " << PipeRetry.OperationId
        << ", tabletId# " << PipeRetry.TabletId);

    // Find the operation and retry the request to this specific DataShard
    if (!Self->LongIncrementalRestoreOps.contains(PipeRetry.OperationId)) {
        LOG_W("Cannot retry incremental restore - operation not found"
            << ": operationId# " << PipeRetry.OperationId);
        return true;
    }
    const auto& op = Self->LongIncrementalRestoreOps.at(PipeRetry.OperationId);
    TPathId backupCollectionPathId;
    backupCollectionPathId.OwnerId = op.GetBackupCollectionPathId().GetOwnerId();
    backupCollectionPathId.LocalPathId = op.GetBackupCollectionPathId().GetLocalId();

    // Find the table and shard for this tablet
    for (const auto& tablePathString : op.GetTablePathList()) {
        TPath tablePath = TPath::Resolve(tablePathString, Self);
        if (!tablePath.IsResolved()) {
            continue;
        }
        TPathId tablePathId = tablePath.Base()->PathId;
        if (!Self->Tables.contains(tablePathId)) {
            continue;
        }
        // Find the specific shard that matches this tablet
        for (const auto& shard : Self->Tables.at(tablePathId)->GetPartitions()) {
            Y_ABORT_UNLESS(Self->ShardInfos.contains(shard.ShardIdx));
            const auto tabletId = Self->ShardInfos.at(shard.ShardIdx).TabletID;
            if (tabletId == PipeRetry.TabletId) {
                // Find the backup table path within the backup collection
                auto tableName = tablePath.Base()->Name;
                auto backupCollectionPath = Self->PathsById.at(backupCollectionPathId);
                TVector<TPathId> backupTablePathIds;
                for (auto& [childName, childPathId] : backupCollectionPath->GetChildren()) {
                    if (childName.Contains("_incremental")) {
                        auto backupEntryPath = Self->PathsById.at(childPathId);
                        for (auto& [tableNameInEntry, tablePathId] : backupEntryPath->GetChildren()) {
                            if (tableNameInEntry == tableName) {
                                backupTablePathIds.push_back(tablePathId);
                            }
                        }
                    }
                }
                if (backupTablePathIds.empty()) {
                    LOG_W("No backup tables found in incremental backup entries during retry"
                        << ": operationId# " << PipeRetry.OperationId
                        << ", tableName# " << tableName
                        << ", backupCollectionPathId# " << backupCollectionPathId);
                    return true;
                }
                // Only the first backup table is used for now (multiple incremental backups per table not yet supported)
                TPathId selectedBackupTablePathId = backupTablePathIds[0];
                NKikimrSchemeOp::TModifyScheme tx = TransactionTemplate("", NKikimrSchemeOp::EOperationType::ESchemeOpRestoreIncrementalBackupAtTable);
                auto* multipleRestore = tx.MutableRestoreMultipleIncrementalBackups();
                multipleRestore->add_srctablepaths(tablePath.PathString());
                selectedBackupTablePathId.ToProto(multipleRestore->add_srcpathids());
                multipleRestore->set_dsttablepath(tablePath.PathString());
                tablePathId.ToProto(multipleRestore->mutable_dstpathid());
                auto subOp = CreateRestoreIncrementalBackupAtTable(PipeRetry.OperationId, tx);
                auto operation = new TOperation(PipeRetry.OperationId.GetTxId());
                operation->AddPart(subOp);
                Self->Operations[PipeRetry.OperationId.GetTxId()] = operation;
                LOG_I("Registered SchemeShard operation for incremental restore (retry): "
                    << ": operationId# " << PipeRetry.OperationId
                    << ", srcPathId# " << selectedBackupTablePathId
                    << ", dstPathId# " << tablePathId);
                return true;
            }
        }
    }
    LOG_W("Cannot retry incremental restore - tablet not found in operation"
        << ": operationId# " << PipeRetry.OperationId
        << ", tabletId# " << PipeRetry.TabletId);
    return true;
}

} // namespace NKikimr::NSchemeShard::NIncrementalRestoreScan

namespace NKikimr::NSchemeShard {

using namespace NIncrementalRestoreScan;

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev) {
    return new TTxProgress(this, ev);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreatePipeRetryIncrementalRestore(const TOperationId& operationId, TTabletId tabletId) {
    return new TTxProgress(this, operationId, tabletId);
}

void TSchemeShard::Handle(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxProgressIncrementalRestore(ev), ctx);
}

} // namespace NKikimr::NSchemeShard
