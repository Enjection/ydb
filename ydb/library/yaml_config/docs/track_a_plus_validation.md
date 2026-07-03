# Track A⁺ — Polynomial, K-independent config validation

Status: in progress. Implements phase **P1** of the config-resolution redesign
(arch-committee handoff doc). This file is the implementation guide for the
work landing in `ydb/library/yaml_config/public/`.

## Problem

The config accept gate validates a proposed YAML config by enumerating *every
distinct resolved document* and re-running the semantic validators on each:

```
ydb/core/cms/console/console_configs_manager.cpp:166
    NYamlConfig::ResolveUniqueDocs(tree, [&](TDocumentConfig&& config) {
        auto cfg = NYamlConfig::YamlToProto(config.second, true, true);
        ValidateConfig(cfg, errors);   // re-run for every distinct doc
    });
```

Cost is `O(K · validate)` where `K` is the number of distinct resolved configs.
`K` is multiplicative in label cardinalities, so a second high-cardinality label
(region/az/host-class crossed with tenant) makes the accept gate explode. The
same enumerate-then-validate shape lives in `distconf_invoke_storage_config.cpp`
and the CLI `ydb_dynamic_config.cpp`.

## Key observation (from the handoff doc, verified against the code)

The merge model (`yaml_config.cpp` `Apply`/`Inherit`/`Append`, lines 266–363) is
*replace-only* for scalar leaves: untagged scalars and `!inherit` deep-merge both
leave each scalar leaf **single-sourced** — the highest-precedence fitting
selector that writes a leaf wins. Only `!append` (sequence concatenation),
`!remove`, and `!inherit:<key>` keyed-sequence merges accumulate.

Therefore, for a single-sourced scalar leaf at path `p`, the set of values it can
take across the *entire* label space is bounded by:

```
V_f[p] = { base value at p } ∪ { value written at p by each selector }
```

— at most `n + 1` literals, computable in `O(n · d)` **without** enumerating the
Cartesian product. Semantic checks then run against `V_f` (per-field value lists)
instead of against `K` resolved documents.

## What landed in this increment (the keystone primitive)

`yaml_config.h` / `yaml_config.cpp`:

```cpp
struct TFieldValueSets {
    TMap<TString, TSet<TString>> Values;   // config-relative path -> possible scalar values
    TSet<TString> FencedPaths;             // paths that cannot be decomposed
    bool IsFenced(const TString& path) const;
};

TFieldValueSets ResolveFieldValueSets(NFyaml::TDocument& doc);

bool ValidateField(const TFieldValueSets& sets, const TString& path,
                   const std::function<bool(const TString&)>& predicate,
                   TString& failingValue);
```

**Soundness contract** (over-approximation; the doc's "no false negatives for
value-coupled, single-sourced, presence-stable fields"):

> For any realizable label tuple `t`, `Resolve(doc, t)` assigns every
> single-sourced scalar leaf at non-fenced path `p` a value contained in
> `Values[p]`.

The set may additionally contain values produced only by *unrealizable* selector
combinations (false positives, never false negatives). Track A's realizability
filter refines this for cross-field (k≥2) rules. Presence (a leaf being absent in
some branch) is the documented caveat — `V_f` covers the values of *present*
leaves, not presence itself.

**Fenced paths**: sequence-valued paths (every `!append` site, plus any
wholesale-replaced or keyed sequence). Callers fall back to per-document
enumeration for these. This matches the doc's `!append` fence list
(`services_enabled`, `resource_broker_config.queues`, `log_config.entry`, …).

## Validation evidence

`yaml_config_ut.cpp` (suite `YamlConfigFieldValueSets`):
- Direct value-set assertions on hand-written fixtures (scalar leaves, overlays,
  `!append` fencing, `!inherit` deep-merge single-sourcing).
- **Soundness cross-check**: for the existing resolution fixtures, run
  `ResolveAll`, walk every resolved document, and assert each resolved scalar
  leaf value is contained in `Values[path]`. This is the unit-level analogue of
  the doc's "shadow-run / accept-set equivalence" gate.
- `ValidateField` exercised with a `TDuration`-parse predicate (the real k=1
  `auth_config.account_lockout.attempt_reset_duration` check shape).

## Next increments (not in this change)

- **P0.5**: hoist the 8 static-only checks (StateStorage over `domains_config`,
  DatabaseConfig reflection) out of the per-resolved-doc loop; run once on the
  post-append static config behind a "no selector touches static sections" guard.
- **P1 wiring**: in `ydb/core/config/validation`, reify the ~6 selector-varied
  rules as `{path, arity, predicate}` and drive them from `ResolveFieldValueSets`
  on the accept gate; run **in parallel** with the legacy enumerate-then-validate
  path and log divergences until zero fleet-wide, then cut over.
- **P4**: cross-field (k=2,3) checks via a realizability filter (Track A inverted
  index); fence aggregate / whole-set `!append` constraints.

Mapping of the current selector-varied validators to `{path, arity}` (verified):

| Check | Path(s) | Arity |
|---|---|---|
| `ValidateAccountLockout` | `/auth_config/account_lockout/attempt_reset_duration` | k=1 |
| `ValidatePasswordComplexity` | `/auth_config/password_complexity/{min_length,min_lower_case_count,min_upper_case_count,min_numbers_count,min_special_chars_count}` | k=5 |
| `ValidateDefaultCompression` | `/column_shard_config/{default_compression,default_compression_level}` | k=2 + presence |
| `ValidateStateStorageConfig` | `/domains_config/...` (static) | hoist |

---

## Implementation status (this branch)

All five phases of the proto-transform K-independent validation plan are
implemented and tested in the `yaml_config` library (fast, isolated build):

- **P-S0 shared foundations** — alias-node expansion in `CollectScalarLeaves`
  (per-node `ResolveAlias`, no whole-doc resolve); wholesale-replace presence
  tracking (`ReplaceRoots`) in `EnumerateRealizableAssignments`; the static
  guard `SelectorWritesUnder`. (Incompatibility-rule pruning was prototyped but
  removed from the hot path: it ballooned the enumerated label set; A+ stays a
  sound over-approximation without it. Re-adding bounded pruning is a follow-up.)
- **P-S1 per-field coercion (regime B)** — `ValidateStructuralAPlus` validates
  every value-set value against its proto field type/enum via descriptor
  reflection; plus the **protoc plugin** generates `GetStaticSectionPaths()`
  from the new `(NMarkers.SelectorStatic)` field marker.
- **P-S2 static hoist + guard** — `SelectorStatic` marker + `StaticGuardSections`
  + `SelectorWritesUnder`; static checks run via the constant base sections in
  each projection.
- **P-S3 sub-tree projection (regime C)** — `EnumerateDistinctProjections`
  resolves one representative per distinct firing-signature (no per-tuple
  `Resolve()`), so the proto transform runs #distinct-projections times, not K.
- **P-S4 cutover — DONE.** `ValidateAPlus` (aggregate structural + guard +
  semantic verdict, K-independent) is the SOLE blocking gate in both
  `ValidateMainConfig` and `ValidateDatabaseConfig`; the O(K) per-resolved-doc
  legacy loops were removed. The database path runs
  `ValidateDatabaseAllowlistAPlus` first (presence-based, selector-aware —
  also closing the old base-only allowlist gap) and passes
  `AppData()->ConfigSwissKnife` into the semantic pass so a build's extra
  validators keep gating. Unknown/deprecated field diagnostics come from
  `CollectFieldDiagnosticsAPlus` on both paths. `StructuralShadowRun` and the
  `ValidateStructuralLegacy`/`ValidateSemanticLegacy` per-doc oracles remain
  TEST-ONLY: they are the executable legacy behavior spec the equivalence
  suites pin A+ against — never wire them back into a gate.

### Tests
- `yaml_config/ut`: the `YamlConfigFieldValueSets`, `YamlConfigAPlus*`,
  `YamlConfigStructuralProjection/Guard/Coercion/Shadow/SectionMarkers`,
  `YamlConfigSemanticAPlus`, `YamlConfigDatabaseAllowlist`,
  `YamlConfigFieldDiagnostics`, `YamlConfigRulePruning` and
  `YamlConfigPolynomialScale` suites — the equivalence suites pin A+ against
  the per-doc oracles (see Post-cutover invariants).
- `protobuf_plugin/ut`: `HasStaticSectionPaths` verifies the generated accessor.
- `cms/console/ut`: rejection-parity and accept-path tests exercise the gate
  end-to-end through the console actor (semantic reject in base and via
  selector, static-guard reject, multi-selector accept, DB allowlist reject in
  base and via selector).

---

## "Strictly not worse than legacy proto validity" — verification & fix

Independent architect + critic review asked: does `ValidateStructuralAPlus` reject
in EVERY case the legacy per-doc `YamlToProto+Transform` would (no false negatives
on field types / enums / structural checks)? **Initial verdict: NO.** The enum-only
reflection layer missed non-enum coercion errors in selector-varied fields outside
the declared transform-read sections.

Key empirical finding: the parser uses `CastRobust=true`, so legacy **silently
coerces** bad scalar int/bool (no throw) — those are NOT divergences. The real
throwing cases are enums and structural type mismatches.

**Fix applied:** the enum-only layer was replaced by **per-section coercion using
the real converter** — for each top-level section, `EnumerateDistinctProjections`
yields its distinct realized values and each is run through
`YamlToProto(preTransform=false)` (the exact JsonToProto phase). Because coercion
is per-field independent, this is additive across sections (K-independent) and,
using the *same converter as legacy*, makes A+'s coercion decision **equal to
legacy's by construction** for every field type and for fenced sequences — closing
false-negative classes #1a (duration/timestamp), #2 (fenced-sequence elements),
and #5 (ephemeral/enum paths).

Tests (`YamlConfigStructuralCoercion`): enum → both reject; int/bool → both accept
(CastRobust) — all assert `!shadow.Diverged` (A+ decides exactly as legacy).

**Remaining residual (documented, not yet closed): #3 transform-read list
completeness.** The cross-section *transform* checks run on the joint projection of
`TransformReadDynamicSections()` (a hand-declared list) ∪ static-guard sections.
If a transform `Prepare*` reads a selector-varied section absent from BOTH lists,
its variant is not projected and a transform error could be missed. The principled
fix is to derive the transform-read set from proto markers as a provable superset
(the `SelectorStatic` plugin machinery is the scaffold); until then a
completeness oracle test (vary every top-level section per label; assert
`StructuralShadowRun` never diverges) should gate the claim. The static-section
guard already covers selector overrides of static sections.

---

## Second review round: polynomiality + completeness (architect + critic)

Two claims re-verified after the marker-derivation work.

**CLAIM 2 (polynomial / K-independent) — gap found and fixed.**
The critic showed the transform pass projected `TransformReadDynamicSections()`
*jointly*: with two transform-read sections varied by two independent
high-cardinality labels (auth_config by tenant, log_config by region), the joint
projection was `(T+1)·(R+1)` — the multiplicative cliff the redesign exists to
remove. **Fix:** `ValidateStructuralAPlus` now runs the full pipeline
(coercion + transform) **per top-level section independently** — additive cost
`Σ_sections #distinct(section)`, polynomial and K-independent. Sound because each
`Prepare*` reads a single *dynamic* transform-read section (plus constant static
sections); no transform check couples two selector-varied sections. New benchmark
`YamlConfigStructuralBenchmark::PerSectionIsAdditiveNotMultiplicative` pins
per-section = `(T+1)+(R+1)` vs joint = `(T+1)·(R+1)`.

**CLAIM 1 (strictly not weaker) — completeness gaps found and fixed.**
The architect audited all 15 `Prepare*` against the markers and found inputs the
transform reads that were neither static nor transform-read:
- `TAppConfig.SelfManagementConfig` (field 86) — now marked `(SelectorStatic)`.
- ephemeral top-level input keys (system_tablets, static_erasure, erasure,
  default_disk_type, fail_domain_type, storage_pool_types, security_config,
  domain_name, …) — `StaticGuardSections()` now derives them from the
  `TEphemeralInputFields` descriptor, so the guard is complete and self-maintaining.

**Residual / safety net.** Marking completeness ("every transform-read section is
classified") is an audited property; the production shadow-run (`StructuralShadowRun`,
wired into the accept gate) is the backstop — any missed section that a real
config varies surfaces as a logged divergence before cutover, exactly as the
gist's phased rollout prescribes.

---

## Semantic validation rules — NOT weakened (verified)

Architect + critic both confirmed: the previously-enforced SEMANTIC rules
(`NConfig::ValidateConfig`: Auth password min-length≥Σ, duration parse, ColumnShard
compression, Monitoring auth-coupling, StateStorage NToSelect/ring ranges, Database
allowlist) are **not weakened** by Track A+.

- The validators themselves are unchanged: `ydb/core/config/validation/` is
  byte-identical to baseline, and `ValidateSemanticAPlus` invokes the REAL
  `NConfig::ValidateConfig` (or the build's `ConfigSwissKnife` on the database
  path) per distinct projection — the semantic decision logic is reused, only
  the enumeration strategy changed (per-section projections instead of every
  resolved doc).
- The reified semantic rules (`DurationRule`/`PasswordComplexityRule`/
  `CompressionRule`) are TEST-ONLY and live in `yaml_config_ut.cpp`.

### ⚠️ Post-cutover invariants
- The per-doc oracles (`StructuralShadowRun`, `ValidateStructuralLegacy`,
  `ValidateSemanticLegacy`) are the LEGACY BEHAVIOR SPEC. The equivalence
  suites (`YamlConfigStructuralShadow`/`Coercion`/`SemanticAPlus`/
  `YamlConfigRulePruning`, the per-section completeness oracle) pin A+ against
  them; deleting the oracles deletes the spec.
- The static-section guard is load-bearing for semantic soundness (Monitoring
  reads `monitoring_config` jointly with static `domains_config`). Do NOT
  relax it without first grouping the coupled pair into a joint projection.
- No gate validator may read TWO selector-variable sections jointly:
  `GateValidatorsReadAtMostOneDynamicSection` (declared read-set table) and
  `EverySectionShadowAgreesWithOracle` (per-section sweep against the oracle)
  enforce this mechanically; update the table when adding a validator.

---

## Behavior deltas vs the removed per-doc gate

The cutover was direct (no feature flag; rollback = revert the commit). These
are the deliberate, user-visible differences from the legacy gate:

1. **Static-section variation is now a hard REJECT.** A selector writing under
   a `SelectorStatic` section (or an ephemeral input key) is rejected by the
   guard with `selector overrides static section path '...'`. Legacy resolved
   and sometimes accepted such configs; the variation has no coherent runtime
   meaning (cluster-wide bootstrap state) and the guard is the precondition
   for per-section semantic soundness.
2. **The DB allowlist is selector-aware and presence-based.** A disallowed
   section introduced ONLY by a database selector is now rejected (the legacy
   base-only reflection check missed it — the long-standing TODO). Presence
   mirrors legacy `HasField`/`FieldSize` semantics: YAML-null values and empty
   sequences do not count, empty mappings do, ephemeral input keys are
   ignored; error text is byte-identical to the legacy allowlist's.
3. **Incompatibility rules prune projections exactly like the resolver.**
   Rules whose referenced labels are all inside the involved set are applied
   during tuple enumeration (`IsCompatiblePartial`); rules referencing outside
   labels are skipped (sound over-approximation). Rules remain pure
   state-space reduction — they never decide what a real node receives.
4. **First-error text.** Rejections carry the first A+ violation
   (structural, then guard, then semantic). The message body comes from the
   REAL validators/converter, so per-validator text is unchanged; the ORDER of
   first error may differ from the legacy first-resolved-doc order when a
   config is invalid in several ways at once.
5. **Known accepted corner:** quoted `"null"`/`""` as a whole-section value is
   indistinguishable from YAML null in the presence views (`Scalar()` loses
   quoting). For message-typed sections both engines agree anyway (a string
   where a mapping is expected fails structurally); TAppConfig has no
   scalar-typed top-level fields, so no decision changes.

---

## Resolved during cutover (was "Open items")

1. Incompatibility-rule pruning — DONE (`IsCompatiblePartial`, exact for
   fully-inside rules; tested by `YamlConfigRulePruning`).
2. Guard strictness — decided: hard error (delta 1 above); relaxing requires
   grouped joint projections first (see Post-cutover invariants).
3. Console diagnostics unification — DONE (`CollectFieldDiagnosticsAPlus` on
   both paths; the inline `collectBlock` duplicate and the per-resolved-doc DB
   collector are gone).
4. `TFieldRule` engine — moved into `yaml_config_ut.cpp` (test harness for the
   enumeration machinery); the `SelectorTransformRead` marker chain — DELETED
   (the validators project every present section, so the classification
   carried no information; extension number 82007 is comment-reserved).
5. DB allowlist coverage — DONE (`ValidateDatabaseAllowlistAPlus` is the
   blocking allowlist; `csk` is threaded into the semantic pass so extra
   swiss-knife validators keep gating).
6. dynamic×dynamic coupling enforcement — DONE mechanically:
   `GateValidatorsReadAtMostOneDynamicSection` (read-set table) +
   `EverySectionShadowAgreesWithOracle` (full per-section sweep vs the
   per-doc oracle).
