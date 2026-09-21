# TYPE-002 — Monomorphization (the keystone)

| | |
|---|---|
| **Status** | in-progress |
| **Dates** | started 2026-09-21 · landed — |
| **Commits** | (added at the end of each sub-session) |
| **Sessions** | 4 of 8 (S11a, S11b, S12a, S12b done; S13a, S13b, S14a, S14b remain) |
| **Depends on** | TYPE-001 (landed 2026-09-18), SEM-001 (landed 2026-09-18); strongly benefits from OPT-001 (landed 2026-09-18) |
| **Frozen decisions relied on** | F-GEN-1, F-GEN-2, F-GEN-3, F-OP-1 (unaffected but adjacent — see §9.8) |

## Why

TurfLang's generics are currently **type erasure**, not monomorphization,
despite two doc comments in `include/Types.h` claiming otherwise (a
documented, real mismatch — `docs/architecture/compiler_pipeline.md` §7
already flags it). Every type parameter erases to one opaque pointer;
`List<int>` and `List<string>` are the identical compiled code and the
identical struct layout. This has four measured consequences
(`ARCHITECTURE_OVERHAUL.md` §9.2):

1. **ICE above one machine word.** `List<Big>` (a 24-byte struct) →
   `Internal compiler error: ... Invalid bitcast: bitcast %Big %x4 to ptr`.
2. **No per-`T` operator dispatch.** Inside `Map<K,V>`, both operands of
   `self.keys[i] == key` have static type `K`, so no
   `operator==(string,string)` candidate can exact-match — a raw pointer
   compare slips through instead. Reproduced by
   `tests/99_known_bugs/set_string_dedup_broken.tr`.
3. **No `sizeof(T)`.** `list.tr:27-28` hardcodes `self.cap * 8` with the
   comment *"In Type Erasure, all T are 64 bits"*.
4. **Fake pointers in the heap.** `CreateIntToPtr` on an `int` value `5`
   stores the pointer value `0x5`. This is the blocking reason: a precise
   garbage collector (F-MEM-1) cannot trace a heap where scalars are
   deliberately indistinguishable from addresses. Monomorphization is
   `F-MEM-1`'s explicit prerequisite (FREEZE.md §6.5 last bullet).

This session (S11a) makes **no compiler behavior change**. Its job is to
answer, in writing and against measured/verified facts rather than the
original blueprint's assumptions, the three questions later sessions cannot
proceed without: how many instantiations actually exist in the corpus (U-2),
where the monomorphizer physically hooks into `main.cpp`'s double-parse
structure (U-4 → U-4′), and how a generic struct instantiation's type
arguments are determined given the language's actual syntax (U-5, a
question the original blueprint didn't know to ask).

## Language semantics

### Old behaviour

```turf
struct Box<T> { val: T }
fn main(): int {
    mut b: Box<int> = Box { val: 5 };
    return b.val;
}
```

Compiles to ONE `TurfStructType` for `Box`, regardless of how many concrete
`Box<...>` instantiations appear anywhere in the program. `b.val`'s storage
is a single opaque pointer; the `int` `5` is stored as the raw pointer value
`0x5` via `CreateIntToPtr`, never dereferenced, and read back via
`CreatePtrToInt`. `GlobalSymbolTable` only ever holds `"Box"`.

### New behaviour (struct LAYOUT still target-only; free functions AND methods land S12a/S12b)

Same program, same output. `Box<int>` will compile to its own,
fully-specialized `TurfStructType` with a real `i64` field (not a pointer),
registered under an extended mangled name; a second `Box<Point>`
instantiation elsewhere in the same program gets its own, independent
layout (S13a/S13b, not yet true — struct LAYOUT itself is still one
shared, erasure-based type this session, by design). `TurfGenericParamType`
never reaches Codegen (INV-GEN-1, still `planned` — true today only for the
FREE-function and METHOD subset S12a/S12b land; the STRUCT's own field
storage still erases, on purpose, until S13a).

**What's real as of S12a**, for a genuinely free function (no `self`):
```turf
fn<T> identity(x: T): T { return x; }
fn main(): int {
    immut a: int = identity(42);
    immut b: double = identity(3.5);
    ...
```
compiles `identity<int>` and `identity<double>` to two separate, real LLVM
functions (verified live: `turf_identity_gen1_int_int` and
`turf_identity_gen1_double_double`, zero `inttoptr`/`ptrtoint` in either) —
not the shared, erased body every generic function still got before this
session.

**What's ALSO real as of S12b**, for a generic METHOD ON a generic
STRUCT (`lib/std/collections/list.tr`'s real, production `List<T>`):
```turf
mut ints: List<int> = List::default();
ints.push(10);
ints.push(20);
mut strs: List<string> = List::default();
strs.push("a");
```
`List<int>.push`/`.length`/`.pop` and `List<string>.push`/`.length`/`.pop`
compile to SIX separate, real LLVM functions (verified live at `-O0`:
`turf_List.push_gen1_int_List_int_ptr_int` takes a genuine `i64 %item`
parameter, not `ptr`; `turf_List.length_gen1_int_List_int_ptr` returns a
genuine `i64`) — with `List<T>`'s own STRUCT LAYOUT still the single,
shared, erasure-based `%List` LLVM type both instantiations' `self`
pointers point at (deliberate — S13a/S13b give each instantiation its own
layout). One narrower case is a disclosed, deferred gap, not covered by
S12b: a method that is generic IN ITS OWN RIGHT on a NON-generic struct
(`fn<T> Box.echo(x: T): T` where `Box` itself has no type parameters) is
never even recorded as an instantiation request today — see *Known
pitfalls*.

### Is this breaking?

**No, at the language level** — every program that compiles today is
intended to compile and behave identically once TYPE-002 lands in full (see
completion criteria in the `ARCHITECTURE_OVERHAUL.md` §8 entry). At the
implementation level, yes, substantially — see *Removed assumptions* below,
grown each sub-session.

## Compiler architecture

**S11a** planned this; **S11b** built the skeleton (report-only, zero
behaviour change — verified by the full suite staying at 676/676 and a
direct `lib/std`/`showcase`/prelude sweep finding no new diagnostic
anywhere):

```
Lexer → Parser → Resolver(2) → TypeChecker → Monomorphizer → Codegen
```

concretely inserted as a **new pre-pass sub-pass 2b.5**, between the
pre-pass's own `TypeChecker` run (sub-pass 2b, `main.cpp:892-901`) and its
struct codegen (sub-pass 2c). See U-4′ below for the full justification.

**What S11b actually added:**
- `include/Monomorphize.h` + `src/Monomorphize.cpp` (new `turfcore` source,
  added to `CMakeLists.txt`): `InstantiationKind` (FreeFunction/Method/
  Struct), `InstantiationRequest`, `InstantiationCache` (deliberately
  linear-scan, not hashed — see the header's own comment: U-2's census
  found ~18-20 real instantiations, so an O(n²) worst case over the whole
  program's `insert()` calls costs nothing, and it lets every comparison go
  through `isEquivalent()` pairwise instead of inventing a `TurfTypePtr`
  hash function that would risk falling back to identity-by-`getName()`,
  INV-TYPE-1), and `InstantiationCollector` (`record()`/`report()`). The
  `Worklist` member is populated but **not drained** this session — S12a's
  job; the shape exists now so S12a's "pop, clone, substitute, push new
  requests" loop (§9.5 steps 2-3) doesn't have to be reworked into it later.
- `FuncCallExprAST::GenericSubstitution` and
  `MethodCallExprAST::GenericSubstitution` (`include/ASTAccess.h`): the
  substitution map `collectGenericSubstitution()` already built and used to
  discard (see *Removed assumptions*) is now persisted on the node, empty
  for the overwhelming majority of (non-generic) calls, never copied by
  `clone()` — same contract as `ResolvedCallTarget`.
- `ResolverFunctionSymbol::mangledNameForInstantiation()`
  (`include/ResolverSymbolBase.h`) — purely additive, not called by
  anything yet. See *Removed assumptions* for why this replaced the
  originally-planned `TurfGenericParamType::getMangledName()` override.
- `TypeChecker::setMonomorphizationCollector()` + a nullable
  `MonoCollector_` member (`include/TypeChecker.h`): every `record()` call
  site (`TypeCheckerCall.cpp`'s `visitFuncCall`/`visitMethodCall`,
  `TypeCheckerDecl.cpp`'s `visitVarDecl`) is guarded on this being
  non-null, so an ordinary compile that never sets it is unaffected.
  `visitVarDecl` also records a **Struct**-kind request for a bare
  `mut b: Box<int> = ...;` with no method ever called on `b` — needed
  because a plain field read (`b.val`) needs `Box<int>`'s own layout to
  exist eventually, and would otherwise be invisible to a collector fed
  only from call sites.
- `GenericInstantiationDepthError` (`include/ErrorsMonomorphize.h`) + `E079`
  (`include/ErrorCodes.h`, `docs/errors/codes/E079.txt`,
  `docs/slm/codes/E079.txt`) and `Monomorphize::kMaxInstantiationDepth = 64`
  — registered and documented, **not yet raised anywhere**: nothing drains
  the worklist this session, so nothing can exceed the cap it would guard.
- `--dump-instantiations` (`src/main.cpp`): attaches the collector to the
  pre-pass's own `TypeChecker` (`PreTC`) for that one `checkAll()` call and
  prints the census to stderr afterward. Verified live against
  `showcase/text_toolkit.tr`: 11 distinct requests, correctly deduplicated
  (`List<string>` reported once as a `[struct]` request despite three
  separate method calls on it) — see this file's own git history for the
  full sample output.
- `tests/38_monomorphization/README.md` — the folder exists; empty of
  `.tr` files until S12a (nothing to test yet).

**S12a made free-function instantiation real** (behaviour change, gated
entirely on whether a call is to a genuinely free — non-`self` — generic
function; every existing generic STRUCT/METHOD call is bit-for-bit
unaffected, still erasure, still S12b/S13a). Verified live end to end:
`identity<int>`/`identity<double>` compile to two separate real functions
with zero `inttoptr`/`ptrtoint`; `sizeof(T)` inside a generic free function
now reports the true size (8 for `int`/`double`, 1 for `bool`); a nested
generic call (`outer<T>` calling `inner<T>`) discovers and drains the
nested instantiation correctly; a genuinely unbounded recursive generic
(`fn<T> wrap(x: &T): int { return wrap(&x); }`) is rejected with E079, not
run to memory exhaustion. Full suite green at 680/680 (676 + 4 new tests)
at both the default and `-O2` optimization levels; `lib/std`/`showcase`/
prelude sweep clean.

**What S12a added, concretely:**
- `Monomorphize::substituteConcreteTypes()` — S11a's plan said to
  "generalize `substituteTypeParams()`" for this; on actually building it,
  S11b's own `substituteReturnType()` (TypeCheckerCall.cpp) turned out to
  already BE that concrete-map walk, complete with the reference-before-
  pointer ordering fix. Relocated here as the real, callable, non-static
  entry point; `TypeCheckerCall.cpp`'s own `substituteReturnType()` is now
  a one-line delegating wrapper, so its existing call sites and behaviour
  are unchanged, verbatim. See *Removed assumptions*.
- `Monomorphize::instantiatedDeclName()` — the pure function of (base name,
  type arguments) that names a clone (e.g. `identity` + `[int]` →
  `identity_gen1_int`). Used as BOTH the clone's `FuncDefExprAST::Name`
  and (via the ordinary, UNMODIFIED `mangledName()`) its LLVM symbol —
  S11b's `ResolverFunctionSymbol::mangledNameForInstantiation()` is
  retired, unused; renaming the declaration once, up front, turned out
  simpler than teaching every Codegen call site a second mangling
  function. The method stays in `ResolverSymbolBase.h` as dead code this
  session (harmless; removing it is a trivial follow-up, not urgent).
- The body type-substitution walker (`substituteTypesInBody()`,
  `Monomorphize.cpp`, anonymous namespace) — walks a freshly-cloned
  function body substituting `TurfGenericParamType`/`NamedType`
  occurrences on the four node kinds that carry an explicit, parse-time
  `TurfTypePtr` (`VarDeclExprAST`, `CastExprAST`, `SizeOfExprAST`,
  `ArrayDeclExprAST` — each gained a new setter, `ASTDecl.h`/`ASTExpr.h`,
  for this). Every other node kind is walked purely for recursion,
  mirroring `ASTDiff.h`'s `CollectAllNodeIDsInternal()`. See *Known
  pitfalls* for the risk class this carries and why it's an accepted one.
- `FuncDefExprAST::setName()`/`::clearTypeParams()` (`ASTDecl.h`) — lets a
  clone be renamed and marked "no longer generic" before it's re-resolved.
- `Monomorphize::drainFreeFunctionRequests()` — the driver. Builds a
  declaration registry `(SourceModule, Name) -> FuncDefExprAST*` by walking
  `PrePassASTs` and recursing into every `ModuleImportExprAST`'s own body
  (confirmed live: an import's body is a real, walkable `BlockExprAST`
  subtree, not just a symbol-table registration); drains the worklist to a
  fixed point (a nested generic call discovered while re-typechecking one
  clone lands on the SAME worklist this loop is already draining); for
  each FreeFunction-kind entry: **skips it (leaves it recorded, undrained)
  if the declaration is struct-bound** (`Params[0].Name == "self"` —
  covers both ordinary methods and a constructor called via
  `Box::new(...)`, which resolves through this same free-function path —
  see *Removed assumptions*, this was found live, not anticipated);
  otherwise clones, substitutes Params/ReturnType/body, renames, clears
  `TypeParams`, re-resolves via `Resolver::resolve()` (the SAME `Resolver`
  instance that already ran `resolveAll()` over the whole program — its
  single-node, Pass-2-only entry point, reused rather than a new one built
  — this was this group's #1 named risk, S11a's plan, and turned out to be
  exactly this reusable already), re-typechecks with a custom error
  handler that captures a failure and re-raises it anchored at the
  instantiation site with a `withRelated()` note at the template
  declaration (§9.5's "where errors are reported" — see *Known pitfalls*
  for why this mechanism currently has very little to actually catch),
  appends the clone to `PrePassASTs` (picked up by the ordinary,
  UNMODIFIED sub-pass 2c/3a/3b codegen loops — no Codegen change needed
  for the free-function case at all), and rewrites every requesting call
  site's `ResolvedCallTarget` to the clone's own symbol.
- `main.cpp` sub-pass 2b.5 now actually calls
  `drainFreeFunctionRequests()`, gated on `PreTCClean` (same reason 2c/3a/
  3b already are). The collector is now attached UNCONDITIONALLY (not just
  under `--dump-instantiations` — S11b's opt-in gate only made sense while
  nothing consumed the worklist).
- Sub-pass 3a/3b's codegen loops gained a template-skip guard: a
  `FuncDefExprAST` with non-empty `TypeParams` AND no `self`-first-param is
  skipped (its own clone, not itself, gets codegenned) — **precisely** this
  population, not "any generic decl": getting this too broad silently
  drops every struct-bound generic's codegen with no replacement,
  reproducing the exact "LLVM function not found" ICE this fix chases (see
  *Removed assumptions*).

**S12b extended this to generic methods on generic structs** — the real,
production case (`List<T>`, `Map<K,V>`, `Stack<T>`, `Queue<T>`), not just
the free-function subset. Verified live end to end against `List<T>`:
`push`/`length`/`pop` on both `List<int>` and `List<string>` compile to six
separate, concrete LLVM functions with struct LAYOUT still fully shared
(by design — S13a). Full suite green at 681/681 (680 + 1 new test) at both
the default and `-O2` optimization levels; `lib/std`/`showcase`/prelude
sweep clean.

**What S12b added, concretely:**
- `InstantiationCollector`/`InstantiationCache` redesigned (S12b
  correction — see *Removed assumptions*): the destructive FIFO
  `Worklist_` (pop an index, act on it or drop it forever) is gone,
  replaced by a `Drained` flag per `InstantiationEntry` and a non-
  destructive full-cache index sweep (`Collector.cache().size()` re-read
  every loop iteration, so growth mid-sweep is picked up automatically).
  Needed the moment a SECOND drain function (methods) existed alongside
  S12a's free-function one — a destructive shared queue meant whichever
  function ran first silently discarded every entry the OTHER function was
  responsible for.
- `buildDeclRegistry()` and `finishInstantiation()` (`Monomorphize.cpp`,
  anonymous namespace) — the rename/clear-TypeParams/re-resolve/re-
  typecheck/register tail extracted out of `drainFreeFunctionRequests()`
  (unchanged behaviour, verified by the full suite staying green
  immediately after the extraction, before any new method-specific code
  was added) so `drainMethodRequests()` doesn't duplicate ~150 lines.
- `Resolver::resolveMonomorphizedClone()` (`Resolver.h`) — a new, small,
  additive Resolver entry point fixing two real bugs found live (see
  *Removed assumptions*): (1) `currentModuleName()` needs to be told which
  module a clone's TEMPLATE was actually declared in, for `self`'s own
  struct-name validation to succeed; (2) a NEW
  `SuppressUnqualifiedVisibilityCheck_` flag on `Resolver`, read by
  `validateNamedType()`'s two `isTypeVisibleUnqualified()` call sites
  (`ResolverDecl.cpp`), lets a clone's SUBSTITUTED type arguments name a
  struct from ANY module the ORIGINAL call site could see — not just what
  the template's own declaring module imports.
- `Monomorphize::drainMethodRequests()` — the Method-kind driver. Finds
  the method's template the same way `drainFreeFunctionRequests()` does
  (shared registry); substitutes every param EXCEPT `self` (index 0) plus
  the return type, the same way; ALSO rewrites `self`'s OWN declared type,
  from bare `StructName*` to `StructName<TypeArgs>*` — the one genuinely
  new mechanism S12b needed, safe despite struct layout staying fully
  erased this session for two independently-verified reasons (both in the
  function's own doc comment, `Monomorphize.cpp`): self's LLVM
  representation is unaffected either way (`getLLVMType()` already
  collapses a `GenericType`-typed struct pointer to the same shared opaque
  type as a bare one), and `CastToType()`'s existing erasure/un-erasure
  branches are driven by the VALUE's actual LLVM shape and the DESTINATION
  type, never by whether the AST source-side annotation says "generic" —
  confirmed by reading `CodegenCore.cpp`'s own comment on the un-erase
  branch, not assumed.
- `main.cpp`'s sub-pass 2b.5 now calls BOTH drain functions in an
  ALTERNATING loop (`while (Progress) { Progress = drainFreeFunctionRequests(...) | drainMethodRequests(...); }`),
  not once each in sequence — they are not independent: re-typechecking a
  method's clone can discover a nested call to a generic free function (or
  vice versa), and that nested request needs the OTHER drain function to
  act on it. This is what the non-destructive collector redesign above
  makes possible at all.

## Runtime changes

None. Eventually none in this group either — TYPE-002 is what makes
RT-001's type descriptors possible, not a runtime change itself.

## Standard-library changes

None. Planned, later sessions only (§8's TYPE-002 entry): `list.tr`'s
`cap * 8` → `cap * sizeof(T)` (S14b only — explicitly not bundled with
anything else).

## New invariants

None moved to `holds` this session — `INV-GEN-1` is still `planned` even
though S12a/S12b's free-function and method clones already individually
satisfy it (a partial, undeclared property isn't the same as the
compiler-wide guarantee the invariant names; that only becomes true, and
gets a debug-build assertion, in S14a). `INVARIANTS.md` already carries
`INV-GEN-1` and `INV-TYPE-2` as `planned — TYPE-002`, and `INV-TEST-1` as
`planned — TYPE-002's tests`; `INV-CG-1` (`planned`) is directly relevant to
U-4′ below. `E079` (a new *diagnostic*, not an invariant) is registered and,
as of S12a, actually enforced — verified live (see *Compiler architecture*
above) — but it guards a total-instantiation-count approximation, not the
literal chain-depth measurement §9.5 describes; see *Known pitfalls*.

## Removed assumptions

- **The blueprint's own file/line map has drifted** (verified this session
  and previously by the planning agent) — e.g. `getLLVMType`'s erasure
  branch is `CodegenCore.cpp:428-430`, not `~352` as the blueprint's
  "Relevant files" list says; `CastToType`'s erase/unerase pair is at
  `CodegenCore.cpp:1428-1490`, not `~1177`/`~1208`. Re-grep before editing,
  don't trust the doc's line numbers.
- **U-4's original framing was unimplementable as written.** The blueprint
  describes option (a) as "run monomorphization only in the authoritative
  [main] pass." Verified live this session: the main pass's own `FnASTs`
  list is explicitly "kept for Resolver/TypeChecker re-validation only"
  (`main.cpp:1057`) — function bodies are never re-codegenned there; only
  `TopLevelASTs` (file-scope statements) get real codegen in the main pass,
  and `main.cpp:1145-1164` makes a bare top-level statement outside any
  function a **hard compile error** ("Statements are not allowed outside of
  a function"). All real function/struct body codegen happens in the
  **pre-pass** (sub-passes 2c/3a/3b). See U-4′.
- **`collectGenericSubstitution()`'s substitution map is discarded, not
  persisted.** It's built fresh, function-locally, at
  `TypeCheckerCall.cpp:449-455`, used only to substitute the return type of
  *that one call expression*, and then goes out of scope. It is not stored
  on the AST anywhere. S11b must add a field to carry it.
- **A generic struct's type arguments never come from its constructor
  expression.** `Types.cpp:206-222`'s own comment says so directly: there is
  no `Box<int> { val: null }` syntax anywhere in the grammar. See U-5.
- **S11a's own plan for the mangling collision fix was slightly wrong,
  corrected in S11b.** S11a's session plan said the fix was
  "`TurfGenericParamType`'s explicit `getMangledName()` override." On
  actually implementing it: mangling only ever happens on a symbol whose
  `Params` are already **concrete** (post-substitution) — a
  `TurfGenericParamType` is never present at the point anything calls
  `mangledName()` on a real, about-to-be-codegenned instantiation.
  Overriding `TurfGenericParamType::getMangledName()` would only affect the
  *template's own* (still erasure-era, still actively used until S13b/S14a)
  mangled name — a change to a hot path this session's zero-behaviour-
  change mandate forbids touching without much more testing than a
  skeleton session warrants. The real fix (implemented instead, purely
  additive): a new `mangledNameForInstantiation()` method that embeds an
  explicit `_gen<N>_<typeargs>_` segment no ordinary `mangledName()` call
  can ever produce, so a monomorphized instantiation's symbol can never
  collide with a hand-written declaration's, regardless of what the
  substituted parameter types happen to be. S12a wires it in when it first
  registers a real specialization; `TurfGenericParamType::getMangledName()`
  is deferred to that session too, to be decided (if it's even still
  needed once the template stops being directly codegenned) with the real
  monomorphization pipeline available to test against, not blind against
  still-active erasure.
- **This repo has no C++-level unit test framework** (no gtest/Catch2 found
  anywhere under `external/` or `CMakeLists.txt`) — "unit-test the mangler"
  from S11a's plan is not literally achievable the way a C++ project with
  gtest would do it. `mangledNameForInstantiation()` is exercised instead
  by inspection (its logic is small and mirrors the already-relied-upon
  `mangledName()`) and will get real, live exercise once S12a calls it for
  the first time and the full suite either stays green or doesn't.
- **`std::unordered_map` iteration order is not declaration order.** The
  existing `Subst` map (`collectGenericSubstitution()`'s output) is keyed
  by type-parameter name with no ordering guarantee — fine for its
  original job (looking up one name at a time to substitute a return
  type), but an `InstantiationRequest`'s `TypeArgs` needs an **ordered**,
  index-comparable vector so two requests for the same instantiation
  produce comparable tuples. Fixed by a second small walk
  (`collectGenericParamNameOrder()`, `TypeCheckerCall.cpp`) that recovers
  first-seen declaration order independently of `Subst`'s own iteration
  order, mirroring `collectGenericSubstitution()`'s recursive shape.
- **S11b's `InstantiationRequest.DeclKey` alone couldn't distinguish two
  different modules' same-named generics.** Fixed in S12a by adding
  `SourceModule` to the struct and its `sameInstantiationAs()` comparison,
  populated at every `record()` call site from data already in hand
  (`Best->SourceModule` / `GenericType::getModule()`) — not deferred as a
  disclosed gap, since the fix was cheap once actually building the thing
  that needed the precision.
- **`InstantiationCache` genuinely needed to be a `std::deque`, not a
  `std::vector`, and this was found live, not anticipated.** S11b's plan
  called it a "linear-scan dedup, not a hash map" and left the container
  choice at that. `drainFreeFunctionRequests()` holds an
  `InstantiationEntry&` across the re-typecheck of one clone's body — and
  that re-typecheck can itself discover a NESTED generic call and record a
  brand new entry on the SAME cache. A `vector` reallocating on that
  `push_back` silently dangled the held reference; the `outer(7)` nested-
  call test (`tests/38_monomorphization/03_nested_generic_call.tr`) failed
  with "LLVM function 'turf_outer_T' not found" — the REWRITE step read
  garbage `Sites` off the dangling reference and never fired, so the call
  site kept pointing at the un-codegenned template. Switched to `deque`
  (stable references across `push_back`, unlike `vector`); no other change
  needed.
- **A struct-bound declaration reached through the free-function call path
  (`Box::new(...)`) is not a valid S12a target, and this was NOT
  anticipated in S11a's plan.** `Resolver::visitFuncCall`'s "self-stripped
  candidate" design means a constructor call is type-checked through the
  exact same machinery as an ordinary free-function call — so it arrives
  at `drainFreeFunctionRequests()` looking identical to a real free
  function. It isn't one: every struct-bound declaration takes an
  implicit `self: StructName*` with **no type argument of its own**
  (`self` is declared as a bare pointer, never `GenericType`), so a body
  referencing `self.<field>` (which a constructor always does) resolves
  that field's type from `GlobalSymbolTable`'s SHARED, never-per-
  instantiation-substituted struct registration — substituting the
  clone's own `Params`/body cannot fix that; only S13a's real
  per-instantiation struct layout can. Found live via
  `tests/06_structs/27_generic_struct_literal_field.tr`'s `Box::new(42)`:
  re-typechecking the naively-cloned `Box.new` failed with "Cannot assign
  value of type 'int' to variable of type 'T'" — `self.val`'s type stayed
  literally `NamedType("T")` no matter what `v`'s own type became. Fixed
  by excluding any `FreeFunction`-kind entry whose declaration has
  `Params[0].Name == "self"` from the drain (left recorded, undrained,
  same treatment as `Method`/`Struct` kinds) — S12b/S13a's job. Also
  caught, along the way: the `MonoCollector_->record()` call site in
  `visitFuncCall` was keying `Box::new`'s request by `CalleeName`
  ("new", the user-facing spelled callee) instead of `Best->BaseName`
  ("Box.new", the real declaration name) — for an ordinary free function
  the two always agree, so this only surfaced once a constructor call was
  actually exercised; fixed alongside the exclusion above.
- **`main.cpp`'s template-skip guard (sub-pass 3a/3b) needed to be
  narrower than "any generic `FuncDefExprAST`," and getting this wrong
  reproduced the exact bug it was meant to prevent.** The first version
  skipped codegen for every `FuncDefExprAST` with non-empty `TypeParams` —
  correct for the free functions S12a actually replaces with a clone, but
  it ALSO silently skipped every struct-bound generic (`Box.new`,
  ordinary methods), which S12a does NOT replace and which therefore still
  needs its ordinary, erasure-era codegen this session. The result was the
  identical "LLVM function ... not found. This is a compiler bug." ICE the
  DeclKey fix above was chasing, just from a different cause. Fixed by
  narrowing the guard to `!TypeParams.empty() && Params[0].Name != "self"`
  — exactly the population `drainFreeFunctionRequests()` itself acts on,
  kept in sync by comment cross-reference in both places.
- **Directly `--resolve`-ing a bare `lib/std/**` file "fails" for two
  different, unrelated reasons, only one of which is real.** Confirmed
  against the untouched baseline (stashed this session's changes and
  re-tested) before concluding either was pre-existing: (a) "No entry
  point found" for every non-prelude module file with no `main()` — this
  is **expected and benign**, not a regression signal, since these files
  were never meant to compile standalone into a binary; AGENTS.md's own
  sweep guidance means "no *new* diagnostic," not "produces a binary." (b)
  `list.tr` and `mem.tr` specifically report `E034` ("struct already
  declared") when compiled bare and standalone — reproduced identically on
  the pre-S11b baseline, so **pre-existing**, not introduced by this
  session, and out of scope to fix here. A future session sweeping
  `lib/std` should grep for a real error code/ICE/"Ambiguous", not treat
  "No entry point found" or this specific E034 as a failure.
- **A destructive shared worklist cannot serve two independent drain
  functions, and this was found live, not anticipated.** S11b/S12a's
  `Worklist_` was a FIFO of indices; `popWorklist()` removed an entry
  whether or not the caller actually acted on it (a Method/Struct-kind
  entry seen by `drainFreeFunctionRequests()` was `continue`-d past but
  still permanently gone from the queue). The moment `drainMethodRequests()`
  existed as a SECOND consumer, this broke silently: whichever drain
  function ran first exhausted the shared queue, discarding every entry of
  the KIND it doesn't handle before the function actually responsible for
  that kind ever got a chance to look. Fixed by replacing the destructive
  queue with a `Drained` flag per entry plus a non-destructive, re-read-
  each-iteration index sweep over the append-only cache (`InstantiationCache`
  is a `std::deque`, so indices and references both stay valid across
  growth) — see `InstantiationCollector`'s own doc comment (Monomorphize.h)
  for the full explanation.
- **`Resolver::resolve()` alone is not "the right scope" for a clone
  originally declared inside an imported module — this needed a real fix,
  not just documentation, and it was two DIFFERENT bugs, found in
  sequence.** (1) `currentModuleName()` (used by `validateNamedType()` to
  decide whether a bare struct name like "List" is "local," no
  `module::` qualification needed) reads `ModuleStack_`, which is empty
  for a bare `resolve()` call issued from OUTSIDE any import's own body
  traversal — exactly the monomorphizer's position. Confirmed live: a
  clone of `List.push` failed re-resolving its own `self: List*` parameter
  with "'List' isn't visible here without qualification." Fixed by pushing
  the template's own declaring module onto `ModuleStack_` for the duration
  (`Resolver::resolveMonomorphizedClone()`, replacing a plain
  `resolve()` call). (2) Fixing (1) immediately surfaced a SECOND, deeper
  failure: a clone's SUBSTITUTED type arguments can name a struct from ANY
  module the ORIGINAL call site could see, not just what the template's
  own declaring module imports — `List<JsonValue>.push`'s clone (declared
  in `list.tr`) needs to reference `JsonValue` (`json.tr`), which
  `list.tr` correctly never imports (an ordinary, non-generic `List` never
  needs to). There is no way to re-derive "was this specific substituted
  value already valid somewhere" from a bare `TurfTypePtr` with no
  back-pointer to where it was written, so the check is suppressed for the
  clone's own resolve() call instead of attempted incorrectly (trusting
  the substituted value, which only ever came from an already-successful
  type-check at a real call site elsewhere in the program) — a new
  `SuppressUnqualifiedVisibilityCheck_` flag on `Resolver`, scoped to
  exactly this one call.
- **Two independent mangling computations that normally agree — because
  both are derived from "which module is this declaration actually
  inside," computed at each pass's own natural traversal point — silently
  disagreed for a clone, and this was the hardest bug this session found**
  (reproduced with a minimal, ~15-line test before being understood at
  all). `ResolverFunctionSymbol::mangledName()` (`ResolverSymbolBase.h`)
  prepends `Sym.SourceModule`. `FuncDefExprAST::codegen()`
  (`CodegenFunc.cpp`) prepends `GlobalSymbolTable::GetCurrentModule()`
  instead — a CODEGEN-TIME GLOBAL, normally set/cleared around a
  `ModuleImportExprAST`'s own `codegen()` traversal of that module's body.
  A monomorphized clone is appended directly to `PrePassASTs`
  (`finishInstantiation`) and codegenned from that flat sub-pass 3a/3b
  loop, never from inside any module-import traversal —
  `GlobalSymbolTable::IsInModule()` is false the whole time. Leaving the
  clone's registered symbol with `SourceModule = "list"` (needed for
  `resolveMonomorphizedClone()`'s OWN validation, per the entry above) made
  the CALL SITE's `mangledName()` (which reads `Sym.SourceModule`) compute
  a DIFFERENT string than what the clone's OWN `codegen()` actually
  registered the LLVM function under (no module prefix at all, since
  `IsInModule()` is false). `TheModule->getFunction(MangledName)` then
  silently misses — `MethodCallExprAST::codegen()` returns `nullptr` with
  **no diagnostic at all** — and when that call sat on the RHS of a
  `||`/`&&` short-circuit, `BinaryExprAST::codegen()`'s own
  `if (!R) return nullptr;` early-return skipped emitting the branch to
  the merge block, leaving an orphaned, empty basic block: "Internal
  compiler error: ... Basic Block ... does not have terminator! label
  %or.merge" — a confusing, badly-misattributed diagnostic pointing at
  LLVM-verifier internals, nowhere near the real cause. Fixed by clearing
  `Clone->ResolvedDef->SourceModule` immediately after
  `resolveMonomorphizedClone()` succeeds: `instantiatedDeclName()` already
  makes the clone's own name globally unique, so it needs no module
  qualifier for mangling at all once renamed — the module context was
  only ever needed for validation, not for the registered symbol going
  forward. Regression test: `tests/38_monomorphization/04_method_on_generic_struct.tr`
  deliberately exercises a generic method call inside a `||` condition,
  inside another struct's own method body, matching the exact shape that
  caught this.
- **A method that is generic in its own right, on a NON-generic struct,
  is not covered by S12b at all — found live while writing this session's
  own tests, not something the plan anticipated.** `fn<T> Box.echo(x: T): T`
  called as `b.echo(42)` where `b: Box` (not `Box<...>`) never gets an
  instantiation request recorded in the first place:
  `TypeCheckerCall.cpp`'s `visitMethodCall` only ever builds `Subst` (and
  therefore only ever calls `MonoCollector_->record()`) when the RECEIVER's
  own type is a `GenericType` — for a non-generic struct, it never is, so
  the whole `if (auto *BaseGT = ...)` block guarding `record()` never
  runs. The call is silently still handled by the OLD, unmodified erasure
  path end to end (confirmed live: it still produces correct results,
  since erasure itself is completely untouched by this group so far — the
  gap is "not yet monomorphized," not "broken"). Closing it needs NEW
  Subst-building logic in `visitMethodCall` — arg-vs-parameter unification
  for the METHOD's own type parameters, mirroring
  `collectGenericSubstitution()`'s existing free-function logic — which is
  real, additional TypeChecker work, not just drain-function wiring;
  deferred to a later session rather than attempted under this session's
  time pressure. Disclosed here rather than silently left for someone to
  rediscover.

## Tests

`tests/38_monomorphization/README.md` added S11b. S12a adds the folder's
first four real tests, all passing (680/680 full suite, default and `-O2`):
- `01_free_function_multi_instantiation.tr` — `identity<int>`/`<double>`/
  `<string>` coexisting correctly in one program.
- `02_sizeof_t_in_generic_body.tr` — `sizeof(T)` reports a real value
  (8/8/1 for int/double/bool), not a hardcoded stand-in. Free-function-only
  half of completion criterion 4; the struct-field half is S13a/S14a's.
- `03_nested_generic_call.tr` — a generic calling another generic using
  its own `T`; this is also the regression test for the `InstantiationCache`
  dangling-reference bug (see *Removed assumptions*) — it failed before
  that fix and passes after, with no other change.
- `fail_generic_recursion_depth.tr` — genuinely unbounded generic
  recursion (`fn<T> wrap(x: &T): int { return wrap(&x); }`) rejected with
  E079, not run to memory exhaustion. Verified live: fires at exactly the
  configured `kMaxInstantiationDepth` (64), reports a (long, includes
  unrelated prelude instantiations — see *Known pitfalls*) chain.

**Not added in S12a, and why:** `fail_generic_requires_operator.tr`
(planned in S11a/S11b, "if exercisable already"). It is not exercisable
yet — see *Known pitfalls*'s entry on instantiation-site diagnostics
currently having very little to catch. Deferred to S13a/S13b, where
`Map<K,V>`'s real `self.keys[i] == key` dispatch bug
(`tests/99_known_bugs/set_string_dedup_broken.tr`) is the natural, already-
scheduled test for this mechanism once struct-bound generics are handled.

S12b adds one more test, all passing (681/681 full suite, default and
`-O2`):
- `04_method_on_generic_struct.tr` — `List<int>` and `List<string>`
  (real, production `lib/std/collections/list.tr`, not a toy stand-in)
  coexisting correctly in one program, exercising `push`/`length`/`pop` on
  both, PLUS a non-generic struct's own (non-generic) method calling a
  generic method inside a `||` short-circuit condition — deliberately
  reproducing the exact shape that caught the mangling-disagreement bug
  (see *Removed assumptions*). This test failed with "Internal compiler
  error: ... does not have terminator! label %or.merge" before that fix
  and passes cleanly after, with no other change.

## Known pitfalls

- **The pre-pass's own TypeChecker errors are informational-only and
  silently discarded** (`PreTCClean`, `main.cpp:895-916`) — if it fails,
  sub-passes 2c/3a/3b are skipped and the *main* pass's TypeChecker
  re-derives and prints the real diagnostic a few hundred lines later. The
  new monomorphizer sub-pass (2b.5) must be gated on `PreTCClean` exactly
  like 2c/3a/3b already are, for the same reason.
- **A monomorphization-only failure has no natural re-derivation point in
  the main pass.** Today's "silently discard pre-pass errors, let the main
  pass re-find them" pattern works because the main pass re-parses and
  re-type-checks the *same source text* the pre-pass already checked. But
  the main pass never sees a monomorphizer's cloned-and-substituted AST —
  it only ever re-type-checks the original generic *template*, which
  type-checks fine generically (Rule 1: a `TurfGenericParamType` matches
  anything). So an instantiation-site error (e.g., `T` lacking an operator
  the body needs) can ONLY ever be discovered and reported by the
  pre-pass's own monomorphizer — it cannot rely on the main pass to
  re-derive and print it the way every other diagnostic in this codebase
  currently does. **This breaks an assumption every other diagnostic in
  `main.cpp` currently relies on** and needs explicit handling in
  S12a (where instantiation-site diagnostics are first built), not
  papered over.
- **Mangling collision** (already flagged by the planning agent, reconfirmed
  live): `TurfGenericParamType` has no `getMangledName()` override, so
  `identity<int>` would mangle identically to a hand-written
  `identity(x: int)`. Must be fixed in S11b before any instantiation is
  registered under a real symbol name.
- **Instantiation-site diagnostics (§9.5's "where errors are reported")
  currently have almost nothing to catch, for free functions.** The
  mechanism S12a built (a custom `TypeChecker` error handler on the clone,
  re-raised anchored at the instantiation site with a `withRelated()` note
  at the template) is real, wired, and exercised by the depth-cap test —
  but a NATURALLY-occurring "generic template type-checks fine, generically
  — but THIS concrete instantiation doesn't" case turned out to not exist
  yet for a free function, in THIS compiler's current state. Every
  operation beyond bare pass-through/assignment on a `TurfGenericParamType`
  (an operator: `a + b` where both are `T`; a `to`-cast: `x to double`
  where `x: T`; a method call: `x.foo()` where `x: T`) is ALREADY rejected
  at the TEMPLATE's own ordinary type-check (sub-pass 2b's plain
  `PreTC.checkAll(PrePassASTs)`, which walks the un-substituted template
  too, unconditionally, before the monomorphizer ever runs) — confirmed
  live with both `a + b` (`E066`, "cannot apply '+' to 'T' and 'T'") and
  `x to double` (`E066`, "Cannot explicitly cast 'T' to 'double'"),
  neither of which ever reached my clone's re-typecheck at all. This is
  not a bug in S12a; it's §9.8's own interaction table being literally
  true one session early ("LANG-001 then adds the operators the
  collections want" — operators inside a generic body don't work at ALL
  yet, generically OR concretely, until LANG-001, which is explicitly
  sequenced AFTER this whole group). The mechanism will get its first
  REAL exercise in S13a/S13b, where a generic STRUCT method's body
  becomes concrete for the first time and things like `Map<K,V>`'s
  `self.keys[i] == key` (`LIMITATIONS.md` §1) start actually dispatching
  through real per-instantiation operator resolution — the point where a
  concrete-only failure can finally occur that the template's own generic
  check couldn't have already caught. Until then, do not assume this
  mechanism is dead code because nothing exercises it — it is wired and
  correct, just ahead of the feature (real generic-body operator support)
  that would give it something new to say.
- **The depth cap is a total-instantiation-count cap, not the literal
  parent-chain-depth measurement §9.5 describes, and the reported "chain"
  is not filtered to the actual offending chain.** `Monomorphize.h`'s own
  doc comment on `kMaxInstantiationDepth` already discloses the
  approximation (no per-request parent pointer is tracked); what's worth
  recording here is the CONCRETE effect, seen live: triggering it with a
  minimal, unrelated program still produced a "chain" naming ~70 entries,
  several of which were `List`/`List.push`/`List.length` instantiations
  from an unrelated prelude import, interleaved with the real `wrap`
  chain — because `GenericInstantiationDepthError`'s chain is built from
  `Collector.cache().all()` (every request recorded so far, in discovery
  order), not a walk of the specific parent-to-child chain that actually
  grew past the cap. Still correctly PREVENTS runaway compilation (the
  cap fires, the compile stops, no OOM) and still names the real offender
  among the noise — just noisier than the doc's own "like a C++ template
  backtrace" framing implies. A future session wanting a clean chain
  would need to add a parent-request pointer to `InstantiationRequest` and
  walk it backward from the request that tripped the cap, instead of
  dumping the whole cache.

## Handbook impact

Deferred to S14b in full (this session only records that it exists):
`docs/architecture/compiler_pipeline.md` §7 ("Generic type erasure",
`compiler_pipeline.md:884-945`) is entirely about the old model and will be
rewritten; `docs/architecture/memory_model.md` §2.7 (`memory_model.md:572-668`)
likewise; `docs/planning/LIMITATIONS.md` items 1-3; `docs/reference/generics_and_operators.md`;
`docs/reference/stdlib/collections.md`.

## Unresolved questions

All three below were the explicit deliverable of this session (S11a).
Each is now **resolved and recorded**, not still open — kept under this
heading per `TEMPLATE.md`'s structure, with the resolution inline.

### U-2 — how many instantiations actually exist? RESOLVED

**Methodology decision (this session's own engineering judgement, not a
frozen decision):** rather than adding compiler instrumentation this
session (which would be a `src/` change, and this session's explicit
mandate is zero behavior change), U-2 was answered by static census across
the whole corpus plus build measurements using the *existing* compiler
unmodified. A real, wired-up instantiation collector is explicitly S11b's
job (stage 5, "wire collector in report-only mode") — this session's
numbers exist to de-risk that work, not replace it.

**Census** (`lib/std`, `tests/`, `showcase/`, 2026-09-21, `c4696c3`):

| | Count |
|---|---:|
| Generic struct declarations in `lib/std` | 6 (`List<T>`, `Map<K,V>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `Pool<T>`) |
| Generic `fn<...>`/method declarations in `lib/std` | 31 |
| Distinct `Base<Args>` spellings across the whole corpus | 37 |
| ...of which concrete (excludes bare-param and negative-test spellings) | ~18 |

Close to the planning agent's earlier estimate (6 / 32 / ~34 / ~20) —
independently reproduced, not just copied.

**Baseline compile time / binary size** (for S14b's completion criterion 6
to diff against later):

| Program | Compile time (wall) | Binary size |
|---|---:|---:|
| Empty `main` | 0.40s | 16 KB |
| `showcase/text_toolkit.tr` (uses `List<string>`) | 0.44s | 17 KB |
| `showcase/kv_store_server.tr` (uses `Map`/collections) | 0.46s | 17 KB |

**Conclusion:** instantiation count in the current corpus is small (~18-20
concrete instantiations total). Instantiation explosion is **not** a
material risk for this codebase as it exists today; §9.4's own reasoning
("current compile times are 0.28-0.52s including a full stdlib re-parse, so
there is real headroom") is confirmed rather than assumed. No design
decision in S11b-S14b needs to be distorted to guard against combinatorial
blowup that isn't there.

### U-4 → U-4′ — where does the monomorphizer physically run? RESOLVED

**Decision:** the full monomorphizer (worklist seeding, clone, substitute,
re-resolve, re-typecheck, register) runs **entirely inside the pre-pass**,
as a new sub-pass **2b.5**, between the pre-pass's own `TypeChecker` run
(sub-pass 2b) and its struct codegen (sub-pass 2c). The main pass needs no
second monomorphizer run.

**Why, with the two facts that make this both necessary and sufficient**
(both verified live against `src/main.cpp` this session, not assumed from
the blueprint):

1. **Necessary:** real LLVM body codegen for every function and struct
   happens *only* in the pre-pass. `main.cpp:1057`'s own comment says
   `FnASTs` (the main pass's re-parse of every `fn`/`struct`/`enum`/
   `import`/`extern`) is *"kept for Resolver/TypeChecker re-validation
   only"* — never re-codegenned. The main pass's `TopLevelASTs` codegens
   only file-scope statements. So the monomorphizer's output (new,
   concrete AST clones) is useless if produced in the main pass — there is
   no second body-codegen pass left to consume it. This is exactly the
   error in the blueprint's original U-4 framing (P.3, discrepancy #2):
   option (a) as written assumed a main-pass body-codegen opportunity that
   does not exist.
2. **Sufficient (the part the original blueprint didn't check):** does the
   pre-pass actually *see* every instantiation request in the program?
   Yes — necessarily. `main.cpp:1145-1164`: a bare top-level statement
   outside any function is a **hard compile error** ("Statements are not
   allowed outside of a function"). Every expression in the entire program
   therefore lives inside a `fn` body (including `main()` itself) or is a
   struct field declaration — both fully present as sub-trees of the
   declarations the pre-pass already parses into `PrePassASTs` and walks in
   sub-passes 2a (Resolve) and 2b (TypeCheck). There is no call site or
   `GenericType` occurrence reachable *only* from the main pass's
   `TopLevelASTs` and invisible to the pre-pass. The pre-pass-only
   placement is therefore complete, not just convenient.

**Consequences for later sessions, recorded here so they aren't
re-discovered under time pressure:**

- Sub-pass 2c/3a/3b's codegen loops must **skip** any `StructDeclExprAST`/
  `FuncDefExprAST` that still has a non-empty `TypeParams` list (the
  template itself never gets real codegen — only its monomorphized clones,
  which sub-pass 2b.5 injects) — S12a/S13a's job, not this session's.
- The new sub-pass 2b.5 must be gated on `PreTCClean` exactly like 2c/3a/3b
  already are (see *Known pitfalls*).
- The main pass's own call-site codegen (inside `TopLevelASTs`, and inside
  `FnASTs`' re-validation) needs **no** second monomorphizer run — it only
  needs to compute the same extended mangled name (module, base name, type
  arguments, parameter types — §9.5's own stated invariant) independently
  at each call site to find the already-emitted LLVM symbol, exactly the
  way an ordinary (non-generic) call already works today via
  `TheModule->getFunction(mangledName)`. This is cheap and requires no
  shared state between the two passes.
- One genuinely new wrinkle (see *Known pitfalls*): a monomorphization-only
  diagnostic (an instantiation-site error) has no natural "the main pass
  re-derives it later" fallback the way every other pre-pass error
  currently does, because the main pass never sees the monomorphizer's
  cloned AST. S12a must report such errors directly and authoritatively
  from the pre-pass, not assume they'll surface again downstream.

### U-5 — how is a generic struct instantiation's type spelled? (new question, raised by the planning agent's P.3, not in the original blueprint) RESOLVED

**Decision:** a generic struct instantiation's concrete type arguments are
determined **entirely by the declared type at the binding site** — a
variable declaration's annotation, a parameter's declared type, a struct
field's declared type, or a function's declared return type — **never**
by the constructor/initializer expression. **No parser change is needed
for TYPE-002.**

**Why:** confirmed directly from `Types.cpp:206-222`'s own comment: *"a
plain struct-init like `Box { val: null }` always produces the bare
`NamedType("Box")` — this language has no syntax for `Box<int> { val:
null }` (an explicit type-argument list on a brace literal)."* There is no
expression-position `<...>` grammar anywhere in the parser. Every existing
generic-struct-construction example in the corpus already writes
`mut b: Box<int> = Box { val: null };` — the declared LHS type is already
the sole source of the instantiation today (it has to be, since the
initializer structurally cannot carry it), and monomorphization does not
change this surface syntax at all; it only changes what the compiler *does*
with the type argument once resolved.

**Consequence for S13a** (where this gets applied, per §13.6's Related
records): the monomorphizer's worklist seed for a generic-struct
`GenericType` occurrence must read concrete type arguments off the
TypeChecker-resolved declared type at the binding site — which the
TypeChecker already computes for `VarDeclExprAST`, parameter types, struct
field types, and return types — not attempt to derive them from the
initializer expression, which cannot carry them by construction.

**One nuance worth recording now, not rediscovered in S13a:** SEM-004
already disclosed that `List<T>::default()` with explicit type arguments on
the call itself does not parse — the bare, unqualified
`List::default()`/`Map::default()` (no `<...>` on the call at all) is the
only spelling that exists, and it relies on this exact same principle even
more starkly: the type argument comes 100% from the LHS declared type, with
*zero* information anywhere in the RHS expression. This is not a new
problem TYPE-002 introduces — it is already exactly how the erasure-era
`List::default()` behaves — monomorphization's job in S13a is to make the
TypeChecker's already-resolved declared-type `GenericType` become the
monomorphizer's worklist seed, in place of being silently erased the way it
is today.

## Related records

[TYPE-001](TYPE-001.md), [SEM-001](SEM-001.md), [OPT-001](OPT-001.md),
[ARCH-001](ARCH-001.md).

---

## Session plan — S11b through S14b

Written this session (S11a, stage 5), owned collectively by all remaining
sub-sessions. Each ends with a green `./scripts/test_runner.sh` and a scoped
commit before the next starts. Depth-first reading order for a session
picking this up cold: this file's *Unresolved questions* (above, especially
U-4′'s consequences and *Known pitfalls*), then the stage list below for
that specific session, then re-derive exact line numbers live (this file's
own *Removed assumptions* already warns the blueprint's line numbers have
drifted once — expect the same here as sessions land).

**S11b — TYPE-002b** · after S11a · *Stand up the monomorphizer skeleton
that collects and reports every instantiation without acting on any of
it.*
1. New `src/Monomorphize.cpp` + `include/Monomorphize.h`, added to
   `CMakeLists.txt`'s explicit source list. `InstantiationKey` (declaration
   identity + ordered concrete type-argument tuple), `InstantiationCache`,
   `Worklist`, per §9.5 — hash/equality via `isEquivalent()`, never
   `getName()` (INV-TYPE-1).
2. Persist the substitution map: new field on `FuncCallExprAST`/
   `MethodCallExprAST`, populated where `collectGenericSubstitution()` is
   called today (`TypeCheckerCall.cpp:449-455` and the method-call
   equivalent near `:621`) instead of being discarded.
3. Fix the mangling collision: extend `ResolverFunctionSymbol::mangledName()`
   (`include/ResolverSymbolBase.h:327-336`); give `TurfGenericParamType` an
   explicit `getMangledName()` override; unit-test it before anything else
   depends on it.
4. Depth cap (default 64) + `GenericInstantiationDepthError` as new error
   code **E079** (confirmed next-free this session — `docs/errors/codes/`
   currently ends at E078), with `docs/errors/codes/E079.txt` and
   `docs/slm/codes/E079.txt`.
5. Wire the collector in **report-only mode** at sub-pass 2b.5 (U-4′'s
   answer) — gated on `PreTCClean` (see *Known pitfalls*). Zero behavior
   change; this is the live instrumentation U-2's static census stood in
   for this session.
6. Create `tests/38_monomorphization/` with a README stating INV-TEST-1.
   Next session may assume: instantiation key/cache/worklist exists; every
   generic call site carries its substitution; mangling is fixed and
   collision-free; E079 + the depth cap exist.

**S12a — TYPE-002c** · after S11b · *First real instantiation: generic free
functions, clone → substitute → re-resolve → re-typecheck → register, with
instantiation-site diagnostics built in from the start.*
1. Generalize `substituteTypeParams()` (`ResolverCore.cpp:146-197`) into a
   concrete-map variant — same recursive walk, preserve the pointer-before-
   reference ordering trap, purely functional (new `TurfTypePtr`s, never
   mutate in place).
2. Clone-and-specialize: `CloneExprAST(Decl)`, rewrite every
   `TurfGenericParamType`, **clear `TypeParams` on the clone** (otherwise
   re-running the Resolver over it re-genericizes it — `visitFuncDef`/
   `visitStructDecl` mutate in place based on `getTypeParams()` being
   non-empty), re-run Resolver+TypeChecker over the clone in the right
   scope. **Hardest single stage in the whole group** — `clone()`'s own doc
   comments say resolved fields are deliberately not copied and cloned ASTs
   "must be re-resolved"; this likely needs a narrow "resolve this one
   declaration in module M's scope" entry point rather than a full
   `resolveAll()` re-run.
3. Instantiation-site diagnostics built here, not deferred: primary
   diagnostic anchored at the instantiation site with `withRelated()` notes
   at the declaration and the offending template line. Account for the
   *Known pitfalls* wrinkle above — this diagnostic has no main-pass
   fallback and must be authoritative from the pre-pass directly.
4. Register the specialization under the fixed mangled name, rewrite the
   call site, keep erasure alive everywhere else — non-generic code must be
   bit-identical to today.
5. Tests: `sizeof(T)` inside a generic free function asserting a *real*
   value (not just absence of a compile error — `sizeof(T)` already
   "compiles" today but is always wrong, always 8); `fail_generic_requires_operator.tr`
   asserting the error lands at the instantiation site; full regression on
   `tests/07_functions_modules/` generic-function tests.
   Next session may assume: generic free functions are monomorphized
   end-to-end; instantiation-site diagnostics with a template backtrace
   exist and are tested; erasure is still live everywhere else.

**S12b — TYPE-002d** · after S12a · *Extend instantiation to generic
methods, including methods on generic structs, with struct layout still
erased.*
1. Method call sites: `TypeChecker::visitMethodCall`
   (`TypeCheckerCall.cpp:621`) and generic-struct receiver handling
   (`~1280-1360`).
2. Partial genericity (`fn<V> f(m: Map<string,V>)`) —
   `tests/07_functions_modules/generic_partial_type_arg_match.tr`.
3. Codegen call-site plumbing: `CodegenStruct.cpp` (`~186/293/381-388`),
   `CodegenFunc.cpp:~1007`, `CodegenVar.cpp:~203`, `CodegenExpr.cpp:~207`,
   `CFG.cpp:~280` (self-call identity comparison via `mangledName()`).
4. Struct layout stays erased/shared this session — deliberate, not an
   oversight; S13a is where layouts split.
5. Tests: generic methods on generic structs; a method generic in its own
   right on a non-generic struct; regression on `tests/23_stdlib_primitives/`
   and `tests/32_set_and_sort_stdlib/`.
   Next session may assume: every generic function-like entity is
   monomorphized and dispatched by its specialized symbol; only struct
   *layout* is still erased.

**S13a — TYPE-002e** · after S12b · *One real `TurfStructType` and one real
LLVM layout per struct instantiation.*
1. Per-instantiation struct registration in `GlobalSymbolTable` under
   extended mangled names — re-derive the opaque-stub mechanism
   (`CodegenCore.cpp:~435-460`) per instantiation instead of sharing under
   the bare base name; preserve `INV-CG-1`'s ordering (guaranteed by U-4′'s
   sub-pass-2b.5 placement above — struct codegen, sub-pass 2c, still runs
   after everything that needs to exist first).
2. Per-instantiation acyclicity: reuse TYPE-001's
   `checkStructLayoutAcyclicity()`/`byValueEdgeTarget()`
   (`ResolverCore.cpp:767`) — do **not** write a second checker.
3. Apply U-5's decision: narrow `Types.cpp:206-229`'s bare-name↔`GenericType`
   equivalence to instantiation-determined-by-declared-type only, and
   rewrite the comment there — it currently justifies the leniency with
   "type-erasure model," which stops being true the moment this session's
   code lands.
4. Tests: `List<int>`/`<double>`/`<string>`/`<Point>`/`<Point*>` in one
   program with independently-asserted behavior; nested generics
   (`List<List<int>>`); regression on
   `tests/06_structs/25_generic_struct_construction.tr` and
   `tests/10_default_init/17_generic_list_default_then_use.tr` (the
   SEM-004 `List::default()` path U-5 named as the most likely thing to
   break here).
   Next session may assume: `GlobalSymbolTable` holds one distinct
   `TurfStructType` per instantiation, each with its own layout, each
   acyclicity-checked post-substitution.

**S13b — TYPE-002f** · after S13a · *Kill the base-name collapse — the
>8-byte ICE dies here.*
1. Remove **both** copies of `FieldAccessExprAST`'s `GenericType`→base-name
   collapse — `CodegenStruct.cpp:715-727` **and** the undocumented second
   copy at `:767-773` (confirmed present this session's planning pass; not
   in the blueprint's own file list).
2. **Completion criterion 2**: `List<Big>` (24-byte struct) compiles, runs,
   prints correctly — the headline test.
3. Transitional diagnostic in place of the ICE if any erasure path survives
   mid-transition.
4. Expect `CastToType`'s struct passthrough and `CodegenFunc`'s
   struct-argument path to need attention too. Canaries:
   `tests/08_e2e_stress/`, `tests/09_boss_fights/`.
5. Tests: `List<Big>`; a >8-byte `T` round-tripping through a generic
   function *and* a struct field; full `tests/06_structs/` sweep.
   Next session may assume: no generic struct shares a layout with another
   instantiation; `T` larger than one machine word is correct everywhere;
   the ICE is gone.

**S14a — TYPE-002g** · after S13b · *Delete erasure, assert INV-GEN-1, make
`sizeof(T)` tell the truth.*
1. Surgical deletion, precisely scoped: `CodegenCore.cpp:428-430`
   (`getLLVMType` erasure), `:1428-1437` (`CastToType` erase branch),
   `:1459-1468` **only** of the unerase block. **Do not** touch
   `:1469-1471` (isPointerLike passthrough) or `:1473+` — that is the
   implicit `self`-dereference for struct-returning methods
   (`fn Node.get(): Node { return self; }`), an unrelated, load-bearing
   feature entangled in the same function with no other mechanism backing
   it. Also `CodegenFunc.cpp:1108-1120` (per-call-site unerase casts); and
   re-check `CodegenCore.cpp:727/741/766, 977-1030, 1122-1165, 1593` for
   now-dead `TurfGenericParamType` handling.
2. **Completion criterion 1**: assert `INV-GEN-1` at the monomorphizer's
   final step *and* independently in `getLLVMType()` — debug builds, never
   removed. Move `INV-GEN-1` and `INV-TYPE-2` to `holds` in
   `INVARIANTS.md`, with a live demonstration, not a claim.
3. `typesCompatible()` Rule 1 stays in code, unchanged — only its doc
   comment changes, to state it is candidate-matching-only and unreachable
   post-monomorphization (§9.6). Same treatment for the array-element and
   `T*` leniencies.
4. **Completion criterion 4**: `sizeof(T)` test asserts a *real* value
   (24 for `Big`, 8 for `int`) in the same program, not just a
   successful compile.
5. Full `lib/std/**`/`showcase/**` direct sweep; all six prelude modules
   direct-compiled.
   Next session may assume: `INV-GEN-1` holds and is asserted; no erasure
   code remains anywhere; `sizeof(T)` is correct per instantiation.

**S14b — TYPE-002h** · after S14a · *Close the group.*
1. **Completion criterion 3**: move
   `tests/99_known_bugs/set_string_dedup_broken.tr` into
   `tests/38_monomorphization/` (renamed off `_broken`) — must pass. Add a
   `Map<string,int>` content-equality test built at **runtime**, never a
   literal (INV-TEST-1 → `holds`).
2. The **one** permitted stdlib change this whole group:
   `lib/std/collections/list.tr:27-28`'s `self.cap * 8` →
   `self.cap * sizeof(T)`. Explicitly do **not** bundle any other stdlib
   cleanup (`List<T*>` retirement, generic `list_sort`, `http.tr`
   simplification, `Map`/`Set` hashing — all later, S24b/STD-003).
3. **Completion criterion 6**: measure compile-time/binary-size deltas
   against this session's (S11a's) baseline table above — recorded, never
   asserted in a test.
4. File handbook debt: `docs/development/handbook-sync/PENDING.md` entries
   for every file in this record's *Handbook impact* table.
5. Finish this file's *Removed assumptions*/*Known pitfalls* sections with
   everything discovered across all 8 sessions. Flip `INDEX.md` to
   `landed`, `Sessions: 8 of 8`.
6. `scripts/session_postflight.sh TYPE-002` and `scripts/check_doc_links.sh`
   clean; each of the six completion criteria individually demonstrated by
   a command whose output was actually looked at.
   Next session may assume (unblocks S15 LANG-001, S18 RT-001, S24 STD-002,
   S24b STD-003, S25 OPT-002): `INV-GEN-1` holds; every instantiation has
   its own type and layout; `sizeof(T)` works; operator dispatch works
   correctly inside generic bodies.
