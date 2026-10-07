# User-Defined `error` Types, `!T`, and `recover`

Technical reference for Turflang's recoverable-error system, as implemented. This document defines **what a Turflang programmer observes** (Part 1) and **how the compiler implements it, pass by pass** (Part 2), with file:line references throughout so the two halves can be cross-checked against the real source.

The original design brief was `docs/planning/Errors.md`, a first-draft proposal doc removed 2026-08-04 once the feature shipped and this document (its precise technical reference) superseded it — see git history if the original prose motivation/worked-examples version is ever needed. This document defines exact contracts (what type each construct has, when each diagnostic fires, what each pass does) and traces the implementation.

**Scope note**: an error variant is a bare name or carries typed PAYLOAD FIELDS declared as part of the variant (`NotFound(path: string)`, LEC-004, `FREEZE.md` §6.18 / F-ERR-1 — §1.2b), read back only by destructuring patterns in a `compare` (§1.4.4); `try` (§1.10) and the pass-through `return` (§1.11) propagate a `!T`'s error. STD-002's universal `string` message (§1.2a, `err.message`) is unchanged and coexists with payloads. Sections written before LEC-004 describe the bare-variant and message-only cases, which are unchanged; where a mechanism grew, it says so.

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

Structurally like a C-style `enum` — a fixed set of variant names, no generics; each variant may additionally declare payload fields (§1.2b). `IOError` becomes a distinct type (`ErrorType`, backed by `TurfUserErrorType`, §2.4), validated in the pipeline and represented as an AST node exactly like a `struct`/`enum` declaration.

**Bare form**, sugar for a single implicit variant:

```turf
error GeneralError;
```

Equivalent to `error GeneralError { DEFAULT }`. Constructed as `GeneralError::DEFAULT`. See §1.7 for the one restriction bare-form error types carry.

### 1.2 Referencing a variant

```turf
MathError::DivisionByZero
```

A bare, **non-callable** reference — identical in spirit to `Color::Red` for a plain `enum`. `MathError::DivisionByZero()` (call syntax, with zero arguments) is rejected the same way `Color::Red()` already is: an error variant is a value, not a function — **except** for the one message-carrying construction form §1.2a describes next. Any OTHER arity (two or more arguments) is rejected the identical way.

```turf
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}
```

### 1.2a Constructing a variant with a message (STD-002)

```turf
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero("division by " + (a to string) + "/0");
    }
    return a / b;
}
```

Exactly **one** `string` argument — `ErrorType::Variant(messageExpr)` — attaches a message to this specific error *occurrence*. `messageExpr` need not be a compile-time constant; it may be built at runtime (string concatenation, a formatted value, …), unlike the variant's own tag, which is always compile-time-known. The variant must still genuinely exist (`NoSuchVariantError` on a typo, exactly like the bare form); any arity other than exactly one is still rejected as "not a function" (§1.2's own rejection).

Every variant of every error type gets the same one `message: string` slot, whatever fields it declares (§1.2b). Reading the message back is via `err.message` (§1.4.3) — a special, narrow field read on an `AnyErrorType`-typed value, not general struct field access. A bare `ErrorType::Variant` reference (no message) still works exactly as before; `err.message` on one simply reads back as `""`, never garbage or a crash.

### 1.2b Payloads: typed fields on a variant (LEC-004)

```turf
error IOError { NotFound(path: string), Denied(path: string, mode: int), Timeout }
```

A variant's fields are named and typed. Any sized type is allowed (primitives, enums, structs, arrays, generic instances) except an error type, a `!T` or `void`; a raw-pointer field needs `unsafe` as a struct field does, and a `pub` error's field types may not be private (E089). Field names are unique within a variant. The declaration's spellings resolve under FDN-003's identity rules like a struct field's.

**Construction** is positional: a variant with *N ≥ 1* fields takes *N* arguments, or *N+1* where the trailing `string` is the message (DEC-12); a variant with no fields takes exactly one (its message), as before. The Resolver decides the count (E096, naming the declared fields), the TypeChecker each argument's type (E096). A bare `E::V` for a variant that has fields is E096 — it has no payload to fill in — except as a direct operand of `==`/`!=` (`ScopeAccessExprAST::IsTagOperand`, set by `Resolver::visitBinary()`) and as a pattern head.

**Reading** is only by pattern (§1.4.4). There is no `err.path`: which fields exist depends on which variant occurred.

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

**The recover expression's own type is always `T` — never `!T`.** This holds for all three forms; a `!T` annotation on the variable receiving a `recover` result would be a mistake, not a second valid spelling.

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

Equality compares **variant identity only**, never the message (STD-002, §1.2a) — `err == MathError::DivisionByZero` is `true` regardless of what message `err` happens to be carrying, exact and total, no partial/field-level comparison to worry about.

**`err.message`** reads the `string` attached at construction (§1.2a), or `""` if the error was a bare (message-less) reference:

```turf
divide(10, 0) recover as err run {
    io::printline(err.message);   // "division by 10/0"
};
```

This is the *only* field `err` has — `err.message` is a special-cased read on `AnyErrorType`, not general struct field access (`err` is not a struct); any other field name is rejected with a dedicated diagnostic.

#### 1.4.4 Reading a payload: error-variant patterns (LEC-004)

```turf
compare err {
    matches IOError::NotFound(path):   ...  done
    matches IOError::Denied(path, ..): ...  done
    matches IOError::Timeout:          ...  done
    otherwise:                         ...
}
```

`matches E::V(sub, ..)` matches an error value whose tag is `E::V` and whose fields match the sub-patterns positionally — the sub-patterns of a struct pattern (`docs/reference/control_flow.md`, `FREEZE.md` §6.17). It lists every field or ends in `..`, and a variant without fields is written bare; a bare name for one that has fields is E092 (write `E::V(..)`). The subject is an open `AnyErrorType` (always needs `otherwise`/`_`, E093) or a declared error type (every variant covered by an unguarded variant pattern whose fields are all irrefutable is exhaustive). A variant pattern is never a catch-all. The message is not bindable in a pattern; `err.message` reads it. Matching a non-error subject, or another error type's variant against a declared-error subject, is E092.

### 1.5 The divergence rule, precisely

For `recover run`/`recover as err run` with a non-`void` `T`: every control-flow path through `BLOCK` must end in `return`, `break`, or `continue`. This is checked structurally (not via full CFG reachability) — see §2.7 for exactly what is and isn't recognized as diverging, and why.

Since LEC-004 a `compare` also counts as diverging when some arm is certain to run (an `otherwise`, or `CompareExprAST::Exhaustive`) and every arm body and the `otherwise` body diverge — reading a payload *is* a `compare` over `err`, so without this every such block needed a dead `return` after it. Still structural: only `return`/`break`/`continue`, `if`-with-`else` and now `compare` are recognised.

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
| `TypeError` ("is an error variant, not a function") | A variant with NO fields is called with any arity other than exactly one `string` argument (0, 2+) |
| `ErrorPayloadError` (E096) | LEC-004: a payload declared wrongly (empty parentheses, missing type, duplicate field, a field of error/`!T`/`void` type) or constructed wrongly (argument count not *N* or *N+1*, an argument not its field's type, a trailing message not a `string`), or a bare `E::V` value for a variant that has fields |
| `TryMisuseError` (E095) | LEC-004: `try` at the top level, in a function not returning `!V` (`main` gets its own hint), inside a `defer`, or on a non-fallible operand |
| `InvalidPatternError` (E092) / `NonExhaustiveCompareError` (E093) | LEC-004, §1.4.4: an error-variant pattern that lists the wrong fields, names a bare variant that has fields, has empty parentheses, sits over a non-error or other-error-type subject; an open error union, or a declared error type with an uncovered variant, without `otherwise` |
| `LogErrorAt` ("Error type '...' has no variant '...'") | A reference (bare, a call target, or STD-002's message-carrying construction) to an unknown variant name, with Damerau-Levenshtein suggestions |
| `TypeError` ("has no field '...' -- only 'message' is readable") | STD-002 (§1.4.3): a field access on an `AnyErrorType`-typed value (`err`) other than `.message` |
| `DuplicateTypeNameError` | Two `error` (or `error`/`struct`/`enum`) declarations share one name |
| `TypeError` ("'recover' can only be used on a fallible...") | `recover` applied to an expression whose type is not `!T` |
| `TypeError` ("doesn't match this call's success type") | `recover produce value` where `value`'s type isn't exactly `T` |
| `TypeError` ("must end every path with 'return'") | `recover run`/`recover as err run` on a non-`void` `T` where the block doesn't diverge (§1.5) |
| `BareErrorAsBindingError` | `recover as err run` where every possible error is bare-form (§1.7) |
| `NeverReturnsErrorWarning` | A `!T` function whose body never actually returns an error-type value (§1.8) |
| Generic "Return type mismatch" / "Cannot initialise..." | `!T` used where `T` (or a compatible type) was expected, or vice versa, anywhere a return/assignment type-check already applies — except `return f();` of exactly the function's own `!T` (§1.11); its hint names `recover`/`try` |

### 1.10 `try`: propagation (LEC-004)

`try <expr>` is a prefix operator (`ParseUnary()`, so `try a() + try b()` is `(try a()) + (try b())`). The operand must be `!U`; the expression is a `U`. On failure it returns the operand's error value **unchanged** from the enclosing function, which must return `!V` (any `V` — the union is open, nothing converts), after running pending `defer`s exactly as `return` does. It is sugar for `e recover as err run { return err; }`, but a node of its own (`TryExprAST`) because the desugaring would misreport: a type error would say "return type mismatch", and the bare-error restriction on `recover as` would fire on an error that is merely passed on. Illegal (E095) at the top level, in a function not returning `!V` (`main`, returning `int`, included), inside a `defer` (INV-DEFER-2 — counted by `Resolver::DeferDepth_`, since `FindEscapingControlFlow()` only walks statements and a `try` is an expression), and on a non-fallible operand. `try` is a reserved word (`TOK_TRY`).

### 1.11 The pass-through `return`

`return f();` where `f(): !T` and the enclosing function returns exactly `!T` returns the fallible value as it is (`ReturnExprAST::PassesThroughFallible`, set by `TypeChecker::visitReturn()` and read by Codegen, which cannot tell it from `return g(x);` by shape). It never unwraps or converts (§5.2's rule stays). This fixed audit F-21, where the success type had been compared against the operand's whole union and the message read "function declares '!Rec' but returns '!Rec'".

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

### 2.2 Lexer: five new keywords (and `try`, LEC-004: `TOK_TRY = -108`, reserved)

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
    std::vector<ErrorVariantInfo> VariantInfos;  // name + payload fields (LEC-004); getVariants() is the names
    bool                      IsBareForm;
};
```

**`ScopeAccessKind::ErrorVariantRef`** (`include/AST.h:1033`) — added to the existing `ScopeAccessKind` enum (the same node type that already represents `EnumName::Variant`, `Module::func`, `StructName::method`, etc.). Set on a `ScopeAccessExprAST` whenever `Left` names a declared error type; always non-callable, mirroring `EnumVariant`'s existing non-callable rule. Kept as its own enum value (rather than reusing `EnumVariant`) because error values lower differently in Codegen (a tag+message struct, not an integer ordinal, §2.8) and participate in `!T`/`recover`/`AnyErrorType` comparisons that plain enums don't.

**`ScopeAccessKind::ErrorVariantConstruct`** (`include/ASTAccess.h`; `ErrorVariantConstructWithMessage` under STD-002, renamed by LEC-004 when variants gained fields) — the *callable* sibling: set when `Left::Right(...)` names a declared error type's variant AND the argument count is one the variant accepts (§1.2a/§1.2b). `ScopeAccessExprAST::IsTagOperand` marks a bare reference that may name a variant with fields (an `==`/`!=` operand or a pattern head). Kept as its own kind rather than a flag on `ErrorVariantRef` because the two dispatch completely differently downstream (Resolver's arity gate, TypeChecker's short-circuit, Codegen's GC-heap-allocate-and-store construction (OVH-002 — a bare `malloc` before it) vs. a compile-time-only global) — see §2.6/§2.7/§2.8.

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

`ErrorType::Variant` construction needed **no new expression node at all**: `Left::Right(args)` already parses generically as `FuncCallExprAST(Callee=ScopeAccessExprAST(...))` regardless of what `Left` is (the same mechanism `StructName::new(...)` uses). Before STD-002, a variant was never callable (§1.2), so this shape was only ever reached to be *rejected* (§2.6). STD-002 gives it a real, accepted meaning for exactly one arity (§1.2a) — Resolver dispatches on `Node.getArgs().size()` to tell the two cases apart (§2.6), still with zero new grammar or AST nodes.

### 2.4 Types

`include/Types.h`. Two collision-risk notes are load-bearing here: neither new type class is named anything close to `TurfError` (`include/Errors.h`), the compiler's own internal diagnostic base class — an unrelated concept that happens to share the English word "error."

**`TurfUserErrorType`** (`Types.h:561`) — the type of a declared `error Name { ... }`:

```cpp
class TurfUserErrorType : public TurfType {
    std::string               Name_;
    std::vector<ErrorVariantInfo> VariantInfos_;  // name + payload fields (LEC-004)
    std::vector<std::string> Variants_;  // the names alone, like TurfEnumType
    bool                      IsBareForm_;
public:
    bool isUserError() const override { return true; }
    bool isBareForm() const { return IsBareForm_; }
    bool hasVariant(const std::string&) const;
    const ErrorVariantInfo *getVariantInfo(const std::string&) const;  // fields, or null
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

**`ParseErrorDecl()`** (`src/Parser.cpp:2077`) — modeled directly on `ParseEnumDecl()`'s variant-list loop (duplicate-variant rejection included); the bare form (`error Name;`) desugars to a single `"DEFAULT"` variant at parse time. Since LEC-004 (`src/ParserDecl.cpp`) a variant name may be followed by `( name: Type, ... )`, each type read with `ParseType()`; the parser rejects only what is syntactic (empty parentheses, a missing `: Type`, a repeated field name — all E096). Whether a type is allowed is the Resolver's (§2.6). **`try`** is parsed in `ParseUnary()` (`src/ParserCore.cpp`) as a prefix operator; `FuncBodyTryScope` records on the enclosing `FuncDefExprAST` that a `try` occurs. **Error-variant patterns** are parsed by `ParsePattern()` (`src/ParserStmt.cpp`): `ParsePostfixExpression()` is told (`PatternHeadCallPending`, one-shot) to stop at the `(` of the first primary of a `matches` header when it is a scope access, and `ParseErrorVariantPatternBody()` reads the parenthesised sub-patterns (and `..`).

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

**`visitFuncCall()`**'s error-type branch (`src/ResolverCall.cpp`) — when `Left` names a known error type: validates the variant exists (`NoSuchVariantError`, for any arity), then by the variant's declared field count *N* (from `TurfUserErrorType::getVariantInfo()`): *N = 0* with exactly one argument (the message, STD-002) or *N ≥ 1* with *N* or *N+1* arguments (LEC-004) sets `ResolvedKind = ErrorVariantConstruct`. Any other count sets `ErrorVariantRef` and raises — a `TypeError` ("is an error variant, not a function") for a variant with no fields, mirroring the `EnumVariant` rejection just above, `ErrorPayloadError` (E096, listing the declared fields) for one with fields. Args are resolved (`visitChild()`) unconditionally at the top of `visitFuncCall()`, before this dispatch.

**Declarations.** `Resolver::resolveDeclarationSpellingsIn()` rewrites each payload field's type spelling to its identity in place (the `TurfType` objects are shared with every registered `TurfUserErrorType`, exactly as a struct's fields are); `visitErrorDecl()` validates each field type (`validateNamedType()`), applies the private-in-public check to a `pub` error (E089) and rejects error/`!T`/`void` field types (E096).

**`try`.** `Resolver::visitTry()` rejects a `try` inside a `defer` (E095, `DeferDepth_`). `FuncDefExprAST::UsesTry` (set by the Parser) makes `buildFunctionSymbol()` mark the function `ReturnsReboundError` — a `try` passes on another call's error just as `return err;` in a `recover as err` block does, so `NeverReturnsErrorWarning` does not fire and `visitRecoverExpr()`'s bare-error check skips such a callee. `visitReturn()` notes a `return f();` whose candidates return `!T` (`ReturnPassesFallible_`) for the same warning.

**Patterns.** `Resolver::normalizeHeaderPattern()` settles a `matches` header's `Type::Variant` spelling: a bare `E::V` over an error type becomes an `ErrorVariantPatternExprAST`; a parsed `Name::Name(...)` pattern whose `Name` is NOT an error type is turned back into the function call it was written as, so `matches strings::to_upper(x):` keeps its value-pattern meaning (`tests/45_pattern_matching/19_module_call_header_unchanged.tr`). `resolveArmPattern()` then resolves the head as an ordinary scope access (marked `IsTagOperand`) and each sub-pattern as a struct pattern's field is.

**`visitScopeAccess()`**'s error-type branch (`src/Resolver.cpp:1815`) — the bare (non-call) reference case: `ResolvedKind = ErrorVariantRef`, validates the variant exists (with suggestions on a near-miss), otherwise a no-op — there are no fields to validate anymore (§5.1).

**`CollectReturnedErrorTypeNames()`** (`src/ResolverCore.cpp`) — a conservative, non-interprocedural structural scan of a function body's `return` statements for a bare `ErrorType::Variant` shape, collecting the left-hand name. Descends through `Block`/`If`/`While`/`For`/`ForEach` (mirroring `MutatesSelfField()`'s recursion shape) and, since OVH-012, also `VarDecl`/`Assignment` (into the initializer/RHS) and `RecoverExpr` (into `Wrapped`, and `ProduceExpr` or `Block` depending on `RecoverKind`) — needed because the idiomatic way a `!T` function surfaces a *different* error than whatever it called failed with is `immut x: T = call() recover run { return ErrorType::Variant; };` (`io::read_lines`, `json::parse_number`/`parse_array`/`parse_object`), and the scan previously had no case for any of those three node kinds at all, so a `return` reachable only through a `recover` block was invisible to it. Feeds `ResolverFunctionSymbol::PossibleErrorTypeNames` (`ResolverSymbolBase.h`), computed once inside `buildFunctionSymbol()`.

Also since OVH-012: when the scan is inside a `recover as err run { ... }` block's `Block` and a `return` names a bare variable matching that block's own `BoundName` (`json::parse`'s `p.parse_value() recover as err run { return err; };` — re-throwing whatever error the wrapped call actually produced), there is no error type *name* to collect at all (`err`'s static type is the type-erased `AnyErrorType`). This sets `ResolverFunctionSymbol::ReturnsReboundError` instead — a separate boolean, `NeverReturnsErrorWarning`'s check below now treats it as "returns a real error" unconditionally, bypassing the `isErrorTypeName()` filter (there is nothing to filter).

This list is **deliberately unfiltered** at collection time: `buildFunctionSymbol()` runs during Pass 1 hoisting, when a later-declared error type might not be registered yet, and a `Left::Right` shape could just as easily name a struct/enum/module reference that has nothing to do with errors. Every consumer filters it through `isErrorTypeName()` at its own, later point (Pass 2, by which point every error type in the program is guaranteed registered) before drawing any conclusion from it. Two consumers:

1. **`NeverReturnsErrorWarning`** (§1.8) — `Resolver::visitFuncDef()`, after the body is fully resolved: if the function's declared return type `isErrorUnion()` and the filtered `PossibleErrorTypeNames` is empty, emit the warning. Deduplicated by source location (a process-lifetime static set) — this compiler resolves every function body **twice** (once in `main.cpp`'s pre-pass, once again in its main pass, a separate re-parse used only for linting, §2.1) — every other existing warning happens to dodge this by living in Codegen instead (which only runs once); this is the first Resolver-level warning to actually hit the duplication.

2. **The bare-error restriction** (§1.7) — `Resolver::visitRecoverExpr()` (`src/Resolver.cpp:1720`), only for `Kind == AsRun`: reads the wrapped call's resolved candidate's `PossibleErrorTypeNames`, filters through `isErrorTypeName()`, and — if the resulting set is non-empty and *every* entry is bare-form (`isBareForm()`) — raises `BareErrorAsBindingError`. If the callee is ambiguous (an unresolved overload, no unique `ResolvedCallTarget`) or its filtered set is empty, the check is skipped rather than guessed at — conservative under-flagging, the same posture `MutatesSelfField()` already takes for a hard-error gate.

**`visitRecoverExpr()`**'s dispatch, in full: resolves the wrapped expression first (populating its callee's `ResolvedCandidates`, needed above). For `Produce`, resolves the produce expression in the current scope. For `Run`/`AsRun`, the block gets its own scope (mirroring if/while/function bodies); `AsRun` additionally declares the bound name as an `AnyErrorType`-typed variable *before* resolving the block, so ordinary variable lookup inside it (including the equality check in §1.4.3, and STD-002's `.message` field read) works with no special-casing.

**`CollectReturnedErrorTypeNames()`** (STD-002 update) also recognizes a `return`ed `FuncCallExprAST` whose callee is a `ScopeAccessExprAST` — the message-carrying construction's shape — collecting `Left` the identical unfiltered way the bare-reference branch already does. Without this, a function that only ever fails via `return ErrorType::Variant("msg");` (never a bare reference) was wrongly flagged with `NeverReturnsErrorWarning` (§1.8) — confirmed live before this fix.

### 2.7 TypeChecker

**`visitScopeAccess()`**'s `ErrorVariantRef` branch (`src/TypeChecker.cpp`, alongside the existing `EnumVariant` branch) — sets the node's type via `GlobalSymbolTable->LookupType(LeftName)`, the real `TurfUserErrorType`.

**`visitFuncCall()`**'s `ErrorVariantConstruct` branch (`src/TypeCheckerCall.cpp`) — short-circuited immediately after the pre-existing `DefaultConstruct` short-circuit, for the identical reason: Resolver never populates `ResolvedCandidates` for this kind either. Checks each field argument against its declared type (`typesCompatible()`, OVH-005's untyped-`int32`-literal rule, literal representability) and a trailing message is a `string` (E096 otherwise), then sets the whole call's type to the registered `TurfUserErrorType` — the SAME type a bare reference gets (`visitScopeAccess()`'s `ErrorVariantRef` branch just above). (It was a `NamedType` under STD-002, which made `err == E::V("msg")` a type error, since `inferRelationalType()` allows `==` against `AnyErrorType` only for a value whose type `isUserError()`.)

**`visitTry()`** (`src/TypeCheckerCall.cpp`): rejects (E095) a `try` with no enclosing function, in a function whose return type is not `!V` (the hint differs for `main`, via `CurrentFuncName_`), and on an operand that is not `!U`; otherwise stashes `U` on `TryExprAST::ResolvedSuccessType` for Codegen and types the expression `U`. **`visitReturn()`** accepts a `!T` operand against exactly the declared `!T` and sets `PassesThroughFallible`. **`visitErrorDecl()`** applies the `unsafe` gate to a raw-pointer field and records generic field instantiations (`List<string>` used only as a payload field would otherwise never be instantiated).

**`checkPattern()`** (`src/TypeCheckerPattern.cpp`) handles `ErrorVariantPatternExprAST`: subject must be an error (E092), the pattern's head must belong to a declared-error subject's type, the field list must be complete or end in `..`, and each sub-pattern is checked against its field's declared type by the same code a struct pattern's fields use (`FieldTypes` is stored for Codegen). It always returns "refutable" (never a catch-all); `FieldsIrrefutable` feeds `visitCompare()`'s coverage of a declared error type's variants.

**`visitFieldAccess()`**'s `AnyErrorType` branch (`src/TypeCheckerCall.cpp`, STD-002) — checked before the ordinary struct-field machinery (an `AnyErrorType` base is never a struct): `.message` types as `string`; any other field name is a dedicated `TypeError`.

**`visitReturn()`**'s `!T` compatibility rule (`src/TypeChecker.cpp:1109`) — a genuinely different rule from ordinary `typesCompatible()`: a function declared `!T` may return either something compatible with `T`, or a value of *any* declared error type (§1.3 — there is no single specific type to check the return value against). `isDeclaredErrorTypeValue()` (`TypeChecker.cpp:1095`) resolves a bare `NamedType` (the shape a would-be constructed variant's type would take, following the same convention `StructName::new(...)` uses) through `GlobalSymbolTable` when needed. The canonical forbidden example (`fn foo(): int { return divide(10, 0); }`, `divide` returning `!int`) is rejected by the ordinary generic fallback with no extra code at all — `!int` and `int` simply aren't the same or a compatible type by any existing rule.

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

**`BoxToI64Slot()` / `UnboxFromI64Slot()`** (`Codegen.cpp:141`, `170`) — the success value is boxed into the 64-bit `successBits` slot: bitcast for same-width types (`double`), `[sz]ext`/`trunc` for narrower integers (`bool`, `char`, `int32`), `ptrtoint`/`inttoptr` for pointers (`string`, generic type parameters). For anything wider than 64 bits — a struct or fixed-size array by value — the value is heap-allocated **on the GC heap** (`turf_rt_alloc`, with `T`'s own real descriptor — OVH-002, `docs/development/changes/OVH-002.md`; a bare, unregistered `malloc` before it, which left any GC-heap pointer INSIDE that boxed `T` — e.g. a `string` field — untraced and at risk of being freed by a collection between the box's construction and its later unboxing, `tests/41_gc/09_boxed_fallible_success_survives_collection.tr`) and the *pointer* is boxed into the slot instead, then loaded back symmetrically on unboxing. This is the same "value must outlive the function that returns it" reasoning already used for an error value (below, and STD-002's own message-carrying construction), applied to the success side.

**`getErrorValueType()`** (`CodegenCore.cpp`, STD-002) — an error value's runtime representation, since STD-002 (§1.2a), is **one fixed LLVM struct regardless of whether a message was attached**, the exact same "one shape, no discriminated union" design `getFallibleValueType()` already uses for `!T` itself:

```
%TurfErrorValue = { i8* tag, i8* message, i8* payload }   ; payload added by LEC-004
```

`payload` is null for a variant with no fields, else points to a GC-allocated per-variant struct (§ `CodegenErrorVariantConstruct()` below). Before STD-002, an error value's entire runtime representation WAS the bare tag string directly (no wrapping struct at all) — a plain `i8*`. That is no longer true in general: `errorPayload`/`err`'s alloca are still an opaque `i8*`/`ptr` at the LLVM level (neither needed any change — pointers carry no static shape), but what that pointer now POINTS TO is always a `TurfErrorValue`, uniformly, whether the error came from a bare reference (`message = ""`, a global constant, §1.2a's compatibility note) or a message-carrying construction. A discriminated union (bare tag string OR struct pointer, told apart at runtime) was deliberately rejected: nothing distinguishes the two shapes without a discriminant, and reading an arbitrary number of bytes past a bare tag string's own NUL terminator to check would be a real memory-safety hazard.

**`BuildErrorTag()`** (`CodegenCore.cpp`) builds a BARE `ErrorType::Variant` reference's runtime value: a pointer to a global constant `TurfErrorValue{tag, message="", payload=null}` (only a variant with no fields is ever a bare value — one with fields is E096 except as an `==` operand, which never builds a value). Fully compile-time-known, so no heap allocation is needed — mirrors `BuildBareTagString()`'s (see next paragraph) own no-allocation global-constant approach, just wrapped in the struct. `BuildGlobalCStringPtr()` is the shared helper (mirroring RT-001's `emitTurfTypeDesc()`/`NameGV`+`NamePtr` idiom, CodegenStruct.cpp — the only other place in this codebase already builds a global constant struct holding a string-pointer field) that builds a `Constant*`-typed string pointer suitable as a `ConstantStruct::get()` field initializer.

**`CodegenErrorVariantConstruct()`** (`CodegenCore.cpp`; `…WithMessage` under STD-002) — `ErrorType::Variant(fields…, [message])`'s construction (§1.2a/§1.2b). Unlike the bare case, the message and fields are in general RUNTIME values, so this GC-heap-allocates (`turf_rt_alloc` — OVH-002; a bare `malloc` before it left `message`'s GC-heap `string` pointer untraced, `tests/41_gc/08_error_message_survives_collection.tr`). Two blocks for a variant with fields: the **payload struct** first — one *named* LLVM struct per variant, `errpayload.<Error>.<Variant>` (`getErrorPayloadStructType()`, built lazily inside a function body, when every struct a field can name is complete), holding the fields by value in declaration order — then the `TurfErrorValue` with its `payload` pointing at it. Being an identified struct, the payload is given a `TurfTypeDesc` by `EmitAllTurfTypeDescs()` with no special case, which is what lets the collector trace a `string`, a `List<string>` or a nested struct held only in a payload (`tests/41_gc/09_error_payload_survives_collection.tr` — it fails if the payload is allocated untraced; verified). Between the two allocations the payload pointer is held only in a register, which the collector scans conservatively. The same "the value must outlive the function that returns it" reasoning `BoxToI64Slot()` uses for an oversized success value. Dispatched from `FuncCallExprAST::codegen()`'s `ErrorVariantConstruct` branch (CodegenFunc.cpp), evaluating the arguments in source order and casting each field argument to its declared type.

**`try`** (`TryExprAST::codegen()`, `CodegenControlFlow.cpp`, next to `ReturnExprAST`): evaluates the `!U` operand, branches on `isOk`; the failure block extracts `errorPayload`, runs `FlushPendingDefers(0)` (hence the placement — that helper is file-static), and returns `WrapFallibleValue(err, true)`; the success block unboxes `U` from `successBits` (`ResolvedSuccessType`) and is where the builder is left. The error pointer travels as-is, so tag, payload and message arrive unchanged. **Pass-through `return`** (`ReturnExprAST::PassesThroughFallible`) emits `ret` of the operand's `FallibleValue` directly instead of wrapping it again.

**Variant patterns** (`emitPatternTest()`, `CodegenPattern.cpp`): the compare subject slot holds a `TurfErrorValue*`; the test loads field 0 and `strcmp`s it with the variant's tag (`BuildErrorVariantTag()`), branching to the next header on mismatch, then — only if some sub-pattern is not `_` — loads field 2 and tests each listed field positionally out of the payload struct with the recursive `emitPatternTest()`, exactly as a struct pattern's fields are.

**`BuildBareTagString()`** (`CodegenCore.cpp`, STD-002 — this IS the pre-STD-002 `BuildErrorTag()`'s exact body, renamed) builds JUST the tag `i8*` — "MathError::DivisionByZero" — with no struct wrapping. Comparing WHICH VARIANT occurred (equality, next paragraph) never needs to look at a message, so this stays the cheap direct path it always was.

`GetErrorTagValue()` extracts a TAG STRING (field 0) for an equality operand, from whichever shape the operand is: a bare reference or a message-carrying construction both have a compile-time-known tag, built directly via `BuildBareTagString()` (skipping evaluation of the message expression entirely — `err == SomeError::Variant("msg")` must not re-run whatever produced `"msg"` just to compare identities). Anything else (`err`, or any other `AnyErrorType`/`isUserError()`-typed expression) is codegenned normally now — giving a `TurfErrorValue*` — then `LoadErrorValueField()` (shared with `err.message`'s own read, below) GEPs+loads field 0. This is also why `err == MathError::DivisionByZero` (§1.4.3) still lowers to a plain `strcmp` on two TAG strings (`getOrCreateStrcmpFn()`), never a struct-level comparison — a string is already unique per declared variant with zero whole-program variant-ID registry needed.

**`err.message`** (`FieldAccessExprAST::codegen()`, CodegenStruct.cpp, STD-002) — checked before the ordinary struct-field path (`err` is never an addressable struct base; its VALUE, not its address, already IS the `TurfErrorValue*`): codegens the base normally, then `LoadErrorValueField(ErrVal, /*FieldIdx=*/1, ...)`.

`IsErrorTagExpr()` / `IsErrorConstructionExpr()` are the structural recognizers used to dispatch on all of this: `IsErrorTagExpr` for `BinaryExprAST`'s equality special-case (below), `IsErrorConstructionExpr` for deciding a `return`ed value's wrap direction (below). Both now check `ScopeAccessKind::ErrorVariantRef` (bare) OR — STD-002 — a `FuncCallExprAST` whose callee is a `ScopeAccessExprAST` with `ResolvedKind == ErrorVariantConstruct` (or, for a variable, its Resolver-set `ResolvedSymbol->Type`) rather than any TypeChecker-derived information, per the pre-pass/main-pass tree-duplication reasoning in §2.1.

**`ReturnExprAST::codegen()`** (`Codegen.cpp:3455`) — inside a `!T`-declared function, *every* `return` — including a bare `return;` for `!void`'s success case — wraps its value into a `%FallibleValue` via `WrapFallibleValue(RawVal, IsError)` before emitting `ret`, where `IsError = IsErrorConstructionExpr(Val)`. A bare `return;` when `T` is `void` wraps `{isOk=true, successBits=0, errorPayload=null}` — the function's actual LLVM return type is never bare `void` once its declared type is `!void`, so a plain `CreateRetVoid()` would be invalid here.

**`RecoverExprAST::codegen()`** (`Codegen.cpp:4547`) — the control-flow lowering:
1. Codegens the wrapped expression, extracts `isOk`, branches to a success block and a failure block.
2. Success block: extracts `successBits`, unboxes to `T`'s real representation (skipped when `T` is `void`), branches to a merge block.
3. Failure block: extracts `errorPayload` -- STD-002: a `TurfErrorValue*`, not a bare tag string directly (§2.8's `getErrorValueType()` paragraph) -- unchanged at THIS extraction step either way, since it's still just an opaque `i8*`/`ptr` at the LLVM level. For `Produce`, codegens the produce expression and branches to merge. For `Run`/`AsRun`, `AsRun` first binds the bound name to an `AnyErrorType`-typed alloca holding `errorPayload` (the same save/restore-`NamedValues` + `GlobalSymbolTable` scope-push/pop idiom the for-loop variable already uses), then codegens the block -- this is what makes `err.message`/`err == Variant` inside that block work, both reading back through the SAME pointer.
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
5. **Codegen**: `math::divide`'s body, on its `b == 0` path, constructs `MathError::DivisionByZero` as a `TurfErrorValue{tag="MathError::DivisionByZero", message=""}` (STD-002: a pointer to a global constant, §2.8) and wraps that pointer into `{isOk=false, successBits=0, errorPayload=<TurfErrorValue*>}` for its `return`. At the call site, `RecoverExprAST::codegen()` extracts `isOk` (false), takes the failure branch, codegens `-1` as the produce value, and merges — the whole expression's runtime value is the `i64` constant `-1`.

A message-carrying variant (`return MathError::DivisionByZero("division by " + (a to string) + "/0");`) differs only in step 5: `CodegenErrorVariantConstruct()` GC-heap-allocates the `TurfErrorValue` instead of using a global constant, since the message is runtime-computed — everything else in this walkthrough (Resolve/TypeCheck/the `recover` side) is identical either way, since both shapes produce the exact same `TurfErrorValue*`-typed value.

---

## Part 4 — File map

| File | Role |
|---|---|
| `include/Lexer.h`, `src/Lexer.cpp` | `TOK_ERROR`/`TOK_RECOVER`/`TOK_PRODUCE`/`TOK_RUN`/`TOK_AS` |
| `include/AST.h`, `include/ASTAccess.h` | `ErrorDeclExprAST`, `RecoverExprAST`, `RecoverKind`, `ScopeAccessKind::ErrorVariantRef` / `ErrorVariantConstruct` (STD-002, LEC-004), `TryExprAST`, `ErrorVariantPatternExprAST` (LEC-004) |
| `include/Types.h` | `TurfUserErrorType`, `TurfErrorUnionType`, `AnyErrorType`, `isUserError()`/`isErrorUnion()`/`isAnyError()` |
| `src/Parser.cpp` | `ParseErrorDecl()`, `ParseType()`'s `!` branch, `ParseExpression()`'s `recover` dispatch |
| `include/Resolver.h`, `src/Resolver.cpp`, `src/ResolverCore.cpp`, `src/ResolverCall.cpp` | Error-type hoisting, `visitFuncCall`/`visitScopeAccess`'s error-type branches (incl. STD-002's one-message-argument construction), `CollectReturnedErrorTypeNames`, `visitFuncDef`'s `NeverReturnsErrorWarning`, `visitRecoverExpr` (incl. the bare-error restriction) |
| `include/ResolverSymbolTable.h`, `src/ResolverSymbolTable.cpp` | The error-type registry, `ResolverFunctionSymbol::PossibleErrorTypeNames` |
| `include/TypeChecker.h`, `src/TypeCheckerCall.cpp` | `visitScopeAccess`'s `ErrorVariantRef` branch, `visitFuncCall`'s `ErrorVariantConstruct` branch (STD-002, LEC-004), `visitTry`, `visitErrorDecl` (LEC-004), `visitFieldAccess`'s `AnyErrorType`/`.message` branch (STD-002), `visitReturn`'s `!T` rule and pass-through, `inferRelationalType`'s `AnyErrorType` branch, `visitRecoverExpr`, `AllPathsDiverge`, `substituteReturnType`'s `TurfErrorUnionType` branch |
| `src/CodegenCore.cpp` | `getFallibleValueType`, `getErrorValueType` (STD-002), `BoxToI64Slot`/`UnboxFromI64Slot`, `BuildErrorTag`/`BuildBareTagString`/`BuildGlobalCStringPtr`/`CodegenErrorVariantConstruct`/`getErrorPayloadStructType`/`LoadErrorValueField` (STD-002, LEC-004), `GetErrorTagValue`, `IsErrorTagExpr`/`IsErrorConstructionExpr` |
| `src/CodegenControlFlow.cpp`, `src/CodegenFunc.cpp` | `ReturnExprAST::codegen()`'s wrap logic, `RecoverExprAST::codegen()`, `FuncCallExprAST::codegen()`'s `ErrorVariantConstruct` branch (STD-002, LEC-004), `TryExprAST::codegen()`, pass-through `return` (LEC-004) |
| `src/CodegenPattern.cpp`, `src/TypeCheckerPattern.cpp`, `src/ResolverExpr.cpp`, `src/ParserStmt.cpp` | Error-variant patterns (LEC-004): parse, `normalizeHeaderPattern`/`resolveArmPattern`, `checkPattern`, `emitPatternTest` |
| `src/CodegenStruct.cpp` | `FieldAccessExprAST::codegen()`'s `err.message` branch (STD-002) |
| `src/CodegenExpr.cpp` | `BinaryExprAST::codegen()`'s equality dispatch |
| `include/Errors.h`, `include/ErrorsFlow.h` | `NeverReturnsErrorWarning`, `BareErrorAsBindingError` |
| `lib/std/io/net.tr`, `lib/std/core/math.tr`, `lib/std/collections/map.tr` | Real stdlib usage: `NetworkError` (`connect`/`bind`/`accept`), `MathError` (`divide`), `MapError` (`try_get`) |
| `tests/25_errors/` | The full regression suite for this feature |

---

## Part 5 — Known limitations, deliberately deferred

### 5.1 Per-variant payload fields — RESOLVED (LEC-004)

This section used to record why variants carried no declared fields: an earlier implementation supported typed fields end to end but was removed because nothing could *read* a payload back safely (different variants of one error type carry different fields, so direct field access cannot work), and STD-002 narrowed the gap to one universal `string` message. LEC-003 supplied the missing half — destructuring patterns in `compare`/`matches` — and LEC-004 (2026-10-01, `FREEZE.md` §6.18) builds on it: variants declare typed fields (§1.2b), a payload is read by `matches E::V(a, b):` (§1.4.4), and the runtime value carries the payload (§2.8). This was deferred item **D15** of `ARCHITECTURE_OVERHAUL.md` §27.

**What stays out of scope** (`FREEZE.md` §6.18): closed error sets in signatures (`!IOError int`), error-type conversion, stack traces (FUTURE F-15), payloads on plain `enum`s (FUTURE F-14b — the payload representation here is designed to be reused), and binding the message in a pattern (`err.message` reads it).

### 5.2 Error-type propagation — RESOLVED (LEC-004)

There used to be no shorthand: every `!T` call site handled the failure with `recover` (`recover as err run { return err; }`), and returning a `!T` call's result from a `!T` function was rejected (audit F-21). `try <expr>` (§1.10) and the pass-through `return f();` (§1.11) are the answer. Nothing implicitly converts a `!T` into a plain `T`: a `!T` can still only become a `T` through `recover` or `try`, each explicit at the call site.

### 5.3 `AllPathsDiverge` is structural, not a full CFG reachability analysis

Covered in §2.7: only `return`/`break`/`continue` are recognized as diverging. A block that only diverges via a call the compiler cannot prove never returns (`sys::exit(...)`, a user's own wrapper around it, `panic()`-style helpers) is conservatively rejected as "not diverging," even though it may be perfectly safe in practice. Closing this gap would need a `@noreturn`-style function annotation, which does not exist in this language.

### 5.4 `recover` cannot appear where its enclosing function has no declared return type context

Not actually a limitation of `recover` itself, but worth stating precisely since §1.4.2 leans on it: a `return`/`break`/`continue` inside a `recover run`/`recover as err run` block is only meaningful (and only compiles) when there genuinely is an enclosing function/loop for it to target, exactly like a bare `return`/`break`/`continue` anywhere else in the language.
