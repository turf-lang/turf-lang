# AGENTS.md

Guidance for AI coding agents working on the Turf compiler itself (not on programs written in Turf — for language syntax and stdlib usage, see `README.md`).

## What this repo is

An LLVM-backed, statically-typed, AOT-compiled language. Pipeline: `Lexer → Parser → Resolver (2-pass) → TypeChecker → Codegen (LLVM IR)`, plus a diagnostics layer with an optional local-SLM-assisted `--smart` mode. C++20, CMake, LLVM 22.

## Build

```bash
./scripts/cmake_build.sh --release   # first run also builds the vendored llama.cpp submodule — slow once, fast after
```

Binary lands at `build/turf`. Rebuild after any `src/`/`include/` change with `cmake --build build -j$(nproc)` (no need to re-run the full `cmake_build.sh` script unless `CMakeLists.txt` itself changed).

## Test

```bash
./scripts/test_runner.sh
```

Runs every `.tr` file under `tests/`. Two conventions the runner enforces:
- A file named `fail_*.tr` is expected to **fail to compile**. A plain-named `.tr` file is expected to **compile and run successfully** (use `assert(cond, "message")` inside `main()` for behavioral checks).
- A `fail_*.tr` file containing the marker comment `// RUNTIME_ABORT_EXPECTED` instead means: must compile successfully, then abort at runtime (non-zero exit) — for stdlib guards that fail at runtime, not compile time, e.g. bounds checks.

**The one trap everyone hits repeatedly on this project**: `scripts/test_runner.sh` silently *excludes* any file whose name ends in `_module.tr` (`grep -v "_module.tr"`), treating it as a companion/helper file with no `main()`. This means:
- A genuine standalone test named e.g. `scope_something_module.tr` is **silently skipped** — 0 result, not a failure, easy to miss entirely.
- A companion file (declares only functions/structs/enums, no `main()`, meant to be `import`ed by a real test) that does **not** end in `_module.tr` gets run as a standalone test and fails with "No entry point found."

After adding or renaming any test file, sweep both directions:
```bash
# standalone tests wrongly excluded (end in _module.tr but have a main())
find tests -name "*_module.tr" | xargs grep -lE '\bfn\s+(int\s+)?main\s*\('
# companions wrongly included (no main(), but don't end in _module.tr, and aren't a fail_* test)
find tests -name "*.tr" -not -name "*_module.tr" -not -name "fail_*" | xargs grep -LE '\bfn\s+(int\s+)?main\s*\('
```
Also watch for names that end in `_module.tr` as a plain SUBSTRING coincidence (e.g. `fail_import_nonexistent_module.tr`, `scope_struct_by_value_across_module.tr`) — the filter is a bare substring match, not a suffix-of-the-meaningful-name check.

**`scripts/test_runner.sh` only covers `tests/`.** For any change to language syntax or semantics, also grep/compile-check `lib/std/*.tr` and `showcase/*.tr` directly — neither is exercised by the test runner (the stdlib is only pulled in by whichever tests happen to `import` it, and showcase programs aren't run by it at all), so a stdlib or showcase file using old syntax can sit broken indefinitely with the full suite reporting green. Confirmed live: a language-syntax change once left two `lib/std/net.tr` methods on removed syntax, caught only by directly compiling the showcase programs that import `net`.

## Repo map

- `src/`, `include/` — the compiler itself (Lexer, Parser, Resolver, TypeChecker, Codegen, Errors, SLM integration).
- `lib/std/` — the standard library, written in Turf itself (`io.tr`, `mem.tr`, `math.tr`, `list.tr`, `stack.tr`, `queue.tr`, `strings.tr`). No compiler special-casing; ordinary `.tr` files.
- `tests/` — the regression suite, organized by numbered topic folders (`06_structs`, `07_functions_modules`, etc.) plus `tests/10_known_bugs/` (excluded from the runner — documents accepted/deferred limitations, not live regressions) and `tests/custom/` (also excluded).
- `docs/architecture/` — deep internals references for specific subsystems, kept accurate and updated alongside the code they describe (e.g. `modules_and_scope_resolution.md` for the `::`/import system). Read the relevant one before making non-trivial changes to that subsystem — don't re-derive from scratch.
- `docs/planning/` — roadmap and issue-tracker-style docs (`ROADMAP_v1.0.0.md` is the authoritative "what's done vs. open" tracker; the `PHASE_*_ISSUES.md` files are historical specs, trimmed to only the genuinely-still-open issues in detail).
- `changelog/issueNNN.txt` — historical, permanent records of specific past changes. Not a tracker; don't edit these to reflect new work, add new ones only if following that same historical-record convention.
- `scripts/` — `cmake_build.sh` (build), `test_runner.sh` (test), `compile_and_run.sh` (compile+run+cleanup one file).

## Conventions this project holds firmly to

- **Doc comments explain WHY, not what.** A hidden invariant, a past bug this code prevents, a non-obvious constraint — not a restatement of the code. Applies to functions touched by a fix and closely adjacent ones.
- **Every fix gets a regression test** in `tests/<NN_topic>/`, following that folder's existing naming/numbering. A bug fix needs both directions covered where it makes sense: a `fail_*.tr` proving the bad case is now rejected, and/or a plain `.tr` proving the good case still works.
- **Small, scoped commits.** One logical fix or feature per commit, not batched sweeps, with a message explaining the *why* and the failure scenario that motivated it — future readers (human or agent) should be able to understand a change from its commit message alone.
  - Title format is `type(scope): summary (@username)` — Conventional Commits (`feat`/`fix`/`test`/`docs`/`refactor`/`misc`, optional parenthesized scope) with the committer's own `git config user.name` appended in parens at the very end. Consistent across recent history; match it going forward even though older commits predate it.
  - No `Co-Authored-By` trailer or other AI-attribution text in the message body.
- **Verify claims live, don't assume from a prior fix's scope.** More than once this project has stated a design rule ("`.` is instance-only") as settled fact in a doc without a fresh test that actually tries to break it. If you're about to write "X is enforced" anywhere, try the specific input that would prove it wrong first.
- Full test suite (`./scripts/test_runner.sh`) must stay green before considering any change done.
