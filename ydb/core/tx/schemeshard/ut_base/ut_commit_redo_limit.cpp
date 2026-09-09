#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(TSchemeShardCheckProposeSize) {

    //TODO: can't check all operations as many of them do not implement
    // TSubOperation::AbortPropose() properly and will abort.

    Y_UNIT_TEST(CreateTableRollsBackBeforeRetry) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;
        const auto initialDomain = DescribePath(runtime, "/MyRoot").GetPathDescription().GetDomainDescription();
        TControlWrapper redoLimit;
        TControlBoard::RegisterSharedControl(redoLimit, runtime.GetAppData().Icb->TabletControls.MaxCommitRedoMB);
        redoLimit.Reset(200, 1, 4096);
        const TString schema = R"(
            Name: "table"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
            UniformPartitionsCount: 3
        )";
        redoLimit = 1;
        TestCreateTable(runtime, ++txId, "/MyRoot", schema, {NKikimrScheme::StatusSchemeError});
        redoLimit = 200;
        TestDescribeResult(DescribePath(runtime, "/MyRoot/table"), {NLs::PathNotExist});
        TestDescribeResult(DescribePath(runtime, "/MyRoot"), {
            NLs::PathsInsideDomain(initialDomain.GetPathsInside()), NLs::ShardsInsideDomain(initialDomain.GetShardsInside()),
        });
        TestCreateTable(runtime, ++txId, "/MyRoot", schema);
        env.TestWaitNotification(runtime, txId);
        RebootTablet(runtime, TTestTxConfig::SchemeShard, runtime.AllocateEdgeActor());
        TestDescribeResult(DescribePath(runtime, "/MyRoot/table"), {NLs::PathExist, NLs::Finished});
        TestDescribeResult(DescribePath(runtime, "/MyRoot"), {
            NLs::PathsInsideDomain(initialDomain.GetPathsInside() + 1), NLs::ShardsInsideDomain(initialDomain.GetShardsInside() + 3),
        });
    }

    Y_UNIT_TEST(TopicCreateAndAlterRollBackBeforeRetry) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;
        const auto initialDomain = DescribePath(runtime, "/MyRoot").GetPathDescription().GetDomainDescription();
        TControlWrapper redoLimit;
        TControlBoard::RegisterSharedControl(redoLimit, runtime.GetAppData().Icb->TabletControls.MaxCommitRedoMB);
        redoLimit.Reset(200, 1, 4096);
        const TString create = R"(
            Name: "topic"
            TotalGroupCount: 2
            PartitionPerTablet: 1
            PQTabletConfig { PartitionConfig { LifetimeSeconds: 10 } }
        )";
        const TString alter = R"(
            Name: "topic"
            TotalGroupCount: 5
            PartitionPerTablet: 1
            PQTabletConfig { PartitionConfig { LifetimeSeconds: 20 } }
        )";
        redoLimit = 1;
        TestCreatePQGroup(runtime, ++txId, "/MyRoot", create, {NKikimrScheme::StatusSchemeError});
        redoLimit = 200;
        TestDescribeResult(DescribePath(runtime, "/MyRoot/topic"), {NLs::PathNotExist});
        TestDescribeResult(DescribePath(runtime, "/MyRoot"), {
            NLs::PathsInsideDomain(initialDomain.GetPathsInside()), NLs::ShardsInsideDomain(initialDomain.GetShardsInside()),
            NLs::PQGroupsInsideDomain(0), NLs::PQPartitionsInsideDomain(0),
        });
        TestCreatePQGroup(runtime, ++txId, "/MyRoot", create);
        env.TestWaitNotification(runtime, txId);
        redoLimit = 1;
        TestAlterPQGroup(runtime, ++txId, "/MyRoot", alter, {NKikimrScheme::StatusSchemeError});
        redoLimit = 200;
        TestDescribeResult(DescribePath(runtime, "/MyRoot/topic"), {
            NLs::PathExist, NLs::Finished, NLs::CheckPQAlterVersion("topic", 1),
        });
        TestDescribeResult(DescribePath(runtime, "/MyRoot"), {
            NLs::PathsInsideDomain(initialDomain.GetPathsInside() + 1), NLs::ShardsInsideDomain(initialDomain.GetShardsInside() + 3),
            NLs::PQGroupsInsideDomain(1), NLs::PQPartitionsInsideDomain(2),
        });
        TestAlterPQGroup(runtime, ++txId, "/MyRoot", alter);
        env.TestWaitNotification(runtime, txId);
        RebootTablet(runtime, TTestTxConfig::SchemeShard, runtime.AllocateEdgeActor());
        TestDescribeResult(DescribePath(runtime, "/MyRoot/topic"), {
            NLs::PathExist, NLs::Finished, NLs::CheckPQAlterVersion("topic", 2),
        });
        TestDescribeResult(DescribePath(runtime, "/MyRoot"), {
            NLs::PathsInsideDomain(initialDomain.GetPathsInside() + 1), NLs::ShardsInsideDomain(initialDomain.GetShardsInside() + 6),
            NLs::PQGroupsInsideDomain(1), NLs::PQPartitionsInsideDomain(5),
        });
    }

    Y_UNIT_TEST(CopyTable) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        // Take control over MaxCommitRedoMB ICB setting.
        // Drop down its min-value limit to be able to set it as low as test needs.
        TControlWrapper MaxCommitRedoMB;
        {
            TControlBoard::RegisterSharedControl(MaxCommitRedoMB, runtime.GetAppData().Icb->TabletControls.MaxCommitRedoMB);
            MaxCommitRedoMB.Reset(200, 1, 4096);
        }

        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "table"
            Columns { Name: "key" Type: "Uint64"}
            Columns { Name: "value" Type: "Utf8"}
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // 1. Set MaxCommitRedoMB to 1 and try to create table.
        //
        // (Check at the operation's Propose tests commit redo size against (MaxCommitRedoMB - 1)
        // to give 1MB leeway to executer/tablet inner stuff to may be do "something extra".
        // So MaxCommitRedoMB = 1 means effective 0 for the size of operation's commit.)
        {
            MaxCommitRedoMB = 1;
            AsyncCopyTable(runtime, ++txId, "/MyRoot", "table-copy", "/MyRoot/table");
            TestModificationResults(runtime, txId,
                {{NKikimrScheme::StatusSchemeError, "local tx commit redo size generated by IgniteOperation() is more than allowed limit"}}
            );
            env.TestWaitNotification(runtime, txId);
        }

        // 2. Set MaxCommitRedoMB back to high value and try again.
        {
            MaxCommitRedoMB = 200;
            AsyncCopyTable(runtime, ++txId, "/MyRoot", "table-copy", "/MyRoot/table");
            env.TestWaitNotification(runtime, txId);
        }
    }

    Y_UNIT_TEST(CopyTables) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        // Take control over MaxCommitRedoMB ICB setting.
        // Drop down its min-value limit to be able to set it as low as test needs.
        TControlWrapper MaxCommitRedoMB;
        {
            TControlBoard::RegisterSharedControl(MaxCommitRedoMB, runtime.GetAppData().Icb->TabletControls.MaxCommitRedoMB);
            MaxCommitRedoMB.Reset(200, 1, 4096);
        }

        const ui64 tables = 100;
        const ui64 shardsPerTable = 1;

        ui64 txId = 100;

        for (ui64 i : xrange(tables)) {
            TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(
                R"(
                    Name: "table-%lu"
                    Columns { Name: "key" Type: "Uint64"}
                    Columns { Name: "value" Type: "Utf8"}
                    KeyColumnNames: ["key"]
                    UniformPartitionsCount: %lu
                )",
                i,
                shardsPerTable
            ));
            env.TestWaitNotification(runtime, txId);
        }

        auto testCopyTables = [](auto& runtime, ui64 txId, ui64 tables) {
            TVector<TEvTx*> schemeTxs;
            for (ui64 i : xrange(tables)) {
                schemeTxs.push_back(CopyTableRequest(txId, "/MyRoot", Sprintf("table-%lu-copy", i), Sprintf("/MyRoot/table-%lu", i)));
            }
            AsyncSend(runtime, TTestTxConfig::SchemeShard, CombineSchemeTransactions(schemeTxs));
        };

        // 1. Set MaxCommitRedoMB to 1 and try to copy tables.
        {
            MaxCommitRedoMB = 1;
            testCopyTables(runtime, ++txId, tables);
            TestModificationResults(runtime, txId,
                {{NKikimrScheme::StatusSchemeError, "local tx commit redo size generated by IgniteOperation() is more than allowed limit"}}
            );
        }

        // 2. Set MaxCommitRedoMB back to high value and try again.
        {
            MaxCommitRedoMB = 200;
            testCopyTables(runtime, ++txId, tables);
            TestModificationResults(runtime, txId, {{NKikimrScheme::StatusAccepted}});
        }
    }

}
