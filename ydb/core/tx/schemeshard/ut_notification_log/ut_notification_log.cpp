#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/schemeshard/schemeshard_impl.h>

#include <util/string/printf.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

namespace {

TVector<TEvSchemeShard::TEvInternalReadNotificationLogResult::TEntry> ReadNotificationLog(
    TTestBasicRuntime& runtime)
{
    auto sender = runtime.AllocateEdgeActor();
    ForwardToTablet(runtime, TTestTxConfig::SchemeShard, sender,
        new TEvSchemeShard::TEvInternalReadNotificationLog());
    TAutoPtr<IEventHandle> handle;
    auto event = runtime.GrabEdgeEvent<TEvSchemeShard::TEvInternalReadNotificationLogResult>(handle);
    UNIT_ASSERT(event);
    return event->Entries;
}

struct TNotificationLogReadResult {
    TVector<TEvSchemeShard::TEvInternalReadNotificationLogResult::TEntry> Entries;
    ui64 MinInFlightPlanStep = 0;
};

TNotificationLogReadResult ReadNotificationLogFull(TTestBasicRuntime& runtime) {
    auto sender = runtime.AllocateEdgeActor();
    ForwardToTablet(runtime, TTestTxConfig::SchemeShard, sender,
        new TEvSchemeShard::TEvInternalReadNotificationLog());
    TAutoPtr<IEventHandle> handle;
    auto event = runtime.GrabEdgeEvent<TEvSchemeShard::TEvInternalReadNotificationLogResult>(handle);
    UNIT_ASSERT(event);
    return {event->Entries, event->MinInFlightPlanStep};
}

} // anonymous namespace

Y_UNIT_TEST_SUITE(TNotificationLogSchemaTests) {
    Y_UNIT_TEST(NotificationLogTableExists) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        // If the table didn't exist, the internal read transaction would fail/crash.
        // The key assertion is: no crash, table is accessible.
        auto entries = ReadNotificationLog(runtime);
        Y_UNUSED(entries);
    }

    Y_UNIT_TEST(CreateTableWritesLogEntry) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key"   Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        bool found = false;
        for (const auto& e : entries) {
            if (e.OperationType == (ui32)TTxState::TxCreateTable && e.PathName == "Table1") {
                found = true;
                UNIT_ASSERT_VALUES_EQUAL(e.TxId, (ui64)txId);
                UNIT_ASSERT_VALUES_EQUAL(e.ObjectType, (ui32)NKikimrSchemeOp::EPathTypeTable);
                UNIT_ASSERT_VALUES_EQUAL(e.Status, (ui32)NKikimrScheme::StatusSuccess);
                UNIT_ASSERT(e.SequenceId > 0);
                break;
            }
        }
        UNIT_ASSERT_C(found, "CREATE TABLE entry not found in notification log");
    }

    Y_UNIT_TEST(AlterTableWritesLogEntry) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key"   Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "extra" Type: "Uint32" }
        )");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        ui32 alterCount = 0;
        for (const auto& e : entries) {
            if (e.OperationType == (ui32)TTxState::TxAlterTable && e.PathName == "Table1") {
                ++alterCount;
            }
        }
        UNIT_ASSERT_C(alterCount >= 1, "ALTER TABLE entry not found in notification log");
    }

    Y_UNIT_TEST(DropTableWritesLogEntry) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key"   Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestDropTable(runtime, ++txId, "/MyRoot", "Table1");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        bool found = false;
        for (const auto& e : entries) {
            if (e.OperationType == (ui32)TTxState::TxDropTable && e.PathName == "Table1") {
                found = true;
                break;
            }
        }
        UNIT_ASSERT_C(found, "DROP TABLE entry not found in notification log");
    }

    Y_UNIT_TEST(SequenceIdsAreMonotonicAcrossOperations) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T2"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        UNIT_ASSERT(entries.size() >= 2);

        for (size_t i = 1; i < entries.size(); ++i) {
            UNIT_ASSERT_C(entries[i].SequenceId > entries[i-1].SequenceId,
                "SequenceIds must be strictly monotonic");
        }
    }

    Y_UNIT_TEST(OverflowRejectsNewOperations) {
        TSchemeShard* schemeshard;
        auto ssFactory = [&schemeshard](const TActorId& tablet, TTabletStorageInfo* info) {
            schemeshard = new TSchemeShard(tablet, info);
            return schemeshard;
        };
        TTestBasicRuntime runtime;
        TTestEnvOptions opts;
        TTestEnv env(runtime, opts, ssFactory);
        ui64 txId = 100;

        // Set a very low limit so we can hit it quickly
        schemeshard->MaxNotificationLogEntries = 2;

        // First table: should succeed and produce log entry #1
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Second table: should succeed and produce log entry #2
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T2"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Verify we have at least 2 entries
        auto entries = ReadNotificationLog(runtime);
        UNIT_ASSERT_C(entries.size() >= 2, "Expected at least 2 notification log entries");

        // Third table: should be rejected with StatusResourceExhausted
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T3"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )", {NKikimrScheme::StatusResourceExhausted});
    }

    Y_UNIT_TEST(RaisingLimitViaConfigUnblocksOperations) {
        TSchemeShard* schemeshard;
        auto ssFactory = [&schemeshard](const TActorId& tablet, TTabletStorageInfo* info) {
            schemeshard = new TSchemeShard(tablet, info);
            return schemeshard;
        };
        TTestBasicRuntime runtime;
        TTestEnvOptions opts;
        TTestEnv env(runtime, opts, ssFactory);
        ui64 txId = 100;

        // Set a low limit via config dispatcher
        {
            auto request = MakeHolder<NConsole::TEvConsole::TEvConfigNotificationRequest>();
            request->Record.MutableConfig()->MutableSchemeShardConfig()->SetMaxNotificationLogEntries(2);
            SetConfig(runtime, TTestTxConfig::SchemeShard, std::move(request));
        }

        // Fill the log to the limit
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T2"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Blocked
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T3"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )", {NKikimrScheme::StatusResourceExhausted});

        // Raise limit via config dispatcher
        {
            auto request = MakeHolder<NConsole::TEvConsole::TEvConfigNotificationRequest>();
            request->Record.MutableConfig()->MutableSchemeShardConfig()->SetMaxNotificationLogEntries(10);
            SetConfig(runtime, TTestTxConfig::SchemeShard, std::move(request));
        }

        // Now the same operation succeeds
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T3"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(LoweringLimitViaConfigBlocksOperations) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Create some tables with default limit (100000)
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T2"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Lower the limit below the current entry count via config dispatcher
        {
            auto request = MakeHolder<NConsole::TEvConsole::TEvConfigNotificationRequest>();
            request->Record.MutableConfig()->MutableSchemeShardConfig()->SetMaxNotificationLogEntries(1);
            SetConfig(runtime, TTestTxConfig::SchemeShard, std::move(request));
        }

        // Now operations are rejected
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T3"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )", {NKikimrScheme::StatusResourceExhausted});
    }

    Y_UNIT_TEST(PlanStepIsRecordedForCoordinatedOps) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // CreateTable goes through coordinator and gets a valid PlanStep
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        UNIT_ASSERT(!entries.empty());

        bool found = false;
        for (const auto& e : entries) {
            if (e.PathName == "Table1" && e.OperationType == (ui32)TTxState::TxCreateTable) {
                found = true;
                // Coordinated operations must have a non-zero PlanStep
                UNIT_ASSERT_C(e.PlanStep > 0,
                    "CreateTable should have a valid PlanStep, got: " << e.PlanStep);
                break;
            }
        }
        UNIT_ASSERT_C(found, "CREATE TABLE entry not found in notification log");
    }

    Y_UNIT_TEST(PlanStepIsRecordedForAlterTable) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key"   Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "extra" Type: "Uint32" }
        )");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        // Both CREATE and ALTER should have valid PlanSteps
        for (const auto& e : entries) {
            if (e.PathName == "Table1") {
                UNIT_ASSERT_C(e.PlanStep > 0,
                    "Table1 entry (opType=" << e.OperationType << ") should have a valid PlanStep, got: " << e.PlanStep);
            }
        }
    }

    Y_UNIT_TEST(PlanStepMonotonicAcrossOperations) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "T2"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        UNIT_ASSERT(entries.size() >= 2);

        // PlanSteps should be monotonically non-decreasing
        // (can be equal if batched in same coordinator step)
        ui64 prevPlanStep = 0;
        for (const auto& e : entries) {
            UNIT_ASSERT_C(e.PlanStep >= prevPlanStep,
                "PlanStep should be monotonically non-decreasing: prev=" << prevPlanStep
                    << " current=" << e.PlanStep << " path=" << e.PathName);
            prevPlanStep = e.PlanStep;
        }

        // Stronger assertion: (PlanStep, TxId) ordering must match SequenceId ordering
        // This verifies that DoPersistNotifications sorts by (PlanStep, TxId) before
        // assigning SequenceIds within a single tablet transaction
        for (size_t i = 1; i < entries.size(); ++i) {
            const auto& prev = entries[i-1];
            const auto& curr = entries[i];
            if (curr.PlanStep != prev.PlanStep || curr.TxId != prev.TxId) {
                bool planStepTxIdOrdering = std::tie(curr.PlanStep, curr.TxId) > std::tie(prev.PlanStep, prev.TxId);
                UNIT_ASSERT_C(planStepTxIdOrdering,
                    "(PlanStep, TxId) ordering must match SequenceId ordering:"
                        << " prev=(" << prev.PlanStep << "," << prev.TxId << ") seqId=" << prev.SequenceId
                        << " curr=(" << curr.PlanStep << "," << curr.TxId << ") seqId=" << curr.SequenceId);
            }
        }
    }

    Y_UNIT_TEST(MkDirDoesNotWriteLogEntry) {
        // MkDir is a simple operation that doesn't go through
        // the multi-part ReadyToNotify path, so no notification is written
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", "DirA");
        env.TestWaitNotification(runtime, txId);

        auto entries = ReadNotificationLog(runtime);
        bool found = false;
        for (const auto& e : entries) {
            if (e.PathName == "DirA") {
                found = true;
                break;
            }
        }
        UNIT_ASSERT_C(!found, "MkDir should not produce a notification log entry");
    }

    Y_UNIT_TEST(WatermarkIsZeroWhenNoInFlightOps) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto result = ReadNotificationLogFull(runtime);
        UNIT_ASSERT(!result.Entries.empty());
        UNIT_ASSERT_VALUES_EQUAL(result.MinInFlightPlanStep, 0u);
    }

    Y_UNIT_TEST(WatermarkReflectsInFlightPlanStep) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // We'll hold TEvSchemaChanged for the first CreateTable to keep it in-flight
        TVector<THolder<IEventHandle>> heldEvents;
        ui64 firstTxId = txId + 1;
        bool captured = false;

        auto observer = [&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvDataShard::EvSchemaChanged) {
                auto* msg = ev->Get<TEvDataShard::TEvSchemaChanged>();
                if (msg->Record.GetTxId() == firstTxId) {
                    captured = true;
                    heldEvents.push_back(THolder<IEventHandle>(ev.Release()));
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observer);

        // Start Table1 creation - it will get a PlanStep but stay in-flight
        // because we hold the TEvSchemaChanged responses
        AsyncCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");

        // Wait until we capture a TEvSchemaChanged for Table1
        // This means the operation reached the datashard and got a PlanStep
        {
            TDispatchOptions opts;
            opts.CustomFinalCondition = [&]() { return captured; };
            runtime.DispatchEvents(opts);
        }

        // Create Table2 and let it complete normally
        // The observer only matches firstTxId, so Table2's events pass through
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table2"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Read notification log — Table2 should be present, Table1 still in-flight
        auto result = ReadNotificationLogFull(runtime);

        // Watermark should reflect Table1's PlanStep (it's still in-flight)
        UNIT_ASSERT_C(result.MinInFlightPlanStep > 0,
            "MinInFlightPlanStep should be > 0 while Table1 is still in-flight, got: "
                << result.MinInFlightPlanStep);

        // Now release held events to let Table1 complete
        for (auto& ev : heldEvents) {
            runtime.Send(ev.Release());
        }
        heldEvents.clear();
        env.TestWaitNotification(runtime, firstTxId);

        // After Table1 completes, watermark should be 0
        auto result2 = ReadNotificationLogFull(runtime);
        UNIT_ASSERT_VALUES_EQUAL_C(result2.MinInFlightPlanStep, 0u,
            "MinInFlightPlanStep should be 0 after all ops complete, got: "
                << result2.MinInFlightPlanStep);
    }
}
