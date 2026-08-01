# User-Defined `error` Types, `!T`, and `recover`

Technical reference for Turflang's recoverable-error system, as implemented. This document defines **what a Turflang programmer observes** (Part 1) and **how the compiler implements it, pass by pass** (Part 2), with file:line references throughout so the two halves can be cross-checked against the real source.

The original design brief is `docs/planning/Errors.md` — read that first for the feature's motivation and worked examples. This document is the precise technical reference for the same feature as actually built: where `Errors.md` describes intent with prose examples, this document defines exact contracts (what type each construct has, when each diagnostic fires, what each pass does) and traces the implementation.

**Scope note**: error variants are bare names only, carrying no payload fields (no `FileNotFound(path: string)`-style data) — see Errors.md's own note and §5.1 below for why this is a deliberate, not-yet-filled gap, not an oversight.

---

## Part 1 — Language semantics

### 1.1 Declaring an error type

```turf
error IOError {
    FileNotFound,
    PermissionDenied,
    InvalidEncoding
}
```

Structurally identical to a C-style `enum` — a fixed set of bare variant names, no payload, no generics. `IOError` becomes a distinct type (`ErrorType`, backed by `TurfUserErrorType`, §2.4), validated in the pipeline and represented as an AST node exactly like a `struct`/`enum` declaration.

**Bare form**, sugar for a single implicit variant:

```turf
error GeneralError;
```

Equivalent to `error GeneralError { DEFAULT }`. Constructed as `GeneralError::DEFAULT`. See §1.7 for the one restriction bare-form error types carry.

### 1.2 Referencing a variant

```turf
MathError::DivisionByZero
```

A bare, **non-callable** reference — identical in spirit to `Color::Red` for a plain `enum`. `MathError::DivisionByZero()` (call syntax, even with zero arguments) is rejected the same way `Color::Red()` already is: an error variant is a value, not a function.

```turf
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}
```

### 1.3 `!T`: fallible return types

`!T` denotes a function that returns either a value of type `T` (success) or a value of **any** declared error type (failure). `T` may be any type — a primitive, a user-defined struct, a generic function's own type parameter, or `void`.

This is an **open union**, unlike e.g. Rust's `Result<T, E>`: a `!T`-declared function's signature does not name which error type(s) it can fail with. Any function returning `!T` may fail with any error type visible to it. There is no way to restrict a `!T` function's signature to a specific closed set of error types.

A `!T` value cannot be used as a plain `T` — it must go through `recover` (§1.4) at the call site:

```turf
mut x: int = divide(10, 2);              // ERROR: divide(10, 2) is !int, not int
mut x: int = divide(10, 2) recover produce 0;  // OK
```

### 1.4 `recover`: the three forms

This is the section that matters most for avoiding confusion — read it carefully before using any of the three forms.

Every `recover` expression wraps a `!T` expression (the **wrapped expression**) and evaluates it, branching on whether it succeeded or failed. What happens next depends entirely on which form is used:

| Form | On success | On failure | Overall expression's type |
|---|---|---|---|
| `expr recover produce value` | evaluates to the wrapped expression's success value | evaluates to `value` | `T` (both branches must agree) |
| `expr recover run { BLOCK }` | evaluates to the wrapped expression's success value; `BLOCK` never runs | `BLOCK` runs instead; must diverge (§1.5) unless `T` is `void` | `T` |
| `expr recover as err run { BLOCK }` | same as `run` | same as `run`, plus `err` is bound inside `BLOCK` | `T` |

**The recover expression's own type is always `T` — never `!T`.** This holds for all three forms, in every worked example in `Errors.md`; a `!T` annotation on the variable receiving a `recover` result would be a mistake, not a second valid spelling.

#### 1.4.1 `recover produce value`

The only form that actually **produces a substitute value of type T** on failure. `value` is checked against `T` exactly — the same way any other assignment/return is type-checked. This is the form to reach for whenever you want a guaranteed, well-typed fallback and want execution to continue normally either way:

```turf
mut x: int = divide(10, 0) recover produce 0;   // x is always a real int, either way
mut p: Point = makePoint(false) recover produce Point::new(0, 0);  // same, for a struct
```

Because `value` must genuinely be a `T`, `recover produce -1` against a `!Point`-typed wrapped expression is rejected — `-1` is not a `Point` (`fail_recover_produce_wrong_type_struct.tr`).

#### 1.4.2 `recover run { BLOCK }` / `recover as err run { BLOCK }`

These forms **do not produce a value from the recover expression on failure at all.** `BLOCK` is not "the fallback value" — it is ordinary code, useful for a side effect (logging, cleanup, `sys::exit`), that runs *instead of* producing anything. Because there is no value to give the enclosing statement on the failure path, `BLOCK` must **diverge**: every path through it must end in `return`, `break`, or `continue` (§1.5) — the one exception being when `T` itself is `void`, where "no value" is trivially fine (§1.6).

**A `return`/`break`/`continue` inside `BLOCK` operates on the function/loop *containing* the `recover` expression — not on the recover expression itself, and not checked against `T` at all.** This is the single most common point of confusion, so to be explicit:

```turf
fn main(): int {
    mut p: Point = makePoint(true) recover run {
        return -1;
    };
    // ...
}
```

Here, `return -1;` returns from `main()` (declared `int`), exiting the whole function immediately — it is checked against `main`'s own return type, **not** against `Point`. The assignment to `p` is abandoned entirely on this path; `p` never receives any value, real or placeholder, when the failure branch is taken. `main()` simply never reaches the line after the `recover` expression. `-1` is `main`'s exit code, not a `Point`.

If what you actually want is "`p` should hold a real `Point` even on failure," that is exactly what `recover produce` is for (§1.4.1) — `recover run`/`recover as err run` answer a different question ("abandon this and do something else"), not "give me a fallback `T`."

#### 1.4.3 `recover as err run { BLOCK }`

Identical to `recover run`, with one addition: the bound name (conventionally `err`) is a variable, scoped to `BLOCK`, holding the specific error value that occurred. Its declared type is `AnyErrorType` (§2.4) — a type-erased handle, since `!T`'s open-union nature (§1.3) means the concrete error type isn't statically known at this point.

`err` supports `==`/`!=` against a bare variant reference (§1.2) to discriminate which error occurred:

```turf
mut x: int = divide(10, 0) recover as err run {
    if err == MathError::DivisionByZero {
        io::printline("division by zero");
    }
    return -1;
};
```

Equality compares **variant identity only** — since a variant carries no payload (the scope note at the top of this document), this comparison is exact and total; there is no partial/field-level comparison to worry about.

### 1.5 The divergence rule, precisely

For `recover run`/`recover as err run` with a non-`void` `T`: every control-flow path through `BLOCK` must end in `return`, `break`, or `continue`. This is checked structurally (not via full CFG reachability) — see §2.7 for exactly what is and isn't recognized as diverging, and why.

### 1.6 The `void` exception

When `T` is `void`, a `recover run`/`recover as err run` block is **not** required to diverge — it may simply run to completion as an ordinary side effect:

```turf
fn writeFile(path: string): !void { ... }

writeFile("a.txt") recover run {
    io::printline("Write failed.");
};
```

There is no value being produced either way (`T` is `void`), so falling off the end of `BLOCK` is unproblematic. `recover produce` is *not* valid when `T` is `void` — there is no value of type `void` to produce (`fail_recover_produce_on_void_success.tr`).

### 1.7 The bare-error restriction on `recover as err`

`recover as err run { ... }` is rejected at compile time when **every** error type the wrapped call can possibly fail with is bare-form (`error Name;`, §1.1). A bare error type has exactly one variant (`DEFAULT`) — there is nothing for `err == ...` to usefully discriminate, so binding it is flagged as pointless rather than silently allowed:

```turf
error GeneralError;

fn maybeFail(x: int): !int {
    if (x < 0) { return GeneralError::DEFAULT; }
    return x;
}

maybeFail(-1) recover as err run { ... };  // BareErrorAsBindingError
```

`recover produce`/`recover run` remain valid on a bare-error-only call — they never need to discriminate. See §2.6 for how "every error type this call can fail with" is actually determined (a static scan of the callee's body, not a declared property of its signature — recall §1.3, `!T` names no specific error types).

### 1.8 The "never returns an error" warning

A `!T`-declared function is *permitted*, not *obliged*, to fail — `!T` widens what a function may return, it doesn't require it to exercise that width. If the compiler's analysis (§2.6) finds no path in a `!T` function's body that actually returns an error-type value, it emits `NeverReturnsErrorWarning` — a warning, not an error, suggesting the `!` may be unnecessary.

### 1.9 Diagnostic reference

| Diagnostic | Raised when |
|---|---|
| `TypeError` ("is an error variant, not a function") | An error variant is called, even with zero arguments |
| `TypeError` ("has fields and must be constructed...") | Historical message text; no variant has fields anymore, so this specific wording is currently unreachable — see §5.1 |
| `LogErrorAt` ("Error type '...' has no variant '...'") | A reference (bare or as a call target) to an unknown variant name, with Damerau-Levenshtein suggestions |
| `DuplicateTypeNameError` | Two `error` (or `error`/`struct`/`enum`) declarations share one name |
| `TypeError` ("'recover' can only be used on a fallible...") | `recover` applied to an expression whose type is not `!T` |
| `TypeError` ("doesn't match this call's success type") | `recover produce value` where `value`'s type isn't exactly `T` |
| `TypeError` ("must end every path with 'return'") | `recover run`/`recover as err run` on a non-`void` `T` where the block doesn't diverge (§1.5) |
| `BareErrorAsBindingError` | `recover as err run` where every possible error is bare-form (§1.7) |
| `NeverReturnsErrorWarning` | A `!T` function whose body never actually returns an error-type value (§1.8) |
| Generic "Return type mismatch" / "Cannot initialise..." | `!T` used where `T` (or a compatible type) was expected, or vice versa, anywhere a return/assignment type-check already applies |

---

## Part 2 — Internal architecture (compiler implementation)

### 2.1 Pipeline recap

```
source.tr
   │
   ▼
 Lexer  ──tokens──▶  Parser  ──AST──▶  Resolver (2 passes)  ──annotated AST──▶  TypeChecker  ──▶  Codegen (LLVM IR)
```

One wrinkle relevant to Codegen specifically (§2.8): `main.cpp` runs a **pre-pass** (its own Resolver + TypeChecker + Codegen sequence, sub-passes 2a/2b/2c/3a/3b) on function bodies before the *mandatory* pass that everything else in the pipeline treats as authoritative. Both the pre-pass and the mandatory pass run a full Resolver→TypeChecker sequence on their *own*, separately-parsed AST instance — so information computed by TypeChecker on one instance is not visible to Codegen running against the other. This is why Codegen (§2.8) never reads TypeChecker's inferred-type map directly, and instead relies on structural information (`ResolvedKind`, `ResolvedSuccessType`) the Resolver/TypeChecker attach directly to AST nodes, which each pass's own local Resolver/TypeChecker run correctly populates on its own tree.

### 2.2 Lexer: five new keywords

`include/Lexer.h:227-231`:

```cpp
TOK_ERROR   = -88, // `error`
TOK_RECOVER = -89, // `recover`
TOK_PRODUCE = -90, // `produce`
TOK_RUN     = -91, // `run`
TOK_AS      = -92, // `as`
```

`!` is not a new token — it reuses the existing bare `!` character token (already used for unary logical-not in expression position, `src/Parser.cpp:1153`). No ambiguity: `ParseType()` (§2.5) is a separate function from expression parsing, so a leading `!` in type position can never be confused with unary-not.

### 2.3 AST representations

**`ErrorDeclExprAST`** (`include/AST.h:989`) — the declaration node, parallel to `EnumDeclExprAST`:

```cpp
class ErrorDeclExprAST : public ExprAST {
    std::string              Name;
    std::vector<std::string> Variants;  // bare names only, no payload
    bool                      IsBareForm;
};
```

**`ScopeAccessKind::ErrorVariantRef`** (`include/AST.h:1033`) — added to the existing `ScopeAccessKind` enum (the same node type that already represents `EnumName::Variant`, `Module::func`, `StructName::method`, etc.). Set on a `ScopeAccessExprAST` whenever `Left` names a declared error type; always non-callable, mirroring `EnumVariant`'s existing non-callable rule. Kept as its own enum value (rather than reusing `EnumVariant`) because error values lower differently in Codegen (a tag string, not an integer ordinal, §2.8) and participate in `!T`/`recover`/`AnyErrorType` comparisons that plain enums don't.

**`RecoverKind`** (`include/AST.h:1111`):

```cpp
enum class RecoverKind { Produce, Run, AsRun };
```

**`RecoverExprAST`** (`include/AST.h:1123`):

```cpp
class RecoverExprAST : public ExprAST {
    std::unique_ptr<ExprAST> Wrapped;      // the !T expression
    RecoverKind              Kind;
    std::unique_ptr<ExprAST> ProduceExpr;  // Kind == Produce
    std::string              BoundName;    // Kind == AsRun
    std::unique_ptr<ExprAST> Block;        // Kind == Run / AsRun (a BlockExprAST)
public:
    TurfTypePtr ResolvedSuccessType;        // set by TypeChecker, read by Codegen (§2.8)
};
```

`Block` reuses the ordinary `BlockExprAST` every `{ ... }` body in the language already uses (if/while/function bodies) — no new block-representation node was needed.

`ErrorType::Variant` construction needed **no new expression node at all**: `Left::Right(args)` already parses generically as `FuncCallExprAST(Callee=ScopeAccessExprAST(...))` regardless of what `Left` is (the same mechanism `StructName::new(...)` uses) — but since a variant is never callable (§1.2), this shape is only ever reached to be *rejected* (§2.6), not to construct anything.

### 2.4 Types

`include/Types.h`. Two collision-risk notes are load-bearing here: neither new type class is named anything close to `TurfError` (`include/Errors.h`), the compiler's own internal diagnostic base class — an unrelated concept that happens to share the English word "error."

**`TurfUserErrorType`** (`Types.h:561`) — the type of a declared `error Name { ... }`:

```cpp
class TurfUserErrorType : public TurfType {
    std::string               Name_;
    std::vector<std::string> Variants_;  // bare names, exactly like TurfEnumType
    bool                      IsBareForm_;
public:
    bool isUserError() const override { return true; }
    bool isBareForm() const { return IsBareForm_; }
    bool hasVariant(const std::string&) const;
};
```

**`TurfErrorUnionType`** (`Types.h:605`) — the type of `!T`:

```cpp
class TurfErrorUnionType : public TurfType {
    TurfTypePtr SuccessType_;
public:
    bool isErrorUnion() const override { return true; }
    const TurfType* getSuccessType() const;
};
```

Deliberately distinct from `TurfUserErrorType`: a `!T` value doesn't name one specific error type (§1.3) — `TurfErrorUnionType` only ever wraps the success type `T`; which concrete error type actually occurred, if any, is a runtime fact (§2.8), not part of the static type.

**`AnyErrorType`** (`Types.h:253`) — the type of `err` (§1.4.3):

```cpp
class AnyErrorType : public TurfType {
public:
    bool isAnyError() const override { return true; }
};
```

A type-erased stand-in: `err`'s real concrete error type isn't knowable at compile time (§1.3), so `AnyErrorType` is what lets `==`/`!=` against a real `TurfUserErrorType` value type-check at all (`TypeChecker::inferRelationalType`, §2.7) despite the two sides never being the "same" static type by name.

### 2.5 Parser

**`ParseErrorDecl()`** (`src/Parser.cpp:2077`) — modeled directly on `ParseEnumDecl()`'s variant-list loop (duplicate-variant rejection included); the bare form (`error Name;`) desugars to a single `"DEFAULT"` variant at parse time.

One subtlety in how the bare form's trailing `;` is handled: every top-level/block/module/`compare`-arm statement loop in this file treats a *braced* error declaration the same way it already treats `struct`/`enum` — a construct that must **not** be followed by a `;`. The bare form therefore self-consumes its own `;` inside `ParseErrorDecl()` itself (mirroring `export`'s identical self-terminating convention), so that by the time control returns to any of those loops, there is no dangling `;` left for them to reject.

**`ParseType()`**'s `!T` branch (`src/Parser.cpp:209`) — checked before every other branch in the function:

```cpp
if (CurTok == '!') {
    getNextToken();
    TurfTypePtr Success = ParseType();
    return std::make_shared<TurfErrorUnionType>(Success);
}
```

**`recover` parsing** (`src/Parser.cpp:1082`, inside `ParseExpression()`) — bound to the whole expression parsed so far, the same "loosest binding" position as the existing `give`/`otherwise` ternary. Dispatches on the keyword following `recover` (`produce` / `run` / `as <name> run`) to build the corresponding `RecoverExprAST` variant; a `run`/`as run` block is parsed via the ordinary `ParseBlock()`.

`IOError::FileNotFound` construction/reference needed **no parser changes** — confirmed by the generic `Left::Right(args)` handling already noted in §2.3.

### 2.6 Resolver

**Registry** (`include/ResolverSymbolTable.h`) — `registerErrorType`/`isErrorTypeName`/`lookupErrorType`/`getAllErrorTypeNames`, a registry parallel in shape to the existing enum registry, populated during Pass 1 hoisting (`hoistTopLevel`/`hoistModuleBody`, mirroring the enum branches there exactly, including inside imported module bodies).

**`visitFuncCall()`**'s error-type branch (`src/Resolver.cpp:1212`) — when `Left` names a known error type, `ResolvedKind = ErrorVariantRef` and a `TypeError` is raised unconditionally: an error variant is never callable, mirroring the pre-existing `EnumVariant` rejection immediately above it in the same function.

**`visitScopeAccess()`**'s error-type branch (`src/Resolver.cpp:1815`) — the bare (non-call) reference case: `ResolvedKind = ErrorVariantRef`, validates the variant exists (with suggestions on a near-miss), otherwise a no-op — there are no fields to validate anymore (§5.1).

**`CollectReturnedErrorTypeNames()`** (`src/Resolver.cpp:208`) — a conservative, non-interprocedural structural scan of a function body's `return` statements for a bare `ErrorType::Variant` shape, collecting the left-hand name. Mirrors `MutatesSelfField()`'s exact recursion shape (only descends through `Block`/`If`/`While`/`For`). Feeds `ResolverFunctionSymbol::PossibleErrorTypeNames` (`ResolverSymbolTable.h:251`), computed once inside `buildFunctionSymbol()`.

This list is **deliberately unfiltered** at collection time: `buildFunctionSymbol()` runs during Pass 1 hoisting, when a later-declared error type might not be registered yet, and a `Left::Right` shape could just as easily name a struct/enum/module reference that has nothing to do with errors. Every consumer filters it through `isErrorTypeName()` at its own, later point (Pass 2, by which point every error type in the program is guaranteed registered) before drawing any conclusion from it. Two consumers:

1. **`NeverReturnsErrorWarning`** (§1.8) — `Resolver::visitFuncDef()`, after the body is fully resolved: if the function's declared return type `isErrorUnion()` and the filtered `PossibleErrorTypeNames` is empty, emit the warning. Deduplicated by source location (a process-lifetime static set) — this compiler resolves every function body **twice** (once in `main.cpp`'s pre-pass, once again in its main pass, a separate re-parse used only for linting, §2.1) — every other existing warning happens to dodge this by living in Codegen instead (which only runs once); this is the first Resolver-level warning to actually hit the duplication.

2. **The bare-error restriction** (§1.7) — `Resolver::visitRecoverExpr()` (`src/Resolver.cpp:1720`), only for `Kind == AsRun`: reads the wrapped call's resolved candidate's `PossibleErrorTypeNames`, filters through `isErrorTypeName()`, and — if the resulting set is non-empty and *every* entry is bare-form (`isBareForm()`) — raises `BareErrorAsBindingError`. If the callee is ambiguous (an unresolved overload, no unique `ResolvedCallTarget`) or its filtered set is empty, the check is skipped rather than guessed at — conservative under-flagging, the same posture `MutatesSelfField()` already takes for a hard-error gate.

**`visitRecoverExpr()`**'s dispatch, in full: resolves the wrapped expression first (populating its callee's `ResolvedCandidates`, needed above). For `Produce`, resolves the produce expression in the current scope. For `Run`/`AsRun`, the block gets its own scope (mirroring if/while/function bodies); `AsRun` additionally declares the bound name as an `AnyErrorType`-typed variable *before* resolving the block, so ordinary variable lookup inside it (including the equality check in §1.4.3) works with no special-casing.

### 2.7 TypeChecker

**`visitScopeAccess()`**'s `ErrorVariantRef` branch (`src/TypeChecker.cpp`, alongside the existing `EnumVariant` branch) — sets the node's type via `GlobalSymbolTable->LookupType(LeftName)`, the real `TurfUserErrorType`.

**`visitReturn()`**'s `!T` compatibility rule (`src/TypeChecker.cpp:1109`) — a genuinely different rule from ordinary `typesCompatible()`: a function declared `!T` may return either something compatible with `T`, or a value of *any* declared error type (§1.3 — there is no single specific type to check the return value against). `isDeclaredErrorTypeValue()` (`TypeChecker.cpp:1095`) resolves a bare `NamedType` (the shape a would-be constructed variant's type would take, following the same convention `StructName::new(...)` uses) through `GlobalSymbolTable` when needed. Errors.md's forbidden example (`fn foo(): int { return divide(10, 0); }`, `divide` returning `!int`) is rejected by the ordinary generic fallback with no extra code at all — `!int` and `int` simply aren't the same or a compatible type by any existing rule.

**`inferRelationalType()`**'s `AnyErrorType` branch — `==`/`!=` between one side typed `AnyErrorType` and the other `isUserError()` (a real declared error-type value) is allowed explicitly; neither side can ever satisfy the ordinary "same `getName()`" shortcut every other type pair gets, since `err`'s type is deliberately erased (§2.4).

**`visitRecoverExpr()`** (`src/TypeChecker.cpp:2444`):
1. Visits the wrapped expression; if its type isn't `isErrorUnion()`, raises `TypeError` ("`recover` can only be used on a fallible expression") and stops.
2. `SuccessTy = <wrapped type>.getSuccessType()`; stashed onto `Node.ResolvedSuccessType` (§2.3) for Codegen to read later, since Codegen cannot rely on this TypeChecker instance's own inferred-type map (§2.1).
3. `Produce`: visits the produce expression, checks its type against `SuccessTy` via ordinary `typesCompatible()` — the `void`-success rejection (§1.6) falls out of this generically, no special-casing needed.
4. `Run`/`AsRun`: visits the block, then requires `AllPathsDiverge(Block)` **unless** `SuccessTy->isVoid()` (§1.6).
5. Sets the whole `RecoverExprAST` node's type to `SuccessTy` — never `!T` (§1.4).

**`AllPathsDiverge()`** (`src/TypeChecker.cpp:2419`) — the structural check backing §1.5, in the same non-interprocedural spirit as `MutatesSelfField()`:

```cpp
static bool AllPathsDiverge(const ExprAST *E) {
  if (dynamic_cast<const ReturnExprAST*>(E) ||
      dynamic_cast<const BreakExprAST*>(E)  ||
      dynamic_cast<const ContinueExprAST*>(E))
    return true;
  // Block: any diverging statement makes the rest unreachable -> the block diverges.
  // If: diverges only if EVERY branch (including a mandatory else) diverges.
  // While/For/anything else: conservatively NOT diverging (may fall through).
}
```

Recognizes exactly `return`/`break`/`continue` — not e.g. a call to `sys::exit(...)`, which the compiler has no way to know never returns (there is no `@noreturn`-style annotation in this language). This is a deliberate scope boundary, not an oversight: teaching this check to recognize one specific function by name would be a narrow, ad-hoc special case that doesn't generalize (what about `abort()`? a user's own wrapper around `sys::exit`?) — a principled `@noreturn` mechanism is future work, not part of this feature. `break`/`continue` are only ever legal syntax inside a loop to begin with (an earlier, unrelated check already rejects a stray one outside of one), so by the time this function ever sees one, it is already known to genuinely divert control away, same as `return`.

**Generic substitution**: `substituteReturnType()` (used by `visitFuncCall()`'s generic-call path to substitute a generic function's own type parameter into its return type) recurses into `TurfErrorUnionType`'s success type the same way it already recurses into `TurfPointerType`/`ArrayType`/`GenericType`/`NamedType` — needed for a generic function declared `fn<T> f(...): !T` to have its `recover`'s success type correctly substituted to the concrete instantiation at each call site, rather than staying the raw, unsubstituted type parameter.

### 2.8 Codegen

`src/Codegen.cpp`. This is the layer with the most machinery, since it is the only pass that has to give `!T`/error values an actual runtime representation.

**`getFallibleValueType()`** (`Codegen.cpp:99`) — `!T` lowers to **one fixed LLVM struct regardless of T**:

```
%FallibleValue = { i1 isOk, i64 successBits, i8* errorPayload }
```

Built once, cached. This means a function's LLVM return type never varies per success type — `getLLVMType()` needs no per-`T` specialization for `!T` at all (`Codegen.cpp:393`, `if (T->isErrorUnion()) return getFallibleValueType();`).

**`BoxToI64Slot()` / `UnboxFromI64Slot()`** (`Codegen.cpp:141`, `170`) — the success value is boxed into the 64-bit `successBits` slot: bitcast for same-width types (`double`), `[sz]ext`/`trunc` for narrower integers (`bool`, `char`, `int32`), `ptrtoint`/`inttoptr` for pointers (`string`, generic type parameters). For anything wider than 64 bits — a struct or fixed-size array by value — the value is heap-allocated (malloc) and the *pointer* is boxed into the slot instead, then loaded back symmetrically on unboxing. This is the same "value must outlive the function that returns it" reasoning already used for error tag strings (below), applied to the success side.

**`BuildErrorTag()`** (`Codegen.cpp:216`) — an error value's **entire runtime representation** is a global string constant uniquely identifying `ErrorType::Variant`, e.g. `"MathError::DivisionByZero"`. Since a variant carries no payload (scope note, top of document), no heap allocation or wrapping struct is needed at all — the tag string *is* the value, an `i8*` like any other string. This is also why `err == MathError::DivisionByZero` (§1.4.3) lowers to a plain `strcmp` on two tag strings (`getOrCreateStrcmpFn()`), rather than needing any whole-program variant-ID registry: a string is already unique per declared variant with zero cross-compilation-unit coordination.

`GetErrorTagValue()` builds a bare `ErrorType::Variant` reference's tag directly as a compile-time constant (skipping its ordinary `ScopeAccessExprAST::codegen()` path, which would do the same thing less directly); any other error-tag-shaped expression (`err`, or any other `AnyErrorType`/`isUserError()`-typed value) is simply codegenned normally, since its value already *is* the tag string.

`IsErrorTagExpr()` / `IsErrorConstructionExpr()` are the structural recognizers used to dispatch on this: `IsErrorTagExpr` for `BinaryExprAST`'s equality special-case (`Codegen.cpp:1370`), `IsErrorConstructionExpr` for deciding a `return`ed value's wrap direction (below). Both check `ScopeAccessKind::ErrorVariantRef` (or, for a variable, its Resolver-set `ResolvedSymbol->Type`) rather than any TypeChecker-derived information, per the pre-pass/main-pass tree-duplication reasoning in §2.1.

**`ReturnExprAST::codegen()`** (`Codegen.cpp:3455`) — inside a `!T`-declared function, *every* `return` — including a bare `return;` for `!void`'s success case — wraps its value into a `%FallibleValue` via `WrapFallibleValue(RawVal, IsError)` before emitting `ret`, where `IsError = IsErrorConstructionExpr(Val)`. A bare `return;` when `T` is `void` wraps `{isOk=true, successBits=0, errorPayload=null}` — the function's actual LLVM return type is never bare `void` once its declared type is `!void`, so a plain `CreateRetVoid()` would be invalid here.

**`RecoverExprAST::codegen()`** (`Codegen.cpp:4547`) — the control-flow lowering:
1. Codegens the wrapped expression, extracts `isOk`, branches to a success block and a failure block.
2. Success block: extracts `successBits`, unboxes to `T`'s real representation (skipped when `T` is `void`), branches to a merge block.
3. Failure block: extracts `errorPayload` (the tag string). For `Produce`, codegens the produce expression and branches to merge. For `Run`/`AsRun`, `AsRun` first binds the bound name to an `AnyErrorType`-typed alloca holding `errorPayload` (the same save/restore-`NamedValues` + `GlobalSymbolTable` scope-push/pop idiom the for-loop variable already uses), then codegens the block.
4. After the block: if the current insert block still lacks a terminator, branch to merge (this is the live, legitimate path for the `void`-success non-diverging case, §1.6). If it already has one (a `return`/`break`/`continue` fired inside the block, leaving one of those constructs' own "dead block after the diverging statement" as the current insert point, §2.3's cross-reference to how `ReturnExprAST`/`BreakExprAST`/`ContinueExprAST` codegen all behave), **branching that dead block to merge anyway is deliberate, not a missed case**: it is provably unreachable at runtime (nothing can reach a block positioned immediately after an unconditional terminator), so it becomes a harmless extra predecessor on the merge block's `PHINode` rather than a correctness problem — this avoids re-implementing `AllPathsDiverge`'s own analysis a second time here, in a form that could drift out of sync with it.
5. Merge block: for a `void` success type, returns a dummy value (never used, by definition — nothing has a `void`-typed variable to receive it). Otherwise, a `PHINode` combines whichever paths actually reached the merge block — the success path always contributes one incoming edge; the failure path contributes one only if it didn't diverge.

**`BinaryExprAST::codegen()`**'s equality dispatch (`Codegen.cpp:1370`) — checked *before* the ordinary short-circuit/generic operator-codegen paths: if either side of `==`/`!=` is error-tag-shaped (`IsErrorTagExpr`), both sides' tags are obtained via `GetErrorTagValue()` and compared via `strcmp`. Without this, both sides would already codegen to plain `i8*` values (indistinguishable, at the LLVM level, from any other string or pointer), and raw pointer-identity comparison would silently give the wrong answer for two logically-equal-but-differently-allocated error references.

### 2.9 Diagnostics

New diagnostics (`include/Errors.h:1625`, `1647`) follow this codebase's existing `...Error : public TurfError` / `...Warning : public TurfWarning` convention unchanged — this is the compiler's own diagnostic machinery reporting on the user-facing `error` keyword feature, a naming collision risk in appearance only (see §2.4's opening note): `NeverReturnsErrorWarning` (§1.8), `BareErrorAsBindingError` (§1.7). Everything else reuses the existing generic `TypeError`/`LogErrorAt`/`WrongArityError` machinery, matching how this codebase already handles most struct/enum diagnostics.

---

## Part 3 — End-to-end walkthrough: `math::divide(10, 0) recover produce -1;`

1. **Parse**: `math::divide(10, 0)` — an ordinary `FuncCallExprAST` (no `error`-specific parsing involved at all, §2.5). `recover produce -1` is then parsed as a `RecoverExprAST(Wrapped=<the call>, Kind=Produce, ProduceExpr=<-1>)`, bound to the whole call expression (§2.5).
2. **Hoist (Pass 1)**: `math::divide`'s signature (`(int, int): !int`) is registered like any ordinary function; `MathError`'s `DivisionByZero` variant is registered into the error-type registry (§2.6) wherever `math.tr`'s own `error MathError { DivisionByZero }` declaration is hoisted.
3. **Resolve (Pass 2)**: the call resolves normally (ordinary overload/arity matching — nothing error-specific). `Resolver::visitRecoverExpr()` resolves the wrapped call, then the produce expression (`Kind == Produce`, so no block/bound-name handling applies).
4. **TypeCheck**: the call's type is `!int` (`TurfErrorUnionType(TurfInt())`). `TypeChecker::visitRecoverExpr()` unwraps to `SuccessTy = int`, stashes it on `Node.ResolvedSuccessType`, type-checks `-1` against `int` (matches), and sets the whole `RecoverExprAST`'s type to `int`.
5. **Codegen**: `math::divide`'s body, on its `b == 0` path, constructs `MathError::DivisionByZero` as a tag string and wraps it into `{isOk=false, successBits=0, errorPayload="MathError::DivisionByZero"}` for its `return`. At the call site, `RecoverExprAST::codegen()` extracts `isOk` (false), takes the failure branch, codegens `-1` as the produce value, and merges — the whole expression's runtime value is the `i64` constant `-1`.

---

## Part 4 — File map

| File | Role |
|---|---|
| `include/Lexer.h`, `src/Lexer.cpp` | `TOK_ERROR`/`TOK_RECOVER`/`TOK_PRODUCE`/`TOK_RUN`/`TOK_AS` |
| `include/AST.h` | `ErrorDeclExprAST`, `RecoverExprAST`, `RecoverKind`, `ScopeAccessKind::ErrorVariantRef` |
| `include/Types.h` | `TurfUserErrorType`, `TurfErrorUnionType`, `AnyErrorType`, `isUserError()`/`isErrorUnion()`/`isAnyError()` |
| `src/Parser.cpp` | `ParseErrorDecl()`, `ParseType()`'s `!` branch, `ParseExpression()`'s `recover` dispatch |
| `include/Resolver.h`, `src/Resolver.cpp` | Error-type hoisting, `visitFuncCall`/`visitScopeAccess`'s error-type branches, `CollectReturnedErrorTypeNames`, `visitFuncDef`'s `NeverReturnsErrorWarning`, `visitRecoverExpr` (incl. the bare-error restriction) |
| `include/ResolverSymbolTable.h`, `src/ResolverSymbolTable.cpp` | The error-type registry, `ResolverFunctionSymbol::PossibleErrorTypeNames` |
| `include/TypeChecker.h`, `src/TypeChecker.cpp` | `visitScopeAccess`'s `ErrorVariantRef` branch, `visitReturn`'s `!T` rule, `inferRelationalType`'s `AnyErrorType` branch, `visitRecoverExpr`, `AllPathsDiverge`, `substituteReturnType`'s `TurfErrorUnionType` branch |
| `src/Codegen.cpp` | `getFallibleValueType`, `BoxToI64Slot`/`UnboxFromI64Slot`, `BuildErrorTag`/`GetErrorTagValue`, `ReturnExprAST::codegen()`'s wrap logic, `RecoverExprAST::codegen()`, `BinaryExprAST::codegen()`'s equality dispatch |
| `include/Errors.h` | `NeverReturnsErrorWarning`, `BareErrorAsBindingError` |
| `lib/std/io/net.tr`, `lib/std/math/math.tr`, `lib/std/collections/map.tr` | Real stdlib usage: `NetworkError` (`connect`/`bind`/`accept`), `MathError` (`divide`), `MapError` (`tryGet`) |
| `tests/25_errors/` | The full regression suite for this feature |

---

## Part 5 — Known limitations, deliberately deferred

### 5.1 No payload fields

An error variant carries no data (§1.1's scope note). This was a deliberate simplification made after the feature's initial implementation already supported typed payload fields end to end (construction, arity/type-checking, a heap-allocated tagged-payload runtime representation) — removed because no real program built against it ever needed to *read* a payload back, and doing so safely requires a real destructuring syntax (different variants of the same error type can carry different fields) that does not exist yet. Re-adding payload support is real future work with its own design pass, most likely as an extension of this language's existing `compare`/`matches` pattern-matching construct (already used for `enum` values) rather than direct field access, given the "which variant, therefore which fields" problem that implies.

### 5.2 No error-type propagation shorthand

There is no `?`-style operator or any other shorthand to auto-forward an unhandled `!T` failure to the caller's own `!T'` return type. Every `!T` call site must handle it with `recover`, explicitly, every time — this is the whole design as specified in `Errors.md`, not a gap.

### 5.3 `AllPathsDiverge` is structural, not a full CFG reachability analysis

Covered in §2.7: only `return`/`break`/`continue` are recognized as diverging. A block that only diverges via a call the compiler cannot prove never returns (`sys::exit(...)`, a user's own wrapper around it, `panic()`-style helpers) is conservatively rejected as "not diverging," even though it may be perfectly safe in practice. Closing this gap would need a `@noreturn`-style function annotation, which does not exist in this language.

### 5.4 `recover` cannot appear where its enclosing function has no declared return type context

Not actually a limitation of `recover` itself, but worth stating precisely since §1.4.2 leans on it: a `return`/`break`/`continue` inside a `recover run`/`recover as err run` block is only meaningful (and only compiles) when there genuinely is an enclosing function/loop for it to target, exactly like a bare `return`/`break`/`continue` anywhere else in the language.
