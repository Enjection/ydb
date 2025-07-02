#include "schemeshard_impl.h"
#include "schemeshard_incremental_restore_scan.h"

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
    TDeque<std::tuple<TOperationId, TTabletId, THolder<TEvDataShard::TEvProposeTransaction>>> RestoreRequests;
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
        // Send restore requests to DataShards
        for (auto& [operationId, tabletId, ev] : RestoreRequests) {
            LOG_D("Sending restore request to DataShard"
                << ": operationId# " << operationId
                << ", tabletId# " << tabletId);
            
            Self->PipeClientCache->Send(ctx, ui64(tabletId), ev.Release());
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

private:
    bool OnRunIncrementalRestore(TTransactionContext&, const TActorContext& ctx) {
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

        const auto& op = Self->LongIncrementalRestoreOps.at(operationId);

        LOG_D("Found incremental restore operation"
            << ": operationId# " << operationId
            << ", txId# " << op.GetTxId()
            << ", tableCount# " << op.GetTablePathList().size());

        // Process each table in the restore operation
        for (const auto& tablePathString : op.GetTablePathList()) {
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

            auto table = Self->Tables.at(tablePathId);
            
            // Send restore request to each shard of the table
            for (const auto& shard : table->GetPartitions()) {
                Y_ABORT_UNLESS(Self->ShardInfos.contains(shard.ShardIdx));
                const auto tabletId = Self->ShardInfos.at(shard.ShardIdx).TabletID;

                // Create schema transaction with TRestoreIncrementalBackup
                NKikimrSchemeOp::TRestoreIncrementalBackup restoreBackup;
                
                // Find the backup table path within the backup collection
                auto tableName = tablePath.Base()->Name;
                auto backupCollectionPath = Self->PathsById.at(pathId);
                
                // Look for the backup table within incremental backup entries
                TVector<TPathId> backupTablePathIds;
                
                // Debug: List all children of the backup collection
                LOG_D("Backup collection children"
                    << ": operationId# " << operationId
                    << ", backupCollectionPathId# " << pathId
                    << ", lookingForTable# " << tableName);
                
                // Find incremental backup entries and look for the table within them
                for (auto& [childName, childPathId] : backupCollectionPath->GetChildren()) {
                    LOG_D("Found backup collection child"
                        << ": operationId# " << operationId
                        << ", childName# " << childName
                        << ", childPathId# " << childPathId);
                    
                    // Check if this is an incremental backup entry (contains "_incremental")
                    if (childName.Contains("_incremental")) {
                        LOG_D("Examining incremental backup entry"
                            << ": operationId# " << operationId
                            << ", entryName# " << childName
                            << ", entryPathId# " << childPathId);
                        
                        // Look for the table within this incremental backup entry
                        auto backupEntryPath = Self->PathsById.at(childPathId);
                        for (auto& [tableNameInEntry, tablePathId] : backupEntryPath->GetChildren()) {
                            LOG_D("Found table in incremental backup"
                                << ": operationId# " << operationId
                                << ", entryName# " << childName
                                << ", tableName# " << tableNameInEntry
                                << ", tablePathId# " << tablePathId);
                            
                            if (tableNameInEntry == tableName) {
                                backupTablePathIds.push_back(tablePathId);
                                LOG_I("Found matching backup table: operationId# " << operationId
                                    << ", backupEntry# " << childName
                                    << ", tableName# " << tableName
                                    << ", backupTablePathId# " << tablePathId);
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
                
                LOG_I("Found " << backupTablePathIds.size() << " incremental backup tables"
                    << ": operationId# " << operationId
                    << ", tableName# " << tableName);
                
                // For now, use the first backup table (we'll process multiple later if needed)
                TPathId selectedBackupTablePathId = backupTablePathIds[0];
                
                // Set correct paths: SrcPathId = backup table, DstPathId = destination table
                selectedBackupTablePathId.ToProto(restoreBackup.MutableSrcPathId());
                tablePathId.ToProto(restoreBackup.MutableDstPathId());

                // Create schema transaction body
                NKikimrTxDataShard::TFlatSchemeTransaction tx;
                tx.MutableCreateIncrementalRestoreSrc()->CopyFrom(restoreBackup);

                LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                           "[IncrementalRestore] SCHEMA_DEBUG: Creating schema transaction with CreateIncrementalRestoreSrc"
                           << " srcPathId=" << restoreBackup.GetSrcPathId().DebugString()
                           << " dstPathId=" << restoreBackup.GetDstPathId().DebugString()
                           << " hasCreateIncrementalRestoreSrc=" << tx.HasCreateIncrementalRestoreSrc());

                // Set proper sequence number
                auto seqNo = Self->NextRound();
                Self->FillSeqNo(tx, seqNo);

                TString txBody;
                Y_ABORT_UNLESS(tx.SerializeToString(&txBody));

                // Create proper schema transaction proposal
                auto proposal = Self->MakeDataShardProposal(tablePathId, operationId, txBody, ctx);
                
                RestoreRequests.emplace_back(operationId, tabletId, std::move(proposal));
                
                LOG_D("Scheduled restore request"
                    << ": operationId# " << operationId
                    << ", tablePathId# " << tablePathId
                    << ", shardIdx# " << shard.ShardIdx
                    << ", tabletId# " << tabletId);
            }
        }

        LOG_N("Incremental restore operation initiated"
            << ": operationId# " << operationId
            << ", backupCollectionPathId# " << pathId
            << ", tableCount# " << op.GetTablePathList().size()
            << ", requestCount# " << RestoreRequests.size());

        return true;
    }

    bool OnPipeRetry(TTransactionContext&, const TActorContext& ctx) {
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

            auto table = Self->Tables.at(tablePathId);
            
            // Find the specific shard that matches this tablet
            for (const auto& shard : table->GetPartitions()) {
                Y_ABORT_UNLESS(Self->ShardInfos.contains(shard.ShardIdx));
                const auto tabletId = Self->ShardInfos.at(shard.ShardIdx).TabletID;

                if (tabletId == PipeRetry.TabletId) {
                    // Create schema transaction with TRestoreIncrementalBackup
                    NKikimrSchemeOp::TRestoreIncrementalBackup restoreBackup;
                    
                    // Find the backup table path within the backup collection
                    auto tableName = tablePath.Base()->Name;
                    
                    // Get backup collection path from the operation
                    TPathId backupCollectionPathId;
                    backupCollectionPathId.OwnerId = op.GetBackupCollectionPathId().GetOwnerId();
                    backupCollectionPathId.LocalPathId = op.GetBackupCollectionPathId().GetLocalId();
                    auto backupCollectionPath = Self->PathsById.at(backupCollectionPathId);
                    
                    // Look for the backup table within incremental backup entries
                    TVector<TPathId> backupTablePathIds;
                    
                    // Debug: List all children of the backup collection during retry
                    LOG_D("Backup collection children during retry"
                        << ": operationId# " << PipeRetry.OperationId
                        << ", backupCollectionPathId# " << backupCollectionPathId
                        << ", lookingForTable# " << tableName);
                    
                    // Find incremental backup entries and look for the table within them
                    for (auto& [childName, childPathId] : backupCollectionPath->GetChildren()) {
                        LOG_D("Found backup collection child during retry"
                            << ": operationId# " << PipeRetry.OperationId
                            << ", childName# " << childName
                            << ", childPathId# " << childPathId);
                        
                        // Check if this is an incremental backup entry (contains "_incremental")
                        if (childName.Contains("_incremental")) {
                            LOG_D("Examining incremental backup entry during retry"
                                << ": operationId# " << PipeRetry.OperationId
                                << ", entryName# " << childName
                                << ", entryPathId# " << childPathId);
                            
                            // Look for the table within this incremental backup entry
                            auto backupEntryPath = Self->PathsById.at(childPathId);
                            for (auto& [tableNameInEntry, tablePathId] : backupEntryPath->GetChildren()) {
                                LOG_D("Found table in incremental backup during retry"
                                    << ": operationId# " << PipeRetry.OperationId
                                    << ", entryName# " << childName
                                    << ", tableName# " << tableNameInEntry
                                    << ", tablePathId# " << tablePathId);
                                
                                if (tableNameInEntry == tableName) {
                                    backupTablePathIds.push_back(tablePathId);
                                    LOG_I("Found matching backup table during retry: operationId# " << PipeRetry.OperationId
                                        << ", backupEntry# " << childName
                                        << ", tableName# " << tableName
                                        << ", backupTablePathId# " << tablePathId);
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
                    
                    // For now, use the first backup table (we'll process multiple later if needed)
                    TPathId selectedBackupTablePathId = backupTablePathIds[0];
                    
                    // Set correct paths: SrcPathId = backup table, DstPathId = destination table
                    selectedBackupTablePathId.ToProto(restoreBackup.MutableSrcPathId());
                    tablePathId.ToProto(restoreBackup.MutableDstPathId());

                    // Create schema transaction body
                    NKikimrTxDataShard::TFlatSchemeTransaction tx;
                    tx.MutableCreateIncrementalRestoreSrc()->CopyFrom(restoreBackup);

                    // Set proper sequence number
                    auto seqNo = Self->NextRound();
                    Self->FillSeqNo(tx, seqNo);

                    TString txBody;
                    Y_ABORT_UNLESS(tx.SerializeToString(&txBody));

                    // Create proper schema transaction proposal
                    auto proposal = Self->MakeDataShardProposal(tablePathId, PipeRetry.OperationId, txBody, ctx);
                    
                    RestoreRequests.emplace_back(PipeRetry.OperationId, tabletId, std::move(proposal));
                    
                    LOG_D("Scheduled retry restore request"
                        << ": operationId# " << PipeRetry.OperationId
                        << ", tablePathId# " << tablePathId
                        << ", shardIdx# " << shard.ShardIdx
                        << ", tabletId# " << tabletId);
                    
                    return true;
                }
            }
        }

        LOG_W("Cannot retry incremental restore - tablet not found in operation"
            << ": operationId# " << PipeRetry.OperationId
            << ", tabletId# " << PipeRetry.TabletId);
        
        return true;
    }
}; // TTxProgress

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
