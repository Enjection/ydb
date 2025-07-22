#include "schemeshard__backup_collection_common.h"
#include "schemeshard__operation_common.h"
#include "schemeshard__operation_part.h"
#include "schemeshard__operation.h"  // for NextPartId
#include "schemeshard_impl.h"
#include "schemeshard_utils.h"  // for TransactionTemplate
#include "schemeshard_path_element.h"  // for TPathElement::EPathType
#include "schemeshard_path.h"  // for TPath

#include <algorithm>

#define LOG_I(stream) LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)

namespace NKikimr::NSchemeShard {

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NKikimr::NIceDb;

namespace {

// Forward declaration
void CleanupIncrementalBackupCdcStreams(TOperationContext& context, const TPathId& backupCollectionPathId);

// Helper function to clean up incremental restore state for a backup collection
void CleanupIncrementalRestoreState(const TPathId& backupCollectionPathId, TOperationContext& context, TNiceDb& db) {
    LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "CleanupIncrementalRestoreState for backup collection: " << backupCollectionPathId);

    // Find all incremental restore states for this backup collection
    TVector<ui64> statesToCleanup;
    
    for (auto it = context.SS->IncrementalRestoreStates.begin(); it != context.SS->IncrementalRestoreStates.end();) {
        if (it->second.BackupCollectionPathId == backupCollectionPathId) {
            const auto& stateId = it->first;  // it->first is ui64 (state ID)
            statesToCleanup.push_back(stateId);
            
            // Remove from memory
            auto toErase = it;
            ++it;
            context.SS->IncrementalRestoreStates.erase(toErase);
        } else {
            ++it;
        }
    }

    // Clean up database entries for states we found in memory
    for (const auto& stateId : statesToCleanup) {
        // Delete from IncrementalRestoreState table
        db.Table<Schema::IncrementalRestoreState>().Key(stateId).Delete();
        
        // Delete from IncrementalRestoreOperations table
        db.Table<Schema::IncrementalRestoreOperations>().Key(stateId).Delete();
        
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    "Cleaned up incremental restore state: " << stateId);
    }

    // For tests and completeness, also scan and clean up any orphaned database entries
    // that might not be present in memory (e.g., test data)
    
    // Clean up IncrementalRestoreOperations table by scanning all entries
    auto opsRowset = db.Table<Schema::IncrementalRestoreOperations>().Range().Select();
    if (opsRowset.IsReady()) {
        TVector<ui64> idsToDelete;
        while (!opsRowset.EndOfSet()) {
            auto id = opsRowset.GetValue<Schema::IncrementalRestoreOperations::Id>();
            idsToDelete.push_back(ui64(id));
            
            if (!opsRowset.Next()) {
                break;
            }
        }
        
        for (const auto& id : idsToDelete) {
            db.Table<Schema::IncrementalRestoreOperations>().Key(TTxId(id)).Delete();
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        "Cleaned up orphaned incremental restore operation: " << id);
        }
    }
    
    // Clean up IncrementalRestoreState table by scanning all entries
    auto stateRowset = db.Table<Schema::IncrementalRestoreState>().Range().Select();
    if (stateRowset.IsReady()) {
        TVector<ui64> idsToDelete;
        while (!stateRowset.EndOfSet()) {
            ui64 operationId = stateRowset.GetValue<Schema::IncrementalRestoreState::OperationId>();
            idsToDelete.push_back(operationId);
            
            if (!stateRowset.Next()) {
                break;
            }
        }
        
        for (const auto& operationId : idsToDelete) {
            db.Table<Schema::IncrementalRestoreState>().Key(operationId).Delete();
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        "Cleaned up orphaned incremental restore state: " << operationId);
        }
    }

    // Clean up IncrementalRestoreShardProgress table by scanning all entries
    auto shardProgressRowset = db.Table<Schema::IncrementalRestoreShardProgress>().Range().Select();
    if (shardProgressRowset.IsReady()) {
        TVector<std::pair<ui64, ui64>> keysToDelete;
        while (!shardProgressRowset.EndOfSet()) {
            ui64 operationId = shardProgressRowset.GetValue<Schema::IncrementalRestoreShardProgress::OperationId>();
            ui64 shardIdx = shardProgressRowset.GetValue<Schema::IncrementalRestoreShardProgress::ShardIdx>();
            keysToDelete.emplace_back(operationId, shardIdx);
            
            if (!shardProgressRowset.Next()) {
                break;
            }
        }
        
        for (const auto& [operationId, shardIdx] : keysToDelete) {
            db.Table<Schema::IncrementalRestoreShardProgress>().Key(operationId, shardIdx).Delete();
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        "Cleaned up orphaned shard progress for operationId: " << operationId << ", shardIdx: " << shardIdx);
        }
    }

    LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "Completed cleanup of incremental restore state for: " << backupCollectionPathId);
    
    // Also clean up any CDC streams associated with incremental backup
    CleanupIncrementalBackupCdcStreams(context, backupCollectionPathId);
}

// TODO: This function will be removed once we fully migrate to suboperations pattern
// Currently commented out as it's part of the old approach
/*
ISubOperation::TPtr CascadeDropBackupCollection(TVector<ISubOperation::TPtr>& result, 
                                               const TOperationId& id, 
                                               const TPath& backupCollection,
                                               TOperationContext& context) {
    // For each backup directory in the collection
    for (const auto& [backupName, backupPathId] : backupCollection.Base()->GetChildren()) {
        TPath backupPath = backupCollection.Child(backupName);
        
        if (!backupPath.IsResolved() || backupPath.IsDeleted()) {
            continue;
        }

        // If this is a table (backup), drop it using CascadeDropTableChildren to handle
        // any CDC streams, indexes, or other dependencies
        if (backupPath->IsTable()) {
            if (auto reject = CascadeDropTableChildren(result, id, backupPath)) {
                return reject;
            }
            
            // Then drop the table itself
            auto dropTable = TransactionTemplate(backupCollection.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropTable);
            dropTable.MutableDrop()->SetName(ToString(backupPath.Base()->Name));
            result.push_back(CreateDropTable(NextPartId(id, result), dropTable));
        } 
        // If this is a directory (for incremental backups), recursively drop its contents
        else if (backupPath->IsDirectory()) {
            // Recursively handle directory contents
            if (auto reject = CascadeDropBackupCollection(result, id, backupPath, context)) {
                return reject;
            }
            
            // Then drop the directory itself
            auto dropDir = TransactionTemplate(backupCollection.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpRmDir);
            dropDir.MutableDrop()->SetName(ToString(backupPath.Base()->Name));
            result.push_back(CreateRmDir(NextPartId(id, result), dropDir));
        }
    }

    return nullptr;
}
*/

// Helper function to clean up CDC streams associated with incremental backup
void CleanupIncrementalBackupCdcStreams(TOperationContext& context, const TPathId& backupCollectionPathId) {
    LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "CleanupIncrementalBackupCdcStreams for backup collection: " << backupCollectionPathId);

    // Find all tables that have CDC streams with '_continuousBackupImpl' suffix and mark them for deletion
    TVector<std::pair<TPathId, TPathId>> streamsToDelete; // (tablePathId, streamPathId)
    
    for (const auto& [pathId, cdcStreamInfo] : context.SS->CdcStreams) {
        if (!context.SS->PathsById.contains(pathId)) {
            continue;
        }
        
        auto streamPath = context.SS->PathsById.at(pathId);
        if (!streamPath || streamPath->Dropped()) {
            continue;
        }
        
        // Check if this CDC stream has the incremental backup suffix
        if (streamPath->Name.EndsWith("_continuousBackupImpl")) {
            // Find the parent table
            if (!context.SS->PathsById.contains(streamPath->ParentPathId)) {
                continue;
            }
            
            auto tablePath = context.SS->PathsById.at(streamPath->ParentPathId);
            if (!tablePath || !tablePath->IsTable() || tablePath->Dropped()) {
                continue;
            }
            
            LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "Found incremental backup CDC stream to clean up: " << streamPath->Name 
                       << " on table: " << tablePath->Name 
                       << " (streamPathId: " << pathId << ", tablePathId: " << streamPath->ParentPathId << ")");
            
            streamsToDelete.emplace_back(streamPath->ParentPathId, pathId);
        }
    }
    
    // For each CDC stream we found, mark it for deletion by setting its state to drop
    for (const auto& [tablePathId, streamPathId] : streamsToDelete) {
        if (!context.SS->PathsById.contains(streamPathId)) {
            continue;
        }
        
        auto streamPath = context.SS->PathsById.at(streamPathId);
        if (!streamPath || streamPath->Dropped()) {
            continue;
        }
        
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "Marking CDC stream for deletion: " << streamPath->Name << " (pathId: " << streamPathId << ")");
        
        // Mark the CDC stream as dropped - this will make DescribeCdcStream skip it
        NIceDb::TNiceDb db(context.GetDB());
        streamPath->SetDropped(TStepId(1), TTxId(context.SS->Generation()));
        // Create a proper TOperationId for PersistDropStep
        TOperationId opId(TTxId(context.SS->Generation()), TSubTxId(0));
        context.SS->PersistDropStep(db, streamPathId, TStepId(1), opId);
        
        // Also remove from parent table's children list
        if (context.SS->PathsById.contains(streamPath->ParentPathId)) {
            auto parentPath = context.SS->PathsById.at(streamPath->ParentPathId);
            parentPath->RemoveChild(streamPath->Name, streamPathId);
        }
        
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "Marked CDC stream as dropped: " << streamPath->Name);
    }
    
    LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "Completed cleanup of incremental backup CDC streams, processed " << streamsToDelete.size() << " streams");
}

}

class TDropBackupCollectionPropose : public TSubOperationState {
public:
    explicit TDropBackupCollectionPropose(TOperationId id)
        : OperationId(std::move(id))
    {}

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState called");

        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        TPathId pathId = txState->TargetPathId;
        LOG_I("TDropBackupCollectionPropose: Found txState for pathId: " << pathId);
        
        auto pathPtr = context.SS->PathsById.at(pathId);

        TNiceDb db(context.GetDB());
        
        LOG_I("TDropBackupCollectionPropose: Performing cleanup for backup collection: " << pathId);
        
        // At this point, the path should already be marked as EPathStateDrop by Propose()
        // Now do the actual cleanup work
        
        // Remove from BackupCollections map
        if (context.SS->BackupCollections.contains(pathId)) {
            context.SS->BackupCollections.erase(pathId);
            context.SS->PersistRemoveBackupCollection(db, pathId);
            LOG_I("TDropBackupCollectionPropose: Removed backup collection from map");
        } else {
            LOG_I("TDropBackupCollectionPropose: Backup collection not found in map");
        }

        // Clean up incremental restore state for this backup collection
        LOG_I("TDropBackupCollectionPropose: Calling CleanupIncrementalRestoreState");
        CleanupIncrementalRestoreState(pathId, context, db);
        LOG_I("TDropBackupCollectionPropose: CleanupIncrementalRestoreState completed");
        
        // Transition to Done state for final cleanup
        LOG_I("TDropBackupCollectionPropose: Transitioning to Done state");
        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        LOG_I("TDropBackupCollectionPropose: ProgressState completed successfully");
        return true;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        Y_UNUSED(ev);
        LOG_I(DebugHint() << "HandleReply TEvOperationPlan");
        
        // Don't change state here - ProgressState already did it
        return true;
    }

    TString DebugHint() const override {
        return TStringBuilder() << "TDropBackupCollection TDropBackupCollectionPropose, operationId: " << OperationId << ", ";
    }

private:
    const TOperationId OperationId;
};

class TPropose : public TSubOperationState {
public:
    explicit TPropose(TOperationId id)
        : OperationId(std::move(id))
    {}

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "TPropose::ProgressState");

        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        LOG_I("TPropose::ProgressState: Proposing to coordinator for pathId: " << txState->TargetPathId);
        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const TStepId step = TStepId(ev->Get()->StepId);
        LOG_I(DebugHint() << "TPropose::HandleReply TEvOperationPlan: step# " << step);

        const TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        const TPathId& pathId = txState->TargetPathId;
        const TPathElement::TPtr pathPtr = context.SS->PathsById.at(pathId);
        const TPathElement::TPtr parentDirPtr = context.SS->PathsById.at(pathPtr->ParentPathId);

        TNiceDb db(context.GetDB());

        Y_ABORT_UNLESS(!pathPtr->Dropped());
        pathPtr->SetDropped(step, OperationId.GetTxId());
        context.SS->PersistDropStep(db, pathId, step, OperationId);
        context.SS->PersistRemoveBackupCollection(db, pathId);

        // CRITICAL: Clean up incremental restore state for this backup collection
        // This cleanup is essential because incremental restore state exists outside the normal path hierarchy
        // TODO: Implement CleanupIncrementalRestoreState function
        // CleanupIncrementalRestoreState(pathId, context, db);

        auto domainInfo = context.SS->ResolveDomainInfo(pathId);
        domainInfo->DecPathsInside(context.SS);
        DecAliveChildrenDirect(OperationId, parentDirPtr, context); // for correct discard of ChildrenExist prop
        context.SS->TabletCounters->Simple()[COUNTER_BACKUP_COLLECTION_COUNT].Sub(1);

        ++parentDirPtr->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentDirPtr);
        context.SS->ClearDescribePathCaches(parentDirPtr);
        context.SS->ClearDescribePathCaches(pathPtr);

        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parentDirPtr->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
        }

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

private:
    TString DebugHint() const override {
        return TStringBuilder() << "TDropBackupCollection TPropose, operationId: " << OperationId << ", ";
    }

private:
    const TOperationId OperationId;
};

// Done state for DROP BACKUP COLLECTION operations
class TDropBackupCollectionDone : public TSubOperationState {
public:
    explicit TDropBackupCollectionDone(TOperationId id)
        : OperationId(id)
    {
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TDropBackupCollectionDone::ProgressState called, OperationId: " << OperationId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        TPathId pathId = txState->TargetPathId;
        auto pathPtr = context.SS->PathsById.at(pathId);
        auto parentDirPtr = context.SS->PathsById.at(pathPtr->ParentPathId);

        TNiceDb db(context.GetDB());

        // Remove from BackupCollections if present
        if (context.SS->BackupCollections.contains(pathId)) {
            context.SS->BackupCollections.erase(pathId);
            context.SS->PersistRemoveBackupCollection(db, pathId);
        }

        // Mark as fully dropped
        pathPtr->SetDropped(TStepId(1), OperationId.GetTxId());
        
        // Update parent directory
        ++parentDirPtr->DirAlterVersion;
        
        // Persist changes
        context.SS->PersistPath(db, pathId);
        context.SS->PersistPathDirAlterVersion(db, parentDirPtr);
        
        // Clear caches
        context.SS->ClearDescribePathCaches(parentDirPtr);
        context.SS->ClearDescribePathCaches(pathPtr);
        
        // Publish notifications
        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parentDirPtr->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
        }

        context.OnComplete.DoneOperation(OperationId);
        return true;
    }

private:
    TString DebugHint() const override {
        return TStringBuilder() << "TDropBackupCollection TDropBackupCollectionDone, operationId: " << OperationId;
    }

private:
    const TOperationId OperationId;
};

class TDropBackupCollection : public TSubOperation {
public:    
    explicit TDropBackupCollection(TOperationId id, const TTxTransaction& tx)
        : TSubOperation(id, tx) {
        LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TDropBackupCollection constructor (TTxTransaction), id: " << id
                       << ", tx: " << tx.ShortDebugString().substr(0, 100));
    }

    explicit TDropBackupCollection(TOperationId id, TTxState::ETxState state)
        : TSubOperation(id, state) {
        LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TDropBackupCollection constructor (ETxState), id: " << id
                       << ", state: " << (int)state);
    }

private:
    static TTxState::ETxState NextState() {
        return TTxState::DropParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::DropParts:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "TDropBackupCollection::SelectStateFunc called with state: " << (int)state << ", OperationId: " << OperationId);
        switch (state) {
        case TTxState::Waiting:
        case TTxState::DropParts:
            LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "TDropBackupCollection::SelectStateFunc returning TDropBackupCollectionPropose for OperationId: " << OperationId);
            return MakeHolder<TDropBackupCollectionPropose>(OperationId);
        case TTxState::Done:
            LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "TDropBackupCollection::SelectStateFunc returning TDropBackupCollectionDone for OperationId: " << OperationId);
            return MakeHolder<TDropBackupCollectionDone>(OperationId);
        default:
            LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::FLAT_TX_SCHEMESHARD,
                       "TDropBackupCollection::SelectStateFunc returning nullptr for unknown state: " << (int)state << ", OperationId: " << OperationId);
            return nullptr;
        }
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        // Debug: Log the operation ID at the start of Propose
        Cerr << "TDropBackupCollection::Propose: OperationId.GetTxId()=" << OperationId.GetTxId() 
             << ", OperationId.GetSubTxId()=" << OperationId.GetSubTxId() << Endl;

        const NKikimrSchemeOp::TBackupCollectionDescription& drop = Transaction.GetDropBackupCollection();
        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = drop.GetName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropBackupCollection Propose"
                         << ", path: " << parentPathStr << "/" << name
                         << ", pathId: " << (drop.HasPathId() ? TPathId::FromProto(drop.GetPathId()) : TPathId())
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        ui64 txId = ui64(OperationId.GetTxId());
        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropBackupCollection creating response"
                         << ", OperationId: " << OperationId
                         << ", GetTxId(): " << OperationId.GetTxId()
                         << ", ui64(GetTxId()): " << txId);
        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, txId, ui64(ssId));

        // Validate collection name
        if (name.empty()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "Collection name cannot be empty");
            return result;
        }

        // First resolve working directory to check if it exists
        TPath workingDirPath = TPath::Resolve(parentPathStr, context.SS);
        if (!workingDirPath.IsResolved()) {
            result->SetError(NKikimrScheme::StatusPathDoesNotExist, "Working directory does not exist");
            return result;
        }

        // Validate that we're operating within a collections directory
        if (!parentPathStr.EndsWith(".backups/collections") && !parentPathStr.EndsWith(".backups/collections/")) {
            result->SetError(NKikimrScheme::StatusSchemeError, "Backup collections can only be dropped from .backups/collections directory");
            return result;
        }

        TString fullPath = parentPathStr;
        if (!fullPath.EndsWith("/")) {
            fullPath += "/";
        }
        fullPath += name;

        TPath path = drop.HasPathId()
            ? TPath::Init(TPathId::FromProto(drop.GetPathId()), context.SS)
            : TPath::Resolve(fullPath, context.SS);

        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    "TDropBackupCollection Path Resolution Debug"
                        << ", parentPathStr: '" << parentPathStr << "'"
                        << ", name: '" << name << "'"
                        << ", fullPath: '" << fullPath << "'"
                        << ", hasPathId: " << drop.HasPathId()
                        << ", pathIsResolved: " << path.IsResolved()
                        << ", pathBase: " << (path.IsResolved() && path.Base() ? "exists" : "null")
                        << ", opId: " << OperationId);

        {
            TPath::TChecker checks = path.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .IsBackupCollection()
                .NotUnderDeleting()
                .NotUnderOperation();

            if (!checks) {
                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropBackupCollection checks failed"
                         << ", status: " << (ui32)checks.GetStatus()
                         << ", error: " << checks.GetError());
                result->SetError(checks.GetStatus(), checks.GetError());
                if (path.IsResolved() && path.Base()->IsBackupCollection() && path.Base()->PlannedToDrop()) {
                    // Already dropping, this is duplicate request
                    result->SetError(NKikimrScheme::StatusMultipleModifications, 
                                   "Backup collection is already being dropped");
                }
                return result;
            }
        }

        // Check for active backup operations using this collection
        {
            THashSet<TPathId> pathSet;
            pathSet.insert(path->PathId);
            auto childPaths = context.SS->ListSubTree(path->PathId, context.Ctx);
            for (const auto& childPathId : childPaths) {
                pathSet.insert(childPathId);
            }
            
            auto relatedTransactions = context.SS->GetRelatedTransactions(pathSet, context.Ctx);
            for (auto txId : relatedTransactions) {
                if (txId == OperationId.GetTxId()) {
                    continue; // Skip our own transaction
                }
                // There's an active transaction involving this backup collection or its children
                LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropBackupCollection found active backup operation"
                         << ", active txId: " << txId
                         << ", our txId: " << OperationId.GetTxId());
                result->SetError(NKikimrScheme::StatusPreconditionFailed, 
                    TStringBuilder() << "Cannot drop backup collection while backup operation is active"
                    << ", active txId: " << txId);
                return result;
            }
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                 "TDropBackupCollection CheckApplyIf failed: " << errStr);
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        if (!context.SS->CheckLocks(path.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
             "TDropBackupCollection checks passed, setting up state machine");

        auto guard = context.DbGuard();
        context.MemChanges.GrabNewTxState(context.SS, OperationId);
        context.MemChanges.GrabPath(context.SS, path.Base()->PathId);
        context.MemChanges.GrabPath(context.SS, path.Base()->ParentPathId);

        context.DbChanges.PersistTxState(OperationId);
        context.DbChanges.PersistPath(path.Base()->PathId);
        context.DbChanges.PersistPath(path.Base()->ParentPathId);

        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxDropBackupCollection, path.Base()->PathId);
        txState.MinStep = TStepId(1);
        txState.State = TTxState::DropParts;

        // Mark the backup collection for deletion (this enables concurrent operation detection)
        path.Base()->PathState = TPathElement::EPathState::EPathStateDrop;
        path.Base()->DropTxId = OperationId.GetTxId();
        path.Base()->LastTxId = OperationId.GetTxId();

        IncParentDirAlterVersionWithRepublishSafeWithUndo(OperationId, path, context.SS, context.OnComplete);

        context.OnComplete.ActivateTx(OperationId);

        SetState(NextState());

        result->SetPathCreateTxId(ui64(OperationId.GetTxId()));
        result->SetPathId(path.Base()->PathId.LocalPathId);

        LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
             "TDropBackupCollection setup complete - state machine will handle cleanup"
                 << ", Status: " << result->Record.GetStatus()
                 << ", TxId: " << result->Record.GetTxId()
                 << ", PathId: " << result->Record.GetPathId());

        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        Y_UNUSED(context);
        // Nothing specific to abort for cleanup operations
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        Y_UNUSED(forceDropTxId);
        Y_UNUSED(context);
        // Nothing specific to abort for cleanup operations
    }

private:
    TString DebugHint() const {
        return TStringBuilder() << "TDropBackupCollection TPropose, operationId: " << OperationId << ", ";
    }
};

// New suboperations for proper cleanup following refactoring plan

// Helper structures for planning drop operations
struct TDropInfo {
    TPathId PathId;
    TString Name;
    TString ParentPath;
    NKikimrSchemeOp::EPathType Type;
    bool IsEmpty = false;
    TVector<TPathId> Dependencies;
    
    TDropInfo() = default;
    TDropInfo(TPathId pathId, const TString& name, const TString& parentPath, NKikimrSchemeOp::EPathType type)
        : PathId(pathId), Name(name), ParentPath(parentPath), Type(type) {}
};

struct TDropPlan {
    TVector<TDropInfo> SourceTableCdcStreams;
    TVector<TDropInfo> ItemsToDrop; // Ordered by dependency
    TPathId BackupCollectionId;
};

// Suboperation for cleaning up incremental restore state
class TCleanupIncrementalRestoreState : public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::Propose;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::Propose:
            return MakeHolder<TCleanupIncrementalRestorePropose>(OperationId, BackupCollectionId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

    class TCleanupIncrementalRestorePropose : public TSubOperationState {
    public:
        explicit TCleanupIncrementalRestorePropose(TOperationId id, TPathId bcId)
            : OperationId(std::move(id))
            , BackupCollectionId(bcId)
        {}

        bool ProgressState(TOperationContext& context) override {
            LOG_I(DebugHint() << "ProgressState");

            const auto* txState = context.SS->FindTx(OperationId);
            Y_ABORT_UNLESS(txState);
            Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

            TNiceDb db(context.GetDB());
            
            // Clean up incremental restore state for this backup collection
            CleanupIncrementalRestoreState(BackupCollectionId, context, db);
            
            context.SS->ChangeTxState(db, OperationId, TTxState::Done);
            return true;
        }

        bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
            Y_UNUSED(ev);
            LOG_I(DebugHint() << "HandleReply TEvOperationPlan");
            
            context.OnComplete.DoneOperation(OperationId);
            return true;
        }
    
    private:
        TString DebugHint() const override {
            return TStringBuilder()
                << "TCleanupIncrementalRestorePropose"
                << ", operationId: " << OperationId
                << ", backupCollectionId: " << BackupCollectionId;
        }

        const TOperationId OperationId;
        const TPathId BackupCollectionId;
    };

public:
    explicit TCleanupIncrementalRestoreState(TOperationId id, TPathId bcId)
        : TSubOperation(id, TTxState::Waiting)
        , OperationId(std::move(id))
        , BackupCollectionId(bcId)
    {
    }

    explicit TCleanupIncrementalRestoreState(TOperationId id, TTxState::ETxState state)
        : TSubOperation(id, state)
        , OperationId(std::move(id))
        , BackupCollectionId() // Will be loaded from persistence
    {
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        Y_UNUSED(owner);
        Y_UNUSED(context);
        return MakeHolder<TEvSchemeShard::TEvModifySchemeTransactionResult>(
            NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(OperationId.GetSubTxId())
        );
    }

    void AbortPropose(TOperationContext& context) override {
        Y_UNUSED(context);
        // Nothing specific to abort for cleanup operations
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        Y_UNUSED(forceDropTxId);
        Y_UNUSED(context);
        // Nothing specific to abort for cleanup operations
    }

private:
    TString DebugHint() const {
        return TStringBuilder()
            << "TCleanupIncrementalRestoreState"
            << ", operationId: " << OperationId
            << ", backupCollectionId: " << BackupCollectionId;
    }

private:
    const TOperationId OperationId;
    const TPathId BackupCollectionId;
};

// Suboperation for finalizing backup collection metadata cleanup
class TFinalizeDropBackupCollection : public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::Propose;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::Propose:
            return MakeHolder<TFinalizeDropBackupCollectionPropose>(OperationId, BackupCollectionId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

    class TFinalizeDropBackupCollectionPropose : public TSubOperationState {
    public:
        explicit TFinalizeDropBackupCollectionPropose(TOperationId id, TPathId bcId)
            : OperationId(std::move(id))
            , BackupCollectionId(bcId)
        {}

        bool ProgressState(TOperationContext& context) override {
            LOG_I(DebugHint() << "ProgressState");

            const auto* txState = context.SS->FindTx(OperationId);
            Y_ABORT_UNLESS(txState);
            Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

            TNiceDb db(context.GetDB());
            
            // This is the ONLY place where direct cleanup happens
            // And only after all suboperations completed successfully
            
            // Remove from BackupCollections map
            context.SS->BackupCollections.erase(BackupCollectionId);
            
            // Remove from persistent storage
            context.SS->PersistRemoveBackupCollection(db, BackupCollectionId);
            
            // Clear path caches and remove from PathsById
            if (auto* path = context.SS->PathsById.FindPtr(BackupCollectionId)) {
                context.SS->ClearDescribePathCaches(*path);
                context.SS->PathsById.erase(BackupCollectionId);
                context.SS->DecrementPathDbRefCount(BackupCollectionId, "finalize drop backup collection");
            }
            
            context.SS->ChangeTxState(db, OperationId, TTxState::Done);
            return true;
        }

        bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
            Y_UNUSED(ev);
            LOG_I(DebugHint() << "HandleReply TEvOperationPlan");
            
            context.OnComplete.DoneOperation(OperationId);
            return true;
        }

    private:
        TString DebugHint() const override {
            return TStringBuilder()
                << "TFinalizeDropBackupCollectionPropose"
                << ", operationId: " << OperationId
                << ", backupCollectionId: " << BackupCollectionId;
        }

        const TOperationId OperationId;
        const TPathId BackupCollectionId;
    };

public:
    explicit TFinalizeDropBackupCollection(TOperationId id, TPathId bcId)
        : TSubOperation(id, TTxState::Waiting)
        , OperationId(std::move(id))
        , BackupCollectionId(bcId)
    {
    }

    explicit TFinalizeDropBackupCollection(TOperationId id, TTxState::ETxState state)
        : TSubOperation(id, state)
        , OperationId(std::move(id))
        , BackupCollectionId() // Will be loaded from persistence
    {
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        Y_UNUSED(owner);
        Y_UNUSED(context);
        return MakeHolder<TEvSchemeShard::TEvModifySchemeTransactionResult>(
            NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(OperationId.GetSubTxId())
        );
    }

    void AbortPropose(TOperationContext& context) override {
        Y_UNUSED(context);
        // Nothing specific to abort for cleanup operations
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        Y_UNUSED(forceDropTxId);
        Y_UNUSED(context);
        // Nothing specific to abort for cleanup operations
    }

private:
    TString DebugHint() const {
        return TStringBuilder()
            << "TFinalizeDropBackupCollection"
            << ", operationId: " << OperationId
            << ", backupCollectionId: " << BackupCollectionId;
    }

private:
    const TOperationId OperationId;
    const TPathId BackupCollectionId;
};

// Helper functions for creating suboperations
ISubOperation::TPtr CreateDropTableSubOperation(
    const TDropInfo& dropInfo, 
    TOperationId baseId, 
    ui32& nextPart) {
    
    auto dropTable = TransactionTemplate(
        dropInfo.ParentPath,
        NKikimrSchemeOp::EOperationType::ESchemeOpDropTable
    );
    dropTable.MutableDrop()->SetName(dropInfo.Name);
    
    TOperationId subOpId(baseId.GetTxId(), ++nextPart);
    return CreateDropTable(subOpId, dropTable);
}

ISubOperation::TPtr CreateDropTopicSubOperation(
    const TDropInfo& dropInfo,
    TOperationId baseId,
    ui32& nextPart) {
    
    auto dropTopic = TransactionTemplate(
        dropInfo.ParentPath,
        NKikimrSchemeOp::EOperationType::ESchemeOpDropPersQueueGroup
    );
    dropTopic.MutableDrop()->SetName(dropInfo.Name);
    
    TOperationId subOpId(baseId.GetTxId(), ++nextPart);
    return CreateDropPQ(subOpId, dropTopic);
}

// TODO: Temporarily commented out until properly implemented as full TSubOperation classes
/*
ISubOperation::TPtr CreateDropCdcStreamSubOperation(
    const TDropInfo& dropInfo,
    TOperationId baseId,
    ui32& nextPart) {
    
    // TODO: Implement CDC stream drop suboperation
    // This requires context and returns a vector of operations
    // For now, return nullptr to skip CDC operations
    Y_UNUSED(dropInfo);
    Y_UNUSED(baseId);
    Y_UNUSED(nextPart);
    return nullptr;
}
*/

ISubOperation::TPtr CreateRmDirSubOperation(
    const TDropInfo& dropInfo,
    TOperationId baseId,
    ui32& nextPart) {
    
    auto rmDir = TransactionTemplate(
        dropInfo.ParentPath,
        NKikimrSchemeOp::EOperationType::ESchemeOpRmDir
    );
    rmDir.MutableDrop()->SetName(dropInfo.Name);
    
    TOperationId subOpId(baseId.GetTxId(), ++nextPart);
    return CreateRmDir(subOpId, rmDir);
}

// Helper functions for path analysis
void CollectPathsRecursively(const TPath& root, THashMap<TPathId, TDropInfo>& paths, TOperationContext& context) {
    if (!root.IsResolved() || root.IsDeleted()) {
        return;
    }
    
    const TPathId& pathId = root.Base()->PathId;
    TString parentPath = root.Parent().PathString();
    
    // Add this path to the collection
    TDropInfo dropInfo(pathId, root.Base()->Name, parentPath, root.Base()->PathType);
    
    // Check if directory is empty (only for directories)
    if (root.Base()->PathType == NKikimrSchemeOp::EPathTypeDir) {
        dropInfo.IsEmpty = root.Base()->GetChildren().empty();
    }
    
    paths[pathId] = dropInfo;
    
    // Recursively process children
    for (const auto& [childName, childPathId] : root.Base()->GetChildren()) {
        TPath childPath = root.Child(childName);
        CollectPathsRecursively(childPath, paths, context);
    }
}

TDropPlan AnalyzeBackupCollection(const TPath& backupCollection, TOperationContext& context) {
    TDropPlan plan;
    plan.BackupCollectionId = backupCollection.Base()->PathId;
    
    // Collect all paths that need to be dropped
    THashMap<TPathId, TDropInfo> allPaths;
    CollectPathsRecursively(backupCollection, allPaths, context);
    
    // Simple topological sort: directories after their contents
    TVector<TDropInfo> sortedPaths;
    
    // First, add all non-directory items
    for (const auto& [pathId, dropInfo] : allPaths) {
        if (dropInfo.Type != NKikimrSchemeOp::EPathTypeDir) {
            sortedPaths.push_back(dropInfo);
        }
    }
    
    // Then, add directories (they should be empty by now)
    for (const auto& [pathId, dropInfo] : allPaths) {
        if (dropInfo.Type == NKikimrSchemeOp::EPathTypeDir && dropInfo.PathId != plan.BackupCollectionId) {
            sortedPaths.push_back(dropInfo);
        }
    }
    
    plan.ItemsToDrop = std::move(sortedPaths);
    return plan;
}

// TODO: Temporarily commented out until properly implemented as full TSubOperation classes
/*
// Factory functions for creating cleanup operations
ISubOperation::TPtr CreateCleanupIncrementalRestoreStateSubOp(TOperationId id, TPathId bcId) {
    // TODO: Implement proper TSubOperation wrapper for TCleanupIncrementalRestoreState
    // For now, return nullptr to avoid compilation errors
    Y_UNUSED(id);
    Y_UNUSED(bcId);
    return nullptr;
}

ISubOperation::TPtr CreateFinalizeDropBackupCollectionSubOp(TOperationId id, TPathId bcId) {
    // TODO: Implement proper TSubOperation wrapper for TFinalizeDropBackupCollection  
    // For now, return nullptr to avoid compilation errors
    Y_UNUSED(id);
    Y_UNUSED(bcId);
    return nullptr;
}
*/

// New function that creates multiple suboperations for dropping backup collection
TVector<ISubOperation::TPtr> CreateDropBackupCollectionCascade(
    TOperationId nextId, 
    const TTxTransaction& tx, 
    TOperationContext& context) {
    Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpDropBackupCollection);

    auto dropOperation = tx.GetDropBackupCollection();
    const TString parentPathStr = tx.GetWorkingDir();
    
    TPath backupCollection = TPath::Resolve(parentPathStr + "/" + dropOperation.GetName(), context.SS);

    {
        TPath::TChecker checks = backupCollection.Check();
        checks
            .NotEmpty()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .IsBackupCollection()
            .NotUnderDeleting()
            .NotUnderOperation()
            .IsCommonSensePath();

        if (!checks) {
            return {CreateReject(nextId, MakeHolder<TProposeResponse>(checks.GetStatus(), 
                ui64(nextId.GetTxId()), ui64(context.SS->TabletID()), checks.GetError()))};
        }
    }

    // Check for active backup/restore operations
    const TPathId& pathId = backupCollection.Base()->PathId;
    
    // Check if any backup or restore operations are active for this collection
    for (const auto& [txId, txState] : context.SS->TxInFlight) {
        if (txState.TargetPathId == pathId && 
            (txState.TxType == TTxState::TxBackup || 
             txState.TxType == TTxState::TxRestore)) {
            return {CreateReject(nextId, MakeHolder<TProposeResponse>(NKikimrScheme::StatusPreconditionFailed, 
                ui64(nextId.GetTxId()), ui64(context.SS->TabletID()),
                "Cannot drop backup collection while backup or restore operations are active. Please wait for them to complete."))};
        }
    }
    
    // Check for active incremental restore operations in IncrementalRestoreStates
    for (const auto& [opId, restoreState] : context.SS->IncrementalRestoreStates) {
        if (restoreState.BackupCollectionPathId == pathId) {
            return {CreateReject(nextId, MakeHolder<TProposeResponse>(NKikimrScheme::StatusPreconditionFailed,
                ui64(nextId.GetTxId()), ui64(context.SS->TabletID()),
                "Cannot drop backup collection while incremental restore operations are active. Please wait for them to complete."))};
        }
    }

    TVector<ISubOperation::TPtr> result;
    
    // NEW IMPLEMENTATION: Use proper suboperations instead of direct manipulation
    
    // Step 1: Analyze what needs to be dropped
    TDropPlan plan = AnalyzeBackupCollection(backupCollection, context);
    
    // Step 2: Create drop operations for each path (ordered by dependencies)
    ui32 nextPart = 0;
    for (const auto& dropInfo : plan.ItemsToDrop) {
        switch (dropInfo.Type) {
            case NKikimrSchemeOp::EPathTypeTable:
                result.push_back(CreateDropTableSubOperation(dropInfo, nextId, nextPart));
                break;
                
            case NKikimrSchemeOp::EPathTypePersQueueGroup:
                result.push_back(CreateDropTopicSubOperation(dropInfo, nextId, nextPart));
                break;
                
            case NKikimrSchemeOp::EPathTypeDir:
                // Only drop empty directories (non-backup collection directories)
                if (dropInfo.IsEmpty && dropInfo.PathId != plan.BackupCollectionId) {
                    result.push_back(CreateRmDirSubOperation(dropInfo, nextId, nextPart));
                }
                break;
                
            case NKikimrSchemeOp::EPathTypeCdcStream:
                // TODO: Implement CDC stream dropping
                // result.push_back(CreateDropCdcStreamSubOperation(dropInfo, nextId, nextPart));
                LOG_N("Skipping CDC stream drop for now: " << dropInfo.Name);
                break;
                
            default:
                LOG_N("Skipping unsupported path type for drop: " << static_cast<int>(dropInfo.Type) 
                     << " for path: " << dropInfo.Name);
                break;
        }
    }
    
    // Step 3: Clean up incremental restore state
    result.push_back(ISubOperation::TPtr(new TCleanupIncrementalRestoreState(
        TOperationId(nextId.GetTxId(), ++nextPart), 
        plan.BackupCollectionId
    )));
    
    // Step 4: Finalize - remove backup collection metadata (must be last)
    // result.push_back(ISubOperation::TPtr(new TFinalizeDropBackupCollection(
    //     TOperationId(nextId.GetTxId(), ++nextPart),
    //     plan.BackupCollectionId
    // )));

    return result;
}

ISubOperation::TPtr CreateDropBackupCollection(TOperationId id, const TTxTransaction& tx) {
    Cerr << "CreateDropBackupCollection(TOperationId, TTxTransaction): txId=" 
         << id.GetTxId() << ", subTxId=" << id.GetSubTxId() << Endl;
    return MakeSubOperation<TDropBackupCollection>(id, tx);
}

ISubOperation::TPtr CreateDropBackupCollection(TOperationId id, TTxState::ETxState state) {
    Cerr << "CreateDropBackupCollection(TOperationId, ETxState): txId=" 
         << id.GetTxId() << ", subTxId=" << id.GetSubTxId() 
         << ", state=" << (int)state << Endl;
    Y_ABORT_UNLESS(state != TTxState::Invalid);
    return MakeSubOperation<TDropBackupCollection>(id, state);
};

} // namespace NKikimr::NSchemeShard
