# TYPE-001 — Struct layout acyclicity

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-18 · landed 2026-09-18 |
| **Commits** | `<pending>` |
| **Sessions** | 1 of 1 |
| **Depends on** | ARCH-001 |
| **Frozen decisions relied on** | F-INIT-3 (a struct may not contain itself by value, transitively, checked once in the Resolver before any layout or default value is computed). |

## Why

`struct A { me: A }` — an infinitely-sized type — compiled with zero
diagnostics. The declaration alone never crashed anything (registering a
struct's name computes no layout), but the moment anything tried to
*instantiate* the type, the compiler itself segfaulted: confirmed live,
`mut x: A;` made `turf --resolve` exit 139 (SIGSEGV), no output at all. The
same held for a cycle spread across two declarations
(`struct A { b: B } struct B { a: A }`) — neither struct is individually
self-referential, so a naive "does this struct contain itself" check on
`visitStructDecl()` alone would have missed it; the cycle only exists once
both declarations are considered together.

This blocks SEM-004 (recursive default init needs every struct to have a
computable default) and TYPE-002 (monomorphizing a type with no finite
layout diverges) — both need INV-LAYOUT-1 to already hold.

## Language semantics

### Old behaviour

```turf
struct A { me: A }
fn main(): int {
    mut x: A;   // turf --resolve: SIGSEGV, exit 139, no output
    return 0;
}
```

A struct declared but never instantiated (`struct A { me: A }` with no
`mut x: A;` anywhere) compiled and linked successfully — the bug was
entirely latent until something forced a layout/default-value computation.

### New behaviour

```turf
struct A { me: A }
fn main(): int { return 0; }
```

now fails at the end of Resolver Pass 1, **before Pass 2 or Codegen ever
runs**, with:

```
── error at line 1, col 1 [E069]

  struct 'A' cannot contain itself by value.
  cycle: A -> A
  Note: field 'A::me' has type 'A' (line 1, col 12)

  Hint: Make one of these fields a reference or pointer ('&A', '&?A', 'A*')
  instead of holding the struct by value -- a struct that contains itself
  by value has no finite size.
```

The mutual, array-mediated, and generic-template cases are rejected the
same way, each naming the full cycle (`cycle: A -> B -> A`, etc.) and one
`Note:` per field that forms it. A pointer field (`next: Node*`, `prev:
&Node`) is unaffected and remains the correct way to write a real
recursive data structure — see `tests/06_structs/08_self_ref_struct.tr`.

### Is this breaking?

Yes, narrowly. A program that *declared* a cyclic struct without ever
instantiating it now fails to compile at all (previously it compiled and
linked, doing nothing useful with the type). No such program could ever
have run correctly — instantiating the type was always either a segfault
or, for a struct declared-but-unused, dead code. No test in the existing
suite, `lib/std/**`, or `showcase/**` exercised this pattern, so nothing in
this repository needed migration.

## Compiler architecture

- **Resolver** — new `Resolver::checkStructLayoutAcyclicity()`
  (`src/ResolverCore.cpp`), called from `resolveAll()` immediately after
  `hoistTopLevel(Nodes)` returns (end of Pass 1, strictly before Pass 2).
  Runs a grey/black-coloured DFS over the by-value containment graph built
  from a new member, `HoistedStructDecls_` (every `StructDeclExprAST *`
  hoisted so far by `hoistTopLevel()`/`hoistModuleBody()`) — added because
  `ResolverTypeRegistry` only ever stored a struct's *name* and declaration
  location, never its fields, so there was nothing to walk without keeping
  the AST nodes around. Reports the first cycle found per strongly-connected
  component (a struct is never re-visited once fully explored/coloured
  black), not once per node touched.

  Edge rule (`byValueEdgeTarget()`, anonymous namespace,
  `ResolverCore.cpp`): a field is an edge to struct `S` if its type, after
  unwrapping any number of array dimensions, is a `NamedType` or a
  `GenericType` whose own base name is `S`. A pointer-shaped field
  (`TurfType::isPointer()` — covers `TurfPointerType`, its
  `TurfReferenceType` subclass, and `OpaquePointerType`/`ptr`) is never an
  edge and is **not** unwrapped further. A `GenericType`'s *type arguments*
  are deliberately not recursed into — this is a template-level check
  (`struct Box<T> { inner: Box<T> }` is a cycle via its own base name
  regardless of `T`; a *different* generic struct's type argument, e.g.
  `List<Container>`, is correctly not flagged just because `Container`
  happens to be the argument — TYPE-002 re-runs the same function per
  concrete instantiation, where a substituted `T` genuinely can turn a
  template-safe generic into a cyclic concrete one).

- **Diagnostics** — new `StructLayoutCycleError` (`include/ErrorsStruct.h`),
  registered as `E069`/`DECLARATION`/`Important=true`
  (`include/ErrorCodes.h`). Human-facing prose at
  `docs/errors/codes/E069.txt`, SLM-facing grounding text at
  `docs/slm/codes/E069.txt`.

No other pass changed. TypeChecker, Codegen, and CFG/analysis are
unaffected — they now simply never see a cyclic struct at all.

## Runtime changes

None.

## Standard-library changes

None. `lib/std/**` was swept by direct `turf --resolve` compile (see
*Known pitfalls* for the two pre-existing, unrelated failures this sweep
surfaced) and by the full test suite, which exercises every collections
module (`list`, `map`, `stack`, `queue`, `set`) through ordinary `import`.
None of them are by-value-cyclic (they're all pointer-backed), so none were
affected.

## New invariants

**INV-LAYOUT-1** (already named in `INVARIANTS.md` as `planned` before this
session — see its own history there): every registered `TurfStructType` has
a finite, computable size. Enforced by
`Resolver::checkStructLayoutAcyclicity()`. Moved to `holds` in
`INVARIANTS.md` by this change record.

## Removed assumptions

- **A struct declaration alone was always safe, regardless of its fields.**
  It is no longer safe to assume every `StructDeclExprAST` that survives
  Pass 1 hoisting has a computable layout — before this check, the only
  place that could ever discover a cyclic struct was whatever downstream
  code first tried to compute its size (default-value construction, GEP
  emission, …), and it discovered it by crashing. Any future pass that
  computes a struct's layout, default value, or GC descriptor may now
  assume INV-LAYOUT-1 holds and skip its own cycle defense.
- **`ResolverTypeRegistry` was a complete picture of "what does the
  Resolver know about struct `S`".** It never was — it only ever held a
  name and a declaration location — but nothing needed `S`'s fields at
  Resolver time before this check, so the gap was invisible.
  `HoistedStructDecls_` is Resolver-instance-local, not part of the
  registry; a future pass wanting a struct's fields at Resolver time should
  decide whether to extend the registry itself or add another parallel
  side-table like this one, not assume one already exists.
- **A `NamedType` field reference that names a registered struct can only
  ever be "the field's type is that struct."** That's still true, but it's
  now also, separately, "that field is an edge in the layout graph" —
  anything that walks `StructField::Type` for a *different* reason (e.g. a
  future pass computing actual byte offsets) needs the exact same
  pointer/array/generic-base unwrapping this check does, or it will
  silently disagree with the Resolver about which fields are "real"
  containment. `byValueEdgeTarget()` is the one function that should be
  extended/reused if a next pass needs the identical unwrapping rule again,
  rather than re-deriving it.

## Tests

Added, `tests/06_structs/`:
- `fail_self_containing_struct.tr` — `struct A { me: A }`.
- `fail_mutual_struct_cycle.tr` — `struct A { b: B } struct B { a: A }`.
- `fail_struct_array_cycle.tr` — `struct A { bs: B[2] } struct B { a: A }`.
- `fail_generic_struct_self_cycle.tr` — `struct Box<T> { inner: Box<T> }`,
  the template-level generic case explicitly called out in this group's
  §8 "Generic interactions" note (not in the original 5-test list, added
  because it exercises a distinct branch of `byValueEdgeTarget()` — the
  `GenericType` case — that none of the other three tests reach).

Not added (already covered, verified still green rather than duplicated):
- "self-referential pointer still compiles" — `08_self_ref_struct.tr` and
  `09_linked_list.tr`/`13_linked_list_traverse.tr` already exercise a
  pointer-backed self-referential struct end-to-end (construct, chain,
  traverse). Confirmed still passing.
- "deep non-cyclic by-value nesting still compiles" —
  `06_struct_nested.tr` (`L1 { l2: L2 } L2 { l3: L3 } L3 { val: int }`,
  3 levels, genuinely acyclic) already covers this. Confirmed still
  passing.

`./scripts/test_runner.sh`: 610/610 passing (606 baseline + 4 new).

## Known pitfalls

- **The struct-layout-cycle check and Pass 1's own duplicate-struct-name
  check can mask each other in an unrelated stdlib sweep.** Directly
  compiling `lib/std/collections/list.tr` or `lib/std/mem/mem.tr` (as the
  *entry* file, `turf --resolve <file>`) fails with `E034`
  (`DuplicateTypeNameError`, "'List'/'Pool' already declared") — **this is
  a pre-existing bug, unrelated to TYPE-001**, confirmed by reproducing it
  on the baseline commit (`9d5751b`) before any of this session's changes.
  Cause: one of the six auto-imported prelude modules (`strings.tr`) does
  `import list::List;`, and `list.tr` itself does `import mem;` — so
  compiling `list.tr` (or `mem.tr`) directly hoists that same struct twice:
  once as the entry file's own top-level declaration, once again via the
  prelude's transitive reach back into the same file. `AGENTS.md`'s
  documented prelude-self-import fix (`ParsePreludeImports()` skipping a
  prelude module whose path equals the entry file's own) only covers a
  prelude module *being* the entry file, not a prelude module *transitively
  re-importing* the entry file through an intermediate module — a strictly
  wider bug class this session did not fix (out of scope for TYPE-001;
  §25's scope discipline applies). Every *other* `lib/std/**` file
  resolves cleanly by direct compile, and both `list`/`mem` compile and
  run correctly through ordinary `import` (exercised by
  `07_functions_modules/06_stdlib_list.tr` and every showcase program).
  A future session touching the prelude-import machinery should be aware
  this gap exists before assuming the existing fix is complete.
- **Do not extend `byValueEdgeTarget()` to recurse into `GenericType`'s
  type arguments "for completeness."** It would be wrong today, not just
  premature: at this point in the pipeline a generic struct's own type
  parameter is still an unsubstituted placeholder, so recursing into
  `Box<T>`'s `T` has nothing concrete to check, and recursing into a
  *different* generic's argument (`List<Container>`'s `Container`) would
  incorrectly treat `List` as containing `Container` by value even though
  `List` almost certainly stores it behind a pointer. This is intentionally
  deferred to TYPE-002, which re-runs the same function **per concrete
  instantiation**, where the argument has actually been substituted into a
  real field type.
- **The DFS's grey/black colouring only matters for `RecoverPerDeclaration`
  or a future multi-diagnostic mode.** In an ordinary compile,
  `TurfError::raise()` calls `std::exit(1)` (or `longjmp`s to the nearest
  recovery point) on the very first cycle found, so today only one
  `StructLayoutCycleError` is ever seen per compile regardless of how many
  independent cycles exist in the program. The colouring (skip anything
  already black) is there so the *function* is correct if that ever
  changes, not because it's currently observable — don't "simplify" it
  into an uncoloured DFS assuming today's single-shot `raise()` behaviour
  is permanent.
- **This check runs twice on a normal compile, and that's correct, not a
  bug to fix.** `main.cpp`'s double-parse pre-pass (§25 item 3 — explicitly
  not to be "fixed" opportunistically) constructs a *separate* `Resolver`
  instance (`PreR`) that also hoists every struct and therefore also runs
  this check, once independently from the main pass's `Resolver R`. Both
  runs see the same source and reach the same verdict; this mirrors every
  other Pass-1 validation in the compiler (e.g. `DuplicateTypeNameError`)
  and is not specific to this check.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| `docs/reference/structs_and_enums.md` §"Self-referential structs" | Silent on by-value self-containment entirely — only documents the pointer-field pattern | Add the by-value rule alongside it; see `handbook-sync/PENDING.md`'s TYPE-001 row for the exact wording and a suggested new example |

### Examples that became invalid

None — no existing handbook/reference example used a by-value cyclic
struct (it never worked, so nothing legitimate depended on it).

### Examples that should be added

`struct A { b: B } struct B { a: A }` rejected as `E069`, contrasted with
the same pair with one field changed to `&B`/`B*` to show the fix — see
the `PENDING.md` entry.

### Sections that must NOT change

The existing `next: Node*` example and its `unsafe { }` / no-raw-pointer-
parameter notes (lines 46–83 of `structs_and_enums.md`) are entirely
correct and unaffected — pointer fields were never in scope for this rule
and must not be re-explained as if they were.

## Unresolved questions

None.

## Related records

[ARCH-001](ARCH-001.md) — established this change-tracking system and
`FREEZE.md`, prerequisite for this group.
