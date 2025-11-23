#pragma once

#include "defs.h"

#include <ydb/core/protos/config.pb.h>

#include <util/generic/string.h>
#include <util/generic/map.h>

namespace NKikimr::NConsole {

/**
 * Configuration source type enumeration.
 * Indicates how the system was initialized.
 */
enum class EConfigSource {
    SeedNodes,      // Initialized from seed nodes (uses ConfigClient, has storage YAML)
    DynamicConfig,  // Initialized from dynamic config (uses DynConfigClient)
    Unknown         // Unable to determine (fallback to DynamicConfig behavior)
};

/**
 * State snapshot of ConfigsDispatcher.
 * Used for observability, monitoring, and testing.
 */
struct TConfigsDispatcherState {
    EConfigSource ConfigSource = EConfigSource::Unknown;
    TString ConfigSourceLabel;
    TString ConfigurationVersion;
    bool HasStorageYaml = false;
    size_t StorageYamlSize = 0;
    bool YamlConfigEnabled = false;
    size_t SubscriptionsCount = 0;
    
    // Replay path tracking - indicates which path was used in last config update
    bool LastReplayUsedSeedNodesPath = false;
    bool LastReplayUsedDynamicConfigPath = false;
    
    TMap<TString, TString> Labels;
    
    TString ToDebugString() const {
        TStringStream ss;
        ss << "ConfigSource: " << ConfigSourceLabel;
        switch (ConfigSource) {
            case EConfigSource::SeedNodes:
                ss << " (seed nodes - uses ConfigClient)";
                break;
            case EConfigSource::DynamicConfig:
                ss << " (dynamic config - uses DynConfigClient)";
                break;
            case EConfigSource::Unknown:
                ss << " (unknown)";
                break;
        }
        ss << "\nConfigurationVersion: " << ConfigurationVersion;
        ss << "\nHasStorageYaml: " << (HasStorageYaml ? "yes" : "no");
        if (HasStorageYaml) {
            ss << " (" << StorageYamlSize << " bytes)";
        }
        ss << "\nYamlConfigEnabled: " << YamlConfigEnabled;
        ss << "\nSubscriptionsCount: " << SubscriptionsCount;
        return ss.Str();
    }
};

namespace TEvConfigsDispatcher {
    enum EEvObserver {
        // Add after existing events
        EvGetStateRequest = EvEnd,
        EvGetStateResponse,
        EvGetStorageYamlRequest,
        EvGetStorageYamlResponse,
        
        EvObserverEnd
    };
    
    static_assert(EvObserverEnd < EventSpaceEnd(TKikimrEvents::ES_CONFIGS_DISPATCHER),
                  "expect EvObserverEnd < EventSpaceEnd(TKikimrEvents::ES_CONFIGS_DISPATCHER)");
    
    /**
     * Request current state of ConfigsDispatcher.
     * Response: TEvGetStateResponse
     */
    struct TEvGetStateRequest : public TEventLocal<TEvGetStateRequest, EvGetStateRequest> {
        TEvGetStateRequest() = default;
    };
    
    /**
     * Response containing current state snapshot.
     */
    struct TEvGetStateResponse : public TEventLocal<TEvGetStateResponse, EvGetStateResponse> {
        TConfigsDispatcherState State;
        
        TEvGetStateResponse(TConfigsDispatcherState state)
            : State(std::move(state))
        {}
    };
    
    /**
     * Request storage YAML config (if available).
     * Only available if initialized from seed nodes.
     * Response: TEvGetStorageYamlResponse
     */
    struct TEvGetStorageYamlRequest : public TEventLocal<TEvGetStorageYamlRequest, EvGetStorageYamlRequest> {
        TEvGetStorageYamlRequest() = default;
    };
    
    /**
     * Response containing storage YAML config.
     * StorageYaml will be empty if not initialized from seed nodes.
     */
    struct TEvGetStorageYamlResponse : public TEventLocal<TEvGetStorageYamlResponse, EvGetStorageYamlResponse> {
        TString StorageYaml;
        
        TEvGetStorageYamlResponse(TString storageYaml)
            : StorageYaml(std::move(storageYaml))
        {}
    };
    
} // namespace TEvConfigsDispatcher

} // namespace NKikimr::NConsole

