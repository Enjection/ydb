#pragma once

#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/public/api/grpc/ydb_query_v1.grpc.pb.h>
#include <ydb/public/sdk/cpp/src/library/issue/yql_issue_message.h>

#include <grpcpp/create_channel.h>

#include <chrono>

namespace NKikimr::NKqp {

// Send UID metadata directly, so server tests do not depend on SDK UID support.
// With the simulated actor runtime, call this from TKikimrRunner::RunCall.
inline NYdb::NQuery::TExecuteQueryResult ExecuteUidQuery(
    TKikimrRunner& kikimr, const TString& query, TMaybe<TString> uid = Nothing(),
    TDuration timeout = TDuration::Seconds(20), bool concurrent = true,
    Ydb::Query::ExecMode mode = Ydb::Query::EXEC_MODE_EXECUTE)
{
    using namespace NYdb;
    auto client = kikimr.GetQueryClient();
    const auto sessionResult = client.GetSession().GetValueSync();
    UNIT_ASSERT_C(sessionResult.IsSuccess(), sessionResult.GetIssues().ToString());
    auto session = sessionResult.GetSession();
    auto stub = Ydb::Query::V1::QueryService::NewStub(
        grpc::CreateChannel(std::string(kikimr.GetEndpoint()), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.AddMetadata("x-ydb-database", "/Root");
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout.MilliSeconds()));
    Ydb::Query::ExecuteQueryRequest request;
    request.set_session_id(session.GetId());
    request.set_exec_mode(mode);
    request.set_concurrent_result_sets(concurrent);
    request.mutable_query_content()->set_syntax(Ydb::Query::SYNTAX_YQL_V1);
    request.mutable_query_content()->set_text(query.data(), query.size());
    if (uid) {
        request.set_uid(uid->data(), uid->size());
    }
    auto stream = stub->ExecuteQuery(&context, request);
    Ydb::Query::ExecuteQueryResponsePart part;
    EStatus status = EStatus::SUCCESS;
    NIssue::TIssues issues;
    std::vector<TResultSet> results;
    while (stream->Read(&part)) {
        if (part.status() != Ydb::StatusIds::SUCCESS) {
            status = static_cast<EStatus>(part.status());
        }
        NIssue::TIssues partIssues;
        NIssue::IssuesFromMessage(part.issues(), partIssues);
        issues.AddIssues(partIssues);
        if (part.has_result_set()) {
            results.emplace_back(part.result_set());
        }
    }
    const auto transport = stream->Finish();
    if (transport.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        status = EStatus::CLIENT_DEADLINE_EXCEEDED;
    } else {
        UNIT_ASSERT_C(transport.ok(), transport.error_message());
    }
    return {TStatus(status, std::move(issues)), std::move(results), {}, {}};
}

} // namespace NKikimr::NKqp
