# AGENTS.md

Guidance for AI coding agents working on the Turf compiler itself (not on programs written in Turf — for language syntax and stdlib usage, see `README.md`).

## What this repo is

An LLVM-backed, statically-typed, AOT-compiled language. Pipeline: `Lexer → Parser → Resolver (2-pass) → TypeChecker → Codegen (LLVM IR)`, plus a diagnostics layer with an optional local-SLM-assisted `--smart` mode. C++20, CMake, LLVM 22.

## Build

```bash
./scripts/cmake_build.sh --release   # first run also builds the vendored llama.cpp submodule — slow once, fast after
```

`--no-smart` (`-DTURF_SMART=OFF`) builds without `--smart`'s model runtime: no llama.cpp submodule, no OpenMP. `src/SLMEngineDisabled.cpp` replaces `src/SLMEngine.cpp`, the only file that includes `llama.h`, so keep llama.cpp out of every other file or this build breaks. The test suite and `turf-api-tests` pass either way (neither needs a model).

The build produces **two binaries** (LEC-001, `docs/development/FREEZE.md` §6.16, F-PKG-1):
- `build/turfc` is the compiler. Everything in this file that says "the compiler" means it: the test runner, the smoke scripts and CI run it directly, and `TURF_COMPILER` (the override the runner reads) points at `turfc`, never at `turf`.
- `build/turf` is the Go front end under `cli/` (it finds `turfc`, checks its driver interface, and forwards; `turf file.tr` is `turfc file.tr`). **Go is optional**: it is only needed for `build/turf`, and without `go` on `PATH` CMake prints `Go not found: build/turf ... will not be built; build/turfc is unaffected` and the compiler and the whole test suite build and run as usual. `-DTURF_BUILD_CLI=OFF` skips it explicitly.

Rebuild after any `src/`/`include/` change with `cmake --build build -j$(nproc)` (no need to re-run the full `cmake_build.sh` script unless `CMakeLists.txt` itself changed). `turf` and `turfc` are **versioned independently**. `turfc`'s release version is the root `VERSION` file; `turf`'s is `cli/internal/version/version.go`. What must fit is the **driver interface** `turfc --version` prints on line 2 (`TurfcInterfaceMajor`/`Minor` in `src/main.cpp`): bump the minor when you add a flag or input a driver may use, and the major when you remove or change the meaning of one (`FREEZE.md` §6.16). User-facing text (README, help, diagnostic hints) says `turf`. `turfc` is the tooling interface and what this file's scripts call. The front end's own checks are `cd cli && go vet ./... && go test ./...` and `./scripts/cli_smoke.sh` (SKIP if `build/turf` is absent, unless `CI` is set).

`./scripts/dev_install.sh` symlinks `build/turf` into `~/.local/bin` (`--prefix DIR` for elsewhere, `--with-turfc` to link the compiler too, `--uninstall` to undo), so `turf` works from any directory and finds `build/turfc` by itself. **Don't export `TURF_HOME` for a dev checkout**: both binaries already find their install root from their own real path, and a `TURF_HOME` pointing at one checkout silently makes every *other* build (a worktree, a scratch build directory) use that checkout's `lib/std` and runtime.

## Test

```bash
./scripts/test_runner.sh
```

Runs every `.tr` file under `tests/`. Takes an optional `-O0`/`-O1`/`-O2`/`-O3`/`-Os` argument (`./scripts/test_runner.sh -O1`) that forwards that flag to every compile invocation — absent, it's whatever the compiler's own default is. CI runs the full suite once per level this way (`.github/workflows/test.yml`) as OPT-001's regression coverage for "optimization levels never change observable behaviour" (INV-OPT-1); `scripts/opt_levels_smoke.sh` covers the same property for `showcase/`, which the runner doesn't touch at all (see below).

**Tool API tests.** `build/turf-api-tests` (built with everything else, source `tests/api/analysis_test.cpp`) unit-tests the compiler query facade (`include/turf/Analysis.h`, SMP-001) — the only supported way to run the front end in-process from a tool. `test_runner.sh` only runs `.tr` files, so run this binary directly after any change to the Lexer/Parser/Resolver/TypeChecker's global state; CI and `session_postflight.sh` both run it. Its fixtures are written to a temp directory at run time, never as `.tr` files under `tests/api/`, which the runner would pick up.

**GC regression tests (`tests/41_gc/`) must be run at `-O0` as well as the default level.** Conservative stack/register scanning can mask a real tracing-collector bug at `-O2` purely by register-allocation luck — a value that would otherwise be the only surviving reference to a GC object happens to still sit in a scanned register or stack slot. `./scripts/test_runner.sh -O0` is the only way to catch that class of bug; it's already part of CI, but a session iterating locally on `runtime/turfrt_gc.c` or its Codegen counterpart should run it directly rather than relying on the default-level run alone.

**GC testing aids (FDN-005)** — runtime environment variables, read by every compiled Turf program, combinable with each other and with any `-O` level:
- `TURF_GC_STRESS=1` — collect on every allocation. The most effective way to surface a missed root; slow on allocation-heavy tests (raise `TURF_TEST_TIMEOUT_SEC`).
- `TURF_GC_VALIDATE=1` (Linux/glibc/x86-64) — runs an independent full-stack mark phase and panics (`GC root missed`) if the production scan missed a block it reaches; also reports stray caller-saved register hits. Pair it with `TURF_GC_STRESS=1` to compare mark sets on every allocation.
- `TURF_GC_NO_PROCFS=1` — pretend `/proc/self/maps` is unreadable, exercising the stack-base fallback a procfs-less container takes (`scripts/gc_no_procfs_smoke.sh`).
- An ASan runtime: configure a separate `build_asan/` with `-DTURF_RT_SANITIZE=ON`, then `TURF_COMPILER=$(pwd)/build_asan/turfc ASAN_OPTIONS=detect_leaks=0 ./scripts/test_runner.sh`. Both build trees write the plain `lib/libturfrt.a`, so don't build one while a suite runs against the other.

`scripts/gc_stress_corpus_smoke.sh` runs the long-running programs in `tests/custom/gc_stress_corpus/`, which `test_runner.sh` never touches.

Two conventions the runner enforces:
- A file named `fail_*.tr` is expected to **fail to compile**. A plain-named `.tr` file is expected to **compile and run successfully** (for behavioral checks, `import test;` and call `test::assert(cond, "message")` inside `main()`; a bare `assert(...)` is E041 "Unknown function", because `assert` lives in the stdlib's `test` module, not in the prelude).
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

**`scripts/test_runner.sh` only covers `tests/`.** For any change to language syntax or semantics, also grep/compile-check `lib/std/**/*.tr` and `showcase/*.tr` directly — neither is exercised by the test runner (the stdlib is only pulled in by whichever tests happen to `import` it, and showcase programs aren't run by it at all), so a stdlib or showcase file using old syntax can sit broken indefinitely with the full suite reporting green. Confirmed live: a language-syntax change once left two `lib/std/net.tr` (now `lib/std/io/net.tr`) methods on removed syntax, caught only by directly compiling the showcase programs that import `net`.

Compiling one of the 6 prelude modules (`int`/`double`/`bool`/`char`/`strings`/`array` — see `PreludeModuleNames` in `Parser.cpp`, all under `lib/std/core/`) *directly* used to be a false positive: `ParsePreludeImports()` auto-imports all 6 into every top-level file unconditionally, which meant compiling e.g. `lib/std/core/char.tr` itself imported `char` into `char.tr`, registering every one of its own functions twice (once as an ordinary top-level function, once again under its own module namespace) — any internal UFCS/operator self-call (`isAlpha()` calling `c.isUpper()`) then falsely reported "Ambiguous ... call", even though the identical code compiled and ran correctly as a real `import char;`-ing user program. Fixed by having `ParsePreludeImports()` skip a prelude name whose resolved stdlib path is the same file currently being compiled as the entry point (resolved via the same `TurfInstallRoot()`/`IndexModuleDir()` lookup a real import uses, so it can't disagree about where a prelude module actually lives). This direct-compile check (`turfc --resolve lib/std/core/char.tr`, etc.) IS this bug class's only regression coverage — `test_runner.sh` can't exercise it since none of the 6 prelude files have a `main()`.

## Repo map

- `src/`, `include/` — the compiler itself (Lexer, Parser, Resolver, TypeChecker, Codegen, Errors, SLM integration); builds to `turfc`.
- `cli/` — the `turf` front end, a standalone Go module (`github.com/turf-lang/turf-lang/cli`; its one third-party dependency is `github.com/BurntSushi/toml` for `turf.toml`, **vendored** in `cli/vendor/` so the build never needs the network; after adding or upgrading a Go dependency, run `go mod vendor` in `cli/` and commit it, or the build fails on an inconsistent vendor directory). It never parses TurfLang source and never rewrites a compiler diagnostic; see `cli/README.md`.
- `lib/std/` — the standard library, written in Turf itself, grouped into logical subfolders purely for browsing (`core/` — `int`/`double`/`bool`/`char`/`array`/`strings`; `collections/` — `list`/`stack`/`queue`/`map`/`set`; `io/` — `io`/`net`/`sys`; `mem/`, `math/`, `time/`, `random/`, `fs/`, `json/`, `http/`, `testing/` — `test`). No compiler special-casing; ordinary `.tr` files. A module's subfolder is invisible to `import` — `import list;` doesn't care that `list.tr` lives under `collections/`; the compiler resolves a bare module name by recursively indexing `lib/std/**` once per compile (`IndexModuleDir()` in `src/Parser.cpp`), so moving a module between subfolders never requires touching an import statement anywhere. See `docs/architecture/modules_and_scope_resolution.md` Part 2.4 for the full search order and the `TURF_HOME` environment variable that anchors it (points at a Turf installation root — `$TURF_HOME/lib/std`/`lib/usr`; falls back to the compiler binary's own location if unset, so a plain dev build needs no setup).
- `include/turf/Analysis.h` — the compiler query facade (SMP-001): the stable 2.x API a tool (turf-lsp, the SLM Sandbox) uses to analyse source in-process. Never drive the Lexer/Parser/Resolver/TypeChecker directly from a tool; `docs/architecture/compiler_pipeline.md` §8 says why.
- `tests/` — the regression suite, organized by numbered topic folders (`06_structs`, `07_functions_modules`, etc.) plus `tests/99_known_bugs/` (excluded from the runner — documents accepted/deferred limitations, not live regressions) and `tests/custom/` (also excluded).
- `docs/reference/` — the user-facing language reference (one topic per file, `stdlib/` for the standard library), rewritten from scratch 2026-08-21 against the live compiler + test suite. Start at `docs/reference/README.md`. This is what a language-website "docs" section would be built from.
- `docs/architecture/` — deep internals references for specific subsystems, kept accurate and updated alongside the code they describe (e.g. `modules_and_scope_resolution.md` for the `::`/import system, `memory_model.md` for stack/heap/lifetime, `compiler_pipeline.md` for the Lexer→Parser→Resolver→TypeChecker→Codegen pass structure and CFG static analysis, both explained in CS terms rather than LLVM-specific ones). Read the relevant one before making non-trivial changes to that subsystem — don't re-derive from scratch.
- `docs/planning/` — roadmap and planning docs. `ROADMAP_v2.0.0.md` is the plan of record for what remains before the first public release; `FUTURE.md` is the post-2.0 backlog; `ROADMAP_v1.0.0_COMPLETE.md` and `ARCHITECTURE_OVERHAUL.md` are historical records (the Architecture Overhaul closed 2026-09-27 — `docs/development/changes/OVH-CLOSE.md`).
- `scripts/` — `cmake_build.sh` (build), `test_runner.sh` (test), `compile_and_run.sh` (compile+run+cleanup one file), `cli_smoke.sh` (the `turf` front end against the real `turfc`).
- `docs/development/` — **if your task is one of the architecture-overhaul feature groups (`ARCH-001`, `TYPE-001`, `SEM-001`, … — `docs/planning/ARCHITECTURE_OVERHAUL.md` Appendix B), read `docs/development/SESSION_PROTOCOL.md` and follow it**; it adds a freeze document, a change record per feature group, and `scripts/session_{pre,post}flight.sh` on top of everything in this file. This file stays the source of truth for build and test conventions.

## Conventions this project holds firmly to

- **Doc comments explain WHY, not what.** A hidden invariant, a past bug this code prevents, a non-obvious constraint — not a restatement of the code. Applies to functions touched by a fix and closely adjacent ones.
- **Every fix gets a regression test** in `tests/<NN_topic>/`, following that folder's existing naming/numbering. A bug fix needs both directions covered where it makes sense: a `fail_*.tr` proving the bad case is now rejected, and/or a plain `.tr` proving the good case still works.
- **Small, scoped commits.** One logical fix or feature per commit, not batched sweeps, with a message explaining the *why* and the failure scenario that motivated it — future readers (human or agent) should be able to understand a change from its commit message alone.
  - Title format is `type(scope): summary (@username)` — Conventional Commits (`feat`/`fix`/`test`/`docs`/`refactor`/`misc`, optional parenthesized scope) with the committer's own `git config user.name` appended in parens at the very end. Consistent across recent history; match it going forward even though older commits predate it.
  - No `Co-Authored-By` trailer or other AI-attribution text in the message body.
- **Everything a module declares is private unless it says `pub`/`public`** (LEC-002, `FREEZE.md` §6.15): functions, methods, structs, enums, errors, machines, extern fns and struct **fields**. A test companion (`*_module.tr`) or a `lib/std` module that another file uses needs `pub` on exactly what that file touches — a call, a type, a field, a method, a UFCS receiver. `E088` names the file and the spelling to add; `turf --migrate=pub --write <module.tr>...` adds it everywhere (the old behaviour) if you'd rather prune afterwards. A `pub` signature or field may not mention a private type (`E089`). The main program file needs none of this. Don't widen `lib/std` to make a test pass — a test that needs `List.data` should declare its own struct with a `data: T*` field (see `tests/05_arrays_pointers_ffi/raw_pointer_index_inside_unsafe_ok.tr`).
- **A `fail_*.tr` proves the file failed to compile, not that it failed for the reason it names.** After adding or changing one, compile it and read the error code. Two wrong-reason "passes" were caught this way during LEC-002 (a reserved word — `run`, `step`, `pub` — used as a name in a companion; and a test that started failing on E088 instead of the gate it was written for). For anything with a distinct error code, assert the code and a fragment of the hint in `tests/api/analysis_test.cpp` (see `testPrivateSymbolAccess`).
- **Verify claims live, don't assume from a prior fix's scope.** More than once this project has stated a design rule ("`.` is instance-only") as settled fact in a doc without a fresh test that actually tries to break it. If you're about to write "X is enforced" anywhere, try the specific input that would prove it wrong first.
- Full test suite (`./scripts/test_runner.sh`) must stay green before considering any change done.
