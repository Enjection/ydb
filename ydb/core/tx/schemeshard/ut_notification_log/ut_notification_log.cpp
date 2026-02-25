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
}
