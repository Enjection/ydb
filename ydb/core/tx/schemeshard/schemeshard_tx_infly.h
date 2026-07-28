#pragma once

#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>

#include <ydb/library/actors/core/actorid.h>
#include <ydb/core/base/tablet_types.h>
#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/core/tx/message_seqno.h>

#include "schemeshard_identificators.h"  // for TStepId, TTxId, TShardIdx
#include "schemeshard_path_db_ref.h"  // for TPathDbRef
#include "schemeshard_subop_types.h"  // for ETxType
#include "schemeshard_subop_state_types.h"  // for ETxState


namespace NKikimrSchemeOp {
    enum EPathState : int;
    class TBuildIndexOutcome;
}

namespace NKikimrTxDataShard {
    class TSplitMergeDescription;
}

namespace NKikimr::NSchemeShard {

// Describes in-progress operation
// A TPathId that reads like a plain field but can only be written by TTxState. The
// entry's reference is acquired against this value, so letting any caller reseat it
// would point an in-flight tx at a path nothing referenced.
//
// Reads convert implicitly, so `pathsById.at(txState->TargetPathId)` still works;
// `.LocalPathId` and friends go through Get().
class TOwnedPathId {
public:
    TOwnedPathId() = default;

    operator const TPathId&() const { return Value; }
    const TPathId& Get() const { return Value; }
    explicit operator bool() const { return bool(Value); }

    // TPathId declares its comparisons as members, so the implicit conversion above
    // would not fire for a left-hand TOwnedPathId. Spell them out.
    friend bool operator==(const TOwnedPathId& l, const TPathId& r) { return l.Value == r; }
    friend bool operator==(const TPathId& l, const TOwnedPathId& r) { return l == r.Value; }
    friend bool operator==(const TOwnedPathId& l, const TOwnedPathId& r) { return l.Value == r.Value; }
    friend bool operator!=(const TOwnedPathId& l, const TPathId& r) { return l.Value != r; }
    friend bool operator!=(const TPathId& l, const TOwnedPathId& r) { return l != r.Value; }
    friend bool operator!=(const TOwnedPathId& l, const TOwnedPathId& r) { return l.Value != r.Value; }

private:
    friend struct TTxState;

    explicit TOwnedPathId(const TPathId& value)
        : Value(value)
    {}

    void Set(const TPathId& value) {
        Value = value;
    }

    TPathId Value = InvalidPathId;
};

struct TTxState {
    struct TShardOperation {
        TShardIdx Idx;                   // shard's internal index
        TTabletTypes::EType TabletType;
        ETxState Operation;         // (first) TxState, that affects on shard

        TString RangeEnd;            // For datashard split

        TShardOperation(TShardIdx idx, TTabletTypes::EType type, ETxState op)
            : Idx(idx), TabletType(type), Operation(op)
        {}
    };

    struct TShardStatus {
        bool Success = false;
        TString Error;
        ui64 BytesProcessed = 0;
        ui64 RowsProcessed = 0;

        TShardStatus() = default;

        explicit TShardStatus(bool success, const TString& error, ui64 bytes, ui64 rows)
            : Success(success)
            , Error(error)
            , BytesProcessed(bytes)
            , RowsProcessed(rows)
        {
        }

        explicit TShardStatus(const TString& error)
            : TShardStatus(false, error, 0, 0)
        {
        }
    };

    using ETxType = NKikimr::NSchemeShard::ETxType;
    using enum ETxType;
    using ETxState = NKikimr::NSchemeShard::ETxState;
    using enum ETxState;

    static TString TypeName(ETxType t) {
        return NKikimr::NSchemeShard::TxTypeName(t);
    }
    static TString StateName(ETxState t) {
        return NKikimr::NSchemeShard::TxStateName(t);
    }

    // persist - TxInFlight:
    ETxType TxType = TxInvalid;
    TOwnedPathId TargetPathId;                      // path (dir or table) being modified
    TOwnedPathId SourcePathId;                      // path (dir or table) being modified
    ETxState State = Invalid;
    TStepId MinStep = InvalidStepId;
    TStepId PlanStep = InvalidStepId;

    // TxCopy or TxRotateCdcStreamAtTable: Stores path for cdc stream to create in case of ContinuousBackup;
    // uses ExtraData through proto
    TPathId CdcPathId = InvalidPathId;
    TMaybe<NKikimrSchemeOp::EPathState> TargetPathTargetState;

    // persist - TxShards:
    TVector<TShardOperation> Shards; // shards + operations on them
    bool NeedUpdateObject = false;
    bool NeedSyncHive = false;
    // not persist:
    THashSet<TShardIdx> ShardsInProgress; // indexes of datashards or pqs that operation waits for
    THashMap<TShardIdx, std::pair<TActorId, ui32>> SchemeChangeNotificationReceived;
    bool ReadyForNotifications = false;
    std::shared_ptr<NKikimrTxDataShard::TSplitMergeDescription> SplitDescription;
    bool TxShardsListFinalized = false;
    TTxId BuildIndexId;
    std::shared_ptr<NKikimrSchemeOp::TBuildIndexOutcome> BuildIndexOutcome;
    // fields below used for backup/restore
    bool Cancel = false;
    THashMap<TShardIdx, TShardStatus> ShardStatuses;
    ui64 DataTotalSize = 0;

    TMessageSeqNo SchemeOpSeqNo;       // For SS -> DS propose events

    TInstant StartTime = TInstant::Zero();

    TTxState()
        : StartTime(::Now())
    {}

    TTxState(ETxType txType, TPathId targetPath, TPathId sourcePath = InvalidPathId)
        : TxType(txType)
        , TargetPathId(targetPath)
        , SourcePathId(sourcePath)
        , State(Waiting)
        , StartTime(::Now())
    {}

    // Read-only views for the DbRefCount reconciliation; the handles themselves stay
    // private so no caller can Reset one and point it at a different path.
    const TPathDbRef& GetTargetPathRef() const { return TargetPathRef; }
    const TPathDbRef& GetSourcePathRef() const { return SourcePathRef; }

private:
    // Arming and disarming are TxInFlight's job: an entry becomes referenced by being
    // inserted, and loses its references only by leaving.
    friend class TTxInFlightMap;

    // DbRefCount references on TargetPathId/SourcePathId. Holding the handle is holding
    // the reference: acquired when the entry enters TxInFlight, released when it leaves.
    TPathDbRef TargetPathRef;
    TPathDbRef SourcePathRef;

    // `sourceExists` is false only for an orphaned in-flight tx whose source path
    // is gone: reference the target, not the missing source.
    void AcquirePathRefs(TSchemeShard* ss, bool sourceExists = true) {
        Y_ABORT_UNLESS(!TargetPathRef && !SourcePathRef);
        TargetPathRef.Reset(ss, TargetPathId, "transaction target path");
        if (SourcePathId && sourceExists) {
            SourcePathRef.Reset(ss, SourcePathId, "transaction source path");
        }
    }

    void DisarmPathRefs() {
        TargetPathRef.DetachWithoutRelease();
        SourcePathRef.DetachWithoutRelease();
    }

    // Late source binding at restore: move the field and its reference together.
    void ReassignSource(TSchemeShard* ss, const TPathId& pathId) {
        SourcePathId.Set(pathId);
        SourcePathRef.Reset(ss, pathId, "transaction source path");
    }

public:
    void AcceptPendingSchemeNotification() {
        ReadyForNotifications = true;
        for (const auto& shard : SchemeChangeNotificationReceived) {
            ShardsInProgress.erase(shard.first);
        }
    }

    void ClearShardsInProgress() {
        ShardsInProgress.clear();
    }

    void UpdateShardsInProgress(ETxState operation = Invalid) {
        for (auto shard : Shards) {
            if (!operation || operation == shard.Operation) {
                ShardsInProgress.insert(shard.Idx);
            }
        }
    }

    bool IsItActuallyMerge() const;

    bool IsCreate() const {
        return NKikimr::NSchemeShard::IsCreate(TxType);
    }

    bool IsDrop() const {
        return NKikimr::NSchemeShard::IsDrop(TxType);
    }

    bool CanDeleteParts() const {
        return NKikimr::NSchemeShard::CanDeleteParts(TxType);
    }
};

// Non-copyable via the TPathDbRef members: assigning over a live tx state drops its refs.
static_assert(!std::is_copy_assignable_v<TTxState>);
static_assert(std::is_move_assignable_v<TTxState>);

// The in-flight tx states, holding a DbRefCount reference on each entry's target and
// source path. Membership is the reference: Emplace acquires, erase releases through
// ~TTxState, and there is no operator[], so an entry cannot exist unreferenced.
//
// The invariant is on the container, not on TTxState::TargetPathId, because a scratch
// tx state assembled before insertion correctly holds no reference.
class TTxInFlightMap {
    using TInner = THashMap<TOperationId, TTxState>;

public:
    using iterator = typename TInner::iterator;
    using const_iterator = typename TInner::const_iterator;

    explicit TTxInFlightMap(TSchemeShard* ss)
        : SS(ss)
    {}

    // Owns references; a copy would double-acquire.
    TTxInFlightMap(const TTxInFlightMap&) = delete;
    TTxInFlightMap& operator=(const TTxInFlightMap&) = delete;
    TTxInFlightMap(TTxInFlightMap&&) = delete;
    TTxInFlightMap& operator=(TTxInFlightMap&&) = delete;

    // The only insertion point, and it always acquires.
    TTxState& Emplace(const TOperationId& opId, TTxState&& state, bool sourceExists = true) {
        auto [it, inserted] = Map.emplace(opId, std::move(state));
        Y_VERIFY_S(inserted, "Trying to create duplicate Tx " << opId);
        it->second.AcquirePathRefs(SS, sourceExists);
        return it->second;
    }

    // ~TTxState releases the entry's references.
    size_t erase(const TOperationId& opId) {
        return Map.erase(opId);
    }

    // TTxInit binds a legacy CopyTable row's source only after its shards are read.
    void ReassignSourcePath(const TOperationId& opId, const TPathId& pathId) {
        Map.at(opId).ReassignSource(SS, pathId);
    }

    // Propose rollback: a Paths snapshot owns restoring the counter, so the entry must
    // go without releasing, or the rollback lands twice.
    size_t EraseDisarmed(const TOperationId& opId) {
        if (auto* txState = Map.FindPtr(opId)) {
            txState->DisarmPathRefs();
        }
        return Map.erase(opId);
    }

    // Teardown: PathsById is cleared alongside, so release would target missing paths.
    void clear() {
        for (auto& [opId, txState] : Map) {
            txState.DisarmPathRefs();
        }
        Map.clear();
    }

    TTxState* FindPtr(const TOperationId& opId) { return Map.FindPtr(opId); }
    const TTxState* FindPtr(const TOperationId& opId) const { return Map.FindPtr(opId); }
    TTxState& at(const TOperationId& opId) { return Map.at(opId); }
    const TTxState& at(const TOperationId& opId) const { return Map.at(opId); }
    bool contains(const TOperationId& opId) const { return Map.contains(opId); }
    size_t size() const { return Map.size(); }
    bool empty() const { return Map.empty(); }

    iterator begin() { return Map.begin(); }
    iterator end() { return Map.end(); }
    const_iterator begin() const { return Map.begin(); }
    const_iterator end() const { return Map.end(); }

private:
    TSchemeShard* SS = nullptr;
    TInner Map;
};

}  // namespace NKikimr::NSchemeShard

template<>
inline void Out<NKikimr::NSchemeShard::TOwnedPathId>(
    IOutputStream& o,
    const NKikimr::NSchemeShard::TOwnedPathId& x)
{
    return x.Get().Out(o);
}
