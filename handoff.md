# Handoff — Track A⁺ K-independent config validation

## What this is

Implementation of the "Track A⁺" config-resolution redesign (from the gist
handoff doc): make YDB config validation **K-independent** (polynomial, not
multiplicative in label cardinalities) while staying **strictly not weaker** than
the legacy per-resolved-document validation, behind a **shadow-run** so the legacy
path stays authoritative until zero divergence.

- **Worktree:** `/home/innokentii/ydbwork2/config-resolution-redesign`
- **Branch:** `config-resolution-redesign` (forked from `./ydb` main, origin = `Enjection/ydb`)
- **Status:** all work staged, **nothing committed**. 44 lib tests + 7 plugin tests green; `ydb/core/cms/console` compiles.

## The core idea

Legacy accept gate = `ResolveUniqueDocs(tree)` → for each distinct resolved doc:
`YamlToProto(transform)` (structural) + `NConfig::ValidateConfig` (semantic). Cost
`O(K · validate)`, K multiplicative in label cardinalities.

A⁺ replaces "validate every resolved config" with "validate every **distinct
projection** of each config **section**, using the **real** validators":
- A check that reads section S only ever sees `distinct(S)` different inputs
  across the whole label space. Validate each once → same coverage, deduplicated.
- Done **per top-level section independently** → cost is `Σ_sections distinct(S)`
  (**additive** → K-independent), not the joint product.
- Uses the **real** `YamlToProto`/`ValidateConfig` per projection → strictly equal
  to legacy by construction (no hand-mirrored predicates).

Sections are classified by **proto field markers**:
- `(NMarkers.SelectorStatic)` — selectors must NOT vary it (guard rejects);
  checked once on the constant base. For structural/aggregate sections
  (domains_config, blob_storage_config, …).
- `(NMarkers.SelectorTransformRead)` — the transform reads it and selectors may
  vary it; projected. (actor_system_config, log_config, interconnect_config,
  grpc_config, auth_config.)

## Files changed

**`ydb/library/yaml_config/public/yaml_config.{h,cpp}`** (pure fyaml, no proto):
- `TFieldValueSets ResolveFieldValueSets(doc)` — per-field value-sets V_f (sound
  over-approx), `ValidateField`.
- `EnumerateRealizableAssignments(doc, paths, fenced)` — realizability filter
  (localized enumeration; presence via wholesale-replace `ReplaceRoots`; alias
  expansion via `IsAlias()/ResolveAlias()` in `CollectScalarLeaves`).
- `TFieldRule` / `ValidateFieldRule` — semantic rule engine (**TEST-ONLY**, see
  Open items).
- `EnumerateDistinctProjections(doc, sectionPrefixes, onProjection)` — **the
  keystone**: dedups label tuples by the firing-signature of section-writing
  selectors and yields ONE representative resolved config per distinct
  projection, built via clone+`Apply`+`RemoveTags` (NOT the slow
  `TDocument::Resolve()`).
- `SelectorWritesUnder(doc, prefixes)` — the static-section guard.
- `TIncompatibilityRules::ReferencedLabels()` (currently unused; see Open items).

**`ydb/library/yaml_config/yaml_config.{h,cpp}`** (outer; calls proto + validators):
- `TransformReadDynamicSections()` / `StaticGuardSections()` — **marker-derived**
  (`TAppConfig::GetTransformReadSectionPaths()` / `GetStaticSectionPaths()`);
  static also adds the ephemeral input keys via the `TEphemeralInputFields`
  descriptor.
- `ValidateStructuralAPlus(doc, allowUnknown, guardViolations)` — per-section
  projection running `YamlToProto(transform=true)`.
- `ValidateSemanticAPlus(doc, allowUnknown)` — per-section projection running the
  real `NConfig::ValidateConfig`.
- `ValidateStructuralLegacy` / `ValidateSemanticLegacy` — per-doc oracles.
- `ValidateDatabaseAllowlistAPlus(doc)` — K-independent DB allowlist (stronger
  than legacy: also inspects selector overlays).
- `TFieldDiagnostics` / `CollectFieldDiagnosticsAPlus(doc)` / `ToWarnings()` —
  unknown+deprecated field collection per editable block (K-independent,
  union-equivalent).
- `StructuralShadowRun(doc)` — runs legacy + A⁺ for BOTH structural and semantic,
  reports divergence.
- helpers `TopSection`, `SplitConfigPath`.

**`ydb/core/config/protos/marker.proto`** — added `SelectorStatic = 82006`,
`SelectorTransformRead = 82007` (FieldOptions extensions).

**`ydb/core/config/tools/protobuf_plugin/main.cpp`** — generates
`GetStaticSectionPaths()` and `GetTransformReadSectionPaths()` on the Root message
from the two markers.

**`ydb/core/protos/config.proto`** — marked `TAppConfig` fields:
- TransformRead: `ActorSystemConfig`, `LogConfig`, `InterconnectConfig`,
  `GRpcConfig`, `AuthConfig`.
- Static: `NameserviceConfig`, `DomainsConfig`, `BlobStorageConfig`,
  `ChannelProfileConfig`, `BootstrapConfig`, `SelfManagementConfig`.

**`ydb/core/cms/console/console_configs_manager.cpp`** — `StructuralShadowRun`
wired into BOTH `ValidateMainConfig` and `ValidateDatabaseConfig` (non-blocking,
exception-swallowed, log-only via `TActivationContext::AsActorContext()` +
`LOG_ERROR_S(... CMS_CONFIGS ...)`). **Legacy validation unchanged and still
authoritative.** DB allowlist (`NConfig::ValidateDatabaseConfig`) preserved.

**Tests:** `ydb/library/yaml_config/yaml_config_ut.cpp` (suites below),
`ydb/core/config/tools/protobuf_plugin/ut.cpp` (+ `ut/protos/config_root_test.proto`).

**Docs:** `ydb/library/yaml_config/docs/track_a_plus_validation.md` (full design +
review history), `.omc/plans/proto-transform-kindependent-validation.md` (plan).

## Build / test

```
cd /home/innokentii/ydbwork2/config-resolution-redesign
# lib tests (fast; config.pb regen only if config.proto/marker.proto changed):
hya make -T -ttt --build=relwithdebinfo -j128 ydb/library/yaml_config/ut \
  -F 'YamlConfigStructural*::*' -F 'YamlConfigAPlus*::*' -F 'YamlConfigSemantic*::*' \
  -F 'YamlConfigDatabaseAllowlist::*' -F 'YamlConfigFieldDiagnostics::*' -F 'YamlConfigFieldValueSets::*'
# plugin codegen tests:
hya make -T -ttt --build=relwithdebinfo -j128 ydb/core/config/tools/protobuf_plugin/ut
# console compile check (heavy):
hya make -T --build=relwithdebinfo -j128 ydb/core/cms/console
```
Quirk: after a fresh `ut` binary build a multi-`-F` invocation sometimes reports
"0 suites" — just re-run; the binary is then cached and matches.

Test suites (34 tests): `YamlConfigFieldValueSets`, `YamlConfigAPlusValidation`
(equivalence-to-oracle + realizability), `YamlConfigAPlusBenchmark`,
`YamlConfigStructuralProjection`, `YamlConfigStructuralGuard`,
`YamlConfigStructuralCoercion`, `YamlConfigStructuralShadow`,
`YamlConfigStructuralSectionMarkers`, `YamlConfigStructuralBenchmark`,
`YamlConfigSemanticAPlus`, `YamlConfigDatabaseAllowlist`,
`YamlConfigFieldDiagnostics`. Plugin: `ValidationTests` incl.
`HasStaticSectionPaths`, `HasTransformReadSectionPaths`.

## Parity with legacy (honest)

- **No false negatives** (A⁺ never accepts what legacy rejects) for all covered
  surfaces — verified by a 32-agent adversarial parity workflow (per-dimension
  finders + independent critic refutation) on top of the earlier architect+critic
  rounds. The one confirmed false negative it found is now **fixed** (see below).
- A⁺ is **intentionally stricter** (over-reject only, never miss): static-section
  guard (now on BOTH the structural AND semantic paths); DB allowlist is
  selector-aware; incompatibility rules ignored (may validate+reject an
  unrealizable projection).
- **`!append`/`!remove`:** validated *correctly* (real `Apply` per projection).
  The only caveat is **cost**: if multiple high-cardinality labels accumulate into
  one section, that section's projections become multiplicative (= legacy K
  there). Correctness preserved; K-independence degrades — the gist's documented
  "accumulation is the breaker".
- **Diff-based validators are NOT on the accept gate** (audit cleared this): the
  console gate only runs `NConfig::ValidateConfig` (single-config); the
  current-vs-proposed `ValidateStaticGroup` / old-vs-new StateStorage live in a
  CLI tool and node_warden, outside the surface A⁺ shadows.

### Parity audit findings — all fixed

1. **DB allowlist empty-mapping false negative (was the only confirmed FN).**
   `ValidateDatabaseAllowlistAPlus` derived its section set from scalar leaves, so
   a disallowed section written as an empty mapping (`auth_config: {}`) — which the
   legacy DB gate rejects — was accepted. **Fixed**: section set now unions a
   presence-based view (`CollectPresentTopSections`, the top-level keys of base
   `config` + every selector overlay), so empty-but-present sections count. Tests:
   `DisallowedEmptyMappingSectionRejected`, `...InSelectorRejected`.
2. **Monitoring cross-section coupling on the semantic path.** `ValidateMonitoringConfig`
   reads `monitoring_config` (variable) jointly with `domains_config.security_config`
   (static); per-section projection froze the static partner at base, and
   `ValidateSemanticAPlus` ran NO guard — so a split-label config varying both could
   evade it (legacy materializes the joint and rejects). **Fixed**: the static guard
   now runs on the semantic path too, making `ValidateSemanticAPlus`
   self-sufficiently not-weaker (no longer relies on the structural pass running
   alongside). Tests: `SemanticGuardRejectsStaticSectionVariation`,
   `MonitoringCrossSectionDecoupledNotWeaker`.
3. **Marker-drift risk** (`monitoring_config`/`column_shard_config` unmarked but
   read by validators). Safe today (column_shard is self-contained; monitoring's
   partner is static-guarded), but **now pinned**: `StaticGuardCoversSemanticCoupledSections`
   asserts `domains_config` + `self_management_config` stay static-guarded, so
   unmarking a coupled static partner fails the build.
4. **Field-diagnostics blind spot.** A block that throws in the `preTransform`
   Preprocess (partial static/aggregate fragment) collected zero unknown fields.
   **Fixed**: `collectBlock` now falls back to a `preTransform=false` pass on throw
   and filters ephemeral top-level keys. UI-only (never gated acceptance); the
   happy path is unchanged.

## Complexity & scale (proven by `YamlConfigPolynomialScale`)

The headline win is a **complexity-class** change, not a constant factor:

- **Legacy:** validates every distinct resolved doc → `K = ∏ᵢ (mᵢ+1)` —
  **exponential in the number of independent labels**. (`ValidateStructuralLegacy`
  / the console accept gate.)
- **A⁺:** validates each distinct per-section projection → `Σ_s distinct(s)`
  validations — **additive across sections, polynomial**. No `∏`.

Measured (relwithdebinfo):
- A config with **legacy K = 1,035,351** validates **fully (structural + semantic,
  real validators) in ~0.65 s** (`HugeConfigValidatesInMillis`); legacy would run
  ~10⁶ transforms.
- `ComplexityClassSeparation` (two independent labels, n=10/20/40): legacy
  `K=(n+1)²`, A⁺ `=2(n+1)`; the **work-ratio grows 5.5×→10.5×→20.5×** — proof
  it is a lower complexity class, not a constant-factor win. Wall-clock tracks the
  model: legacy ~n² (20→92→540 ms), A⁺ ~linear (3.7→8.5→24 ms).
- `AgreesWithLegacyAndIsFaster` (K=1681): identical accept decision, **~23× faster**.

**Second-order caveat (honest):** within a *single* section varied by `n`
selectors, A⁺ wall time is `O(n²)` — `EnumerateDistinctProjections`
(`public/yaml_config.cpp:1622`) rebuilds each of the `n+1` representatives by
cloning + re-parsing the **whole document**, and the firing-signature dedup
re-scans involved selectors per tuple. This is **polynomial** (vs legacy
exponential) and negligible at the real shape (gist `bbfba6d`: ~150 selectors
spread across many sections ⇒ small per-section `n`), but it is **optimizable to
near-linear** (clone only the base-config subtree per projection; index selectors
by label value for O(1) signature lookup). Deferred as a perf item — it does NOT
affect correctness and was left out of the audited hot path on purpose. Tracked
as `GrowthIsPolynomialNotExponential` (asserts the polynomial, not-exponential
bound).

## Open items / next steps

1. **Incompatibility-rule pruning** in `EnumerateDistinctProjections` /
   `EnumerateRealizableAssignments` — currently ignored (sound over-approx, but
   causes over-rejection divergence when rules are active). `ReferencedLabels()`
   exists for this; a prior naive attempt ballooned the label product (avoid —
   prune without enumerating all rule-referenced labels). Needed to reach
   zero-divergence shadow before cutover.
2. **Decide strictness policy:** the static-section guard now runs on BOTH the
   structural and semantic paths (deliberately stricter — see Parity fix #2). It is
   the load-bearing reason the per-section semantic decomposition stays not-weaker
   for cross-section couplings (Monitoring/StateStorage). **Do NOT relax it** without
   first grouping marker-coupled section pairs into joint projections; relaxing it
   alone re-opens the Monitoring false negative. This is the main tension to settle
   before shadow can reach zero divergence.
3. **Unify console diagnostics:** the DB path still collects unknown fields
   per-resolved-doc; switch it to `CollectFieldDiagnosticsAPlus` (the main path
   already behaves equivalently). The main path has an inline `collectBlock` that
   duplicates the lib function — can adopt the lib API.
4. **`TFieldRule` semantic engine is TEST-ONLY.** Production semantic validation
   uses projection + real `ValidateConfig` (`ValidateSemanticAPlus`), which is
   strictly-not-worse by construction. Decide whether to keep the `TFieldRule`
   engine (granular, but hand-mirrored predicates risk divergence) or delete it.
5. **Cutover prerequisites (do NOT skip):** zero-divergence shadow fleet-wide;
   NEVER remove the legacy gate (`console_configs_manager.cpp:170`) or the DB
   allowlist (`NConfig::ValidateDatabaseConfig`) without A⁺ equivalents — A⁺
   semantic does not include the DB allowlist. The "cutover invariant" is in the
   design doc.
6. **Mark completeness:** if a new `Prepare*` reads a new section, it must be
   marked Static or TransformRead. Partially addressed:
   `StaticGuardCoversSemanticCoupledSections` pins the known coupled static
   partners. Still worth adding a fuller oracle that, for a NEW `Prepare*` coupling
   two *dynamic* sections (which the guard can't catch), asserts
   `StructuralShadowRun` never diverges — that failure mode is currently
   audited-not-tested.
7. **Near-linear projection rebuild (perf, not correctness).** Make
   `EnumerateDistinctProjections` clone only the base-config subtree per projection
   (not the whole doc + re-`ParseConfig`) and index selectors by label value for
   O(1) firing-signature lookup. Turns the single-section `O(n²)` into ~`O(n)`. Guard
   with `ProjectionSetEqualsFullEnumeration` + the scale suite. See Complexity §.

## Reviewer findings already addressed

- Alias nodes silently dropped → fixed (`IsAlias()/ResolveAlias()` in
  `CollectScalarLeaves`; both engine and oracle).
- Presence under untagged wholesale-replace → `ReplaceRoots` deletion modeling in
  `EnumerateRealizableAssignments`.
- Incompatibility pruning ballooned enumeration (13.8s) → removed from hot path
  (now sound over-approx; see Open #1).
- Joint transform projection was multiplicative `(T+1)(R+1)` → switched to
  per-section (additive). Benchmark `PerSectionIsAdditiveNotMultiplicative`.
- Enum-only coercion missed non-enum type errors → replaced with per-section real
  converter (`YamlToProto` preTransform=false originally, now full transform).
- `SelfManagementConfig` + ephemeral input keys read by transform but unmarked →
  marked / descriptor-derived.
- `CastRobust=true` makes legacy silently coerce bad int/bool (no throw) — A⁺
  matches via the same converter (tests assert agreement, not unconditional
  reject).
