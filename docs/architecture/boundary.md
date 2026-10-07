# The Compiler / Runtime / Standard-Library Boundary

What "owns" a given piece of TurfLang's behavior, and how that ownership is
enforced rather than merely claimed. Written by **STD-001**
(`docs/planning/ARCHITECTURE_OVERHAUL.md` §8), which exists specifically to
close the gap between what `AGENTS.md` and `ARCHITECTURE_OVERHAUL.md` §3
*say* about this boundary and what the compiler actually does — see
§5.2/§15 of that document for the audit this doc finishes closing out.

Related docs:
- `docs/planning/ARCHITECTURE_OVERHAUL.md` §3 (target boundary diagram, the
  three rules) and §15 (the standard-library architecture chapter this doc
  is the "boundary is now real, not just diagrammed" follow-up to).
- `docs/architecture/pointers_and_references.md` §1.7/§2.5 — the
  raw-pointer capability mechanism this doc's §3 summarizes.
- `include/Intrinsics.h` — the closed intrinsic list and the prelude
  module list, both authoritative in that header; this doc explains the
  *why*, the header is the *what*.
- `scripts/check_boundary.sh` — the mechanical enforcement for every claim
  this document makes. Run it; do not take this document's word for it.

---

## 1. The three boundary rules (recap)

From `ARCHITECTURE_OVERHAUL.md` §3.3, restated here because this doc is
where their *enforcement* lives:

- **Rule B1** — the compiler owns anything whose correctness depends on
  TurfLang's type system (monomorphization, overload/operator resolution,
  GC descriptor emission, bounds-check elimination).
- **Rule B2** — the runtime (`libturfrt`) owns anything that must be true
  across the whole process and cannot be re-derived per translation unit
  (the heap, the collector, the root set, panic/OOM behavior).
- **Rule B3** — everything else is ordinary TurfLang in `lib/std`, and the
  standard library gets no capability a user cannot request explicitly.

Rule B3's second half — "no capability a user cannot request explicitly" —
is the one that was false until LANG-002, and is what §3 below confirms is
now actually true, not merely stated.

## 2. The closed intrinsic list

`include/Intrinsics.h` is the single, enumerated answer to "what does the
compiler know about by name?" — four categories, each recognized through a
genuinely different mechanism (real lexer keywords, LLVM intrinsic IDs,
`libturfrt` C entrypoints reached by name from Codegen-emitted IR, and the
prelude module list, §4 below). F-STD-2 (FREEZE.md §6.8): adding to it is
an explicit, change-recorded act, never a silent side effect of unrelated
work.

**Enforcement.** `scripts/check_boundary.sh` check 3 cross-checks every
name registered via `RegisterBuiltin(...)` in `src/Builtins.cpp`, plus the
two lexer-level intrinsics (`sizeof`, `typeof`), against `Intrinsics.h`'s
own text — a future addition to `g_builtins` that forgets to also document
itself in `Intrinsics.h` fails this check.

**What this enforcement does *not* claim.** It cannot mechanically prove
"no compiler-known symbol exists outside `Intrinsics.h`" for the whole
compiler — that would require exhaustively enumerating every
`CalleeName == "..."` comparison anywhere in `src/`, most of which are
ordinary dispatch on an already-resolved, already-typed call (not a
special-cased *name*). One genuine, narrow exception is worth naming
explicitly rather than leaving for a future session to "discover":

### One documented exception

`Codegen`'s own, independent type-inference helper — `getExprType()`
(`src/CodegenCore.cpp`), the duplicate of the TypeChecker's `NodeTypes_`
that `ARCHITECTURE_OVERHAUL.md` §25 item 4 names and explicitly forbids
unifying as a drive-by fix — has a last-resort fallback for a
`FuncCallExprAST` it cannot otherwise type:

```cpp
// Fallback for intrinsic path: malloc/realloc return ptr, free returns void
if (CalleeName == "malloc" || CalleeName == "realloc")
  return TurfPtr().get();
if (CalleeName == "free")
  return TurfVoid().get();
```

This is reached only after every other lookup in `getExprType()` (mangled
name, raw `CalleeName`, and a generic-substitution unification pass over
every declared function) has already failed to find a match. It grants no
capability, changes no legality (the TypeChecker — the authoritative type
checker, per Rule B1 — has already accepted or rejected the call by the
time Codegen runs), and only affects Codegen's own internal bookkeeping for
a call it would otherwise be unable to type at all. Verified live: it does
not affect what compiles — `lib/std/mem/mem.tr`'s `extern fn malloc`/
`realloc` (and any user file's own `extern fn free`, e.g.
`tests/05_arrays_pointers_ffi/*.tr`) are already found through the ordinary
`FuncReturnTypes` lookups earlier in the same function; this is a fallback
for an edge case in registration ordering, not the primary path.

**Left alone, deliberately.** Removing it means touching the exact
duplicate-inference engine `ARCHITECTURE_OVERHAUL.md` §25 item 4 protects
from unrelated refactors ("a large refactor with no user-visible benefit
... §9.7 gives a cheaper answer"). STD-001's own scope is boundary
*enforcement*, not a rewrite of a neighboring, already-documented wart —
doing so would be exactly the kind of "looks tempting from inside this
feature group" mistake §25 exists to name. `scripts/check_boundary.sh`
therefore does not attempt to catch this class of fallback; it is recorded
here instead, so it cannot be "discovered" again as if new.

## 3. No capability is granted by file path

`F-STD-1` (FREEZE.md §6.8) / `INV-CAP-1` (`docs/development/INVARIANTS.md`):
the raw-pointer capability (`ptr`/`T*` usable without `unsafe { }`) is
**declared in source** — a file's own first-line `unsafe module;` pragma
(`docs/architecture/pointers_and_references.md` §1.7/§2.8) — never inferred
from where the file lives on disk. The mechanism this replaced,
`IsPathUnderLibStd()` (an unconditional path-prefix check granting the
entire `lib/std` tree raw-pointer privileges by location alone, invisible
in the file that used it), no longer exists anywhere in the compiler.

**Enforcement.** `scripts/check_boundary.sh` check 1 greps `src/` and
`include/` for a *live* reference to `IsPathUnderLibStd` (a definition or
call site — doc-comment mentions explaining the history are excluded on
purpose, see the script's own comment) and fails if one exists; it also
confirms `HasRawPointerCapability()` — the declared-capability replacement,
`src/ParserModule.cpp` — is still exported. This is what makes AGENTS.md's
"No compiler special-casing; ordinary `.tr` files" claim about `lib/std`
true for the raw-pointer capability specifically, and *tested* rather than
merely asserted.

Being under the resolved install root (`TurfInstallRoot()`) still decides
whether a declared `unsafe module;` pragma is *honored unconditionally* vs.
needing `--allow-unsafe-modules` — this is not a regression back to
path-based privilege: the capability must be **declared** either way (an
undeclared file gets nothing regardless of location); location only
answers "is opting in ambient or does it need an explicit flag." See
`pointers_and_references.md` §1.7 for the full two-condition rule.

## 4. Prelude module specification

`int`, `double`, `bool`, `char`, `strings`, `array`, `io`, `math`, `sys` are
auto-imported into every top-level compile with no `import` statement
(`ParsePreludeImports()`, `src/ParserModule.cpp`), and `io`, `math` and `sys`
also have their public free functions callable by bare name
(`PreludeBareModuleNames`; `FREEZE.md` §6.25) — see
`docs/architecture/modules_and_scope_resolution.md` Part 1.3/2.4 for the
user-facing and internal mechanics. Before STD-001, this list
(`PreludeModuleNames`) was a bare hardcoded C++ array with no written
specification of *why* these modules and no check that it was complete or
correct. (It was six until 2026-10-07, when the owner added `io`, `math` and
`sys`; those three files moved into `core/` so the rule below stayed true.)

**The specification**: the prelude is exactly the set of `.tr` files under
`lib/std/core/`. This is not incidental — `ARCHITECTURE_OVERHAUL.md` §3.2's
target boundary diagram already labels that layer `core (prelude)`. Nine
files live there today (`int.tr`, `double.tr`, `bool.tr`, `char.tr`,
`strings.tr`, `array.tr`, `io.tr`, `math.tr`, `sys.tr`), matching
`PreludeModuleNames` exactly.

**Deliberately not derived from a directory listing at compile time.**
`lib/std`'s other subfolders (`collections/`, `io/`, `mem/`, `math/`, …)
are explicitly documented (`AGENTS.md`, `modules_and_scope_resolution.md`
Part 2.4) as purely organizational — invisible to `import`, which resolves
a module by name via a recursive index of the whole tree, never by
subfolder. Making `core/` additionally drive *auto-import* behavior at
runtime would make that one subfolder secretly load-bearing in a way none
of the others are — a file moved into or out of `core/` for ordinary
organizational reasons would silently change what every program in
existence gets auto-imported with. Keeping `PreludeModuleNames` an
explicit, named list — checked *against* `core/`'s contents rather than
*generated from* them — keeps any such change visible and deliberate,
matching F-STD-2's "explicit, change-recorded act" standard for the
intrinsic list above.

**Enforcement.** `scripts/check_boundary.sh` check 2 diffs
`PreludeModuleNames`'s declared contents against a live directory listing
of `lib/std/core/*.tr` and fails on any drift in either direction — a
module added to `core/` without updating the array, or vice versa, is
caught immediately rather than sitting as a silent inconsistency.

## 5. A `mem::alloc` result may never hold a GC-heap pointer

OVH-002 (`docs/development/changes/OVH-002.md`): F-MEM-3 (FREEZE.md
§6.5) says the two heaps never merge and the collector never traces into
manual memory — the corollary this rule makes explicit is that nothing may
rely on the collector doing so implicitly, either. A `mem::alloc`/
`mem::alloc_zeroed`/`malloc`/`calloc`/`realloc` result (the manual heap,
by definition) must never be the ONLY thing holding a reference to a
GC-heap value (a `string`, a collection's backing buffer, any struct
containing either) — such a pointer is invisible to the collector's mark
phase no matter how reachable it looks from Turf source, so the value it
points at can be freed out from under it the moment nothing else roots it.

This was found live at four sites (`lib/std/json/json.tr`'s per-element
boxing, `showcase/kv_store_server.tr`'s per-entry boxing, and two
compiler-emitted allocations — a message-carrying error value, an
oversized `!T` success box — in `src/CodegenCore.cpp`), all four now fixed:
the two stdlib/showcase sites moved the boxed value into its containing
`List<T>` by value instead (a `List<T>`'s own backing buffer is precisely
traced, `docs/architecture/memory_model.md` §2.6), and the two
compiler-emitted sites moved from a bare `malloc` to `turf_rt_alloc` with a
real descriptor.

**Enforcement.** `scripts/check_boundary.sh`:
- Check 4: `getOrCreateMallocFn()` (the bare-`malloc` helper both
  compiler-emitted sites used before this fix) has no remaining definition
  or caller anywhere in `src/Codegen*.cpp` — deleted outright once its last
  caller was migrated, so a reintroduction under any name calling `malloc`
  directly for a GC-heap-bound value is the thing to watch for if this
  check ever needs updating.
- Check 5: a best-effort textual scan of `lib/std`/`showcase` for
  `mem::alloc(sizeof(SomeType))` where `SomeType` has a `string` or
  collection-typed field — exactly the shape both stdlib/showcase sites
  had. Best-effort, not exhaustive (see this check's own comment in the
  script): it does not catch a numeric-literal-sized `mem::alloc(N) to
  T*` box (the exact shape `kv_store_server.tr` used before this fix), a
  box built through an intermediate helper function, or `mem::Pool<T>`
  (§6 below) — those remain a human-reviewed judgment call, the same
  "best-effort, not total" posture §2's closed-intrinsic-list check
  already takes.

## 6. `mem::Pool<T>` — a known, disclosed, unfixed gap

`mem::Pool<T>` (`lib/std/mem/mem.tr`) exists to batch a set of *manual*
allocations for one-call bulk freeing — its own doc comment describes `T`
as "always some `*T`/`void*`" (a pointer the pool takes ownership of), the
intended, correct usage: `Pool<T>.items` (a `malloc`/`realloc`'d array)
holding manual-heap pointers is manual memory holding manual memory,
no cross-heap issue at all.

**Verified live (OVH-002): `Pool<T>` is not actually restricted to a
pointer-shaped `T`.** `T` is an ordinary, unconstrained generic parameter
(F-GEN-3: no trait/constraint system exists to express "T must be a raw
pointer" even if this project wanted to) — `mut pool: mem::Pool<string> =
mem::Pool::default(); pool.track(someString);` compiles and runs today,
storing a GC-heap `string` pointer inside `Pool<T>.items`'s manual-heap
array, invisible to the collector by construction — precisely §5's
forbidden shape, reachable through the public standard library with no
special effort. Left unfixed here deliberately: `Pool<T>` has no non-goal
naming it as OVH-002's business specifically, fixing it would need either
a constraint system (rejected, F-GEN-3) or a runtime check with no
precedent elsewhere in this stdlib, and nothing in `tests/`, `lib/std`, or
`showcase` actually instantiates `Pool<T>` at a GC-heap-backed `T` today.
Recorded here, rather than silently discovered again later, as a disclosed
gap — the same posture MEM-002's own "a GC pointer stored only in manual
memory is not a root" unsoundness note already takes.

## 7. What this document deliberately does not claim

Per `ARCHITECTURE_OVERHAUL.md` §25 item 24 ("do not clean up
`docs/architecture/`") and item 4 (do not unify Codegen's own duplicate
type-inference engine): this document names the one known exception in §2
rather than pretending the closed-intrinsic-list claim is unconditionally
total, and changes no behavior anywhere in the compiler — STD-001's own
scope is enforcement and specification of an already-landed boundary
(LANG-002, RT-001), not a redesign of it. §5/§6 (OVH-002) are additive to
that same posture: a best-effort mechanical check plus an honestly
disclosed, unfixed gap, not a claim that every possible cross-heap mistake
is now caught.
