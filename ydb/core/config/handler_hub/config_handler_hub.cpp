#include "config_handler_hub.h"

#include <ydb/core/cms/console/configs_dispatcher.h>
#include <ydb/core/cms/console/console.h>
#include <ydb/core/protos/console_config.pb.h>

#include <ydb/library/services/services.pb.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>

#include <util/generic/vector.h>

namespace NKikimr::NConfigHub {

using namespace NActors;
using namespace NKikimr::NConsole;

class TConfigHandlerHub : public TActorBootstrapped<TConfigHandlerHub> {
public:
    explicit TConfigHandlerHub(TConfigHandlerRegistry registry)
        : Registry_(std::move(registry))
    {
    }

    void Bootstrap(const TActorContext& ctx) {
        Become(&TThis::StateWork);

        TVector<ui32> kinds;
        kinds.reserve(Registry_.size());
        for (auto& [kind, factory] : Registry_) {
            Handlers_.emplace(kind, factory());
            kinds.push_back(kind);
        }

        LOG_INFO_S(ctx, NKikimrServices::CMS_CONFIGS,
            "TConfigHandlerHub: subscribing to ConfigsDispatcher for "
                << kinds.size() << " kind(s)");

        // Single shared subscription for the union of registered kinds.
        ctx.Send(MakeConfigsDispatcherID(SelfId().NodeId()),
            new TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest(kinds, SelfId()));
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvConsole::TEvConfigNotificationRequest, Handle);
            IgnoreFunc(TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse);
        default:
            Y_ABORT("TConfigHandlerHub: unexpected event type: %" PRIx32 " event: %s",
                ev->GetTypeRewrite(), ev->ToString().data());
        }
    }

private:
    void Handle(TEvConsole::TEvConfigNotificationRequest::TPtr& ev, const TActorContext& ctx) {
        const auto& rec = ev->Get()->Record;
        const auto& config = rec.GetConfig();

        if (rec.ItemKindsSize() > 0) {
            // Normal path: dispatch only the kinds that actually changed.
            for (ui32 kind : rec.GetItemKinds()) {
                if (auto it = Handlers_.find(kind); it != Handlers_.end()) {
                    it->second->Apply(config, ctx);
                }
            }
        } else {
            // Defensive: a notification without kind hints — refresh all handlers.
            for (auto& [kind, handler] : Handlers_) {
                Y_UNUSED(kind);
                handler->Apply(config, ctx);
            }
        }

        // Ack: ctor copies SubscriptionId + ConfigId from the request record.
        ctx.Send(ev->Sender,
            new TEvConsole::TEvConfigNotificationResponse(rec), 0, ev->Cookie);
    }

private:
    TConfigHandlerRegistry Registry_;
    THashMap<ui32, std::unique_ptr<IConfigKindHandler>> Handlers_;
};

IActor* CreateConfigHandlerHub(TConfigHandlerRegistry registry) {
    return new TConfigHandlerHub(std::move(registry));
}

} // namespace NKikimr::NConfigHub
