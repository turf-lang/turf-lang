# Phase 3 — GitHub Issues: The Language Backbone

> All issues in this document depend on **Phase 2 (Issues #12–#20)** being fully merged first.
> Issues #173 and #174 are Phase 2 tail items — diagnostic infrastructure that Phase 3 depends on.
> They must be completed before Phase 3 proper begins.
> The ordering within Phase 3 is strict — see **Execution Order** at the bottom.

---

## Final Turflang Phase 3 Syntax Reference (locked in)

> **2026-08 status note**: everything in this block is now implemented —
> FFI, `compare`, `enum`, logical NOT, bitwise ops, string ops, default init,
> first-class functions, `do`/`while`, and error handling. The one exception
> is the **error-handling example below**, which still shows the original
> `catch`/`?`/`error.XYZ`/`match` design — that design was superseded before
> implementation. The real, shipped feature uses named `error` types and
> `recover produce` / `recover run` / `recover as x run` instead; see
> Issue #188's own status note and `docs/planning/Errors.md` for the accurate
> syntax. Also, module-qualified calls and enum-value access use `::`, not
> `.` (`io::printline(...)`, `Color::Red`) — this example block predates
> that change and was left as-is below rather than silently rewritten. The
> first-class-function example still uses `io.printline`-era `.` module
> syntax in spirit but is otherwise real, working code today (only
> unqualified `fn` names are first-class values — see Issue #187's status
> note). **Phase 3 is now complete** — Issue #189 (for-each loops over
> arrays), its last remaining item, landed 2026-08-02; see its own section
> below.

```turflang
// FFI — bind a C library function
extern fn strlen(s: string): int;
extern fn malloc(size: int): ptr;   // ptr = opaque raw pointer (i8*), Phase 3 only
extern fn free(p: ptr): void;

// do/while — IMPLEMENTED (#180)
do {
    io.printline("iterating");
} while (x > 0);

// compare (switch — no body fall-through, multi-value matching supported)
compare (x) {
    matches (1):
        io.printline("one");
    done

    matches (2):
        io.printline("two");
    done

    other:
        io.printline("something else");
}

// compare — multi-value arm (stacked matches, shared body)
compare (x) {
    matches (1):
    matches (2):
    matches (3):
        io.printline("one, two, or three");
    done
}

// enum
enum Color { Red, Green, Blue }
mutable c: Color = Color.Red;

// Logical NOT
if !done { io.printline("still going"); }

// Bitwise operators
mutable flags: int = a & b;
mutable mask:  int = a | 0xFF;
mutable flip:  int = ~a;
mutable xord:  int = a ^ b;     // ^ is XOR; ** or math.pow for exponentiation
mutable left:  int = 1 << 4;
mutable right: int = 256 >> 2;

// String operations
mutable greeting: string = "Hello, " + name;
mutable same:     bool   = name == "world";
mutable first:    int    = name[0];  // character access (int)

// Default initialization (no explicit value required)
mutable count: int;      // 0
mutable ratio: double;   // 0.0
mutable flag:  bool;     // false
mutable label: string;   // ""
mutable ptr:   Node*;    // null

// First-class functions — IMPLEMENTED (#187)
fn apply(f: fn(int): int, x: int): int {
    return f(x);
}

fn double_it(n: int): int { return n * 2; }

mutable result: int = apply(double_it, 5); // 10

// Error unions — this design is SUPERSEDED. #188 shipped instead as named
// `error` types + `recover produce`/`recover run`/`recover as x run` — see
// Issue #188's status note and docs/planning/Errors.md for the real syntax:
//
//   error MathError { DivisionByZero }
//   fn divide(a: int, b: int): !int {
//       if b == 0 { return MathError::DivisionByZero; }
//       return a / b;
//   }
//   mut x: int = divide(10, 0) recover produce 0;
```

---

## Completed Issues (2026-07 cleanup — full specs removed, see git history if needed)

All of the following are implemented and confirmed working in the current compiler; their full Description/Implementation/Acceptance-Criteria sections were removed from this file since they no longer describe open work. See `docs/planning/ROADMAP_v1.0.0.md` for the tracked checklist and `changelog/issueNNN.txt` for the ones with a historical writeup.

- **#173: `TurfWarning`** — non-interrupting diagnostic class, used by CFG/lint warnings.
- **#174: Decoupled hints** — `TurfError(Loc, Msg, Hint)`, rendered separately by `DiagnosticEngine`.
- **#175: `extern fn` FFI** — the entire stdlib (`io`, `mem`, `math`, `strings`, `list`, `stack`, `queue`) is now FFI-based.
- **#176: Self-referential structs** — two-step opaque-stub-then-`setBody()` struct creation in `Codegen.cpp`.
- **#177: `null` type enforcement**.
- **#178: Default variable initialization** — `mutable x: int;` zero-initializes.
- **#179: Logical NOT `!`**.
- **#181: `compare` statement** — `TOK_COMPARE`/`ParseCompareExpr()`, multi-value `matches` arms.
- **#182: Bitwise operators** (`&`, `|`, `~`, `^`, `<<`, `>>`).
- **#183: String operations** (`+`, `==`, `s[i]`).
- **#184: `enum` declaration**.
- **#185: CFG Phase 1** — cyclomatic complexity + infinite-loop-with-no-exit `TurfWarning`s (`CFG::calculateCyclomaticComplexity()`/`hasUnbreakableLoop()`).
- **#186: CFG Phase 2** — liveness analysis (`LivenessAnalysisPass`, dead-store/unused-variable `TurfWarning`s) + infinite-recursion-without-base-case detection (`RecursionWithoutBaseError`).
- **#187: First-class functions** — `fn(T1, T2, ...): R` type syntax; bare unqualified function names usable as values (arguments, variable initializers, return values); indirect calls through a function-typed variable/parameter. See its own section below for the deliberately-out-of-scope parts (`Module::func`/`StructName::method` as values).
- **#180: `do { } while(cond)`** — `TOK_DO`/`DoWhileExprAST`, body always runs at least once, condition checked after the body. Implemented in `Resolver::visitDoWhile`/`TypeChecker::visitDoWhile`/`DoWhileExprAST::codegen()`.
- **#188: Error handling** — shipped as named `error` types (`error MathError { DivisionByZero }`, bare-name variants, no payloads) plus `recover produce <value>` / `recover run { ... }` / `recover as <name> run { ... }` at `!T` call sites, not the original `catch`/`?`/`error.XYZ`/`match` design. See its own section below and `docs/planning/Errors.md` for the full, accurate spec.

Note: enum-value and module-qualified access now use `::` (`Color::Red`, `io::printline(...)`), not `.` as shown in some of the original issue text — see `docs/architecture/modules_and_scope_resolution.md`.

---

## Issues #180, #185, #186, #187 — do/while, CFG Phase 1, CFG Phase 2, First-Class Functions

Implemented and confirmed working (see the "Completed Issues" summary above for
#180/#185/#186/#187's one-line descriptions and the relevant source files); their
full Description/Implementation/Acceptance-Criteria sections were removed
here, same convention as the #173-#184 cleanup above. See git history for the
original specs if needed.

**#187's deliberately-out-of-scope note** (not covered by the original spec
above, discovered while implementing): only an unqualified, bare `fn` name
resolves as a first-class function value (an argument, a variable
initializer/reassignment, or a return value) — `Module::func` and
`StructName::method` used bare as a value still raise "functions aren't
first-class yet" (`Resolver::visitScopeAccess`). Extending first-class-ness to
qualified references is a real, separate follow-up, not started.

---

## Issue #188 — Error Handling (`error` Types + `recover`)

**Status: DONE (2026-07-31).** Shipped with a different design than originally proposed. The `catch`/`?`/`error.XYZ`/`match` design that used to be specced in full here (and in `ERROR_HANDLING_RFC.md`) was replaced before implementation started with named `error` types (bare-name variants only, no payloads) and three `recover` forms at `!T` call sites: `recover produce <value>`, `recover run { ... }`, `recover as <name> run { ... }`. Full spec: `docs/planning/Errors.md`. Implemented across Lexer/Parser/AST (`TOK_RECOVER`/`TOK_PRODUCE`/`TOK_RUN`/`TOK_AS`, `RecoverExprAST`), Resolver/TypeChecker (`visitRecoverExpr`, `TurfErrorUnionType`, the bare-error `recover as err run` restriction, "function never returns an error" warning), and Codegen (`!T` lowers to a `{i1, i64, i8*}` struct, `recover` to real branch/PHI control flow, `==` on error values to a strcmp-based tag comparison). Its original Description/Lexer/AST/Parser/Resolver/TypeChecker/Codegen/Acceptance-Criteria sections were removed from this file since they describe the superseded design, not shipped work — see git history if the old proposal is needed for reference.

---

## Issue #192 — For-Each Loop over Arrays

**Labels:** `parser`, `lexer`, `ast`, `resolver`, `typechecker`, `codegen`, `phase-3`
**Depends on:** Arrays (done), first-class functions (done, unrelated but confirms Phase 3's other language-backbone pieces are in place)
**Status: DONE (2026-08-02).** This was the last item blocking Phase 3 completion — Phase 3 is now complete in full. Shipped as a dedicated `ForEachExprAST` AST node (not a mode flag on `ForExprAST`) mirroring `ForExprAST`'s handling across every pipeline stage (Resolver/TypeChecker/Codegen, plus the CFG/Lint/Analysis/ASTPrinter side-passes). Codegen desugars to a compiler-generated ascending `0..Size-1` counted loop over the array's compile-time-known size (`ArrayType::getSize()`) — see `ForEachExprAST::codegen()` in `Codegen.cpp`. `break`/`continue` reuse the same `LoopBlocks` mechanism every other loop construct uses.

Scope decision made during implementation, beyond what this issue originally specified: unsized array *parameters* (`T[]`, `ArrayType::getSize() == -1`) are rejected with a dedicated `ForEachUnsizedArrayError`, not silently mis-compiled — such a parameter lowers to a bare opaque pointer with no length carried at runtime at all (`getLLVMType()` in `Codegen.cpp`), so there's no safe way to bound the loop. Non-array iterables (scalars, bare structs, `List<T>`/`Map<K,V>`, error unions) are rejected with `ForEachNonArrayError`, naming the actual type.

Regression tests: `tests/04_control_flow/22_foreach_single_var.tr` through `29_foreach_nested_loops.tr` (single-var form, paired `(index, value)` form, struct/enum/nested-array element types, `break`/`continue`, loop nesting), and `tests/04_control_flow/fail_foreach_*.tr` (non-array target, unsized-array parameter, pair-form-over-non-array, pair-form-with-range conflict, duplicate `(x, x)` names).

### Description

Turf already has a range-based `for` (`for i in 0..10 step i += 1 { ... }`, see `ForExprAST`), but no way to iterate directly over the elements of an array. Add a for-each form that iterates an array's elements without the caller managing an index or bound manually:

```turflang
for value in numbers {
    io::printline(value to string);
}

for user in users {   // users: User[5] — array of structs works exactly the same way
    io::printline(user.name);
}
```

**Scope restriction — the built-in array type (`T[]`), for any `T`.** "Primitive array" here means Turf's own built-in, fixed-size array type as opposed to a library/generic collection (`List<T>`, `Map<K, V>`, etc.) — it does **not** mean the array has to hold primitive elements. An array of anything — `int[]`, `User[]` (structs), `Color[]` (enums), even arrays of arrays — is a valid for-each target. What's rejected is for-each over an operand that **isn't an array at all**: a bare scalar (`int`, `double`, `bool`, `string`, `char`), a struct value used directly (not wrapped in an array), a `List<T>`/`Map<K,V>` (out of scope for this issue — those aren't the built-in array type), an error union, etc. Attempting to for-each over any of these must produce a clear compile-time error naming the actual type involved (e.g. something like `for-each requires an array, got 'int'`), not a crash and not a silent miscompile.

**Loop-variable forms:**
- `for value in numbers { ... }` — binds each element value in turn.
- `for (index, value) in numbers { ... }` — binds both the element's index and its value. This is the form intended to scale to future iterables (e.g. `List<T>`, `Map<K, V>`) once for-each grows beyond the built-in array type, so its shape (a parenthesized `(index, value)` pair) should be chosen with that generalization in mind even though this issue only needs it to work for arrays.

**Errors required:**
- For-each over a non-array expression (a scalar, a bare struct, a `List<T>`/`Map<K,V>`, an error union, etc.) → clear compile-time error naming the actual type, not a crash.
- The `(index, value)` form over a non-array follows the same error case.

### Acceptance Criteria

- `for value in numbers { ... }` compiles and iterates every element of an array in order, regardless of element type (primitive, struct, or enum).
- `for (index, value) in numbers { ... }` binds both correctly (`index` is `int`, `value` matches the array's element type).
- `break`/`continue` inside a for-each loop behave the same as in the existing range-based `for`/`while`/`do-while`.
- For-each over a non-array value is rejected with a specific, actionable error message naming the actual type.
- Once this lands, Phase 3 is complete in full.

---

## Execution Order

This order minimizes breakage and respects all dependency chains. Items already done are struck through in spirit (kept for historical dependency context, not re-litigated):

```
#173 (TurfWarning)                — DONE
#174 (hint decoupling)            — DONE

── Phase 3 proper ──

#175 (extern fn FFI)              — DONE
#176 (self-referential structs)   — DONE
#177 (null enforcement)           — DONE

#178 (default initialization)     — DONE
#179 (logical NOT !)              — DONE
#180 (do/while)                   — DONE
#181 (compare/matches)            — DONE
#182 (bitwise operators)          — DONE
#183 (string operations)          — DONE
#184 (enum)                       — DONE

#185 (CFG Phase 1)                — DONE
#186 (CFG Phase 2)                — DONE

#187 (first-class functions)      — DONE
#188 (error handling: error types + recover) — DONE

── Last item ──

#189 (for-each over arrays)       — DONE (2026-08-02)
```

**Phase 3 status:** Complete. #189 (for-each loops) was the last remaining item — it landed 2026-08-02. Every other Phase 3 item, including #180 and #188 whose "OPEN" status was stale in earlier versions of this doc, was already confirmed implemented in the current codebase (source-code grep, 2026-08-02). Phase 4 (`docs/planning/ROADMAP_v1.0.0.md` §6) is now the active phase.
