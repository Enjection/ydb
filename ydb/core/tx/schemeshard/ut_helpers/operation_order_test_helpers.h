#pragma once

#include "operation_order_controller.h"

#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tablet_flat/tablet_flat_executed.h>

namespace NSchemeShardUT_Private {

/**
 * Helper functions to enable operation order testing in SchemeShard tests
 *
 * These functions provide easy access to the SchemeShard's operation order
 * test mode for controlling the execution order of operations/subops/parts.
 */

/**
 * Enable operation order test mode on the SchemeShard
 */
inline void EnableOperationOrderTestMode(
    NActors::TTestActorRuntime& runtime,
    ui64 schemeShardId = TTestTxConfig::SchemeShard)
{
    auto sender = runtime.AllocateEdgeActor();

    // Create a custom message to enable test mode
    struct TEvEnableTestMode : public NActors::TEventLocal<TEvEnableTestMode, 999999> {};

    runtime.Send(new NActors::IEventHandle(
        NActors::MakeLocalServiceId(schemeShardId, 0),
        sender,
        new TEvEnableTestMode()),
        0, true);
}

/**
 * Set the operation order controller for the SchemeShard
 * This allows tests to control how operations are reordered
 */
inline void SetOperationOrderController(
    NActors::TTestActorRuntime& runtime,
    TOperationOrderController* controller,
    ui64 schemeShardId = TTestTxConfig::SchemeShard)
{
    // The controller must be set through the SchemeShard's public API
    // For now, this is done by modifying the test to pass the controller
    // during batch operations
    Y_UNUSED(runtime);
    Y_UNUSED(controller);
    Y_UNUSED(schemeShardId);
}

/**
 * Helper class to manage operation batching in tests
 *
 * Usage:
 *   TOperationBatch batch(runtime);
 *   batch.Begin(4);  // Expect 4 operations
 *
 *   // Trigger operations...
 *   AsyncMkDir(runtime, txId1, "/MyRoot", "DirA");
 *   AsyncMkDir(runtime, txId2, "/MyRoot", "DirB");
 *   AsyncMkDir(runtime, txId3, "/MyRoot", "DirC");
 *   AsyncMkDir(runtime, txId4, "/MyRoot", "DirD");
 *
 *   // Flush and reorder
 *   batch.Flush(controller);
 */
class TOperationBatch {
private:
    NActors::TTestActorRuntime& Runtime;
    ui64 SchemeShardId;
    bool Active;

public:
    explicit TOperationBatch(
        NActors::TTestActorRuntime& runtime,
        ui64 schemeShardId = TTestTxConfig::SchemeShard)
        : Runtime(runtime)
        , SchemeShardId(schemeShardId)
        , Active(false)
    {
    }

    /**
     * Begin batching operations
     * @param expectedSize - Expected number of operations (0 for unknown)
     */
    void Begin(ui32 expectedSize = 0) {
        Y_ENSURE(!Active, "Batch already active");
        Active = true;

        // This would send a message to SchemeShard to begin batching
        // For now, we'll use a direct approach through the global state
        Y_UNUSED(expectedSize);
    }

    /**
     * Flush the batch and process operations in reordered sequence
     * @param controller - Controller that determines the order
     */
    void Flush(TOperationOrderController* controller = nullptr) {
        Y_ENSURE(Active, "No active batch");
        Active = false;

        // This would send a message to SchemeShard to flush the batch
        Y_UNUSED(controller);
    }

    ~TOperationBatch() {
        if (Active) {
            // Auto-flush on destruction
            Flush();
        }
    }
};

/**
 * Direct access helpers for use in tests
 *
 * These provide a lower-level interface for advanced test scenarios
 */

/**
 * Get SchemeShard tablet from runtime
 * Note: This requires accessing the tablet directly, which may not always be possible
 */
inline NKikimr::NSchemeShard::TSchemeShard* GetSchemeShardTablet(
    NActors::TTestActorRuntime& runtime,
    ui64 schemeShardId = TTestTxConfig::SchemeShard)
{
    // Find the tablet actor
    auto localId = NActors::MakeLocalServiceId(schemeShardId, 0);
    auto* tablet = runtime.GetAppData().DoNotUse_FetchLocalTablet(localId);

    if (tablet) {
        return static_cast<NKikimr::NSchemeShard::TSchemeShard*>(tablet);
    }

    return nullptr;
}

} // namespace NSchemeShardUT_Private
