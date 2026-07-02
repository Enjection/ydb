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
- **P-S4 shadow-run cutover** — `ValidateAPlus` (aggregate structural + guard +
  semantic verdict, K-independent, no legacy re-run) is wired into BOTH console
  accept gates (`ValidateMainConfig`, `ValidateDatabaseConfig`) as a
  non-blocking, logged shadow. The gate CAPTURES its own legacy verdict and the
  shadow compares against it, so (a) no O(K) legacy enumeration is repeated for
  the shadow, and (b) the shadow observes legacy-REJECTED configs too — the
  legacy-rejects/A+-accepts direction that decides cutover safety. On the
  database path `ValidateDatabaseAllowlistAPlus` is folded into the A+ verdict,
  shadowing the legacy DB allowlist (`NConfig::ValidateDatabaseConfig`), which
  has no counterpart inside `ValidateConfig`. `StructuralShadowRun` (which
  re-derives the legacy verdict itself, 2× O(K)) remains for tests and offline
  head-to-head parity audits only.

### Tests (all green)
- `yaml_config/ut`: 18 tests — `YamlConfigFieldValueSets`, `YamlConfigAPlus*`,
  `YamlConfigStructuralProjection` (projection set == full enumeration; work
  independent of an uncoupled high-cardinality label), `YamlConfigStructuralGuard`,
  `YamlConfigStructuralEnumCoercion`, `YamlConfigStructuralShadow`.
- `protobuf_plugin/ut`: `HasStaticSectionPaths` verifies the generated accessor.

### Known follow-ups
- Wire `StaticGuardSections()`/`TransformReadDynamicSections()` to the generated
  marker tables (mark the real static sections in `config.proto`; needs a global
  rebuild).
- Re-introduce bounded incompatibility-rule pruning to trim A+ false positives.
- Exact per-field coercion via the real converter (currently enum/type via
  reflection; representative-run + shadow-run backstop the rest).

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

- `NConfig::ValidateConfig` is unchanged and remains the SOLE acceptance gate
  (its verdict is captured-then-rethrown, not altered).
  `ydb/core/config/validation/` is byte-identical to baseline.
- The only accept-path A+ addition is the `ValidateAPlus` shadow (additive,
  log-only, exception-swallowed) — it cannot affect acceptance.
- The wired A+ component (`ValidateStructuralAPlus`) is proto-transform/STRUCTURAL
  only; it never calls a semantic validator, so it does not replace the min/max
  layer. The reified semantic rules (`DurationRule`/`PasswordComplexityRule`/
  `CompressionRule`) are TEST-ONLY.

### ⚠️ Cutover invariant
"Cutover" in this document refers ONLY to the structural/proto-transform path and,
separately, to the semantic path AFTER it is complete and shadow-gated. It must
NEVER be read as removing the semantic gate in `ValidateMainConfig`.
The semantic A+ engine may replace that gate only once Monitoring is reified and
StateStorage aggregates are explicitly delegated/fenced, and only after a
zero-divergence semantic shadow-run — the same discipline used for the structural
layer.

---

## Known intentional divergence classes (bucket these in cutover math)

The zero-divergence cutover criterion must classify `DIVERGED` log lines; the
following classes are *known and intentional*, not parity bugs, and must not be
allowed to either block cutover indefinitely or drown out a real divergence:

1. **Incompatibility rules** (A+ over-reject): A+ enumerates rule-pruned label
   combinations legacy skips — see open item 1.
2. **Static-section guard** (A+ over-reject): a selector varying a
   `SelectorStatic` section is rejected by A+ by policy; legacy resolves and
   may accept — see open item 2.
3. **Selector-aware DB allowlist** (A+ over-reject, safe direction): A+ catches
   a disallowed section introduced only by a DB selector, which the legacy
   base-only check misses (a latent legacy bug, not an A+ one).
4. **csk-only validators on the DB path** (legacy-only reject): the captured DB
   gate verdict includes `csk->ValidateConfig` (ConfigSwissKnife); the A+
   semantic side mirrors `NConfig::ValidateConfig`. Identical in the default
   build; a deployment installing extra csk validators produces
   `legacyRejected=1 / aplusRejected=0` lines attributable to this class.
5. **Null-literal corners** (shadow-log only): `Scalar()` loses quoting, so a
   quoted `"null"`/`""` top-level value is treated as YAML null by the
   presence/field-setting views. For message-typed sections the combined
   verdicts still agree (structural failure on both sides).

Additionally note the combined-verdict comparison is ONE bit per path: a config
where A+ is simultaneously weaker on one axis and stricter on another logs no
divergence. The per-axis oracle (`StructuralShadowRun`) remains available for
offline audits where axis-level attribution is needed.

---

## Open items / cutover prerequisites

1. **Incompatibility-rule pruning** in `EnumerateDistinctProjections` /
   `EnumerateRealizableAssignments` — rules are intentionally ignored (sound
   over-approximation; rules exist for state-space reduction only, never for
   deciding what a real node receives). The residue is a benign
   over-reject-divergence class: a config invalid ONLY under a rule-pruned label
   combination logs a permanent `DIVERGED` line. Either prune rules whose
   referenced labels fall entirely inside the involved set (exact, cheap — a
   prior naive attempt that enumerated ALL rule-referenced labels ballooned the
   product; avoid that), or bucket this divergence class in the shadow log so it
   cannot block the zero-divergence criterion.
2. **Strictness policy:** the static-section guard runs on BOTH the structural
   and semantic paths (deliberately stricter than legacy). It is the
   load-bearing reason the per-section semantic decomposition stays not-weaker
   for cross-section couplings (Monitoring/StateStorage). Do NOT relax it
   without first grouping marker-coupled section pairs into joint projections.
3. **Unify console diagnostics:** the DB path still collects unknown fields
   per-resolved-doc; switch it to `CollectFieldDiagnosticsAPlus` (the main path
   already behaves equivalently, via an inline `collectBlock` that duplicates
   the lib function — can adopt the lib API).
4. **`TFieldRule` semantic engine is TEST-ONLY.** Production semantic validation
   uses projection + real `ValidateConfig` (`ValidateSemanticAPlus`). Decide
   whether to keep the `TFieldRule` engine (granular, but hand-mirrored
   predicates risk divergence) or delete it. Same decision needed for the
   `SelectorTransformRead` marker chain (`GetTransformReadSectionPaths()` /
   `TransformReadDynamicSections()`), which currently has no production
   consumer: either drive the validators' section grouping from it or drop it —
   dead-but-load-bearing-looking markers are a correctness trap.
5. **Cutover prerequisites (do NOT skip):** zero-divergence shadow fleet-wide;
   NEVER remove the legacy gate or the DB allowlist
   (`NConfig::ValidateDatabaseConfig`) without A+ equivalents wired and shadowed
   — A+ semantic does not include the DB allowlist
   (`ValidateDatabaseAllowlistAPlus` is its counterpart and is now folded into
   the database-path shadow verdict).
6. **Marker completeness:** if a new `Prepare*` reads a new section, it must be
   marked Static or TransformRead. `StaticGuardCoversSemanticCoupledSections`
   pins the known coupled static partners. Still missing: a mechanical check for
   a NEW validator/`Prepare*` coupling two *dynamic* sections — the per-section
   projection cannot see that joint state and the guard cannot catch it (today
   no gate validator does this; the property is audited-not-enforced). Before
   cutover this needs enforcement (e.g. validators declare their read-sections
   and CI rejects dynamic×dynamic couplings), not an audit.
