#pragma once

#include <ydb/core/testlib/actors/test_runtime.h>
#include <ydb/core/testlib/tablet_helpers.h>  // for ResolveTablet
#include <ydb/core/tx/schemeshard/schemeshard_private.h>
#include <ydb/core/tx/tx.h>  // for TTestTxConfig

#include <util/generic/hash.h>
#include <util/generic/vector.h>
#include <util/string/builder.h>

#include <functional>

namespace NSchemeShardUT_Private {

using namespace NActors;
using namespace NKikimr::NSchemeShard;

/**
 * Blocks TEvProgressOperation events and allows releasing them in controlled order.
 *
 * This helper intercepts messages that SchemeShard sends to itself to progress
 * operation parts. By capturing and selectively releasing these messages, tests
 * can control the exact order in which operation parts execute.
 *
 * Usage:
 *   TOperationPartsBlocker blocker(runtime);
 *
 *   // ... trigger schema operation ...
 *
 *   // Wait for all parts to be captured
 *   blocker.WaitForParts(txId, expectedPartCount);
 *
 *   // Release in specific order
 *   blocker.ReleasePart(txId, 2);  // Release part 2 first
 *   blocker.ReleasePart(txId, 0);  // Then part 0
 *   blocker.ReleasePart(txId, 1);  // Then part 1
 *
 *   // Or release in permutation order
 *   blocker.ReleaseInOrder(txId, {2, 0, 1});
 */
class TOperationPartsBlocker {
public:
    using TPartId = ui32;
    using TTxId = ui64;

    struct TCapturedPart {
        THolder<IEventHandle> Event;
        TPartId PartId;
        bool Released = false;
    };

    struct TOperationParts {
        TVector<TCapturedPart> Parts;
        THashMap<TPartId, size_t> PartIdToIndex;
        size_t ExpectedCount = 0;
        bool Complete = false;
    };

public:
    explicit TOperationPartsBlocker(TTestActorRuntime& runtime,
                                    std::function<bool(TTxId)> txFilter = {},
                                    ui64 schemeShardTabletId = TTestTxConfig::SchemeShard)
        : Runtime_(runtime)
        , TxFilter_(std::move(txFilter))
        , SchemeShardActorId_(ResolveTablet(runtime, schemeShardTabletId))
    {
        Cerr << "... TOperationPartsBlocker: resolved SchemeShard " << schemeShardTabletId
             << " to ActorId " << SchemeShardActorId_ << Endl;

        // Use SetObserverFunc for better compatibility with TServer-based tests
        // This pattern is proven to work in ut_restore.cpp and other schemeshard tests
        PrevObserver_ = Runtime_.SetObserverFunc([this](TAutoPtr<IEventHandle>& ev) {
            if (Stopped_) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            if (ev->GetTypeRewrite() != TEvPrivate::TEvProgressOperation::EventType) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            // Filter by SchemeShard ActorId - only capture events sent to/from our SchemeShard
            if (ev->Recipient != SchemeShardActorId_) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            auto* msg = ev->Get<TEvPrivate::TEvProgressOperation>();
            TTxId txId = msg->TxId;
            TPartId partId = msg->TxPartId;

            // Apply filter if set
            if (TxFilter_ && !TxFilter_(txId)) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            Cerr << "... blocking TEvProgressOperation txId=" << txId
                 << " partId=" << partId << Endl;

            auto& op = Operations_[txId];
            size_t idx = op.Parts.size();
            op.Parts.push_back({
                THolder<IEventHandle>(ev.Release()),
                partId,
                false
            });
            op.PartIdToIndex[partId] = idx;

            return TTestActorRuntime::EEventAction::DROP;
        });
    }

    ~TOperationPartsBlocker() {
        Stop();
    }

    // Stop capturing new events (already captured remain)
    void Stop() {
        if (!Stopped_) {
            Runtime_.SetObserverFunc(PrevObserver_);
            Stopped_ = true;
        }
    }

    // Check if blocker has been stopped
    bool IsStopped() const {
        return Stopped_;
    }

    // Wait until we capture expectedCount parts for txId
    void WaitForParts(TTxId txId, size_t expectedCount) {
        auto& op = Operations_[txId];
        op.ExpectedCount = expectedCount;

        Runtime_.WaitFor(TStringBuilder() << "WaitForParts txId=" << txId
                                          << " count=" << expectedCount,
            [&]() {
                return op.Parts.size() >= expectedCount;
            });

        op.Complete = true;
    }

    // Wait for specified txId to have at least minParts captured
    bool WaitForPartsWithTimeout(TTxId txId, size_t minParts, TDuration timeout) {
        auto deadline = TInstant::Now() + timeout;

        while (TInstant::Now() < deadline) {
            auto it = Operations_.find(txId);
            if (it != Operations_.end() && it->second.Parts.size() >= minParts) {
                return true;
            }

            TDispatchOptions opts;
            opts.CustomFinalCondition = [&]() {
                auto it = Operations_.find(txId);
                return it != Operations_.end() && it->second.Parts.size() >= minParts;
            };
            Runtime_.DispatchEvents(opts, TDuration::MilliSeconds(100));
        }
        return false;
    }

    // Wait for any operation to have at least minParts
    TTxId WaitForAnyOperation(size_t minParts = 1) {
        TTxId foundTxId = 0;
        Runtime_.WaitFor("WaitForAnyOperation", [&]() {
            for (auto& [txId, op] : Operations_) {
                if (op.Parts.size() >= minParts) {
                    foundTxId = txId;
                    return true;
                }
            }
            return false;
        });
        return foundTxId;
    }

    // Get number of captured parts for a txId
    size_t GetPartCount(TTxId txId) const {
        auto it = Operations_.find(txId);
        return it != Operations_.end() ? it->second.Parts.size() : 0;
    }

    // Check if txId has any captured parts
    bool HasOperation(TTxId txId) const {
        return Operations_.contains(txId);
    }

    // Get all captured txIds
    TVector<TTxId> GetCapturedTxIds() const {
        TVector<TTxId> result;
        for (const auto& [txId, _] : Operations_) {
            result.push_back(txId);
        }
        return result;
    }

    // Release a specific part
    void ReleasePart(TTxId txId, TPartId partId) {
        auto opIt = Operations_.find(txId);
        Y_ABORT_UNLESS(opIt != Operations_.end(),
                       "TxId %lu not found in captured operations", txId);

        auto& op = opIt->second;
        auto it = op.PartIdToIndex.find(partId);
        Y_ABORT_UNLESS(it != op.PartIdToIndex.end(),
                       "Part %u not found for txId %lu", partId, txId);

        auto& captured = op.Parts[it->second];
        Y_ABORT_UNLESS(!captured.Released,
                       "Part %u already released for txId %lu", partId, txId);

        Cerr << "... releasing TEvProgressOperation txId=" << txId
             << " partId=" << partId << Endl;

        IEventHandle* ev = captured.Event.Release();
        ui32 nodeId = ev->GetRecipientRewrite().NodeId();
        ui32 nodeIdx = nodeId - Runtime_.GetFirstNodeId();
        Runtime_.Send(ev, nodeIdx, /* viaActorSystem */ true);
        captured.Released = true;
    }

    // Release parts in specific order (by part ID)
    void ReleaseInOrder(TTxId txId, const TVector<TPartId>& order) {
        for (TPartId partId : order) {
            ReleasePart(txId, partId);
        }
    }

    // Try to release a part, returns false if not captured
    bool TryReleasePart(TTxId txId, TPartId partId) {
        auto opIt = Operations_.find(txId);
        if (opIt == Operations_.end()) {
            Cerr << "... TryReleasePart: TxId " << txId << " not found" << Endl;
            return false;
        }

        auto& op = opIt->second;
        auto it = op.PartIdToIndex.find(partId);
        if (it == op.PartIdToIndex.end()) {
            Cerr << "... TryReleasePart: Part " << partId << " not captured for txId " << txId << Endl;
            return false;
        }

        auto& captured = op.Parts[it->second];
        if (captured.Released) {
            Cerr << "... TryReleasePart: Part " << partId << " already released for txId " << txId << Endl;
            return false;
        }

        Cerr << "... releasing TEvProgressOperation txId=" << txId
             << " partId=" << partId << Endl;

        IEventHandle* ev = captured.Event.Release();
        ui32 nodeId = ev->GetRecipientRewrite().NodeId();
        ui32 nodeIdx = nodeId - Runtime_.GetFirstNodeId();
        Runtime_.Send(ev, nodeIdx, /* viaActorSystem */ true);
        captured.Released = true;
        return true;
    }

    /**
     * Release captured parts using permutation indices.
     *
     * The permutation specifies the ORDER in which to release captured parts,
     * where indices refer to positions in the captured parts list (not part IDs).
     *
     * Example: If we captured parts [1, 2, 5] (in that order), and permutation is [2, 0, 1],
     * we release: part 5 (index 2), part 1 (index 0), part 2 (index 1)
     */
    void ReleaseByPermutationIndices(TTxId txId, const TVector<ui32>& permutation) {
        auto opIt = Operations_.find(txId);
        Y_ABORT_UNLESS(opIt != Operations_.end(),
                       "TxId %lu not found in captured operations", txId);

        auto& op = opIt->second;
        Y_ABORT_UNLESS(permutation.size() <= op.Parts.size(),
                       "Permutation size %zu exceeds captured parts %zu for txId %lu",
                       permutation.size(), op.Parts.size(), txId);

        for (ui32 idx : permutation) {
            Y_ABORT_UNLESS(idx < op.Parts.size(),
                           "Permutation index %u out of range for txId %lu", idx, txId);

            auto& captured = op.Parts[idx];
            Y_ABORT_UNLESS(!captured.Released,
                           "Part at index %u already released for txId %lu", idx, txId);

            Cerr << "... releasing TEvProgressOperation txId=" << txId
                 << " partId=" << captured.PartId << " (index " << idx << ")" << Endl;

            IEventHandle* ev = captured.Event.Release();
            ui32 nodeId = ev->GetRecipientRewrite().NodeId();
            ui32 nodeIdx = nodeId - Runtime_.GetFirstNodeId();
            Runtime_.Send(ev, nodeIdx, /* viaActorSystem */ true);
            captured.Released = true;
        }
    }

    // Release all remaining parts for txId (in capture order)
    void ReleaseAll(TTxId txId) {
        auto opIt = Operations_.find(txId);
        if (opIt == Operations_.end()) {
            return;
        }

        auto& op = opIt->second;
        for (auto& captured : op.Parts) {
            if (!captured.Released) {
                Cerr << "... releasing TEvProgressOperation txId=" << txId
                     << " partId=" << captured.PartId << " (ReleaseAll)" << Endl;

                IEventHandle* ev = captured.Event.Release();
                ui32 nodeId = ev->GetRecipientRewrite().NodeId();
                ui32 nodeIdx = nodeId - Runtime_.GetFirstNodeId();
                Runtime_.Send(ev, nodeIdx, /* viaActorSystem */ true);
                captured.Released = true;
            }
        }
    }

    // Release all parts for all operations
    void ReleaseAllOperations() {
        for (auto& [txId, _] : Operations_) {
            ReleaseAll(txId);
        }
    }

    // Get captured part IDs for a txId (in capture order)
    TVector<TPartId> GetPartIds(TTxId txId) const {
        TVector<TPartId> result;
        auto it = Operations_.find(txId);
        if (it != Operations_.end()) {
            for (const auto& part : it->second.Parts) {
                result.push_back(part.PartId);
            }
        }
        return result;
    }

    // Get number of unreleased parts for txId
    size_t GetUnreleasedCount(TTxId txId) const {
        auto it = Operations_.find(txId);
        if (it == Operations_.end()) {
            return 0;
        }

        size_t count = 0;
        for (const auto& part : it->second.Parts) {
            if (!part.Released) {
                ++count;
            }
        }
        return count;
    }

    // Clear all captured operations
    void Clear() {
        Operations_.clear();
    }

private:
    TTestActorRuntime& Runtime_;
    std::function<bool(TTxId)> TxFilter_;
    TActorId SchemeShardActorId_;  // Resolved from tablet ID
    TTestActorRuntime::TEventObserver PrevObserver_;
    THashMap<TTxId, TOperationParts> Operations_;
    bool Stopped_ = false;
};

} // namespace NSchemeShardUT_Private
