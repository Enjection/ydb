#include "schemeshard_impl.h"
#include "schemeshard_incremental_restore_scan.h"
#include "schemeshard_utils.h"

#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/tx/tx_allocator_client/client.h>

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

// Propose function following export system pattern
THolder<TEvSchemeShard::TEvModifySchemeTransaction> IncrementalRestorePropose(
    TSchemeShard* ss,
    TTxId txId,
    const TPathId& sourcePathId,
    const TPathId& destPathId,
    const TString& srcTablePath,
    const TString& dstTablePath
) {
    auto propose = MakeHolder<TEvSchemeShard::TEvModifySchemeTransaction>(ui64(txId), ss->TabletID());

    auto& modifyScheme = *propose->Record.AddTransaction();
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpRestoreMultipleIncrementalBackups);
    modifyScheme.SetInternal(true);
    
    // Set WorkingDir - use parent directory of destination table
    TString workingDir = "/";
    if (auto pos = dstTablePath.rfind('/'); pos != TString::npos && pos > 0) {
        workingDir = dstTablePath.substr(0, pos);
    }
    modifyScheme.SetWorkingDir(workingDir);

    auto& restore = *modifyScheme.MutableRestoreMultipleIncrementalBackups();
    restore.add_srctablepaths(srcTablePath);
    sourcePathId.ToProto(restore.add_srcpathids());
    restore.set_dsttablepath(dstTablePath);
    destPathId.ToProto(restore.mutable_dstpathid());

    return propose;
}

class TTxProgress: public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
private:
    // Input params
    TEvPrivate::TEvRunIncrementalRestore::TPtr RunIncrementalRestore = nullptr;
    struct {
        TOperationId OperationId;
        TTabletId TabletId;
        explicit operator bool() const { return OperationId && TabletId; }
    } PipeRetry;

    // Transaction lifecycle support (following export pattern)
    TEvTxAllocatorClient::TEvAllocateResult::TPtr AllocateResult = nullptr;
    TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr ModifyResult = nullptr;
    TTxId CompletedTxId = InvalidTxId;

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

    // Transaction lifecycle constructors (following export pattern)
    explicit TTxProgress(TSelf* self, TEvTxAllocatorClient::TEvAllocateResult::TPtr& ev)
        : TTransactionBase(self)
        , AllocateResult(ev)
    {
    }

    explicit TTxProgress(TSelf* self, TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr& ev)
        : TTransactionBase(self)
        , ModifyResult(ev)
    {
    }

    explicit TTxProgress(TSelf* self, TTxId completedTxId)
        : TTransactionBase(self)
        , CompletedTxId(completedTxId)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_PROGRESS_INCREMENTAL_RESTORE;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        if (AllocateResult) {
            return OnAllocateResult(txc, ctx);
        } else if (ModifyResult) {
            return OnModifyResult(txc, ctx);
        } else if (CompletedTxId) {
            return OnNotifyResult(txc, ctx);
        } else if (RunIncrementalRestore) {
            return OnRunIncrementalRestore(txc, ctx);
        } else if (PipeRetry) {
            return OnPipeRetry(txc, ctx);
        } else {
            Y_ABORT("unreachable");
        }
    }

    void Complete(const TActorContext& ctx) override {
        // NOTE: Operations are now created and scheduled directly in Execute methods
        // using Self->Execute(CreateRestoreIncrementalBackupAtTable(newOperationId, newTx), ctx)
        // This ensures proper SchemeShard operation coordination with plan steps.
        
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
    
    // Transaction lifecycle methods (following export pattern)
    bool OnAllocateResult(TTransactionContext& txc, const TActorContext& ctx);
    bool OnModifyResult(TTransactionContext& txc, const TActorContext& ctx);
    bool OnNotifyResult(TTransactionContext& txc, const TActorContext& ctx);
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
        NKikimrSchemeOp::TModifyScheme tx = TransactionTemplate("", NKikimrSchemeOp::EOperationType::ESchemeOpRestoreMultipleIncrementalBackups);
        auto* multipleRestore = tx.MutableRestoreMultipleIncrementalBackups();
        multipleRestore->add_srctablepaths(tablePath.PathString());
        selectedBackupTablePathId.ToProto(multipleRestore->add_srcpathids());
        multipleRestore->set_dsttablepath(tablePath.PathString());
        tablePathId.ToProto(multipleRestore->mutable_dstpathid());

        // Create a NEW unique operation for this incremental restore (don't reuse the backup collection operation ID)
        ui64 newOperationId = ui64(Self->GetCachedTxId(ctx));
        TTxTransaction newTx;
        newTx.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpRestoreMultipleIncrementalBackups);
        auto* newMultipleRestore = newTx.MutableRestoreMultipleIncrementalBackups();
        newMultipleRestore->add_srctablepaths(tablePath.PathString());
        selectedBackupTablePathId.ToProto(newMultipleRestore->add_srcpathids());
        newMultipleRestore->set_dsttablepath(tablePath.PathString());
        tablePathId.ToProto(newMultipleRestore->mutable_dstpathid());

        // Store context for transaction lifecycle
        TSchemeShard::TIncrementalRestoreContext context;
        context.SourceBackupTablePathId = selectedBackupTablePathId;
        context.DestinationTablePathId = tablePathId;
        context.SourceTablePath = tablePath.PathString();
        context.DestinationTablePath = tablePath.PathString();
        context.OriginalOperationId = ui64(operationId.GetTxId());
        Self->IncrementalRestoreContexts[newOperationId] = context;

        // Use proper transaction pattern following export system
        ctx.Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate(), 0, newOperationId);
        LOG_I("Requested transaction allocation for incremental restore: "
            << ": newOperationId# " << newOperationId
            << ", originalOperationId# " << operationId
            << ", srcPathId# " << selectedBackupTablePathId
            << ", dstPathId# " << tablePathId);
        }

    LOG_N("Incremental restore operation initiated"
        << ": operationId# " << operationId
        << ", backupCollectionPathId# " << pathId
        << ", tableCount# " << Self->LongIncrementalRestoreOps.at(operationId).GetTablePathList().size());

    return true;
}

bool NKikimr::NSchemeShard::NIncrementalRestoreScan::TTxProgress::OnPipeRetry(TTransactionContext& txc, const TActorContext& ctx) {
    Y_UNUSED(txc);
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
                // Create a NEW unique operation for this incremental restore retry (don't reuse the original operation ID)
                ui64 newOperationId = ui64(Self->GetCachedTxId(ctx));
                TTxTransaction newTx;
                newTx.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpRestoreMultipleIncrementalBackups);
                auto* newMultipleRestore = newTx.MutableRestoreMultipleIncrementalBackups();
                newMultipleRestore->add_srctablepaths(tablePath.PathString());
                selectedBackupTablePathId.ToProto(newMultipleRestore->add_srcpathids());
                newMultipleRestore->set_dsttablepath(tablePath.PathString());
                tablePathId.ToProto(newMultipleRestore->mutable_dstpathid());

                // Store context for transaction lifecycle (retry case)
                TSchemeShard::TIncrementalRestoreContext context;
                context.SourceBackupTablePathId = selectedBackupTablePathId;
                context.DestinationTablePathId = tablePathId;
                context.SourceTablePath = tablePath.PathString();
                context.DestinationTablePath = tablePath.PathString();
                context.OriginalOperationId = ui64(PipeRetry.OperationId.GetTxId());
                Self->IncrementalRestoreContexts[newOperationId] = context;

                // Use proper transaction pattern following export system
                ctx.Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate(), 0, newOperationId);
                LOG_I("Requested transaction allocation for incremental restore (retry): "
                    << ": newOperationId# " << newOperationId
                    << ", originalOperationId# " << PipeRetry.OperationId
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

// Transaction lifecycle methods (following export pattern)

bool NKikimr::NSchemeShard::NIncrementalRestoreScan::TTxProgress::OnAllocateResult(TTransactionContext& txc, const TActorContext& ctx) {
    Y_UNUSED(txc);
    Y_ABORT_UNLESS(AllocateResult);

    const auto txId = TTxId(AllocateResult->Get()->TxIds.front());
    const ui64 operationId = AllocateResult->Cookie;

    LOG_D("TTxProgress: OnAllocateResult"
        << ": txId# " << txId
        << ", operationId# " << operationId);

    if (!Self->IncrementalRestoreContexts.contains(operationId)) {
        LOG_E("TTxProgress: OnAllocateResult received unknown operationId"
            << ": operationId# " << operationId);
        return true;
    }

    const auto& context = Self->IncrementalRestoreContexts.at(operationId);
    
    // Send propose message with proper context
    auto propose = IncrementalRestorePropose(
        Self, 
        txId, 
        context.SourceBackupTablePathId, 
        context.DestinationTablePathId,
        context.SourceTablePath,
        context.DestinationTablePath
    );
    
    ctx.Send(Self->SelfId(), propose.Release());
    
    // Track transaction for completion handling
    Self->TxIdToIncrementalRestore[txId] = operationId;
    
    LOG_I("TTxProgress: Sent incremental restore propose"
        << ": txId# " << txId
        << ", operationId# " << operationId
        << ", srcPathId# " << context.SourceBackupTablePathId
        << ", dstPathId# " << context.DestinationTablePathId);
    
    return true;
}

bool NKikimr::NSchemeShard::NIncrementalRestoreScan::TTxProgress::OnModifyResult(TTransactionContext& txc, const TActorContext& ctx) {
    Y_UNUSED(txc);
    Y_UNUSED(ctx);
    Y_ABORT_UNLESS(ModifyResult);
    const auto& record = ModifyResult->Get()->Record;

    LOG_D("TTxProgress: OnModifyResult"
        << ": txId# " << record.GetTxId()
        << ", status# " << record.GetStatus());

    auto txId = TTxId(record.GetTxId());
    
    if (!Self->TxIdToIncrementalRestore.contains(txId)) {
        LOG_E("TTxProgress: OnModifyResult received unknown txId"
            << ": txId# " << txId);
        return true;
    }
    
    ui64 operationId = Self->TxIdToIncrementalRestore.at(txId);
    
    if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
        LOG_I("TTxProgress: Incremental restore transaction accepted"
            << ": txId# " << txId
            << ", operationId# " << operationId);
        
        // Transaction subscription is automatic - when txId is added to TxInFlight
        // and tracked in Operations, completion notifications will be sent automatically
        // No explicit subscription needed since we have TxIdToIncrementalRestore mapping
    } else {
        LOG_W("TTxProgress: Incremental restore transaction rejected"
            << ": txId# " << txId
            << ", operationId# " << operationId
            << ", status# " << record.GetStatus());
        
        // Clean up tracking on rejection
        Self->TxIdToIncrementalRestore.erase(txId);
        Self->IncrementalRestoreContexts.erase(operationId);
    }

    return true;
}

bool NKikimr::NSchemeShard::NIncrementalRestoreScan::TTxProgress::OnNotifyResult(TTransactionContext& txc, const TActorContext& ctx) {
    Y_UNUSED(txc);
    Y_UNUSED(ctx);
    LOG_D("TTxProgress: OnNotifyResult"
        << ": completedTxId# " << CompletedTxId);

    if (!Self->TxIdToIncrementalRestore.contains(CompletedTxId)) {
        LOG_W("TTxProgress: OnNotifyResult received unknown txId"
            << ": txId# " << CompletedTxId);
        return true;
    }
    
    ui64 operationId = Self->TxIdToIncrementalRestore.at(CompletedTxId);

    LOG_I("TTxProgress: Incremental restore transaction completed"
        << ": txId# " << CompletedTxId
        << ", operationId# " << operationId);

    // Clean up tracking and context
    Self->TxIdToIncrementalRestore.erase(CompletedTxId);
    Self->IncrementalRestoreContexts.erase(operationId);

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

// Transaction lifecycle constructor functions (following export pattern)
NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvTxAllocatorClient::TEvAllocateResult::TPtr& ev) {
    return new TTxProgress(this, ev);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TEvSchemeShard::TEvModifySchemeTransactionResult::TPtr& ev) {
    return new TTxProgress(this, ev);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxProgressIncrementalRestore(TTxId completedTxId) {
    return new TTxProgress(this, completedTxId);
}

void TSchemeShard::Handle(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxProgressIncrementalRestore(ev), ctx);
}

} // namespace NKikimr::NSchemeShard
