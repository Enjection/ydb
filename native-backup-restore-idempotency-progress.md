# Native backup/restore UID implementation progress

The [TDD plan](native-backup-restore-idempotency-tdd-plan.md) defines the target. End-to-end delivery is **incomplete**. Existing import/export implementations and SDK APIs remain unchanged.

## Implemented

- Additive Query Service `uid`, C++ `.Uid(...)`, Python sync/async `uid=...`, and SQL `WITH (uid = '...')`. Keys use 1–256 ASCII bytes from `[A-Za-z0-9_.:-]`; compare exact original DDL bytes.
- Full backup, incremental backup, and restore store UID/body on their native operation records. Startup reconstructs the tablet/family/UID index. No independent receipt table, canonicalization version, database incarnation, or retention timer.
- Admission stages operation state, UID/body, and index in one local propose transaction, including redo accounting and rollback. Reply and execution effects follow commit. Generated children clear the external identity.
- Restore creates recoverable UID metadata at initial admission, including pending initial activation; startup and later activation preserve it.
- Forget/ordinary cleanup deletes UID with native metadata. Completion alone reserves it. Reuse with different DDL is allowed after actual deletion.
- Owner checks precede domain/body comparison. Cross-domain collision within one tablet/family returns ALREADY_EXISTS; changed body returns PRECONDITION_FAILED; denied access returns UNAUTHORIZED without operation ID/body disclosure.
- TxProxy now performs authorized native UID lookup before collection-path checks. A hit replays the retained operation even after collection deletion. A miss continues through ordinary collection ACL/admission checks.
- Dedicated serializable `TEvLookupNativeOperation` / `TEvProposeNativeOperation` messages prevent old SchemeShards from interpreting these as ordinary unkeyed proposals. Lookup shares propose validation but cannot enter IgniteOperation or reserve a UID. Replies come from transaction Complete. Both protocol waits are bounded at 30 seconds; generation tags ignore stale lookup timers. Event ToString omits UID, DDL, and tokens.
- Public metadata UIDs use ExecMode 60; cross-node KQP keeps guarded types 15/16 on the wire and normalizes them for local execution. Missing UID/guard pairs are rejected before effects. SQL-only UID statements remain guarded by their SQL syntax.
- Public scheme status handling explicitly preserves native UID ALREADY_EXISTS/UNAUTHORIZED through KQP. Legacy scheme handler behavior is preserved.
- SQL/KQP validation rejects unsupported keyed requests before effects, including batches with AST caching/per-statement variations.

## Passing evidence

| Check | Evidence |
| --- | --- |
| Restore Forget finalization and recovery | `42364`: all 156 tests passed across the three affected restore suites, including queued-finalization and reboot regressions |
| Final native UID matrix | `58076`: all 30 passed, including DataShard child preparation/reboot, delayed older submission, and collection recreation |
| Native and shared proposal regressions | `98242`: all 477 passed across `ut_base`, `ut_pq_reboots`, `ut_full_backup`, `ut_backup_collection`, `ut_incremental_restore`, and `ut_incremental_restore_reboots`; includes 29 native UID cases |
| Collection recreation and legacy UIDs | `24759`: all four checks passed: retained failed-operation replay after collection recreation, export UID, and both import UID variants |
| Real-server data, pruning, and response loss | `27117`: both request/SQL variants passed; seven admitted operations and two exact-data restores per variant |
| Public KQP UID cases | `68761`: all 22 passed, including validation with AST cache/per-statement combinations, wire guards, serialization, lost reply recovery, deleted-collection replay, and status mapping |
| C++ SDK | `11025`: all 384 client/gRPC/issue unit tests passed with `-Werror`; includes the 21 query cases also checked in `60323` |
| Python SDK | `96119`: 11 behavioral tests and two checks passed for guarded transport, sync/async retry preservation, and non-executing modes |
| SQL translation | `93684`: 1198 passed, one skipped after the UID rename |
| SDK static analysis | Direct production clang-tidy passed for all three query implementation translation units; configuration unchanged |
| SDK dependencies | `ok (121 ya.make)` |

The broader native log is `/tmp/native-uid-rollback-regressions-full.log`. The latest functional log is `/tmp/native-uid-data-pruning-recovery.log`. Earlier response-loss-only variants passed in `15307`; the expanded `27117` run supersedes that data-test scope.

## Real-server coverage

The functional test uses file-backed PDisks and a forwarding gRPC proxy that consumes the first admitted operation result, then aborts the SDK stream with UNAVAILABLE. Each retry uses a new SDK pool and the original SQL/UID. The first operation ID is retained only by the test oracle.

Both request and SQL UID variants verify:

- Full backup, two incrementals containing inserts/updates/deletes, restart after the second incremental admission, and exact restored rows.
- Original-ID recovery after every lost admission response, terminal-state replay, and unchanged snapshot/operation sets on retry.
- A second full backup followed by deletion of old snapshot tables while keeping their native operation records. Old full/incremental UIDs still replay.
- Continued incremental capture after pruning, a second restore with process restart after admission, and another exact row comparison.
- Successful Forget of all seven native operations and removal from their operation lists.

## Remaining work and verification limits

Audit follow-up: `94527` reproduced premature restore Forget while an actual finalization request was held before execution. The handler now only permits `Completed`/`Failed`, retaining the UID during `Finalizing`. The focused regression passed in `78133`. It now also checks the 99% progress state and a reboot while finalization is queued. The first broader run `65919` failed to link the test's enum-printing assertion (29 other tests passed); numeric enum comparison fixes that test-only issue. The three affected suites passed all 156 tests in `42364`, `/tmp/native-uid-finalizing-forget-restore-suites-rerun.log`; the lifecycle audit is complete.

Controller integration (plan G) still requires the controller repository location. This workspace contains only YDB checkouts. Local YDB server/SDK implementation and verification are complete. End-to-end delivery remains incomplete until controller integration is handled.

The final native matrix passed all 30 cases in `58076`, `/tmp/native-uid-child-prepared-recovery.log`. Each family recovers without a client retry both before dispatch and after a DataShard child prepares. An older submission delivered after recovery resolves to the admitted newer operation ID. The collection-recreation and unchanged import/export UID checks passed in `24759`, `/tmp/native-uid-recreation-legacy-regressions.log`.

Coverage limits: the child-preparation test targets schema-child preparation, not every incremental-replay checkpoint. Full-backup failure, capacity, owner checks, metrics, and both retry/Forget orders have focused coverage; not every such case is independently repeated for each family. The shared admission checks, all-family redo/namespace/Forget/reboot tests, existing native recovery suites, and real-data tests provide the combined evidence. Native cancellation and alternate restore destinations retain the boundaries below.

SDK production static analysis, dependency checking, C++/Python transport tests, public-service validation, and usage examples have been reviewed. Temporary SDK clang-tidy generated headers/compilation database were removed. Final source whitespace checks pass; no import/export files changed.

## Public and mixed-version guards

- Public ExecMode 60 requires UID and vice versa; SDKs select it only for keyed execution. Parse/Validate/Explain are not upgraded to execution.
- KQP wire types 15/16 retain the guard during serialization and normalize for local processing. Legacy proto2 readers see UNDEFINED rather than EXECUTE. Both forwarding variants and malformed UID/guard pairs passed in `49443` and the final `68761` matrix.
- Dedicated SchemeShard lookup/propose events protect actual admission, with bounded waits and no ordinary-event fallback. Ignored-protocol tests passed in `64929`.
- A public SDK test lost the postcommit SchemeShard admission reply, timed out, and recovered the original operation ID with one admission (`49443`). Real gRPC stream loss passed in the latest `27117` functional run.

## Recovery and rollback fixes verified by tests

- Restore startup required a context-aware change-path-state factory. Admission also keyed its long-operation map by a control suboperation ID while startup/finalization used the parent ID; these now consistently use the parent ID.
- Reaching that finalization branch exposed backup directory names missing full/incremental suffixes. Existing naming helpers now produce the correct paths. Six cleanup failures from `4363` passed in `63037`, and the final native suites remain green in `98242`.
- `28557`: oversized incremental admission crashed at `TCreateTable::AbortPropose`. Table creation now tracks path/table/shard/transaction/domain undo state, defers writes, and reverses its table counter.
- `75500`: table rollback progressed, then `TAlterPQ::AbortPropose` crashed. Topic creation and alteration now track undo state, including deep copies of owned partitions and counters, and defer database writes.
- `75783`: incremental rejection/reuse passed; restore rejection left the collection marked as restoring. Restore control and change-path-state proposals now capture path and transaction state before mutation.
- `47964`: all-family redo rejection passed, including immediate reuse with different DDL, successful completion, rejected-ID absence after reboot, and original-ID replay. Two ordinary-operation tests assumed an empty domain; they now compare against initial counts. Both passed in the full `98242` run.
- Earlier fixture corrections: recovery observers preserve the tablet scheduling tracer; submission helpers receive their own edge actor's reply; the functional restart fixture uses persistent PDisks. Counter tests use the standard `SchemeShard/NativeUid/` prefix.
- Deleted-collection replay originally failed in TxProxy (`54595`); authorized lookup now precedes collection validation. Native access/namespace statuses originally became GENERIC_ERROR (`40133`); explicit native status transport now preserves them.

## Existing feature boundaries

- Record deletion audit: full/incremental operation records are removed by their Forget handlers; restore records also have collection-drop cleanup. These deletion paths have no timer-based record expiry. Snapshot-table pruning retains operation metadata. The usage document now states the per-family cleanup rules.

- Native CancelOperation is currently UNSUPPORTED for full backup, incremental backup, and restore; this change does not add cancellation or a pre-admission cancellation fence.
- The current native restore path copies to the table paths recorded in the collection (`schemeshard__operation_restore_backup_collection.cpp`). A new target-prefix/database restore API is outside this UID extension. Test additional destinations only where an existing supported native workflow provides them.

## Tooling constraints

- Publication constraint: the user authorized publishing these YDB changes as a draft PR in `Enjection/ydb` only. Controller integration remains separately outstanding.
- Keep **all ya builds sequential**, including SDK targets. Apparently small targets can expand into many uncached tool compilations. Do not overlap build graphs.
- Use `--no-bazel-remote-store`: remote cache downloads repeatedly fail with InvalidChunkLength. Preserve local cache; no force rebuild, no `-j`. Tests include build; use pipefail and `2>&1 | tee ... | tail`.
- Keep active build inputs stable and write source edits with atomic temporary-file rename. Earlier in-place edits raced compiler mmap reads.
- SDK builds/tests require `-DUSER_CXXFLAGS=-Werror`. Direct production clang-tidy **passes** for `exec_query.cpp`, `client_session.cpp`, and `session_state_handler.cpp` after resolving the 32 earlier findings. Logs: `/tmp/native-uid-sdk-clang-tidy-green.log` and `/tmp/native-uid-sdk-clang-tidy-{client_session,session_state_handler}.log`. Fixes cover optional checks, forwarding, self-assignment, internal destructor access, and owning connection handles across precommit. Narrow false-positive comments explain non-owning atomic-pointer assignment, synchronous lvalue callbacks, and query inputs consumed/copied before suspension; the configuration is unchanged. Do not lint tests. Temporary compilation database/generated headers under `ydb/public/sdk/cpp/build/clang-tidy` must not be committed.
- SDK style dry-run found extensive existing whole-file churn; avoid unrelated formatting. Current source diff checks pass. Repeated import/export filename checks show no changed import/export files.
