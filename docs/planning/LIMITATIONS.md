# Known Limitations (post-v1.0.0 fix list)

Everything in this document is a **real, confirmed, still-open** gap in the
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

**Status: underlying mechanism unblocked by TYPE-002 (monomorphization),
actual stdlib conversion still NOT done — deliberately out of this
group's scope** (`ARCHITECTURE_OVERHAUL.md` §8's TYPE-002 entry: "generic
`list_sort`... all later, S24b/STD-003"). A genuinely generic
`fn<T> List.sort(cmp: fn(T, T): int)` would now type-check and dispatch
correctly per-instantiation, the same way `Set<string>`/`Map<string,V>`'s
own methods now do (item 1) — but `list_sort`/`list_binary_search`/
`list_find` themselves are still the concrete, `List<int>`-only free
functions described below; converting them to real generic methods is
stdlib cleanup, not a compiler fix, and is left for a future session.

**Where:** `lib/std/collections/list.tr`'s own header comment on
`list_sort`/`list_binary_search`/`list_find` (see that file — the full
four-attempts-tried writeup already lives there and isn't duplicated here).

Summary: a genuinely generic `fn<T> List.sort(cmp: fn(T, T): int)` (or any
of three other approaches tried) cannot be made to work for the same root
cause as item 1 — `T` never carries a concrete type into either overload
resolution *or* a `to`-cast/coercion from inside the generic body itself.
`list_sort`/`list_binary_search`/`list_find` are therefore concrete,
non-generic free functions over `List<int>` specifically, not generic
methods — the established, deliberate design response to this exact class
of limitation, and the model item 1's `Request.get_header()`-style
workaround follows.

---

## 3. A function generic in only *some* of a multi-param generic struct's type arguments

**Status: FIXED during the same pass that documented items 1–2** (see
`src/Types.cpp`'s `typesCompatible()` — the `GenericType`-vs-`GenericType`
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

**Where:** `lib/std/collections/list.tr`.

`List<T>` exposes `push`/`pop`/`length` only. There is no
`List.get(i: int): T` or `List.set(i: int, val: T): void` method — every
caller (inside the stdlib and out) reaches into the public `data: T*`
field directly (`l.data[i]`, `l.data[i] = x`) to read or write by index.
This works today (the field is public and `T*` indexing is ordinary
pointer arithmetic), but it's an ergonomic/encapsulation gap, not a design
decision documented anywhere: a real `List<T>` should have `get`/`set` (or
an `operator[]`-style subscript, if that ever becomes a language feature)
so callers don't need to know `data`/`len`/`cap` are the backing fields at
all. Likely straightforward to add (unlike items 1–2, `get`/`set` don't
need cross-element comparison, so they don't hit the generic-equality gap
at all) — probably just hasn't been done yet because every internal caller
so far has been fine reaching into `.data[i]` directly.

---

## Priority for the post-v1.0.0 pass

1. Item 4 (`List.get`/`set`) — no known blocker, purely additive, do it
   first.
2. Item 1 (`Map`/`Set` string equality) — the highest-value fix; blocks
   `Map<string, V>`/`Set<string>` from being trustworthy off-the-shelf
   collections, which is a real, common shape (string-keyed lookups,
   string sets) most programs eventually want. A trait/constraint system
   (`fn<T: Eq> ...`) is probably the right long-term shape, since it also
   directly unblocks item 2.
3. Item 2 (generic comparator/sort over `T`) — same underlying fix as
   item 1 would likely unblock this too; revisit once item 1's approach is
   chosen.
