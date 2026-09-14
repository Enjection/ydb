#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>

#include <ydb/public/api/grpc/ydb_query_v1.grpc.pb.h>

#include <library/cpp/testing/common/network.h>
#include <library/cpp/testing/unittest/registar.h>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace NYdb;
using namespace NYdb::NQuery;

namespace {

    class TQueryService: public Ydb::Query::V1::QueryService::Service {
    public:
        explicit TQueryService(bool retry = false)
            : Retry(retry)
        {
        }

        grpc::Status ExecuteQuery(
            grpc::ServerContext*, const Ydb::Query::ExecuteQueryRequest* request,
            grpc::ServerWriter<Ydb::Query::ExecuteQueryResponsePart>* writer) override {
            Ydb::Query::ExecuteQueryResponsePart response;
            {
                std::lock_guard guard(Mutex);
                Requests.push_back(*request);
                response.set_status(Retry && Requests.size() == 1
                                        ? Ydb::StatusIds::UNAVAILABLE
                                        : Ydb::StatusIds::UNSUPPORTED);
            }
            response.add_issues()->set_message("IDEMPOTENCY_NOT_SUPPORTED: mock endpoint");
            writer->Write(response);
            return grpc::Status::OK;
        }

        std::vector<Ydb::Query::ExecuteQueryRequest> GetRequests() const {
            std::lock_guard guard(Mutex);
            return Requests;
        }

    private:
        const bool Retry;
        mutable std::mutex Mutex;
        std::vector<Ydb::Query::ExecuteQueryRequest> Requests;
    };

    void CheckKey(const std::optional<std::string>& key, bool retry, EExecMode mode = EExecMode::Execute) {
        NTesting::InitPortManagerFromEnv();
        const auto port = NTesting::GetFreePort();
        const std::string endpoint = "127.0.0.1:" + std::to_string(port);
        TQueryService service(retry);
        auto server = grpc::ServerBuilder()
                          .AddListeningPort(TString{endpoint}, grpc::InsecureServerCredentials())
                          .RegisterService(&service)
                          .BuildAndStart();
        UNIT_ASSERT(server);
        TDriver driver(TDriverConfig().SetEndpoint(endpoint).SetDiscoveryMode(EDiscoveryMode::Off).SetDatabase("/Root"));
        TQueryClient client(driver);

        auto settings = TExecuteQuerySettings().ExecMode(mode).ClientTimeout(TDuration::Seconds(10));
        if (key) {
            settings.Uid(*key);
        }
        settings.RetrySettings(TRetryOperationSettings().MaxRetries(retry ? 1 : 0).Idempotent(true));
        const auto copiedSettings = settings;
        const std::string ddl = "-- keep these bytes\nBACKUP `daily`;";
        const auto result = client.ExecuteQuery(ddl, TTxControl::NoTx(), copiedSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::UNSUPPORTED);
        UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "IDEMPOTENCY_NOT_SUPPORTED");
        const auto requests = service.GetRequests();
        UNIT_ASSERT_VALUES_EQUAL(requests.size(), retry ? 2 : 1);
        for (const auto& request : requests) {
            UNIT_ASSERT_VALUES_EQUAL(request.has_uid(), key.has_value());
            if (key) {
                UNIT_ASSERT_VALUES_EQUAL(request.uid(), *key);
            }
            UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(request.exec_mode()), static_cast<int>(mode));
            UNIT_ASSERT_VALUES_EQUAL(request.query_content().text(), ddl);
        }
        driver.Stop(true);
    }

} // namespace

Y_UNIT_TEST_SUITE(QueryIdempotency) {

    Y_UNIT_TEST(NonExecutingModesAreNeverUpgradedToExecute) {
        for (auto mode : {EExecMode::Unspecified, EExecMode::Parse, EExecMode::Validate, EExecMode::Explain}) {
            CheckKey("backup:mode", false, mode);
        }
    }

    Y_UNIT_TEST(AbsentKey) {
        CheckKey(std::nullopt, false);
    }

    Y_UNIT_TEST(ExactKeyAfterSettingsCopy) {
        CheckKey("ключ with spaces/and?symbols!", false);
        CheckKey(std::string(120, 'x') + "ключ", false);
        CheckKey(std::string("a\0b", 3), false);
    }

    Y_UNIT_TEST(ExplicitEmptyKeyIsPresent) {
        CheckKey("", false);
    }

    Y_UNIT_TEST(RetryPreservesIdentity) {
        CheckKey("ключ/retry with spaces", true);
    }

} // Y_UNIT_TEST_SUITE(QueryIdempotency)
