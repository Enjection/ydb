# Native backup and restore UID idempotency: TDD plan

Status: server/SDK implementation and local verification are ready for review. Controller integration remains outstanding; this document does not announce released support. See the [verification evidence and limits](native-backup-restore-idempotency-progress.md).

This revision follows the decision to extend the existing import/export UID convention and add request equality checking for native SQL operations. It supersedes the earlier independent key-registry design and the broader [original proposal](https://gist.github.com/Enjection/2db8a849b79f37b486dcc13445b38868) where they require independent terminal receipts, a 30-day retention guarantee, resolve-by-key, or cancellation fencing. Database incarnation and semantic SQL canonicalization remain out of scope.

## 1. Existing mechanism and intended reuse

The current server reads `OperationParams.labels["uid"]` through [GetUid](ydb/core/tx/schemeshard/schemeshard_xxport__helpers.cpp). Import/export create transactions look up an operation by UID before creating work, persist UID with the operation, and start execution after commit. Startup rebuilds the indexes from the operation tables. Forget cleanup removes the operation and its UID mapping.

Relevant precedents:

- [Export UID lookup and creation](ydb/core/tx/schemeshard/schemeshard_export__create.cpp), [import UID lookup and creation](ydb/core/tx/schemeshard/schemeshard_import__create.cpp).
- [Export persistence and removal](ydb/core/tx/schemeshard/schemeshard_export.cpp), [import persistence and removal](ydb/core/tx/schemeshard/schemeshard_import.cpp), [startup loading](ydb/core/tx/schemeshard/schemeshard__init.cpp).
- Existing `UidAsIdempotencyKey` tests in [ut_export](ydb/core/tx/schemeshard/ut_export/ut_export.cpp) and [ut_restore](ydb/core/tx/schemeshard/ut_restore/ut_restore.cpp).

This is an operation-specific pattern, not a generic deduplication service. Import/export currently return the existing operation without comparing the new body. Index-building UID handling instead returns `ALREADY_EXISTS`. Reuse the convention and durable admission pattern; preserve those existing APIs and their behavior. Add equality checking to the new native SQL support, without silently changing existing import/export or index semantics.

**Explicit boundary: leave existing import/export implementations as is.** Do not modify their public APIs, SDK UID settings, validation, request comparison, admission, persistence, replay, Forget, or cleanup. Use their code as a precedent and call existing helpers unchanged where appropriate. This work adds native SQL support; it does not generalize or refactor import/export into a new shared registry.

## 2. Native operation contract

- Support full `BACKUP`, incremental `BACKUP`, and `RESTORE` through Query Service. Extend APIs and SDKs additively; preserve unkeyed behavior and native operation ID formats.
- Accept exactly one supported executable statement, in NoTx mode, with supported SQL syntax and no bound parameters. Reject unsupported statements, batches, scripts, modes, and execution-affecting inputs absent from the compared identity before effects.
- Use one caller-supplied UID. Accept request metadata or a SQL option; if both are present, require exact equality. Never silently ignore a UID or downgrade to unkeyed execution.
- For these new native entry points, UIDs are case-sensitive, 1–256 ASCII bytes from `[A-Za-z0-9_.:-]`. No prefix is required; no trimming or normalization. Omitted means unkeyed; explicitly empty is invalid. Do not impose these new validation rules on existing import/export clients or globally change `GetUid` semantics.
- Compare exact original DDL bytes within the selected UID namespace. Whitespace, comments, quoting, options, and trailing semicolons participate in equality. A different body produces a conflict without changing the existing operation.
- Persist UID, original DDL, and recoverable native operation state in the same SchemeShard local propose transaction. Acknowledgement and native execution effects follow durable commit.
- Replay returns the original native operation ID; its current or terminal state remains available through the existing operation interface while the operation record exists. Success, failure, or cancellation alone does not release the UID.
- Deduplication lasts for the operation record's lifetime. Eligible Forget/cleanup removes both operation and UID mapping. After removal, the same UID may create new work, even if its DDL differs. There is no independent terminal receipt or fixed retention window.
- Authenticate and authorize replay as operation access. A UID is not a credential. No database incarnation field or retry guarantee across database deletion/recreation is introduced.

### UID scope

Follow the existing implementation pattern: **SchemeShard tablet + operation family + UID**. The UID index itself has no database, collection, user, or individual operation ID component.

For native operations, retain the earlier choice of three independent families: full backup, incremental backup, and restore. Their UID maps are separate from each other and from existing import/export maps. Same UID bytes can identify one operation in each family.

Each stored operation also retains its existing database/domain binding. After finding a UID, authorize access and check that binding before returning or comparing the operation. A UID already used by another resolved database on the same SchemeShard is a namespace collision, not an independent admission; return `ALREADY_EXISTS` without exposing the other operation. UIDs on different SchemeShard tablets are independent. Require valid authenticated database routing; do not copy permissive internal unresolved-domain lookup behavior into a public native entry point.

Examples:

```text
full backup, database A, uid=X, DDL=A → create operation 123
full backup, database A, uid=X, DDL=A → return operation 123
full backup, database A, uid=X, DDL=B → conflict; no work
incremental backup, database A, uid=X → independent operation, subject to prerequisites
full backup, database B on same tablet, uid=X → namespace collision
Forget/cleanup removes operation 123 → uid=X may subsequently admit a new full backup
```

## 3. API, SDK, and SQL transport

Use `uid` consistently as the submission identity. Where an entry point already has `OperationParams`, use the existing `labels["uid"]` convention. Query Service's `ExecuteQueryRequest` has no `OperationParams`, so it still needs an additive field; do not add unrelated operation controls merely to carry a UID.

Query Service additions (implemented in the worktree; existing values/tags remain unchanged):

```proto
enum ExecMode {
    // Existing modes, including EXEC_MODE_EXECUTE = 50, remain unchanged.
    EXEC_MODE_EXECUTE_WITH_UID = 60;
}

message ExecuteQueryRequest {
    // Existing fields retain their names, types, and tags.
    // Requires EXEC_MODE_EXECUTE_WITH_UID; the SDK selects it automatically.
    optional string uid = 16 [(Ydb.sensitive) = true];
}
```

The guarded mode requires `uid`, and metadata `uid` requires the guarded mode. This prevents an older endpoint from ignoring an unknown UID field and executing an unkeyed operation. SDKs preserve Parse/Validate/Explain rather than upgrading them to execution. SQL-only UID statements continue to use ordinary execution; older SQL parsers reject the unfamiliar syntax.

Tag 16 is occupied only by the current working tree's unreleased `idempotency_key` addition. Replace that draft field with `uid`, after verifying it has not been published and checking tag availability. Do not rename any released public field or repurpose a released tag. Apply the same compatibility check to draft SDK and SQL names.

Proposed Python sync/async pool use:

```python
result = pool.execute_with_retries("BACKUP `daily`;", uid="backup:019abc")
```

Proposed C++ setting: `TExecuteQuerySettings().Uid("backup:019abc")`.

Proposed SQL-only editor use:

```sql
BACKUP `daily` WITH (uid = 'backup:019abc');
BACKUP `daily` INCREMENTAL WITH (uid = 'backup:019def');
RESTORE `daily` WITH (uid = 'restore:019ghi');
```

These are additions for native execution, not claims about currently released Query SDK syntax. Existing Python import/export `.with_uid(...)` and their wire mapping remain unchanged. Use one effective UID internally; do not maintain separate UID and idempotency-key identities. Replace unreleased draft spellings and their tests. Retain aliases only if a published compatibility obligation is discovered, with equality required between all supplied forms.

For Table Service, reuse `ExecuteSchemeQueryRequest.operation_params.labels["uid"]` only if that entry point is explicitly implemented and advertised. Initial delivery targets Query Service; unsupported native SQL entry points must reject supplied UID syntax before effects. Do not claim generic UID enforcement across unrelated existing RPCs.

Request path:

```text
SDK request.uid / SQL WITH(uid=...) / supported OperationParams.labels["uid"]
  -> gRPC validation
  -> KQP serializable request and per-execution state
  -> TModifyScheme native UID metadata { Uid, OriginalDdl }
  -> TxProxy routing
  -> SchemeShard propose: lookup, compare, replay or atomically admit
```

Use the actual incoming query text as `OriginalDdl`; clients do not submit a second body or fingerprint. Keep UID and text outside reusable compiled plans. SchemeShard continues to execute the parsed operation. The initial comparison needs a stored DDL string, not canonical SQL or a canonicalization version. Future typed/batch identities require an explicit extension and must preserve the comparison rule of retained operations.

Moving a retry's UID from SQL to request metadata changes its original DDL and conflicts. Keep both UID and original text stable across retries. New logical operations can use either placement.

## 4. Storage and admission

Extend existing native operation records rather than introducing an independent `NativeOperationKeys` receipt table:

```text
FullBackups[operation_id]
  existing state, domain binding, owner, inputs, timestamps, outcome, ...
  Uid = "backup:019abc"
  OriginalDdl = "BACKUP `daily`;"

FullBackupsByUid["backup:019abc"] -> operation_id  // reconstructed in memory

Equivalent fields/indexes for incremental backup and restore.
```

Audit the creation timing of each native record. In particular, restore's UID and body must exist durably at initial admission, even if later restore execution currently creates other tracking rows. Move or extend the ordinary operation metadata creation as necessary; never start work and attach UID afterward. Persist the first admitted execution inputs and bind generated children to that operation ID.

Admission sequence:

1. Validate UID, request shape, authenticated database routing, and supported kind.
2. Look up the selected family's UID index before `IgniteOperation`, child expansion, new-work schema prerequisites, and capacity checks.
3. If found, authorize access and verify domain membership. Compare stored original DDL. Return the original native identity for equality, or an explicit conflict for differing text.
4. If absent, run ordinary native admission checks and stage operation metadata, UID/body, and the in-memory UID index together.
5. Include all added metadata in the existing redo accounting. Roll back the operation and UID index together on proposal or redo rejection.
6. Commit before replying or activating execution. Reconstruct UID indexes on reboot and resume committed native work without client resubmission.

Full backup already stages `PersistTxState` and `PersistFullBackupOp` together. Use that local transaction boundary. A retry's newly allocated transaction ID must not replace the original ID in TxProxy/KQP responses. Generated child requests must not independently admit the external UID.

On Forget/cleanup, remove UID mapping and native metadata together at actual deletion. If existing cleanup is asynchronous, the UID remains reserved until that deletion commits. A replay racing cleanup either observes the old operation or admits new work after removal; it must not see an index pointing to missing state. Do not introduce a separate UID GC schedule or retention quota. Account for added DDL bytes under operation storage/request limits; existing-operation replay must bypass new-work admission quotas.

## 5. TDD workflow and milestones

For each milestone, add a behavior test, observe the intended failure, implement the change, and run focused tests followed by the nearest affected suites. A compilation error alone is not a behavioral red result. Use deterministic event interception and existing reboot harnesses; assert actual effects and durable identity.

| Step | Deliverable | Exit evidence |
| --- | --- | --- |
| A | UID transport, SQL syntax, validation | Exact UID/text reaches admission; unsupported requests have no effects |
| B | Full-backup admission and body equality | Concurrent replay returns one ID; different body conflicts; rollback leaves UID unused |
| C | All three native kinds and recovery | Each kind resumes committed work after reboot without client retry |
| D | UID lifetime, Forget, authorization | Mapping follows operation lifetime; deletion and replay races are consistent |
| E | SDKs and capability | C++/Python sync/async and SQL editor preserve identity; advertised support is accurate |
| F | Native data correctness | Backup/incremental/restore data matches expectations under failures |
| G | Controller integration | Stable UID retries recover ambiguous submission without list-difference heuristics |

### A — transport and validation

- Absent UID preserves unkeyed behavior. Test 1/256-byte boundaries, empty/257-byte rejection, allowed punctuation, disallowed whitespace/NUL/non-ASCII, and case sensitivity.
- Request-only, SQL-only, equal dual values, conflicting dual values, and duplicate SQL options.
- Parse full/incremental backup and restore, including existing restore options, while retaining original text unchanged.
- Reject SELECT, unrelated DDL, DML, scripts, explicit transactions, unsupported modes/syntax, parameters, and multi-statement requests before effects. Include a valid first statement followed by an unsupported statement or a later UID-bearing statement.
- Run validation with AST caching and per-statement execution enabled and disabled. Compiler errors must not replace the required unsupported-operation validation for otherwise valid unsupported requests.
- Cross-node KQP serialization, query-cache reuse with different UIDs, and exact-body conflicts despite equivalent compiled plans.
- Reject malformed or unsupported internal UID metadata before ignition; children cannot reuse the external admission identity.

### B — atomic full-backup admission and equality

- Sequential and concurrent same-UID/same-body submissions with different incoming transaction IDs return one original native ID.
- Concurrent same-UID/different-body submissions admit one winner and return conflict to the other. No second native capture occurs.
- Changed collection, options, whitespace, comments, quoting, or semicolons under the same UID conflict while the original record exists.
- Different UIDs produce distinct intended operations subject to ordinary workflow admission; a rejection does not reserve a UID.
- Proposal/suboperation failure and redo-limit rejection roll back operation metadata and the UID index together. Retry the rejected UID with another valid body.
- Lost response and retry return the SDK-visible original ID; GetOperation accepts that identity.
- A replay cannot acknowledge a mapping whose admission transaction has not committed.
- Running, successful, failed, and cancelled operations replay their existing identity without restarting execution, where those native outcomes are supported.

### C — all kinds and crash recovery

Run independently for full backup, incremental backup, and restore:

| Boundary | Required result |
| --- | --- |
| Before admission commit | No durable UID or native effects; retry may admit once |
| After commit, before dispatch | Reboot recovers and executes the admitted operation without a retry |
| After dispatch, before response | Retry returns original ID |
| After completion, before response | Replay resolves original operation while its record exists |
| During child execution/checkpoint recovery | No second logical capture/restore; child retries remain attached to the original ID |

Also test frontend/session changes, external gRPC response loss, and a delayed original submission arriving after a retry. Verify three independent native UID namespaces and separation from existing import/export UIDs. Test distinct SchemeShard tablets and same-tablet database collisions with no cross-database disclosure.

A changed, dropped, or recreated collection must not turn an existing UID into replacement work while operation metadata remains. Authorized replay should reach the original operation without requiring current source artifacts; adjust TxProxy routing prerequisites as needed without bypassing authorization. Reboot must preserve original DDL for conflict checking.

### D — operation lifetime, Forget, and access

- Completion alone leaves the UID reserved, for successful and failed outcomes. Ordinary cancellation where supported does not release it.
- Eligible Forget/cleanup removes UID, DDL, and operation metadata together. After removal and reboot, the same UID may create a new operation, including with a different body.
- Existing active-operation Forget restrictions remain enforced. No UID is removed while its operation remains active or cleanup still owns work.
- Race replay with asynchronous cleanup and final deletion. Accept old-operation replay or a new admission after deletion, never premature reuse or a dangling UID entry.
- Existing native operation expiry policies, if any, remove UID through the same cleanup path; document them per advertised kind. There is no new fixed 30-day guarantee.
- Unauthorized replay/conflict probes reveal neither original body nor protected operation metadata. UID lookup must retain valid domain and access checks.
- At ordinary operation capacity, existing replay succeeds while new admission fails before effects. Account for original DDL in storage/redo limits.
- Metrics distinguish new admission, replay, conflict, and recovery; ordinary replay is not an error. Avoid logging new UID/body metadata or credentials verbatim.

### E — SDK, UI, and capability

- C++ `.Uid(...)` and Python sync/async `uid=...` preserve presence and exact bytes through mapping, settings copies, retry loops, and session/endpoint replacement.
- Existing import/export UID settings and unkeyed Query SDK calls retain compatibility and behavior.
- SQL editor execution works with `WITH (uid=...)` and no separate UI control. Real public-service tests cover equal and differing SQL/request UIDs.
- Unsupported and conflict statuses/issues reach callers unchanged; no retry drops UID or regenerates it.
- Advertise native UID deduplication plus exact-DDL comparison separately from legacy import/export UID support. Legacy UID support alone is not evidence of native support.
- Define endpoint/capability verification before keyed metadata submission: old servers can ignore unknown fields, so adding `uid` alone is insufficient. Mixed-endpoint routing must not silently lose the guarantee. Target current-version native support; no requirement to implement native UID semantics in older binaries.
- Use a guarded execution mode for metadata UIDs so older public endpoints reject the request before execution. Preserve original SQL bytes. Protect cross-node KQP routing with a guarded query type whose old-reader default is UNDEFINED, and retain that guarded value during serialization. A new internal query action alone is unsafe because the proto2 action defaults to EXECUTE.
- Guard actual SchemeShard admission as well as lookup. Dedicated native lookup/propose event types let older tablets reject or ignore the protocol without interpreting it as an unkeyed ModifyScheme request. Bound waits and verify that neither lookup nor admission retries fall back to ordinary unkeyed work.
- Keep retry deadlines/policies unchanged unless separately tested. `RetrySettings(idempotent=True)` is a retry-policy flag, not the server UID.

### F — native data correctness

Use real server/SDK recipe tests, not only mocks or operation-list counts:

- Full backup plus incrementals containing inserts, updates, and deletes restores expected data after response loss at each stage.
- Replaying each UID creates no additional native operation, snapshot, or incremental boundary while its record exists.
- Exercise existing supported restore-to-another-prefix/database workflows and continued incremental capture after pruning older snapshots. Distinguish snapshot pruning from deleting native operation metadata and its UID.
- Restart during admission, capture, and restore replay. Successful terminal restore must include completed incremental replay, including deletes.
- Require expected data and identity assertions. Reaching either READY or ERROR is insufficient; unsupported required native functionality must fail rather than skip acceptance tests.

### G — controller integration

Identify the controller repository/test target; do not assume it lives in this tree. Server/SDK delivery and controller delivery remain separately tracked.

- Persist UID, exact original DDL, database binding, and submission intent before sending. Keep UID/body unchanged across retries and worker restarts.
- Crash-before-send recovers by submission; acceptance with a lost response recovers by identical UID replay and persists the original native ID. Poll existing GetOperation thereafter.
- Use replay to recover submission identity; no new resolve-by-UID RPC is required. Remove list-difference reconciliation only for workflows using the advertised native contract.
- Coordinate Forget, automatic cleanup, and retries. Keep operation metadata until submission recovery is no longer needed, durably stop retries before requesting Forget, and document any existing per-kind automatic expiry. No fixed retry window can be inferred from this UID contract.
- After metadata removal, delayed submissions can create new work. Do not use not-found as proof of nonacceptance or automatically recreate an uncertain forgotten operation. This lifetime limitation is accepted; no permanent tombstones are added.
- Intentional attempts after failure use new catalog operations/UIDs. Preserve the replacement-full policy for failed incremental capture.
- Keep workflow leases, restore-chain pins, manifests, and existing cancellation/cleanup protections. Use ordinary cancellation by known native ID where supported; cancellation of acceptance-unknown work remains a separate workflow problem, without claiming a UID fence.
- Reconcile or drain outstanding unkeyed requests before migration; assigning UID later cannot retroactively deduplicate them. Existing S3 import/export UIDs retain their separate namespaces and semantics.

## 6. Adjust the existing worktree

The earlier draft implementation is not the reference architecture for this revision:

- Reuse applicable ASCII validation, exact-body forwarding, SQL/KQP rejection, postcommit admission, SDK retry, and recovery tests.
- Replace unreleased `idempotency_key`/`IdempotencyKey` transport and settings with UID naming after the compatibility audit above.
- Move UID and original DDL into native operation metadata; replace the independent registry with family-specific indexes reconstructed from those records. Include index insertion/removal in proposal rollback and Forget.
- Replace tests that require replay after operation deletion with tests that prove consistent UID release on deletion. Remove draft terminal-receipt columns and the 30-day receipt regression scaffold as part of the implementation revision.
- Update database-scope tests to exercise actual tablet/family UID namespaces and database collision handling.
- Leave import/export production code and SDK behavior unchanged. Add native-specific helpers when additional comparison or validation is needed; do not alter shared UID helper behavior. Existing import/export UID tests may be run as regression checks without changing their expected behavior.
- Update progress notes, examples, capability descriptions, and release notes to this contract. Earlier passing tests remain historical evidence only where their requirements still apply.

## 7. Repository map and verification

| Area | Starting points |
| --- | --- |
| Existing UID convention | `schemeshard_xxport__helpers.cpp`, `schemeshard_export__create.cpp`, `schemeshard_import__create.cpp` |
| Public API | `ydb/public/api/protos/ydb_query.proto`, `ydb_operation.proto`, `ydb_table.proto` |
| gRPC/KQP transport | `ydb/core/grpc_services/query/rpc_execute_query.cpp`, `ydb/core/kqp/common/events/query.h`, `ydb/core/protos/kqp.proto` |
| SQL and request validation | `yql/essentials/sql/v1/translation`, `ydb/core/kqp/host`, `compile_service`, `session_actor` |
| Native execution routing | `ydb/core/kqp/executer_actor/kqp_scheme_executer.cpp`, `ydb/core/tx/tx_proxy/schemereq.cpp` |
| Atomic admission and persistence | `ydb/core/tx/schemeshard/schemeshard__operation.cpp`, `schemeshard_schema.h`, `schemeshard__init.cpp`, native backup/restore persistence and Forget handlers |
| Native tests | `ydb/core/tx/schemeshard/ut_full_backup`, `ut_backup_collection`, `ut_incr_backup_reboots`, `ut_incremental_restore`, `ut_incremental_restore_reboots`, `ut_incr_restore_reboots` |
| Existing UID tests | `ydb/core/tx/schemeshard/ut_export`, `ut_restore` |
| C++ SDK | `ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/query.h`, `src/client/query/impl/exec_query.cpp`, `tests/unit/client/query` |
| Python SDK | `contrib/python/ydb/py3/ydb/query`, `aio/query`, `ydb/public/sdk/python/tests`; coordinate standalone SDK changes |
| Public service and data tests | `ydb/core/kqp/ut/scheme`, `ydb/tests/functional/backup_collection` |

Run from repository root. Tests include builds; no `-j` or force rebuild. Match filters to actual added test names and confirm a nonzero test count.

```bash
set -o pipefail

./ya make --build relwithdebinfo -tA ydb/core/tx/schemeshard/ut_full_backup \
    -F '*Idempotency*' 2>&1 | tail

./ya make --build relwithdebinfo -tA ydb/core/tx/schemeshard/ut_backup_collection \
    -F '*Idempotency*' 2>&1 | tail

./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/scheme \
    -F '*Idempotency*' 2>&1 | tail

./ya make --build relwithdebinfo -tA yql/essentials/sql/v1/translation/ut \
    -F '*Idempotency*' -F '*Uid*' 2>&1 | tail

./ya make --build relwithdebinfo -tA ydb/core/tx/schemeshard/ut_export \
    -F '*UidAsIdempotencyKey*' 2>&1 | tail

./ya make --build relwithdebinfo -tA ydb/core/tx/schemeshard/ut_restore \
    -F '*UidAsIdempotencyKey*' 2>&1 | tail

./ya make --build relwithdebinfo -tA ydb/core/tx/schemeshard/ut_full_backup \
    -F '*Idempotency*' --test-retries 10 2>&1 | tail

./ya make --build relwithdebinfo -DUSER_CXXFLAGS=-Werror -tA \
    ydb/public/sdk/cpp/tests/unit/client/query 2>&1 | tail

./ya make --build relwithdebinfo -tA ydb/public/sdk/python/tests 2>&1 | tail
```

Run nearest full affected suites after focused checks. Follow [SDK instructions](ydb/public/sdk/cpp/AGENTS.md) for formatting, production clang-tidy, dependency checking, and release notes, and [actor instructions](ydb/library/actors/AGENTS.md) for event serialization, ownership, and postcommit effects. Use C++20 or earlier.

## 8. Completion checklist and extension boundaries

- [x] UID naming/transport, presence, ASCII rules, exact-DDL equality, and supported request shapes are implemented and documented.
- [x] Existing released import/export and other UID consumers retain their behavior.
- [x] All three native families pass concurrent replay/conflict, rollback, lost-response, and reboot/recovery-without-retry tests.
- [x] UID indexes rebuild from native records; Forget/cleanup consistently releases UID with operation deletion. Restore finalization blocks premature Forget, including after reboot; all 156 tests in the three affected suites pass.
- [x] Namespace, database collision, authorization, and metadata accounting tests pass.
- [x] C++/Python sync/async and SQL-only paths preserve UID/body. The guarded execution mode and documented native contract distinguish support from legacy import/export UIDs; no separate capability-discovery RPC is added.
- [x] Real data correctness and durable identity tests pass, including failed-operation replay without new capture.
- [ ] Controller integration: tracked separately, awaiting its repository location. No end-to-end completion claim without it.
- [x] The old independent receipt/TTL design and incompatible draft tests have been removed from the implementation.

Independent retained receipts, a guaranteed retention interval, resolve-by-UID RPCs, and pre-submission cancellation fences are deferred extensions, not acceptance criteria for this revision. Semantic equivalence, database incarnation fencing, and multi-statement operations remain out of scope.

UID naming can be reused by future non-SchemeShard operations, but each owner must implement durable admission and document scope, comparison, and lifetime. A future batch requires its own parent identity and recoverable orchestration; it must not apply one external UID independently to each statement.
