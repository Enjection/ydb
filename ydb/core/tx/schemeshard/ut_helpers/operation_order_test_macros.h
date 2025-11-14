#pragma once

#include "operation_order_controller.h"

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/common/env.h>

#include <util/string/cast.h>

namespace NSchemeShardUT_Private {

/**
 * Helper to get test parameters with type conversion
 */
inline TString GetTestParamStr(const char* name, const TString& defaultValue = "") {
    return GetTestParam(name, defaultValue);
}

inline ui32 GetTestParamUi32(const char* name, ui32 defaultValue = 0) {
    TString value = GetTestParam(name, "");
    return value.empty() ? defaultValue : FromString<ui32>(value);
}

} // namespace NSchemeShardUT_Private

/**
 * Y_UNIT_TEST_WITH_ORDER_SHUFFLE - Macro for random shuffle testing
 *
 * Creates a test that runs with both default order and random shuffled orders.
 * The test implementation receives the order mode as a template parameter.
 *
 * Example:
 *   Y_UNIT_TEST_WITH_ORDER_SHUFFLE(MyTest) {
 *       // Test implementation that uses mode template parameter
 *   }
 *
 * This will generate:
 *   - MyTest_Default: Runs once with natural order
 *   - MyTest_Random: Runs 10 times with random orders
 */
#define Y_UNIT_TEST_WITH_ORDER_SHUFFLE(N)                                    \
    template<NSchemeShardUT_Private::TOperationOrderController::EOrderMode mode> \
    void N##_impl(NUnitTest::TTestContext&);                                 \
    struct TTestRegistration##N {                                            \
        TTestRegistration##N() {                                             \
            TCurrentTest::AddTest(#N "_Default",                             \
                [](NUnitTest::TTestContext& ctx) {                           \
                    N##_impl<NSchemeShardUT_Private::TOperationOrderController::Default>(ctx); \
                }, false);                                                    \
            TCurrentTest::AddTest(#N "_Random",                              \
                [](NUnitTest::TTestContext& ctx) {                           \
                    for (int i = 0; i < 10; ++i) {                          \
                        N##_impl<NSchemeShardUT_Private::TOperationOrderController::Random>(ctx); \
                    }                                                         \
                }, false);                                                    \
        }                                                                     \
    };                                                                        \
    static TTestRegistration##N testRegistration##N;                         \
    template<NSchemeShardUT_Private::TOperationOrderController::EOrderMode mode> \
    void N##_impl(NUnitTest::TTestContext&)

/**
 * Y_UNIT_TEST_ALL_ORDERS - Macro for exhaustive permutation testing
 *
 * Creates a test marked as ya:manual that tests all or sampled permutations.
 * Supports configurable sampling strategies via test parameters.
 *
 * Test Parameters:
 *   --test-param max_permutations=N     - Limit number of permutations tested
 *   --test-param sampling_strategy=S    - Strategy: "all", "random", "distributed", "first"
 *
 * Example:
 *   Y_UNIT_TEST_ALL_ORDERS(MyTest, 4) {
 *       // Test implementation receives order vector
 *       // Will test all 24 permutations of 4 operations
 *   }
 *
 * Running:
 *   ya make -ttt --test-tag=ya:manual
 *   ya make -ttt --test-tag=ya:manual --test-param max_permutations=100 --test-param sampling_strategy=random
 */
#define Y_UNIT_TEST_ALL_ORDERS(N, MaxOps)                                    \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order);    \
    Y_UNIT_TEST(N) {                                                         \
        using namespace NSchemeShardUT_Private;                              \
        /* Read test parameters */                                           \
        ui32 maxPermutations = GetTestParamUi32("max_permutations", 0);     \
        TString samplingStrategy = GetTestParamStr("sampling_strategy", "all"); \
                                                                             \
        auto testFunc = [&](const TVector<ui64>& order) {                   \
            N##_impl(CurrentTest, order);                                    \
        };                                                                    \
                                                                             \
        TestAllPermutations(MaxOps, testFunc, maxPermutations, samplingStrategy); \
    }                                                                         \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order)

/**
 * Y_UNIT_TEST_WITH_SPECIFIC_ORDER - Macro for deterministic order testing
 *
 * Creates a test that runs with a specific pre-defined operation order.
 * Useful for regression testing of known problematic orderings.
 *
 * Example:
 *   Y_UNIT_TEST_WITH_SPECIFIC_ORDER(MyTest, 2, 0, 1, 3) {
 *       // Test will run with operations in order: 2, 0, 1, 3
 *   }
 */
#define Y_UNIT_TEST_WITH_SPECIFIC_ORDER(N, ...)                              \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order);    \
    Y_UNIT_TEST(N) {                                                         \
        TVector<ui64> order = {__VA_ARGS__};                                \
        N##_impl(CurrentTest, order);                                        \
    }                                                                         \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order)
