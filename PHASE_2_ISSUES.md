# Phase 2 — GitHub Issues: The Grand Syntactic Overhaul

> All these issues depend on **Phase 1 (Issues #9, #10, #11)** being fully merged first.
> They can be worked in sub-groups but the order within each group matters (see dependencies).

---

## Final Turflang Syntax Reference (locked in)

```turflang
// Variables
mutable x: int = 5;
mut y: double = 3.14;
immutable PI: double = 3.14159;
immut MAX: int = getMax();

// Functions
fn add(a: int, b: int): int {
    return a + b;
}

// Structs
struct Point {
    x: int,
    y: int
}

// Struct initialization (named fields)
mutable p: Point = Point { x: 10, y: 20 };

// Casting
mutable n: double = 5;
mutable s: string = n to string;

// Ternary
mutable r: int = x > 0 give x otherwise -x;

// If blocks (no `then`)
if x > 0 {
    io.printline("positive");
} elseif x == 0 {
    io.printline("zero");
} else {
    io.printline("negative");
}

// Assert and fail
assert(x > 0);
assert(x > 0, "x must be positive");
fail("something went wrong");
```

---

## Issue #12 — Variable Declaration Overhaul: `mutable`/`mut` and `immutable`/`immut`

**Labels:** `parser`, `lexer`, `breaking-change`, `phase-2`
**Depends on:** #9, #10, #11
**Files:** `src/Lexer.cpp`, `include/Lexer.h`, `src/Parser.cpp`, `src/TypeChecker.cpp`, `src/SLMIntegration.cpp`

### Description

Replace all current C-style `type name = value` variable declarations with the new explicit mutability syntax. The parser currently uses a `GlobalSymbolTable` lookup inside `ParsePrimaryBase()` (L596) to decide whether an identifier is a type (declaration) or a variable (expression). This "Lexer Hack" must be completely removed.

**New syntax:**
```turflang
mutable x: int = 5;
mut x: int = 5;       // alias for mutable
immutable PI: double = 3.14159;
immut PI: double = 3.14159; // alias for immutable
```

### Lexer Changes (`Lexer.cpp`, `Lexer.h`)

Add to `Keywords` map:
```cpp
{"mutable",   TOK_MUTABLE},
{"mut",       TOK_MUTABLE},   // same token — aliases at Lexer level
{"immutable", TOK_IMMUTABLE},
{"immut",     TOK_IMMUTABLE}, // same token — aliases at Lexer level
```

Add token enums to `Lexer.h`:
```cpp
TOK_MUTABLE   = -65,
TOK_IMMUTABLE = -66,
TOK_COLON     = -67,
```

Handle `:` in the Lexer's character loop: when the character is `:` and the next character is NOT `:` (to distinguish from `::` scope resolution if ever needed), return `TOK_COLON`.

### Parser Changes (`Parser.cpp`)

1. **Add `ParseLetStatement(bool IsImmutable)`** — a new dedicated parsing function:
   ```
   Consume TOK_MUTABLE or TOK_IMMUTABLE
   Expect TOK_IDENTIFIER → save as Name
   Expect TOK_COLON
   Call ParseType() → save as DeclType
   Expect '=' 
   Call ParseExpression() → save as Init
   Expect ';'
   Return VarDeclExprAST(Name, DeclType, Init, IsImmutable)
   ```

2. **Update `ParseBlock()`** — add cases at the top of the while loop:
   ```
   if (CurTok == TOK_MUTABLE)   return ParseLetStatement(false);
   if (CurTok == TOK_IMMUTABLE) return ParseLetStatement(true);
   ```

3. **Remove the Lexer Hack** from `ParsePrimaryBase()`:
   - Delete the `GlobalSymbolTable->LookupType(IdentifierStr)` branch (L596-631).
   - Delete the entire `case TOK_TYPE_INT:` / `case TOK_TYPE_DOUBLE:` etc. block (L715-756). These type keywords in statement position are now invalid — only valid in type-annotation position (after `:`).
   - The `ParseType()` function is still used internally by `ParseLetStatement`, function prototypes, and struct fields. It does not need to change.

4. **Update `ParseType()`** — remove the `GlobalSymbolTable->LookupType()` call at L224. Replace with unconditional `NamedType(BaseName)` creation. The Resolver will validate it later.

### AST / TypeChecker Changes

- `VarDeclExprAST` gains a `bool IsImmutable` field.
- `TypeChecker::visitAssignment` (or `visitVarExpr`): if the target variable's `IsImmutable` is `true` and this is not its first assignment (i.e., it is being reassigned), emit: `TypeError: Cannot reassign immutable variable 'x'`.

### SLMIntegration.cpp Update

`findVariableDeclarationLine()` (L236) currently scans for patterns like `"int x = ..."`. After this issue, update the patterns to scan for `"mutable x:"` and `"immut x:"`.

### Acceptance Criteria

- `mutable x: int = 5;` parses and compiles to the same IR as `int x = 5` did before.
- `mut x: int = 5;` is identical to `mutable x: int = 5;`.
- `immutable x: int = 5; x = 10;` emits a TypeChecker error, not a runtime crash.
- `int x = 5` (old syntax) now emits a clean parse error: `Expected 'mutable' or 'immutable' to begin a declaration.`
- The `GlobalSymbolTable->LookupType()` call in `ParsePrimaryBase` is gone.

---

## Issue #13 — Function Signature Syntax Overhaul: `fn name(arg: type): returnType`

**Labels:** `parser`, `lexer`, `breaking-change`, `phase-2`
**Depends on:** #12 (for `TOK_COLON`)
**Files:** `src/Parser.cpp`, `lib/std/*.tr`, `tests/*.tr`

### Description

Change function signatures from `fn returnType name(type arg)` to `fn name(arg: type): returnType`. This makes the parser LL(1) at the function level — the parser sees `fn`, then immediately reads the name without needing to parse an arbitrarily complex return type first.

**Old:**
```turflang
fn int add(int a, int b) { return a + b }
```

**New:**
```turflang
fn add(a: int, b: int): int {
    return a + b;
}
```

**Void functions (no return type needed):**
```turflang
fn greet(name: string): void {
    io.printline("hello");
}
```
Return type is always explicit (strictly typed language). No implicit void.

### Parser Changes (`Parser.cpp`)

Update `ParsePrototype()` and `ParseFuncDef()`:

```
Consume TOK_FN
Expect TOK_IDENTIFIER → save as FuncName
Expect '('
Parse parameter list: each param is: Identifier TOK_COLON ParseType()
  Params separated by ','
Expect ')'
Expect TOK_COLON
Call ParseType() → save as ReturnType
If CurTok == '{': parse body (function definition)
If CurTok == ';': external declaration (forward decl, used by @intrinsic)
```

### Standard Library & Test File Updates

Every `.tr` file in `lib/std/` and every file in `tests/` must be updated to the new signature format. This is mechanical but must be done before any test can pass.

### Acceptance Criteria

- `fn add(a: int, b: int): int { return a + b; }` parses and compiles correctly.
- Old-style `fn int add(int a, int b)` emits a clean parse error.
- All standard library `.tr` files updated and compile without errors.

---

## Issue #14 — Struct Syntax Overhaul: Named Fields and `StructName { field: val }` Init

**Labels:** `parser`, `breaking-change`, `phase-2`
**Depends on:** #12 (for `TOK_COLON`), #13
**Files:** `src/Parser.cpp`, `src/TypeChecker.cpp`, `src/Codegen.cpp`

### Description

Two changes:

**1. Struct field declarations use `name: type` syntax:**
```turflang
struct Point {
    x: int,
    y: int
}
```

**2. Struct initialization uses named fields (Rust-style brace syntax):**
```turflang
mutable p: Point = Point { x: 10, y: 20 };
```
Named fields mean the user does not need to remember field order. TypeChecker validates that all fields are provided and no unknown fields are given.

### Parser Changes (`Parser.cpp`)

**`ParseStructDecl()`**: change field parsing from `ParseType()` then `Identifier` to `Identifier` then `TOK_COLON` then `ParseType()`.

**Remove `StructInitExprAST` positional construction from `ParsePostfixExpression()`** — the `Identifier '(' args ')` path that called `GlobalSymbolTable->LookupType()` (L347-357) is removed with Issue #12. It needs to be replaced with a new path in `ParsePrimaryBase()`: when the parser sees `TOK_IDENTIFIER` followed by `{`, it creates a struct init:

```
ParsePrimaryBase sees: TOK_IDENTIFIER then peeks at next token
If next token is '{':
  Consume both
  Parse comma-separated: Identifier ':' ParseExpression() pairs
  Expect '}'
  Return StructInitExprAST(name, namedFields)
```

**`StructInitExprAST`** gains a `std::vector<std::pair<std::string, ExprAST*>> NamedFields` instead of a positional `Args` vector.

### TypeChecker Changes

`TypeChecker::visitStructInit`:
- Look up the struct definition.
- Verify every declared field has a corresponding named argument.
- Verify no extra fields are provided.
- Emit specific errors: `Missing field 'x' in initialization of 'Point'`.

### Codegen Changes

`StructInitExprAST::codegen()`: reorder the `NamedFields` to match the order of fields in the struct definition before emitting `insertvalue` instructions.

### Acceptance Criteria

- `struct Point { x: int, y: int }` parses correctly.
- `Point { x: 10, y: 20 }` and `Point { y: 20, x: 10 }` both compile identically.
- `Point { x: 10 }` (missing `y`) emits: `TypeChecker: Missing field 'y' in initialization of 'Point'`.
- `Point { x: 10, y: 20, z: 5 }` (unknown field) emits: `TypeChecker: Unknown field 'z' in struct 'Point'`.
- Old positional `Point(10, 20)` syntax emits a clean parse error.

---

## Issue #15 — Cast Syntax: `x to type`

**Labels:** `parser`, `lexer`, `breaking-change`, `phase-2`
**Depends on:** #12
**Files:** `src/Lexer.cpp`, `include/Lexer.h`, `src/Parser.cpp`, `src/Codegen.cpp`

### Description

Replace the current `int(x)`, `double(x)`, `string(x)` cast syntax (which required a `GlobalSymbolTable` lookup to distinguish casts from function calls) with the `x to type` keyword cast.

**New:**
```turflang
mutable n: double = 3.99;
mutable i: int = n to int;
mutable s: string = i to string;
```

### Lexer Changes

Add to `Keywords` map:
```cpp
{"to", TOK_TO},
```

Add to `Lexer.h`:
```cpp
TOK_TO = -68,
```

### Parser Changes

Add `TOK_TO` to the `BinopPrecedence` table with a **very low precedence** (lower than `||`, higher than assignment) so it binds after the full left-hand expression is built:

```cpp
BinopPrecedence[TOK_TO] = 5; // lower than || (15)
```

In `ParseBinOpRHS()`: add a case for `BinOp == TOK_TO`. Instead of creating a `BinaryExprAST`, create a `CastExprAST(LHS, ParseType())`. Note: the RHS of `to` is parsed by calling `ParseType()` directly, not `ParseUnary()`.

### Codegen Changes

`CastExprAST::codegen()` already exists and handles the underlying LLVM conversions. No changes needed there.

**Remove** `ParseCastExpr()` and the old `int(x)` parsing path from `ParsePrimaryBase()` (this is already cleaned up in Issue #12 when the type-in-statement-position blocks are removed).

### Acceptance Criteria

- `5.5 to int` compiles to the same IR as `int(5.5)` did before.
- `42 to string` compiles correctly.
- `x to int to double` parses left-to-right as `(x to int) to double`.
- Old `int(x)` cast syntax emits: `Expected 'mutable' or 'immutable'...` (a parse error, since `int` is now only valid in type-annotation position).

---

## Issue #16 — Unify `if`: Remove `then`, Add Ternary `cond give a otherwise b`

**Labels:** `parser`, `lexer`, `breaking-change`, `phase-2`
**Depends on:** #12
**Files:** `src/Lexer.cpp`, `include/Lexer.h`, `src/Parser.cpp`, `src/AST.h`, `tests/*.tr`

### Description

Two changes in one issue since they both affect `ParseIfExpr`:

**1. Remove `then` keyword.** The expression-style `if cond then a else b` is deleted. All `if` blocks must use `{ }`. `elseif` keyword stays.

**2. Add ternary operator `cond give a otherwise b`.** The `otherwise` keyword replaces the `:` to avoid any ambiguity with the type-annotation colon introduced in Issue #12.

```turflang
// No more: if x > 0 then x else -x
// New ternary:
mutable abs_x: int = x > 0 give x otherwise -x;
```

// Block if (unchanged structure, just now mandatory):
if x > 0 {
    io.printline("positive");
} elseif x == 0 {
    io.printline("zero");
} else {
    io.printline("negative");
}
```

### Lexer Changes

1. **Remove** `{"then", TOK_THEN}` from the Keywords map.
2. **Add** `{"give", TOK_GIVE}` and `{"otherwise", TOK_OTHERWISE}` to the Keywords map.
3. **Add** `TOK_GIVE = -69`, `TOK_OTHERWISE = -70` to `Lexer.h`.
4. No symbol needed — `give` is a plain keyword, zero lexer character-loop changes required.

### Parser Changes

**`ParseIfExpr()`:**
- Delete the `if (CurTok == TOK_THEN)` branch (L459-462).
- Delete `IsBlockForm` tracking variable.
- The function now always expects `{` after the condition. If it doesn't find `{`, emit: `Expected '{' after if-condition. Use '? ... otherwise ...' for single-expression ternary.`
- The `elseif` chain and `else` block parsing remain unchanged.

**`ParseExpression()`**: Add a ternary layer **between** `ParseBinOpRHS` and the caller. After `ParseBinOpRHS` returns the `LHS`, check:

```
if CurTok == TOK_GIVE:
    consume 'give'
    parse ThenExpr = ParseExpression()   // recursive, handles nested ternary
    expect TOK_OTHERWISE
    consume 'otherwise'
    parse ElseExpr = ParseExpression()
    return TernaryExprAST(LHS, ThenExpr, ElseExpr)
```

**New AST node `TernaryExprAST`** (or desugar inline to `IfExprAST` — simpler, no new Codegen needed).

**`ParsePrimaryBase()`:**
- Remove the `case TOK_THEN:` and `case TOK_ELSE:` blocks (L691-713) — `then` no longer exists. `else` alone at the start of a primary is still an error, handle it with a clean message.

### Acceptance Criteria

- `x > 0 give x otherwise -x` parses and compiles to the same IR as the equivalent `if` block.
- `if cond then val else val` emits: `Expected '{' after if-condition`.
- Nested ternary `a give (b give c otherwise d) otherwise e` parses correctly.
- `elseif` chains still work.

---

## Issue #17 — Strict Semicolon Rule: Delete `RequiresSemicolon()`

**Labels:** `parser`, `phase-2`
**Depends on:** #12, #13, #14, #15, #16 (must be done last in Phase 2 — all block-producing constructs must be updated first)
**Files:** `src/Parser.cpp`, `src/main.cpp`

### Description

The `RequiresSemicolon()` function at `Parser.cpp L981-991` uses `dynamic_cast` on every parsed AST node to decide if a semicolon is required. This is expensive and breaks whenever a new block construct is added without updating the function.

**New rule (simple and absolute):**
- Expression statements end with `;`.
- Block-producing constructs (`if`, `while`, `for`, `fn`, `struct`) end with `}` and never need `;`.

### Parser Changes

**Delete `RequiresSemicolon()`** entirely.

**Update `ParseBlock()`** (L993-1033):
```cpp
while (CurTok != '}' && CurTok != TOK_EOF) {
    // Skip any stray ';' (empty statements are fine)
    if (CurTok == ';') { getNextToken(); continue; }

    // Block-producing statements: parse and do NOT expect ';'
    if (CurTok == TOK_MUTABLE || CurTok == TOK_IMMUTABLE) {
        Exprs.push_back(ParseLetStatement(...));
        continue; // ParseLetStatement already consumed ';'
    }
    if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_FOR ||
        CurTok == TOK_FN || CurTok == TOK_STRUCT) {
        Exprs.push_back(ParseExpression()); // These parse their own '}'
        continue;
    }

    // Everything else is an expression statement: MUST end with ';'
    auto Expr = ParseExpression();
    if (!Expr) return nullptr;
    if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after statement.");
        return nullptr;
    }
    getNextToken(); // eat ';'
    Exprs.push_back(std::move(Expr));
}
```

**Update top-level parsing in `main.cpp`**: Apply the same strict rule for top-level statements (outside any block). Top-level `fn` and `struct` declarations do not need `;`. Top-level expressions (if any are valid) require `;`.

### Acceptance Criteria

- `RequiresSemicolon` is deleted and not referenced anywhere.
- Missing `;` after `mutable x: int = 5` produces a clean `Expected ';' after statement` error.
- `if x > 0 { ... };` (semicolon after block) produces a clean `Unexpected ';'` error.
- All tests still pass (since test files are updated in #12-#16).

---

## Issue #18 — Two-Pass Resolver: Hoist Structs and Functions for Forward Declarations

**Labels:** `resolver`, `phase-2`
**Depends on:** #12, #13, #14 (new syntax must exist first)
**Files:** `src/Resolver.cpp`, `src/ResolverSymbolTable.cpp`

### Description

After the syntactic overhaul, the parser no longer needs `GlobalSymbolTable` lookups mid-parse. The Resolver now becomes the single authority for name resolution. To allow structs and functions to be used before they are defined (forward references), the Resolver must do a **hoisting sweep** before its full traversal.

### Resolver Changes

Add a new pass in `Resolver::run()`:

```
Pass 1 — Hoisting:
  Walk only the top-level AST nodes.
  For each StructDeclExprAST: call SymTable_.registerStruct(Name, opaqueType)
  For each FuncDefExprAST: call SymTable_.registerCurrentFileFunction(sym)
    - Only register the signature (name, params, returnType), not the body.

Pass 2 — Full Resolution (existing behavior):
  Walk the entire AST, resolve all variable references, function calls, and type annotations.
  Because Pass 1 already registered all structs and functions, forward references resolve correctly.
```

**This is why the compiler no longer needs `GlobalSymbolTable->LookupType()` in `ParseType()`**: The parser creates a `NamedType("MyStruct")` placeholder. The Resolver's Pass 2 resolves it by looking up the name that was registered in Pass 1.

### Acceptance Criteria

```turflang
fn foo(): int {
    return bar();   // bar() defined below — should work
}

fn bar(): int {
    return 42;
}
```
Compiles without errors.

```turflang
struct Node {
    val: int,
    next: Node*    // self-referential — should work
}
```
Compiles without errors (combined with Issue #19's LLVM fix).

---

## Issue #19 — Remove `try/onError/raise`

**Labels:** `parser`, `lexer`, `codegen`, `breaking-change`, `phase-2`
**Depends on:** #20 (`assert` and `fail` must exist before `raise` can be removed from Codegen)
**Files:** `src/Lexer.cpp`, `include/Lexer.h`, `src/Parser.cpp`, `src/AST.h`, `src/Codegen.cpp`

### Description

Remove the `try/onError/raise` exception system entirely. Error handling in Turflang is done through return values (`Result`-style structs) and `fail()` for unrecoverable panics.

### Critical: Fix Codegen First

`RaiseExprAST` is currently called directly inside `BinaryExprAST::codegen()` at:
- **L691**: division by zero error path
- **L727**: modulo by zero error path

Both must be replaced with a `fail()` call **before** `RaiseExprAST` is deleted:

```cpp
// Replace:
auto RaiseEx = std::make_unique<RaiseExprAST>(Loc, std::move(ExInit));
RaiseEx->codegen();
// With:
// Call the fail() intrinsic codegen directly:
CodegenIntrinsic("fail", {Builder->CreateGlobalString("Division by zero")}, Loc);
```

### Lexer Changes

Remove from `Keywords` map: `"try"`, `"onError"`, `"raise"`.

### Parser Changes

- Delete `ParseTryExpr()` and `ParseRaiseExpr()` function bodies.
- Remove `case TOK_TRY:` from `ParsePrimaryBase()`.

### AST Changes

Delete from `AST.h`: `TryExprAST`, `OnErrorExprAST`, `RaiseExprAST` class definitions.

### Codegen Changes

Delete the `codegen()` method bodies for `TryExprAST`, `OnErrorExprAST`, and `RaiseExprAST`.

### Acceptance Criteria

- The compiler builds cleanly after deletion.
- `try { } onError(e) { }` emits: `Unexpected token 'try'`.
- Division by zero at runtime calls `fail("Division by zero")` and aborts with a message.

---

## Issue #20 — `assert()` and `fail()` Built-in Intrinsics

**Labels:** `builtins`, `codegen`, `phase-2`
**Depends on:** #9, #10, #11 (Phase 1 must be done; does NOT depend on other Phase 2 issues — can be done first in Phase 2)
**Files:** `src/Builtins.cpp`, `include/Builtins.h`

### Description

Add two built-in intrinsics critical for testing and error handling:

```turflang
assert(x > 0);
assert(x > 0, "x must be positive");
fail("something went horribly wrong");
```

`assert` is the first thing needed after Phase 1 completes, because it allows rewriting all compiler tests without manually inspecting output values.

### Builtins.cpp Changes

In `RegisterBuiltins()`, register:
```cpp
{"assert", {/*arity*/ -1, /*variadic*/}},  // accepts 1 or 2 args
{"fail",   {/*arity*/ 1}},
```

In `CodegenIntrinsic()`:

**`fail` (1 argument — string message):**
```cpp
// Get or declare fprintf and abort from libc
Function *FPrintF = GetOrDeclareFPrintF();
Function *Abort   = GetOrDeclareAbort();
Value *Stderr = Builder->CreateLoad(..., GetStderr());
Builder->CreateCall(FPrintF, {Stderr, MsgVal});
Builder->CreateCall(Abort, {});
Builder->CreateUnreachable();
```

**`assert` (1 or 2 arguments):**
```cpp
// Arg 0: condition (bool)
// Arg 1 (optional): message string
Value *Cond = Args[0];
// Generate: if (!cond) { fail(msg); }
BasicBlock *FailBB = BasicBlock::Create(..., "assert.fail");
BasicBlock *PassBB = BasicBlock::Create(..., "assert.pass");
Builder->CreateCondBr(Cond, PassBB, FailBB);
Builder->SetInsertPoint(FailBB);
// Call fail() with either the user message or a default "Assertion failed"
CodegenIntrinsic("fail", {msg}, Loc);
Builder->SetInsertPoint(PassBB);
```

### Acceptance Criteria

- `assert(true)` compiles and does nothing at runtime.
- `assert(false)` aborts with a message to stderr.
- `assert(false, "custom message")` aborts with `"custom message"` to stderr.
- `fail("oh no")` always aborts with `"oh no"` to stderr.
- `assert(1 == 2)` in a test file correctly fails the test.

---

## Execution Order

This order minimizes breakage across the phase:

```
#20 (assert/fail)        — independent, do first
#12 (variables)          — foundational, all others depend on it
#13 (functions)          — depends on #12 (TOK_COLON)
#14 (structs)            — depends on #12, #13
#15 (cast `to`)          — depends on #12
#16 (if/ternary)         — depends on #12
#19 (remove try/raise)   — depends on #20 (fail must exist before raise is deleted)
#18 (two-pass resolver)  — depends on #12-#16 (needs new syntax to be parseable)
#17 (semicolons)         — depends on ALL above (do absolutely last)
```
