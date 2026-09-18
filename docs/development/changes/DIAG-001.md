# DIAG-001 — Structural error classification and the lint VarDecl gap

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-18 · landed 2026-09-18 |
| **Commits** | (see the commits that follow this record in `git log`) |
| **Sessions** | 1 of 1 |
| **Depends on** | ARCH-001 |
| **Frozen decisions relied on** | F-DIAG-3 (no new `SLM::ErrorCategory` values without an explicit request), §6.9 ("deriving a category by matching text in the message is to be deleted, not extended") |

## Why

Two independent, small, self-contained problems, both named in
`ARCHITECTURE_OVERHAUL.md` §8's DIAG-001 section:

1. `SLM::ErrorCategory` — the label that picks which `docs/slm/*.txt` doc
   gets fed to the SLM and how wide a code-context window it gets — was taken
   from the raising `TurfError` subclass's own `Category_` field when set,
   and otherwise **re-derived by matching substrings in the final rendered
   message text** (`ErrorCategory.h`'s `classifyError()`). That fallback ran
   for every diagnostic with no `TurfError` instance at all: the raw
   `LogErrorAt()` path, used at every syntax/structural error site in the
   Parser, Lexer, and parts of Codegen (169 call sites, confirmed by grep).
   `classifyError()`'s own comment chain already documented several bugs
   this caused (`"for enum"` matching the `for` control keyword, eight
   builtin names wrongly routed to `BUILTIN_FUNCTION`). Verified live during
   this session: several of `classifyError()`'s own dedicated LogErrorAt
   patterns (`"Expected 'in' after loop variable"`, `"Expected '{' after
   while condition"`, `"Expected '{' after elseif condition"`, `"for loop
   range"`, `"Expected 'then' or '{'"`, `"Expected identifier after 'for'"`,
   `"Expected 'step' after range"`, `"Expected '{' after step expression"`)
   no longer matched **any** currently-raised message at all — every one of
   the eight had silently drifted out of sync with the actual wording as
   those messages were reworded over time, exactly the failure mode §6.9
   predicts for text matching and never caught because nothing exercises
   `classifyError()`'s output directly.
2. `LintExpression()` (`src/Lint.cpp`) had no `VarDeclExprAST` branch in its
   dispatch, so every lint check (division-by-zero, self-comparison,
   redundant boolean comparison, ...) was silently skipped for the entire
   right-hand side of every variable declaration. Confirmed live before the
   fix: `mutable x: int = 5 / 0;` compiled with no warning at all.

## Language semantics

No user-visible language semantics changed. This group is diagnostics-routing
and lint-coverage only — the *text* of every existing error/warning message
is unchanged; only which `SLM::ErrorCategory` it carries internally, and
whether an initializer expression gets linted at all, changed.

### Old behaviour
- `LogErrorAt()` took no category; every call site's diagnostic entered the
  SLM path (when `--smart` is on) as `GENERAL`, then `classifyError()` tried
  to re-derive a category from the rendered message text at the moment the
  SLM path ran.
- `mutable x: int = 5 / 0;` compiled with zero warnings.

### New behaviour
- `LogErrorAt(Loc, Msg, Category)` takes an explicit `SLM::ErrorCategory` at
  every one of its 163 real call sites (audited individually this session;
  see *Removed assumptions* for how each was decided). `classifyError()` no
  longer exists.
- `mutable x: int = 5 / 0;` emits `DivisionByZeroLiteralWarning`, the same as
  a bare top-level statement `x = 5 / 0;` always did. Same for self-
  comparison and redundant-boolean-comparison inside a declaration's
  initializer.

### Is this breaking?
No. No error/warning message text changed, no new compile-time rejection was
introduced (the lint fix only makes existing *warnings* fire in a case where
they previously didn't — warnings never block compilation), and no new
`ErrorCategory` value was added (F-DIAG-3).

## Compiler architecture

- **Diagnostics (`include/ErrorCategory.h`)** — `classifyError()` deleted in
  full (the substring-pattern-matching `ErrorCategory` classifier).
  `extractContextKeyword()` is **kept**: despite living in the same file and
  historically being `classifyError()`'s first input, it has two independent,
  legitimate uses that have nothing to do with category derivation —
  doc-path routing (`getCategoryDocPath()`'s `constructDocs` map, used by
  both the live `--smart` pipeline and `turf --explain`) and the structural-
  prior "does the model's patch mention the triggering keyword" scoring
  bonus in `SLMIntegration.cpp`'s `computeStructuralPrior()`. Neither
  consumer re-derives an `ErrorCategory` from it. `categoryFromString()` is
  also kept — it parses the *model's own* self-reported category out of its
  JSON response, an unrelated concern from re-deriving a category from a
  *compiler* message.
- **Diagnostics (`include/Errors.h`, `include/Errors*.h`)** — audited every
  `TurfError`/`TurfWarning` subclass's `Category_` (88 classes). Found the
  audit was already almost entirely done by prior sessions: only two
  `TurfError` subclasses were left at the untagged `GENERAL` default
  (`SemanticError`, `ModuleMethodNotFoundError`), and both already carried a
  doc comment explaining it as a deliberate judgment call, not an oversight
  — left as-is, with `ModuleMethodNotFoundError`'s comment updated to drop
  its now-stale reference to `classifyError()`. `SelfAssignmentWarning`/
  `SelfComparisonWarning` (`TurfWarning`) explicitly set `Category_ =
  GENERAL` themselves for the same documented reason — also left as-is.
- **Diagnostics (`include/Lexer.h`, `src/Lexer.cpp`)** — `LogErrorAt()` gained
  a third parameter, `SLM::ErrorCategory Category = SLM::ErrorCategory::
  GENERAL`, forwarded unconditionally into
  `DiagnosticEngine::add(Loc, Msg, "", IsWarning, {}, Category)` (previously
  called the `(Loc, Msg, IsWarning)` overload, which always passed the
  default `GENERAL`). Inert for a warning-shaped message (the SLM path is
  gated on `!IsWarning`), same as `TurfWarning::Category_`.
- **Parser/Lexer/Codegen (`src/Parser{Core,Decl,Stmt,Literal,Module}.cpp`,
  `src/Lexer.cpp`, `src/CodegenStruct.cpp`, `src/CodegenExpr.cpp`,
  `src/main.cpp`)** — every one of the 163 real `LogErrorAt()` call sites now
  passes an explicit category, decided per call site by reading its message
  and surrounding parse context (not by pattern-matching the message text at
  runtime — a human, reading each site once, this session). See *Removed
  assumptions* for the category-assignment method.
- **Lint (`src/Lint.cpp`)** — `LintExpression()` gained a
  `dynamic_cast<VarDeclExprAST *>` branch, recursing into
  `VD->getInitializer()`, mirroring the existing `ReturnExprAST`/
  `AssignmentExprAST`/... branches' shape exactly.
- **`SLMIntegration.{h,cpp}`** — `fetchSuggestion()` no longer calls
  `classifyError()` as a `GENERAL` fallback; `category` is used exactly as
  received (ground truth only). `contextKeyword` extraction is retained,
  now documented as doc-routing/scoring-only. Stale doc comments referencing
  `classifyError()` as a live fallback (in `Errors.h`, `Lexer.h`,
  `SLMIntegration.h`) were rewritten to describe the current, classifyError-
  free design.

## Runtime changes
None.

## Standard-library changes
None — this group never touches `lib/std`. `lib/std/**` and `showcase/**`
were both swept via `turf --resolve` per file (see *Tests*) to confirm
nothing broke; every failure found is pre-existing and reproduces
identically on the unmodified baseline (verified via `git stash`/rebuild/
compare before restoring this session's changes) — see *Known pitfalls*.

## New invariants
None newly *enforced* by this group. **INV-LINT-1**
(`docs/development/INVARIANTS.md`) stays `planned` (its actual claim —
exhaustive dispatch — is still not built), but its status note is updated to
record that the one concrete instance it was written to describe
(`VarDeclExprAST`'s initializer silently skipped) is now fixed and verified
live. `LintExpression()`'s dispatch is still a sequence of independent
`dynamic_cast` checks, not an exhaustive switch/visitor that would make the
*next* missing node kind loud at compile time — that refactor is a separate,
larger change than DIAG-001 actually specified ("`LintExpression()` recurses
into `VarDeclExprAST`'s initializer") and was deliberately not attempted
here; see *Known pitfalls*.

## Removed assumptions

- **"A diagnostic's category can be safely re-derived from its rendered
  message text."** This was never safe (see *Why*'s eight confirmed-stale
  patterns) and is no longer attempted anywhere in the compiler. A future
  `LogErrorAt()` call site that copy-pastes an existing one without also
  copying its category argument silently gets `GENERAL` — there is no
  runtime fallback left to paper over a forgotten category. `GENERAL` is a
  legitimate, common answer (many `LogErrorAt()` sites genuinely have no
  best-fit category — internal-codegen-invariant messages in
  `CodegenStruct.cpp`, for instance), so nothing *fails* when a category is
  missing; it just gets the loosest, ±8-line SLM context window
  (`contextSizeForCategory()`) instead of a tighter, category-specific one.
- **How the 163 `LogErrorAt()` categories were decided** (so a future
  session extending a file with a new call site can match the convention
  instead of guessing): each site's category mirrors the `TurfError`
  subclass category already used for the *same construct* elsewhere in the
  codebase, not a fresh judgment call per site. Concretely: every
  `ParserDecl.cpp` site (struct/enum/machine/error-type/extern/function/
  operator declaration syntax) is `DECLARATION`, matching every
  `ErrorsModule.h`/`ErrorsStruct.h` class for the same constructs. Every
  for-loop/while/do-while/compare/matches structural syntax error in
  `ParserStmt.cpp` is `CONTROL_FLOW`, matching `ControlFlowError`/
  `UnreachableCodeError`/etc.'s existing tagging — **not** the old
  `classifyError()`'s specific (and, per *Why*, stale) literal-string
  patterns, which were not trusted as ground truth for this audit. Plain
  expression/statement syntax errors (`ParserCore.cpp`, `ParserLiteral.cpp`,
  `Lexer.cpp`, `main.cpp`, and the non-structural half of `ParserStmt.cpp`)
  are `SYNTAX_ERROR`. Array-literal syntax is `ARRAY_ERROR`. `CodegenStruct.cpp`'s
  three messages literally prefixed `"Internal error:"` (plus the sibling
  `"Failed to create LLVM StructType"`/`"Unknown type for field"`, the same
  shape of internal codegen-invariant failure) are `GENERAL` — there is no
  user-facing construct to route them to. `CodegenStruct.cpp`'s
  `"Struct '...' has no field named '...'"` is `UNDECLARED_VAR`, matching
  `UnknownStructFieldError`'s own tag for the identical message shape raised
  from the TypeChecker path. `CodegenExpr.cpp`'s address-of-non-addressable
  message is `GENERAL` (no existing category fits — the same kind of
  judgment call `SemanticError`/`ModuleMethodNotFoundError` already made
  explicitly).

## Tests

- Added `tests/14_cfg_analysis/fail_37_lint_vardecl_div_by_zero.tr`
  (`RUNTIME_ABORT_EXPECTED` — the warning is non-fatal, but `5 / 0` still
  genuinely traps at runtime once compiled), `38_lint_vardecl_self_comparison.tr`,
  `39_lint_vardecl_redundant_bool.tr` — each proves a specific
  `LintBinaryExpr()` check (division-by-zero, self-comparison, redundant
  boolean comparison respectively) now fires from inside a variable
  declaration's initializer, where none of the three fired before this
  session's `Lint.cpp` fix. All three verified live before being added to
  the suite (exact warning text observed, not assumed).
- No dedicated `.tr` test for the category-classification change itself —
  per DIAG-001's own *Testing scope*, classification isn't `.tr`-testable.
  Verified instead by: (1) full `--smart` pipeline exercised end-to-end on a
  live syntax error (missing `;`), confirming no crash and a correctly
  category-routed, sandbox-validated AI suggestion; (2) `lib/std/**` and
  `showcase/**` swept via `turf --resolve` per file, compared against the
  unmodified baseline via `git stash` (see *Known pitfalls*).
- `*_module.tr` naming sweep run in both directions (AGENTS.md's mandatory
  check for any test file added/renamed): none of the three new/renamed
  files match `*_module.tr`. The sweep surfaced one pre-existing,
  **unrelated** hit (`tests/07_functions_modules/export_qualified_cross_module.tr`,
  a real standalone test whose name happens to end in `_module.tr` as a bare
  substring) — not touched, not introduced by this session, out of scope.
- `./scripts/test_runner.sh`: 627/627 passing (624 baseline + 3 new).

## Known pitfalls

- **`lib/std/**` direct-`--resolve` sweep is noisy by design, not a
  regression signal on its own.** Most `lib/std/*.tr` files have no `main()`
  and correctly report "No entry point found" when compiled directly — this
  is expected (they're libraries, meant to be `import`ed, not entry files)
  and unrelated to DIAG-001. A smaller set
  (`lib/std/collections/list.tr`, `lib/std/mem/mem.tr`) report a genuine
  `E034` "already declared" struct-duplication error when compiled directly
  as the entry file — confirmed via `git stash`/rebuild/compare that this
  reproduces identically on the unmodified `refactor/architecture` baseline
  (`a1a243c`), so it predates this session and is out of scope here. Likely
  cause (not investigated further — out of DIAG-001's scope): `main.cpp`'s
  double-parse pre-pass (`docs/architecture/compiler_pipeline.md` §"The
  double-parse wrinkle") registering a struct type into `GlobalSymbolTable`
  twice when the file containing the struct declaration is itself the
  top-level entry file, as opposed to being reached only via `import`. A
  future session touching the pre-pass or `GlobalSymbolTable` struct
  registration should check this before assuming it is new.
- **`INV-LINT-1` is only partially satisfied.** The `VarDeclExprAST` gap
  named by DIAG-001 is closed, but `LintExpression()`'s dispatch is still an
  unordered sequence of independent `dynamic_cast<X*>` checks — a *next*
  missing node kind would still be silently skipped, with no compiler
  warning and no test failure pointing at it. Converting the whole dispatch
  to an exhaustive switch/visitor (as `INVARIANTS.md`'s `INV-LINT-1` row
  suggests as the eventual enforcement mechanism) touches every branch in
  the function, not just the one this group's spec named, and was
  deliberately left for its own future change rather than folded in here
  under DIAG-001's stated scope.
- **A copy-pasted `LogErrorAt()` call site silently defaults to `GENERAL`**
  if the category argument is dropped during the copy — there is no runtime
  fallback left to catch this (that fallback is exactly what was removed).
  Not a compile error, not a test failure; only a slightly worse SLM
  context-window choice. Worth a lint/grep check in a future session if this
  ever becomes a real problem in practice.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| Handbook ch09 (diagnostics/lint) | Documents "a variable declaration's initializer is never linted" as a live pitfall. | Rewrite as historical: "this *was* true (confirmed via `mutable x: int = 5/0;` producing no warning) and is now fixed (DIAG-001) — `LintExpression()` recurses into `VarDeclExprAST`'s initializer as of 2026-09-18." Per §19's row 4, this is a rewrite, not a deletion. |
| `ch10_diagnostics_and_slm.tex` | Describes the classification path, presumably including the old text-matching fallback. | Describe `ErrorCategory` as ground-truth-only: `TurfError::Category_` for typed errors, an explicit `LogErrorAt()` argument for the untyped path. No fallback exists. |

### Examples that became invalid
None — no example in the handbook demonstrated `classifyError()`'s internal
behavior directly (it was never a user-facing API).

### Examples that should be added
A `mutable x: int = 5 / 0;` example for ch09, showing the warning that now
fires (see *Tests* — exact wording available from the three new `.tr` files
and this session's live verification).

### Sections that must NOT change
Every existing error/warning *message* users see is byte-identical to
before this group — only internal routing changed. No handbook section
documenting message wording, hints, or error codes needs to change.

Added to `docs/development/handbook-sync/PENDING.md` accordingly.

## Unresolved questions
None.

## Related records
None yet — first group to touch `ErrorCategory.h`/`Lint.cpp` since ARCH-001.
