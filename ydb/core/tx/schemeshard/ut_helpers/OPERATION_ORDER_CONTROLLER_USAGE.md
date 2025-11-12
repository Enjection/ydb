# Operation Order Controller - Usage Guide

## Overview

The Operation Order Controller framework provides tools to test different execution orderings of parallel operations in SchemeShard tests. This helps catch order-dependent bugs and race conditions.

## Quick Start

### 1. Basic Usage with Test Macros

The simplest way to use the framework is with the provided test macros:

```cpp
#include <ydb/core/tx/schemeshard/ut_helpers/operation_order_test_macros.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(TMyTestSuite) {
    // Test with random shuffling (runs 10 times with different orders)
    Y_UNIT_TEST_WITH_ORDER_SHUFFLE(MyParallelTest) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Configure the test
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

        // Your test logic here
        // The 'mode' template parameter is automatically set by the macro
    }

    // Test all permutations (for small operation sets)
    // This test is marked as ya:manual in ya.make
    Y_UNIT_TEST_ALL_ORDERS(MyExhaustiveTest, 4) {
        // Test receives 'order' parameter with permutation indices
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        // Use the order vector to control operation submission
        // ...
    }

    // Test specific problematic order
    Y_UNIT_TEST_WITH_SPECIFIC_ORDER(MyRegressionTest, 2, 0, 1, 3) {
        // Test with operations in order: 2, 0, 1, 3
        // ...
    }
}
```

### 2. Manual Integration with TOperationOrderController

For more control, you can use `TOperationOrderController` directly:

```cpp
#include <ydb/core/tx/schemeshard/ut_helpers/operation_order_controller.h>

Y_UNIT_TEST(ManualOrderTest) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);

    // Create controller
    TOperationOrderController controller;
    controller.SetMode(TOperationOrderController::Random, 42); // seed = 42

    // Collect operations
    TVector<ui64> txIds;
    for (int i = 0; i < 4; ++i) {
        txIds.push_back(++txId);
    }

    // Get ordered operations
    auto orderedTxIds = controller.GetNextOrder(txIds);

    // Submit operations in the controlled order
    for (ui64 id : orderedTxIds) {
        // Submit operation with id
    }
}
```

## Operation Ordering Strategies

### Default Mode
Operations execute in natural (arrival) order.
- **Use case**: Regular tests, baseline behavior
- **Performance**: No overhead

```cpp
controller.SetMode(TOperationOrderController::Default);
```

### Random Mode
Operations are shuffled randomly with a seed.
- **Use case**: Quick smoke testing, CI/CD
- **Performance**: Fast (10-100 iterations)
- **Reproducibility**: Yes (with seed)

```cpp
controller.SetMode(TOperationOrderController::Random, seed);
```

### Exhaustive Mode
Tests all permutations.
- **Use case**: Critical paths, pre-release validation
- **Performance**: Expensive (N! permutations)
- **Limit**: Use only for ≤5 operations

```cpp
controller.SetMode(TOperationOrderController::Exhaustive);

// Example: iterate through all permutations
TVector<ui64> ops = {0, 1, 2, 3};
do {
    auto ordered = controller.GetNextOrder(ops);
    // Test with this ordering
} while (controller.HasMorePermutations());
```

### Deterministic Mode
Use a specific pre-defined order.
- **Use case**: Regression testing, known bugs
- **Performance**: Single run

```cpp
controller.SetPredefinedOrder({2, 0, 1, 3}); // Specific order
```

## Test Parameters

For exhaustive tests marked as `ya:manual`, control behavior via command-line:

```bash
# Test all permutations (default)
ya make -ttt --test-tag=ya:manual

# Random sampling with 100 permutations
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=random

# Distributed sampling (evenly spaced)
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=distributed

# First N permutations
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=50 \
    --test-param sampling_strategy=first
```

### Sampling Strategies

| Strategy | Description | Best For |
|----------|-------------|----------|
| `all` (default) | Tests every permutation | ≤5 operations |
| `random` | Random sampling with fixed seed | Large sets (6+ ops) |
| `distributed` | Evenly spaced through permutation space | Good coverage |
| `first` | First N permutations sequentially | Quick validation |

## Example: Updating an Existing Test

### Before (Original Test)
```cpp
Y_UNIT_TEST(CreateSequenceParallel) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    ui64 txId = 100;

    runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

    for (int i = 1; i <= 4; ++i) {
        TestCreateSequence(runtime, ++txId, "/MyRoot", Sprintf(R"(
            Name: "seq%d"
        )", i));
    }
    env.TestWaitNotification(runtime, {txId-3, txId-2, txId-1, txId});
}
```

### After (With Order Control)
```cpp
Y_UNIT_TEST_ALL_ORDERS(CreateSequenceParallel, 4) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    ui64 txId = 100;

    runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);

    // Map order indices to operations
    TVector<ui64> txIds;
    TVector<TString> sequences;

    for (ui64 i : order) {
        sequences.push_back(Sprintf("seq%lu", i + 1));
        txIds.push_back(++txId);
    }

    // Submit operations in the specified order
    for (size_t i = 0; i < order.size(); ++i) {
        TestCreateSequence(runtime, txIds[i], "/MyRoot", Sprintf(R"(
            Name: "%s"
        )", sequences[i].data()));
    }

    // Wait for all
    env.TestWaitNotification(runtime, txIds);

    // Verify results are consistent regardless of order
    for (const auto& seq : sequences) {
        TestLs(runtime, "/MyRoot/" + seq, false, NLs::PathExist);
    }
}
```

### ya.make Configuration
```ya.make
UNITTEST()

SRCS(
    ut_sequence.cpp
)

# Mark exhaustive test as manual
TAG(
    ya:manual
)

PEERDIR(
    ydb/core/tx/schemeshard/ut_helpers
    # ... other dependencies
)

END()
```

## Advanced Usage: Custom Operation Batching

For complex scenarios, implement custom batching logic:

```cpp
Y_UNIT_TEST(ComplexOrderingTest) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);

    TOperationOrderController controller;
    controller.SetMode(TOperationOrderController::Random, 42);

    // Phase 1: Create operations
    TVector<std::function<void()>> operations;
    for (int i = 0; i < 4; ++i) {
        operations.push_back([&, i]() {
            TestCreateSequence(runtime, txId++, "/MyRoot",
                Sprintf(R"(Name: "seq%d")", i));
        });
    }

    // Get indices for this order
    TVector<ui64> indices;
    for (ui64 i = 0; i < operations.size(); ++i) {
        indices.push_back(i);
    }

    // Apply ordering
    auto orderedIndices = controller.GetNextOrder(indices);

    // Execute in order
    for (ui64 idx : orderedIndices) {
        operations[idx]();
    }

    // Wait and verify
    // ...
}
```

## Debugging Tips

### 1. Finding Problematic Orders

When a test fails with a specific order:

```cpp
Y_UNIT_TEST(DebugTest) {
    TOperationOrderController controller;
    controller.SetMode(TOperationOrderController::Random, failingSeed);

    // Add detailed logging
    Cerr << "Testing with seed: " << controller.GetSeed() << Endl;

    auto order = controller.GetNextOrder(operations);
    Cerr << "Order: ";
    for (auto idx : order) {
        Cerr << idx << " ";
    }
    Cerr << Endl;

    // Run test...
}
```

### 2. Reproducing Failures

Use the seed from failed test runs:

```cpp
// In CI log: "Testing with seed: 12345"
controller.SetMode(TOperationOrderController::Random, 12345);
```

### 3. Isolating Order Dependencies

```cpp
Y_UNIT_TEST_WITH_SPECIFIC_ORDER(IsolateRaceCondition, 3, 1, 0, 2) {
    // Test the exact problematic order
    // Add assertions to understand the state
}
```

## Performance Guidelines

### Test Complexity

| Operations | Permutations | Strategy | Estimated Time |
|------------|--------------|----------|----------------|
| 2 | 2 | all | <1s |
| 3 | 6 | all | <1s |
| 4 | 24 | all | 1-5s |
| 5 | 120 | all or sample | 5-30s |
| 6 | 720 | sample | 30s-2m |
| 7+ | 5,040+ | random sample | 2-10m |

### Recommendations

- **CI/CD**: Use Random mode with 10-20 iterations
- **Pre-commit**: Use Random mode with 5-10 iterations
- **Nightly**: Use Exhaustive for ≤5 ops, Random sample for larger
- **Pre-release**: Full exhaustive for critical paths

## Integration Checklist

When adding order control to a test:

- [ ] Identify parallel operations in the test
- [ ] Choose appropriate macro or manual integration
- [ ] Add operation ordering logic
- [ ] Verify results are order-independent
- [ ] Add logging for debugging
- [ ] Mark exhaustive tests with `ya:manual` tag
- [ ] Document expected behavior
- [ ] Test with multiple seeds/orders

## Common Patterns

### Pattern 1: Parallel Creation Operations
```cpp
Y_UNIT_TEST_ALL_ORDERS(ParallelCreate, NumOps) {
    // Submit all operations based on order
    // Wait for completion
    // Verify all created successfully
}
```

### Pattern 2: Mixed Operation Types
```cpp
Y_UNIT_TEST_ALL_ORDERS(MixedOperations, 5) {
    // Operations: create, alter, create, split, alter
    // Map order indices to operation types
    // Execute in specified order
    // Verify final state is consistent
}
```

### Pattern 3: Dependent Operations with Controlled Order
```cpp
Y_UNIT_TEST(DependentOps) {
    // Phase 1: Operations that can be reordered
    auto orderedPhase1 = controller.GetNextOrder(phase1Ops);
    // Execute phase1

    // Phase 2: Operations dependent on phase1
    auto orderedPhase2 = controller.GetNextOrder(phase2Ops);
    // Execute phase2
}
```

## References

- Main controller: `operation_order_controller.h`
- Test macros: `operation_order_test_macros.h`
- Test plan: `PARALLEL_OPERATIONS_TESTING_PLAN.md`
- Example tests: `ut_sequence/ut_sequence.cpp`

## Support

For questions or issues:
1. Review test plan: `PARALLEL_OPERATIONS_TESTING_PLAN.md`
2. Check example implementations in test suites
3. Add detailed logging to understand ordering behavior
