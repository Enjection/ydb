#include "schemeshard__operation_common.h"
#include "schemeshard_private.h"

#include <ydb/core/base/hive.h>
#include <ydb/core/tx/datashard/datashard.h>


namespace NKikimr::NSchemeShard::NCdcStreamState {

namespace {

bool IsExpectedTxType(TTxState::ETxType txType) {
    switch (txType) {
    case TTxState::TxCreateCdcStreamAtTable:
    case TTxState::TxCreateCdcStreamAtTableWithInitialScan:
    case TTxState::TxAlterCdcStreamAtTable:
    case TTxState::TxAlterCdcStreamAtTableDropSnapshot:
    case TTxState::TxDropCdcStreamAtTable:
    case TTxState::TxDropCdcStreamAtTableDropSnapshot:
    case TTxState::TxRotateCdcStreamAtTable:
        return true;
    default:
        return false;
    }
}

}  // namespace anonymous


// NCdcStreamState::TConfigurePartsAtTable
//
TConfigurePartsAtTable::TConfigurePartsAtTable(TOperationId id)
    : OperationId(id)
{
    IgnoreMessages(DebugHint(), {});
}

bool TConfigurePartsAtTable::ProgressState(TOperationContext& context) {
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " ProgressState"
                            << ", at schemeshard: " << context.SS->SelfTabletId());

    auto* txState = context.SS->FindTx(OperationId);
    Y_ABORT_UNLESS(txState);
    Y_ABORT_UNLESS(IsExpectedTxType(txState->TxType));
    const auto& pathId = txState->TargetPathId;

    if (NTableState::CheckPartitioningChangedForTableModification(*txState, context)) {
        NTableState::UpdatePartitioningForTableModification(OperationId, *txState, context);
    }

    NKikimrTxDataShard::TFlatSchemeTransaction tx;
    context.SS->FillSeqNo(tx, context.SS->StartRound(*txState));
    FillNotice(pathId, tx, context);

    txState->ClearShardsInProgress();
    Y_ABORT_UNLESS(txState->Shards.size());

    for (ui32 i = 0; i < txState->Shards.size(); ++i) {
        const auto& idx = txState->Shards[i].Idx;
        const auto datashardId = context.SS->ShardInfos[idx].TabletID;
        auto ev = context.SS->MakeDataShardProposal(pathId, OperationId, tx.SerializeAsString(), context.Ctx);
        context.OnComplete.BindMsgToPipe(OperationId, datashardId, idx, ev.Release());
    }

    txState->UpdateShardsInProgress(TTxState::ConfigureParts);
    return false;
}

bool TConfigurePartsAtTable::HandleReply(TEvDataShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) {
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " HandleReply " << ev->Get()->ToString()
                            << ", at schemeshard: " << context.SS->SelfTabletId());

    if (!NTableState::CollectProposeTransactionResults(OperationId, ev, context)) {
        return false;
    }

    return true;
}


// NCdcStreamState::TProposeAtTable
//
TProposeAtTable::TProposeAtTable(TOperationId id)
    : OperationId(id)
{
    IgnoreMessages(DebugHint(), {TEvDataShard::TEvProposeTransactionResult::EventType});
}

bool TProposeAtTable::ProgressState(TOperationContext& context) {
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " ProgressState"
                            << ", at schemeshard: " << context.SS->SelfTabletId());

    const auto* txState = context.SS->FindTx(OperationId);
    Y_ABORT_UNLESS(txState);
    Y_ABORT_UNLESS(IsExpectedTxType(txState->TxType));

    TSet<TTabletId> shardSet;
    for (const auto& shard : txState->Shards) {
        Y_ABORT_UNLESS(context.SS->ShardInfos.contains(shard.Idx));
        shardSet.insert(context.SS->ShardInfos.at(shard.Idx).TabletID);
    }

    context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, txState->MinStep, shardSet);
    return false;
}

bool TProposeAtTable::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) {
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " HandleReply TEvOperationPlan"
                            << ", step: " << ev->Get()->StepId
                            << ", at schemeshard: " << context.SS->SelfTabletId());

    const auto* txState = context.SS->FindTx(OperationId);
    Y_ABORT_UNLESS(txState);
    Y_ABORT_UNLESS(IsExpectedTxType(txState->TxType));
    const auto& pathId = txState->TargetPathId;

    Y_ABORT_UNLESS(context.SS->PathsById.contains(pathId));
    auto path = context.SS->PathsById.at(pathId);

    Y_ABORT_UNLESS(context.SS->Tables.contains(pathId));
    auto table = context.SS->Tables.at(pathId);

    NIceDb::TNiceDb db(context.GetDB());

    bool isContinuousBackupStream = false;
    if (txState->CdcPathId && context.SS->PathsById.contains(txState->CdcPathId)) {
        auto cdcPath = context.SS->PathsById.at(txState->CdcPathId);
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " Checking CDC stream name"
                               << ", cdcPathId: " << txState->CdcPathId
                               << ", streamName: " << cdcPath->Name
                               << ", at schemeshard: " << context.SS->SelfTabletId());
        if (cdcPath->Name.EndsWith("_continuousBackupImpl")) {
            isContinuousBackupStream = true;
        }
    } else {
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " CdcPathId not found"
                               << ", cdcPathId: " << txState->CdcPathId
                               << ", at schemeshard: " << context.SS->SelfTabletId());
    }

    // Check if this is an index implementation table
    bool isIndexImplTable = false;
    TPathId parentPathId = path->ParentPathId;
    TPathId grandParentPathId;
    if (parentPathId && context.SS->PathsById.contains(parentPathId)) {
        auto parentPath = context.SS->PathsById.at(parentPathId);
        if (parentPath->IsTableIndex()) {
            isIndexImplTable = true;
            grandParentPathId = parentPath->ParentPathId;
        }
    }

    // For index impl tables in continuous backup:
    // Synchronize with parent table version (catch up if behind, stay if equal, increment if ahead)
    if (isContinuousBackupStream && isIndexImplTable && grandParentPathId) {
        if (context.SS->Tables.contains(grandParentPathId)) {
            auto parentTable = context.SS->Tables.at(grandParentPathId);
            ui64 currentImplVersion = table->AlterVersion;
            ui64 currentParentVersion = parentTable->AlterVersion;

            // Impl table should never be ahead of parent table
            // If behind or equal, sync to parent version
            // This handles interleaved operations correctly
            if (currentImplVersion <= currentParentVersion) {
                table->AlterVersion = currentParentVersion;
                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                            DebugHint() << " Synchronized index impl table version to parent table"
                                       << ", implTablePathId: " << pathId
                                       << ", parentTablePathId: " << grandParentPathId
                                       << ", oldImplVersion: " << currentImplVersion
                                       << ", parentVersion: " << currentParentVersion
                                       << ", newImplVersion: " << table->AlterVersion
                                       << ", at schemeshard: " << context.SS->SelfTabletId());

                // Also sync parent index entity version to match
                if (context.SS->Indexes.contains(parentPathId)) {
                    auto index = context.SS->Indexes.at(parentPathId);
                    index->AlterVersion = table->AlterVersion;

                    // Persist the index version update
                    db.Table<Schema::TableIndex>().Key(parentPathId.LocalPathId).Update(
                        NIceDb::TUpdate<Schema::TableIndex::AlterVersion>(index->AlterVersion)
                    );

                    auto parentPath = context.SS->PathsById.at(parentPathId);
                    context.SS->ClearDescribePathCaches(parentPath);
                    context.OnComplete.PublishToSchemeBoard(OperationId, parentPathId);

                    LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                                DebugHint() << " Also synced parent index entity version"
                                           << ", indexPathId: " << parentPathId
                                           << ", newVersion: " << index->AlterVersion
                                           << ", at schemeshard: " << context.SS->SelfTabletId());
                }
            } else {
                // Impl version is ahead (shouldn't happen, but handle gracefully)
                table->AlterVersion += 1;
                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                            DebugHint() << " WARNING: Impl table version ahead of parent, incrementing"
                                       << ", implTablePathId: " << pathId
                                       << ", implVersion: " << currentImplVersion
                                       << ", parentVersion: " << currentParentVersion
                                       << ", newImplVersion: " << table->AlterVersion
                                       << ", at schemeshard: " << context.SS->SelfTabletId());
            }
        } else {
            // Fallback: just increment normally
            table->AlterVersion += 1;
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        DebugHint() << " Parent table not found, incrementing normally"
                                   << ", implTablePathId: " << pathId
                                   << ", newVersion: " << table->AlterVersion
                                   << ", at schemeshard: " << context.SS->SelfTabletId());
        }
    } else {
        table->AlterVersion += 1;
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    DebugHint() << " Incremented table version"
                               << ", pathId: " << pathId
                               << ", newVersion: " << table->AlterVersion
                               << ", at schemeshard: " << context.SS->SelfTabletId());
    }

    // For parent tables with indexes in continuous backup:
    // Sync the index entity version to match the parent table
    // Note: impl table version is synced during impl table's own CDC operation
    if (isContinuousBackupStream && !isIndexImplTable) {
        for (const auto& [childName, childPathId] : path->GetChildren()) {
            auto childPath = context.SS->PathsById.at(childPathId);

            // Skip non-index children (CDC streams, etc.)
            if (!childPath->IsTableIndex()) {
                continue;
            }

            // Skip deleted indexes
            if (childPath->Dropped()) {
                continue;
            }

            // Sync parent index version with parent table version
            if (context.SS->Indexes.contains(childPathId)) {
                auto index = context.SS->Indexes.at(childPathId);
                index->AlterVersion = table->AlterVersion;

                // Persist the index version update
                db.Table<Schema::TableIndex>().Key(childPathId.LocalPathId).Update(
                    NIceDb::TUpdate<Schema::TableIndex::AlterVersion>(index->AlterVersion)
                );

                context.SS->ClearDescribePathCaches(childPath);
                context.OnComplete.PublishToSchemeBoard(OperationId, childPathId);

                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                            DebugHint() << " Synced parent index version with parent table"
                                       << ", parentTable: " << path->Name
                                       << ", indexName: " << childName
                                       << ", indexPathId: " << childPathId
                                       << ", newVersion: " << index->AlterVersion
                                       << ", at schemeshard: " << context.SS->SelfTabletId());
            }
        }
    }

    context.SS->PersistTableAlterVersion(db, pathId, table);

    context.SS->ClearDescribePathCaches(path);
    context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

    context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
    return true;
}

bool TProposeAtTable::HandleReply(TEvDataShard::TEvSchemaChanged::TPtr& ev, TOperationContext& context) {
    LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " TEvDataShard::TEvSchemaChanged"
                            << " triggers early, save it"
                            << ", at schemeshard: " << context.SS->SelfTabletId());

    NTableState::CollectSchemaChanged(OperationId, ev, context);
    return false;
}


// NCdcStreamState::TProposeAtTableDropSnapshot
//
bool TProposeAtTableDropSnapshot::HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) {
    TProposeAtTable::HandleReply(ev, context);

    const auto* txState = context.SS->FindTx(OperationId);
    Y_ABORT_UNLESS(txState);
    const auto& pathId = txState->TargetPathId;

    Y_ABORT_UNLESS(context.SS->TablesWithSnapshots.contains(pathId));
    const auto snapshotTxId = context.SS->TablesWithSnapshots.at(pathId);

    auto it = context.SS->SnapshotTables.find(snapshotTxId);
    if (it != context.SS->SnapshotTables.end()) {
        it->second.erase(pathId);
        if (it->second.empty()) {
            context.SS->SnapshotTables.erase(it);
        }
    }

    context.SS->SnapshotsStepIds.erase(snapshotTxId);
    context.SS->TablesWithSnapshots.erase(pathId);

    NIceDb::TNiceDb db(context.GetDB());
    context.SS->PersistDropSnapshot(db, snapshotTxId, pathId);

    context.SS->TabletCounters->Simple()[COUNTER_SNAPSHOTS_COUNT].Sub(1);
    return true;
}

}  // NKikimr::NSchemeShard::NCdcStreamState
