# The Compiler / Runtime / Standard-Library Boundary

What "owns" a given piece of TurfLang's behavior, and how that ownership is
enforced rather than merely claimed. The rules are frozen in
`docs/development/FREEZE.md` §6.8 (F-STD-1, F-STD-2) and §6.5 (F-MEM-3); this
document says how the compiler keeps them true, and
`scripts/check/check_boundary.sh` checks the mechanical parts on every run. Run
it; do not take this document's word for it.

Related docs:
- `docs/archive/ARCHITECTURE_OVERHAUL.md` §3 (the target boundary diagram and the
  three rules) and §15 (the standard-library architecture).
- `docs/architecture/pointers_and_references.md` §1.7 — the raw-pointer
  capability mechanism §3 below summarizes.
- `docs/architecture/runtime.md` — what `libturfrt` owns.
- `src/sema/Intrinsics.h` — the closed intrinsic list and the prelude module
  list, authoritative in that header; this document explains the *why*, the
  header is the *what*.

---

## 1. The three boundary rules

- **Rule B1** — the compiler owns anything whose correctness depends on
  TurfLang's type system (monomorphization, overload and operator resolution,
  GC descriptor emission, bounds-check elimination, structural hashing).
- **Rule B2** — the runtime (`libturfrt`) owns anything that must be true across
  the whole process and cannot be re-derived per translation unit (the heap, the
  collector, the root set, panic and OOM behavior).
- **Rule B3** — everything else is ordinary TurfLang in `lib/std`, and the
  standard library gets no capability a user cannot request explicitly.

Rule B3's second half is what §3 enforces: the one privilege the library needs
that user code does not have by default, raw pointers, is a declaration any file
can make.

## 2. The closed intrinsic list

`src/sema/Intrinsics.h` is the single, enumerated answer to "what does the
compiler know about by name?" There are four kinds, each recognized through a
different mechanism:

1. **Bare intrinsics**: `sizeof`, `typeof`, `heap`, `hash_of`, `gc_alloc`,
   `gc_alloc_zeroed`, `gc_realloc`, `gc_collect`, `gc_is_managed`,
   `gc_checked_free`, `gc_pin`, `gc_unpin`, and `caller_location` (legal only in a
   function marked `@caller_location`, `caller_location.md`). `sizeof` is a lexer
   keyword with its own grammar production; the rest are entries in the builtin
   registry (`RegisterBuiltin(...)`, `src/sema/Builtins.cpp`) whose arity the
   registry holds. The only place a name is compared is `LookupIntrinsic()` in
   `Intrinsics.h`, which maps it to the `BareIntrinsic` enum; the Resolver asks it
   **only for a bare call that no declared function matched** and records the
   answer on the call node (`FuncCallExprAST::ResolvedIntrinsic`). Every later
   pass reads that tag and never compares a name, so a program that declares its
   own `heap` or `typeof` gets its own function, not the built-in.
2. **LLVM intrinsics**: IDs from `llvm::Intrinsic::*` that Codegen requests
   directly; invisible to the Resolver and TypeChecker.
3. **Runtime entrypoints**: the C functions in `libturfrt` (`runtime/turfrt.h`,
   the `turf_rt_*` family) that Codegen-emitted IR calls by name. Never reachable
   from Turf source: no `extern fn` in `lib/std` declares them.
4. **The prelude module list** (§4): not a callable symbol, but the other half of
   "what the compiler knows by name".

Adding to the list is an explicit, change-recorded act, never a side effect of
unrelated work (F-STD-2).

**Enforcement.** `check_boundary.sh` check 3 cross-checks every name registered
through `RegisterBuiltin(...)`, plus `sizeof` and `typeof`, against
`Intrinsics.h`'s own text, so a later addition to the registry that forgets to
document itself fails the check (INV-STD-2).

**What this check does not claim.** It cannot prove that no compiler-known symbol
exists outside `Intrinsics.h` for the whole compiler: that would mean
enumerating every `CalleeName == "..."` comparison in `src/`, most of which are
ordinary dispatch on an already-resolved call (`main`, the `new`/`default`
constructors, `self`) and not a special-cased symbol. What was once a narrow
exception here, a last-resort `malloc`/`realloc`/`free` fallback in Codegen's
own `getExprType()`, no longer exists in the code.

## 3. No capability is granted by file path

F-STD-1 / INV-CAP-1: the raw-pointer capability (`ptr`/`T*` usable without
`unsafe { }`, and in plain function signatures) is **declared in source** — a
file's own first-statement `unsafe module;` pragma
(`pointers_and_references.md` §1.7, §2.5.1) — and never inferred from where the
file lives. `HasRawPointerCapability()` (`src/frontend/ParserModule.cpp`,
exported through `Parser.h`) is the single query. It requires the file to have
parsed the pragma, full stop; location only decides whether the declaration is
*honored* on its own:

- under the resolved install root (`TurfInstallRoot()`): honored, which is how
  `lib/std` uses it with no flag;
- in a package build: honored for the root package and for dependencies the root
  manifest lists under `[unsafe] allow` (per package, not transitive);
- anywhere else: honored only with `--allow-unsafe-modules`.

An undeclared file gets nothing regardless of location. The standard library is
the model of this: of its 24 modules, the 12 that bind C or manage raw memory
declare `unsafe module;` (`list`, `stack`, `queue`, `set`, `map`, `hashing`,
`mem`, `strings`, `io`, `fs`, `net`, `time`), and the other 12 (`os`, `random`,
`json`, `http`, `test`, and the prelude modules `math`, `sys`, `int`, `double`,
`bool`, `char`, `array`) contain no raw-pointer type in code at all, reaching
memory only through the safe wrappers the first group exports. A user's own FFI bindings can get the identical capability
with the same line. The raw C bindings themselves (`malloc`, `fopen`, `socket`,
...) are `extern fn` declarations that stay private to their module behind
checked wrappers (`FREEZE.md` §6.20, F-API-4).

**Enforcement.** `check_boundary.sh` check 1 greps `src/` and `include/` for a
*live* reference to `IsPathUnderLibStd` (the name of the path-prefix check this
design excludes; doc-comment mentions that explain the history are excluded on
purpose) and fails if one exists, and confirms that `HasRawPointerCapability()`
is still exported from `Parser.h`. The function that answers "is this diagnostic's
location library code?" for the print-time system-module warning filter is
deliberately named `IsLibStdWarningLocation()`: it grants no capability, and it
must not reuse the guarded name.

## 4. Prelude module specification

`int`, `double`, `bool`, `char`, `strings`, `array`, `io`, `math` and `sys` are
auto-imported into every top-level compile with no `import` statement
(`ParsePreludeImports()`, `src/frontend/ParserModule.cpp`; the list is
`PreludeModuleNames`). `io`, `math` and `sys` also have their public free
functions callable by bare name (`PreludeBareModuleNames`; `FREEZE.md` §6.25); the
other six are reached through UFCS, operators and `module::` only, because their
function names (`length`, `max` over arrays, `is_even`) read as methods and would
be wrong as free names. See `modules_and_scope_resolution.md` for the user-facing
and internal mechanics.

**The specification**: the prelude is exactly the set of `.tr` files under
`lib/std/core/`. That is not incidental — `ARCHITECTURE_OVERHAUL.md` §3.2's
boundary diagram labels the layer `core (prelude)`. Nine files live there
(`int.tr`, `double.tr`, `bool.tr`, `char.tr`, `strings.tr`, `array.tr`, `io.tr`,
`math.tr`, `sys.tr`), matching `PreludeModuleNames` exactly.

**Deliberately not derived from a directory listing at compile time.**
`lib/std`'s other subfolders (`collections/`, `io/`, `mem/`, `fs/`, ...) are
purely organizational and invisible to `import`, which resolves a module by name
through a recursive index of the whole tree, never by subfolder. Letting `core/`
additionally drive auto-import at run time would make that one folder secretly
load-bearing: a file moved in or out for tidiness would silently change what every
program is imported with. Keeping `PreludeModuleNames` an explicit list, checked
*against* `core/` rather than *generated from* it, keeps any such change visible
and deliberate.

**Enforcement.** `check_boundary.sh` check 2 diffs `PreludeModuleNames` against a
live listing of `lib/std/core/*.tr` and fails on drift in either direction.

## 5. A `mem::alloc` result may never hold a GC-heap pointer

F-MEM-3 (FREEZE.md §6.5) says the two heaps never merge and the collector never
traces into manual memory. The corollary is that nothing may rely on the collector
doing so implicitly. A result of `mem::alloc`, `mem::alloc_zeroed`, `malloc`,
`calloc` or `realloc` (the manual heap, by definition) must never be the **only**
thing holding a reference to a GC-heap value (a `string`, a collection's backing
buffer, any struct containing either). Such a pointer is invisible to the mark
phase however reachable it looks from Turf source, so the value it points at can
be freed out from under it as soon as nothing else roots it.

The rule is kept at both kinds of allocation site:

- **Standard library and showcase code** boxes nothing by hand: a collection of
  structs holds them by value (`List<Entry>`, not `List<Entry*>` with each entry
  `mem::alloc`'d), because a `List<T>`'s own backing buffer is precisely traced
  (`memory_model.md` §2.6). `lib/std/json/json.tr` and
  `showcase/kv_store_server.tr` are written this way.
- **Compiler-emitted allocations** that must outlive the creating function go on
  the GC heap through `turf_rt_alloc` with a real type descriptor, never through a
  bare `malloc`: an error value and its payload (`CodegenErrorVariantConstruct()`),
  an oversized `!T` success value boxed into the 64-bit slot (`BoxToI64Slot()`),
  and a `heap()` copy (`HeapAllocateValue()`), all in
  `src/codegen/CodegenCore.cpp`. Regression tests are in `tests/41_gc/`
  (`08_error_message_survives_collection.tr`,
  `09_boxed_fallible_success_survives_collection.tr`,
  `09_error_payload_survives_collection.tr`).

**Enforcement.** `check_boundary.sh`:
- Check 4: `getOrCreateMallocFn()` (the bare-`malloc` helper the compiler-emitted
  sites once used) has no definition or caller anywhere in
  `src/codegen/Codegen*.cpp`. A reintroduction under any name that calls `malloc`
  directly for a GC-heap-bound value is the thing to watch for if this check ever
  needs updating.
- Check 5: a best-effort textual scan of `lib/std` and `showcase` for
  `mem::alloc(sizeof(SomeType))` where `SomeType` has a `string` or
  collection-typed field. It is best-effort, not exhaustive: it does not catch a
  numeric-literal-sized `mem::alloc(N) to T*` box, a box built through an
  intermediate helper, or `mem::Pool<T>` (§6); those remain a human-reviewed
  judgment, the same "best-effort, not total" posture as §2's check.

## 6. `mem::Pool<T>` — a known, disclosed gap

`mem::Pool<T>` (`lib/std/mem/mem.tr`) batches a set of *manual* allocations for
one-call bulk freeing; its documentation describes `T` as "always some `*T` /
`void*`" (a pointer the pool takes ownership of). Used as intended, its backing
array holds manual-heap pointers: manual memory holding manual memory, with no
cross-heap issue.

**`Pool<T>` is not actually restricted to a pointer-shaped `T`.** `T` is an
ordinary unconstrained generic parameter, and TurfLang has no trait or constraint
system to say "T must be a raw pointer" (F-GEN-3). `mut pool: mem::Pool<string> =
mem::Pool::default(); pool.track(someString);` compiles and runs, storing a
GC-heap `string` pointer in the pool's manual-heap array where the collector
cannot see it, which is exactly §5's forbidden shape, reachable through the public
standard library. It is left unfixed: closing it needs either a constraint system
(rejected) or a run-time check with no precedent elsewhere in the library, and
nothing in `tests/`, `lib/std` or `showcase` instantiates `Pool<T>` at a
GC-backed `T`. It is recorded here as a disclosed gap, the same posture as the
documented rule that a GC pointer stored only in manual memory is not a root.

## 7. Other mechanical checks

`check_boundary.sh` also enforces two structural invariants that protect the
boundary from the inside:

- **Check 6 (INV-GEN-2)**: every generic-parameter query — does a type contain an
  open `TurfGenericParamType`, substitute it, unify it against a concrete type —
  goes through the one shared walker, `src/sema/TypeWalk.h`. A bare
  `dynamic_cast<const TurfGenericParamType` anywhere else in `src/` or `include/`
  is a hand-rolled copy of the pattern. Two exceptions are excluded on purpose:
  the INV-GEN-1 assertion sites (`assert(!dynamic_cast<...>)`, which check that
  none reaches Codegen) and `CodegenCore.cpp`, whose separate `getExprType()`
  type-inference engine reimplements the pattern with raw `const TurfType *`.
- **Check 7 (INV-API-1)**: the per-subsystem front-end state save and restore
  hooks (`PushParserCompileState`, `DiagnosticEngine::saveState`, ...) are called
  only from the query facade (`src/tooling/AnalysisFacade.cpp`), which is the one
  supported way to run a compile in-process from a tool
  (`include/turf/Analysis.h`).

## 8. What this document does not claim

- It does not claim every possible cross-heap mistake is caught: §5's check is
  best-effort and §6 is an open gap.
- It does not claim the intrinsic list is mechanically total (§2).
- It does not describe a redesign of the boundary: the document and the script
  record and enforce what the compiler, runtime and library already do. Codegen's
  duplicate type-inference engine (`getExprType()`) is a known wart that is
  deliberately left alone.
