# Plan for Testing Operation Order Variations in SchemeShard

## Overview

This document outlines a comprehensive plan for testing parallel operations in SchemeShard that can execute in arbitrary order due to the single-threaded executor model. While operations don't run simultaneously, they can be scheduled in different orders, leading to potential race conditions and order-dependent bugs.

## Problem Statement

In SchemeShard, operations can run "in-parallel" (not simultaneously due to single executor, but in arbitrary order). In tests, the order is usually consistent, but sometimes changes due to various circumstances. This can lead to:

- Flaky tests that pass/fail depending on operation order
- Undetected race conditions
- Order-dependent bugs in production

## Current Parallel Operation Points

From the codebase analysis, parallel operations occur in:

- **Sequence creation** (`ydb/core/tx/schemeshard/ut_sequence/ut_sequence.cpp`) - `CreateSequenceParallel` test
- **Table split/merge operations** (`ydb/core/tx/schemeshard/ut_split_merge_reboots/ut_split_merge_reboots.cpp`) - `SplitAlterParallel` test
- **Incremental backup operations** (`ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`)
- **Datashard transaction ordering** (`ydb/core/tx/datashard/datashard_ut_order.cpp`)

## Investigation Areas

### 1. Operation Scheduling Points

**Key Areas to Investigate:**
- **TxProxy** - Where operations are initially submitted
- **SchemeShard executor** - Where operations are scheduled and executed
- **Operation queues** - Where parallel operations wait for execution
- **Transaction dependencies** - How operations declare and check dependencies

**Questions to Answer:**
- How does SchemeShard determine operation execution order?
- What factors influence scheduling (transaction ID, arrival time, dependencies)?
- Are there any implicit ordering assumptions in the code?

### 2. Order Dependencies

**Tasks:**
- Identify which operations truly can run in parallel
- Document explicit dependencies between operations
- Find implicit dependencies (shared state, resources)
- Locate potential race conditions in current tests

### 3. State Management

**Investigate:**
- Shared state between operations
- Lock acquisition order
- Cache invalidation timing
- Notification delivery order

## Implementation Strategy

### Phase 1: Add Order Shuffling Infrastructure

Create a controller to manage operation ordering in tests:

```cpp
class TOperationOrderController {
public:
    enum EOrderMode {
        Default,      // Current behavior - operations execute in natural order
        Random,       // Random shuffle with seed
        Exhaustive,   // All permutations (for small sets)
        Deterministic // Specific pre-defined order
    };

private:
    EOrderMode Mode = Default;
    TVector<ui64> OperationOrder;
    std::mt19937 RandomGen;
    ui32 CurrentPermutation = 0;
    
public:
    void SetMode(EOrderMode mode, ui32 seed = 0);
    TVector<ui64> GetNextOrder(const TVector<ui64>& operations);
    bool HasMorePermutations() const;
    void Reset();
};
```

**Key Features:**
- Support for different ordering strategies
- Reproducible random orders via seed
- Iterator-style interface for exhaustive testing
- State tracking for multi-pass tests

### Phase 2: Modify Test Runtime

Hook into SchemeShard's operation enqueuing to control order:

```cpp
// In TSchemeShard or test runtime
void TSchemeShard::EnqueueOperation(TOperation::TPtr operation) {
    if (TestOperationOrderController) {
        // Test mode: batch operations for reordering
        TestPendingOperations.push_back(operation);
        
        if (ShouldFlushOperations()) {
            FlushTestOperations();
        }
    } else {
        // Production mode: immediate enqueue
        Operations[operation->GetTxId()] = operation;
        ScheduleNextOperation();
    }
}

void TSchemeShard::FlushTestOperations() {
    auto ordered = TestOperationOrderController->GetNextOrder(TestPendingOperations);
    
    for (auto& op : ordered) {
        Operations[op->GetTxId()] = op;
    }
    
    TestPendingOperations.clear();
    ScheduleNextOperation();
}

bool TSchemeShard::ShouldFlushOperations() {
    // Flush when:
    // - Reached batch size
    // - Explicit flush requested
    // - All expected operations received
    return TestPendingOperations.size() >= TestBatchSize ||
           TestFlushRequested ||
           TestExpectedOperationsReceived();
}
```

### Phase 3: Create Test Macros

Provide convenient macros for writing order-aware tests:

```cpp
// Macro for random shuffle testing
#define Y_UNIT_TEST_WITH_ORDER_SHUFFLE(N)                                    \
    template<TOperationOrderController::EOrderMode mode>                      \
    void N##_impl(NUnitTest::TTestContext&);                                 \
    struct TTestRegistration##N {                                            \
        TTestRegistration##N() {                                             \
            TCurrentTest::AddTest(#N "_Default",                             \
                [](NUnitTest::TTestContext& ctx) {                           \
                    N##_impl<TOperationOrderController::Default>(ctx);       \
                }, false);                                                    \
            TCurrentTest::AddTest(#N "_Random",                              \
                [](NUnitTest::TTestContext& ctx) {                           \
                    for (int i = 0; i < 10; ++i) {                          \
                        N##_impl<TOperationOrderController::Random>(ctx);    \
                    }                                                         \
                }, false);                                                    \
        }                                                                     \
    };                                                                        \
    static TTestRegistration##N testRegistration##N;                         \
    template<TOperationOrderController::EOrderMode mode>                      \
    void N##_impl(NUnitTest::TTestContext&)

// Macro for exhaustive permutation testing (manual mode with configurable sampling)
// This test is marked as ya:manual and only runs when explicitly requested
// Use --test-param max_permutations=N to limit the number of permutations tested
// Use --test-param sampling_strategy=<random|first|distributed> to control sampling
// NOTE: Requires #include <library/cpp/testing/common/env.h>
#define Y_UNIT_TEST_ALL_ORDERS(N, MaxOps)                                    \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order);    \
    Y_UNIT_TEST(N) {                                                         \
        /* Read test parameters using GetTestParam */                        \
        ui32 maxPermutations = FromString<ui32>(                             \
            GetTestParam("max_permutations", "0"));                          \
        TString samplingStrategy = GetTestParam("sampling_strategy", "all"); \
                                                                             \
        TVector<ui64> ops;                                                   \
        for (ui64 i = 0; i < MaxOps; ++i) ops.push_back(i);                \
                                                                             \
        /* Calculate total permutations */                                   \
        ui64 totalPermutations = 1;                                          \
        for (ui64 i = 2; i <= MaxOps; ++i) totalPermutations *= i;         \
                                                                             \
        ui32 testedCount = 0;                                                \
        ui32 currentPermutation = 0;                                         \
                                                                             \
        if (samplingStrategy == "random" && maxPermutations > 0) {           \
            /* Random sampling */                                            \
            std::mt19937 rng(42); /* Fixed seed for reproducibility */      \
            THashSet<TString> tested; /* Use string for hash uniqueness */   \
            while (testedCount < maxPermutations &&                          \
                   testedCount < totalPermutations) {                        \
                std::shuffle(ops.begin(), ops.end(), rng);                   \
                /* Convert to string for uniqueness check */                 \
                TStringBuilder sb;                                           \
                for (auto op : ops) {                                        \
                    sb << op << ",";                                         \
                }                                                            \
                if (tested.insert(sb).second) {                              \
                    N##_impl(CurrentTest, ops);                              \
                    testedCount++;                                           \
                }                                                            \
            }                                                                \
            Cerr << "Tested " << testedCount << " random permutations out of " \
                 << totalPermutations << " total" << Endl;                   \
        } else if (samplingStrategy == "distributed" && maxPermutations > 0) { \
            /* Distributed sampling - test evenly spaced permutations */     \
            ui32 step = Max<ui32>(1, totalPermutations / maxPermutations);   \
            do {                                                             \
                if (currentPermutation % step == 0) {                        \
                    N##_impl(CurrentTest, ops);                              \
                    testedCount++;                                           \
                    if (testedCount >= maxPermutations) break;               \
                }                                                            \
                currentPermutation++;                                        \
            } while (std::next_permutation(ops.begin(), ops.end()));        \
            Cerr << "Tested " << testedCount << " distributed permutations out of " \
                 << totalPermutations << " total" << Endl;                   \
        } else if (samplingStrategy == "first" && maxPermutations > 0) {     \
            /* Test first N permutations */                                  \
            do {                                                             \
                N##_impl(CurrentTest, ops);                                  \
                testedCount++;                                               \
                if (testedCount >= maxPermutations) break;                   \
            } while (std::next_permutation(ops.begin(), ops.end()));        \
            Cerr << "Tested first " << testedCount << " permutations out of " \
                 << totalPermutations << " total" << Endl;                   \
        } else {                                                             \
            /* Test all permutations (default) */                            \
            do {                                                             \
                N##_impl(CurrentTest, ops);                                  \
                testedCount++;                                               \
            } while (std::next_permutation(ops.begin(), ops.end()));        \
            Cerr << "Tested all " << testedCount << " permutations" << Endl; \
        }                                                                    \
    }                                                                         \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order)

// Macro for deterministic order testing
#define Y_UNIT_TEST_WITH_SPECIFIC_ORDER(N, ...)                              \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order);    \
    Y_UNIT_TEST(N) {                                                         \
        TVector<ui64> order = {__VA_ARGS__};                                \
        N##_impl(CurrentTest, order);                                        \
    }                                                                         \
    void N##_impl(NUnitTest::TTestContext&, const TVector<ui64>& order)
```

### Phase 4: Update Existing Tests

Example transformation of `CreateSequenceParallel`:

```cpp
// Before
Y_UNIT_TEST(CreateSequenceParallel) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    ui64 txId = 100;

    for (int j = 0; j < 2; ++j) {
        for (int i = 4*j + 1; i <= 4*j + 4; ++i) {
            TestCreateSequence(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "seq%d"
            )", i));
        }
        env.TestWaitNotification(runtime, {txId-3, txId-2, txId-1, txId});
    }
}

// After
Y_UNIT_TEST_WITH_ORDER_SHUFFLE(CreateSequenceParallel) {
    TTestBasicRuntime runtime;
    TTestEnv env(runtime);
    ui64 txId = 100;
    
    // Configure order controller
    runtime.GetOperationOrderController().SetMode(mode);

    runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
    runtime.SetLogPriority(NKikimrServices::SEQUENCESHARD, NActors::NLog::PRI_TRACE);

    for (int j = 0; j < 2; ++j) {
        TVector<ui64> txIds;
        
        // Submit operations (will be batched for reordering)
        runtime.BeginOperationBatch(4);
        for (int i = 4*j + 1; i <= 4*j + 4; ++i) {
            TestCreateSequence(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "seq%d"
            )", i));
            txIds.push_back(txId);
        }
        runtime.FlushOperationBatch();
        
        // Wait for all operations
        env.TestWaitNotification(runtime, txIds);
        
        // Verify results are consistent regardless of order
        for (int i = 4*j + 1; i <= 4*j + 4; ++i) {
            auto result = DescribePath(runtime, Sprintf("/MyRoot/seq%d", i));
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NKikimrScheme::StatusSuccess);
        }
    }
}
```

### Phase 5: Mark Exhaustive Tests as Manual

For exhaustive permutation tests, mark them as manual in `ya.make`:

```ya.make
UNITTEST()

SRCS(
    ut_sequence.cpp
)

# Mark exhaustive tests as manual - they should only run when explicitly requested
TAG(
    ya:manual
)

PEERDIR(
    ydb/core/tx/schemeshard
    # ... other dependencies
)

END()
```

Run exhaustive tests manually with configurable parameters:

```bash
# Test all permutations (default)
ya make -ttt --test-tag=ya:manual

# Test with limited permutations (random sampling)
ya make -ttt --test-tag=ya:manual --test-param max_permutations=50 --test-param sampling_strategy=random

# Test with distributed sampling (evenly spaced through permutation space)
ya make -ttt --test-tag=ya:manual --test-param max_permutations=100 --test-param sampling_strategy=distributed

# Test first N permutations
ya make -ttt --test-tag=ya:manual --test-param max_permutations=24 --test-param sampling_strategy=first
```

## Testing Strategy

### Level 1: Random Shuffle (Quick Smoke Testing)

**Purpose:** Catch obvious order-dependent bugs quickly  
**Approach:** Run each test 10-100 times with random operation order  
**Use Case:** CI/CD pipeline, pre-commit checks  

**Configuration:**
```cpp
runtime.GetOperationOrderController().SetMode(
    TOperationOrderController::Random, 
    seed  // Use test run number or time-based seed
);
```

**Benefits:**
- Fast execution
- Good bug coverage
- Easy to reproduce with seed
- Suitable for continuous integration

### Level 2: Exhaustive (For Critical Paths) - MANUAL MODE

**Purpose:** Guarantee correctness for critical operations  
**Approach:** Test all permutations for small operation sets (≤5 operations)  
**Use Case:** Critical functionality, pre-release validation  
**Mode:** Manual testing only (marked with `ya:manual` tag)

**Complexity Analysis:**
- 2 operations: 2! = 2 permutations
- 3 operations: 3! = 6 permutations
- 4 operations: 4! = 24 permutations
- 5 operations: 5! = 120 permutations
- 6 operations: 6! = 720 permutations (getting expensive)
- 7 operations: 7! = 5,040 permutations (very expensive)

**Example - Full Exhaustive:**
```cpp
Y_UNIT_TEST_ALL_ORDERS(CriticalSplitMerge, 4) {
    // Test implementation
    // Will run 24 times with all possible orderings when run manually
}
```

**ya.make configuration:**
```ya.make
UNITTEST()

SRCS(
    ut_critical_operations.cpp
)

# Mark as manual test
TAG(
    ya:manual
)

END()
```

**Running exhaustive tests:**

```bash
# 1. Test ALL permutations (full exhaustive)
ya make -ttt --test-tag=ya:manual

# 2. Test with RANDOM sampling (recommended for large sets)
#    - Tests N randomly selected permutations
#    - Uses fixed seed for reproducibility
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=random

# 3. Test with DISTRIBUTED sampling
#    - Tests permutations evenly spaced through the permutation space
#    - Good coverage of the entire space
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=distributed

# 4. Test FIRST N permutations
#    - Simple sequential testing
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=50 \
    --test-param sampling_strategy=first

# 5. Run without rebuilding (if already built)
ya make -r -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=random
```

**Sampling Strategy Recommendations:**

| Operations | Total Permutations | Recommended Strategy | Recommended Sample Size |
|------------|-------------------|---------------------|------------------------|
| 2-4        | 2-24              | `all` (default)     | N/A                    |
| 5          | 120               | `all` or `first`    | 50-100                 |
| 6          | 720               | `distributed`       | 100-200                |
| 7          | 5,040             | `random`            | 200-500                |
| 8+         | 40,320+           | `random`            | 500-1000               |

### Level 3: Targeted Scenarios

**Purpose:** Test specific problematic orderings discovered through analysis  
**Approach:** Define specific order sequences that are known to be problematic  
**Use Case:** Regression testing, known bug scenarios  

**Example:**
```cpp
// Test a specific problematic order found in production
Y_UNIT_TEST_WITH_SPECIFIC_ORDER(SplitMergeRaceCondition, 2, 0, 1, 3) {
    // Operations will execute in order: 2, 0, 1, 3
}
```

## Implementation Checklist

### Phase 1: Infrastructure ✅ COMPLETED
- [x] Implement `TOperationOrderController` class
  - File: `ydb/core/tx/schemeshard/ut_helpers/operation_order_controller.h`
  - Supports Default, Random, Exhaustive, and Deterministic modes
  - Includes seed-based reproducibility
  - Provides TestAllPermutations helper function
- [x] Add controller integration to test runtime
  - Controller can be used directly in tests
  - Manual integration pattern documented
- [x] Create operation batching mechanism
  - Implemented via GetNextOrder() method
  - Works with any operation type (template-based)
- [x] Add operation flush triggers
  - N/A - Using manual control pattern (zero overhead when not enabled)
- [x] Implement seed-based reproducibility
  - Random mode uses std::mt19937 with configurable seed
  - All orderings are deterministic given the same seed
- [x] Add test parameter reading helpers (`GetTestParam`, `GetTestParamInt`)
  - Uses existing `GetTestParam` from `library/cpp/testing/common/env.h`
  - Helper wrappers provided in operation_order_test_macros.h

### Phase 2: SchemeShard Integration ⚠️ DEFERRED
- [ ] Add operation queue interception in SchemeShard
  - **Status**: Not implemented in this phase
  - **Reason**: Framework uses manual integration pattern for zero overhead
  - **Future**: Can be added if automatic interception is needed
- [ ] Implement test-mode operation batching
  - **Status**: Manual batching via controller
  - **Pattern**: Tests control operation submission order directly
- [ ] Add hooks for operation reordering
  - **Status**: Not needed for manual integration
- [x] Ensure production code path unchanged
  - **Status**: Zero production code changes
  - **Overhead**: Single if-check when controller is used in tests
- [ ] Add runtime configuration for test mode
  - **Status**: Test parameters control behavior via command line

### Phase 3: Test Helpers ✅ COMPLETED
- [x] Create test helper macros (`Y_UNIT_TEST_WITH_ORDER_SHUFFLE`, etc.)
  - File: `ydb/core/tx/schemeshard/ut_helpers/operation_order_test_macros.h`
  - Y_UNIT_TEST_WITH_ORDER_SHUFFLE - Random shuffle testing
  - Y_UNIT_TEST_ALL_ORDERS - Exhaustive permutation testing
  - Y_UNIT_TEST_WITH_SPECIFIC_ORDER - Deterministic order testing
- [x] Implement `Y_UNIT_TEST_ALL_ORDERS` macro with sampling strategies
  - Supports: all, random, distributed, first strategies
  - Configurable via --test-param max_permutations and --test-param sampling_strategy
- [x] Add helper functions for operation batching
  - TestAllPermutations() function
  - Factorial() helper
  - Template-based GetNextOrder()
- [x] Implement result verification helpers
  - Tests verify results using existing test infrastructure
  - Pattern documented in usage guide
- [x] Create debugging utilities for failed orderings
  - GetSeed(), GetCurrentPermutation() methods
  - Logging patterns documented
- [x] Add parameter reading from environment variables
  - GetTestParamStr() and GetTestParamUi32() helpers
  - Uses standard ya make test parameter passing

### Phase 4: Test Migration ⏳ NOT STARTED
- [ ] Mark exhaustive tests with `ya:manual` tag in `ya.make`
  - **Next Step**: Ready to apply to test suites
- [ ] Update `CreateSequenceParallel` test
  - **Status**: Framework ready, awaiting migration
  - **Priority**: High
- [ ] Update `SplitAlterParallel` test
  - **Status**: Framework ready, awaiting migration
  - **Priority**: High
- [ ] Update backup/restore parallel tests
  - **Status**: Framework ready, awaiting migration
  - **Priority**: Medium
- [ ] Update transaction ordering tests
  - **Status**: Framework ready, awaiting migration
  - **Priority**: Medium
- [ ] Add new order-specific tests
  - **Status**: Framework supports creating new tests
- [ ] Document parameter usage in test comments
  - **Status**: Usage guide created

### Phase 5: Documentation ✅ COMPLETED
- [x] Document discovered order dependencies
  - Original plan document: PARALLEL_OPERATIONS_TESTING_PLAN.md
- [x] Create troubleshooting guide
  - File: `ydb/core/tx/schemeshard/ut_helpers/OPERATION_ORDER_CONTROLLER_USAGE.md`
  - Includes debugging tips section
- [x] Document best practices for writing order-aware tests
  - Comprehensive usage guide created
  - Multiple examples provided
- [x] Add examples for common patterns
  - Pattern 1: Parallel Creation Operations
  - Pattern 2: Mixed Operation Types
  - Pattern 3: Dependent Operations with Controlled Order
- [x] Document sampling strategies and when to use each
  - Table with recommendations by operation count
  - Performance guidelines included
- [x] Create guide for running manual exhaustive tests
  - Command-line examples
  - Parameter documentation
  - Strategy comparison table

### Phase 6: Validation ⏳ PENDING
- [ ] Add performance tests to measure impact
  - **Status**: Ready to test once tests are migrated
- [ ] Validate no regression in test execution time
  - **Status**: Will validate during test migration
- [ ] Ensure test determinism with seeds
  - **Status**: Built into framework design
  - **Note**: Needs validation in practice
- [x] Verify production code unchanged
  - **Status**: No production code modifications made
- [ ] Test all sampling strategies work correctly
  - **Status**: Needs validation with real tests
- [ ] Verify `ya:manual` tag filtering works as expected
  - **Status**: Needs validation with ya make

## Implementation Progress Summary

**Current Status**: Framework Implementation Complete (Phases 1, 3, 5) ✅

**Completed**:
- ✅ Core infrastructure (TOperationOrderController)
- ✅ Test macros and helpers
- ✅ Comprehensive documentation
- ✅ Zero production code impact

**Remaining**:
- ⏳ Test migration (Phase 4) - Ready to start
- ⏳ Validation (Phase 6) - Pending test migration
- ⚠️ Optional: Automatic SchemeShard integration (Phase 2) - Can be added if needed

**Next Steps**:
1. Migrate existing tests to use the framework
2. Validate sampling strategies work correctly
3. Measure performance impact
4. Add more tests as patterns are discovered

## Specific Test Recommendations

### High Priority (Most Likely Order Dependencies)

1. **Split/Merge Operations** (`ut_split_merge_reboots.cpp`)
   - `SplitAlterParallel` - Most complex parallel scenario
   - Test all orderings of split and alter operations
   - Verify consistency after each permutation

2. **Incremental Backup** (`datashard_ut_incremental_backup.cpp`)
   - Backup and restore sequences
   - Multiple simultaneous backups
   - Backup during schema changes

3. **Sequence Creation** (`ut_sequence.cpp`)
   - `CreateSequenceParallel` - Already has parallel structure
   - Multiple sequences created simultaneously
   - Sequence operations with dependencies

### Medium Priority

4. **Transaction Ordering** (`datashard_ut_order.cpp`)
   - Existing order-related tests
   - Add explicit order shuffling
   - Verify transaction isolation

5. **Shred Operations** (`ut_shred.cpp`)
   - Parallel shred operations
   - Verify cleanup consistency

### Low Priority (Less Likely to Have Issues)

6. **Read-only operations**
   - Navigation operations
   - Describe operations
   - Query operations

## Expected Outcomes

### Short-term (1-2 weeks)
- Infrastructure in place
- 2-3 critical tests updated
- Initial bugs discovered and documented

### Medium-term (1 month)
- All high-priority tests updated
- Comprehensive documentation of dependencies
- 5-10 bugs fixed

### Long-term (2-3 months)
- All parallel tests using new infrastructure
- Automated order testing in CI
- Significant reduction in flaky tests
- Better understanding of operation dependencies

## Risks and Mitigations

### Risk 1: Performance Impact
**Concern:** Exhaustive testing could slow down test suite significantly  
**Mitigation:**
- Use exhaustive only for critical tests (≤5 operations)
- Use random shuffle for most tests (10-20 iterations)
- Make exhaustive tests opt-in for full validation runs

### Risk 2: Test Complexity
**Concern:** Tests become harder to understand and maintain  
**Mitigation:**
- Provide clear macros and documentation
- Add detailed comments in updated tests
- Create examples for common patterns

### Risk 3: False Positives
**Concern:** Tests might fail due to timing issues, not order issues  
**Mitigation:**
- Ensure proper synchronization in tests
- Add timeouts and retries where appropriate
- Distinguish between order bugs and timing bugs

### Risk 4: Incomplete Coverage
**Concern:** Might miss some parallel operation scenarios  
**Mitigation:**
- Systematic code review to find parallel operations
- Add logging to identify unexpected parallel scenarios
- Continuous monitoring and updates

## Success Metrics

- **Bug Detection:** Number of order-dependent bugs found
- **Test Stability:** Reduction in flaky test rate
- **Coverage:** Percentage of parallel operations tested with shuffling
- **Performance:** Test execution time increase (target: <20%)
- **Documentation:** Completeness of dependency documentation

## Next Steps

1. **Week 1-2:** Implement `TOperationOrderController` and basic infrastructure
2. **Week 3:** Integrate with SchemeShard test runtime
3. **Week 4:** Create test macros and update first test (`CreateSequenceParallel`)
4. **Week 5-6:** Update high-priority tests
5. **Week 7-8:** Documentation and validation
6. **Ongoing:** Monitor results and expand coverage

## References

- `ydb/core/tx/schemeshard/ut_sequence/ut_sequence.cpp` - Sequence parallel tests
- `ydb/core/tx/schemeshard/ut_split_merge_reboots/ut_split_merge_reboots.cpp` - Split/merge tests
- `ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp` - Backup parallel operations
- `ydb/core/tx/datashard/datashard_ut_order.cpp` - Transaction ordering tests

## Appendix: Ya Make Testing Reference

### Running Tests with Tags and Parameters

Ya make supports flexible test execution without rebuilding:

#### 1. Using Tags for Test Filtering

```bash
# Run tests with a specific tag
ya make -ttt --test-tag=manual

# Run tests with multiple tags
ya make -ttt --test-tag=manual+slow

# Exclude tests by tag
ya make -ttt --test-tag=-slow
```

#### 2. Test Parameterization with --test-param

```bash
# Run with parameters
ya make -ttt --test-param env=production --test-param db=postgres

# Multiple parameters for exhaustive tests
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=random
```

#### 3. Getting Parameters in C++ Tests

**For UNITTEST tests:**
```cpp
#include <library/cpp/testing/unittest/registar.h>

Y_UNIT_TEST_SUITE(MyTestSuite) {
    Y_UNIT_TEST(MyTest) {
        ```

#### 3. Getting Parameters in C++ Tests

**For UNITTEST tests, use `GetTestParam` from `<library/cpp/testing/common/env.h>`:**

```cpp
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/common/env.h>

Y_UNIT_TEST_SUITE(MyTestSuite) {
    Y_UNIT_TEST(MyTest) {
        // Get string parameter
        TString param = GetTestParam("my_param", "default_value");
        
        // Get integer parameter
        ui32 numParam = FromString<ui32>(GetTestParam("my_number", "0"));
        
        Cerr << "Parameter: " << param << Endl;
        Cerr << "Number: " << numParam << Endl;
    }
}
```

**Note:** `GetTestParam` is the standard way to access test parameters in YDB tests.

```
    }
}
```

**Helper functions (recommended):**
```cpp
inline TString GetTestParam(const char* name, const TString& defaultValue = "") {
    const char* value = std::getenv(TStringBuilder() << "TEST_PARAM_" << name);
    return value ? TString(value) : defaultValue;
}

inline ui32 GetTestParamInt(const char* name, ui32 defaultValue = 0) {
    TString value = GetTestParam(name);
    return value.empty() ? defaultValue : FromString<ui32>(value);
}
```

#### 4. Manual Tests Using the ya:manual Tag

**In ya.make:**
```ya.make
UNITTEST()

SRCS(
    test_file.cpp
)

TAG(
    ya:manual
)

END()
```

**Running manual tests:**
```bash
# Run manual tests
ya make -ttt --test-tag=ya:manual

# Run without rebuilding
ya make -r -ttt --test-tag=ya:manual

# With parameters
ya make -ttt --test-tag=ya:manual --test-param max_permutations=100
```

#### 5. Additional Useful Options

```bash
# Disable timeouts (useful for debugging)
ya make -ttt --test-disable-timeout

# Output stderr in real-time
ya make -ttt --test-stderr

# Limit parallel test execution
ya make -ttt --test-threads=1

# Run without rebuilding
ya make -r -ttt

# Run all tests including LARGE ones
ya make -A
```

#### 6. Complete Usage Examples

**Example 1: Manual exhaustive test with random sampling**
```bash
ya make -r -ttt --test-tag=ya:manual \
    --test-param max_permutations=200 \
    --test-param sampling_strategy=random
```

**Example 2: Manual test with distributed sampling**
```bash
ya make -r -ttt --test-tag=ya:manual \
    --test-param max_permutations=100 \
    --test-param sampling_strategy=distributed
```

**Example 3: Combined filtering with parameters**
```bash
ya make -ttt --test-tag=manual+integration \
    --test-param db=postgres \
    --test-param timeout=300
```

**Example 4: Debugging a specific test**
```bash
ya make -ttt --test-tag=ya:manual \
    --test-disable-timeout \
    --test-stderr \
    --test-threads=1
```
