# Code Review Gaps — 2026-07 Session

Compiled during a full file-by-file code review pass (Lexer → Parser → Resolver →
TypeChecker → Codegen) plus a targeted enum/`compare`-`matches` probing pass.
This is a **gap inventory to fix later**, not a bug tracker for issues already
fixed during the review (those were fixed inline, given regression tests, and
are not repeated here — see git history and `tests/*/` for what changed).

Severity key:
- **CRITICAL** — silently wrong/dropped behavior, no diagnostic anywhere.
- **HIGH** — a whole feature/type of program is unusable, but at least it's a
  compile error (not silent).
- **MEDIUM** — a real, reachable gap, but with a low-cost workaround.
- **LOW** — usability/ergonomics; nothing is unsafe, just missing polish.

Each entry lists **Where** (file/function) so this doc can be used directly as
a punch list — grep the function name to start.

---

## CRITICAL (found completing gap #5, the stdlib rewrite)

### 0. A method call on an instance of a struct declared in an IMPORTED module silently compiled to nothing — FIXED 2026-07-24
**Where:** `src/TypeChecker.cpp`, `visitMethodCall()`'s struct-method-call block.

**This is the single most severe bug found this entire session.** Discovered
while finally testing gap #5's stdlib rewrite (`import list; ... l.push(10);
l.pop(); ...`) end-to-end — every test looked like it passed (exit 0, no
errors) purely because **nothing was actually running**.

**Repro:**
```turf
// mymod.tr
struct Box { val: int }
fn Box.trap(): void { fail("should abort"); }
```
```turf
// user.tr
import mymod;
fn main(): int {
    mut b: Box = Box { val: 0 };
    b.trap();
    return 0;
}
```
Compiles clean, runs, exits 0 — `b.trap()` never executes at all, despite
`Box.trap()`'s own generated LLVM IR being completely correct in isolation
(confirmed via `--emit-llvm`: it correctly calls `fprintf`+`abort`). The bug
is in the **caller**: `--emit-llvm` on `main()` shows its body is just
`ret i64 0` — the `VarDecl` and the method call are both silently absent.

**Root cause:** `TypeChecker::visitMethodCall()`'s struct-method resolution
looked up candidates via `RST_.lookupCurrentFileFunctions(StructName + "." +
MethodName)` **only**. A struct method declared inside an *imported* module
is hoisted into the *module* registry instead
(`Resolver::hoistModuleBody()` → `registerFunctionSignature()` →
`registerModuleFunction()`, since the module name is non-empty there) —
`lookupCurrentFileFunctions()` can never find it, full stop, regardless of
struct name, arity, or anything else. `ArityMatches` was always empty,
`Node.ResolvedCallTarget` stayed null, and the function fell through to "be
permissive" (`setType(&Node, nullptr)`). Codegen's
`MethodCallExprAST::codegen()` then hit `if (!CalleeF) return nullptr;`
**with no diagnostic at all** for this path (unlike the module-qualified
call path, which at least has `llvm::report_fatal_error` for a missing
function) — and `BlockExprAST::codegen()`'s already-documented "never checks
for a null statement result" behavior (see gap #1's "related systemic
finding") silently dropped both the failed call statement and, in most real
programs, the `VarDecl` that depended on its result too.

**Blast radius:** this affected **every struct declared in any imported
module, called from any other file** — not just generic structs (that's a
separate, additional gap, see #0b below). Any object-oriented Turf code
spanning more than one file was silently non-functional. This is why
`list.tr`/`stack.tr`/`queue.tr` (and by extension anything built on them)
had literally never been exercised successfully before this session, despite
looking syntactically fine once the earlier syntax-staleness issues were
fixed.

**Fix:** also search `RST_.findInModules(StructName + "." + MethodName)`
and merge those candidates in. Regression tests:
`tests/07_functions_modules/06_stdlib_list.tr`,
`07_stdlib_stack.tr`, `08_stdlib_queue.tr` — each **re-verified via
`--emit-llvm`**, not just exit code, specifically because this failure mode
so convincingly fakes success.

### 0b. A generic struct method's return type wasn't substituted when GlobalSymbolTable's struct copy diverges from the AST's — FIXED 2026-07-24
**Where:** `src/TypeChecker.cpp`, `visitMethodCall()`'s struct-method-call block.

Once gap #0 above was fixed, calling `l.pop(): T` on a `List<int>` compiled
and ran, but `l.pop() == 30` failed type-checking with "Cannot compare 'T'
with 'int'" — the same literal-generic-return-type gap as gap #3, but gap
#3's fix (`collectGenericSubstitution`/`substituteReturnType` in
`visitFuncCall()`) only unifies against a callee's own *parameter* types,
and a struct method's `self` parameter carries no type arguments of its own
(it's a bare `StructName*`, see `ParseFuncDef`'s self-parameter
construction) — the concrete type argument only exists on the *base
expression's* type (`l: List<int>`).

Implementing that substitution surfaced a second, deeper issue: the natural
approach is to scan the struct's own fields for `TurfGenericParamType`
occurrences to recover the type parameter's name — but this doesn't work,
because **`GlobalSymbolTable`'s registered `TurfStructType` for a generic
struct is a separate object from the `StructDeclExprAST` AST node**,
constructed once at parse time (`ParseStructDecl`) directly from the
as-parsed fields. `Resolver::visitStructDecl()`'s substitution (from this
session's earlier generics work, gap #2) mutates the **AST node's** copy of
`Fields` in place — nothing propagates that back into
`GlobalSymbolTable`'s already-constructed, separate `TurfStructType`, so
its fields are permanently stuck with the original, unsubstituted
`NamedType("T")`. This is the same "two symbol tables" architecture
(`ResolverSymbolTable` vs. the LLVM-aware `SymbolTable`/`GlobalSymbolTable`)
that has caused multiple divergence bugs already this session — see the
`project_generics_namedtype_gap` memory note.

**Fix:** scan for `NamedType` occurrences that don't resolve to any known
struct/enum via `GlobalSymbolTable->LookupType()` (rather than
`TurfGenericParamType` occurrences, which will never be found there),
treating an unresolved name as a type-parameter candidate. Exact for every
generic struct in this language's stdlib today (all single-type-parameter);
not fully general for a hypothetical multi-parameter struct whose fields
don't mention every parameter in declaration order. Same regression tests
as gap #0 (all three stdlib collections are generic).

---

## CRITICAL

### 1. `FieldAccessExprAST` silently drops field access on an r-value base — FIXED 2026-07-23
**Where:** `src/Codegen.cpp`, `FieldAccessExprAST::codegenLValue()` and
`FieldAccessExprAST::codegen()`.

**Status: fixed.** `codegenLValue()` now has the same r-value fallback
`ArrayAccessExprAST`/`MethodCallExprAST` already had: if `Base->codegenLValue()`
returns null, evaluate `Base->codegen()` instead, spill the result into a
temp alloca, and GEP into that. Regression tests:
`tests/06_structs/20_rvalue_field_to_var.tr` (new), and
`tests/06_structs/16_struct_rvalue.tr` (pre-existing — see the note under
"Related systemic finding" below: this test was silently not testing
anything until this fix landed).

**Related systemic finding (investigated, NOT fixed — reverted):**
`BlockExprAST::codegen()` (`src/Codegen.cpp`) never checks whether a
statement's `codegen()` call returned null — it just assigns to `LastVal`
and continues to the next statement unconditionally. This is the reason the
bug above was silent instead of at least aborting compilation: a null
`Value*` from a failing statement (by convention elsewhere in this file,
"an error was already reported downstream") just gets dropped on the floor,
and the block moves on as if that statement never existed. This is a
systemic risk — *any* codegen path that returns nullptr without first
raising a diagnostic (as `FieldAccessExprAST` did) produces a silently
dropped statement, not a compile error. Tried hardening this
(`if (!LastVal) { ExitScope(); return nullptr; }` immediately after the
codegen call, stopping further statements in the block) — **this broke the
test suite**: several stdlib-call tests failed to link with "undefined
reference to `turf_math_power_int_int`" etc. This means the current
silently-continue behavior is load-bearing, most likely tied to the
pre-pass/prototype codegen architecture (see `DISCOVERED_ISSUES.md` #9's
note on `g_protoOnlyPass` — a function body can apparently be legitimately
partially/unsuccessfully pre-pass-codegen'd before its callees' prototypes
exist yet, relying on a later full pass to retry and succeed). Reverted the
hardening change. **Left as an open, unfixed, higher-risk architectural gap**
— fixing it properly needs a real understanding of when a mid-block null
return is "everything is fine, this is expected" vs. "something broke
silently", which the pre-pass/full-pass split currently conflates. Anyone
picking this up should start by fully mapping `g_protoOnlyPass`'s call graph
before touching `BlockExprAST::codegen()` again.

**Repro:**
```turf
struct Box { val: int }
fn makeBox(): Box { return Box { val: 42 }; }
fn main(): int {
    io.printline(makeBox().val);   // prints NOTHING — not even a blank line
    io.printline("after");         // this still runs
    return 0;
}
```
Compiles with **zero errors or warnings**. At runtime the `io.printline(makeBox().val)`
call is simply never executed — control silently falls through to `"after"`.
If the result is assigned to a variable instead (`mut p: int = makeBox().val;`),
the variable is never registered in Codegen's symbol table (no alloca, no
`GlobalSymbolTable->DeclareSymbol`), so the **next** use of that variable fails
with a confusing, wrong-cause `ReferenceError`-style "you're trying to use 'p'
before telling me what it is" — nowhere near the actual bug.

**Root cause:** `FieldAccessExprAST::codegenLValue()` unconditionally does
`Value *BaseLVal = Base->codegenLValue(); if (!BaseLVal) return nullptr;`.
`FuncCallExprAST` (like every r-value-only AST node) does not override
`codegenLValue()`, so it inherits the base class default
(`ExprAST::codegenLValue() { return nullptr; }`). `FieldAccessExprAST::codegen()`
then does `Value *Ptr = codegenLValue(); if (!Ptr) return nullptr;` with **no
r-value fallback and no diagnostic** — it just silently returns null, and every
caller up the chain (`VarDeclExprAST::codegen()`, `MethodCallExprAST::codegen()`'s
arg evaluation, etc.) treats a null `Value*` as "already reported, propagate
silently" per the codebase's established convention.

**The fix pattern already exists twice in the same file** — both
`ArrayAccessExprAST::codegen()` and `MethodCallExprAST::codegen()` already
handle exactly this case correctly:
- `ArrayAccessExprAST::codegen()` (~line 3181): "If the base cannot be treated
  as an L-value (e.g., it is an R-value array returned from a function call),
  we codegen it as an R-value" — evaluates `Base->codegen()`, uses
  `CreateExtractValue`/a temp alloca instead.
- `MethodCallExprAST::codegen()` (~line 3403): "If Base is an rvalue (like
  `User(10)`), we need a temporary alloca to pass by reference" — evaluates
  `Base->codegen()`, spills into a fresh `alloca`, uses that as `self`.

`FieldAccessExprAST` needs the same "spill r-value into a temp alloca, then
GEP into that" fallback. This is a same-file, same-pattern fix — should be
quick once picked up.

**Blast radius:** any `expr().field` where `expr()` is not itself an addressable
lvalue — chained field access off a function call return, off another method
call's return, off a ternary, etc. Given struct-returning functions are common
and idiomatic, this is very reachable in ordinary code, not an edge case.

### 1b. Chained struct-method calls returning `self` by pointer: wrong result + crash — FIXED 2026-07-24
**Where:** `src/Codegen.cpp`, `getExprType()`'s `MethodCallExprAST` case, and
`MethodCallExprAST::codegen()`'s r-value-`Base` handling.

Found while writing a regression test for gap #6 below (`self` returned by
pointer for method chaining) — chaining a `Counter*`-returning method
(`fn Counter.increment(): Counter* { self.val += 1; return self; }`) two
calls deep silently corrupted the mutation, and three calls deep crashed
outright:
```turf
c.increment().increment();              // compiles, but c.val ends up 1, not 2
c.increment().increment().increment();  // "Internal error: could not determine
                                         //  type of method call base"
```
Two separate bugs, both now fixed:
- **Wrong result (2 calls deep):** `MethodCallExprAST::codegen()`'s
  r-value-`Base` fallback (`Value *Val = Base->codegen(); SelfVal =
  CreateAlloca(...); CreateStore(Val, SelfVal);`) unconditionally wrapped
  `Val` in a fresh alloca — correct when `Val` is a struct passed by value
  (e.g. a struct-init call result), but wrong when `Val` is *already a
  pointer* (the `Counter*` returned by a chained call): the second
  `.increment()`'s `self` ended up `Counter**` instead of `Counter*`, so its
  mutation landed on the throwaway temp alloca instead of the real struct.
  Fixed by checking `getExprType(Base.get())` for `TurfPointerType` and, if
  so, using `Val` directly as `SelfVal` with no extra alloca.
- **Crash (3 calls deep):** `getExprType()`'s `MethodCallExprAST` case
  unwraps a pointer `BaseTy` (`PtrTy->getPointee()`) but — unlike its
  sibling `FieldAccessExprAST` case a few lines below in the same function —
  never resolved the resulting `NamedType` to the real `TurfStructType`, so
  the outermost call's `BaseTy` came back as `NamedType("Counter")`, which
  `dynamic_cast<const TurfStructType *>` doesn't match, and there was no
  other fallback for a non-`VariableExprAST` base. Fixed by adding the same
  `NamedType` → `GlobalSymbolTable->LookupType()` resolution step
  `FieldAccessExprAST` already had.

Regression test: `tests/06_structs/23_self_pointer_chaining.tr`.

---

## HIGH

### 2. Generic structs have no valid syntax to instantiate with explicit type arguments — FIXED 2026-07-24 (different approach than originally planned)
**Where:** `src/Types.cpp` (`typesCompatible()`), `src/Parser.cpp`
(`ParseStructDecl()`), `src/Codegen.cpp` (`FieldAccessExprAST`), `src/TypeChecker.cpp`
(`visitMethodCall()`, `visitFieldAccess()`).

**Status: fixed — but NOT via the `Identifier<TypeArgs> { ... }` syntax this
entry originally proposed.** Parsing `Box<int> { val: null }` would need
real backtracking (`<` after an identifier in expression position is
otherwise just the less-than operator), which this hand-written
recursive-descent parser doesn't have — implementing that was judged too
large/risky for what turned out to be unnecessary. Instead: `typesCompatible()`
now treats a bare struct-init's type (`Box { val: null }`, always plain
`NamedType("Box")`) as compatible with a `GenericType` of the same base name
on the other side of an assignment — sound because every instantiation of a
generic struct shares the exact same LLVM layout under this compiler's
type-erasure model (a field typed with the struct's own type parameter is
always a pointer, erasing to an opaque pointer regardless of pointee), so
there's no monomorphized layout to actually pick between; the concrete type
argument only needs to be carried by the surrounding declared type.

**Three more bugs found and fixed while making this work end-to-end**
(none of these were anticipated by the original gap description — each was
only discovered by pushing a real generic-struct usage all the way through
construction → field access → generic method calls):
- `ParseStructDecl()`'s own `GlobalSymbolTable->RegisterType(Name, SType)`
  call ran *inside* the temporary scope pushed for resolving the struct's
  generic type parameters, so it was immediately un-registered when that
  scope popped at the end of parsing — every generic struct's own name was
  permanently unlookupable immediately after being declared, even within
  the same file. Fixed by moving the registration outside that scope.
- Codegen's `FieldAccessExprAST` (`codegenLValue()`/`codegen()`) resolved a
  `NamedType` base to its real `TurfStructType` but had no equivalent for a
  `GenericType` base — field access on a `Box<int>`-typed variable failed.
- `TypeChecker::visitMethodCall()`'s struct-method lookup and
  `visitFieldAccess()` had the identical gap — a method call on a generic
  struct instance fell through to "be permissive" (`ResolvedCallTarget`
  left null), and Codegen's `MethodCallExprAST::codegen()` then silently
  returned nullptr with **no diagnostic** — the same silent-failure class as
  gap #1, surfacing only as an unrelated-looking "you're trying to use 'p'
  before telling me what it is" error at the next place the resulting
  variable was referenced somewhere Codegen actually reached.

Also found in passing, cosmetic only, since FIXED 2026-07-24: comparing a
pointer to an address-of expression (`p == &n`) used to produce
`BinaryExpr(op='�')` in the `--dump-resolved`/`--dump-ast` debug dump — the
same class of bug as the `TOK_AND`/`TOK_OR`-cast-to-char issue fixed earlier
this session (`tests/03_typechecker/fail_logical_operator_non_numeric.tr`'s
doc comment), just in `ASTPrinter.h` this time instead of an error-message
formatter. The actual compiled program always behaved correctly (confirmed
via a scratch repro) — this was purely a debug-dump display bug. Fixed by
adding `OpToDisplayString()` (a small token→string table for every
multi-character operator, falling back to the raw char only for genuinely
single-character, printable operators) and using it in both
`visitBinary()`/`visitUnary()`.

Regression test: `tests/06_structs/25_generic_struct_construction.tr`
(replaces `tests/10_known_bugs/fail_generic_struct_uninstantiable.tr`, which
used the `Box<int> { ... }` syntax that was never actually implemented).

**Original repro, still accurate for context (`Box<int> { val: null }` still doesn't parse — use `Box { val: null }` with an explicit variable type annotation instead):**

**Repro:**
```turf
struct Box<T> { val: T* }
fn main(): int {
    mut b: Box<int> = Box<int> { val: null };   // parse error
    return 0;
}
```
- `Box { val: null }` parses, but produces the bare non-generic type `Box`
  (no type args attached) — incompatible with a `Box<int>`-typed variable.
- `Box<int> { val: null }` is not accepted at all: the brace-init branch only
  matches directly after a bare identifier; `<` is parsed as a relational
  operator instead, cascading into unrelated syntax errors.
- `Box<int>()` (positional-call construction, still documented as the usage
  pattern in `lib/std/stack.tr`/`queue.tr`) was intentionally removed
  elsewhere in favor of brace-init (see `ParsePostfixExpression`'s comment:
  "Positional Point(10, 20) is no longer valid").

**Effect:** `lib/std/list.tr`, `stack.tr`, `queue.tr`, `collections.tr` are all
currently non-functional under the current grammar — none of their documented
usage examples actually compile. No test in `tests/` exercises a generic
struct end-to-end, which is why this was never caught.

**Fix needed:** Parser support for `Identifier<TypeArgs> { ... }` struct-literal
syntax, threading the explicit type arguments through `StructInitExprAST` →
TypeChecker (to validate field types get the right substitution) → Codegen
(to pick/mangle the right LLVM struct instantiation).

**Tracked at:** `tests/10_known_bugs/fail_generic_struct_uninstantiable.tr`

### 3. Generic function type inference doesn't flow through call-site return types — FIXED 2026-07-24
**Where:** `src/TypeChecker.cpp`, `visitFuncCall()`; `src/Types.cpp`, `typesCompatible()`.

**Status: fixed.** Added `collectGenericSubstitution()` (walks a matched
candidate's declared parameter types against the actual argument types,
recursing through pointer/array/generic wrapping, to build a
`{TypeParamName -> ConcreteType}` map) and `substituteReturnType()` (rewrites
the declared return type through that map) in `TypeChecker.cpp`, applied
right after `Node.ResolvedCallTarget = Best;` in `visitFuncCall()`. Also
fixed a second, adjacent gap found while testing the pointer-wrapped case:
`typesCompatible()` had type-erasure leniency for a bare `T` and for `T[]`
but not for `T*`, so a generic function taking a pointer to its own type
parameter (`fn<T> identityPtr(x: T*): T*`) failed overload resolution
outright before the return-type issue could even be reached — added the
matching pointer-pointee leniency rule. Regression test (moved and expanded
from `tests/10_known_bugs/fail_generic_array_param.tr`):
`tests/07_functions_modules/generic_array_return_inference.tr`.

**Repro:**
```turf
fn<T> firstElem(arr: T[]): T { return arr[0]; }
fn main(): int {
    mut a: int[5] = [10, 20, 30, 40, 50];
    assert(firstElem(a) == 10, "");   // "Cannot compare 'T' with 'int'"
    return 0;
}
```
`TypeChecker::getType()` for a call returns the callee's **declared** return
type verbatim — here, the literal `TurfGenericParamType("T")` — instead of
the type `T` was actually instantiated to at this call site (`int`, inferred
from the `int[5]` argument). Comparing that literal `T` against a concrete
`10` then fails type-checking.

Note: this is **downstream of, and distinct from**, the NamedType/
TurfGenericParamType substitution work done this session (see
`docs/planning/generics_and_operators_roadmap.md` and
`Resolver::substituteTypeParams()` in `src/Resolver.cpp`) — that work fixed
bare scalar generics (`fn<T> identity(x: T): T` now works correctly for
int/bool/string/double). This is a genuine, separate type-**inference** gap:
the compiler needs to unify each generic parameter with the argument types
actually passed at a given call site and substitute that into the reported
return type, not just avoid rejecting `T` as an unknown name.

**Fix needed:** in `TypeChecker::visitFuncCall()`, once the best-matching
`ResolverFunctionSymbol` candidate is selected, walk its declared parameter
types against the actual argument types to build a `{TypeParamName -> ConcreteType}`
substitution map, then apply that substitution to the declared return type
before calling `setType(&Node, ...)`.

**Tracked at:** `tests/10_known_bugs/fail_generic_array_param.tr`

---

## MEDIUM

### 4. `compare`/`matches`: duplicate variant/value patterns are not rejected — FIXED 2026-07-23
**Where:** `src/TypeChecker.cpp`, `visitCompare()` (arm/pattern validation);
codegen consumer: `src/Codegen.cpp`, `CompareExprAST::codegen()`.

**Status: fixed.** `visitCompare()` now tracks a `std::set<std::string>` of
pattern keys (enum variant name, integer literal value, or string literal
value — the same narrow set of pattern kinds the existing exhaustiveness
check already recognized) across all arms, and raises an error at the
second occurrence. Deliberately does **not** catch equivalent-but-differently-
written duplicates (`matches 5:` vs `matches 2+3:`), since that would need a
general constant-folder this codebase doesn't have at the TypeChecker level
— only exact literal/variant repeats are caught. Regression tests:
`tests/14_enums/fail_duplicate_matches_pattern.tr` (enum),
`tests/14_enums/fail_duplicate_matches_int_literal.tr` (int), and
`tests/14_enums/19_matches_stacked_patterns_no_false_positive.tr` (confirms
legitimate pattern-stacking across distinct variants/values is not
false-flagged).

**Repro (enum/int subject — the risky case):**
```turf
enum Color { Red, Green, Blue }
fn main(): int {
    mut c: Color = Color::Red;
    mut result: int = 0;
    compare c {
        matches Color::Red: result = 1; done
        matches Color::Red: result = 2; done   // duplicate — no error
        matches Color::Green: result = 3; done
        matches Color::Blue: result = 4; done
    }
    io.printline(result);   // prints 1 -- but this is NOT a guarantee
    return 0;
}
```
Compiles with no error or warning. At the LLVM level, `CompareExprAST::codegen()`'s
integer/enum path calls `SwitchInst::addCase()` once per pattern with **no
duplicate check** — two cases end up with the same `ConstantInt` case value
pointing at two different basic blocks. LLVM's `SwitchInst` does not
deduplicate or reject this at construction time, and which arm actually runs
is an artifact of insertion order rather than a language-level guarantee —
this is relying on incidental LLVM behavior, not a specified semantics.

**String subject case is lower-risk but has the same gap:** the string path
(`EmitStrcmp` chain) evaluates arms sequentially, so a duplicate `matches "a":`
deterministically has the first occurrence win — safe, but still silently
accepts genuinely-dead code with no warning.

**Fix needed:** in `TypeChecker::visitCompare()`, while building `CoveredVariants`
(already collected for the exhaustiveness check, see the existing `std::set`
there), detect a name/literal appearing twice across arms and raise a
"duplicate/unreachable matches pattern" error (or warning) at the second
occurrence's location. For non-enum literal patterns (int/string), the same
duplicate-detection loop can run keyed on the literal's value instead of an
enum variant name.

### 5. Standard library files still use pre-brace-init constructor syntax — FIXED 2026-07-24
**Where:** `lib/std/list.tr`, `lib/std/stack.tr`, `lib/std/queue.tr` — rewritten.
`lib/std/collections.tr` — deleted (user decision, see below).

**Status: `list.tr`/`stack.tr`/`queue.tr` fixed, tested, and working.**
Rewritten to use current syntax: `import mem;` (semicolon), `data: T*`
field order consistently (was `T* data` in `stack.tr`/`queue.tr`),
`null`/`expr to T*` instead of the old `T*(0)`/`T*(expr)` call-style casts,
and `fail("...")` instead of `mem.free(T*(0))` as the empty-collection trap
(the old trap didn't actually trap — freeing null is a well-defined no-op in
C, so it silently did nothing either, just for a different, simpler reason
than gaps #0/#0b below). This rewrite is what surfaced gaps #0, #0b, and the
two entries in "OTHER BUGS FOUND ALONG THE WAY" below — getting these three
files to genuinely work end-to-end (not just compile) took five additional,
independent bug fixes across the Parser, TypeChecker, and Codegen. Every
example in the "Usage" doc comment at the top of each file is now the
literal, verified-working syntax.

**`lib/std/collections.tr` was deleted rather than rewritten** (user
decision). It was an alternate, redundant reimplementation of
List/Stack/Queue (colliding struct and function names with the other
three — importing both would conflict), used a *third* cast syntax
(`as T*`, which isn't a keyword in this grammar at all — no `as` token
exists), called `mem.alloc`/`mem.grow` with an `init()`-based constructor
pattern relying on the old positional-call syntax
(`self.items = List<T>();`), and nothing anywhere imported it.

**Fix verification note:** the first pass of "working" tests for this gap
were false positives — see gap #0's writeup for why exit-code-only
verification wasn't enough here specifically, and why the final regression
tests were re-checked via `--emit-llvm`.

---

## OTHER BUGS FOUND ALONG THE WAY (also while completing gap #5)

### A. A syntax error inside an imported module crashed the entire compiler — FIXED 2026-07-24
**Where:** `src/Parser.cpp`, `ParseFile()`'s per-statement error-recovery paths.

`ParseFile()` (used for every imported module) used to respond to a nested
parse failure by calling `getNextToken()` once and continuing the loop,
token by token, instead of giving up. For a sufficiently broken input, this
eventually ran the underlying `ifstream`'s read position past EOF
repeatedly, which crashed the **entire compiler process** with an unhandled
`std::__ios_failure` ("basic_ios::clear: iostream error") instead of
reporting the real syntax error and stopping. Reproduced via
`extern fn f(ptr: void*): void;` inside an imported module — see bug B
below for why that specific line is itself a real, separate bug.

Fixed by returning `nullptr` immediately on a nested parse failure instead
of limping forward. Also fixed `ParseImport()`'s three-path fallback chain
(compiler stdlib → source-file directory → CWD) to stop trying further
paths once a module is found-but-broken (tracked via
`DiagnosticEngine::getDiagnosticCount()` growing during an attempt) rather
than cascading into an unrelated, confusing "module not found" error on top
of the real one. **Also hardened `scripts/test_runner.sh`**: a `fail_*.tr`
test used to count a compiler crash (exit code ≥ 128, i.e. died from a
signal) as "PASS (Rejected properly)" — identical to a genuine clean
rejection — which would have silently masked a regression back to this
exact crash forever. Regression test:
`tests/07_functions_modules/fail_import_syntax_error.tr` (+ companion
`broken_extern_param_module.tr`).

### B. `extern fn` re-exported through a module call (`mem.free(x)`) always failed with "LLVM function not found" — FIXED 2026-07-24
**Where:** `include/ResolverSymbolTable.h`, `ResolverFunctionSymbol::mangledName()`.

`mangledName()` always computed a Turf-style mangled name
(`turf_ModuleName_BaseName_ParamTypes...`), with no special case for an
`extern fn` — but `Codegen`'s `ExternFuncDeclExprAST::codegen()` always
registers the LLVM function under its **raw declared name** (e.g. `"free"`,
never Turf-mangled at all, since it has to link against the real C symbol).
A *direct*, unqualified call to an extern (`free(x)`, from the file that
declared it) happened to keep working regardless, purely because
`FuncCallExprAST::codegen()` has a fallback that retries the plain callee
name when the mangled lookup misses — but
`MethodCallExprAST::codegen()`'s module-call path (used for `mem.free(x)`)
has no equivalent fallback, and hit a hard
`llvm::report_fatal_error("Internal error: LLVM function ... not found")`
instead. This is likely why `mem.free`/`mem.malloc`/etc. were never called
in module-qualified form anywhere before this session — `lib/std/mem.tr`'s
own Turf-level wrapper functions (`mem.alloc`, `mem.grow`, ...) route
through the *wrapped* extern, not a direct module-qualified extern call, so
this specific path was never exercised.

Also fixed a real, separate stdlib bug this uncovered: `mem.tr`'s own
`extern fn realloc(ptr: void*, ...)`/`free(ptr: void*)`/`memset(ptr: ...)`
declarations use `ptr` as a **parameter name** — but `ptr` is this
language's own reserved opaque-pointer type keyword, so this is a genuine
parse error (and the exact trigger for bug A above, when hit inside the
imported `mem.tr`). Renamed to `p` throughout, with a doc-comment note.

Fix: added an `IsExtern` flag to `ResolverFunctionSymbol`, set at both
hoisting sites (current-file and module-body), and `mangledName()` returns
the raw `BaseName` when set. Regression test:
`tests/05_arrays_pointers_ffi/14_mem_stdlib.tr`.

---

## LOW

### 6. Struct methods cannot return `self` by value (only by pointer) — FIXED 2026-07-24
**Where:** `src/TypeChecker.cpp`, `visitReturn()`; `src/Codegen.cpp`, `CastToType()`.

**Status: fixed.** `visitReturn()` now accepts a pointer-to-struct return
value whose pointee matches the declared bare-struct return type (checked
by name, since `ParseType()` never distinguishes `NamedType` from
`TurfStructType` at parse time). `CastToType()` performs the actual
dereference — a `CreateLoad` through the pointer — when casting a
pointer-to-struct value down to the bare struct type, inside the existing
`if (ValTy->isPointerTy())` block (same block as the generic-erasure
un-erase logic from gap #1's fix). Verified the returned value is a genuine
by-value copy (mutating the original afterward doesn't affect a variable
holding the returned copy), and that chaining still works
(`n.get_self().get_self().val`). Regression test:
`tests/06_structs/24_self_value_return_and_chaining.tr` (moved and expanded
from `tests/09_boss_fights/fail_struct_method_chain.tr`, which had captured
exactly this bug and no longer belongs in `10_known_bugs`-style "expected to
fail" territory).

### 7. `docs/planning/DISCOVERED_ISSUES.md` is stale — DONE 2026-07-24
Marked with a status header pointing here; ~17 of its 23 entries were
re-verified fixed and now have real regression tests (they previously had
none — each was only confirmed via a one-off scratch repro, not a permanent
test). New coverage: `tests/01_syntax/09_braceless_condition_struct_ambiguity.tr`
(#1), `tests/02_resolver/fail_bare_intrinsic_typo.tr` (#8),
`tests/03_typechecker/fail_printline_custom_struct.tr` (#11),
`tests/03_typechecker/04_typeof_expressions.tr` (re-enabled previously
commented-out assertions, #9/#10/#18 already had prior coverage),
`tests/05_arrays_pointers_ffi/11_array_return_and_assign.tr` (#3/#21),
`tests/05_arrays_pointers_ffi/12_array_decl_no_initializer.tr` (#14),
`tests/05_arrays_pointers_ffi/13_ffi_ptr_coercion_and_struct_cast.tr`
(#15/#23), `tests/06_structs/03_constructor.tr` (re-enabled a commented-out
assertion, #16-adjacent), `tests/06_structs/21_nested_field_access.tr` (#5),
`tests/06_structs/22_static_constructor_init.tr` (#16 — this entire
80-line Codegen mechanism had zero test coverage before this), and
`tests/06_structs/23_self_pointer_chaining.tr` (#22's pointer-return half —
which is also what surfaced gap #1b above).

**Also found via the same pass:** two other commented-out assertions
unrelated to any DISCOVERED_ISSUES.md entry —
`tests/05_arrays_pointers_ffi/08_multidim_array_complex.tr` (3D array
r-value chained indexing) and `tests/03_typechecker/04_typeof_expressions.tr`
(`typeof()` string-equality checks) — both re-enabled after confirming they
now pass; they'd been silently disabled with no tracking issue.

#20 (stdlib pointer syntax) is resolved as gap #5 above. #6/#12 (cascading
top-level error messages) are FIXED — see gap #8 below. #13 (FFI sockets)
remains open; a large separate feature, not re-verified/fixed this pass.

### 8. A parse error deep inside a function body cascaded into a second, unrelated top-level error — FIXED 2026-07-24
**Where:** `src/main.cpp` (both the pre-pass and main-pass top-level parsing
loops), new shared helper `SkipToNextTopLevelBoundary()`.

**Status: fixed.** This was `DISCOVERED_ISSUES.md` #6/#12's "confusing error
cascade" complaint (the typo-suggestion half of those two entries was
already fixed independently; this was the remaining half).

**Repro:**
```turf
fn main(): int {
    iff (5 > 3) {        // typo: 'if' -> real error here
        io.printline("hi");
    }
    return 0;
}
```
Used to print the real "Expected ';' after statement" error (with a correct
"did you mean 'if'?" hint) **and then** a second, totally unrelated
"Statements are not allowed outside of a function" hard error — because
`ParseExpression()` for the whole `fn main() {...}` returns `nullptr`
*normally* (not via `longjmp`) after the nested body-parse failure, and
`main.cpp`'s top-level recovery loop had no handling for that specific
case — only for the `longjmp` path. `CurTok` was left wherever the nested
parse gave up (partway through the malformed body, not a clean top-level
boundary), and the next loop iteration misread the leftover tokens (the
rest of the body, its closing `}`, etc.) as new top-level content.

Fixed with a proper depth-aware recovery: `SkipToNextTopLevelBoundary()`
tracks brace/paren/bracket nesting (starting at depth 1, since the
construct whose parse just failed always already consumed its own opening
`{` before the failure occurred) and skips forward until that brace closes,
rather than a flat scan that stops at the first `;` or boundary-looking
token it happens to see — which is very often still inside the malformed
construct's own unconsumed body. Replaced all eight ad-hoc recovery loops
(pre-pass/main-pass × `fn`-family/`import` × `longjmp`-path/plain-return-path)
with calls to this one shared function. Not a fully general fix for every
conceivable parse-failure point (a failure before any body brace even
opens is a different shape this doesn't formally model), but empirically
correct for the realistic cases tested, including multi-function files with
the broken function in the middle. Regression test:
`tests/01_syntax/fail_typo_keyword_cascading_error.tr`.

---

### 9. Enum exhaustiveness in `compare` was never enforced for a function PARAMETER subject — FIXED 2026-07-24
**Where:** `src/TypeChecker.cpp` (`TypeChecker::visitCompare`), `src/CFG.cpp`
(`CFGBuilder::visitCompare`, new), `include/CFG.h`, `src/Codegen.cpp` (CFG
builder call site).

**Status: fixed.** Found while reviewing the CFG/Analysis/ValueRange/
AliasAnalysis cluster — `CompareExprAST` had no `CFGBuilder::visitExpr`
dispatch case at all, so a `compare` was invisible to the CFG (treated as an
opaque statement with no branches). Debugging that surfaced a second,
unrelated, more severe bug: `TypeChecker::visitCompare` used whatever type
`getType()` returned for the subject verbatim. A local `mut`/`immut`
variable already gets its `NamedType("Direction")` corrected to the real
`TurfEnumType` by `Resolver::visitVarDecl()`, but a function PARAMETER's
type never goes through that correction (`ParseType()` always produces a
bare `NamedType` regardless of position, and `Resolver::visitFuncDef()`
declares parameters via a separate path that doesn't fix it up). So
`SubjectTy->isEnum()` was unconditionally `false` for any `compare` whose
subject is a function parameter — an extremely common shape — silently
disabling the entire exhaustiveness check for it. Confirmed via repro: a
non-exhaustive 4-variant enum-parameter `compare` (2 variants covered, no
`otherwise`) compiled with zero diagnostics before the fix.

Fixed `visitCompare` by re-resolving a `NamedType` subject through
`GlobalSymbolTable->LookupType()` before checking `isEnum()`. Separately
added `CFGBuilder::visitCompare()` so `compare` participates in CFG
reachability/termination analysis at all — after three iterations (see that
function's doc comment in `src/CFG.cpp` for the full history), landed on:
`compare` is *never* a hard CFG terminator by itself (preserves the
widely-used "exhaustive compare + trailing belt-and-suspenders return"
idiom used throughout the test suite — an earlier, more "precise" version
broke 7+ existing tests), but the "nothing matched" fallthrough *edge* is
still omitted when the compare is provably exhaustive (needed separately, to
avoid `DefiniteAssignmentPass` false-flagging a variable assigned in every
arm as possibly-unassigned via its intersection-based dataflow).

**Known, accepted remaining limitation:** a function whose *entire* body is
a single exhaustive `compare` with no trailing statement at all still gets a
false "missing return" — see `tests/10_known_bugs/fail_compare_only_body_missing_return.tr`
for the full writeup and workaround.

Regression tests: `tests/14_enums/fail_enum_non_exhaustive_param.tr`,
`tests/14_enums/20_compare_param_subject_exhaustive.tr`,
`tests/14_enums/21_compare_definite_assignment.tr`.

---

### 10. `ValueRangePass`'s compile-time array-bounds check never fired for a variable index — FIXED 2026-07-24
**Where:** `src/ValueRange.cpp` (`ValueRangePass::runOnCFG`, `evalConstExpr`).

**Status: fixed.** Found while reviewing the same CFG/Analysis/ValueRange/
AliasAnalysis cluster. `ValueRangePass` is meant to statically prove
constant-index array accesses out of bounds at compile time (a literal
index like `arr[3]` was already caught elsewhere and unaffected). It
resolved variables via `GlobalSymbolTable->LookupSymbol(name)` — a lookup
against the SymbolTable's *current* scope stack. This pass runs inside
`FuncDefExprAST::codegen()` *after* `Body->codegen()` has already fully
executed, and `BlockExprAST::codegen()` unconditionally
`EnterScope()`/`ExitScope()`s around its statements — including the
function's own top-level body block. By the time `ValueRangePass` ran, that
scope had already been popped, so `LookupSymbol` could never find any local
of the function being analyzed (not just ones in nested blocks — even
top-level ones), silently making the pass's constant tracking a no-op:
```turf
fn main(): int {
    mut arr: int[3] = [10, 20, 30];
    mut i: int = 5;
    io.printline(arr[i]);   // provably OOB, but compiled with zero diagnostics
    return 0;               // only failed at RUNTIME via the separate bounds check
}
```
`DefiniteAssignmentPass` had already hit and fixed this exact class of bug
(see its doc comment in `src/Analysis.cpp`): it keys its dataflow on raw
`VariableSymbol*` (via each AST node's `ResolvedSymbol`, set once by the
Resolver and owned by the AST itself — no dependency on SymbolTable scope
timing) instead of `SymbolID`. Applied the identical fix to `ValueRangePass`.

Regression tests: `tests/05_arrays_pointers_ffi/fail_array_oob_via_constant_propagation.tr`
(top-level), `fail_array_oob_nested_block_constant.tr` (nested block, the
originally-suspected narrower case), `15_array_valid_constant_index_var.tr`
(valid in-bounds accesses must still compile and run correctly).

---

### 11. `AliasAnalysisPass` misidentified a shadowed variable as the same allocation, producing a false "Double free" — FIXED 2026-07-24
**Where:** `src/AliasAnalysis.cpp` (`AliasAnalysisPass::runOnCFG`).

**Status: fixed.** Last file in the CFG/Analysis/ValueRange/AliasAnalysis
cluster. Unlike `ValueRangePass`/`DefiniteAssignmentPass`, this pass never
touched `GlobalSymbolTable`, so it wasn't hit by gap #10's scope-timing bug
-- but it had a sibling problem: its points-to map was keyed by the
variable's raw `std::string` name, not its resolved identity. That conflates
two *different* variables that happen to share a name across a shadowing
boundary. Concretely:
```turf
mut p: int* = malloc(sizeof(int) * 4);
free(p);
if true {
    mut p: int* = null;   // shadows the outer p; never allocated
    free(p);               // safe -- but flagged as re-freeing the OUTER p
}
```
The inner `mut p: int* = null;`'s initializer is neither a `malloc()`/
`realloc()` call nor a plain variable alias -- the only two initializer
shapes the pass updates the map for -- so it never overwrote the stale
`"p" -> (freed) alloc site` entry the OUTER `p` left behind. The inner
`free(p)` then looked up that stale entry and raised a false "Double free
detected for 'p'" for code that never double-frees anything. Fixed by
keying the map on each `VariableExprAST`/`VarDeclExprAST`'s `ResolvedSymbol`
(a `VariableSymbol*`, set once by the Resolver and already
shadowing-aware) instead of the bare name -- same identity-based approach
as gap #10's fix, and the one `DefiniteAssignmentPass` already used (see
`src/Analysis.cpp`). With per-declaration identity as the key, the inner and
outer `p` are simply different map entries; no explicit "clear on
shadowing" logic was needed once the key itself couldn't collide.

There was no pre-existing test coverage at all for double-free/use-after-free
detection. Added `tests/05_arrays_pointers_ffi/fail_double_free.tr`,
`fail_use_after_free.tr` (both genuine-defect regression tests, to make sure
the identity-based fix didn't accidentally make the pass stop catching real
bugs), and `16_alias_analysis_shadowing.tr` (the false-positive repro above,
now must compile and run cleanly).

---

### 12. `assert()`/`fail()` with the wrong argument count crashed the compiler (SIGABRT), not a diagnostic — FIXED 2026-07-24
**Where:** `src/TypeChecker.cpp` (`TypeChecker::visitFuncCall`'s "Bare
language intrinsic path"), `src/Builtins.cpp` (`CodegenIntrinsic`).

**Status: fixed.** First finding from the Builtins/Lint review. `assert()`
(zero args) and `fail()` (zero args) both crashed the compiler outright:
```
/usr/include/c++/.../stl_vector.h:1272: ... vector<...>::operator[] ...:
Assertion '__n < this->size()' failed.
```
Two independent gaps compounded:
1. `assert` is registered in the ad-hoc `BuiltinInfo` registry with
   `Arity == -1` ("variadic/unknown") because it genuinely accepts 1 or 2
   args (condition, optional message) -- not one fixed count. That value
   disabled `TypeChecker::visitFuncCall`'s generic single-arity check
   *entirely* for `assert`, so a 0-arg or 3+-arg call was never rejected at
   all -- unlike `typeof`/`lengthof`, which have fixed arity and so already
   got a proper "expects N arguments" error.
2. `CodegenIntrinsic`'s `assert`/`fail`/`lengthof` handlers unconditionally
   index `Args[0]` (and `assert` also `Args[1]`) with no bounds check,
   trusting the TypeChecker arity check completely -- a trust that's
   reasonable for `print`/`printline`/`input`, which are only ever reached
   through a real, fixed-arity `@intrinsic`-decorated Turf function in
   `lib/std/io.tr` (protected by full overload resolution), but not for the
   four true "bare intrinsics" (`typeof`, `lengthof`, `assert`, `fail`),
   which bypass that and go through the ad-hoc registry instead.

Even for `fail()` (which *does* have correct fixed arity, 1, and so *was*
being rejected by the arity check) the crash still happened, because
`main.cpp`'s pre-pass (sub-pass 2b) runs its own `TypeChecker` instance
first but deliberately discards its errors (`// Errors in the pre-pass TC
are informational only here`), and sub-pass 3b then unconditionally
codegens the same AST anyway, regardless of what the pre-pass TypeChecker
found. So the arity error fired and was silently swallowed during the
pre-pass, `CodegenIntrinsic("fail", {}, ...)` ran with an empty `Args`
vector, and `Args[0]` was a real out-of-bounds access — a crash the user
hit before the *mandatory* main-pass TypeChecker (which would have reported
this cleanly) ever got to run.

Fixed on two fronts: (1) an explicit "1 or 2 arguments" check for `assert`
in `TypeChecker::visitFuncCall` (mirroring the existing `typeof`/`lengthof`
special cases) closes the TypeChecker-side gap, and gives `fail` a proper
return type too (it previously fell through to an unset/null type even on
valid calls); (2) `CodegenIntrinsic`'s `lengthof`/`fail`/`assert` handlers
now bounds-check `Args` before indexing (matching a pattern the `typeof`
handler already used, "Defensive check retained as belt-and-suspenders") —
defense in depth, so any *future* gap of this shape degrades to a clean
`SemanticError` instead of undefined behavior, regardless of which pass hits
it. `print`/`printline`/`input` were deliberately left unhardened: they're
structurally unreachable through this fragile path (see the doc comment in
`src/Builtins.cpp`).

Regression tests: `tests/03_typechecker/fail_assert_no_args.tr`,
`fail_assert_too_many.tr`, `fail_fail_no_args.tr`,
`10_assert_fail_valid_arity.tr`.

---

### 13. `!=` on doubles used ordered (not unordered) FCmp, silently breaking the `x != x` NaN-detection idiom — FIXED 2026-07-24
**Where:** `src/Codegen.cpp` (`BinaryExprAST::codegen()`'s `TOK_NEQ` case,
and the double→bool truthiness cast in `CastToType`).

**Status: fixed.** Found while writing a regression test for gap #14 (the
Lint.cpp self-comparison wording fix) — the test used `math.sqrt(-1.0)` to
get a real NaN and asserted `x != x` for it, and the assertion failed.
`!=` between two `double`s emitted `CreateFCmpONE` ("ordered and not
equal" — requires *both* operands to be non-NaN). Per IEEE-754/C semantics,
`!=` must be the exact logical negation of `==` for every input including
NaN: `NaN == NaN` is `false`, so `NaN != NaN` must be `true`. `ONE` gives
`false` for `NaN != NaN` (wrong — same as `==`, since neither operand
qualifies as "ordered"), silently breaking `x != x`, the standard cross-
language idiom for `isnan(x)`. `==` was already correct (`CreateFCmpOEQ`,
"ordered and equal" — correctly `false` for NaN). The double→bool
truthiness cast (used for `if someDouble { ... }`-style implicit checks)
had the identical bug for the same reason: `Val != 0.0` should be `true`
for a NaN `Val` (NaN is "not equal to zero"), but `FCmpONE` gave `false`,
making NaN evaluate as falsy. Both fixed to `CreateFCmpUNE` ("unordered or
not equal").

Regression test: `tests/02_resolver/warn_self_comparison_nan.tr`.

---

### 14. Lint's self-comparison warning claimed `x == x`/`x != x` are unconditionally true/false — SOFTENED 2026-07-24
**Where:** `src/Lint.cpp` (`LintBinaryExpr`'s self-comparison check).

**Status: wording softened** (see user decision below — the alternative of
excluding variables from the check was explicitly rejected as too costly).
`x == x` is `false` and `x != x` is `true` when `x` is a `double` holding
NaN — the opposite of what the warning claimed for those two operators
specifically. Lint.cpp runs structurally, with no type information
available, so it can't tell a `double` variable from an `int` one and
narrow the check precisely. Excluding all variable self-comparisons from
`==`/`!=` would have gutted the check's main value (catching copy-paste
mistakes like `a.val == a.val`), so instead of narrowing scope, the
wording for `==`/`!=` (only — `<`/`>`/`<=`/`>=` remain unconditional, since
every IEEE-754 *ordered* comparison against NaN is correctly `false`
regardless of type) was changed from an absolute claim to a hedge: "This is
always true/false (unless the value can be NaN)."

Regression test: `tests/02_resolver/warn_self_comparison_nan.tr` (also
exercises gap #13 above, discovered while writing this same test).

---

### 15. `LintExpression` ran 3 times per function body, tripling every warning — FIXED 2026-07-24
**Where:** `src/main.cpp` (pre-pass sub-pass 1's parse loop, and the "Phase
Separation: Lint All" section).

**Status: fixed.** Found empirically while verifying gap #14's fix (a
single self-comparison warning printed three times for one line). Root
cause: the pipeline lints the same logical function body from three
separate call sites:
1. `main.cpp`'s pre-pass sub-pass 1 called `LintExpression()` immediately
   after parsing each `fn`/`struct`/`enum`/`@`/`extern`/`import` node —
   before Resolver or TypeChecker had even run on it.
2. `FuncDefExprAST::codegen()` has its own embedded
   `LintExpression(Body.get())` call, which fires when the pre-pass
   codegens that same AST node's body (sub-pass 3b) — linting the
   *identical* AST node a second time.
3. The main pass fully **re-parses the entire file from scratch**
   (`main.cpp`'s "Phase Separation: Parse All", a deliberate, documented
   design for forward-declaration/orphaned-block-avoidance reasons) into a
   structurally-identical but distinct set of AST nodes, then its own
   "Phase Separation: Lint All" section lints those too via the `FnASTs`
   loop — a third pass over what is, from the user's point of view, the
   same source line.
Fixed by removing the two redundant call sites (1, and the `FnASTs` half
of 3) and keeping only the embedded codegen-time lint (2) plus the
`TopLevelASTs` half of 3 (non-`fn` top-level statements have no embedded
codegen-time lint of their own, so that source is still needed). Did not
touch `Codegen.cpp`'s embedded call itself, since `Sandbox.cpp` also
depends on it as its own lint source.

No dedicated regression test beyond re-verifying `tests/02_resolver/warn_redundant_cmp.tr`
and `warn_self_comparison_nan.tr` each now print their warning exactly
once — this is pipeline plumbing, not something a single `.tr` file's
compile/run outcome can assert on its own.

---

## Cross-references
- `tests/10_known_bugs/` — machine-checkable regression tests for gaps #2 and #3
  above (excluded from `scripts/test_runner.sh`'s normal run).
- `docs/planning/generics_and_operators_roadmap.md` — the aspirational "real
  monomorphization" design; current generics are type-erasure-based (see
  `Resolver::substituteTypeParams()` in `src/Resolver.cpp` for how a bare
  generic type parameter is threaded through today).
- Memory: `project_generics_namedtype_gap` (this session's persistent-memory
  note) has the deep technical trace of how `NamedType` vs `TurfGenericParamType`
  interact across passes, for whoever picks up gap #2 or #3.
