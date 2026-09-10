#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>

#include <ydb/public/api/grpc/ydb_query_v1.grpc.pb.h>
#include <grpcpp/create_channel.h>

#include <atomic>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/draft/ydb_backup.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/operation/operation.h>

namespace NKikimr::NKqp {

    using namespace NYdb;

    Y_UNIT_TEST_SUITE(NativeBackupIdempotency) {
        Y_UNIT_TEST_TWIN(UidLengthRejectedBeforeAdmission, Sql) {
            NKikimrConfig::TAppConfig config;
            config.MutableFeatureFlags()->SetEnableBackupService(true);
            TKikimrRunner kikimr(NKqp::TKikimrSettings(config).SetEnableBackupService(true));
            for (const TString& uid : TVector<TString>{"", TString(129, 'a'), TString(127, 'a') + "я"}) {
                auto settings = NQuery::TExecuteQuerySettings().ClientTimeout(TDuration::Seconds(20));
                TString ddl = "BACKUP `missing`;";
                if constexpr (Sql) {
                    ddl = "BACKUP `missing` WITH (uid = '" + uid + "');";
                } else {
                    settings.Uid(uid);
                }
                const auto result = kikimr.GetQueryClient().ExecuteQuery(ddl, NQuery::TTxControl::NoTx(), settings).GetValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::BAD_REQUEST, result.GetIssues().ToString());
                UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "INVALID_NATIVE_OPERATION_UID");
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
                // Buffered ExecuteQuery always enables concurrent result sets.
                // StreamExecuteQuery exercises both wire types explicitly.
                auto iterator = kikimr.RunCall([&] {
                    return kikimr.GetQueryClient().StreamExecuteQuery(ddl, NQuery::TTxControl::NoTx(),
                                                                      NQuery::TExecuteQuerySettings().Uid(uid).ConcurrentResultSets(Concurrent).ClientTimeout(TDuration::Seconds(20)))
                        .GetValueSync();
                });
                UNIT_ASSERT_VALUES_EQUAL_C(iterator.GetStatus(), EStatus::SUCCESS, iterator.GetIssues().ToString());
                TString id;
                for (;;) {
                    const auto part = kikimr.RunCall([&] { return iterator.ReadNext().GetValueSync(); });
                    if (part.EOS()) {
                        break;
                    }
                    UNIT_ASSERT_VALUES_EQUAL_C(part.GetStatus(), EStatus::SUCCESS, part.GetIssues().ToString());
                    if (part.HasResultSet()) {
                        TResultSetParser parser(part.GetResultSet());
                        UNIT_ASSERT(parser.TryNextRow());
                        id = parser.ColumnParser("operation_id").GetUtf8();
                    }
                }
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
            const TString reason = AccessDenied ? "Access to the operation is denied" : "Native backup already exists";
            const auto intercepted = std::make_shared<std::atomic<bool>>(false);
            auto* runtime = kikimr.GetTestServer().GetRuntime();
            const auto previousObserver = runtime->SetObserverFunc([runtime, intercepted, status, reason](TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TRequest::EventType) {
                    const auto& record = event->Get<TRequest>()->Record;
                    if (record.TransactionSize() == 1 && !record.GetTransaction(0).GetNativeOperationIdentity().GetLookupOnly() && record.GetTransaction(0).GetNativeOperationIdentity().GetUid() == "backup:status") {
                        // Model SchemeShard's refusal before it admits any native
                        // work, exercising TxProxy, KQP, gRPC, and SDK conversion.
                        auto response = MakeHolder<TResponse>(status, record.GetTxId(), record.GetTabletId(), reason);
                        runtime->Send(new IEventHandle(event->Sender, event->GetRecipientRewrite(), response.Release()), 0, true);
                        intercepted->store(true);
                        return TAction::DROP;
                    }
                }
                return TAction::PROCESS;
            });
            const auto result = kikimr.RunCall([&] {
                return kikimr.GetQueryClient().ExecuteQuery("BACKUP `uid_status`;", NQuery::TTxControl::NoTx(),
                                                            NQuery::TExecuteQuerySettings().Uid("backup:status").ClientTimeout(TDuration::Seconds(20)))
                    .GetValueSync();
            });
            runtime->SetObserverFunc(previousObserver);
            UNIT_ASSERT(intercepted->load());
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), AccessDenied ? EStatus::UNAUTHORIZED : EStatus::ALREADY_EXISTS,
                                       result.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), reason);
            UNIT_ASSERT(result.GetResultSets().empty());
        }

        Y_UNIT_TEST_TWIN(NativeRequestTimeoutAllowsRetry, Admission) {
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
                    if (record.TransactionSize() == 1 && record.GetTransaction(0).GetNativeOperationIdentity().GetLookupOnly() != Admission
                        && record.GetTransaction(0).GetNativeOperationIdentity().GetUid() == "backup:old-tablet") {
                        intercepted->store(true);
                        return TAction::DROP; // Model a lost lookup or admission request.
                    }
                }
                return TAction::PROCESS;
            });
            auto client = kikimr.GetQueryClient();
            const TString sql = "BACKUP `old_tablet`;";
            const auto settings = NQuery::TExecuteQuerySettings().Uid("backup:old-tablet").ClientTimeout(TDuration::Seconds(60)).RetrySettings(NQuery::TRetryOperationSettings().MaxRetries(0));
            auto future = kikimr.RunInThreadPool([&] {
                return client.ExecuteQuery(sql, NQuery::TTxControl::NoTx(), settings).GetValueSync();
            });
            runtime->WaitFor("native request", [&] { return intercepted->load(); });
            runtime->SimulateSleep(TDuration::Seconds(31));
            const auto failed = runtime->WaitFuture(future);
            runtime->SetObserverFunc(previous);
            UNIT_ASSERT_VALUES_EQUAL_C(failed.GetStatus(), EStatus::UNAVAILABLE, failed.GetIssues().ToString());
            UNIT_ASSERT(failed.GetResultSets().empty());
            // The failed attempt reserved nothing. The same request can proceed
            // once requests reach the tablet again.
            const auto retried = kikimr.RunCall([&] {
                return client.ExecuteQuery(sql, NQuery::TTxControl::NoTx(), settings).GetValueSync();
            });
            UNIT_ASSERT_VALUES_EQUAL_C(retried.GetStatus(), EStatus::SUCCESS, retried.GetIssues().ToString());
        }

        Y_UNIT_TEST(LostAdmissionReplyRecoversOriginalSdkOperationId) {
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
                    if (record.TransactionSize() == 1 && !record.GetTransaction(0).GetNativeOperationIdentity().GetLookupOnly() && record.GetTransaction(0).GetNativeOperationIdentity().GetUid() == "backup:lost-api") {
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
                        // no response and must recover through a new SDK call.
                        return TAction::DROP;
                    }
                }
                return TAction::PROCESS;
            });
            auto client = kikimr.GetQueryClient();
            const TString sql = "BACKUP `lost_reply`;";
            auto settings = NQuery::TExecuteQuerySettings().Uid("backup:lost-api").ClientTimeout(TDuration::Seconds(2)).RetrySettings(NQuery::TRetryOperationSettings().MaxRetries(0));
            const auto failed = kikimr.RunCall([&] {
                return client.ExecuteQuery(sql, NQuery::TTxControl::NoTx(), settings).GetValueSync();
            });
            const auto expectedId = ToString(originalId->load());
            settings.ClientTimeout(TDuration::Seconds(20));
            const auto recovered = kikimr.RunCall([&] {
                return client.ExecuteQuery(sql, NQuery::TTxControl::NoTx(), settings).GetValueSync();
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

            auto client = kikimr.GetQueryClient();
            const TString query = "BACKUP `retained_uid`;";
            const auto settings = NQuery::TExecuteQuerySettings().Uid("backup:retained");
            const auto original = client.ExecuteQuery(query, NQuery::TTxControl::NoTx(), settings).GetValueSync();
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
            const auto replay = client.ExecuteQuery(query, NQuery::TTxControl::NoTx(), settings).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(replay.GetStatus(), EStatus::SUCCESS, replay.GetIssues().ToString());
            TResultSetParser replayParser(replay.GetResultSet(0));
            UNIT_ASSERT(replayParser.TryNextRow());
            UNIT_ASSERT_VALUES_EQUAL(replayParser.ColumnParser("operation_id").GetUtf8(), originalId);
            const auto conflict = client.ExecuteQuery(query + " -- changed", NQuery::TTxControl::NoTx(), settings).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(conflict.GetStatus(), EStatus::PRECONDITION_FAILED, conflict.GetIssues().ToString());
            UNIT_ASSERT_STRING_CONTAINS(conflict.GetIssues().ToString(), "UID_CONFLICT");
            const auto fresh = client.ExecuteQuery(query, NQuery::TTxControl::NoTx(),
                                                   NQuery::TExecuteQuerySettings().Uid("backup:fresh"))
                                   .GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(fresh.GetStatus(), EStatus::SCHEME_ERROR, fresh.GetIssues().ToString());
        }
    } // Y_UNIT_TEST_SUITE(NativeBackupIdempotency)

} // namespace NKikimr::NKqp
