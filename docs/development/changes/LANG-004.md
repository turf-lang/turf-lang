# LANG-004 — Small language and FFI ergonomics

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-18 · landed 2026-09-18 |
| **Commits** | e54e81e, d96b379, e4ed9c5 |
| **Sessions** | 1 of 1 |
| **Depends on** | ARCH-001 |
| **Frozen decisions relied on** | F-PTR-1 (raw pointers are FFI-only — why item 2 is confined to `extern` call sites), F-STD-3 (`string` remains a NUL-terminated byte pointer — why item 2's coercion is a no-op cast, not a representation change) |

## Why

Three independently-verified papercuts, each blocking a perfectly ordinary
program, none interacting with a frozen decision (`ARCHITECTURE_OVERHAUL.md`
§8 LANG-004):

1. `for i in 0..5 { ... }` failed to parse: *"Expected 'step' after the
   range end value."* Every range `for` had to spell out its own `step`
   clause, even for the overwhelmingly common increment-by-one case.
2. `extern fn puts(s: ptr): int32; puts("hi");` failed to type-check: *"No
   overload of 'puts' matches argument types (string)"* — despite `string`
   and `ptr`/`T*`/`void*` already sharing the identical LLVM representation,
   with the documented workaround being to declare the FFI parameter as
   `string` instead of `ptr`.
3. `extern fn` could not be declared inside a function body: *"Expected ';'
   after statement."*, pointing at the line *after* the extern, not the
   extern itself.

## Language semantics

### Old behaviour
- A range `for` required `step`: `for i in 0..10 step i++ { }`.
- `puts("hi")` against `extern fn puts(s: ptr): int32;` was a type error.
- `extern fn` only parsed in the top-level declaration switch.

### New behaviour
- `step` is optional; omitting it is exactly `step <var>++`:
  `for i in 0..10 { }` now compiles and behaves identically to the explicit
  form (verified live — see *Tests*).
- A `string` argument (literal or runtime-built) coerces to a `ptr`/`T*`/
  `void*` parameter **at an `extern fn` call site only** — `puts("hi")`
  now compiles.
- `extern fn` may be declared inside a function body, is registered live at
  its declaration point (Pass 2), and is callable from there onward in the
  file's sequential resolution order (see *Known pitfalls* for the exact
  scoping this does and doesn't give you).

### Is this breaking?
No. All three changes are strictly more permissive — every program that
compiled before this change still compiles and behaves identically (no
existing test's expected pass/fail status changed; the full suite stayed
green throughout, 628 → 632 with the four new tests this session added).

## Compiler architecture

- **Parser** (`src/ParserStmt.cpp`)
  - `ParseForExpr()`: when `CurTok != TOK_STEP` after the range end value,
    instead of raising `Expected 'step' ...`, synthesizes
    `PostfixExprAST(ForLoc, '+', VariableExprAST(ForLoc, VarName))` as the
    step node — the identical AST an explicit `step <var>++` produces, so
    every later pass (Resolver/TypeChecker/Codegen) treats the default and
    explicit forms indistinguishably. `step { ... }` (the keyword present
    with no expression) is still correctly rejected — only the keyword's
    *absence* is now accepted.
  - `ParseBlock()`: gained a `TOK_EXTERN` branch calling `ParseExternDecl()`
    directly, mirroring the existing `TOK_EXPORT` branch immediately above
    it and for the identical reason — `ParseExternDecl()` already consumes
    its own trailing `;`, so falling through to the generic
    expression-statement fallback (which expects to consume its own `;`)
    would misfire against whatever token comes *after* the one already
    consumed.
- **Resolver** (`src/ResolverDecl.cpp`, `Resolver::visitExternFuncDecl()`)
  - Now checks `FunctionNestingDepth_ > 0` (the same counter
    `visitFuncDef()` already uses for `NestedFunctionError`). At depth 0
    (ordinary top-level extern, already hoisted by `hoistTopLevel()` in
    Pass 1) behaviour is unchanged — type validation only. At depth > 0
    (a function-local extern, which Pass 1 never sees — see *Removed
    assumptions*), the function now builds a `ResolverFunctionSymbol`
    exactly mirroring `hoistTopLevel()`'s own `ExternFuncDeclExprAST`
    branch and calls `SymTable_.registerCurrentFileFunction(Sym)` itself.
- **TypeChecker** (`src/TypeCheckerCall.cpp`)
  - New static predicate `isExternStringToPtrCoercion(ArgTy, ParamTy)`:
    true iff `ArgTy->isString()` and `ParamTy` is pointer-like
    (`isPointer() && !isReference()`, or named `"ptr"`/`"void*"`).
  - Wired into the type-compatibility filter at both call-resolution sites
    that iterate a candidate's parameters against argument types —
    `visitFuncCall()`'s direct-call path and `visitMethodCall()`'s
    module-qualified-call path — as `!typesCompatible(...) &&
    !(Cand->IsExtern && isExternStringToPtrCoercion(...))`. `Cand->IsExtern`
    is the existing `ResolverFunctionSymbol` flag distinguishing a real
    `extern fn` from an ordinary function; every non-extern candidate never
    even evaluates the new predicate.
  - **Not** touched: `typesCompatible()` itself (`src/Types.cpp`), UFCS
    call resolution (`visitMethodCall()`'s receiver-binding path), and
    Codegen — `CastToType()` already treats any pointer-like-to-pointer-like
    cast as a no-op (`CodegenCore.cpp`, confirmed by inspection and by this
    session's own live test passing with no Codegen change).
- **Codegen**: no changes. `ExternFuncDeclExprAST::codegen()` only ever
  creates a module-level `llvm::Function` — it never reads or depends on
  the current `IRBuilder` insertion point, so invoking it while another
  function's body is mid-codegen (the function-local case) was already
  safe before this session touched anything; confirmed by reading the
  function, not assumed.

## Runtime changes
None.

## Standard-library changes
None. `lib/std/**` swept directly (`turf --resolve` per file) — no
behavior change, same two pre-existing unrelated `E034` failures
(`lib/std/collections/list.tr`, `lib/std/mem/mem.tr`) already documented in
DIAG-001/HYG-001's own records, reconfirmed present on the unmodified
baseline via `git stash` before this session touched anything.

## New invariants
None. LANG-004's own spec names no new `INV-*`, and none of the three
items introduces a structurally-enforced property distinct from what
already exists (item 2 *reuses* the existing FFI-only pointer gate as its
scoping mechanism rather than adding a new one).

## Removed assumptions

- **"`Resolver::visitExternFuncDecl()` is a pure validation pass; the
  symbol is always already registered by the time Pass 2 reaches it."**
  True only for a top-level extern. `hoistTopLevel()` never enters a
  function body — that split is the entire reason Pass 1/Pass 2 exist (see
  `docs/architecture/compiler_pipeline.md` §3) — so a function-local extern
  (newly legal as of this session) reaches `visitExternFuncDecl()` with no
  prior registration at all. Verified live before adding the
  `FunctionNestingDepth_ > 0` registration branch: the parser accepted
  `fn main(): int { extern fn abs(x: int): int; return abs(-42); }` but the
  call to `abs` failed to resolve ("unknown function"), i.e. the syntax fix
  alone would have shipped a feature that parses but can never be used.
- **"A function-local declaration is naturally scoped to its enclosing
  function, the way a local variable is."** Not true for a function-local
  `extern fn` as implemented here — see *Known pitfalls*. This project has
  no block-scoped-function concept anywhere else to reuse (a nested `fn` is
  rejected outright by `NestedFunctionError`), so there was no existing
  scoping mechanism to extend; building a genuinely scoped one was judged
  out of proportion to "one switch case" (the spec's own sizing) and is
  flagged, not silently assumed away.
- **"`string`→`ptr` coercion needs a Codegen-side cast."** It does not —
  `CastToType()` (`CodegenCore.cpp`) already treats any pointer-like value
  cast to a pointer-like destination type as a no-op (`return Val;`, no new
  branch needed), confirmed by reading the function before writing the
  TypeChecker fix, not assumed from the "same LLVM representation" claim
  alone.

## Tests

- `tests/04_control_flow/30_for_range_default_step.tr` — a plain `int`
  range `for` with no `step`, compared against the same range with an
  explicit `step i++` (asserts identical accumulated sums, not just a
  hardcoded total, so an off-by-one couldn't silently cancel out), plus a
  `double`-ranged default-step case exercising `PostfixExprAST`'s `double`
  branch specifically.
- `tests/04_control_flow/fail_for_range_malformed_step.tr` — `step` is
  optional, but when the keyword IS written it must still be followed by a
  real expression; `for i in 0..5 step { ... }` must still be rejected, not
  silently treated as if `step` had been omitted. Verified live before
  writing the test.
- `tests/05_arrays_pointers_ffi/19_ffi_string_to_ptr_coercion.tr` — an
  `extern fn strlen(s: ptr): int;` called with both a string literal and a
  runtime-built string (`"hel" + "lo"`, string `operator +`), so the fix
  isn't accidentally literal-only.
- `tests/05_arrays_pointers_ffi/20_extern_fn_function_local.tr` — a
  function-local `extern fn abs`, called and asserted on, proving
  resolution actually works end-to-end (not just that the syntax parses).
- Live-verified, not `.tr`-tested (no test for this exists or is needed —
  ordinary functions can never declare a raw-pointer parameter at all,
  `checkNoRawPointersInSignature()`, so the "does item 2 leak into ordinary
  code" question can't even be posed as a `.tr` program): `fn takesPtr(p:
  ptr): int { ... }` still fails at the signature itself with `E066`,
  before argument matching is ever reached.
- `./scripts/test_runner.sh`: 632/632 passing (628 baseline + 4 new).
- `*_module.tr` naming sweep run in both directions: none of the four new
  test files match `*_module.tr`, in substring or suffix form. The sweep
  surfaced the same pre-existing, unrelated hit DIAG-001/HYG-001 already
  documented (`tests/07_functions_modules/export_qualified_cross_module.tr`)
  — not touched, not introduced by this session.
- `lib/std/**` and `showcase/**` swept via direct `turf --resolve`/full
  compile per file: no new failures. The six prelude modules
  (`lib/std/core/{int,double,bool,char,strings,array}.tr`) still compile
  directly with no "Ambiguous ... call" regression.

## Known pitfalls

- **A function-local `extern fn` is registered file-wide, not lexically
  scoped to its enclosing function.** `CurrentFileFunctions_` (the registry
  `registerCurrentFileFunction()` writes into) is a flat, per-file table
  with no block-scoping concept — nothing else in this compiler's
  declaration model has one (a nested `fn` is rejected outright). Once a
  function-local extern is registered, during Pass 2's sequential
  top-to-bottom walk, it becomes callable from any function reached
  *later* in that walk, not just from within its own declaring function. A
  function reached *earlier* in the walk cannot see it (Pass 2 already
  finished visiting that function's body before the extern was
  registered), which does at least preserve ordinary declare-before-use
  within a single function. Building genuine lexical scoping would need a
  real scoped-function-symbol mechanism — explicitly out of scope here
  (`ARCHITECTURE_OVERHAUL.md` §8 LANG-004: *"Low value, but it is one
  switch case"*). For a binding meant to be reused across a whole file,
  file-scope declaration remains the clearer choice; this is documented as
  a real limitation, not silently left as a surprise.
- **The default `step` is synthesized at `ForLoc` (the `for` keyword's own
  location), not at the range end value's location.** Matters only for
  diagnostic caret placement on a hypothetical future error raised from
  inside the synthesized step node itself — none exists today (a bare
  `<var>++` on an already-resolved loop variable cannot itself fail to
  resolve or type-check), so this is not observed to matter in practice,
  only worth knowing if a future change adds a check that could fire here.
- **`isExternStringToPtrCoercion()` is checked only at the two call-sites
  named above (`visitFuncCall`, `visitMethodCall`'s module-qualified
  path).** It is deliberately NOT checked in `visitMethodCall()`'s UFCS
  receiver-binding path — calling a `ptr`-parameter extern via UFCS syntax
  (`"hi".puts()`, if such a thing were ever declared) is unaffected by this
  session and still resolves however it did before. Not verified either
  direction, since no such UFCS-on-extern case is known to exist in
  `lib/std`/`showcase`/`tests` today; flagged for whoever next touches UFCS
  resolution to be aware the two paths diverge here.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| `docs/reference/control_flow.md` §"`for` — range form" | States `step` is currently mandatory. | See `handbook-sync/PENDING.md`'s LANG-004 row — full replacement text queued there, not edited directly (`docs/reference/` is paused per that file's own header). |
| `docs/reference/ffi_and_unsafe.md` §1 | Documents `string`→`ptr` as unsupported and function-local `extern fn` as a parse-time gotcha with a specific (now stale) error-message walkthrough. | Same — queued in `handbook-sync/PENDING.md`, not edited directly. |
| `docs/planning/DESIGN_NOTES.md` §4 | Listed all three gaps as open. | **Done in this session** — edited directly (this file is not part of the paused handbook/reference set), replaced with a pointer to this record. |

### Examples that became invalid
None in already-published material — `docs/reference/` was never edited to
describe the old mandatory-`step`/no-coercion/no-local-extern state as
anything other than current fact, so nothing there is now internally
contradictory; it is simply stale until the `PENDING.md` queue is worked.

### Examples that should be added
The three worked examples from this record's *Language semantics* section
(bare-`step` range `for`, `string`-argument `extern fn` call, function-local
`extern fn`), once `docs/reference/` resumes.

### Sections that must NOT change
Every other part of `docs/reference/control_flow.md` (for-each form,
`break`/`continue`, `compare`) and `docs/reference/ffi_and_unsafe.md` (the
raw-pointer gate, `unsafe {}`, `&T`) is untouched by this session and still
accurate.

## Unresolved questions
None.

## Related records
None — this is the first session to touch the `for`/FFI ergonomics surface
under the change-tracking system.
