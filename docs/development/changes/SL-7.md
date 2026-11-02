# SL-7 — Compiler fixes for a wrong argument count and a missing return type

The rest of TDP-2-001's rule layer (`docs/planning/ROADMAP_v3.0.0.md` SL-7):
the two error classes DX-1 left without a `Fix:`. Issue #339.

| | |
|---|---|
| **Status** | landed |
| **Dates** | 2026-11-02 |
| **Line** | 2.3.0 |
| **Depends on** | TDP-2-001 (the checked `Fix:`), F-S4a (`--check` reports every diagnostic) |

## Why

A call with one argument too many or too few, and a function that declares
no return type, are among a beginner's commonest errors. The compiler often
knows the one edit that fixes them, but not at the place the error is raised:
the Resolver checks argument counts before any type is known, and the parser
reports a missing return type before it has read the body.

## How it works

- **Alternatives.** `SLM::DiagnosticHints::FixAlternatives`
  (`src/diagnostics/ErrorCategory.h`) holds fixes the raise site cannot choose
  between, each a whole fix with its own edits, reason and description
  (`TurfError::withFixAlternative()`). `checkCompilerFix()` (`Lexer.cpp`)
  checks every one with `CompilerFixes::check()`, and the error gets a `Fix:`
  only when exactly one passes. Two that pass mean the compiler cannot tell
  which was meant, and the error keeps its hint alone. An error whose
  alternatives exceed the compile's remaining check budget gets none, since
  an unchecked alternative cannot be ruled out.
- **E044, wrong argument count.** The parser records where each argument and
  the `)` are written (`CallArgSpans`, on `FuncCallExprAST` and
  `MethodCallExprAST`). `CompilerFixes::arityAlternatives()` builds, for each
  overload one argument away: with one too many, the removal of each argument
  (with the `,` that joins it to a neighbour); with one too few, the variable
  named like the missing parameter, placed at that parameter's position. The
  Resolver offers it only when a variable of that name is in scope; the
  TypeChecker, which keeps no scopes, offers every parameter's name and the
  re-check rejects one that names no variable. An insertion is skipped when it would push an
  argument already named for its parameter out of place, or would pass a
  variable the call already passes. A call written across lines, or with more
  than `MaxArityAlternatives` (6) alternatives, gets none. The re-check does
  the type matching the Resolver cannot: `describe(3, true, 7)` against
  `describe(count: int, done: bool)` keeps only the removal of `7`.
- **E106, no return type.** When `{` follows the `)` of a function's
  parameters, `MissingReturnTypeHints()` (`ParserDecl.cpp`) offers `: void`,
  `: int`, `: double`, `: bool`, `: string` and the parameters' own types,
  inserted after the `)`. The re-check analyses the whole program, so the body
  and every caller decide: `fn twice(x: int) { return x * 2; }` called as
  `immut y: int = twice(4);` keeps only `: int`.

## Tests

- `tests/55_compiler_fixes/`: `fail_fix_extra_argument.tr`,
  `fail_fix_missing_argument.tr`, `fail_fix_method_missing_argument.tr`,
  `fail_fix_missing_return_type.tr`,
  `fail_fix_missing_return_type_void.tr` (each `EXPECT-FIX`), and
  `fail_fix_none_when_arity_ambiguous.tr` (`EXPECT-NO-DIAG: Fix:`).
- `scripts/slm/slm_deterministic_fix_smoke.sh` runs the five fixtures with a
  fix: the fix is printed with and without `--smart`, `--smart` never reaches
  the model, and the fixed program compiles and runs.

## Known pitfalls

- Each alternative costs one analysis of the program, out of the ten a compile
  checks (`CompilerFixes::MaxChecksPerCompile`). An E106 uses up to six.
- `return 0;` fits `int` and `double` alike: a function whose body and callers
  allow both gets no fix.
