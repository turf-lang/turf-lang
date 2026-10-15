# TurfLang 2.x → 3.0.0 — Proposed Roadmap

> **Status: PROPOSAL, not a decision.** Written 2026-10-15 on
> `release/post-2.0.0` (`VERSION` = `2.0.1-dev`). Nothing here is frozen until
> it has a change record and, where it touches a frozen surface, a `FREEZE.md`
> entry. Sources: `docs/planning/FUTURE.md` (the backlog),
> `docs/planning/ARCHITECTURE_SIMPLIFICATION.md` (F-S0 … F-S5), Part VI of
> `docs/development/ROADMAP_v2.0.0.md` (the freeze), `FREEZE.md` §6.5 / §6.16 /
> §6.20 / §6.25 / §6.27, `INVARIANTS.md`, `docs/development/changes/SLM-001.md`
> with the code under `src/slm/` and `scripts/slm/`, and two draft proposals
> from the Turf Design Proposals (TDP) series, **TDP-001** (fixes in
> diagnostics without `--smart`) and **TDP-002** (checked arithmetic and
> auto-vectorization), both created 2026-10-15 and carried here as DX-1 and
> NM-1 … NM-6. Items that exist in `FUTURE.md` keep their `F-` ids; new ones
> introduced here have `V-`, `AN-`, `RT-`, `CC-`, `DX-`, `SL-`, `NM-` and `B-`
> ids.
>
> **Reading order:** Part 1 (the versioning contract: the rules everything
> else obeys) → Part 2 (what every item is, and its slot) → Part 3
> (milestones) → Part 4 (what 3.0 is) → Part 5 (decisions needed).

---

## Part 1 — The versioning contract

### 1.1 What a number means

| Bump | Contents | Compatibility promise |
|---|---|---|
| **2.N.P** (patch) | Bug fixes, documentation, packaging. No new language surface, no new stdlib API, no new diagnostics except to fix a wrong one. | A 2.N.0 program behaves identically. |
| **2.N.0** (minor) | Additive features, new warnings, new stdlib API, performance, internal re-architecture. Anything on the list in 1.2. | Every program that compiled and ran correctly on 2.(N-1) compiles and runs with the same observable behaviour, apart from the exceptions in 1.3. |
| **3.0.0** (major) | Anything that fails the minor promise (Part 4's register). | Shipped with a `turf --migrate=…` for every mechanical break, as `--migrate=pub` was for 2.0. |

There is **no fixed number of minors** before 3.0 and no calendar. A milestone
(Part 3) is a theme; it ships as one minor, or several if it is large. Version
numbers are assigned when a minor is tagged, not in this document: milestones
are named `M1 … Mn`, and the tag is "the next minor". `3.0.0` is cut when the
breaking register (Part 4) is worth the migration cost, or earlier if a
soundness hole cannot be closed within the limits of 1.3 (a).

### 1.2 What a minor may do (verified additive)

| Change | Allowed in a minor if |
|---|---|
| New **syntax** | It cannot change the parse of any existing program. A new word is a **contextual keyword** (meaningful only in one grammatical position, so an identifier called `async` or `await` keeps working); it is never a new reserved word. Checked 2026-10-15 with `turfc --check`: a program declaring locals named `async`, `await`, `spawn`, `thread`, `yield`, `select` and `lock` compiles, while the same program with `try` (a real reserved word) is rejected with E108. Users may already be using those names. |
| New **stdlib API** | It is a new `pub` item in an existing module or a new module, recorded in `tests/46_stdlib_surface/` + `scripts/check/stdlib_surface_check.sh` (F-API-1: existing items are never removed, renamed or re-signatured before 3.0). A **new module is not added to the prelude** (§6.25). Adding a bare-callable name to one of the three bare-callable prelude modules can shadow or collide with a user name, so it needs the check in V-3. |
| New **warning** | See 1.4. |
| New **error** | **Never** for a program that compiled, except 1.3. |
| **Runtime** change | It is unobservable by a correct program: same output, same exit statuses (§6.21), same panic text class, stable object addresses (§6.5). The GC algorithm, allocator, thread-safety of internals, and speed are all changeable under this rule. |
| New **diagnostic output** (a `Fix:` line, a `--diagnostics=json` form, a changed `--smart` label) | It is additive text or a new flag. Tools should read DX-2's JSON, not the text; DX-2 lands before DX-1 changes default output. |
| **Compiler internals** | Diagnostic **codes** (`E###`, warnings included) and exit statuses are the contract; message text and diagnostic order are not (decision D-6). |
| **Driver interface** | `TurfcInterfaceMinor` bumps for a new flag or input; `TurfcInterfaceMajor` only in a major (`FREEZE.md` §6.16). `turf` and `turfc` still version independently. |

### 1.3 The only two exceptions to "never reject a program that compiled"

(a) **Soundness.** A minor may reject (or change the behaviour of) a program
only to close a hole where *safe* code corrupts memory, crashes the compiler
or silently computes a wrong answer (the R2 rule of the 2.0 roadmap).
Requirements: a change record, owner sign-off (Part VI already escalates "a bug
whose only fix changes frozen semantics"), a warning in the previous minor when
the hole can be detected conservatively, and a line in the release notes.
(b) **Programs that relied on undefined behaviour in `unsafe` code.** A newly
added check may panic where the program previously corrupted memory silently,
but only under an opt-in mode (RT-5) in a minor. Default-on is a 3.0 item (B-2).

### 1.4 The warn → deny ladder (how analysis improves without breaking)

This is the rule that lets "more analysis, more warnings, better infinite-loop
and recursion detection, more checks in `unsafe`" land in 2.x.

Facts the ladder rests on (checked 2026-10-15):
- There is no `-Werror` / `--deny-warnings` in `src/driver/main.cpp`; warnings
  never stop a compile today.
- Infinite *loops* are warnings (`InfiniteLoopWarning`,
  `SuspiciousInfiniteLoopWarning`), but infinite *recursion* is a hard error
  (`RecursionWithoutBaseError`, raised from `runFunctionFlowPasses()` when
  `Cfg.allPathsRecurse()`). Extending that detection to new cases would turn
  programs that compile today into errors, so it is a **breaking** change if it
  is an error.

The ladder:

1. **Rung 0 — new check lands as a warning in a minor.** Always. It has a
   stable diagnostic code, a hint, and a measured false-positive record: the
   change record states the result over `lib/std`, `tests/`, `showcase/` and
   `examples/` (zero un-triaged hits is the bar).
2. **Rung 1 — the warning can be silenced.** V-1 (lint levels) lands first,
   so a user is never forced to edit correct code to quiet a new warning.
3. **Rung 2 — it stays a warning for the rest of 2.x.** Promotion to an error
   is a 3.0 item (register B-1). A check that is heuristic ("argument does not
   shrink") stays a warning in every version and lives in an opt-in group.
4. **`--deny-warnings` is upgrade-unsafe by definition** unless the user pins
   the lint set (V-1's `lint-set = "2.N"`): a new warning in 2.N+1 must not
   fail a build that pinned 2.N. That is the one thing a new flag must not get
   wrong; see D-2.

### 1.5 Mechanics each minor must satisfy

1. **Compatibility corpus.** The 2.0.0 test tree is kept as an immutable
   `tests/compat/2.0/` (copy, not move) and must pass byte-identically on every
   2.x compiler. A minor that edits a test to make it pass is a 3.0 change and
   CI says so (extends `tests/46_stdlib_surface/`'s idea from API to behaviour).
2. **API lock diff** in the release notes: added `pub` items, added diagnostic
   codes, added flags, new `TurfcInterfaceMinor`.
3. **Performance floor.** REL-005's baseline (`docs/development/BENCHMARKS.md`)
   is not regressed for single-threaded programs by more than the noise band,
   even when the milestone adds threads (CC-1's single-thread fast path).
4. **Every full suite gate** the 2.0 release used: `test_runner.sh`,
   `TURF_GC_STRESS` + ASan, the Windows/macOS smokes, `release_deps_check.sh`.
5. **Release procedure:** branch `release/vX.Y.Z`, `VERSION` and `turf`'s
   `version.go`, tag `v*` (publishes via `release.yml`), `gen_recipes.sh` for
   Homebrew/Scoop/AUR, then `VERSION` → next `-dev`, as 140ba12 did.
6. **FREEZE.md entry** for every item that adds surface; **INVARIANTS.md** row
   for every new guarantee; one **change record** per item
   (`docs/development/changes/<ID>.md`).
7. **Support window.** After 3.0.0 the last 2.x minor gets bug-fix patches for a
   stated period (D-8).

---

## Part 2 — Every item, classified

**Kind:** `A` additive · `W` warning-first (ladder 1.4) · `I` internal
(unobservable, ships under 1.2's runtime/internals rows) · `D` decision needed
before it can be classified · `B` breaking candidate (register, Part 4).
Complexity uses `FUTURE.md`'s scale (XS < S < M < L < XL).

### 2.1 Versioning infrastructure (V — prerequisites for the rest)

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **V-1** | **Lint levels.** Every diagnostic code is `allow`/`warn`/`deny`; set by CLI (`--allow E###`, `--deny …`), by `turf.toml [lints]`, and a `lint-set = "2.N"` pin. **No source syntax.** | A | M | Must land before the first new warning. The manifest lives in the `turf-cli` repository. |
| **V-2** | **Deprecation.** A `@deprecated("use X")` form and a warning on use, honoured by `turf doc` / LSP. Removal only in 3.0 (B-3). | A | S | `@` is already the spelling of `@caller_location`. Contextual, no new reserved word. |
| **V-3** | **Prelude-shadow check.** A script that, for any proposed addition to a bare-callable prelude module, lists every `showcase/`, `examples/`, `tests/` and `lib/std` program that declares the same name. | A | XS | Guards 1.2's stdlib row. |
| **V-4** | **`tests/compat/2.0/` corpus and its CI job** (1.5.1). | A | S | |
| **V-5** | **Diagnostic-code registry.** One file listing every code, its kind (error/warning), the version it appeared in and its level. Generated into `docs/reference/cli_and_diagnostics.md`. | A | S | Without it the API-lock diff of 1.5.2 cannot be automated. |

### 2.2 Analysis and diagnostics (AN)

All are `W` unless stated. None may reject a program that compiled.

| ID | Item | Cx | Notes |
|---|---|---|---|
| **AN-1** | **More `unsafe` checks (compile time).** Candidates, each its own change record: a raw-pointer dereference not provably non-null (reuse `NullabilityPass`'s narrowing for `T*`); `mem::free` of a pointer not derived from `mem::alloc` in the same function (extends `AliasAnalysisPass`'s use-after-free/double-free tracking); a write through a `T*` derived from an immutable array; pointer arithmetic with no bounds evidence from `ValueRangePass`; integer-to-pointer `to` casts; an `unsafe { }` block that contains no unsafe operation; a very large `unsafe` block. | M–L | Promotion to errors is B-1. |
| **AN-2** | **Better infinite-loop detection.** Today `Cfg.hasUnbreakableLoop()` is purely structural (a back-edge cycle with no `break`/`return` and no successor outside the cycle) plus `Lint.cpp`'s `SuspiciousInfiniteLoopWarning`. Add, using liveness/def information already in the CFG: a loop whose condition variables are never written in the body; a loop whose every exit is guarded by a condition `ValueRangePass` proves false; a loop whose only exit is behind a call that cannot return. | M | Warning only, forever: loop termination is undecidable, so there is no rung 2. |
| **AN-3** | **Better infinite-recursion detection.** Today only **self**-recursion on all paths is detected (`allPathsRecurse()`) and it is a fatal error. Add, as *warnings*: (a) mutual recursion where every path through a call-graph cycle recurses (needs a call graph: `CallGraphPass` was deleted by OPT-002, so AN-3 either rebuilds one or waits for F-S4's IR; prefer rebuilding a small one over blocking); (b) a base case that `ValueRangePass` proves unreachable; (c) in an opt-in `pedantic` group, "no argument decreases along the recursive call" (a heuristic, never promoted). | M–L | (a) and (b) are *proven* cases and are the B-1 promotion candidates, making them consistent with today's self-recursion error. (c) never leaves the opt-in group. |
| **AN-4** | **Pointer nullability, full** (F-5): fields, arguments, returns of a narrowed `&?T`, across calls. | M | **Increases acceptance**: programs that needed a local cast no longer do. Additive. The pass stays sound. |
| **AN-5** | **Manual-heap leak detection** (F-6): "malloc'd and never freed" on the manual heap only. | M | |
| **AN-6** | **Opt-in `int → double` precision-loss lint** (F-22). | S | Never an error (§10.5 of the Overhaul). |
| **AN-7** | **Dead-code family:** unused private function, unused import, unused `pub` item in a binary root. Needs the module graph. | S–M | Depends on V-1. |
| **AN-8** | **Panic backtraces** (F-6b), symbolized with the DWARF `-g` already emits. | M | Runtime change; output goes to stderr after the panic line, which §6.21 treats as non-contract. |
| **AN-9** | **Error return traces** (F-15, first half): record `@caller_location` of each `try`. Closed error sets in signatures (second half) are `A` only if optional. | M | The open union `!T` is the frozen design and does not change. |

### 2.3 Runtime and GC (RT)

`FREEZE.md` §6.5 freezes: non-moving, mark-sweep, precise on the heap,
conservative on stack and registers, stop-the-world, single-threaded. Its
non-moving rule is justified by FFI (`write(2)` and `send(2)` get stable
buffers) and is **not** reopened here. §E also rejects a moving or generational
collector and "write barriers in preparation".

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **RT-1** | **GC tier 1: better algorithms inside §6.5.** Size-segregated free lists and bump allocation; lazy/incremental *sweep*; side mark-bit tables; adaptive heap-growth policy; a faster interior-pointer block lookup; **parallel marking** by N worker threads *while the world is stopped* (still stop-the-world; only the collector's own threads are new, which §6.5's "single-threaded" does not need to mean). | I | L | Unobservable. Gated by `TURF_GC_STRESS` + `TURF_GC_VALIDATE` + the benchmark floor. |
| **RT-2** | **GC tier 2: generational or concurrent collection, no language change.** Dirty-page tracking from the OS instead of compiler-emitted write barriers (candidate mechanisms, none prototyped: Linux soft-dirty bits or `mprotect`, `GetWriteWatch` on Windows, Mach VM protection on macOS). It works for stores made by C code and by raw `T*` writes, which a compiler barrier cannot see. | I (if OS tracking) / D | XL | Reverses nothing in §6.5 if the collector stays non-moving and the mutator is unaware. Needs the owner to confirm that §E's "no generational collector" is about a *moving* one (D-4). |
| **RT-2′** | Same goal with **compiler-emitted write barriers** on pointer-typed field stores. | **B** | XL | A store made by `unsafe` code or by C into a GC object skips the barrier, so a pointer hidden from the collector is freed live. That makes the FFI contract stricter (B-4). Only chosen if RT-2 is measured not to work. |
| **RT-3** | **Per-thread allocation buffers and lock-free fast path.** | I | M | Prerequisite of CC-1; also lowers single-thread cost. |
| **RT-4** | **Checked arithmetic vs vectorization** (F-24). Specified as NM-1 … NM-6 in 2.9 (draft TDP-002). The default stays checked. | I / A | M | `INV-OPT-2` (elision only when proven) is not weakened. |
| **RT-5** | **Opt-in runtime check mode** (`turf build --debug-checks`, building on the existing `TURF_RT_SANITIZE`): red zones around `mem::alloc` blocks, poison on `mem::free`, double-free/invalid-free, `mem::copy`/`mem::set` bounds when the block size is known, uninitialised-read of a manual-heap block. Panics with a named diagnostic, exit 134. | A | M–L | Default off in 2.x. Default on at `-O0` is B-2. |
| **RT-6** | **Stack-overflow headroom and depth reporting.** A recursion-depth hint in the `panic: stack overflow` text. | I | S | |

### 2.4 Language features (from `FUTURE.md` §A)

| ID | Item | Kind | Cx | Depends on |
|---|---|---|---|---|
| **F-1** | **Closures/lambdas**, in the four phases of `FUTURE.md` F-1: capture-free (thin pointers; parser + lowering) → `{code, env}` + captures + type descriptors → stdlib adoption (`List.map/filter`, closure `http` router) → stack-allocated non-escaping environments. | A | L | FDN-001/002 (done); one risk, see B-7 |
| **G-1** | Array-of-function-values spelling. Decide **with** F-1 (element size changes). The *additive* fix is a parenthesised type or a type alias; changing what `fn(int): int[2]` means is B-6. | A / B | S | F-1 |
| **F-2** | **Iteration over collections**: structural protocol (`length()` + `get(i)`) lowering to an index loop; `Map` via `keys()`. Decide protocol vs `next(): !E` vs `each(f)` after F-1 exists. | A | M | F-1 (soft) |
| **F-3** | **Storable `Slice<T>`** (struct fields, returns, collections of views, views into `List<T>` buffers). | A | M | FDN-002's escape analysis; RT-1 block table |
| **F-6c** | **Expression-position type arguments.** Specified as LG-1 (`List<int>::default()`) and LG-2 (`f::<int>(…)`) in 2.10; `f<int>(…)` cannot be made unambiguous. | A | S–M | none |
| **F-14** | `compare` as an expression, range patterns, nested or-patterns. | A | M | none |
| **F-14b** | Payloads on plain `enum`s (sum types), reusing LEC-004's representation. | A | M | none |
| **F-15b** | Closed error sets in signatures, error conversion. | A | M | AN-9 |
| **F-16** | Circular imports. A module-system change, but it only *accepts* programs that are rejected today. | A | M | none |
| **F-11d** | In-source `@test` functions. | A | S | `turf test` (§6.22) |

### 2.5 Concurrency and async (CC)

`F-8` is XL because nothing is designed for it and because it presses on three
frozen things at once: the collector (§6.5), **safe code cannot corrupt
memory** (the R2 guarantee 2.0 ships on), and **collections are shared, copied
on demand** (§6.27). Threads that share a `List<T>` without a rule would break
the second guarantee in safe code. The traits/constraints system that would
express `Send`/`Sync` is rejected (§E), so the rule has to be something else.
This proposal therefore writes down **constraints and gates**, and leaves the
design to a dedicated plan, `docs/planning/CONCURRENCY_PLAN.md`, created at CC-0.

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **CC-0** | **Concurrency design plan** with an owner decision on the sharing model (D-3). Recommended default: **share nothing by default.** `spawn` takes a function value and a *transferable* argument (a compiler-known structural predicate: primitives, strings, value structs/arrays of transferable parts; a collection or `&T` is deep-copied or rejected); shared state only through stdlib `Mutex<T>`/atomics/channels whose unsafe core is inside `lib/std`. | D | M | Written before any implementation. |
| **CC-1** | **Thread-aware runtime**, no user-visible threads yet: per-thread stack bounds and register capture (extends INV-RT-4 to N threads); stop-the-world handshake (safepoints or signal-based); allocation under RT-3; `panic` and signal handlers thread-safe (INV-RT-5); the **single-thread fast path**: locks and handshakes are compiled out until the first thread starts. | I | XL | RT-1, RT-3, F-S0/S1/S2 (reentrant compiler pipeline is separate, but the same global-state audit applies to `libturfrt`) |
| **CC-2** | `thread` stdlib module: `spawn`, `join`, `sleep`, thread-local storage. Uses F-1 for the entry function; before F-1, a named function plus a transferable argument. | A | L | CC-0, CC-1 |
| **CC-3** | `sync` module: `Mutex<T>`, `RwLock<T>`, atomics (a closed intrinsic list, like the GC's), channels. | A | L | CC-2 |
| **CC-4** | **Async.** Two shapes: (a) a *library* of stackful tasks on the CC-1 runtime, so no new syntax; (b) stackless `async fn` / `await` as **contextual keywords** (1.2). Recommended: (a) first, (b) only if a measured need appears. | A | XL | CC-3, F-1, F-2 |
| **CC-5** | Non-blocking I/O and an event loop (F-12, first half). | A | L | CC-4 |
| **CC-6** | TLS/HTTPS by binding a vetted C library through FFI (F-12, second half); UDP. | A | L | none hard; CC-5 for async use |

### 2.6 Architecture (F-S, from `ARCHITECTURE_SIMPLIFICATION.md`)

Invisible to users by 1.2's internals row: diagnostic codes, exit statuses,
emitted code behaviour and the benchmark floor are the proof; message text and
order may shift (D-6). Each phase needs the explicit decision the Simplification
document calls for.

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **F-S0** | `LexerState`: the 18 lexer globals become an owned struct. | I | S–M | Concurrent tokenization, F-11b. |
| **F-S1** | `ParserState`. | I | M | |
| **F-S2** | Remove the `GlobalSymbolTable` singleton; delete `CompileBuffer.cpp`'s and `Sandbox.cpp`'s save/restore dances. | I | M–L | Needed by CC-1's global-state audit, F-7a, and a concurrent LSP. |
| **F-S3** | A real `SemanticModel` separate from AST mutation; folds in D18 (two type-inference engines). Decision first: mutate-in-place vs immutable trees (Simplification §9). | I | L–XL | The generic-substitution split-brain (§1 of that document) is resolved as a side effect; phase 0★ (`isOrWrapsGenericParam` reuse) is a patch-level fix and can ship in 2.0.x if reproduced. |
| **F-S4** | Promote the CFG to a lowering IR ("TIR"); Codegen emits from it; folds in D19 (the double-parse pre-pass) and INV-CG-1. Staged as F-S4a … F-S4e below. | I | L–XL | Gives AN-3(a) a call graph and a home for NM-2's check elision, F-5's interprocedural nullability and F-1 phase 4's stack environments. |
| **F-S5** | Dependency layering: frontend library independent of Codegen/LLVM. | I | M | `turf-fmt`/`turf-lsp` link only what they need. |

#### F-S4 in detail: from a CFG of AST pointers to a lowered IR

**Today** (read from `src/analysis/CFG.h`, `Analysis.cpp`, `CodegenExpr.cpp`,
`docs/architecture/compiler_pipeline.md` §6):

- A `CFG` is built **per function, inside `FuncDefExprAST::codegen()`**, so
  analysis and code generation of one function are interleaved, and analysis of
  function B cannot see facts about function A unless A happened to run first.
- A `TurfBasicBlock` holds a list of **non-owning `ExprAST *`** at statement
  granularity plus a terminator kind (`None`, `Return`, `Break`, `Continue`,
  `Branch`, `Unconditional`). Control flow *inside* an expression (`&&`, `||`,
  `try`, `matches` used as an expression) is invisible to it, and `defer` runs are
  modelled separately (`StatementsWithDeferredRuns`).
- Results travel back to Codegen by **mutating AST flags**:
  `ValueRangePass::markElidableChecks()` sets `ProvenNoOverflow` and
  `ProvenInBounds`, and Codegen reads them to emit `nsw` arithmetic or skip a
  bounds check. A second CFG is built for that pass (the Simplification
  document's evidence for the idea).
- Codegen then **re-walks the original AST**, re-deriving what each operator
  and conversion means (the pattern Simplification §4.2 calls "Codegen
  re-discovering language semantics").
- `main.cpp` runs a throwaway Resolver/TypeChecker **pre-pass** (D19) so struct
  layouts exist before Codegen needs them.

**Target:** the CFG becomes the representation that is analysed *and* lowered,
with the checks written as instructions instead of flags on syntax. It is
introduced by strangling, not by rewrite: every stage ships as an unobservable
minor and keeps `test_runner.sh` green.

| Stage | What changes | Unlocks | Gate |
|---|---|---|---|
| **F-S4a — analysis leaves Codegen** | `runFunctionFlowPasses()` (already callable without Codegen through the query facade) runs as its own whole-program phase **before** Codegen. Its results go into a side table keyed by node id (`AnalysisFacts`: proven-no-overflow, proven-in-bounds, escape provenance), not into AST flags. | `--check` without Codegen; the call graph AN-3(a) needs; interprocedural passes (F-5); deterministic diagnostic order independent of code-generation order (allowed by D-6). | Emitted LLVM is byte-identical on the whole corpus; diagnostic *codes* identical. |
| **F-S4b — one CFG, built once** | The CFG that analysis builds is the only one: OPT-002's second CFG is deleted. Expression-level control flow (`&&`, `||`, `try`, `matches`-expression, `recover`) gets real blocks. | A CFG that agrees with what runs, so a fact proven on it is true of the emitted code. | Same as F-S4a, plus the flow-pass test areas unchanged. |
| **F-S4c — TIR** | A typed, per-function IR of basic blocks of instructions: loads and stores, calls, conversions with their source and target types, operator **identity** (`IntAddChecked`, `IntAddNoOverflow`, `DoubleAdd`), `CheckIndex`, `Panic(location)`, `Alloc(descriptor)`, `DeferRun`, `Return`. Checks are **instructions**, so "elide this check" means "delete this instruction, justified by the fact table". | The place INV-OPT-2 is enforced structurally; NM-2's range proofs and NM-3's wrapping operations become instruction kinds; F-1 phase 4 (stack-allocated non-escaping closure environments) and bounds-check hoisting have somewhere to live. | A TIR verifier (typed operands, every block terminated, every check dominates its use) runs under `TURF_GC_STRESS` CI. |
| **F-S4d — Codegen from TIR, function by function** | Lowering AST (+ SemanticModel from F-S3) → TIR, and TIR → LLVM, for a growing subset (straight-line integer code first, then control flow, then calls, then `defer`/`try`). Functions outside the subset still use the AST path. | Codegen stops re-deriving semantics; struct layout becomes a property of the module (the TIR module's type table), which is how the double-parse pre-pass (D19) goes away. | **Differential test:** for every function lowered via TIR, the LLVM it emits equals what the AST path emits for it (identical text, or equal after normalization), at `-O0` and `-O2`; `-g` debug locations and `@caller_location` panic text preserved instruction by instruction. |
| **F-S4e — retire the AST path** | The AST code generator and the pre-pass are deleted; F-S5 can then split the frontend from LLVM. | Dependency layering; a compile that does not need LLVM headers up to TIR. | Full suite, fuzz, GC-stress + ASan, benchmark floor. |

Risks to name: **debug info** and **panic locations** must be carried per
instruction or `-g` and `@caller_location` regress silently; **compile time**
rises while two representations coexist; and the TIR must model `defer`, `try`
and `recover` exactly as the current CFG plus Codegen do, or an error path leaks
(INV-ERR-1). The strangler order (F-S4d) is chosen so that the differential test
catches each of these on the smallest possible subset.

**Relationship to F-S3 (decision D-20).** Simplification §4.2 keeps the
`SemanticModel` (a query interface: what is this symbol, what type is this
expression) separate from a Bound/Typed IR (the program after semantic decisions).
This proposal keeps them separate in *purpose* and recommends **not** adding a
third tree: AST + `SemanticModel` lower directly into TIR, and TIR is where operator
identity, conversions and checks become explicit, so it plays the Bound IR's role.
Revisit only if lowering code is seen re-deriving the same decision in several
places. rustc's HIR → THIR → MIR is the larger precedent; this repository is
smaller than rustc and a third representation is a third thing to keep consistent.

**Interaction with the near-term plan.** NM-2 (range-based elision) and AN-3 (a)
are written to work on today's AST-flag mechanism and migrate to F-S4a's side
table and F-S4c's instructions later; they do not wait for F-S4. F-S4a is the
cheapest first step and is sequenced early in M6.

### 2.7 Packaging, tooling, platforms, stdlib

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **F-7a** | Separate compilation (per-module caching of non-generic code). | I | L–XL | F-S2. Trigger: a real project's build time. |
| **F-7c** | Registry, semver ranges, `turf publish` (reserved in 2.0), workspaces. | A | L | `turf.lock` carries a format version so an old `turf` rejects a newer lock with a message, never silently misreads it. |
| **F-7b** | Turf-to-Turf binary ABI. | D | XL | Only for binary packages; the C ABI is the stable interop. Leave unscheduled. |
| **F-11** | LSP completion, find-references, rename, code actions (the `--smart` fixes), incremental re-analysis. | A | L | F-S2/S3 |
| **F-11b** | Real pretty-printer (trivia-preserving lexer). `turf fmt`'s verified-result guarantee (§6.23) holds. | A | M | F-S0 |
| **F-11c** | Documentation generator over `pub` items and doc comments. | A | M | V-2 |
| **F-19** | Standard library expansion: richer `os`, `time` formatting, regex, CSV, compression, crypto hashes, a process API. Each through LEC-007's conventions and the API lock. | A | varies | V-3 |
| **F-20** | Cross-linking; other platforms (Linux/Windows arm64, others need their own register capture, INV-RT-4). | A | L | |
| **F-9** | SLM: `@slm { }` blocks, LoRA adapters, corpus growth. F-DIAG-1/2/3 keep their gates. | A | L | F-S2/S3 |
| **F-10** | Self-hosting. | A | XL | F-1, F-7a, a stable 2.x |
| **F-25** | Speculative paradigms (`DESIGN_NOTES.md` §7–8). | D | — | Revisit later; §25 rejects user-facing `comptime`/macros. |

### 2.8 Diagnostics and the SLM (DX, SL)

**What exists today** (read from `src/slm/`, `scripts/slm/`, and
`docs/development/changes/SLM-001.md`; the typo case below was run on
2026-10-15):

- `--smart` runs a pipeline: **rule-based rename fix** (`tryDeterministicFix()`,
  11 error classes, renames only) → on-disk cache → **grammar-constrained
  model** → parse and splice-site resolution → **sandbox** (Lex/Parse/Resolve/
  TypeCheck/Lint recheck; never Codegen) → confidence → display gate (0.8).
- The rule layer already works **with no model**: `turfc --smart --check` on a
  one-typo file prints `[Deterministic Fix] 'nmae' -> 'name' validated -- the
  model was never called.` in about 0.03 s. It still needs the `--smart` flag,
  and it labels the result **`[AI Suggestion]  Compiler fix:`** even though no
  model ran.
- A fix is **shown, never applied**: the source file is unchanged after the run.
- The "score" is effectively **binary**. `finalizeConfidence()` returns
  `max(0.9, prior)` for a sandbox-validated fix and `min(prior, 0.5)` otherwise;
  the structural prior (`computeStructuralPrior()`, bonuses summing to 1.1) only
  matters for logging and ranking, as its own comment says.
- The benchmark (`scripts/slm/slm_benchmark/`) has **22 cases** over 14 mutation
  kinds and 15 error codes (`FUTURE.md` F-9 still says 6). It needs the real
  model, runs by hand, takes tens of seconds per case, classifies outcomes by
  **reading the compiler's stdout**, and reports: deterministic-fix rate,
  sandbox-validation rate, semantic-correctness rate (the mutated test's own
  `test::assert`s, run after the fix, are the oracle), no-suggestion rate,
  false-confidence rate, p50/p95 latency, cache hit rate.
- Model-free tests already exist: `scripts/slm/slm_replay/` (recorded model
  answers fed through parse → splice → sandbox → retry via `TURF_SLM_REPLAY`,
  works in a `--no-smart` build), `slm_deterministic_fix_smoke.sh`,
  `slm_cache_smoke.sh`, and two CI checks on the prompt documents.

#### Diagnostics

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **DX-1** | **Fixes in diagnostics without `--smart`** (draft **TDP-001**). A `Fix:` line with the corrected line, printed in every build with no flag and no model, only when the compiler computed exactly one edit **and** a re-check shows the original error gone and no new one. An error class declares its fix beside its hint; nothing matches an error's name or text. `--smart` keeps its place: rule fix first, model only when there is none. | A | M–L | See "Findings on TDP-001" below. |
| **DX-2** | **Machine-readable diagnostics** (`--diagnostics=json`, SARIF-shaped): code, severity, spans, hint, related locations, and the DX-1 fix as a list of `{span, replacement}`. TDP-001 lists this as an open question. | A | M | `TurfcInterfaceMinor` bump. Gives `turf-lsp` quick fixes (F-11), the benchmark (SL-2) and tools a contract other than scraping text, which D-6 puts outside the compatibility promise. Land **before** DX-1's text appears in default output. |
| **DX-3** | **Hint backfill.** TDP-001 counts 20 codes whose hint is only general advice and 5 with none (E105, E106, E115, E116, E118). A fix builds on a concrete hint, so these come first. | A | S–M | The count is the TDP's; recompute it with the checked-in script DX-1 requires. |
| **DX-4** | **`--fix` apply mode** (later proposal). Applies only validated edits, atomically, on explicit request, with the same verified-result guarantee `turf fmt` has (§6.23). | A | M | Not before DX-1 and DX-2. |

#### The SLM: scoring and checks on the model itself

The aim is a model-assisted path whose answers are *measured* trustworthy,
cheaper where a rule suffices, and honest about what produced them. Every SL
item preserves F-DIAG-1 (sandbox verification is never removed) and F-DIAG-2
(an unvalidated suggestion never clears the display gate), and respects F-DIAG-3
(no new `SLM::ErrorCategory` values without an explicit request).

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **SL-0** | **Provenance.** Every suggestion carries `rule | model | cache`; output says `Compiler fix` for a rule, `Model suggestion` for a model, never `[AI Suggestion]` for a rule. | A | XS | Prerequisite for SL-1 and DX-1. The label is output text (D-6). |
| **SL-1** | **Fix Score v2: a decomposed score replaces the binary one.** See below. | A | M | |
| **SL-2** | **Benchmark scoring: a utility score with a confidence interval**, per-code and per-category tables, and a rules-only baseline. See below. | A | M | |
| **SL-3** | **Corpus growth and a held-out split.** From 22 to several hundred cases. See below. | A | M | F-9's "corpus growth" item. |
| **SL-4** | **Checks on the model itself.** Qualification, drift, determinism, resource bounds. See below. | A | M–L | |
| **SL-5** | **Checks on what a suggestion does** (beyond "it compiles"): capability diff, deletion detection, API-surface stability, warning delta. See below. | A | M | Feeds SL-1. |
| **SL-6** | **Richer prompt context.** `known_types` covers one call site today (SLM-001 *Known pitfalls*: variable-initializer mismatches and operator mismatches have none). Add: the declaration of each struct a diagnostic names, the signature of a called function, the enclosing function's parameter types. | A | M | Easier after F-S3 (a queryable `SemanticModel`). |
| **SL-7** | **Coverage of the rule layer.** Today 11 rename classes. Add the classes DX-1 identifies (E060 make `mut`, E102 insert `;`, arity, missing return type …), each re-derived (not assumed) the way SLM-001 excluded `UseBeforeDeclarationError`. | A | M | Shared with DX-1: one rule engine, two surfaces. |
| **SL-8** | **`@slm { }` blocks** (F-9) are gated on SL-1 and SL-5: generated source may enter a program only at tier `proven` or `verified`. | A | L | F-DIAG-1/2 unchanged. |

**SL-1 — Fix Score v2.** Today a sandbox pass is worth 0.9 whatever the edit
did. A fix that *deletes* the failing statement also passes the sandbox. The
score becomes a set of facts the compiler can compute deterministically after
the sandbox run, shown to the user as a short checklist and reduced to a tier:

| Fact | How it is computed | Effect |
|---|---|---|
| **Valid** | the original diagnostic is gone and no new error (today's rule) | Hard gate. Failing scores 0 and is never shown. |
| **Warning delta** | multiset of diagnostic codes after minus before (the sandbox already runs Lint) | A new warning lowers the tier. |
| **Minimality** | token-level edit distance and lines touched, normalized by the error span | A wide edit lowers the tier. |
| **Semantic loss** | tokens removed that are calls, `assert`s, `return`s or assignments, counted on the token stream (the file may not parse before the fix) | A net loss caps the tier at `plausible`. Catches "fix by deletion". |
| **Surface stability** | symbol table before and after: function signatures, struct layouts, `pub` items outside the edited span | A change caps the tier at `plausible`. |
| **Capability diff** | SL-5 | A new capability forces `plausible` with a stated reason. |
| **Ambiguity** | number of distinct validated candidates (rule engine), or agreement across k extra samples (model, opt-in `--smart=thorough`) | Disagreement lowers the tier. |
| **Provenance** | rule, model, cache-of-model | A rule that is unique and valid is `proven`. A model prior is the category's measured precision from SL-2, not a constant. |

Tiers: **`proven`** (a unique rule fix, valid, no flags), **`verified`** (a
model fix, valid, every other fact clean), **`plausible`** (valid but flagged;
shown with the flag spelled out), **rejected** (never shown). The display gate
becomes a tier policy, not a 0.8 threshold. Weights, if any, are fitted on the
benchmark (SL-3's training split), frozen in code, and changing them requires
rerunning the benchmark and recording the result. **Calibration is reported**:
for each tier, the semantic-correct rate on the held-out split. If `verified`
is not measurably more reliable than `plausible`, the tiers are wrong.

**SL-2 — Benchmark scoring.** The current table is eight numbers with no single
verdict and no way to say whether a change helped. Add:

- **Utility per case:** `+1` a shown fix that is semantically correct, `0`
  nothing shown, `−K` a shown fix that is wrong or does not even recompile.
  `K = 3` is the proposed default (D-12): a wrong fix is worse than none because
  the user trusts it. Mean utility is the headline, with a **bootstrap 95%
  confidence interval**, because a few hundred cases do not support a bare
  percentage. A change is an improvement only if the intervals separate.
- **Rules-only baseline:** the same corpus with the model disabled. What the
  model adds beyond DX-1/SL-7 is `utility(model) − utility(rules)`; if that is
  not positive after latency, the 1 GB download is not earning its place.
- **Calibration:** reliability by tier and a Brier score for the old confidence
  (so the change to SL-1 is measured, not asserted).
- **Abstention quality:** on the hard negatives of SL-3 (cases where nothing
  should be suggested), the rate of correct silence.
- **Cost:** p50/p95 latency and peak RSS, per path (rule, cache, model).
- **Machine-read results:** classify outcomes from DX-2's JSON, not stdout text;
  write one JSON record per run under `bench/results/`-style storage so a trend
  across commits is visible.

**SL-3 — Corpus.** 22 cases over-fit quickly, and the prompt documents in
`docs/slm/*.txt` are tuned against the corpus. Proposals:
generate cases mechanically from the passing tests with `mutate.py`'s mutators,
one per error code the suite exercises (the suite has 771 `fail_*.tr` files);
split **train / held-out** by *source test file*, so prompt and weight tuning
never sees the held-out set; add **hard negatives** (use-before-declaration, two
equally good renames, a diagnostic that needs a design decision); add
multi-error files and programs from `examples/` and `showcase/`. The semantic
oracle stays "the original asserts still hold".

**SL-4 — Checks on the model.** None of these add a network dependency.

| Check | What it does |
|---|---|
| **Qualification** | `scripts/slm/qualify_model.sh <file.gguf>` runs the benchmark and a fixed smoke set and records, with the model's SHA-256, its utility, false-confidence rate, latency and RSS. A model meeting the published thresholds is "qualified"; `--smart` prints one note when the loaded `.gguf` is not in the qualified list (it never blocks: users choose their model). |
| **Determinism** | Fixed seed and temperature 0; a CI check that the same input produces the same output twice **with the cache bypassed** (the cache would hide non-determinism). The llama.cpp revision, the model SHA-256 and the prompt-document hash are part of the cache fingerprint (today: the compiler binary's size and mtime, plus a hash of the prompt body and documents). |
| **Drift** | A scheduled, optional CI job restores a cached qualified model, runs the benchmark, and fails on a utility drop outside the interval. The model-free replay suite runs on every push, as now. |
| **Output conformance** | Counts per run: grammar escapes, parse failures, splice failures by kind (the issue-#267 class of harness bug), empty answers. A rise is a harness regression, not a model one. |
| **Resource bounds** | A wall-clock budget for the whole `--smart` run (not just attempts), a RAM check before loading, a token cap; fail closed to the compiler's own diagnostics. |
| **Prompt injection** | Source text, comments and string literals reach the prompt. A fixed set of hostile-source test cases ("ignore the above and …") is part of the replay suite, and SL-5's capability diff is the backstop. |

**SL-5 — Checks on what a suggestion does.** The sandbox proves a fix
*compiles*. It cannot see that the fix changed the program's meaning or reach.
All are computed by the sandbox on the patched source and are conservative
(they only lower the tier, never reject on their own):

- **Capability diff:** the edit introduces `unsafe`, `extern`, a new `import`
  of `os`, `fs`, `http` or `io`, a raw pointer, or `mem::` calls that the
  original did not have.
- **Deletion:** the edit removes more code than it adds (SL-1's semantic loss).
- **API surface:** a function signature, struct layout or `pub` item outside the
  edited span changed.
- **Warning delta:** new warnings, including the AN-* family once it exists.
- **Codegen recheck (optional):** run Codegen to an in-memory module and the LLVM
  verifier, without linking or running anything. The sandbox does not reach
  Codegen today (SLM-001). **Executing** a suggested fix is deliberately not
  proposed in the compiler: only the benchmark runs patched programs.

#### Findings on TDP-001 (checked against the repository, 2026-10-15)

1. **Part of the mechanism exists.** The rule layer, its sandbox validation, the
   11 wired classes and the model-free speed are already in `--smart`. TDP-001's
   Motivation says corrected code "only appears with `--smart`, which needs the
   larger build and a model": the model is not needed for renames. What is true is
   that it needs the flag, and that only renames are covered. DX-1 is therefore
   "move the rule engine out of `src/slm/`, show it by default, extend the
   classes, relabel", not a new mechanism.
2. **No build dependency on the model runtime.** `src/slm/Sandbox.cpp` and
   `SLMIntegration.cpp` are compiled in every build (`CMakeLists.txt`); only
   `SLMEngine.cpp` is swapped for `SLMEngineDisabled.cpp` in a `--no-smart`
   build. A `--no-smart` build running the rule fix was **not** tested here:
   verify it before relying on it.
3. **The re-check is the existing sandbox**, which saves and restores global
   lexer and symbol-table state (the workaround Architecture Simplification
   §1 documents). That makes "one re-check per fix, capped per compile" (the TDP's
   own bound) the right design, and makes F-S2 the real fix for the cost.
4. **Spelling.** The explain command is `turf --explain E040` today (the TDP
   writes `turf explain`). Decide which spelling DX-1 documents.
5. **Numbers.** The repository has 771 `fail_*.tr` files, as the TDP says. A
   count of distinct `EXPECT-DIAG` codes gives 140; the TDP's "136 of 149 codes"
   uses a different method. The checked-in script the TDP asks for settles it.
6. **`EXPECT-FIX` does not exist yet;** `EXPECT-DIAG` does. E102 (missing `;`)
   and E060 (assignment to an immutable) both exist in `ErrorCodes.h`. The
   benchmark's semicolon case has no code in its baseline output, so a missing
   `;` may print without one; confirm before listing E102 as the first set.
7. **Text compatibility.** New `Fix:` lines are output text, outside the
   compatibility promise (D-6), but doc pages that quote diagnostics and any
   scraper change. DX-2 should land first.

### 2.9 Numeric speed (NM) — draft **TDP-002**, checked arithmetic and vectorization

The default does not change: integer overflow traps in every build at every
`-O` level. The measured cost (F-24: `structs`, 53% slower than `clang -O3`;
removing only the overflow branches made `-O3` vectorize and reach parity)
is attacked in two ways that weaken nothing. Other designs are rejected in the
TDP and stay rejected here: a global flag (semantics would depend on the build),
wrapping by default in optimized builds, and undefined overflow.

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **NM-1** | **Baseline benchmark first.** Check in the program behind the 53% figure beside its C twin under `bench/`, record the result with the others. | A | S | The TDP's first deliverable. `bench/structs.tr` exists but is a different program. |
| **NM-2** | **Wider proof-based elision.** Extend `ValueRangePass::markElidableChecks()` (it marks `ProvenNoOverflow` today) from constants to **ranges and loop-carried values**: counters bounded by the loop condition, values reduced by `%` or masked, sums of those. | I | M–L | Unobservable (INV-OPT-2). The TDP says plainly it will **not** help a sum of array elements, the motivating loop. Verification: the same program with elision on and off gives identical results (extend `scripts/dev/fuzz/` to compare); a mistaken range silently removes a needed check, which is the worst failure here. |
| **NM-3** | **`math::wrapping_add`, `wrapping_sub`, `wrapping_mul`** (`int` only), defined modulo 2^64, never trapping. Functions, not operators: no grammar change, visible and searchable. | A | S–M | See findings below. |
| **NM-4** | Re-measure with the trap exits restructured (one shared cold, `noreturn` trap block per function). | I | S | **Unverified idea**, not in the TDP: whether LLVM 22's early-exit vectorization could handle a loop whose only side exit is a shared noreturn call. A measurement, not a claim. If it works it removes the cost with no source change. |
| **NM-5** | Shifts and negation (`EmitCheckedShl`, `EmitCheckedIntNegate`). | D | S | Out of scope until a measurement shows the same effect. |
| **NM-6** | Batch-then-check. | D | L | Deferred by the TDP: it can change what runs before the panic. May return as its own proposal for effect-free reductions, with a precise applicability rule. |

`RT-4` in 2.3 is replaced by NM-1 … NM-6.

#### Findings on TDP-002 (checked against the repository, 2026-10-15)

1. **Locations match.** `EmitCheckedIntArith` is defined at
   `src/codegen/CodegenCore.cpp:1856` and called from `CodegenExpr.cpp` for `+`,
   `-`, `*`; `ProvenNoOverflow` is on `BinaryExprAST` and its sibling node.
2. **`math` is `lib/std/core/math.tr`;** it has no `wrapping_*` today, so NM-3
   adds names to an existing module, which F-API-1 allows in a minor (the API-lock
   test records them). A user's own `wrapping_add` is unaffected, as the TDP says.
3. **Answer to the TDP's first open question** (how the compiler learns the
   functions are special without recognising a name). `src/sema/Intrinsics.h`
   lists three kinds: language-level bare intrinsics, LLVM intrinsics, runtime
   entry points. None is "a module function the compiler lowers specially". The
   registry already works by the Resolver tagging the call node
   (`ResolvedIntrinsic`) so that no later pass compares a name and a user's own
   `heap` wins. The smallest addition that keeps Principle V is a fourth kind
   in the same file, keyed by **(module, function) of the resolved declaration**,
   so a program defining its own `wrapping_add` gets its own function. It is a
   closed-list change (F-STD-2), so it needs a change record and a `FREEZE.md`
   entry, which is the TDP's "deliberate, published change".
4. **`-O0`.** A call to a wrapping function must lower to a plain `add`/`sub`/`mul`
   with no `nsw`, even at `-O0`, never an actual call; otherwise "same result at
   every level" holds but the speed advantage is lost where people debug.
5. **`int32` forms** are left out here: the TDP says `int32` exists only for ABI
   declarations, and this was not re-verified.
6. **The Constitution (Principles I–VIII) is not in this repository** (it
   appears only in `docs/development/handbook-sync/DONE.md`). Both TDPs cite it.
   This roadmap treats it as external; decision D-14 says where TDPs live.

### 2.10 Generics at use sites and wider `guess` (LG)

**Request:** make generics `guess`-friendly (`mut a = List:int::default();` or
something more readable), and find more places where inference helps without
costing speed or memory safety.

#### What the compiler does today (each line run on 2026-10-15)

| Source | Result |
|---|---|
| `mut l = List<int>::default();` | E110: `<` after a name in an expression is always less-than. |
| `mut l = List::default();` | E129 (nothing to infer). |
| `mut b = Box::new(42);` with `fn<T> Box.new(v: T)` | **E129**, although `T` is in the parameter list and a generic *function* call would infer it. |
| `mut b: Box<int> = Box::new(42);` | Works: a generic struct takes its type arguments **from the declared type it is stored into**. |
| `identity::<int>(3)` | E108 (`::` must be followed by a name). |
| `mut f = identity;` | E137 (no function type to read). |
| `mut a = ::b;` and `b < 2 > ::b` | E103: `::` cannot begin an operand. |
| `for x in arr` over `T[N]` | The element type is inferred. `for x in list` is E005 (F-2 is the fix). |

`guess` itself is frozen as Model A (`FREEZE.md` §6.3): initializer-only, local,
one-directional, the type fixed at the declaration. Three frozen rules bound
this work, and the proposals below are chosen to fit inside them:

- **F-GUESS-2: Model B stays rejected.** `mut l = List::default(); l.push(1);`
  (infer the element type from a *later* statement) cannot work, and no syntax
  makes it work: it would make a declaration's meaning depend on code below it,
  and codegen would need whole-function typing before the first line.
  Everything below is therefore about giving the *initializer* enough
  information, which is also why it costs nothing at run time.
- **F-GUESS-3: never for parameters, return types or struct fields.** Public
  signatures stay explicit (F-7a's separate compilation relies on that).
- **§6.3's last clause: `guess` never applies to "a generic type argument".**
  LG-3 narrows this one clause; it needs a `FREEZE.md` amendment.

#### Why not `List:int::default()`

| Spelling | Verdict |
|---|---|
| `List<int>::default()` | **Recommended (LG-1).** Same spelling as the type position, so there is one way to write a type argument. |
| `List::<int>::default()` | Unambiguous with no lookahead, but a second spelling and noisy. Used only where `<` cannot work (LG-2). |
| `List:int::default()` | **Rejected.** `:` already means "has type" in annotations, parameters, fields and `fn(T): R`, and ends `matches` and `otherwise` headers. Nesting is undecidable by eye: in `Map:string:List:int::default()` nothing says where `List`'s argument ends. It would also be a *second* spelling next to `<>` in type positions, or a change to every generic program (breaking, 3.0). |
| `List[int]::default()` | Rejected. `[` is indexing, and the parser consults no semantic state, so it cannot tell a type from a value. |
| `List(int)::default()` | Rejected. `(` is a call. |

Changing `<T>` in *type* positions to anything else is a 3.0-only break for no
gain once LG-1 exists. If it is ever wanted: both spellings are accepted for a
whole 2.x minor, with `--migrate` (register entry B-11 below).

#### Items

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **LG-1** | **`Name<Args>::member` in expression position** (this is `F-6c`). After an identifier or `module::Name`, `<` starts a speculative type-argument list; it commits only if the list closes with `>` **immediately followed by `::`**, otherwise it is less-than. `mut l = List<int>::default();`, `Map<string, List<int>>::default()`, `list::List<int>::default()`. | A | S–M | Safe because `x < y > ::z` is already a syntax error, so only programs rejected today change meaning. Bounded backtracking over a short type list; the parser stays free of semantic state. With `guess` it is plain Model A over an explicit type expression. Replaces the E110 hint. |
| **LG-2** | **Explicit type arguments on generic functions and function values: `name::<T>(…)`, `obj.method::<T>(…)`, `immut g = identity::<int>;`.** `::` followed by `<` cannot start anything else (E108 today). `f<int>(x)` cannot work: it already parses as comparisons when `int`'s place is a variable. | A | S–M | Gives `mut g = identity::<int>;` its missing source (E137), and makes `fn<T> make(): T` callable when LG-4 lands. |
| **LG-3** | **Struct type arguments from the constructor's arguments when no declared type supplies them.** `mut b = Box::new(42);` is `Box<int>`; `Pair::new("a", 1)` is `Pair<string, int>`. Exactly generic-function inference: every type parameter must occur in the parameter list, arguments must agree (E056), literals take their `guess` defaults first. A declared/expected type still wins when present, and a disagreement is E056. | A | M | Local, one-directional, one expression, so inside the spirit of F-GUESS-3; amends the §6.3 clause above. `List::default()` has no argument to read, so it still needs LG-1. |
| **LG-4** | **Type parameters that occur only in the return type** (`fn<T> make(): T`, `fn<T> parse(s: string): !T`), accepted when each call has an expected type or an LG-2 argument; E129 moves from the declaration to the call that has neither. | D | M | Additive (accepts more). Touches overload resolution, so it needs a design note first. `mut x = make();` stays E129 by design; `mut x = make::<int>();` and `mut x: int = make();` work. |
| **LG-5** | **Lambda parameter and return types from the expected function type** (needs F-1). `list.map(fn(x) { return x + 1; })`: `x` is read from the callee's `fn(T): R` with `T` already fixed by the receiver; `R`, which occurs only in the callee's parameter, is read from the body's `return`. With no expected type (`mut f = fn(x) {…};`) it is E128/E129 like any other `guess`. | D | M | **Relaxes F-GUESS-3 for lambdas only** (an expression with a known context, not a declaration with a public signature). The full-annotation form `fn(x: int): int {…}` stays valid. |
| **LG-6** | **Stdlib constructors that carry the element type**, additive under F-API-1: `List::from(items: T[])`, `Set::from(items: T[])`. With LG-3, `mut l = List::from([1, 2, 3]);` is `List<int>`. | A | S | Check that an `int[3]` literal converts to a `T[]` parameter view (#260) before promising it. |
| **LG-7** | **Inference you can see** (no language change): E129/E110 point at LG-1/LG-3 spellings through DX-1's `Fix:` where one candidate exists; LSP **inlay hints** for `guess`-inferred types and a code action "write the inferred type" (the compiler already records `WasGuessInferred`); `typeof` already prints a type at run time. | A | S–M | Readability without losing the short form. M1a and F-11. |

#### Considered and not proposed

- **Model B** (above): rejected by F-GUESS-2.
- **`List<guess>` partial annotations** (`mut l: List<guess> = …`): redundant once
  LG-1 and LG-3 exist, and it would add a way to spell the §6.3 clause LG-3
  already narrows.
- **`guess` for parameters, returns, fields, or `export immut` constants:**
  rejected by F-GUESS-3 and §6.3; public signatures stay explicit.
- **Wider integer promotion on overflow** (`mut x = 999…9;`): rejected by §6.3
  (there is no wider type).
- **Default type arguments** (`List<T = int>`): hides cost and defeats the point of
  being able to read the type at the use.

#### Speed and memory safety

All of LG-1 … LG-7 are front-end only. Each one resolves to the **same
monomorphized instance** that the annotated spelling produces (F-GEN-1/2;
INV-TYPE-2 makes the two inference engines agree for monomorphized nodes).
Gate for each item: a test pair, annotated vs inferred, compiled with
`--emit-llvm` at every `-O` level, whose output must be identical apart from
names and debug locations; and `TURF_GC_STRESS` runs for the new constructors.
Nothing here weakens a bounds, overflow, nullability or escape check, because
none of them runs before the type is fully known.

---

## Part 3 — Milestones (themes, each one or more minors)

Order is by **dependency and risk**, not by importance. A milestone is done when
its gates (1.5) are green; it then becomes the next minor. Items inside a
milestone may ship as separate minors if one is slow.

| Milestone | Theme | Items | Why here |
|---|---|---|---|
| **M0** (2.0.x patches) | Bug fixes and documentation only | bugs found in 2.0.0; the Simplification document's phase 0★ if a live repro confirms it | Nothing additive. |
| **M1** | **Contract tooling** | V-1, V-2, V-3, V-4, V-5, F-S0, F-S1 | Every later warning, deprecation and compatibility claim needs these. Cheap, low risk, mostly invisible. |
| **M1a** | **Diagnostics and SLM trust** | SL-0, DX-2, DX-3, SL-3, SL-2, SL-1, SL-5, SL-7, DX-1, SL-4, SL-6 (in that order) | The user-visible win that needs no new language feature. Order: provenance and the machine-readable form first, then the corpus and the scoreboard (so every later change is *measured*), then the new score and checks, then DX-1 shipping fixes by default, then model governance. Needs V-5 (the code registry) and benefits from F-S2 (the sandbox's save/restore dance). |
| **M1b** | **Numeric speed** | NM-1, NM-3, NM-4, NM-2 (in that order) | NM-1 is the baseline everything is judged against; NM-3 is small, additive and gives hot loops an immediate escape; NM-4 is a cheap experiment that may remove the need for source changes; NM-2 is the careful compiler work. Independent of M1a. |
| **M2** | **Analysis I** | AN-1, AN-2, AN-3 (a)(b), AN-4, AN-6, AN-7 | The user-visible payoff that is independent of everything else; entirely warn-first. Starts the clock on the B-1 promotions (each warning needs ≥ 2 minors of field time first). |
| **M3** | **Closures** | F-1 phases 1–2, G-1, LG-1, LG-2, LG-3, LG-6 | Largest language ergonomic gap; unblocks CC-2, F-2, F-10. The LG items are parser and type-checker work independent of closures and can ship first as their own minor. |
| **M4** | **Iteration, views, patterns** | F-1 phase 3, F-2, F-3, F-14, F-14b, LG-4, LG-5 | Builds on M3. LG-4 and LG-5 need decisions D-17 and D-18 first. |
| **M5** | **Runtime I** | RT-1, RT-3, RT-6, AN-5, AN-8, AN-9 (RT-4 is M1b) | Must precede threads: it is the same allocator and collector. Benchmark-driven. |
| **M6** | **Semantic model and IR** (parallel track) | F-S2, F-S4a (D-21), F-S3, then F-S4b → F-S4c → F-S4d → F-S4e, F-S5, AN-3(c) | Highest-risk internals; shipped as unobservable minors; gives later milestones a call graph, an IR and a reentrant pipeline. May run alongside M3–M5. |
| **M7** | **Concurrency I** | CC-0, CC-1, CC-2, RT-5 | CC-0's design is a gate: no implementation before the owner answers D-3. |
| **M8** | **Concurrency II** | CC-3, CC-4, CC-5, CC-6 | Builds on M7's runtime. |
| **M9** | **GC tier 2** | RT-2 (OS-tracked), or RT-2′ → feeds Part 4 | After threads, because the barrier/dirty-tracking design must know the thread model. |
| **M10** | **Build and packages** | F-7a, F-7c, F-11, F-11b, F-11c, F-11d | Needs F-S2 and F-S5. |
| **M11** | **Stdlib and platforms** | F-19, F-20, F-15b, F-16 | Open-ended; may be split into many minors. |
| **M12** | **Self-hosting** | F-10 | Last, as `FUTURE.md` says: self-hosting a compiler that is still gaining basic features means doing that work twice. |
| **3.0.0** | Part 4 | the register | |

M1, M1a, M1b, M2 and M6 can overlap; M3 → M4; M5 → M7 → M8 → M9 is a chain. SL-8 (`@slm` blocks) waits for SL-1 and SL-5 and is scheduled with F-9, after M1a. AN-* warnings become SL-5 inputs as they land.

---

## Part 4 — What 3.0.0 is

### 4.1 The breaking register

Nothing is added to a minor because it is on this list; it is on this list
because it could not go in a minor. Each entry names *why* it fails the minor
promise and what the migration is.

| ID | Item | Why it is breaking | Migration |
|---|---|---|---|
| **B-1** | **Promote proven warnings to errors:** AN-3 (a)/(b) (consistent with today's self-recursion error), selected AN-1 `unsafe` checks, any warning with ≥ 2 minors of field time and no false positive reported. | Rejects programs that compile in 2.x. | Under `lint-set = "3.0"` (V-1) the 2.x compiler already reports these as errors, so a project can fix its code before upgrading; `turf --migrate=3.0` annotates the sites. |
| **B-2** | **Runtime debug checks (RT-5) on by default at `-O0`.** | A program with latent memory corruption starts panicking. | `--no-debug-checks`. |
| **B-3** | **Removal of items deprecated by V-2.** | F-API-1 forbids removal before 3.0. | `@deprecated` message names the replacement; `--migrate` renames mechanically where the replacement has the same signature. |
| **B-4** | **GC write-barrier contract** (only if RT-2′ is chosen over RT-2). | `unsafe` and C code that stores a GC pointer into a GC object must call a barrier. | `mem::` helper plus a debug-check (RT-5) that finds missing barriers. |
| **B-5** | **New reserved words**, only if a contextual keyword cannot express a feature (CC-4(b)). | An identifier with that name stops parsing. | `--migrate` renames the identifier. |
| **B-6** | **Meaning of `fn(T): R[N]`** (G-1, option "fix the grammar"). | Changes the parse of an existing type. | Not needed if the additive spelling of G-1 suffices; likely never. |
| **B-7** | **Function values in `extern` struct fields.** F-1 makes a Turf function value a two-word `{code, env}`; the design keeps `extern fn` parameters thin, but a *struct field* of function type passed to C (or a function value cast through `void*` in `unsafe`) changes size. | Layout change. | Decide in F-1's stage 1 whether such fields stay thin (then no break). Listed so it is decided on purpose. |
| **B-8** | **Concurrency soundness rules that reject existing code**, e.g. a `mem::root`ed global mutated from more than one thread-entry function. | Only if CC-0 finds an existing pattern unsound once threads exist. | Case by case; likely empty. |
| **B-9** | **`turf.toml` / `turf.lock` format major bump**, only if F-7c needs it. | Old manifests rejected. | A converter in `turf`. |
| **B-10** | **Warning-set drift:** retire the `lint-set` pins older than N. | Pinned builds change. | Documented. |
| **B-11** | **A different spelling for type arguments in type positions** (only if the owner wants `<T>` replaced). | Every program with generics. | 2.x accepts both spellings for a whole minor; `turf --migrate=generics` rewrites. LG-1 removes the readability reason to do this. |

### 4.2 What adds nothing to the register

DX-1, DX-2, SL-0 … SL-8 and NM-1 … NM-6 are all additive or unobservable and
put nothing in the register. The designs TDP-002 rejects would be breaking
(a global wrapping flag, wrapping by default in optimized builds, undefined
overflow) and stay rejected. The one 3.0-adjacent follow-up is B-2's neighbour:
if NM-3 and RT-5 together show that default-checked arithmetic is still too
costly, the answer is more NM-2 proof, not a changed default.

### 4.3 The shape of the release

3.0.0 may be small. If the register is nearly empty after M12, the right
answer is to keep shipping 2.x minors; a major is a cost to users, not a
milestone to reach. Its trigger is "the register justifies it" or "a
soundness hole cannot be closed under 1.3 (a)".

---

## Part 5 — Decisions the owner needs to make

Each carries a recommendation so the default is never ambiguous.

| ID | Question | Recommendation |
|---|---|---|
| **D-1** | Rust-style **editions** (per-package opt-in to breaking changes) instead of a 3.0? | **No.** It adds a second compatibility axis for a compiler this size. Use 3.0 + `--migrate`. |
| **D-2** | What does `--deny-warnings` mean across minors? | Add it only together with V-1's `lint-set = "2.N"` pin, and document it as upgrade-unsafe without the pin. |
| **D-3** | Concurrency sharing model (CC-0). | Share nothing by default; transferable arguments; shared state only through stdlib `Mutex`/atomics/channels. |
| **D-4** | Is §E's "no generational collector" about a *moving* one? May RT-2 use OS dirty-page tracking, keeping non-moving? | Yes to both, with RT-1 measured first so the need is evidenced. |
| **D-5** | May RT-1's parallel mark use collector-owned worker threads under §6.5's "single-threaded"? | Yes; amend §6.5 to "the *mutator* is single-threaded until CC-1" and add the worker rule. |
| **D-6** | Are diagnostic message text and ordering outside the contract? | Yes. Codes and exit statuses are the contract. |
| **D-7** | Contextual keywords vs. new reserved words (B-5). | Contextual only; B-5 stays empty unless proven impossible. |
| **D-8** | Support window for the last 2.x after 3.0. | Bug-fix patches for 6 months; no features. |
| **D-9** | Order of M5 (runtime) vs. M3 (closures). | M3 first (user-visible, unblocks others), unless a benchmark regression says otherwise. |
| **D-11** | Does a rule-based `Fix:` print by default (DX-1), with `--smart` meaning "model assistance" only? | Yes, after DX-2 exists. Keep `--smart` accepting the old behaviour. |
| **D-12** | SL-2's penalty `K` for a wrong shown fix, and the shipping bar for a model fix (tier `verified`). | `K = 3`; a model path ships only if its mean utility beats the rules-only baseline with separated 95% intervals and false-confidence on the held-out split is 0 for tier `proven`. Revisit after SL-3 gives real numbers. |
| **D-13** | Does `--smart` warn about an unqualified model (SL-4)? | Warn once, never block. |
| **D-14** | TDPs and the Constitution live outside this repository. How are they referenced? | Each accepted TDP gets a one-page mirror in `docs/development/proposals/TDP-NNN.md` with its status, so the repository stays self-contained; roadmap items cite the TDP id. |
| **D-15** | How does NM-3's compiler-known lowering get declared (TDP-002's first open question)? | A fourth entry kind in `src/sema/Intrinsics.h` keyed by the resolved declaration's (module, function); change record plus `FREEZE.md` entry. |
| **D-17** | LG-4: allow type parameters that occur only in the return type? | Yes, with LG-2 explicit arguments or an expected type; E129 at the call that has neither. |
| **D-18** | LG-5: relax F-GUESS-3 for lambdas only (parameter types from the expected function type, return type from the body when the callee's `R` is open)? | Yes, lambdas only; declarations keep explicit signatures. |
| **D-19** | LG-3: amend `FREEZE.md` §6.3's "never applies to a generic type argument" to allow inference from constructor arguments? | Yes, that clause only; Model B (F-GUESS-2) stays rejected. |
| **D-20** | A separate Bound/Typed tree between the SemanticModel and TIR? | No: lower AST + SemanticModel into TIR; TIR carries operator identity and checks. Revisit if lowering re-derives decisions. |
| **D-21** | Start F-S4a (analysis out of Codegen) before F-S3? | Yes. It needs only the existing query facade and removes the AST-flag channel; F-S3 can follow. |
| **D-16** | May SL-5's optional Codegen recheck run inside `--smart`? | Yes (compile to an in-memory module and verify; never link or run). |
| **D-10** | Where does `PROPOSED_ROADMAP_3.0.0.md` go when accepted? | Replaced by `docs/development/ROADMAP_v2.N.0.md` per milestone, with this file kept as the register and Part 1; `FUTURE.md` is trimmed to what is unscheduled. |

---

## Appendix — Traceability to the sources

- `FUTURE.md` §A (F-1, F-2, F-3, F-6c, F-7a/b/c, F-11/b/c, F-14/b, F-15, F-20),
  §B (F-S0 … F-S5), §C (F-5, F-6, F-6b, F-8, F-9, F-10, F-12, F-16, F-19,
  F-22, F-24, F-25), Known gaps (G-1) are each placed exactly once in Part 2.
  §D (superseded) and §E (rejected) are not reopened except D-4/D-5's two
  readings of §E.
- `ARCHITECTURE_SIMPLIFICATION.md` phases 0★, 0–5 map to F-S3's note, F-S0, F-S1,
  F-S2, F-S3, F-S4, F-S5. Its §9 open questions are D-decisions inside F-S3,
  F-S4, and the numbering question is D-10.
- `FUTURE.md` F-9 (SLM: `@slm` blocks, LoRA, corpus growth) is split: corpus
  growth is SL-3, `@slm` blocks are SL-8, LoRA adapters stay in 2.7's F-9 row and
  are judged by SL-2's scoreboard. F-24 is NM-1 … NM-6 (TDP-002). F-11's "`--smart`
  fixes as code actions" is DX-1 + DX-2.
- `SLM-001.md` items 2–5 (deterministic layer, type context, cache, benchmark) are
  the existing base for SL-0 … SL-7; its *Known pitfalls* (partial `known_types`
  coverage, 11 wired rename classes) are SL-6 and SL-7.
- `FUTURE.md` F-6c and G-1 (array of function values) are specified by LG-1/LG-2 and decided with F-1; `FREEZE.md` §6.3 and F-GUESS-1/2/3 bound the LG items.
- New to this document: LG-1 … LG-7, V-1 … V-5, AN-1 … AN-3 (extended), AN-7 … AN-9, RT-1
  … RT-6, CC-0 … CC-6, DX-1 … DX-4, SL-0 … SL-8, NM-1 … NM-6, B-1 … B-11,
  D-1 … D-21.
