# Phase 3 — GitHub Issues: The Language Backbone

> All issues in this document depend on **Phase 2 (Issues #12–#20)** being fully merged first.
> Issues #171 and #172 are Phase 2 tail items — diagnostic infrastructure that Phase 3 depends on.
> They must be completed before Phase 3 proper begins.
> The ordering within Phase 3 is strict — see **Execution Order** at the bottom.

---

## Final Turflang Phase 3 Syntax Reference (locked in)

```turflang
// FFI — bind a C library function
extern fn strlen(s: string): int;
extern fn malloc(size: int): ptr;   // ptr = opaque raw pointer (i8*), Phase 3 only
extern fn free(p: ptr): void;

// do/while
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

// First-class functions
fn apply(f: fn(int): int, x: int): int {
    return f(x);
}

fn double_it(n: int): int { return n * 2; }

mutable result: int = apply(double_it, 5); // 10

// Error unions
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

## Issue #171 — `TurfWarning`: Non-Interrupting Diagnostic Class

**Labels:** `diagnostics`, `infrastructure`, `phase-2-tail`
**Depends on:** #17 (Phase 2 complete)
**Files:** `include/Errors.h`, `src/DiagnosticEngine.cpp`

### Description

Every CFG analysis pass (Issues #183, #184) emits **warnings**, not errors. Currently, every diagnostic goes through `TurfError::raise()` which calls `longjmp` and halts the compilation pass. A warning must never do this — it must register the message and return normally so analysis continues.

Add a `TurfWarning` class alongside `TurfError` in `Errors.h`.

### Implementation

```cpp
// In include/Errors.h:
class TurfWarning {
  SourceLocation Loc;
  std::string Message;

public:
  TurfWarning(SourceLocation Loc, const std::string &Msg)
      : Loc(Loc), Message(Msg) {}

  // NEVER interrupts compilation flow — always returns normally.
  void emit() const {
    DiagnosticEngine::add(Loc, Message, /*IsWarning=*/true);
  }
};
```

`DiagnosticEngine::add` already accepts `IsWarning` — verify it renders warnings with a yellow prefix (`Warning:`) distinct from errors (`Error:`). If not, update the rendering path.

### Acceptance Criteria

- `TurfWarning(loc, "msg").emit()` adds a yellow `Warning:` entry to the diagnostic engine and returns without interrupting the compilation pass.
- A source file that triggers only warnings still compiles to a binary successfully.
- Warnings appear after the build output, sorted by line number (same as errors).
- No existing `TurfError` subclass is affected.

---

## Issue #172 — Decouple Hints from `TurfError`

**Labels:** `diagnostics`, `infrastructure`, `dx`, `phase-2-tail`
**Depends on:** #171
**Files:** `include/Errors.h`, `src/SLMIntegration.cpp`

### Description

All `TurfError` subclasses currently concatenate the hint string directly into the `Message` field using `Colors::BRIGHT_GREEN + "Hint: ..."`. This makes it impossible to:

1. Render hints differently than the error body (e.g., collapsible in an IDE).
2. Pass hints separately to the SLM as distinct structured context.
3. Programmatically suppress or format hints in automated tests.

### Implementation

**Step 1: Update `TurfError` base class**

```cpp
// Before:
TurfError(SourceLocation Loc, const std::string &Msg) : Loc(Loc), Message(Msg) {}

// After:
TurfError(SourceLocation Loc, const std::string &Msg,
          const std::string &Hint = "")
    : Loc(Loc), Message(Msg), Hint_(Hint) {}

protected:
  std::string Hint_;
```

**Step 2: Update `DiagnosticEngine::add`**

Accept an optional `Hint` parameter. Render it on a separate line, color-coded distinctly:

```
── error at line 7, col 3

  Type mismatch!
  Cannot assign 'string' to 'int'.

  Hint: Check that you're not mixing incompatible types.

  7 │     mutable x: int = "hello";
    │                      ^ Here!
```

**Step 3: Migrate all existing error classes**

For every class in `Errors.h`, extract the `"Hint: ..."` suffix out of the `Message` string concatenation and pass it as the new `Hint` parameter. This is mechanical — search for `Colors::BRIGHT_GREEN + "Hint:` to find all instances.

**Step 4: Update `SLMIntegration.cpp`**

`fetchSuggestion()` builds the model's context from the error. After this issue, include the `Hint_` string as a separate context field in the prompt JSON, giving the model better structured input.

### Acceptance Criteria

- All error classes compile. No `Colors::BRIGHT_GREEN + "Hint:` concatenations remain inside `Message` strings.
- Error output renders the hint on a visually distinct line.
- SLM context includes the hint as a separate JSON field.
- All existing tests still pass (output format changes are cosmetic).

---

## Issue #173 — `extern fn` FFI

**Labels:** `lexer`, `parser`, `ast`, `codegen`, `phase-3`
**Depends on:** #171, #172 (Phase 2 tail complete)
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`

### Description

Add the ability to declare external C functions that the compiler can call via LLVM `ExternalLinkage`. This is the foundation for rewriting the standard library in Phase 4 and is required before self-referential structs (which need `malloc` in test code).

```turflang
extern fn strlen(s: string): int;
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;
```

> **Note on pointer types in Phase 3:** `unsafe_ptr<T>` does not exist yet — it is introduced in Phase 4 alongside `unsafe { }` block enforcement. In Phase 3, all FFI pointer types use the existing `T*` raw pointer syntax (e.g., `Node*`, `int*`). For C functions that return opaque/untyped pointers (e.g., `malloc`, `dlopen`), Phase 3 introduces a minimal `ptr` keyword — a plain opaque pointer, identical to `i8*` in LLVM IR. Users cast it to the target type with `to`. Phase 4 renames `T*` → `unsafe_ptr<T>` and adds enforcement.

### Lexer Changes

Add to `Keywords` map:
```cpp
{"extern", TOK_EXTERN},
```

Add to `Lexer.h`:
```cpp
TOK_EXTERN = -71,
```

### AST Changes (`AST.h`)

Add `ExternFuncDeclExprAST`:

```cpp
class ExternFuncDeclExprAST : public ExprAST {
  std::string Name_;
  std::vector<std::pair<std::string, TurfTypePtr>> Params_;
  TurfTypePtr ReturnType_;
public:
  ExternFuncDeclExprAST(SourceLocation Loc, std::string Name,
                        std::vector<std::pair<std::string, TurfTypePtr>> Params,
                        TurfTypePtr ReturnType);
  const std::string &getName() const { return Name_; }
  const auto &getParams() const { return Params_; }
  TurfTypePtr getReturnType() const { return ReturnType_; }
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitExternFuncDecl(*this); }
};
```

### Parser Changes

In `ParsePrimaryBase()` / top-level dispatch: when `CurTok == TOK_EXTERN`, call `ParseExternDecl()`:

```
Consume TOK_EXTERN
Expect TOK_FN
Expect TOK_IDENTIFIER → save as Name
Expect '('
Parse parameter list: each param is: Identifier TOK_COLON ParseType()
  (parameter names are required — they serve as documentation)
Expect ')'
Expect TOK_COLON
Call ParseType() → save as ReturnType
Expect ';'
Return ExternFuncDeclExprAST(Name, Params, ReturnType)
```

### Resolver Changes

In `hoistTopLevel()`: add a case for `ExternFuncDeclExprAST`. Register the extern function in `SymTable_` under the current-file function registry with an `IsExtern = true` flag. This makes the function visible to forward-calling code.

### TypeChecker Changes

`visitExternFuncDecl`: validate return type and parameter types are known (not `NamedType` references to undeclared structs). No body to check.

### Codegen Changes

`ExternFuncDeclExprAST::codegen()`:

```cpp
// Build LLVM function type from params + return type
FunctionType *FT = FunctionType::get(RetLLVMType, ParamLLVMTypes, /*isVarArg=*/false);
// Create with ExternalLinkage — no body block created
Function *F = Function::Create(FT, Function::ExternalLinkage, Name_, TheModule.get());
// Name the parameters for IR readability
for (auto &Arg : F->args()) { Arg.setName(Params[idx].first); }
return F;
```

### Acceptance Criteria

- `extern fn strlen(s: string): int;` compiles without error.
- `extern fn malloc(size: int): ptr;` compiles without error. `ptr` maps to `i8*` in LLVM IR.
- Calling `strlen("hello")` in a function body generates a call to the C `strlen` in the LLVM IR.
- `--emit-llvm` shows `declare i64 @strlen(i8*)` and `declare i8* @malloc(i64)`.
- `mutable p: Node* = malloc(8) to Node*;` compiles — cast from `ptr` to `Node*` using existing `to` syntax.
- Missing `;` after the extern declaration emits `Expected ';' after extern function declaration`.
- Attempting to define a body (`extern fn foo(): int { return 1; }`) emits `extern function declarations cannot have a body`.

---

## Issue #174 — Self-Referential Structs (LLVM Backend)

**Labels:** `codegen`, `ast`, `phase-3`
**Depends on:** #173 (malloc needed in linked-list test code)
**Files:** `src/Codegen.cpp`

### Description

The Resolver (Issue #18) already accepts `Node*` in a `Node` struct field — the name-level validation passes. The bug is in `StructDeclExprAST::codegen()`, which does a **single-shot** `StructType::create(Context, FieldTypes, Name)`. When it tries to evaluate the `FieldTypes` for a `Node*` field, it calls `getLLVMType("Node")` — but `Node` is not yet in `LLVMTypeCache` (it's being created right now). This causes infinite recursion.

**Fix**: Two-step struct creation.

### Implementation (`Codegen.cpp`)

In `StructDeclExprAST::codegen()`, replace the single-shot call with:

```cpp
// Step 1: Create an opaque (body-less) struct type and register it immediately.
// Now getLLVMType("Node") will find it in the cache and return a pointer to it
// without recursing back into codegen.
llvm::StructType *ST = llvm::StructType::create(TheContext, Name_);
LLVMTypeCache[Name_] = ST;  // register BEFORE evaluating fields

// Step 2: Now evaluate all field LLVM types.
// Self-referential fields (Node*) find "Node" in the cache → opaque ptr → safe.
std::vector<llvm::Type *> FieldTypes;
for (const auto &Field : Fields_) {
    FieldTypes.push_back(getLLVMType(Field.Type.get()));
}

// Step 3: Set the body now that all field types are resolved.
ST->setBody(FieldTypes);
```

Also verify the `getLLVMType()` fallback path at `Codegen.cpp` (the opaque struct fallback that already exists) is consistent with this approach — it should be, since it already does `StructType::create` without a body.

### Test File

`test_linked_list.tr` (created in Issue #18) exercises this. After this fix, `./turf test_linked_list.tr --emit-llvm` should show a proper LLVM struct type definition for `Node` without segfault or infinite recursion.

### Acceptance Criteria

- `struct Node { val: int, next: Node* }` compiles to valid LLVM IR.
- `--emit-llvm` shows: `%Node = type { i64, ptr }` (or equivalent).
- Creating a `Node` instance in a function body compiles and links.
- No infinite recursion or segfault during codegen.

---

## Issue #175 — `null` Type Enforcement (TypeChecker)

**Labels:** `typechecker`, `phase-3`
**Depends on:** #174
**Files:** `src/TypeChecker.cpp`, `include/Errors.h`

### Description

Currently, `null` is a codegen-level literal that produces an LLVM null pointer. The TypeChecker doesn't validate what it's assigned to. This means `mutable x: int = null;` silently compiles to undefined behavior.

**Rule**: `null` is only a valid initializer (or assignment value) for pointer types. Assigning `null` to a primitive (`int`, `double`, `bool`, `string`) is a TypeChecker error.

### TypeChecker Changes

In `TypeChecker::visitVarDecl` and `TypeChecker::visitAssignment`:

```
If the initializer expression has type NullType:
    If the declared type is NOT a pointer type (TurfPointerType, i.e. `T*` or `ptr`):
        Emit NullAssignmentError
```

Add `NullAssignmentError` to `Errors.h`:

```cpp
class NullAssignmentError : public TurfError {
public:
  NullAssignmentError(SourceLocation Loc, const std::string &VarName,
                      const std::string &TypeName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Cannot assign 'null' to variable '" +
                      Colors::CYAN + VarName + Colors::BRIGHT_RED +
                      "' of type '" + Colors::CYAN + TypeName +
                      Colors::BRIGHT_RED + "'." + Colors::RESET,
                  "Hint: 'null' is only valid for pointer types. "
                  "If you want a zero value, use the default initializer "
                  "(declare without '= null').") {}
};
```

### Acceptance Criteria

- `mutable x: int = null;` emits `NullAssignmentError`.
- `mutable p: Node* = null;` compiles correctly.
- `mutable p: Node*;` (default init from #176) initializes `p` to `null` implicitly.
- `mutable s: string = null;` emits `NullAssignmentError`.

---

## Issue #176 — Default Variable Initialization

**Labels:** `parser`, `phase-3`
**Depends on:** #175
**Files:** `src/Parser.cpp`

### Description

When a variable is declared without `= value`, auto-inject a zero value based on the declared type. This eliminates undefined behavior from uninitialized variables and enables the `mutable count: int;` pattern.

**Default values by type:**
| Type | Default |
|---|---|
| `int` | `0` |
| `double` | `0.0` |
| `bool` | `false` |
| `string` | `""` |
| pointer (`T*` or `ptr`) | `null` |

### Parser Changes (`ParseLetStatement`)

After parsing the type annotation (`ParseType()`), if the next token is `;` (not `=`):
- Do NOT error.
- Synthesize a default initializer `ExprAST` based on the declared type:
  - `IntType` → `NumberExprAST(0)`
  - `DoubleType` → `NumberExprAST(0.0)`
  - `BoolType` → `BoolExprAST(false)`
  - `StringType` → `StringExprAST("")`
  - pointer type → `NullExprAST()`
  - Unknown/named type (`NamedType`) → synthesize a zero-struct init (or defer to TypeChecker — emit a `DefaultInitExprAST` that the TypeChecker resolves)

```
ParseLetStatement:
  ...
  if CurTok == '=':
      consume '='
      Init = ParseExpression()
  else:
      Init = SynthesizeDefault(DeclType)
  expect ';'
  return VarDeclExprAST(Name, DeclType, Init, IsImmutable)
```

### Acceptance Criteria

- `mutable x: int;` compiles identically to `mutable x: int = 0;`.
- `mutable p: Node*;` compiles identically to `mutable p: Node* = null;`.
- `mutable s: string;` compiles identically to `mutable s: string = "";`.
- `immutable PI: double;` is a TypeChecker error (`immutable` requires an explicit initializer — a default-initialized `immutable` is always zero, which is useless and likely a bug).

---

## Issue #177 — Logical NOT `!` Operator

**Labels:** `parser`, `lexer`, `phase-3`
**Depends on:** #176
**Files:** `src/Lexer.cpp`, `src/Parser.cpp`, `src/Codegen.cpp`

### Description

The `!` character is currently only recognized as part of `!=`. Adding it as a standalone unary `not` operator is required for idiomatic boolean logic.

```turflang
mutable done: bool = false;
if !done { io.printline("still running"); }
```

### Lexer Changes

The Lexer's character loop currently reads `!` and peeks for `=` to produce `TOK_NEQ`. Extend this:

```cpp
case '!':
    if (getChar() == '=') return TOK_NEQ;   // !=  (existing)
    ungetChar();
    return '!';                              // standalone '!' (new)
```

### Parser Changes (`ParseUnary`)

Add a case for `'!'` at the top of `ParseUnary()`:

```cpp
if (CurTok == '!') {
    getNextToken(); // consume '!'
    auto Operand = ParseUnary();
    // Desugar: !x → x != true
    return std::make_unique<BinaryExprAST>(
        Loc, TOK_NEQ,
        std::move(Operand),
        std::make_unique<BoolExprAST>(Loc, true)
    );
}
```

This reuses the existing `!=` codegen path — no new Codegen required.

### Acceptance Criteria

- `!true` compiles to `false`.
- `!false` compiles to `true`.
- `if !flag { ... }` parses and compiles correctly.
- `!!x` (double negation) parses as `!(!x)` and compiles correctly.
- `!=` as a binary operator is unaffected.

---

## Issue #178 — `do { } while(cond)` Statement

**Labels:** `parser`, `lexer`, `ast`, `codegen`, `phase-3`
**Depends on:** #177
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`

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

## Issue #179 — `compare` Statement (Switch)

**Labels:** `parser`, `lexer`, `ast`, `codegen`, `phase-3`
**Depends on:** #178
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`

### Description

Add a `compare`/`matches`/`done` switch statement. No **body** fall-through — each arm's body is fully isolated. However, **multi-value matching** is supported by stacking `matches` arms before a shared body and a single `done`. The `other` arm is optional and must be last if present.

```turflang
// Single-value arm
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

// Multi-value arm — stacked matches, one shared body
compare (x) {
    matches (1):
    matches (2):
    matches (3):
        io.printline("one, two, or three");
    done

    other:
        io.printline("something else");
}
```

> **⛔ Explicitly Rejected**: `loop { }` keyword. It adds nothing over `while(true)`. `TOK_LOOP` must NOT be added at any point during implementation.

### Lexer Changes

Add to `Keywords` map:
```cpp
{"compare", TOK_COMPARE},
{"matches", TOK_MATCHES},
{"done",    TOK_DONE},
{"other",   TOK_OTHER},
```

Add to `Lexer.h`:
```cpp
TOK_COMPARE = -73,
TOK_MATCHES = -74,
TOK_DONE    = -75,
TOK_OTHER   = -76,
```

### AST Changes

Add `CompareExprAST`:

```cpp
struct CompareArm {
  std::unique_ptr<ExprAST> Value;     // nullptr for 'other' arm
  std::vector<std::unique_ptr<ExprAST>> Body;
};

class CompareExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Subject_;
  std::vector<CompareArm> Arms_;
  bool HasOtherArm_;
public:
  // ...
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitCompare(*this); }
};
```

### Parser Changes (`ParseCompare()`)

```
Consume TOK_COMPARE
Expect '('
Subject = ParseExpression()
Expect ')'
Expect '{'
Loop:
  if CurTok == TOK_MATCHES:
      // Collect one or more stacked 'matches (val):' headers
      Values = []
      while CurTok == TOK_MATCHES:
          consume 'matches'
          expect '('
          Values.push_back(ParseExpression())
          expect ')'
          expect ':'
      // Now parse the shared body (no body between stacked headers)
      parse body statements until TOK_DONE
      consume TOK_DONE
      // Each value in Values maps to the same body block
      for each V in Values:
          Arms.push_back({V, Body})
  elif CurTok == TOK_OTHER:
      consume 'other'
      expect ':'
      parse body statements until '}'
      HasOtherArm = true
      (break loop — other must be last)
  else:
      error "Expected 'matches' or 'other' in compare block"
Expect '}'
Return CompareExprAST(Subject, Arms, HasOtherArm)
```

### Codegen (`CompareExprAST::codegen()`)

```
Subject = codegen Subject_
AfterBB = new block "compare.after"

For each matches arm:
  ArmBB   = new block "compare.arm.N"
  ArmVal  = codegen arm value
  (build LLVM switch or chain of CondBr)

DefaultBB = HasOtherArm ? new OtherBB : AfterBB

Emit: switch Subject, DefaultBB [ case ArmVal → ArmBB, ... ]

For each ArmBB:
  [arm body IR]
  Br → AfterBB   // no fall-through

If HasOtherArm:
  [other body IR]
  Br → AfterBB

SetInsertPoint(AfterBB)
```

Prefer LLVM `SwitchInst` over chained `CondBr` for integer subjects — it compiles to a jump table when the backend deems it profitable.

### TypeChecker

Subject and all `matches` arm values must be the same type (`int` or `enum`). `string` subjects use `strcmp` chains (lower to `CondBr`, not `SwitchInst`). TypeChecker enforces this distinction.

### Acceptance Criteria

- The single-value example in the description compiles and runs correctly.
- Multi-value stacked arms (`matches (1): matches (2): body done`) compile correctly — both values jump to the same body block.
- Missing `done` after an arm body emits `Expected 'done' after matches arm body`.
- A `matches` arm with a body statement between two stacked headers emits a parse error.
- `other` appearing before a `matches` arm emits `'other' must be the last arm in compare`.
- Two `other` arms emit `Duplicate 'other' arm in compare`.
- `compare (x) { }` (no arms) is valid (no-op) — or emit a warning via `TurfWarning`.

---

## Issue #180 — Bitwise Operators (`&`, `|`, `~`, `^`, `<<`, `>>`)

**Labels:** `lexer`, `parser`, `codegen`, `phase-3`
**Depends on:** #179
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `src/Parser.cpp`, `src/Codegen.cpp`

### Description

Add the six bitwise operators for integer types. The `^` symbol is repurposed as **XOR** (not exponentiation — use `math.pow` or `**` for that). `~` is unary bitwise NOT.

### Lexer Changes

Add to `Lexer.h`:
```cpp
TOK_LSHIFT  = -77,   // <<
TOK_RSHIFT  = -78,   // >>
```

`&`, `|`, `~`, `^` are single characters and do not conflict with existing tokens in Turflang (verify `&&` and `||` are handled before checking single-char versions). Add disambiguation in the character loop:

```cpp
case '&': if (peek() == '&') { advance(); return TOK_AND; } return '&';
case '|': if (peek() == '|') { advance(); return TOK_OR;  } return '|';
case '<': if (peek() == '<') { advance(); return TOK_LSHIFT; } /* existing < handling */
case '>': if (peek() == '>') { advance(); return TOK_RSHIFT; } /* existing > handling */
case '~': return '~';
case '^': return '^';
```

### Parser Changes (BinopPrecedence)

Add to `BinopPrecedence` map (higher number = tighter binding):

```cpp
BinopPrecedence['|']        = 12;   // bitwise OR — below comparison
BinopPrecedence['^']        = 13;   // bitwise XOR
BinopPrecedence['&']        = 14;   // bitwise AND
BinopPrecedence[TOK_LSHIFT] = 20;   // shift — above additive
BinopPrecedence[TOK_RSHIFT] = 20;
```

Add `'~'` to `ParseUnary()` (same pattern as `!` in #177):

```cpp
if (CurTok == '~') {
    getNextToken();
    auto Operand = ParseUnary();
    return std::make_unique<UnaryExprAST>(Loc, '~', std::move(Operand));
}
```

### Codegen Changes (`BinaryExprAST::codegen()` and `UnaryExprAST::codegen()`)

```cpp
case '&':      return Builder->CreateAnd(L, R, "andtmp");
case '|':      return Builder->CreateOr(L, R, "ortmp");
case '^':      return Builder->CreateXor(L, R, "xortmp");
case TOK_LSHIFT: return Builder->CreateShl(L, R, "shltmp");
case TOK_RSHIFT: return Builder->CreateLShr(L, R, "lshrtmp");  // logical shift
// Unary:
case '~':      return Builder->CreateNot(Operand, "nottmp");
```

TypeChecker: all bitwise operands must be `int`. Applying them to `double` or `bool` is a `TypeError`.

### Acceptance Criteria

- `5 & 3` evaluates to `1`.
- `5 | 3` evaluates to `7`.
- `5 ^ 3` evaluates to `6` (XOR, not power).
- `1 << 4` evaluates to `16`.
- `256 >> 2` evaluates to `64`.
- `~0` evaluates to `-1` (all bits set, two's complement).
- `1.5 & 2` emits a TypeChecker `TypeError`.

---

## Issue #181 — String Operations (`+`, `==`, `s[i]`)

**Labels:** `codegen`, `typechecker`, `phase-3`
**Depends on:** #180
**Files:** `src/Codegen.cpp`, `src/TypeChecker.cpp`

### Description

Strings in Turflang are currently raw C strings (`i8*`). Three fundamental operations are missing:

1. **Concatenation** (`+`): `"Hello, " + name`
2. **Equality** (`==`): `name == "world"`
3. **Character access** (`s[i]`): `name[0]` → produces `int` (character code)

No new AST nodes — intercept in existing handlers.

### Codegen Changes (`BinaryExprAST::codegen()`)

**`+` for strings** — intercept when both operands are `string` (check via TypeChecker annotation):

```cpp
// Declare the turf_string_concat helper (or inline):
// i8* turf_string_concat(i8* a, i8* b)
// Implementation: malloc(strlen(a) + strlen(b) + 1), strcpy, strcat, return ptr
Function *ConcatFn = getOrDeclareStringConcat();
return Builder->CreateCall(ConcatFn, {L, R}, "concattmp");
```

`turf_string_concat` is declared as an `extern fn` and implemented in `lib/std/string.tr` during Phase 4. For now, declare it inline in Codegen with a `turf_` prefix to avoid clashing with libc names.

**`==` for strings** — intercept when both operands are `string`:

```cpp
// Declare strcmp: i32 strcmp(i8*, i8*)
Function *StrcmpFn = getOrDeclareStrcmp();
Value *Cmp = Builder->CreateCall(StrcmpFn, {L, R}, "cmptmp");
// strcmp returns 0 for equal — convert to bool:
return Builder->CreateICmpEQ(Cmp, ConstantInt::get(TheContext, APInt(32, 0)), "eqtmp");
```

**`s[i]` for strings** (`ArrayAccessExprAST::codegen()`):

```cpp
// s is i8*, i is int
// GEP: i8* ptr = s + i
Value *GEP = Builder->CreateGEP(IntegerType::get(TheContext, 8), S, {Idx}, "chartmp");
// Load the byte
Value *Char = Builder->CreateLoad(IntegerType::get(TheContext, 8), GEP, "charval");
// Zero-extend to i64 (Turflang int)
return Builder->CreateZExt(Char, IntegerType::get(TheContext, 64), "charext");
```

### TypeChecker Changes

`TypeChecker::visitBinary`:
- `string + string` → infer result type as `string`.
- `string == string` → infer result type as `bool`.
- `string + int` or `int + string` → `TypeError: string concatenation requires both operands to be 'string'. Use 'value to string' to convert.`

`TypeChecker::visitArrayAccess` (when base is `string`):
- Infer element type as `int` (character code).
- Index must be `int`.

### Acceptance Criteria

- `"Hello, " + "world"` compiles and produces `"Hello, world"` at runtime.
- `name == "world"` compiles and evaluates correctly.
- `"hello"[1]` produces `101` (`'e'` in ASCII).
- `"hi" + 5` emits a TypeChecker error with a hint to use `5 to string`.

---

## Issue #182 — `enum` Declaration

**Labels:** `parser`, `lexer`, `ast`, `codegen`, `typechecker`, `phase-3`
**Depends on:** #181
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`

### Description

Add first-class enum types. Enum variants are compile-time integer constants. The enum name is a proper type, making `Color.Red` type-checked against `Color` declarations.

```turflang
enum Color { Red, Green, Blue }

mutable c: Color = Color.Red;

compare (c to int) {
    matches (0): io.printline("red"); done
    matches (1): io.printline("green"); done
    matches (2): io.printline("blue"); done
}
```

### Lexer Changes

```cpp
{"enum", TOK_ENUM},  // TOK_ENUM = -79
```

### AST Changes

Add `EnumDeclExprAST`:

```cpp
class EnumDeclExprAST : public ExprAST {
  std::string Name_;
  std::vector<std::string> Variants_;  // ordered: index = value
public:
  // ...
  const std::string &getName() const { return Name_; }
  const std::vector<std::string> &getVariants() const { return Variants_; }
  llvm::Value *codegen() override;
  void accept(ASTVisitor &V) override { V.visitEnumDecl(*this); }
};
```

### Parser

```
Consume TOK_ENUM
Expect TOK_IDENTIFIER → save as Name
Expect '{'
Parse comma-separated list of TOK_IDENTIFIER (variant names)
Expect '}'
Return EnumDeclExprAST(Name, Variants)
```

### Resolver

`hoistTopLevel()`: register enum name in `SymTable_.registerStruct(Name)` (reuse struct registry for now — enums are named types). Store variant → integer mappings in a separate `EnumRegistry`.

### TypeChecker

- `Color` is a valid type name.
- `Color.Red` is type `Color` (i.e., `int` at the IR level, but `Color` at the TypeChecker level).
- Assigning an `int` literal directly to a `Color` variable requires an explicit `to Color` cast.

### Codegen

`EnumDeclExprAST::codegen()`: register each variant as a named global `i64` constant:

```cpp
// Color.Red  = 0
// Color.Green = 1
// Color.Blue  = 2
for (size_t i = 0; i < Variants_.size(); ++i) {
    std::string QualName = Name_ + "." + Variants_[i];
    auto *GV = new GlobalVariable(*TheModule, IntegerType::get(TheContext, 64),
                                  /*isConstant=*/true, GlobalValue::InternalLinkage,
                                  ConstantInt::get(TheContext, APInt(64, i)), QualName);
}
```

`VariableExprAST::codegen()`: when name contains `.`, look up the qualified global variable.

### Acceptance Criteria

- `enum Color { Red, Green, Blue }` compiles.
- `Color.Red` produces `0`, `Color.Green` produces `1`, `Color.Blue` produces `2`.
- `mutable c: Color = Color.Red;` type-checks correctly.
- `mutable c: Color = 0;` emits `TypeError: cannot assign 'int' to 'Color' without explicit cast`.
- `mutable c: Color = 0 to Color;` compiles.

---

## Issue #183 — CFG Phase 1: Cyclomatic Complexity & Infinite Loop Detection

**Labels:** `cfg`, `static-analysis`, `phase-3`
**Depends on:** #171 (`TurfWarning` must exist), #182
**Files:** `include/CFG.h`, `src/CFG.cpp`

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

## Issue #184 — CFG Phase 2: Liveness Pass & Infinite Recursion Detection

**Labels:** `cfg`, `static-analysis`, `phase-3`
**Depends on:** #183
**Files:** `include/CFG.h`, `src/CFG.cpp`, `src/CallGraph.cpp` (existing)

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

## Issue #185 — First-Class Functions

**Labels:** `typechecker`, `ast`, `codegen`, `phase-3`
**Depends on:** #184
**Files:** `include/Types.h`, `src/Parser.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`

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

## Issue #186 — `!T` Error Unions (`catch`, `?`, `error.XYZ`)

**Labels:** `parser`, `lexer`, `ast`, `typechecker`, `codegen`, `resolver`, `phase-3`
**Depends on:** #185 (all other Phase 3 items must be complete first)
**Files:** `include/Lexer.h`, `src/Lexer.cpp`, `include/AST.h`, `include/Types.h`, `src/Parser.cpp`, `src/Resolver.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`, `include/Errors.h`, `lib/std/*.tr`

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

This order minimizes breakage and respects all dependency chains:

```
#171 (TurfWarning)                — Phase 2 tail, do first — needed for CFG warnings
#172 (hint decoupling)            — Phase 2 tail, do second — needed before adding new errors

── Phase 3 proper ──

#173 (extern fn FFI)              — Group A, do first — unlocks malloc in test code
#174 (self-referential structs)   — Group A — depends on #173
#175 (null enforcement)           — Group A — depends on #174

#176 (default initialization)     — Group B — depends on #175
#177 (logical NOT !)              — Group B — depends on #176
#178 (do/while)                   — Group B — depends on #177
#179 (compare/matches)            — Group B — depends on #178
#180 (bitwise operators)          — Group B — depends on #179
#181 (string operations)          — Group B — depends on #180
#182 (enum)                       — Group B — depends on #181

#183 (CFG Phase 1)                — Group C — depends on #171, #182
#184 (CFG Phase 2)                — Group C — depends on #183

#185 (first-class functions)      — Group D — depends on #184
#186 (!T error unions)            — Group E — LAST. depends on ALL above.
```

**Why `!T` is last:** It touches the most files and introduces the most new concepts. Every other Phase 3 item produces a clean, stable compiler state that `!T` can be built on top of. The Resolver, TypeChecker, and Codegen are at their most stable after #185.
