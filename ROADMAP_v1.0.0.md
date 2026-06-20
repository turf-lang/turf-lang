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

*See `PHASE_2_ISSUES.md` for full issue descriptions.*

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
- [x] **#14: Struct Named-Field Syntax and `StructName { field: val }` Init**
- [x] **#15: Cast Syntax `x to type`**
- [x] **#16: Unify `if` and Add Ternary `cond give a otherwise b`**
- [x] **#19: Remove `try/onError/raise`**
- [x] **#18: Two-Pass Resolver for Forward Declarations**
- [x] **#17: Strict Semicolons — Delete `RequiresSemicolon()`**

---

## 4. Phase 2 Tail: Diagnostic Infrastructure

*Two small issues appended after Phase 2 completes. They are infrastructure that Phase 3 actively depends on — CFG warnings require `TurfWarning`, and the growing error taxonomy requires structured hints. See `PHASE_3_ISSUES.md` Issues #171 and #172.*

- [ ] **#171: `TurfWarning` — Non-Interrupting Diagnostic Class 🔴**
  - **Implementation**: Add `class TurfWarning` to `Errors.h`. Unlike `TurfError::raise()`, `TurfWarning::emit()` calls `DiagnosticEngine::add(Loc, Msg, /*IsWarning=*/true)` and returns normally — no `longjmp`. Every CFG analysis warning uses this class.

- [ ] **#172: Decouple Hints from `TurfError` 🔴**
  - **Implementation**: Change `TurfError` constructor to accept a separate `Hint` string: `TurfError(Loc, Msg, Hint)`. `DiagnosticEngine` renders them separately. Migrate all existing error classes in `Errors.h`. Allows the SLM integration to pass the hint string directly to the model as distinct context.

---

## 5. Phase 3: The Language Backbone

*The structural primitives required to build real programs. See `PHASE_3_ISSUES.md` for full issue descriptions.*

**New syntax introduced in this phase:**
```turflang
// FFI
extern fn strlen(s: string): int;

// Error unions
fn divide(a: int, b: int): !int {
    if b == 0 { return error.DivisionByZero("cannot divide by zero"); }
    return a / b;
}
mutable x: int = divide(10, 0) catch 0;
mutable y: int = divide(10, 2)?;

// do/while
do { io.printline("once"); } while (x > 0);

// compare/switch
compare (x) {
    matches (1): io.printline("one"); done
    matches (2): io.printline("two"); done
    other: io.printline("other");
}

// enum
enum Color { Red, Green, Blue }
mutable c: Color = Color.Red;

// Bitwise
mutable flags: int = 0b1010 & 0b1100;
mutable shifted: int = 1 << 4;

// First-class functions
fn apply(f: fn(int): int, x: int): int { return f(x); }
```

**Group A — Infrastructure (do first):**
- [ ] **#173: `extern fn` FFI 🔴**
- [ ] **#174: Self-Referential Structs (LLVM Backend) 🔴**
- [ ] **#175: `null` Type Enforcement (TypeChecker) 🔴**

**Group B — Language Features:**
- [ ] **#176: Default Variable Initialization 🔴**
- [ ] **#177: Logical NOT `!` Operator 🔴**
- [ ] **#178: `do { } while(cond)` Statement 🔴**
- [ ] **#179: `compare` Statement (Switch) 🔴**
- [ ] **#180: Bitwise Operators (`&`, `|`, `~`, `^`, `<<`, `>>`) 🔴**
- [ ] **#181: String Operations (`+`, `==`, `s[i]`) 🔴**
- [ ] **#182: `enum` Declaration 🟡**

**Group C — CFG Static Analysis (Phases 1 & 2):**
- [ ] **#183: CFG Phase 1 — Cyclomatic Complexity & Infinite Loop Detection 🔴**
- [ ] **#184: CFG Phase 2 — Liveness Pass & Infinite Recursion Detection 🔴**

**Group D — Type System Completion:**
- [ ] **#185: First-Class Functions 🟡**
- [ ] **#186: `!T` Error Unions (`catch`, `?`, `error.XYZ`) 🔴** *(last — heaviest)*

> **⛔ Explicitly Rejected**: `loop { }` keyword. Adds nothing over `while(true)`. `TOK_LOOP` must NOT be added at any point.

---

## 6. Phase 4: Standard Library & Memory Model

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

- [ ] **Module Search Path & Circular Import Guard 🔴**
  - **Implementation**: Search `<compiler_bin_dir>/lib/std/` first, then `./`. `std::set<std::string> ActiveImports` during parsing; error on duplicate import.

- [ ] **`unsafe { }` Block 🔴**
  - **Implementation**: `TOK_UNSAFE`, `UnsafeBlockExprAST`. TypeChecker: any raw pointer type (`T*`, `unsafe_ptr<T>`) or `malloc`/`free`/`realloc` call outside an `unsafe` block is a compile error. No grace period — enforced from day one of Phase 4.

- [ ] **Safe References (`&T`) 🔴**
  - **Implementation**: `TOK_AMPERSAND` in type position produces `SafeRefType`. TypeChecker guarantees non-null. Codegen lowers to a regular pointer (same IR — the safety is purely a compile-time contract).

- [ ] **Rewrite `std/io.tr` via FFI 🔴**
  - **Implementation**: Replace all `@intrinsic` entries. Bind `printf`, `fopen`, `fread`, `fwrite`, `fclose`, `fgets` via `extern fn`.

- [ ] **Rewrite `std/math.tr` via FFI 🔴**
  - **Implementation**: Replace all `@intrinsic(\"llvm.sin.f64\")` etc. Bind `sin`, `cos`, `sqrt`, `pow`, `floor`, `ceil`, `round`, `fabs` from `libm`. Add native `min`, `max`, `clamp`. Expose `PI` and `E` as module-level `immutable` declarations.

- [ ] **Write `std/string.tr` via FFI 🔴**
  - **Implementation**: Bind `strlen`, `strcmp`, `strstr`, `strcpy`, `strcat`, `toupper`, `tolower`, `malloc`. Implement `length`, `equals`, `concat`, `contains`, `indexOf`, `trim`, `split`, `startsWith`, `endsWith`, `toUpper`, `toLower`.

- [ ] **Write `std/os.tr` via FFI 🟡**
  - **Implementation**: Bind `getenv`, `exit`. Expose `env`, `exit`. Argc/argv hook.

- [ ] **Write `std/mem.tr` via FFI 🟡**
  - **Implementation**: Bind `malloc`, `realloc`, `free` as `mem.alloc`, `mem.realloc`, `mem.free`. Only callable from `unsafe { }` blocks after the `unsafe` enforcement is in place.

- [ ] **Delete `@intrinsic` and `Builtins.cpp` Monolith 🔴**
  - **Implementation**: Once all stdlib modules use `extern fn`, delete `TOK_INTRINSIC`, `@intrinsic` parsing, `CodegenIntrinsic()`, `IntrinsicArity`, and `EarlyArityMap`. The TypeChecker is now the sole authority for arity and type validation.

- [ ] **Generics with Type Erasure 🟡**
  - **Implementation**: `list<T>` and `map<K,V>` compile to a single LLVM struct (`{ i8* data, i64 length, i64 capacity }`) regardless of `T`. The TypeChecker tracks the element type for validation. No code explosion. Trade-off: no LLVM-level specialization (can be added opt-in with `@specialize` post-1.0.0 if needed).

- [ ] **Operator Overloading 🟡**
  - **Implementation**: Magic methods (`__add__`, `__eq__`, `__index__`, etc.) on structs. TypeChecker desugars `Matrix + Matrix` into a `MethodCallExprAST`. Codegen sees only a normal method call — zero LLVM-level changes required.

---

## 7. Phase 5: Prompt-Oriented Programming (SLM Integration)

*The Two-Adapter Architecture (Base: Qwen2.5-Coder-1.5B ~1GB RAM).*

**Architecture Note**: The SLM (`SLMEngine.cpp`) calls `llama.h` directly as a C++ library compiled into the Turflang binary. It does NOT go through Turflang's FFI system.

- [ ] **`@slm` Syntax & Parser 🔴**
  - **Implementation**: `TOK_SLM_BLOCK`. `SLMBlockExprAST`. Parser generates from `@slm { "prompt" }`.

- [ ] **Structured Context Serialization 🔴**
  - **Implementation**: `serializeSymbolContext()` extracts the current function signature and all visible variables/types from `ResolverSymbolTable` to build the prompt prefix.

- [ ] **The SLM Query Loop 🔴**
  - **Implementation**: `SLMBlockExprAST::codegen()` invokes `SLMEngine`. Returned string passes through `Lexer → Parser → Resolver → TypeChecker`. Retry up to 3× on TypeChecker failure, feeding the error back into the prompt.

- [ ] **Dynamic Error Context for `--smart` 🟡**
  - **Implementation**: Size context window dynamically: ±3 lines for syntax errors, ±8 lines for semantic errors.

- [ ] **LoRA Fine-Tuning 🟡**
  - **Implementation**: Convert `needs_slm_training_dynamiccontext/` to JSONL for Adapter A (Error Healing). Convert `lib/std/` to JSONL for Adapter B (POP Codegen).

---

## 8. Phase 6: Developer Tooling & Ecosystem

- [ ] **LSP Server (`turf --lsp`) 🔴**
  - **Implementation**: Persistent process, JSON-RPC over stdin/stdout. Serve autocomplete and hover types via `ResolverSymbolTable` and TypeChecker annotations.

- [ ] **`turf test` 🟡**
  - **Implementation**: `test "name" { ... }` block. `turf test` generates a hidden `main()` that invokes all test blocks.

- [ ] **`turf fmt` 🟡**
  - **Implementation**: `FormatVisitor : public ASTVisitor` reconstructs canonical source from the AST.

- [ ] **`tpm` (Turf Package Manager) 🟡**
  - **Implementation**: `turf get <github-url>`. Clones into `.tpm/packages/`, adds to module search path.

---

## 9. Phase 7: Self-Hosting & Bootstrapping

*The final milestone: writing Turflang in Turflang.*

**Requirements for Self-Hosting:**
- Phase 3 complete (specifically **`extern fn` FFI**, to bind LLVM's C API).
- Phase 4 complete (specifically **`std/io.tr`**, to read/write source files and object files).

**Bootstrap cycle:**
1. Write new compiler logic using *existing* Turflang features.
2. Compile using the *current* compiler binary.
3. Output is a new binary that understands the new feature.
4. Use the new feature in everyday Turflang programs.

**SLM in the Self-Hosted Compiler:** The `@slm { }` syntax is for user programs. The compiler's own SLM calls become ordinary `extern fn` FFI calls binding `llama.cpp`'s C API:

```turflang
extern fn llama_backend_init(): bool;
extern fn llama_model_load_from_file(path: string, params: unsafe_ptr<void>): unsafe_ptr<void>;
extern fn llama_decode(ctx: unsafe_ptr<void>, batch: unsafe_ptr<void>): int;
```

---

## Diagnostics Polish (Phase 5/6, not before)

*The error class taxonomy is still changing through Phase 3. These are deferred until the error surface is stable.*

- [ ] **Source Spans** — Track `{Line, StartCol, EndCol}` per token. Underline the full failing expression, not just `^`.
- [ ] **Error Codes + `turf --explain`** — Every error class gets a unique code (e.g., `E042`). `turf --explain E042` prints a multi-paragraph markdown explanation with examples.
- [ ] **Broader "Did You Mean?"** — Extend `SuggestionIndex` to cover struct fields (`user.pasword` → `user.password`) and module methods (`string.lenght` → `string.length`).
