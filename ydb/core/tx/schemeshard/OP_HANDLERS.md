# Per-op handlers

## Problem

Adding a schemeshard operation today touches 5+ central files: the proto enum, `MakeOperationParts` factory switch (~144 cases), `ExtractChangingPaths` audit switch (~130 cases), `GetOperationClass` (~131 cases), and others. Every op's logic is scattered.

## Design

Each op owns its handlers in its own `.cpp`. A YAML manifest tells codegen which ops have migrated; codegen emits the central dispatch. Cross-platform, no template metaprogramming, no source scanning.

**`generated/op_handler_overrides.yaml`** — single source of truth for migration state:

```yaml
ops:
  - ESchemeOpCreateTable
```

**`generated/op_handlers.h`** (codegen output, build-only) — `IsRegistered<Op>` predicates + `extern` declarations + `TryDispatch` switches.

**`schemeshard__op_handlers.h`** — `YDB_DEFINE_OP_FACTORY` and `YDB_DEFINE_OP_AUDIT` macros. Each macro `static_assert`s `IsRegistered<Op>` (using a macro for an op not in the YAML is a compile error pointing to the file to edit) and bakes in the canonical signature (wrong types = compile error, typo in op name = compile error).

## Op author writes

```cpp
// In schemeshard__operation_<your_op>.cpp:

YDB_DEFINE_OP_FACTORY(ESchemeOpCreateTable, op, tx, ctx) {
    if (tx.GetCreateTable().HasCopyFromTable()) {
        return CreateCopyTable(op.NextPartId(), tx, ctx);
    }
    return {CreateNewTable(op.NextPartId(), tx)};
}

YDB_DEFINE_OP_AUDIT(ESchemeOpCreateTable, tx, paths) {
    paths.emplace_back(NKikimr::JoinPath({tx.GetWorkingDir(), tx.GetCreateTable().GetName()}));
}
```

Then add the op to `op_handler_overrides.yaml` and delete its case from the central legacy switches.

## Central dispatch (mid-migration)

```cpp
TVector<TString> ExtractChangingPaths(const NKikimrSchemeOp::TModifyScheme& tx) {
    if (auto handled = NGenerated::NOpHandlers::TryCollectChangingPaths(tx)) {
        return std::move(*handled);
    }
    // Legacy switch — shrinks one case per migration.
    switch (tx.GetOperationType()) { ... }
}
```

## Where we are

- `ESchemeOpCreateTable` migrated as the pilot.
- Trait pattern alternative lives in PR #23; codegen pattern in PR #25 (this branch).
- 1 of ~130 ops moved.

## Where we're going

Each migration removes one case from the legacy switches. End state, when YAML covers every `EOperationType`:

1. Codegen flips out of fallback mode (one config change). `TryDispatch` becomes exhaustive `Dispatch`.
2. Central functions collapse to one line:
   ```cpp
   TVector<TString> ExtractChangingPaths(const NKikimrSchemeOp::TModifyScheme& tx) {
       return NGenerated::NOpHandlers::CollectChangingPaths(tx);
   }
   ```
3. Delete `op_handler_overrides.yaml` (migration ratchet, no longer needed).
4. Delete the legacy switches in `schemeshard__operation.cpp` and `schemeshard_audit_log_fragment.cpp` (~1000 LOC removed).
5. Missing handlers become linker errors permanently — the codegen-emitted `extern` is unsatisfied.

## Failure modes

| Mistake | Where caught |
|---|---|
| Used macro, op not in YAML | Compile (`static_assert` with file-pointing message) |
| Wrong signature | Compile (macro bakes in the signature) |
| Typo in op name | Compile (`NKikimrSchemeOp::Typo` undeclared) |
| YAML lists nonexistent op | Codegen (validates against proto enum) |
| In YAML, function not defined | Linker (extern unresolved) |

## Tooling

`ss_tool` (`ydb/tools/ss_tool/`) reads the codegen output for migration progress:

```
ss_tool ops list [--registered|--unregistered]
ss_tool ops show <OpName>
ss_tool ops migration-status
```
