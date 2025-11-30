#pragma once

#include <util/generic/vector.h>
#include <util/generic/hash_set.h>
#include <util/generic/string.h>
#include <util/string/builder.h>
#include <util/random/shuffle.h>
#include <util/stream/output.h>

#include <random>
#include <algorithm>

namespace NSchemeShardUT_Private {

/**
 * TOperationOrderController - Controls the execution order of parallel operations in tests
 *
 * This class provides mechanisms to test different operation orderings in SchemeShard tests
 * to catch order-dependent bugs and race conditions. While SchemeShard is single-threaded,
 * operations can be scheduled in arbitrary order, leading to potential bugs.
 *
 * Usage:
 *   TOperationOrderController controller;
 *   controller.SetMode(TOperationOrderController::Random, 42); // seed for reproducibility
 *   auto orderedOps = controller.GetNextOrder(operations);
 */
class TOperationOrderController {
public:
    enum EOrderMode {
        Default,      // Natural order - operations execute as they arrive
        Random,       // Random shuffle with seed for reproducibility
        Exhaustive,   // All permutations (for small operation sets)
        Deterministic // Specific pre-defined order
    };

private:
    EOrderMode Mode = Default;
    TVector<ui64> PredefinedOrder;
    std::mt19937 RandomGen;
    ui32 CurrentPermutation = 0;
    ui32 Seed = 0;
    bool HasMorePermutationsValue = false;

public:
    TOperationOrderController()
        : RandomGen(0)
    {}

    /**
     * Set the ordering mode
     * @param mode - The ordering strategy to use
     * @param seed - Random seed for reproducibility (used in Random mode)
     */
    void SetMode(EOrderMode mode, ui32 seed = 0) {
        Mode = mode;
        Seed = seed;
        RandomGen.seed(seed);
        CurrentPermutation = 0;
        HasMorePermutationsValue = (mode == Exhaustive);
    }

    /**
     * Set a specific order for Deterministic mode
     * @param order - Vector of indices specifying the order
     */
    void SetPredefinedOrder(const TVector<ui64>& order) {
        PredefinedOrder = order;
        Mode = Deterministic;
    }

    /**
     * Get the next ordering of operations
     * @param operations - Vector of operation identifiers
     * @return Vector of operation identifiers in the desired order
     */
    template<typename T>
    TVector<T> GetNextOrder(const TVector<T>& operations) {
        switch (Mode) {
            case Default:
                return operations;

            case Random: {
                TVector<T> result = operations;
                Shuffle(result.begin(), result.end(), RandomGen);
                return result;
            }

            case Exhaustive: {
                TVector<T> result = operations;
                if (CurrentPermutation > 0) {
                    // Generate next permutation
                    HasMorePermutationsValue = std::next_permutation(result.begin(), result.end());
                } else {
                    // First permutation - ensure sorted order
                    std::sort(result.begin(), result.end());
                    HasMorePermutationsValue = true;
                }
                CurrentPermutation++;
                return result;
            }

            case Deterministic: {
                Y_ENSURE(PredefinedOrder.size() == operations.size(),
                         "Predefined order size doesn't match operations size");
                TVector<T> result;
                result.reserve(operations.size());
                for (ui64 idx : PredefinedOrder) {
                    Y_ENSURE(idx < operations.size(), "Invalid index in predefined order");
                    result.push_back(operations[idx]);
                }
                return result;
            }
        }

        return operations;
    }

    /**
     * Check if there are more permutations to test (for Exhaustive mode)
     */
    bool HasMorePermutations() const {
        return HasMorePermutationsValue;
    }

    /**
     * Reset the controller to initial state
     */
    void Reset() {
        CurrentPermutation = 0;
        RandomGen.seed(Seed);
        HasMorePermutationsValue = (Mode == Exhaustive);
    }

    /**
     * Get current mode
     */
    EOrderMode GetMode() const {
        return Mode;
    }

    /**
     * Get current seed (for Random mode)
     */
    ui32 GetSeed() const {
        return Seed;
    }

    /**
     * Get current permutation number
     */
    ui32 GetCurrentPermutation() const {
        return CurrentPermutation;
    }
};

/**
 * Helper function to calculate factorial (number of permutations)
 */
inline ui64 Factorial(ui64 n) {
    ui64 result = 1;
    for (ui64 i = 2; i <= n; ++i) {
        result *= i;
    }
    return result;
}

/**
 * Helper to generate all permutations with sampling strategies
 */
template<typename TTestFunc>
void TestAllPermutations(
    ui32 numOperations,
    TTestFunc testFunc,
    ui32 maxPermutations = 0,
    const TString& samplingStrategy = "all")
{
    TVector<ui64> ops;
    for (ui64 i = 0; i < numOperations; ++i) {
        ops.push_back(i);
    }

    ui64 totalPermutations = Factorial(numOperations);
    ui32 testedCount = 0;
    ui32 currentPermutation = 0;

    if (samplingStrategy == "random" && maxPermutations > 0) {
        // Random sampling
        std::mt19937 rng(42); // Fixed seed for reproducibility
        THashSet<TString> tested;

        while (testedCount < maxPermutations && testedCount < totalPermutations) {
            Shuffle(ops.begin(), ops.end(), rng);

            // Convert to string for uniqueness check
            TStringBuilder sb;
            for (auto op : ops) {
                sb << op << ",";
            }

            if (tested.insert(sb).second) {
                testFunc(ops);
                testedCount++;
            }
        }

        Cerr << "Tested " << testedCount << " random permutations out of "
             << totalPermutations << " total" << Endl;

    } else if (samplingStrategy == "distributed" && maxPermutations > 0) {
        // Distributed sampling - test evenly spaced permutations
        ui32 step = Max<ui32>(1, totalPermutations / maxPermutations);

        do {
            if (currentPermutation % step == 0) {
                testFunc(ops);
                testedCount++;
                if (testedCount >= maxPermutations) {
                    break;
                }
            }
            currentPermutation++;
        } while (std::next_permutation(ops.begin(), ops.end()));

        Cerr << "Tested " << testedCount << " distributed permutations out of "
             << totalPermutations << " total" << Endl;

    } else if (samplingStrategy == "first" && maxPermutations > 0) {
        // Test first N permutations
        do {
            testFunc(ops);
            testedCount++;
            if (testedCount >= maxPermutations) {
                break;
            }
        } while (std::next_permutation(ops.begin(), ops.end()));

        Cerr << "Tested first " << testedCount << " permutations out of "
             << totalPermutations << " total" << Endl;

    } else {
        // Test all permutations (default)
        do {
            testFunc(ops);
            testedCount++;
        } while (std::next_permutation(ops.begin(), ops.end()));

        Cerr << "Tested all " << testedCount << " permutations" << Endl;
    }
}

} // namespace NSchemeShardUT_Private
