#pragma once

#include "operation_order_controller.h"

#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/base/pipe_cache.h>

#include <util/generic/vector.h>
#include <util/generic/ptr.h>

namespace NSchemeShardUT_Private {

using namespace NKikimr;
using namespace NActors;

/**
 * TOperationOrderRuntimeController - Runtime integration for operation order control
 *
 * This class integrates TOperationOrderController with TTestActorRuntime,
 * providing automatic operation batching and reordering for tests using event observation.
 *
 * Usage:
 *   TOperationOrderRuntimeController runtimeCtrl(runtime);
 *   TOperationOrderController controller;
 *   controller.SetMode(TOperationOrderController::Random, 42);
 *   runtimeCtrl.SetOperationOrderController(&controller);
 *
 *   runtimeCtrl.BeginOperationBatch(4);
 *   // Submit operations via normal test helpers - they will be intercepted
 *   runtimeCtrl.FlushOperationBatch();
 */
class TOperationOrderRuntimeController {
public:
    struct TBatchedEvent {
        TAutoPtr<IEventHandle> Handle;
        ui64 TxId;  // For tracking and logging

        TBatchedEvent(TAutoPtr<IEventHandle> handle, ui64 txId)
            : Handle(handle)
            , TxId(txId)
        {}
    };

private:
    TTestActorRuntime& Runtime;
    TOperationOrderController* Controller = nullptr;
    bool BatchingEnabled = false;
    ui32 ExpectedBatchSize = 0;
    TVector<TBatchedEvent> BatchedEvents;
    TTestActorRuntime::TEventObserverHolder ObserverHolder;

public:
    explicit TOperationOrderRuntimeController(TTestActorRuntime& runtime)
        : Runtime(runtime)
    {}

    /**
     * Set the operation order controller to use
     */
    void SetOperationOrderController(TOperationOrderController* controller) {
        Controller = controller;
    }

    /**
     * Get the current controller
     */
    TOperationOrderController* GetOperationOrderController() {
        return Controller;
    }

    /**
     * Begin batching operations
     * @param expectedSize - Expected number of operations in batch (optional hint)
     */
    void BeginOperationBatch(ui32 expectedSize = 0) {
        Y_ENSURE(!BatchingEnabled, "Already in batching mode");
        BatchingEnabled = true;
        ExpectedBatchSize = expectedSize;
        BatchedEvents.clear();
        if (expectedSize > 0) {
            BatchedEvents.reserve(expectedSize);
        }

        // Install event observer to intercept TEvForward events to SchemeShard
        ObserverHolder = Runtime.AddObserver<TEvPipeCache::TEvForward>(
            [this](TEvPipeCache::TEvForward::TPtr& ev) {
                if (BatchingEnabled) {
                    OnEventObserved(ev);
                }
            });
    }

    /**
     * Check if currently batching operations
     */
    bool IsBatching() const {
        return BatchingEnabled;
    }

    /**
     * Flush batched operations - apply ordering and send them
     */
    void FlushOperationBatch() {
        Y_ENSURE(BatchingEnabled, "Not in batching mode");

        if (BatchedEvents.empty()) {
            Cerr << "FlushOperationBatch: Warning - no operations in batch" << Endl;
            BatchingEnabled = false;
            ObserverHolder = {};
            return;
        }

        if (!Controller || Controller->GetMode() == TOperationOrderController::Default) {
            // No reordering - send in original order
            Cerr << "FlushOperationBatch: Sending " << BatchedEvents.size()
                 << " operations in original order" << Endl;
            for (auto& event : BatchedEvents) {
                Runtime.Send(event.Handle.Release(), 0, true);
            }
        } else {
            // Reorder operations
            TVector<ui64> indices;
            for (ui64 i = 0; i < BatchedEvents.size(); ++i) {
                indices.push_back(i);
            }

            auto orderedIndices = Controller->GetNextOrder(indices);

            // Log the ordering for debugging
            Cerr << "FlushOperationBatch: Sending " << orderedIndices.size()
                 << " operations in order: ";
            for (auto idx : orderedIndices) {
                Cerr << "tx" << BatchedEvents[idx].TxId << " ";
            }
            Cerr << Endl;

            // Send operations in the ordered sequence
            for (ui64 idx : orderedIndices) {
                Runtime.Send(BatchedEvents[idx].Handle.Release(), 0, true);
            }
        }

        // Reset batching state
        BatchedEvents.clear();
        BatchingEnabled = false;
        ExpectedBatchSize = 0;
        ObserverHolder = {};
    }

    /**
     * Cancel batching without sending operations
     */
    void CancelOperationBatch() {
        BatchedEvents.clear();
        BatchingEnabled = false;
        ExpectedBatchSize = 0;
        ObserverHolder = {};
    }

    /**
     * Get number of batched operations
     */
    ui32 GetBatchSize() const {
        return BatchedEvents.size();
    }

    /**
     * Check if batch is ready to flush (has expected number of operations)
     */
    bool IsBatchReady() const {
        return BatchingEnabled &&
               ExpectedBatchSize > 0 &&
               BatchedEvents.size() >= ExpectedBatchSize;
    }

private:
    /**
     * Called by observer when an event is intercepted
     */
    void OnEventObserved(TEvPipeCache::TEvForward::TPtr& ev) {
        // Extract transaction ID for logging
        ui64 txId = 0;
        if (ev->Get()->Ev) {
            auto* modifyEv = dynamic_cast<TEvSchemeShard::TEvModifySchemeTransaction*>(ev->Get()->Ev.Get());
            if (modifyEv && modifyEv->Record.HasTxId()) {
                txId = modifyEv->Record.GetTxId();
            }
        }

        Cerr << "OnEventObserved: Batching operation tx" << txId << Endl;

        // Take ownership of the event handle
        TAutoPtr<IEventHandle> handle(ev.Release());
        BatchedEvents.emplace_back(std::move(handle), txId);

        // Prevent the event from being delivered immediately
        // by nullifying the original pointer
        ev = nullptr;
    }
};

/**
 * Helper wrapper for TTestActorRuntime that adds operation order control
 *
 * This is a convenience wrapper that provides a cleaner API for tests.
 *
 * Usage:
 *   TTestActorRuntime runtime;
 *   TTestEnv env(runtime);
 *
 *   TOperationOrderRuntime orderedRuntime(runtime);
 *   TOperationOrderController controller;
 *   controller.SetMode(TOperationOrderController::Random, 42);
 *   orderedRuntime.SetOperationOrderController(&controller);
 *
 *   orderedRuntime.BeginOperationBatch(4);
 *   // Submit operations via normal test helpers - they will be intercepted
 *   TestCreateSequence(runtime, ++txId, "/MyRoot", R"(Name: "seq1")");
 *   TestCreateSequence(runtime, ++txId, "/MyRoot", R"(Name: "seq2")");
 *   TestCreateSequence(runtime, ++txId, "/MyRoot", R"(Name: "seq3")");
 *   TestCreateSequence(runtime, ++txId, "/MyRoot", R"(Name: "seq4")");
 *   orderedRuntime.FlushOperationBatch();
 */
class TOperationOrderRuntime {
private:
    TTestActorRuntime& Runtime;
    TOperationOrderRuntimeController Controller;

public:
    explicit TOperationOrderRuntime(TTestActorRuntime& runtime)
        : Runtime(runtime)
        , Controller(runtime)
    {}

    TTestActorRuntime& GetRuntime() { return Runtime; }
    TOperationOrderRuntimeController& GetController() { return Controller; }

    void SetOperationOrderController(TOperationOrderController* ctrl) {
        Controller.SetOperationOrderController(ctrl);
    }

    void BeginOperationBatch(ui32 expectedSize = 0) {
        Controller.BeginOperationBatch(expectedSize);
    }

    void FlushOperationBatch() {
        Controller.FlushOperationBatch();
    }

    void CancelOperationBatch() {
        Controller.CancelOperationBatch();
    }

    bool IsBatching() const {
        return Controller.IsBatching();
    }

    ui32 GetBatchSize() const {
        return Controller.GetBatchSize();
    }

    bool IsBatchReady() const {
        return Controller.IsBatchReady();
    }
};

/**
 * RAII helper for automatic batch management
 *
 * Usage:
 *   {
 *       TOperationOrderBatchScope batch(orderedRuntime, 4);
 *       // Submit operations - they will be batched
 *   } // Automatically flushed on scope exit
 */
class TOperationOrderBatchScope {
private:
    TOperationOrderRuntimeController& Controller;
    bool AutoFlush;
    bool Flushed = false;

public:
    TOperationOrderBatchScope(TOperationOrderRuntimeController& ctrl, ui32 expectedSize = 0, bool autoFlush = true)
        : Controller(ctrl)
        , AutoFlush(autoFlush)
    {
        Controller.BeginOperationBatch(expectedSize);
    }

    TOperationOrderBatchScope(TOperationOrderRuntime& runtime, ui32 expectedSize = 0, bool autoFlush = true)
        : Controller(runtime.GetController())
        , AutoFlush(autoFlush)
    {
        Controller.BeginOperationBatch(expectedSize);
    }

    ~TOperationOrderBatchScope() {
        if (AutoFlush && !Flushed && Controller.IsBatching()) {
            Controller.FlushOperationBatch();
        }
    }

    void Flush() {
        if (!Flushed) {
            Controller.FlushOperationBatch();
            Flushed = true;
        }
    }

    void Cancel() {
        if (!Flushed) {
            Controller.CancelOperationBatch();
            Flushed = true;
        }
    }
};

} // namespace NSchemeShardUT_Private
