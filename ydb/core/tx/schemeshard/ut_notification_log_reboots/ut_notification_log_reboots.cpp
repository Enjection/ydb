#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/test_with_reboots.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/schemeshard/schemeshard_impl.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(TNotificationLogReboots) {

    Y_UNIT_TEST_WITH_REBOOTS(CreateTableWithReboots) {
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                // no inactive initialization needed
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
}
