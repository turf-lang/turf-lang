# Turflang Version 1.0.0 Roadmap
### *The Definitive Single Source of Truth*

This document outlines the exact path to `v1.0.0`. Every item here is derived from a full architectural review of the codebase. It details the missing pieces required to transition Turflang from an academic compiler into a production-ready, self-hostable programming language.

---

## 1. The Core Philosophy

1. **AOT Native Compilation**: LLVM is the backbone. No garbage collector; memory is managed via `malloc`/`free` or custom allocators.
2. **Prompt-Oriented Programming (POP)**: Deep integration with an onboard SLM (`@slm`) to generate and heal code using structured context.
3. **C-Interoperability**: First-class FFI to call any standard C library function directly.
4. **Multi-Pass Pipeline**: `Parser -> Resolver -> TypeChecker -> Codegen`. Name resolution and semantics are separated from LLVM IR generation.

---

## 2. Phase 1: Finish the Migration (Codegen Detox)
*Before adding features, the internal architecture must be clean. Epic 3 is complete. Epic 4 remains.*

- [x] **Issue #9: Strip Fallback Overload Loops**
  - **Implementation**: Remove the `if (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')` fallback loops in `Codegen.cpp`. Codegen must strictly use `Node.ResolvedCallTarget->mangledName()`.
- [x] **Issue #10: Strip Semantic Checks from Codegen**
  - **Implementation**: Remove all type guards (e.g., `isIntegerTy`, `isDoubleTy`) from intrinsic handlers like `abs`, `power`, and `print`. The `TypeChecker` pass handles this; Codegen must blindly trust `Node.ResolvedCallTarget->ReturnType`.
- [x] **Issue #11: Clean Pipeline**
  - **Implementation**: Turn off "shadow mode" in `main.cpp`. Enforce the strict sequence: `Parser -> Resolver -> TypeChecker -> Codegen`. Remove legacy `GlobalSymbolTable` references from Codegen.

---

## 3. Phase 2: The Grand Syntactic Overhaul & Assert
*Moving to a modern, LL(1) friendly syntax to unblock testing and eliminate parser hacks. Must be done immediately after Phase 1. Pratt parsing (operator precedence for arithmetic/logic) is preserved completely — it operates on expressions, not declarations. See `PHASE_2_ISSUES.md` for full GitHub issue descriptions.*

**Final Syntax (locked in):**
```turflang
mutable x: int = 5;                           // mutable variable
mut y: double = 3.14;                          // alias for mutable
immutable PI: double = 3.14159;                // runtime-immutable
immut MAX: int = getMax();                     // alias for immutable
fn add(a: int, b: int): int { return a + b; } // function, return type always explicit
struct Point { x: int, y: int }                // struct declaration
mutable p: Point = Point { x: 10, y: 20 };    // named-field struct init
mutable n: int = 3.99 to int;                 // cast with 'to' keyword
mutable r: int = x > 0 give x otherwise -x;   // ternary: give...otherwise
```

- [ ] **#20: `assert(cond)` & `fail(msg)` 🔴** *(do first — needed before `raise` can be deleted)*
  - **Implementation**: Register `assert` and `fail` in `Builtins.cpp`. `assert` generates a conditional branch; on failure calls `fprintf(stderr)` and `abort()`. `fail` unconditionally aborts with a message. Accepts 1 or 2 args.
- [ ] **#12: Variables to `mutable`/`mut` and `immutable`/`immut` 🔴**
  - **Implementation**: Add `TOK_MUTABLE`, `TOK_IMMUTABLE`, `TOK_COLON`. Both `mutable`/`mut` map to `TOK_MUTABLE`; `immutable`/`immut` map to `TOK_IMMUTABLE`. Delete `GlobalSymbolTable->LookupType()` Lexer Hack from `ParsePrimaryBase()` (L596). Remove type-keyword-in-statement-position blocks. `VarDeclExprAST` gains `bool IsImmutable`. TypeChecker rejects reassignment of immutable variables. Update `SLMIntegration::findVariableDeclarationLine` to scan for new `mutable x:` patterns.
- [ ] **#13: Functions to `fn name(arg: type): returnType {}` 🔴**
  - **Implementation**: Update `ParsePrototype()` / `ParseFuncDef()` to parse `fn name(name: type): RetType { }`. Return type always explicit. Update all stdlib `.tr` files and test files.
- [ ] **#14: Struct Named-Field Syntax and `StructName { field: val }` Init 🔴**
  - **Implementation**: `ParseStructDecl()` parses fields as `name: type,`. Struct init changes from positional `Point(x, y)` to named-field `Point { x: 10, y: 20 }`. TypeChecker validates all fields provided and no unknown fields. Codegen reorders named fields to match struct layout before emitting IR.
- [ ] **#15: Cast Syntax `x to type` 🔴**
  - **Implementation**: Add `TOK_TO` keyword. Add `to` to `BinopPrecedence` at precedence 5 (lower than `||`). In `ParseBinOpRHS`, when `BinOp == TOK_TO`, parse the RHS with `ParseType()` and create a `CastExprAST`. Old `int(x)` cast syntax removed by #12 cleanup.
- [ ] **#16: Unify `if` and Add Ternary `cond give a otherwise b` 🔴**
  - **Implementation**: Delete `then` from Keywords map. `ParseIfExpr()` always requires `{`. Add `TOK_GIVE` and `TOK_OTHERWISE` keywords. In `ParseExpression()`, after `ParseBinOpRHS` returns the condition, if `CurTok == TOK_GIVE`: consume `give`, parse ThenExpr, expect `TOK_OTHERWISE`, parse ElseExpr, desugar to `IfExprAST`. `elseif` stays. No `?` symbol involved — zero ambiguity.
- [ ] **#19: Rip out `try/onError/raise` 🔴**
  - **Implementation**: First replace `RaiseExprAST` in `BinaryExprAST::codegen()` at L691 and L727 with `fail()` calls (depends on #20). Then delete `TOK_TRY`, `TOK_ONERROR`, `TOK_RAISE`, all three AST nodes, `ParseTryExpr()`, `ParseRaiseExpr()`, and their Codegen bodies.
- [ ] **#18: Two-Pass Resolver for Forward Declarations 🔴**
  - **Implementation**: Add hoisting Pass 1 to `Resolver::run()` that registers all struct names and function signatures before Pass 2 resolves bodies. Allows functions to call functions defined below them.
- [ ] **#17: Strict Semicolons — Delete `RequiresSemicolon()` 🔴** *(do last)*
  - **Implementation**: Delete `RequiresSemicolon()` (L981-991). Update `ParseBlock()` to explicitly enumerate block-producing constructs (`if`, `while`, `for`, `fn`, `struct` — no `;`) vs expression statements (require `;`).

---

## 4. Phase 3: The Language Backbone
*The structural primitives required to build real programs.*

- [ ] **FFI (`extern fn`) 🔴**
  - **Implementation**: Lexer `TOK_EXTERN`. AST `ExternFuncDeclExprAST`. Parser parses `extern fn Name(params): RetType`. Codegen uses `llvm::Function::Create` with `llvm::Function::ExternalLinkage` without generating a body block.
- [ ] **String Operations 🔴**
  - **Implementation**: 
    - `+`: `BinaryExprAST::codegen` intercepts string operands, calls intrinsic `turf_string_concat` (uses `malloc`, `strcpy`, `strcat`).
    - `==`: Intercepts and routes to `strcmp`.
    - `s[i]`: `ArrayAccessExprAST::codegen` generates `GEP` on `i8*` to load `i8`.
- [ ] **`do { } while(cond);` 🔴**
  - **Implementation**: Add `TOK_DO`, `DoWhileExprAST`. Codegen emits the body block, evaluates condition, then conditionally branches back to the body block.
- [ ] **`compare` Statement (Switch) 🔴**
  - **Syntax**:
    ```turflang
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
    ```
  - **Implementation**: Add `TOK_COMPARE`, `TOK_MATCHES`, `TOK_DONE`, `TOK_OTHER`. `ParseCompare()` parses the subject expression in `()`, then loops: each arm starts with `matches (value):` or `other:`. Each `matches` arm body ends with `done` (explicit break — no fall-through ever). `other` arm is optional, must be last, has no `done`. Codegen emits LLVM `switch` instruction with one `BasicBlock` per arm.
> **⛔ Explicitly Rejected**: `loop { }` was considered and rejected. It adds nothing over `while (true) { }` and pollutes the keyword namespace. `TOK_LOOP` must NOT be added at any point during implementation.

- [ ] **Logical NOT `!` 🔴**
  - **Implementation**: Currently Lexer only recognizes `!=`. Add unary `'!'` case in `Parser::ParseUnary()` generating a comparison against `true` (`!= true`).
- [ ] **Default Variable Initialization 🔴**
  - **Implementation**: In Parser's variable declaration, if no `=` is found, auto-inject an assignment of `0` (int), `0.0` (double), `false` (bool), `""` (string), or `null` (pointer).
- [ ] **Self-Referential Structs (LLVM Backend) 🔴**
  - **Implementation**: The fallback path in `getLLVMType()` (Codegen.cpp L123-126) already creates an opaque struct correctly. The bug is in `StructDeclExprAST::codegen()` (Codegen.cpp L2777), which does a single-shot `StructType::create(Context, FieldTypes, Name)` and will infinite-loop on a self-referential field. Fix: change it to a two-step: (1) `auto *ST = StructType::create(Context, Name)` — register the opaque type in `LLVMTypeCache` immediately. (2) Evaluate field LLVM types (self-referential fields now resolve to a pointer to the already-registered opaque type). (3) `ST->setBody(FieldTypes)`.
- [ ] **`null` Type Enforcement 🔴**
  - **Implementation**: In `TypeChecker::visitVarDecl`, emit an error if `null` is assigned to a non-pointer primitive (e.g., `mutable x: int = null;`).
- [ ] **Bitwise Operators 🔴**
  - **Implementation**: Add `TOK_LSHIFT` (`<<`), `TOK_RSHIFT` (`>>`). Wire `&`, `|`, `~`, `<<`, `>>` to LLVM's `CreateAnd`, `CreateOr`, `CreateNot`, `CreateShl`, `CreateLShr` in `BinaryExprAST::codegen()`. (Decision: repurpose `^` to XOR, use `**` or `math.pow` for exponentiation).
- [ ] **`enum` 🟡**
  - **Implementation**: Add `TOK_ENUM` and `EnumDeclExprAST`. Codegen maps each variant (e.g., `Color.Red`) to a static `int64` constant.
- [ ] **First-Class Functions 🟡**
  - **Implementation**: Add `FuncType(ReturnType, [ParamTypes])` to `Types.h`. In Codegen, function references generate `llvm::Function*` values, allowing them to be passed as arguments.

---

## 5. Phase 4: Standard Library & Modules
*Rewrite the entire standard library in Turflang using `extern fn` FFI. The current stdlib files (`io.tr`, `math.tr`, etc.) all use `@intrinsic` wrappers that call hardcoded C++ Codegen paths — this is the monolith pattern we are eliminating. After Phase 3 (FFI), every module is rewritten from scratch in pure Turflang. The compiler no longer needs to know what `strlen` or `fopen` is.*

- [ ] **Module Search Path & Circular Imports 🔴**
  - **Implementation**: Modify `import` logic to search `<compiler_bin_dir>/lib/std/` first, then `./`. Maintain a `std::set<std::string> ActiveImports` during parsing; error if an import is already in the set.
- [ ] **Rewrite `std/io.tr` using FFI 🔴**
  - **Implementation**: Replace all `@intrinsic` entries. Use `extern fn` to bind `printf`, `fopen`, `fread`, `fwrite`, `fclose`, `fgets`. Expose `print`, `printline`, `input`, `readFile`, `writeFile`, `fileExists` as normal Turflang functions.
- [ ] **Rewrite `std/math.tr` using FFI 🔴**
  - **Implementation**: Replace all `@intrinsic("llvm.sin.f64")` etc. entries. Use `extern fn` to bind `sin`, `cos`, `sqrt`, `pow`, `floor`, `ceil`, `round`, `fabs` from `libm`. Add native `min`, `max`, `clamp`. Expose constants `PI` and `E` as module-level `immutable` declarations.
- [ ] **Write `std/string.tr` using FFI 🔴**
  - **Implementation**: Use `extern fn` to bind `strlen`, `strcmp`, `strstr`, `strcpy`, `strcat`, `toupper`, `tolower`, `malloc`. Implement `length`, `equals`, `concat`, `contains`, `indexOf`, `trim`, `split`, `startsWith`, `endsWith`, `toUpper`, `toLower` as Turflang functions.
- [ ] **Write `std/os.tr` using FFI 🟡**
  - **Implementation**: Use `extern fn` to bind `getenv`, `exit`. Expose `env`, `exit`. Command-line `argc`/`argv` access requires a small compiler-level hook to pass them to `main`.
- [ ] **Write `std/mem.tr` using FFI 🟡**
  - **Implementation**: Use `extern fn` to bind `malloc`, `realloc`, `free`. Expose them as `mem.alloc`, `mem.realloc`, `mem.free` — consistent with the existing `mem.tr` but now user-visible and FFI-backed.
- [ ] **Delete `@intrinsic` and the Builtins.cpp monolith 🔴**
  - **Implementation**: Once all stdlib modules are rewritten using `extern fn`, the entire `@intrinsic("name")` annotation mechanism becomes dead weight. Delete `TOK_INTRINSIC` from the Lexer, delete the `@intrinsic` parsing logic from the Parser, and delete `CodegenIntrinsic()` from `Builtins.cpp`. The hardcoded arity table (`IntrinsicArity`) and the `EarlyArityMap` in `Codegen.cpp` are also deleted at this point — the TypeChecker (Issue #8) is now the single source of truth for arity and type validation for all function calls.

---

## 6. Phase 5: Prompt-Oriented Programming (SLM Integration)
*The Two-Adapter Architecture (Base: Qwen2.5-Coder-1.5B ~1GB RAM).*

**Architecture Note**: The SLM (`SLMEngine.cpp`) calls `llama.h` directly as a C++ library compiled into the Turflang binary. It does NOT go through Turflang's FFI system — it has direct C++ access to the AST, Resolver symbol tables, and TypeChecker output. This is by design: compiler internals communicate natively, not through the language runtime.

- [ ] **`@slm` Syntax & Parser 🔴**
  - **Implementation**: Lexer parses `@slm { "prompt" }` into `TOK_SLM_BLOCK`. Parser generates `SLMBlockExprAST`.
- [ ] **Structured Context Serialization 🔴**
  - **Implementation**: `serializeSymbolContext()` extracts the current function signature and all visible variables/types from the `ResolverSymbolTable` to build the prompt prefix.
- [ ] **The SLM Query Loop 🔴**
  - **Implementation**: `SLMBlockExprAST::codegen()` invokes `SLMEngine`. The returned string is passed back through the pipeline (`Lexer -> Parser -> Resolver -> TypeChecker`). If TypeChecker fails, retry up to 3 times, feeding the compiler error back into the prompt.
- [ ] **Dynamic Error Context for `--smart` 🟡**
  - **Implementation**: Modify `SLMIntegration::fetchSuggestion` to size the context window dynamically: ±3 lines for syntax errors, ±8 lines for semantic errors (matching `needs_slm_training_dynamiccontext/` tests).
- [ ] **LoRA Fine-Tuning 🟡**
  - **Implementation**: Convert `needs_slm_training_dynamiccontext/` to JSONL for Adapter A (Error Healing). Convert `lib/std/` functions to JSONL for Adapter B (POP Codegen).

---

## 7. Phase 6: Developer Tooling & Ecosystem
*Tooling drives adoption.*

- [ ] **LSP Server (`turf --lsp`) 🔴**
  - **Implementation**: Run the compiler as a persistent background process communicating via JSON-RPC over stdin/stdout. Serve autocomplete and hover types by querying the `ResolverSymbolTable` and `TypeChecker` AST annotations.
- [ ] **`turf test` 🟡**
  - **Implementation**: Add `test "name" { ... }` block to Parser. Running `turf test` automatically generates a hidden `main()` that invokes all test blocks and reports passes/fails.
- [ ] **`turf fmt` 🟡**
  - **Implementation**: Create a `FormatVisitor : public ASTVisitor` that reconstructs standard-styled source code from the AST.
- [ ] **`tpm` (Turf Package Manager) 🟡**
  - **Implementation**: `turf get <github-url>`. Clones repository into `.tpm/packages/` and automatically includes it in the compiler's module search path.

---

## 8. Phase 7: Self-Hosting & Bootstrapping
*The final milestone: writing Turflang in Turflang.*

When adding features (like a new keyword) to a self-hosted compiler, the **bootstrap cycle** is used:
1. Write the logic for the new keyword in the compiler's source code using *existing* features (functions, `if` statements, FFI to LLVM).
2. Compile this source code using the *current* compiler binary.
3. The output is a new compiler binary that understands the new keyword.
4. You can now use the new keyword in everyday Turflang programs.

**Requirements for Self-Hosting:**
- Phase 3 must be complete (specifically **FFI**, to bind LLVM's C API).
- Phase 4 must be complete (specifically `std/io.tr`, to read/write source files and object files).

**SLM in the Self-Hosted Compiler:**
The `@slm { }` syntax is for *user programs*. The compiler's *own* SLM calls (error healing, prompt building, sandbox validation) are C++ today, but in the self-hosted compiler they become ordinary `extern fn` FFI calls binding `llama.cpp`'s C API directly:
```turflang
extern fn bool llama_backend_init(): bool;
extern fn void* llama_model_load_from_file(path: string, params: void*): void*;
extern fn int llama_decode(ctx: void*, batch: void*): int;
// ... etc
```
No special SLM keyword is needed inside the compiler itself. Adding a new `--flag` or a new SLM feature is just writing a new Turflang function that calls these `extern fn` bindings.
