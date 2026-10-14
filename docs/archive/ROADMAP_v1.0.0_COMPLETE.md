# Turflang Version 1.0.0 Roadmap
### *The Definitive Single Source of Truth*

This document outlines the exact path to `v1.0.0`. Every item here is derived from a full architectural review of the codebase and the RFC documents. It details the missing pieces required to transition Turflang from an academic compiler into a production-ready, self-hostable programming language.

---

## 1. The Core Philosophy

1. **AOT Native Compilation**: LLVM is the backbone. Memory is managed via `malloc`/`free` in `unsafe` blocks (Phase 3), transitioning to safe references (`&T`) in Phase 4.
2. **Prompt-Oriented Programming (POP)**: Deep integration with an onboard SLM (`@slm`) to generate and heal code using structured context.
3. **C-Interoperability**: First-class FFI (`extern fn`) to call any standard C library function directly.
4. **Multi-Pass Pipeline**: `Parser → Resolver → TypeChecker → Codegen`. Name resolution and semantics are separated from LLVM IR generation.
5. **Errors as Values**: Recoverable errors use `!T` error unions (type-checked, runtime-messaged). Unrecoverable panics use `fail()`.

---

## 2. Phase 1: Finish the Migration (Codegen Detox) ✅

- [x] **Issue #9: Strip Fallback Overload Loops**
- [x] **Issue #10: Strip Semantic Checks from Codegen**
- [x] **Issue #11: Clean Pipeline**

---

## 3. Phase 2: The Grand Syntactic Overhaul ✅

*`PHASE_2_ISSUES.md` (the original per-issue spec doc) was removed 2026-08-04 — everything it tracked is complete and is now this section's checklist below; the full original issue text is retrievable from git history if ever needed.*

**Final Syntax (locked in):**
```turflang
mutable x: int = 5;                           // mutable variable
mut y: double = 3.14;                          // alias for mutable
immutable PI: double = 3.14159;                // runtime-immutable
immut MAX: int = getMax();                     // alias for immutable
fn add(a: int, b: int): int { return a + b; } // function, return type always explicit
struct Point { x: int, y: int }               // struct declaration
mutable p: Point = Point { x: 10, y: 20 };   // named-field struct init
mutable n: int = 3.99 to int;                 // cast with 'to' keyword
mutable r: int = x > 0 give x otherwise -x;  // ternary: give...otherwise
```

- [x] **#20: `assert(cond)` & `fail(msg)`**
- [x] **#12: Variables to `mutable`/`mut` and `immutable`/`immut`**
- [x] **#13: Functions to `fn name(arg: type): returnType {}`**
- [x] **#14: Struct Named-Field Syntax and `StructName { field: val }` Init**  — **superseded**: `StructName { field: val }` is rejected by the compiler today ("'Point { ... }' is no longer valid syntax for constructing a struct", verified 2026-09-17), and `StructName::new(...)`/`::default()` are the only construction forms — so the `Point { x: 10, y: 20 }` line in the "Final Syntax (locked in)" block above is stale history, kept as written for the record. `docs/reference/structs_and_enums.md` is correct and authoritative; the language is not changing here (`ARCHITECTURE_OVERHAUL.md` §5 C-3, `docs/development/changes/ARCH-001.md`).
- [x] **#15: Cast Syntax `x to type`**
- [x] **#16: Unify `if` and Add Ternary `cond give a otherwise b`**
- [x] **#19: Remove `try/onError/raise`**
- [x] **#18: Two-Pass Resolver for Forward Declarations**
- [x] **#17: Strict Semicolons — Delete `RequiresSemicolon()`**

---

## 4. Phase 2 Tail: Diagnostic Infrastructure

*Two small issues appended after Phase 2 completes. They are infrastructure that Phase 3 actively depends on — CFG warnings require `TurfWarning`, and the growing error taxonomy requires structured hints. See `PHASE_3_ISSUES.md` Issues #173 and #174.*

- [x] **#173: `TurfWarning` — Non-Interrupting Diagnostic Class 🔴**
  - **Implementation**: Add `class TurfWarning` to `Errors.h`. Unlike `TurfError::raise()`, `TurfWarning::emit()` calls `DiagnosticEngine::add(Loc, Msg, /*IsWarning=*/true)` and returns normally — no `longjmp`. Every CFG analysis warning uses this class.

- [x] **#174: Decouple Hints from `TurfError` 🔴**
  - **Implementation**: Change `TurfError` constructor to accept a separate `Hint` string: `TurfError(Loc, Msg, Hint)`. `DiagnosticEngine` renders them separately. Migrate all existing error classes in `Errors.h`. Allows the SLM integration to pass the hint string directly to the model as distinct context.

---

## 5. Phase 3: The Language Backbone

*The structural primitives required to build real programs. `PHASE_3_ISSUES.md` (the original per-issue spec doc) was removed 2026-08-04 — everything it tracked is complete and is now this section's checklist below; the full original issue text, including the superseded `catch`/`?`/`error.XYZ` design notes, is retrievable from git history if ever needed. Phase 3 is complete — Issue #189 (for-each loops), the last remaining item, landed 2026-08-02 (see Group B below). Phase 4 (§6 below) is now fully complete, including the memory-model items. Phase 5 (§7) is complete for the item actually scoped into 1.0.0 (the `--smart` retry loop); the rest of that phase's checklist is real future work, deferred to `docs/planning/FUTURE.md`. Diagnostics Polish (§9) is complete. **Turflang 1.0.0 is feature-complete as of 2026-08-21** — see the closing note at the bottom of this document. `docs/planning/FINAL_SPRINT.md`, the scoped dependency-ordered checklist that carried Phase 4's remainder + narrowed Phase 5 + Diagnostics Polish to completion, was removed once everything it tracked landed — same convention as `PHASE_2_ISSUES.md`/`PHASE_3_ISSUES.md` above; retrievable from git history.*

**New syntax introduced in this phase** (illustrative only — kept close to the original design intent rather than kept in sync keystroke-for-keystroke; see `PHASE_3_ISSUES.md`'s "Final Turflang Phase 3 Syntax Reference" for the accurate, current syntax, including the fact that error handling shipped as `error` types + `recover produce`/`recover run`/`recover as x run`, not the `catch`/`?`/`error.XYZ` shown here):
```turflang
// FFI
extern fn strlen(s: string): int;

// Error handling (shipped design — see docs/architecture/errors_and_recover.md)
error MathError { DivisionByZero }
fn divide(a: int, b: int): !int {
    if b == 0 { return MathError::DivisionByZero; }
    return a / b;
}
mut x: int = divide(10, 0) recover produce 0;

// do/while
do { io::printline("once"); } while (x > 0);

// compare/switch
compare (x) {
    matches (1): io::printline("one"); done
    matches (2): io::printline("two"); done
    other: io::printline("other");
}

// enum
enum Color { Red, Green, Blue }
mut c: Color = Color::Red;

// Bitwise
mut flags: int = 0b1010 & 0b1100;
mut shifted: int = 1 << 4;

// First-class functions
fn apply(f: fn(int): int, x: int): int { return f(x); }

// For-each over arrays (any element type) — IMPLEMENTED (#189)
for value in numbers { io::printline(value to string); }
for (index, value) in numbers { io::printline(index to string); }
for user in users { io::printline(user.name); }   // structs/enums work too — only non-arrays are rejected
```

**Group A — Infrastructure (do first):**
- [x] **#175: `extern fn` FFI 🔴**
- [x] **#176: Self-Referential Structs (LLVM Backend) 🔴** (two-step opaque-stub-then-`setBody()` struct creation, see `Codegen.cpp`'s `StructDeclExprAST::codegen()`)
- [x] **#177: `null` Type Enforcement (TypeChecker) 🔴**

**Group B — Language Features:**
- [x] **#178: Default Variable Initialization 🔴**
- [x] **#179: Logical NOT `!` Operator 🔴**
- [x] **#180: `do { } while(cond)` Statement 🔴** — `TOK_DO`/`DoWhileExprAST`, body runs at least once, condition checked after the body (`Resolver::visitDoWhile`/`TypeChecker::visitDoWhile`/`DoWhileExprAST::codegen()`)
- [x] **#181: `compare` Statement (Switch) 🔴**
- [x] **#182: Bitwise Operators (`&`, `|`, `~`, `^`, `<<`, `>>`) 🔴**
- [x] **#183: String Operations (`+`, `==`, `s[i]`) 🔴**
- [x] **#184: `enum` Declaration 🟡**
- [x] **#189: For-Each Loop over Arrays 🟡** *(last Phase 3 item — DONE 2026-08-02)* — `for value in arr { ... }` / `for (index, value) in arr { ... }`, works over the built-in array type (`T[]`) for ANY element type, including structs and enums (not restricted to primitive elements). Non-arrays (scalars, bare structs, `List<T>`/`Map<K,V>`, etc.) and unsized `T[]` array parameters (no runtime length to bound the loop) are rejected with dedicated error messages (`ForEachNonArrayError`/`ForEachUnsizedArrayError` in `Errors.h`), not a crash. Implemented as a dedicated `ForEachExprAST` node (not a mode flag on `ForExprAST`) that desugars in Codegen to a compiler-generated ascending `0..Size-1` counted loop over the array's compile-time-known size — see `ForEachExprAST::codegen()` in `Codegen.cpp`. Regression tests: `tests/04_control_flow/22-29_foreach_*.tr` and `fail_foreach_*.tr`.

**Group C — CFG Static Analysis (Phases 1 & 2):**
- [x] **#185: CFG Phase 1 — Cyclomatic Complexity & Infinite Loop Detection 🔴** (this checklist was stale — `CFG::calculateCyclomaticComplexity()`/`CFG::hasUnbreakableLoop()` are implemented in `src/CFG.cpp` and wired up in `Codegen.cpp`; confirmed live 2026-07-30 while starting #187)
- [x] **#186: CFG Phase 2 — Liveness Pass & Infinite Recursion Detection 🔴** (also stale — `LivenessAnalysisPass` (`src/Analysis.cpp`) and `RecursionWithoutBaseError` (`Errors.h`, raised from `Codegen.cpp`) are both implemented; confirmed live 2026-07-30)

**Group D — Type System Completion:**
- [x] **#187: First-Class Functions 🟡** — `FuncType` (`Types.h`), `fn(T1, T2, ...): R` type syntax (`Parser.cpp`'s `ParseType()`), bare function names as values (`Resolver::visitVariable`'s `ResolvedFunctionRef` path, restricted to the same self-file/self-module/on-demand-import visibility bare calls already get), and indirect calls through a function-typed variable/parameter (`Resolver`/`TypeChecker`/`Codegen`'s `visitFuncCall`/`FuncCallExprAST::codegen()`). Functions can be passed as arguments, stored in variables, and returned from other functions. **Deliberately out of scope**: `Module::func`/`StructName::method` used bare as a value still raises "functions aren't first-class yet" (`Resolver::visitScopeAccess`) — only unqualified function names are first-class values today.
- [x] **#188: `!T` Error Unions + `recover` 🔴** *(last — heaviest)* — DONE 2026-07-31. **Design superseded**: the `catch`/`?`/`error.XYZ` shape below (originally specced in a now-removed `ERROR_HANDLING_RFC.md`, see git history) was replaced before implementation; the actual feature (named `error` types with bare-name variants, `::`-qualified construction, `recover produce`/`recover run`/`recover as x run`) is specced in `docs/architecture/errors_and_recover.md`. All three stages landed: Lexer/Parser/AST syntax (2026-07-30), Resolver/TypeChecker semantics including the bare-error `recover as err` restriction and the "never returns an error" warning (2026-07-31), and Codegen -- `!T` lowers to a fixed `{i1,i64,i8*}` struct, error variants to a malloc'd tagged payload, `recover` to real branch/PHI control flow, `==` on error values to a strcmp-based tag comparison (2026-07-31, verified via direct compile+run, not just compile).

> **⛔ Explicitly Rejected**: `loop { }` keyword. Adds nothing over `while(true)`. `TOK_LOOP` must NOT be added at any point.

---

## 6. Phase 4: Standard Library & Memory Model ✅

*Rewrite the entire standard library in Turflang via `extern fn` FFI. Simultaneously, transition the memory model: raw pointers are locked inside `unsafe { }` blocks (no grace period), and safe references (`&T`) become the default.*

**New syntax introduced in this phase:**
```turflang
// Safe references
mutable val: int = 42;
mutable ref: &int = &val;  // compiler-guaranteed non-null

// Raw pointers — only inside unsafe
unsafe {
    mutable p: unsafe_ptr<Node> = malloc(sizeof(Node)) to unsafe_ptr<Node>;
    free(p);
}

// Generics (type erasure — ONE LLVM struct per generic, not per instantiation)
mutable nums: list<int> = list<int> {};
mutable names: list<string> = list<string> {};
```

- [x] **Struct Pointers & `self` Returns 🔴**
  - **Implementation**: Bridge the value vs pointer gap for structs. Allow `malloc` (`ptr`) to be safely cast to structural pointers, and allow struct methods to return `self` by reference (`*self` / `&self`). Resolves Issues #22 and #23. Confirmed: `raw to Node*` (ptr → struct-pointer cast) compiles and runs; chained self-returning method calls (`c.increment().increment()`) work.

- [x] **Module Search Path & Circular Import Guard 🔴**
  - **Implementation**: Search `<compiler_bin_dir>/lib/std/` first, then `./`. `std::set<std::string> ActiveImports` during parsing; error on duplicate import. Done, and substantially exceeds this original spec — see `docs/architecture/modules_and_scope_resolution.md` for the full on-demand-import/`::`-namespace-resolution/circular-import-diagram system that replaced it.

- [x] **`unsafe { }` Block 🔴** (2026-08-18, narrow scope, not full ARC — see git history for the removed `FINAL_SPRINT.md`'s Track A, which carried this to completion)
  - **Implementation**: `TOK_UNSAFE`/`UnsafeBlockExprAST` gate raw pointer types (`ptr`, `T*`) everywhere outside `lib/std` — declaring one, casting to/from one, or accessing a field through one now requires lexically being inside `unsafe { }`; a struct with a raw pointer field needs its entire declaration wrapped. An ordinary function's parameters/return type may never be raw pointers at all, not even inside `unsafe { }`. Note this uses the existing `ptr`/`T*` types directly, not a new `unsafe_ptr<T>` generic wrapper as this section's own example syntax above shows — that part of the original sketch was dropped as unnecessary scope during implementation.

- [x] **Safe References (`&T`) 🔴** (2026-08-18) — Same track as `unsafe {}` above. `&expr` (address-of) now always produces a `&T`, a compiler-guaranteed non-null reference that auto-derefs like a raw pointer and always safely widens into `T*`/`ptr`; the reverse needs an explicit `to` cast inside `unsafe { }`.

- [x] **Rewrite `std/io.tr` via FFI 🔴**
  - **Implementation**: `@intrinsic` removed entirely (now a hard parse error). `lib/std/io.tr` binds `printf`/`scanf`/`getchar`/`calloc` via `extern fn`; `print`/`printline`/`input` are ordinary Turf functions.

- [x] **Rewrite `std/math.tr` via FFI 🔴**
  - `lib/std/math/math.tr` is fully `extern fn`-based (no `@intrinsic`). Libm bindings (`sqrt`/`sin`/`cos`/`tan`/`floor`/`ceil`/`round`/`log`/`log2`/`log10`/`exp`/`pow`/`fabs`) are all declared via `extern from "m" fn` and directly callable as `math::sqrt(x)` etc.; `abs`/`power`/`min`/`max`/`clamp`/`divide` are hand-written Turf wrappers on top. `math::PI`/`math::E` (`export immut`) landed too.

- [x] **Write `std/string.tr` via FFI 🔴** (shipped as `lib/std/strings.tr`, not `string.tr`)
  - `extern fn`-based (`strlen`, `strstr`, `strncpy`, `toupper`, `tolower`, `malloc`); string `+`/`==`/indexing work at the language level (see #183 above) independent of this module.

- [x] **Write `std/os.tr` via FFI 🟡** (2026-08-21) — `lib/std/os/os.tr`: `env`/`set_env` wrap `getenv`/`setenv`; `argc`/`argv` read from `TurfRuntimeGetArgc`/`TurfRuntimeGetArgv`, two C-ABI helpers the compiler synthesizes directly in IR at codegen time (`CodegenFunc.cpp`'s `EnsureArgvHelpers()`), since `fn main(): int` has no language syntax to declare argc/argv as params. Process exit beyond `main()`'s own return is already covered by `sys::exit` (`sys.tr`), so no os.tr-level wrapper was needed for that. This closes exactly what blocked real programs (env vars + argv) — the broader `os` wishlist (process spawning, etc.) is unscoped stdlib expansion, not a 1.0.0 blocker; see `docs/planning/FUTURE.md`.

- [x] **Write `std/mem.tr` via FFI 🟡**
  - **Implementation**: `lib/std/mem.tr` binds `malloc`/`calloc`/`realloc`/`free` via `extern fn`. Not gated behind an `unsafe {}` block -- the entire `lib/std` tree is exempt from that gate by design (see the `unsafe { }` entry above), so this doesn't need migrating even now that the feature exists.

- [x] **Delete `@intrinsic` and `Builtins.cpp` Monolith 🔴**
  - The `@intrinsic` decorator/keyword is fully removed from the parser (`'@' is not valid here`, hard error). `Builtins.cpp` still exists, but `CodegenIntrinsic()` itself is gone entirely (last step of the now-removed `STRING_UFCS_OPERATOR_CHAR_RFC.md`'s build order, see git history): `lengthof` -- the only other true intrinsic besides `typeof` -- was deleted (`strings::length(s)`, part of the auto-imported prelude, is the replacement), which left `typeof` as `CodegenIntrinsic()`'s only remaining case, so its logic was inlined directly into its one call site instead of kept behind a dispatcher. `assert`/`fail` were never routed through `CodegenIntrinsic()` at all by this point -- they moved to `lib/std/test.tr` earlier. `Builtins.cpp` today only owns `BuiltinInfo`/`RegisterBuiltin`/`LookupBuiltin` (typeof's arity-check registry) and `CodegenPanic` (the compiler's own internal runtime-guard panic sequence, never user-reachable).

- [x] **Generics with Type Erasure 🟡** (implemented as a hybrid, not exactly as specced)
  - Generic instantiations (`List<int>`, `Box<string>`, etc.) get per-instantiation mangled type NAMES (`getMangledName()` → `"List_int"`) for frontend type-identity/validation purposes, but `Codegen`'s `getLLVMType()` resolves every instantiation of a given base to the SAME underlying `llvm::StructType` (generic fields lower to opaque pointers — "Type Erasure: Generic Type Parameters are compiled as void*"), so there genuinely is no LLVM-level code explosion per instantiation, matching the spec's actual goal even though the mechanism differs from "one shared struct, erased type name" as literally described.

- [x] **Operator Overloading 🟡**
  - **Implementation**: Shipped with a different design than originally proposed — a now-removed `generics_and_operators_roadmap.md`'s double-underscore magic-method sketch (`__add__`/`__eq__`/etc.) was superseded before implementation by the `operator` keyword design from a now-removed `STRING_UFCS_OPERATOR_CHAR_RFC.md` §4 (see git history for both): `operator +(lhs: T, rhs: U): V { ... }` at top level, arity distinguishes unary/binary, exact-type-match resolution with coercion off, builtin-pair overloads (`int`/`double`/`bool`/`char`/`Pointer`) rejected at declaration. Covers `[]`/`[]=` indexing too (lvalue-preserving, no pointer returns). Tests: `tests/18_operator_overload/`.

---

## 7. Phase 5: Prompt-Oriented Programming (SLM Integration) — narrowed scope ✅

*The Two-Adapter Architecture (Base: Qwen2.5-Coder-1.5B ~1GB RAM). 1.0.0 narrowed this phase's scope to only the `--smart` retry loop below; `@slm` in-source syntax, structured context serialization, and LoRA fine-tuning are real future work for post-1.0.0 Turflang — see `docs/planning/FUTURE.md`, which supersedes the three unchecked items this section used to list.*

**Architecture Note**: The SLM (`SLMEngine.cpp`) calls `llama.h` directly as a C++ library compiled into the Turflang binary. It does NOT go through Turflang's FFI system.

- [x] **Dynamic Error Context for `--smart` 🟡**
  - **Implementation**: `src/SLMIntegration.cpp` sizes the context window per deterministic error category (tight ±3 for self-contained statement errors, wider for semantic errors needing more surrounding code) and pins relevant out-of-window definitions (e.g. a variable/function's declaration line) into the prompt regardless of distance.

- [x] **`--smart` Retry Loop 🔴** (2026-08-21) — `validateAndApplySuggestion()`'s attempt loop in `SLMIntegration.cpp` (`kMaxSlmAttempts`): on `verifySuggestion()`'s sandbox-compile failure, the real compiler diagnostic (`Sandbox::getValidationDiagnostics()`) is fed back into a fresh `fetchSuggestion()` call as a `"previous_attempt_failed"` prompt field instead of giving up immediately. Capped at 3 total attempts; if all 3 fail, `fix_mode` downgrades to `"NONE"` and the original compiler error surfaces, same as before this loop existed. This is the narrow, `--smart`-only version of what the rest of this phase's original "SLM Query Loop" item described — the `@slm`-block-codegen version of a retry loop (below) is separate and still not built.

---

## 8. Phase 6: Developer Tooling & Ecosystem

*Post-1.0.0 Turflang (v1.x) work — none of this blocks the 1.0.0 tag. See `docs/planning/FUTURE.md` for how this is sequenced relative to the other post-1.0.0 work (POP/`@slm`, stdlib expansion, memory architecture).*

- [ ] **LSP Server (`turf --lsp`) 🔴**
  - **Implementation**: Persistent process, JSON-RPC over stdin/stdout. Serve autocomplete and hover types via `ResolverSymbolTable` and TypeChecker annotations.

- [ ] **`turf test` 🟡**
  - **Implementation**: `test "name" { ... }` block. `turf test` generates a hidden `main()` that invokes all test blocks.

- [ ] **`turf fmt` 🟡**
  - **Implementation**: `FormatVisitor : public ASTVisitor` reconstructs canonical source from the AST.

- [ ] **`tpm` (Turf Package Manager) 🟡**
  - **Implementation**: `turf get <github-url>`. Clones into `.tpm/packages/`, adds to module search path.

---

## 9. Phase 7: Self-Hosting & Bootstrapping — retargeted to Turflang 2.0.0

*The final milestone: writing Turflang in Turflang. Decided 2026-08-21: self-hosting is big enough, and independent enough of everything else post-1.0.0, to be its own major-version milestone rather than a "Phase 7" tail on the 1.0.0 roadmap. The full requirements/bootstrap-cycle/SLM-FFI-binding sketch that used to live in this section is unchanged in substance and now lives in `docs/planning/FUTURE.md` under "Turflang 2.0.0", alongside the prerequisite check (Phase 3's `extern fn` FFI and Phase 4's `std/io.tr` — both already satisfied today).*

---

## Diagnostics Polish ✅

*The error class taxonomy stopped changing once Phase 3 landed, which unblocked this. All three items are now complete (2026-08-21):*

- [x] **Source Spans** — `SourceLocation`'s `EndLine`/`EndCol` are now populated uniformly for *every* token kind (`gettok()` in `Lexer.cpp`, not just identifiers/keywords as originally scoped), and `DiagnosticEngine::printDiag()` consumes them: it underlines the full token width (`^~~~~`) instead of a single `^` whenever an end position was recorded, falling back to a bare `^` only for the rare case with no end position. Genuine multi-token spans (underlining a whole sub-expression like `add(3, "4")`, not just one token) remain unbuilt — a real future item, tracked in `docs/planning/FUTURE.md`, not a 1.0.0 blocker.
- [x] **Error Codes + `turf --explain`** — Every one of the 68 `TurfError` subclasses has a stable `E0NN` code (`include/ErrorCodes.h`'s `AllErrorCodes` table, looked up via `typeid(*this)` at raise-time). `turf --explain E0NN` (`main.cpp`) prints the matching `docs/slm/*.txt` writeup; a curated `Important` subset gets a more insistent footer nudge in `printDiag()` instead of the routine "Run `turf --explain E0NN`" line.
- [x] **Broader "Did You Mean?"** — `SuggestionIndex::suggestVisible()` now covers struct fields (`TypeCheckerCall.cpp`'s field-access check, feeding it the struct's own field names — `user.pasword` → `user.password`), module functions (`ModuleMethodNotFoundError`), and static/instance struct methods (`StructMethodNotFoundError`, shared between `StructName::method()` and `instance.method()` call shapes) — on top of the variable/type/keyword coverage that already existed.

---

## Turflang 1.0.0: Complete

Every phase in this document (1 through 6) is done. Phase 7 (self-hosting) was
deliberately retargeted to Turflang 2.0.0, not dropped — see §9 above. What's
left beyond that — the rest of Phase 5/6's checklists, remaining stdlib
modules, the memory-architecture RFC, and further-out SLM/language-paradigm
sketches — is real, tracked future work, not a 1.0.0 gap: see
`docs/planning/FUTURE.md` for the post-1.0.0 sequencing and
`docs/archive/DESIGN_NOTES.md` for the longer-tail design rationale behind
each item.
