# SchemeShard "path footprint" extraction — research plan

Status: EXECUTED (2026-09-03). Result in .omc/research/schemeshard-path-footprint/report.md; prototype on branch feat/schemeshard-path-footprint (uncommitted). Post-execution caveats and follow-ups: §8.
Research session: `.omc/research/schemeshard-path-footprint/`

## 0. Problem (as stated by the schema-CDC colleague)

For **every** scheme operation, produce a normalized list of the paths the
operation tried / intends to touch:

| field | meaning |
|---|---|
| `Id` | `TPathId` (invalid when the path does not exist yet / never existed) |
| `AbsPath` | `/Root/db/dir/table` |
| `RelPathToParent` | leaf name |
| `RelPathToDatabase` | `dir/table` (database = `TPath::GetDomainPathString()`) |
| `RelPathToWorkingDir` | value as it appeared relative to `TModifyScheme.WorkingDir` |
| `WorkingDirRelToDb` | `WorkingDir` expressed relative to the database |
| `ProtoRef` | pointer back to the field(s) of the **original** request proto that carried the path |

Timing: before Propose, or at the end of Propose (accepted **or rejected**),
but never after execution starts.

Hard constraint: **surgical**. No rewrite of SchemeShard, no touching the 82
`Propose()` implementations or the 233 `TPath::Resolve/Child` call sites.

## 1. Scouting findings (verified in tree, 2026-09-02)

Files: `ydb/core/tx/schemeshard/`

1. **There is already a per-op-type proto→paths switch**:
   `ExtractChangingPaths(const TModifyScheme&)` in
   `schemeshard_audit_log_fragment.cpp:327` (136 `case`s, one per
   `EOperationType`, `Y_ABORT` on a missing case so it is kept complete).
   It returns plain `TVector<TString>`, no ids, no roles, no proto mapping.
   It is **inconsistent**: 102 cases join with `WorkingDir`, 23 push the raw
   field (Move*, TruncateTable, StreamingQuery, ApplyIndexBuild joins
   `TablePath+IndexName`, RestoreMultipleIncrementalBackups ...). Whether those
   raw fields are really absolute must be checked per case (possible latent
   audit bugs). Called from `schemeshard_audit_log.cpp:108,186` only.

2. **Propose driver** (`schemeshard__operation.cpp`):
   `TTxOperationPropose::Execute` → schema-rewrite → per transaction
   `TOperation::SplitIntoTransactions` (auto-generates `MkDir`s for
   intermediate dirs, line 908) → `ConstructParts` (goes through
   `AppData()->SchemeOperationFactory->MakeOperationParts`, an existing
   injection seam, `schemeshard_operation_factory.h`) →
   `TSchemeShard::ProcessOperationParts` (line 97) loops `part->Propose(owner,
   context)` and classifies the response (Done / ConditionalAccepted /
   Accepted / rejected → `AbortOperationPropose`). Audit log is emitted at
   line 474 with the *original* record and the final response.
   **This loop is the single natural hook point**: every part, its
   `TModifyScheme` (`part->GetTransaction()`), and its Propose result are
   visible in one place.

3. **Compound operations build derived protos before Propose**: e.g.
   `CreateCdcStream` (`schemeshard__operation_create_cdc_stream.cpp:643-790`)
   resolves `workingDirPath/tablePath/streamPath` at construction time and
   emits parts via `TransactionTemplate(absoluteWorkingDir, opType)`
   (`schemeshard__operation_part.h:804`). So the per-part protos have an
   **absolute `WorkingDir`** and a leaf `Name` — already close to normalized.
   The catch: mapping a derived part back to the original request field is
   indirect (part → originating transaction index → field).

4. **Ground truth after Propose exists for accepted parts**:
   `TTxState::TargetPathId/SourcePathId` (`schemeshard_tx_infly.h:79`),
   `TMemoryChanges::GrabPath/GrabNewPath/GrabDomain` (88 call sites,
   `schemeshard__operation_memory_changes.h:105`), and
   `TSideEffects::PublishToSchemeBoard` (`schemeshard__operation_side_effects.h:95`).
   These are "what was actually touched" and can serve as a **test oracle**,
   but they do not exist for rejected proposals and carry no proto mapping.

5. **Normalization primitives already exist on `TPath`** (`schemeshard_path.h`):
   `Resolve`, `ResolveWithInactive`, `Init(pathId)`, `PathString()`,
   `Parent()`, `LeafName()`, `GetDomainPathString()`, `GetPathIdForDomain()`,
   `FirstExistedParent()`, `IsResolved()`. Nothing new is needed for layer 2.

6. **Proto shape**: `TModifyScheme` has ~110 fields; ~168 path-ish string
   fields (`*Path|Name|Dir|Prefix|Src|Dst*`) across `flat_scheme_op.proto`.
   Sub-messages are shared across op types (`TDrop` by ~20 drop ops,
   `TTableDescription` by Create/Alter, `TMove` by 3 ops). Custom field
   options have precedent (`config.proto`, `feature_flags.proto`), but editing
   `flat_scheme_op.proto` relinks ~660 test modules (see
   `.omc/analysis/proto-blast-radius-plan.md`) — acceptable once, not for
   iteration.

7. **Test vehicles**: `ut_helpers/test_env.h` (`TTestEnv`, `TTestEnvOptions`),
   `ut_base`, `ut_cdc_stream`, `ut_index_build`, `ut_move`,
   `ut_backup_collection`, `ut_auditsettings` (audit-log output assertions —
   parity harness for finding 1).

## 1a. Known offenders (must be first-class test cases, not afterthoughts)

Each of these breaks the naive "WorkingDir + Name" model. Verified locations.

| Offender | What breaks | Where |
|---|---|---|
| **CopyTable / ConsistentCopyTables** — many-to-many | `TConsistentTableCopyingConfig.CopyTableDescriptions[]` each with absolute `SrcPath`/`DstPath`; every item may additionally **create/drop/rotate CDC streams on the source** (`CreateSrcCdcStream`, `DropSrcCdcStream`, `IndexImplTableCdcStreams`, `IndexImplTableDropCdcStreams` maps keyed by index name) → touched paths include `src/<stream>`, `src/<index>/indexImplTable/<stream>` and the PQ group under the stream. Field paths need repeated index and map key. | `flat_scheme_op.proto:1465-1490`, `schemeshard__operation_copy_table.cpp`, `..._consistent_copy_tables.cpp` |
| **MoveTable / MoveIndex / MoveSequence** | `TMove.SrcPath/DstPath` are absolute (not under WorkingDir). `TMoveIndex.SrcPath/DstPath` are **leaf names relative to `TablePath`** (parent table), and move cascades to children (indexes, impl tables). Move also reaches "inactive" paths via `TPath::ResolveWithInactive` (dst may be a just-dropped name). | `schemeshard__operation_move_table.cpp:818-904`, `..._move_index.cpp`, `..._move_table_index.cpp` |
| **Drop by id** | `TDrop.Id` (local path id) bypasses `WorkingDir/Name` entirely: `RmDir`, `DropTable`, `DropPQ`, `DropView`, ... do `drop.HasId() ? TPath::Init(MakeLocalId(id)) : Resolve(WorkingDir).Dive(Name)`. `AlterTable` likewise accepts `PathId`/`Id_Deprecated`. The extractor must resolve **id → path** to fill `AbsPath`, and mark `ProtoRef = "Drop.Id"`. Drop also cascades to the full subtree (indexes, cdc streams, PQ). | `schemeshard__operation_rmdir.cpp:32`, `..._alter_table.cpp:607`, 20 drop/alter files (see S1) |
| **Backup* family under `.backups/collections`** | `BackupBackupCollection`, `BackupIncrementalBackupCollection`, `RestoreBackupCollection`, `CreateLongIncrementalBackupOp`, `CreateFullBackupOp`: paths are computed from `<domain>/.backups/collections/<name>/<TargetDir>/<relativeItemPath>` plus the collection's **stored entry list** (not in the request); CreateTable has a special branch for parents under that dir. Static extraction can only yield the collection path; the concrete table/stream set is runtime-derived (`Implicit`). | `schemeshard__backup_collection_common.cpp:53`, `..._backup_backup_collection.cpp:43-168`, `..._create_table.cpp:480` |
| **Index build family** | `ApplyIndexBuild`/`CancelIndexBuild`/`InitiateBuildIndex*`: `TablePath` absolute + `IndexName` leaf under it; impl tables under the index are implicit. | `schemeshard_audit_log_fragment.cpp:455,482` |
| **CreateIndexedTable / CreateCdcStream / AlterTable with sequences** | One request → N parts with derived protos (table, index, impl table, lock, PQ, stream-at-table, drop-sequence). Children are never named in the request. | `..._create_cdc_stream.cpp:643-790`, `..._alter_table.cpp:875` |
| **Ops with no path or a runtime-only path** | `AlterLogin`, `IncrementalRestoreFinalize`, `ModifyACL` (path only), `AlterUserAttributes`, `UpgradeSubDomain`, `ChangePathState`. | `ExtractChangingPaths` cases |
| **Auto-generated MkDirs** | `SplitIntoTransactions` turns `CreateTable WorkingDir=/R Name=a/b/t` into `MkDir /R a`, `MkDir /R/a b`, `CreateTable /R/a/b t`. | `schemeshard__operation.cpp:908` |
| **"Raw" cases in the audit extractor** | 23 cases push the field without joining `WorkingDir` (StreamingQuery, TruncateTable, Move*, RestoreMultipleIncrementalBackups, ...). Verify each is truly absolute; some may be audit bugs. | `schemeshard_audit_log_fragment.cpp:327-750` |

Consequence for the design: `EKind` needs at least `LeafUnderWorkingDir`,
`PathUnderWorkingDir`, `Absolute`, `LeafUnderSibling(field)`, `ById(field)`,
`Implicit`; `ProtoRef` needs repeated index and map key; and the product must
combine a static layer (request fields) with a per-part layer (derived
protos) plus a post-Propose `TTxState` layer for `Implicit` entries.

## 2. Candidate designs (to be compared, not all built)

Layered; each layer independently small.

### Layer 1 — static extractor: `TModifyScheme` → `TVector<TPathRef>`

`TPathRef { TString Raw; EKind Kind; ERole Role; TString FieldPath; int RepeatedIndex; }`

- `EKind`: `LeafUnderWorkingDir` (Name), `PathUnderWorkingDir` (may contain
  `/`, may be absolute), `Absolute`, `LeafUnderSibling` (IndexName under
  TablePath), `Implicit` (paths the op will touch that are not in the proto).
- `ERole`: `Target`, `Source`, `Parent`, `Dependency` (e.g. table of a cdc
  stream), `Child` (cascaded).
- `FieldPath`: protobuf field path string, e.g. `"MoveTable.SrcPath"`,
  `"CreateConsistentCopyTables.CopyTableDescriptions[2].DstPath"` → this is the
  "map back to the request proto" requirement; resolvable through
  `google::protobuf::Reflection` at runtime and human-readable.

Options:
- **1a. Typed table keyed by op type** (recommended starting point): a
  `static const THashMap<EOperationType, TVector<TFieldSpec>>` where
  `TFieldSpec = {fieldPath, Kind, Role}`; a tiny reflection walker reads the
  values. No proto change. Completeness enforced by a unit test iterating
  `EOperationType_descriptor()` (mirrors the `Y_ABORT` switch). Then rewire
  `ExtractChangingPaths` to consume it (dedupe + parity test via
  `ut_auditsettings`).
- **1b. Proto custom field option** `[(NKikimrSchemeOp.PathRef) = {...}]` on
  the ~168 fields + generic walker. Most declarative, but per-op semantics of
  shared sub-messages and the proto blast radius make it a *phase 2* candidate
  only if 1a shows the table is unmaintainable.
- **1c. Extend the existing switch in place** (add roles/kinds to
  `ExtractChangingPaths`). Cheapest, but keeps the audit-log coupling and
  the 136-case switch as the source of truth. Fallback if 1a is rejected.

### Layer 2 — normalizer: `TPathRef + WorkingDir + TSchemeShard*` → `TTouchedPath`

Pure function over `TPath`: resolve, compute `Id` (or invalid + nearest
existing parent id), `AbsPath`, leaf, rel-to-db, rel-to-workingdir,
`WorkingDirRelToDb`, `Exists`, `IsUnderOperation`. One new file
`schemeshard_path_footprint.{h,cpp}`, ~200 lines, no existing file changed.

### Layer 3 — hook placement (choose one, keep the others as oracles)

- **H1 original-request level**: in `TTxOperationPropose::Execute` right after
  rewrite, over `record.GetTransaction()` — exact proto mapping, but misses
  generated MkDirs and compound-op children.
- **H2 per-part level** (recommended): in `ProcessOperationParts` before/after
  `part->Propose(...)`, over `part->GetTransaction()`; tag each footprint entry
  with `(originalTxIndex, partIndex, proposeStatus)`. Covers auto-MkDirs and
  derived parts. Mapping back = derived field path + originating tx index.
- **H3 factory seam**: wrap `IOperationFactory::MakeOperationParts` with a
  decorator that records footprints per constructed part — zero edits to the
  Propose driver, but no access to the Propose status.
- **H4 dynamic recorder** (experiment/oracle only): a recorder on
  `TSchemeShard` activated during Propose that logs every
  `TPath::Resolve/Child/Dive` → "tried to touch" for rejected ops too. Noisy,
  role-less; use to *measure* the gaps of layer 1, not as the product.

Storage: `TOperation::PathFootprint` (in-memory, per txId, mirrors the
already in-memory `TOperation`), plus an observer callback so schema-CDC can
consume it at end of Propose without SchemeShard knowing about CDC.

## 3. Open scope decisions (answer with the colleague; record here)

1. Granularity: request-named paths only (level 0), plus generated parts and
   auto-MkDirs (level 1), or full cascaded subtree of drops/moves
   (indexes, impl tables, cdc streams, PQ groups; level 2)? Plan assumes
   **level 1 in the product, level 2 measured as a gap report**.
2. Is per-part mapping (derived proto + originating tx index) acceptable as
   "map back to the original protobuf", or must every entry point at a field of
   the *client's* request? (Compound ops make the latter impossible for
   implicit children — they simply are not in the request.)
3. Rejected proposals: is "resolved as far as possible" (id of nearest
   existing ancestor + intended leaf) enough?
4. Ops with runtime-derived paths (`BackupBackupCollection`,
   `RestoreBackupCollection`, `IncrementalRestoreFinalize`, index-build
   family): report `Implicit` entries after Propose from `TTxState`, or leave
   them out of the static layer?

## 4. Overnight research stages (max 5 agents concurrently)

All commands run from repo root. Build/test per user rules:
`hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/<ut> -F '*Filter*' 2>&1 | tail -50`

### S1 — Field inventory per op type (haiku/sonnet, read-only)
Script over `schemeshard__operation_*.cpp`: for each `Create*` factory and
`Propose`, collect `Transaction.Get<Sub>()...Get<Field>()` chains that flow into
`TPath::Resolve/Child/Dive/ResolveWithInactive/JoinPath`. Output
`findings/s1-field-inventory.md`: table `opType → {field path, kind, role}`.
Cross-check against `ExtractChangingPaths`; list mismatches and the 23
"raw" cases with a verdict (absolute? bug?).

### S2 — Compound/derived op map (sonnet, read-only)
For every op whose `ConstructParts` returns >1 part or runs
`SplitIntoTransactions`-generated MkDirs: list derived part op types and how
each derived `WorkingDir/Name` is computed from the original fields.
Output `findings/s2-derived-parts.md`. Deliverable: the exact
`(originalTxIndex, partIndex) → fieldPath` mapping rule for H2.

### S3 — Dynamic oracle experiment (sonnet, writes scratch code, NOT committed)
Temporarily instrument `TPath::Resolve/ResolveWithInactive/Child/Dive` (or a
recorder set on `TSchemeShard` around `ProcessOperationParts`) to log
`opType, partOpType, status, resolved abs path, isResolved` to a file.
Run `ut_base`, `ut_cdc_stream`, `ut_index_build`, `ut_move`,
`ut_backup_collection`, `ut_consistent_copy_tables`. Aggregate per op type
the distinct path *shapes* touched. Output `findings/s3-dynamic-oracle.md`
plus the raw dump in `findings/s3-raw/`. Then `git checkout` the instrumented
files (verify `git status` clean except `.omc/`).

### S4 — Prototype layer 1a + layer 2 + H2 hook (opus, writes code on a branch)
Branch `feat/schemeshard-path-footprint` from `main`. New files
`schemeshard_path_footprint.{h,cpp}`; table seeded from S1; hook in
`ProcessOperationParts`; `TOperation::PathFootprint`; observer callback stub.
Rewire `ExtractChangingPaths` to the extractor **behind an assert-parity
check** (old vs new) rather than replacing it outright. Unit test
`ut_path_footprint` (new dir, `ut_helpers` `TTestEnv`) covering, at minimum,
every row of §1a: MkDir, CreateTable with intermediate dirs (auto-MkDir),
CreateIndexedTable, CreateCdcStream, MoveTable + MoveIndex (relative to
parent table), DropTable **by id** and by name with index+cdc children,
ConsistentCopyTables with 2+ items and `CreateSrcCdcStream` /
`IndexImplTableCdcStreams`, BackupBackupCollection under
`.backups/collections`, ApplyIndexBuild, a rejected CreateTable (parent
missing), ModifyACL, AlterLogin (empty footprint). Also a completeness test over
`EOperationType`. Keep diff to existing files < ~80 lines.
Do not commit; leave the working tree on the branch with a summary diffstat in
`findings/s4-prototype.md`.

### S5 — Cross-validation + write-up (opus, depends on S1–S4)
Compare S4 output against S3 oracle and against `TTxState` target/source ids
for accepted ops; produce gap list by op type. Write `report.md`:
recommendation (design + hook), diffstat, known gaps, and answers needed for
§3. Include the build/test commands used and their tail output.

Parallelism: S1, S2, S3 run concurrently; S4 starts after S1 (needs the
table); S5 last.

## 5. Acceptance criteria for the overnight result

- A design memo a SchemeShard maintainer can read in 5 minutes with the
  chosen hook point and the diff size to existing files.
- A compiling prototype on a branch with green new tests and green
  `ut_auditsettings` (parity), `ut_base` smoke.
- A per-op-type coverage table: static extractor vs dynamic oracle, with every
  gap classified as (a) implicit child, (b) runtime-derived, (c) table bug.
- A list of the questions in §3 that still block, with the recommended answer.

## 6. Non-goals

- Persisting the footprint (in-memory only, same lifetime as `TOperation`).
- Changing any `Propose()` implementation or `TPath` semantics.
- Editing `flat_scheme_op.proto` in the prototype (1b is deferred).

## 7. Launch

`/oh-my-claudecode:research AUTO: execute .omc/plans/schemeshard-path-footprint-research-plan.md stages S1-S5`

## 8. Caveats and follow-up research (post-execution discussion, 2026-09-03)

Detailed reasoning lives in
`.omc/research/schemeshard-path-footprint/thoughts-replay-completeness.md`.
This section is the checklist so nothing is rediscovered later.

### 8.1 Two completeness claims, never conflate them

| claim | status |
|---|---|
| **P (paths)**: every string/id `Propose()` interprets as a path is known, attributable, canonicalizable, relocatable | close: 430/462 shapes measured, plus **7 known one-line field misses** (§8.3); becomes an invariant with the enforcement loop (§8.4) |
| **S (state on replay)**: streaming patched requests reproduces the same state elsewhere | not achievable by extraction alone (§8.7) |

### 8.2 What the current branch extracts, exactly

1. Paths named in the user request proto: **yes** (after §8.3).
2. Paths named in derived parts (index impl tables, CDC streams, PQ groups,
   locks, auto-MkDirs): **yes**, attributed to the derived part's proto.
3. Paths touched in memory that no proto names: **partial**.
   - Writes by single-part subtree walks (force drops, `MoveTableIndex`
     children, continuous-backup stream beside a table, backup-collection
     fan-out): only `Implicit` anchors.
   - Reads (ancestor chain for ACL/limit checks, dependencies enumerated from
     state): only nearest existing parent id and database id.
4. Post-Propose execution-state writes: **out of scope** (needs a Done-time
   hook).

### 8.3 Known missing path fields (each a one-line table addition)

Found by a direct proto-field audit; invisible to the S5 shape comparison
because a copy source looks like a target (`/MyRoot/<seg1>`).

| field | resolved at | role |
|---|---|---|
| `CreateTable.CopyFromTable` | `schemeshard__operation_copy_table.cpp:568,978` | Source |
| `CreateColumnTable.CopyFromTable` | olap create path | Source |
| `CopySequence.CopyFrom` | `schemeshard__operation_copy_sequence.cpp:579` | Source |
| AlterTable column `DefaultFromSequence` | `schemeshard__operation_alter_table.cpp:665` | Dependency |
| Replication `Target[].DstPath`, `DirectoryPath` | `..._create_replication.cpp:80,91`, `..._alter_replication.cpp:57` | Dependency (**`SrcPath` is remote: never rewrite**) |
| `AlterPersQueueGroup.OffloadConfig.IncrementalBackup.DstPath` | `schemeshard__operation_alter_pq.cpp:328` | Dependency |
| `AlterContinuousBackup.TakeIncrementalBackup.DstPath` | `..._alter_continuous_backup.cpp:128` (split child) | Target |

Residual after these: ~35 op types neither exercised by tests nor opened by
a person (almost all plain `WorkingDir + Name`).

### 8.4 Enforcement loop (turns P from a snapshot into an invariant)

- Descriptor-walk unit test: every path-like string field reachable from
  `TModifyScheme` is in the table or in an explicit not-a-path allowlist.
- `TPath` recorder behind a test hook (the S3 oracle): every
  `Resolve/Child/Dive/Init` during a part's `Propose()` must be covered by that
  part's footprint (ancestors excluded). A new resolution site fails a test.
- Verb test (exists): op type reads the submessage with the matching verb.
- Shape-only comparison is insufficient; both tests above are needed.

### 8.5 Wire mapping, O(1), no reflection

- Replace `TString FieldPath` with `enum EPathField` (~100 values),
  `TStringBuf` values into the request proto, constexpr arrays for
  name/kind/role. Setter = `switch` on the enum with generated accessors.
- Preferred: one X-macro list per op type driving extractor, in-place
  rewriter (`RewritePaths(tx, fn)`, no lookup at all), name table, and
  completeness test, so they cannot drift. Irregular shapes (~10) stay
  hand-written.
- Layer 2 stays O(depth) via `TPath::Resolve`; materialize strings lazily if
  consumers mostly need ids.

### 8.6 By-id canonicalization and relocation

- 7 id fields, 6 name forms: `Drop.Id`; `AlterTable.PathId`/`Id_Deprecated`;
  `AlterPersQueueGroup.PathId`; `AlterBlockStoreVolume.PathId`;
  `AlterReplication.PathId` (also Transfer); `SplitMergeTablePartitions.TableOwnerId+TableLocalId`
  (→ `TablePath`). Run on the owning SchemeShard, on a **copy** (never mutate
  the part's `Transaction`). Unknown id → op is rejected anyway; emit an
  untransformable marker.
- `ApplyIf` has no name form: strip or re-derive on the consumer.
- Relocation `/root/db1 → /root_new/dir/db2`: `WorkingDir := newDb +
  WorkingDirRelToDb`; `Absolute` values `:= newDb + RelPathToDatabase`;
  `PathUnderWorkingDir` only when leading `/`; leaves untouched. Apply to the
  **original request** (needs the H1 pass, ~10 lines in `IgniteOperation`),
  never to derived parts. Top-level `DatabaseName`, owner, token: consumer
  policy.
- Existing in-tree seam if SchemeShard-side rewriting is ever wanted:
  `TSchemeTxTraits::NeedRewrite` + `Rewrite(TTag, TTxTransaction&)`
  (`schemeshard__op_traits.h`, used by two backup ops).

### 8.7 Why S does not follow from P (replay caveats)

Accepted ≠ committed (hook is at Propose; cut at Done or reconcile);
physical layout is target-decided (splits, shards, pools, tablet ids);
non-deterministic rewrites (`TargetDir` from `Now()`, timestamped streams,
fulltext rowid auto-provisioning); internal ops must not be streamed (double
apply), and target derivation depends on target state; environment-dependent
acceptance (flags, limits, quotas, `ApplyIf`); data-dependent ops (Copy,
Truncate, index build, Restore); principals (owners, SIDs, logins).
Achievable guarantee: same **logical** tree given identical start and full
acceptance. Proposed experiment: replay the six suites' original requests
through canonicalize+relocate into a second `TTestEnv` and diff
`DescribePath` with physical fields masked (~1 day; harness exists).

### 8.8 Read/write sets: cheap now vs. MVCC later

- Write set at Propose is available **by construction** today:
  `TMemoryChanges` (undo log; `Paths`, `Tables`, `Indexes`, `CdcStreams`,
  `Sequences`, `Shards`, ... stacks) diffed before/after each part's
  `Propose()`, plus `TSideEffects::PublishPaths` (paths with new versions).
  ~15 lines in the hook. Caveat: ops with `IsUndoChangesSafe() == false`
  (direct `GetDB`) must carry a "write set may be incomplete" bit.
- Read set has a single choke point: `TPath::Dive`/`TPath::Init` (~50-line
  per-activation recorder; the S3 oracle proved it). Noisy (ancestor checks),
  needs classification; sees resolutions, not versioned reads.
- An MVCC/OCC turn would make read+write sets first-class transaction
  artifacts (complete by construction, rejected txs included, versions as a
  causal clock) but is the whole-SchemeShard rewrite; it still would not give
  proto-field attribution, so the table is needed in every design.
  SchemeBoard publications are already a versioned change feed of path
  elements, minus intent and rejected ops.

### 8.9 Proto annotations (option 1b revisited)

- Good fit for ~110 fields: `[(ss.path) = {kind, role, base, key_is_leaf}]`
  and `[(ss.not_a_path) = true]`; descriptor-walk test enforces coverage at
  the point a field is born.
- Per-field is not enough for: same field resolved differently per op
  (`CreateCdcStream.TableName` split in top-level/Alter/Rotate AtTable, not in
  Create/Drop AtTable, finding D7); which submessage is active
  (`CreateIndexedTable`, `AlterColumnTable` vs `AlterTable` fallback);
  conditional rules (id-vs-name, leading-slash). Put op-level facts on
  `EOperationType` enum-value options (`submessage`, `cascade`, split
  overrides, `Implicit` markers); express id-vs-name as
  `id_alternative_of`; keep 3-4 hand-written rules. Alternative for D7-class
  cases: make `Propose()` resolve uniformly (behavior change, separate
  decision).
- Never reflect on the Propose path: compile annotations once at static init
  (or codegen) into the enum-indexed tables of §8.5. Cheapest first step:
  keep the hand table and add a test asserting it equals the annotations.
- Costs: `flat_scheme_op.proto` edits relink ~660 test modules (batch them);
  custom options need an extension number and a small `ss_annotations.proto`.

## 9. Round 2 execution stages (from §8; one tree owner at a time, commit per stage)

Branch `feat/schemeshard-path-footprint` on fork `enjection`. Every stage ends
with `ut_path_footprint` green, a conventional commit, and a push. Sequential:
the same file is touched by most stages and parallel editors collided in
round 1.

| id | stage | source | commits |
|---|---|---|---|
| S7a | add the 7 missing path fields + pure tests | §8.3 | feat |
| S7b | descriptor-walk completeness test with not-a-path allowlist | §8.4 | test |
| S7c | Propose-time write set from `TMemoryChanges` diff + `PublishPaths`, `WriteSetMayBeIncomplete` bit | §8.8 | feat |
| S7d | H1 pass: footprint of each original request transaction with `OriginalTxIndex`, joined to parts | §8.6 | feat |
| S7e | `CanonicalizeToPaths` (by-id → by-path, on a copy) and `RelocatePaths` (database relocation) + tests | §8.6 | feat |
| S7f | O(1): `EPathField` enum, `TStringBuf` values, X-macro-driven extractor/rewriter/name table | §8.5 | refactor |
| S7g | replay experiment as a test: env A ops → canonicalize+relocate → env B, diff `DescribePath` masked | §8.7 | test |
| S7h | `TPath` read-set recorder behind a test hook + coverage assertion test | §8.4, §8.8 | feat/test |
| S7i | observer callback as the production channel, log demoted to DEBUG, computation gated | report §6.2 | feat |
| S7j | `ResolveWithInactive` for Move* via `part->GetOperationId()` | report §6.4 | fix |
| S7k | rewire `ExtractChangingPaths` onto the extractor (audit output changes for 8 buggy families) | report §6.3 | feat, separate decision |
| S7l | proto annotations | §8.9 | deferred: proto surface decision, relinks ~660 modules |
| S7m | `CanonicalizeToPaths` must return/update the footprint so `RelocatePaths` sees the canonicalized WorkingDir (defect found by S7g) | findings s7-round2 §S7g | fix |

Status 2026-09-03: S7a-S7j done and pushed (see draft PR Enjection/ydb#32); S7g done (zero divergences on the request-named subset; §4 classes untested by construction); S7m+S7k in progress; S7l deferred. Executed order differed from the table: S7f before S7e so the rewriter was written once on the enum.
