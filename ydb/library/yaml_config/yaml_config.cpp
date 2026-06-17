#include "yaml_config.h"

#include "yaml_config_parser.h"

#include <ydb/core/base/appdata.h>

#include <library/cpp/protobuf/json/json2proto.h>
#include <library/cpp/protobuf/json/util.h>

#include <util/string/builder.h>
#include <util/string/cast.h>

#include <ydb/core/config/protos/marker.pb.h>
#include <ydb/core/protos/netclassifier.pb.h>
#include <ydb/core/config/validation/validators.h>

namespace NKikimr::NYamlConfig {

NKikimrConfig::TAppConfig YamlToProto(
    const NFyaml::TNodeRef& node,
    bool allowUnknown,
    bool preTransform,
    TSimpleSharedPtr<NProtobufJson::IUnknownFieldsCollector> unknownFieldsCollector)
{
    TStringStream sstr;

    sstr << NFyaml::TJsonEmitter(node);

    TString resolvedJsonConfig = sstr.Str();

    NJson::TJsonValue json;

    NJson::ReadJsonTree(resolvedJsonConfig, &json);

    NKikimrConfig::TAppConfig yamlProtoConfig;
    NYaml::Parse(json, NYaml::GetJsonToProtoConfig(allowUnknown, std::move(unknownFieldsCollector)), yamlProtoConfig, preTransform, /*phase=*/ nullptr, /*relaxed=*/ true);

    return yamlProtoConfig;
}

void ResolveAndParseYamlConfig(
    const TString& mainYamlConfig,
    const TMap<ui64, TString>& volatileYamlConfigs,
    const TMap<TString, TString>& labels,
    NKikimrConfig::TAppConfig& appConfig,
    std::optional<TString> databaseYamlConfig,
    TString* resolvedYamlConfig,
    TString* resolvedJsonConfig,
    TSimpleSharedPtr<NProtobufJson::IUnknownFieldsCollector> unknownFieldsCollector)
{
    TStringStream resolvedJsonConfigStream;
    bool hasMetadata = false;
    if (mainYamlConfig) {
        auto tree = NFyaml::TDocument::Parse(mainYamlConfig);

        if (tree.Root().Map().Has("metadata")) {
            hasMetadata = true;
        }

        if (databaseYamlConfig) {
            auto d = NFyaml::TDocument::Parse(*databaseYamlConfig);
            NYamlConfig::AppendDatabaseConfig(tree, d);
        }

        for (auto& [_, config] : volatileYamlConfigs) {
            auto d = NFyaml::TDocument::Parse(config);
            NYamlConfig::AppendVolatileConfigs(tree, d);
        }

        TSet<NYamlConfig::TNamedLabel> namedLabels;
        for (auto& [name, label] : labels) {
            namedLabels.insert(NYamlConfig::TNamedLabel{name, label});
        }

        auto config = NYamlConfig::Resolve(tree, namedLabels);

        if (resolvedYamlConfig) {
            TStringStream resolvedYamlConfigStream;
            resolvedYamlConfigStream << config.second;
            *resolvedYamlConfig = resolvedYamlConfigStream.Str();
        }

        resolvedJsonConfigStream << NFyaml::TJsonEmitter(config.second);

        if (resolvedJsonConfig) {
            *resolvedJsonConfig = resolvedJsonConfigStream.Str();
        }
    } else {
        resolvedJsonConfigStream << "{}";
    }

    NJson::TJsonValue json;
    Y_ABORT_UNLESS(NJson::ReadJsonTree(resolvedJsonConfigStream.Str(), &json), "Got invalid config from Console");

    if (hasMetadata) {
        appConfig.SetYamlConfigEnabled(true);
    }

    NYaml::Parse(json, NYaml::GetJsonToProtoConfig(true, std::move(unknownFieldsCollector)), appConfig, true, /*phase=*/ nullptr, /*relaxed=*/ true);
}

void ReplaceUnmanagedKinds(const NKikimrConfig::TAppConfig& from, NKikimrConfig::TAppConfig& to) {
    if (from.HasNameserviceConfig()) {
        to.MutableNameserviceConfig()->CopyFrom(from.GetNameserviceConfig());
    }

    if (from.HasNetClassifierDistributableConfig()) {
        to.MutableNetClassifierDistributableConfig()->CopyFrom(from.GetNetClassifierDistributableConfig());
    }

    if (from.NamedConfigsSize()) {
        to.MutableNamedConfigs()->CopyFrom(from.GetNamedConfigs());
    }
}

class TLegacyValidators
    : public IConfigValidator
{
public:
    EValidationResult ValidateConfig(
        const NKikimrConfig::TAppConfig& config,
        std::vector<TString>& msg) const override
    {
        auto res = NKikimr::NConfig::ValidateConfig(config, msg);
        switch (res) {
            case NKikimr::NConfig::EValidationResult::Ok:
                return EValidationResult::Ok;
            case NKikimr::NConfig::EValidationResult::Warn:
                return EValidationResult::Warn;
            case NKikimr::NConfig::EValidationResult::Error:
                return EValidationResult::Error;
        }
    }
};

class TDefaultConfigSwissKnife : public IConfigSwissKnife {
public:
    TDefaultConfigSwissKnife() {
        Validators["LegacyValidators"] = MakeSimpleShared<TLegacyValidators>();
    }

    bool VerifyReplaceRequest(const Ydb::Config::ReplaceConfigRequest&, Ydb::StatusIds::StatusCode&, NYql::TIssues&) const override {
        return true;
    }

    bool VerifyMainConfig(const TString&) const override {
        return true;
    };

    bool VerifyStorageConfig(const TString&) const override {
        return true;
    }
};


std::unique_ptr<IConfigSwissKnife> CreateDefaultConfigSwissKnife() {
    return std::make_unique<TDefaultConfigSwissKnife>();
}

EValidationResult IConfigSwissKnife::ValidateConfig(
    const NKikimrConfig::TAppConfig& config,
    std::vector<TString>& msg) const
{
    for (const auto& [name, validator] : GetValidators()) {
        EValidationResult result = validator->ValidateConfig(config, msg);
        if (result == EValidationResult::Error) {
            return EValidationResult::Error;
        }
    }

    if (msg.size() > 0) {
        return EValidationResult::Warn;
    }

    return EValidationResult::Ok;
}

// ---------------------------------------------------------------------------
// Track A+ : structural (proto-transform) validation.
// ---------------------------------------------------------------------------

namespace {

TVector<TString> SplitConfigPath(const TString& path) {
    TVector<TString> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        TString key = (slash == TString::npos) ? path.substr(start) : path.substr(start, slash - start);
        if (!key.empty()) {
            parts.push_back(key);
        }
        if (slash == TString::npos) {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

// Top-level config section ("/<first component>") of a config-relative path.
TString TopSection(const TString& path) {
    auto parts = SplitConfigPath(path);
    return parts.empty() ? TString("/") : TString("/") + parts[0];
}

// Top-level section keys ("/<key>") that PRESENT anywhere in the config space:
// the keys of the base `config` mapping and of every `selector_config/<i>/config`
// overlay mapping. Unlike ResolveFieldValueSets (which derives sections from
// scalar leaves / sequences), this is PRESENCE-based and therefore also reports
// a section written as an EMPTY mapping (e.g. `auth_config: {}`), which carries
// no leaf. Best-effort: tolerates malformed documents.
// Ephemeral structural input keys (snake_case, no leading slash): top-level YAML
// keys the transform moves into the proto and that are NOT TAppConfig fields
// (hosts, host_configs, security_config, ...). With preTransform=false these are
// seen by MergeJson2Proto as "unknown" top-level fields, so unknown-field
// collection done without the transform must filter them out.
const TSet<TString>& EphemeralInputKeys() {
    static const TSet<TString> keys = [] {
        TSet<TString> v;
        const auto* eph = NKikimrConfig::TEphemeralInputFields::descriptor();
        for (int i = 0; i < eph->field_count(); ++i) {
            TString name = eph->field(i)->name();
            NProtobufJson::ToSnakeCaseDense(&name);
            v.insert(name);
        }
        return v;
    }();
    return keys;
}

TSet<TString> CollectPresentTopSections(NFyaml::TDocument& doc) {
    TSet<TString> sections;
    auto addKeys = [&](NFyaml::TNodeRef node) {
        if (!node || node.Type() != NFyaml::ENodeType::Mapping) {
            return;
        }
        auto map = node.Map();
        for (auto it = map.begin(); it != map.end(); ++it) {
            sections.insert(TString("/") + it->Key().Scalar());
        }
    };
    try {
        auto root = doc.Root().Map();
        if (root.Has("config")) {
            addKeys(root.at("config"));
        }
        if (root.Has("selector_config")) {
            auto selectors = root.at("selector_config").Sequence();
            for (size_t i = 0; i < selectors.size(); ++i) {
                auto item = selectors.at(static_cast<int>(i)).Map();
                if (item.Has("config")) {
                    addKeys(item.at("config"));
                }
            }
        }
    } catch (const std::exception&) {
        // Best-effort; a malformed doc is rejected elsewhere by the real parser.
    }
    return sections;
}

} // namespace

const TVector<TString>& TransformReadDynamicSections() {
    // Sections TransformProtoConfig reads AND selectors may vary (regime C).
    // DERIVED from the (NMarkers.SelectorTransformRead) proto markers via the
    // config protoc plugin -- the list lives next to the fields and is a
    // provable cover of the marked transform-read sections (closes the
    // hand-declared-list completeness gap).
    static const TVector<TString> sections = [] {
        TVector<TString> v;
        for (const auto& p : NKikimrConfig::TAppConfig::GetTransformReadSectionPaths()) {
            v.push_back(p);
        }
        return v;
    }();
    return sections;
}

const TVector<TString>& StaticGuardSections() {
    // Structural/static sections selectors must not vary (regime A guard).
    // DERIVED from the (NMarkers.SelectorStatic) proto markers, plus the
    // non-proto ephemeral structural input keys (top-level YAML that the
    // transform moves into the proto -- not TAppConfig fields).
    static const TVector<TString> sections = [] {
        TVector<TString> v;
        for (const auto& p : NKikimrConfig::TAppConfig::GetStaticSectionPaths()) {
            v.push_back(p);
        }
        // Ephemeral structural input keys (top-level YAML the transform moves
        // into the proto; not TAppConfig fields, so they carry no marker).
        // Derived from the TEphemeralInputFields descriptor so the guard stays
        // complete as fields are added (e.g. hosts, host_configs, system_tablets,
        // static_erasure, erasure, default_disk_type, storage_pool_types,
        // security_config, domain_name).
        const auto* eph = NKikimrConfig::TEphemeralInputFields::descriptor();
        for (int i = 0; i < eph->field_count(); ++i) {
            TString name = eph->field(i)->name();
            NProtobufJson::ToSnakeCaseDense(&name);
            v.push_back(TString("/") + name);
        }
        return v;
    }();
    return sections;
}

TVector<TStructuralViolation> ValidateStructuralAPlus(
    NFyaml::TDocument& doc, bool allowUnknown, TVector<TString>* guardViolations)
{
    TVector<TStructuralViolation> out;

    // Static-section guard.
    if (guardViolations) {
        *guardViolations = SelectorWritesUnder(doc, StaticGuardSections());
    }

    // Run the FULL proto pipeline (JsonToProto coercion + TransformProtoConfig)
    // on the DISTINCT realized projections of EACH top-level section,
    // INDEPENDENTLY -- the source of polynomiality. The cost is
    //   sum over sections of (#distinct values of that section),
    // which is ADDITIVE across sections and therefore independent of K (the
    // multiplicative number of fully-resolved configs). Projecting sections
    // jointly would re-introduce the very cliff the redesign removes (two
    // sections each varied by a different high-cardinality label -> |A|*|B|).
    //
    // Per-section is sound because:
    //  - JsonToProto coercion is per-field independent (a field's coercion
    //    depends only on its own value), so per-section == full-doc coercion;
    //  - each TransformProtoConfig Prepare* step reads a single dynamic section
    //    (actor_system/log/interconnect/grpc/auth) plus the constant static
    //    sections, so no transform check couples two selector-varied sections.
    //    (If a future Prepare* couples two transform-read sections, those must
    //    be grouped into one projection; the shadow-run would flag the drift.)
    //
    // Using the SAME converter as the legacy per-doc path, the per-section
    // decision equals legacy's for every field type and structural check -- so
    // A+ is strictly not weaker (never accepts what legacy rejects).
    TSet<TString> sections;
    {
        auto sets = ResolveFieldValueSets(doc);
        for (const auto& [p, _] : sets.Values) { sections.insert(TopSection(p)); }
        for (const auto& p : sets.FencedPaths) { sections.insert(TopSection(p)); }
    }
    for (const auto& section : sections) {
        EnumerateDistinctProjections(doc, TVector<TString>{section},
            [&](NFyaml::TNodeRef configNode) {
                try {
                    YamlToProto(configNode, allowUnknown, /*preTransform*/ true);
                } catch (const std::exception& e) {
                    out.push_back(TStructuralViolation{e.what()});
                }
            });
    }

    return out;
}

std::optional<TString> ValidateStructuralLegacy(NFyaml::TDocument& doc, bool allowUnknown) {
    std::optional<TString> err;
    ResolveUniqueDocs(doc, [&](TDocumentConfig&& cfg) {
        if (err) {
            return;
        }
        try {
            YamlToProto(cfg.second, allowUnknown, /*preTransform*/ true);
        } catch (const std::exception& e) {
            err = TString(e.what());
        }
    });
    return err;
}

TVector<TStructuralViolation> ValidateSemanticAPlus(NFyaml::TDocument& doc, bool allowUnknown) {
    TVector<TStructuralViolation> out;

    // Static-section guard -- MUST run on the semantic path too, not only the
    // structural one. Several semantic checks couple a selector-VARIABLE section
    // with a SelectorStatic one read from the SAME resolved config:
    //   - ValidateMonitoringConfig reads monitoring_config (variable) AND
    //     domains_config.security_config.enforce_user_token_requirement (static);
    //   - ValidateStateStorageConfig reads domains_config + self_management_config.
    // Per-section projection validates monitoring_config with domains_config frozen
    // at the constant base. That is sound ONLY because selectors are forbidden to
    // vary the static section: the base IS its single realizable value. If a
    // selector DID vary the static section, the legacy per-doc gate would evaluate
    // the joint (variable, varied-static) pair that no single-section projection
    // reproduces -- so without this guard the semantic path would be strictly
    // weaker than legacy for that shape. Enforcing the guard here (rather than
    // relying on the structural pass running alongside) makes ValidateSemanticAPlus
    // self-sufficiently not-weaker. Over-rejects an unrealizable static variation;
    // never accepts what legacy rejects.
    for (const auto& path : SelectorWritesUnder(doc, StaticGuardSections())) {
        out.push_back(TStructuralViolation{
            TStringBuilder() << "selector varies static section '" << path
                             << "' which a semantic check reads jointly with a variable section"});
    }

    // Per-section projection running the REAL NConfig::ValidateConfig -- additive
    // (K-independent), exact (same validator as legacy). A check coupling a
    // dynamic section with a static one (e.g. Monitoring reading monitoring_config
    // + domains_config.security_config) is correct because the static section's
    // value is the constant base in every projection (the guard above rejects any
    // selector that varies it). Aggregate checks reading only static sections
    // (StateStorage) run on that constant base.
    TSet<TString> sections;
    {
        auto sets = ResolveFieldValueSets(doc);
        for (const auto& [p, _] : sets.Values) { sections.insert(TopSection(p)); }
        for (const auto& p : sets.FencedPaths) { sections.insert(TopSection(p)); }
    }
    for (const auto& section : sections) {
        EnumerateDistinctProjections(doc, TVector<TString>{section},
            [&](NFyaml::TNodeRef configNode) {
                try {
                    auto cfg = YamlToProto(configNode, allowUnknown, /*preTransform*/ true);
                    std::vector<TString> errors;
                    if (NKikimr::NConfig::ValidateConfig(cfg, errors) == NKikimr::NConfig::EValidationResult::Error) {
                        out.push_back(TStructuralViolation{
                            errors.empty() ? TString("semantic validation error") : errors.front()});
                    }
                } catch (const std::exception& e) {
                    out.push_back(TStructuralViolation{e.what()});
                }
            });
    }
    return out;
}

std::optional<TString> ValidateSemanticLegacy(NFyaml::TDocument& doc, bool allowUnknown) {
    std::optional<TString> err;
    ResolveUniqueDocs(doc, [&](TDocumentConfig&& cfg) {
        if (err) {
            return;
        }
        try {
            auto proto = YamlToProto(cfg.second, allowUnknown, /*preTransform*/ true);
            std::vector<TString> errors;
            if (NKikimr::NConfig::ValidateConfig(proto, errors) == NKikimr::NConfig::EValidationResult::Error) {
                err = errors.empty() ? TString("semantic validation error") : errors.front();
            }
        } catch (const std::exception& e) {
            err = TString(e.what());
        }
    });
    return err;
}

TVector<TStructuralViolation> ValidateDatabaseAllowlistAPlus(NFyaml::TDocument& doc) {
    TVector<TStructuralViolation> out;

    // Every top-level config section that appears anywhere in the DB config
    // space (base + any selector overlay). We union TWO views:
    //  - leaf/sequence-derived sections (ResolveFieldValueSets), and
    //  - presence-derived section keys (CollectPresentTopSections).
    // The second view is essential here: the legacy DB gate rejects a disallowed
    // section even when it is written as an EMPTY mapping (e.g. `auth_config: {}`),
    // which has no scalar leaf and so is invisible to ResolveFieldValueSets. The
    // allowlist is a PRESENCE check, so an empty-but-present section must count.
    TSet<TString> sections;
    {
        auto sets = ResolveFieldValueSets(doc);
        for (const auto& [p, _] : sets.Values) { sections.insert(TopSection(p)); }
        for (const auto& p : sets.FencedPaths) { sections.insert(TopSection(p)); }
    }
    for (const auto& s : CollectPresentTopSections(doc)) {
        sections.insert(s);
    }

    // Allowed top-level sections = TAppConfig fields marked AllowInDatabaseConfig.
    const auto* desc = NKikimrConfig::TAppConfig::descriptor();
    TSet<TString> allowed;
    for (int i = 0; i < desc->field_count(); ++i) {
        const auto* f = desc->field(i);
        if (f->options().GetExtension(NKikimrConfig::NMarkers::AllowInDatabaseConfig)) {
            TString name = f->name();
            NProtobufJson::ToSnakeCaseDense(&name);
            allowed.insert(TString("/") + name);
        }
    }

    for (const auto& section : sections) {
        if (!allowed.contains(section)) {
            const TString fieldName = section.StartsWith("/") ? section.substr(1) : section;
            out.push_back(TStructuralViolation{
                TStringBuilder() << "'" << fieldName << "' is not allowed to be used in the database configuration"});
        }
    }
    return out;
}

TVector<TString> TFieldDiagnostics::ToWarnings() const {
    TVector<TString> out;
    out.reserve(UnknownFields.size() + DeprecatedFields.size());
    for (const auto& [path, info] : DeprecatedFields) {
        out.push_back(TStringBuilder() << "deprecated field '" << path << "'");
    }
    for (const auto& [path, info] : UnknownFields) {
        out.push_back(TStringBuilder() << "unknown field '" << path << "'");
    }
    return out;
}

TFieldDiagnostics CollectFieldDiagnosticsAPlus(NFyaml::TDocument& doc, bool allowUnknown) {
    TFieldDiagnostics result;
    const auto& deprecatedPaths = NKikimrConfig::TAppConfig::GetReservedChildrenPaths();

    auto record = [&](const TBasicUnknownFieldsCollector& collector, const TString& prefix,
                      bool dropEphemeralTop) {
        for (const auto& [path, info] : collector.GetUnknownKeys()) {
            // Reserved (deprecated) paths are config-content-relative; strip the
            // location prefix ("/<prefix>") before matching.
            const TString leafPath = path.substr(prefix.size() + 1);
            if (dropEphemeralTop) {
                // The transform was skipped, so ephemeral top-level keys surface
                // here as false "unknown" fields -- drop them.
                auto parts = SplitConfigPath(leafPath);
                if (!parts.empty() && EphemeralInputKeys().contains(parts[0])) {
                    continue;
                }
            }
            if (deprecatedPaths.contains(leafPath)) {
                result.DeprecatedFields[path] = info;
            } else {
                result.UnknownFields[path] = info;
            }
        }
    };

    auto collectBlock = [&](const NFyaml::TNodeRef& configNode, const TString& prefix) {
        auto collector = MakeSimpleShared<TBasicUnknownFieldsCollector>(prefix);
        try {
            YamlToProto(configNode, allowUnknown, /*preTransform*/ true, collector);
            record(*collector, prefix, /*dropEphemeralTop*/ false);
        } catch (const std::exception&) {
            // A partial fragment (e.g. a selector overlay, or a config carrying a
            // partial static/aggregate section) can throw in the preTransform
            // Preprocess BEFORE MergeJson2Proto ever fires OnUnknownField, leaving
            // the block's unknown fields entirely uncollected. Retry WITHOUT the
            // transform so MergeJson2Proto runs directly and reports unknown
            // fields, filtering out ephemeral top-level keys the transform would
            // otherwise have consumed. Recovers diagnostics parity with the legacy
            // per-resolved-doc collection (which sees complete, transformable docs).
            auto fallback = MakeSimpleShared<TBasicUnknownFieldsCollector>(prefix);
            try {
                YamlToProto(configNode, allowUnknown, /*preTransform*/ false, fallback);
            } catch (const std::exception&) {
                // Even untransformed parse failed; keep whatever was gathered.
            }
            record(*fallback, prefix, /*dropEphemeralTop*/ true);
        }
    };

    try {
        auto root = doc.Root().Map();
        if (root.Has("config")) {
            collectBlock(root.at("config"), "config");
        }
        if (root.Has("selector_config")) {
            auto selectors = root.at("selector_config").Sequence();
            for (size_t i = 0; i < selectors.size(); ++i) {
                auto item = selectors.at(static_cast<int>(i)).Map();
                if (item.Has("config")) {
                    collectBlock(item.at("config"),
                        TStringBuilder() << "selector_config/" << i << "/config");
                }
            }
        }
    } catch (const std::exception&) {
        // Best-effort; field diagnostics never block acceptance.
    }

    return result;
}

TStructuralShadowResult StructuralShadowRun(NFyaml::TDocument& doc, bool allowUnknown) {
    TStructuralShadowResult result;

    auto legacyErr = ValidateStructuralLegacy(doc, allowUnknown);
    result.LegacyRejected = legacyErr.has_value();
    if (legacyErr) {
        result.LegacyError = *legacyErr;
    }

    result.APlusViolations = ValidateStructuralAPlus(doc, allowUnknown, &result.GuardViolations);
    result.APlusRejected = !result.APlusViolations.empty() || !result.GuardViolations.empty();

    result.Diverged = (result.LegacyRejected != result.APlusRejected);

    // Semantic (NConfig::ValidateConfig) shadow comparison.
    auto semLegacy = ValidateSemanticLegacy(doc, allowUnknown);
    result.SemanticLegacyRejected = semLegacy.has_value();
    if (semLegacy) {
        result.SemanticLegacyError = *semLegacy;
    }
    result.SemanticAPlusRejected = !ValidateSemanticAPlus(doc, allowUnknown).empty();
    result.SemanticDiverged = (result.SemanticLegacyRejected != result.SemanticAPlusRejected);

    return result;
}

} // namespace NKikimr::NYamlConfig
