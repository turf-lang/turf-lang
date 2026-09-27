# TYPE-002 — Monomorphization (the keystone)

| | |
|---|---|
| **Status** | landed (reopened 2026-09-22 for S15, six real defects against the "landed" claim; re-closed 2026-09-22 once all were fixed/scoped and postflight passed) |
| **Dates** | started 2026-09-21 · landed 2026-09-21 · reopened 2026-09-22 (S15) · re-landed 2026-09-22 |
| **Commits** | S15: `d8e167a` (D1), `d5cf0f8` (D2), `730d1ae` (D3), `c2f46ea` (D4), `5a9b1a7` (D5), `7ef9869` (D6), `aab2421` (D7 test), `da85f2a` (D8) |
| **Sessions** | 8 of 8 (S11a, S11b, S12a, S12b, S13a, S13b, S14a, S14b — all done); **S15 reopened the group** (independent review found 6 defects, 4 regressions against pre-TYPE-002, 1 silent wrong-code bug — see that session's own entry below) |
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

**What's ALSO real as of S13a**: every generic STRUCT instantiation
(`List<int>`, `List<string>`, `List<double>`, `List<Point>`, `List<Point*>`,
even nested `List<List<int>>`) now gets its own real, independently-
registered `TurfStructType` (`GlobalSymbolTable`, keyed by its mangled
instantiation name — e.g. `List_gen1_int`) and, once that clone reaches
Codegen, its own real, distinct `llvm::StructType` — verified live via a
temporary debug print during this session (see *Compiler architecture*):
```
[S13a-debug] registering List_gen1_int with 3 fields: data:int* len:int cap:int
[S13a-debug] registering List_gen1_string with 3 fields: data:string* len:int cap:int
[S13a-debug] registering List_gen1_double with 3 fields: data:double* len:int cap:int
[S13a-debug] registering List_gen1_Point with 3 fields: data:Point* len:int cap:int
[S13a-debug] registering List_gen1_Point_ptr with 3 fields: data:Point** len:int cap:int
[S13a-debug] registering List_gen1_List_int with 3 fields: data:List<int>* len:int cap:int
```
each correctly substituted, distinct from every other instantiation and
from the shared template. **This session is deliberately additive only**:
nothing yet REDIRECTS any consumer (`self`'s own LLVM pointee type via
`getLLVMType()`'s `GenericType` branch, or `FieldAccessExprAST`'s two
`GenericType`→base-name collapse points) to use these new per-instantiation
types — they exist, are independently verifiable, and are acyclicity-
checked, but every actual field access and `self` pointer still resolves
through the shared, erasure-era base-name layout, exactly as before this
session. That's S13b's job (see its own session-plan entry, below) — this
is why S13a changes *zero* observable program behaviour (confirmed by the
full suite staying byte-for-byte unchanged, 682/682, at both optimization
levels) despite adding real, independent type identity underneath.

**What's ALSO real as of S13b — completion criterion 2**:
```turf
struct Big { a: int, b: int, c: int }   // 24 bytes
mut l: List<Big> = List::default();
l.push(Big::new(1, 2, 3));
```
compiles, runs, and produces correct results — verified live via
`--emit-llvm`: `List<Big>`'s monomorphized `push`/`pop` clones take/return a
genuine `%Big` aggregate (`define internal void
@turf_List.push_gen1_Big...(ptr %self, %Big %item)`, `define internal %Big
@turf_List.pop_gen1_Big...(ptr %self)`), not an erased pointer. Before this
session, the identical program ICEd with "Invalid bitcast: bitcast %Big
%x4 to ptr" (§9.2's own measured consequence #1). This is S13b's whole
point: it makes Codegen actually CONSUME the per-instantiation
`TurfStructType`/layout S13a built but deliberately left unused — every
`GenericType`→base-name collapse S13a explicitly left alone is now gone
(`FieldAccessExprAST`'s two copies, `getLLVMType()`'s own GenericType
branch, `CastToType()`'s struct-passthrough and implicit-deref branches),
plus one new mechanism S13a's own scope didn't anticipate needing
(`FuncCallExprAST::ConstructedType` — see *Compiler architecture*). Also
verified: a >8-byte struct round-trips correctly through a generic FREE
function (`fn<T> identity(x: T): T` called with a `Big` argument — this
half was already real since S12a, confirmed rather than assumed) and
through another struct's own field (`struct Holder { items: List<Big> }`).

**Was NOT covered as of S13b, closed by S14a** (see that session's own
entry below): `StructName::new(...)` — an EXPLICIT, hand-written
constructor — on a >8-byte generic instantiation. `Box.new`'s own body/
signature (`fn<T> Box.new(v: T): void { self.val = v; }`) was completely
unmonomorphized through S13b (S12a's own exclusion, standing until S14a
extended monomorphization to cover constructors too, now that S13a's
per-instantiation struct layout makes it correct).

**What's real as of S14a — completion criteria 1 and (the struct-field
half of) 4**: `INV-GEN-1` now `holds` (`INVARIANTS.md`) — erasure is
deleted outright, not left dead, and `Box<Big>::new(...)` (an explicit
constructor call, not just `::default()`) now works correctly:
```turf
struct Box<T> { val: T }
fn<T> Box.new(v: T): void { self.val = v; }
...
mut b: Box<Big> = Box::new(Big::new(1, 2, 3));   // real %Big by-value ctor arg
```
and `sizeof(T)` now tells the truth inside a generic STRUCT method too
(not just a free function, S12a's own scope), including for a >8-byte `T`:
```turf
fn<T> Box.sizeOfT(): int { return sizeof(T); }
...
test::assert(Box<Big>::default().sizeOfT() == 24, "...");  // was always 8 under erasure
```
Getting to a SAFE, assertable `INV-GEN-1` required closing two more real,
previously-disclosed-or-latent gaps first (constructors, and a method
generic in its own right on a non-generic struct) plus two genuinely new
discoveries this session made along the way (a non-concrete "substitution"
that used to be silently recorded and harmlessly ignored under erasure, and
generic TEMPLATES reaching Codegen through a second path main.cpp's own
guard never covered) — see this session's own *Compiler architecture* and
*Removed assumptions* entries below for the full story of each.

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

**S13a gives each struct instantiation its own real `TurfStructType` and
LLVM layout** — deliberately additive (see *New behaviour* above for what
this does and does NOT yet change). Full suite green at 682/682 (681 + 1
new test) at both the default and `-O2` optimization levels;
`lib/std`/`showcase`/prelude sweep clean; both plan-named regression tests
(`tests/06_structs/25_generic_struct_construction.tr`,
`tests/10_default_init/17_generic_list_default_then_use.tr`) still pass.

**What S13a added, concretely:**
- `StructDeclExprAST::setName()`/`::clearTypeParams()` (`ASTDecl.h`) — the
  struct counterpart of `FuncDefExprAST`'s S12a-era setters, same contract:
  rename to the mangled instantiation name and mark "no longer generic"
  before the clone is re-resolved. Unlike `FuncDefExprAST`, no codegen
  template-skip guard needed adding for structs — `main.cpp`'s sub-pass 2c
  already codegens every `StructDeclExprAST` it finds unconditionally (no
  `TypeParams`-empty check exists there at all), so a clone with
  `TypeParams` already cleared is picked up automatically, and the
  TEMPLATE's own struct decl node is deliberately left in `PrePassASTs` and
  keeps codegenning the shared, erasure-era layout every EXISTING
  (non-monomorphized) reference still needs.
- `Resolver::checkStructLayoutAcyclicityForClone()` (`Resolver.h`/
  `ResolverCore.cpp`) — the per-instantiation acyclicity check the plan
  called for, built by REFACTORING (not duplicating) the Pass-1 template-
  level `checkStructLayoutAcyclicity()`: its DFS/`byValueEdgeTarget()` core
  is now a shared free function, `detectStructLayoutCycles(ByName,
  StartNames)`, parameterized over which names to start a fresh DFS from.
  The Pass-1 caller starts from every hoisted struct; this new one starts
  from just the one freshly-substituted clone, registered into a COPY of
  `ByName` under its own already-mangled name. See *Known pitfalls* for a
  real limitation found while implementing this, not the plan's own
  framing: `byValueEdgeTarget()` always resolves a `GenericType`-typed field
  to its BASE name, never a specific instantiation, which means this
  per-clone check provably cannot catch any cycle the Pass-1 template-level
  check doesn't already catch, for any input reachable through this
  language's actual (finite) type syntax — see that section for the full
  argument and the one cross-struct cycle shape it still misses.
- `Monomorphize::drainStructRequests()` — the Struct-kind driver, following
  the same clone/substitute/register/re-resolve shape as the other two
  drain functions, with two real differences from both, found live while
  testing this session (see *Removed assumptions*): (1) its own registry
  (`buildStructDeclRegistry()`) is keyed by BARE STRUCT NAME ONLY, not
  `(SourceModule, DeclKey)` the way `buildDeclRegistry()` (functions) is —
  a Struct-kind request's own `SourceModule` field turned out to be
  unreliable (read off the receiver's PARSED `GenericType::getModule()`,
  which reflects use-site spelling, not the struct's real declaring
  module); (2) for the SAME reason, `resolveMonomorphizedClone()`'s module
  argument is the registry's OWN recorded true declaring module
  (`StructRegistryEntry::Module`, captured while walking `PrePassASTs`),
  never `Entry.Request.SourceModule`. Registers the new `TurfStructType`
  directly into `GlobalSymbolTable` (bullet 1 of S13a's own plan) and runs
  `checkStructLayoutAcyclicityForClone()` on the clone BEFORE registering it
  (bullet 2) — both wrapped in the same style of nested `setjmp`/`longjmp`
  harness `finishInstantiation()` already established for the resolve step,
  for the identical defensive reason (an unexpected failure here means a
  compiler bug, not a user error, since every substituted field type
  already passed this exact validation once at its original call site).
  Deliberately does NOT rewrite anything at the requesting call sites — see
  this function's own doc comment (`Monomorphize.h`) for why nothing needs
  to be repointed at the new type yet this session.
- `main.cpp`'s sub-pass 2b.5 loop now calls `drainStructRequests()` as a
  THIRD participant in the same alternating fixed-point loop, for the
  identical reason methods/free-functions already do: a struct request can
  be discovered mid-sweep by either of the other two drain functions (a
  method clone's own `self` type, or a nested generic call's
  `VarDeclExprAST`).
- `Types.cpp:206-222`'s bare-name↔`GenericType` compatibility comment
  rewritten (bullet 3 of S13a's own plan) — the LOGIC is unchanged (still
  needed: U-5 already established there is no `Box<int> { ... }` literal
  syntax, so this leniency is a syntactic necessity, not an erasure-model
  convenience), but its OWN justification no longer appeals to "the
  type-erasure model, no information lost" (no longer true the moment this
  session's per-instantiation types exist) — rewritten to cite U-5 directly:
  a generic struct instantiation's type arguments come ENTIRELY from the
  declared type at the binding site, never the initializer, so this
  TypeChecker-only candidate-matching rule never has to choose BETWEEN
  layouts and stays correct regardless of how many distinct layouts exist
  by the time Codegen runs.

**S13b kills the base-name collapse — the >8-byte ICE dies here**, making
Codegen actually consume the per-instantiation registration S13a built.
Full suite green at 683/683 (682 + 1 new test) at both the default and
`-O2` optimization levels; `lib/std`/`showcase`/prelude sweep clean (same
pre-existing E034/warning items S13a already confirmed on the baseline,
nothing new); `tests/08_e2e_stress/`/`tests/09_boss_fights/` canaries and a
full `tests/06_structs/` sweep all pass (covered by the same full-suite
run — `test_runner.sh` walks all of `tests/`).

**What S13b added, concretely:**
- `TypeCheckerCall.cpp`'s `visitFieldAccess()` gained its OWN Struct-kind
  `MonoCollector_->record()` call, mirroring `visitMethodCall()`'s existing
  one — found live, not anticipated by S13a's plan: a GenericType-typed
  value that's field-accessed directly, with NO method ever called on it
  and NO separate matching `VarDeclExprAST` elsewhere (e.g. `struct Holder
  { items: List<int> } ... h.items.data`), was never recorded as an
  instantiation request at all under S13a alone, so `drainStructRequests()`
  never drained it and this session's now-per-instantiation field access
  found nothing registered under its mangled name.
- `getLLVMType()`'s `GenericType` branch (`CodegenCore.cpp`) rewritten:
  looks up the per-instantiation LLVM type by
  `Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs())`
  instead of the shared base name, with a forward-declared opaque-stub
  fallback for the identical forward-reference reason the prior, base-name
  version already needed one (a struct field referencing an instantiation
  whose own struct decl hasn't codegenned yet in `main.cpp`'s sub-pass 2c
  iteration order) — same mechanism, re-keyed.
- `FieldAccessExprAST::codegenLValue()`/`::codegen()`'s two
  `GenericType`→base-name collapse points (`CodegenStruct.cpp`) — the
  session's headline deletion target — both now resolve to the
  per-instantiation `TurfStructType` via the same
  `instantiatedDeclName()`-based lookup, so `GEP`/`Load` compute offsets
  against the REAL, correctly-sized layout.
- `CastToType()` (`CodegenCore.cpp`) gained two fixes found live while
  testing, both flagged as likely by S13a's own plan ("expect CastToType's
  struct passthrough... to need attention too"): (1) the struct-to-struct
  passthrough (`ValTy->isStructTy()` check) now also recognizes a
  `GenericType` `DestType`, not just `NamedType`/`TurfStructType` — without
  this, a by-value generic struct result (now genuinely `isStructTy()`,
  not an erased pointer) fell through toward the string-error guard; (2)
  the "implicit self-dereference" branch's `ResolvedDest` resolution
  (`fn Holder.get(): List<Big> { return self.items; }`-shaped code) now
  also resolves a `GenericType` destination to its per-instantiation type,
  for the identical reason `FieldAccessExprAST` needed to.
- `FuncCallExprAST::ConstructedType` (new field, `ASTAccess.h`) — the one
  genuinely NEW mechanism this session needed, not just a collapse
  deletion. Found live: `StructName::default()`/`StructName::new(...)`'s
  OWN static type is ALWAYS the bare `NamedType(StructName)` (U-5: a
  construction call structurally cannot carry a type argument), so
  `FuncCallExprAST::codegen()`'s construction branch (`CodegenFunc.cpp`)
  had no way to know it was building `Box<Big>` specifically rather than
  the shared, erasure-era `Box` — meaning `mut b: Box<Big> =
  Box::default();`'s LHS alloca (correctly sized, via the `getLLVMType()`
  fix above) and its RHS construction's OWN alloca (still the shared,
  wrong-sized one) silently disagreed. Fixed by having
  `TypeChecker::visitVarDecl()` (`TypeCheckerDecl.cpp`) — the one place
  that already knows both the declared GenericType AND that the
  initializer is exactly a construction call — write the concrete type
  onto the call node itself; `CodegenFunc.cpp`'s construction branch reads
  it back to look up the per-instantiation `TurfStructType` instead of the
  bare struct name, falling back to the old bare-name lookup when unset
  (every ordinary, non-generic construction). `GetDefaultValue()`
  (`CodegenCore.cpp`) needed NO change at all -- it already recurses
  through whatever `TurfStructType*` it's handed via `getFields()`, so
  passing it the per-instantiation type was sufficient on its own.

**S14a closes the two remaining monomorphization gaps, then deletes
erasure and asserts `INV-GEN-1`.** By far the largest single-session scope
change in this group: the user explicitly asked, correctly, why S14a's
original "just delete erasure" framing (written in S11a, before either gap
existed as a known fact) made sense at all if two real code paths still
depended on it — the answer is it didn't, and this session's real job
turned out to be "close both gaps, THEN delete," not "delete." Full suite
green at 684/684 (683 + 1 new test) at both the default and `-O2`
optimization levels; `lib/std`/`showcase`/prelude sweep clean (same
pre-existing E034/warning items every prior session already confirmed on
the baseline).

**What S14a added, concretely:**
- `Monomorphize::drainConstructorRequests()` — the constructor counterpart
  of `drainMethodRequests()`, draining the struct-bound `FreeFunction`-kind
  entries `drainFreeFunctionRequests()` has always deliberately skipped
  (S12a's own exclusion) now that S13a's per-instantiation struct layout
  makes it safe. Shares `drainFreeFunctionRequests()`'s registry/Sites-
  rewrite shape (a constructor call is still recorded as `FreeFunction`-kind,
  `TypeCheckerCall.cpp`'s `visitFuncCall` never changed how it records
  this), but the CLONE-BUILDING step reuses `drainMethodRequests()`'s
  self-type-rewrite technique — extracted into a new shared helper,
  `cloneAndSubstituteSelfBound()` (`Monomorphize.cpp`, anonymous
  namespace), used by both drain functions now.
- `isKnownGenericStructName()` (`Monomorphize.cpp`) — a new helper both
  `drainMethodRequests()`/`drainConstructorRequests()` call to decide
  whether a self-bound clone's `self` parameter needs `GenericType`
  wrapping (the struct itself is generic, e.g. `List<T>`) or should stay a
  bare `StructName*` (only the METHOD/CONSTRUCTOR has its own type
  parameters, e.g. `fn<T> Box.echo(x: T): T` on a non-generic `Box`) --
  `cloneAndSubstituteSelfBound()` takes this as an explicit
  `WrapSelfAsGeneric` parameter now, rather than always wrapping.
- `TypeCheckerCall.cpp`'s `visitMethodCall()` gained a whole new
  substitution path: alongside the EXISTING struct-level `Subst` (recovered
  from the receiver's own `GenericType`, empty for a non-generic struct), it
  now ALSO unifies the METHOD's OWN parameter types against the actual call
  arguments (`MethodOwnSubst`, mirroring `visitFuncCall()`'s
  `collectGenericSubstitution()` call for an ordinary free function) --
  closing S12b's own disclosed gap ("a method generic in its own right on a
  non-generic struct... never gets an instantiation request recorded in the
  first place"). This was a genuine TYPE-CHECKING gap, not just a
  monomorphization one: before this fix, `b.echo(42)` (`b: Box`, not
  `Box<...>`) reported the call's own type as the literal, un-substituted
  `TurfGenericParamType("T")` instead of `int`.
- `FuncCallExprAST::ConstructedType`'s hand-off (S13b) is now ALSO written
  by `TypeChecker::visitAssignment()` (`TypeCheckerExpr.cpp`), not just
  `visitVarDecl()` -- found live (see *Removed assumptions*): `h.items =
  List::default();` (an ASSIGNMENT, not a `VarDeclExprAST` initializer) is
  the exact shape S12b's/S13a's own `Holder.items = List::default();` test
  pattern uses, and it crashed once the shared template struct's own
  codegen was skipped (below) with no `ConstructedType` to fall back on.
- `FuncDefExprAST::codegen()` and `StructDeclExprAST::codegen()`
  (`CodegenFunc.cpp`/`CodegenStruct.cpp`) both gained an internal, single-
  source-of-truth guard: `if (!TypeParams.empty()) return <sentinel>;` at
  the very top, BEFORE any of their own body runs. `main.cpp`'s own sub-pass
  3a/3b template-skip guards for functions are widened to match (the old
  guard exempted self-bound declarations; nothing is exempt now that
  constructors AND methods are both monomorphized) -- but the REAL fix is
  the internal guard, found live: `ModuleImportExprAST::codegen()`
  (`CodegenStruct.cpp`) walks an imported module's own body via
  `BlockExprAST::codegen()`, calling `FuncDefExprAST`/`StructDeclExprAST`
  `::codegen()` directly, completely bypassing main.cpp's flat-loop guard.
  Confirmed live: even a trivial `fn main(): int { return 0; }` crashed,
  because the auto-imported PRELUDE itself is walked this second way.
- `getLLVMType()`'s `TurfGenericParamType` erasure branch (`CodegenCore.cpp`)
  is now a debug-build `assert()` (INV-GEN-1) instead of `return ptr` --
  falls through to the function's own "unknown/future types" catch-all
  (`getDoubleTy`) in a release build rather than crash outright.
- `CastToType()` (`CodegenCore.cpp`): the "cast TO generic parameter" branch
  (IntToPtr/bitcast erase) is deleted outright and replaced with the same
  style of `assert()`; the numeric half of "cast FROM generic parameter"
  (PtrToInt/bitcast un-erase to int/bool/double) is deleted with NO
  replacement needed -- the surrounding `if (ValTy->isPointerTy())` block's
  other two branches (`isPointerLike` passthrough, the `self`-by-value
  implicit-dereference block) are untouched, exactly as the original S11a
  plan specified ("do NOT touch" those two).
- `CodegenFunc.cpp`'s per-call-site PRE-unerase (a redundant manual
  PtrToInt/bitcast done before even calling `CastToType()`) is deleted --
  it existed only to feed `CastToType()`'s own erase branch a value it
  could already handle; with that branch gone, feeding it pre-converted
  values would be silently pointless, not just redundant.
- `Monomorphize.cpp`'s `finishInstantiation()` gained its OWN `assert()`
  (INV-GEN-1, completion criterion 1's "at the monomorphizer's final step"
  half) -- checks a freshly-produced clone's own Params/ReturnType
  immediately after substitution, catching a bug at the exact clone that
  produced it rather than later, at whatever Codegen call site first
  happens to touch it. The clone's body is covered independently by the two
  Codegen-side asserts above, which fire at actual use.
- `Types.cpp`'s `typesCompatible()` Rule 1 (§9.6) and its `T*`-pointer
  sibling: logic UNCHANGED, comments rewritten to stop citing "type erasure"
  as the justification (no longer accurate) and instead frame the rule as a
  candidate-matching convenience for a generic TEMPLATE's own,
  still-unsubstituted body -- now provably UNREACHABLE for anything else,
  precisely because `INV-GEN-1` holds.
- `INVARIANTS.md`: `INV-GEN-1` and `INV-TYPE-2` both moved to `holds`, with
  a live demonstration recorded, not just a claim.

**S14b closes the group.** All six of §8's TYPE-002 completion criteria
individually demonstrated by a command whose output was actually looked at
(see *Unresolved questions* → *Completion criteria, demonstrated*, below,
for all six in one place). `INV-TEST-1` moves to `holds`. Full suite green
at 689/689 (686 + 3 new tests) at both the default and `-O2` optimization
levels; `lib/std`/`showcase`/prelude sweep clean.

**What S14b added, concretely:**
- **Completion criterion 3**:
  `tests/99_known_bugs/set_string_dedup_broken.tr` moved into
  `tests/38_monomorphization/10_set_string_dedup.tr`, rewritten to assert
  the now-correct outcome instead of the old bug (confirmed live BEFORE
  moving it: running the file completely unchanged already printed "Set
  \<string\> dedup is FIXED"). A second, new test,
  `11_map_string_key_equality.tr`, covers the plan's own explicit ask for
  a `Map<string,int>` content-equality test built at runtime (verified
  live: `m.put(dup, 99)` where `dup` is a `strings::trim()`-built
  duplicate of an existing key correctly UPDATES that key's value instead
  of inserting a second entry). `INV-TEST-1` moves to `holds`
  (`INVARIANTS.md`).
- `lib/std/collections/list.tr`'s `cap * 8` → `cap * sizeof(T)` (see
  *Standard-library changes*) plus a new regression test,
  `12_list_growth_over_8_bytes.tr`, proving the fix at a scale
  (100 pushes) the existing `06_generic_struct_over_8_bytes.tr` (S13b)
  never exercised.
- Two new tests committed properly rather than left as session-only
  scratch reproductions (see S14a's own *Removed assumptions*, which
  originally described these as verified-but-uncommitted):
  `08_generic_constructor.tr` (`Box<Big>::new(...)`, a real, concrete
  by-value constructor argument) and
  `09_method_generic_on_nongeneric_struct.tr` (`Box.echo`-style dispatch
  on a non-generic struct).
- `lib/std/collections/set.tr`'s `Set.indexOf()` and
  `lib/std/collections/map.tr`'s `Map.indexOf()` doc comments rewritten
  from "KNOWN LIMITATION" to "FIXED" (see *Standard-library changes*).
- `docs/planning/LIMITATIONS.md` items 1 and 2 updated directly (not
  queued via the handbook-sync process, since this file is an
  actively-maintained bug tracker, not part of the paused handbook/
  reference) — item 1 (`Map`/`Set` generic string equality) marked
  `Status: FIXED`, with the old "Confirmed-broken repro" retitled as
  historical; item 2 (generic comparator/sort) marked "underlying
  mechanism unblocked, actual stdlib conversion still deferred" (the
  conversion itself — making `list_sort`/`list_binary_search`/`list_find`
  real generic methods — is explicitly out of scope, `S24b`/`STD-003`).
- **Completion criterion 6**: compile-time/binary-size measurements taken
  against S11a's own baseline table (recorded below, in *Unresolved
  questions* → *Completion criteria, demonstrated*) — no regression in
  either dimension despite the full monomorphization pipeline now being
  active end to end; if anything, both measurements came back slightly
  BETTER than the pre-TYPE-002 baseline, consistent with OPT-001's
  `Internalize`+`GlobalDCE` stripping the now-genuinely-dead generic
  TEMPLATE code (S14a) that erasure used to leave permanently reachable.
- `docs/development/handbook-sync/PENDING.md` gained five new rows: full
  rewrites owed for `docs/architecture/compiler_pipeline.md` §7,
  `docs/architecture/memory_model.md` §2.7,
  `docs/reference/generics_and_operators.md`'s two erasure-era sections,
  and `docs/reference/stdlib/collections.md`'s shared-design section —
  plus one row explicitly recording that `LIMITATIONS.md` was NOT queued
  here, since it was already fixed directly (above).
- `INDEX.md`: `TYPE-002` flipped to `landed`. This file's own header:
  `Sessions: 8 of 8`, `Status: landed`.

**S15 reopens the group.** An independent review of the "landed" state found
six real defects, verified live BEFORE any fix (each repro run against the
S14b baseline, not assumed from the review's own writeup) — four regressions
against the pre-TYPE-002 compiler (`c4696c3`), one silent wrong-code bug, one
already-broken-before-TYPE-002 gap the plan itself asked for. All fixed or
explicitly, recordedly scoped out; full suite green at 702/702 (689 + 13 new
tests: 2 for D1, 2 for D2, 4 for D3, 2 for D4, 1 for D5 (+2 companion
modules), 1 for D6, 1 for D7) at both the default and `-O2` optimization
levels; `lib/std`/`showcase`/prelude sweep clean throughout, re-checked after
every single fix, not just at the end.

**D1 — instantiation sites discovered after an entry is drained were never
rewritten (silent wrong code, order-dependent).** `InstantiationCollector::
record()` (`Monomorphize.h`) has always appended new call sites to an
existing cache entry regardless of `Drained` — every drain function skipped
a `Drained` entry outright, so a call site discovered while re-typechecking
a LATER clone in the same fixed-point sweep (e.g. `c.set(raw)` inside
`Wrap.makeFrom<int>`'s own clone body, on an entry `w.set(1)` had already
drained earlier in the same `main()`) never had its `ResolvedCallTarget`
rewritten. `MethodCallExprAST::codegen()`'s `TheModule->getFunction()` miss
then silently returned `nullptr` — no diagnostic, the call simply vanished
(`makeFrom(42)` printed `0`). The free-function equivalent was louder but
still a compiler-bug message: "LLVM function 'turf_inner_T' not found ...
This is a compiler bug." Root cause confirmed exactly as hypothesized:
`Drained` conflated two questions — "don't clone this again" and "don't
rewrite new sites again" — that need different answers once instantiation
order isn't guaranteed to discover every call site before an entry drains.
Fixed by giving `InstantiationEntry` its own `ClonedSymbol`/`SitesRewritten`
fields (`Monomorphize.h`): every drain function's sweep, on finding an
ALREADY-drained entry of the kind it owns, now rewrites
`Sites[SitesRewritten..)` instead of skipping the entry outright.
`Drained` still means, exclusively, "don't clone again." **Also required**
(the single most valuable fix in this session, per the plan's own framing):
`MethodCallExprAST::codegen()`'s `TheModule->getFunction()` miss now raises
"Internal error: LLVM function '...' not found after successful name
resolution" (`CodegenStruct.cpp`), matching `FuncCallExprAST`'s existing
behavior — a monomorphization miss can no longer delete a call silently.
Tests: `13_late_discovered_method_instantiation.tr`,
`14_late_discovered_free_function_instantiation.tr` — both confirmed to
fail (wrong output / ICE) before the fix, pass after.

**D2 — a generic type used only as a struct field was never instantiated
(ICE or silently wrong layout, regression).** `struct Holder { items:
List<Big>, n: int }` with no `Holder`/`List<Big>` variable anywhere ICE'd
("Cannot allocate unsized type") the moment a `Holder` variable existed, or
silently reported `sizeof(Holder) == 8` (not 32) when it didn't — confirmed
live, both shapes, before any fix. Root cause: `visitVarDecl`/
`visitMethodCall`/`visitFieldAccess` (`TypeCheckerCall.cpp`/
`TypeCheckerDecl.cpp`) record a Struct-kind instantiation request at THEIR
OWN site, but nothing ever walked a STRUCT DECLARATION's own field list.
Fixed by `recordGenericFieldInstantiations()`, called from `visitStructDecl`
(`TypeCheckerDecl.cpp`) for every field, recursing through pointer/array
wrapping AND into a `GenericType`'s own type arguments (so `List<List<Big>>`
requests both instantiations, verified live via `--dump-instantiations`).
Gated on `isFullyConcreteType()`, the same predicate S14a's identical
"inner<T>" bug uses, so a generic struct's own still-open field
(`Holder<T> { items: List<T> }`) is never recorded as a bogus non-concrete
request. **Secondary fix**: `getLLVMType()`'s GenericType opaque-stub
fallback (`CodegenCore.cpp`) stays reachable in general (a legitimate
forward-reference mechanism, confirmed by reading `StructDeclExprAST::
codegen()`'s own Step-1 stub-reuse logic) — but a stub still opaque once the
WHOLE module has finished codegenning can only mean a request was recorded
but never drained. `main.cpp` now scans for exactly that, right before
`verifyModule()`, and raises "monomorphized struct instantiation ... was
never given a real layout" instead of letting the raw LLVM-verifier dump be
the only signal. Tests: `15_generic_struct_field_only.tr` (asserts
`sizeof(Holder) == 32`, both with and without a `Holder` variable in the
program's history — the no-variable case verified via a throwaway
`--dump-instantiations` run before writing the test proper),
`16_nested_generic_struct_field.tr`.

**D3 — four stdlib collections still allocated `cap * 8`, corrupting the
heap for any T over one machine word (memory corruption, regression: a safe
compile-time ICE pre-TYPE-002 traded for an unsafe runtime one).** S14b
fixed `list.tr` and called it "the ONE stdlib change this whole group
permits itself" — `stack.tr`, `queue.tr`, `set.tr`, `map.tr` (both its
`keys` and `values` arrays, independently), and `mem.tr`'s `Pool<T>` all had
the identical line, untouched. Confirmed live, before any fix: pushing/
enqueuing/putting 20 24-byte `Big` values through `Stack<Big>`/
`Queue<Big>`/`Map<int,Big>` aborted with glibc `realloc(): invalid next
size` or `Fatal glibc error: malloc.c:2199 (sysmalloc): assertion failed`.
Fixed identically to `list.tr`'s own S14b fix: `cap * sizeof(T)`
(`map.tr` needs TWO separate sizes, `sizeof(K)`/`sizeof(V)`, not one shared
`byteSize` — a real, independent bug beyond the shared-line description
this defect started from). `mem.tr`'s `Pool<T>` is latent, not live (always
instantiated at a pointer type in every real use), fixed for consistency
anyway. Tests: `17_stack_growth_over_8_bytes.tr`,
`18_queue_growth_over_8_bytes.tr`, `19_map_growth_over_8_bytes.tr`,
`20_set_growth_over_8_bytes.tr` (`Set<T>` needed an explicit `operator ==`
overload on `Big`, since `T` must support `==` — D7's own gap) — all four
push 20 elements (multiple growth reallocations) and read every value back,
not just check for a crash; all four confirmed to abort on the pre-fix
stdlib and pass after.

**D4 — a generic struct containing another generic struct at the same `T`
failed to compile (regression: worked pre-TYPE-002, now a compile error).**
`struct Outer<T> { inner: Inner<T> }` then `self.inner.set(x)` inside
`Outer`'s own method reported "No overload of 'Inner.set' matches argument
types (int)" — confirmed live before any fix, both a local toy-struct shape
and the real stdlib shape (`Holder<T> { items: List<T> }`). Root cause,
confirmed exactly as hypothesized: `visitMethodCall`'s/`visitFieldAccess`'s
(`TypeCheckerCall.cpp`) shared "recover this struct's own generic
substitution by scanning its fields for a name to match" `Scan` lambda
handled a field type that is directly a `TurfGenericParamType`/`NamedType`,
or wrapped in a pointer/array — but never recursed into a field whose OWN
type is ANOTHER `GenericType`. `self.inner`'s type stayed the literal,
un-substituted `Inner<T>` instead of `Inner<int>`. Fixed by adding a
`GenericType` case to both duplicated `Scan` lambdas, recursing into the
nested type's own type arguments exactly like the existing pointer/array
cases already do. Tests: `21_nested_generic_struct_same_t.tr` (local toy
struct), `22_nested_generic_struct_over_stdlib_list.tr` (real `List<T>`) —
both confirmed to fail to compile before the fix, pass after.

**D5 — two modules declaring the same generic free function collided
(regression: compiled fine pre-TYPE-002).** `fn<T> tag(x: T): int` declared
identically in two different modules (`amod`/`bmod`), both called as
`tag(5)` from a third file, reported "Duplicate function definition:
'tag_gen1_int'" — confirmed live before any fix.
`InstantiationRequest::sameInstantiationAs()` DOES key on `SourceModule`
(correctly distinct cache entries), but `instantiatedDeclName()` (the
function that names the CLONE) only ever used (base name, type arguments) —
both instantiations produced the identically-spelled clone name regardless.
Fixed by giving `instantiatedDeclName()` an optional `Module` parameter,
passed only from `finishInstantiation()` using the request's own
`SourceModule` (reliable for FreeFunction/Method-kind requests — comes from
the already-resolved symbol, per `buildDeclRegistry()`'s own doc comment).
Every struct-naming call site is left unchanged — struct names are already
globally unique by language rule (E034), confirmed live (a duplicate struct
declaration across two modules is rejected independently of anything this
group touches). `Clone->ResolvedDef->SourceModule` is still cleared after
resolving (unchanged, load-bearing per the existing `%or.merge`-bug
comment) — the name itself is now module-qualified, so it no longer needs
to carry the module forward for mangling. Test:
`23_duplicate_generic_free_function_across_modules.tr` plus two companion
modules (`dup_generic_fn_amod_module.tr`/`dup_generic_fn_bmod_module.tr`) —
confirmed to fail to compile before the fix, pass after.

**D6 — `INV-GEN-1` did not hold for a method's own type params on a generic
struct (not a functional regression — broken before TYPE-002 too, with a
different, safer failure mode — but the invariant's `holds` claim was
false for this shape).** `fn<T, U> Wrap.pick(other: U): U` on `struct
Wrap<T>` — the method redeclaring the struct's own `T` alongside a new `U`
of its own, this language's standard idiom for the shape (`fn<T> List.push`
does exactly this) — aborted a debug build's `finishInstantiation()` own
`INV-GEN-1` assertion; a release build produced invalid LLVM IR. Root cause
confirmed exactly as hypothesized: the recorded Method-kind
`InstantiationRequest` only ever carried the RECEIVER's own type arguments
(`[int]` for `Wrap<int>`), never `U`'s value — `drainMethodRequests()`
pairs `TypeArgs` positionally against the template's FULL `TypeParams`
(`[T, U]`), so with only one entry, `U` was never substituted. **Chose the
preferred outcome (make it work), not the fallback (reject with a
diagnostic)** — this is exactly `List<T>`'s own idiom, rejecting it would
reject a shape the stdlib itself relies on for every single-parameter
generic struct method. Fixed by: (1) `ResolverFunctionSymbol` gains a
`TypeParams` field (a declaration's own `fn<...>` list, in order,
`ResolverSymbolBase.h`/`ResolverCore.cpp`) so `visitMethodCall`
(`TypeCheckerCall.cpp`) can build the request's `TypeArgs` positioned to
match `Best->TypeParams` via `CombinedSubst` (struct-level `Subst` +
`MethodOwnSubst`), not just the receiver's own `GenericType` args; (2)
`self`'s own wrapping needed a SEPARATE fix, found while building (1):
wrapping `self` as `Wrap<int,int>*` (the FULL extended `TypeArgs`) instead
of `Wrap<int>*` (just the struct's own) would misname its per-instantiation
struct lookup and miss the real registration — fixed by
`genericStructOwnTypeParamCount()` (replacing the old boolean
`isKnownGenericStructName()`), which slices `Request.TypeArgs[0,
StructOwnParamCount)` for `self`'s wrapping specifically, while the FULL
list still flows into `cloneAndSubstituteSelfBound()`'s own `Subst`-building
for the clone's body. `INVARIANTS.md`'s INV-GEN-1 row corrected: briefly,
unknowingly false for this specific shape between S14a and this fix. Test:
`24_method_own_type_param_on_generic_struct.tr` — confirmed to abort the
assertion before the fix, pass after (including mixed T/U types and
multiple coexisting instantiations, verified live via a throwaway scratch
program before writing the committed test).

**D7 — the planned `fail_generic_requires_operator.tr` was added; its own
stated requirement (anchor at the instantiation site) is not met, and this
is a recorded scope decision, not a silent gap.** `Set<Big>` (no
`operator==` overload) correctly rejects `s.add(...)` with "Cannot compare
struct values with '=='.", confirming the mechanism fires for a real
T-lacking-an-operator case — the S12a/S13a deferral reasoning ("not
exercisable yet") is now stale, corrected in this file's own *Tests*
section below. **Scope decision**: the plan's own stated requirement — the
diagnostic must point at the instantiation site — is NOT met (it anchors at
`set.tr`'s own `Set.contains()`, the innermost requester, one hop short of
the user's own `s.add(...)` call). Fixing this properly needs a parent-
request pointer on `InstantiationRequest` and a backward walk at both this
diagnostic's primary anchor and E079's own chain (two birds, per the
existing *Known pitfalls* entry on E079's chain noise) — a real feature
addition, not a defect fix, filed as **D25** in
`ARCHITECTURE_OVERHAUL.md` §27.2 rather than bundled into this session.

**D8 — dead erasure call-site casts in two files (hygiene only, no
behavior change).** `ARCHITECTURE_OVERHAUL.md` §8 named `CodegenFunc`'s
per-call-site un-erase cast for deletion (done, S14a) but missed two
siblings: `CodegenStruct.cpp`'s method-call argument adaptation and
`CodegenFunc.cpp`'s constructor-call argument adaptation each still had a
full `argcast_ptr`/`argcast_int`/`argcast_erase_dbl`/`argcast_bit`
erase/unerase branch set. Verified dead, not assumed: instrumented all four
branches (plus the double<->pointer pair) with an `fprintf` probe in both
files and compiled every file under `tests/`, `lib/std/`, and `showcase/` —
zero hits, confirming the enclosing `if (V->getType() != ExpectedTy)` never
fires at all any more. Exactly the "hybrid erasure path kept for
compatibility" this group's own *Must NOT reinterpret* list forbids.
Replaced (not silently deleted) with a raised internal-error diagnostic in
both files, so a future regression here reports clearly instead of
resurrecting an illegal-IR crash. Completion criterion 5's own claim ("no
erasure path survives ... nothing left in Codegen attempts the
erase/unerase cast") corrected in this file's own *Unresolved questions*
section — it was not literally true between S14a and this fix.

**D9 — documentation corrections, folded into the fixes above rather than
a separate pass**: `docs/development/handbook-sync/PENDING.md`'s
`collections.md` row corrected to say the ">8 bytes is no longer a hard
limit" claim was false for five of six collections until D3 landed, not
true since S14b as the row previously implied. `INVARIANTS.md`'s INV-GEN-1
row corrected under D6, above. `docs/planning/LIMITATIONS.md` item 1's
`Status: FIXED` banner re-verified correct and left untouched, per the
plan's own instruction.

## Runtime changes

None. Eventually none in this group either — TYPE-002 is what makes
RT-001's type descriptors possible, not a runtime change itself.

## Standard-library changes

**S14b's own, one permitted functional change** (§8's TYPE-002 entry,
explicitly not bundled with anything else): `lib/std/collections/list.tr`'s
`List.push()` growth allocation, `self.cap * 8` → `self.cap * sizeof(T)`.
`data: T*` is a pointer field (always 8 bytes regardless of `T`), so this
line computes the byte size of the *array* `data` points at — for a T over
8 bytes, `cap * 8` silently under-allocated that array, a real, previously
unreachable-to-fix heap-overflow bug (there was no `sizeof(T)` to compute
the right number with, before monomorphization). Verified live:
`tests/38_monomorphization/12_list_growth_over_8_bytes.tr` pushes 100
`Big` (24-byte) elements through multiple growth reallocations and reads
every one back uncorrupted — this specific shape (more than ~2 elements)
would have silently corrupted memory under the old `cap * 8` even after
S13b/S14a's own fixes made `List<Big>` compile and run at all, since
`06_generic_struct_over_8_bytes.tr` (S13b) only ever pushes 2 elements,
which happens to fit inside the old, wrong allocation by coincidence.

**Comment-only fixes, not functional changes** (see *Removed assumptions*
for why these are in scope despite the "one permitted change" rule — they
correct now-actively-misleading documentation, not behavior):
`lib/std/collections/set.tr`'s `Set.indexOf()` and
`lib/std/collections/map.tr`'s `Map.indexOf()` both carried a long,
detailed "KNOWN LIMITATION" doc comment describing the exact bug §9.2
names as measured consequence #2 ("No per-`T` operator dispatch") — both
rewritten to say "FIXED", with the mechanism explanation updated to match.

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
- **A Struct-kind `InstantiationRequest`'s own `SourceModule` field is
  unreliable as a registry key, unlike a FreeFunction/Method one, and this
  was found live, not anticipated by S13a's plan.** `drainFreeFunctionRequests()`/
  `drainMethodRequests()` both key their template registry by
  `(SourceModule, DeclKey)`, and it works because their `SourceModule` comes
  from the already-RESOLVED symbol's own `SourceModule` field
  (`Best->SourceModule`) — reliable regardless of how the call site spelled
  the reference. A Struct-kind request's `SourceModule`
  (`TypeCheckerDecl.cpp`'s `visitVarDecl`, `TypeCheckerCall.cpp`'s
  `visitMethodCall`) instead comes from the receiver's own PARSED
  `GenericType::getModule()` — which reflects how the type was SPELLED at
  its use site (empty unless written with an explicit `mod::` qualifier),
  not the struct's actual declaring module. Confirmed live:
  `lib/std/collections/list.tr`'s own `list_sort(l: List<int>, ...)` refers
  to `List` completely unqualified (declared in the same file) —
  `BaseGT->getModule()` is `""`, but a registry keyed the
  `buildDeclRegistry()` way would only ever have `"list::List"` registered
  (from recursing into `list.tr`'s own `ModuleImportExprAST` body), never
  `"::List"` — a guaranteed miss for EVERY unqualified use of a generic
  struct anywhere, not just this one, surfacing as "Internal error: no
  generic struct declaration found for 'List'." Fixed by keying
  `buildStructDeclRegistry()` by BARE NAME ONLY, which loses no real
  disambiguating power here specifically because structs, unlike free
  functions, are ALREADY flat/globally-namespaced everywhere else in this
  compiler: `GlobalSymbolTable::RegisterType()`/`LookupType()`
  (`SymbolTable.h`) take no module argument at all — two different modules
  cannot legally declare two different structs sharing one name today
  regardless of what this registry does. The SAME fix applies to
  `resolveMonomorphizedClone()`'s own module argument: it now reads the
  registry's own recorded TRUE declaring module
  (`StructRegistryEntry::Module`), not `Entry.Request.SourceModule`, for the
  identical reason.
- **A direct field access on a GenericType-typed value that never went
  through a `VarDeclExprAST` or a method call was invisible to the
  instantiation collector, and this was found live while testing S13b, not
  anticipated by S13a's plan.** S11b/S13a's two record() call sites
  (`visitVarDecl`, `visitMethodCall`) cover "a variable declared with this
  type" and "a method called on this type," but not "a field of this
  GenericType read directly" — e.g. `struct Holder { items: List<int> }`
  then `h.items.data` with no method call and no separate `List<int>`
  variable anywhere else in the program. Under S13a alone this was harmless
  (nothing consumed Struct-kind registrations yet); the moment S13b made
  `FieldAccessExprAST` require the per-instantiation type to exist, this
  became a real, reachable "Cannot access field on non-struct type"
  regression waiting to happen on any program shaped this way. Fixed by
  adding the identical recording call to `TypeCheckerCall.cpp`'s
  `visitFieldAccess()`.
- **`StructName::new(...)` (an explicit, hand-written constructor) on a
  >8-byte generic instantiation is NOT fixed by S13b, and this is a
  disclosed, not silently missed, gap.** S12a's drain functions
  deliberately exclude every struct-bound declaration reached through the
  free-function call path (`Box::new(...)`'s "self-stripped candidate") —
  `self` carries no type argument of its own, so its body can't be
  correctly substituted without per-instantiation struct layout. S13a
  built that layout; nothing in S12a-S13b actually goes back and drains
  these excluded constructor declarations. Concretely: `fn<T> Box.new(v: T):
  void { self.val = v; }`'s OWN parameter `v: T` is still erased to `ptr`
  (Type Erasure, unmonomorphized) — for `v: Big` (24 bytes), the call site
  would need `CastToType()`'s erase-to-generic-param branch to stuff a
  24-byte struct into one pointer-sized slot, which it structurally cannot
  do (this is the literal "Invalid bitcast" failure mode, just at the
  CALL site instead of the field-access site S13b actually fixed). Not hit
  by anything in `lib/std` today (no stdlib generic struct declares an
  explicit `.new`) or by this session's own tests (deliberately scoped to
  `::default()`-constructed instantiations). **Closed by S14a** — see that
  session's own entry, below.
- **The user directly asked why S14a's OWN original scope ("delete
  erasure") made sense if the constructor gap above, and a SECOND gap S12b
  disclosed, both still depended on it — and was right that it didn't.**
  S14a's session plan was written in S11a, before either gap was a known
  fact. Deleting `CastToType()`/`getLLVMType()`'s erasure branches while
  `Box.new` (constructors) and `fn<T> Box.echo(x: T): T` (a method generic
  in its own right on a non-generic struct) still relied on them to move a
  value into/out of a still-open `TurfGenericParamType` slot would have
  been an IMMEDIATE regression on real, already-passing tests
  (`tests/06_structs/27_generic_struct_literal_field.tr`'s `Box::new(42)`).
  S14a's real scope became "close both gaps, verify erasure is genuinely
  unused by anything, THEN delete it" — confirmed, not assumed, precisely
  because deleting it first and finding out what broke would have been
  strictly worse than closing the gaps up front.
- **A method generic in its own right on a NON-generic struct — S12b's own
  disclosed gap — was closed by S14a, and turned out to be a genuine
  TYPE-CHECKING bug, not just a missing monomorphization step.**
  `TypeCheckerCall.cpp`'s `visitMethodCall()` only ever built a
  substitution map (`Subst`) from the RECEIVER's own `GenericType` — for
  `b.echo(42)` where `b: Box` (not `Box<...>`), `Subst` stayed empty and
  the call fell straight through to `setType(&Node, Best->ReturnType)`
  with NO substitution at all, reporting the call's own type as the
  literal, un-substituted `TurfGenericParamType("T")` instead of `int` --
  independent of monomorphization entirely; this was already wrong under
  plain erasure too, just never surfaced because nothing downstream
  happened to need the precise type. Fixed by unifying the METHOD's OWN
  parameter types against the actual call arguments
  (`collectGenericSubstitution()`, the same mechanism `visitFuncCall()`
  already uses for an ordinary free function), merged with the existing
  struct-level `Subst`. Closing this ALSO required a new decision at
  monomorphization time: whether a self-bound clone's `self` parameter
  needs `GenericType` wrapping (the STRUCT is generic) or must stay bare
  (only the method/constructor has its own type parameters) --
  `isKnownGenericStructName()` (`Monomorphize.cpp`) makes this call by
  checking whether the struct name is actually declared with its own
  `<...>` list anywhere in `PrePassASTs`, since the two cases are
  indistinguishable from `InstantiationRequest.TypeArgs` alone (both are
  just "a Method-kind request with some TypeArgs").
- **A generic function calling ANOTHER generic function through its own
  STILL-OPEN type parameter recorded a bogus, non-concrete "instantiation
  request" — harmless under erasure, a hard crash once `INV-GEN-1` is
  asserted, and found only by actually turning the assertion on and running
  the full suite.** `fn<T> outer(x: T): T { return inner(x); }` — when
  `outer`'s own, still-UNSUBSTITUTED template body is type-checked (as part
  of the whole program's ordinary `resolveAll()`/`checkAll()`, which the
  monomorphization collector is attached to UNCONDITIONALLY since S12a),
  `inner(x)`'s own `collectGenericSubstitution()` call unifies `inner`'s
  `T` against `x`'s type — which, in the TEMPLATE, is `outer`'s OWN, still-
  open `TurfGenericParamType("T")`, not a concrete type. The resulting
  `Subst` is non-empty (`{"T": TurfGenericParamType("T")}`) but NOT
  concrete, and every existing `MonoCollector_->record()` call site
  (`visitFuncCall`, both of `visitMethodCall`'s paths, `visitFieldAccess`)
  only ever gated on `!Subst.empty()`, never on every resulting type
  argument being fully concrete — recording a bogus `"inner<T>"` request
  that `drainFreeFunctionRequests()` dutifully cloned into a REAL,
  permanently dead LLVM function nothing ever called, but whose own
  parameter was STILL a literal `TurfGenericParamType`. Completely
  invisible under erasure (`getLLVMType(TurfGenericParamType)` silently
  returned `ptr`, no complaint) — confirmed live via
  `tests/38_monomorphization/03_nested_generic_call.tr` crashing the new
  `getLLVMType()` assertion the moment erasure's tolerant fallback was
  gone. Fixed by extracting `isOrWrapsGenericParam()`/`isFullyConcreteType()`
  out of `TypeCheckerDecl.cpp`'s anonymous namespace into
  `TypeCheckerInternal.h` (external linkage, shared across the
  TypeChecker*.cpp split) and gating EVERY `MonoCollector_->record()` call
  site in `TypeCheckerCall.cpp` on every resulting `TypeArgs` entry being
  fully concrete, not just the map being non-empty --
  `TypeCheckerDecl.cpp`'s own `visitVarDecl()` already had this gate since
  S11b; the other four call sites (all added S12a-S14a) never did.
- **`ModuleImportExprAST::codegen()`'s own recursive body walk is a SECOND,
  completely independent path to `FuncDefExprAST`/`StructDeclExprAST`
  `::codegen()`, invisible to main.cpp's flat sub-pass 3a/3b template-skip
  loops, and this was found live via the single most surprising crash of
  the whole session: a completely trivial `fn main(): int { return 0; }`
  tripped the brand-new `getLLVMType()` assertion.** `main.cpp`'s own
  guards only ever examine top-level `PrePassASTs` entries directly; a
  module's OWN body (walked by `ModuleImportExprAST::codegen()` via
  `BlockExprAST::codegen()`, itself invoked once per import, including
  every auto-imported PRELUDE module) calls `FuncDefExprAST::codegen()`
  directly on each of its own declarations with no equivalent guard at all
  — meaning ANY still-generic template declared inside ANY imported module
  (the prelude included) reached Codegen regardless of how carefully
  main.cpp's own loops were guarded. Fixed at the actual source rather than
  by duplicating the guard at every call site: `FuncDefExprAST::codegen()`/
  `StructDeclExprAST::codegen()` now check their OWN `TypeParams` at the
  very top, unconditionally, so EVERY caller — main.cpp's two loops, this
  module-body walk, and any future one — is correctly covered by
  construction, not by remembering to duplicate a check.
- **Skipping a generic STRUCT template's own codegen entirely (mirroring
  the function-template fix) broke construction contexts S13b's own
  `ConstructedType` mechanism didn't cover, and this was found live as a
  SEPARATE crash from the ones above, with a different signature entirely
  ("Cannot allocate unsized type").** S13a/S13b deliberately left the
  shared, erasure-era struct TEMPLATE's own codegen running unconditionally
  (main.cpp's sub-pass 2c has no `TypeParams`-empty guard for structs at
  all), reasoning that it coexists harmlessly with per-instantiation clones
  while erasure still exists to make an open `TurfGenericParamType` field
  safe to lower. The moment that reasoning stopped being true (erasure
  deleted, this session) and struct templates were ALSO skipped
  (mirroring functions), `CodegenFunc.cpp`'s `DefaultConstruct`/
  `ConstructorCall` branch's own fallback (`if (!T) T =
  GlobalSymbolTable->LookupType(StructName);`, S13b) started resolving to
  a struct that would NEVER get `setBody()` called on it again —
  permanently opaque, zero-sized — for every construction context
  `ConstructedType` doesn't cover. Found live via
  `tests/38_monomorphization/04_method_on_generic_struct.tr`'s own
  `h.items = List::default();` (an ASSIGNMENT, not a `VarDeclExprAST`
  initializer — the ONE context S13b's `ConstructedType` hand-off
  originally covered). Fixed by extending the SAME hand-off to
  `TypeChecker::visitAssignment()` (`TypeCheckerExpr.cpp`). Disclosed,
  not silently assumed complete: a construction call used directly as a
  function ARGUMENT or in a `return` statement is still NOT covered by
  `ConstructedType` — neither is hit by `lib/std`, `showcase/`, or this
  group's own tests, but a future program writing
  `someFn(List::default())` or `return List::default();` for a GENERIC
  struct instantiation would hit the identical "Cannot allocate unsized
  type" crash. Left for a future session to extend `ConstructedType`'s
  hand-off to cover argument/return-position construction too, following
  the exact same pattern established here for `VarDeclExprAST`/
  `AssignmentExprAST`.
- **S14b confirmed, by direct testing, that BOTH bugs `docs/planning/LIMITATIONS.md`
  items 1–2 describe are the SAME root cause §9.2 (this record's own *Why*
  section) named as measured consequence #2, and that fixing it needed
  NOTHING beyond what S12b–S14a already built.** Not assumed from the
  group's own stated goals — verified live by compiling
  `tests/99_known_bugs/set_string_dedup_broken.tr` completely UNCHANGED
  and observing it print "Set\<string\> dedup is FIXED" (the file's own,
  deliberately loud self-check for exactly this), and independently for
  `Map<string, V>` via a fresh scratch reproduction, BEFORE writing any
  new code this session. This is why S14b's own scope for this item was
  pure verification + moving/rewriting a test + fixing now-actively-wrong
  documentation (`set.tr`/`map.tr`'s own doc comments, `LIMITATIONS.md`)
  — not a code fix, since none was needed. Correcting those comments (not
  behavior) is judged in scope despite the "one permitted stdlib change"
  rule (§8's TYPE-002 entry, about `list.tr`'s `sizeof(T)` fix
  specifically): leaving a doc comment insisting a bug is still present,
  in the SHIPPED stdlib, describing a mechanism that no longer exists,
  would be actively misleading in a way plain staleness elsewhere in this
  record's own *Handbook impact* queue isn't — those files are already
  known-paused and queued; `set.tr`/`map.tr` are live, always-compiled
  source a reader has no reason to expect is behind.

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

S13a adds one more test, all passing (682/682 full suite, default and
`-O2`):
- `05_struct_multi_instantiation.tr` — `List<int>`, `List<double>`,
  `List<string>`, `List<Point>` (a generic struct instantiated with a
  user-defined struct as its type argument, BY VALUE), `List<Point*>` (the
  SAME base struct instantiated with a pointer type argument instead — a
  genuinely different instantiation that must coexist independently of the
  by-value one), and a nested `List<List<int>>`, all coexisting correctly
  in one program with independently-asserted behaviour, plus a final
  coexistence check confirming the FIRST instantiation (`List<int>`) is
  still uncorrupted after every later one has been created and used. Not a
  test of NEW observable behaviour (S13a is deliberately additive — see
  *New behaviour*) — this is a regression/coverage test proving every one
  of these instantiations still compiles and runs correctly with the new
  per-instantiation `TurfStructType`/registry machinery sitting underneath,
  unused but present. `List<List<int>>` is written with a space before the
  closing `>>` (`List<List<int> >`) — this parser lexes `>>` as a single
  right-shift token with no generic-context disambiguation, a pre-existing,
  unrelated limitation this session doesn't touch.

S13b adds one more test, all passing (683/683 full suite, default and
`-O2`):
- `06_generic_struct_over_8_bytes.tr` — **completion criterion 2**:
  `List<Big>` (`struct Big { a: int, b: int, c: int }`, 24 bytes) via
  `push`/`pop`/`length`/direct `data[i]` indexing, all with real, concrete
  struct-by-value parameter/return types (not erased pointers) in the
  monomorphized clones. This is the exact program shape that ICEd with
  "Invalid bitcast: bitcast %Big %x4 to ptr" before this session (§9.2's
  measured consequence #1) — now compiles, runs, and asserts correctly.
  Also covers the other two shapes bullet 5 of S13a's own plan named: a
  >8-byte struct round-tripping through a generic FREE function
  (`identity<Big>`) and through another struct's own field (`struct Holder
  { items: List<Big> }`). Deliberately constructed via `::default()` only
  (never `Big::new(...)`-as-a-GENERIC-struct's-own-constructor) — see
  *Removed assumptions* for the disclosed `StructName::new(...)` gap this
  doesn't cover.

S14a adds three more tests, all passing (686/686 full suite, default and
`-O2`):
- `07_sizeof_t_in_struct_method.tr` — **completion criterion 4's struct-
  field half**: `sizeof(T)` inside a generic STRUCT's own method
  (`fn<T> Box.sizeOfT(): int { return sizeof(T); }`), asserting REAL
  values for `int`/`double`/a >8-byte `Big` (24) in the same program, not
  just a successful compile — `02_sizeof_t_in_generic_body.tr` (S12a)
  already covered the free-function half; this is the half that could only
  ever be asked correctly once S13a/S13b gave each instantiation its own
  real layout.
- `08_generic_constructor.tr` — the `StructName::new(...)` gap disclosed
  through S13b, closed this session: `Box<Big>::new(...)` (an explicit
  constructor call, not `::default()`) with a real, concrete `%Big`
  by-value constructor argument, verified live via `--emit-llvm`
  (`turf_Box.new_gen1_Big...(ptr %self, %Big %v)`), alongside a same-file
  `Box<int>::new(...)` coexistence check.
- `09_method_generic_on_nongeneric_struct.tr` — S12b's own disclosed gap,
  closed this session: `fn<T> Box.echo(x: T): T` where `Box` declares no
  type parameters of its own at all. Covers `int`/`string`/a >8-byte `Big`
  all dispatching correctly to three separate, concrete clones with `self`
  correctly staying a bare `Box*` (never `GenericType`-wrapped, since `Box`
  itself isn't generic) — verified live via `--emit-llvm`.

S14b adds three more tests, all passing (689/689 full suite, default and
`-O2`) — **`INV-TEST-1` (INVARIANTS.md) moves to `holds`**:
- `10_set_string_dedup.tr` — **completion criterion 3**. Moved and
  rewritten from `tests/99_known_bugs/set_string_dedup_broken.tr`, which
  this record's own `docs/planning/LIMITATIONS.md` cross-references
  described as confirmed-broken since before TYPE-002 existed. Same
  runtime-built (`strings::trim()`) duplicate the original file used —
  INV-TEST-1's own worked example, now asserting the CORRECT outcome
  (`s.length() == 2`) instead of the old bug.
- `11_map_string_key_equality.tr` — the plan's own explicit ask alongside
  completion criterion 3: a `Map<string,int>` content-equality test built
  at runtime, never a literal. `m.put(dup, 99)` (`dup` a
  `strings::trim()`-built duplicate of an already-`put()` key) correctly
  UPDATES the existing entry instead of inserting a second one.
- `12_list_growth_over_8_bytes.tr` — proves `list.tr`'s `cap * 8` →
  `cap * sizeof(T)` fix (this session's one permitted stdlib change) at a
  scale (100 pushes, multiple growth reallocations) `06_generic_struct_over_8_bytes.tr`
  (S13b, which only ever pushes 2 elements) never exercised — reads every
  element back and confirms none were corrupted by the fixed allocation
  size.

S15 adds thirteen more tests plus two companion modules, all passing
(702/702 full suite, default and `-O2`) — each one confirmed to **fail
before its own fix and pass after**, verified live via `git stash` on just
that defect's own source changes, not merely asserted:
- `13_late_discovered_method_instantiation.tr` /
  `14_late_discovered_free_function_instantiation.tr` — **D1**. The method
  case failed silently (wrong printed value, no diagnostic) before the fix;
  the free-function case failed loudly ("LLVM function ... not found").
- `15_generic_struct_field_only.tr` / `16_nested_generic_struct_field.tr` —
  **D2**. Both ICE'd ("Cannot allocate unsized type") before the fix.
- `17_stack_growth_over_8_bytes.tr` / `18_queue_growth_over_8_bytes.tr` /
  `19_map_growth_over_8_bytes.tr` / `20_set_growth_over_8_bytes.tr` — **D3**.
  All four aborted (glibc `realloc()`/`malloc.c` errors) before the fix.
- `21_nested_generic_struct_same_t.tr` /
  `22_nested_generic_struct_over_stdlib_list.tr` — **D4**. Both failed to
  compile ("No overload of '...' matches argument types") before the fix.
- `23_duplicate_generic_free_function_across_modules.tr` (plus
  `dup_generic_fn_amod_module.tr`/`dup_generic_fn_bmod_module.tr`) — **D5**.
  Failed to compile ("Duplicate function definition") before the fix.
- `24_method_own_type_param_on_generic_struct.tr` — **D6**. Aborted an
  `INV-GEN-1` assertion (debug build) before the fix.
- `fail_generic_requires_operator.tr` — **D7**. Newly added (not a
  fail-before/pass-after pair — this test's whole point is that the
  mechanism ALREADY correctly rejects the program; see this defect's own
  entry above for the anchoring-quality gap deliberately left open,
  D25).

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
- **`checkStructLayoutAcyclicityForClone()` (S13a) provably cannot catch
  any cycle the Pass-1 template-level check doesn't already catch, for any
  program reachable through this language's actual (finite) type syntax —
  found by careful analysis while implementing it, not something the plan's
  own "reuse, do NOT write a second checker" framing flagged in advance.**
  The reused `byValueEdgeTarget()` (`ResolverCore.cpp`) always resolves a
  `GenericType`-typed field to its BASE name (`GT->getBaseName()`), never a
  specific mangled instantiation — this is true whether it's called from
  the Pass-1 template sweep or this session's new per-clone check. A DIRECT
  self-reference (`struct Node<T> { next: Node<T> }`) is ALREADY caught at
  Pass-1, template level, before monomorphization ever runs (the template's
  own field already names "Node", regardless of what `T` substitutes to) —
  confirmed by the ResolverCore.cpp doc comment this session's work is
  built on top of. The genuinely NEW case per-clone substitution could in
  principle expose — a type parameter substituted to a concrete struct that
  itself, transitively, embeds the SAME instantiation back (e.g.
  `struct A<T> { val: T }` + `struct B { back: A<B> }`, instantiated via
  `mut x: A<B> = ...;`) — is NOT caught either: `A<B>`'s clone's own
  substituted field `val: B` correctly finds the edge to `B` (this edge
  genuinely didn't exist at template level, where `A`'s own `val: T` field
  has no edge at all), but `B`'s OWN field `back: A<B>` resolves via
  `byValueEdgeTarget()`'s `GenericType` branch to the BASE name `"A"` (the
  TEMPLATE), never back to the specific clone `"A_gen1_B"` the DFS actually
  started from — so the walk terminates at the template's own dead-end
  `val: T` field instead of closing the cycle. No FALSE NEGATIVE risk
  exists for any of `lib/std`'s real generic structs today (none embed a
  user-supplied type argument by value at all — every one uses `T*`), so
  this is a real, disclosed gap in COVERAGE, not a live correctness bug —
  closing it in general would need `byValueEdgeTarget()` itself to resolve
  a `GenericType` field to a SPECIFIC already-registered clone's mangled
  name when one exists, instead of always the base name, which is a
  genuinely different (and larger) change than "reuse the existing
  mechanism," left to a future session if this gap is ever hit for real.

## Handbook impact

**Filed, S14b** — `docs/development/handbook-sync/PENDING.md` gained five
rows: `docs/architecture/compiler_pipeline.md` §7 ("Generic type erasure",
`compiler_pipeline.md:884-945`, entirely about the old model, needs a full
rewrite and probably a section rename); `docs/architecture/memory_model.md`
§2.7 (`memory_model.md:572-668`, all three subsections, including deleted-
code quotes at now-wrong line numbers); `docs/reference/generics_and_operators.md`'s
"Type-erasure model" and "`List<T*>` workaround" sections;
`docs/reference/stdlib/collections.md`'s shared-design section — plus a
fifth row explicitly recording that `docs/planning/LIMITATIONS.md` items
1-3 were NOT queued there, since S14b updated them directly this session
(see *Standard-library changes*/*Removed assumptions* for why that file is
treated differently from the paused handbook/`docs/reference/`).

**Corrected, S15/D9** — the `collections.md` row's own claim (">8 bytes is
no longer a hard limit") was retroactively false for five of the six
collections between when it was written (S14b) and D3 landing (S15): only
`list.tr` had actually been fixed at S14b. The row now says so explicitly,
naming D3 and the live corruption repros that proved it. `INVARIANTS.md`'s
INV-GEN-1 row corrected under D6 (its own entry, above) rather than queued
here — like `LIMITATIONS.md`, `INVARIANTS.md` is an actively-maintained
tracker, not part of the paused handbook.

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

### Completion criteria, demonstrated (S14b)

§8's TYPE-002 entry (`ARCHITECTURE_OVERHAUL.md`) names six completion
criteria for this group. All six, each demonstrated live by a command
whose output was actually looked at, not merely claimed:

1. **`INV-GEN-1` holds, with a live demonstration.** `assert()`s in three
   places (`getLLVMType()`, `CastToType()`, `Monomorphize.cpp`'s
   `finishInstantiation()`) — confirmed to actually fire, not just exist,
   by the S14a session's own debugging history (`tests/38_monomorphization/
   03_nested_generic_call.tr` genuinely tripped `getLLVMType()`'s assertion
   before the concreteness-gating fix landed). Full suite green with the
   assertions compiled in (an ordinary, non-`NDEBUG` `cmake --build`).
2. **`List<Big>` (a 24-byte struct) compiles, runs, and prints correctly.**
   `tests/38_monomorphization/06_generic_struct_over_8_bytes.tr` (S13b) and
   `12_list_growth_over_8_bytes.tr` (S14b, 100 pushes/multiple growth
   reallocations) both pass. Verified live via `--emit-llvm` (S13b) that
   the monomorphized `push`/`pop` clones carry a genuine `%Big` aggregate
   parameter/return type.
3. **`tests/99_known_bugs/set_string_dedup_broken.tr` moved into
   `tests/38_monomorphization/`, passing, plus a runtime-built
   `Map<string,int>` content-equality test.** Done this session:
   `10_set_string_dedup.tr`/`11_map_string_key_equality.tr`, both passing.
4. **`sizeof(T)` reports a real value.** Free-function case:
   `02_sizeof_t_in_generic_body.tr` (S12a). Struct-method case, including a
   >8-byte `T`: `07_sizeof_t_in_struct_method.tr` (S14a) — `Box<Big>.sizeOfT()
   == 24`, not the old, always-8 erasure answer.
5. **The `>8-byte` ICE is gone, replaced by a diagnostic if any erasure
   path survives.** No erasure path survives at all (deleted outright,
   S14a) — the original "Invalid bitcast: bitcast %Big %x4 to ptr" ICE
   this whole group exists to fix cannot occur, structurally, since nothing
   left in Codegen attempts the erase/unerase cast that produced it.
   **Correction (S15/D8):** this claim was NOT literally true between S14a
   and S15 — `CodegenStruct.cpp`'s method-call argument adaptation and
   `CodegenFunc.cpp`'s constructor-call argument adaptation each still had
   a full `argcast_ptr`/`argcast_int`/`argcast_erase_dbl`/`argcast_bit`
   erase/unerase branch set, S14a's own deletion pass having covered only
   `CodegenCore.cpp`'s `getLLVMType()`/`CastToType()` and the *per-call-site
   PRE-unerase* in `CodegenFunc.cpp` (a different mechanism, see S14a's own
   entry above), not these two call-argument-adaptation sites. Verifiably
   dead (an `fprintf` probe on all four branches in both files, zero hits
   compiling every file under `tests/`, `lib/std/`, and `showcase/`) but
   still present in source, and therefore exactly the "hybrid erasure path
   kept for compatibility" this group's own *Must NOT reinterpret* list
   forbids — closed by S15/D8, which replaces the whole mismatch block in
   both files with a raised internal-error diagnostic instead of deleting
   it silently, so a future regression here reports loudly rather than
   resurrecting an illegal-IR crash.
6. **Compile-time/binary-size deltas measured against S11a's own baseline,
   recorded, not asserted in a test.** Same three programs, same
   methodology (S14b, this session):

   | Program | S11a baseline (wall / size) | S14b, post-landing (wall / size) |
   |---|---:|---:|
   | Empty `main` | 0.40s / 16 KB | ~0.26s / 15.4 KB |
   | `showcase/text_toolkit.tr` (`List<string>`) | 0.44s / 17 KB | ~0.40s / 16.4 KB |
   | `showcase/kv_store_server.tr` (`Map`/collections) | 0.46s / 17 KB | ~0.40s / 16.9 KB |

   No regression in either dimension across all three programs — if
   anything, both compile time and binary size came back slightly BETTER
   than the pre-TYPE-002 baseline, despite the full clone/substitute/
   re-resolve/re-typecheck/register monomorphization pipeline now running
   on every compile. Consistent with two things confirmed earlier in this
   group rather than assumed now: U-2's own census (S11a) found only
   ~18-20 real instantiations in the whole corpus (no combinatorial
   blowup risk), and OPT-001's `Internalize`+`GlobalDCE` now has
   genuinely-dead generic TEMPLATE code (S14a's widened skip guards) to
   strip that erasure used to leave permanently reachable and compiled.
   Wall-clock figures are single-machine, informal timings (`/usr/bin/time`,
   three runs each, this session's own dev environment) — expect ordinary
   machine-to-machine variance, not a rigorous benchmark; the comparison
   that matters is the RELATIVE one (no growth), not the absolute numbers.

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
