# `@caller_location` / `caller_location`

A compiler mechanism that lets a function report *its caller's* file and line,
the way Rust's `#[track_caller]` does, without unwinding or inlining-frame
complications (this language has neither). A standard-library guard such as
`List.pop` on an empty list, or a failing `test::assert()`, can then name the
line in the user's program that made the bad call, instead of a line inside the
library.

Related docs:
- `docs/architecture/errors_and_recover.md`: the error-payload machinery, an
  unrelated mechanism.
- `src/sema/Intrinsics.h` (category 1): the `caller_location` entry.
- `docs/errors/codes/E084.txt`: the user-facing page for the one diagnostic.
- `lib/std/testing/test.tr`: the original application, `assert`/`fail`.

---

## 1. Syntax and semantics

**Declaring** a caller-location-aware function: `@caller_location` immediately
before `fn`:

```turf
@caller_location
fn fail(msg: string): void {
    sys::panic(caller_location + ": " + msg);
}
```

The marker may be combined with other annotations (`@ownership(...)`, ...) on
a function, in any order among themselves; the annotations come first, then
`pub`, then `fn` (`pub @caller_location` is rejected with a hint that moves the
`pub`). An `operator` declaration takes `@caller_location` too, but only on its
own with no other annotation. `extern fn` cannot carry it: an `extern fn` has no
Turf body to read the value in. Every operator dispatch path (binary, unary,
compound assignment, postfix, index read, index write) passes the hidden
argument through `CallOperator()` (`src/codegen/CodegenCore.cpp`), stamped with
the operator expression's line. `List`'s and `Set`'s `[]`/`[]=` use it, so an
out-of-range index names the caller's line
(`tests/42_caller_location/02_caller_location_on_operators.tr`).

**Reading** the value: the bare keyword `caller_location`, legal only inside the
body of such a function, typed `string`, with the shape:

```text
<file>:<line>
```

The file is spelled as the compiler was given it (the path on the command line,
or the import's resolved path).

**Propagation is transitive and automatic.** A call from inside one
`@caller_location` function to another *forwards* the caller's own inherited
location, rather than stamping the call's own (internal) line:

```turf
@caller_location
fn assert(cond: bool, msg: string): void {
    if !cond { fail(msg); }   // forwards assert's OWN caller, not this line
}
```

A call from anywhere else (ordinary code, or a non-marked function) stamps that
call's own textual location:

```turf
test::assert(x == 5, "should be 5");   // stamps THIS line
```

No explicit forwarding syntax exists or is needed. Whether a call forwards or
stamps depends entirely on whether the *currently executing function itself*
has a `caller_location` value in scope (§3.5).

## 2. Worked example

```turf
@caller_location
fn where(): string { return caller_location; }

@caller_location
fn wrapper(): string { return where(); }

fn main(): int {
    io::printline(where());     // this line's own location
    io::printline(wrapper());   // ALSO this line, not wrapper()'s call to where()
    return 0;
}
```

Both print statements name their own `main()` call sites;
`wrapper()`'s internal call to `where()` never appears in the output. A struct
method marked the same way (`@caller_location fn P.where(): string`) behaves
identically when called as `p.where()`.
`tests/42_caller_location/01_basic_and_transitive_forwarding.tr` and
`03_caller_location_through_ufcs.tr` cover it.

## 3. Implementation, pass by pass

### 3.1 Lexer

`caller_location` is a reserved keyword, `TOK_CALLER_LOCATION`
(`src/frontend/Lexer.h`, mapped in `src/frontend/Lexer.cpp`), reserved the way
`self` (`TOK_SELF`) is. A user can never declare a variable or parameter with
this name, so there is no collision to handle downstream.

### 3.2 Parser

**The marker.** `@` starts `ParseAnnotatedDecl()` (`src/frontend/ParserDecl.cpp`,
reached from `ParsePrimaryBase()`'s `TOK_AT` case in
`src/frontend/ParserCore.cpp`). It loops over `@name(args)` annotations; the
name `caller_location` is a keyword rather than an identifier, so it is
recognised by token and sets a local `HasCallerLocation` flag instead of
producing an `Annotation`. After the annotations and an optional `pub` it
requires `fn` (delegating to `ParseFuncDef(HasCallerLocation)`) or, when
`@caller_location` stands alone, `operator`
(`ParseOperatorDecl(HasCallerLocation)`). Anything else is an
`Expected 'fn' or 'operator' after '@caller_location'` error. The flag goes
straight into `FuncDefExprAST`'s constructor (`HasCallerLocation`,
`src/frontend/ASTDecl.h`), a plain parameter and not a global or thread-local,
which keeps the parser reentrant (matters for `Sandbox.cpp`'s incremental
reparse passes).

`IsTopLevelBoundaryToken()` (`src/frontend/ParserCore.cpp`, governing
`SkipToNextTopLevelBoundary()`'s panic-mode recovery) excludes `@`. Every
failure path through `ParseAnnotatedDecl()` consumes at least the `@` before
returning, so a failed construct starting with `@` still makes forward progress,
while treating `@` as a safe place to stop would make the "already at a
boundary" guard fire before a legal `@caller_location fn ...` is parsed.

**Reading the value.** `caller_location` gets its own tiny AST node,
`CallerLocationExprAST` (`src/frontend/ASTExpr.h`), not `VariableExprAST`: the
value is never a declared Turf-level parameter (§3.3), so it has no
`ResolvedSymbol`, is never assignable, and must not be reachable through the
generic variable machinery (lvalue codegen, `self` special-casing, ...) that
assumes a declared symbol.

### 3.3 AST and Resolver: deliberately not a real parameter

`FuncDefExprAST::HasCallerLocation` is a plain flag; the hidden value is **never**
added to `Params`. Everything else follows from that choice: keeping it out of
the Turf-level, arity- and type-checked signature means overload resolution,
name mangling and generic substitution stay unaware this feature exists.
`ResolverFunctionSymbol` (`src/resolve/ResolverSymbolBase.h`) has a matching
`HasCallerLocation`, set by `Resolver::buildFunctionSymbol()`
(`src/resolve/ResolverCore.cpp`) from the AST flag. The Resolver reads it in one
place: a marked function named as a value (`Resolver::visitVariable`'s
first-class reference path, `src/resolve/ResolverExpr.cpp`) is E137, because a
`fn(...)` type has no slot for the hidden argument and an indirect call could
not pass one. It does no validation of `caller_location` usage (§3.4).
A monomorphized clone of a generic marked function keeps the flag (the clone
copies it), so `List<T>.pop` and friends work for every instantiation.

### 3.4 TypeChecker: the sole enforcement point

Mirrors the `unsafe { }` gate's shape (`docs/architecture/pointers_and_references.md`
§2.5). One bool, `InCallerLocationFunc_` (`src/sema/TypeChecker.h`), is
saved and restored around `visitFuncDef()`'s body traversal. A `fn` may be
declared lexically inside another function's body, so this has to be save and
restore, not a blind push and pop, or an inner unmarked function would inherit
the outer one's flag. `visitCallerLocation()` types the node `string` when the
flag is set, and otherwise raises `CallerLocationOutsideMarkedFunctionError`
(**E084**, `src/diagnostics/ErrorsFlow.h`).

### 3.5 Codegen

A marked function's real LLVM signature gets one extra trailing `ptr` parameter
named `"caller_location"` (`FuncDefExprAST::codegen()`,
`src/codegen/CodegenFunc.cpp`), never reflected in `Params`. The debug-info
subroutine type gets a matching `string` entry so the DWARF agrees with the real
signature. The per-parameter alloca-binding loop iterates `TheFunc->args()`
rather than `Params`, so it picks up the extra argument with one special case
(its type, `TurfString()`, since there is no `Params` entry to read it from);
the alloca, the store, the `NamedValues` registration and the `-g` variable are
the generic code every parameter goes through.

**Reading** (`CallerLocationExprAST::codegen()`) and **forwarding or stamping**
(the auto-supplied trailing argument at a call to any `HasCallerLocation`
callee) share one helper, `BuildCallerLocationValue(StampLoc)`
(`src/codegen/CodegenCore.cpp`). If `NamedValues` has a `"caller_location"`
entry (Codegen is inside the body of a function also marked), it forwards that
function's inherited value, loaded from its alloca. No separate propagation
bookkeeping exists anywhere: this single lookup against state
`FuncDefExprAST::codegen()` already saves and restores per function *is* the
transitivity mechanism (INV-CG-2 records the hidden-argument accounting).
Otherwise it stamps a compile-time `"<file>:<line>"` global string built from the
call's own `SourceLocation`, so the root of any chain always names a real
textual call site.

The hidden argument is appended at every kind of direct call, each of which
checks the resolved target's `HasCallerLocation`:

| Call form | Where |
|---|---|
| free function | `FuncCallExprAST::codegen()` (`src/codegen/CodegenFunc.cpp`) |
| module function call (`strings::substring(...)`), UFCS call (`s.substring(1, 3)`), struct method, static method | `MethodCallExprAST::codegen()` (`src/codegen/CodegenStruct.cpp`) |
| operator overload | `CallOperator()` (`src/codegen/CodegenCore.cpp`) |

The loop that binds a callee's real LLVM arguments against the caller's
`EvaluatedArgs` (the written, Turf-level arguments) bounds itself to
`EvaluatedArgs.size()` before the trailing argument is appended, otherwise it
would read past the end of an argument list that was never written that long.
The arity assertion in `MethodCallExprAST::codegen()` adds one for a marked
callee for the same reason.

## 4. Applied in the standard library

The mechanism is used where a library guard fires on a caller's mistake:

- `lib/std/testing/test.tr`: `fail(msg)` and both overloads of `assert`. `fail()`
  builds the combined message itself (`caller_location + ": " + msg`) and passes
  the formatted string to `sys::panic()` unchanged, so `sys::panic()` needs no
  knowledge of the feature.
- Collections: `List.pop`/`get`/`set` and `List`'s `[]`/`[]=`, `Stack.pop`/
  `peek`, `Queue.dequeue`/`peek`, `Map.get` and `Map`'s `[]`, `Set`'s `[]`.
- Other modules: `strings::substring`, `StringBuilder.reserve`,
  `random::random_int`, `JsonValue.get`/`at`, `Request.get_header`,
  `Server.get_route`.
  `tests/42_caller_location/fail_stdlib_*_names_caller.tr` pin the caller's line
  for `List.pop`, `Map.get`, `Map`'s `[]`, `Queue.dequeue`, `Stack.pop` and
  `substring`.

`assert_eq`, `assert_neq`, `assert_fail_eq` and `assert_fail_neq` are not marked.
Neither are the many other `sys::panic()` call sites across `lib/std`
(`mem.tr`, `net.tr`, ...). The mechanism is general; marking more guards is a
per-function change.

## 5. Known limitations

- **No way to opt out of forwarding.** A marked function calling another marked
  function always forwards; there is no syntax to force a stamp.
- **Only `fn` and `operator` declarations can be marked.** An `extern fn` has no
  Turf body to read the value in, and the parser rejects the combination.
- **The hidden parameter always occupies the last slot.** Nothing places it
  elsewhere in a signature.
- **The message format belongs to whichever function reads `caller_location`.**
  `fail()`'s `"<file>:<line>: <msg>"` is `test.tr`'s own choice, not
  compiler-enforced.
