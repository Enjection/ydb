#pragma once

#include <ydb/core/base/appdata.h>
#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/yql/providers/common/db_id_async_resolver/db_async_resolver.h>
#include <ydb/library/yql/providers/common/db_id_async_resolver/mdb_endpoint_generator.h>
#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>
#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>

#include <ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.h>

#include <ydb/library/yql/providers/generic/connector/libcpp/client.h>
#include <ydb/library/yql/providers/s3/actors_factory/yql_s3_actors_factory.h>
#include <ydb/library/yql/providers/solomon/gateway/yql_solomon_gateway.h>

#include <yql/essentials/core/dq_integration/transform/yql_dq_task_transform.h>
#include <yql/essentials/minikql/computation/mkql_computation_node.h>
#include <yql/essentials/public/issue/yql_issue_message.h>
#include <ydb/public/api/protos/ydb_value.pb.h>

#include <yt/yql/providers/yt/provider/yql_yt_gateway.h>
#include <ydb/library/logger/actor.h>

namespace NKikimrConfig {
    class TQueryServiceConfig;
}

namespace NKikimr::NKqp {

    bool CheckNestingDepth(const google::protobuf::Message& message, ui32 maxDepth);

    NYql::IYtGateway::TPtr MakeYtGateway(const NMiniKQL::IFunctionRegistry* functionRegistry, const NKikimrConfig::TQueryServiceConfig& queryServiceConfig);

    NYql::IHTTPGateway::TPtr MakeHttpGateway(const NYql::THttpGatewayConfig& httpGatewayConfig, NMonitoring::TDynamicCounterPtr countersRoot);

    NYql::IPqGateway::TPtr MakePqGateway(const std::shared_ptr<NYdb::TDriver>& driver, const NYql::TPqGatewayConfig& pqGatewayConfig);


    struct TKqpFederatedQuerySetup {
        NYql::IHTTPGateway::TPtr HttpGateway;
        NYql::NConnector::IClient::TPtr ConnectorClient;
        NYql::ISecuredServiceAccountCredentialsFactory::TPtr CredentialsFactory;
        NYql::IDatabaseAsyncResolver::TPtr DatabaseAsyncResolver;
        NYql::TS3GatewayConfig S3GatewayConfig;
        NYql::TGenericGatewayConfig GenericGatewayConfig;
        NYql::TYtGatewayConfig YtGatewayConfig;
        NYql::IYtGateway::TPtr YtGateway;
        NYql::TSolomonGatewayConfig SolomonGatewayConfig;
        NYql::ISolomonGateway::TPtr SolomonGateway;
        NMiniKQL::TComputationNodeFactory ComputationFactory;
        NYql::NDq::TS3ReadActorFactoryConfig S3ReadActorFactoryConfig;
        NYql::TTaskTransformFactory DqTaskTransformFactory;
        NYql::TPqGatewayConfig PqGatewayConfig;
        NYql::IPqGateway::TPtr PqGateway;
        NKikimr::TDeferredActorLogBackend::TSharedAtomicActorSystemPtr ActorSystemPtr;
        std::shared_ptr<NYdb::TDriver> Driver;
    };

    struct IKqpFederatedQuerySetupFactory {
        using TPtr = std::shared_ptr<IKqpFederatedQuerySetupFactory>;
        virtual std::optional<TKqpFederatedQuerySetup> Make(NActors::TActorSystem* actorSystem) = 0;
        virtual ~IKqpFederatedQuerySetupFactory() = default;
    };

    struct TKqpFederatedQuerySetupFactoryNoop: public IKqpFederatedQuerySetupFactory {
        std::optional<TKqpFederatedQuerySetup> Make(NActors::TActorSystem*) override {
            return std::nullopt;
        }
    };

    struct TKqpFederatedQuerySetupFactoryDefault: public IKqpFederatedQuerySetupFactory {
        TKqpFederatedQuerySetupFactoryDefault(){};

        TKqpFederatedQuerySetupFactoryDefault(
            NActors::TActorSystemSetup* setup,
            const NKikimr::TAppData* appData,
            const NKikimrConfig::TAppConfig& appConfig);

        std::optional<TKqpFederatedQuerySetup> Make(NActors::TActorSystem* actorSystem) override;

    private:
        NYql::THttpGatewayConfig HttpGatewayConfig;
        NYql::IHTTPGateway::TPtr HttpGateway;
        NYql::TS3GatewayConfig S3GatewayConfig;
        NYql::TGenericGatewayConfig GenericGatewaysConfig;
        NYql::TYtGatewayConfig YtGatewayConfig;
        NYql::IYtGateway::TPtr YtGateway;
        NYql::TSolomonGatewayConfig SolomonGatewayConfig;
        NYql::ISolomonGateway::TPtr SolomonGateway;
        NYql::ISecuredServiceAccountCredentialsFactory::TPtr CredentialsFactory;
        NYql::NConnector::IClient::TPtr ConnectorClient;
        std::optional<NActors::TActorId> DatabaseResolverActorId;
        NYql::IMdbEndpointGenerator::TPtr MdbEndpointGenerator;
        NYql::NDq::TS3ReadActorFactoryConfig S3ReadActorFactoryConfig;
        NYql::TTaskTransformFactory DqTaskTransformFactory;
        NYql::TPqGatewayConfig PqGatewayConfig;
        NYql::IPqGateway::TPtr PqGateway;
        NKikimr::TDeferredActorLogBackend::TSharedAtomicActorSystemPtr ActorSystemPtr;
        std::shared_ptr<NYdb::TDriver> Driver;
    };

    struct TKqpFederatedQuerySetupFactoryMock: public IKqpFederatedQuerySetupFactory {
        TKqpFederatedQuerySetupFactoryMock() = delete;

        TKqpFederatedQuerySetupFactoryMock(
            NYql::IHTTPGateway::TPtr httpGateway,
            NYql::NConnector::IClient::TPtr connectorClient,
            NYql::ISecuredServiceAccountCredentialsFactory::TPtr credentialsFactory,
            NYql::IDatabaseAsyncResolver::TPtr databaseAsyncResolver,
            const NYql::TS3GatewayConfig& s3GatewayConfig,
            const NYql::TGenericGatewayConfig& genericGatewayConfig,
            const NYql::TYtGatewayConfig& ytGatewayConfig,
            NYql::IYtGateway::TPtr ytGateway,
            const NYql::TSolomonGatewayConfig& solomonGatewayConfig,
            const NYql::ISolomonGateway::TPtr& solomonGateway,
            NMiniKQL::TComputationNodeFactory computationFactory,
            const NYql::NDq::TS3ReadActorFactoryConfig& s3ReadActorFactoryConfig,
            NYql::TTaskTransformFactory dqTaskTransformFactory,
            const NYql::TPqGatewayConfig& pqGatewayConfig,
            NYql::IPqGateway::TPtr pqGateway,
            NKikimr::TDeferredActorLogBackend::TSharedAtomicActorSystemPtr actorSystemPtr,
            std::shared_ptr<NYdb::TDriver> driver)
            : HttpGateway(httpGateway)
            , ConnectorClient(connectorClient)
            , CredentialsFactory(credentialsFactory)
            , DatabaseAsyncResolver(databaseAsyncResolver)
            , S3GatewayConfig(s3GatewayConfig)
            , GenericGatewayConfig(genericGatewayConfig)
            , YtGatewayConfig(ytGatewayConfig)
            , YtGateway(ytGateway)
            , SolomonGatewayConfig(solomonGatewayConfig)
            , SolomonGateway(solomonGateway)
            , ComputationFactory(computationFactory)
            , S3ReadActorFactoryConfig(s3ReadActorFactoryConfig)
            , DqTaskTransformFactory(dqTaskTransformFactory)
            , PqGatewayConfig(pqGatewayConfig)
            , PqGateway(pqGateway)
            , ActorSystemPtr(actorSystemPtr)
            , Driver(driver)
        {
        }

        std::optional<TKqpFederatedQuerySetup> Make(NActors::TActorSystem*) override {
            return TKqpFederatedQuerySetup{
                HttpGateway, ConnectorClient, CredentialsFactory,
                DatabaseAsyncResolver, S3GatewayConfig, GenericGatewayConfig,
                YtGatewayConfig, YtGateway, SolomonGatewayConfig,
                SolomonGateway, ComputationFactory, S3ReadActorFactoryConfig,
                DqTaskTransformFactory, PqGatewayConfig, PqGateway, ActorSystemPtr, Driver};
        }

    private:
        NYql::IHTTPGateway::TPtr HttpGateway;
        NYql::NConnector::IClient::TPtr ConnectorClient;
        NYql::ISecuredServiceAccountCredentialsFactory::TPtr CredentialsFactory;
        NYql::IDatabaseAsyncResolver::TPtr DatabaseAsyncResolver;
        NYql::TS3GatewayConfig S3GatewayConfig;
        NYql::TGenericGatewayConfig GenericGatewayConfig;
        NYql::TYtGatewayConfig YtGatewayConfig;
        NYql::IYtGateway::TPtr YtGateway;
        NYql::TSolomonGatewayConfig SolomonGatewayConfig;
        NYql::ISolomonGateway::TPtr SolomonGateway;
        NMiniKQL::TComputationNodeFactory ComputationFactory;
        NYql::NDq::TS3ReadActorFactoryConfig S3ReadActorFactoryConfig;
        NYql::TTaskTransformFactory DqTaskTransformFactory;
        NYql::TPqGatewayConfig PqGatewayConfig;
        NYql::IPqGateway::TPtr PqGateway;
        NKikimr::TDeferredActorLogBackend::TSharedAtomicActorSystemPtr ActorSystemPtr;
        std::shared_ptr<NYdb::TDriver> Driver;
    };

    IKqpFederatedQuerySetupFactory::TPtr MakeKqpFederatedQuerySetupFactory(
        NActors::TActorSystemSetup* setup,
        const NKikimr::TAppData* appData,
        const NKikimrConfig::TAppConfig& config);

    NMiniKQL::TComputationNodeFactory MakeKqpFederatedQueryComputeFactory(NMiniKQL::TComputationNodeFactory baseComputeFactory, const std::optional<TKqpFederatedQuerySetup>& federatedQuerySetup);

    // Used only for unit tests
    bool WaitHttpGatewayFinalization(NMonitoring::TDynamicCounterPtr countersRoot, TDuration timeout = TDuration::Minutes(1), TDuration refreshPeriod = TDuration::MilliSeconds(100));

    NYql::TIssues TruncateIssues(const NYql::TIssues& issues, ui32 maxLevels = 50, ui32 keepTailLevels = 3);

    template <typename TIssueMessage>
    void TruncateIssues(google::protobuf::RepeatedPtrField<TIssueMessage>* issuesProto, ui32 maxLevels = 50, ui32 keepTailLevels = 3) {
        NYql::TIssues issues;
        NYql::IssuesFromMessage(*issuesProto, issues);
        NYql::IssuesToMessage(TruncateIssues(issues, maxLevels, keepTailLevels), issuesProto);
    }

    NYql::TIssues ValidateResultSetColumns(const google::protobuf::RepeatedPtrField<Ydb::Column>& columns, ui32 maxNestingDepth = 90);

    using TGetSchemeEntryResult = TMaybe<NYdb::NScheme::ESchemeEntryType>;

    NThreading::TFuture<TGetSchemeEntryResult> GetSchemeEntryType(
        const std::optional<TKqpFederatedQuerySetup>& federatedQuerySetup,
        const TString& endpoint,
        const TString& database,
        bool useTls,
        const TString& structuredTokenJson,
        const TString& path);

}  // namespace NKikimr::NKqp
