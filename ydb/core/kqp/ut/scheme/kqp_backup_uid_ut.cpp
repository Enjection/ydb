#include "kqp_uid_test_helpers.h"

#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/schemeshard/common/operation_idempotency.h>

#include <ydb/public/api/grpc/ydb_query_v1.grpc.pb.h>
#include <grpcpp/create_channel.h>

#include <atomic>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/draft/ydb_backup.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/operation/operation.h>

namespace NKikimr::NKqp {

    using namespace NYdb;

    Y_UNIT_TEST_SUITE(OperationIdempotencyCapabilities) {
        Y_UNIT_TEST(UnlistedOperationsAreUnsupported) {
            UNIT_ASSERT(!NSchemeShard::SupportsOperationIdempotency(NKikimrSchemeOp::ESchemeOpCreateTable));
            UNIT_ASSERT(!NSchemeShard::SupportsOperationIdempotency(static_cast<NKikimrSchemeOp::EOperationType>(1000000)));
            UNIT_ASSERT(!NSchemeShard::SupportsSqlOperationIdempotency("create"));
            UNIT_ASSERT(!NSchemeShard::SupportsSqlOperationIdempotency("unknown"));
            UNIT_ASSERT(!NSchemeShard::SupportsSqlOperationIdempotency(""));

            NKqpProto::TKqpSchemeOperation operation;
            UNIT_ASSERT(!NSchemeShard::GetSchemeOperationForIdempotency(operation));
            operation.MutableCreateTable()->SetOperationType(NKikimrSchemeOp::ESchemeOpCreateTable);
            UNIT_ASSERT(!NSchemeShard::GetSchemeOperationForIdempotency(operation));
            operation.MutableCreateObject();
            UNIT_ASSERT(!NSchemeShard::GetSchemeOperationForIdempotency(operation));
        }

        Y_UNIT_TEST(PhysicalOperationMustMatchDeclaredType) {
            NKqpProto::TKqpSchemeOperation operation;
            auto* payload = operation.MutableBackup();
            payload->SetOperationType(NKikimrSchemeOp::ESchemeOpBackupBackupCollection);
            UNIT_ASSERT(NSchemeShard::GetSchemeOperationForIdempotency(operation) == payload);
            UNIT_ASSERT(NSchemeShard::SupportsOperationIdempotency(payload->GetOperationType()));
            UNIT_ASSERT(NSchemeShard::SupportsSqlOperationIdempotency("backup"));

            payload->SetOperationType(NKikimrSchemeOp::ESchemeOpCreateTable);
            UNIT_ASSERT(!NSchemeShard::GetSchemeOperationForIdempotency(operation));
            // A supported payload type cannot make an unrelated KQP operation eligible.
            operation.MutableCreateTable()->SetOperationType(NKikimrSchemeOp::ESchemeOpBackupBackupCollection);
            UNIT_ASSERT(!NSchemeShard::GetSchemeOperationForIdempotency(operation));

            payload = operation.MutableBackupIncremental();
            payload->SetOperationType(NKikimrSchemeOp::ESchemeOpBackupIncrementalBackupCollection);
            UNIT_ASSERT(NSchemeShard::GetSchemeOperationForIdempotency(operation) == payload);
            UNIT_ASSERT(NSchemeShard::SupportsSqlOperationIdempotency("backupIncremental"));
            payload = operation.MutableRestore();
            payload->SetOperationType(NKikimrSchemeOp::ESchemeOpRestoreBackupCollection);
            UNIT_ASSERT(NSchemeShard::GetSchemeOperationForIdempotency(operation) == payload);
            UNIT_ASSERT(NSchemeShard::SupportsSqlOperationIdempotency("restore"));

            // ObjectType selects a different executor path even if a payload is present.
            operation.SetObjectType("test-object");
            UNIT_ASSERT(!NSchemeShard::GetSchemeOperationForIdempotency(operation));
        }
    }

    Y_UNIT_TEST_SUITE(BackupIdempotency) {
        Y_UNIT_TEST(UidLengthRejectedBeforeAdmission) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config).SetEnableBackupService(true));
            for (const TString& uid : TVector<TString>{"", TString(129, 'a'), TString(127, 'a') + "я"}) {
                const auto result = ExecuteUidQuery(kikimr, "BACKUP `missing`;", uid);
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
                UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "INVALID_OPERATION_UID");
            }
        }

        Y_UNIT_TEST(RequestUidRejectsNonExecutingModes) {
            TKikimrRunner kikimr;
            for (const auto mode : {Ydb::Query::EXEC_MODE_UNSPECIFIED, Ydb::Query::EXEC_MODE_PARSE,
                    Ydb::Query::EXEC_MODE_VALIDATE, Ydb::Query::EXEC_MODE_EXPLAIN}) {
                const auto result = ExecuteUidQuery(kikimr, "BACKUP `missing`;", TString("backup:mode"),
                    TDuration::Seconds(20), true, mode);
                const bool invalidMode = mode == Ydb::Query::EXEC_MODE_UNSPECIFIED || mode == Ydb::Query::EXEC_MODE_PARSE;
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), invalidMode ? EStatus::BAD_REQUEST : EStatus::UNSUPPORTED,
                    result.GetIssues().ToString());
                UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(),
                    invalidMode ? "Unexpected query mode" : "IDEMPOTENCY_NOT_SUPPORTED");
            }
        }

        Y_UNIT_TEST_TWIN(ForwardedUidRetainsOriginalBytesAndOrdinaryQueryType, Concurrent) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config).SetEnableBackupService(true).SetUseRealThreads(false));
            const auto create = kikimr.RunCall([&] {
                return kikimr.GetTableClient().CreateSession().GetValueSync().GetSession().ExecuteSchemeQuery(
                                                                                              "CREATE BACKUP COLLECTION `forward_uid` (TABLE `/Root/KeyValue`) WITH (STORAGE = 'cluster');")
                    .GetValueSync();
            });
            UNIT_ASSERT_VALUES_EQUAL_C(create.GetStatus(), EStatus::SUCCESS, create.GetIssues().ToString());
            auto* runtime = kikimr.GetTestServer().GetRuntime();
            unsigned forwarded = 0;
            const TString ddl = "-- exact original bytes\nBACKUP `forward_uid`;";
            const TString uid = TString(120, 'x') + "ключ"; // 128 UTF-8 bytes.
            const auto previous = runtime->SetObserverFunc([&](TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TEvKqp::TEvQueryRequest::EventType) {
                    auto* request = event->Get<TEvKqp::TEvQueryRequest>();
                    if (request->Record.GetRequest().GetUid() == uid) {
                        const auto ordinary = Concurrent ? NKikimrKqp::QUERY_TYPE_SQL_GENERIC_CONCURRENT_QUERY
                                                         : NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY;
                        UNIT_ASSERT_VALUES_EQUAL(request->GetType(), ordinary);
                        request->CalculateSerializedSize(); // Materialize the local gRPC context for forwarding.
                        UNIT_ASSERT_VALUES_EQUAL(request->Record.GetRequest().GetType(), ordinary);
                        const auto bytes = request->Record.SerializeAsString();
                        request->Record.Clear();
                        UNIT_ASSERT(request->Record.ParseFromString(bytes));
                        UNIT_ASSERT_VALUES_EQUAL(request->GetType(), ordinary);
                        UNIT_ASSERT_VALUES_EQUAL(request->GetQuery(), ddl);
                        UNIT_ASSERT_VALUES_EQUAL(request->Record.GetRequest().GetUid(), uid);
                        ++forwarded;
                    }
                }
                return NActors::TTestActorRuntimeBase::EEventAction::PROCESS;
            });
            TString originalId;
            for (unsigned attempt = 0; attempt != 2; ++attempt) {
                const auto result = kikimr.RunCall([&] {
                    return ExecuteUidQuery(kikimr, ddl, uid, TDuration::Seconds(20), Concurrent);
                });
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
                UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);
                TResultSetParser parser(result.GetResultSet(0));
                UNIT_ASSERT(parser.TryNextRow());
                const TString id(parser.ColumnParser("operation_id").GetUtf8());
                UNIT_ASSERT(!id.empty());
                if (!attempt) {
                    originalId = id;
                } else {
                    UNIT_ASSERT_VALUES_EQUAL(id, originalId);
                }
            }
            runtime->SetObserverFunc(previous);
            UNIT_ASSERT(forwarded >= 2);
        }

        Y_UNIT_TEST_TWIN(SchemeShardUidErrorsReachCaller, AccessDenied) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config)
                                     .SetEnableBackupService(true)
                                     .SetUseRealThreads(false));
            auto session = kikimr.RunCall([&] {
                return kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            });
            const auto create = kikimr.RunCall([&] {
                return session.ExecuteSchemeQuery(R"(
            CREATE BACKUP COLLECTION `uid_status` (TABLE `/Root/KeyValue`)
            WITH (STORAGE = 'cluster');
        )")
                    .GetValueSync();
            });
            UNIT_ASSERT_VALUES_EQUAL_C(create.GetStatus(), EStatus::SUCCESS, create.GetIssues().ToString());

            using TRequest = NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction;
            using TResponse = NSchemeShard::TEvSchemeShard::TEvModifySchemeTransactionResult;
            using TAction = NActors::TTestActorRuntimeBase::EEventAction;
            const auto status = AccessDenied ? NKikimrScheme::StatusAccessDenied : NKikimrScheme::StatusAlreadyExists;
            const TString reason = AccessDenied ? "Access to the operation is denied" : "Backup already exists";
            const auto intercepted = std::make_shared<std::atomic<bool>>(false);
            auto* runtime = kikimr.GetTestServer().GetRuntime();
            const auto previousObserver = runtime->SetObserverFunc([runtime, intercepted, status, reason](TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TRequest::EventType) {
                    const auto& record = event->Get<TRequest>()->Record;
                    if (record.TransactionSize() == 1 && !record.GetTransaction(0).GetOperationIdempotency().GetLookupOnly() && record.GetTransaction(0).GetOperationIdempotency().GetUid() == "backup:status") {
                        // Model SchemeShard's refusal before it admits any backup or restore
                        // work, exercising TxProxy, KQP, and the public gRPC response.
                        auto response = MakeHolder<TResponse>(status, record.GetTxId(), record.GetTabletId(), reason);
                        runtime->Send(new IEventHandle(event->Sender, event->GetRecipientRewrite(), response.Release()), 0, true);
                        intercepted->store(true);
                        return TAction::DROP;
                    }
                }
                return TAction::PROCESS;
            });
            const auto result = kikimr.RunCall([&] {
                return ExecuteUidQuery(kikimr, "BACKUP `uid_status`;", TString("backup:status"));
            });
            runtime->SetObserverFunc(previousObserver);
            UNIT_ASSERT(intercepted->load());
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), AccessDenied ? EStatus::UNAUTHORIZED : EStatus::ALREADY_EXISTS,
                                       result.GetIssues().ToString());
            UNIT_ASSERT_C(HasIssue(result.GetIssues(),
                AccessDenied ? NYql::TIssuesIds::KIKIMR_ACCESS_DENIED : NYql::TIssuesIds::KIKIMR_ALREADY_EXISTS),
                result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), reason);
            UNIT_ASSERT(result.GetResultSets().empty());
        }

        Y_UNIT_TEST_TWIN(IdempotencyRequestTimeoutAllowsRetry, Admission) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config)
                                     .SetEnableBackupService(true)
                                     .SetUseRealThreads(false));
            auto session = kikimr.RunCall([&] {
                return kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            });
            const auto create = kikimr.RunCall([&] {
                return session.ExecuteSchemeQuery(
                                  "CREATE BACKUP COLLECTION `old_tablet` (TABLE `/Root/KeyValue`) WITH (STORAGE = 'cluster');")
                    .GetValueSync();
            });
            UNIT_ASSERT_VALUES_EQUAL_C(create.GetStatus(), EStatus::SUCCESS, create.GetIssues().ToString());

            using TRequest = NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction;
            using TAction = NActors::TTestActorRuntimeBase::EEventAction;
            const auto intercepted = std::make_shared<std::atomic<bool>>(false);
            auto* runtime = kikimr.GetTestServer().GetRuntime();
            const auto previous = runtime->SetObserverFunc([intercepted](TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TRequest::EventType) {
                    const auto& record = event->Get<TRequest>()->Record;
                    if (record.TransactionSize() == 1 && record.GetTransaction(0).GetOperationIdempotency().GetLookupOnly() != Admission
                        && record.GetTransaction(0).GetOperationIdempotency().GetUid() == "backup:old-tablet") {
                        intercepted->store(true);
                        return TAction::DROP; // Model a lost lookup or admission request.
                    }
                }
                return TAction::PROCESS;
            });
            const TString sql = "BACKUP `old_tablet`;";
            auto future = kikimr.RunInThreadPool([&] {
                return ExecuteUidQuery(kikimr, sql, TString("backup:old-tablet"), TDuration::Seconds(60));
            });
            runtime->WaitFor("idempotency request", [&] { return intercepted->load(); });
            runtime->SimulateSleep(TDuration::Seconds(31));
            const auto failed = runtime->WaitFuture(future);
            runtime->SetObserverFunc(previous);
            UNIT_ASSERT_VALUES_EQUAL_C(failed.GetStatus(), EStatus::UNAVAILABLE, failed.GetIssues().ToString());
            UNIT_ASSERT(failed.GetResultSets().empty());
            // The failed attempt reserved nothing. The same request can proceed
            // once requests reach the tablet again.
            const auto retried = kikimr.RunCall([&] {
                return ExecuteUidQuery(kikimr, sql, TString("backup:old-tablet"), TDuration::Seconds(60));
            });
            UNIT_ASSERT_VALUES_EQUAL_C(retried.GetStatus(), EStatus::SUCCESS, retried.GetIssues().ToString());
        }

        Y_UNIT_TEST(LostAdmissionReplyRecoversOriginalOperationId) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config)
                                     .SetEnableBackupService(true)
                                     .SetUseRealThreads(false));
            auto session = kikimr.RunCall([&] {
                return kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            });
            const auto create = kikimr.RunCall([&] {
                return session.ExecuteSchemeQuery(
                                  "CREATE BACKUP COLLECTION `lost_reply` (TABLE `/Root/KeyValue`) WITH (STORAGE = 'cluster');")
                    .GetValueSync();
            });
            UNIT_ASSERT_VALUES_EQUAL_C(create.GetStatus(), EStatus::SUCCESS, create.GetIssues().ToString());

            using TPropose = NSchemeShard::TEvSchemeShard::TEvModifySchemeTransaction;
            using TReply = NSchemeShard::TEvSchemeShard::TEvModifySchemeTransactionResult;
            using TAction = NActors::TTestActorRuntimeBase::EEventAction;
            const auto originalId = std::make_shared<std::atomic<ui64>>(0);
            const auto proposals = std::make_shared<std::atomic<unsigned>>(0);
            const auto lost = std::make_shared<std::atomic<bool>>(false);
            auto* runtime = kikimr.GetTestServer().GetRuntime();
            const auto previous = runtime->SetObserverFunc([originalId, proposals, lost](TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TPropose::EventType) {
                    const auto& record = event->Get<TPropose>()->Record;
                    if (record.TransactionSize() == 1 && !record.GetTransaction(0).GetOperationIdempotency().GetLookupOnly() && record.GetTransaction(0).GetOperationIdempotency().GetUid() == "backup:lost-api") {
                        if (++*proposals == 1) {
                            originalId->store(record.GetTxId());
                        }
                    }
                } else if (event->GetTypeRewrite() == TReply::EventType) {
                    const auto& record = event->Get<TReply>()->Record;
                    if (originalId->load() && record.GetTxId() == originalId->load() && record.GetStatus() == NKikimrScheme::StatusAccepted && record.HasOperationId() && !lost->exchange(true))
                    {
                        // SchemeShard emits this reply only from transaction
                        // Complete. Work remains admitted while the caller gets
                        // no response and must recover through a new gRPC call.
                        return TAction::DROP;
                    }
                }
                return TAction::PROCESS;
            });
            const TString sql = "BACKUP `lost_reply`;";
            const auto failed = kikimr.RunCall([&] {
                return ExecuteUidQuery(kikimr, sql, TString("backup:lost-api"), TDuration::Seconds(2));
            });
            const auto expectedId = ToString(originalId->load());
            const auto recovered = kikimr.RunCall([&] {
                return ExecuteUidQuery(kikimr, sql, TString("backup:lost-api"));
            });
            runtime->SetObserverFunc(previous);
            UNIT_ASSERT(lost->load());
            UNIT_ASSERT_VALUES_EQUAL_C(failed.GetStatus(), EStatus::CLIENT_DEADLINE_EXCEEDED, failed.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL_C(recovered.GetStatus(), EStatus::SUCCESS, recovered.GetIssues().ToString());
            TResultSetParser parser(recovered.GetResultSet(0));
            UNIT_ASSERT(parser.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser("operation_id").GetUtf8(), expectedId);
            UNIT_ASSERT_VALUES_EQUAL(proposals->load(), 1);
            NOperation::TOperationClient operations(kikimr.GetDriver());
            const auto operation = kikimr.RunCall([&] {
                return operations.Get<NYdb::NBackup::TFullBackupResponse>(
                                     TOperation::TOperationId("ydb://fullbackup/14?id=" + expectedId))
                    .GetValueSync();
            });
            UNIT_ASSERT_VALUES_EQUAL_C(operation.Status().GetStatus(), EStatus::SUCCESS, operation.Status().GetIssues().ToString());
        }

        Y_UNIT_TEST(ReplayAfterCollectionDeletionKeepsOriginalOperation) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config).SetEnableBackupService(true));
            auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
            const auto create = session.ExecuteSchemeQuery(R"(
            CREATE BACKUP COLLECTION `retained_uid` (TABLE `/Root/KeyValue`)
            WITH (STORAGE = 'cluster');
        )")
                                    .GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(create.GetStatus(), EStatus::SUCCESS, create.GetIssues().ToString());

            const TString query = "BACKUP `retained_uid`;";
            const auto original = ExecuteUidQuery(kikimr, query, TString("backup:retained"));
            UNIT_ASSERT_VALUES_EQUAL_C(original.GetStatus(), EStatus::SUCCESS, original.GetIssues().ToString());
            TResultSetParser parser(original.GetResultSet(0));
            UNIT_ASSERT(parser.TryNextRow());
            const auto originalId = parser.ColumnParser("operation_id").GetUtf8();
            const TOperation::TOperationId operationId("ydb://fullbackup/14?id=" + originalId);
            NOperation::TOperationClient operations(kikimr.GetDriver());
            const auto deadline = TInstant::Now() + TDuration::Seconds(60);
            bool completed = false;
            while (TInstant::Now() < deadline) {
                const auto operation = operations.Get<NYdb::NBackup::TFullBackupResponse>(operationId).GetValueSync();
                if (operation.Ready()) {
                    UNIT_ASSERT_VALUES_EQUAL_C(operation.Status().GetStatus(), EStatus::SUCCESS, operation.Status().GetIssues().ToString());
                    completed = true;
                    break;
                }
                Sleep(TDuration::MilliSeconds(10));
            }
            UNIT_ASSERT_C(completed, "Full backup did not complete");
            const auto drop = session.ExecuteSchemeQuery("DROP BACKUP COLLECTION `retained_uid`;").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(drop.GetStatus(), EStatus::SUCCESS, drop.GetIssues().ToString());
            const auto retained = operations.Get<NYdb::NBackup::TFullBackupResponse>(operationId).GetValueSync();
            UNIT_ASSERT(retained.Ready());
            UNIT_ASSERT_VALUES_EQUAL_C(retained.Status().GetStatus(), EStatus::SUCCESS, retained.Status().GetIssues().ToString());

            // Collection deletion cannot replace retained operation metadata or
            // make its owner depend on artifacts needed only for new work.
            const auto replay = ExecuteUidQuery(kikimr, query, TString("backup:retained"));
            UNIT_ASSERT_VALUES_EQUAL_C(replay.GetStatus(), EStatus::SUCCESS, replay.GetIssues().ToString());
            TResultSetParser replayParser(replay.GetResultSet(0));
            UNIT_ASSERT(replayParser.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(replayParser.ColumnParser("operation_id").GetUtf8(), originalId);
            const auto conflict = ExecuteUidQuery(kikimr, query + " -- changed", TString("backup:retained"));
            UNIT_ASSERT_VALUES_EQUAL_C(conflict.GetStatus(), EStatus::PRECONDITION_FAILED, conflict.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS(conflict.GetIssues().ToString(), "UID_CONFLICT");
            const auto fresh = ExecuteUidQuery(kikimr, query, TString("backup:fresh"));
            UNIT_ASSERT_VALUES_EQUAL_C(fresh.GetStatus(), EStatus::SCHEME_ERROR, fresh.GetIssues().ToString());
        }
    } // Y_UNIT_TEST_SUITE(BackupIdempotency)

} // namespace NKikimr::NKqp
