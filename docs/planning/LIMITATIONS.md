# Known Limitations (post-v1.0.0 fix list)

**CLOSED as of STD-003** (`docs/development/changes/STD-003.md`,
`docs/planning/ARCHITECTURE_OVERHAUL.md` §8) — every item below is now
FIXED. Item 2 is the interesting case: this document's own working
assumption (that TYPE-002/monomorphization had already unblocked it) was
WRONG, discovered only by attempting the conversion live during STD-003 --
it was a real, separate, genuinely still-open compiler gap, fixed directly
in the compiler as part of this same group rather than deferred further.
Kept in place, not deleted, as the historical record of what Type Erasure
used to cost and exactly how each item was actually resolved — a future
session should treat this file as archived history, not as a live tracker;
new gaps get their own change record or `tests/99_known_bugs/` entry
instead of a new item here.

Everything in this document was originally a **real, confirmed, then-open**
gap in the
compiler's generics support — not a bug in any individual stdlib module.
Every item below traces back to the same root cause: **Type Erasure with no
monomorphization**. A generic function/method's body is type-checked and
codegenned exactly *once*, shared across every instantiation (every type
parameter erases to one uniform 8-byte slot) — there is no per-instantiation
re-checking or re-codegenning the way a monomorphizing generics
implementation (Rust, C++ templates) would do it. That design choice is
what makes generics cheap and simple in this compiler, and it's also the
single reason every limitation below exists.

Fixing any of these for real means one of two things, neither undertaken
lightly:
- **Per-instantiation codegen** (true monomorphization) — a substantial
  Codegen architecture change, probably the biggest lever here.
- **A trait/constraint system** (e.g. `fn<T: Eq> ...`) — lets a generic
  body require and use a real per-T operation without needing full
  monomorphization, but is a real new language feature, not a bugfix.

None of this blocks v1.0.0 shipping — every item below has either a
documented, working non-generic workaround, or simply isn't exercised by
anything in the shipped stdlib surface. Fix after v1.0.0.

---

## 1. `Map<K, V>` / `Set<T>` generic equality doesn't dispatch to `operator==` for `string`

**Status: FIXED** — **TYPE-002** (monomorphization), S12b (methods) through
S14a (erasure deleted), landed 2026-09-21. `Map<string, V>.indexOf()`/
`Set<string>.indexOf()` each now get their own real, separately-codegenned
clone per instantiation, so `self.keys[i]`/`item` are concretely typed as
`string` during that clone's own re-typecheck — overload resolution
correctly dispatches to `strings.tr`'s real, content-comparing
`operator==(string, string)`. Verified live (S14b), with a genuinely
runtime-built duplicate key/item (never a literal, see the trap noted
below): `tests/38_monomorphization/10_set_string_dedup.tr` and
`11_map_string_key_equality.tr` (moved and rewritten from
`tests/99_known_bugs/set_string_dedup_broken.tr`, which this section used
to describe as confirmed-broken). `Map<K,V>`'s/`Set<T>`'s existing generic
`.get()`/`.contains()`/`.tryGet()`/`indexOf()`/`add()` methods are correct
directly now, as this section originally hoped a real fix would achieve —
no caller-side workaround needed any more, though `http.tr`'s own
non-generic workaround (below) is left in place, since simplifying it back
is optional stdlib cleanup explicitly deferred out of the TYPE-002 group.

**Where the bug used to be (kept for history):**
`lib/std/collections/map.tr`'s `Map.indexOf()`, `lib/std/collections/set.tr`'s `Set.indexOf()`.

`self.keys[i] == key` (Map) / `self.items[i] == item` (Set) inside these
generic method bodies compares by raw pointer/bit identity, not content,
whenever `K`/`T = string`. Mechanism:

1. `TypeChecker::visitBinary()`'s operator-overload gate resolves a
   candidate by `sameTypeExact(candidateParamType, operandType)`.
2. From *inside* a generic method body, both operands have the STATIC type
   `TurfGenericParamType("K")`/`("T")` — never the real, concrete
   `string` — because (per the root cause above) the body is checked once,
   generically.
3. No candidate's declared `operator==(string, string)` signature can ever
   exact-match a bare `K`/`T`, so overload resolution silently fails.
4. `TypeChecker::inferRelationalType()`'s "same-name" fallback
   (`LhsTy->getName() == RhsTy->getName()`, both `"K"`/`"T"`) lets the
   comparison through anyway, and Codegen emits a bare pointer-identity
   `ICmp` on the erased slot.

This is **correct** for `K`/`T` = `int`/`double`/`bool`/pointer (bit-exact
identity genuinely is `==`'s meaning for those), and **wrong** for `string`
(two content-equal keys/items from different allocations compare unequal).

**The literal-string trap:** two occurrences of the *same string literal*
in one source file can appear to work by sheer coincidence — the linker
merges identical read-only string constants into one address, so
`m.put("a", 1); m.contains("a")` can be pointer-equal purely by luck. This
is exactly why `tests/07_functions_modules/09_stdlib_map.tr`'s all-literal
`Map<string, int>` keys passed despite the bug already being present, and
why `tests/34_http_and_json/03_http_manual_routing_roundtrip.tr` (a
freshly-*parsed*, non-literal header value) is what actually surfaced it.
Never trust a same-literal repro to prove this class of bug fixed or
absent — use a value built at runtime (`strings::trim`/`substring`/
concat/anything parsed) for a real test.

**Historical repro (no longer reproduces — TYPE-002 fixed this, see the
`Status: FIXED` banner above; kept so the shape of the original bug stays
legible)**, now `tests/38_monomorphization/10_set_string_dedup.tr`,
asserting the opposite (correct) outcome:

```turflang
mut s: Set<string> = Set::default();
s.add("hello");
s.add("world");
immut dup: string = strings::trim(" hello "); // separate allocation, same content
s.add(dup); // used to NOT be a no-op; now correctly is
// s.length() is now 2, as it always should have been
```

**Working non-generic workaround** (proven live in `lib/std/http/http.tr`):
if the generic collection is a field of your own **concrete, non-generic**
host struct, a plain method on that struct sees the field's type fully
concretely — `self.headers.keys[i] == name` inside `Request.get_header()`
(a non-generic method on the non-generic `Request` struct) correctly
reaches the real `operator==(string, string)`, because `Request` never
needed the generic-erasure detour in the first place. See
`Request.get_header()`/`has_header()` and `Server.route_index()`/
`has_route()`/`get_route()` in `http.tr` for two working examples.

A *free function* generic in only some of a multi-param generic struct's
type arguments works the same way — see item 3 below, which is what makes
`tests/07_functions_modules/generic_partial_type_arg_match.tr`'s
`fn<V> mapContainsStrKey(m: Map<string, V>, key: string): bool` compile and
compare correctly (`K` is pinned concrete to `string` in the signature,
only `V` stays generic).

**(Historical: this used to say "no general fix without monomorphization or
a trait/constraint system, see this document's intro" — monomorphization
is exactly what landed, TYPE-002, and it does make both `Map<K, V>`'s and
`Set<T>`'s existing generic `.get()`/`.contains()`/`.tryGet()`/`indexOf()`/
`add()` methods correct directly, with no caller-side wrapper needed, as
hoped.)**

---

## 2. The same generic-equality gap blocks a generic comparator/sort over `T`

**Status: FIXED (STD-003), after first being RE-CONFIRMED STILL BLOCKED
earlier in the same session.** This document's own working assumption
through STD-003's own §8 entry ("generic `list_sort`... all later,
S24b/STD-003") was that monomorphization had already unblocked the
underlying mechanism and only the stdlib conversion itself remained. That
assumption was initially found to be WRONG by actually attempting the
conversion live: a comparator parameter typed `fn(T, T): int` inside a
generic method failed to type-check correctly at the call site (`No
matching overload`), because `collectGenericSubstitution()`/
`typesCompatible()` never recursed into a `FuncType`'s own parameter/return
types — a real, independent gap from the `Map`/`Set` equality bug item 1
fixed, which TYPE-002 never touched.

Unlike item 1 (fixed by TYPE-002, a prerequisite this group only
consumed), this gap was fixed **directly in the compiler as part of
STD-003 itself**, on the reasoning that a comparator callback --
`self.otherMethod(concreteArg)`-shaped code more generally -- is ordinary
code any Turf program might write, not an inconvenience specific to this
stdlib file; leaving a real, silently-broken language feature in place
because fixing it happened to fall slightly outside one session's original
ticket was judged the wrong tradeoff. Five call sites needed the identical
missing case (a `FuncType` branch, exactly like the existing pointer/
array/`GenericType` branches each already had):
`Resolver::substituteTypeParams()` (`src/resolve/ResolverCore.cpp`),
`typesCompatible()` (`src/sema/Types.cpp`), `collectGenericSubstitution()`/
`collectGenericParamNameOrder()` (`src/sema/TypeCheckerCall.cpp`), and
`Monomorphize::substituteConcreteTypes()` (`src/sema/Monomorphize.cpp`). See
`docs/development/changes/STD-003.md` for the full root-cause writeup,
live repro, and the fix itself.

The bare-`<`/`>`-comparison fallback (`TypeChecker::inferRelationalType()`'s
generic-name bypass, which covers `==`/`!=` but not ordering operators)
remains genuinely unfixed — not needed, since the comparator-callback
approach above is now the working design and was always the more
general/idiomatic one (a `qsort`-style callback, not requiring `T` to
support native `<`).

**Where:** `lib/std/collections/list.tr`'s `List.sort`/`binary_search`/
`find` are now real `fn<T> List....` methods, verified across `int`/
`double`/`string`/a plain-data struct
(`tests/38_monomorphization/27_generic_list_sort_and_search.tr`).
`list_sort`/`list_binary_search`/`list_find` remain as thin, **deprecated**
wrappers over the concrete `List<int>` case for one cycle (STD-003's own
"prefer keeping wrappers" guidance), not removed outright.

---

## 3. A function generic in only *some* of a multi-param generic struct's type arguments

**Status: FIXED during the same pass that documented items 1–2** (see
`src/sema/Types.cpp`'s `typesCompatible()` — the `GenericType`-vs-`GenericType`
per-argument recursion). Listed here for completeness/history, since it's
the mechanism that makes item 1's free-function workaround viable for a
`Map<string, V>` with `V` still generic, not just a fully concrete
`Map<string, string>`.

Before the fix: `typesCompatible()` had no rule for two `GenericType`s of
the same base name (e.g. parameter `Map<string, V>` vs. argument
`Map<string, string>`) — only a bare-name-vs-`GenericType` leniency rule
existed (`Box` vs. `Box<AnyArg>`). A call like
`mapContainsStrKey(m: Map<string, string>, ...)` against a declared
parameter `Map<string, V>` was rejected with "No matching overload" even
though `collectGenericSubstitution()` (the step that runs *after* a
candidate is accepted) already recursed per-argument correctly — nothing
upstream of it did the same recursion for the "is this candidate even a
match" filter. Fixed by recursing `typesCompatible()`'s own per-argument
check the same way, with a `TurfGenericParamType` bypass per argument
(same type-erasure leniency the existing `T[]`/`T*` rules already use) and
`isEquivalent()` (not full `typesCompatible()`) for concrete-vs-concrete
argument pairs, deliberately excluding numeric widening between type
arguments themselves.

Regression test: `tests/07_functions_modules/generic_partial_type_arg_match.tr`.

---

## 4. `List<T>` has no `get(i)`/`set(i, val)` indexing methods

**Status: FIXED — LANG-001, completed by LEC-006.** `operator[]`/`operator[]=`
give `l[i]`/`l[i] = v` with bounds checking, the "better form of this
request" LANG-001's own stdlib commit message names directly. LANG-001
declared them for the concrete `List<int>` only, because `operator`
declarations could not be generic; LEC-006 lifted that, so they are now
`operator<T> [](l: List<T>, i: int): T` and work for every element type
(`Map<K, V>` and `Set<T>` likewise). `l.data[i]` remains the unchecked,
`unsafe`-gated raw path.

**Where the gap used to be (kept for history):**
`lib/std/collections/list.tr` used to expose `push`/`pop`/`length` only,
with every caller reaching into the public `data: T*` field directly.

---

## Priority for the post-v1.0.0 pass (historical — all items above are now closed)

1. ~~Item 4 (`List.get`/`set`)~~ — done, LANG-001.
2. ~~Item 1 (`Map`/`Set` string equality)~~ — done, TYPE-002.
3. ~~Item 2 (generic comparator/sort over `T`)~~ — done, STD-003 (a real
   compiler fix, not a stdlib workaround; see that item's own entry above).
