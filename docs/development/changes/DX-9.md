# DX-9 — Every diagnostic worded to the guide

The roadmap item DX-9 (`docs/planning/ROADMAP_v3.0.0.md`, TDP-2-028, decision
D-28): `docs/development/DIAGNOSTICS_STYLE.md`, then every compiler message
and hint rewritten to it. Issues #335 (the guide and the headings) and #337
(the rewrite).

| | |
|---|---|
| **Status** | landed |
| **Dates** | 2026-11-01 (guide) · 2026-11-02 (rewrite) |
| **Line** | 2.3.0 |
| **Depends on** | DX-10 (a snapshot of every code, so the rewrite is a diff), DX-5 (labelled spans) |

## Why

A beginner read "Type mismatch!", "Oops! I can't convert 'bool' to a
string!" or "Wait a minute! You already created a variable named 'n'." The
banners repeated the heading, the messages said what the reader did wrong
instead of what the compiler found, code was quoted with `'`, and many hints
explained rather than said what to do.

## What changed

- **Every message** is in the first person, one idea per sentence, names the
  types (`I cannot store a `string` in `n`, which is an `int`.`), quotes code
  in backticks and has no `!`. **Every hint** starts with a verb the reader
  can act on. Covered: the classes in `src/diagnostics/Errors*.h`, the raw
  `LogErrorAt` sites in the lexer, the parser, Codegen and the driver, the
  Resolver, the TypeChecker, the monomorphizer, the flow passes and lint, and
  the annotation checks (`@test`, `@ffi`, `@ownership`, `@deprecated`).
- **No banners.** `TypeError` prints its message alone (its `CustomBanner`
  argument is kept and ignored); `SyntaxError`, `ControlFlowError`,
  `SemanticError`, `ErrorPayloadError`, `InvalidPatternError` and
  `TryMisuseError` print the raise site's sentence. A warning's message has no
  `Warning:` prefix: the heading says `warning`. The JSON form's `title` is
  `null`.
- **An internal compiler error** says it is a compiler bug and where to report
  it.
- **`SpanLabels::aType()`** (`src/sema/SpanLabels.h`) writes a type with its
  article, `an `int`` or `a `string``, for the TypeChecker's messages.
- **Unchanged on purpose:** `Fix:` descriptions (`replace 'a' with 'b'`), which
  `EXPECT-FIX` markers pin and which the fix engine builds from the edit; the
  facts given to `--smart` (`DiagnosticHints::KnownTypes`).

## Tests and checks

- `EXPECT-DIAG` text markers across `tests/` and the fragments in
  `tests/api/analysis_test.cpp` follow the new text; snapshots and the JSON
  fixtures are regenerated.
- **The 2.0 corpus gate checks codes, not text.** `compat_corpus_check.py`
  runs `turfc --test` with `TURF_TEST_DIAG_CODES_ONLY=1`, so a `fail_*`
  program there is held to the codes its `EXPECT-DIAG` lines name and not to
  their quoted text: message wording is outside the compatibility promise
  (D-6, `FREEZE.md` 6.9). Every marked `fail_*` program in the corpus names
  its code.
- **`GENERIC-CODE`** (`tests/DIAGNOSTIC_GAPS.txt`) is listed by hand: with no
  banner, no line of the output says that an E066 is another mistake under
  the type-mismatch code.

## Known pitfalls

- A tool that matches message text breaks with each rewording; codes are the
  contract. turf-lsp's code actions for E031, E145 and E163 read the name in
  backticks (and in single quotes, for 2.2).
- `extractContextKeyword()` (`ErrorCategory.h`) still picks a `--smart` page
  by words in the message; a rewording that drops "struct", "enum", "module"
  or "import" changes the page.
- `Lexer.cpp` picks `UnexpectedToken`'s default hint by the message's start
  (`I did not expect a `;``); the three parser sites that raise it must keep
  that start.
