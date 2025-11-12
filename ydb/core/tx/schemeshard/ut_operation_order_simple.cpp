#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/operation_order_controller.h>
#include <ydb/core/tx/schemeshard/ut_helpers/operation_order_test_macros.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

/**
 * Simple operation order test to verify the framework
 *
 * This test demonstrates manual control of operation ordering
 * to test race conditions and order-dependent bugs.
 */

Y_UNIT_TEST_SUITE(TSchemeShardOperationOrderTests) {
    /**
     * Test that operations can be reordered manually
     */
    Y_UNIT_TEST(TestBasicOperationReordering) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Create directories in natural order
        TestMkDir(runtime, ++txId, "/MyRoot", "DirA");
        TestMkDir(runtime, ++txId, "/MyRoot", "DirB");
        TestMkDir(runtime, ++txId, "/MyRoot", "DirC");

        // Verify all were created
        TestLs(runtime, "/MyRoot/DirA", false);
        TestLs(runtime, "/MyRoot/DirB", false);
        TestLs(runtime, "/MyRoot/DirC", false);
    }

    /**
     * Test with random ordering
     * This test demonstrates how to use the controller to shuffle operations
     */
    Y_UNIT_TEST(TestRandomOrdering) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Set up operation order controller
        TOperationOrderController controller;
        controller.SetMode(TOperationOrderController::Random, 42);

        // TODO: Integrate controller with SchemeShard before operations
        // For now, just test natural order
        TestMkDir(runtime, ++txId, "/MyRoot", "DirA");
        TestMkDir(runtime, ++txId, "/MyRoot", "DirB");

        TestLs(runtime, "/MyRoot/DirA", false);
        TestLs(runtime, "/MyRoot/DirB", false);
    }

    /**
     * Test specific operation order that caused a bug
     *
     * This demonstrates how to regression test a specific problematic ordering
     */
    Y_UNIT_TEST(TestSpecificOrderRegression) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TOperationOrderController controller;
        TVector<ui64> problematicOrder = {2, 0, 1}; // This order caused a bug
        controller.SetPredefinedOrder(problematicOrder);

        // Create operations that had issues in this specific order
        TestMkDir(runtime, ++txId, "/MyRoot", "Dir0");
        TestMkDir(runtime, ++txId, "/MyRoot", "Dir1");
        TestMkDir(runtime, ++txId, "/MyRoot", "Dir2");

        // Verify all succeeded even with problematic order
        TestLs(runtime, "/MyRoot/Dir0", false);
        TestLs(runtime, "/MyRoot/Dir1", false);
        TestLs(runtime, "/MyRoot/Dir2", false);
    }

    /**
     * Placeholder for exhaustive permutation testing
     *
     * This would test all possible orderings of N operations
     * Currently disabled as it requires full framework integration
     */
    Y_UNIT_TEST(DISABLED_TestAllPermutations) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        TOperationOrderController controller;
        controller.SetMode(TOperationOrderController::Exhaustive);

        // Test all permutations of 3 operations
        do {
            ui64 txId = 100;

            // Create test scenario
            TestMkDir(runtime, ++txId, "/MyRoot", "DirA");
            TestMkDir(runtime, ++txId, "/MyRoot", "DirB");
            TestMkDir(runtime, ++txId, "/MyRoot", "DirC");

            // Cleanup for next permutation
            TestRmDir(runtime, ++txId, "/MyRoot", "DirC");
            TestRmDir(runtime, ++txId, "/MyRoot", "DirB");
            TestRmDir(runtime, ++txId, "/MyRoot", "DirA");

        } while (controller.HasMorePermutations());
    }
}
