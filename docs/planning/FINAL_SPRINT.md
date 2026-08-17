# Final Sprint — The Road to Turflang 1.0.0

Phases 1–3 are complete. Operator overloading is complete. `docs/planning/ROADMAP_v1.0.0.md`
Phase 4 is complete except for the memory-model item below. This document is
the scoped, dependency-ordered checklist for everything still standing
between the current tree and a `1.0.0` tag: the remainder of Phase 4, a
narrowed slice of Phase 5, and all of Diagnostics Polish.

Scope was deliberately narrowed in a 2026-08-04 planning conversation —
several items in the original roadmap are large enough to be their own
post-1.0 phase. See "Explicitly deferred" at the bottom before assuming
something here is missing by accident.

---

## Track A: `unsafe { }` + Safe References (`&T`)

**Scope decision (2026-08-04): narrow, not the full ARC RFC.** There are two
very different things both called "Phase 4 memory work" in this codebase's
planning docs:

- The **roadmap's own checklist item** (`ROADMAP_v1.0.0.md` §6): gate raw
  pointer types behind `unsafe { }`, and add `&T` as a compiler-guaranteed
  non-null reference. Its own example syntax shows no ref-counting at all:
  ```turflang
  mutable val: int = 42;
  mutable ref: &int = &val;  // compiler-guaranteed non-null
  ```
- The much larger **`MEMORY_ARCHITECTURE_RFC.md`** proposal: full ARC
  (hidden `ref_count` on every heap object, automatic retain/release
  injected at every assignment/scope-exit), heap-allocated mutable strings
  and "slice" arrays replacing today's raw-C-string/stack-array model,
  `unsafe_ptr<T>` as the sanctioned FFI escape hatch.

**Track A targets the first one only.** Today's strings are still raw
C-strings (`CreateGlobalStringPtr`, immutable, `EmitStringConcat`'s own
`TURF-MEMLEAK` comment in `Codegen.cpp`), arrays are still stack
`AllocaInst`, and the entire stdlib (`mem.tr`, `net.tr`, `list.tr`, ...) is
still built directly on `malloc`/`free`/`void*`. None of that changes in
this track. `STDLIB_ROADMAP.md` §4.3 already reached this same conclusion
independently: the ARC half of the RFC is "compiler + Codegen work,
fundamentally different in kind" from everything else in this document, and
is explicitly out of scope here.

**Implementation sketch:**

1. **`unsafe { }` block** — new `TOK_UNSAFE`, a `UnsafeBlockExprAST` node
   (block scope, no new control-flow semantics — it doesn't affect
   break/continue/return, purely a lexical gate). Resolver/TypeChecker track
   an "inside unsafe" flag on the scope stack, same mechanism already used
   for loop-context (`LoopBlocks`) tracking.
2. **Gate raw pointer types outside `unsafe { }`** — `ptr` (`OpaquePointerType`,
   `include/Types.h`), `T*` struct pointers, and any `void*`/pointer-typed
   `extern fn` parameter or return type become a TypeChecker error when their
   *use* (declaration, cast, dereference) is lexically outside an `unsafe`
   block. This is the actual "no grace period" enforcement the roadmap's
   philosophy section (§1) already commits to.
3. **`&T` non-null reference type** — new type wrapping an existing pointee
   type, address-of operator (`&val`) producing it, auto-deref on use (no
   explicit `*ref` needed for field/member access, matching the roadmap's
   example). Compiler rejects assigning `null` or a raw `ptr`/`T*` to a
   `&T`-typed location without going through `unsafe`.
4. **Open design question, not yet resolved — flag before starting:** stdlib
   modules like `mem.tr`/`net.tr` are *implemented* using raw pointers
   internally (that's the whole point of `extern fn` FFI bindings) but
   should still be *callable* from safe code without every caller wrapping
   every call in `unsafe { }`. The RFC's own "safe-to-unsafe bridging"
   section gestures at this (temporarily exposing a raw pointer from a safe
   type) but that's ARC-era design. For the narrow scope here, the likely
   answer is: `extern fn` bodies (there are none — they're bindings) don't
   need gating, only *caller-side* raw-pointer-typed locals/params/casts do,
   and stdlib wrapper functions that take/return `void*` (`mem::alloc`,
   etc.) simply keep their current signatures and are usable from safe code
   as ordinary calls — the pointer *value* just can't be stored in a
   caller-side variable outside `unsafe` without an explicit escape hatch.
   This needs a real decision (and probably a short addendum to this file)
   before implementation starts, not mid-implementation.

**Resolved (2026-08-17), see GitHub issue #238:**

- **Trust boundary:** the entire `lib/std/` tree is exempt from the `unsafe`
  gate. `mem.tr`/`net.tr`/`list.tr`/etc. keep using `ptr`/`T*`/`void*`
  exactly as they do today — no migration of existing stdlib files. The gate
  applies only to code outside `lib/std/`.
- **Caller-side locals:** binding a stdlib call's pointer-typed return value
  to a variable in user code requires `unsafe { }`, even though the call
  itself (`mem::alloc(64)`) is an ordinary safe call. Example:
  ```turflang
  unsafe {
    mutable p: ptr = mem::alloc(64);
  }
  ```
- **Struct fields:** a `T*`-typed field is itself a gated use. The *entire
  struct declaration* must be lexically inside `unsafe { }` if it has a raw
  pointer field — not just the code that later dereferences it. This blocks
  raw-pointer-containing types from being introduced into safe user code even
  when the field is never dereferenced.
- **Escape hatch form:** block-only (`unsafe { }`) for 1.0.0. No
  expression-level `unsafe(expr)` form — out of scope, would expand the
  parser surface beyond this track's sketch.

---

## Track B: Phase 4 Stdlib Remainder

Small, mechanical, no open design questions.

- **`std/os.tr`** (`lib/std/os/os.tr`, new file, new `os` subfolder matching
  the existing `core/`/`collections/`/`io/`/`mem/`/`math/`/`testing/`
  convention) — `extern fn getenv(name: string): string` /
  `extern fn setenv(...)`, process argv/argc (needs a way to reach `main`'s
  argv from Turf — check how `main.cpp` currently invokes the compiled
  `main()` and whether argv is already plumbed through or needs a new
  `extern fn` surface), exit code beyond `main()`'s own return value
  (`sys::exit` already exists in `sys.tr` — confirm it's sufficient or needs
  an os.tr-level wrapper). `STDLIB_ROADMAP.md` §2.7 has the full gap list;
  only env vars + argv are actually blocking real programs today.
- **`math::PI` / `math::E`** — trivial `export immut` constants in
  `lib/std/math/math.tr`, the same feature `net.tr`'s `AF_INET` etc. already
  use. No blockers.

---

## Track C: SLM Retry Loop (Phase 5, narrowed)

**Scope decision (2026-08-04):** the trigger stays `--smart` — no new
`@slm { "prompt" }` parser syntax (`TOK_SLM_BLOCK`/`SLMBlockExprAST`) in this
sprint. LoRA fine-tuning (converting `needs_slm_training_dynamiccontext/`
and `lib/std/` to JSONL, training Adapter A/B) is also deferred — it's a
data/training project, not a compiler feature, and doesn't block anything
else here.

**What actually changes:** `src/SLMIntegration.cpp`'s
`validateAndApplySuggestion()` today is single-shot — `fetchSuggestion()`
once, `verifySuggestion()` (sandbox compile check) once, and on failure it
just downgrades `fix_mode` to `"NONE"` and gives up
(`SLMIntegration.cpp:708-717`). The roadmap's "SLM Query Loop" item ("Retry
up to 3× on TypeChecker failure, feeding the error back into the prompt")
is genuinely not implemented yet — this is the loop to build:

1. On `verifySuggestion()` failure, don't immediately give up. Pull the
   failure reason from `Sandbox::getValidationDiagnostics()` (already
   populated by `validate()` — see `include/Sandbox.h:89-92`) — this is the
   real compiler's own error message for *why the suggested fix didn't
   work*, not a re-guess.
2. Feed that diagnostic text back into a second `fetchSuggestion()` call as
   additional context (extend the existing user-prompt JSON shape in
   `SLMIntegration.cpp:548-568` with a `"previous_attempt_failed"` field, or
   similar) — same pattern the file already uses for pinned
   out-of-window definitions (`getCodeContextWithPinnedLines`), just applied
   across a retry instead of within a single prompt.
3. Cap at 3 total attempts (matches the roadmap wording exactly). Each
   attempt still goes through the same sandbox validation and confidence
   scoring path that exists today — nothing about `computeStructuralPrior`/
   `finalizeConfidence` needs to change, they just get called once per
   attempt instead of once total.
4. If all 3 attempts fail, current behavior (downgrade to `NONE`, surface
   the original compiler error to the user) is unchanged.

No parser, Resolver, or Codegen changes required — this is entirely
contained in `SLMIntegration.cpp`/`Sandbox.cpp`.

---

## Track D: Diagnostics Polish

All three items from `ROADMAP_v1.0.0.md`'s Diagnostics Polish section.
Not attempted earlier because "the error class taxonomy is still changing
through Phase 3" — that's no longer true, Phase 3 is done, so this is
unblocked.

- **Source Spans — partially done.** `SourceLocation` (`include/Lexer.h`)
  now carries `EndLine`/`EndCol` (default `-1` = "not computed"), added
  while building `turf-lsp` support (turf-lang/turf-lang#243) —
  `gettok()` populates them for identifier/keyword tokens, which reaches
  every AST node's name location for free (struct/function/variable/
  field/method/module names, #230). Remaining scope, unchanged from the
  original ask: (1) `DiagnosticEngine`'s own `printDiag()` doesn't consume
  `EndCol` yet — the CLI's `^` output is still single-point; wiring it up
  to underline a span when `EndCol != -1` is a `Lexer.cpp`-local change.
  (2) Operators, string/char/number literals, and genuine multi-token
  spans (a whole `add(3, "4")`-shaped sub-expression, not just one
  identifier) still have no end position — extending coverage there is
  the "mechanical but wide-reaching" work the original estimate describes.
  Payoff once both land: underline the full failing expression in
  `DiagnosticEngine` output, not just a single `^`.
- **Error Codes + `turf --explain`** — every `TurfError` subclass in
  `Errors.h`/`ErrorsLex.h` gets a unique code (`E042` etc.). `turf --explain
  E042` prints a multi-paragraph markdown explanation — likely reusing the
  same `docs/slm/*.txt` per-category doc files the SLM integration already
  loads (`loadDocFile` in `SLMIntegration.cpp`), rather than inventing a
  second parallel documentation set.
- **Broader "Did You Mean?"** — `include/SuggestionIndex.h`'s
  `SuggestionIndex::suggestVisible` already exists and is wired up for
  variable-name typos (`ResolverExpr.cpp:113`). This is an *extension*, not
  new infrastructure: feed it struct field names at `.field` access sites
  and module/stdlib function names at `Module::func` call sites, the same
  trie already built for variables.

---

## Suggested Execution Order

Tracks B, C, and D have no dependencies on each other or on Track A — they
can be done in any order, or in parallel across sessions. Track A is the
only one worth sequencing deliberately:

1. **Resolve Track A's open design question first** (how deep the `unsafe`
   gate reaches relative to stdlib FFI wrappers) — everything else in Track
   A blocks on that answer, and it's a design decision, not implementation
   work, so it costs nothing to settle early.
2. **Track B** — do this early regardless; it's small, unblocks nothing else
   but is easy to knock out first for momentum.
3. **Track A** (once §1 above is settled) — the largest, riskiest item;
   touches Types.h, Parser, Resolver, TypeChecker, and Codegen's pointer
   handling throughout.
4. **Track C** and **Track D** — independent of Track A and each other,
   safe to interleave or parallelize.

---

## Explicitly Deferred Past 1.0.0

Recorded here so it's clear these were a deliberate scope call, not an
oversight:

- **Full ARC** (`MEMORY_ARCHITECTURE_RFC.md`'s ref-counting, heap-managed
  strings/arrays, `unsafe_ptr<T>`) — Track A above covers only the narrow
  roadmap checklist item, not this.
- **`@slm { "prompt" }` in-source syntax** (`TOK_SLM_BLOCK`/
  `SLMBlockExprAST`, structured symbol-context serialization for arbitrary
  user code generation) — Track C covers only the `--smart` retry loop.
- **LoRA fine-tuning** (Adapter A/B training data + training run).
- **Phase 6** (LSP server, `turf fmt`, `turf test`, `tpm` package manager)
  and **Phase 7** (self-hosting) — unchanged, already tracked as post-1.0 in
  `ROADMAP_v1.0.0.md`.
- **Remaining stdlib modules** beyond `os` (`time`, `json`, `http`, `path`/
  `fs`, `random`, `Set<T>`, sort/search helpers) — see `STDLIB_ROADMAP.md`
  §2.7/§3 for the full list; none of these block 1.0.0 the way `os.tr` does.
- **`cfg_analysis_roadmap.md`'s advanced static-analysis phases** (beyond
  what's already shipped — see that file's own status note) — a longer-term
  effort, not gating 1.0.0.
