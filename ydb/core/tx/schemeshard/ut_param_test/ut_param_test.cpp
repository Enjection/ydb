#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/common/env.h>

#include <util/string/cast.h>

using namespace NKikimr;
using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(TParameterizedTestDemo) {

    Y_UNIT_TEST(SimpleParameterReading) {
        // This test demonstrates reading parameters from ya make --test-param
        // Run with: ya make -ttt --test-tag=ya:manual --test-param my_string=hello --test-param my_number=42
        
        TString stringParam = GetTestParam("my_string", "default_value");
        ui32 numberParam = FromString<ui32>(GetTestParam("my_number", "123"));
        
        Cerr << "========================================" << Endl;
        Cerr << "PARAMETER TEST RESULTS:" << Endl;
        Cerr << "  my_string = " << stringParam << Endl;
        Cerr << "  my_number = " << numberParam << Endl;
        Cerr << "========================================" << Endl;
        
        // Test passes with any parameters, just prints them
        UNIT_ASSERT(!stringParam.empty());
        UNIT_ASSERT(numberParam > 0);
    }

    Y_UNIT_TEST(PermutationSamplingDemo) {
        // This test demonstrates the sampling strategies for exhaustive testing
        // Run with:
        // ya make -ttt --test-tag=ya:manual --test-param max_permutations=10 --test-param sampling_strategy=random
        
        ui32 maxPermutations = FromString<ui32>(GetTestParam("max_permutations", "0"));
        TString samplingStrategy = GetTestParam("sampling_strategy", "all");
        
        Cerr << "========================================" << Endl;
        Cerr << "PERMUTATION SAMPLING DEMO:" << Endl;
        Cerr << "  max_permutations = " << maxPermutations << Endl;
        Cerr << "  sampling_strategy = " << samplingStrategy << Endl;
        Cerr << "========================================" << Endl;
        
        // Simulate operations
        TVector<ui64> ops = {0, 1, 2, 3};
        ui64 totalPermutations = 24; // 4!
        
        if (maxPermutations == 0) {
            Cerr << "Testing ALL " << totalPermutations << " permutations (default)" << Endl;
        } else {
            Cerr << "Testing up to " << maxPermutations << " out of " 
                 << totalPermutations << " permutations" << Endl;
            Cerr << "Using sampling strategy: " << samplingStrategy << Endl;
        }
        
        ui32 testedCount = 0;
        
        if (samplingStrategy == "random" && maxPermutations > 0) {
            // Demonstrate random sampling
            Cerr << "Simulating random sampling..." << Endl;
            std::mt19937 rng(42);
            THashSet<TString> tested; // Use string representation to avoid hash issues
            
            while (testedCount < maxPermutations && testedCount < totalPermutations) {
                std::shuffle(ops.begin(), ops.end(), rng);
                
                // Convert to string for uniqueness check
                TStringBuilder sb;
                for (auto op : ops) {
                    sb << op << ",";
                }
                
                if (tested.insert(sb).second) {
                    Cerr << "  Permutation " << (testedCount + 1) << ": ";
                    for (auto op : ops) {
                        Cerr << op << " ";
                    }
                    Cerr << Endl;
                    testedCount++;
                }
            }
        } else if (samplingStrategy == "distributed" && maxPermutations > 0) {
            // Demonstrate distributed sampling
            Cerr << "Simulating distributed sampling..." << Endl;
            ui32 step = Max<ui32>(1, totalPermutations / maxPermutations);
            ui32 currentPermutation = 0;
            
            std::sort(ops.begin(), ops.end());
            do {
                if (currentPermutation % step == 0) {
                    Cerr << "  Permutation " << (testedCount + 1) << " (index " << currentPermutation << "): ";
                    for (auto op : ops) {
                        Cerr << op << " ";
                    }
                    Cerr << Endl;
                    testedCount++;
                    if (testedCount >= maxPermutations) break;
                }
                currentPermutation++;
            } while (std::next_permutation(ops.begin(), ops.end()));
        } else if (samplingStrategy == "first" && maxPermutations > 0) {
            // Demonstrate first N sampling
            Cerr << "Simulating first N permutations sampling..." << Endl;
            std::sort(ops.begin(), ops.end());
            
            do {
                Cerr << "  Permutation " << (testedCount + 1) << ": ";
                for (auto op : ops) {
                    Cerr << op << " ";
                }
                Cerr << Endl;
                testedCount++;
                if (testedCount >= maxPermutations) break;
            } while (std::next_permutation(ops.begin(), ops.end()));
        } else {
            // Test all permutations
            Cerr << "Testing all permutations (no sampling)..." << Endl;
            std::sort(ops.begin(), ops.end());
            
            do {
                if (testedCount < 5 || testedCount >= totalPermutations - 2) {
                    // Only print first 5 and last 2 to avoid spam
                    Cerr << "  Permutation " << (testedCount + 1) << ": ";
                    for (auto op : ops) {
                        Cerr << op << " ";
                    }
                    Cerr << Endl;
                } else if (testedCount == 5) {
                    Cerr << "  ... (showing first 5 and last 2 only)" << Endl;
                }
                testedCount++;
            } while (std::next_permutation(ops.begin(), ops.end()));
        }
        
        Cerr << "========================================" << Endl;
        Cerr << "TOTAL TESTED: " << testedCount << " permutations" << Endl;
        Cerr << "========================================" << Endl;
        
        UNIT_ASSERT(testedCount > 0);
    }

    Y_UNIT_TEST(ActualSchemeShardTest) {
        // This test creates sequences in different orders based on parameters
        // Run with: ya make -ttt --test-tag=ya:manual --test-param operation_order=3,1,0,2
        
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::SEQUENCESHARD, NActors::NLog::PRI_TRACE);

        TString orderStr = GetTestParam("operation_order", "0,1,2,3");
        Cerr << "========================================" << Endl;
        Cerr << "Creating sequences in order: " << orderStr << Endl;
        Cerr << "========================================" << Endl;

        // Parse operation order
        TVector<ui32> order;
        TVector<TString> parts = StringSplitter(orderStr).Split(',').ToList<TString>();
        for (const auto& part : parts) {
            order.push_back(FromString<ui32>(part));
        }

        // Create sequences in the specified order
        TVector<ui64> txIds;
        for (ui32 idx : order) {
            TestCreateSequence(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "seq%d"
            )", idx));
            txIds.push_back(txId);
            Cerr << "Created sequence seq" << idx << " with txId " << txId << Endl;
        }

        // Wait for all operations
        env.TestWaitNotification(runtime, txIds);

        // Verify all sequences exist
        for (ui32 idx : order) {
            TestLs(runtime, Sprintf("/MyRoot/seq%d", idx), false, NLs::PathExist);
            Cerr << "Verified sequence seq" << idx << " exists" << Endl;
        }

        Cerr << "========================================" << Endl;
        Cerr << "All sequences created successfully!" << Endl;
        Cerr << "========================================" << Endl;
    }

} // Y_UNIT_TEST_SUITE(TParameterizedTestDemo)
