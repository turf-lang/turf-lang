# TurfLang 2.0.0 — Release Roadmap

> **Status:** plan of record, written 2026-09-26 by the Architecture Overhaul
> closure audit, against `refactor/architecture` @ `cde1d30`. **Phase 0
> (Overhaul closure) is now done** — see
> `docs/development/changes/OVH-CLOSE.md` (landed 2026-09-27). **Phase 1's
> `SMP-002` (one generic type walker) is also done** — see
> `docs/development/changes/SMP-002.md` (landed 2026-09-27). **`FDN-003`
> (module-scoped type identity) is done** — see
> `docs/development/changes/FDN-003.md` (landed 2026-09-27; its stage-1
> decision is frozen as `FREEZE.md` §6.11, DEC-13 answered `main`).
> **`FDN-001` (safe heap allocation of user types) is also done** — see
> `docs/development/changes/FDN-001.md` (landed 2026-09-27; DEC-5 answered
> `heap(expr): &T` intrinsic, frozen as `FREEZE.md` §6.12). **`FDN-002`
> (reference and array escape rules) is also done** — see
> `docs/development/changes/FDN-002.md` (landed 2026-09-27; its design was
> stated directly in this roadmap rather than needing a DEC- decision,
> frozen as `FREEZE.md` §6.13). **`SMP-001` (compiler query facade) is
> in progress** — the facade and the Sandbox port landed 2026-09-28; the
> `turf-lsp` port (stage 3, its own repository) remains — see
> `docs/development/changes/SMP-001.md`. **`FDN-004` and `FDN-006` are
> done** (both landed 2026-09-28). **`FDN-005` (GC hardening) is done
> pending one CI confirmation** — see `docs/development/changes/FDN-005.md`
> (landed 2026-09-28, follow-up 2026-09-29: the full-stack mark-set
> comparison and the no-`/proc` fallback, which was freeing live objects
> and is now fixed). The one criterion item not yet observed complete is
> `TURF_GC_STRESS` + ASan at `-O2`, now a CI step. Its CI jobs stay on
> through RC as a gate. Part I's sequence has the
> current state of every Phase 1 item; the rest of Phase 1 and all of
> Phases 2+ remain unimplemented. Items marked
> **DECISION** need an explicit answer from the project owner before their
> session starts; each carries a recommendation so the default is never
> ambiguous.
>
> **Reading order:** Part 0 (rules) → Part I (sequence) → the milestone you
> are implementing in Part IV → Parts V–IX as referenced.
>
> **Companion documents:**
> `docs/development/OVERHAUL_CLOSURE_AUDIT.md` (evidence) ·
> `docs/development/changes/OVH-CLOSE.md` (Phase 0's closure record — the
> temporary `docs/planning/OVERHAUL_REMAINING_WORK.md` handoff it closed out
> is deleted as of that record) ·
> `docs/planning/FUTURE.md` (everything deliberately after 2.0) ·
> `docs/planning/ARCHITECTURE_OVERHAUL.md` (historical specification) ·
> `docs/planning/ARCHITECTURE_SIMPLIFICATION.md` (discussion document; its
> assessment is Part III here) · `docs/development/FREEZE.md` (still binding).

---

## Part 0 — What 2.0.0 is, and the rule that decides what goes in it

**2.0.0 is the first public TurfLang**: the point at which an outside developer
can install it, start a multi-file project with dependencies, rely on its
memory-safety and numeric guarantees, read documentation that matches the
compiler, and file bugs against a frozen language. It is not "every good idea
at once". After 2.0, `docs/planning/FUTURE.md` becomes the backlog.

### 0.1 The inclusion rule

A piece of work belongs **before** 2.0.0 only if at least one of these holds:

| Rule | Meaning | Why it forces pre-2.0 |
|---|---|---|
| **R1 — Breaking** | It changes the meaning of, or rejects, programs that compile today. | After 2.0 a breaking change costs a 3.0. The major-version window is the only cheap time. |
| **R2 — Soundness** | It closes a hole where safe code corrupts memory, crashes the compiler, or produces a silently wrong answer. | A public language cannot ship known unsoundness in its safe subset. |
| **R3 — Shipping** | Without it, users cannot obtain, build, depend on, or distribute a TurfLang program. | That is what "public" means. |
| **R4 — Mandated** | The project owner put it in scope: D7, D13, D14, D15. | Stated requirement. |
| **R5 — Release quality** | A release gate (Part VII–IX) fails without it: diagnostics that point at the wrong place, docs that describe a different language, a tool that does not build. | Credibility of the first impression. |

**Purely additive features wait**, however attractive. "Would this be
useful?" is never sufficient. Every item in Part IV names the rule that puts
it there.

### 0.2 What stays frozen

`docs/development/FREEZE.md` remains binding throughout. Nothing in this
roadmap reinterprets a frozen decision. Where a milestone needs a *new* frozen
decision (visibility defaults, pattern syntax, the `try` keyword, typed
allocation), the milestone's first stage is to write that decision into
`FREEZE.md` with the owner's approval — `FREEZE.md` §6.0's rule: sessions never
freeze on their own initiative.

### 0.3 The three kinds of work

Every milestone is exactly one kind, so runtime and packaging work cannot
disappear under a list of syntax features:

- **Foundation (FDN, SMP)** — required for the architecture or runtime to be
  correct and maintainable. `SMP-` items are Foundation work sourced from the
  Architecture Simplification initiative.
- **Language / ecosystem completeness (LEC)** — required for a coherent public
  language.
- **Release engineering (REL)** — required to actually ship.

### 0.4 How a milestone is executed

Identical to an overhaul feature group: `scripts/session_preflight.sh <ID>`
(it finds `### <ID> — ` headings in this file), `SESSION_PROTOCOL.md`, a
change record `docs/development/changes/<ID>.md`, `INDEX.md` row,
`INVARIANTS.md` for any new invariant, `handbook-sync/PENDING.md` for doc
debt (until REL-006 drains it), `scripts/session_postflight.sh <ID>`. A
milestone sized **L** or **XL** writes its own session sub-plan into its
change record in its first session, as TYPE-002 did.

---

## Part I — Sequence

```
PHASE 0  Overhaul closure            OVH-001 … OVH-015   DONE (OVH-CLOSE.md, 2026-09-27)
   │                                  gate: closure gate demonstrated clean
   ▼
PHASE 1  Foundation                   SMP-002  one generic type walker      DONE (SMP-002.md, 2026-09-27)
   │                                  FDN-003  module-scoped type identity  DONE (FDN-003.md, 2026-09-27)
   │                                  FDN-001  safe heap allocation         DONE (FDN-001.md, 2026-09-27)
   │                                  FDN-002  reference & array escape rules  DONE (FDN-002.md, 2026-09-27)
   │                                  FDN-004  string indexing: bounds + `char` (D17)  DONE (FDN-004.md, 2026-09-28)
   │                                  FDN-006  runtime contract (stack overflow, OOM, ABI stamp)  DONE (FDN-006.md, 2026-09-28)
   │                                  SMP-001  compiler query facade (parallel, any time in 1–2)  stages 1–2 DONE, turf-lsp port pending (SMP-001.md, 2026-09-28)
   │                                  FDN-005  GC hardening  stages 1–4 DONE (FDN-005.md, 2026-09-28/29); runs on to RC as a CI gate
   ▼
PHASE 2  Language & ecosystem         LEC-002  visibility — `pub`/`public` (D13)  stages 1–3 DONE (LEC-002.md, 2026-09-29)
   │                                  LEC-001  projects, dependencies, build (D7)
   │                                  LEC-003  pattern matching (D14)
   │                                  LEC-004  error payloads + `try` (D15)
   │                                  LEC-006  collection indexing coherence (generic operators)
   │                                  LEC-005  diagnostics reliability (incl. D25)
   │                                  LEC-007  standard library 2.0 surface
   ▼
═══════  LANGUAGE FREEZE  (Part VI)   tag v2.0.0-freeze
   ▼
PHASE 3  Stabilization & release      REL-001  version, CLI, runtime ABI stamp
                                      REL-002  tooling: `turf test`, `turf fmt`, LSP, VS Code
                                      REL-003  platforms & distribution (D20)
                                      REL-004  conformance suite, CI, fuzzing, sanitizers
                                      REL-005  performance baseline (starts in Phase 1)
                                      REL-006  documentation (reference rewrite, migration guide)
                                      REL-007  release candidates → 2.0.0
                                      FDN-005  GC hardening completes (RC blocker)
```

### I.1 Dependency edges (hard — a violation produces wrong code or rework)

| Edge | Why |
|---|---|
| OVH closure → everything | Building features on a collector that frees live objects (OVH-001/002) or a type-parameter model that collides with user names (OVH-006) multiplies the damage. |
| SMP-002 → LEC-003, LEC-004, LEC-006 | Pattern matching over generic subjects, generic payload types and generic operators each add another "walk a type looking for `T`" site. Unify the walkers first (STD-003 found three silently incomplete ones). |
| FDN-003 → LEC-002 → LEC-001 | Visibility needs per-module type identity (two private `Parser` structs in two modules must coexist); packages need visibility (an API surface) and per-module identity (two packages both declaring `Node`). |
| FDN-001 → LEC-004 | Typed error payloads are GC objects with descriptors; the same typed-allocation path. |
| LEC-003 → LEC-004 | Payloads are read back by destructuring patterns (D15 is "D14 wearing a different hat", §27). |
| FDN-004 → LEC-003 | `char` patterns over string elements should be written once, against the final `s[i]` type. |
| LEC-002 → LEC-007 | The stdlib's 2.0 API surface *is* its `pub` set. |
| SMP-001 → REL-002 | The LSP is rebuilt on the facade, not on today's globals. |
| everything language-visible → FREEZE → REL-006 | Documentation is written once, against the frozen language. |

### I.2 Soft edges

FDN-005 benefits from REL-004's fuzz corpus; LEC-005 is cheapest after
LEC-003/004 have added their diagnostics; REL-003's packaging work can start
in Phase 1 for the Tier-1 platform.

### I.3 Rough size

| Phase | Items | Estimated sessions |
|---|---|---|
| 0 | 15 | 8–10 |
| 1 | 8 | 14–18 |
| 2 | 7 | 18–24 |
| 3 | 7 (+FDN-005) | 10–14 |

---

## Part II — Reclassification of the Deferred Work Register (§27)

Every row of `ARCHITECTURE_OVERHAUL.md` §27 receives exactly one decision.
**2.0** rows are milestones in Part IV. **FUTURE** rows move to
`docs/planning/FUTURE.md` with their rationale. **Superseded** rows are closed
here with the reason. **Rejected** rows stay in §25/FREEZE and are not
backlog.

| # | Item | Decision | Rule / reason |
|---|---|---|---|
| D1 | Closures / lambdas | **FUTURE** (F-1) | Additive; first-class non-capturing functions already exist. Its prerequisites *are* in 2.0 (FDN-001 typed GC allocation for environments, OVH-010 roots for environments held by C). Detailed design sketch in FUTURE F-1. |
| D2 | Iteration over collections | **FUTURE** (F-2) | Additive. OVH-003's bounds-checked `get(i)` makes index loops safe; `for x in list` adds no new capability. Protocol sketch in FUTURE F-2. |
| D3 | `Slice<T>` | **FUTURE** (F-3) | Additive. Its *soundness* half — `T[]` parameters escaping the caller's frame (D23) — is **2.0** in FDN-002. |
| D4 | `Map`/`Set` hashing | **Superseded** | Done by STD-003 item 6 (`hashing.tr`, O(1) average lookup). |
| D5 | Full nullability analysis (interprocedural, field narrowing, narrowed args/returns) | **FUTURE** (F-5) | Additive precision; the current intraprocedural pass is sound (INV-NREF-1/2). |
| D6 | Manual-heap leak detection | **FUTURE** (F-6) | Additive diagnostics; scope shrank with GC exactly as §27 predicted. |
| D7 | Separate compilation / stable ABI / package manager | **2.0 — scoped** (LEC-001, Part IX) | R3 + R4. 2.0 needs a project model, dependency fetching, a lockfile and collision-free module resolution. It does **not** need separate compilation, a stable Turf ABI, or a registry — those are FUTURE F-7a/b/c. |
| D8 | Concurrency / threads / async | **FUTURE** (F-8) | Additive and XL; GC redesign per §14.11. |
| D9 | `@slm { }` blocks, LoRA fine-tuning | **FUTURE** (F-9) | Additive; SLM-001's corpus is the prerequisite for judging either. |
| D10 | Self-hosting | **FUTURE** (F-10) | Additive, XL; needs D1 and a stable 2.x. (The old `FUTURE.md` called self-hosting "Turflang 2.0.0" — that label is retired; 2.0.0 now means the first public release.) |
| D11 | LSP, `turf test`, package tool | **2.0 — partially** (REL-002, LEC-001) | R3/R5: `turf test`, the existing formatter, and a *building* LSP ship; LSP features beyond diagnostics/hover/goto-definition are FUTURE F-11. |
| D12 | TLS/HTTPS, UDP, non-blocking I/O | **FUTURE** (F-12) | Additive; TLS is large and security-sensitive; non-blocking needs D8. |
| D13 | Per-declaration visibility | **2.0** (LEC-002) | R1 + R4 (default-private is breaking). |
| D14 | Richer pattern matching | **2.0** (LEC-003) | R4. |
| D15 | Error payloads + propagation shorthand | **2.0** (LEC-004) | R4. |
| D16 | Circular imports | **FUTURE** (F-16) | Additive; only cost today is `mem::Pool<T>`'s documented duplication. |
| D17 | `s[i]` yields `int`, not `char` | **2.0 — DECISION** (FDN-004) | R1: breaking, so now or never (until 3.0). Bundled with the soundness fix R2 (string indexing is unchecked, audit F-17). Recommendation: change it. |
| D18 | Two type-inference engines | **FUTURE** (Simplification, F-S3) | Internal; INV-TYPE-2 makes them agree for monomorphized nodes. Not user-visible. Revisit trigger stands. |
| D19 | Double-parse pre-pass | **FUTURE** (Simplification, F-S4) | Internal, load-bearing, risky. Not user-visible. |
| D20 | Cross-compilation vs. target runtime | **2.0 — decide** (REL-003) | R3: `--target` must either work end-to-end or fail with a diagnostic, not a host-linker error. Recommendation: object-only cross-compilation with a clear diagnostic at link time; full cross-linking FUTURE F-20. |
| D21 | Postfix `++`/`--` and operator overloading | **Superseded** | LANG-001 decided and implemented it (postfix routes through overloads; prefix unified onto `PostfixExprAST`). |
| D22 | `int → double` above 2^53 | **FUTURE** (F-22) | Additive opt-in lint, never an error (§10.5). |
| D23 | `T[]` parameters alias the caller's frame, no escape analysis | **2.0** (FDN-002) | R2. |
| D24 | Checked arithmetic blocks auto-vectorization | **FUTURE** (F-24) | Performance only; F-OPT-2 forbids removing checks by flag, and the range-proof direction needs no semantic change. The measurement is preserved in FUTURE. |
| D25 | Instantiation diagnostics anchor at the innermost requester; E079 chain is a cache dump | **2.0** (LEC-005) | R5: the flagship "stdlib errors at your call site" story (§15.6(b)) is one hop short, and every user of a stdlib generic with an unsupported `T` sees it. S-sized. |

**New items the audit added that §27 never had** (all justified by rules in
0.1, all pre-existing rather than Overhaul regressions):

| New | Item | Milestone | Rule |
|---|---|---|---|
| N1 | Safe heap allocation of user types (DoD-8 intent) | FDN-001 | R2 (no safe way to build a dynamic linked structure) |
| N2 | `return &local;` / stored `&local` dangling references | FDN-002 | R2 |
| N3 | Global struct namespace (`struct List` impossible even with no imports) | FDN-003 | R1 + R3 |
| N4 | `string` indexing unchecked | FDN-004 | R2 |
| N5 | Runtime failure modes (stack overflow SIGSEGV, OOM, no version stamp) | FDN-006, REL-001 | R3 + R5 |
| N6 | Module shadowing, `turf_modules/` vs compiler search path | LEC-001 | R3 |
| N7 | `List<T>[i]` works only for `int` | LEC-006 | R5 (incoherent shipped API) |
| N8 | Parse-error cascade, misleading hints, duplicate warnings, `_x` not lexable | LEC-005, LEC-003 | R5 |
| N9 | No `StringBuilder` (the frozen string decision §15.9 relies on it); quadratic `strings::repeat` | LEC-007 | R5 |
| N10 | `turf-lsp` does not build against HEAD | SMP-001, REL-002 | R5 |

---

## Part III — Architecture Simplification assessment

`ARCHITECTURE_SIMPLIFICATION.md` diagnoses one problem — ambient global state
instead of explicit inputs/outputs and stable representations — and proposes
phases 0★–5. Its diagnosis survives the Overhaul intact; if anything the
Overhaul strengthened it (the STD-003 and SLM-001 evidence in its §1, and this
audit's F-06, where the "split brain" between the Resolver-mutated AST and the
`GlobalSymbolTable` copy produces a *user-visible* bug). Nothing in the
Overhaul incidentally solved a phase: monomorphization added a new consumer of
the global tables (the monomorphizer re-runs the Resolver/TypeChecker on
clones through the same globals) rather than removing one.

| Phase | Current relevance | Required for 2.0? | Why | Risk | Decision |
|---|---|---|---|---|---|
| **0★** — one notion of "is/contains a generic parameter" | **Higher** than when written: it is now a user-visible correctness bug (audit F-06: a user `struct T` breaks `List<int>`) | **Yes** | R2 | Low–medium | Correctness half in **OVH-006** (Phase 0); the unification of the seven hand-written walkers is **SMP-002** (Phase 1). |
| **0** — `LexerState` | Unchanged; 18 lexer globals | No | Needed for *concurrent* tokenization and a trivia-preserving formatter; neither is a 2.0 requirement (the 2.0 formatter is the existing indentation formatter, REL-002) | Low | **After 2.0** (FUTURE F-S0). Do not start during stabilization. |
| **1** — `ParserState` | Unchanged | No | Same as 0 | Low–medium | **After 2.0** (F-S1). |
| **2** — drop the `GlobalSymbolTable` singleton | Unchanged | **Partially** | The *user-visible* consequence is that every tool re-implements a save/restore dance and breaks on every refactor (`turf-lsp` is broken today). A stable, tested query **facade** inside the compiler removes that consequence without removing the globals. | Facade: low. Full removal: medium–high | **SMP-001** (facade) before 2.0; full singleton removal **after 2.0** (F-S2). |
| **3** — real `SemanticModel` | Stronger evidence, same size | No | High-risk change to the Resolver/TypeChecker contract; SMP-001's facade gives tools a stable API while this is done later behind it | High | **After 2.0** (F-S3), with D18 folded in. Needs its own freeze decision (mutable vs immutable trees, Simplification §9). |
| **4** — CFG → lowering IR | Unchanged; OPT-002 now runs a second CFG build before Codegen, which is more evidence for it | No | Codegen contract change; D19 interacts | High | **After 2.0** (F-S4), with D19 folded in. |
| **5** — dependency layering | Partial already (`turfcore` library target exists); parser TUs still include `Codegen.h` (HYG-001's unresolved question) | No | Build-system work after 0–4 | Medium | **After 2.0** (F-S5). The `Codegen.h`-in-parser include is recorded there. |
| §8 — comments | Unchanged | No | Folded into whichever phase touches a file | — | Unchanged recommendation. |

**Tracking.** Recommended: after 2.0.0 ships, the post-2.0 phases become a
tracked initiative with IDs (`SMP-0NN` continuing this numbering), their own
`FREEZE.md` entries for the phase-3 fork, change records, and the same session
protocol — exactly the question `ARCHITECTURE_SIMPLIFICATION.md` §9 left open.
Before 2.0 only SMP-001 and SMP-002 are tracked, as Foundation milestones in
this roadmap. **Simplification never blocks 2.0 beyond those two**, and they
are both bounded, incremental and shippable after each session.

---

## Part IV — Milestones

Each milestone uses the same template. "Stages" are the session plan; an L/XL
milestone refines them in its change record before writing code.

### Phase 0 — Overhaul closure — **DONE** (2026-09-27)

`OVH-001` … `OVH-015` were specified in the temporary handoff
`docs/planning/OVERHAUL_REMAINING_WORK.md` (deleted on closure) and gated by
that file's §4. The closure record,
[`docs/development/changes/OVH-CLOSE.md`](../development/changes/OVH-CLOSE.md),
says the Overhaul is closed, lists the gate evidence, and records the
audit's DoD-8 reclassification (into FDN-001/FDN-002 below). Phase 1 is
unblocked.

---

### SMP-002 — One generic type walker

**Kind** Foundation (Simplification 0★, second half) · **Rule** R2 (it is the
source of a recurring class of generics bugs) · **Size** M · **Prereqs** OVH-006

**Objective.** Replace the seven independently hand-written recursive
functions that answer "does this type contain an open generic parameter, and
substitute it" with one shared type visitor, so a new type shape (`FuncType`
was the last one missed, STD-003 Fix 2) is added once.

**Rationale.** `ARCHITECTURE_SIMPLIFICATION.md` §1 lists them:
`Resolver::substituteTypeParams`, `typesCompatible()` (Rules 1/3/4),
`collectGenericSubstitution()`/`collectGenericParamNameOrder()`,
`Monomorphize::substituteConcreteTypes()`, `visitCast`'s legality bypass,
`isOrWrapsGenericParam()`/`isFullyConcreteType()`. Three were found silently
incomplete in one session; OVH-006/007/008 are three more instances. Phase 2
adds pattern matching, payload types and generic operators — each another
walker if this is not done first.

**Subsystems** Types, Resolver, TypeChecker, Monomorphizer. **Runtime/stdlib/tooling** none.

**Stages.** (1) Introduce `include/TypeWalk.h`: a visitor over every
`TurfType` subclass with `containsOpenParam()`, `substitute(map)`,
`unify(pattern, concrete, map&)`; exhaustive over the type hierarchy (a
`switch` on a kind enum or a visitor base whose default case asserts — the
INV-LINT-1 lesson). (2) Migrate one call site at a time, full suite after
each. (3) Delete the old functions.

**Tests.** Existing suite is the guard; add one test per type shape × per
walker purpose in `tests/38_monomorphization/` (pointer, reference, nullable
reference, array, `GenericType`, `FuncType`, `!T`) used as a generic
argument, in a cast, in a callback parameter and as a return type.

**Docs** `compiler_pipeline.md` (monomorphization section). **Breaking** none.
**Risks** A behaviour difference hidden in one of the seven (they are not all
identical on purpose — Rule 4 deliberately uses `isEquivalent()` between type
*arguments*). Containment: migrate one site per commit; a site whose
behaviour must differ gets an explicit, documented parameter, not a fork.
**Non-goals** No `SemanticModel`, no removal of `GlobalSymbolTable`.
**Completion** `grep` finds no `dynamic_cast<const TurfGenericParamType` outside
`TypeWalk` and the INV-GEN-1 assertions; new invariant **INV-GEN-2** "every
generic-parameter query goes through `TypeWalk`", enforced by a
`check_boundary.sh` grep.

---

### FDN-003 — Module-scoped type identity

**Kind** Foundation · **Rules** R1, R3 · **Size** L · **Prereqs** OVH closure

**Objective.** A struct/enum/error/machine type is identified by *(owning
module, name)*, not by bare name. Two modules may each declare `Parser`,
`Node`, `Config`; a user may declare `struct List` in a program that never
imports `list`.

**Rationale.** Today type identity is global by bare name
(`modules_and_scope_resolution.md` §2.6: *"two modules declaring the same
struct name is a genuine, permanent conflict regardless of import mode"*;
`LLVMTypeCache` keyed by bare name). Verified: a program with **no imports**
cannot declare `struct List` (the prelude's `strings` pulls `list::List`
in), and `struct Parser` collides with `json`'s *internal* helper. With
packages (LEC-001) and private types (LEC-002) this is unworkable: every
struct name in every dependency, public or not, becomes reserved for the
whole program, and adding a struct to the stdlib in a minor release can break
user code.

**Subsystems.** Parser (`RegisterType`), `ResolverTypeRegistry`
(`TypeOwningModule_`), `GlobalSymbolTable`/`LLVMTypeCache` keys, TypeChecker
type identity (`isEquivalent`, `sameTypeExact`), Monomorphizer mangling
(clone names), Codegen (`%struct` names, method mangling
`turf_<module>_<Struct>.<method>…`), descriptor names (`TurfTypeDesc.name`),
DWARF type names, `typeof()` output, diagnostics.

**Stages.**
1. **Design + freeze entry (DECISION).** Recommended: internal identity is
   `module::Name`; user-visible spelling is unchanged (`Parser` where it is
   visible unqualified, `json::Parser` otherwise); `typeof()` returns the
   *qualified* name only when the type is not declared in the current file
   (so every existing test's `typeof` expectation on a local type is
   unchanged); LLVM struct names become `mod.Name`. The main-program file's
   module name is `main` (it has no module name today — pick and freeze it).
2. Registry and symbol tables keyed by qualified identity; name lookup keeps
   today's visibility rules (`isTypeVisibleUnqualified`) as the *resolution*
   step from spelling to identity.
3. Codegen/mangling/descriptors/debug info switch to qualified identity.
4. Remove `DuplicateTypeNameError`'s cross-module case (keep same-module
   duplicates as errors); an unqualified reference that could mean two
   visible types becomes an **ambiguity error** naming both.
5. Stdlib and showcase sweep.

**Tests.** `tests/07_functions_modules/`: two companion modules each
declaring `struct Node` with different fields, both used from main;
`struct List` in a file with no imports; `struct Parser` + `import json`;
ambiguity error when both are imported unqualified with `[*]`; generic
instantiation of `a::Box<b::Item>` vs `b::Box<a::Item>`; `typeof` output for
local and imported types; DWARF shows qualified names (`debuginfo_smoke.sh`).

**Docs.** `modules_and_scope_resolution.md` §2.6 rewritten; `runtime.md`
(descriptor names). **Breaking.** Only `typeof()` output for imported types
(qualified) and programs that relied on the old error; mechanical. **Risks.**
Wide blast radius — every place that builds a type name string. Containment:
stage 2 keeps a compatibility accessor returning the bare name for display
only, and a debug assertion that identity comparisons never go through it
(INV-TYPE-1's rule restated: identity never comes from a display string).
**Non-goals** No per-declaration visibility (LEC-002), no nested modules, no
package names in identities yet (LEC-001 prefixes the package if needed).
**Completion** all tests above; full suite; stdlib/showcase sweep; new
**INV-TYPE-3** "type identity is (module, name)", enforced by the registry's
key type.

---

### FDN-001 — Safe heap allocation of user types

**Kind** Foundation · **Rule** R2 (and the intent of Overhaul DoD 8) · **Size** M · **Prereqs** OVH closure (OVH-001, OVH-002, OVH-010)

**Objective.** Safe code can allocate a value of any user type on the GC heap
and hold a `&T` to it, so a linked list, tree or graph of *dynamic* size is
writable without `unsafe` — the capability §0.5 of the Overhaul promised and
LANG-002's tests only demonstrated with a fixed set of stack nodes.

**Rationale.** Today the only safe way to obtain a `&Node` is `&localVar`; the
GC heap is reachable only through the stdlib's untyped `gc_alloc(n): void*`,
which needs `unsafe`. Verified: writing the obvious `make()` function returns
a dangling reference to a dead stack frame and silently computes the wrong
answer (audit F-16).

**Design (DECISION — freeze entry required).** Recommended: a new closed-list
intrinsic `heap(expr)`:

```turf
fn cons(v: int, next: &?Node): &Node {
    mut n: Node = Node::default();
    n.val = v;
    n.next = next;
    return heap(n);          // copies n into a GC block with Node's descriptor; returns &Node
}
```

- Type rule: `heap(e: T): &T` for any sized `T` (structs, arrays, primitives);
  `T` must not contain a `&U` field that `heap` would copy from a *stack*
  reference (FDN-002's rule applies to the operand).
- Lowering: `turf_rt_alloc(sizeof(T), &desc_T)` (the real per-type descriptor —
  OVH-001's wide form), store, return the pointer. No new representation:
  `&T` is already an opaque pointer, and the GC already scans `&T` fields
  (`INV-NREF`'s tracing note, LANG-002).
- Why an intrinsic rather than syntax (`new T`): it is one entry in
  `include/Intrinsics.h` (F-STD-2's closed list), no grammar change, and it
  composes with `T::default()`/`T::new()`, which already exist. Alternatives
  considered: `T::heap(...)` (conflicts with user methods named `heap`),
  `box T` keyword (grammar change, no benefit).
- `gc_alloc`/`gc_realloc` stay stdlib-internal.

**Subsystems** Parser (none if an intrinsic), Resolver/TypeChecker (builtin
signature), Codegen (descriptor reference), `Intrinsics.h`, `Builtins.cpp`.
**Runtime** none new. **Stdlib** `json`/`http` may use it instead of by-value
lists where a node needs identity; not required.

**Tests.** `tests/39_nullable_references/05_dynamic_linked_list.tr` (1,000
nodes built in a loop through a helper, traversed, summed; survives
`gc_collect()` with churn at `-O0`); `06_dynamic_binary_tree.tr` (insert N
random keys, in-order traversal sorted); `07_graph_adjacency.tr` (heap nodes
held in a `List<&Node>`, each with a `List<&Node>` of neighbours, including a
cycle — the GC must collect the whole graph once unreachable, checked with
`gc_is_managed`/stats); `fail_heap_of_void.tr`.

**Docs** `pointers_and_references.md`, `memory_model.md`, reference docs.
**Breaking** none (additive; required by R2 because the safe alternative does
not exist). **Risks** Low; the GC path already exists. **Non-goals** No
destructors, no manual `free` of `heap` objects (INV-MEM-1 already panics),
no placement/arena allocation.
**Completion** the three tests at every `-O` level; `gc_rss_smoke.sh`-style
bounded-RSS check for a loop that builds and drops lists.

---

### FDN-002 — Reference and array escape rules

**Kind** Foundation · **Rule** R2 · **Size** M · **Prereqs** FDN-001 (so the
diagnostic can suggest `heap(...)`) · absorbs §27 D23

**Objective.** A reference to stack storage can never outlive the frame it
points into, statically, for the cases the language can express.

**Rationale.** `&T`/`&?T` are the language's *safe* references, but nothing
stops `return &local;`, storing `&local` into a struct that is returned or
heap-allocated, or returning/storing an unsized `T[]` parameter (which aliases
the caller's frame, D23). Each produces a silently dangling pointer.

**Design (freeze entry).** An intraprocedural, conservative rule in the
style of the existing CFG passes (no lifetime annotations, no borrow checker —
that would violate the "small language" constraint of Overhaul §0.6):
- Every reference-typed value carries a *provenance* in a new dataflow pass:
  `Stack(frame-local)`, `Param` (came in from the caller), `Heap` (from
  `heap()`, a collection, or a `&` of a heap object's field), `Unknown`.
- **Error** when a `Stack` provenance value flows into: a `return`; a field of
  a `Param`/`Heap` object; a collection (`push`, `set`, `operator[]=`); the
  operand of `heap()`; a global root.
- `T[]` parameters are `Param` provenance but may not be converted to `ptr`
  and returned (D23).
- `self` is `Param`.
- Everything the pass cannot classify is `Unknown` and **allowed** (no false
  positives on existing code — soundness improves monotonically; record the
  residual gap honestly in the change record).

**Subsystems** CFG/analysis (new `EscapePass` beside `NullabilityPass`),
diagnostics (new error class, generic category per F-DIAG-3).
**Tests** `tests/39_nullable_references/fail_return_address_of_local.tr` (the
audit's F-16 program), `fail_store_local_ref_into_param_field.tr`,
`fail_push_local_ref_into_list.tr`, `fail_return_unsized_array_param_as_ptr.tr`,
plus positives: returning `&param.field`, returning `heap(local)`.
**Breaking** yes (R1) — programs that compiled into dangling references now
fail; they were already broken. **Risks** False positives on legitimate
patterns — measure against the full suite + stdlib + showcase before landing;
any hit is either a real bug (fix the program) or a pass bug. **Non-goals**
No lifetime syntax, no interprocedural summaries (FUTURE F-5 neighbourhood).
**Completion** all negative tests rejected, no new diagnostics in `tests/`,
`lib/std`, `showcase`; new **INV-MEM-3** documented with its known residual
(`Unknown` provenance).

---

### FDN-004 — String indexing: bounds checking, and `s[i]` as `char` (D17)

**Kind** Foundation · **Rules** R2 (bounds), R1 (type) · **Size** S–M · **Prereqs** OVH closure

**Objective.** `s[i]` on a `string` is bounds-checked against `strings::length`
semantics (panic with `@caller_location`-style attribution), and — **DECISION**
— yields `char` instead of `int`.

**Rationale.** Verified: `"abc"[5]` reads past the terminator and returns
garbage (audit F-17). The `int` result type is the recorded inconsistency C-7
/ D17: `char` exists and is one byte. It is breaking, so after 2.0 it can only
change in 3.0.

**Design notes.** Bounds check cost: `string` is a NUL-terminated pointer
(F-STD-3, frozen), so a check is `O(n)` `strlen` per access. Options:
(a) check `i >= 0 && i < strlen(s)` (correct, `O(n)`, loops become `O(n²)`
unless OPT-002-style elision proves `i < strlen` from the loop condition —
extend `ValueRangePass` to recognise `while i < strings::length(s)`);
(b) check only `i >= 0` and "not past the terminator" by scanning up to `i`
(also `O(i)`). **Recommendation:** (a) with the elision extension, and a
stdlib `strings::byte_at_unchecked` inside `unsafe` for hot paths. Measure the
stdlib's own string loops before and after; record numbers.
For the type change: `s[i]` yields `char`; comparisons with character
literals (`s[i] == 'a'`) become natural; code comparing with integers
(`s[i] == 97`) must write `s[i] to int` — **automatable** with a type-directed
rewrite (the compiler knows every `s[i]` site; a `--migrate` mode can print
the edits).

**Tests** `tests/12_string_ops/`: OOB read panics (`RUNTIME_ABORT_EXPECTED`),
negative index panics, in-range reads return `char`; `typeof(s[0])` is `char`;
elision proven by `--emit-llvm -O0` on a canonical loop (INV-OPT-2 style).
**Stdlib** `strings.tr`, `char.tr`, `json.tr`, `http.tr` migrate (they compare
bytes with integers). **Breaking** yes, twice; migration documented in Part V.
**Non-goals** No fat strings (F-STD-3), no UTF-8 semantics (a string is
bytes), no string slicing syntax.
**Completion** tests pass; the stdlib's string-heavy tests show no worse than
+10% runtime at `-O2` (record the numbers).

---

### FDN-006 — Runtime contract

**Kind** Foundation · **Rules** R3, R5 · **Size** M · **Prereqs** OVH closure

**Objective.** Every runtime failure a user can hit ends in a documented
message and exit code, and a binary can never silently run against the wrong
runtime.

**Stages.**
1. **Stack overflow** — today a bare SIGSEGV, exit 139, no message (audit
   F-20). Install a `sigaltstack` + `SIGSEGV` handler in `turf_rt_init()` that
   recognises a guard-page fault near the stack limit and reports
   `panic: stack overflow` through `turf_rt_panic`'s format; other SIGSEGVs
   report `panic: segmentation fault (this is a bug in unsafe code or in
   TurfLang)` with the faulting address. Exit code: 134 like every panic.
2. **OOM** — verify and test the policy RT-001 put "in one place": an
   impossible `gc_alloc`/`heap`/collection growth ends in `panic: out of
   memory` (the audit's attempt spun in `strings::repeat`'s quadratic loop
   instead of reaching it — write the test against a direct large
   allocation). `mem::alloc` keeps its own `sys::panic` path (F-MEM-3).
3. **Compiler/runtime ABI stamp** — `libturfrt` exports
   `turf_rt_abi_2_0` (a versioned symbol); every compiled object references
   it, so linking against a mismatched runtime fails at link time with a
   symbol name that says why, instead of undefined behaviour at run time.
   The compiler's driver checks `TURF_HOME/lib/libturfrt.a` exists before
   invoking `cc` and reports a TurfLang diagnostic (not a linker error) if
   not.
4. **Panic format and exit codes** documented once (`runtime.md`, reference
   docs): panic → 134 (SIGABRT), stack overflow → 134, `sys::exit(n)` → n.

**Tests** `scripts/runtime_smoke.sh` gains: deep recursion prints
`panic: stack overflow`; huge allocation prints `panic: out of memory`;
linking with `TURF_HOME` pointing at a runtime-less tree gives the TurfLang
diagnostic. **Risks** Signal handling interacts with the GC's conservative
scan (the alt stack must not confuse `g_stack_base`) — test with a collection
triggered near the stack limit. **Non-goals** No unwinding, no `defer` on
panic (INV-DEFER-1 stays), no backtraces (FUTURE F-6b).
**Completion** smoke checks pass; `runtime.md` documents all four.

---

### FDN-005 — GC hardening and validation

**Kind** Foundation · **Rule** R2 · **Size** M (spread across Phases 1–3) · **Prereqs** OVH-001/002/010

**Objective.** Retire MEM-002's two honest open questions before a public
release: *the collector has only run this repository's tests*, and *the
register-capture technique has not been validated independently*.

**Stages.**
1. **Stress corpus** — long-running programs: a 10⁶-node graph built and
   rewired repeatedly; a `Map<string, List<string>>` churned for 10⁷
   operations; the `http_server` showcase under a scripted client for 10⁵
   requests; each asserts content integrity at the end and bounded RSS.
2. **Collect-on-every-allocation mode** — a `TURF_GC_STRESS=1` environment
   variable (runtime-internal, documented as a testing aid, like
   `TURF_RT_STATS_DUMP`) that forces a collection on every allocation. Run
   the **entire** test suite under it at `-O0` and `-O2` in CI. This is the
   single most effective way to surface the F-01/F-02 class of bug, which
   today only appears by luck.
3. **Independent validation** — build the runtime with ASan in a CI job
   (freed-block reuse becomes a hard error), and cross-check root-finding
   against a debug mode that additionally scans the full stack range
   conservatively *and* compares mark sets.
4. **Pause/throughput numbers** re-measured and published.

**Tests** the corpus above, as CI jobs (some nightly). **Risks** Finds bugs —
that is the point; budget for fixes. **Non-goals** No generational, moving,
incremental or concurrent collector (§25 items 9–11).
**Completion** the whole suite green under `TURF_GC_STRESS=1` at `-O0` and
`-O2` with ASan; stress corpus green; MEM-002's open questions answered in a
change record.

---

### SMP-001 — Compiler query facade

**Kind** Foundation (Simplification phase 2, facade half) · **Rule** R5 · **Size** M · **Prereqs** OVH closure

**Objective.** One stable, in-compiler, tested API that analyses a set of
source buffers and answers tool queries, so `turf-lsp`, `Sandbox` and future
tools stop reaching into globals.

**Rationale.** `turf-lsp/src/CompileBuffer.cpp` saves/restores ~10 globals
around every analysis; `Sandbox::validate()` does the same inside the
compiler (Simplification §1). `turf-lsp` does not build against HEAD today
(it overrides the deleted `visitDefaultInit`). The facade is `turf-lsp`'s own
plan item A.

**Design.** `include/turf/Analysis.h`:
```cpp
namespace turf {
struct SourceBuffer { std::string Path; std::string Text; };
struct AnalysisOptions { bool AllowUnsafeModules = false; /* … */ };
class AnalysisResult {                       // owns its ASTs; read-only
 public:
  const std::vector<Diagnostic>& diagnostics() const;
  const TurfType* typeAt(const std::string& Path, unsigned Line, unsigned Col) const;
  std::optional<SourceLocation> definitionOf(const std::string& Path, unsigned Line, unsigned Col) const;
  std::vector<SymbolInfo> symbolsInScope(const std::string& Path, unsigned Line, unsigned Col) const;
};
AnalysisResult analyze(const std::vector<SourceBuffer>&, const AnalysisOptions&);
}
```
Internally it runs Lexer→…→TypeChecker (no Codegen), and the save/restore of
globals lives **only** here, under a mutex (single analysis at a time is
acceptable for 2.0). `Sandbox` is ported onto it first (it is in-repo and
tested by `slm_deterministic_fix_smoke.sh`).

**Stages** (1) facade + unit tests (a small C++ test binary in `tests/api/`,
built by CMake, run by CI); (2) port `Sandbox`; (3) port `turf-lsp` (in its
repo) and add a CI job there that builds against the compiler's tagged
release. **Breaking** none for users. **Risks** Globals the facade forgets to
isolate — the unit test runs two analyses of different programs back to back
and diffs results against isolated runs. **Non-goals** No removal of
globals, no concurrency, no incremental re-analysis (all FUTURE F-S*).
**Completion** `Sandbox` and `turf-lsp` use only the facade; the API header is
documented as the 2.0 tool API (semver-stable for 2.x).

---

### LEC-002 — Per-declaration visibility: `pub` (D13)

**Kind** Language/ecosystem · **Rules** R1, R4 · **Size** M–L · **Prereqs** FDN-003

**Current limitation.** Everything declared at a module's top level is
visible to every importer (`modules_and_scope_resolution.md` §1.3). There is
no way to hide a helper function, a helper struct (`json`'s `Parser` is part
of the stdlib's public surface by accident), or a struct field
(`List<T>.data`, a raw pointer, is public because nothing can be private —
STD-003 item 5).

**Decisions (freeze entries — DECISION, recommendation shown).**

| Question | Recommendation | Why |
|---|---|---|
| Default | **private to the module**; `pub` opts in | A package's API must be explicit to be versionable (LEC-001); "public by default" makes every helper a semver commitment. 2.0 is the only cheap moment to flip it (R1). |
| Keyword | `pub` or `public` (prefix; synonyms, like `mut`/`mutable`) | Short, precedent (Rust), no conflict found in `tests/`/`lib/std`/`showcase` — grep done 2026-09-29, no identifier uses either word. The `public` spelling is an owner amendment (2026-09-29). |
| What can be `pub` | `fn`, methods (`pub fn Point.len()`), `struct`, `enum`, `error`, `machine`, struct **fields**, `export immut` | Uniform rule: every named declaration. |
| Fields | private unless `pub`, even in a `pub struct` | Needed to hide `List.data`; consistent with the uniform rule. `T::default()` stays legal from other modules (it zero-fills and cannot break an invariant zero cannot); `T::new()` is an ordinary method with its own visibility. |
| Enum/error variants, machine states | public iff the type is | A variant is part of the type's meaning; per-variant privacy has no use case. |
| Operators | always visible wherever their operand types are | Dispatch is type-directed (F-OP-3); the orphan rule (INV-OP-2) already ties an operator to a type's module. A private operator would make `a + b` depend on import visibility. |
| `export immut` | keeps its meaning (a module-level constant visible to importers) — `export` *is* its `pub` | Avoids a second keyword on one declaration; top-level private constants are additive later. |
| Private-in-public | error: a `pub` signature/field may not mention a private type | Otherwise an importer can obtain a value of a type it cannot name. |
| UFCS | a private function is not a UFCS candidate outside its module | Visibility must not be bypassable by call syntax. |
| Generics | a `pub` generic's body may call private helpers; the monomorphized clone is resolved **in the template's module context**, never the instantiation site's | Otherwise every stdlib generic would have to make its helpers public. |
| Packages (LEC-001) | two levels only: module-private and `pub`. A package's *internal* modules are not importable from outside the package, so their `pub` items are package-visible | Three effective levels with one keyword; `pub(package)` is FUTURE if ever needed. |

**Subsystems.** Lexer (`TOK_PUB`), Parser (all declaration forms, fields),
AST (a `Visibility` field on declarations and struct fields), Resolver
(visibility checks at every cross-module lookup: functions, types, fields,
methods, constants; UFCS candidate filtering), TypeChecker (field access,
private-in-public), Monomorphizer (context of clone resolution — **verify
live** how clones are resolved today before relying on it), diagnostics.

**Stages.** (1) Freeze entries; grep for `pub` identifiers. (2) Parse and
store visibility, default private, **but enforce nothing** — then add `pub` to
the whole stdlib and showcase (mechanical; a script can add `pub` to every
top-level declaration and every field, then a human removes it from helpers
like `json::Parser`, `Map.bucket*`, `hashing` internals, `List.data`).
(3) Enforce for functions/types/constants. (4) Enforce for fields and methods.
(5) Private-in-public rule. (6) Migration tool: `turf --migrate=pub <files>`
prints (or with `--write` applies) `pub` on every top-level declaration and
field of a multi-module project — the exact v1 behaviour, as a starting point.

**Diagnostics.** "`helper` is private to module `m`" with a note at the
declaration and a hint "mark it `pub` in m.tr if it is meant to be used from
other modules"; private-in-public names both declarations.

**Tests.** `tests/07_functions_modules/visibility/`: private fn/struct/field/
method/constant rejected from another module; `pub` accepted; UFCS on a
private function rejected; a `pub fn<T>` calling a private helper works when
instantiated from another module; private-in-public rejected; `T::default()`
of a struct with private fields allowed; single-file programs unaffected.
**Stdlib impact** every module; the public surface is then *reviewed* as part
of LEC-007. **Showcase** all programs. **Breaking** yes (R1): every
multi-module program must add `pub` — automated by stage 6.
**Risks.** Monomorphization context (stage 3) and the prelude (its items must
be `pub`). **Non-goals.** No `pub(package)`/`protected`, no friend modules,
no per-variant visibility, no private top-level constants.
**Completion** tests; stdlib public surface listed in the change record;
migration tool round-trips the v1 test suite.

---

### LEC-001 — Projects, dependencies and builds (D7, scoped)

**Kind** Language/ecosystem · **Rules** R3, R4 · **Size** L · **Prereqs** FDN-003, LEC-002

The full answer to *"how does an external person obtain, build, import and
distribute a non-trivial TurfLang project?"* is Part IX. This milestone
builds it. Of D7's three named problems, 2.0 builds **one**:

| Problem | 2.0? | Why |
|---|---|---|
| Package management (manifest, dependencies, lockfile, fetch, build) | **Yes, minimal** | R3 — a public language needs it. |
| Separate compilation | **No** (FUTURE F-7a) | Monomorphization needs generic *source* anyway; whole-program builds are fast (0.26–0.40 s for the showcase, TYPE-002 measurements) and give LTO-equivalent optimization for free. Revisit when a real project's build time says so. |
| Stable Turf-to-Turf binary ABI | **No** (FUTURE F-7b) | Packages ship as source, so no binary compatibility promise is needed. The stable interop ABI is the C ABI (`extern fn`), which exists. The compiler↔runtime ABI is internal and lock-step (FDN-006 stamps it). |
| Registry, semver ranges, publishing | **No** (FUTURE F-7c) | Git-hosted dependencies with pinned revisions are sufficient and have no infrastructure to run. `turf-cli`'s registry prototype is the starting point later. |

**Current state (verified).** Module names are flat file stems resolved in
the order `$TURF_HOME/lib/std/**` → `lib/usr/**` → the source file's
directory → cwd (`modules_and_scope_resolution.md` §2.4). A local `json.tr`
is **silently shadowed** by the stdlib (audit F-19); two files named
`util.tr` in different directories cannot coexist. `turf-cli` (a separate Go
tool, last touched 2026-06) writes `turf.toml`/`turf.lock` and installs into
`turf_modules/`, which the compiler never searches.

**Decisions (DECISION — recommendation shown).**

1. **One binary.** Project commands live in the compiler driver as
   subcommands (`turf new`, `turf build`, `turf run`, `turf test`,
   `turf fmt`, `turf clean`); `turf file.tr` keeps working exactly as today.
   `turf-cli` is not shipped with 2.0; its manifest/lockfile/semver code is
   the reference for the FUTURE registry. *Why:* one version to ship,
   lock-step with the stdlib and runtime it must match, no second toolchain
   (Go) in the release pipeline, no two programs both called `turf`.
2. **Package layout.**
   ```
   myapp/
     turf.toml          # manifest
     turf.lock          # generated; commit it
     src/main.tr        # binary package: entry point
     src/<name>.tr      # library package: root module, named after the package
     src/**.tr          # internal modules (importable only inside this package)
     tests/*.tr         # `turf test` (REL-002)
   ```
3. **Manifest** (`turf.toml`, a documented TOML subset):
   ```toml
   [package]
   name = "myapp"
   version = "0.1.0"
   turf = "2.0"                    # minimum compiler version; major must match

   [dependencies]
   geometry = { path = "../geometry" }
   jsonx    = { git = "https://github.com/someone/jsonx", tag = "v1.2.0" }
   crc      = { git = "https://example.org/crc.git", rev = "3f2a9c1" }

   [unsafe]
   allow = ["crc"]                 # dependencies permitted to declare `unsafe module;` (F-STD-1)
   ```
4. **Module identity and resolution.** A module is identified by
   *(package, path under `src/`)*. Within a package, `import foo;` resolves to
   `src/foo.tr` (or `src/**/foo.tr` if unique — ambiguity is an error); a
   dependency is imported by its package name (`import jsonx;` → its root
   module); the stdlib keeps its bare names. **Any name that resolves to more
   than one of {own module, dependency root, stdlib module} is a hard error
   naming every candidate** — never silent shadowing. Single-file mode (no
   `turf.toml` up the directory tree) keeps today's search order *but* turns
   the silent stdlib shadow into the same error.
5. **Fetching and reproducibility.** `turf build` resolves dependencies,
   fetches git dependencies with the system `git` into
   `~/.cache/turf/src/<name>-<commit>/`, writes `turf.lock` (package, source,
   resolved commit, SHA-256 of the fetched tree), and verifies the hash on
   every build; `--offline` never touches the network; `--locked` fails if
   the lockfile would change. Diamond dependencies must agree on one commit,
   otherwise a hard error listing both requirers (no multi-version support in
   2.0).
6. **Capabilities across packages.** A dependency that declares
   `unsafe module;` builds only if the consuming manifest lists it under
   `[unsafe] allow` — the existing `--allow-unsafe-modules` mechanism
   (INV-CAP-1), made per-package and visible in the consumer's manifest.
7. **Distribution.** A library is distributed by pushing a git tag; a program
   is distributed as the linked executable (it depends only on libc/libm and
   the C libraries its `extern from` declarations name — `libturfrt` is
   linked statically, `CMakeLists.txt`'s RT-001 rationale).

**Subsystems.** Driver (`main.cpp` subcommands, a TOML-subset parser, git
invocation, cache, lockfile, hashing via LLVM's `SHA256`), Parser module
resolution (`ParseModuleImportByName`, `IndexModuleDir`), Resolver module
identity (with FDN-003), diagnostics.

**Stages.** (1) Freeze entries 1–6. (2) Collision errors in single-file mode
(small, shippable alone). (3) Manifest + path dependencies + `turf build/run/new`.
(4) Git dependencies, cache, lockfile, hashing, `--offline/--locked`.
(5) `[unsafe] allow`. (6) Example multi-package project in
`examples/` (new top-level folder, built by CI).

**Tests.** Script-level (the test runner is single-file): `scripts/project_smoke.sh`
builds `examples/` projects: path dependency, git dependency served from a
local bare repository (no network in CI), diamond agreement and conflict,
lockfile tamper detection, `--offline`, `--locked`, module-name collision
errors (own vs std, dep vs std, dep vs dep), internal module not importable
from outside, unsafe dependency rejected without `allow`. Single-file
collision test in `tests/07_functions_modules/`.
**Docs** new `docs/reference/projects_and_packages.md`; README Quick Start.
**Breaking** the silent-shadow case becomes an error (R1, safe). **Risks**
Scope creep toward a registry — refuse it (FUTURE F-7c). **Non-goals**
Registry, semver ranges, workspaces, build scripts, separate compilation,
binary packages, per-package compiler flags.
**Completion** `project_smoke.sh` green in CI; the Part IX walkthrough works
verbatim on a clean machine.

---

### LEC-003 — Richer pattern matching (D14)

**Kind** Language/ecosystem · **Rule** R4 · **Size** L · **Prereqs** SMP-002, FDN-004

**Current limitation.** `compare subject { matches <expr>: … done … otherwise: … }`
compares the subject with each `matches` value by equality; stacked
`matches` headers share a body; enum/machine subjects get exhaustiveness
checking. There are no bindings, no destructuring, no guards, and nothing can
read an error payload (D15) — `docs/architecture/errors_and_recover.md` §5.1
names destructuring as the missing piece.

**Design (freeze entries — DECISION, recommendation shown).**

- **Backward compatibility first.** A top-level `matches <expr>:` keeps its
  exact meaning (equality against a value expression, which may be a
  variable). New pattern forms are syntactically distinct, so no existing
  program changes meaning.
- **New pattern forms** (usable at top level and nested):
  - `_` — wildcard. Requires the lexer to accept `_`-prefixed identifiers
    (today `_x` does not lex at all, audit F-22, although the compiler's own
    lint hints recommend a `_` prefix). `_` alone is reserved as the
    wildcard; `_name` is an ordinary identifier (and suppresses the
    unused-variable warning, as the hint already promises).
  - **Struct destructuring:** `matches Point { x: 0, y }:` — `y` binds the
    field; `x: 0` tests it; `x: _` ignores it; `..` ignores the rest. Inside
    a destructuring pattern a bare identifier **binds** (Rust's rule); to
    compare a field with a variable, use a guard.
  - **Error-variant destructuring** (with LEC-004):
    `matches IOError::NotFound(path):` binds payload fields positionally.
  - **Guards:** `matches Point { x, y } if x == y:`.
  - **Alternatives:** existing header stacking; no nested `|` in 2.0.
- **Bindings** are immutable, scoped to the arm body, and definitely assigned
  (the CFG models them as defs on the arm's entry edge).
- **Exhaustiveness.** Enum/machine/`bool` subjects without `otherwise` must
  cover every variant (existing rule, extended through guards: a guarded arm
  never counts toward exhaustiveness). Struct patterns are refutable unless
  every field position is a binding/`_`. An `AnyErrorType` subject (an open
  union) always requires `otherwise`. A non-exhaustive `compare` over an
  `int`/`string` subject stays legal (current behaviour, "no-op when nothing
  matches") — changing that is R1 with no soundness benefit.
- **Unreachable arms** (an arm after an irrefutable one, a duplicate literal)
  are warnings through `DiagnosticEngine`.
- **`compare` stays a statement.** An expression form is FUTURE (F-14).
- **Excluded from 2.0:** range patterns, `double` literal patterns, nested
  or-patterns, enum payloads (plain enums stay ordinal-only — sum types over
  `enum` are FUTURE F-14b; LEC-004's payload representation is designed so
  they can reuse it).

**Lowering.** Arms are tested in source order (first match wins — confirm
the current `compare` semantics against `control_flow.md` and freeze it).
Each pattern compiles to a sequence of field loads + comparisons + guard, all
on the AST side of Codegen as today (Simplification phase 4 would move this to
a lowering IR later — the arm-order semantics must be independent of that).
Monomorphized subjects are concrete, so no generic patterns exist at Codegen
(INV-GEN-1). CFG passes (`DefiniteAssignmentPass`, `NullabilityPass`,
`EscapePass` from FDN-002) must see bindings and guard edges.

**Subsystems.** Lexer (`_`), Parser (pattern grammar), AST (a `Pattern`
hierarchy), Resolver (binding scopes), TypeChecker (pattern typing,
exhaustiveness), CFG/analysis, Codegen, diagnostics, `ASTDiff`/`clone`
(SLM sandbox), `turf-lsp` (via SMP-001).

**Tests.** New `tests/45_pattern_matching/`: each form; nesting two levels;
bindings shadowing an outer name; guards that fail fall through; generic
subjects (`Pair<int,string>` destructured inside a generic function);
exhaustiveness errors (`fail_` for a missing enum variant, a guarded-only
cover, an `AnyErrorType` without `otherwise`); unreachable-arm warning;
`_x` identifiers anywhere; every existing `compare` test unchanged.
**Docs** `control_flow.md` §compare rewritten; handbook row. **Breaking**
`_` becomes reserved (grep); otherwise none. **Risks** Grammar ambiguity
between `matches Name { … }` destructuring and a brace-bodied expression — a
struct name followed by `{` in pattern position is always a pattern (struct
brace-literal *expressions* were removed, C-3), so the ambiguity does not
exist; confirm with the parser. **Non-goals** as excluded above.
**Completion** all tests; the stdlib uses destructuring somewhere real
(`json` value kinds are a natural candidate).

---

### LEC-004 — Error payloads and propagation (D15)

**Kind** Language/ecosystem · **Rule** R4 · **Size** L · **Prereqs** LEC-003, FDN-001, OVH-002

**Current limitation.** Error variants are bare names; STD-002 added exactly
one optional `string` message per value (`MathError::DivisionByZero("msg")`,
read as `err.message` inside `recover as err run`). There is no typed payload
(`FileNotFound(path)`), no way to destructure one, and no propagation
shorthand — every call site writes `recover as err run { return err; }`, and
even returning a `!T` call result from a `!T` function is rejected with the
self-contradictory "function declares '!Rec' but returns '!Rec'" (audit F-21).

**Design (freeze entries — DECISION, recommendation shown).**

1. **Declaration:** `error IOError { NotFound(path: string), Denied(path: string, mode: int), Timeout }`.
   Field lists are named and typed; any sized type is allowed (structs,
   generics instantiated concretely, other error-free types).
2. **Construction:** positional, `IOError::NotFound(p)`. A variant with *N*
   declared fields accepts *N* arguments (fields) or *N+1* (fields + the
   STD-002 message, which stays universal so `err.message` keeps working on
   an open-union `err` without destructuring). Variants without fields keep
   STD-002's 0-or-1 rule unchanged.
3. **Reading payloads:** only through LEC-003 destructuring —
   `compare err { matches IOError::NotFound(path): … done otherwise: … }` —
   because `err` is an open union (`AnyErrorType`) and which fields exist
   depends on the variant. No `err.path` field syntax.
4. **Propagation shorthand:** prefix keyword **`try`**:
   `mut n: int = try parse_int(s);` ≡ `parse_int(s) recover as e run { return e; }`.
   Legal only inside a function whose return type is `!U` (any `U` — the
   error union is open, so no conversion exists or is needed); inside
   `defer` it is illegal (a deferred statement may not return, INV-DEFER-2);
   in `main` (returns `int`) it is an error with a hint. `try` runs pending
   `defer`s like any `return` (it *is* one). Alternatives considered: postfix
   `?` (Rust) — rejected because TurfLang spells control flow with words
   (`recover`, `produce`, `otherwise`, `give`); `recover raise` — longer and
   reads worse in nested expressions.
5. **Pass-through return:** `return f();` where `f(): !T` and the enclosing
   function returns exactly `!T` is legal and returns the fallible value
   unchanged (it never unwraps — no implicit propagation into a non-fallible
   return type, the §5.2 rule stays).
6. **Runtime representation:** `%TurfErrorValue = { i8* tag, i8* message,
   i8* payload }`, GC-allocated with a descriptor (OVH-002 moved it to the GC
   heap); `payload` points to a GC-allocated per-variant struct with its own
   `TurfTypeDesc`, or is null for a field-less variant. `!T`'s
   `%FallibleValue` is unchanged. Tag equality stays `strcmp` on tag strings.

**Subsystems.** Parser (`ParseErrorDecl`, construction arity, `try`), Types
(`TurfUserErrorType` gains per-variant field lists), Resolver, TypeChecker
(construction, destructuring types, `try` context), Codegen
(`CodegenErrorVariantConstructWithMessage` generalised, payload structs +
descriptors via `EmitAllTurfTypeDescs()`), the never-returns-error lint (a
`try` counts as a possible error return), diagnostics.

**Tests.** `tests/25_errors/`: payload declaration/construction/destructuring;
payload containing a struct and a `List<string>` survives `gc_collect()`
(`-O0`); `try` in nested expressions (`try a() + try b()`), in a loop with
`defer` (defers run on propagation), in `main` (`fail_`), in a non-fallible
function (`fail_`), in `defer` (`fail_`); pass-through `return f();`; message
+ payload together; every existing STD-002 test unchanged.
**Stdlib migration** `io`/`fs`/`net`/`json`/`http` errors gain payloads where
a caller would want them (the file path in `FileError::NotFound`, the byte
offset in `JsonError::*`); internal `recover as err run { return err; }`
becomes `try` (mechanical). **Breaking** `try` is a reserved word (grep);
otherwise additive. **Risks** GC tracing of payloads (covered by tests and
FDN-005's stress mode). **Non-goals** closed error sets in signatures
(`!IOError int`), error-type conversion, stack traces (FUTURE F-15),
payloads on plain `enum`s (FUTURE F-14b).
**Completion** tests; `errors_and_recover.md` §5.1/§5.2 rewritten as
resolved.

---

### LEC-006 — Collection indexing coherence: generic operator declarations

**Kind** Language/ecosystem · **Rule** R5 · **Size** M · **Prereqs** SMP-002, OVH-003

**Objective.** `l[i]` / `l[i] = v` work for every `List<T>`, `Map<K,V>`,
`Set<T>` instantiation, not only `List<int>`/`Map<string,int>`/`Set<int>`.

**Rationale.** LANG-001's completion criterion "`List<T>[i]` works" holds
only for `T = int`, because `operator` declarations cannot be generic
(`ParseOperatorDecl()` parses no type-parameter list; `list.tr:84-100`). A
public language whose indexing works for `List<int>` and silently not for
`List<string>` is incoherent (R5). OVH-003 provides `get`/`set` so the gap is
not a safety problem; this makes the operator form uniform.

**Design.** Allow `operator<T> [](l: List<T>, i: int): T` (type parameters
after the `operator` keyword, mirroring `fn<T>`). Resolution stays
**exact-match** (F-OP-1): candidate matching unifies the declared operand
types against the concrete operand types via SMP-002's `unify`, then requires
`sameTypeExact` on the substituted signature — no widening introduced. The
monomorphizer instantiates operators like functions. The orphan rule
(INV-OP-2) applies to the generic struct's declaring module. Keep the
existing concrete `List<int>` operators until the generic ones land, then
delete them.

**Tests** `tests/18_operator_overload/`: generic `operator[]`/`[]=` on
`List<string>`, `List<Big>`, `Map<string, List<int>>`; exact-match regression
(`fail_operator_exact_match_no_coercion.tr` style for a generic operator);
orphan rule for a generic operator. **Non-goals** generic arithmetic
operators on user types beyond what falls out; expression-position type
arguments (`List<int>::default()`, FUTURE F-6c).
**Completion** every stdlib collection indexes uniformly; `docs/reference/stdlib/collections.md` updated.

---

### LEC-005 — Diagnostics reliability (includes D25)

**Kind** Language/ecosystem · **Rule** R5 · **Size** M · **Prereqs** LEC-003, LEC-004 (so their new diagnostics are covered)

**Scope — each item verified live by the audit:**
1. **D25** — instantiation-site errors anchor at the innermost requester
   (inside `lib/std`), one hop short of the user's call; E079's "chain" is a
   dump of the whole instantiation cache. Add a parent pointer to
   `InstantiationRequest` (`include/Monomorphize.h`), thread the current
   parent through the nested TypeChecker the monomorphizer constructs, anchor
   the primary diagnostic at the **outermost user-code** request, and print
   the real chain (user call → `Set<Big>.add` → `Set<Big>.contains` → …).
2. **Parse-error cascade.** A syntax error inside a function body (a lambda
   attempt, `List<int>::default()`, `_x`) is followed by "Statements are not
   allowed outside of a function", which is false and usually printed
   *first*. Recovery must resynchronise at the enclosing function's closing
   brace and suppress that follow-on error.
3. **Hint quality.** E040 for an unknown *type-like* name suggests
   `mut Node: int = 5;`; E040's hint must depend on whether the name was used
   in type/constructor position. Audit every hint that recommends syntax for
   consistency with the language (the `_` prefix hint becomes true with
   LEC-003).
4. **Duplicate warnings.** An unused `immut` reports both "declared but never
   used" and "dead store"; report one.
5. **False positives.** `x += 1` flagged as a dead store (LANG-001 pitfall);
   uses inside `recover` operands (OVH-012 fixes the ones the stdlib hits —
   this item finishes the class).
6. **INV-LINT-1** — make `LintExpression`'s dispatch exhaustive (planned
   since DIAG-001), so the next node kind added (LEC-003's patterns!) cannot
   be silently skipped.
7. **Stdlib warning policy** — if not decided in OVH-012, decide here.

**Tests** one per item in `tests/14_cfg_analysis/`, `tests/38_monomorphization/`
and `tests/01_syntax/`; the runner cannot assert warning *text*, so add a
`// EXPECT-DIAG: <code>` marker convention to `test_runner.sh` (a
`fail_`-style test that also greps the diagnostic code) — this also fixes the
OVH-005 "wrong-reason pass" class permanently. **Non-goals** new SLM
categories (F-DIAG-3), `@slm` (FUTURE).
**Completion** each item demonstrated; `EXPECT-DIAG` adopted by every `fail_`
test added after this milestone and back-filled for `tests/36_numeric_semantics/`.

---

### LEC-007 — Standard library 2.0 surface

**Kind** Language/ecosystem · **Rules** R5, R1 (API removals) · **Size** M · **Prereqs** LEC-002, LEC-004, LEC-006, FDN-004

**Objective.** The stdlib that ships in 2.0 is coherent, `pub`-annotated,
documented, and *frozen* as the 2.x compatibility surface.

**Scope.**
- **API review** of every module's `pub` set (LEC-002 stage 2 produced it):
  remove accidental exports; consistent naming (`snake_case` functions
  throughout — check); consistent fallibility (a function that can fail on
  input returns `!T`; a function that fails only on programmer error panics
  with `@caller_location`) — document the rule in `docs/reference/stdlib/README`.
- **Removals (R1, done now or not until 3.0):** the deprecated
  `list_sort`/`list_binary_search`/`list_find` wrappers (STD-003 kept them
  "for one cycle"); anything the review marks.
- **`StringBuilder`** in `strings` — the frozen string decision (§15.9,
  F-STD-3) justifies keeping `string` a C pointer *because* a builder covers
  incremental construction; it does not exist. `strings::repeat` becomes
  linear (use the builder).
- **Payloads/`try`** (LEC-004) adopted where they add information.
- **Resource cleanup idiom** — every handle type (`fs` files, `net` sockets,
  `TcpListener`) has an explicit `close()` and every example uses
  `defer x.close();` (F-MEM-2: no finalizers).
- **`test` module** gains what `turf test` (REL-002) needs: nothing beyond
  `assert*`/`fail` plus exit-code conventions, unless REL-002 decides
  otherwise.
- **Not added:** new modules (TLS, UDP, async, regex, …) — FUTURE F-12/F-19.

**Tests** each touched module's tests; a new `tests/46_stdlib_surface/`
compiling a program that references every `pub` item once (catches
accidental removals in 2.x — a compile-only API lock).
**Docs** `docs/reference/stdlib/**` rewritten in REL-006 from the reviewed
list. **Completion** API lock test green; review recorded in the change
record.

---

### REL-001 — Version, CLI, and the runtime ABI stamp

**Kind** Release engineering · **Rule** R3 · **Size** S · **Prereqs** FDN-006 (ABI stamp)

**Scope.** `turf --version` (compiler version, LLVM version, runtime ABI
version, install root) and `turf --help` (today both print "Unknown
option"); the version single-sourced from CMake into the binary, the runtime
(`turf_rt_abi_2_0`), the release archive name and README; exit codes of the
driver documented (0 ok, 1 compile error, 2 usage error — decide and
freeze); unknown flags fail with a suggestion; subcommand dispatch
(`new/build/run/test/fmt/clean`, LEC-001/REL-002) with `turf <cmd> --help`.
**Tests** a `scripts/cli_smoke.sh`. **Completion** the smoke passes; README
shows `turf --version` output.

---

### REL-002 — Developer tooling: `turf test`, `turf fmt`, LSP, editor

**Kind** Release engineering · **Rules** R3, R5 · **Size** M · **Prereqs** SMP-001, LEC-001

**`turf test`.** Discovers `tests/**/*.tr` in a project, compiles and runs
each, and applies the conventions `scripts/test_runner.sh` already enforces
(plain file must exit 0; `fail_*` must fail to compile; `RUNTIME_ABORT_EXPECTED`
must compile then abort; `EXPECT-DIAG` from LEC-005). `test_runner.sh`
becomes a thin wrapper over it, so the conventions have one implementation.
In-source `@test` functions are FUTURE (language surface).

**`turf fmt`.** The existing `--format`/`--write` (indentation-only) is
promoted to a subcommand with a stability guarantee: idempotent
(`fmt(fmt(x)) == fmt(x)`), never changes the token stream (verified by
re-lexing), runs over `lib/std/**` and `tests/**` in CI with `--check`. A
full pretty-printer needs a trivia-preserving lexer (Simplification phase 0)
— FUTURE F-11b.

**LSP.** `turf-lsp` rebuilt on SMP-001's facade: diagnostics, hover (type at
position), go-to-definition, document symbols. Its CI builds against the
tagged compiler. Completion, rename, find-references and formatting-on-save
beyond `turf fmt` are FUTURE F-11. The VS Code extension (`editors/vscode`)
is published pointing at the release binaries; it must not assume features
the server does not have.

**Tests** `turf test` runs the repository's own suite with identical results
to `test_runner.sh`; `turf fmt --check` clean on the repo; the LSP's own test
suite in its repo green against the release candidate.
**Non-goals** debugger adapter (gdb works via `-g`), doc generator (FUTURE
F-11c), REPL.

---

### REL-003 — Platforms, distribution, installation (includes D20)

**Kind** Release engineering · **Rule** R3 · **Size** M (Tier 1) / L (each extra platform) · **Prereqs** OVH-013 decision, FDN-006

**Decisions (DECISION — recommendation shown).**

| Question | Recommendation |
|---|---|
| Tier 1 (tested every commit, release-blocking) | **Linux x86-64, glibc** |
| Tier 2 (built and smoke-tested per release, best effort) | **macOS arm64** — only if the runtime port (AAPCS64 callee-saved `x19–x28`, `fp`, `d8–d15`; stack bounds via `pthread_get_stackaddr_np`; no `/proc`) lands and passes `gc_smoke.sh` + FDN-005's stress mode; otherwise not shipped. |
| Windows | **Not in 2.0** (FUTURE F-20b): Win64 callee-saved set (`rbx rbp rdi rsi r12–r15 xmm6–15`), stack limits via `GetCurrentThreadStackLimits`, MinGW vs MSVC toolchain, `libm`/`-l` differences. |
| Cross-compilation (D20) | `--target` produces an **object file** for any LLVM triple; linking for a non-host triple fails with a TurfLang diagnostic ("cross-linking is not supported in 2.0; the runtime is built for <host>") instead of a host-linker error. Full cross-linking needs per-target runtime archives — FUTURE F-20. |
| Install layout | `turf-<ver>-<platform>/{bin/turf, lib/libturfrt.a, lib/std/**, share/turf/examples/**, LICENSE, README.md}`; `TURF_HOME` optional (binary-relative discovery already works); `lib/usr` is retired in favour of LEC-001's cache. |
| Channels | GitHub release archives + SHA-256 (exists); an `install.sh` that verifies the checksum; Homebrew/Scoop/AUR recipes updated only for shipped platforms (their repos are at v0.6.0 today). |

**Stages** (1) Tier-1 archive in the new layout + `install.sh` + a clean-VM
smoke (`docker run ubuntu:24.04` / `debian:stable` with only `cc` installed:
download, install, `turf new`, `turf build`, run); (2) the D20 diagnostic;
(3) optional macOS port; (4) packaging recipes.
**Tests** `scripts/release_smoke.sh` in CI on a tag build. **Risks** the
runtime's Linux-specific stack discovery (`/proc/self/maps`) inside minimal
containers (it has a fallback; FDN-005 must test the fallback path too —
**done 2026-09-29**: the fallback was freeing live objects and is fixed,
`scripts/gc_no_procfs_smoke.sh`). The known stdlib/driver blockers for
macOS and Windows (a glibc-only `malloc_usable_size` that breaks `-O0`
links, the Linux-only `sockaddr_in` layout and socket constants, no
Winsock, 32-bit `long` returns) are tabled in `FDN-005.md` *Portability
review*.
**Completion** the clean-machine walkthrough of Part IX passes on every
shipped platform.

---

### REL-004 — Conformance suite, CI, fuzzing, sanitizers

**Kind** Release engineering · **Rules** R2, R5 · **Size** M · **Prereqs** LEC-005 (`EXPECT-DIAG`), REL-002 (`turf test`)

Implements Part VII. Stages: (1) the coverage map (`tests/CONFORMANCE.md`,
spec section → test files) and gap filling; (2) runner covers `lib/std`
direct compiles, `showcase/`, `examples/` (today it covers none of them —
`AGENTS.md`'s standing trap); (3) sanitizer CI jobs (compiler under ASan/UBSan
running the full suite; runtime under ASan — FDN-005); (4) crash fuzzing:
reuse SLM-001's mutator (`scripts/slm_benchmark/mutate.py`) plus a
token-level mutator over `tests/**` as seeds, run nightly with a time budget;
any ICE, assertion, segfault or hang is filed as a release blocker with the
minimized input added to `tests/`; (5) every `-O` level, `-g`, and
`TURF_GC_STRESS=1` in the matrix (CI time budget permitting: nightly for the
expensive axes).
**Completion** Part VII's gate is green.

---

### REL-005 — Performance baseline

**Kind** Release engineering · **Rule** R5 · **Size** S · **Prereqs** none (start early)

A small benchmark set (the `structs` benchmark D24 mentions, a string-heavy
program, a `Map` churn, a GC-heavy allocation loop, a JSON round-trip,
compile time of the stdlib + showcase) with numbers recorded per milestone in
`docs/development/BENCHMARKS.md`. Not a release gate on absolute numbers;
**a gate on unexplained regressions** (> 20% on any benchmark between the
freeze tag and the release needs a recorded explanation). §24's "there is no
benchmark suite at all" risk is retired by this item.

---

### REL-006 — Documentation for 2.0

**Kind** Release engineering · **Rule** R5 · **Size** L · **Prereqs** LANGUAGE FREEZE

**Scope.**
- Drain `docs/development/handbook-sync/PENDING.md`'s `docs/reference/` rows
  (~25 today, more after Phase 1–2) by **rewriting** `docs/reference/**`
  against the frozen 2.0 compiler, as the 2026-08-21 rewrite did against
  1.0. Every code block in the reference is extracted and compiled by CI
  (`scripts/doc_examples.sh`), so the docs cannot drift again silently.
- `docs/reference/projects_and_packages.md` (LEC-001),
  `docs/reference/memory_and_safety.md`: the two heaps, what is and is not
  guaranteed (Part VIII's guarantees table), `unsafe`/capabilities/FFI
  boundaries, `mem::root`.
- **Migration guide** `docs/MIGRATING_TO_2.md` from Part V.
- README rewritten: version, platforms, install, quick start with
  `turf new`, the language tour updated (`guess`, `defer`, `&?T`, `heap`,
  `pub`, patterns, `try`).
- `docs/architecture/**` checked against the code (they are maintained
  continuously; this is the final audit).
- The handbook (`turf-handbook` repository) resumes from PENDING.md's
  handbook rows — **not** a 2.0 gate (it is a book, not the reference).
**Completion** `doc_examples.sh` green; PENDING.md has no `docs/reference/`
rows; migration guide covers every Part V row.

---

### REL-007 — Release candidates and 2.0.0

**Kind** Release engineering · **Rule** R3 · **Size** S (process) · **Prereqs** all milestones

`v2.0.0-rc.N` tags cut from `master` after the freeze; each RC runs the full
Part VII–VIII gates and the clean-machine walkthrough; bugs are triaged with
Part VI's classification; an RC with zero open release blockers for 7 days
becomes `v2.0.0`. The `refactor/architecture` branch is merged to `master`
before the first RC (it is the development branch for all of this).

---

## Part V — 2.0 compatibility: every intentional breaking change since v1.0.0

"Automated" means a mechanical rewrite is feasible (the compiler knows every
affected site); none needs a compatibility shim, and shims that would violate
a frozen decision are ruled out.

| # | Change (landed or planned) | Old (v1.0.0) | New (2.0) | Why | Migration | Automated? |
|---|---|---|---|---|---|---|
| 1 | Out-of-range literals (SEM-002) | silently clamped (`999…9` → `INT64_MAX`) | compile error E076 | F-NUM-2: no silent wrong answers | write the intended value / type | n/a (error points at it) |
| 2 | Signed `int` overflow (SEM-002) | wrapped silently | runtime panic | F-NUM-3 | use explicit arithmetic that cannot overflow; wrapping helpers are FUTURE F-24 | n/a |
| 3 | Compile-time literal overflow (SEM-002) | wrapped | compile error | F-NUM-2 | fix the constant | n/a |
| 4 | Implicit default initialization removed (SEM-004) | `mut c: Config;` was zero-filled and readable | read before assignment is E008 | F-INIT-1: one rule | `mut c: Config = Config::default();` | **yes** — E008 names the type; a `--migrate` rewrite can insert `T::default()` |
| 5 | New reserved words | — | `guess`, `defer`, `caller_location` (SEM-003, LANG-003, STD-002); planned `pub`, `try`, `_` | new features | rename identifiers | yes (rename) |
| 6 | By-value struct cycles (TYPE-001) | compiled, then crashed | E069 | INV-LAYOUT-1 | make one field `&?T` | no (design choice) |
| 7 | `mem::free` on a GC pointer (MEM-002) | freed silently (then double-free/corruption) | panic "cross-heap free" | F-MEM-3 | don't free strings/collection buffers | no |
| 8 | Raw pointers in user signatures (LANG-002) | impossible | allowed with `unsafe module;` (+ flag outside the install root) | F-STD-1 | add the pragma | no (explicit opt-in by design) |
| 9 | Deprecated `list_sort`/`list_binary_search`/`list_find` (LEC-007) | concrete `List<int>` free functions | removed; `l.sort(cmp)`, `l.binary_search(x, cmp)`, `l.find(pred)` | STD-003 kept them one cycle | call the methods | yes |
| 10 | Raw-pointer indexing gated (OVH-003) | `l.data[i]` from safe code, unchecked | compile error outside `unsafe`/capability | F-PTR-1 | `l.get(i)` / `l.set(i, v)` / `l[i]` | yes |
| 11 | `int32` literals and `++` (OVH-005) | `mut n: int32 = 5;` rejected; `c++` crashed the compiler | literal accepted if representable; `++`/`--`/`op=` on `int32` a clear error | F-NUM-1 | `c = (c + 1) to int32;` | yes |
| 12 | Module-scoped type identity (FDN-003) | one global struct namespace | per-module; `typeof()` of an imported type is qualified | packages, privacy | adjust `typeof` expectations | yes |
| 13 | Escape rules (FDN-002) | `return &local;` compiled (dangling) | compile error | soundness | `return heap(local);` | partially (the diagnostic suggests it) |
| 14 | `s[i]` (FDN-004, DECISION) | `int`, unchecked | `char`, bounds-checked | D17, soundness | `s[i] to int` where an integer is needed | **yes** |
| 15 | Visibility (LEC-002) | everything public | private unless `pub` | D13 | add `pub` to the API | **yes** (`--migrate=pub`) |
| 16 | Module resolution (LEC-001) | local module silently shadowed by stdlib | ambiguity error | no silent shadowing | rename the local module | no (naming choice) |
| 17 | Operators (LANG-001) | 10 overloadable; compound assignment ignored overloads | full set; `a op= b` uses `operator op` | F-OP-2 | none (additive) — but a type that defined `+` now gets `+=` | n/a |
| 18 | Runtime (RT-001) | binaries linked only libc/libm | every binary links `libturfrt` | F-MEM-4 | install layout includes `lib/libturfrt.a` | n/a (toolchain) |

The v1-style implicit behaviours that were **not** kept as shims, on purpose:
wrapping arithmetic (a flag would violate F-OPT-1's spirit — §25 item 8
requires numbers first; F-24 holds the measurement), eager default
initialization (F-INIT-1), path-based stdlib privilege (F-STD-1).

---

## Part VI — The 2.0 language freeze

**When.** After FDN-001, FDN-002, FDN-003, FDN-004, LEC-001 … LEC-007 have
landed and their change records are complete: tag `v2.0.0-freeze` and add a
*"2.0 frozen language"* section to `docs/development/FREEZE.md` listing every
construct and its semantics by reference to the (then rewritten) reference
docs.

**After the freeze, without an explicit release review recorded in a change
record, nobody adds or changes:** syntax; semantics of any existing
construct; implicit conversions; inference rules; runtime semantics (GC
behaviour, panic behaviour, exit codes); standard-library `pub` API or
documented behaviour.

**Every post-freeze finding is classified as exactly one of:**

| Class | Meaning | Action |
|---|---|---|
| **bug** | the compiler/runtime/stdlib disagrees with the frozen spec | fix; regression test |
| **documentation issue** | the spec text is wrong or unclear, the implementation is right | fix the docs |
| **release blocker** | a bug in a gate of Parts VII–VIII (crash on valid input, unsoundness, wrong answer, failing clean-machine install) | fix before any RC is promoted |
| **post-2.0 feature** | anything additive | `FUTURE.md`, never into 2.0 |

A "bug" whose only fix changes the frozen semantics is escalated to the owner
as a release decision, not fixed silently.

---

## Part VII — Conformance and release test suite

**Structure.** `tests/` stays the regression suite (topic folders). REL-004
adds `tests/CONFORMANCE.md`, mapping every spec area below to the test files
that cover it, positive **and** negative, and CI fails if an area has no
negative test.

| Area | Must cover (positive + negative) |
|---|---|
| Lexer/parser | every literal form (`35_`), `_` identifiers, `>>` in generics, error recovery (no cascade) |
| Type system | implicit/explicit conversions table, `bool`/`char` isolation, cast legality |
| Generics & monomorphization | instantiation per tuple, >8-byte `T`, nested generics, name hygiene (OVH-006), depth cap with real chain (LEC-005), instantiation-site errors |
| Numeric semantics | representability for `int`/`int32`/`double`, compile-time and runtime overflow, `INT64_MIN`, division traps |
| Initialization | definite assignment for every type, arrays, `T::default()` |
| Operators | every overloadable operator, generic operators (LEC-006), exact-match, never-overloadable list |
| References & pointers | `&T`, `&?T` narrowing, raw pointers gated (OVH-003), escape rules (FDN-002), `heap()` (FDN-001) |
| Error handling | `!T`, three `recover` forms, payloads, `try`, pass-through return, divergence rule |
| Pattern matching | every pattern form, guards, exhaustiveness, unreachable arms |
| Visibility & modules | `pub` on every declaration kind, private-in-public, UFCS filtering, module identity, collisions, packages (script) |
| Runtime | panic format/exit codes, stack overflow, OOM, bootstrap, ABI stamp |
| GC | retention, interior pointers, large descriptors, manual-heap roots, error payloads, `TURF_GC_STRESS=1` over the whole suite |
| Manual memory | `mem::alloc/free`, cross-heap free panic, `Pool` |
| `defer` | ordering, every exit path incl. `try`, not on panic |
| FFI | `extern fn`, `extern from`, callbacks, `int32`, `string→ptr`, capability rules |
| Standard library | every module, API lock test (LEC-007) |
| Diagnostics | `EXPECT-DIAG` code checks for every `fail_` test; `--explain` for every code |
| Optimizer | whole suite at `-O0/-O1/-O2/-O3/-Os` identical (INV-OPT-1); proven check elision |
| Debug info | `debuginfo_smoke.sh`; `llvm-dwarfdump --verify` on every showcase |
| Targets | D20 diagnostic; `--target` object emission |
| Packages & distribution | `project_smoke.sh`, `release_smoke.sh`, clean-machine walkthrough |

**Release blockers (automatic):** any compiler crash, assertion, ICE or hang
on a valid program, or on any program the fuzzer produces (invalid input must
produce diagnostics, not crashes); any GC stress failure; any `-O`-level
output difference; any sanitizer report in the compiler or runtime.

---

## Part VIII — Runtime release gate

2.0 does not ship because the compiler builds programs. It ships when each
question has a written, tested answer (FDN-006, REL-001, REL-003 own them):

| Question | 2.0 answer | Where tested |
|---|---|---|
| Where does `libturfrt` come from? | Built by the same CMake project; shipped in the release archive as `lib/libturfrt.a`; found via `TURF_HOME` or relative to the binary | `release_smoke.sh` |
| How is it versioned? | Lock-step with the compiler; `turf --version` prints both; runtime exports `turf_rt_abi_2_0` | `cli_smoke.sh` |
| How is it linked? | Statically, always (`-L<root>/lib -lturfrt`), so produced binaries need no TurfLang files at run time | `release_smoke.sh` (run a binary after deleting the install) |
| Supported platforms? | Tier 1 Linux x86-64 glibc; Tier 2 per REL-003's decision | CI matrix |
| Runtime missing? | Driver diagnostic before linking, naming the searched path | `runtime_smoke.sh` |
| OOM? | `panic: out of memory` via `turf_rt_panic`, exit 134 (GC heap); `mem::alloc` failure panics via `sys::panic` | `runtime_smoke.sh` |
| Panic? | `panic: <kind>: <msg> (<file>:<line>)` to stderr, exit 134, no unwinding, no `defer` | existing + FDN-006 |
| Stack overflow? | `panic: stack overflow`, exit 134 | FDN-006 |
| GC descriptors? | Emitted by the compiler once per type in `EmitAllTurfTypeDescs()`, variable-length bitmap (OVH-001), referenced from every allocation site, consumed by the mark phase | `runtime_smoke.sh`, `41_gc` |
| Compiler/runtime ABI? | The `turfrt.h` entry points listed in `include/Intrinsics.h` + `TurfTypeDesc`/`TurfErrorValue` layouts; internal, lock-step, never a user promise | `check_boundary.sh` |
| Cross-compilation? | Objects yes, linking no, with a diagnostic (D20) | REL-003 test |
| Runtime compatibility enforcement? | Link-time via the ABI symbol | `runtime_smoke.sh` |

**Memory-safety statement 2.0 makes** (documented in REL-006, each clause
backed by a test area in Part VII): in code with no `unsafe { }` block and in
a module without `unsafe module;`, a program cannot read or write outside an
object (bounds checks on arrays, strings and collections; raw-pointer
operations gated), cannot use a reference to a dead stack frame *in the cases
FDN-002 classifies* (the residual `Unknown`-provenance gap is documented),
cannot use a GC object after it is collected (precise heap tracing +
conservative stack scanning + `mem::root` for manual-heap references),
cannot free GC memory manually, and cannot dereference a null `&?T` without a
check. Integer overflow and division by zero panic. `unsafe`, capability
modules and FFI are outside these guarantees by definition.

---

## Part IX — D7 answered: the external developer's path

```sh
# 1. Obtain (Tier 1: Linux x86-64)
curl -fsSL https://github.com/turf-lang/turf-lang/releases/download/v2.0.0/install.sh | sh
turf --version

# 2. Start a project
turf new geometry --lib         # library package: src/geometry.tr
turf new plotter                # binary package: src/main.tr
cd plotter

# 3. Depend on something
#    turf.toml
#    [dependencies]
#    geometry = { path = "../geometry" }
#    jsonx    = { git = "https://github.com/someone/jsonx", tag = "v1.2.0" }

# 4. Import and build
#    src/main.tr:   import geometry;   import jsonx;   import io;
turf build                      # fetches, writes turf.lock, compiles everything from source
turf run
turf test                       # tests/*.tr

# 5. Distribute
git tag v0.1.0 && git push --tags     # a library: consumers pin the tag
cp build/plotter /somewhere           # a program: one static-runtime executable
```

What happens underneath: the manifest names each dependency's source; the
driver fetches git sources into `~/.cache/turf/src/`, pins commits and
content hashes in `turf.lock`, resolves each `import` to exactly one of
{own module, dependency root module, stdlib module} or reports an ambiguity,
and compiles the whole program — every module of every package — in one
invocation, monomorphizing generics across package boundaries exactly as it
does within one file today. No binary artifacts, registry, or ABI
compatibility are involved, by design (Part IV LEC-001).

---

## Part X — 2.0.0 Definition of Done

TurfLang 2.0.0 ships only when every line is demonstrated:

1. The Architecture Overhaul is closed (`OVH-CLOSE` record; its gate passed).
2. Every OVH defect is fixed and has a regression test.
3. Every 2.0 row of Part II is complete (D7-scoped, D13, D14, D15, D17
   decision, D20 decision, D23, D25, N1–N10).
4. SMP-001 and SMP-002 are complete; no other Simplification phase is a
   precondition.
5. The language freeze (Part VI) is in effect, recorded in `FREEZE.md`.
6. The tool API (SMP-001) and the `pub` stdlib surface (LEC-007) are
   documented as stable for 2.x.
7. The runtime gate (Part VIII) is green; runtime distribution is solved for
   every shipped platform.
8. The standard library is coherent, `pub`-reviewed, API-locked, and tested.
9. Diagnostics are reliable: LEC-005 complete; every `fail_` test checks its
   code.
10. FFI boundaries and capabilities are documented (`memory_and_safety.md`,
    `ffi_and_unsafe.md`).
11. Memory behaviour is defined: the Part VIII safety statement is published
    and each clause is tested; FDN-005's stress mode is green.
12. `-O0 -g` debugging works on every Tier-1/Tier-2 platform.
13. Optimization is correct at every level (INV-OPT-1 over the whole suite,
    incl. GC stress) and configurable (`-O*`, `--target`, `-mcpu`).
14. Supported targets are listed with their tier.
15. The clean-machine build/install/new/build/run/test walkthrough works on
    every shipped platform.
16. Every `showcase/` and `examples/` program builds warning-free and runs.
17. `docs/reference/**`, `docs/architecture/**` and README match the
    implementation; reference code blocks compile in CI.
18. `docs/MIGRATING_TO_2.md` covers every Part V row.
19. Part VII's release suite passes; nightly fuzzing has run ≥ 7 days with no
    open crash.
20. No known valid program crashes the compiler.
21. No known release-blocking runtime correctness issue remains (GC,
    escape, bounds, overflow).
22. `docs/planning/FUTURE.md` holds every intentionally deferred item with its
    rationale.
23. An external developer can go from download to a multi-file project with a
    dependency using only the README and the reference — no knowledge of the
    compiler's internals required.

---

## Part XI — Decision register

| ID | Decision | Recommendation | Blocks |
|---|---|---|---|
| DEC-1 | Reclassify Overhaul DoD 8 (dynamic linked structures) into FDN-001/002 | Yes (audit §5) | Overhaul closure |
| DEC-2 | Platforms for the next release (OVH-013) and for 2.0 (REL-003) | Linux x86-64 Tier 1; macOS arm64 Tier 2 only if ported; Windows FUTURE | OVH-013, REL-003 |
| DEC-3 | FREEZE narrowings (OVH-014) | Record rows 2–3; implement row 1; row 4 → LEC-005 | OVH-014 |
| DEC-4 | Suppress `lib/std`-origin warnings by default | Yes, with `--stdlib-warnings` | OVH-012 / LEC-005 |
| DEC-5 | Heap allocation surface (FDN-001) | `heap(expr): &T` intrinsic — **decided 2026-09-27** (`FREEZE.md` §6.12) | FDN-001 |
| DEC-6 | `s[i]` becomes `char` (D17) | Yes, with bounds checks | FDN-004 |
| DEC-7 | Visibility default and keyword (LEC-002) | Private by default, `pub`/`public` — **decided 2026-09-29** (`FREEZE.md` §6.15) | LEC-002 |
| DEC-8 | Project tool ownership (LEC-001) | Compiler driver subcommands; `turf-cli` not shipped | LEC-001 |
| DEC-9 | Package/module identity and collision rule (LEC-001) | Package root module = package name; collisions are errors | LEC-001 |
| DEC-10 | Pattern syntax (LEC-003) | Top-level `matches expr` unchanged; destructuring forms bind | LEC-003 |
| DEC-11 | Propagation keyword (LEC-004) | Prefix `try` | LEC-004 |
| DEC-12 | Payload + message coexistence (LEC-004) | N fields or N+1 with trailing message | LEC-004 |
| DEC-13 | Main-file module name for qualified type identity (FDN-003) | `main` — **decided 2026-09-27** (`FREEZE.md` §6.11; never spelled: a main-file type's identity is its bare name) | FDN-003 |

---

## Part XII — After 2.0

2.0.0 is not the end of TurfLang; it is the point at which TurfLang becomes a
public, stable language project. From the day it ships:

- `docs/planning/FUTURE.md` is the backlog. Its first section is the
  deliberately excluded 2.0 candidates (closures, collection iteration,
  slices, expression-position type arguments, enum payloads, Windows,
  cross-linking, registry, separate compilation), each with the rationale
  recorded when it was excluded.
- The post-2.0 Simplification phases (F-S0 … F-S5) become a tracked
  initiative with their own freeze entries and change records.
- `ARCHITECTURE_OVERHAUL.md` and this roadmap become historical records; new
  work gets a new planning document per initiative, linked from `FUTURE.md`.
- 2.x releases may add features (additive only, per Part VI) and fix bugs;
  anything breaking waits for a 3.0 plan.

