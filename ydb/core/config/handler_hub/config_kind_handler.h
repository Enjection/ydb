#pragma once

#include <ydb/core/protos/config.pb.h>

#include <ydb/library/actors/core/actor.h>

#include <util/generic/hash.h>

#include <functional>
#include <memory>

namespace NKikimr::NConfigHub {

/**
 * A per-kind handler for config updates delivered through ConfigsDispatcher.
 *
 * A handler owns the application logic for a single config "kind"
 * (TConfigItem::EKind == a TAppConfig field number). The hub instantiates one
 * handler per registered kind and routes updates to it.
 *
 * For an opaque/marker-tagged config (a string field whose content is YAML of a
 * schema the cluster does not know), the handler reads its own section out of
 * the filtered TAppConfig and parses that string with ITS OWN protobuf schema
 * (an additional .proto that never has to be part of TAppConfig).
 */
class IConfigKindHandler {
public:
    virtual ~IConfigKindHandler() = default;

    /**
     * Invoked once with the initial config right after subscription, and again
     * on every update whose ItemKinds include this handler's kind.
     *
     * `config` is the dispatcher-FILTERED TAppConfig: only the subscribed
     * kinds' sections are populated (see ReplaceConfigItems in
     * ydb/core/cms/console/util.cpp). The opaque string field arrives
     * byte-for-byte; the dispatcher never inspects its content.
     */
    virtual void Apply(const NKikimrConfig::TAppConfig& config,
                       const NActors::TActorContext& ctx) = 0;
};

/** Lazily constructs a handler instance for one kind. */
using TConfigKindHandlerFactory = std::function<std::unique_ptr<IConfigKindHandler>()>;

/** kind (TConfigItem::EKind == TAppConfig field number) -> handler factory. */
using TConfigHandlerRegistry = THashMap<ui32, TConfigKindHandlerFactory>;

/**
 * Small ergonomic builder for a registry.
 *
 *   auto registry = TConfigHandlerRegistryBuilder()
 *       .Add<TMyPrivateConfigHandler>(
 *           (ui32)NKikimrConsole::TConfigItem::MyPrivateConfigItem, myDeps)
 *       .AddFactory(
 *           (ui32)NKikimrConsole::TConfigItem::OtherItem,
 *           [] { return std::make_unique<TOtherHandler>(); })
 *       .Build();
 */
class TConfigHandlerRegistryBuilder {
public:
    template <class THandler, class... TArgs>
    TConfigHandlerRegistryBuilder& Add(ui32 kind, TArgs... args) {
        Registry_.emplace(kind, [args...]() -> std::unique_ptr<IConfigKindHandler> {
            return std::make_unique<THandler>(args...);
        });
        return *this;
    }

    TConfigHandlerRegistryBuilder& AddFactory(ui32 kind, TConfigKindHandlerFactory factory) {
        Registry_.emplace(kind, std::move(factory));
        return *this;
    }

    TConfigHandlerRegistry Build() {
        return std::move(Registry_);
    }

private:
    TConfigHandlerRegistry Registry_;
};

/**
 * Example/adapter handler: forwards every Apply() to a callback.
 *
 * Useful as a thin handler when the application logic is a free function or a
 * lambda rather than a stateful object — and as the canonical example of
 * implementing IConfigKindHandler. A realistic "private config" handler would
 * instead pull its section out of `config` and parse the opaque string with its
 * own schema, e.g.:
 *
 *   void Apply(const NKikimrConfig::TAppConfig& config, const TActorContext&) override {
 *       const auto& raw = config.GetMyPrivateConfig().GetPayload(); // opaque YAML/text
 *       NMyApp::TPrivateConfig parsed;                              // additional .proto
 *       NYamlConfig::ParseInto(raw, parsed);                        // client-owned parse
 *       ApplyParsed(parsed);
 *   }
 */
class TFunctionalConfigKindHandler : public IConfigKindHandler {
public:
    using TApplyFn = std::function<void(const NKikimrConfig::TAppConfig&,
                                        const NActors::TActorContext&)>;

    explicit TFunctionalConfigKindHandler(TApplyFn fn)
        : Fn_(std::move(fn))
    {
    }

    void Apply(const NKikimrConfig::TAppConfig& config,
               const NActors::TActorContext& ctx) override {
        if (Fn_) {
            Fn_(config, ctx);
        }
    }

private:
    TApplyFn Fn_;
};

} // namespace NKikimr::NConfigHub
