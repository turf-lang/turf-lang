# Pointers, Safe References, and `unsafe { }`

Complete internal + external reference for Turflang's pointer system as it
exists after issue #238 ("Track A" of the now-removed `FINAL_SPRINT.md`,
folded into `docs/planning/ROADMAP_v1.0.0_COMPLETE.md` §6 once complete — the
original design sketch, the 2026-08-17 resolved decisions, and the
2026-08-18 implementation summary are retrievable from git history) **and**
after LANG-002 (`docs/development/changes/LANG-002.md`): the `unsafe { }`
block, the `&T` safe-reference type, the raw-pointer gate that ties them
together, and — new in LANG-002 — the nullable `&?T` safe reference, the
`NullabilityPass` that checks it, and the declared-capability system that
replaced the old, purely path-based `lib/std` trust boundary. Like
`docs/architecture/modules_and_scope_resolution.md`, this describes both
**what a Turflang programmer observes** (Part 1) and **how the compiler
actually implements it, pass by pass** (Part 2), with file:line references
throughout.

Related docs:
- `docs/planning/ROADMAP_v1.0.0_COMPLETE.md` §6 — the shipped-feature summary this
  doc expands on.
- `docs/development/changes/LANG-002.md` — the `&?T`/capability change record.
- `README.md`'s "FFI" and "`unsafe { }` and safe references (`&T`)"
  sections — the short, example-driven version of Part 1 below.

---

## Part 1 — External reference (language semantics)

### 1.1 The four pointer-shaped types

| Type | Spelling | Nullable? | Pointee known? | Where it's legal |
|---|---|---|---|---|
| Opaque pointer | `ptr` | yes | no (maps to an untyped `i8*`/`ptr` in LLVM IR) | Inside `unsafe { }`, or in a file that declares the raw-pointer capability (1.2a) |
| Typed raw pointer | `T*` | yes | yes | Inside `unsafe { }`, or in a file that declares the raw-pointer capability (1.2a) |
| Safe reference | `&T` | **no** — compiler-guaranteed | yes | Anywhere — no `unsafe { }` needed |
| Nullable safe reference | `&?T` | **yes** — but checked (1.6) | yes | Anywhere — no `unsafe { }` needed |

`ptr` is the untyped FFI escape hatch (what `extern fn malloc(size: int): ptr;`
returns) — you cast it to a concrete `T*` with `to` before using it for
anything struct-shaped. `T*`, `&T`, and `&?T` all know their pointee type and
all support field/member access with no explicit dereference syntax
(Turflang has no `*p` operator at all — see 1.3). What distinguishes the
three at the *value* level is a compile-time promise, or the deliberate
absence of one: a `&T` can never be null; a `&?T` might be, and the compiler
checks every use; a `T*` might be, and nothing checks it at all (that's the
FFI escape hatch's whole point). All four compile to the exact same LLVM
value (see 2.6) — this is a purely frontend, zero-runtime-cost distinction.

### 1.2 The gate: where a raw pointer type is and isn't allowed

Four kinds of "use" of a raw pointer type (`ptr` or `T*` — **not** `&T` or
`&?T`, both exempt from all of this — `&?T`'s own safety is enforced by a
completely separate mechanism, `NullabilityPass`, see 1.6) are checked:

| Use | Example | Needs `unsafe { }`? |
|---|---|---|
| **Declaration** | `mutable p: ptr = ...;` | Yes |
| **Cast** | `raw to int*` (either direction) | Yes |
| **Dereference** (field/member access, or indexing, through a raw pointer base) | `p.field` where `p: Node*`; `l.data[i]` where `data: T*` (OVH-003) | Yes |
| **Struct field** | `struct Node { next: Node* }` | Yes — the *entire* `struct { }` decl, not just later dereferences |

A fifth kind of use is checked **unconditionally**, and `unsafe { }` does
**not** fix it:

| Use | Example | Fixable with `unsafe { }`? |
|---|---|---|
| **Plain function signature** | `fn get(p: Node*): int` | **No, never** — use `&T` instead |

Three things are exempt from the whole gate:

1. **A file that declares the raw-pointer capability (LANG-002/STD-001,
   see 1.2a below).** `mem.tr`/`net.tr`/`list.tr`/etc. keep using
   `ptr`/`T*`/`void*` exactly as they always have — they just now say so,
   in source, on their own first line.
2. **`extern fn` declarations.** An FFI binding's parameters/return type
   can be `ptr`/`void*`/`T*` freely — that's the whole point of FFI. Only
   *binding the call's result to a variable in your own code* is gated
   (it's a declaration, per the table above):
   ```turflang
   extern fn malloc(size: int): ptr;   // fine, no unsafe needed here

   unsafe {
       mutable p: ptr = malloc(64);    // binding the result IS gated
   }
   ```
3. **The implicit `self` parameter.** `self` is always non-null and its
   type is still an ordinary raw pointer under the hood (see 2.5.3) — a
   struct method reads/writes `self.field` freely with no `unsafe { }`,
   and `self` doesn't count against the function-signature rule either.

### 1.3 `&T`, in detail

`&expr` (the address-of operator) always produces a `&T`:

```turflang
mutable val: int = 42;
mutable ref: &int = &val;   // compiler-guaranteed non-null
```

This is true unconditionally — `&expr` cannot produce a raw `T*` anymore
(that's a deliberate breaking change from before this issue; see 2.3.2).
The operand must be an addressable lvalue (a named variable, a struct
field, an array element) — `&(x + 1)` is rejected the same way it always
was, unrelated to this issue.

**`heap(expr)` is the other source of a `&T`** (FDN-001,
`docs/planning/ROADMAP_v2.0.0.md`, DEC-5) — and the only one whose pointee
outlives the current stack frame. `&expr` above always points into
whatever stack storage `expr` names, so a `&Node` obtained that way dangles
the moment its frame pops; `heap(expr): &T` instead copies `expr`'s value
onto the GC heap and returns a `&T` pointing there, safe to return, store
in a struct field, or hold in a collection for as long as anything keeps
it reachable:

```turflang
fn cons(v: int, next: &?Node): &Node {
    mut n: Node = Node::default();
    n.val = v;
    n.next = next;
    return heap(n);   // n's stack copy is about to pop; this one won't
}
```

No `unsafe { }` needed — `heap()` returns exactly the same `&T` `&expr`
does, just backed by GC-heap storage instead of the stack. Full mechanism
(the real per-type `TurfTypeDesc`, why `void` is rejected):
`docs/architecture/memory_model.md` §1.5, §2.6.1.

**Auto-deref.** `&T` supports `.field`/`.method()` with no explicit
dereference, identically to `T*` — there is no `*ref` syntax in this
language at all, for either pointer kind.

**Widening is one-way and asymmetric:**

```turflang
mutable ref: &int = &val;
mutable raw: int* = ref;          // &T -> T*: always allowed, no unsafe needed
mutable back: &int = raw;         // T* -> &T: REJECTED, even inside unsafe
mutable back2: &int = raw to &int; // explicit cast, needs unsafe { }
```

A `&T` is always a valid `T*` (it's non-null by construction, so widening
loses nothing) — assigning or passing a `&T` where a `T*`/`ptr` is expected
just works, no cast, no `unsafe { }`. The reverse direction can't be done
implicitly at all: a raw pointer *might* be null, so turning one into a
`&T` requires you to explicitly assert it isn't, via a `to` cast — and
that cast itself is gated (it's a "cast" per the table in 1.2), so it must
be written inside `unsafe { }`.

`null` is never compatible with `&T` in either direction — not as an
initializer, not in a comparison against a bare `&T`-typed value's
declared type. This is the whole reason `&T` exists.

### 1.4 `unsafe { }`, in detail

`unsafe { }` is a **lexical gate**, not a new control-flow construct. It
doesn't affect `break`/`continue`/`return` semantics inside it — a
`return` lexically inside `unsafe { }` still returns from the *enclosing
function* exactly as if the braces weren't there:

```turflang
fn get(p: &Node): int {
    unsafe {
        return p.val;   // returns from get(), not from the unsafe block
    }
}
```

It's usable in three positions:

1. **As a statement inside a function body** (the common case) — wraps an
   ordinary sequence of statements.
2. **Nested inside another block** (`if`, `while`, another `unsafe { }`,
   ...) — nesting is harmless; `unsafe { unsafe { } }` just stays
   authorized throughout.
3. **At the top level, wrapping a `struct`/`fn`/`enum`/`machine`
   declaration**:
   ```turflang
   unsafe {
       struct Node {
           val: int,
           next: Node*
       }
   }
   ```
   This is required for any struct with a raw pointer field (1.2's fourth
   row) — the *whole declaration* has to be inside `unsafe { }`, because a
   raw pointer field is itself a gated use regardless of whether anything
   ever dereferences it.

**It's an expression, and its value is its last statement's value** — the
same rule an ordinary `{ }` block already follows:

```turflang
test::assert(unsafe { someRawPtr.field; } == expected, "...");
```

This is the idiomatic way to gate a single expression inline (a
dereference deep inside a larger expression, say) without restructuring
the surrounding code into a separate statement.

### 1.5 Worked examples

**A linked list** (the canonical "genuinely needs a nullable pointer"
case — `&T` can't represent "no next node"):

```turflang
unsafe {
    struct Node {
        val: int,
        next: Node*
    }
}

fn Node.new(val: int): void {
    self.val = val;
    unsafe {
        self.next = null;   // struct field write through self, still needs unsafe: it's a raw-typed field
    }
}

// Plain functions may never take/return a raw pointer -- &Node instead.
fn sum_list(head: &Node): int {
    unsafe {
        mutable current: Node* = head;   // &Node -> Node*: no cast needed (safe-to-raw widening)
        mutable total: int = 0;
        while current != null {
            total = total + current.val;  // dereference through a raw base -- needs unsafe (already are)
            current = current.next;
        }
        return total;
    }
}

fn main(): int {
    mutable a: Node = Node::new(1);
    mutable b: Node = Node::new(2);
    a.next = &b;   // &Node -> Node* widening, no unsafe needed for the assignment itself
    return sum_list(&a) - 3;   // &a is &Node, matches sum_list's parameter directly
}
```

Note the recursion/traversal shape: a function that needs to *walk* a
chain of nullable pointers takes a safe, non-null `&T` at its own boundary
(the caller can never legitimately pass "nothing"), then immediately
widens it into a raw, walkable local inside its own `unsafe { }` body.

**A generic container holding a safe reference:**

```turflang
unsafe {
    struct Box<T> {
        val: T*   // still raw underneath -- see 2.5.1 for why &T fields work too, unchanged
    }
}

fn<T> Box.set(v: &T): void {
    self.val = v;   // &T -> T*: safe-to-raw widening, no unsafe needed
}

fn<T> Box.get(): &T {
    unsafe {
        return self.val to &T;   // T* -> &T: needs an explicit cast inside unsafe
    }
}
```

**FFI:**

```turflang
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;

fn main(): int {
    unsafe {
        mutable buf: ptr = malloc(64);   // binding the extern call's result: gated
        free(buf);                        // the call itself was never gated
    }
    return 0;
}
```

### 1.6 What definitely does NOT compile

```turflang
// (1) raw pointer declared outside unsafe
fn main(): int {
    mutable p: ptr = null;   // ERROR: Unsafe pointer use!
    return 0;
}

// (2) raw pointer struct field outside unsafe
struct Node {
    next: Node*   // ERROR: Unsafe pointer use! (whole decl needs unsafe { })
}

// (3) raw-to-safe cast outside unsafe
fn f(p: int*): void { }   // ALSO independently illegal, see (4) below
fn g(p: int*): void {
    mutable r: &int = p to &int;   // ERROR: Unsafe pointer use! (cast, even inside a fn body)
}

// (4) raw pointer in a plain function's signature -- unsafe doesn't help
unsafe {
    fn takesRaw(p: ptr): int {   // ERROR: Raw pointer in function signature!
        return 0;                // wrapping the whole fn in unsafe { } does NOT fix this
    }
}

// (5) null is never a &T
fn main(): int {
    mutable r: &int = null;   // ERROR: Cannot assign 'null' to variable of type '&int'
    return 0;
}
```

Live regression tests for each of these, plus their passing counterparts,
live in `tests/05_arrays_pointers_ffi/fail_ptr_outside_unsafe.tr`,
`fail_cast_raw_pointer_outside_unsafe.tr`,
`fail_struct_raw_ptr_field_outside_unsafe.tr`,
`fail_fn_signature_raw_pointer_disallowed.tr`, and
`18_unsafe_block_and_safe_reference.tr`.

### 1.7 The raw-pointer capability declaration (LANG-002/STD-001)

Before LANG-002, "is this file allowed to use raw pointers freely?" was
answered by a filesystem path check (`IsPathUnderLibStd()`): the entire
`lib/std` tree was exempt from the `unsafe { }` gate, unconditionally, by
virtue of *where the file lived on disk* — invisible in the file itself,
ungreppable across the tree, and unavailable to a user writing perfectly
legitimate FFI bindings for their own C library.

That privilege is now **declared in source**, as the file's own first
top-level statement:

```turflang
unsafe module;
```

A file that writes this line is granted the raw-pointer capability — every
`ptr`/`T*` use in it (declarations, casts, dereferences, struct fields, and
plain function signatures, i.e. every row in 1.2's tables) is exempt from
the gate, *without* needing to wrap each one in `unsafe { }`. Two conditions
decide whether the declaration is actually **honored**:

1. **The file is under the resolved install root** (`TurfInstallRoot()` —
   the same anchor `import` already uses to find `lib/std`/`lib/usr`). This
   is unconditional: `lib/std`'s own files (now annotated with `unsafe
   module;` themselves) need no extra flag, exactly as before.
2. **Anywhere else**, only with the `--allow-unsafe-modules` CLI flag. A
   user's own FFI bindings get the *identical* capability `lib/std` has —
   just opt-in rather than ambient, so a plain `turf myfile.tr` never
   silently grants it.

```turflang
unsafe module;

fn cmpints(a: ptr, b: ptr): int32 {   // legal here (LANG-002's F1) --
    return 0;                          // was flatly rejected before, even
}                                       // inside unsafe { }
```

Compiled with `turf myfile.tr --allow-unsafe-modules` (outside the install
root) or unconditionally (inside it, e.g. any `lib/std` module).

This closes the FFI half of the same problem `&?T` (1.8) closes on the
data-structure side: before LANG-002, no user file could write a C
callback's comparator signature (`fn cmpints(a: ptr, b: ptr): int32`, what
`qsort`/`bsearch`/a signal handler need) — `checkNoRawPointersInSignature()`
rejected `ptr`/`T*` params in *any* non-`extern` function, everywhere,
`unsafe { }` or not (1.2's fifth-row rule had no exception at all). The
capability declaration is what makes an exception possible, auditably.

### 1.8 `&?T`, in detail (LANG-002)

`&?T` is a **nullable, compiler-checked** safe reference — the type that
closes the audit's single most visible language gap: before it existed,
user code had no safe way to write a linked list at all (`&T` can't
represent "no next node"; `T*` could, but was gated behind `unsafe { }` and
banned outright from ordinary function signatures).

```turflang
struct Node {
    val: int,
    next: &?Node   // no unsafe { } needed anywhere in this struct decl
}
```

`null` is assignable to `&?T` (and to nothing else safe — 1.6's rule (5)
still holds for plain `&T`). Widening is implicit and one-way, same
asymmetric shape as `&T`/`T*` (1.3):

```turflang
mutable a: Node = Node::default();
mutable p: &?Node = &a;        // &Node -> &?Node: always allowed (a &T is never null)
mutable raw: Node* = p;         // &?Node -> Node*: always allowed (widening to raw is always sound)
```

**Field-accessing a `&?T` requires a null check first.** `p.val`/`p.next`
compile only where `NullabilityPass` (2.5, `include/Analysis.h`) has proven
`p` non-null on every path reaching that access — a direct `p != null` /
`p == null` comparison, narrowed through `if`/`else`, an early return,
a `while` condition, or a `&&` chain:

```turflang
fn sum(head: &?Node): int {
    mutable total: int = 0;
    mutable current: &?Node = head;
    while (current != null) {          // narrows `current` to non-null...
        total = total + current.val;   // ...so this is legal here
        current = current.next;
    }
    return total;
}
```

Field-accessing an unguarded `&?T` is `PossibleNullDereferenceError` (E081)
— see `tests/39_nullable_references/fail_deref_without_check.tr` and the
narrowing-shape tests in the same directory (`03_narrowing_shapes.tr`) for
every supported narrowing source.

**`&?T -> &T` needs an explicit, narrowing-checked cast.** Unlike every
other widening direction above, this one is never granted implicitly
(LANG-002's own "Must NOT reinterpret" rule: `&T` stays non-null, the two
types are never merged) — `p to &T` is syntactically legal without
`unsafe` (neither side is a raw pointer), but is rejected
(`UnnarrowedNullableCastError`, E082) unless `p` is proven non-null at that
exact cast site, by the same `NullabilityPass`:

```turflang
fn takesSafeRef(n: &Node): int { return n.val; }

fn maybeSum(p: &?Node): int {
    if (p != null) {
        return takesSafeRef(p to &Node);   // legal: narrowed right above
    }
    return -1;
}
```

See `tests/39_nullable_references/04_narrowed_cast.tr` (the passing
counterpart) and `fail_cast_without_narrowing.tr`.

**Every use is checked, not only `variable.field`** (2026-10-12; before it
the three cases below compiled, and a null faulted or, once optimized, read
garbage):

- **A method call is a use.** `q.bump()` passes `q` as `self`, so it needs
  the same proof as `q.val` (E081). A UFCS call whose callee takes the `&?T`
  itself (`fn count(p: &?Node)` called as `q.count()`) reads nothing and
  needs none.
- **A `&?T` that is not a variable cannot be used at all.** Narrowing is
  per-variable (below), so nothing can prove `p.next`, `find()` or `list[i]`
  non-null: `p.next.val`, `find().val`, `list[i].val`, a method call on one
  (E081) and `(p.next to &Node)` (E082) are rejected by the type checker
  (`TypeChecker::rejectNullableNonVariableUse()`), with a hint to store the
  value in a variable and check that. Comparing such a value with `null`,
  storing it and passing it to a `&?T` parameter stay legal.
- **A loop keeps a proof it does not disturb.** `if (p == null) { return; }`
  followed by a loop that never assigns `p` leaves `p` non-null inside the
  loop and after it; a loop that may assign it ends the proof.

- **A deferred use is checked where it runs.** `defer use(q.val);` runs when
  its block exits, so a later statement of that block that may set `q` to
  null (anything but `&x` or a `&T` value; a later `defer` too) makes it E081
  (`TypeChecker::rejectDeferredNullableUse()`). Before, a list walk
  `while (w != null && ..) { defer use(w.val); w = w.next; }` compiled and
  read address 0x8.

Tests: `08_narrowing_survives_loops.tr`, `09_method_call_after_check.tr`,
`10_nullable_field_through_a_variable.tr` and the `fail_*` files beside them.

**Known, deliberate limitations** (documented, not silently missing —
matching this doc's own §2.7 precedent for the raw-pointer gate):
narrowing is tracked **per-variable only**, never for a general expression
(`p.field != null` narrows nothing; only a bare variable compared directly
to `null` does) — and it is **intraprocedural**: a null check in one
function tells a *different* function nothing about its own parameter.
Function-call arguments and `return` values are **not** narrowing-checked
against a `&T`-typed parameter/return type — only variable declarations and
plain assignments are (see `docs/development/changes/LANG-002.md`'s *Known
pitfalls* for the full list and the reasoning).

### 1.9 Reference and array escape rules (FDN-002)

Every `&T`/`&?T` value above is guaranteed non-null (and, for `&?T`,
null-checked before use) — but neither guarantee says anything about
*where* the memory it points to lives, or how long that memory survives.
FDN-002 (`docs/planning/ROADMAP_v2.0.0.md`; `docs/development/changes/
FDN-002.md`; `FREEZE.md` §6.13/F-MEM-5) closes the one hazard that
survived every earlier reference-safety milestone: a reference into the
CURRENT function's own stack frame, smuggled out past the point where that
frame is popped.

```turflang
struct Node { val: int }

fn make(): &Node {
    mut n: Node = Node::default();
    n.val = 42;
    return &n;              // E086: dangles the moment make() returns
}
```

`EscapePass` (`include/Analysis.h`, `src/Analysis.cpp`) tracks, for every
reference-typed/raw-pointer/unsized-array (`T[]`) parameter or local, one of
three provenances — **Stack** (this frame's own storage), **Param** (came
in from the caller, including `self`), **Heap** (from `heap()`, FDN-001) —
and rejects a `Stack`-provenance value that flows into a `return`, a field
of a `Param`-/`Heap`-owned object, a `List`/`Map`/`Set`/`Stack`/`Queue`
mutator or `operator[]=`, or `heap()`'s own operand. The fix for the
example above is exactly FDN-001's own intrinsic:

```turflang
fn make(): &Node {
    mut n: Node = Node::default();
    n.val = 42;
    return heap(n);          // OK: a fresh, GC-heap-owned copy
}
```

A reference reached through an ALREADY-reference-typed parameter is `Param`
provenance and is never flagged, no matter how far it's passed along — this
pass is intraprocedural only (no borrow checker, no lifetime syntax, no
interprocedural summaries — Overhaul §0.6's "small language" constraint), so
it extends the same trust ordinary reference-parameter semantics already
grant, rather than trying to verify further up the call chain:

```turflang
fn firstFieldAddr(param: &Node): &int {
    return &param.val;      // OK: Param provenance, not this frame's storage
}
```

**Known, deliberate limitations**, in the same "documented, not silently
missing" style as §1.8's own list just above (`docs/development/changes/
FDN-002.md`'s *Known pitfalls* has the full account): tracking is
**per-variable, one field/hop deep** — `p.next.field = &local` is not
caught (`p.next`'s own provenance isn't derived from `p`'s); `&expr[i]`
(any array-indexed address-of) is **not tracked at all**, since a raw
pointer's own element indexing (`list.data[i]`, always heap-backed) can't
be told apart from a genuine local fixed array's without per-field type
information this pass doesn't have — a dangling `&fixedArr[i]` returned
directly is a residual gap (`INV-MEM-6`, `docs/development/INVARIANTS.md`);
and a reference smuggled out **inside a by-value struct copy**, or through
an **intermediate function call**, is `Unknown` and silently allowed. Every
one of these is a false-NEGATIVE gap, never a false positive — soundness
improves monotonically, and this pass would rather miss a real bug than
reject a program that was actually safe.

---

## Part 2 — Internal reference (compiler implementation)

Pipeline position: `Lexer → Parser → Resolver → TypeChecker → Codegen`.
The gate is enforced entirely in the **TypeChecker** — Resolver and
Codegen barely know `unsafe { }` exists.

### 2.1 The type hierarchy

`include/Types.h`:

```
TurfType
 ├─ OpaquePointerType      — `ptr`  (Types.h:297)
 ├─ TurfPointerType        — `T*`   (Types.h:401)
 │   └─ TurfReferenceType  — `&T`   (Types.h:435)
 └─ ... (everything else, unrelated)
```

`&T` (`TurfReferenceType`) is deliberately a **subclass** of the raw
pointer type (`TurfPointerType`), not a sibling. At the LLVM level a `&T`
and a `T*` are identical (both an opaque pointer value) — the only
difference is a frontend-only static contract. Inheritance means
`isPointer()` is `true` for `&T` too, "for free" — every existing
`dynamic_cast<const TurfPointerType *>` call site throughout Codegen
(auto-deref field access, argument passing, address-of caching,
pointer-cast codegen — roughly 15 sites across `CodegenStruct.cpp`/
`CodegenCore.cpp`) matches a `&T` value exactly the way it already matched
a `T*`, with **zero** Codegen changes required.

The safe/raw distinction lives in one new predicate instead:

```cpp
// TurfType base (Types.h:98, :106)
virtual bool isPointer() const { return false; }
virtual bool isReference() const { return false; }   // new
```

`OpaquePointerType`/`TurfPointerType` override only `isPointer()` (stay
`false` for `isReference()`); `TurfReferenceType` overrides both
`isReference()` (`true`) and `getName()` (`"&" + Pointee->getName()`,
`Types.h:439`). This predicate is consulted in exactly two places: the
`typesCompatible()` rules (2.5.4) and the TypeChecker's gate helpers
(2.5.1).

### 2.2 Lexing and parsing

**Lexer.** One new token, `TOK_UNSAFE = -102` (`Lexer.h:340`), one
`Keywords` map entry (`Lexer.cpp:162`). No other lexer changes — the
existing identifier-token branch in `gettok()` handles keyword lookup
generically for any new keyword.

**`&T` as a type annotation.** `ParseType()` (`src/ParserCore.cpp`) gets a
leading-`&` branch, symmetric to the existing leading-`!` (error-union)
branch: consume `&`, recurse into `ParseType()` for the pointee, wrap in
`TurfReferenceType`. This is unambiguous with the *expression*-level `&`
(both the binary bitwise-AND and the unary address-of operator) because
`ParseType()` is a wholly separate function, only ever called from type
positions (declarations, parameters, return types, struct fields) —
never reached through `ParseExpression()`.

**`unsafe { }` as a block.** `ParseUnsafeExpr()` is a thin wrapper, the
same shape as `ParseWhileExpr()`: consume `unsafe`, require `{`, delegate
straight to the existing `ParseBlock()`. No new block-parsing logic is
needed at all — `ParseBlock()` already supports nested `struct`/`fn`/
`enum`/`machine` declarations as ordinary block statements (this is what
makes `unsafe { struct Node { ... } }` legal inside a function body with
zero extra grammar). `ParseUnsafeExpr()` is reachable from two ordinary
dispatch points — `ParsePrimary()`'s ` case TOK_UNSAFE:` and
`ParseBlock()`'s own statement-keyword list — plus one special one below.

**Top-level `unsafe { struct ... }` and why it's not just a plain AST
node.** Several parts of the compiler treat a file's top level as a
**flat list** of decl-shaped nodes, found by `dynamic_cast`, not by a
recursive AST walk:

- `Resolver::hoistTopLevel()` (`src/ResolverCore.cpp`) — Pass 1 of the
  Resolver, registers every struct/enum/machine/error name before Pass 2
  resolves bodies.
- `main.cpp`'s multi-pass codegen driver — separate passes over the
  top-level node list for struct-type codegen, then function-signature
  codegen, then function bodies.
- The TypeChecker/Sandbox "resumable per top-level declaration" machinery
  (`checkAll(Nodes, RecoverPerDeclaration)`).

If `unsafe { struct Node { ... } }` produced a real, nested
`UnsafeBlockExprAST` wrapper node sitting directly in that top-level list,
`Node` would be invisible to every one of those — none of them recurse
into an arbitrary wrapper node looking for structs inside it.

The fix: **flatten at parse time, only at the top level.**
`ParseFile()` (`src/ParserModule.cpp`) and `main.cpp`'s two mirrored
top-level parse loops each get a dedicated `TOK_UNSAFE` branch that calls
`ParseUnsafeExpr()` directly, then passes the result to
`AppendFlattenedTopLevelDecl()` (new, declared in `include/Parser.h`,
defined in `ParserModule.cpp`):

```cpp
void AppendFlattenedTopLevelDecl(std::unique_ptr<ExprAST> AST,
                                  std::vector<std::unique_ptr<ExprAST>> &Target,
                                  bool RecordForSandbox);
```

If `AST` is an `UnsafeBlockExprAST`, its children (`takeChildren()`, see
2.3.1) are moved out and pushed onto `Target` **individually** — the
wrapper itself is discarded, never reaching the flat list at all. Each
spliced-in child is also registered by raw pointer identity in a
file-local `std::unordered_set<const ExprAST *> UnsafeTopLevelDecls`
(cleared once per compile, alongside the other parser-level per-compile
caches). `WasDeclaredInsideTopLevelUnsafe(const ExprAST *)` — the read
side, exported via `Parser.h` — is what the TypeChecker's gate later
consults to know a flattened top-level struct/fn was actually authorized.

Everywhere **else** — inside a function body, inside an ordinary nested
block — `unsafe { }` stays a genuine nested `UnsafeBlockExprAST` node,
because those contexts are already walked recursively by every pass; no
flattening is needed or performed there.

One more small piece: `IsPathUnderLibStd(const std::string &Path)`
(`ParserModule.cpp`, exported via `Parser.h`) is the `lib/std` trust-
boundary check — canonicalizes `Path` and `<TurfInstallRoot>/lib/std` via
`llvm::sys::fs::real_path()` and checks a path-separator-bounded prefix
match. It reuses the exact same `TurfInstallRoot()`/`real_path()`
primitives `ParsePreludeImports()`'s self-import-skip check already
depends on, and is exported specifically so the TypeChecker (which is
kept LLVM-free at the header level) can call it without linking against
`llvm::sys::fs` itself.

### 2.3 AST

**`UnsafeBlockExprAST`** (`include/ASTStmt.h`), shaped like `IfExprAST`
rather than plain `BlockExprAST`: it carries the `unsafe` keyword's own
`SourceLocation` (`getLoc()`), plus a single `Body` child (always a
`BlockExprAST`, from `ParseUnsafeExpr()`). `accept()` dispatches to a new
`visitUnsafeBlock()` on `ASTVisitor` (empty default body, like every other
visitor method). Two extra accessors exist purely to support the
top-level flattening in 2.2:

```cpp
// BlockExprAST
std::vector<std::unique_ptr<ExprAST>> takeExpressions();  // moves Expressions out

// UnsafeBlockExprAST
std::vector<std::unique_ptr<ExprAST>> takeChildren();     // delegates to Body's takeExpressions()
```

**`AddressOfExprAST` retype.** The AST node and its Parser/Resolver/
Codegen handling are all unchanged from before this issue — only the
TypeChecker's inferred *type* for the node changed (2.5.2). This node was
already fully implemented (parsing `&expr` in `ParseUnary()`, an
addressability check via `codegenLValue()` in Codegen) before issue #238;
this issue only changed what static type the operator's result carries.

### 2.4 Resolver

`Resolver::visitUnsafeBlock()` (`src/ResolverStmt.cpp`) is a one-line
delegation:

```cpp
void Resolver::visitUnsafeBlock(UnsafeBlockExprAST &Node) {
  visitChild(Node.getBodyMut());
}
```

This reuses `visitBlock()`'s own `enterScope()`/`exitScope()` bracket
(since `Body` is a `BlockExprAST`) rather than duplicating it. No
unsafe-tracking state exists in the Resolver at all — the Resolver's job
is name resolution, not type-level rules; all gate enforcement lives in
the TypeChecker (2.5). `hoistTopLevel()` needed **no** changes, precisely
because of the parse-time flattening in 2.2 — it never sees an
`UnsafeBlockExprAST` at the top level to begin with.

**A real bug found and fixed here:** `substituteTypeParams()`
(`src/ResolverCore.cpp`) rewrites a generic function/struct's parsed
`NamedType("T")` placeholders into real `TurfGenericParamType`s,
recursing through pointer/array/generic wrappers. Its pointer-recursion
branch used to be:

```cpp
if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty.get())) {
  TurfTypePtr NewPointee = substituteTypeParams(PT->getPointee(), TypeParams);
  return ... std::make_shared<TurfPointerType>(NewPointee);   // BUG
}
```

Because `TurfReferenceType` **is-a** `TurfPointerType`, this branch
matched a `&T` parameter/field too — and then unconditionally rebuilt a
plain `TurfPointerType`, silently turning every substituted generic `&T`
into `T*`. Confirmed live: `fn<T> Box.set(v: &T)` reported its own
parameter as `T*` in the "raw pointer in function signature" diagnostic.
Fixed by checking for `TurfReferenceType` first and reconstructing the
same subclass. The identical bug, in an independent second
substitution helper (`substituteReturnType()`, for method/function return
types), was found and fixed the same way in `TypeCheckerCall.cpp` — see
2.5 below. Any *third* site that reconstructs a `TurfPointerType` from an
existing `dynamic_cast` match should be treated as suspect for the same
reason.

### 2.5 TypeChecker — the actual gate

All new state and helpers live in `include/TypeChecker.h`.

#### 2.5.1 State and helpers

```cpp
// TypeChecker.h
int UnsafeDepth_ = 0;   // mirrors Resolver::FunctionNestingDepth_

static bool isRawPointer(const TurfType *T) {
  return T && T->isPointer() && !T->isReference();
}

bool isUnsafeGateExempt(const SourceLocation &Loc, const ExprAST *Node = nullptr) const;
bool checkUnsafeGate(const TurfType *T, SourceLocation Loc, const ExprAST *Node = nullptr);
void checkNoRawPointersInSignature(const FuncDefExprAST &Node);
```

`isUnsafeGateExempt()` (`TypeCheckerCore.cpp:140`) is `true` when any of:

1. `UnsafeDepth_ > 0` — lexically inside a (nested) `unsafe { }` block.
2. `Node` was spliced from a top-level `unsafe { }` (`WasDeclaredInsideTopLevelUnsafe(Node)`).
3. The declaring file has the raw-pointer capability
   (`HasRawPointerCapability(Loc.Filename)`, 1.7/2.8 below) — **not** a
   `lib/std` path check; this paragraph described the pre-LANG-002
   mechanism (`IsPathUnderLibStd()`) and was left stale after that group
   replaced it. `HasRawPointerCapability()` requires the file to have
   parsed a leading `unsafe module;` pragma, honored unconditionally under
   the install root and only with `--allow-unsafe-modules` elsewhere.

`checkUnsafeGate()` (`TypeCheckerCore.cpp:150`) is the actual enforcement
point: if `T` `isRawPointer()` and the location isn't exempt, it raises a
"Raw pointer type '...' cannot be used outside an 'unsafe' block."
diagnostic (via the ordinary `reportError()` path every TypeChecker
diagnostic funnels through) and returns `true`; otherwise it's a no-op
returning `false`.

`UnsafeDepth_` is incremented/decremented bracketing `Body`'s traversal in
`visitUnsafeBlock()` (`TypeCheckerCore.cpp:213`) — an `int` counter, not a
`bool`, so nested `unsafe { unsafe { } }` stays authorized throughout, and
it's reset to `0` in `resetForRecovery()` (`TypeChecker.h:211`) alongside
`ReturnTypeStack_`/`MachineNarrowStack_` — a `TypeError` raised mid-body
inside `unsafe { }` skips the matching `--UnsafeDepth_` on the way out via
longjmp, and without this reset that would leave every *subsequent*
top-level declaration in the same file wrongly gated (or exempt).

`visitUnsafeBlock()` itself also sets the node's inferred type to its
`Body`'s type (not unconditionally `void`) — this is what makes
`unsafe { expr; }` usable as a value-producing expression (1.4), mirroring
`visitBlock()`'s identical "last expression's type, or void if empty"
rule.

#### 2.5.2 The five gate call sites

| Gate | Site | What's checked |
|---|---|---|
| Declaration | `visitVarDecl` (`TypeCheckerDecl.cpp:76`) | The declared type |
| Cast | `visitCast` (`TypeCheckerExpr.cpp:693-694`) | Destination type first, then operand type |
| Dereference (field access) | `visitFieldAccess` (`TypeCheckerCall.cpp:1223`) | The base expression's type |
| Dereference (indexing) | `visitArrayAccess` (`TypeCheckerCall.cpp`, OVH-003) | The base expression's type — same `self`-exemption reasoning as `visitFieldAccess`, verbatim |
| Struct field | `visitStructDecl` (`TypeCheckerDecl.cpp:158`) | Any field's type; the struct's own `Loc` is what gets checked for exemption |

The cast site is worth spelling out since it's the one place a
raw↔safe *conversion* actually happens:

```cpp
// TypeCheckerExpr.cpp, visitCast()
if (!checkUnsafeGate(Node.getDestTypePtr().get(), Node.getLoc(), &Node))
  checkUnsafeGate(OperandTy.get(), Node.getLoc(), &Node);
```

Destination checked first; only if the destination *isn't* itself a
gate violation do we also check the operand. This means `raw to &T`
(destination safe, operand raw) is gated on the operand — it's the
"raw-to-safe, might have been null" direction the whole feature exists to
guard — while `ref to T*` (destination raw, operand safe) is gated on the
destination, even though that specific conversion is separately always
*value-sound* (see the `typesCompatible()` Rule 2.5 in 2.5.4) — an
explicit cast still names a raw pointer type, so it's still a "cast" use
under the table in 1.2.

The struct-field site stops at the first raw-pointer field found — one
diagnostic per struct is enough, since the fix (wrap the whole decl) is
identical regardless of which field triggered it.

#### 2.5.3 `self` and the function-signature rule

`checkNoRawPointersInSignature()` (`TypeCheckerCore.cpp:174`), called
unconditionally from `visitFuncDef()` (i.e. *not* gated by `UnsafeDepth_`
at all — a plain function's signature is checked regardless of whether
the whole declaration happens to be lexically inside `unsafe { }`),
iterates `Node.getParams()` and the declared return type, raising "Raw
pointer in function signature!" for any `isRawPointer()` match. `extern
fn` goes through a separate visitor (`visitExternFuncDecl()`) entirely and
is unaffected either way.

Both this check and `visitFieldAccess`'s dereference gate special-case
`self` **by parameter/variable name**:

```cpp
// checkNoRawPointersInSignature, TypeCheckerCore.cpp
if (Param.Name == "self") continue;

// visitFieldAccess, TypeCheckerCall.cpp
bool IsSelfBase = false;
if (const auto *BaseVar = dynamic_cast<const VariableExprAST *>(Node.getBase()))
  IsSelfBase = BaseVar->getName() == "self";
if (!IsSelfBase) checkUnsafeGate(...);
```

`self`'s implicit parameter is still synthesized as an ordinary raw
`TurfPointerType(NamedType(StructName))` (`ParserDecl.cpp`, unchanged by
this issue) — it was never retyped to `&Self`. `self` is always non-null
by construction, but retyping it risked disturbing method-mangled-name
and `dynamic_cast` assumptions across a large amount of already-tested
code, for a change that would have been purely cosmetic (the *behavior*
of `self.field` access is identical either way). Name-based exemption is
safe here specifically because `self` is a reserved keyword token
(`TOK_SELF`) — a user can never declare their own variable literally
named `self`, so there's no collision risk.

This is also why `Counter.increment(): &Counter { ... return self to
&Counter; }`-shaped methods (returning `self` by reference for chaining,
1.5's `Box` example is the same shape) need their own `unsafe { }` around
the `return` — converting `self`'s raw type into the declared `&Counter`
return type is exactly the raw-to-safe cast direction that always needs
one, `self`'s exemption from the *dereference* gate notwithstanding.

#### 2.5.4 `typesCompatible()` rules (`src/Types.cpp`)

Three changes, all inside the existing null/opaque-pointer-coercion rule
block:

```cpp
// Rule 1 (null): excludes &T targets
auto isPointerLikeStrict = [](const TurfType *T) {
  return (T->isPointer() && !T->isReference()) ||
         T->isNamed("ptr") || T->isNamed("void*");
};

// Rule 2 (opaque ptr/void* <-> T*): raw -> &T direction excluded,
// &T -> opaque direction UNCHANGED (still allowed)
if (isOpaquePtr(From) && To->isPointer() && !To->isReference()) return true;
if (isOpaquePtr(To) && From->isPointer()) return true;

// Rule 2.5 (new): &T -> concrete T2* always allowed (safe-to-raw widening)
if (const auto *FromRef = dynamic_cast<const TurfReferenceType *>(From)) {
  if (!To->isReference()) {
    if (const auto *ToPtr = dynamic_cast<const TurfPointerType *>(To)) {
      if (FromRef->getPointee()->isEquivalent(ToPtr->getPointee().get()))
        return true;
    }
  }
}
```

Both `isPointer()` checks above matter *because* `&T` inherits it — without
the explicit `&& !T->isReference()`/`!To->isReference()` guards, `null`
and a bare `ptr`/`void*` would already have satisfied the pre-existing
rules by accident, which is exactly backwards for a type whose entire
purpose is a non-null guarantee. Rule 2.5 deliberately uses
`isEquivalent()` for the pointee comparison, not a recursive
`typesCompatible()` call — that keeps it from accidentally also permitting
unsound pointee widening (e.g. `&int -> double*` riding along on the
unrelated int-to-double numeric-widening rule elsewhere in this same
function).

No rule was added for the reverse direction (raw `T*`/`ptr` → `&T`) — it
correctly falls through to the function's final `return false;`, which is
exactly what forces the explicit `to`-cast-inside-`unsafe` path in 1.3.

The pre-existing pointer-to-generic-parameter rule
(`Types.cpp`, checked via `dynamic_cast<const TurfPointerType *>` on both
sides before this issue existed) needed **no changes** to also cover
`&T` — since `&T` already `dynamic_cast`s to `TurfPointerType*`
successfully via inheritance, a generic function/struct parameterized
over `T` already accepts a `&T` argument the identical way it always
accepted a `T*` one.

The identical `&T`-vs-`T*` reconstruction bug 2.4 found in
`substituteTypeParams()` was also present in `TypeCheckerCall.cpp`'s
`substituteReturnType()` (the TypeChecker's own, independent generic
return-type substitution helper) and fixed the same way — check
`TurfReferenceType` before the general `TurfPointerType` branch.

### 2.6 Codegen

**Almost nothing changed.** `UnsafeBlockExprAST::codegen()`
(`CodegenControlFlow.cpp`) is a pure pass-through:

```cpp
Value *UnsafeBlockExprAST::codegen() {
  return Body->codegen();
}
```

No unsafe-tracking exists in Codegen at all — by the time Codegen runs,
the TypeChecker has already rejected anything the gate disallows, so
there's nothing left to check. `getLLVMType()`'s existing
`dynamic_cast<const TurfPointerType *>` branch already covers
`TurfReferenceType` (derived-to-base `dynamic_cast` always succeeds), so
both `T*` and `&T` lower to the identical LLVM representation — an opaque
`ptr` in LLVM's own type system, with no `&`/reference-specific IR
construct anywhere. `AddressOfExprAST::codegen()` is completely unchanged
from before this issue: it still just returns the operand's lvalue
address via `codegenLValue()`, regardless of whether the TypeChecker
labels the result `T*` or `&T`.

**One real fix was needed**, in `CFG.cpp`'s return-path/control-flow
analysis (the "does every path through this function return?" check).
Its statement dispatcher special-cases specific AST node kinds via
`dynamic_cast` (`if/while/for/return/break/continue/...`); anything that
doesn't match one of those falls into a generic "ordinary statement,
opaque to this analysis" bucket. `UnsafeBlockExprAST` didn't match any
existing case, so a `return` lexically inside `unsafe { }` was invisible
to this pass — producing a false "doesn't always return" diagnostic on
any function whose only `return` happened to be `unsafe`-wrapped. Fixed
by adding an explicit case that delegates straight to the block's `Body`,
exactly like the `BlockExprAST` case right next to it:

```cpp
} else if (auto *Unsafe = dynamic_cast<UnsafeBlockExprAST *>(E)) {
  visitBlock(Unsafe->getBodyMut());
}
```

### 2.7 Known gaps / deliberately out of scope

- **Calling a method through a raw pointer receiver isn't gated either** —
  only reading/writing a *field* through one is. `visitMethodCall()` has
  no equivalent check.
- **No `unsafe_ptr<T>`.** The much larger `MEMORY_ARCHITECTURE_RFC.md`
  proposal (full ARC, heap-managed strings/arrays, a dedicated
  `unsafe_ptr<T>` FFI type) is explicitly out of scope for this track —
  see `docs/planning/FUTURE.md`'s "Memory Architecture (ARC)" section. `ptr`/`T*` are
  the only raw-pointer spellings; they're unchanged in meaning, just newly
  gated.
- **`self` stays a raw pointer internally**, by deliberate choice (2.5.3)
  — not retyped to `&Self`.
- **Codegen's own independent `getExprType()`** (`CodegenCore.cpp`, a
  second, parallel type-inference implementation used only for internal
  Codegen bookkeeping) has at least two more sites that reconstruct a
  plain `TurfPointerType` from an existing pointer/reference match — one
  in its generic-struct-field-substitution logic, one in its
  `AddressOfExprAST` type cache. Both were deliberately **not** patched:
  Codegen runs after the TypeChecker has already validated everything, and
  every consumer of this function's result only cares about
  "pointer-shaped for GEP/auto-deref purposes" (`isPointer()`, still
  correctly `true` either way), never about the safe/raw distinction — so
  the divergence is real but behaviorally inert. Flagged here in case a
  future change makes Codegen care about `isReference()` directly.

### 2.8 LANG-002 additions, by pass

Summary only — `docs/development/changes/LANG-002.md` has the full detail.

- **Types (`include/Types.h`, `src/Types.cpp`).** `TurfNullableReferenceType`
  is a SIBLING of `TurfReferenceType` (both direct `TurfPointerType`
  subclasses), not a further subclass — `isReference()` stays `false` for
  it (only `&T` is the "always non-null" contract), and a new
  `isNullableReference()` predicate is the one place callers ask "is this
  specifically `&?T`". That single choice makes almost every existing
  pointer-widening `typesCompatible()` rule apply to `&?T` unchanged; only
  one new rule (`&?T -> T2*`, mirroring the existing `&T -> T2*` rule) was
  needed.
- **Parser (`src/ParserCore.cpp`, `src/ParserModule.cpp`).** `ParseType()`
  gets a one-token `?` lookahead right after `&`. The `unsafe module;`
  capability pragma is parsed by `ParseLeadingUnsafeModuleDecl()`
  (`ParserModule.cpp`) at the three places a file's first token is seen
  (`ParseFile()` for imports; `main.cpp`'s pre-pass and main-pass entry-file
  loops) — see that function's own doc comment for why one token of
  lookahead forces it to consume `unsafe` before it can tell a pragma apart
  from a genuine top-level `unsafe { }` block.
- **TypeChecker (`include/TypeChecker.h`, `src/TypeCheckerCore.cpp`).**
  `isRawPointer()` additionally excludes `isNullableReference()` — `&?T` is
  exempt from the raw-pointer gate the same way `&T` is (its own safety is
  a SEPARATE mechanism, NullabilityPass, not this gate).
  `HasRawPointerCapability()` (`ParserModule.cpp`, exported via `Parser.h`)
  replaces `IsPathUnderLibStd()` at both call sites
  (`isUnsafeGateExempt()`, `checkNoRawPointersInSignature()`).
- **CFG-based analysis (`include/Analysis.h`, `src/Analysis.cpp`).**
  `NullabilityPass` — a forward dataflow pass over the CFG, structurally
  copied from `DefiniteAssignmentPass` just above it in the same file
  (a `std::unordered_set<const VariableSymbol*>` per block, meet =
  intersection), but two-state rather than three (see its own doc comment
  for why "proven non-null, or not" is enough). Run from
  `FuncDefExprAST::codegen()` (`src/CodegenFunc.cpp`), the same pipeline
  position as every other CFG pass — after the TypeChecker, before real
  LLVM codegen.
- **Generics (`src/ResolverCore.cpp`'s `substituteTypeParams()`,
  `src/Monomorphize.cpp`'s `substituteConcreteTypes()`).** Both needed the
  identical fix `&T` already required (2.4's own doc comment): a
  `dynamic_cast<TurfNullableReferenceType*>` branch, checked BEFORE the
  generic `TurfPointerType` fallback, so a substituted generic `&?T` field/
  parameter/return type doesn't silently rebuild as a plain `T*`.
- **Codegen.** Nothing — `getLLVMType()`'s existing
  `dynamic_cast<TurfPointerType*>` branch already covers the new sibling
  class for free, exactly as it did for `&T` (2.6).

### 2.9 FDN-002 additions, by pass

Summary only — `docs/development/changes/FDN-002.md` has the full detail.

- **CFG-based analysis (`include/Analysis.h`, `src/Analysis.cpp`).**
  `EscapePass` — a forward dataflow pass over the CFG, beside
  `NullabilityPass` in the same file: one `std::unordered_map<const
  VariableSymbol*, RefProvenance>` per block, meet = "keep an entry only
  where every predecessor agrees on the exact same provenance" (the same
  shape as `NullabilityPass`'s `NonNullSet` intersection, generalized from
  a boolean to a 3-value enum), transfer = process a block's statements in
  order via a recursive `ASTVisitor` (`EscapeCheckVisitor`), same idiom as
  `NarrowingVisitor` just above it. Run from `FuncDefExprAST::codegen()`
  (`src/CodegenFunc.cpp`) immediately after `NullabilityPass`.
- **Diagnostics (`include/ErrorsMemory.h`, `include/ErrorCodes.h`).** One
  new class, `DanglingReferenceEscapeError` (E086, `MEMORY_ERROR` category
  — no new SLM category, per F-DIAG-3), parameterized by a short sink
  description string so one class covers all five sinks.
- **No Lexer/Parser/AST/Types/Resolver/TypeChecker/Monomorphizer/Codegen
  changes.** This is the one CFG pass in the whole milestone list that
  needed nothing upstream — every fact it needs (a symbol's `IsParameter`,
  its `Type->isPointer()`/array size, `ArrayAccessExprAST::
  ResolvedIndexSetTarget` from LANG-001, `FuncCallExprAST`'s callee name for
  `heap()`) already existed on the AST from earlier milestones.
