#pragma once

#include "schemeshard_identificators.h"
#include "schemeshard_subop_types.h"

#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>

#include <util/datetime/base.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>

namespace NKikimr::NSchemeShard {

// Result of attempting to claim a version change
enum class EClaimResult {
    Claimed,    // First to claim, registered new claim
    Joined,     // Sibling (same TxId) already claimed, joined existing claim
    Conflict,   // Different TxId already claimed - real conflict
};

// Represents a pending version change for a path
struct TPendingVersionChange {
    TPathId PathId;                          // Resource being modified
    ui64 OriginalVersion = 0;                // Version when first claim made
    ui64 ClaimedVersion = 0;                 // Target version (may increase via Max)
    TTxId ClaimingTxId = InvalidTxId;        // Shared by all sibling parts
    THashSet<TSubTxId> Contributors;         // Which parts contributed to this claim
    ETxType OperationType = ETxType::TxInvalid;  // Type of operation
    TInstant ClaimTime;                      // When the first claim was made
    bool Applied = false;                    // Whether change was applied

    // For debugging/auditing
    TString DebugInfo;

    TPendingVersionChange() = default;

    TPendingVersionChange(
        TPathId pathId,
        ui64 originalVersion,
        ui64 claimedVersion,
        TTxId txId,
        TSubTxId subTxId,
        ETxType opType,
        TString debugInfo = "")
        : PathId(pathId)
        , OriginalVersion(originalVersion)
        , ClaimedVersion(claimedVersion)
        , ClaimingTxId(txId)
        , OperationType(opType)
        , ClaimTime(TInstant::Now())
        , DebugInfo(std::move(debugInfo))
    {
        Contributors.insert(subTxId);
    }
};

// Centralized registry for tracking pending version changes
// Provides idempotent version updates with sibling coordination
class TVersionRegistry {
public:
    // Claim a version change for an operation
    // Returns: Claimed (first), Joined (sibling), or Conflict (different TxId)
    EClaimResult ClaimVersionChange(
        TOperationId opId,
        TPathId pathId,
        ui64 currentVersion,
        ui64 targetVersion,
        ETxType opType,
        TString debugInfo = "");

    // Check if a path has a pending version change
    bool HasPendingChange(TPathId pathId) const;

    // Get the effective version for a path (claimed version if pending, else current)
    // CRITICAL: All siblings call this to get the SAME version for shard notifications
    ui64 GetEffectiveVersion(TPathId pathId, ui64 currentVersion) const;

    // Get the TxId that claimed a path
    TMaybe<TTxId> GetClaimingTxId(TPathId pathId) const;

    // Check if this operation (or its sibling) claimed a path
    bool IsClaimedByTxId(TPathId pathId, TTxId txId) const;

    // Get the pending change for a path (for testing/debugging)
    const TPendingVersionChange* GetPendingChange(TPathId pathId) const;

    // Apply all pending changes for a TxId (call when ALL parts complete)
    // This marks changes as applied - actual persistence is done by caller
    void MarkApplied(TTxId txId);

    // Rollback all pending changes for a TxId (call on abort)
    void RollbackChanges(TTxId txId);

    // Cleanup completed transaction
    void RemoveTransaction(TTxId txId);

    // Get all paths claimed by a TxId
    const THashSet<TPathId>* GetClaimedPaths(TTxId txId) const;

    // Get all pending changes (for persistence/loading)
    const THashMap<TPathId, TPendingVersionChange>& GetAllPendingChanges() const {
        return PendingByPath_;
    }

    // Clear all pending changes (used during loading)
    void Clear() {
        PendingByPath_.clear();
        ClaimsByTxId_.clear();
    }

    // Add a change directly (used during loading from DB)
    void LoadChange(TPendingVersionChange change);

private:
    // Primary index: PathId -> pending change
    THashMap<TPathId, TPendingVersionChange> PendingByPath_;

    // Secondary index: TxId -> claimed paths (shared by all siblings)
    THashMap<TTxId, THashSet<TPathId>> ClaimsByTxId_;
};

} // namespace NKikimr::NSchemeShard
