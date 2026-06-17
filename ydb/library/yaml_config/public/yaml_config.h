#pragma once

#include <ydb/library/fyamlcpp/fyamlcpp.h>
#include <ydb/library/actors/core/actor.h>

#include <openssl/sha.h>

#include <functional>

#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/set.h>
#include <util/generic/map.h>
#include <util/generic/hash.h>
#include <util/stream/str.h>

#include <unordered_map>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace NKikimr::NYamlConfig {

struct TYamlConfigEx : public yexception {};

using TDocumentConfig = std::pair<NFyaml::TDocument, NFyaml::TNodeRef>;

/**
 * Open - labels like tenant, where we don't know final set of values
 * Closed - labels with predefined set of values, e.g. size
 */
enum class EYamlConfigLabelTypeClass {
    Open,
    Closed,
};

/**
 * TNamedLabel - representation of label used for selector
 */
class TNamedLabel {
public:
    TString Name;
    TString Value;
    bool Inv = false;

    bool operator<(const TNamedLabel& other) const { return Name < other.Name; }
};

/**
 * TLabelType - represents known set of values for a label with its type
 */
class TLabelType {
public:
    EYamlConfigLabelTypeClass Class;
    TSet<TString> Values;

    bool operator==(const TLabelType& other) const { return Values == other.Values; }
};

class TLabelValueSet {
public:
    TSet<TString> Values;

    bool operator==(const TLabelValueSet& other) const { return Values == other.Values; }
};

class TSelector {
public:
    TMap<TString, TLabelValueSet> In;
    TMap<TString, TLabelValueSet> NotIn;
};

/**
 * TLabel is a representation of label for config resolution
 *
 * It can be in three states:
 * - Empty with Type == EType::Empty and Value == ""
 *   it equals empty or undefined label
 * - Common with Type == EType::Common and Value == arbitrary string
 *   it equals defined label with corresponding value
 * - Negative with Type == EType::Negative and Value == ""
 *   it is used for Open labels and equals any label not present in labels
 *   discovered by CollectLabels (and also not equals Empty label)
 */
struct TLabel {
    enum class EType {
        Negative = 0,
        Empty,
        Common,
    };

    EType Type;
    TString Value;

    bool operator<(const TLabel& other) const {
        int lhs = static_cast<int>(Type);
        int rhs = static_cast<int>(other.Type);
        return std::tie(lhs, Value) < std::tie(rhs, other.Value);
    }

    bool operator==(const TLabel& other) const {
        int lhs = static_cast<int>(Type);
        int rhs = static_cast<int>(other.Type);
        return std::tie(lhs, Value) == std::tie(rhs, other.Value);
    }
};

struct TIncompatibilityRule {
    struct TLabelPattern {
        TString Name;
        std::variant<std::monostate, THashSet<TString>> Values;
        bool Negated = false;

        bool Matches(const TLabel& label, const TString& actualLabelName) const;
    };

    TVector<TLabelPattern> Patterns;
    TString RuleName;

    enum class ESource {
        BuiltIn,
        UserDefined
    };
    ESource Source = ESource::BuiltIn;
};

class TIncompatibilityRules {
public:
    static constexpr const char* UNSET_LABEL_MARKER = "$unset";
    static constexpr const char* EMPTY_LABEL_MARKER = "$empty";

    static TIncompatibilityRules GetDefaultRules();

    void AddRule(TIncompatibilityRule rule);
    void RemoveRule(const TString& ruleName);

    bool IsCompatible(const TVector<TLabel>& combination,
                     const TVector<std::pair<TString, TSet<TLabel>>>& labelNames) const;

    bool IsCompatible(const TMap<TString, TString>& labels) const;

    void MergeWith(const TIncompatibilityRules& userRules);

    size_t GetRuleCount() const { return RulesByName.size(); }
    size_t GetDisabledCount() const { return DisabledRules.size(); }

    bool HasRules() const { return !RulesByName.empty() || !DisabledRules.empty(); }

    // Every label name referenced by any active rule. Used by the A+
    // realizability filter to enumerate the labels an incompatibility rule
    // constrains alongside the rule-coupled labels, so pruning is exact.
    THashSet<TString> ReferencedLabels() const;

private:
    TMap<TString, TIncompatibilityRule> RulesByName;
    THashSet<TString> DisabledRules;

    friend TIncompatibilityRules ParseIncompatibilityRules(const NFyaml::TNodeRef& root);
};

struct TYamlConfigModel {
    struct TSelectorModel {
        TString Description;
        TSelector Selector;
        NFyaml::TNodeRef Config;
    };

    const NFyaml::TDocument& Doc;
    NFyaml::TNodeRef Config;
    TMap<TString, TLabelType> AllowedLabels;
    TVector<TSelectorModel> Selectors;
    TIncompatibilityRules IncompatibilityRules;
};

/**
 * Collects all labels present in document
 * For Open labels gathers all labels from all selectors
 * For Closed labels additionally validates that there is no additional labels
 */
TMap<TString, TLabelType> CollectLabels(NFyaml::TDocument& doc);

/**
 * Parses config and fills corresponding struct
 */
TYamlConfigModel ParseConfig(NFyaml::TDocument& doc);

/**
 * Generates config for specific set of labels applying all matching selectors
 */
TDocumentConfig Resolve(
    const NFyaml::TDocument& doc,
    const TSet<TNamedLabel>& labels);

struct TResolvedConfig {
    TVector<TString> Labels;
    TMap<TSet<TVector<TLabel>>, TDocumentConfig> Configs;
};

/**
 * Generates configs for all label combinations
 */
TResolvedConfig ResolveAll(NFyaml::TDocument& doc);

/**
 * Generates unique resolved documents without materializing label combinations
 */
void ResolveUniqueDocs(
    NFyaml::TDocument& doc,
    const std::function<void(TDocumentConfig&&)>& onDocument);

// ---------------------------------------------------------------------------
// Track A+ : polynomial, K-independent config validation.
//
// The merge model (Apply/Inherit/Append) is replace-only for scalar leaves:
// untagged scalars and !inherit deep-merge both leave each scalar leaf
// single-sourced (the highest-precedence fitting selector that writes a leaf
// wins). Only !append / !remove / !inherit:<key> keyed-sequence merges
// accumulate. Therefore the set of values a scalar leaf can take across the
// entire label space is bounded by {base value} U {value written by each
// selector} -- at most n+1 literals, computable in O(n*d) WITHOUT enumerating
// the Cartesian product of label values.
//
// Paths are config-relative, '/'-separated, with a leading '/'. For example
// /auth_config/account_lockout/attempt_reset_duration.
// ---------------------------------------------------------------------------

/**
 * Per-field value-sets.
 *
 * Values[p] is a sound over-approximation of the values the scalar leaf at path
 * p can take across the whole label space. Soundness: for any realizable label
 * tuple t, Resolve(doc, t) assigns every single-sourced scalar leaf at a
 * non-fenced path p a value contained in Values[p]. The set may additionally
 * contain values produced only by unrealizable selector combinations (possible
 * false positives, never false negatives). Presence (a leaf being absent in
 * some branch) is out of scope -- Values covers values of present leaves.
 *
 * FencedPaths are sequence-valued paths (every !append site, and any keyed or
 * wholesale-replaced sequence). They cannot be decomposed into an independent
 * per-field value-set; callers fall back to per-document enumeration for them.
 */
struct TFieldValueSets {
    TMap<TString, TSet<TString>> Values;
    TSet<TString> FencedPaths;

    bool IsFenced(const TString& path) const { return FencedPaths.contains(path); }
};

/**
 * Computes per-field value-sets. Cost O(n*d), independent of K (the number of
 * distinct resolved configs).
 */
TFieldValueSets ResolveFieldValueSets(NFyaml::TDocument& doc);

/**
 * Runs a per-value predicate over a single field's value-set (the k=1 A+ check
 * shape). Returns false and fills failingValue with the first value for which
 * predicate returns false. A path with no values (absent everywhere) or a fenced
 * path trivially passes. Cost O(|Values[path]|), independent of K.
 */
bool ValidateField(
    const TFieldValueSets& sets,
    const TString& path,
    const std::function<bool(const TString&)>& predicate,
    TString& failingValue);

/**
 * A joint assignment of values to a set of coupled field paths, as produced by
 * one realizable label tuple. A path maps to its scalar value, or to nullopt if
 * the leaf is absent under that tuple (presence is tracked so presence-coupled
 * rules can be modelled, per the A+ soundness caveat).
 */
using TFieldAssignment = TMap<TString, std::optional<TString>>;

/**
 * The realizability filter (Track A localised enumeration).
 *
 * Returns the EXACT set of joint assignments the coupled paths can take across
 * the whole label space. Computed by resolving only over the labels the
 * coupled-field selectors actually constrain, so the cost is independent of
 * high-cardinality labels (e.g. tenant) the rule does not couple -- the
 * empirical "arity is anti-correlated with cardinality" property keeps this
 * small for k>=2 rules. If any coupled path is fenced (accumulating sequence),
 * the path is reported in 'fenced' and the rule must fall back to enumeration.
 */
TVector<TFieldAssignment> EnumerateRealizableAssignments(
    NFyaml::TDocument& doc,
    const TVector<TString>& paths,
    TSet<TString>& fenced);

/**
 * A reified semantic rule: a set of coupled field paths plus a predicate over a
 * realizable assignment. Check returns an error message when the assignment is
 * invalid, or std::nullopt when valid.
 */
struct TFieldRule {
    TVector<TString> Paths;
    std::function<std::optional<TString>(const TFieldAssignment&)> Check;
};

struct TFieldRuleViolation {
    TString Message;
    TFieldAssignment Witness;
};

/**
 * Evaluates a rule against the whole config space without per-document
 * enumeration: it runs Check over every realizable joint assignment of the
 * rule's coupled fields. Returns every violation found (empty => the rule holds
 * for every resolved config). If a coupled path is fenced, it is reported in
 * 'fenced' and the caller must validate it via enumeration instead.
 *
 * This is faithful to full enumeration: the set of assignments evaluated equals
 * the set of (coupled-field projections of) all resolved configs.
 */
TVector<TFieldRuleViolation> ValidateFieldRule(
    NFyaml::TDocument& doc,
    const TFieldRule& rule,
    TSet<TString>& fenced);

// ---------------------------------------------------------------------------
// Track A+ : structural (proto-transform) validation support.
//
// Structural checks read whole config sections, not single fields. A check that
// reads section S holds over the whole label space iff it holds for every
// DISTINCT realizable projection of the config onto S. Two helpers below give
// the building blocks; the proto-aware orchestration lives in the outer
// yaml_config library (which can call YamlToProto / the proto descriptors).
// ---------------------------------------------------------------------------

/**
 * The static-section guard. Returns every selector-written config path that
 * falls at or under one of the given config-relative static section prefixes
 * (e.g. "/domains_config", "/host_configs"). An empty result means no selector
 * touches any static section -- the precondition for running the static
 * structural checks exactly once on the base config. A non-empty result is an
 * (intentional) accept-set narrowing: such configs must be rejected or fully
 * enumerated.
 */
TVector<TString> SelectorWritesUnder(
    NFyaml::TDocument& doc,
    const TVector<TString>& staticPrefixes);

/**
 * Invokes onProjection once per DISTINCT realizable projection of the config
 * onto the given top-level section prefixes. The node passed to the callback is
 * the fully resolved "config" node of a representative label tuple for that
 * projection (tags removed, aliases resolved -- exactly what the legacy path
 * would feed to YamlToProto).
 *
 * Cost is independent of any high-cardinality label that does not vary one of
 * the requested sections (it enumerates only the labels those sections'
 * selectors constrain, plus any label an incompatibility rule references), and
 * dedups by the serialized projection -- so a check reading these sections runs
 * #distinct-projections times rather than K times.
 */
void EnumerateDistinctProjections(
    NFyaml::TDocument& doc,
    const TVector<TString>& sectionPrefixes,
    const std::function<void(NFyaml::TNodeRef)>& onProjection);

/**
 * Calculates hash of resolved config
 * Used to ensure that cli resolves config the same as a server
 */
size_t Hash(const TResolvedConfig& config);

/**
 * Validates single YAML volatile config schema
 */
void ValidateVolatileConfig(NFyaml::TDocument& doc);

/**
 * Appends volatile configs to the end of selectors list
 * **Important**: Document should be a list with selectors
 */
void AppendVolatileConfigs(NFyaml::TDocument& config, NFyaml::TDocument& volatileConfig);

/**
 * Appends volatile configs to the end of selectors list
 * **Important**: Node should be a list with selectors
 */
void AppendVolatileConfigs(NFyaml::TDocument& config, NFyaml::TNodeRef& volatileConfig);

/**
 * Appends database config to the end of selectors list
 * **Important**: Document should be correct DatabaseConfig
 */
void AppendDatabaseConfig(NFyaml::TDocument& config, NFyaml::TDocument& databaseConfig);

/**
 * Fuses base config with console config (top-level per-key merge)
 * - Console config values take precedence (console wins)
 * - Base config fills in missing top-level keys only
 * - metadata, allowed_labels, selector_config from console preserved
 * @param baseConfig - simple format config (just fields, no metadata/selectors)
 * @param consoleConfig - full format config (metadata, config, allowed_labels, selector_config)
 * @return fused config document
 */
NFyaml::TDocument FuseConfigs(const TString& baseConfig, const TString& consoleConfig);

/**
 * Parses config version
 */
ui64 GetVersion(const TString& config);

/**
 * Represents config metadata
 */
struct TMainMetadata {
    std::optional<ui64> Version;
    std::optional<TString> Cluster;
};

/**
 * Represents config metadata
 */
struct TStorageMetadata {
    std::optional<ui64> Version;
    std::optional<TString> Cluster;
};

/**
 * Represents volatile config metadata
 */
struct TVolatileMetadata {
    std::optional<ui64> Version;
    std::optional<TString> Cluster;
    std::optional<ui64> Id;
};

/**
 * Represents database config metadata
 */
struct TDatabaseMetadata {
    // maybe we should enforce Cluster as well
    std::optional<ui64> Version;
    std::optional<TString> Database;
};

struct TError {
    TString Error;
};

/**
 * Parses config metadata
 */
std::variant<TMainMetadata, TDatabaseMetadata, TError> GetGenericMetadata(const TString& config);

/**
 * Parses config metadata
 */
TMainMetadata GetMainMetadata(const TString& config);

/**
 * Parses database config metadata
 */
TDatabaseMetadata GetDatabaseMetadata(const TString& config);

/**
 * Parses storage config metadata
 */
TStorageMetadata GetStorageMetadata(const TString& config);

/**
 * Parses volatile config metadata
 */
TVolatileMetadata GetVolatileMetadata(const TString& config);

/**
 * Replaces metadata in config
 */
TString ReplaceMetadata(const TString& config, const TMainMetadata& metadata);

/**
 * Takes valid MainConfig and increases version exactly by one
 */
TString UpgradeMainConfigVersion(const TString& config);

/**
 * Takes valid StorageConfig and increases version exactly by one
 */
TString UpgradeStorageConfigVersion(const TString& config);

/**
 * Takes valid DatabaseConfig and increases version exactly by one
 */
TString UpgradeDatabaseConfigVersion(const TString& config);

/**
 * Replaces metadata in database config
 */
TString ReplaceMetadata(const TString& config, const TDatabaseMetadata& metadata);

/**
 * Replaces metadata in storage config
 */
TString ReplaceMetadata(const TString& config, const TStorageMetadata& metadata);

/**
 * Replaces volatile metadata in config
 */
TString ReplaceMetadata(const TString& config, const TVolatileMetadata& metadata);

/**
 * Checks whether string is volatile config or not
 */
bool IsVolatileConfig(const TString& config);

/**
 * Checks whether string is main config or not
 */
bool IsMainConfig(const TString& config);

/**
 * Checks whether string is storage config or not
 */
bool IsStorageConfig(const TString& config);

/**
 * Checks whether string is main config or not
 */
bool IsDatabaseConfig(const TString& config);

/**
 * Checks whether string is static config or not
 */
bool IsStaticConfig(const TString& config);

/**
 * Strips metadata from config
 */
TString StripMetadata(const TString& config);

} // namespace NKikimr::NYamlConfig
