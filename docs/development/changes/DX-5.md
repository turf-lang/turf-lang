# DX-5 — Labelled spans

The roadmap item DX-5 (`docs/planning/ROADMAP_v3.0.0.md`, TDP-2-028): the
primary span of a diagnostic says what the underlined code is, and secondary
spans show where an expectation came from. Issue #338.

| | |
|---|---|
| **Status** | landed |
| **Dates** | 2026-11-02 |
| **Line** | 2.3.0 |
| **Depends on** | DX-10 (snapshots: every change shows as a diff), DX-2 (the JSON form) |

## Why

A snippet underlined the error's token and said `Here!`, and the related
locations (`declared here`, `candidate: ...`) were `Note:` lines with a line
and a column to look up. A type mismatch pointed at `immut` rather than the
value, and did not show the type it was compared with.

## How it works

- **Primary label.** `SLM::DiagnosticHints::Label` (`withLabel()` on
  `TurfError` and `TurfWarning`, the hints of a TypeChecker `reportError()`).
  Without one, the code's own label: `ErrorCodes::defaultLabel()`
  (`src/diagnostics/PrimaryLabels.h`, one row per registered code, worded by
  `docs/development/DIAGNOSTICS_STYLE.md`). `diagLabel()` (`Lexer.h`) chooses,
  for the text form, the JSON form and the query facade alike.
- **Secondary spans** are the diagnostic's related locations in its own file,
  on its line or an earlier one, the first three of them
  (`drawnRelated()`); each is labelled with its description. The rest stay
  `Note:` lines.
- **The renderer** (`renderSnippet()`, `Lexer.cpp`) shows every line that
  carries a span, in order, with `┆` for lines left out (one line between
  two shown ones is shown instead). On a line, spans are underlined `^~~`
  (primary) and `---` (secondary) on one row when they do not overlap, and on
  rows below when they do. A row's rightmost label sits beside its
  underline; the others hang below, joined to their span by `│`.
- **Expression spans.** The parser records each expression's source text,
  first token to last, on `ExprAST` (`getSpan()`): `ParseUnary()` for an
  operand, `ParseBinOpRHS()` for an operator and its operands, a cast with its
  operand. `CloneExprAST()` copies it. A written type's end is recorded too:
  `VarDeclExprAST::TypeLoc`, `ParamDecl::TypeLoc`, the new
  `FuncDefExprAST::RetTypeLoc`, and `VariableSymbol::TypeLoc`/`TypeEndCol`.
- **The sites that know the types** put the primary span on the value and say
  its type (`this is an `int``, `src/sema/SpanLabels.h`), and point back at
  the written type (`` `s` is a `string` because of this ``):
  an initializer (E066 and the codes `codeForMismatch()` picks), an
  assignment, an argument when one overload has the arity (E056/E025), a
  `return` (E064) and a condition (E066, the keyword labelled
  `` `if` needs a `bool` ``). A duplicate declaration already had its first
  declaration as a related location.
- **JSON schema 1.1** adds `label`, the primary span's label (`null` with a
  null `span`); `related` entries are the secondary spans, labelled by
  `message`. `docs/reference/schemas/diagnostics.schema.json` and its
  fixtures are 1.1.
- **The query facade.** `turf::Diagnostic::Label`. turf-lsp lists it as the
  first `relatedInformation` entry, at the diagnostic's own range.
- **A note in another module names its file.** `PrivateSymbolAccessError`
  (E088) took a declaration's line and column with no file, so the note read
  as a place in the importing file, and the renderer would have drawn it
  there; `InModuleFile()` (`ErrorsModule.h`) gives it the module's file, as
  `candidateLocation()` does for candidates.

## Tests

- `turf-api-tests` `testPrimaryLabels`: every registered code has a label
  that starts lowercase and ends without a full stop, every label row names a
  registered code, and the facade carries an initializer mismatch's label,
  primary position and secondary span.
- `tests/snapshots/diagnostics/`: every code's text and JSON.
- turf-lsp `scripts/smoke.py` `t_diagnostics_span_labels`.

## Known pitfalls

- A related location with no file is drawn in the diagnostic's file. A raise
  site whose related location is in another module must say which file.
- An expression built by a pass rather than read by the parser has no span;
  the site's own location is used.
- `MaxSecondarySpans` (3): a list of every overload stays notes.
