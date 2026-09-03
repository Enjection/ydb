# Thoughts: from "path footprint" to "replay into another database"

Status: THOUGHTS (2026-09-03), not executed. Companion to `report.md`.
Written after the questions: how do we map back to the wire proto, can we
rewrite for a relocated database, can it be O(1), does by-id become by-path,
and does all of that give the same state on replay.

There are two different completeness claims. Keep them apart.

| claim | meaning | status |
|---|---|---|
| **P: path-addressing completeness** | every string or id that `Propose()` interprets as a path is known, attributable to its proto field, canonicalizable to a name, and relocatable | close; measured below |
| **S: state completeness on replay** | streaming the patched requests into another database yields the same scheme state | not achievable by extraction alone; needs completion filtering, reconciliation, and a definition of "same" |

---

## 1. Where P stands today

Evidence chain: S1 opened 75 of 136 op types against source, the rest rest on
the two dominant shapes by convention; S3 exercised 82 of 136 op types (as
request or part) through 561 tests; S5/S6 measured 430 of 462 non-noise path
shapes reproduced, 64 of 67 observed part op types gap-free.

A direct proto-field audit (every `string` field whose name contains
Path/Name/Dir/Table/From in a message reachable from `TModifyScheme`, checked
for a mention in the extractor) found **7 fields that `Propose()` resolves and
the table does not list**. Each is a one-line table addition:

| field | resolved at | role |
|---|---|---|
| `CreateTable.CopyFromTable` (row table) | `schemeshard__operation_copy_table.cpp:568,978` | Source |
| `CreateColumnTable.CopyFromTable` | olap create path | Source |
| `CopySequence.CopyFrom` | `schemeshard__operation_copy_sequence.cpp:579` | Source |
| `AlterTable` column `DefaultFromSequence` | `schemeshard__operation_alter_table.cpp:665` (absolute or child of the table) | Dependency |
| `CreateReplication/AlterReplication` `Target[].DstPath`, `DirectoryPath` | `..._create_replication.cpp:80,91`, `..._alter_replication.cpp:57` | Dependency (local side only; `SrcPath` is on the remote cluster and must NOT be rewritten) |
| `AlterPersQueueGroup.OffloadConfig.IncrementalBackup.DstPath` | `schemeshard__operation_alter_pq.cpp:328` | Dependency |
| `AlterContinuousBackup.TakeIncrementalBackup.DstPath` | `..._alter_continuous_backup.cpp:128` (split child under WorkingDir) | Target |

Why S5's 430/462 did not show them: the coverage compared path *shapes* with
fixture names replaced by placeholders, so a copy source `/MyRoot/<seg1>`
is indistinguishable from the target `/MyRoot/<seg1>`. Shape comparison
catches missing *kinds* of paths, not a missing second path of the same shape.

Remaining risk after adding these seven: op types that no test exercises and
no one opened (roughly 35, almost all plain `WorkingDir + Name` creates and
drops). That risk is retired by the enforcement loop in §3, not by more
reading.

**Answer to "is 100% close":** yes for P. Seven known one-line misses, a
mechanical audit that finds this class, and an oracle that finds the other
class. With both wired into the unit-test suite, P becomes a maintained
invariant rather than a snapshot.

## 2. The three transformations that P enables

1. **Map back to the wire field.** Replace `TString FieldPath` with an enum
   (`EPathField`, ~100 values), `TStringBuf` values into the request proto, and
   constexpr arrays for name/kind/role. Setter is a `switch` on the enum with
   the generated accessor. O(1), no reflection, no allocation. Better: drive
   extractor, rewriter, name table, and completeness test from one X-macro list
   per op type so they cannot drift.

2. **Canonicalize by-id to by-path.** Seven id fields, six name forms:
   `Drop.Id` → `WorkingDir`+`Drop.Name`; `AlterTable.PathId`/`Id_Deprecated`,
   `AlterPersQueueGroup.PathId`, `AlterBlockStoreVolume.PathId`,
   `AlterReplication.PathId` → `WorkingDir`+`Name`;
   `SplitMergeTablePartitions.TableOwnerId/TableLocalId` → `TablePath`.
   Must run on the owning SchemeShard (the id means nothing elsewhere) and on a
   copy (`Propose()` must see the client's request). Unknown id → the op is
   rejected anyway; emit an "untransformable" marker. `ApplyIf` has no name
   form: strip or re-derive on the consumer.

3. **Relocate.** With `RelPathToDatabase` and `WorkingDirRelToDb` in hand:
   `WorkingDir := newDb + WorkingDirRelToDb`; `Absolute` values
   `:= newDb + RelPathToDatabase`; `PathUnderWorkingDir` values only when they
   start with `/`; leaves untouched. Apply to the **original** request (H1),
   never to derived parts; the target regenerates parts and auto-MkDirs.
   Top-level `DatabaseName`, owner, token, and `ApplyIf` are consumer policy.

## 3. Enforcement loop for P (turns a snapshot into an invariant)

- **Proto-field audit as a unit test.** Walk `TModifyScheme` descriptors
  recursively; every string field whose name matches the path heuristic must be
  either in the table or in an explicit "not a path" allowlist (`FamilyName`,
  `KeyColumnNames`, `SchemaPresetName`, `ValueParamName`, ...). A new proto
  field fails the test until someone classifies it.
- **Oracle diff as a unit test.** Keep the S3 `TPath` recorder behind a test
  hook; for each `ut_*` run, assert every `Resolve/Child/Dive/Init` during a
  part's `Propose()` is a prefix-or-equal of some footprint entry of that part
  (parents and root excluded). A new resolution site fails the test.
- **Verb test** (already in the suite): op type reads the submessage with the
  matching verb.

## 4. Why S does not follow from P

- **Accepted is not committed.** The hook fires at end of Propose; ops can be
  aborted later (coordinator, shard failure, `AbortUnsafe`, force drop). Cut
  the stream at Done, or reconcile.
- **Physical layout is target-decided.** Split/merge boundaries are data-driven
  and issued internally; shard counts, channel/storage-pool bindings, tablet
  ids, followers come from target config. "Same" must mean "same
  `DescribePath` minus physical fields".
- **Non-deterministic rewrites.** `BackupBackupCollection.TargetDir` is stamped
  with `Now()`; continuous-backup streams carry timestamp names;
  `CreateIndexedTable` mutates the request (fulltext rowid) before splitting.
- **Internal operations.** SchemeShard synthesizes requests (backup collection
  → ConsistentCopyTables, index build sub-ops, splitter). Stream originals only,
  never derived ones. Target derives from *its* state; one divergence
  compounds.
- **Environment-dependent acceptance.** Feature flags, limits, quotas,
  `ApplyIf`. Accepted at source may reject at target; extraction cannot
  predict it.
- **Data-dependent ops.** CopyTable, TruncateTable, index build,
  RestoreBackupCollection produce schema plus data.
- **Principals.** Owners, ACL SIDs, `AlterLogin` users must exist on the
  target with the same meaning.

Achievable guarantee: replaying canonicalized, relocated, completion-filtered
original requests in transaction order reproduces the same **logical** scheme
tree, provided the target starts identical and accepts every operation.

## 5. Experiment to quantify S (proposed, not run)

Reuse the S3 harness: capture original `TEvModifySchemeTransaction`s from the
six suites, replay them through canonicalize+relocate into a second
`TTestEnv` under a different database path, and diff the two `DescribePath`
trees with physical fields masked. Bucket every divergence into the §4 classes.
The bucket sizes are the honest completeness statement for S. Estimated cost:
one day, because the recorder, harness, and normalizer already exist.

## 6. Open decisions

1. Stream at Propose (what the colleague asked) or at Done (what replay
   needs)? Recommendation: record at Propose, publish at Done, keyed by txId.
2. Is per-part attribution enough, or must H1 (original-request) attribution
   be added? Recommendation: add H1 (~10 lines); it is required for relocation
   anyway.
3. Who owns relocation: consumer-side reflection-free rewriter (recommended)
   or the existing `TSchemeTxTraits::NeedRewrite` seam inside SchemeShard?

## 7. Read/write sets without MVCC, and what MVCC would change

- **Write set exists today.** `TMemoryChanges` is the Propose undo log:
  `Paths`, `Tables`, `Indexes`, `CdcStreams`, `Sequences`, `Shards`, ...
  stacks of grabbed elements. Recording stack sizes before each part's
  `Propose()` and reading the new entries after yields the exact per-part
  in-memory write set, cascades included. `TSideEffects::PublishPaths` adds
  the new versions. ~15 lines in the hook. Ops with `IsUndoChangesSafe() ==
  false` (direct `GetDB`) need a "may be incomplete" bit.
- **Read set has one choke point.** `TPath::Dive` / `TPath::Init`. The S3
  recorder proved a per-activation logger there captures ancestor chains and
  state-derived dependencies across 561 tests with no breakage. ~50 lines.
  It records resolutions, not versioned reads, so it can inform but not
  validate; ancestor noise needs classification.
- **MVCC/OCC** would make both sets first-class transaction artifacts
  (complete by construction, rejected transactions included, versions as a
  causal clock for a change feed). It is the whole-SchemeShard rewrite: every
  `Propose()` reads `PathsById` directly via `TPath`; concurrency today is
  pessimistic (`PathState`, lock paths) plus client-side `ApplyIf`. Even then
  the field-attribution table is still required: sets carry path ids, not
  proto fields. SchemeBoard already publishes a versioned change feed of path
  elements; what it lacks is intent, attribution, and rejected operations.

## 8. Proto annotations as the source of truth

- Field options: `[(ss.path) = {kind: LEAF|PATH|ABSOLUTE|SIBLING_LEAF|BY_ID,
  role: TARGET|SOURCE|PARENT|DEPENDENCY, base: "TablePath", key_is_leaf:
  true}]`, `[(ss.not_a_path) = true]` for the rest. A descriptor-walk test
  makes every unannotated string field a failure at the point it is added.
- Enum-value options on `EOperationType`: `{submessage: "Drop", cascade:
  true, split_table_name: true, implicit: "..."}`. This is where the
  op-level facts live that per-field annotations cannot express: which
  submessage is active (`CreateIndexedTable`, `AlterColumnTable` vs
  `AlterTable`), per-op resolution overrides (D7), `Implicit` markers.
- Rules that stay in code (3-4): id-vs-name precedence (or
  `id_alternative_of: "Name"`), the `AlterTable.Name` fallback, leading-slash
  absolute detection.
- No runtime reflection on the Propose path: compile annotations once at
  static init (or codegen) into the enum-indexed tables. Cheapest first step:
  keep the hand table, add a test asserting it equals the annotations.
- Costs: `flat_scheme_op.proto` edits relink ~660 test modules; extension
  number and `ss_annotations.proto` needed; derived parts reuse the same
  messages so they are covered for free.
- Alternative for the D7 class: make `Propose()` resolve `AtTable` table
  names uniformly instead of encoding the inconsistency in annotations.
