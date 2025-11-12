#pragma once

#include "operation_order_controller.h"
#include "schemeshard__operation.h"

#include <util/generic/vector.h>
#include <util/generic/ptr.h>

namespace NKikimr::NSchemeShard {

/**
 * Test mode support for operation order testing
 *
 * This provides infrastructure to control the order of operation/subop/part
 * execution in SchemeShard for testing purposes. Operations naturally shuffle
 * due to async processing, and this allows manual control of that shuffling.
 */
class TOperationOrderTestMode {
public:
    struct TBatchedPart {
        ISubOperation::TPtr Part;
        TOperation::TPtr Operation;
        ui64 TxId;
        TSubTxId SubTxId;

        TBatchedPart(ISubOperation::TPtr part, TOperation::TPtr operation, ui64 txId, TSubTxId subTxId)
            : Part(std::move(part))
            , Operation(std::move(operation))
            , TxId(txId)
            , SubTxId(subTxId)
        {}
    };

private:
    bool Enabled = false;
    bool Batching = false;
    ui32 ExpectedBatchSize = 0;
    TVector<TBatchedPart> BatchedParts;
    NSchemeShardUT_Private::TOperationOrderController* Controller = nullptr;

public:
    /**
     * Check if test mode is enabled
     */
    bool IsEnabled() const {
        return Enabled;
    }

    /**
     * Enable test mode
     */
    void Enable() {
        Enabled = true;
    }

    /**
     * Disable test mode
     */
    void Disable() {
        Enabled = false;
        Batching = false;
        BatchedParts.clear();
    }

    /**
     * Set the operation order controller
     */
    void SetController(NSchemeShardUT_Private::TOperationOrderController* ctrl) {
        Controller = ctrl;
    }

    /**
     * Get the controller
     */
    NSchemeShardUT_Private::TOperationOrderController* GetController() {
        return Controller;
    }

    /**
     * Check if currently batching
     */
    bool IsBatching() const {
        return Batching;
    }

    /**
     * Begin batching operations/parts
     */
    void BeginBatch(ui32 expectedSize = 0) {
        Y_ENSURE(Enabled, "Test mode not enabled");
        Y_ENSURE(!Batching, "Already batching");
        Batching = true;
        ExpectedBatchSize = expectedSize;
        BatchedParts.clear();
        if (expectedSize > 0) {
            BatchedParts.reserve(expectedSize);
        }
    }

    /**
     * Add a part to the batch
     */
    void AddToBatch(ISubOperation::TPtr part, TOperation::TPtr operation, ui64 txId, TSubTxId subTxId) {
        Y_ENSURE(Batching, "Not in batching mode");
        BatchedParts.emplace_back(std::move(part), std::move(operation), txId, subTxId);
    }

    /**
     * Get batched parts (for flushing)
     */
    const TVector<TBatchedPart>& GetBatchedParts() const {
        return BatchedParts;
    }

    /**
     * Check if batch is ready to flush
     */
    bool IsBatchReady() const {
        return Batching &&
               ExpectedBatchSize > 0 &&
               BatchedParts.size() >= ExpectedBatchSize;
    }

    /**
     * Reorder batched parts according to controller
     * Returns reordered parts
     */
    TVector<TBatchedPart> ReorderAndFlush() {
        Y_ENSURE(Batching, "Not in batching mode");

        TVector<TBatchedPart> result;

        if (!Controller || BatchedParts.empty()) {
            // No controller or no parts - return in original order
            result = std::move(BatchedParts);
        } else {
            // Reorder according to controller
            TVector<ui64> indices;
            for (ui64 i = 0; i < BatchedParts.size(); ++i) {
                indices.push_back(i);
            }

            auto orderedIndices = Controller->GetNextOrder(indices);

            result.reserve(orderedIndices.size());
            for (ui64 idx : orderedIndices) {
                result.push_back(std::move(BatchedParts[idx]));
            }
        }

        // Reset batching state
        BatchedParts.clear();
        Batching = false;
        ExpectedBatchSize = 0;

        return result;
    }

    /**
     * Cancel batching without flushing
     */
    void CancelBatch() {
        BatchedParts.clear();
        Batching = false;
        ExpectedBatchSize = 0;
    }

    /**
     * Get batch size
     */
    ui32 GetBatchSize() const {
        return BatchedParts.size();
    }
};

} // namespace NKikimr::NSchemeShard
