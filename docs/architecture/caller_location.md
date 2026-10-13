# `@caller_location` / `caller_location`

STD-002 (`docs/archive/ARCHITECTURE_OVERHAUL.md` §8/§15.6(a)): a compiler
mechanism letting a function report *its caller's* file and line, the way
Rust's `#[track_caller]`/`Location::caller()` does, without unwinding or
inlining-frame complications (this language has neither). Fixes the
concrete problem `sys::panic`-based stdlib guards had: a failing
`test::assert()` used to print nothing but its message text — no clue
which of possibly hundreds of calling test files actually failed, since the
panic path had no notion of "caller" at all, only "this line, right here in
test.tr."

Related docs:
- `docs/architecture/errors_and_recover.md` — the OTHER half of STD-002
  (§15.6(c), error payloads); unrelated mechanism, same feature group.
- `src/sema/Intrinsics.h` §1 — the `caller_location` bare-intrinsic entry.
- `lib/std/testing/test.tr` — the real, shipped application: `assert`/`fail`
  are both `@caller_location`.

---

## 1. Syntax and semantics

**Declaring** a caller-location-aware function — `@caller_location`
immediately before `fn`:

```turf
@caller_location
fn fail(msg: string): void {
    sys::panic(caller_location + ": " + msg);
}
```

An `operator` declaration takes it the same way (`@caller_location` then
`operator`), with no other annotation beside it. Every operator dispatch path
(binary, unary, compound assignment, postfix, index read, index write) passes
the hidden argument through `CallOperator()` (CodegenCore.cpp), stamped with
the operator expression's line. `List`'s and `Set`'s `[]`/`[]=` use it, so an
out-of-range index names the caller's line (`tests/42_caller_location/
02_caller_location_on_operators.tr`).

**Reading** the value — the bare keyword `caller_location`, legal only
inside such a function's body, typed `string`:

```turf
"<file>:<line>"
```

**Propagation is transitive and automatic.** A call from inside one
`@caller_location` function to ANOTHER `@caller_location` function
*forwards* the caller's own inherited location, rather than stamping the
call's own (internal) line:

```turf
@caller_location
fn assert(cond: bool, msg: string): void {
    if !cond { fail(msg); }   // forwards assert's OWN caller -- not this line
}
```

A call from anywhere else (ordinary code, or a non-marked function) STAMPS
that call's own textual location:

```turf
test::assert(x == 5, "should be 5");   // stamps THIS line
```

No explicit forwarding syntax exists or is needed — whether a given call
forwards or stamps falls entirely out of whether the CURRENTLY EXECUTING
function itself has a `caller_location` value in scope (see §3.4).

## 2. Worked example

```turf
@caller_location
fn where(): string { return caller_location; }

@caller_location
fn wrapper(): string { return where(); }

fn main(): int {
    io::printline(where());     // this line's own location
    io::printline(wrapper());   // ALSO this line -- not wrapper()'s internal call to where()
    return 0;
}
```

Verified live (this session): both print statements name their OWN
respective `main()` call sites; `wrapper()`'s internal call to `where()`
never appears anywhere in the output. See
`tests/42_caller_location/01_basic_and_transitive_forwarding.tr`.

## 3. Implementation, pass by pass

### 3.1 Lexer

One new keyword, `TOK_CALLER_LOCATION` (`src/frontend/Lexer.h`), mapped from the
literal `caller_location` — reserved exactly like `self` (`TOK_SELF`) is,
so a user can never declare a variable or parameter with this name (no
collision risk to worry about anywhere downstream).

### 3.2 Parser

**The `@caller_location` marker** — `ParsePrimaryBase()`'s `TOK_AT` case
(`src/frontend/ParserCore.cpp`) is the ONLY legal use of `@` in this language (every
other `@word` still gets the pre-existing "no decorator syntax" rejection,
unchanged): consumes `@`, then `caller_location`, then requires `fn` to
follow, then delegates to `ParseFuncDef(/*HasCallerLocation=*/true)`.
`ParseFuncDef()` threads this bool straight into `FuncDefExprAST`'s
`HasCallerLocation` field (`src/frontend/ASTDecl.h`) — a plain constructor
parameter, not a global/thread-local flag, keeping the parser reentrant-safe
the same way every other parameter here already is (matters for
`Sandbox.cpp`'s incremental reparse passes).

`src/frontend/Parser.h`'s `IsTopLevelBoundaryToken()` doc comment (governing
`SkipToNextTopLevelBoundary()`'s panic-mode recovery) was updated to match:
`@` is still excluded from the boundary-token set, but the REASON changed —
every failure path through the new `TOK_AT` case still consumes at least
the `@` token itself before returning, so forward progress is still
guaranteed even though "zero tokens consumed on failure" (the pre-STD-002
invariant) no longer holds for the ONE now-legal spelling.

**Reading the value** — `caller_location` gets its OWN tiny AST node,
`CallerLocationExprAST` (`src/frontend/ASTExpr.h`), rather than falling through
to the ordinary `VariableExprAST`/`self` path: the value it reads is never a
real Turf-level declared parameter (see §3.3 for why), so it has no
`ResolvedSymbol`, is never assignable, and must not be reachable through any
of the generic `VariableExprAST` machinery (LValue codegen, name-based
`self` special-casing, …) that assumes a real declared symbol exists.

### 3.3 AST / Resolver — deliberately NOT a real parameter

`FuncDefExprAST::HasCallerLocation` is a plain bool flag — the hidden value
is **never** added to `Params`. This is the single design decision
everything else follows from: keeping it out of the Turf-level, arity- and
type-checked signature means ordinary overload resolution, mangling, and
generic substitution all stay completely unaware this feature exists.
`ResolverFunctionSymbol` (`src/resolve/ResolverSymbolBase.h`) gains a matching
`HasCallerLocation` bool, populated by `buildFunctionSymbol()`
(`src/resolve/ResolverCore.cpp`) straight from the AST flag — this is the ONLY
Resolver-visible trace of the feature; Resolver does no validation of
`caller_location`'s own usage at all (see §3.4 for where that lives).

### 3.4 TypeChecker — the sole enforcement point

Mirrors the `unsafe { }` gate's own shape exactly (`checkUnsafeGate()`,
`docs/architecture/pointers_and_references.md` §2.5): TypeChecker alone
decides whether `caller_location` is legal here, via one bool member,
`InCallerLocationFunc_` (`src/sema/TypeChecker.h`), save/restored around
`visitFuncDef()`'s body traversal (a `fn` CAN be declared lexically inside
another function's body block in this language — `ParseBlock()` accepts a
nested `fn` as an ordinary statement — so this must be save/restore, not a
blind push/pop, to avoid an inner non-marked function inheriting the outer
one's flag). `visitCallerLocation()` sets the node's type to `string` when
`InCallerLocationFunc_`, else raises `CallerLocationOutsideMarkedFunctionError`
(**E084**, `src/diagnostics/ErrorsFlow.h`).

### 3.5 Codegen — the whole mechanism, in one paragraph

A `@caller_location` function's REAL LLVM signature gets one extra trailing
`i8*`/`ptr` parameter named `"caller_location"`
(`FuncDefExprAST::codegen()`, `src/codegen/CodegenFunc.cpp`) — never reflected in
`Params`, so the Turf-level signature Resolver/TypeChecker/mangling see is
completely unaffected. The existing per-parameter alloca-binding loop
already iterates `TheFunc->args()` (not `Params`), so it picks up this
extra argument for free with one special case (its TYPE, `TurfString()`,
since there is no `Params` entry to read one from) — everything else
(alloca, store, `NamedValues` registration, `-g` debug-info) is the exact
same generic code every ordinary parameter already goes through.

**Reading** (`CallerLocationExprAST::codegen()`) and **forwarding/stamping**
(`FuncCallExprAST::codegen()`'s auto-supplied trailing argument, for a call
to any `HasCallerLocation` callee) both funnel through one shared helper,
`BuildCallerLocationValue(StampLoc)` (`src/codegen/CodegenCore.cpp`): if
`NamedValues` currently has a `"caller_location"` entry (i.e. Codegen is
inside the body of a function ALSO declared `@caller_location`), forwards
that function's own inherited value, loaded straight out of its alloca — no
separate propagation bookkeeping exists ANYWHERE; this single lookup
against state `FuncDefExprAST::codegen()` already saves/restores per
function IS the entire transitivity mechanism. Otherwise, stamps a
compile-time `"<file>:<line>"` global string constant built from the call's
own `SourceLocation` — so the ROOT of any propagation chain always names a
real, textual call site, never a placeholder.

The call-site injection (`FuncCallExprAST::codegen()`, CodegenFunc.cpp)
required one structural change: the loop binding a callee's real LLVM args
against the caller's `EvaluatedArgs` (Turf-level, user-written arguments)
now explicitly bounds itself to `EvaluatedArgs.size()` before appending the
auto-supplied trailing argument separately — without that bound, the loop
would walk off the end of `EvaluatedArgs` trying to satisfy the extra LLVM
parameter from an argument list that was never written that long.

## 4. Applied in the standard library

`lib/std/testing/test.tr`'s `fail(msg)` and both overloads of
`assert(cond[, msg])` are `@caller_location`. `fail()`'s body builds the
combined message itself (`caller_location + ": " + msg`) and passes the
already-formatted string to `sys::panic()` UNCHANGED — `panic()` and every
OTHER `sys::panic()` call site across `lib/std` (`mem.tr`, `net.tr`,
`list.tr`, …) needed zero changes, deliberately: this is a narrow,
disclosed scope boundary, not an oversight (see
`docs/development/changes/STD-002.md`'s *Removed assumptions*).
`assert_eq`/`assert_neq`/`assert_fail_eq`/`assert_fail_neq` remain
unmarked — retrofitting them (and the REST of `lib/std`'s many other
`sys::panic()` call sites) is real future work, not part of this group's
scope, and the mechanism is fully general and ready for it whenever a
future session takes it on.

## 5. Known limitations, deliberately out of scope

- **No way to opt out of forwarding.** A `@caller_location` function calling
  another `@caller_location` function ALWAYS forwards; there is no syntax
  to force a stamp instead. Nothing in this feature group needed one.
- **Only free functions and struct methods can be marked** — the parser
  requires `@caller_location` to precede `fn` specifically; there is no
  equivalent for, say, an `extern fn` (which has no body to read the value
  in anyway) or any other declaration shape.
- **The hidden parameter always occupies the LAST slot.** No mechanism
  exists (or is needed) to place it elsewhere in a function's signature.
- **A message-formatting decision belongs to whichever function reads
  `caller_location`**, not to the mechanism itself — `fail()`'s
  `"<file>:<line>: <msg>"` shape is `test.tr`'s own choice, not a
  compiler-enforced format.
