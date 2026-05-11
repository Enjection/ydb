# Per-op handlers

## Problem

Adding a schemeshard operation today touches 5+ central files: the proto enum, `MakeOperationParts` factory switch (~144 cases), `ExtractChangingPaths` audit switch (~130 cases), `GetOperationClass` (~131 cases), and others. Every op's logic is scattered across the schemeshard tree.

## End-state design (where we're going)

**Each op owns its handlers in its own `.cpp`. The codegen emits the central dispatch directly from the proto enum. There is no central switch, no manifest, no wrapper.**

What an op author writes — full surface, forever:

```cpp
// In schemeshard__operation_<your_op>.cpp:

YDB_DEFINE_OP_FACTORY(ESchemeOpFoo, op, tx, ctx) {
    return {CreateNewFoo(op.NextPartId(), tx)};
}

YDB_DEFINE_OP_AUDIT(ESchemeOpFoo, tx, paths) {
    paths.emplace_back(NKikimr::JoinPath({tx.GetWorkingDir(), tx.GetCreateFoo().GetName()}));
}
```

That's it. Nothing else to edit. Compile-time errors catch typos and signature drift. Linker errors catch missing definitions.

What the central dispatch looks like at end state:

```cpp
TVector<TString> ExtractChangingPaths(const NKikimrSchemeOp::TModifyScheme& tx) {
    return NGenerated::NOpHandlers::CollectChangingPaths(tx);
}
```

One line. The codegen-emitted `CollectChangingPaths` is the dispatch.

## Migration-time scaffolding (temporary)

Until every op is migrated we need a way to incrementally move ops without breaking everything else. **The pieces below are scaffolding — they all get deleted at the end.**

| Scaffolding | Purpose | Removed when |
|---|---|---|
| `op_handler_overrides.yaml` | Tells codegen which ops have migrated; gates the macro `static_assert` so unmigrated ops can't accidentally use the macros. | Every op is migrated. |
| `Try*Dispatch` returning `std::optional` | Lets the codegen handle migrated ops while un-migrated ops fall through to legacy. | Codegen flips to exhaustive `Dispatch`. |
| Legacy switches in `schemeshard__operation.cpp` and `schemeshard_audit_log_fragment.cpp` | Handle un-migrated ops. Each migration deletes one case. | Empty switch — file deleted or function collapsed to the codegen call. |
| The `if (auto handled = TryDispatch(...)) return *handled;` prelude in central functions | Routes migrated ops through codegen, falls through to legacy switch otherwise. | Replaced by direct call to the exhaustive codegen function. |

So the **mid-migration** version of the op author flow has three steps:

1. Write the handler in your op's `.cpp` (same as end state).
2. Add the op name to `op_handler_overrides.yaml` (one line).
3. Delete the matching case from the central legacy switch.

After full migration only step 1 remains.

## Comparison

| Concern | Today (pre-refactor) | Mid-migration | End state |
|---|---|---|---|
| Files to touch when adding a new op | 5+ central files + op's .cpp | YAML + central switch + op's .cpp | op's .cpp only |
| Where is `Foo`'s dispatch logic? | Search across factory switch, audit switch, class switch, ... | Same, but for un-migrated ops only | One file: `schemeshard__operation_foo.cpp` |
| Catch a missing handler | Manual review | Linker error (extern unresolved if op in YAML) | Linker error always |
| Catch wrong signature | Compile error at the call site | Compile error in macro `static_assert` | Same |
| Central audit switch length | ~600 lines | shrinks each migration | one line |

## Where we are

- `ESchemeOpCreateTable` migrated as the pilot (1 of ~130).
- All scaffolding in place: YAML, codegen, macros, central preludes.
- Trait pattern alternative is in PR #23; this codegen pattern is PR #25.

## End-state cleanup (the "we're done" PR)

When the YAML lists every `EOperationType`:

1. Flip the codegen template's `default: return std::nullopt` branch to `default: Y_UNREACHABLE()` (one-line config). `TryDispatch` becomes exhaustive `Dispatch`.
2. Replace the central function bodies with `return NGenerated::NOpHandlers::Dispatch(...);`.
3. Delete the legacy switches (~1000 LOC removed across `schemeshard__operation.cpp` and `schemeshard_audit_log_fragment.cpp`).
4. Delete `op_handler_overrides.yaml` — strict mode doesn't read it.
5. Drop the `static_assert` predicate from the `YDB_DEFINE_OP_*` macros (no YAML to gate against).

After this PR: missing handlers are linker errors permanently. Adding a new op = write the function. Done.

## Failure modes (mid-migration AND after)

| Mistake | Where caught |
|---|---|
| Used macro, op not in YAML (mid-migration only) | Compile (`static_assert` with file-pointing message) |
| Wrong signature | Compile (macro bakes in the signature) |
| Typo in op name | Compile (`NKikimrSchemeOp::Typo` undeclared) |
| YAML lists nonexistent op (mid-migration only) | Codegen (validates against proto enum) |
| In YAML, function not defined | Linker (extern unresolved) |
| End-state: missing handler for a new op | Linker |

No silent failures. Every mistake is caught before the change reaches users.

## Tooling

`ss_tool` (`ydb/tools/ss_tool/`) reports migration progress:

```
ss_tool ops list [--registered|--unregistered]
ss_tool ops show <OpName>
ss_tool ops migration-status
```

This tool itself is migration-time scaffolding — once every op is registered, `migration-status` always reports "100% — done." Keep it for documentation; or delete after the cleanup PR.
