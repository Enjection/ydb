#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/test_with_parts_permutations.h>
#include <ydb/core/tx/datashard/datashard_ut_common_kqp.h>
#include <ydb/core/tx/datashard/ut_common/datashard_ut_common.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NSchemeShardUT_Private;
using namespace NKikimr;
using namespace NKikimr::NDataShard::NKqpHelpers;

Y_UNIT_TEST_SUITE(TPartsPermutationTests) {

    void SetupLogging(TTestActorRuntimeBase& runtime) {
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
    }

    void PrepareDirs(TTestBasicRuntime& runtime, TTestEnv& env, ui64& txId) {
        TestMkDir(runtime, ++txId, "/MyRoot", ".backups");
        env.TestWaitNotification(runtime, txId);
        TestMkDir(runtime, ++txId, "/MyRoot/.backups", "collections");
        env.TestWaitNotification(runtime, txId);
    }

    /**
     * Test that demonstrates how TOperationPartsBlocker works.
     * Creates an indexed table and verifies we can capture and control
     * the order of operation parts.
     */
    Y_UNIT_TEST(BasicPartsBlockerUsage) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        // Create blocker to capture TEvProgressOperation events
        TOperationPartsBlocker blocker(runtime);

        // Create indexed table - this will have multiple parts
        // Part 0: Create table
        // Part 1: Create index
        AsyncCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Table1"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            }
            IndexDescription {
                Name: "idx1"
                KeyColumnNames: ["value"]
                Type: EIndexTypeGlobal
            }
        )");

        // Wait for parts to be captured
        // Note: indexed table creation typically has 2 parts (table + index)
        blocker.WaitForParts(txId, 2);

        Cerr << "Captured " << blocker.GetPartCount(txId) << " parts for txId " << txId << Endl;
        auto partIds = blocker.GetPartIds(txId);
        for (auto partId : partIds) {
            Cerr << "  Part " << partId << Endl;
        }

        // Release in reverse order to test ordering control
        // Use ReleaseByPermutationIndices since we're permuting by capture order, not part IDs
        blocker.ReleaseByPermutationIndices(txId, {1, 0});
        // Release any remaining parts that weren't in the permutation
        blocker.ReleaseAll(txId);
        // Stop blocking to allow subsequent TEvProgressOperation events through
        blocker.Stop();

        // Wait for completion
        env.TestWaitNotification(runtime, txId);

        // Verify table was created
        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table1"), {
            NLs::PathExist,
            NLs::IsTable
        });

        // Verify index was created
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table1/idx1"), {
            NLs::PathExist
        });
    }

    /**
     * Test that runs the same operation with all possible permutations
     * of part execution order using ForEachPartsPermutation.
     */
    Y_UNIT_TEST(IndexedTableAllPermutations) {
        ForEachPartsPermutation(2, [](const TVector<ui32>& permutation) {
            TTestBasicRuntime runtime;
            TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
            ui64 txId = 100;

            runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_WARN);

            TOperationPartsBlocker blocker(runtime);

            AsyncCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
                TableDescription {
                    Name: "TestTable"
                    Columns { Name: "key" Type: "Uint32" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                }
                IndexDescription {
                    Name: "ValueIndex"
                    KeyColumnNames: ["value"]
                    Type: EIndexTypeGlobal
                }
            )");

            blocker.WaitForParts(txId, 2);
            // Use permutation as indices into captured parts, not as part IDs
            blocker.ReleaseByPermutationIndices(txId, permutation);
            // Release any remaining parts that weren't in the permutation
            blocker.ReleaseAll(txId);
            // Stop blocking to allow subsequent TEvProgressOperation events through
            blocker.Stop();
            env.TestWaitNotification(runtime, txId);

            // Verify
            TestDescribeResult(DescribePath(runtime, "/MyRoot/TestTable"), {
                NLs::PathExist,
                NLs::IsTable
            });
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/TestTable/ValueIndex"), {
                NLs::PathExist
            });
        });
    }

    /**
     * Test using TTestWithPartsPermutations framework.
     * This is a template for the actual failing test scenario.
     */
    Y_UNIT_TEST(IndexedTableWithFramework) {
        TTestWithPartsPermutations t;
        t.Config.ExpectedPartCount = 2;
        t.Config.MaxPermutations = 2;  // 2! = 2 permutations
        t.Config.EnvOptions = TTestEnvOptions().EnableBackupService(true);

        t.Run([](TTestActorRuntime& runtime, TTestEnv& env,
                 TOperationPartsBlocker& blocker, const TVector<ui32>& permutation) {
            ui64 txId = 100;

            runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_WARN);

            AsyncCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
                TableDescription {
                    Name: "TestTable"
                    Columns { Name: "key" Type: "Uint32" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                }
                IndexDescription {
                    Name: "ValueIndex"
                    KeyColumnNames: ["value"]
                    Type: EIndexTypeGlobal
                }
            )");

            blocker.WaitForParts(txId, permutation.size());
            // Use permutation as indices into captured parts, not as part IDs
            blocker.ReleaseByPermutationIndices(txId, permutation);
            // Release any remaining parts that weren't in the permutation
            blocker.ReleaseAll(txId);
            // Stop blocking to allow subsequent TEvProgressOperation events through
            blocker.Stop();
            env.TestWaitNotification(runtime, txId);

            TestDescribeResult(DescribePath(runtime, "/MyRoot/TestTable"), {
                NLs::PathExist,
                NLs::IsTable
            });
        });
    }

    /**
     * This test mimics the failing scenario:
     * - Multiple tables with indexes
     * - Backup collection with incremental backup
     * - Incremental restore with multiple tables and indexes
     *
     * The schema version mismatch error occurs because operation parts
     * for different tables/indexes can execute in different orders,
     * leading to inconsistent schema version increments.
     *
     * Error: "schema version mismatch during metadata loading for:
     *         /Root/Table2/idx2 expected 3 got 4"
     */
    Y_UNIT_TEST(MultipleTablesWithIndexesIncrementalRestore_PartsPermutation) {
        // This test will be filled with the actual scenario once we understand
        // how many parts the incremental restore operation has.
        //
        // For now, we test a simpler scenario: creating two tables with indexes
        // sequentially, which can expose ordering issues in schema version handling.

        TTestWithPartsPermutations t;
        t.Config.ExpectedPartCount = 2;  // Will be adjusted based on actual operation
        t.Config.MaxPermutations = 24;   // Test up to 4! permutations
        t.Config.EnvOptions = TTestEnvOptions().EnableBackupService(true);

        t.Run([](TTestActorRuntime& runtime, TTestEnv& env,
                 TOperationPartsBlocker& blocker, const TVector<ui32>& permutation) {
            ui64 txId = 100;

            runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_WARN);

            // Create first table with index
            TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
                TableDescription {
                    Name: "Table1"
                    Columns { Name: "key" Type: "Uint32" }
                    Columns { Name: "val1" Type: "Uint32" }
                    KeyColumnNames: ["key"]
                }
                IndexDescription {
                    Name: "idx1"
                    KeyColumnNames: ["val1"]
                    Type: EIndexTypeGlobal
                }
            )");
            env.TestWaitNotification(runtime, txId);

            // Now create second table with index - this is where we control parts order
            AsyncCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
                TableDescription {
                    Name: "Table2"
                    Columns { Name: "key" Type: "Uint32" }
                    Columns { Name: "val2" Type: "Uint32" }
                    KeyColumnNames: ["key"]
                }
                IndexDescription {
                    Name: "idx2"
                    KeyColumnNames: ["val2"]
                    Type: EIndexTypeGlobal
                }
            )");

            blocker.WaitForParts(txId, permutation.size());
            // Use permutation as indices into captured parts, not as part IDs
            blocker.ReleaseByPermutationIndices(txId, permutation);
            // Release any remaining parts that weren't in the permutation
            blocker.ReleaseAll(txId);
            // Stop blocking to allow subsequent TEvProgressOperation events through
            blocker.Stop();
            env.TestWaitNotification(runtime, txId);

            // Verify both tables exist with correct indexes
            TestDescribeResult(DescribePath(runtime, "/MyRoot/Table1"), {
                NLs::PathExist,
                NLs::IsTable
            });
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table1/idx1"), {
                NLs::PathExist
            });

            TestDescribeResult(DescribePath(runtime, "/MyRoot/Table2"), {
                NLs::PathExist,
                NLs::IsTable
            });
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table2/idx2"), {
                NLs::PathExist
            });

            // Query both tables to trigger schema version check
            // This is where the flaky test fails with schema version mismatch
            auto table1Desc = DescribePath(runtime, "/MyRoot/Table1", true, true);
            auto table2Desc = DescribePath(runtime, "/MyRoot/Table2", true, true);

            Cerr << "Table1 version: " << table1Desc.GetPathDescription().GetSelf().GetPathVersion() << Endl;
            Cerr << "Table2 version: " << table2Desc.GetPathDescription().GetSelf().GetPathVersion() << Endl;
        });
    }

    /**
     * Manual test for specific permutation reproduction.
     * Use this to reproduce a specific failure.
     */
    Y_UNIT_TEST(SpecificPermutationReproduction) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        TOperationPartsBlocker blocker(runtime);

        // Specify the exact permutation that caused the failure
        TVector<ui32> failingPermutation = {1, 0};  // Adjust based on failure

        AsyncCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "TestTable"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            }
            IndexDescription {
                Name: "ValueIndex"
                KeyColumnNames: ["value"]
                Type: EIndexTypeGlobal
            }
        )");

        blocker.WaitForParts(txId, failingPermutation.size());
        // Use permutation as indices into captured parts, not as part IDs
        blocker.ReleaseByPermutationIndices(txId, failingPermutation);
        // Release any remaining parts that weren't in the permutation
        blocker.ReleaseAll(txId);
        // Stop blocking to allow subsequent TEvProgressOperation events through
        blocker.Stop();
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/TestTable"), {
            NLs::PathExist,
            NLs::IsTable
        });
    }

    /**
     * Test to discover how many parts an incremental backup restore operation has.
     * Run this first to determine the correct ExpectedPartCount.
     */
    Y_UNIT_TEST(DiscoverIncrementalRestorePartCount) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);
        PrepareDirs(runtime, env, txId);

        // Create backup collection
        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", R"(
            Name: "TestCollection"
            ExplicitEntryList {
                Entries {
                    Type: ETypeTable
                    Path: "/MyRoot/Table1"
                }
                Entries {
                    Type: ETypeTable
                    Path: "/MyRoot/Table2"
                }
            }
            Cluster: {}
            IncrementalBackupConfig: {}
        )");
        env.TestWaitNotification(runtime, txId);

        // Create tables with indexes
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Table1"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "val1" Type: "Uint32" }
                KeyColumnNames: ["key"]
            }
            IndexDescription {
                Name: "idx1"
                KeyColumnNames: ["val1"]
                Type: EIndexTypeGlobal
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Table2"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "val2" Type: "Uint32" }
                KeyColumnNames: ["key"]
            }
            IndexDescription {
                Name: "idx2"
                KeyColumnNames: ["val2"]
                Type: EIndexTypeGlobal
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // Create full backup
        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/TestCollection")");
        env.TestWaitNotification(runtime, txId);

        Cerr << "Full backup created" << Endl;

        // Drop tables
        TestDropTable(runtime, ++txId, "/MyRoot", "Table1");
        env.TestWaitNotification(runtime, txId);
        TestDropTable(runtime, ++txId, "/MyRoot", "Table2");
        env.TestWaitNotification(runtime, txId);

        Cerr << "Tables dropped" << Endl;

        // Now start blocking to discover restore operation parts
        TOperationPartsBlocker blocker(runtime);

        // Trigger restore
        AsyncRestoreBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/TestCollection")");

        // Wait for parts to be captured (give it some time)
        TDispatchOptions opts;
        opts.CustomFinalCondition = [&]() {
            return blocker.GetPartCount(txId) > 0;
        };
        runtime.DispatchEvents(opts, TDuration::Seconds(10));

        Cerr << "Discovered " << blocker.GetPartCount(txId) << " parts for restore operation" << Endl;
        auto partIds = blocker.GetPartIds(txId);
        for (auto partId : partIds) {
            Cerr << "  Part " << partId << Endl;
        }

        // Release all parts to complete the restore
        blocker.ReleaseAll(txId);
        // Stop blocking to allow subsequent TEvProgressOperation events through
        blocker.Stop();
        env.TestWaitNotification(runtime, txId);

        // Verify restore worked
        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table1"), {
            NLs::PathExist,
            NLs::IsTable
        });
        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table2"), {
            NLs::PathExist,
            NLs::IsTable
        });
    }

    //
    // ==================================================================================
    // Copy of the original failing test: MultipleTablesWithIndexesIncrementalRestore
    // This test uses TServer-based setup matching the original datashard test pattern.
    // ==================================================================================
    //

    static NKikimrPQ::TPQConfig DefaultPQConfig() {
        NKikimrPQ::TPQConfig pqConfig;
        pqConfig.SetEnabled(true);
        pqConfig.SetEnableProtoSourceIdInfo(true);
        pqConfig.SetTopicsAreFirstClassCitizen(true);
        pqConfig.SetMaxReadCookies(10);
        pqConfig.AddClientServiceType()->SetName("data-streams");
        pqConfig.SetCheckACL(false);
        pqConfig.SetRequireCredentialsInNewProtocol(false);
        pqConfig.MutableQuotingConfig()->SetEnableQuoting(false);
        return pqConfig;
    }

    static void SetupLoggingForServer(TTestActorRuntime& runtime) {
        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::CHANGE_EXCHANGE, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::CONTINUOUS_BACKUP, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::REPLICATION_SERVICE, NLog::PRI_DEBUG);
    }

    /**
     * Copy of the original failing test: MultipleTablesWithIndexesIncrementalRestore
     *
     * This test creates two tables with indexes, performs incremental backup,
     * drops the tables, and then restores them. The flaky failure occurs during
     * restore when operation parts execute in different orders, causing schema
     * version mismatch errors like:
     *   "schema version mismatch during metadata loading for: /Root/Table2/idx2 expected 3 got 4"
     *
     * This version integrates TOperationPartsBlocker to capture and control
     * the execution order of restore operation parts.
     */
    Y_UNIT_TEST(MultipleTablesWithIndexesIncrementalRestore) {
        using namespace NDataShard;
        using namespace Tests;

        TPortManager portManager;
        TServer::TPtr server = new TServer(TServerSettings(portManager.GetPort(2134), {}, DefaultPQConfig())
            .SetUseRealThreads(false)
            .SetDomainName("Root")
            .SetEnableChangefeedInitialScan(true)
            .SetEnableBackupService(true)
            .SetEnableRealSystemViewPaths(false)
        );

        auto& runtime = *server->GetRuntime();
        const auto edgeActor = runtime.AllocateEdgeActor();

        SetupLoggingForServer(runtime);
        InitRoot(server, edgeActor);

        // Create first table with index
        CreateShardedTable(server, edgeActor, "/Root", "Table1",
            TShardedTableOptions()
                .Columns({
                    {"key", "Uint32", true, false},
                    {"val1", "Uint32", false, false}
                })
                .Indexes({
                    {"idx1", {"val1"}, {}, NKikimrSchemeOp::EIndexTypeGlobal}
                }));

        // Create second table with different index
        CreateShardedTable(server, edgeActor, "/Root", "Table2",
            TShardedTableOptions()
                .Columns({
                    {"key", "Uint32", true, false},
                    {"val2", "Uint32", false, false}
                })
                .Indexes({
                    {"idx2", {"val2"}, {}, NKikimrSchemeOp::EIndexTypeGlobal}
                }));

        // Insert data into both tables
        ExecSQL(server, edgeActor, R"(
            UPSERT INTO `/Root/Table1` (key, val1) VALUES (1, 100), (2, 200);
            UPSERT INTO `/Root/Table2` (key, val2) VALUES (1, 1000), (2, 2000);
        )");

        // Create backup collection with both tables
        ExecSQL(server, edgeActor, R"(
            CREATE BACKUP COLLECTION `MultiTableCollection`
              ( TABLE `/Root/Table1`
              , TABLE `/Root/Table2`
              )
            WITH
              ( STORAGE = 'cluster'
              , INCREMENTAL_BACKUP_ENABLED = 'true'
              );
        )", false);

        // Full backup
        ExecSQL(server, edgeActor, R"(BACKUP `MultiTableCollection`;)", false);
        SimulateSleep(server, TDuration::Seconds(1));

        // Modify both tables
        ExecSQL(server, edgeActor, R"(
            UPSERT INTO `/Root/Table1` (key, val1) VALUES (3, 300);
            UPSERT INTO `/Root/Table2` (key, val2) VALUES (3, 3000);
        )");

        // Incremental backup
        ExecSQL(server, edgeActor, R"(BACKUP `MultiTableCollection` INCREMENTAL;)", false);
        SimulateSleep(server, TDuration::Seconds(5));

        // Capture expected states
        auto expected1 = KqpSimpleExecSuccess(runtime, R"(
            SELECT key, val1 FROM `/Root/Table1` ORDER BY key
        )");
        auto expected2 = KqpSimpleExecSuccess(runtime, R"(
            SELECT key, val2 FROM `/Root/Table2` ORDER BY key
        )");

        // Drop both tables
        ExecSQL(server, edgeActor, R"(DROP TABLE `/Root/Table1`;)", false);
        ExecSQL(server, edgeActor, R"(DROP TABLE `/Root/Table2`;)", false);

        // Restore
        ExecSQL(server, edgeActor, R"(RESTORE `MultiTableCollection`;)", false);
        runtime.SimulateSleep(TDuration::Seconds(10));

        // Verify both tables and indexes
        auto actual1 = KqpSimpleExecSuccess(runtime, R"(
            SELECT key, val1 FROM `/Root/Table1` ORDER BY key
        )");
        auto actual2 = KqpSimpleExecSuccess(runtime, R"(
            SELECT key, val2 FROM `/Root/Table2` ORDER BY key
        )");

        UNIT_ASSERT_VALUES_EQUAL(expected1, actual1);
        UNIT_ASSERT_VALUES_EQUAL(expected2, actual2);

        // Verify indexes work
        auto idx1Query = KqpSimpleExecSuccess(runtime, R"(
            SELECT key FROM `/Root/Table1` VIEW idx1 WHERE val1 = 300
        )");
        UNIT_ASSERT_C(idx1Query.find("uint32_value: 3") != TString::npos, "Index idx1 should work");

        auto idx2Query = KqpSimpleExecSuccess(runtime, R"(
            SELECT key FROM `/Root/Table2` VIEW idx2 WHERE val2 = 3000
        )");
        UNIT_ASSERT_C(idx2Query.find("uint32_value: 3") != TString::npos, "Index idx2 should work");

        // Verify both index implementation tables were restored
        auto idx1ImplCount = KqpSimpleExecSuccess(runtime, R"(
            SELECT COUNT(*) FROM `/Root/Table1/idx1/indexImplTable`
        )");
        UNIT_ASSERT_C(idx1ImplCount.find("uint64_value: 3") != TString::npos, "Table1 index impl should have 3 rows");

        auto idx2ImplCount = KqpSimpleExecSuccess(runtime, R"(
            SELECT COUNT(*) FROM `/Root/Table2/idx2/indexImplTable`
        )");
        UNIT_ASSERT_C(idx2ImplCount.find("uint64_value: 3") != TString::npos, "Table2 index impl should have 3 rows");

        // Verify index impl tables have correct data
        auto idx1ImplData = KqpSimpleExecSuccess(runtime, R"(
            SELECT val1, key FROM `/Root/Table1/idx1/indexImplTable` WHERE val1 = 300
        )");
        UNIT_ASSERT_C(idx1ImplData.find("uint32_value: 300") != TString::npos, "Table1 index should have val1=300");
        UNIT_ASSERT_C(idx1ImplData.find("uint32_value: 3") != TString::npos, "Table1 index should have key=3");

        auto idx2ImplData = KqpSimpleExecSuccess(runtime, R"(
            SELECT val2, key FROM `/Root/Table2/idx2/indexImplTable` WHERE val2 = 3000
        )");
        UNIT_ASSERT_C(idx2ImplData.find("uint32_value: 3000") != TString::npos, "Table2 index should have val2=3000");
        UNIT_ASSERT_C(idx2ImplData.find("uint32_value: 3") != TString::npos, "Table2 index should have key=3");
    }

    /**
     * Same test as above but with TOperationPartsBlocker to control restore operation parts order.
     * This allows testing all permutations of restore operation execution order.
     */
    Y_UNIT_TEST(MultipleTablesWithIndexesIncrementalRestore_WithPartsControl) {
        using namespace NDataShard;
        using namespace Tests;

        // Run with all permutations - adjust the part count based on discovery
        // The restore operation for 2 tables with indexes likely has 4+ parts
        ForEachPartsPermutation(4, [](const TVector<ui32>& permutation) {
            TPortManager portManager;
            TServer::TPtr server = new TServer(TServerSettings(portManager.GetPort(2134), {}, DefaultPQConfig())
                .SetUseRealThreads(false)
                .SetDomainName("Root")
                .SetEnableChangefeedInitialScan(true)
                .SetEnableBackupService(true)
                .SetEnableRealSystemViewPaths(false)
            );

            auto& runtime = *server->GetRuntime();
            const auto edgeActor = runtime.AllocateEdgeActor();

            // Reduce logging for permutation tests
            runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NLog::PRI_WARN);

            InitRoot(server, edgeActor);

            // Create first table with index
            CreateShardedTable(server, edgeActor, "/Root", "Table1",
                TShardedTableOptions()
                    .Columns({
                        {"key", "Uint32", true, false},
                        {"val1", "Uint32", false, false}
                    })
                    .Indexes({
                        {"idx1", {"val1"}, {}, NKikimrSchemeOp::EIndexTypeGlobal}
                    }));

            // Create second table with different index
            CreateShardedTable(server, edgeActor, "/Root", "Table2",
                TShardedTableOptions()
                    .Columns({
                        {"key", "Uint32", true, false},
                        {"val2", "Uint32", false, false}
                    })
                    .Indexes({
                        {"idx2", {"val2"}, {}, NKikimrSchemeOp::EIndexTypeGlobal}
                    }));

            // Insert data into both tables
            ExecSQL(server, edgeActor, R"(
                UPSERT INTO `/Root/Table1` (key, val1) VALUES (1, 100), (2, 200);
                UPSERT INTO `/Root/Table2` (key, val2) VALUES (1, 1000), (2, 2000);
            )");

            // Create backup collection with both tables
            ExecSQL(server, edgeActor, R"(
                CREATE BACKUP COLLECTION `MultiTableCollection`
                  ( TABLE `/Root/Table1`
                  , TABLE `/Root/Table2`
                  )
                WITH
                  ( STORAGE = 'cluster'
                  , INCREMENTAL_BACKUP_ENABLED = 'true'
                  );
            )", false);

            // Full backup
            ExecSQL(server, edgeActor, R"(BACKUP `MultiTableCollection`;)", false);
            SimulateSleep(server, TDuration::Seconds(1));

            // Modify both tables
            ExecSQL(server, edgeActor, R"(
                UPSERT INTO `/Root/Table1` (key, val1) VALUES (3, 300);
                UPSERT INTO `/Root/Table2` (key, val2) VALUES (3, 3000);
            )");

            // Incremental backup
            ExecSQL(server, edgeActor, R"(BACKUP `MultiTableCollection` INCREMENTAL;)", false);
            SimulateSleep(server, TDuration::Seconds(5));

            // Capture expected states
            auto expected1 = KqpSimpleExecSuccess(runtime, R"(
                SELECT key, val1 FROM `/Root/Table1` ORDER BY key
            )");
            auto expected2 = KqpSimpleExecSuccess(runtime, R"(
                SELECT key, val2 FROM `/Root/Table2` ORDER BY key
            )");

            // Drop both tables
            ExecSQL(server, edgeActor, R"(DROP TABLE `/Root/Table1`;)", false);
            ExecSQL(server, edgeActor, R"(DROP TABLE `/Root/Table2`;)", false);

            // NOW: Set up parts blocker BEFORE triggering restore
            TOperationPartsBlocker blocker(runtime);

            // Trigger restore (async - we'll control the parts)
            ExecSQL(server, edgeActor, R"(RESTORE `MultiTableCollection`;)", false);

            // Wait for any operation to be captured first
            // The restore will create multiple operations - we want the one with the most parts
            runtime.SimulateSleep(TDuration::Seconds(2));

            // Find the operation with most parts (this is the main restore operation)
            auto txIds = blocker.GetCapturedTxIds();
            Cerr << "Captured " << txIds.size() << " operations" << Endl;
            for (auto txId : txIds) {
                Cerr << "  TxId " << txId << " has " << blocker.GetPartCount(txId) << " parts" << Endl;
            }

            // Release parts for operations that have the expected part count
            bool foundMatchingOp = false;
            for (auto txId : txIds) {
                size_t partCount = blocker.GetPartCount(txId);
                if (partCount >= permutation.size()) {
                    Cerr << "Releasing parts for txId " << txId << " in order: "
                         << TPartsPermutationIterator::FormatPermutation(permutation) << Endl;
                    // Use permutation as indices into captured parts, not as part IDs
                    blocker.ReleaseByPermutationIndices(txId, permutation);
                    // Release any remaining parts that weren't in the permutation
                    blocker.ReleaseAll(txId);
                    foundMatchingOp = true;
                } else {
                    // Release other operations normally
                    blocker.ReleaseAll(txId);
                }
            }

            if (!foundMatchingOp && !txIds.empty()) {
                Cerr << "WARNING: No operation with exactly " << permutation.size()
                     << " parts found, releasing all normally" << Endl;
            }

            // Stop blocking and release any remaining
            blocker.Stop();
            blocker.ReleaseAllOperations();

            runtime.SimulateSleep(TDuration::Seconds(10));

            // Verify both tables and indexes
            auto actual1 = KqpSimpleExecSuccess(runtime, R"(
                SELECT key, val1 FROM `/Root/Table1` ORDER BY key
            )");
            auto actual2 = KqpSimpleExecSuccess(runtime, R"(
                SELECT key, val2 FROM `/Root/Table2` ORDER BY key
            )");

            UNIT_ASSERT_VALUES_EQUAL(expected1, actual1);
            UNIT_ASSERT_VALUES_EQUAL(expected2, actual2);

            // Verify indexes work
            auto idx1Query = KqpSimpleExecSuccess(runtime, R"(
                SELECT key FROM `/Root/Table1` VIEW idx1 WHERE val1 = 300
            )");
            UNIT_ASSERT_C(idx1Query.find("uint32_value: 3") != TString::npos,
                "Index idx1 should work with permutation " +
                TPartsPermutationIterator::FormatPermutation(permutation));

            auto idx2Query = KqpSimpleExecSuccess(runtime, R"(
                SELECT key FROM `/Root/Table2` VIEW idx2 WHERE val2 = 3000
            )");
            UNIT_ASSERT_C(idx2Query.find("uint32_value: 3") != TString::npos,
                "Index idx2 should work with permutation " +
                TPartsPermutationIterator::FormatPermutation(permutation));
        }, 24);  // Test up to 24 permutations (4!)
    }

} // Y_UNIT_TEST_SUITE
