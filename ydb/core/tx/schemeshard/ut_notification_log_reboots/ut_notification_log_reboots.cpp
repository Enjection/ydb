#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/test_with_reboots.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/schemeshard/schemeshard_impl.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

namespace {

TVector<TEvSchemeShard::TEvInternalReadNotificationLogResult::TEntry> ReadNotificationLog(
    TTestActorRuntime& runtime)
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

Y_UNIT_TEST_SUITE(TNotificationLogReboots) {

    // Exhaustive reboot tests: verify operations complete across arbitrary reboot points.
    // Notification log persistence is verified separately below since ForwardToTablet/GrabEdgeEvent
    // is incompatible with the pipe reset framework (adding events shifts injection points).

    Y_UNIT_TEST_WITH_REBOOTS(CreateTableWithReboots) {
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
            }

            t.TestEnv->ReliablePropose(runtime, CreateTableRequest(++t.TxId, "/MyRoot",
                "Name: \"Table1\""
                "Columns { Name: \"key\"   Type: \"Uint64\" }"
                "Columns { Name: \"value\" Type: \"Utf8\" }"
                "KeyColumnNames: [\"key\"]"),
                {NKikimrScheme::StatusAccepted, NKikimrScheme::StatusAlreadyExists,
                 NKikimrScheme::StatusMultipleModifications});
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                TestDescribeResult(DescribePath(runtime, "/MyRoot/Table1"),
                    {NLs::Finished, NLs::IsTable});
            }
        });
    }

    Y_UNIT_TEST_WITH_REBOOTS(MultipleCreateTablesWithReboots) {
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                t.TestEnv->ReliablePropose(runtime, CreateTableRequest(++t.TxId, "/MyRoot",
                    "Name: \"T1\""
                    "Columns { Name: \"key\" Type: \"Uint64\" }"
                    "KeyColumnNames: [\"key\"]"),
                    {NKikimrScheme::StatusAccepted, NKikimrScheme::StatusAlreadyExists,
                     NKikimrScheme::StatusMultipleModifications});
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            t.TestEnv->ReliablePropose(runtime, CreateTableRequest(++t.TxId, "/MyRoot",
                "Name: \"T2\""
                "Columns { Name: \"key\" Type: \"Uint64\" }"
                "KeyColumnNames: [\"key\"]"),
                {NKikimrScheme::StatusAccepted, NKikimrScheme::StatusAlreadyExists,
                 NKikimrScheme::StatusMultipleModifications});
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                TestDescribeResult(DescribePath(runtime, "/MyRoot/T1"),
                    {NLs::Finished, NLs::IsTable});
                TestDescribeResult(DescribePath(runtime, "/MyRoot/T2"),
                    {NLs::Finished, NLs::IsTable});
            }
        });
    }

    Y_UNIT_TEST_WITH_REBOOTS(AlterTableWithReboots) {
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                t.TestEnv->ReliablePropose(runtime, CreateTableRequest(++t.TxId, "/MyRoot",
                    "Name: \"Table1\""
                    "Columns { Name: \"key\"   Type: \"Uint64\" }"
                    "Columns { Name: \"value\" Type: \"Utf8\" }"
                    "KeyColumnNames: [\"key\"]"),
                    {NKikimrScheme::StatusAccepted, NKikimrScheme::StatusAlreadyExists,
                     NKikimrScheme::StatusMultipleModifications});
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestAlterTable(runtime, ++t.TxId, "/MyRoot", R"(
                Name: "Table1"
                Columns { Name: "extra" Type: "Uint32" }
            )");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                TestDescribeResult(DescribePath(runtime, "/MyRoot/Table1"),
                    {NLs::Finished, NLs::IsTable,
                     NLs::CheckColumns("Table1", {"key", "value", "extra"}, {}, {"key"})});
            }
        });
    }

    // Targeted notification log persistence test: verify log entries survive a reboot.
    // Uses plain Y_UNIT_TEST with explicit RebootTablet since ReadNotificationLog
    // (ForwardToTablet + GrabEdgeEvent) is incompatible with Y_UNIT_TEST_WITH_REBOOTS.
    Y_UNIT_TEST(NotificationLogSurvivesReboot) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);

                // Create a table — generates a notification log entry
                t.TestEnv->ReliablePropose(runtime, CreateTableRequest(++t.TxId, "/MyRoot",
                    "Name: \"Table1\""
                    "Columns { Name: \"key\" Type: \"Uint64\" }"
                    "KeyColumnNames: [\"key\"]"),
                    {NKikimrScheme::StatusAccepted, NKikimrScheme::StatusAlreadyExists,
                     NKikimrScheme::StatusMultipleModifications});
                t.TestEnv->TestWaitNotification(runtime, t.TxId);

                // Verify log entry before reboot
                auto entriesBefore = ReadNotificationLog(runtime);
                UNIT_ASSERT_VALUES_EQUAL(entriesBefore.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(entriesBefore[0].PathName, "Table1");
                ui64 seqIdBefore = entriesBefore[0].SequenceId;

                // Reboot the SchemeShard
                TActorId sender = runtime.AllocateEdgeActor();
                RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);

                // Verify log entries survive reboot
                auto entriesAfter = ReadNotificationLog(runtime);
                UNIT_ASSERT_VALUES_EQUAL(entriesAfter.size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(entriesAfter[0].PathName, "Table1");
                UNIT_ASSERT_VALUES_EQUAL(entriesAfter[0].SequenceId, seqIdBefore);
            }
        });
    }
}
