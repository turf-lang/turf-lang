# Testing the compiler

How the TurfLang compiler is tested, for someone changing it: what each suite
covers, how to run it, and how to add a test that will catch the bug you just
fixed. `AGENTS.md` holds the same conventions in short form; this page explains
them.

A project's own tests use the same runner and the same markers. That side is
in [Testing a TurfLang project](../guides/testing.md) and the reference,
[Projects and packages: `turf test`](../reference/projects_and_packages.md#tests-turf-test).

---

## 1. What runs, and when

| Suite | What it checks | Run it after |
|---|---|---|
| `scripts/test_runner.sh` | every `.tr` file under `tests/` | any change |
| `scripts/test_runner.sh -O0` | the same at `-O0` | codegen, runtime or GC changes |
| `build/turf-api-tests` | the in-process query facade (`include/turf/Analysis.h`) | a change to the front end's global state, or a new error code |
| `scripts/check/*.sh`, `*.py` | the repository against itself: error codes, docs, stdlib surface, boundaries | the matching area (section 6) |
| `scripts/smoke/*_smoke.sh` | whole tools and programs the runner never touches | the matching area (section 7) |

CI runs all of them, the suite at every optimisation level. Build first:
`cmake --build build -j<n>` rebuilds `build/turfc`, `build/turf` (when Go is
installed) and `build/turf-api-tests`.

Two habits save time. Use about half your cores (`cmake --build build -j8`,
`TURF_TEST_JOBS=8`), so the machine stays usable. And do not export
`TURF_HOME` for a checkout: it points every build at one checkout's `lib/std`,
so another worktree or build directory silently tests the wrong standard
library. Unset it (`env -u TURF_HOME ...`) if your shell sets it.

## 2. The regression suite

```bash
./scripts/test_runner.sh            # the compiler's default level
./scripts/test_runner.sh -O0        # every compile at -O0
./scripts/test_runner.sh -O2 -g     # a level and debug info
```

It is a thin wrapper over `turfc --test tests/`, the same runner `turf test`
uses for a project. The wrapper adds what is particular to this repository: it
skips `tests/custom/` and `tests/99_known_bugs/` (not live regressions), and
it enforces `tests/DIAGNOSTIC_GAPS.txt` (section 4).

`TURF_TEST_JOBS` caps how many tests run at once, `TURF_TEST_TIMEOUT_SEC`
bounds one test's run (raise it for `TURF_GC_STRESS=1`), and
`TURF_TEST_EXTRA_EXCLUDE` (space-separated paths under `tests/`) skips tests a
special build cannot pass by design. `TURF_COMPILER` runs the suite against
another `turfc`, such as a sanitizer build.

To run one folder, give the runner that directory:

```bash
TURF_TEST_JOBS=8 build/turfc --test tests/36_numeric_semantics
```

`--test` takes a directory, not a file. For one file, compile it (and run it,
for a plain test) yourself:

```bash
build/turfc tests/36_numeric_semantics/10_int_power_exact.tr -o /tmp/t && /tmp/t
```

### The layout

Tests live in numbered topic folders, `tests/01_syntax/` to
`tests/52_arrays_of_references/` and on. Put a test in the folder for the
feature it exercises and follow that folder's naming; add a folder only for a
new area. `tests/api/` holds the facade tests' C++ source, not `.tr` files.

## 3. Kinds of test

The file name decides what passing means:

| File | Passes when |
|---|---|
| `name.tr` | it compiles, runs, and exits 0 |
| `fail_name.tr` | it fails to compile (a compiler crash does not count) |
| `fail_name.tr` with `// RUNTIME_ABORT_EXPECTED` | it compiles, then exits non-zero |
| `name_module.tr` | never run: a helper another test imports |

A plain test checks behaviour with the `test` module:

```turflang
import test;

fn main(): int {
    test::assert_eq(7 / 2, 3, "integer division truncates");
    test::assert((-7) % 2 == -1, "the remainder takes the dividend's sign");
    return 0;
}
```

A bare `assert(...)` is E041: `assert` lives in the `test` module, not the
prelude.

### Pin the reason, not just the outcome

A `fail_*` test proves the file was rejected, not that it was rejected for the
reason it was written for. A typo in the test, or an unrelated error the
compiler hits first, passes it just as well. So the runner requires every
compile-time `fail_*` test to say what the compiler must report:

```turflang
// EXPECT-DIAG: E148
// EXPECT-DIAG: 'Point' is not generic -- it takes no type arguments
```

The markers, all matched literally against the output with colour stripped:

| Marker | Holds when |
|---|---|
| `// EXPECT-DIAG: E011` | the compile output contains `[E011` |
| `// EXPECT-DIAG: some text` | ... contains this text (use it for a warning, which has no code) |
| `// EXPECT-NO-DIAG: some text` | ... does not contain it (the shape of a false-positive or cascade fix) |
| `// EXPECT-FIX: replace 'nmae' with 'name'` | the output has a `Fix:` line with exactly this description |
| `// EXPECT-RUNTIME: some text` | the program's own output contains it (abort tests) |
| `// EXPECT-EXIT: 134` | the program exits with this status (an abort that prints nothing) |

Every line must hold, and they work on plain, `fail_*` and abort tests alike: a
plain test can pin the warnings it compiles with. Match prose, never column
numbers. Where one code covers several mistakes (E066 is the type-mismatch
catch-all; E056 covers several "no such overload" shapes), pin the message
title or a phrase of the message too, since the code alone cannot tell them
apart.

### Pin the fix

An error whose raise site knows the exact edit prints a `Fix:` block in every
build (`src/diagnostics/CompilerFix.cpp`; `docs/reference/cli_and_diagnostics.md`,
"Fixes"). The runner treats the fix as part of what a test pins:

- Each `// EXPECT-FIX: <description>` must match a printed `Fix:` line exactly
  (`replace 'nmae' with 'name'`, `remove 'pub'`, `insert ';' at the end of line 12`).
- In this repository's suite (run with `--diag-register`, as
  `scripts/test_runner.sh` does), every printed `Fix:` line must have a marker.
  A `fail_*` test that starts printing a fix fails until it is pinned, so a new
  fix cannot appear unreviewed. A project's own `turf test` has no register,
  so there the marker is opt-in.
- Then (always here, and in a project only for a test with a marker) the runner reruns the compiler with `--smart-write-fix`, analyses the
  fixed program in-process, and requires it to have at least as many fewer
  errors as fixes printed and no error the original lacked.
- To pin that a fix is *not* offered (an ambiguous typo), use
  `// EXPECT-NO-DIAG: Fix:`; see `tests/55_compiler_fixes/`.

`TURF_FIX_DEBUG=1` prints a `[fix]` line to stderr for each error with fix-its
the fix path looks at, and why its fix was not shown (an error without
fix-its, such as an ambiguous typo, prints none). `python3 scripts/dev/diagnostic_fix_coverage.py`
counts, per error code, how many `fail_*` tests print a hint and a fix
(`--fixes` lists each fix, `--list` each code).

A `RUNTIME_ABORT_EXPECTED` test must carry `EXPECT-RUNTIME` (or `EXPECT-EXIT`
when the abort is silent by design). Because stdout is flushed before a panic,
an abort test can also check what a program printed: print, then panic, and
pin both.

After writing a `fail_*` test, compile it and read the error. Wrong-reason
passes have been found this way more than once, for example a test failing on
a reserved word used as a name instead of on the rule it was written for.

### Helper modules: the `_module.tr` trap

The runner skips any file whose name contains `_module.tr` (a substring match,
not a suffix of the meaningful name), treating it as a helper with no `main`.
That cuts both ways:

- a real test named `something_module.tr` is silently never run;
- a helper not named `*_module.tr` is run as a test and fails with "No entry
  point found".

Sweep both directions after adding or renaming a test:

```bash
# tests wrongly skipped: named *_module.tr but have a main()
find tests -name "*_module.tr" | xargs grep -lE '\bfn\s+(int\s+)?main\s*\('
# helpers wrongly run: no main(), not *_module.tr, not fail_*
find tests -name "*.tr" -not -name "*_module.tr" -not -name "fail_*" \
  | xargs grep -LE '\bfn\s+(int\s+)?main\s*\('
```

Everything a module declares is private unless it says `pub`, so a helper
needs `pub` on exactly what its test uses (E088 names what to add). Do not
widen `lib/std` to make a test pass; declare what the test needs in the test.

## 4. The diagnostic register

`tests/DIAGNOSTIC_GAPS.txt` lists the `fail_*` tests whose diagnostic is not
pinned, and the diagnostics a beginner-friendly compiler should not produce:

| Kind | Meaning |
|---|---|
| `UNPINNED` | not yet confirmed to fail for its stated reason; only ever shrinks |
| `WRONG-REASON` | rejected, but for a different reason than the test was written for |
| `NO-CODE` | an error printed with no `[Exxx]`, so `turf explain` has nothing to say |
| `GENERIC-CODE` | the right error under a catch-all code (an E066 that is not "Type mismatch!") |
| `CASCADE` | one mistake, several errors |

The runner checks every entry against the real output, so a fixed entry left in
the file fails the suite: delete the line when you fix it. When a test cannot be
pinned to the diagnostic it was written for, register it rather than weaken the
test. New work does not add `UNPINNED` lines. The file's header has the exact
format and the fix for each kind.

## 5. Every fix gets a test

A bug fix comes with the test that would have caught it, in the folder for its
area: a `fail_*` test showing the bad case is now rejected, and a plain test
showing the good case still works, where both make sense. A crash found by the
fuzzer (section 7) gets its minimised input as a `fail_*` test.

When a fix changes what users see, the same commit updates the reference pages
under `docs/reference/` and the error pages `docs/errors/codes/Exxx.txt` and
`docs/slm/codes/Exxx.txt`. They describe the language as it is, never its
history.

### A new error code

Every error has a code, and every code has two pages and a raiser:

1. Add a constant and a table row at the end of
   `src/diagnostics/ErrorCodes.h` (never renumber or reuse a number).
2. Raise it: `reportError(Loc, ErrCode::X, ...)` in the type checker,
   `LogErrorAt(Loc, ErrCode::X, ...)` in the parser, lexer or codegen. The
   code is a required parameter in each, so forgetting it does not compile.
3. Write `docs/errors/codes/Exxx.txt` (for people) and
   `docs/slm/codes/Exxx.txt` (for the `--smart` model). Write their examples by
   running them.
4. Add a `fail_*` test pinning it, and an assertion of the code and a phrase of
   the message in `tests/api/analysis_test.cpp`.
5. Update the code count and category table in
   `docs/reference/cli_and_diagnostics.md`.

`scripts/check/error_codes_check.sh` fails on a code missing a page or a raiser.

## 6. Checks over the repository

| Script | Fails when |
|---|---|
| `scripts/check/error_codes_check.sh` | a registered code has no human or SLM page, or nothing raises it |
| `scripts/check/doc_examples.py` | a ```` ```turflang ```` block in `docs/reference/` or `docs/guides/` does not compile (or compiles when marked `<!-- doc-example: fails Exxx -->`) |
| `scripts/check/check_doc_links.sh` | a link or anchor between docs is broken |
| `scripts/check/stdlib_surface_check.sh` | a `pub` item of `lib/std` is used by no file in `tests/46_stdlib_surface/`, or a public name is not `snake_case` |
| `scripts/check/prelude_shadow_check.sh NAME...` | not a CI gate: run it before adding a name to `io`, `math` or `sys`. It exits 1 and lists `path:line` for every `showcase/`, `examples/`, `tests/` and `lib/std` program that already declares a function of that name |
| `scripts/check/conformance_check.py` | `tests/CONFORMANCE.md` or `tests/CONFORMANCE_ITEMS.md` names a test that does not exist, or a frozen decision has no evidence |
| `scripts/check/check_boundary.sh` | the compiler/runtime/stdlib boundary rules are broken (`docs/architecture/boundary.md`) |
| `scripts/slm/slm_doc_examples_check.sh` | a `CORRECT` example under `docs/slm/` does not compile |
| `scripts/slm/slm_prompt_budget.sh` | the `--smart` system prompt no longer fits the model's batch |
| `build/turf fmt --check lib/std tests` | a `.tr` file there is not formatted (`turf fmt --write` fixes it) |

Adding a `pub` item to `lib/std` means adding a use of it to
`tests/46_stdlib_surface/` in the same change; renaming or removing one breaks
the 2.x surface.

Adding a function to `io`, `math` or `sys` also adds a bare-callable name
(`FREEZE.md` §6.25), which a program that declares its own function of that
name hides. Run `scripts/check/prelude_shadow_check.sh NAME` for each new name
and read every program it lists before adding the name; a hit is a case to
look at, not automatically a break.

## 7. Smoke scripts and other suites

The runner only covers `tests/`. These cover the rest:

- `scripts/smoke/tree_programs_smoke.sh`: every `lib/std` module checked as a
  module, the prelude modules compiled directly, and every `showcase/` program
  at `-O0`, `-O2` and `-g` with `llvm-dwarfdump --verify`. Run it after any
  language-rule change, since a stdlib or showcase file using a removed form
  is otherwise never compiled.
- `scripts/smoke/opt_levels_smoke.sh`: the showcase programs give the same
  output at every level.
- `scripts/smoke/runtime_smoke.sh`, `gc_smoke.sh`, `mem_smoke.sh`: the C
  runtime and collector from C.
- `scripts/smoke/fmt_smoke.sh`: the formatter's modes and that it is idempotent
  over the repository. Run it after touching `src/tooling/Formatter.cpp`.
- `scripts/smoke/test_cmd_smoke.sh`: breaks each test convention on purpose and
  checks the run fails for that reason. Run it after changing
  `src/tooling/TestRunner.cpp` or `TestExpectations.cpp`; a suite of passing
  tests cannot show a convention still bites.
- `scripts/smoke/cli_smoke.sh`, `examples_smoke.sh`, `release_smoke.sh`: the
  `turf` front end, the `examples/` projects, and an installed release archive.
- `scripts/slm/slm_replay_smoke.sh`: the `--smart` path without a model
  (`TURF_SLM_REPLAY`). Run it after changing `src/slm/SLMIntegration.cpp` or
  `Sandbox.cpp`.
- `scripts/slm/slm_deterministic_fix_smoke.sh`: the compiler's own fixes end to
  end. Each fixture prints its `EXPECT-FIX` with and without `--smart`; `--smart`
  never reaches the model; the program `--smart-write-fix` writes compiles and
  runs; a clean compile never enters the fix path. Run it after changing
  `src/diagnostics/CompilerFix.cpp`, a `withFixIt()` site, or
  `src/slm/SLMIntegration.cpp`.

Each POSIX smoke script with a Windows meaning has a PowerShell twin of the
same name; change both together.

### The garbage collector

Conservative stack scanning can hide a missed root at `-O2`, when a value
happens to survive in a register. Run `tests/41_gc/` (or the whole suite) at
`-O0` after any change to `runtime/turfrt_gc.c` or the code that allocates.
Every compiled program also reads:

- `TURF_GC_STRESS=1`: collect on every allocation, the surest way to expose a
  missed root;
- `TURF_GC_VALIDATE=1` (Linux, glibc, x86-64): an independent full-stack mark
  that panics `GC root missed` if the real scan missed a block;
- `TURF_GC_NO_PROCFS=1`: behave as if `/proc/self/maps` were unreadable.

For an ASan-instrumented runtime, configure a separate `build_asan/` with
`-DTURF_RT_SANITIZE=ON` and run the suite with
`TURF_COMPILER=$(pwd)/build_asan/turfc ASAN_OPTIONS=detect_leaks=0`.

### Crashes and the fuzzer

Invalid input must produce a diagnostic, never a crash.
`scripts/dev/fuzz/crash_fuzz.py` mutates the repository's own programs and
compiles each; a signal, a sanitizer report, an assertion, `LLVM ERROR`, an
internal compiler error, a hang or exit status 2 is a finding, minimised into
`fuzz-findings/`. Run it against a compiler built with
`-DTURF_SANITIZE_COMPILER=ON` (`ulimit -s 65536` first: ASan's larger frames
overflow the default stack on the deep-nesting tests).
