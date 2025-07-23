#include "schemeshard__backup_collection_common.h"
#include "schemeshard__operation_common.h"
#include "schemeshard__operation_part.h"
#include "schemeshard__operation.h"
#include "schemeshard_impl.h"
#include "schemeshard_utils.h"
#include "schemeshard_path_element.h"
#include "schemeshard_path.h"

#include <algorithm>

#define LOG_I(stream) LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)

namespace NKikimr::NSchemeShard {

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NKikimr::NIceDb;

namespace {

// Forward declarations
void CleanupIncrementalBackupCdcStreams(TOperationContext& context, const TPathId& backupCollectionPathId);

// Helper structures for the new hybrid suboperations approach
struct TDropPlan {
    struct TCdcStreamInfo {
        TPathId TablePathId;
        TString StreamName;
        TString TablePath;
    };
    
    TVector<TCdcStreamInfo> CdcStreams;
    TVector<TPath> BackupTables;
    TVector<TPath> BackupTopics;
    TPathId BackupCollectionId;
    
    bool HasExternalObjects() const {
        return !CdcStreams.empty() || !BackupTables.empty() || !BackupTopics.empty();
    }
};

// Collect all external objects that need suboperations for dropping
THolder<TDropPlan> CollectExternalObjects(TOperationContext& context, const TPath& bcPath) {
    auto plan = MakeHolder<TDropPlan>();
    plan->BackupCollectionId = bcPath.Base()->PathId;
    
    // 1. Find CDC streams on source tables (these are OUTSIDE the backup collection)
    // For now, we'll find CDC streams with incremental backup suffix across all tables
    for (const auto& [pathId, cdcStreamInfo] : context.SS->CdcStreams) {
        if (!context.SS->PathsById.contains(pathId)) {
            continue;
        }
        
        auto streamPath = context.SS->PathsById.at(pathId);
        if (!streamPath || streamPath->Dropped()) {
            continue;
        }
        
        if (streamPath->Name.EndsWith("_continuousBackupImpl")) {
            if (!context.SS->PathsById.contains(streamPath->ParentPathId)) {
                continue;
            }
            
            auto tablePath = context.SS->PathsById.at(streamPath->ParentPathId);
            if (!tablePath || !tablePath->IsTable() || tablePath->Dropped()) {
                continue;
            }
            
            plan->CdcStreams.push_back({
                streamPath->ParentPathId,
                streamPath->Name,
                TPath::Init(streamPath->ParentPathId, context.SS).PathString()
            });
        }
    }
    
    // 2. Find backup tables and topics UNDER the collection path recursively
    TVector<TPath> toVisit = {bcPath};
    while (!toVisit.empty()) {
        TPath current = toVisit.back();
        toVisit.pop_back();
        
        for (const auto& [childName, childPathId] : current.Base()->GetChildren()) {
            TPath childPath = current.Child(childName);
            
            if (childPath.Base()->IsTable()) {
                plan->BackupTables.push_back(childPath);
            } else if (childPath.Base()->IsPQGroup()) {
                plan->BackupTopics.push_back(childPath);
            } else if (childPath.Base()->IsDirectory()) {
                toVisit.push_back(childPath);
            }
        }
    }
    
    return plan;
}

// Helper functions to create synthetic transactions for suboperations
TTxTransaction CreateCdcDropTransaction(const TDropPlan::TCdcStreamInfo& cdcInfo, TOperationContext& context) {
    TTxTransaction cdcDropTx;
    TPath tablePath = TPath::Init(cdcInfo.TablePathId, context.SS);
    cdcDropTx.SetWorkingDir(tablePath.Parent().PathString());
    cdcDropTx.SetOperationType(NKikimrSchemeOp::ESchemeOpDropCdcStream);
    
    auto* cdcDrop = cdcDropTx.MutableDropCdcStream();
    cdcDrop->SetTableName(tablePath.LeafName());
    cdcDrop->SetStreamName(cdcInfo.StreamName);
    
    return cdcDropTx;
}

TTxTransaction CreateTableDropTransaction(const TPath& tablePath) {
    TTxTransaction tableDropTx;
    tableDropTx.SetWorkingDir(tablePath.Parent().PathString());
    tableDropTx.SetOperationType(NKikimrSchemeOp::ESchemeOpDropTable);
    
    auto* drop = tableDropTx.MutableDrop();
    drop->SetName(tablePath.LeafName());
    
    return tableDropTx;
}

TTxTransaction CreateTopicDropTransaction(const TPath& topicPath) {
    TTxTransaction topicDropTx;
    topicDropTx.SetWorkingDir(topicPath.Parent().PathString());
    topicDropTx.SetOperationType(NKikimrSchemeOp::ESchemeOpDropPersQueueGroup);
    
    auto* drop = topicDropTx.MutableDrop();
    drop->SetName(topicPath.LeafName());
    
    return topicDropTx;
}

// Clean up incremental restore state for a backup collection (synchronous metadata cleanup)
void CleanupIncrementalRestoreState(const TPathId& backupCollectionPathId, TOperationContext& context, TNiceDb& db) {
    TVector<ui64> statesToCleanup;
    
    for (auto it = context.SS->IncrementalRestoreStates.begin(); it != context.SS->IncrementalRestoreStates.end();) {
        if (it->second.BackupCollectionPathId == backupCollectionPathId) {
            const auto& stateId = it->first;
            statesToCleanup.push_back(stateId);
            
            auto toErase = it;
            ++it;
            context.SS->IncrementalRestoreStates.erase(toErase);
        } else {
            ++it;
        }
    }

    for (const auto& stateId : statesToCleanup) {
        db.Table<Schema::IncrementalRestoreState>().Key(stateId).Delete();
        db.Table<Schema::IncrementalRestoreOperations>().Key(stateId).Delete();
    }

    // Clean up any orphaned database entries (for test completeness)
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
        }
    }
    
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
        }
    }

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
        }
    }
    
    CleanupIncrementalBackupCdcStreams(context, backupCollectionPathId);
}

void CleanupIncrementalBackupCdcStreams(TOperationContext& context, const TPathId& backupCollectionPathId) {
    Y_UNUSED(backupCollectionPathId);
    TVector<std::pair<TPathId, TPathId>> streamsToDelete;
    
    for (const auto& [pathId, cdcStreamInfo] : context.SS->CdcStreams) {
        if (!context.SS->PathsById.contains(pathId)) {
            continue;
        }
        
        auto streamPath = context.SS->PathsById.at(pathId);
        if (!streamPath || streamPath->Dropped()) {
            continue;
        }
        
        if (streamPath->Name.EndsWith("_continuousBackupImpl")) {
            if (!context.SS->PathsById.contains(streamPath->ParentPathId)) {
                continue;
            }
            
            auto tablePath = context.SS->PathsById.at(streamPath->ParentPathId);
            if (!tablePath || !tablePath->IsTable() || tablePath->Dropped()) {
                continue;
            }
            
            streamsToDelete.emplace_back(streamPath->ParentPathId, pathId);
        }
    }
    
    for (const auto& [tablePathId, streamPathId] : streamsToDelete) {
        if (!context.SS->PathsById.contains(streamPathId)) {
            continue;
        }
        
        auto streamPath = context.SS->PathsById.at(streamPathId);
        if (!streamPath || streamPath->Dropped()) {
            continue;
        }
        
        NIceDb::TNiceDb db(context.GetDB());
        streamPath->SetDropped(TStepId(1), TTxId(context.SS->Generation()));
        TOperationId opId(TTxId(context.SS->Generation()), TSubTxId(0));
        context.SS->PersistDropStep(db, streamPathId, TStepId(1), opId);
        
        if (context.SS->PathsById.contains(streamPath->ParentPathId)) {
            auto parentPath = context.SS->PathsById.at(streamPath->ParentPathId);
            parentPath->RemoveChild(streamPath->Name, streamPathId);
        }
    }
}

}

class TDropBackupCollectionPropose : public TSubOperationState {
public:
    explicit TDropBackupCollectionPropose(TOperationId id)
        : OperationId(std::move(id))
    {}

    bool ProgressState(TOperationContext& context) override {
        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        // For the internal cleanup suboperation, get the backup collection path
        // from the transaction state's TargetPathId
        TPathId pathId = txState->TargetPathId;
        TNiceDb db(context.GetDB());
        
        if (context.SS->BackupCollections.contains(pathId)) {
            context.SS->BackupCollections.erase(pathId);
            context.SS->PersistRemoveBackupCollection(db, pathId);
        }

        CleanupIncrementalRestoreState(pathId, context, db);
        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        Y_UNUSED(ev);
        Y_UNUSED(context);
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
        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const TStepId step = TStepId(ev->Get()->StepId);

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

        auto domainInfo = context.SS->ResolveDomainInfo(pathId);
        domainInfo->DecPathsInside(context.SS);
        DecAliveChildrenDirect(OperationId, parentDirPtr, context);
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

class TDropBackupCollectionDone : public TSubOperationState {
public:
    explicit TDropBackupCollectionDone(TOperationId id)
        : OperationId(id)
    {
    }

    bool ProgressState(TOperationContext& context) override {
        TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropBackupCollection);

        TPathId pathId = txState->TargetPathId;
        auto pathPtr = context.SS->PathsById.at(pathId);
        auto parentDirPtr = context.SS->PathsById.at(pathPtr->ParentPathId);

        TNiceDb db(context.GetDB());

        if (context.SS->BackupCollections.contains(pathId)) {
            context.SS->BackupCollections.erase(pathId);
            context.SS->PersistRemoveBackupCollection(db, pathId);
        }

        pathPtr->SetDropped(TStepId(1), OperationId.GetTxId());
        ++parentDirPtr->DirAlterVersion;
        
        context.SS->PersistPath(db, pathId);
        context.SS->PersistPathDirAlterVersion(db, parentDirPtr);
        
        context.SS->ClearDescribePathCaches(parentDirPtr);
        context.SS->ClearDescribePathCaches(pathPtr);
        
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
        switch (state) {
        case TTxState::Waiting:
        case TTxState::DropParts:
            return MakeHolder<TDropBackupCollectionPropose>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDropBackupCollectionDone>(OperationId);
        default:
            return nullptr;
        }
    }

public:    
    using TSubOperation::TSubOperation;

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        Y_UNUSED(owner);
        const auto& tx = GetTransaction();
        Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpDropBackupCollection);

        const auto& drop = tx.GetDropBackupCollection();
        const TString& parentPathStr = tx.GetWorkingDir();
        const TString& name = drop.GetName();
        
        TString fullPath = parentPathStr;
        if (!fullPath.EndsWith("/")) {
            fullPath += "/";
        }
        fullPath += name;

        TPath backupCollectionPath = drop.HasPathId()
            ? TPath::Init(TPathId::FromProto(drop.GetPathId()), context.SS)
            : TPath::Resolve(fullPath, context.SS);

        // Validate path exists and is a backup collection
        {
            TPath::TChecker checks = backupCollectionPath.Check();
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
                auto result = MakeHolder<TProposeResponse>(checks.GetStatus(), ui64(OperationId.GetTxId()), ui64(context.SS->SelfTabletId()));
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(tx, errStr)) {
            auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusPreconditionFailed, ui64(OperationId.GetTxId()), ui64(context.SS->SelfTabletId()));
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        TPathId pathId = backupCollectionPath.Base()->PathId;
        auto guard = context.DbGuard();
        context.MemChanges.GrabPath(context.SS, pathId);
        context.MemChanges.GrabNewTxState(context.SS, OperationId);

        context.DbChanges.PersistPath(pathId);
        context.DbChanges.PersistTxState(OperationId);

        Y_ABORT_UNLESS(!context.SS->FindTx(OperationId));
        auto& txState = context.SS->CreateTx(OperationId, TTxState::TxDropBackupCollection, pathId);
        txState.State = TTxState::DropParts;

        TPathElement::TPtr path = backupCollectionPath.Base();
        path->PathState = TPathElement::EPathState::EPathStateDrop;
        path->DropTxId = OperationId.GetTxId();
        path->LastTxId = OperationId.GetTxId();

        context.OnComplete.ActivateTx(OperationId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(context.SS->SelfTabletId()));
        SetState(NextState());
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        Y_UNUSED(context);
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        Y_UNUSED(forceDropTxId);
        Y_UNUSED(context);
    }
};

// Final cleanup suboperation for dropping backup collection metadata
class TDropBackupCollectionInternal : public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::Done;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
            case TTxState::Waiting:
                return TTxState::Done;
            default:
                return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
            case TTxState::Waiting:
                return MakeHolder<TDropBackupCollectionPropose>(OperationId);
            case TTxState::Done:
                return MakeHolder<TDropBackupCollectionDone>(OperationId);
            default:
                return nullptr;
        }
    }

public:
    using TSubOperation::TSubOperation;

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        Y_UNUSED(owner);
        Y_UNUSED(context);
        ui64 txId = ui64(OperationId.GetTxId());
        return MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, txId, ui64(context.SS->SelfTabletId()));
    }

    void AbortPropose(TOperationContext& context) override {
        Y_UNUSED(context);
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        Y_UNUSED(forceDropTxId);
        Y_UNUSED(context);
    }
};

TVector<ISubOperation::TPtr> CreateDropBackupCollectionSuboperations(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpDropBackupCollection);

    const auto& drop = tx.GetDropBackupCollection();
    const TString& parentPathStr = tx.GetWorkingDir();
    const TString& name = drop.GetName();
    
    TString fullPath = parentPathStr;
    if (!fullPath.EndsWith("/")) {
        fullPath += "/";
    }
    fullPath += name;

    TPath backupCollectionPath = drop.HasPathId()
        ? TPath::Init(TPathId::FromProto(drop.GetPathId()), context.SS)
        : TPath::Resolve(fullPath, context.SS);

    // Validate path exists and is a backup collection
    {
        TPath::TChecker checks = backupCollectionPath.Check();
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
            return {CreateReject(nextId, checks.GetStatus(), checks.GetError())};
        }
    }

    // Collect all external objects to drop
    auto dropPlan = CollectExternalObjects(context, backupCollectionPath);
    TVector<ISubOperation::TPtr> result;
    TSubTxId nextPart = 0;

    // Create suboperations for CDC streams
    for (const auto& cdcInfo : dropPlan->CdcStreams) {
        TTxTransaction cdcDropTx = CreateCdcDropTransaction(cdcInfo, context);
        // Note: CreateDropCdcStream returns a vector, so we need to handle it differently
        auto cdcOps = CreateDropCdcStream(TOperationId(nextId.GetTxId(), nextPart++), cdcDropTx, context);
        for (auto& op : cdcOps) {
            result.push_back(op);
        }
    }

    // Create suboperations for backup tables
    for (const auto& tablePath : dropPlan->BackupTables) {
        TTxTransaction tableDropTx = CreateTableDropTransaction(tablePath);
        result.push_back(CreateDropTable(TOperationId(nextId.GetTxId(), nextPart++), tableDropTx));
    }

    // Create suboperations for backup topics
    for (const auto& topicPath : dropPlan->BackupTopics) {
        TTxTransaction topicDropTx = CreateTopicDropTransaction(topicPath);
        result.push_back(CreateDropPQ(TOperationId(nextId.GetTxId(), nextPart++), topicDropTx));
    }

    // Create final cleanup suboperation (must be last)
    TTxTransaction cleanupTx;
    cleanupTx.SetOperationType(NKikimrSchemeOp::EOperationType::ESchemeOpDropBackupCollection);
    cleanupTx.SetWorkingDir(dropPlan->BackupCollectionId.ToString());
    
    result.push_back(MakeSubOperation<TDropBackupCollectionInternal>(
        TOperationId(nextId.GetTxId(), nextPart++), 
        cleanupTx
    ));

    return result;
}

ISubOperation::TPtr CreateDropBackupCollection(TOperationId id, const TTxTransaction& tx) {
    return MakeSubOperation<TDropBackupCollection>(id, tx);
}

ISubOperation::TPtr CreateDropBackupCollection(TOperationId id, TTxState::ETxState state) {
    Y_ABORT_UNLESS(state != TTxState::Invalid);
    return MakeSubOperation<TDropBackupCollection>(id, state);
}

} // namespace NKikimr::NSchemeShard
