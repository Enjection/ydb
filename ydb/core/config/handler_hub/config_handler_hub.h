#pragma once

#include "config_kind_handler.h"

#include <ydb/library/actors/core/actor.h>

namespace NKikimr::NConfigHub {

/**
 * Creates the config handler hub actor.
 *
 * The hub is a single subscriber on top of the (unchanged) ConfigsDispatcher:
 *   - on Bootstrap it instantiates one handler per entry in `registry` and
 *     subscribes to the union of their kinds with a single
 *     TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest;
 *   - on each TEvConsole::TEvConfigNotificationRequest it routes the (filtered)
 *     TAppConfig to the handler(s) whose kind appears in ItemKinds, then acks
 *     with TEvConfigNotificationResponse.
 *
 * IMPORTANT — this parametrizes the SUBSCRIBER side only, not the dispatcher.
 * Every kind in `registry` must already be SERVED by the ConfigsDispatcher,
 * i.e. it is a TAppConfig field number, a TConfigItem::EKind value, and is
 * present in the dispatcher's served-kinds set. Subscribing to a kind the
 * dispatcher does not serve aborts the dispatcher in
 * TConfigsDispatcher::CheckKinds (configs_dispatcher.cpp). To make the served
 * set itself configurable, the dispatcher must be parametrized separately
 * (e.g. an ExtraServedKinds field on TConfigsDispatcherInitInfo).
 */
NActors::IActor* CreateConfigHandlerHub(TConfigHandlerRegistry registry);

} // namespace NKikimr::NConfigHub
