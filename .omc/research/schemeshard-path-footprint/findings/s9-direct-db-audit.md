# S9 audit: Propose bodies with direct DB access (write set not derivable from TMemoryChanges)

Method: static scan of every `THolder<TProposeResponse> Propose(...)` body under
`ydb/core/tx/schemeshard/` (`schemeshard__operation_*.cpp`, `index/*.cpp`,
`olap/operations/*.cpp`): count `GetDB(` calls, `MemChanges.Grab*` calls, raw
map writes. Driver semantics: `GetDB()` sets `DirectAccessGranted`, so
`IsUndoChangesSafe()` becomes false; a later part failure in the same request
is a `Y_VERIFY` (`schemeshard__operation.cpp:222,553`), i.e. tablet reload, not
undo. The S7c write set is `CollectPathIdsSince` over the undo stacks, so for
these ops it is **empty**, not partial (the S7c test pins exactly this for
CreateTable: `ut_path_footprint.cpp:1294-1295`).

| | count |
|---|--:|
| Propose bodies scanned | 108 |
| undo-safe (write set by construction) | 69 |
| direct DB access (`GetDB`) | **39** |
| of which zero `MemChanges.Grab*` (write set empty) | 37 |
| of which mixed (`GetDB` + Grab): AlterSecret, CreateSecret | 2 |
| raw map writes with neither Grab nor GetDB | 0 |

## The 39 direct-access Propose bodies, by family

- **Tables / dirs / ACL (high traffic):** TCreateTable, TRmDir, TModifyACL,
  TAlterUserAttrs
- **Subdomains:** TCreateSubDomain, TAlterSubDomain, TDropSubdomain,
  TCreateExtSubDomain, TDropExtSubdomain, TUpgradeSubDomain,
  TUpgradeSubDomainDecision, TDropForceUnsafe
- **Column store / table (olap):** TCreateOlapStore, TAlterOlapStore,
  TDropOlapStore, TCreateColumnTable, TAlterColumnTable
- **Sequences:** TCreateSequence, TAlterSequence, TDropSequence, TCopySequence
- **Replication:** TCreateReplication, TAlterReplication, TDropReplication
- **PQ:** TDropPQ
- **Solomon / RTMR / Kesus / BSV:** TCreateSolomon, TAlterSolomon, TDropSolomon,
  TCreateRTMR, TCreateKesus, TDropKesus, TCreateBlockStoreVolume,
  TDropBlockStoreVolume, TAssignBlockStoreVolume
- **Secrets (mixed):** TCreateSecret, TAlterSecret
- **Index build internals:** TFinalizeBuildIndex, TPrepareIndexValidation
- **Control:** TTxCancelTx (not through ProcessOperationParts anyway)

Notable: DropTable, AlterTable, MkDir, CreateTableIndex, CDC and backup
families are undo-safe and therefore covered by construction.

## Assessment

The "write set by construction" claim holds for 64% of Propose bodies. For the
other 36%, including CreateTable, the reliable signals at end of Propose are:

1. the footprint **entries** (request-named paths, all ops);
2. **Published** (`TSideEffects::PublishPaths`): every path whose version bumps,
   which is what the rest of the system (SchemeBoard) observes; creates publish
   the new path and its parent, drops publish the path and parent;
3. **TxState.TargetPathId/SourcePathId** of the TxStates created by the part.

Recommended contract: `EffectiveWriteSet = WriteSet ∪ Published ∪ TxStateTargets`,
with `WriteSetMayBeIncomplete` kept as the honesty bit. For schema CDC,
`Published` is arguably the right definition of "written" anyway.

Recommended gate extension (dynamic, cheap): for undo-safe parts assert
`Published ⊆ WriteSet ∪ Entries` and report `WriteSet \ Published` per op type,
which measures how much Published under-reports; for direct-access parts assert
`Entries ∩ (Published ∪ TxStateTargets) ≠ ∅` unless the part is pathless.

Long-term fix is the codebase's own direction: migrate the 39 to
`TMemoryChanges`/`TStorageChanges` (undo-safe pattern already used by the newer
ops). CreateTable, RmDir, ModifyACL, AlterSubDomain first by traffic. That is
per-op work, out of scope for this branch.
