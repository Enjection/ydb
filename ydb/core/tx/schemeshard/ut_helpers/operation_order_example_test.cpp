/**
 * Example test demonstrating the Operation Order Controller framework
 *
 * This file shows how to use the various ordering strategies
 * to test parallel operations in SchemeShard.
 *
 * NOTE: This is an example file for documentation purposes.
 * It is not built or run by default.
 */

#include "operation_order_test_macros.h"
#include "operation_order_controller.h"
#include "helpers.h"

#include <library/cpp/testing/unittest/registar.h>

using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(TOperationOrderExamples) {

    /**
     * Example 1: Simple test with random shuffling
     * This test runs with both default order and 10 random orders
     */
    Y_UNIT_TEST_WITH_ORDER_SHUFFLE(SimpleRandomShuffleExample) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

        // Note: 'mode' template parameter is automatically provided by the macro
        TOperationOrderController controller;
        controller.SetMode(mode, 42);  // seed = 42 for reproducibility

        // Collect operations to be reordered
        TVector<ui64> txIds;
        for (int i = 1; i <= 4; ++i) {
            txIds.push_back(++txId);
        }

        // Get the ordered version
        auto orderedTxIds = controller.GetNextOrder(txIds);

        // Submit operations in the specified order
        for (size_t i = 0; i < orderedTxIds.size(); ++i) {
            ui64 seqNum = orderedTxIds[i] - 100;  // Calculate original sequence number
            TestCreateSequence(runtime, orderedTxIds[i], "/MyRoot", Sprintf(R"(
                Name: "seq%lu"
            )", seqNum));
        }

        // Wait for all operations
        env.TestWaitNotification(runtime, orderedTxIds);

        // Verify all sequences created successfully (order-independent verification)
        for (int i = 1; i <= 4; ++i) {
            TestLs(runtime, Sprintf("/MyRoot/seq%d", i), false, NLs::PathExist);
        }
    }

    /**
     * Example 2: Exhaustive testing with all permutations
     * This test tries all possible orderings (4! = 24 permutations)
     *
     * Run with:
     *   ya make -ttt --test-tag=ya:manual
     *   ya make -ttt --test-tag=ya:manual --test-param max_permutations=10 --test-param sampling_strategy=random
     */
    Y_UNIT_TEST_ALL_ORDERS(ExhaustivePermutationExample, 4) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

        // 'order' parameter contains the permutation: e.g., [2, 0, 3, 1]
        Cerr << "Testing permutation: ";
        for (auto idx : order) {
            Cerr << idx << " ";
        }
        Cerr << Endl;

        // Map order indices to operations
        TVector<ui64> txIds;
        for (ui64 i : order) {
            txIds.push_back(++txId);
        }

        // Submit operations in the specified order
        for (size_t i = 0; i < order.size(); ++i) {
            ui64 seqNum = order[i] + 1;  // Convert 0-based to 1-based
            TestCreateSequence(runtime, txIds[i], "/MyRoot", Sprintf(R"(
                Name: "seq%lu"
            )", seqNum));
        }

        // Wait for all operations
        env.TestWaitNotification(runtime, txIds);

        // Verify results are consistent regardless of order
        for (ui64 i = 1; i <= 4; ++i) {
            TestLs(runtime, Sprintf("/MyRoot/seq%lu", i), false, NLs::PathExist);
        }
    }

    /**
     * Example 3: Test a specific problematic order
     * Useful for regression testing when a specific order causes issues
     */
    Y_UNIT_TEST_WITH_SPECIFIC_ORDER(SpecificOrderRegressionExample, 2, 0, 1, 3) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

        // 'order' parameter is {2, 0, 1, 3} as specified in macro
        Cerr << "Testing specific problematic order: ";
        for (auto idx : order) {
            Cerr << idx << " ";
        }
        Cerr << Endl;

        TVector<ui64> txIds;
        for (ui64 i : order) {
            txIds.push_back(++txId);
        }

        // Submit operations
        for (size_t i = 0; i < order.size(); ++i) {
            ui64 seqNum = order[i] + 1;
            TestCreateSequence(runtime, txIds[i], "/MyRoot", Sprintf(R"(
                Name: "seq%lu"
            )", seqNum));
        }

        env.TestWaitNotification(runtime, txIds);

        // Verify
        for (ui64 i = 1; i <= 4; ++i) {
            TestLs(runtime, Sprintf("/MyRoot/seq%lu", i), false, NLs::PathExist);
        }
    }

    /**
     * Example 4: Manual controller usage without macros
     * For maximum control over the test logic
     */
    Y_UNIT_TEST(ManualControllerExample) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

        // Create controller manually
        TOperationOrderController controller;
        controller.SetMode(TOperationOrderController::Random, 12345);

        // Run multiple iterations with different random orders
        for (int iteration = 0; iteration < 5; ++iteration) {
            Cerr << "Iteration " << iteration << " with seed " << controller.GetSeed() << Endl;

            // Collect operations
            TVector<ui64> txIds;
            for (int i = 1; i <= 3; ++i) {
                txIds.push_back(++txId);
            }

            // Reorder
            auto orderedTxIds = controller.GetNextOrder(txIds);

            Cerr << "Order: ";
            for (auto id : orderedTxIds) {
                Cerr << id << " ";
            }
            Cerr << Endl;

            // Submit and verify
            for (size_t i = 0; i < orderedTxIds.size(); ++i) {
                ui64 seqNum = orderedTxIds[i] - 100;
                TestCreateSequence(runtime, orderedTxIds[i], "/MyRoot", Sprintf(R"(
                    Name: "seq_iter%d_num%lu"
                )", iteration, seqNum));
            }

            env.TestWaitNotification(runtime, orderedTxIds);
        }
    }

    /**
     * Example 5: Testing with different operation types
     * Shows how to handle mixed operations (create, alter, etc.)
     */
    Y_UNIT_TEST_ALL_ORDERS(MixedOperationTypesExample, 3) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

        // First, create a table that we'll alter
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Now test parallel operations in different orders
        // Operation 0: Create a sequence
        // Operation 1: Create another table
        // Operation 2: Alter the first table

        TVector<std::function<ui64()>> operations;

        // Op 0: Create sequence
        operations.push_back([&]() {
            ui64 id = ++txId;
            TestCreateSequence(runtime, id, "/MyRoot", R"(Name: "seq1")");
            return id;
        });

        // Op 1: Create table
        operations.push_back([&]() {
            ui64 id = ++txId;
            TestCreateTable(runtime, id, "/MyRoot", R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Uint64" }
                KeyColumnNames: ["key"]
            )");
            return id;
        });

        // Op 2: Alter table
        operations.push_back([&]() {
            ui64 id = ++txId;
            TestAlterTable(runtime, id, "/MyRoot", R"(
                Name: "Table1"
                Columns { Name: "value2" Type: "Utf8" }
            )");
            return id;
        });

        // Execute in the specified order
        TVector<ui64> txIds;
        for (ui64 idx : order) {
            txIds.push_back(operations[idx]());
        }

        // Wait for all
        env.TestWaitNotification(runtime, txIds);

        // Verify all operations succeeded
        TestLs(runtime, "/MyRoot/seq1", false, NLs::PathExist);
        TestLs(runtime, "/MyRoot/Table2", false, NLs::PathExist);
        // Could add more detailed verification here
    }
}
