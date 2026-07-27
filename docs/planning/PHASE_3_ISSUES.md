# Phase 3 — GitHub Issues: The Language Backbone

> All issues in this document depend on **Phase 2 (Issues #12–#20)** being fully merged first.
> Issues #173 and #174 are Phase 2 tail items — diagnostic infrastructure that Phase 3 depends on.
> They must be completed before Phase 3 proper begins.
> The ordering within Phase 3 is strict — see **Execution Order** at the bottom.

---

## Final Turflang Phase 3 Syntax Reference (locked in)

> **2026-07 status note**: most of this block IS the current language (FFI, `compare`,
> enum, logical NOT, bitwise ops, string ops, default init) — but **do/while,
> first-class functions, and error unions (`!T`/`catch`/`?`/`error.XYZ`/`match`)
> are still NOT implemented** (see the per-issue status list below). Also,
> module-qualified calls and enum-value access now use `::`, not `.`
> (`io::printline(...)`, `Color::Red`) — this example block predates that
> change and was left as-is below rather than silently rewritten, since the
> do-while/first-class-function/error-union examples are aspirational anyway.

```turflang
// FFI — bind a C library function
extern fn strlen(s: string): int;
extern fn malloc(size: int): ptr;   // ptr = opaque raw pointer (i8*), Phase 3 only
extern fn free(p: ptr): void;

// do/while — NOT YET IMPLEMENTED (#180)
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

// First-class functions — NOT YET IMPLEMENTED (#187)
fn apply(f: fn(int): int, x: int): int {
    return f(x);
}

fn double_it(n: int): int { return n * 2; }

mutable result: int = apply(double_it, 5); // 10

// Error unions — NOT YET IMPLEMENTED (#188, see ERROR_HANDLING_RFC.md)
fn divide(a: int, b: int): !int {
    if b == 0 {
        return error.DivisionByZero("cannot divide by zero");
    }
    return a / b;
}

// Handling — catch (fallback)
mutable x: int = divide(10, 0) catch 0;

// Handling — ? (propagation, caller must also return !T)
fn doMath(): !int {
    mutable x: int = divide(10, 2)?;
    return x * 10;
}

// Handling — match (pattern branch)
match divide(10, 0) {
    Ok(val) => { io.printline(val); }
    Err(e)  => { io.printline(e.message); }
}
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

Note: enum-value and module-qualified access now use `::` (`Color::Red`, `io::printline(...)`), not `.` as shown in some of the original issue text — see `docs/architecture/modules_and_scope_resolution.md`.

---

## Issue #180 — `do { } while(cond)` Statement

**Labels:** `parser`, `lexer`, `ast`, `codegen`, `phase-3`
**Depends on:** #179
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`
**Status (2026-07): NOT implemented.** No `TOK_DO`/`DoWhileExprAST` anywhere in the codebase.

### Description

Add `do { body } while (cond);` — a loop that always executes its body at least once.

```turflang
do {
    io.printline("at least once");
    x = x - 1;
} while (x > 0);
```

### Lexer Changes

Add to `Keywords` map:
```cpp
{"do", TOK_DO},
```

Add to `Lexer.h`:
```cpp
TOK_DO = -72,
```

### AST Changes (`AST.h`)

Add `DoWhileExprAST`:

```cpp
class DoWhileExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Body_;
  std::unique_ptr<ExprAST> Cond_;
public:
  DoWhileExprAST(SourceLocation Loc,
                 std::unique_ptr<ExprAST> Body,
                 std::unique_ptr<ExprAST> Cond);
  ExprAST *getBody() const { return Body_.get(); }
  ExprAST *getCond() const { return Cond_.get(); }
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitDoWhile(*this); }
};
```

Add `visitDoWhile(DoWhileExprAST &)` to `ASTVisitor` interface (with a default no-op).

### Parser Changes

In `ParseBlock()` (and top-level), add a case for `TOK_DO`:

```
Consume TOK_DO
Expect '{'
Parse body = ParseBlock()
Expect TOK_WHILE
Expect '('
Parse Cond = ParseExpression()
Expect ')'
Expect ';'
Return DoWhileExprAST(Loc, Body, Cond)
```

### Codegen (`DoWhileExprAST::codegen()`)

```
LoopBodyBB: (jump here unconditionally from entry)
  [body IR]
  → evaluate Cond
  → CondBr: if true → LoopBodyBB, if false → AfterBB
AfterBB:
  (continue)
```

Key difference from `while`: the condition is checked **after** the body, guaranteeing at least one execution.

### Resolver/TypeChecker

Standard: enter scope for body, resolve body, resolve condition. TypeChecker: condition must be `bool`.

### Acceptance Criteria

- `do { x = x - 1; } while (x > 0);` compiles and produces correct IR.
- Body executes at least once even when the condition is `false` on entry.
- `break` and `continue` inside a `do/while` behave correctly.
- Missing `while` after `}` emits `Expected 'while' after do-block`.

---

## Issue #185 — CFG Phase 1: Cyclomatic Complexity & Infinite Loop Detection

**Labels:** `cfg`, `static-analysis`, `phase-3`
**Depends on:** #173 (`TurfWarning` must exist), #184
**Files:** `include/CFG.h`, `src/CFG.cpp`
**Status (2026-07): NOT implemented.** Current `CFG`/`CFGBuilder` (`include/CFG.h`) only builds basic blocks and does reachability / missing-return / dead-code-after-terminator analysis — no `calculateCyclomaticComplexity()`, no `hasUnbreakableLoop()`.

### Description

Two static analysis passes built on the existing CFG infrastructure. Both emit `TurfWarning` (non-interrupting) — the program still compiles if a warning is triggered.

### Pass 1: Cyclomatic Complexity Monitor

**Goal:** Warn when a function is too complex to read or maintain.

**Formula:** `M = E - N + 2P` where:
- `E` = total edges (count all `Successors` across all `Blocks`)
- `N` = total nodes (count of `Blocks`)
- `P` = 1 (single connected component per function CFG)

```cpp
// In CFG.h:
int CFG::calculateCyclomaticComplexity() const;

// Implementation:
int CFG::calculateCyclomaticComplexity() const {
    int E = 0, N = static_cast<int>(Blocks.size());
    for (const auto &Block : Blocks)
        E += static_cast<int>(Block.Successors.size());
    return E - N + 2;
}
```

**Trigger:** After building the CFG in `Codegen.cpp`, call this method. If `M > 15`:

```cpp
TurfWarning(FuncLoc,
    "Function '" + FuncName + "' has cyclomatic complexity " +
    std::to_string(M) + " (threshold: 15). "
    "Consider breaking it into smaller functions.").emit();
```

### Pass 2: Infinite Loop Detection

**Goal:** Detect `while(true)` loops with no reachable exit.

```cpp
// In CFG.h:
bool CFG::hasUnbreakableLoop() const;
```

**Algorithm:**
1. Run a DFS from `EntryBlock`, tracking visited blocks and the current DFS path (for cycle detection).
2. When a back-edge is found (a `Successor` already in the current path), a cycle exists.
3. Collect all blocks within the cycle.
4. If **none** of the cycle's blocks contain a `TerminatorKind::Break` or `TerminatorKind::Return` → infinite loop.

**Trigger:** After CFG construction:

```cpp
if (CFG.hasUnbreakableLoop()) {
    TurfWarning(FuncLoc,
        "Function '" + FuncName + "' contains a loop with no exit path. "
        "This will run forever.").emit();
}
```

### Acceptance Criteria

- A function with 16 branches triggers a cyclomatic complexity warning.
- A function with `while(true) { x = x + 1; }` triggers an infinite loop warning.
- A function with `while(true) { if (x > 10) { break; } }` does NOT trigger the warning (break is present).
- Both warnings use `TurfWarning` and do not prevent compilation.
- `--resolve` flag (resolver-only mode) does NOT trigger CFG warnings (CFG runs after Codegen begins).

---

## Issue #186 — CFG Phase 2: Liveness Pass & Infinite Recursion Detection

**Labels:** `cfg`, `static-analysis`, `phase-3`
**Depends on:** #185
**Files:** `include/CFG.h`, `src/CFG.cpp`, `src/CallGraph.cpp` (existing)
**Status (2026-07): NOT implemented.** `CallGraphPass` (`include/CallGraph.h`) exists but is a minimal ~26-line call-edge tracker — no liveness data-flow analysis, no `RecursionWithoutBaseError`.

### Description

Two more static analysis passes. The liveness pass is the first real data-flow analysis in the compiler. Infinite recursion detection leverages the existing `CallGraph` infrastructure already built in `main.cpp`.

### Pass 3: Liveness Analysis (Dead Stores & Unused Variables)

**Goal:** Warn about variables that are assigned but never read.

**Algorithm:** Classic backward data-flow analysis (worklist algorithm).

```cpp
class LivenessAnalysisPass {
  // Per-block sets:
  std::unordered_map<TurfBasicBlock*, std::unordered_set<std::string>> Gen_;   // read before written
  std::unordered_map<TurfBasicBlock*, std::unordered_set<std::string>> Kill_;  // written
  std::unordered_map<TurfBasicBlock*, std::unordered_set<std::string>> LiveIn_;
  std::unordered_map<TurfBasicBlock*, std::unordered_set<std::string>> LiveOut_;

public:
  void run(const CFG &G);
};
```

**Worklist algorithm:**
1. Initialize `Gen` and `Kill` for each block by walking its statements.
2. Initialize `LiveIn[B] = Gen[B]` for all `B`.
3. Repeat until convergence:
   ```
   For each block B (reverse postorder):
     LiveOut[B] = ∪ LiveIn[S] for each successor S of B
     LiveIn[B]  = Gen[B] ∪ (LiveOut[B] − Kill[B])
   ```

**Triggers:**
- **Dead store**: variable in `Kill[stmt]` but not in `LiveOut[B]` and not read later in the block.
  - Emit: `TurfWarning(loc, "Variable '" + name + "' is assigned but never read (dead store).")`
- **Unused variable**: a `VarDeclExprAST` whose name never appears in any `Gen` set across all blocks.
  - Emit: `TurfWarning(loc, "Variable '" + name + "' is declared but never used.")`

### Pass 4: Infinite Recursion Detection

**Goal:** Detect functions where every path from entry to exit passes through a recursive call — meaning there is no base case.

**Algorithm** (uses the existing `CallGraph`):
1. During CFG construction, tag basic blocks that contain a call to the **current** function (self-call).
2. Compute all paths from `EntryBlock` to `ExitBlock` using DFS (with visited set to prevent infinite traversal of loops).
3. If **every** path passes through a self-call block: no base case exists.
4. Emit fatal error: `RecursionWithoutBaseError`.

Add `RecursionWithoutBaseError` to `Errors.h`:

```cpp
class RecursionWithoutBaseError : public TurfError {
public:
  RecursionWithoutBaseError(SourceLocation Loc, const std::string &FuncName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Infinite recursion detected in '" +
                      Colors::CYAN + FuncName + Colors::BRIGHT_RED + "'." +
                      Colors::RESET,
                  "Hint: Every recursive function needs a base case — "
                  "a path that returns without calling itself.") {}
};
```

This is a **fatal error** (uses `raise()`) because infinite recursion guarantees a stack overflow at runtime with no recovery.

### Acceptance Criteria

**Liveness:**
- `mutable x: int = 5; mutable y: int = x + 1;` then `return y;` (x is read → no warning, y is read → no warning).
- `mutable x: int = 5; return 0;` (x declared, never read) → unused variable warning.
- `mutable x: int = 5; x = 10; return x;` (first assignment is dead store since x is overwritten before read) → dead store warning for the first assignment.

**Infinite Recursion:**
- `fn foo(): int { return foo(); }` → `RecursionWithoutBaseError`.
- `fn factorial(n: int): int { if n == 0 { return 1; } return n * factorial(n - 1); }` → NO error (base case exists).

---

## Issue #187 — First-Class Functions

**Labels:** `typechecker`, `ast`, `codegen`, `phase-3`
**Depends on:** #186
**Files:** `include/Types.h`, `src/Parser.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`
**Status (2026-07): NOT implemented.** `Resolver::visitScopeAccess()` explicitly rejects a bare function reference with "functions aren't first-class yet"; no `FuncType`.

### Description

Allow functions to be passed as arguments, stored in variables, and returned from other functions. This is required before generics can be useful (generic algorithms like `map`, `filter`, `sort` all take functions as parameters).

```turflang
fn apply(f: fn(int): int, x: int): int {
    return f(x);
}

fn double_it(n: int): int { return n * 2; }

mutable result: int = apply(double_it, 5);  // 10
```

### Types.h Changes

Add `FuncType`:

```cpp
class FuncType : public TurfType {
  TurfTypePtr ReturnType_;
  std::vector<TurfTypePtr> ParamTypes_;
public:
  FuncType(TurfTypePtr ReturnType, std::vector<TurfTypePtr> ParamTypes);
  std::string getName() const override; // e.g. "fn(int, int): double"
  bool isFunc() const override { return true; }
  TurfTypePtr getReturnType() const { return ReturnType_; }
  const std::vector<TurfTypePtr> &getParamTypes() const { return ParamTypes_; }
};
```

### Parser Changes (`ParseType()`)

When `ParseType()` encounters `TOK_FN`:

```
Consume TOK_FN
Expect '('
Parse comma-separated type list
Expect ')'
Expect TOK_COLON
Parse return type
Return FuncType(ReturnType, ParamTypes)
```

This means `fn(int): int` is a valid type expression in any type-annotation position.

### TypeChecker Changes

`TypeChecker::visitFuncCall`: when the callee resolves to a variable of `FuncType`, validate argument count and types against `FuncType::getParamTypes()`.

`TypeChecker::visitVarDecl`: when the declared type is `FuncType`, the initializer must be a function name that matches the signature.

### Codegen Changes

`VariableExprAST::codegen()`: when the resolved symbol has type `FuncType`, look up the `llvm::Function*` by name and return it as a value (functions are already first-class in LLVM IR).

`FuncCallExprAST::codegen()`: when the callee is a variable (not a direct function name), load the `llvm::Function*` value and emit `CreateCall` with it.

### Acceptance Criteria

- `fn apply(f: fn(int): int, x: int): int { return f(x); }` compiles.
- `apply(double_it, 5)` produces `10`.
- Passing a function with the wrong signature emits a TypeChecker error.
- `mutable f: fn(int): int = double_it;` compiles and calling `f(3)` produces `6`.

---

## Issue #188 — `!T` Error Unions (`catch`, `?`, `error.XYZ`)

**Labels:** `parser`, `lexer`, `ast`, `typechecker`, `codegen`, `resolver`, `phase-3`
**Depends on:** #187 (all other Phase 3 items must be complete first)
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `include/Types.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`, `include/Errors.h`, `lib/std/*.tr`
**Status (2026-07): NOT implemented.** No `!T`/`catch`/`?`/`error.XYZ`/`match` tokens anywhere. See `docs/planning/ERROR_HANDLING_RFC.md` — this remains an open design proposal, not yet started.

### Description

Turflang's recoverable error system. Functions that can fail return `!T` (an error union) instead of throwing an exception. The TypeChecker enforces that `!T` is always explicitly handled before use.

**Design decisions (locked in):**
- **Single global error enum**: all error variants across the entire program share one compile-time registry. Variants are registered by the Resolver's Pass 1 by scanning `return error.XYZ(...)` statements.
- **Runtime message string**: every error variant carries an `i8*` message string at runtime.
- **LLVM representation**: `{ i1 is_ok, i64 value_or_error_id, i8* message }` — one struct per `!T` instantiation (only `!int`, `!bool`, etc. in Phase 3; `!struct` deferred).
- `!T` is **not erased** — it is a concrete LLVM struct. Type erasure applies only to generics.

### Lexer Changes

Add to `Keywords` map:
```cpp
{"catch",     TOK_CATCH},
{"match",     TOK_MATCH},
{"error",     TOK_ERROR},
```

Add to `Lexer.h`:
```cpp
TOK_CATCH = -80,
TOK_MATCH = -81,
TOK_ERROR = -82,
```

`?` as a postfix operator: recognize in the character loop. Return `TOK_QUESTION` (`= -83`).

### Types.h Changes

Add `ErrorUnionType`:

```cpp
class ErrorUnionType : public TurfType {
  TurfTypePtr ValueType_;  // The T in !T
public:
  ErrorUnionType(TurfTypePtr ValueType);
  TurfTypePtr getValueType() const { return ValueType_; }
  std::string getName() const override; // e.g. "!int"
  bool isErrorUnion() const override { return true; }
};
```

### AST Changes

Add:
- `ErrorUnionReturnExprAST` — for `return error.DivisionByZero("msg")`
- `CatchExprAST` — for `expr catch fallback`
- `PropagateExprAST` — for `expr?`
- `MatchExprAST` — for `match expr { Ok(v) => ... Err(e) => ... }`

### Parser Changes

**`ParseType()`**: when `CurTok == '!'`, consume it and parse the inner type:

```cpp
if (CurTok == '!') {
    getNextToken();
    auto InnerType = ParseType();
    return std::make_shared<ErrorUnionType>(std::move(InnerType));
}
```

**`ParseExpression()`** (postfix layer): after parsing an expression, if `CurTok == TOK_CATCH`:

```
consume 'catch'
FallbackExpr = ParseExpression()
return CatchExprAST(MainExpr, FallbackExpr)
```

If `CurTok == TOK_QUESTION`:

```
consume '?'
return PropagateExprAST(MainExpr)
```

**`ParsePrimaryBase()`**: when `CurTok == TOK_ERROR`:

```
consume 'error'
expect '.'
Variant = expect TOK_IDENTIFIER
expect '('
Message = ParseExpression()   // must be string type
expect ')'
return ErrorUnionReturnExprAST(Variant, Message)
```

**`ParseBlock()`**: when `CurTok == TOK_MATCH`:

```
consume 'match'
Subject = ParseExpression()
expect '{'
OkArm:  'Ok' '(' BindName ')' '=>' '{' body '}'
ErrArm: 'Err' '(' BindName ')' '=>' '{' body '}'
expect '}'
return MatchExprAST(Subject, OkBind, OkBody, ErrBind, ErrBody)
```

### Resolver Changes (Pass 1)

Extend `hoistTopLevel()` to scan function bodies for `ErrorUnionReturnExprAST` nodes and register each `error.XYZ` variant name in a `GlobalErrorRegistry` (similar to `RegisteredStructs_`). The registry maps variant name → integer ID.

```
GlobalErrorRegistry:
  DivisionByZero  → 1
  FileNotFound    → 2
  OutOfMemory     → 3
```

### TypeChecker Changes

- `visitFuncDef`: if return type is `ErrorUnionType`, mark the function as error-returning.
- `visitErrorUnionReturn`: check that `error.XYZ` is a registered variant. Check message is `string`.
- `visitVarDecl`: if the initializer has type `!T` and the declared type is `T` (not `!T`), emit `UnwrappedErrorUnionError` — the user must use `catch` or `?`.
- `visitCatch`: narrow the type from `!T` to `T` (the fallback value is type-checked to be `T`).
- `visitPropagate`: the enclosing function must also return `!T` (or `!U` for any `U`). If not, emit `PropagateOutsideErrorUnionError`.
- `visitMatch`: bind `Ok(v)` as type `T`, bind `Err(e)` as type `{ error_id: int, message: string }`.

### Codegen

**LLVM struct type** (created once per `!T` instantiation):

```llvm
%ErrorUnion_int = type { i1, i64, i8* }
;                       is_ok, value/id, message
```

**`return error.XYZ("msg")`** lowers to:

```llvm
; is_ok = 0, error_id = N (from GlobalErrorRegistry), message = "msg"
%eu = alloca %ErrorUnion_int
%ok = getelementptr %ErrorUnion_int, %eu, 0, 0
store i1 0, %ok
%id = getelementptr %ErrorUnion_int, %eu, 0, 1
store i64 N, %id
%msg = getelementptr %ErrorUnion_int, %eu, 0, 2
store i8* @.str.msg, %msg
%ret = load %ErrorUnion_int, %eu
ret %ErrorUnion_int %ret
```

**`catch` expression** lowers to a conditional branch on `is_ok`:

```llvm
%is_ok = extractvalue %ErrorUnion_int %result, 0
br i1 %is_ok, label %ok_bb, label %err_bb
ok_bb:
  %val = extractvalue %ErrorUnion_int %result, 1
  ; use val
err_bb:
  ; use fallback
```

**`?` operator** lowers to an early return of the error union if `is_ok == 0`.

### Standard Library Updates

Update `lib/std/io.tr` function return types that can fail to return `!void` or `!string`. At minimum, file I/O functions must use `!T` — print functions can remain `void`.

### Acceptance Criteria

- `fn divide(a: int, b: int): !int { ... }` compiles.
- `mutable x: int = divide(10, 0);` (no catch/?) emits `UnwrappedErrorUnionError`.
- `mutable x: int = divide(10, 0) catch 0;` compiles and produces `0` at runtime.
- `mutable x: int = divide(10, 2) catch 0;` produces `5`.
- `divide(10, 2)?` inside a `!int` function propagates correctly.
- `match divide(10, 0) { Ok(v) => { ... } Err(e) => { io.printline(e.message); } }` prints `"cannot divide by zero"`.
- Unknown `error.XYZ` variant (not seen in any function) emits a Resolver error.

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
#180 (do/while)                   — OPEN — Group B — depends on #179 (done)
#181 (compare/matches)            — DONE
#182 (bitwise operators)          — DONE
#183 (string operations)          — DONE
#184 (enum)                       — DONE

#185 (CFG Phase 1)                — OPEN — Group C — depends on #173 (done), #184 (done)
#186 (CFG Phase 2)                — OPEN — Group C — depends on #185 (open)

#187 (first-class functions)      — OPEN — Group D — depends on #186 (open)
#188 (!T error unions)            — OPEN — Group E — LAST. depends on ALL above.
```

**Remaining open work, in original dependency order:** #180 → #185 → #186 → #187 → #188 (the doc's original ordering no longer strictly holds since #180/#181 aren't sequential dependencies of each other in practice — #181 shipped despite #180 not being done — but nothing here blocks starting #180 or #185 independently).

**Why `!T` is last:** It touches the most files and introduces the most new concepts. Every other Phase 3 item produces a clean, stable compiler state that `!T` can be built on top of.
