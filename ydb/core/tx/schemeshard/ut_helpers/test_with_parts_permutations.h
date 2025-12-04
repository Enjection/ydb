#pragma once

#include "test_env.h"
#include "operation_parts_blocker.h"
#include "parts_permutation.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NSchemeShardUT_Private {

/**
 * Test framework that runs schema operations with all possible part orderings.
 *
 * Similar to TTestWithReboots, but instead of injecting reboots at different
 * points, it controls the order in which operation parts are executed.
 *
 * This helps catch flaky tests caused by non-deterministic execution order
 * of operation parts (sub-operations) in SchemeShard.
 *
 * Usage:
 *
 *   Y_UNIT_TEST(MyTest) {
 *       TTestWithPartsPermutations t;
 *       t.Run([&](TTestActorRuntime& runtime, TTestEnv& env,
 *                 TOperationPartsBlocker& blocker, const TVector<ui32>& permutation) {
 *           // Setup: create tables, etc.
 *           auto txId = TestCreateTable(...);
 *
 *           // Wait for operation parts to be captured
 *           blocker.WaitForParts(txId, permutation.size());
 *
 *           // Release parts in the order specified by permutation
 *           blocker.ReleaseInOrder(txId, permutation);
 *
 *           env.TestWaitNotification(runtime, txId);
 *
 *           // Verify results
 *           TestDescribeResult(...);
 *       });
 *   }
 */
class TTestWithPartsPermutations {
public:
    struct TConfig {
        // Expected number of parts (required for permutation testing)
        size_t ExpectedPartCount = 0;

        // Maximum permutations to test (0 = all)
        size_t MaxPermutations = 120;  // 5! = 120

        // Stop after first failure
        bool StopOnFirstFailure = true;

        // Specific permutation to test (-1 = all)
        ssize_t SpecificPermutation = -1;

        // Skip initial permutations (for CI sharding)
        size_t SkipPermutations = 0;

        // Sample rate (test every Nth permutation)
        size_t SampleRate = 1;

        // Filter which TxIds to control
        std::function<bool(ui64 txId)> TxFilter;

        // Test environment options
        TTestEnvOptions EnvOptions;
    };

    TConfig Config;
    THolder<TTestActorRuntime> Runtime;
    THolder<TTestEnv> TestEnv;
    ui64 TxId = 1000;

    // Statistics
    size_t PermutationsTested = 0;
    size_t PermutationsFailed = 0;
    TVector<TVector<ui32>> FailedPermutations;

public:
    TTestWithPartsPermutations()
        : Config()
    {}

    explicit TTestWithPartsPermutations(TConfig config)
        : Config(std::move(config))
    {}

    /**
     * Run test with all permutations of part execution order.
     *
     * @param testScenario Function that receives runtime, env, blocker, and current permutation.
     *                     The test should:
     *                     1. Trigger the schema operation
     *                     2. Wait for parts: blocker.WaitForParts(txId, permutation.size())
     *                     3. Release in order: blocker.ReleaseInOrder(txId, permutation)
     *                     4. Wait for completion and verify results
     */
    void Run(std::function<void(TTestActorRuntime& runtime,
                                TTestEnv& env,
                                TOperationPartsBlocker& blocker,
                                const TVector<ui32>& permutation)> testScenario) {
        Y_ABORT_UNLESS(Config.ExpectedPartCount > 0,
                       "ExpectedPartCount must be set for permutation testing");

        // Build permutation config
        TPartsPermutationConfig permConfig;
        permConfig.MaxPermutations = Config.MaxPermutations;
        permConfig.SpecificPermutation = Config.SpecificPermutation;
        permConfig.SkipPermutations = Config.SkipPermutations;
        permConfig.SampleRate = Config.SampleRate;
        permConfig.TxFilter = Config.TxFilter;

        TConfiguredPermutationIterator iter(Config.ExpectedPartCount, permConfig);

        size_t totalToTest = iter.EstimatedTestCount();
        Cerr << "==== Testing up to " << totalToTest << " permutations for "
             << Config.ExpectedPartCount << " parts ====" << Endl;

        while (iter.Next()) {
            auto permutation = iter.Current();
            size_t permIndex = iter.CurrentIndex();

            Cerr << "==== Permutation " << (iter.TestedCount()) << "/" << totalToTest
                 << " (index " << permIndex << "): "
                 << TPartsPermutationIterator::FormatPermutation(permutation)
                 << " ====" << Endl;

            try {
                RunWithPermutation(testScenario, permutation);
                ++PermutationsTested;
            } catch (const yexception& ex) {
                ++PermutationsTested;
                ++PermutationsFailed;
                FailedPermutations.push_back(permutation);

                Cerr << "FAILED with permutation "
                     << TPartsPermutationIterator::FormatPermutation(permutation)
                     << ": " << ex.what() << Endl;

                if (Config.StopOnFirstFailure) {
                    throw;
                }
            }
        }

        Cerr << "==== Completed: " << PermutationsTested << " permutations tested, "
             << PermutationsFailed << " failed ====" << Endl;

        if (PermutationsFailed > 0 && !Config.StopOnFirstFailure) {
            TStringBuilder msg;
            msg << PermutationsFailed << " permutations failed: ";
            for (const auto& perm : FailedPermutations) {
                msg << TPartsPermutationIterator::FormatPermutation(perm) << " ";
            }
            UNIT_FAIL(msg);
        }
    }

    /**
     * Run test with a specific permutation order.
     * Useful for reproducing specific failures.
     */
    void RunWithPermutation(
            std::function<void(TTestActorRuntime& runtime,
                              TTestEnv& env,
                              TOperationPartsBlocker& blocker,
                              const TVector<ui32>& permutation)> testScenario,
            const TVector<ui32>& permutation) {

        PrepareRuntime();

        // Create blocker that will capture parts
        TOperationPartsBlocker blocker(*Runtime, Config.TxFilter);

        testScenario(*Runtime, *TestEnv, blocker, permutation);

        // Release any remaining parts
        blocker.ReleaseAllOperations();
    }

    /**
     * Run with specific permutation index.
     * Useful for CI sharding.
     */
    void RunPermutationIndex(
            std::function<void(TTestActorRuntime& runtime,
                              TTestEnv& env,
                              TOperationPartsBlocker& blocker,
                              const TVector<ui32>& permutation)> testScenario,
            size_t permIndex) {

        auto permutation = TPartsPermutationIterator::GetPermutation(
            Config.ExpectedPartCount, permIndex);

        Cerr << "==== Running permutation index " << permIndex
             << ": " << TPartsPermutationIterator::FormatPermutation(permutation)
             << " ====" << Endl;

        RunWithPermutation(testScenario, permutation);
    }

    /**
     * Discover the number of parts for an operation.
     * Runs the test once without releasing parts to count them.
     *
     * @param setupAndTrigger Function that sets up the test and triggers the operation.
     *                        Should NOT wait for completion.
     * @param txIdOut Output parameter for the captured txId
     * @return Number of parts discovered
     */
    size_t DiscoverPartCount(
            std::function<void(TTestActorRuntime& runtime, TTestEnv& env)> setupAndTrigger,
            ui64& txIdOut) {

        PrepareRuntime();

        TOperationPartsBlocker blocker(*Runtime, Config.TxFilter);

        setupAndTrigger(*Runtime, *TestEnv);

        // Wait a bit for parts to be captured
        TDispatchOptions opts;
        opts.CustomFinalCondition = [&]() {
            return !blocker.GetCapturedTxIds().empty();
        };
        Runtime->DispatchEvents(opts, TDuration::Seconds(5));

        auto txIds = blocker.GetCapturedTxIds();
        if (txIds.empty()) {
            return 0;
        }

        // Find operation with most parts
        size_t maxParts = 0;
        for (ui64 txId : txIds) {
            size_t count = blocker.GetPartCount(txId);
            if (count > maxParts) {
                maxParts = count;
                txIdOut = txId;
            }
        }

        Cerr << "Discovered " << maxParts << " parts for txId " << txIdOut << Endl;
        return maxParts;
    }

private:
    void PrepareRuntime() {
        Runtime = MakeHolder<TTestBasicRuntime>();
        TestEnv = MakeHolder<TTestEnv>(*Runtime, Config.EnvOptions);
        TxId = 1000;
    }
};


/**
 * Simpler interface for manual permutation control in tests.
 *
 * Usage:
 *   Y_UNIT_TEST(MyTest) {
 *       ForEachPartsPermutation(3, [](const TVector<ui32>& perm) {
 *           // Run test with this permutation
 *           TTestBasicRuntime runtime;
 *           TTestEnv env(runtime);
 *           TOperationPartsBlocker blocker(runtime);
 *
 *           // ... trigger operation ...
 *           blocker.WaitForParts(txId, 3);
 *           blocker.ReleaseInOrder(txId, perm);
 *           // ... verify ...
 *       });
 *   }
 */
inline void ForEachPartsPermutation(
        size_t partCount,
        std::function<void(const TVector<ui32>& permutation)> testFunc,
        size_t maxPermutations = 0) {

    TPartsPermutationIterator iter(partCount);
    size_t count = 0;
    size_t total = TPartsPermutationIterator::TotalPermutations(partCount);

    Cerr << "==== Testing " << (maxPermutations > 0 ? Min(maxPermutations, total) : total)
         << " permutations for " << partCount << " parts ====" << Endl;

    while (iter.Next()) {
        if (maxPermutations > 0 && count >= maxPermutations) {
            break;
        }

        auto perm = iter.Current();
        TString permStr = TPartsPermutationIterator::FormatPermutation(perm);
        Cerr << "==== Permutation " << (count + 1) << "/"
             << (maxPermutations > 0 ? Min(maxPermutations, total) : total)
             << ": " << permStr
             << " ====" << Endl;

        try {
            testFunc(perm);
        } catch (const yexception& ex) {
            Cerr << "==== FAILED at permutation " << (count + 1)
                 << ": " << permStr << " ====" << Endl;
            Cerr << "Error: " << ex.what() << Endl;
            ythrow yexception() << "Test failed with permutation " << permStr
                                << " (index " << count << "): " << ex.what();
        } catch (...) {
            Cerr << "==== FAILED at permutation " << (count + 1)
                 << ": " << permStr << " ====" << Endl;
            throw;
        }
        ++count;
    }

    Cerr << "==== All " << count << " permutations passed ====" << Endl;
}


/**
 * Macro for creating tests that run with all permutations.
 * Generates multiple test cases, one per permutation.
 */
#define Y_UNIT_TEST_WITH_PARTS_PERMUTATIONS(N, PART_COUNT)                    \
    void N##_Impl(TTestActorRuntime& runtime, TTestEnv& env,                  \
                  TOperationPartsBlocker& blocker, const TVector<ui32>& perm);\
    Y_UNIT_TEST(N) {                                                          \
        TTestWithPartsPermutations t;                                         \
        t.Config.ExpectedPartCount = PART_COUNT;                              \
        t.Run([](TTestActorRuntime& runtime, TTestEnv& env,                   \
                 TOperationPartsBlocker& blocker, const TVector<ui32>& perm) {\
            N##_Impl(runtime, env, blocker, perm);                            \
        });                                                                   \
    }                                                                         \
    void N##_Impl(TTestActorRuntime& runtime, TTestEnv& env,                  \
                  TOperationPartsBlocker& blocker, const TVector<ui32>& perm)

} // namespace NSchemeShardUT_Private
