#include <ydb/core/tx/datashard/ut_common/datashard_ut_common.h>
#include "datashard_ut_common_kqp.h"

#include <ydb/core/base/path.h>
#include <ydb/core/change_exchange/change_sender_common_ops.h>
#include <ydb/core/persqueue/events/global.h>
#include <ydb/core/persqueue/user_info.h>
#include <ydb/core/persqueue/write_meta.h>
#include <ydb/core/tx/scheme_board/events.h>
#include <ydb/core/tx/scheme_board/events_internal.h>
#include <ydb/public/sdk/cpp/client/ydb_datastreams/datastreams.h>
#include <ydb/public/sdk/cpp/client/ydb_persqueue_public/persqueue.h>
#include <ydb/public/sdk/cpp/client/ydb_topic/topic.h>
#include <library/cpp/protobuf/json/proto2json.h>
#include <ydb/public/lib/value/value.h>

#include <library/cpp/digest/md5/md5.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>

#include <util/generic/size_literals.h>
#include <util/string/join.h>
#include <util/string/printf.h>
#include <util/string/strip.h>

namespace NKikimr {

using namespace NDataShard;
using namespace NDataShard::NKqpHelpers;
using namespace Tests;

Y_UNIT_TEST_SUITE(IncrementalBackup) {
    using namespace NYdb::NPersQueue;
    using namespace NYdb::NDataStreams::V1;

    using TCdcStream = TShardedTableOptions::TCdcStream;

    static bool AreJsonsEqual(const TString& actual, const TString& expected, bool assertOnParseError = true) {
        bool parseResult;
        NJson::TJsonReaderConfig config;
        config.DontValidateUtf8 = true;
        NJson::TJsonValue actualJson;
        parseResult = NJson::ReadJsonTree(actual, &config, &actualJson, true);
        if (assertOnParseError) {
            Cerr << actual <<Endl;
            UNIT_ASSERT(parseResult);
        } else if (!parseResult) {
            return false;
        }

        NJson::TJsonValue expectedJson;
        parseResult = NJson::ReadJsonTree(expected, &config, &expectedJson, true);
        if (assertOnParseError) {
            Cerr << expected <<Endl;
            UNIT_ASSERT(parseResult);
        } else if (!parseResult) {
            return false;
        }

        class TScanner: public NJson::IScanCallback {
            NJson::TJsonValue& Actual;
            bool Success = true;

        public:
            explicit TScanner(NJson::TJsonValue& actual)
                : Actual(actual)
            {}

            bool Do(const TString& path, NJson::TJsonValue*, NJson::TJsonValue& expectedValue) override {
                // Skip if not "***"
                if (expectedValue.GetStringRobust() != "***") {
                    return true;
                }

                // Discrepancy in path format here.
                // GetValueByPath expects ".array.[0]" while Scanner provides with ".array[0]".
                // Don't use "***" inside a non-root array.
                UNIT_ASSERT_C(!path.Contains("["), TStringBuilder()
                    << "Please don't use \"***\" inside an array. Seems like " << path << " has array on the way");

                NJson::TJsonValue actualValue;
                // If "***", find a corresponding actual value
                if (!Actual.GetValueByPath(path, actualValue)) {
                    // Couldn't find an actual value for "***"
                    Success = false;
                    return false;
                }

                // Replace "***" with actual value
                expectedValue = actualValue;
                return true;
            }

            bool IsSuccess() const {
                return Success;
            }
        };

        TScanner scanner(actualJson);
        expectedJson.Scan(scanner);

        if (!scanner.IsSuccess()) {
            return false; // actualJson is missing a path to ***
        }

        return actualJson == expectedJson;
    }

    static void AssertJsonsEqual(const TString& actual, const TString& expected) {
        NKikimrChangeExchange::TChangeRecord proto;

        bool parseResult = proto.ParseFromString(actual);
        UNIT_ASSERT(parseResult);
        // if (assertOnParseError) {
        //     UNIT_ASSERT(parseResult);
        // } else if (!parseResult) {
        //     return false;
        // }

        auto config = NProtobufJson::TProto2JsonConfig()
            .SetFormatOutput(false)
            .SetEnumMode(NProtobufJson::TProto2JsonConfig::EnumName);
        TString actualStr = NProtobufJson::Proto2Json(proto.GetCdcDataChange(), config);

        UNIT_ASSERT_C(AreJsonsEqual(actualStr, expected), TStringBuilder()
            << "Jsons are different: " << actualStr << " != " << expected);
    }

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

    static void SetupLogging(TTestActorRuntime& runtime) {
        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::CHANGE_EXCHANGE, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::PQ_READ_PROXY, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::PQ_METACACHE, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::CONTINUOUS_BACKUP, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::REPLICATION_SERVICE, NLog::PRI_DEBUG);
    }

    TCdcStream WithInitialScan(TCdcStream streamDesc) {
        streamDesc.InitialState = NKikimrSchemeOp::ECdcStreamStateScan;
        return streamDesc;
    }

    TCdcStream Updates(NKikimrSchemeOp::ECdcStreamFormat format, const TString& name = "Stream") {
        return TCdcStream{
            .Name = name,
            .Mode = NKikimrSchemeOp::ECdcStreamModeUpdate,
            .Format = format,
        };
    }

    TShardedTableOptions SimpleTable() {
        return TShardedTableOptions();
    }

    using TActionFunc = std::function<ui64(TServer::TPtr)>;

    ui64 ResolvePqTablet(TTestActorRuntime& runtime, const TActorId& sender, const TString& path, ui32 partitionId) {
        auto streamDesc = Ls(runtime, sender, path);

        const auto& streamEntry = streamDesc->ResultSet.at(0);
        UNIT_ASSERT(streamEntry.ListNodeEntry);

        const auto& children = streamEntry.ListNodeEntry->Children;
        UNIT_ASSERT_VALUES_EQUAL(children.size(), 1);

        auto topicDesc = Navigate(runtime, sender, JoinPath(ChildPath(SplitPath(path), children.at(0).Name)),
            NSchemeCache::TSchemeCacheNavigate::EOp::OpTopic);

        const auto& topicEntry = topicDesc->ResultSet.at(0);
        UNIT_ASSERT(topicEntry.PQGroupInfo);

        const auto& pqDesc = topicEntry.PQGroupInfo->Description;
        for (const auto& partition : pqDesc.GetPartitions()) {
            if (partitionId == partition.GetPartitionId()) {
                return partition.GetTabletId();
            }
        }

        UNIT_ASSERT_C(false, "Cannot find partition: " << partitionId);
        return 0;
    }

    auto GetRecords(TTestActorRuntime& runtime, const TActorId& sender, const TString& path, ui32 partitionId) {
        NKikimrClient::TPersQueueRequest request;
        request.MutablePartitionRequest()->SetTopic(path);
        request.MutablePartitionRequest()->SetPartition(partitionId);

        auto& cmd = *request.MutablePartitionRequest()->MutableCmdRead();
        cmd.SetClientId(NKikimr::NPQ::CLIENTID_WITHOUT_CONSUMER);
        cmd.SetCount(10000);
        cmd.SetOffset(0);
        cmd.SetReadTimestampMs(0);
        cmd.SetExternalOperation(true);

        auto req = MakeHolder<TEvPersQueue::TEvRequest>();
        req->Record = std::move(request);
        ForwardToTablet(runtime, ResolvePqTablet(runtime, sender, path, partitionId), sender, req.Release());

        auto resp = runtime.GrabEdgeEventRethrow<TEvPersQueue::TEvResponse>(sender);
        UNIT_ASSERT(resp);

        TVector<std::pair<TString, TString>> result;
        for (const auto& r : resp->Get()->Record.GetPartitionResponse().GetCmdReadResult().GetResult()) {
            const auto data = NKikimr::GetDeserializedData(r.GetData());
            result.emplace_back(r.GetPartitionKey(), data.GetData());
        }

        return result;
    }

    void WaitForContent(TServer::TPtr server, const TActorId& sender, const TString& path, const TVector<TString>& expected) {
        while (true) {
            const auto records = GetRecords(*server->GetRuntime(), sender, path, 0);
            for (ui32 i = 0; i < std::min(records.size(), expected.size()); ++i) {
                AssertJsonsEqual(records.at(i).second, expected.at(i));
            }

            if (records.size() >= expected.size()) {
                UNIT_ASSERT_VALUES_EQUAL_C(records.size(), expected.size(),
                    "Unexpected record: " << records.at(expected.size()).second);
                break;
            }

            SimulateSleep(server, TDuration::Seconds(1));
        }
    }

    static THolder<TEvTxUserProxy::TEvProposeTransaction> SchemeTxTemplate(
            NKikimrSchemeOp::EOperationType type,
            const TString& workingDir = {})
    {
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetExecTimeoutPeriod(Max<ui64>());

        auto& tx = *request->Record.MutableTransaction()->MutableModifyScheme();
        tx.SetOperationType(type);

        if (workingDir) {
            tx.SetWorkingDir(workingDir);
        }

        return request;
    }

    static ui64 RunSchemeTx(
        TTestActorRuntimeBase& runtime,
        THolder<TEvTxUserProxy::TEvProposeTransaction>&& request,
        TActorId sender = {},
        bool viaActorSystem = false,
        TEvTxUserProxy::TEvProposeTransactionStatus::EStatus expectedStatus = TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecInProgress)
    {
        if (!sender) {
            sender = runtime.AllocateEdgeActor();
        }

        runtime.Send(new IEventHandle(MakeTxProxyID(), sender, request.Release()), 0, viaActorSystem);
        auto ev = runtime.GrabEdgeEventRethrow<TEvTxUserProxy::TEvProposeTransactionStatus>(sender);

        UNIT_ASSERT_VALUES_EQUAL_C(ev->Get()->Record.GetStatus(), expectedStatus, "Status: " << ev->Get()->Record.GetStatus() << " Issues: " << ev->Get()->Record.GetIssues());

        return ev->Get()->Record.GetTxId();
    }

    ui64 AsyncCreateContinuousBackup(
            Tests::TServer::TPtr server,
            const TString& workingDir,
            const TString& tableName)
    {
        auto request = SchemeTxTemplate(NKikimrSchemeOp::ESchemeOpCreateContinuousBackup, workingDir);

        auto& desc = *request->Record.MutableTransaction()->MutableModifyScheme()->MutableCreateContinuousBackup();
        desc.SetTableName(tableName);

        return RunSchemeTx(*server->GetRuntime(), std::move(request));
    }

    ui64 AsyncAlterTakeIncrementalBackup(
            Tests::TServer::TPtr server,
            const TString& workingDir,
            const TString& tableName)
    {
        auto request = SchemeTxTemplate(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup, workingDir);

        auto& desc = *request->Record.MutableTransaction()->MutableModifyScheme()->MutableAlterContinuousBackup();
        desc.SetTableName(tableName);
        desc.MutableTakeIncrementalBackup();

        return RunSchemeTx(*server->GetRuntime(), std::move(request));
    }

    TString MakeUpsertJson(ui32 key, ui32 value) {
        auto keyCell = TCell::Make<ui32>(key);
        auto valueCell = TCell::Make<ui32>(value);

        TString layout =
            R"({
                "Key":   {"Tags":[1],"Data":%s},
                "Upsert":{"Tags":[2],"Data":%s}
               })";

        NJsonWriter::TBuf keyJson;
        keyJson.WriteString(TSerializedCellVec::Serialize({keyCell}));
        auto keyData = keyJson.Str();

        NJsonWriter::TBuf valueJson;
        valueJson.WriteString(TSerializedCellVec::Serialize({valueCell}));
        auto valueData = valueJson.Str();

        return Sprintf(layout.c_str(), keyData.c_str(), valueData.c_str());
    }

    Y_UNIT_TEST(Basic) {
        TPortManager portManager;
        TServer::TPtr server = new TServer(TServerSettings(portManager.GetPort(2134), {}, DefaultPQConfig())
            .SetUseRealThreads(false)
            .SetDomainName("Root")
            .SetEnableChangefeedInitialScan(true)
        );

        auto& runtime = *server->GetRuntime();
        const auto edgeActor = runtime.AllocateEdgeActor();

        SetupLogging(runtime);
        InitRoot(server, edgeActor);
        CreateShardedTable(server, edgeActor, "/Root", "Table", SimpleTable());

        ExecSQL(server, edgeActor, R"(
            UPSERT INTO `/Root/Table` (key, value) VALUES
            (1, 10),
            (2, 20),
            (3, 30);
        )");

        WaitTxNotification(server, edgeActor, AsyncCreateContinuousBackup(server, "/Root", "Table"));

        ExecSQL(server, edgeActor, R"(
            UPSERT INTO `/Root/Table` (key, value) VALUES
            (1, 100),
            (2, 200),
            (3, 300);
        )");

        WaitForContent(server, edgeActor, "/Root/Table/continuousBackupImpl", {
            MakeUpsertJson(1, 100),
            MakeUpsertJson(2, 200),
            MakeUpsertJson(3, 300),
        });

        WaitTxNotification(server, edgeActor, AsyncAlterTakeIncrementalBackup(server, "/Root", "Table"));

        SimulateSleep(server, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/Table/incBackupImpl`
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 100 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 200 } }, "
            "{ items { uint32_value: 3 } items { uint32_value: 300 } }");

        ExecSQL(server, edgeActor, R"(
            UPSERT INTO `/Root/Table` (key, value) VALUES
            (2, 2000),
            (5, 5000),
            (7, 7000);
        )");

        ExecSQL(server, edgeActor, "DELETE FROM `/Root/Table` ON (key) VALUES (3), (5), (8);");

        SimulateSleep(server, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(
            KqpSimpleExec(runtime, R"(
                SELECT key, value FROM `/Root/Table/incBackupImpl`
                )"),
            "{ items { uint32_value: 1 } items { uint32_value: 100 } }, "
            "{ items { uint32_value: 2 } items { uint32_value: 200 } }, "
            "{ items { uint32_value: 3 } items { uint32_value: 300 } }");
    }

} // Cdc

} // NKikimr
