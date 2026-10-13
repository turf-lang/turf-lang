# User-Defined `error` Types, `!T`, `try`, and `recover`

Technical reference for TurfLang's recoverable-error system. **Part 1** defines
what a TurfLang programmer observes; **Part 2** traces how the compiler
implements it, pass by pass, so the two halves can be cross-checked against the
source. The frozen decision is `docs/development/FREEZE.md` §6.18 (F-ERR-1); the
user-facing guide is `docs/reference/error_handling.md`.

An error variant is a bare name or carries typed **payload fields** declared as
part of the variant (`NotFound(path: string)`, §1.2b). Every error value also
carries one universal `string` **message** (§1.2a, `err.message`), which coexists
with payloads. A payload is read back only by destructuring patterns in a
`compare` (§1.4.4). `try` (§1.10) and the pass-through `return` (§1.11) propagate
a `!T`'s error. This is the *recoverable* channel; the other failure channel,
panics, is §1.12.

---

## Part 1 — Language semantics

### 1.1 Declaring an error type

```turflang
error IOError {
    FileNotFound,
    PermissionDenied,
    InvalidEncoding
}
```

Structurally like a C-style `enum`: a fixed set of variant names, no generics;
each variant may additionally declare payload fields (§1.2b). `IOError` is a
distinct type (`TurfUserErrorType`, §2.4), represented in the AST like a
`struct`/`enum` declaration. A duplicate type name or variant name is rejected
(E034 for a type declared twice).

**Bare form**, sugar for a single implicit variant:

```turflang
error GeneralError;
```

Equivalent to `error GeneralError { DEFAULT }`, constructed as
`GeneralError::DEFAULT`. See §1.7 for the one restriction bare-form error types
carry.

### 1.2 Referencing a variant

```turflang
MathError::DivisionByZero
```

A bare, **non-callable** reference, identical in spirit to `Color::Red` for a
plain `enum`. `MathError::DivisionByZero()` (call syntax with zero arguments) is
rejected the way `Color::Red()` is: an error variant is a value, not a function
(E135, `Not callable!`), **except** for the message-carrying construction of
§1.2a and the payload construction of §1.2b. A name that is not a variant of the
type is E039 (`No such variant`), with spelling suggestions.

```turflang
error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}
```

### 1.2a Constructing a variant with a message

```turflang
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero("division by " + (a to string) + "/0");
    }
    return a / b;
}
```

Exactly **one** `string` argument, `ErrorType::Variant(messageExpr)`, attaches a
message to this *occurrence*. `messageExpr` need not be a compile-time constant;
it may be built at run time, unlike the variant's tag, which is always known at
compile time. The variant must exist (E039 on a typo), and any other arity for a
variant with no fields is E135.

Every variant of every error type has the same one `message: string` slot,
whatever fields it declares (§1.2b). It is read back as `err.message` (§1.4.3),
a special, narrow read on an `AnyErrorType`-typed value, not general struct field
access. A bare `ErrorType::Variant` reference has no message; `err.message` on it
reads `""`, never garbage.

### 1.2b Payloads: typed fields on a variant

```turflang
error IOError { NotFound(path: string), Denied(path: string, mode: int), Timeout }
```

A variant's fields are named and typed. Any sized type is allowed (primitives,
enums, structs, arrays, generic instances) except an error type, a `!T` or `void`;
a raw-pointer field needs `unsafe` exactly as a struct field does, and a `pub`
error's field types may not be private (E089). Field names are unique within a
variant. The field spellings resolve under the type-identity rules like a struct
field's.

**Construction** is positional: a variant with *N ≥ 1* fields takes *N*
arguments, or *N+1* where the trailing `string` is the message; a variant with no
fields takes exactly one, its message. The Resolver decides the argument count
and the TypeChecker each argument's type, both as E096 (`Invalid error payload`),
naming the declared fields. A bare `E::V` for a variant that has fields is E096
too — it has no payload to fill in — except as a direct operand of `==`/`!=`
(`ScopeAccessExprAST::IsTagOperand`, set by `Resolver::visitBinary()`) and as a
pattern head. An error value's payload is an ordinary value, so storing a
reference to a local in one is an escape (E086, `pointers_and_references.md` §1.9).

**Reading** is only by pattern (§1.4.4). There is no `err.path`: which fields
exist depends on which variant occurred, and asking for one is E096.

### 1.3 `!T`: fallible return types

`!T` denotes a function that returns either a value of type `T` (success) or a
value of **any** declared error type (failure). `T` may be any type: a primitive,
a struct, a generic function's own type parameter, or `void`.

This is an **open union**, unlike Rust's `Result<T, E>`: a `!T` signature does not
name which error type(s) it can fail with, and there is no way to restrict it to a
closed set. A `!T` value cannot be used as a plain `T`; it must go through
`recover` (§1.4) or `try` (§1.10):

```turflang
mut x: int = divide(10, 2);                     // ERROR (E066): divide(...) is !int, not int
mut y: int = divide(10, 2) recover produce 0;   // OK
```

### 1.4 `recover`: the three forms

Every `recover` expression wraps a `!T` expression (the **wrapped expression**),
evaluates it, and branches on success or failure.

| Form | On success | On failure | Type of the whole expression |
|---|---|---|---|
| `expr recover produce value` | the wrapped expression's success value | `value` | `T` (both branches agree) |
| `expr recover run { BLOCK }` | the success value; `BLOCK` never runs | `BLOCK` runs instead; must diverge (§1.5) unless `T` is `void` | `T` |
| `expr recover as err run { BLOCK }` | same as `run` | same as `run`, plus `err` is bound inside `BLOCK` | `T` |

**The type of a `recover` expression is always `T`, never `!T`.** `recover` on an
expression that is not `!T` is E132 (`Invalid 'recover' use!`).

#### 1.4.1 `recover produce value`

The only form that **produces a substitute value of type `T`** on failure.
`value` is checked against `T` exactly, like any assignment or return:

```turflang
mut x: int = divide(10, 0) recover produce 0;
mut p: Point = makePoint(false) recover produce Point::new(0, 0);
```

A `value` that is not a `T` (`recover produce -1` against a `!Point`) is E066
(`'recover produce int' doesn't match this call's success type 'P'`), and
`recover produce` on a `!void` is E066 as well: there is no `void` value to
produce.

#### 1.4.2 `recover run { BLOCK }` / `recover as err run { BLOCK }`

These forms **do not produce a value on failure at all.** `BLOCK` is not "the
fallback value"; it is ordinary code, useful for a side effect, that runs
*instead of* producing anything. With no value to give the enclosing statement,
`BLOCK` must **diverge**: every path ends in `return`, `break` or `continue`
(§1.5), unless `T` is `void` (§1.6). A block that does not is E132
(`Non-diverging 'recover run'!`).

**A `return`/`break`/`continue` inside `BLOCK` operates on the function or loop
*containing* the `recover` expression**, not on the `recover` expression, and is
not checked against `T`:

```turflang
fn main(): int {
    mut p: Point = makePoint(true) recover run {
        return -1;
    };
    // ...
}
```

Here `return -1;` returns from `main()` (declared `int`), checked against
`main`'s return type, not `Point`. The assignment to `p` is abandoned on that
path; `p` never receives a value. If `p` should hold a real `Point` even on
failure, that is `recover produce` (§1.4.1). A function that wants to turn an
error into its own return value does it in `BLOCK`, so a payload is read in a
`compare` inside `BLOCK` (§1.4.4).

#### 1.4.3 `recover as err run { BLOCK }`

Identical to `recover run`, plus the bound name (conventionally `err`) is a
variable scoped to `BLOCK` and holding the error that occurred. Its type is
`AnyErrorType` (§2.4), a type-erased handle, because `!T`'s open union (§1.3)
means the concrete error type is not known statically.

`err` supports `==`/`!=` against a variant reference (§1.2) to discriminate:

```turflang
mut x: int = divide(10, 0) recover as err run {
    if err == MathError::DivisionByZero {
        io::printline("division by zero");
    }
    return -1;
};
```

Equality compares **variant identity only**, never the message or payload;
`err == MathError::DivisionByZero` is true whatever message `err` carries, and a
variant with fields may be compared by its bare name here (`IsTagOperand`).

**`err.message`** reads the `string` attached at construction (§1.2a), or `""` for
a bare reference:

```turflang
divide(10, 0) recover as err run {
    io::printline(err.message);   // "division by 10/0"
    return 1;
};
```

It is the *only* field `err` has: `err.message` is a special-cased read on
`AnyErrorType`, not struct access, and any other field name is E096
(`No such field on an error value!`).

#### 1.4.4 Reading a payload: error-variant patterns

```turflang
compare err {
    matches IOError::NotFound(path):   ...  done
    matches IOError::Denied(path, ..): ...  done
    matches IOError::Timeout:          ...  done
    otherwise:                         ...
}
```

`matches E::V(sub, ..)` matches an error value whose tag is `E::V` and whose
fields match the sub-patterns positionally; the sub-patterns are those of a
struct pattern (`docs/reference/control_flow.md`, `FREEZE.md` §6.17). It lists
every field or ends in `..`, and a variant without fields is written bare; a bare
name for one that has fields is E092 (write `E::V(..)`). The subject is an open
`AnyErrorType` (it always needs `otherwise`/`_`, E093) or a declared error type
(exhaustive when every variant is covered by an unguarded variant pattern whose
fields are all irrefutable). A variant pattern is never a catch-all. The message
is not bindable in a pattern; `err.message` reads it. Matching a non-error
subject, or another error type's variant against a declared-error subject, is
E092.

### 1.5 The divergence rule, precisely

For `recover run`/`recover as err run` with a non-`void` `T`, every control-flow
path through `BLOCK` must end in `return`, `break` or `continue`. The check is
structural, not full CFG reachability (§2.7): a block diverges when some
statement in it diverges; an `if` diverges when it has an `else` and every branch
does; a `compare` diverges when some arm is certain to run (an `otherwise`, or
arms that cover every value) and every arm body diverges. Reading a payload *is*
a `compare` over `err`, which is why `compare` counts. `while`/`for` and anything
else are assumed to fall through.

### 1.6 The `void` exception

When `T` is `void`, a `recover run`/`recover as err run` block need not diverge;
it may run to completion as an ordinary side effect:

```turflang
fn writeFile(path: string): !void { ... }

writeFile("a.txt") recover run {
    io::printline("Write failed.");
};
```

There is no value being produced either way. `recover produce` is not valid when
`T` is `void` (§1.4.1).

### 1.7 The bare-error restriction on `recover as err`

`recover as err run { ... }` is rejected (E016, `Bare error as binding`) when
**every** error type the wrapped call can fail with is bare-form (`error Name;`,
§1.1). A bare error type has one variant (`DEFAULT`), so nothing remains for
`err == ...` to discriminate; binding it is flagged as pointless:

```turflang
error GeneralError;

fn maybeFail(x: int): !int {
    if (x < 0) { return GeneralError::DEFAULT; }
    return x;
}

maybeFail(-1) recover as err run { ... };  // E016
```

`recover produce` and `recover run` stay valid on a bare-error-only call. How
"every error type this call can fail with" is determined is §2.6: a static scan of
the callee's body, not a property of its signature (§1.3).

### 1.8 The "never returns an error" warning

A `!T` function is *permitted*, not obliged, to fail. If the analysis (§2.6)
finds no path in a `!T` function's body that returns an error value, it emits
`NeverReturnsErrorWarning` (a warning, no code), suggesting the `!` may be
unnecessary. A function that passes an error on (`try`, a pass-through `return`,
or `return err;` inside `recover as err`) counts as returning one.

### 1.9 Diagnostic reference

| Diagnostic | Raised when |
|---|---|
| E135 `NotCallableError` ("Not callable!") | A variant with no fields is called with other than exactly one `string` argument |
| E039 `NoSuchVariantError` | A reference, call target or construction names an unknown variant (with suggestions) |
| E034 `DuplicateTypeNameError` | Two `error` (or `error`/`struct`/`enum`) declarations share a name in one file |
| E096 `ErrorPayloadError` | A payload declared wrongly (empty parentheses, missing type, duplicate field, a field of error/`!T`/`void` type) or constructed wrongly (argument count not *N* or *N+1*, an argument not its field's type, a trailing message not a `string`); a bare `E::V` value for a variant that has fields; `err.<field>` other than `.message` |
| E095 `TryMisuseError` | `try` at the top level, in a function not returning `!V` (`main` gets its own hint), inside a `defer`, or on a non-fallible operand |
| E092 `InvalidPatternError` / E093 `NonExhaustiveCompareError` | An error-variant pattern that lists the wrong fields, names a bare variant that has fields, has empty parentheses, or sits over a non-error or other-error-type subject; an open error union, or a declared error type with an uncovered variant, without `otherwise` |
| E132 `InvalidRecoverError` | `recover` on an expression that is not `!T` ("Invalid 'recover' use!"); a `recover run`/`as err run` block on a non-`void` `T` that does not diverge ("Non-diverging 'recover run'!", §1.5) |
| E016 `BareErrorAsBindingError` | `recover as err run` where every possible error is bare-form (§1.7) |
| E066 `Type mismatch!` | `recover produce value` where `value`'s type isn't `T`, or on a `!void`; a `!T` used to initialise or assign a `T` |
| E064 (return type mismatch) | `return f();` of a `!T` from a function that does not return `!T` (the hint names `recover` and `try`), or whose success type differs — except `return f();` of exactly the function's own `!T` (§1.11) |
| `NeverReturnsErrorWarning` | A `!T` function whose body never returns an error value (§1.8) |

### 1.10 `try`: propagation

`try <expr>` is a prefix operator (`ParseUnary()`, so `try a() + try b()` is
`(try a()) + (try b())`). The operand must be `!U`; the expression has type `U`.
On failure it returns the operand's error value **unchanged** from the enclosing
function, which must return `!V` (any `V`; the union is open, nothing converts),
after running pending `defer`s exactly as `return` does. It is sugar for
`e recover as err run { return err; }`, but it is a node of its own
(`TryExprAST`) because the desugaring would misreport: a type error would say
"return type mismatch", and the bare-error restriction on `recover as` would fire
on an error that is merely passed on.

`try` is illegal (E095) at the top level, in a function not returning `!V` (`main`,
returning `int`, included), inside a `defer` (INV-DEFER-2; counted by
`Resolver::DeferDepth_`, since `FindEscapingControlFlow()` only walks statements
and a `try` is an expression), and on a non-fallible operand. `try` is a reserved
word (`TOK_TRY`).

```turflang
fn twice(n: int): !int {
    immut v: int = try open(n);   // an error from open() leaves twice() unchanged
    return v * 2;
}
```

### 1.11 The pass-through `return`

`return f();` where `f(): !T` and the enclosing function returns exactly `!T`
returns the fallible value as it is (`ReturnExprAST::PassesThroughFallible`, set
by `TypeChecker::visitReturn()` and read by Codegen, which cannot tell it from
`return g(x);` by shape). It never unwraps or converts: nothing implicitly turns a
`!T` into a `T`, and a `!T` becomes a `T` only through `recover` or `try`, each
explicit at the call site.

### 1.12 The other failure channel: panics and caller location

A violated precondition (an index out of range, `pop` on an empty collection) is
not an error value; it is a **panic**: the library calls `sys::panic(msg)`, which
writes the message to stderr and aborts (exit status 134). So that the message
names the *caller's* mistake rather than the library line that noticed it, the
guard is declared `@caller_location` and builds its message from the hidden
`caller_location` string (`"<file>:<line>"`):

```turflang
@caller_location
fn check_index(i: int, len: int): void {
    if (i < 0 || i >= len) {
        sys::panic(caller_location + ": index out of range");
    }
}
```

A call to such a function from ordinary code stamps its own source line, and a
call from inside another `@caller_location` function forwards the inherited
location. The mechanism is documented in `docs/architecture/caller_location.md`;
the convention for which failures are `!T` and which are panics (`!T` for the
input or environment, a caller-located panic for a violated precondition) is in
`docs/reference/stdlib/README.md`. `test::assert` and `test::fail` are
`@caller_location` too.

---

## Part 2 — Internal architecture (compiler implementation)

### 2.1 Pipeline recap

```
source.tr
   │
   ▼
 Lexer ──tokens──▶ Parser ──AST──▶ Resolver (2 passes) ──annotated AST──▶ TypeChecker ──▶ Codegen (LLVM IR)
```

The driver (`src/driver/main.cpp`) runs a **pre-pass** (its own Resolver,
TypeChecker and Codegen sub-passes over function bodies) before the main pass
that the rest of the pipeline treats as authoritative; see
`compiler_pipeline.md`. Each pass runs a full Resolver→TypeChecker sequence on its
*own*, separately parsed AST, so a fact the TypeChecker records in its side table
(`NodeTypes_`) on one tree is not visible to Codegen reading the other. That is
why Codegen never reads the inferred-type map for the error system and relies on
data attached directly to AST nodes: `ScopeAccessExprAST::ResolvedKind`,
`RecoverExprAST::ResolvedSuccessType`, `TryExprAST::ResolvedSuccessType`,
`ReturnExprAST::PassesThroughFallible`.

### 2.2 Lexer

`src/frontend/Lexer.h` defines the keyword tokens `TOK_ERROR`, `TOK_RECOVER`,
`TOK_PRODUCE`, `TOK_RUN`, `TOK_AS` and `TOK_TRY` (reserved). `!` is not a new
token: it reuses the bare `!` character token also used for logical-not in
expression position. There is no ambiguity, because `ParseType()` (§2.5) is a
separate function from expression parsing.

### 2.3 AST representations

**`ErrorDeclExprAST`** (`src/frontend/ASTDecl.h`) is the declaration node,
parallel to `EnumDeclExprAST`:

```cpp
class ErrorDeclExprAST : public ExprAST, public VisibleDecl {
    std::string                   Name;
    std::vector<ErrorVariantInfo> VariantInfos;  // name + payload fields
    std::vector<std::string>      Variants;      // the names alone
    bool                          IsBareForm;
};
```

**`ScopeAccessKind::ErrorVariantRef`** (`src/frontend/ASTAccess.h`) is a value of
the existing `ScopeAccessKind` enum (the same node that represents
`EnumName::Variant`, `Module::func`, `StructName::method`). It is set on a
`ScopeAccessExprAST` whenever `Left` names a declared error type and is always
non-callable, mirroring `EnumVariant`. It is its own kind, not a reuse of
`EnumVariant`, because error values lower differently in Codegen (a
`TurfErrorValue`, not an integer ordinal, §2.8) and take part in
`!T`/`recover`/`AnyErrorType` comparisons that plain enums do not.

**`ScopeAccessKind::ErrorVariantConstruct`** is the *callable* sibling, set when
`Left::Right(...)` names a declared error type's variant with an argument count the
variant accepts (§1.2a/§1.2b). `ScopeAccessExprAST::IsTagOperand` marks a bare
reference that may name a variant with fields (an `==`/`!=` operand or a pattern
head). It is a kind of its own, not a flag on `ErrorVariantRef`, because the two
dispatch completely differently downstream: the Resolver's arity gate, the
TypeChecker's short-circuit, and Codegen's GC-heap allocate-and-store construction
versus a compile-time global (§2.6 to §2.8).

**`RecoverKind` and `RecoverExprAST`** (`src/frontend/ASTStmt.h`):

```cpp
enum class RecoverKind { Produce, Run, AsRun };

class RecoverExprAST : public ExprAST {
    std::unique_ptr<ExprAST> Wrapped;       // the !T expression
    RecoverKind              Kind;
    std::unique_ptr<ExprAST> ProduceExpr;   // Kind == Produce
    std::string              BoundName;     // Kind == AsRun
    std::unique_ptr<ExprAST> Block;         // Kind == Run / AsRun (a BlockExprAST)
public:
    TurfTypePtr ResolvedSuccessType;        // set by TypeChecker, read by Codegen
};
```

`Block` reuses the ordinary `BlockExprAST` every other `{ ... }` body uses.
**`TryExprAST`** (`ASTStmt.h`) carries the operand and a `ResolvedSuccessType`.
**`ErrorVariantPatternExprAST`** (`src/frontend/ASTExpr.h`) is the `matches E::V(...)`
header.

Constructing a variant needs **no expression node of its own**:
`Left::Right(args)` parses generically as `FuncCallExprAST(Callee =
ScopeAccessExprAST(...))` whatever `Left` is, the same shape `StructName::new(...)`
uses, and the Resolver tells the cases apart (§2.6).

### 2.4 Types

`src/sema/Types.h`. None of these is named anything close to `TurfError`
(`src/diagnostics/Errors.h`), the compiler's own diagnostic base class, an
unrelated concept that shares the English word.

**`TurfUserErrorType`**, the type of a declared `error Name { ... }`:

```cpp
class TurfUserErrorType : public TurfType {
    std::string                   Name_;
    std::vector<ErrorVariantInfo> VariantInfos_;  // name + payload fields
    std::vector<std::string>      Variants_;      // the names alone, like TurfEnumType
    bool                          IsBareForm_;
public:
    bool isUserError() const override { return true; }
    bool isBareForm() const { return IsBareForm_; }
    bool hasVariant(const std::string &) const;
    const ErrorVariantInfo *getVariantInfo(const std::string &) const;  // fields, or null
};
```

**`TurfErrorUnionType`**, the type of `!T`:

```cpp
class TurfErrorUnionType : public TurfType {
    TurfTypePtr SuccessType_;
public:
    bool isErrorUnion() const override { return true; }
    const TurfType *getSuccessType() const;
};
```

It wraps only the success type: which concrete error type occurred, if any, is a
run-time fact (§2.8), not part of the static type.

**`AnyErrorType`**, the type of `err` (§1.4.3):

```cpp
class AnyErrorType : public TurfType {
public:
    bool isAnyError() const override { return true; }
};
```

A type-erased stand-in. It is what lets `==`/`!=` against a real
`TurfUserErrorType` value type-check at all (`TypeChecker::inferRelationalType()`,
§2.7), since the two sides never share a static type by name.

### 2.5 Parser

**`ParseErrorDecl()`** (`src/frontend/ParserDecl.cpp`) is modeled on
`ParseEnumDecl()`'s variant-list loop (duplicate-variant rejection included). The
bare form (`error Name;`) desugars to a single `"DEFAULT"` variant at parse time.
A variant name may be followed by `( name: Type, ... )`, each type read with
`ParseType()`; the parser rejects only what is syntactic (empty parentheses, a
missing `: Type`, a repeated field name, all E096), and whether a type is allowed
is the Resolver's (§2.6).

The bare form's trailing `;` is self-consumed inside `ParseErrorDecl()`: every
top-level, block, module and `compare`-arm statement loop treats a *braced*
declaration as one that must **not** be followed by `;` (as for `struct`/`enum`),
so the bare form consumes its own, and no dangling `;` is left for those loops.

**`ParseType()`**'s `!T` branch (`src/frontend/ParserCore.cpp`), before every other
branch:

```cpp
if (CurTok == '!') {
    getNextToken();
    TurfTypePtr Success = ParseType();
    return std::make_shared<TurfErrorUnionType>(Success);
}
```

**`recover`** is parsed in `ParseExpression()` (`ParserCore.cpp`), bound to the
whole expression parsed so far, the same loosest-binding position as the
`give`/`otherwise` ternary. It dispatches on the keyword after `recover`
(`produce` / `run` / `as <name> run`) to build the corresponding `RecoverExprAST`
variant; a `run` block is parsed by the ordinary `ParseBlock()`.

**`try`** is parsed in `ParseUnary()` as a prefix operator; `FuncBodyTryScope`
records on the enclosing `FuncDefExprAST` (`UsesTry`) that a `try` occurs.

**Error-variant patterns** are parsed by `ParsePattern()` (`src/frontend/ParserStmt.cpp`):
`ParsePostfixExpression()` is told (`PatternHeadCallPending`, one-shot) to stop at
the `(` of the first primary of a `matches` header when it is a scope access, and
`ParseErrorVariantPatternBody()` reads the parenthesised sub-patterns and `..`.

Referencing or constructing a variant needs no parser change (§2.3).

### 2.6 Resolver

**Registry** (`src/resolve/ResolverTypeRegistry.h`, `ResolverTypeRegistry.cpp`,
reached through `ResolverSymbolTable`): `registerErrorType`, `isErrorTypeName`,
`lookupErrorType`, `getAllErrorTypeNames`, a registry parallel to the enum one,
populated during Pass 1 hoisting (`hoistTopLevel`/`hoistModuleBody`), including
inside imported module bodies.

**`visitFuncCall()`**'s error-type branch (`src/resolve/ResolverCall.cpp`). When
`Left` names a known error type it validates that the variant exists
(`NoSuchVariantError`, for any arity), then by the variant's declared field count
*N* (`TurfUserErrorType::getVariantInfo()`): *N = 0* with exactly one argument (the
message), or *N ≥ 1* with *N* or *N+1* arguments, sets
`ResolvedKind = ErrorVariantConstruct`. Any other count sets `ErrorVariantRef` and
raises E135 for a variant with no fields (mirroring the `EnumVariant` rejection
just above) or `ErrorPayloadError` (E096, listing the declared fields) for one
with fields. Arguments are resolved unconditionally at the top of
`visitFuncCall()`, before this dispatch.

**Declarations.** `Resolver::resolveDeclarationSpellingsIn()`
(`ResolverTypeIdentity.cpp`) rewrites each payload field's type spelling to its
identity in place (the `TurfType` objects are shared with every registered
`TurfUserErrorType`, as a struct's fields are). `visitErrorDecl()`
(`ResolverDecl.cpp`) validates each field type, applies the private-in-public
check to a `pub` error (E089) and rejects error, `!T` and `void` field types
(E096).

**`try`.** `Resolver::visitTry()` rejects a `try` inside a `defer` (E095,
`DeferDepth_`). `FuncDefExprAST::UsesTry` (set by the Parser) makes
`buildFunctionSymbol()` mark the function `ReturnsReboundError`: a `try` passes on
another call's error just as `return err;` in a `recover as err` block does, so
`NeverReturnsErrorWarning` does not fire and `visitRecoverExpr()`'s bare-error
check skips such a callee. `visitReturn()` notes a `return f();` whose candidates
return `!T` (`ReturnPassesFallible_`) for the same warning.

**Patterns.** `Resolver::normalizeHeaderPattern()` (`ResolverExpr.cpp`) settles a
`matches` header's `Type::Variant` spelling: a bare `E::V` over an error type
becomes an `ErrorVariantPatternExprAST`; a parsed `Name::Name(...)` pattern whose
`Name` is *not* an error type is turned back into the function call it was written
as, so `matches strings::to_upper(x):` keeps its value-pattern meaning
(`tests/45_pattern_matching/19_module_call_header_unchanged.tr`).
`resolveArmPattern()` then resolves the head as an ordinary scope access (marked
`IsTagOperand`) and each sub-pattern as a struct pattern's field is.

**`visitScopeAccess()`**'s error-type branch (`ResolverCall.cpp`): the bare
(non-call) reference sets `ResolvedKind = ErrorVariantRef` and validates the
variant exists, with suggestions on a near-miss. There are no fields to validate
here (a bare reference to a variant with fields is the TypeChecker's E096, except
as an `IsTagOperand`).

**`CollectReturnedErrorTypeNames()`** (`src/resolve/ResolverCore.cpp`) is a
conservative, non-interprocedural structural scan of a function body's `return`
statements, collecting the left-hand name of a returned `ErrorType::Variant`
reference or `ErrorType::Variant(args)` construction (a `FuncCallExprAST` whose
callee is a `ScopeAccessExprAST`). It descends through
`Block`/`If`/`While`/`For`/`ForEach` (mirroring `MutatesSelfField()`'s recursion
shape), into `VarDecl`/`Assignment` initializers, and into `RecoverExpr` (the
wrapped expression, and the `ProduceExpr` or `Block` according to `RecoverKind`).
The recover case matters because the idiomatic way for a `!T` function to surface
a *different* error than the one it called is `immut x: T = call() recover run {
return ErrorType::Variant; };` (`io::read_lines`, the `json::parse_*` functions).
Feeds `ResolverFunctionSymbol::PossibleErrorTypeNames` (`ResolverSymbolBase.h`),
computed once inside `buildFunctionSymbol()`.

When the scan is inside a `recover as err run { ... }` block and a `return` names a
bare variable equal to that block's own `BoundName` (`json::parse`'s `p.parse_value()
recover as err run { return err; };`, re-throwing whatever the wrapped call
produced), there is no error type *name* to collect, since `err`'s static type is
the erased `AnyErrorType`. That sets `ResolverFunctionSymbol::ReturnsReboundError`
instead, a separate boolean that `NeverReturnsErrorWarning`'s check treats as
"returns a real error" unconditionally.

The name list is **deliberately unfiltered** at collection time:
`buildFunctionSymbol()` runs during Pass 1 hoisting, when a later-declared error
type might not be registered yet, and a `Left::Right` shape could equally name a
struct, enum or module reference. Every consumer filters through `isErrorTypeName()`
at its own, later point (Pass 2, by which every error type is registered). Two
consumers:

1. **`NeverReturnsErrorWarning`** (§1.8), in `Resolver::visitFuncDef()` after the
   body is resolved: if the declared return type `isErrorUnion()` and neither the
   filtered `PossibleErrorTypeNames` is non-empty nor `ReturnsReboundError` /
   `ReturnPassesFallible_` is set, emit the warning. It is deduplicated by source
   location (a process-lifetime set), because the compiler resolves every function
   body twice (§2.1) and, unlike warnings emitted from Codegen, which runs once,
   this one would otherwise print twice.
2. **The bare-error restriction** (§1.7), in `Resolver::visitRecoverExpr()`, only
   for `Kind == AsRun`: reads the wrapped call's resolved target's
   `PossibleErrorTypeNames`, filters through `isErrorTypeName()`, and raises
   `BareErrorAsBindingError` if the resulting set is non-empty and every entry is
   bare-form. If the callee is ambiguous (an unresolved overload, no unique
   `ResolvedCallTarget`), has `ReturnsReboundError`, or has an empty filtered set,
   the check is skipped: conservative under-flagging, the posture `MutatesSelfField()`
   also takes for a hard-error gate.

**`visitRecoverExpr()`**'s dispatch: it resolves the wrapped expression first
(populating its callee's `ResolvedCandidates`). `Produce` resolves the produce
expression in the current scope. `Run`/`AsRun` give the block its own scope
(mirroring if/while/function bodies); `AsRun` declares the bound name as an
`AnyErrorType` variable *before* resolving the block, so ordinary lookup inside it
(the equality check of §1.4.3, the `.message` read) needs no special case.

### 2.7 TypeChecker

**`visitScopeAccess()`**'s `ErrorVariantRef` branch (`src/sema/TypeCheckerCall.cpp`)
sets the node's type from `GlobalSymbolTable->LookupType(LeftName)`, the real
`TurfUserErrorType`. A bare reference to a variant that has fields (and is not an
`IsTagOperand`) is E096.

**`visitFuncCall()`**'s `ErrorVariantConstruct` branch is short-circuited
immediately after the `DefaultConstruct` short-circuit, for the same reason: the
Resolver never populates `ResolvedCandidates` for this kind. It checks each field
argument against its declared type (`typesCompatible()`, the untyped-`int32`-literal
rule, literal representability) and that a trailing message is a `string` (E096
otherwise), then sets the call's type to the registered `TurfUserErrorType`, the
*same* type a bare reference gets. That has to be the real type, not a `NamedType`
standing in for it, because `inferRelationalType()` allows `==` against
`AnyErrorType` only for a value whose type `isUserError()`.

**`visitTry()`** rejects (E095) a `try` with no enclosing function, in a function
whose return type is not `!V` (the hint differs for `main`, via `CurrentFuncName_`),
and on an operand that is not `!U`; otherwise it stashes `U` on
`TryExprAST::ResolvedSuccessType` for Codegen and types the expression `U`.
**`visitErrorDecl()`** (`TypeCheckerDecl.cpp`) applies the `unsafe` gate to a
raw-pointer field and records generic field instantiations (a `List<string>` used
only as a payload field would otherwise never be instantiated).

**`checkPattern()`** (`src/sema/TypeCheckerPattern.cpp`) handles
`ErrorVariantPatternExprAST`: the subject must be an error (E092), the pattern's
head must belong to a declared-error subject's type, the field list must be
complete or end in `..`, and each sub-pattern is checked against its field's
declared type by the code a struct pattern's fields use (`FieldTypes` is stored for
Codegen). It always returns "refutable" (never a catch-all); `FieldsIrrefutable`
feeds `visitCompare()`'s coverage of a declared error type's variants.

**`visitFieldAccess()`**'s `AnyErrorType` branch (`TypeCheckerCall.cpp`), checked
before the ordinary struct-field machinery (an `AnyErrorType` base is never a
struct): `.message` is typed `string`; any other field name is E096.

**`visitReturn()`**'s `!T` rule (`src/sema/TypeCheckerStmt.cpp`) differs from
ordinary `typesCompatible()`: a function declared `!T` may return something
compatible with `T`, *or* a value of any declared error type (§1.3: there is no
single type to check against), decided by `isDeclaredErrorTypeValue()`. A returned
`!T` operand is accepted against exactly the declared `!T`, and the node records
`PassesThroughFallible`. `fn foo(): int { return divide(10, 0); }` (`divide`
returning `!int`) is rejected by the ordinary generic fallback with no extra code:
`!int` and `int` are not compatible by any existing rule.

**`inferRelationalType()`**'s `AnyErrorType` branch: `==`/`!=` between one side
typed `AnyErrorType` and the other `isUserError()` is allowed explicitly; neither
side can satisfy the ordinary "same `getName()`" shortcut, since `err`'s type is
erased on purpose (§2.4).

**`visitRecoverExpr()`** (`TypeCheckerCall.cpp`):
1. Visits the wrapped expression; if its type is not `isErrorUnion()`, raises
   E132 and stops.
2. `SuccessTy` = the wrapped type's success type, stashed on
   `Node.ResolvedSuccessType` (§2.3) for Codegen, which cannot rely on this
   TypeChecker instance's inferred-type map (§2.1).
3. `Produce`: visits the produce expression and checks it against `SuccessTy` via
   `typesCompatible()`; the `void`-success rejection falls out of this generically.
4. `Run`/`AsRun`: visits the block, then requires `AllPathsDiverge(Block)` unless
   `SuccessTy->isVoid()`.
5. Types the whole `RecoverExprAST` as `SuccessTy`, never `!T`.

**`AllPathsDiverge()`** (`TypeCheckerCall.cpp`) is the structural check behind §1.5:

```cpp
static bool AllPathsDiverge(const ExprAST *E) {
  // return / break / continue            -> true
  // Block: any diverging statement       -> true (the rest is unreachable)
  // If: has an else and every branch diverges
  // Compare: some arm certain to run (otherwise, or Exhaustive) and every
  //          arm body, otherwise included, diverges
  // While/For/anything else              -> false (may fall through)
}
```

It recognizes exactly `return`/`break`/`continue`. A call to `sys::exit(...)` is not
recognized, because the language has no `@noreturn` annotation and teaching the
check one function by name would be an ad-hoc special case that does not
generalize (a user's own wrapper, `abort()`). `break`/`continue` are only legal
syntax inside a loop, rejected elsewhere by an earlier check, so by the time this
sees one it is known to divert control, as `return` does.

**Generic substitution.** `substituteReturnType()` (a thin wrapper over
`Monomorphize::substituteConcreteTypes()` and the shared `TypeWalk::substitute()`)
recurses into `TurfErrorUnionType`'s success type as it does into pointers, arrays
and generic arguments, so a generic `fn<T> f(...): !T` has its `recover`'s success
type substituted to the concrete instantiation at each call site.

### 2.8 Codegen

`src/codegen/Codegen*.cpp`. This layer has the most machinery, because it is the
only pass that gives `!T` and error values a run-time representation.

**`getFallibleValueType()`** (`CodegenCore.cpp`): `!T` lowers to **one fixed LLVM
struct regardless of `T`**:

```
%FallibleValue = { i1 isOk, i64 successBits, i8* errorPayload }
```

Built once and cached, so a function's LLVM return type never varies per success
type, and `getLLVMType()` needs no per-`T` case for `!T`
(`if (T->isErrorUnion()) return getFallibleValueType();`).

**`BoxToI64Slot()` / `UnboxFromI64Slot()`** box the success value into the 64-bit
`successBits` slot: bitcast for same-width types (`double`), sign- or zero-extend and
truncate for narrower integers (`bool`, `char`, `int32`), `ptrtoint`/`inttoptr` for
pointers (`string`, generic type parameters). A value wider than 64 bits (a struct
or fixed-size array by value) is copied onto the **GC heap** (`turf_rt_alloc` with
the type's own real descriptor) and the *pointer* is boxed instead, loaded back
symmetrically on unboxing. The descriptor is what keeps a GC-heap pointer inside
the boxed `T` (a `string` field) traced; `boundary.md` §5 states the rule and
`tests/41_gc/09_boxed_fallible_success_survives_collection.tr` checks it. This is
the same "must outlive the function that returns it" reasoning as an error value
(below), applied to the success side.

**`getErrorValueType()`** (`CodegenCore.cpp`): an error value's run-time
representation is **one fixed LLVM struct whatever was attached**, the same
"one shape, no discriminated union" design as `!T` itself:

```
%TurfErrorValue = { i8* tag, i8* message, i8* payload }
```

`payload` is null for a variant with no fields, else points to a GC-allocated
per-variant struct (`CodegenErrorVariantConstruct()` below). `errorPayload` in a
`%FallibleValue` and `err`'s own alloca are still an opaque `i8*`/`ptr` at the LLVM
level; what that pointer points to is always a `TurfErrorValue`, whether the error
came from a bare reference (`message = ""`, a global constant) or a construction. A
discriminated union (bare tag string *or* struct pointer, told apart at run time)
was rejected: nothing distinguishes the two shapes without a discriminant, and
reading past a bare tag string's NUL terminator to check would be a memory-safety
hazard.

**`BuildErrorTag()`** builds a **bare** `ErrorType::Variant` reference's value: a
pointer to a global constant `TurfErrorValue{tag, message = "", payload = null}`
(only a variant with no fields is ever a bare value; one with fields is E096 except
as an `==` operand, which never builds a value). It is fully compile-time-known, so
needs no allocation. `BuildGlobalCStringPtr()` is the shared helper that builds a
`Constant*` string pointer usable as a `ConstantStruct::get()` field initializer.

**`CodegenErrorVariantConstruct()`** builds `ErrorType::Variant(fields…, [message])`.
The message and fields are in general run-time values, so it allocates on the GC
heap with `turf_rt_alloc`: a bare `malloc` would leave `message`'s GC-heap `string`
untraced and at risk of collection. For a variant with fields there are two blocks:
the **payload struct** first, a *named* LLVM struct per variant,
`errpayload.<Error>.<Variant>` (`getErrorPayloadStructType()`, built lazily inside a
function body when every struct a field can name is complete), holding the fields
by value in declaration order; then the `TurfErrorValue` with its `payload` pointing
at it. Being a named struct, the payload receives a `TurfTypeDesc` from
`EmitAllTurfTypeDescs()` with no special case, which is what lets the collector
trace a `string`, a `List<string>` or a nested struct held only in a payload
(`tests/41_gc/09_error_payload_survives_collection.tr` fails if the payload is
allocated untraced; `08_error_message_survives_collection.tr` covers the message).
Between the two allocations the payload pointer is held only in a register, which
the collector scans conservatively. It is dispatched from
`FuncCallExprAST::codegen()`'s `ErrorVariantConstruct` branch (`CodegenFunc.cpp`),
which evaluates the arguments in source order and casts each field argument to its
declared type.

**`BuildBareTagString()`** builds just the tag `i8*` (`"MathError::DivisionByZero"`)
with no struct around it. Comparing *which variant* occurred never needs a message,
so equality stays on this cheap path.

**`GetErrorTagValue()`** extracts the TAG STRING (field 0) for an equality operand
from whichever shape the operand is. A bare reference or a message-carrying
construction has a compile-time-known tag, built directly via
`BuildBareTagString()` (skipping evaluation of the message expression:
`err == SomeError::Variant("msg")` must not re-run whatever produced `"msg"` just to
compare identities). Anything else (`err`, or any `AnyErrorType`/`isUserError()`
expression) is code-generated normally, giving a `TurfErrorValue*`, and
`LoadErrorValueField()` loads field 0. `LoadErrorValueField()` is shared with
`err.message`'s read and the pattern test. So `err == MathError::DivisionByZero`
lowers to a plain `strcmp` on two tag strings (`getOrCreateStrcmpFn()`), never a
struct comparison: a tag string is unique per declared variant with no whole-program
variant-ID registry.

**`err.message`** (`FieldAccessExprAST::codegen()`, `CodegenStruct.cpp`) is checked
before the ordinary struct-field path (`err` is never an addressable struct base;
its *value* already is the `TurfErrorValue*`): it code-generates the base normally,
then `LoadErrorValueField(ErrVal, /*FieldIdx=*/1, ...)`.

`IsErrorTagExpr()` and `IsErrorConstructionExpr()` (`CodegenCore.cpp`) are the
structural recognizers for dispatch: `IsErrorTagExpr` for `BinaryExprAST`'s
equality special case, `IsErrorConstructionExpr` for deciding a `return`ed value's
wrap direction. Both check a bare `ErrorVariantRef`, or a `FuncCallExprAST` whose
callee is a `ScopeAccessExprAST` with `ResolvedKind == ErrorVariantConstruct` (or,
for a variable, its Resolver-set `ResolvedSymbol->Type`), using Resolver data rather
than the TypeChecker's, per §2.1.

**Variant patterns** (`emitPatternTest()`, `CodegenPattern.cpp`): the compare
subject slot holds a `TurfErrorValue*`; the test loads field 0 and `strcmp`s it with
the variant's tag (`BuildErrorVariantTag()`), branching to the next header on
mismatch; then, only if some sub-pattern is not `_`, loads field 2 and tests each
listed field positionally out of the payload struct with the recursive
`emitPatternTest()`, exactly as a struct pattern's fields are.

**`ReturnExprAST::codegen()`** (`CodegenControlFlow.cpp`): inside a `!T` function,
*every* `return`, including a bare `return;` for `!void`'s success case, wraps its
value into a `%FallibleValue` via `WrapFallibleValue(RawVal, IsError)` before `ret`,
where `IsError = IsErrorConstructionExpr(Val)`. A bare `return;` when `T` is `void`
wraps `{isOk = true, successBits = 0, errorPayload = null}`: the function's LLVM
return type is never bare `void` once its declared type is `!void`. A pass-through
`return` (`PassesThroughFallible`) emits `ret` of the operand's `%FallibleValue`
directly instead of wrapping it again.

**`try`** (`TryExprAST::codegen()`, `CodegenControlFlow.cpp`, beside
`ReturnExprAST`): evaluates the `!U` operand and branches on `isOk`. The failure
block extracts `errorPayload`, runs `FlushPendingDefers(0)` (hence the placement:
that helper is file-static), and returns `WrapFallibleValue(err, true)`; the success
block unboxes `U` from `successBits` (`ResolvedSuccessType`) and is where the
builder is left. The error pointer travels as-is, so tag, payload and message arrive
unchanged.

**`RecoverExprAST::codegen()`** (`CodegenFunc.cpp`), the control-flow lowering:
1. Code-generates the wrapped expression, extracts `isOk`, and branches to a success
   block and a failure block.
2. Success block: extracts `successBits`, unboxes to `T`'s real representation
   (skipped when `T` is `void`), and branches to a merge block.
3. Failure block: extracts `errorPayload` (a `TurfErrorValue*`, opaque at the LLVM
   level). For `Produce`, code-generates the produce expression (cast to `T`) and
   branches to merge. For `Run`/`AsRun`, `AsRun` first binds the bound name to an
   `AnyErrorType` alloca holding `errorPayload` (the same save/restore-`NamedValues`
   plus `GlobalSymbolTable` scope idiom the `for` loop variable uses), then
   code-generates the block; this is what makes `err.message` and `err == Variant`
   inside the block work, both reading through the same pointer.
4. After the block: if the current insert block has no terminator, branch to merge.
   That is the live path for the `void`-success non-diverging case (§1.6). It is
   also what happens to the unreachable "after return" block that
   `ReturnExprAST::codegen()` leaves as the insert point after a diverging
   statement, which becomes a harmless extra predecessor on the merge block's
   `PHINode`. Branching it to merge, rather than special-casing it away, avoids
   re-implementing `AllPathsDiverge`'s analysis here in a form that could drift out
   of sync with it.
5. Merge block: for a `void` success type, a dummy value (never used). Otherwise a
   `PHINode` combines whichever paths reached the merge block: the success path
   always, and the failure path only if it did not diverge.

**`BinaryExprAST::codegen()`**'s equality dispatch (`CodegenExpr.cpp`) is checked
before the ordinary short-circuit and generic operator paths: if either side of
`==`/`!=` is error-tag-shaped (`IsErrorTagExpr`), both sides' tags are obtained
through `GetErrorTagValue()` and compared with `strcmp`. Without this, both sides
would already code-generate to plain `i8*` values, and pointer-identity comparison
would give the wrong answer for two logically equal but differently allocated error
references.

### 2.9 Diagnostics

The error-system diagnostics follow the codebase's usual `...Error : public
TurfError` / `...Warning : public TurfWarning` convention (the naming collision with
the user-facing `error` keyword is in appearance only, §2.4): `NeverReturnsErrorWarning`
(`src/diagnostics/ErrorsFlow.h`), `BareErrorAsBindingError` (`ErrorsMemory.h`),
`ErrorPayloadError` and `TryMisuseError` (`ErrorsPayload.h`), `NoSuchVariantError`
(`ErrorsScope.h`), and the pattern errors (`ErrorsPattern.h`). The rest reuse the
generic `TypeError`/`LogErrorAt`/`WrongArityError` machinery, as most struct and
enum diagnostics do.

---

## Part 3 — End-to-end walkthrough: `math::divide(10, 0) recover produce -1;`

1. **Parse**: `math::divide(10, 0)` is an ordinary `FuncCallExprAST` (no
   `error`-specific parsing at all, §2.5). `recover produce -1` is parsed as
   `RecoverExprAST(Wrapped = <the call>, Kind = Produce, ProduceExpr = <-1>)`, bound
   to the whole call expression.
2. **Hoist (Pass 1)**: `math::divide`'s signature (`(int, int): !int`) is registered
   like any function; `MathError`'s `DivisionByZero` variant is registered in the
   error-type registry (§2.6) wherever `math.tr`'s `pub error MathError {
   DivisionByZero }` declaration is hoisted.
3. **Resolve (Pass 2)**: the call resolves normally (overload and arity matching,
   nothing error-specific). `Resolver::visitRecoverExpr()` resolves the wrapped
   call, then the produce expression (`Kind == Produce`, so no block or bound-name
   handling).
4. **TypeCheck**: the call's type is `!int` (`TurfErrorUnionType(TurfInt())`).
   `TypeChecker::visitRecoverExpr()` unwraps to `SuccessTy = int`, stashes it on
   `Node.ResolvedSuccessType`, checks `-1` against `int`, and types the whole
   `RecoverExprAST` as `int`.
5. **Codegen**: `math::divide`'s body, on its `b == 0` path, builds
   `MathError::DivisionByZero` as a pointer to the global constant
   `TurfErrorValue{tag = "MathError::DivisionByZero", message = "", payload = null}`
   and wraps it into `{isOk = false, successBits = 0, errorPayload = <ptr>}` for its
   `return`. At the call site `RecoverExprAST::codegen()` extracts `isOk` (false),
   takes the failure branch, code-generates `-1` as the produce value, and merges;
   the whole expression's run-time value is the `i64` constant `-1`.

A message-carrying variant (`return MathError::DivisionByZero("division by " + (a to
string) + "/0");`) differs only in step 5: `CodegenErrorVariantConstruct()`
GC-heap-allocates the `TurfErrorValue` instead of using a global constant, since the
message is computed at run time. A variant with payload fields additionally
allocates the per-variant payload struct. The Resolve, TypeCheck and `recover` sides
are identical either way, because every shape yields the same `TurfErrorValue*`-typed
value.

---

## Part 4 — File map

| File | Role |
|---|---|
| `src/frontend/Lexer.h`, `Lexer.cpp` | `TOK_ERROR`/`TOK_RECOVER`/`TOK_PRODUCE`/`TOK_RUN`/`TOK_AS`/`TOK_TRY` |
| `src/frontend/ASTDecl.h`, `ASTStmt.h`, `ASTExpr.h`, `ASTAccess.h` | `ErrorDeclExprAST`; `RecoverExprAST`, `RecoverKind`, `TryExprAST`; `ErrorVariantPatternExprAST`; `ScopeAccessKind::ErrorVariantRef` / `ErrorVariantConstruct`, `IsTagOperand` |
| `src/sema/Types.h` | `TurfUserErrorType`, `TurfErrorUnionType`, `AnyErrorType`, `isUserError()`/`isErrorUnion()`/`isAnyError()` |
| `src/frontend/ParserDecl.cpp`, `ParserCore.cpp`, `ParserStmt.cpp` | `ParseErrorDecl()`; `ParseType()`'s `!` branch, `ParseExpression()`'s `recover` dispatch, `ParseUnary()`'s `try`; `ParsePattern()`, `ParseErrorVariantPatternBody()` |
| `src/resolve/ResolverCall.cpp`, `ResolverCore.cpp`, `ResolverDecl.cpp`, `ResolverStmt.cpp`, `ResolverExpr.cpp`, `ResolverTypeIdentity.cpp` | `visitFuncCall`/`visitScopeAccess` error-type branches; `visitRecoverExpr` (incl. the bare-error restriction); `visitTry`; `CollectReturnedErrorTypeNames`; `visitFuncDef`'s `NeverReturnsErrorWarning`; `visitErrorDecl`; `visitReturn`; `normalizeHeaderPattern`/`resolveArmPattern`; `resolveDeclarationSpellingsIn` |
| `src/resolve/ResolverTypeRegistry.h`, `ResolverTypeRegistry.cpp`, `ResolverSymbolBase.h` | The error-type registry; `ResolverFunctionSymbol::PossibleErrorTypeNames`, `ReturnsReboundError` |
| `src/sema/TypeCheckerCall.cpp`, `TypeCheckerStmt.cpp`, `TypeCheckerDecl.cpp`, `TypeCheckerPattern.cpp`, `TypeCheckerExpr.cpp` | `visitScopeAccess`, `visitFuncCall`'s construction branch, `visitFieldAccess`'s `AnyErrorType` branch, `visitTry`, `visitRecoverExpr`, `AllPathsDiverge`; `visitReturn` and `isDeclaredErrorTypeValue`; `visitErrorDecl`; `checkPattern`; `inferRelationalType` |
| `src/codegen/CodegenCore.cpp` | `getFallibleValueType`, `getErrorValueType`, `BoxToI64Slot`/`UnboxFromI64Slot`, `BuildErrorTag`, `BuildBareTagString`, `BuildGlobalCStringPtr`, `CodegenErrorVariantConstruct`, `getErrorPayloadStructType`, `LoadErrorValueField`, `GetErrorTagValue`, `IsErrorTagExpr`/`IsErrorConstructionExpr`, `WrapFallibleValue` |
| `src/codegen/CodegenControlFlow.cpp`, `CodegenFunc.cpp` | `ReturnExprAST::codegen()`, `TryExprAST::codegen()`; `RecoverExprAST::codegen()`, `FuncCallExprAST::codegen()`'s `ErrorVariantConstruct` branch |
| `src/codegen/CodegenPattern.cpp`, `CodegenStruct.cpp`, `CodegenExpr.cpp` | `emitPatternTest`; `err.message`; `BinaryExprAST::codegen()`'s equality dispatch |
| `src/diagnostics/ErrorsFlow.h`, `ErrorsMemory.h`, `ErrorsPayload.h`, `ErrorsScope.h`, `ErrorsPattern.h` | The diagnostics of §2.9 |
| `lib/std/core/math.tr`, `lib/std/collections/map.tr`, `lib/std/io/net.tr`, `lib/std/core/io.tr` | Real use: `MathError` (`divide`), `MapError` (`try_get`), `NetworkError`, `FileError` |
| `tests/25_errors/` | The regression suite for this feature |

---

## Part 5 — Limits, by design

### 5.1 What stays out of scope

Closed error sets in signatures (`!IOError int`), error-type conversion, stack
traces, payloads on plain `enum`s, and binding the message in a pattern
(`err.message` reads it). `FREEZE.md` §6.18 states them. The payload representation
(§2.8) is designed to be reused if enum payloads are added.

### 5.2 No implicit unwrapping

A `!T` becomes a `T` only through `recover` or `try`, each explicit at the call
site; the pass-through `return` (§1.11) hands the `!T` on unchanged and converts
nothing.

### 5.3 `AllPathsDiverge` is structural

Only `return`/`break`/`continue`, `if`-with-`else` and a covering `compare` are
recognized (§2.7). A block that only diverges through a call the compiler cannot
prove never returns (`sys::exit(...)`, a user's own wrapper, a `panic()`-style
helper) is conservatively rejected as not diverging, even if safe in practice.
Closing this gap needs a `@noreturn`-style annotation, which the language does not
have.

### 5.4 `recover` needs an enclosing function or loop

A `return`/`break`/`continue` inside a `recover run`/`recover as err run` block is
only meaningful, and only compiles, when there is an enclosing function or loop for
it to target, exactly as anywhere else in the language (§1.4.2).
