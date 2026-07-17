# Turflang Roadmap: Strings · Enums · Stdlib Rewrite

> **Status:** Design complete, awaiting implementation.
> **Scope:** Three interlocking milestones that must land together.
> **Author:** a2ys (architecture) + Antigravity (design doc)
> **Date:** 2026-07-17

---

## Table of Contents

1. [Locked Decisions](#locked-decisions)
2. [Milestone 0 — `mem` + `math` Stdlib Rewrite](#milestone-0--mem--math-stdlib-rewrite)
3. [Milestone 1 — String Operations](#milestone-1--string-operations)
4. [Milestone 2 — Enums](#milestone-2--enums)
5. [Complete File Change List](#complete-file-change-list)
6. [Test Matrix](#test-matrix)
7. [Scalability Architecture](#scalability-architecture)
8. [Verification Plan](#verification-plan)

---

## Locked Decisions

| ID | Decision |
|---|---|
| D1 | Enum underlying type is parameterized via `TurfEnumType::UnderlyingType`. Default = new `TurfInt32` singleton (`i32`). Never hardcoded. `enum Color : u8` works in Phase 2 by adding `u8` to the type system only. |
| D2 | String ordering (`<`, `>`, `<=`, `>=`) is byte-by-byte via `strcmp`. Unicode-aware sorting deferred to `unicode.compare(...)` / `unicode.collate(...)`. |
| D3 | Enum variants always require the prefix: `Direction::North`. No unqualified variant import, ever. Prevents ambiguity when two enums share a variant name (e.g. `Direction::North` vs `Compass::North`). |
| D4 | `compare` on an enum without `otherwise` is a **compile error** if any variant is uncovered. Missing variants are listed by name in the error. `otherwise` always satisfies exhaustiveness. This is Phase 1. |
| D5 | No auto-generated `Direction_to_string()` C functions per enum. Enums are **pure metadata** in the compiler. **We do NOT add any new `@intrinsic` functions to the stdlib.** Instead, metadata is accessed entirely through native language syntax: casts (`d to string`, `"North" to Direction`) and namespace pseudo-variants (`Direction::COUNT`, `Direction::VALUES`, `Direction::NAMES`). |
| D6 | `@intrinsic` is deprecated for stdlib use. `extern fn` is the correct path. `mem.tr` and `math.tr` are rewritten this sprint. `io.tr` (`print`, `printline`, `input`) stays `@intrinsic` in Phase 1 — they are genuinely irreducible (type-polymorphic format dispatch). |

---

## Milestone 0 — `mem` + `math` Stdlib Rewrite

### Background: The `@intrinsic` Problem

The compiler has two mechanisms for calling external functions:

| Mechanism | How it works | Problem |
|---|---|---|
| `@intrinsic("name")` | Parser records the name on `FuncDefExprAST`. Codegen dispatches to `CodegenIntrinsic()` which hardcodes LLVM IR in C++. | Every new stdlib function requires editing compiler C++. Tightly coupled. |
| `extern fn name(...): T;` | Parser creates `ExternFuncDeclExprAST`. Codegen emits `Function::Create(ExternalLinkage, name)` — a real LLVM prototype that resolves against libc at link time. | **Already fully implemented.** Clean FFI path. |

**Today's `mem.tr` is 9 lines of `@intrinsic` stubs. `math.tr` is 10 lines of `@intrinsic` stubs. Neither has any real implementation.** `collections.tr` calls bare `malloc`/`realloc` with no module qualifier at all, bypassing `mem` entirely.

### What Stays as `@intrinsic` (Irreducible Primitives)

These cannot be expressed as `extern fn` — they require compiler-side semantics:

| Builtin | Why it must stay |
|---|---|
| `typeof(x)` | Returns a compile-time string based on the LLVM type of `x`. No C function knows Turf types. |
| `lengthof(s)` | Could become `extern fn strlen` — documented as migration path in Phase 2. |
| `assert(cond, msg)` | Emits a `CondBr` → fail block. Control-flow IR, not a simple call. |
| `fail(msg)` | Emits `fprintf` + `abort` + `unreachable`. Must signal LLVM that the block terminates. |
| `print` / `printline` | Type-polymorphic: format string (`%lld`, `%g`, `%s`) depends on LLVM type of argument. Requires compiler dispatch. Phase 2: migrate via `fmt` module. |
| `input` | Multi-step: `calloc` + `scanf` + `getchar` with control flow. Phase 2: migrate once `calloc` is in `mem.tr`. |

### New `lib/std/mem.tr`

**Replaces:** 3-line `@intrinsic` stub file.

```turf
// ─── Low-level C allocator bindings ──────────────────────────────────────────
extern fn malloc(size: int): void*;
extern fn calloc(count: int, size: int): void*;
extern fn realloc(ptr: void*, new_size: int): void*;
extern fn free(ptr: void*): void;

// ─── Memory primitives ────────────────────────────────────────────────────────
extern fn memcpy(dst: void*, src: void*, n: int): void*;
extern fn memmove(dst: void*, src: void*, n: int): void*;
extern fn memset(ptr: void*, val: int, n: int): void*;
extern fn memcmp(a: void*, b: void*, n: int): int;

// ─── Safe Turf-level helpers ──────────────────────────────────────────────────

// alloc: malloc with OOM abort
fn alloc(size: int): void* {
    mut result: void* = malloc(size);
    if result == null {
        fail("mem.alloc: allocation failed — out of memory");
    }
    return result;
}

// alloc_zeroed: calloc wrapper
fn alloc_zeroed(count: int, elem_size: int): void* {
    return calloc(count, elem_size);
}

// grow: realloc with OOM abort
fn grow(ptr: void*, new_size: int): void* {
    mut result: void* = realloc(ptr, new_size);
    if result == null {
        fail("mem.grow: allocation failed — out of memory");
    }
    return result;
}

// dup: copy n bytes from src into a fresh heap buffer
fn dup(src: void*, n: int): void* {
    mut dst: void* = alloc(n);
    memcpy(dst, src, n);
    return dst;
}

// zero: write n zero bytes at ptr
fn zero(ptr: void*, n: int): void {
    memset(ptr, 0, n);
}
```

**Why `calloc`?** Allocates and zeroes. The compiler's `int → string` fix (see §1A Bug 1) and future `io.tr` migration both need zeroed buffers.

**Why `memcpy`/`memmove`/`memset`/`memcmp`?** `string.tr`'s `substring` and `dup`, `enum.VALUES` array, and future struct copying all need byte-level primitives. Without them in `mem.tr`, every stdlib module embeds its own C binding — the opposite of the intended design.

**Why `grow` / `alloc`?** `list.tr` and `string.tr` pattern: "realloc or die." Checked wrappers avoid repeating the null check everywhere.

### New `lib/std/math.tr`

**Replaces:** 10-line `@intrinsic` stub file.

```turf
// ─── C math library bindings (requires -lm at link time) ─────────────────────
extern from "m" fn pow(base: double, exp: double): double;
extern from "m" fn sqrt(val: double): double;
extern from "m" fn sin(val: double): double;
extern from "m" fn cos(val: double): double;
extern from "m" fn tan(val: double): double;
extern from "m" fn floor(val: double): double;
extern from "m" fn ceil(val: double): double;
extern from "m" fn round(val: double): double;
extern from "m" fn fabs(val: double): double;
extern from "m" fn log(val: double): double;
extern from "m" fn log2(val: double): double;
extern from "m" fn log10(val: double): double;
extern from "m" fn exp(val: double): double;

// ─── abs: overloaded ──────────────────────────────────────────────────────────
fn abs(val: double): double {
    return fabs(val);
}
fn abs(val: int): int {
    if val < 0 { return 0 - val; }
    return val;
}

// ─── power: four overloads (mirrors previous @intrinsic behaviour) ────────────
fn power(base: double, exp: double): double { return pow(base, exp); }
fn power(base: int, exp: int): int          { return pow(base to double, exp to double) to int; }
fn power(base: int, exp: double): double    { return pow(base to double, exp); }
fn power(base: double, exp: int): double    { return pow(base, exp to double); }

// ─── min / max / clamp — previously missing from stdlib ──────────────────────
fn min(a: int, b: int): int       { if a < b { return a; } return b; }
fn min(a: double, b: double): double { if a < b { return a; } return b; }
fn max(a: int, b: int): int       { if a > b { return a; } return b; }
fn max(a: double, b: double): double { if a > b { return a; } return b; }

fn clamp(val: int, lo: int, hi: int): int {
    if val < lo { return lo; }
    if val > hi { return hi; }
    return val;
}
fn clamp(val: double, lo: double, hi: double): double {
    if val < lo { return lo; }
    if val > hi { return hi; }
    return val;
}
```

**New additions not previously available:** `tan`, `log`, `log2`, `log10`, `exp`, `min`, `max`, `clamp`.

**`extern from "m"`:** The `ExternFuncDeclExprAST` already handles `LibName_` — it appends `-lm` to the linker flags automatically via `g_RequiredLibs`.

### `collections.tr` Fix

**Problem:** Calls bare `malloc`, `realloc`, `fail` with no module qualifier.

**Fix:** Add `import mem;` at the top; replace `malloc(...)` → `mem.alloc(...)`, `realloc(...)` → `mem.grow(...)`.

### `Builtins.cpp` Cleanup

**Remove** `CodegenIntrinsic` cases for: `"malloc"`, `"realloc"`, `"free"`, `"power"`, `"abs"`, `"llvm.*"`.

These are now handled by `extern fn` declarations in `mem.tr` and `math.tr`. The C++ compiler no longer needs to know about them.

**Keep** (with explanatory comment): `"print"`, `"printline"`, `"input"`, `"typeof"`, `"lengthof"`, `"assert"`, `"fail"`.

**Important:** Compiler-internal `malloc` calls inside `Codegen.cpp` (e.g. `EmitStringConcat`, the int→string cast fix) are **not** the same as stdlib intrinsics. They stay as `TheModule->getFunction("malloc")` lookups — the compiler emitting its own IR for low-level string operations. This is correct and untouched.

---

## Milestone 1 — String Operations

### 1A — Bug Fixes

#### Bug 1 — Dangling Stack Pointer: `int to string` and `double to string` ⚠️

**File:** `src/Codegen.cpp`, `CastExprAST::codegen()`, lines 1627–1668.

**Root cause:** Both cast paths use `TmpB.CreateAlloca` — a stack allocation at function entry — as the `snprintf` output buffer. When the result escapes the function (return value, stored variable), the pointer points to dead stack memory.

**Broken today — silently corrupts memory:**
```turf
fn make_label(n: int): string {
    return n to string;  // pointer into this function's dead stack frame
}
fn main(): int {
    mut label: string = make_label(42);
    io.printline(label);  // reads garbage / crashes
    return 0;
}
```

**Fix:** Replace `TmpB.CreateAlloca` with `malloc(32)` in both branches.

```cpp
// BEFORE (broken):
AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize, "int_to_str_buf");

// AFTER (correct):
Function *MallocF = TheModule->getFunction("malloc");
if (!MallocF) {
    FunctionType *FT = FunctionType::get(
        PointerType::get(*TheContext, 0),
        {Type::getInt64Ty(*TheContext)}, false);
    MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc", TheModule.get());
}
Value *Buf = Builder->CreateCall(MallocF, {BufSize}, "int_to_str_buf");
```

Apply identically to the `double → string` branch.

#### Bug 2 — Missing String Ordering Operators

**Root cause:** `<`, `>`, `<=`, `>=` are only wired for numeric types. String comparison by ordering (e.g. alphabetical sort) panics or gives nonsense results.

**Fix in `TypeChecker.cpp` — `inferRelationalType()`:**
```cpp
// Add before the "requires numeric operands" guard:
if (LhsTy->isString() && RhsTy->isString()) {
    return TurfBool();  // byte-by-byte ordering via strcmp (Decision D2)
}
```

**Fix in `Codegen.cpp` — new helper + wire-up:**
```cpp
static Value* EmitStrcmpRelational(Value* L, Value* R, int Op) {
    Value* Cmp  = EmitStrcmp(L, R);   // existing; returns i32
    Value* Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    switch (Op) {
        case '<':     return Builder->CreateICmpSLT(Cmp, Zero, "strlt");
        case '>':     return Builder->CreateICmpSGT(Cmp, Zero, "strgt");
        case TOK_LEQ: return Builder->CreateICmpSLE(Cmp, Zero, "strle");
        case TOK_GEQ: return Builder->CreateICmpSGE(Cmp, Zero, "strge");
        default:      llvm_unreachable("invalid string relational op");
    }
}
// In BinaryExprAST::codegen(), at the top of the <, >, LEQ, GEQ cases:
if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString())
    return EmitStrcmpRelational(L, R, Op);
```

#### Bug 3 — Memory Leak in `EmitStringConcat` (documented, Phase 1 accepted)

Every `+` on strings calls `malloc` and never frees. Tagged `// TURF-MEMLEAK` for future arena/GC work. No code change in Phase 1.

### 1B — New `lib/std/string.tr`

Import: `import str;` — call as `str.fn_name(args)`.

**C extern bindings (in `string.tr`):**
```turf
extern fn strstr(haystack: string, needle: string): string;
extern fn strncpy(dst: string, src: string, n: int): string;
extern fn toupper(c: int): int;
extern fn tolower(c: int): int;
extern fn strlen(s: string): int;
```

**Function surface:**

| Function | Signature | Implementation strategy |
|---|---|---|
| `contains` | `(s: string, needle: string): bool` | `strstr != null` |
| `index_of` | `(s: string, needle: string): int` | `strstr` ptr arithmetic; -1 if not found |
| `starts_with` | `(s: string, prefix: string): bool` | Turf char loop |
| `ends_with` | `(s: string, suffix: string): bool` | Length-offset check in Turf |
| `repeat` | `(s: string, n: int): string` | Turf accumulate via `+` |
| `count_char` | `(s: string, ch: int): int` | Turf loop |
| `substring` | `(s: string, start: int, end: int): string` | `strncpy` into `mem.alloc`'d buffer |
| `replace` | `(s: string, old: string, new: string): string` | Turf iterate + concat |
| `to_upper` | `(s: string): string` | Loop with `toupper` per char, build string |
| `to_lower` | `(s: string): string` | Loop with `tolower` per char, build string |
| `trim` | `(s: string): string` | Find first/last non-space index, `substring` |
| `length` | `(s: string): int` | `strlen(s)` — ergonomic alias, migration path for `lengthof` |

### 1C — Test Suite: `tests/13_string_stdlib/`

**19 files total (17 pass, 2 fail).**

| # | File | Purpose | Category |
|---|---|---|---|
| 01 | `01_int_to_string_return.tr` | Return `n to string` from a function — regression for the dangling-pointer bug | Bug fix |
| 02 | `02_double_to_string_return.tr` | Same for `double to string` | Bug fix |
| 03 | `03_string_comparison_ordering.tr` | `"apple" < "banana"`, `"z" > "a"`, mixed `<=`/`>=` | New feature |
| 04 | `04_str_contains.tr` | Basic + empty string edge cases | Stdlib |
| 05 | `05_str_index_of.tr` | Found at offset 6; not-found returns -1 | Stdlib |
| 06 | `06_str_starts_ends.tr` | Prefix == full string; suffix edge cases | Stdlib |
| 07 | `07_str_repeat.tr` | `str.repeat("ab", 50)` → verify `lengthof` == 100 | Memory stress |
| 08 | `08_str_replace.tr` | Replace all; non-overlapping; empty replacement | Stdlib |
| 09 | `09_str_upper_lower.tr` | `to_lower(to_upper(s)) == s` round-trip | Correctness |
| 10 | `10_str_trim.tr` | All-space, no-space, leading/trailing only | Edge cases |
| 11 | `11_dsa_palindrome_check.tr` | Two-pointer palindrome via char index + `int` comparison | **DSA: two-pointer** |
| 12 | `12_dsa_anagram_check.tr` | Anagram via `int[128]` char frequency array | **DSA: hash counting** |
| 13 | `13_dsa_count_words.tr` | Word count tokenizer on a space-separated string | **DSA: tokenizer** |
| 14 | `14_dsa_run_length_encode.tr` | RLE: `"aaabbc" → "3a2b1c"` using char access + `int to string` | **DSA: RLE** |
| 15 | `15_dsa_reverse_string_recursive.tr` | Recursive reversal via char concat — stresses call stack | **DSA: recursion** |
| 16 | `16_dsa_longest_common_prefix.tr` | LCP of a fixed array of strings | **DSA: trie baseline** |
| 17 | `17_stress_10k_concat.tr` | Concatenate 10 000 short strings in a loop, verify final length | Memory stress |
| F1 | `fail_string_ordering_non_string.tr` | `"hello" < 42` → type error | Negative |
| F2 | `fail_string_substr_oob.tr` | `str.substring("hi", 5, 10)` → runtime `assert` | Negative |

---

## Milestone 2 — Enums

### 2A — Language Design

**Phase 1 (this sprint): C-style integer-backed enums**
```turf
enum Direction { North, South, East, West }

mut d: Direction = Direction::North;

compare d {
    matches Direction::North:
        io.printline("going north");
    done
    matches Direction::South:
        io.printline("going south");
    done
    // COMPILE ERROR if East and West are missing and there is no otherwise:
}
```

**Phase 2 (future, no compiler changes needed for basics):**
```turf
enum Color : u8 { Red, Green, Blue }   // explicit underlying type
enum Result { Ok(int), Err(string) }    // algebraic (tagged union)
```

### 2B — Type System Changes

#### `TurfInt32` — New Singleton

Turf's native `int` is `i64`. Enums need a stable `i32` type. These must be distinct so `getLLVMType()` dispatches correctly, and so the pattern extends to `u8` etc. in Phase 2.

**Add to `include/Types.h`:**
```cpp
class Int32Type : public TurfType {
public:
    std::string getName() const override { return "i32"; }
    bool isInt()   const override { return false; }  // NOT Turf int (i64)
    bool isInt32() const override { return true; }
    bool isNumeric() const override { return true; }
    std::string getMangledName() const override { return "i32"; }
};

TurfTypePtr TurfInt32();  // singleton factory

// Add to TurfType base:
virtual bool isInt32() const { return false; }
virtual bool isEnum()  const { return false; }
```

#### `TurfEnumType` — New Class

```cpp
class TurfEnumType : public TurfType {
    std::string                   Name_;
    std::vector<std::string>      Variants_;         // ordered: ["North", "South", "East", "West"]
    std::map<std::string, int>    VariantValues_;    // {"North":0, "South":1, ...}
    std::shared_ptr<TurfType>     UnderlyingType_;   // default: TurfInt32()

public:
    TurfEnumType(std::string Name,
                 std::vector<std::string> Variants,
                 std::shared_ptr<TurfType> UnderlyingType = TurfInt32());

    std::string getName() const override { return Name_; }
    bool isEnum() const override { return true; }

    const TurfType*  getUnderlyingType()    const { return UnderlyingType_.get(); }
    TurfTypePtr      getUnderlyingTypePtr() const { return UnderlyingType_; }
    int  getVariantValue(const std::string& V) const;
    bool hasVariant(const std::string& V)      const;
    const std::vector<std::string>& getVariants() const { return Variants_; }
    int numVariants() const { return static_cast<int>(Variants_.size()); }

    std::string getMangledName() const override {
        return Name_ + "_enum_" + UnderlyingType_->getMangledName();
    }
};
```

**`getLLVMType()` additions:**
```cpp
if (auto* ET = dynamic_cast<const TurfEnumType*>(T))
    return LLVMTypeCache[Key] = getLLVMType(ET->getUnderlyingType()); // delegate, not hardcoded

if (T->isInt32())
    return LLVMTypeCache[Key] = Type::getInt32Ty(*TheContext);
```

### 2C — Lexer

**`include/Lexer.h`:** Add `TOK_ENUM = -81`.

**`src/Lexer.cpp`:** Add `{"enum", TOK_ENUM}` to the keywords map.

`TOK_SCOPE` (`::`) already exists at -74. No new token needed for variant access.

### 2D — New AST Nodes

#### `EnumDeclExprAST`

```cpp
class EnumDeclExprAST : public ExprAST {
    std::string              Name;
    std::vector<std::string> Variants;
    TurfTypePtr              UnderlyingType;  // nullptr = TurfInt32 default
    SourceLocation           Loc;
public:
    EnumDeclExprAST(SourceLocation Loc, std::string Name,
                    std::vector<std::string> Variants,
                    TurfTypePtr UnderlyingType = nullptr);
    const std::string&              getName()     const { return Name; }
    const std::vector<std::string>& getVariants() const { return Variants; }
    const SourceLocation&           getLoc()      const { return Loc; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitEnumDecl(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<EnumDeclExprAST>(Loc, Name, Variants, UnderlyingType);
    }
};
```

#### `EnumAccessExprAST`

Used for both `Direction::North` and the synthetic metadata variants (`COUNT`, `VALUES`, `NAMES`).

```cpp
class EnumAccessExprAST : public ExprAST {
    std::string    EnumName;  // "Direction"
    std::string    Variant;   // "North" | "COUNT" | "VALUES" | "NAMES"
    SourceLocation Loc;
public:
    EnumAccessExprAST(SourceLocation Loc, std::string EnumName, std::string Variant);
    const std::string& getEnumName() const { return EnumName; }
    const std::string& getVariant()  const { return Variant; }
    const SourceLocation& getLoc()   const { return Loc; }
    llvm::Value* codegen() override;
    void accept(ASTVisitor& V) override { V.visitEnumAccess(*this); }
    std::unique_ptr<ExprAST> clone() const override {
        return std::make_unique<EnumAccessExprAST>(Loc, EnumName, Variant);
    }
};
```

**`CompareExprAST` gets a new field:**
```cpp
bool SubjectIsString = false;  // existing
bool SubjectIsEnum   = false;  // NEW: set by TypeChecker::visitCompare
```

**`ASTVisitor.h` additions:**
```cpp
class EnumDeclExprAST;
class EnumAccessExprAST;
// in ASTVisitor base class:
virtual void visitEnumDecl(EnumDeclExprAST& Node) {}
virtual void visitEnumAccess(EnumAccessExprAST& Node) {}
```

### 2E — Parser

**New `ParseEnumDecl()`** — called when `CurTok == TOK_ENUM`:
1. Consume `TOK_ENUM`.
2. Read identifier → enum name.
3. *(Future Phase 2):* If `CurTok == TOK_COLON`, read underlying type via `ParseType()`.
4. Expect `{`.
5. Read comma-separated identifiers as variant names.
6. Expect `}`.
7. Return `EnumDeclExprAST`.

**`ParsePrimary()` — handle `Identifier::Identifier`:**

After reading an identifier, if the next token is `TOK_SCOPE` (`::`) and the token after is another identifier, create an `EnumAccessExprAST(X, Y)` unconditionally. The Resolver validates whether `X` is a known enum — the Parser does not need to know at parse time.

```
Direction :: North
^           ^
EnumName    Variant
```

**Top-level dispatch:** Add `case TOK_ENUM: return ParseEnumDecl();`.

### 2F — Resolver

**`ResolverSymbolTable.h` — new APIs:**
```cpp
void registerEnumType(const std::string& Name, std::shared_ptr<TurfEnumType> ET);
std::shared_ptr<TurfEnumType> lookupEnumType(const std::string& Name) const;
bool isEnumName(const std::string& Name) const;
```

**Pass 1 — `hoistTopLevel()`:** For each `EnumDeclExprAST`, build a `TurfEnumType` and register it in both `RST_` and `GlobalSymbolTable`.

**Pass 2 — `visitEnumAccess()`:**
1. Look up `EnumName`. Not found → `ReferenceError` + `SuggestionIndex` suggestion.
2. Found. Variant not in enum AND variant ≠ `"COUNT"`, `"VALUES"`, `"NAMES"` → `ReferenceError("'Direction' has no variant 'Norht'. Did you mean 'North'?")`.
3. Otherwise → valid. TypeChecker handles the type.

**`visitVarDecl()` extension:** When `DeclType` is `NamedType("Direction")` and `RST_.isEnumName("Direction")`, replace with the resolved `TurfEnumType`. Same pattern as struct resolution.

### 2G — TypeChecker

**New visitor methods:**
```cpp
void visitEnumDecl(EnumDeclExprAST& Node) override;
void visitEnumAccess(EnumAccessExprAST& Node) override;
```

**`visitEnumDecl`:** `setType(&Node, TurfVoid())`.

**`visitEnumAccess`:**
- Look up `TurfEnumType` via `GlobalSymbolTable->LookupType(EnumName)`.
- `"COUNT"` → `setType(&Node, TurfInt())` (returns an integer).
- `"VALUES"` → `setType(&Node, TurfPointerType(enumTypePtr))` (returns `Direction*`).
- `"NAMES"` → `setType(&Node, TurfPointerType(TurfString()))` (returns `string*`).
- Valid variant → `setType(&Node, enumTypePtr)`.
- Unknown → error + suggestion.

**`typesCompatible()` changes:**
```cpp
// Enums are only compatible with the same named enum. NOT implicitly int.
if (From->isEnum() || To->isEnum()) {
    if (!From || !To) return true;  // permissive on unknowns
    return From->getName() == To->getName();
}
```

**`inferRelationalType()` changes:**
- `==` / `!=` between same-enum values → `TurfBool()`.
- `<`, `>`, `<=`, `>=` between enum values → **type error**: `"Cannot use ordering operators on enum type 'Direction'. Cast to int first: (d to int) < (e to int)"`.

**`visitCompare()` — exhaustiveness check (D4):**

```cpp
// After type-checking the subject:
bool IsEnum = SubjectTy && SubjectTy->isEnum();
Node.SubjectIsEnum = IsEnum;

const TurfEnumType* ET = IsEnum
    ? dynamic_cast<const TurfEnumType*>(SubjectTy.get()) : nullptr;

std::set<std::string> CoveredVariants;

for (auto& Arm : Node.getArmsMut()) {
    for (auto& PatExpr : Arm.Values) {
        check(PatExpr.get());
        if (ET)
            if (auto* EA = dynamic_cast<EnumAccessExprAST*>(PatExpr.get()))
                CoveredVariants.insert(EA->getVariant());
    }
    check(Arm.Body.get());
}
if (Node.getOtherBody())
    check(Node.getOtherBodyMut());

// Exhaustiveness gate
if (ET && !Node.getOtherBody()) {
    std::vector<std::string> Missing;
    for (const auto& V : ET->getVariants())
        if (!CoveredVariants.count(V))
            Missing.push_back(V);

    if (!Missing.empty()) {
        std::string Msg = "Non-exhaustive enum comparison on '" + ET->getName()
                        + "'.\nMissing variants:";
        for (const auto& M : Missing)
            Msg += "\n    " + ET->getName() + "::" + M;
        Msg += "\nHint: Add 'otherwise:' to handle remaining variants.";
        reportError(Node.getLoc(), Msg);
    }
}
```

**Error example output:**
```
── error at line 12, col 5

  Non-exhaustive enum comparison on 'Direction'.
  Missing variants:
      Direction::East
      Direction::West
  Hint: Add 'otherwise:' to handle remaining variants.

  12 │ compare d {
     │ ^ Here!
```

### 2H — Codegen

**`EnumDeclExprAST::codegen()`:**
- Register `TurfEnumType` in `GlobalSymbolTable`.
- Emit **zero LLVM IR** (enums are compile-time metadata only).
- Return `ConstantInt::get(Int64Ty, 0)` sentinel.

**`EnumAccessExprAST::codegen()`:**
```cpp
Value* EnumAccessExprAST::codegen() {
    auto EnumTy = GlobalSymbolTable->LookupType(EnumName);
    auto* ET = dynamic_cast<const TurfEnumType*>(EnumTy.get());
    llvm::Type* LLVMTy = getLLVMType(ET->getUnderlyingType()); // NOT hardcoded i32
    
    if (Variant == "COUNT") {
        return ConstantInt::get(Int64Ty, ET->numVariants());
    }
    if (Variant == "VALUES") {
        // Emit global constant array of underlying type, return GetElementPtr to first element
    }
    if (Variant == "NAMES") {
        // Emit global constant array of string pointers, return GetElementPtr
    }
    
    return ConstantInt::get(LLVMTy, ET->getVariantValue(Variant));
}
```

**`BinaryExprAST::codegen()` — enum equality:**
```cpp
// When both sides are TurfEnumType (same name):
if (LHSTyAST && LHSTyAST->isEnum() && RHSTyAST && RHSTyAST->isEnum()) {
    if (Op == TOK_EQ)  return Builder->CreateICmpEQ(L, R, "enumeq");
    if (Op == TOK_NEQ) return Builder->CreateICmpNE(L, R, "enumne");
}
```

**`CastExprAST::codegen()` additions:**
```cpp
// Enum → int (sign-extend underlying i32 to Turf i64)
if (SrcTy->isEnum() && DstName == "int")
    return Builder->CreateSExt(Val, Type::getInt64Ty(*TheContext), "enumtoint");

// int → Enum (truncate i64 to underlying type + runtime bounds check)
if (SrcName == "int" && DstTy && DstTy->isEnum()) {
    auto* ET = dynamic_cast<const TurfEnumType*>(DstTy);
    llvm::Type* UndTy = getLLVMType(ET->getUnderlyingType());
    Value* Truncated = Builder->CreateTrunc(Val, UndTy, "intToenum");
    // emit: if (Truncated < 0 || Truncated >= numVariants) fail(...)
    EmitRuntimeBoundsCheck(Builder->CreateSExt(Truncated, Int64Ty),
                           ET->numVariants(), "enum cast", Loc);
    return Truncated;
}

// Enum → string
if (SrcTy->isEnum() && DstName == "string") {
    // Emits LLVM `switch` on Val, with each case returning the corresponding global string constant
}

// string → Enum
if (SrcTy->isString() && DstTy && DstTy->isEnum()) {
    // Emits sequence of `strcmp` block jumps, returns Enum value if matched, else emits runtime `fail`
}
```

**`CompareExprAST::codegen()` — enum subject:** When `SubjectIsEnum = true`, the subject value is already the underlying type (e.g. `i32`). Each pattern `EnumAccessExprAST::codegen()` returns a constant of the same underlying type. The **existing integer-switch codegen path works without structural change** — the LLVM `switch` instruction naturally accepts any integer type.

### 2I — Built-in Enum Reflection (Zero Intrinsics)

By shifting reflection from stdlib intrinsics to **native language casts** and **pseudo-variants**, we perfectly eliminate the need for `@intrinsic` backdoors while still keeping enums as pure metadata (no unused C functions generated).

| Operation | Syntax | Implementation |
|---|---|---|
| `enum_to_string(d)` | `d to string` | Standard `CastExprAST` emitting a `switch` instruction mapping the value to global string constants. |
| `enum_from_string(s)` | `s to Direction` | Standard `CastExprAST` emitting a `strcmp` cascade, terminating with a runtime `fail` if not found. |
| `enum_count()` | `Direction::COUNT` | Resolves as a compile-time `ConstantInt` via `EnumAccessExprAST`. |
| `enum_values()` | `Direction::VALUES` | Returns a pointer to a lazily emitted global array of values (`Direction*`). |
| `enum_names()` | `Direction::NAMES` | Returns a pointer to a lazily emitted global array of names (`string*`). |

### 2J — Test Suite: `tests/14_enums/`

**23 files total (18 pass, 5 fail).**

| # | File | What it tests | Category |
|---|---|---|---|
| 01 | `01_enum_decl_basic.tr` | Declare, assign, basic `compare` | Sanity |
| 02 | `02_enum_equality.tr` | `d1 == d2`, `d1 != d2` | Equality |
| 03 | `03_enum_compare_exhaustive.tr` | All variants covered + otherwise — no error | Exhaustiveness: pass |
| 04 | `04_enum_function_param.tr` | Pass enum to function, return from function | Type passing |
| 05 | `05_enum_array.tr` | `Direction[4]` array of enums | Array integration |
| 06 | `06_enum_in_struct.tr` | Struct with an enum field | Struct integration |
| 07 | `07_enum_cast_to_int.tr` | `d to int` correct value; `(0 to Direction)` | Cast path |
| 08 | `08_enum_count.tr` | `Direction::COUNT == 4` | D5: metadata |
| 09 | `09_enum_to_string.tr` | `(Direction::North to string) == "North"` | D5: metadata |
| 10 | `10_enum_from_string.tr` | `("North" to Direction) == Direction::North` | D5: metadata |
| 11 | `11_enum_values.tr` | Iterate via `Direction::VALUES` | D5: metadata |
| 12 | `12_enum_loop_all_variants.tr` | Cast `0..COUNT-1 to Direction`, compare each | Exhaustive iteration |
| 13 | `13_dsa_state_machine.tr` | Traffic-light FSM: `State` enum cycles Red→Green→Yellow | **DSA: FSM** |
| 14 | `14_dsa_card_suit.tr` | `Suit` + `Rank` enums, count valid 5-card poker hands | **DSA: combinatorics** |
| 15 | `15_dsa_direction_navigation.tr` | BFS grid nav on a 5×5 grid using `Direction` enum | **DSA: BFS** |
| 16 | `16_dsa_priority_levels.tr` | Priority-queue simulation with `Priority` enum | **DSA: heap sim** |
| 17 | `17_stress_100_variants.tr` | 100-variant enum; loop all via cast, verify `to string` | Stress: large switch |
| 18 | `18_stress_enum_in_nested_struct.tr` | Nested structs with enum fields; serialize all | Stress: composite |
| F1 | `fail_enum_wrong_type.tr` | `mut d: Direction = 3` — no implicit int→enum | Negative: type safety |
| F2 | `fail_enum_wrong_variant.tr` | `Direction::Norht` — error + "did you mean North?" | Negative: resolver |
| F3 | `fail_enum_ordering.tr` | `Direction::North < Direction::South` — type error | Negative: ordering |
| F4 | `fail_enum_cross_assign.tr` | `mut d: Direction = Color::Red` — wrong enum | Negative: mismatch |
| F5 | `fail_enum_non_exhaustive.tr` | `compare` missing 2 variants, no `otherwise` | Negative: exhaustiveness |

---

## Complete File Change List

### Milestone 0 (Stdlib Rewrite)

| Action | File | What changes |
|---|---|---|
| MODIFY | `lib/std/mem.tr` | Full rewrite: 3 stubs → `extern fn` + 5 Turf helpers |
| MODIFY | `lib/std/math.tr` | Full rewrite: 10 stubs → `extern fn` + Turf impls + 8 new functions |
| MODIFY | `lib/std/collections.tr` | Add `import mem;`; replace bare allocator calls |
| MODIFY | `src/Builtins.cpp` | Remove 6 `CodegenIntrinsic` cases; add explanatory comments |

### Milestone 1 (Strings)

| Action | File | What changes |
|---|---|---|
| MODIFY | `src/Codegen.cpp` | Fix int/double→string alloca bug; add `EmitStrcmpRelational`; wire string ordering in `BinaryExprAST::codegen()` |
| MODIFY | `src/TypeChecker.cpp` | `inferRelationalType`: allow string ordering |
| NEW | `lib/std/string.tr` | 12 string functions |
| NEW | `tests/13_string_stdlib/` | 19 test files |

### Milestone 2 (Enums)

| Action | File | What changes |
|---|---|---|
| MODIFY | `include/Lexer.h` | Add `TOK_ENUM = -81` |
| MODIFY | `src/Lexer.cpp` | Add `{"enum", TOK_ENUM}` keyword |
| MODIFY | `include/Types.h` | Add `isEnum()`, `isInt32()` to base; add `Int32Type`, `TurfInt32()`, `TurfEnumType` |
| MODIFY | `src/Types.cpp` | Implement `TurfInt32()` singleton; `TurfEnumType` constructor + methods |
| MODIFY | `include/AST.h` | Add `EnumDeclExprAST`, `EnumAccessExprAST`; add `SubjectIsEnum` to `CompareExprAST` |
| MODIFY | `include/ASTVisitor.h` | Forward-declare new nodes; add `visitEnumDecl`, `visitEnumAccess` |
| MODIFY | `src/Parser.cpp` | Add `ParseEnumDecl()`; handle `X::Y` in `ParsePrimary()`; wire `TOK_ENUM` |
| MODIFY | `include/ResolverSymbolTable.h` | Add `registerEnumType`, `lookupEnumType`, `isEnumName` |
| MODIFY | `src/Resolver.cpp` | Hoist enums in Pass 1; validate in `visitEnumAccess`; resolve type in `visitVarDecl` |
| MODIFY | `include/TypeChecker.h` | Add `visitEnumDecl`, `visitEnumAccess` declarations |
| MODIFY | `src/TypeChecker.cpp` | Implement both visitors; update `typesCompatible`, `inferRelationalType`, `visitCompare` (exhaustiveness), `visitVarDecl` |
| MODIFY | `src/Codegen.cpp` | `getLLVMType`: enum + i32 branches; `getExprType`: enum access; implement `EnumDeclExprAST::codegen()` and `EnumAccessExprAST::codegen()`; enum `==`/`!=`; enum ↔ int cast; string ↔ enum cast. |
| NEW | `tests/14_enums/` | 23 test files |

---

## Scalability Architecture

### Strings

1. **Stdlib-first:** Compiler handles exactly 6 string operations (`+`, `==`/`!=`/`<`/`>`/`<=`/`>=`, `to string`, `from string`, `s[i]`, `lengthof`). Everything else lives in `string.tr`.
2. **`extern fn` bridge:** New C-backed string operations are added to `string.tr` only. Zero compiler C++ changes.
3. **UFCS roadmap:** Future `s.contains("x")` resolves to `str.contains(s, "x")` automatically.
4. **Unicode:** `unicode.compare(...)` / `unicode.collate(...)` as a separate module — byte-by-byte is the default forever (D2).
5. **`str.length(s)` replaces `lengthof`:** `string.tr` exposes `extern fn strlen` → `str.length(s)`. `lengthof` stays as deprecated alias.

### Enums

1. **Underlying type parameterized (D1):** `enum Color : u8` works in Phase 2 by adding `u8` to `Types.h` and one branch in `getLLVMType()`. Zero changes to enum-specific parser, resolver, typechecker, or codegen logic.
2. **Exhaustiveness from day one (D4):** Non-exhaustive `compare` is a compile error in Phase 1. The exhaustiveness check is already wired.
3. **Pure metadata through syntax (D5):** Enums are natively castable to strings via the compiler, completely avoiding standard library bloat, C-function generation, and `@intrinsic` workarounds. 
4. **Phase 2: Algebraic enums:** `TurfEnumType` gains `std::vector<TurfTypePtr> VariantPayload`. The integer-backed path is completely untouched.
5. **Phase 2: Enum methods:** `fn Direction.opposite(): Direction` via the existing struct-method mechanism.
6. **Phase 2: Iteration:** `Direction::VALUES` array provides robust foundation for `for val in Direction::VALUES {}`.

---

## Verification Plan

### Build
```bash
cd build && cmake --build . -j$(nproc) 2>&1 | tail -30
```
Expected: zero errors, zero warnings introduced.

### Full Test Suite
```bash
cd tests && ./runner.sh 2>&1 | tee test_results.log
grep -E "PASS|FAIL" test_results.log | tail -40
```
Expected:
- All existing tests (`01_*` through `12_*`) pass — zero regressions.
- All `tests/13_string_stdlib/*.tr` (non-fail) pass.
- All `tests/14_enums/*.tr` (non-fail) pass.
- All `fail_*.tr` exit non-zero with a relevant error message.

### Spot Checks
```bash
# Bug regression: dangling pointer fix
./turf tests/13_string_stdlib/01_int_to_string_return.tr
# Expected: prints "42", no crash

# Exhaustiveness error
./turf tests/14_enums/fail_enum_non_exhaustive.tr
# Expected: error listing Direction::East and Direction::West

# Enum reflection
./turf tests/14_enums/09_enum_to_string.tr
# Expected: prints "North" via cast

# DSA boss fight
./turf tests/13_string_stdlib/14_dsa_run_length_encode.tr
./turf tests/14_enums/13_dsa_state_machine.tr
./turf tests/14_enums/15_dsa_direction_navigation.tr
```
