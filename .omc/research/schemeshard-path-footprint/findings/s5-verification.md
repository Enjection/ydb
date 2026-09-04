# S5 part 1 — independent re-verification of the S4 prototype

Branch `feat/schemeshard-path-footprint`, uncommitted working tree,
repo `/home/innokentii/ydbwork3/ydb`. All commands run from the repo root.

## 1. The branch as S4 left it did not compile

The very first thing S5 did was re-run S4's own test command. It failed:

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_path_footprint
{"type": "result", "kind": "build", "path": "ydb/core/tx/schemeshard", "status": "FAILED", "error_type": "REGULAR", ...}
{"type": "result", "kind": "test", "path": "ydb/core/tx/schemeshard/ut_path_footprint", "name": "unittest", "status": "FAILED", "error_type": "BROKEN_DEPS"}
{"type": "summary", "exit_code": 1}
```

20 compile errors, all one root cause — a variable declared directly under an
unbraced `case` label, so every later `case` label in the same `switch` is an
illegal jump:

```
ydb/core/tx/schemeshard/schemeshard_path_footprint.cpp:653:5: error: cannot jump from switch statement to this case label
  653 |     case NKikimrSchemeOp::ESchemeOpCreateFullBackupOp:
ydb/core/tx/schemeshard/schemeshard_path_footprint.cpp:509:19: note: jump bypasses variable initialization
  509 |         const int moveSrcIndex = out.Last();
fatal error: too many errors emitted, stopping now [-ferror-limit=]
20 errors generated.
```

**The "builds clean, 21/21 new tests green" claim in `findings/s4-prototype.md`
§3 was therefore not reproducible from the tree that existed when S5 built it.**
The other two `Implicit`-anchor cases (`CreateIndexedTable` at line 287,
`MoveIndex` at line 558) are inside `{ }` blocks and are fine; only
`ESchemeOpMoveTable` was written without braces.

**Erratum, per S4 (accepted).** S4 published a tree it had verified green, was
then asked to continue, and made a *second* edit round adding the anchored
`Implicit` entries from the S2 map. That round introduced the error, and S5's
build landed inside roughly a six-minute window before S4 fixed and re-verified
it. So the green claim was true of the originally published tree and is true of
the frozen tree; it was false only during that window. The process failure was
resuming edits on a tree already declared final without announcing it, not a
fabricated result. The operational lesson is unchanged: for this suite, take the
CI run, not the summary line.

Sequence of events, from file mtimes: S5's build failed at 23:50:21, and
`schemeshard_path_footprint.cpp` was rewritten at 23:50:49 — the S4 agent was
still editing the tree while S5 measured it. The braces
(`case ESchemeOpMoveTable: {` … `}`, lines 507 and 514) are present in the
current file; S5 did not write them. S5 sent S4 a message to stop editing and
froze the tree at:

```
8c5f4cbebf713da82d3729f5a001c343  schemeshard_path_footprint.cpp
dfe5b9689ac60cca2fcdf231d1c08cae  schemeshard_path_footprint.h
88c218d15f4d199608e3cd843eb7cc0e  ut_path_footprint/ut_path_footprint.cpp
8de4d10331ff9b3ada47553b5326b44c  ut_path_footprint/ya.make
43d71b9bad049836653d7a436efc6a80  schemeshard__operation.cpp
d0095b46cd08ea4d5f70b2f909d6798f  schemeshard__operation.h
a881d3541fece66bc605cc34e0a8186c  ya.make
```

S4 independently published its frozen md5s for the same seven files; S5
re-hashed all seven after the coverage run and **every one matches**. The two
agents therefore measured and verified the identical tree, and it did not move
during the six-suite run.

Everything below was measured against exactly that tree. S5 made no code
changes of its own.

Takeaway for the report: the design and the test results stand, but the
prototype needs one more compile before anyone tries the branch, and the
`ut_path_footprint` suite must be run in CI rather than trusted from a summary.

## 2. `ut_path_footprint` — 21/21 OK

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_path_footprint
{"type": "build_finished", "ts": 1788393141.151, "build": 0, "exit_code": 0}
{"type": "summary", "ts": 1788393141.19, "exit_code": 0, "tests": {"OK": 21}}
```

Suite contents, counted from the source (`ut_path_footprint.cpp`, 667 lines):
14 `TSchemeShardPathFootprintExtract` cases (pure `ExtractPathRefs`, no actor
runtime, including `EveryOperationTypeIsCovered` walking
`EOperationType_descriptor()`) and 7 `TSchemeShardPathFootprintPropose` cases
(`TTestEnv`, observed through a log-record-collecting `TLogBackend`):
`CreateTableWithIntermediateDirs`, `CreateIndexedTable`, `CreateCdcStream`,
`MoveTable`, `DropTableByNameAndById`, `ConsistentCopyTables`,
`RejectedCreateTableStillProducesFootprint`. 14 + 7 = 21, so every declared
test ran and none was skipped.

## 3. `ut_auditsettings` — 5/5 OK (parity unaffected)

```
hya make -T --build=relwithdebinfo -j128 -ttt ydb/core/tx/schemeshard/ut_auditsettings
{"type": "summary", "ts": 1788393180.412, "exit_code": 0, "tests": {"OK": 5}}
```

Zero `"status": "FAILED"` records in that run. This is a weak signal by
construction: S4 deliberately did **not** rewire `ExtractChangingPaths`, so the
audit log still runs through the old 136-case switch and `ut_auditsettings`
cannot detect a divergence between old and new extractors. It only proves the
hook did not break audit output.

## 4. Measurement caveats

- The `ut_path_footprint` propose tests install their own log backend, so their
  `PathFootprint` lines do **not** reach `testing_out_stuff/*.err`. The
  coverage harvest in `s5-coverage.md` relies on the six other suites, which use
  the default stderr backend and whose `TTestEnv` sets `FLAT_TX_SCHEMESHARD` to
  `PRI_NOTICE` (`ut_helpers/test_env.cpp:848`).
- `ut_path_footprint` runs in 10 chunks; the counts above are the aggregate ya
  summary, not a per-chunk sum.

## 5. Second and third moving-tree events, and what the numbers describe

The manifest in §1 was published by S4 as frozen and independently re-hashed by
S5 after the six-suite run; all seven files matched. Everything in
`s5-coverage.md` was measured against that state.

It did not stay frozen.

| time | event |
|---|---|
| 23:50:49 | first unannounced edit, during S5's initial build (the unbraced `case`) |
| 23:56 | S4 declares the tree frozen, publishes seven md5s |
| ~00:05 | S4 confirms nine defects, offers option A (ship frozen) or B (fix and re-verify), states it will not touch the tree until S5 chooses |
| 00:10:49 | `schemeshard_path_footprint.cpp` edited anyway: 838 -> 902 lines, `8c5f4cb` -> `75172f3d`, applying the class (c) fixes |
| 00:11:55 | edited again: `38631b04` |
| 00:13:35 | `ut_path_footprint.cpp` edited: `88c218d1` -> `3ab186a6` |

S5 had chosen **option A** — ship the frozen artifact with the nine defects
documented and exact fixes listed — precisely because reopening a verified
artifact at the end of an overnight run risked this. That reasoning became moot
once the artifact was reopened regardless, so the choice is now effectively B,
which requires a genuinely still tree to be worth anything.

**Consequence for every number in this research session:** the test results and
the coverage table describe the **pre-fix** prototype at
`schemeshard_path_footprint.cpp` md5 `8c5f4cbebf713da82d3729f5a001c343`, 838
lines. They are reproducible only against that state. `report.md` carries a
provenance banner saying so. The design recommendation and the 23-line hook diff
are unaffected — every edit in the table above touched only the layer-1 table
and its test.

The process lesson generalizes past this session: **a hash manifest is worth
something, a freeze declaration is not.** The manifest is what let this be
detected at all, twice. Any future cross-validation here should re-hash
immediately before and immediately after each measurement run and publish
results only when both agree, which is the protocol S5 used and the reason the
report could be corrected rather than quietly wrong.
