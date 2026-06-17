#pragma once

#include <ydb/library/fyamlcpp/fyamlcpp.h>
#include <ydb/library/actors/core/actor.h>
#include <library/cpp/protobuf/json/json2proto.h>

#include <ydb/core/protos/config.pb.h>
#include <ydb/core/protos/console_config.pb.h>
#include <ydb/library/yaml_config/public/yaml_config.h>
#include <ydb/public/api/protos/ydb_status_codes.pb.h>
#include <yql/essentials/public/issue/yql_issue.h>
#include <ydb/public/api/protos/ydb_config.pb.h>

#include <openssl/sha.h>

#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/set.h>
#include <util/generic/map.h>
#include <util/stream/str.h>

#include <unordered_map>
#include <map>
#include <optional>
#include <string>

namespace NKikimr::NYamlConfig {

struct TBasicUnknownFieldsCollector : public NProtobufJson::IUnknownFieldsCollector {
    explicit TBasicUnknownFieldsCollector(TString rootPrefix = {})
        : RootPrefix(std::move(rootPrefix))
    {}

    void OnEnterMapItem(const TString& key) override {
        CurrentPath.push_back(key);
    }

    void OnEnterArrayItem(ui64 id) override {
        CurrentPath.push_back(ToString(id));
    }

    void OnLeaveMapItem() override {
        CurrentPath.pop_back();
    }

    void OnLeaveArrayItem() override {
        CurrentPath.pop_back();
    }

    void OnUnknownField(const TString& key, const google::protobuf::Descriptor& value) override {
        UnknownKeys[BuildPath(key)] = {key, value.full_name()};
    }

    const TMap<TString, std::pair<TString, TString>>& GetUnknownKeys() const {
        return UnknownKeys;
    }

    TString GetCurrentPath() const {
        return BuildPath();
    }

private:
    TString BuildPath(TStringBuf leaf = {}) const {
        TString path;
        if (RootPrefix) {
            path.append("/");
            path.append(RootPrefix);
        }
        for (const auto& piece : CurrentPath) {
            path.append("/");
            path.append(piece);
        }
        if (leaf) {
            path.append("/");
            path.append(leaf);
        }
        return path;
    }

private:
    TString RootPrefix;
    TVector<TString> CurrentPath;
    TMap<TString, std::pair<TString, TString>> UnknownKeys;
};

/**
 * Converts YAML representation to ProtoBuf
 */
NKikimrConfig::TAppConfig YamlToProto(
    const NFyaml::TNodeRef& node,
    bool allowUnknown = false,
    bool preTransform = true,
    TSimpleSharedPtr<NProtobufJson::IUnknownFieldsCollector> unknownFieldsCollector = nullptr);

// ---------------------------------------------------------------------------
// Track A+ : structural (proto-transform) validation, K-independent.
//
// The legacy accept gate runs YamlToProto(transform=true) on every distinct
// resolved doc (cost multiplicative in label cardinalities). The A+ variant
// decomposes the work:
//   (A/C) run the proto transform on each DISTINCT projection of the
//         transform-read sections (regime A = static-only -> 1 projection;
//         regime C = selector-varied section -> #distinct values);
//   (B)   validate per-field enum/type coercion over the A+ value-sets, so a
//         bad value in any tenant variant is caught without resolving K docs.
// The static-section guard rejects (or flags) configs whose selectors override
// a structural/static section, which is the precondition for (A).
// ---------------------------------------------------------------------------

struct TStructuralViolation {
    TString Message;
};

/**
 * Config-relative section prefixes the proto transform reads and selectors may
 * vary (regime C). DECLARED list, validated by the shadow-run; intended to be
 * derived from proto field markers by the config protoc plugin.
 */
const TVector<TString>& TransformReadDynamicSections();

/**
 * Config-relative static/structural section prefixes selectors must not vary
 * (the guard set). DECLARED list, to be marker-derived.
 */
const TVector<TString>& StaticGuardSections();

/**
 * K-independent structural validation over the whole label space.
 * `guardViolations`, if non-null, receives selector-written static paths (the
 * accept-set-narrowing guard). Returns every structural/coercion violation
 * found (empty => structurally valid for every resolved config, modulo guard).
 */
TVector<TStructuralViolation> ValidateStructuralAPlus(
    NFyaml::TDocument& doc,
    bool allowUnknown = true,
    TVector<TString>* guardViolations = nullptr);

/**
 * Legacy oracle: run YamlToProto(transform=true) on every distinct resolved
 * doc; returns the first structural error encountered, or nullopt. Used by the
 * shadow-run comparison and the equivalence tests.
 */
std::optional<TString> ValidateStructuralLegacy(
    NFyaml::TDocument& doc,
    bool allowUnknown = true);

/**
 * K-independent SEMANTIC validation: runs the REAL NConfig::ValidateConfig
 * (Auth/ColumnShard/Monitoring min-max rules + StateStorage aggregates) on each
 * DISTINCT per-section projection. Because it invokes the exact legacy validator
 * per projection, its decision equals the legacy per-doc semantic gate
 * (strictly not weaker), and the cost is additive across sections (K-independent)
 * -- the same per-section decomposition used for the structural pass.
 *
 * Aggregate/whole-structure checks (StateStorage) read only static sections
 * (domains_config, self_management_config) that selectors are FORBIDDEN to vary
 * (SelectorStatic guard), so they are validated once on the constant base in
 * every projection -- which is sound precisely because those sections cannot be
 * selector-varied.
 */
TVector<TStructuralViolation> ValidateSemanticAPlus(
    NFyaml::TDocument& doc, bool allowUnknown = true);

/**
 * Legacy oracle: run YamlToProto + NConfig::ValidateConfig on every distinct
 * resolved doc; returns the first semantic error, or nullopt.
 */
std::optional<TString> ValidateSemanticLegacy(
    NFyaml::TDocument& doc, bool allowUnknown = true);

/**
 * K-independent database-config allowlist. Rejects any top-level config section
 * that appears ANYWHERE in the database config (base OR a selector overlay) and
 * is not marked (NMarkers.AllowInDatabaseConfig). Mirrors the per-field check
 * NConfig::ValidateDatabaseConfig, but by inspecting the whole config space it
 * ALSO covers fields a database-config selector introduces -- which the legacy
 * base-only check misses (the TODO in TConfigsManager::ValidateDatabaseConfig).
 * 'doc' is the database config document (its `config` subtree + selectors).
 */
TVector<TStructuralViolation> ValidateDatabaseAllowlistAPlus(NFyaml::TDocument& doc);

/**
 * Unknown / deprecated field diagnostics, collected K-independently. Paths are
 * the editable YAML locations: "/config/..." for the base config and
 * "/selector_config/<i>/config/..." for the i-th selector overlay. Each entry
 * maps that path to {leaf key, declaring proto message full_name}.
 *
 * Deprecated = the field's content-relative path is in
 * TAppConfig::GetReservedChildrenPaths(); everything else unknown.
 */
struct TFieldDiagnostics {
    TMap<TString, std::pair<TString, TString>> UnknownFields;
    TMap<TString, std::pair<TString, TString>> DeprecatedFields;

    bool Empty() const { return UnknownFields.empty() && DeprecatedFields.empty(); }

    // One human-readable warning line per field, for surfacing to the user.
    TVector<TString> ToWarnings() const;
};

/**
 * Collects unknown and deprecated fields over the UNRESOLVED document, per
 * editable block (base config + each selector overlay). This is union-equivalent
 * to collecting across every resolved doc -- unknown-field-ness is a per-path
 * property under replace-only merge -- but is K-independent (no enumeration).
 * Best-effort: never throws.
 */
TFieldDiagnostics CollectFieldDiagnosticsAPlus(NFyaml::TDocument& doc, bool allowUnknown = true);

struct TStructuralShadowResult {
    bool LegacyRejected = false;
    bool APlusRejected = false;
    bool Diverged = false;                 // LegacyRejected != APlusRejected
    TString LegacyError;                   // first legacy error, if rejected
    TVector<TStructuralViolation> APlusViolations;
    TVector<TString> GuardViolations;      // selector-written static paths

    // Semantic (NConfig::ValidateConfig) decisions.
    bool SemanticLegacyRejected = false;
    bool SemanticAPlusRejected = false;
    bool SemanticDiverged = false;         // SemanticLegacyRejected != SemanticAPlusRejected
    TString SemanticLegacyError;
};

/**
 * Shadow-run: runs BOTH the legacy per-doc structural validation and the
 * K-independent A+ structural validation, comparing their accept/reject
 * decisions. Intended to run on every accept during the migration window; the
 * legacy decision still gates acceptance while divergences are logged, until
 * zero divergence is observed fleet-wide and the A+ path can take over.
 */
TStructuralShadowResult StructuralShadowRun(
    NFyaml::TDocument& doc,
    bool allowUnknown = true);

/**
 * Resolves config for given labels and stores result to appConfig
 * Stores intermediate resolve data in resolvedYamlConfig and resolvedJsonConfig if given
 */
void ResolveAndParseYamlConfig(
    const TString& mainYamlConfig,
    const TMap<ui64, TString>& volatileYamlConfigs,
    const TMap<TString, TString>& labels,
    NKikimrConfig::TAppConfig& appConfig,
    std::optional<TString> databaseYamlConfig = std::nullopt,
    TString* resolvedYamlConfig = nullptr,
    TString* resolvedJsonConfig = nullptr,
    TSimpleSharedPtr<NProtobufJson::IUnknownFieldsCollector> unknownFieldsCollector = nullptr);

enum class EValidationResult {
    Ok,
    Warn,
    Error,
};

class IConfigValidator {
public:
    virtual ~IConfigValidator() = default;

    virtual EValidationResult ValidateConfig(
        const NKikimrConfig::TAppConfig& config,
        std::vector<TString>& msg) const = 0;
};

/**
 * Replaces kinds not managed by yaml config (e.g. NetClassifierConfig) from config 'from' in config 'to'
 * if corresponding configs are presenet in 'from'
 */
void ReplaceUnmanagedKinds(const NKikimrConfig::TAppConfig& from, NKikimrConfig::TAppConfig& to);

using TValidatorsMap = TMap<TString, TSimpleSharedPtr<IConfigValidator>>;

class IConfigSwissKnife {
public:
    virtual ~IConfigSwissKnife() = default;
    virtual bool VerifyReplaceRequest(const Ydb::Config::ReplaceConfigRequest& request, Ydb::StatusIds::StatusCode& status, NYql::TIssues& issues) const = 0;
    virtual bool VerifyMainConfig(const TString& config) const = 0;
    virtual bool VerifyStorageConfig(const TString& config) const = 0;
    virtual EValidationResult ValidateConfig(
        const NKikimrConfig::TAppConfig& config,
        std::vector<TString>& msg) const;

    const TMap<TString, TSimpleSharedPtr<IConfigValidator>>& GetValidators() const {
        return Validators;
    }
protected:
    TMap<TString, TSimpleSharedPtr<IConfigValidator>> Validators;
};


std::unique_ptr<IConfigSwissKnife> CreateDefaultConfigSwissKnife();

} // namespace NKikimr::NYamlConfig
