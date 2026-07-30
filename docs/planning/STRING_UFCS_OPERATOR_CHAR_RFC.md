# String / char / UFCS / Operator Overloading — Design RFC

> **STATUS (2026-07-30): Design discussion complete, nothing implemented yet.**
> This is the consolidated output of a design conversation covering four
> interlocking decisions: shrinking the compiler's built-in type set, moving
> `String`'s behavior entirely into the standard library, adding `char`,
> adding Uniform Function Call Syntax, and adding operator overloading. The
> four are treated as one RFC because none of them is independently coherent —
> String's design depends on operator overloading existing, UFCS's value
> proposition depends on String staying primitive, and operator overloading's
> `[]`/`[]=` special case depends on UFCS's resolution-tier model.
>
> This supersedes **Phase 2** of `generics_and_operators_roadmap.md` (the
> `__add__`-style magic-method proposal). Phase 1 of that document (generics)
> is unaffected — already implemented via type erasure, unrelated to this RFC.
>
> **Implementation not started.** §7 is the agreed build order — start at
> step 1 (`char`). Tracked as 7 tasks in the task list (`char` → delete
> String's hardcoded operator codegen → `operator` keyword → UFCS resolver →
> `SymbolTable::registerFunction` duplicate-detection fix → `prelude.tr` /
> `strings.tr` rewrite → delete `lengthof()`). First-class functions is a
> deliberately separate, later effort — not part of this RFC or its tasks.

---

## 0. Guiding philosophy

Every decision below was made against one standing rule the user set early and
applied consistently throughout: **make the compiler dumber.** Concretely,
that cashed out as three repeated preferences, applied every time a design
fork came up:

- **One uniform rule beats a rule-plus-carve-out.** (e.g. UFCS applies to
  every type, structs included — a "primitives only" exception would need
  *more* special-case logic to justify forbidding something, not less.)
- **Exact-match resolution, no silent coercion**, anywhere a new resolution
  path is introduced (operators, UFCS self-argument matching) — deliberately
  narrower than the numeric-coercion fallback the existing `fn` overload
  resolver already has, to avoid silently picking the wrong overload.
- **Hard errors over silent fallback** for anything ambiguous or conflicting
  (duplicate signatures, operator conflicts across imports, builtin-pair
  overload attempts).

---

## 1. Core built-in type set

The compiler knows about exactly these types and nothing else:

```
int, double, bool, char, String, Array, Pointer
```

Everything else — every method, every operator behavior, every utility
function — lives in the standard library. Literal syntax for these types
(`1`, `2.5`, `'b'`, `"a"`) stays compiler-recognized; that part is
unavoidable and was never in question.

Two of these are "primitive in representation and syntax only":

- **`String`** keeps its compiler-recognized type name, literal syntax
  (`""`), and underlying representation (unchanged — still what today's
  `string` primitive is). It gets **zero** hardcoded behavior: no built-in
  `+`, `==`, `<`, no built-in methods. All of that is deleted from the
  compiler and re-implemented in `string.tr` via `operator` overloads (§4)
  and UFCS-callable free functions (§5).
- **`char`** is genuinely primitive with a narrow surface: comparisons are
  built in, arithmetic is not (see §3).

`int`, `double`, `bool`, `char`, `Pointer` retain a real hardcoded fast path
for arithmetic/comparison — this is the "builtin type" set referenced by the
hard-error rule in §4. **`String` is deliberately excluded from that set**,
even though it's compiler-recognized, specifically so `string.tr` is legally
allowed to define its own `operator +`/`operator ==`/etc. If String stayed in
the builtin-op set, its own stdlib operator definitions would be illegal
under the builtin-pair hard-error rule — the whole point would be
self-defeating.

### What actually gets deleted from the compiler

Confirmed via grep against `Codegen.cpp` before this RFC was written — string
behavior is not just "some helpers in the wrong file," it's operators
special-cased directly in binary-op codegen, in at least six places:

- `EmitStringConcat` (`+`)
- `EmitStrcmpRelational` + four separate relational-op branches (`==`, `!=`,
  `<`, `>`, `<=`, `>=`)

All of the above are deleted and replaced by `operator` definitions in
`string.tr`, resolved through the same generic operator-lookup path any other
type uses (§4).

**Known follow-on cleanup, not required for this RFC but enabled by it:**
`Codegen.cpp` has two more independent strcmp-chain implementations — enum-
to-string conversion, and `match`/`switch` dispatch over a string subject.
Both currently hand-roll comparison logic instead of reusing
`EmitStrcmpRelational`. Once `==` on `String` is real operator-lookup instead
of a hardcoded branch, both of those can emit calls to whatever `==` resolves
to for the subject's type instead of maintaining their own copy. Three
independent implementations of "compare two strings" collapse into one.

---

## 2. String indexing

`s[i]` is not special-cased for `String`. It is an ordinary instance of the
`operator []` mechanism (§4.4): `string.tr` defines
`operator [](s: String, i: int): char`. Getting a character out of a string
is not a compiler feature — it's a stdlib operator overload like any other.

---

## 3. `char`

- **1 byte. No Unicode handling for now** — `char` is a byte, not a
  codepoint. (`String` is unaffected either way — it was never going to
  change encoding as part of this RFC.)
- **No arithmetic.** `char` does not get built-in `+`/`-`/`*`/`/`, and — per
  §4's builtin-pair hard-error rule — **no user/stdlib `operator` overload
  can add it either**, since `char` is in the builtin-op set. Arithmetic on
  character values requires an explicit cast to `int` first. This is a
  deliberate strictness choice (`char` is not "int with a costume," unlike
  C) consistent with the exact-match/no-coercion stance applied everywhere
  else in this RFC.
- **Comparisons are built in** (`==`, `!=`, `<`, `>`, `<=`, `>=`) — needed for
  `match`/sorting/basic usage, and, like arithmetic, not overloadable by
  users for the same builtin-pair reason.
- **`char.tr`** is a prelude module providing UFCS-callable functions —
  `isDigit(c: char): bool` called as `c.isDigit()`, etc. Same pattern as
  every other prelude module (§5.3).

---

## 4. Operator Overloading

### 4.1 Syntax

New top-level-only keyword `operator`, reusing existing operator tokens
(no new operator symbols, no custom-operator-definition system — precedence
and associativity are already fixed by the grammar since these are overloads
of existing tokens, not new ones):

```
operator +(lhs: String, rhs: String): String { ... }
operator +(lhs: String, rhs: int): String { ... }
operator ==(lhs: String, rhs: String): bool { ... }
```

Arity distinguishes unary from binary overloads of the same symbol (one
param = unary, two = binary), same as most C-family languages with operator
overloading.

### 4.2 Resolution

`a + b` resolves against the overload set for `+` using the same
exact-type-match resolution machinery the compiler already has for ordinary
function overloading (`SymbolTable`'s existing "exact-type-match +
numeric-coercion" resolver — but for operators specifically, **coercion is
turned off**, exact match only):

1. Check the builtin fast path first (`int`/`double`/`bool`/`char`/`Pointer`
   arithmetic and comparisons). If both operand types are in this set and the
   builtin defines this operator, that's final.
2. Otherwise, search `operator` declarations visible at the call site
   (import-scoped — see §4.5) for an exact operand-type match.
3. No match → type error, same as an unresolvable ordinary call.

### 4.3 Hard rules

- **Builtin-pair overloads are a hard compile error at the declaration
  site**, not just unreachable-due-to-resolution-order. `operator +(lhs:
  int, rhs: int)` never compiles, full stop.
- **Overload selection is on operand types only.** Return type is not part
  of the signature used for selection. Two `operator +(String, String)`
  declarations differing only in return type is a **duplicate-definition
  error**, not something resolved by call-site expected type.
- **One universal conflict rule, no exemptions**: two visible declarations
  (operator overloads, plain `fn` overloads, or UFCS candidates — see §5.4)
  sharing an identical signature but different bodies is a hard compile
  error. This closes a real, confirmed gap: `SymbolTable.h`'s
  `TurfModule::registerFunction` currently silently drops a second
  registration whenever `MangledName` matches an existing one (comment:
  "duplicate mangled names are silently ignored (prototype re-entry)"). That
  logic can't currently distinguish "same declaration seen twice" (fine,
  legitimate no-op — e.g. forward-declaration-then-body) from "two genuinely
  different bodies that collide on signature" (should be an error). Fixing
  this needs origin-tracking (which AST node / which import contributed the
  symbol), not just a `MangledName` string comparison. This is a
  general-purpose fix, not operator-specific — it also plugs the same hole
  for ordinary overloaded `fn` declarations.

### 4.4 `[]` and `[]=` — indexing

Modeled on Kotlin's `get`/`set` convention, with one important divergence
driven by a real correctness requirement: **`operator[]` never returns a
pointer.** (Rejected explicitly — returning `T*` from `[]` would make
`arr[row][col]` index into a pointer on the second step rather than into
whatever the container conceptually holds, and reintroduces raw-pointer
semantics the rest of this design is trying to keep out of user-facing
container code.)

Because of that constraint, indexing needs more care than the other
operators:

- **`IndexExprAST` stays a genuine lvalue-classified AST node all the way
  through Resolver/TypeChecker.** It is *not* eagerly desugared into
  `.get()`/`.set()` calls at parse time. This matters because `arr[i]` needs
  to work as a true assignable storage location for cases beyond plain
  assignment — `arr[i] += 1`, `++arr[i]`, `swap(arr[i], arr[j])` (pass by
  reference) all require `arr[i]` to behave as a real lvalue up through
  semantic analysis, not collapse into a temporary value early.
- **Lowering strategy is chosen per-use, once the base type is known**
  (post-typecheck):
  - **Builtin `Array`**: lowers to a real memory address (GEP), exactly as
    today. Supports everything an address supports — load, store, `&arr[i]`,
    passing to a `T*` parameter, direct load-inc-store for `++`. No change
    in behavior for builtin arrays.
  - **User/stdlib container with `operator []` / `operator []=`**: no real
    address exists (by design, per the no-pointer-return rule above), so:
    - plain read → `.get(i)` (via `operator []`)
    - plain assignment target → `.set(i, x)` (via `operator []=`)
    - compound assignment / increment → `.set(i, <op>(.get(i), x))`,
      synthesized from `get` + the already-resolved `operator <op>` + `set`
      — no third operator needed.
- **Evaluation-order correctness requirement**: for compound-assignment
  fusion (`f()[g()] += 1`), the base (`f()`) and index (`g()`) expressions
  must be evaluated **exactly once each**, then reused for both the `get`
  and the `set` call — not re-evaluated per call. This needs a real lowering
  step (late typecheck or codegen) that hoists them into temporaries; it is
  not a cosmetic AST rewrite. (Every language that has done get/set-based
  operator overloading — C# indexers, Kotlin, Python's `__getitem__`/
  `__setitem__` — has had to solve this exact problem.)
- **`&arr[i]` and passing `arr[i]` to a `T*`/reference parameter is only
  ever legal when `arr` is a builtin `Array`.** For an `operator[]`-based
  container, there is no address to take, by design. This must be a clear
  compile error ("cannot take a reference into a container accessed via
  `operator[]`"), not a silent copy-through or a value that looks like it
  works but doesn't mutate the container.
- `[]`/`[]=` are subject to the same builtin-pair hard-error rule as every
  other operator: builtin `Array` indexing is compiler-internal and can
  never be overridden by a user `operator []` declaration. The operator is
  reachable only for non-builtin (struct/stdlib) container types.

### 4.5 Visibility

Operator overloads are **import-scoped, not global.** An `operator +
(String, int)` defined in some module is only in the resolution set for a
given call site if that module is imported — same visibility rule as any
other symbol, no new concept. This directly avoids the "global operator
lookup becomes messy" problem: non-imported operator definitions simply
aren't in scope. Conflicts across two *imported* modules defining the same
operand-type signature are caught by the universal conflict rule (§4.3).

---

## 5. UFCS (Uniform Function Call Syntax)

### 5.1 Rule

**One universal rule: dot-call syntax first checks for a bound
(struct-attached) method, then falls back to a free function taking the
receiver as its first argument.** Applies to every type — this was an
explicit decision to avoid a "UFCS only for primitives, structs must use
bound methods" carve-out, which would need active logic to justify
forbidding something with no safety benefit.

- For structs: `fn User.print()` (bound) is checked first; `fn print(u:
  User)` (free function, UFCS) is the fallback. Both are reachable via
  `user.print()`.
- For primitives (`int`, `double`, `bool`, `char`, `String`, `Array`): there
  is no bound-method mechanism available at all — struct-method registration
  is keyed to a real declared struct name, and primitives aren't structs.
  UFCS is therefore not optional sugar for these types, it's the *only*
  mechanism by which dot-call syntax can work on them at all.

### 5.2 Resolution order — two full tiers, not a flat pool

Illustrative case: given

```
struct User {}
fn User.print()          // bound, 0 params besides self
fn print(User)            // free fn, 1 param
fn print(User, bool)      // free fn, 2 params
```

`user.print(true)` resolves as:

1. Collect **every** attached-method overload of `print` on `User` and run
   full-signature resolution (exact match) against the actual call
   `(true)`. Here: zero matches (arity alone rules out the 0-arg bound
   method). **This tier is exhausted in full before falling through** — a
   single non-matching attached candidate does not short-circuit into an
   error; the whole attached-method overload set is checked first.
2. Only then, fall to the UFCS-visible free-function overload set for
   `print` (import-scoped, first param type = `User`): `print(User)` and
   `print(User, bool)`. Full-signature match against `(user, true)` → one
   match, `print(User, bool)`. Resolved.

This means "a bound method just has priority over UFCS" is not quite
precise — it's two independent, complete overload-resolution passes run in
sequence. A consequence worth stating explicitly: the universal conflict
rule (§4.3/§5.4) fires **within** a tier (two attached candidates, or two
UFCS candidates, with an identical signature is always an error) but tier
order itself is the **intended, non-error** resolution *across* tiers — if
`fn User.print(bool)` and `fn print(User, bool)` both existed, the attached
one wins silently by design, because the UFCS tier is never even consulted
once tier 1 produces a match.

### 5.3 Visibility — import-scoped, with a prelude exception

Same rule as operators (§4.5): a UFCS candidate is only in scope if the
module defining it is imported. The stated exception is the **prelude** —
`int.tr`, `double.tr`, `bool.tr`, `char.tr`, `string.tr`, `array.tr`,
collected into `prelude.tr` and auto-imported into every file. This is what
makes `s.trim()` "just work," and what makes an LSP able to show `.`
suggestions on a `String`/`int`/`char` value without the file containing an
explicit `import string;` — the prelude modules are always in scope. Any
other free function (user-defined, or from a non-prelude stdlib module)
requires an explicit `import` before it becomes reachable via UFCS.

### 5.4 Match strictness and conflicts

- UFCS's implicit-self-argument matching is **exact-match only, no numeric
  coercion** — same reasoning as operators (§4.2): avoids a `char` value
  silently binding to a UFCS candidate written for `int` via implicit
  widening.
- The universal conflict rule (§4.3) applies identically here: two visible
  UFCS candidates (from different imported modules) with an identical
  signature is a hard error. One conflict rule, applied uniformly across
  operator overloads, plain `fn` overloads, and UFCS candidates — not three
  separate checks.

### 5.5 Implementation shape (no AST desugaring)

`str.length()` is **not** rewritten into a synthesized `strings::length(str)`
call node. `MethodCallExprAST` keeps its parsed shape; resolution attaches a
`ResolvedCallTarget` pointing at the free function's `FunctionSymbol` (the
receiver becomes the first argument at codegen time) — reusing the exact
mechanism `TypeChecker`/`Codegen` already use for ordinary overload
resolution (`ResolvedCallTarget` already exists and is already read by
`Codegen.cpp` independent of how a call was spelled at the source level).
No new rewrite pass; codegen already knows how to call a resolved symbol
regardless of source syntax.

### 5.6 Emergent characterization (informal, not a new rule)

Given §5.2's tiering, a bound method is precisely "a UFCS candidate that
gets an entire resolution pass to itself before UFCS is even consulted" —
there is no other semantic difference between a bound method and a
same-shaped free function, assuming Turf has no access-control system
granting bound methods private-field visibility a free function lacks
(true as of this RFC). Worth keeping in mind for documentation/mental-model
purposes, not a rule that needs separate enforcement.

---

## 6. Relationship between the two new resolution systems

Operator overloading (§4) and UFCS (§5) are **separate resolution tracks**,
not one merged system:

- Operators are keyed on `(operator symbol, operand-type-tuple)`, entirely
  independent of the bound-method / UFCS name-and-receiver-type lookup.
- UFCS is keyed on `(name, receiver-type)` and is a fallback after bound
  methods, per §5.2.
- Both share the same *philosophy* (exact-match-only, import-scoped
  visibility, one universal conflict rule) but are implemented and resolved
  independently. `a + b` never goes through the UFCS path; `s.trim()` never
  goes through operator resolution.

---

## 7. Suggested build order

Not yet started — sequencing proposal for when implementation begins:

1. `char` primitive (self-contained, no dependencies on the rest).
2. Delete `String`'s hardcoded operator codegen
   (`EmitStringConcat`/`EmitStrcmpRelational`/relational branches); decide
   `String`'s exact runtime representation if it's changing at all (this RFC
   does not require a representation change — String stays what it is today,
   only its *behavior* moves out).
3. `operator` keyword + resolution (§4) — String's own `operator +`/`==`/etc.
   in `string.tr` are the first real consumer and the proof this works.
4. UFCS resolver work (§5) — two-tier resolution, `ResolvedCallTarget`-based
   implementation, prelude auto-import wiring.
5. Fix the universal duplicate/conflict-detection gap in
   `SymbolTable.h::TurfModule::registerFunction` (§4.3) — needed by both #3
   and #4, since both rely on it to catch collisions correctly.
6. Populate `prelude.tr` (`int.tr`, `double.tr`, `bool.tr`, `char.tr`,
   `string.tr`, `array.tr`); migrate/rewrite `strings.tr` onto the new
   operator + UFCS mechanisms.
7. Delete `lengthof()` from compiler internals (`Builtins.cpp`,
   `Resolver.cpp`, `TypeChecker.cpp`, `Codegen.cpp`) once `strings.tr`'s
   replacement is confirmed a strict drop-in across `tests/`.
8. First-class functions (separate RFC) — this whole sequence exists to
   shrink/generalize the compiler's special-case surface before that lands.

## 8. Open items not yet decided (surfaced during this RFC, deferred)

- Exact whitelist of overloadable operator tokens (assumed: `+ - * / == !=
  < > <= >= [] []=` plus unary `-`/`!`; never formally enumerated).
- Whether `match`/`switch`-over-`String` and enum-to-string codegen actually
  get refactored to reuse operator-lookup `==` (identified as a clean
  follow-on in §1, not committed to).
- `String`'s exact representation is unchanged by this RFC (stays what
  today's `string` primitive is) — if a growable/length-tracked
  representation is wanted later, that is a separate, larger decision
  explicitly out of scope here.
