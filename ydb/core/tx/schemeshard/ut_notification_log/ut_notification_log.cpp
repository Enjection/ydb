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
            if (e.OperationType == (ui32)ENotificationOperationType::Create && e.PathName == "Table1") {
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
            if (e.OperationType == (ui32)ENotificationOperationType::Alter && e.PathName == "Table1") {
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
            if (e.OperationType == (ui32)ENotificationOperationType::Drop) {
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
