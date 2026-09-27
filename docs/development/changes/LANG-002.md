# LANG-002 — Nullable references `&?T` and declared capabilities

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-23 · landed 2026-09-23 |
| **Commits** | (see `git log --oneline -- '*.h' '*.cpp' 'tests/39_nullable_references' 'tests/05_arrays_pointers_ffi/21_unsafe_module_capability_and_multi_extern.tr'` on this branch) |
| **Sessions** | 1 of 2 (roadmap estimated S16–S17; landed in one) |
| **Depends on** | none (independent of TYPE-002) |
| **Frozen decisions relied on** | F-PTR-1, F-STD-1 |

## Why

Verified live before this group started: `struct Node { next: Node* }`
required a top-level `unsafe { }`, and a plain function could never take or
return a raw pointer at all (`fn walk(n: Node*)` was rejected even inside
`unsafe { }`) — meaning user code had **no safe way to write a linked
list**, the single most visible language gap the architecture audit found.
Separately, `IsPathUnderLibStd` granted the entire `lib/std` tree raw-pointer
privileges by filesystem path alone — invisible in the file that used it,
ungreppable, and unavailable to a user writing legitimate FFI bindings for
their own C library, even though `lib/std/net`/`lib/std/fs` need exactly
that same privilege.

## Language semantics

### Old behaviour

```turflang
unsafe {
    struct Node { val: int, next: Node* }
}
fn walk(n: Node*): int { ... }   // ERROR even inside unsafe { }, always
```

Any file under `lib/std` could use `ptr`/`T*`/`void*` freely, regardless of
whether it said so anywhere.

### New behaviour

```turflang
struct Node { val: int, next: &?Node }   // no unsafe { } needed at all
fn walk(n: &?Node): int {
    if (n != null) { return n.val; }
    return -1;
}
```

A file's first top-level statement may declare `unsafe module;`, which
grants raw-pointer use throughout that file (honored unconditionally under
the install root, elsewhere only with `--allow-unsafe-modules`).

### Is this breaking?

Yes, narrowly, and by design (§8's own "Breaking changes" note). Every
`lib/std` file that used `ptr`/`T*`/`void*` (12 files: `mem`, the five
collections, `strings`, `fs`, `io`, `net`, `json`, `time`) now needs — and
has been given — a one-line `unsafe module;` addition. No user-code break:
`unsafe { struct … Node* }` keeps working exactly as before: `&?T` is a new,
additive option, not a replacement for `T*`.

## Compiler architecture

- **Lexer.** No new tokens. `&?T` reuses the existing `&` token plus a
  one-token `?` lookahead (unambiguous — `?` has no other meaning in this
  grammar); `unsafe module;` reuses the existing `unsafe`/identifier/`;`
  tokens.
- **Parser.** `ParseType()` (`src/ParserCore.cpp`): the `&`-branch gets a
  `CurTok == '?'` check before recursing into the pointee. New:
  `ParseLeadingUnsafeModuleDecl()` (`src/ParserModule.cpp`, declared
  `Parser.h`) — consumed at the three places a file's first token is seen
  (`ParseFile()`; `main.cpp`'s pre-pass and main-pass entry loops). New:
  `ParseUnsafeBlockBody()` (`src/ParserStmt.cpp`) — `ParseUnsafeExpr()`'s
  post-`'unsafe'` half, split out so the capability-pragma lookahead can
  share it once it determines a leading `unsafe` wasn't actually the
  pragma.
- **Types.** `TurfNullableReferenceType` (`include/Types.h`) — a SIBLING of
  `TurfReferenceType` (both direct `TurfPointerType` subclasses), not a
  further subclass of it. `isReference()` stays `false` for it (only `&T`
  is the "always non-null" contract); a new `isNullableReference()`
  predicate is the one place code asks "is this specifically `&?T`". One
  new `typesCompatible()` rule (`src/Types.cpp`, mirroring the existing
  `&T -> T2*` rule): `&?T -> T2*` widening. Every OTHER widening direction
  (`null -> &?T`, `&T -> &?T`, `&?T -> ptr/void*`) needed zero new rules —
  they already worked, for free, as a direct consequence of
  `isReference() == false` for `&?T` (see `TurfNullableReferenceType`'s own
  doc comment, Types.h, for the full reasoning). `&?T -> &T` (narrowing)
  deliberately has NO `typesCompatible()` rule at all, ever — the only
  bridge is an explicit `to` cast, checked by NullabilityPass.
- **Resolver / Monomorphize.** `substituteTypeParams()`
  (`src/ResolverCore.cpp`) and `substituteConcreteTypes()`
  (`src/Monomorphize.cpp`, what `TypeCheckerCall.cpp`'s
  `substituteReturnType()` now delegates to) each needed the SAME fix `&T`
  already required when it landed: a `dynamic_cast<TurfNullableReferenceType
  *>` branch, checked before the generic `TurfPointerType` fallback, or a
  substituted generic `&?T` silently rebuilds as a plain `T*`.
- **TypeChecker.** `isRawPointer()` (`include/TypeChecker.h`) additionally
  excludes `isNullableReference()` — `&?T` is exempt from the `unsafe` gate
  (declaration, cast, struct field, plain-signature rules) the same way
  `&T` is; its OWN safety is a completely separate mechanism.
  `HasRawPointerCapability()` (`src/ParserModule.cpp`, exported
  `include/Parser.h`) replaces `IsPathUnderLibStd()` — now DELETED — at
  both its call sites (`isUnsafeGateExempt()`,
  `checkNoRawPointersInSignature()`, `src/TypeCheckerCore.cpp`).
- **CFG / analysis.** `NullabilityPass` (`include/Analysis.h`,
  `src/Analysis.cpp`) — a forward dataflow pass over the CFG, structurally
  copied from `DefiniteAssignmentPass` in the same file (a
  `std::unordered_set<const VariableSymbol*>` per block, meet =
  intersection, silent-then-report two-phase shape), but TWO-state rather
  than the three-state (NonNull/Nullable/Null) lattice `DESIGN_NOTES.md` §1
  sketches — "proven non-null, or not" is all either check needs; see the
  class's own doc comment for why. Narrowing (`&&`/`||` short-circuit,
  direct `== null`/`!= null` comparisons) is implemented via
  `NarrowingVisitor`, an `ASTVisitor` subclass (matching this file's
  existing `AnalysisVisitor` idiom) that returns an `{OnTrue, OnFalse}`
  pair per expression. Two diagnostics: `PossibleNullDereferenceError`
  (E081, field access) and `UnnarrowedNullableCastError` (E082, the `to &T`
  cast). Run from `FuncDefExprAST::codegen()` (`src/CodegenFunc.cpp`),
  immediately after `AliasAnalysisPass` — same pipeline position as every
  other CFG-based check (after the TypeChecker, before real LLVM codegen).
- **Codegen.** No changes. `getLLVMType()`'s existing
  `dynamic_cast<TurfPointerType*>` branch already covers the new sibling
  class, exactly as it did for `&T`.
- **CLI.** New `--allow-unsafe-modules` flag (`src/main.cpp`,
  `g_AllowUnsafeModules`, `include/Lexer.h`) — must be set before any
  parsing happens, since `HasRawPointerCapability()` consults it.

## Runtime changes
None. `&?T` lowers to the exact same opaque pointer LLVM value as `&T`/`T*`
— zero Codegen cost, zero runtime representation change.

## Standard-library changes
12 `lib/std` files gained a one-line `unsafe module;` first statement (the
only files that actually used `ptr`/`T*`/`void*`, found by grepping every
`.tr` file for raw-pointer-shaped syntax and confirmed by direct-compiling
each afterward): `lib/std/mem/mem.tr`, `lib/std/collections/{list,map,queue,
set,stack}.tr`, `lib/std/core/strings.tr`, `lib/std/fs/fs.tr`,
`lib/std/io/{io,net}.tr`, `lib/std/json/json.tr`, `lib/std/time/time.tr`.
None of them migrated any field to `&?T` — §8 explicitly asked that this NOT
be bundled into this group ("Opportunistically, data-structure-shaped
stdlib code can migrate `T*` -> `&?T` -- but do NOT bundle that migration
into this group").

## New invariants

- **INV-CAP-1**: no raw-pointer capability is ever granted by filesystem
  path alone. Every grant traces to an explicit `unsafe module;` in the
  file itself (`HasRawPointerCapability()`, `src/ParserModule.cpp`), plus —
  outside the install root — an explicit `--allow-unsafe-modules` flag.
  `IsPathUnderLibStd` no longer exists.
- **INV-NREF-1**: `&?T`'s field-access/dereference is checked, never
  gated by `unsafe { }` — `isRawPointer()` (`include/TypeChecker.h`)
  excludes `isNullableReference()` unconditionally; `NullabilityPass`
  (`src/Analysis.cpp`) is the sole enforcement point for whether a specific
  access is sound.
- **INV-NREF-2**: `&?T -> &T` is NEVER implicit — `typesCompatible()`
  (`src/Types.cpp`) has no rule for this pair, on either side, permanently.
  The only bridge is an explicit `p to &T` cast, itself gated by
  `NullabilityPass` (not `checkUnsafeGate` — neither side is a raw
  pointer).

## Removed assumptions

- **"Under `lib/std`" used to mean "raw pointers always allowed."** It no
  longer does — a `lib/std` file with no `unsafe module;` line is gated
  exactly like any other file (verified live: temporarily removing the line
  from `mem.tr` during development reproduced `checkNoRawPointersInSignature
  `'s "Raw pointer in function signature!" on `alloc`'s `void*` return,
  confirming the OLD blanket exemption is genuinely gone, not just
  redirected).
- **"A file's raw-pointer privilege is knowable from its path alone"** is no
  longer true anywhere in the compiler — grep for `unsafe module;` is now
  the only correct way to find every capability-holding file; a path check
  would be wrong (both false-negative for a future non-`lib/std` capability
  file, and, in THIS repo's own dev-build layout specifically, false-positive
  — see the pitfall below).
- **"`isReference()` implies non-null"** is no longer safe to assume for
  every `TurfPointerType` subclass that ISN'T raw — `TurfNullableReferenceType
  ` deliberately does NOT set it, precisely so this old assumption keeps
  holding for the one type it was ever meant to describe (`&T`). Any new
  code that branches on `isReference()` to mean "safe to deref unconditionally"
  must now also check `!isNullableReference()`, or ask `isRawPointer()`
  instead if what it actually means is "is this gated".
- **"Every raw-pointer-shaped `dynamic_cast<TurfPointerType*>` match is safe
  to rebuild as a plain `TurfPointerType`"** was already false for `&T`
  (2.4's documented bug); it is now ALSO false for `&?T`, in the identical
  two call sites (`substituteTypeParams()`, `substituteConcreteTypes()`).
  A third such site would need the identical fix a third time.

## Tests

- **Added** `tests/39_nullable_references/01_linked_list.tr` — the group's
  own proof: push/pop/traversal/length over `&?Node`, no `unsafe { }`
  anywhere.
- **Added** `tests/39_nullable_references/02_binary_tree.tr` — the second
  canonical nullable-pointer data structure (two nullable children).
- **Added** `tests/39_nullable_references/03_narrowing_shapes.tr` — one
  function per narrowing source: `if`/`else`, early return, `while`
  condition, `&&` short-circuit, and re-narrowing after a reassignment
  resets prior narrowing.
- **Added** `tests/39_nullable_references/04_narrowed_cast.tr` — the
  `p to &T` narrowing cast, both narrowed (compiles) and unnarrowed
  (caught at the call site before ever reaching the cast).
- **Added** four `fail_*.tr` in the same directory:
  `fail_deref_without_check.tr` (E081), `fail_cast_without_narrowing.tr`
  (E082), `fail_null_to_safe_reference.tr` (`&T` stays non-null,
  unaffected by `&?T` existing), `fail_raw_pointer_signature_no_capability.tr`
  (a file with NO `unsafe module;` at all still can't use `ptr` in a plain
  signature).
- **Added** `tests/05_arrays_pointers_ffi/21_unsafe_module_capability_and_
  multi_extern.tr` — F1 (a capability-declaring file may write a
  `ptr`-taking plain function) and F2 (two `extern fn`s inside one
  top-level `unsafe { }` block) folded in from §15.8.
- Full suite: 718 -> 727 tests, all green (`./scripts/test_runner.sh`).
- `lib/std/**` and `showcase/**` compiled directly: all resolve (the two
  pre-existing `E034` self-import hits in `list.tr`/`mem.tr` are the
  documented, unrelated prelude-self-import bug class, confirmed unchanged
  by diffing their error text against `AGENTS.md`'s own description of it).

## Known pitfalls

- **F2 could not be reproduced.** §15.8 names "a second `extern fn` inside a
  top-level `unsafe { }` block fails with 'Expected ';' after statement'"
  as a live, verified parser bug. Direct repro attempts against the CURRENT
  parser — plain externs, `void`-returning externs, `extern from "lib"`
  externs, two-vs-three declarations, with and without parameters — all
  compiled cleanly. Most likely explanation: it was fixed as an
  unrelated side effect of some other change between the audit and this
  session (this compiler's parser has had continuous churn on statement-
  terminator handling). `21_unsafe_module_capability_and_multi_extern.tr`
  locks in the now-correct behavior as a regression test either way — if
  this ever regresses, that test will catch it even though this session
  never touched the code path it exercises.
- **`&?T -> &T` narrowing is checked for variable declarations and plain
  assignments only** — NOT for function-call arguments or `return`
  statements. `typesCompatible()` never grants `&?T -> &T` implicitly at
  all (see New invariants), so this gap can't silently compile something
  unsound; it means a would-be-legal narrowed pass-by-value or narrowed
  return currently has no path to compile at all (must go through an
  explicit local `to`-cast first, then pass/return the result). Widening
  this is a natural, separate follow-up — NOT a soundness fix, an
  ergonomics one.
- **Narrowing is per-variable and intraprocedural, nothing more.**
  `if (p.field != null)` narrows nothing (only a bare variable compared
  directly to `null` does); a null check in one function tells a different
  function nothing about ITS OWN parameter's state, even for what looks
  like the same variable across a call boundary. This is the deliberate
  scope §8's own "Risks" paragraph asks for ("Start deliberately local...
  document the limits rather than over-engineering"), not an oversight.
- **The "capability declared, but `--allow-unsafe-modules` NOT passed,
  outside the install root, should be rejected" fail case is NOT
  exercisable as a `tests/` regression test in THIS repository.**
  `TurfInstallRoot()`'s fallback (no `TURF_HOME` set) resolves to
  `<compiler binary's directory>/..`, which for a standard `build/turf`
  layout IS this repository's own root — meaning `tests/`, `showcase/`,
  and even `src/` are ALL "under the install root" from the compiler's own
  point of view, same as `lib/std`. Verified live, both ways, OUTSIDE the
  repository (`/tmp/...`, with `TURF_HOME` unset via `env -u TURF_HOME`):
  a file there with `unsafe module;` and a `ptr`-taking function is
  rejected without `--allow-unsafe-modules` and accepted with it. The
  mechanism is correct; it is simply untestable from a file that lives
  inside this checkout. `fail_raw_pointer_signature_no_capability.tr`
  covers the OTHER, fully testable half (no pragma at all, rejected
  regardless of location).
- **`isNullableReference()` must be checked anywhere `isReference()` is
  currently used to mean "definitely non-null, safe to skip a null check."**
  Exactly two such sites existed before this group (`typesCompatible()`,
  the `unsafe` gate) and both were audited and fixed; a THIRD such site,
  if one is ever added, needs the same care — see Removed assumptions.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| `docs/architecture/pointers_and_references.md` | §1.1's "three pointer-shaped types" | Updated in this session — now four, plus new §1.7/§1.8/§2.8 |
| `docs/reference/structs_and_enums.md` | "Self-referential structs" documented the `unsafe { }`-wrapped `T*` pattern as THE way | Updated in this session — `&?T` is now the lead example, `T*` demoted to a "still legal, rarely needed" note |
| `docs/reference/ffi_and_unsafe.md` | §3's linked-list example, §4's pointer-type table (both said "anywhere under the standard library") | Updated in this session — `&?T` example added, table + new §4a for the capability declaration |
| `README.md` | Its own "FFI"/"`unsafe { }` and safe references (`&T`)" sections (per `pointers_and_references.md`'s own "Related docs" list) still describe the pre-LANG-002 world | NOT updated this session — lower-traffic than the reference docs above; flagged in `handbook-sync/PENDING.md` |

### Examples that became invalid
None outright invalid (raw `T*` self-referential structs still compile
exactly as documented) — the OLD examples are now "still correct, no longer
the recommended pattern," which is why they were kept as a labeled
secondary example rather than deleted.

### Examples that should be added
Done in this session: the `&?T` linked-list example in both
`structs_and_enums.md` and `ffi_and_unsafe.md`, and §1.8's narrowing
example in `pointers_and_references.md`.

### Sections that must NOT change
`pointers_and_references.md` §1.3–§1.6 (plain `&T`) and §2.1–§2.7 (the
pre-existing `unsafe`/`&T` implementation) — this group is additive to all
of it; none of `&T`'s own semantics changed.

## Unresolved questions
None raised during this session — no frozen decision was contested.

## Related records
None yet (first session to touch this area since `ARCH-001`).
