# TYPE-002 — Monomorphization (the keystone)

| | |
|---|---|
| **Status** | in-progress |
| **Dates** | started 2026-09-21 · landed — |
| **Commits** | (added at the end of each sub-session) |
| **Sessions** | 1 of 8 (S11a done; S11b, S12a, S12b, S13a, S13b, S14a, S14b remain) |
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

### New behaviour (target — not yet implemented)

Same program, same output. But `Box<int>` compiles to its own,
fully-specialized `TurfStructType` with a real `i64` field (not a pointer),
registered under an extended mangled name; a second `Box<Point>`
instantiation elsewhere in the same program gets its own, independent
layout and compiled methods. `TurfGenericParamType` never reaches Codegen
(INV-GEN-1).

### Is this breaking?

**No, at the language level** — every program that compiles today is
intended to compile and behave identically once TYPE-002 lands in full (see
completion criteria in the `ARCHITECTURE_OVERHAUL.md` §8 entry). At the
implementation level, yes, substantially — see *Removed assumptions* below,
grown each sub-session.

## Compiler architecture

No pass exists yet. Planned (§9.5 of the blueprint, confirmed against
source this session — see *Unresolved questions*, U-4′):

```
Lexer → Parser → Resolver(2) → TypeChecker → Monomorphizer → Codegen
```

concretely inserted as a **new pre-pass sub-pass 2b.5**, between the
pre-pass's own `TypeChecker` run (sub-pass 2b, `main.cpp:882-916`) and its
struct codegen (sub-pass 2c, `main.cpp:918-955`). See U-4′ below for the
full justification. Nothing in `src/` changed this session.

## Runtime changes

None (this session). Eventually none in this group either — TYPE-002 is
what makes RT-001's type descriptors possible, not a runtime change itself.

## Standard-library changes

None (this session). Planned, later sessions only (§8's TYPE-002 entry):
`list.tr`'s `cap * 8` → `cap * sizeof(T)` (S14b only — explicitly not
bundled with anything else).

## New invariants

None added this session. `INVARIANTS.md` already carries `INV-GEN-1` and
`INV-TYPE-2` as `planned — TYPE-002`, and `INV-TEST-1` as
`planned — TYPE-002's tests`; `INV-CG-1` (`planned`) is directly relevant to
U-4′ below. None move to `holds` until S14a/S14b actually build the
enforcing mechanism.

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

## Tests

None added this session (no compiler behavior to test yet). `S11b` is where
`tests/38_monomorphization/` is first created.

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
