#pragma once

#include <util/generic/vector.h>
#include <util/string/builder.h>

#include <algorithm>
#include <functional>
#include <numeric>

namespace NSchemeShardUT_Private {

/**
 * Iterator over all permutations of indices [0, N).
 *
 * Usage:
 *   TPartsPermutationIterator iter(3);  // 3 parts: 0, 1, 2
 *   while (iter.Next()) {
 *       auto order = iter.Current();  // e.g., {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, ...
 *       // ... test with this order ...
 *   }
 */
class TPartsPermutationIterator {
public:
    explicit TPartsPermutationIterator(size_t partCount)
        : Current_(partCount)
        , First_(true)
    {
        std::iota(Current_.begin(), Current_.end(), 0u);
    }

    // Reset iterator to beginning
    void Reset() {
        std::iota(Current_.begin(), Current_.end(), 0u);
        First_ = true;
    }

    // Returns true if there's a permutation available
    bool Next() {
        if (First_) {
            First_ = false;
            return !Current_.empty();
        }
        return std::next_permutation(Current_.begin(), Current_.end());
    }

    // Get current permutation
    const TVector<ui32>& Current() const {
        return Current_;
    }

    // Get total number of permutations (N!)
    static size_t TotalPermutations(size_t n) {
        size_t result = 1;
        for (size_t i = 2; i <= n; ++i) {
            result *= i;
        }
        return result;
    }

    // Get permutation at specific index (0-based)
    static TVector<ui32> GetPermutation(size_t partCount, size_t permIndex) {
        TVector<ui32> result(partCount);
        std::iota(result.begin(), result.end(), 0u);

        for (size_t i = 0; i < permIndex; ++i) {
            if (!std::next_permutation(result.begin(), result.end())) {
                break;  // Wrapped around
            }
        }
        return result;
    }

    // Format permutation for logging
    static TString FormatPermutation(const TVector<ui32>& perm) {
        TStringBuilder sb;
        sb << "[";
        for (size_t i = 0; i < perm.size(); ++i) {
            if (i > 0) sb << ", ";
            sb << perm[i];
        }
        sb << "]";
        return sb;
    }

private:
    TVector<ui32> Current_;
    bool First_;
};


/**
 * Configuration for permutation testing.
 */
struct TPartsPermutationConfig {
    // Maximum number of permutations to test (0 = unlimited)
    size_t MaxPermutations = 0;

    // Specific permutation index to test (-1 = all)
    ssize_t SpecificPermutation = -1;

    // Skip first N permutations (for distributed testing / CI sharding)
    size_t SkipPermutations = 0;

    // Only test every Nth permutation (for sampling)
    size_t SampleRate = 1;

    // Filter for which TxIds to control (empty = all)
    std::function<bool(ui64 txId)> TxFilter;
};


/**
 * Helper class for iterating over permutations with configuration.
 *
 * Supports:
 * - Maximum permutation limit
 * - Skipping initial permutations (for CI sharding)
 * - Sampling (test every Nth permutation)
 * - Specific permutation selection
 *
 * Usage:
 *   TConfiguredPermutationIterator iter(4, config);
 *   while (iter.Next()) {
 *       auto perm = iter.Current();
 *       size_t idx = iter.CurrentIndex();
 *       // ... run test with perm ...
 *   }
 */
class TConfiguredPermutationIterator {
public:
    TConfiguredPermutationIterator(size_t partCount, const TPartsPermutationConfig& config)
        : Config_(config)
        , TotalPermutations_(TPartsPermutationIterator::TotalPermutations(partCount))
        , CurrentIndex_(0)
        , TestedCount_(0)
        , Iter_(partCount)
        , Started_(false)
    {
        // Handle specific permutation mode
        if (Config_.SpecificPermutation >= 0) {
            Current_ = TPartsPermutationIterator::GetPermutation(
                partCount, static_cast<size_t>(Config_.SpecificPermutation));
        }
    }

    // Returns true if there's a permutation available
    bool Next() {
        // Specific permutation mode - only one iteration
        if (Config_.SpecificPermutation >= 0) {
            if (!Started_) {
                Started_ = true;
                CurrentIndex_ = static_cast<size_t>(Config_.SpecificPermutation);
                return true;
            }
            return false;
        }

        // Check max permutations limit
        if (Config_.MaxPermutations > 0 && TestedCount_ >= Config_.MaxPermutations) {
            return false;
        }

        // Iterate through permutations
        while (true) {
            if (!Started_) {
                Started_ = true;
                if (!Iter_.Next()) {
                    return false;
                }
            } else {
                if (!Iter_.Next()) {
                    return false;
                }
                ++CurrentIndex_;
            }

            // Skip initial permutations
            if (CurrentIndex_ < Config_.SkipPermutations) {
                continue;
            }

            // Apply sample rate
            if (Config_.SampleRate > 1) {
                size_t adjustedIdx = CurrentIndex_ - Config_.SkipPermutations;
                if (adjustedIdx % Config_.SampleRate != 0) {
                    continue;
                }
            }

            // Found a valid permutation
            Current_ = Iter_.Current();
            ++TestedCount_;
            return true;
        }
    }

    // Get current permutation
    const TVector<ui32>& Current() const {
        return Current_;
    }

    // Get current permutation index
    size_t CurrentIndex() const {
        return CurrentIndex_;
    }

    // Get number of permutations tested so far
    size_t TestedCount() const {
        return TestedCount_;
    }

    // Get total number of possible permutations
    size_t TotalPermutations() const {
        return TotalPermutations_;
    }

    // Get estimated number of permutations to test
    size_t EstimatedTestCount() const {
        size_t available = TotalPermutations_;

        // Apply skip
        if (Config_.SkipPermutations < available) {
            available -= Config_.SkipPermutations;
        } else {
            return 0;
        }

        // Apply sample rate
        if (Config_.SampleRate > 1) {
            available = (available + Config_.SampleRate - 1) / Config_.SampleRate;
        }

        // Apply max limit
        if (Config_.MaxPermutations > 0 && available > Config_.MaxPermutations) {
            available = Config_.MaxPermutations;
        }

        // Specific permutation mode
        if (Config_.SpecificPermutation >= 0) {
            return 1;
        }

        return available;
    }

private:
    TPartsPermutationConfig Config_;
    size_t TotalPermutations_;
    size_t CurrentIndex_;
    size_t TestedCount_;
    TPartsPermutationIterator Iter_;
    TVector<ui32> Current_;
    bool Started_;
};

} // namespace NSchemeShardUT_Private
