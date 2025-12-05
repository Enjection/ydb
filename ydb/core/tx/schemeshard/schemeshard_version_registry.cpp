#include "schemeshard_version_registry.h"

#include <util/generic/algorithm.h>

namespace NKikimr::NSchemeShard {

EClaimResult TVersionRegistry::ClaimVersionChange(
    TOperationId opId,
    TPathId pathId,
    ui64 currentVersion,
    ui64 targetVersion,
    ETxType opType,
    TString debugInfo)
{
    TTxId txId = opId.GetTxId();
    TSubTxId subTxId = opId.GetSubTxId();

    if (auto* existing = PendingByPath_.FindPtr(pathId)) {
        if (existing->ClaimingTxId == txId) {
            // SIBLING CLAIM - join existing
            // Take max version (in case siblings computed different targets)
            existing->ClaimedVersion = Max(existing->ClaimedVersion, targetVersion);
            existing->Contributors.insert(subTxId);

            return EClaimResult::Joined;
        }

        // Different TxId - actual conflict between different operations
        return EClaimResult::Conflict;
    }

    // First claim - register new
    PendingByPath_[pathId] = TPendingVersionChange(
        pathId,
        currentVersion,
        targetVersion,
        txId,
        subTxId,
        opType,
        std::move(debugInfo)
    );
    ClaimsByTxId_[txId].insert(pathId);

    return EClaimResult::Claimed;
}

bool TVersionRegistry::HasPendingChange(TPathId pathId) const {
    return PendingByPath_.contains(pathId);
}

ui64 TVersionRegistry::GetEffectiveVersion(TPathId pathId, ui64 currentVersion) const {
    if (auto* pending = PendingByPath_.FindPtr(pathId)) {
        // Return the claimed version - all siblings see the same value
        return pending->ClaimedVersion;
    }
    return currentVersion;
}

TMaybe<TTxId> TVersionRegistry::GetClaimingTxId(TPathId pathId) const {
    if (auto* pending = PendingByPath_.FindPtr(pathId)) {
        return pending->ClaimingTxId;
    }
    return Nothing();
}

bool TVersionRegistry::IsClaimedByTxId(TPathId pathId, TTxId txId) const {
    if (auto* pending = PendingByPath_.FindPtr(pathId)) {
        return pending->ClaimingTxId == txId;
    }
    return false;
}

const TPendingVersionChange* TVersionRegistry::GetPendingChange(TPathId pathId) const {
    return PendingByPath_.FindPtr(pathId);
}

void TVersionRegistry::MarkApplied(TTxId txId) {
    auto* claims = ClaimsByTxId_.FindPtr(txId);
    if (!claims) {
        return;
    }

    for (const TPathId& pathId : *claims) {
        if (auto* change = PendingByPath_.FindPtr(pathId)) {
            change->Applied = true;
        }
    }
}

void TVersionRegistry::RollbackChanges(TTxId txId) {
    auto* claims = ClaimsByTxId_.FindPtr(txId);
    if (!claims) {
        return;
    }

    for (const TPathId& pathId : *claims) {
        PendingByPath_.erase(pathId);
    }
    ClaimsByTxId_.erase(txId);
}

void TVersionRegistry::RemoveTransaction(TTxId txId) {
    auto* claims = ClaimsByTxId_.FindPtr(txId);
    if (!claims) {
        return;
    }

    for (const TPathId& pathId : *claims) {
        PendingByPath_.erase(pathId);
    }
    ClaimsByTxId_.erase(txId);
}

const THashSet<TPathId>* TVersionRegistry::GetClaimedPaths(TTxId txId) const {
    return ClaimsByTxId_.FindPtr(txId);
}

void TVersionRegistry::LoadChange(TPendingVersionChange change) {
    TPathId pathId = change.PathId;
    TTxId txId = change.ClaimingTxId;

    PendingByPath_[pathId] = std::move(change);
    ClaimsByTxId_[txId].insert(pathId);
}

} // namespace NKikimr::NSchemeShard
