# Contributing to TurfLang

This repository is the TurfLang compiler (`turfc`), its C runtime, the standard
library, and the `turf` front end. This page is the short version of how to
work on it. [`AGENTS.md`](AGENTS.md) has the full build and test conventions,
and [`docs/development/TESTING.md`](docs/development/TESTING.md) explains the
test suites.

## Reporting a problem

Open an issue on GitHub with the smallest program that shows it, the command
you ran, what happened and what you expected. Include `turf version`. A
compiler crash, an internal compiler error or a wrong diagnostic is always a
bug, whatever the input.

## Building

See [Building from source](README.md#building-from-source) in the README. After
the first build, `cmake --build build -j<n>` rebuilds what changed. Go is
optional: without it, `build/turf` is skipped and `build/turfc` and the test
suite build as usual.

## Before you send a change

1. Run the suite, and the checks for what you touched:

   ```bash
   ./scripts/test_runner.sh
   build/turf-api-tests
   ./build/turf fmt --check lib/std tests
   ```

   [TESTING.md](docs/development/TESTING.md) lists the extra suites for codegen,
   runtime, documentation and stdlib changes.
2. Give a fix its regression test: a `fail_*.tr` that pins the error code it
   must report, and a plain test for the case that must keep working, in the
   `tests/<NN_topic>/` folder for that area.
3. Update the documentation the change affects: `docs/reference/` for the
   language and standard library, `docs/errors/codes/` and `docs/slm/codes/`
   for error codes. Documentation describes the language as it is, never its
   history.

## Conventions

- **One logical change per commit**, with a message that explains why and the
  failure that motivated it. The title is Conventional Commits with your
  `git config user.name` at the end:
  `fix(sema): E130's hint names the real capability (@yourname)`.
- **Every error has a code, a page and a raiser.** A new code is a row at the
  end of `src/diagnostics/ErrorCodes.h` (never renumbered or reused) plus its
  two pages; [TESTING.md](docs/development/TESTING.md#a-new-error-code) has the
  steps.
- **Everything is private unless it says `pub`**, in `lib/std` as in any
  module. A new `pub` item in `lib/std` needs a use in
  `tests/46_stdlib_surface/`.
- **No compiler special cases for the standard library.** `lib/std` is
  ordinary TurfLang; a rule it needs is a rule any module can use.
- **Comments explain why**: the invariant, the constraint, what would break.
- **Check claims by running them.** Before writing that something is enforced,
  try the input that would prove it wrong.

## Where things are

| Path | What |
|---|---|
| `src/` | the compiler, one directory per stage: `frontend/`, `resolve/`, `sema/`, `analysis/`, `codegen/`, `diagnostics/`, `slm/`, `tooling/`, `driver/` |
| `include/turf/` | the public query facade for tools |
| `runtime/` | the C runtime linked into every program, including the collector |
| `lib/std/` | the standard library, written in TurfLang |
| `cli/` | the `turf` front end (Go) |
| `tests/` | the regression suite |
| `docs/reference/` | the language reference |
| `docs/guides/` | task-oriented guides for TurfLang users |
| `docs/architecture/` | how the compiler and runtime work inside |
