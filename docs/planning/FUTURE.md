# TurfLang: after 2.0.0

> **Status:** the post-2.0 backlog. Rewritten 2026-09-26 by the Architecture
> Overhaul closure audit. It absorbs (a) every row of
> `docs/planning/ARCHITECTURE_OVERHAUL.md` §27 (the Deferred Work Register)
> that is **not** scheduled for 2.0, with its original rationale, and (b)
> every item of the previous version of this file, which was written after
> v1.0.0 and before the Overhaul.
>
> **What changed from the previous version, so nothing looks silently
> dropped:** its "Turflang 2.0.0: Self-Hosting & Bootstrapping" heading is
> retired — **2.0.0 now means the first public release**
> (`docs/planning/ROADMAP_v2.0.0.md`); self-hosting is F-10 below. Its item 5
> (ARC memory architecture) is superseded by the Overhaul's tracing-GC
> decision (§D below). Its item 1 (tooling) is partly in 2.0 (REL-002). Its
> item 6 (CFG remainder) is partly done (OPT-002). Everything else is here.
>
> **How to use it:** after 2.0.0 ships, this is where development resumes.
> Section A is the list of things deliberately kept *out* of 2.0 — the first
> candidates for 2.1+. Nothing here may be implemented as a breaking change
> in 2.x (ROADMAP_v2.0.0.md Part VI); anything breaking waits for a 3.0 plan.
> Rejected ideas are in §E and are not backlog.
>
> Complexity scale (the Overhaul's): **XS** < 1 session · **S** 1 · **M** 2–3
> · **L** 4–6 · **XL** 7+ with its own sub-plan.

---

## Known gaps

Behaviour found while testing that is neither a deliberate exclusion (§A) nor
a rejected idea (§E): the language does not say what should happen, or the
compiler cannot express it yet.

### G-1 — No syntax for an array of function values

- **Found** 2026-10-05, while probing first-class functions after the
  call-through-a-non-function fix. First-class functions themselves are
  supported (`fn(T): R` types; named functions passed, stored in variables and
  returned; see `docs/reference/functions_and_modules.md` §2). Only the
  array-of-function-values spelling is missing.
- **What happens.** `mut fs: fn(int): int[2] = [inc, inc];` does not declare an
  array of two functions: `fn(int): int[2]` parses as *one* function type whose
  return type is `int[2]`, so the initializer is rejected with a generic `E066`
  ("Cannot initialise 'fn(int): int[2]' variable 'fs' with value of type
  'fn(int): int[2]'") — a message that reads as a self-contradiction. Wrapping
  the element type, `(fn(int): int)[2]`, is rejected too, with `E109`
  ("Expected a type name"): the type grammar has no parenthesised type.
- **Why it is a gap, not a bug.** Neither the reference nor this file says an
  array of function values is allowed or forbidden, and the grammar offers no
  way to write one. It is an unspecified corner of the type grammar.
- **To decide.** (1) A spelling for a parenthesised/grouped type (or a type
  alias) so the element type can be written; (2) whether `fn(T): R[N]` keeps
  its current meaning (it must, in 2.x: changing it is a breaking change, see
  ROADMAP_v2.0.0.md Part VI); (3) a clearer diagnostic for the E066 above
  whichever way (1) goes, since the current text names two identical types.
  Related to F-1 (closures): a `{code, env}` representation changes the element
  size, so decide the two together.

---

## A. Deliberately excluded from 2.0 — first candidates afterwards

Each was considered for 2.0 and excluded under the roadmap's inclusion rule
(Part 0.1: additive features wait). The "Relationship to 2.0" line records
what 2.0 does that this item builds on.

### F-1 — Closures / lambdas (§27 D1)

- **Complexity** L · **Excluded from 2.0:** yes, deliberately (additive).
- **Current limitation.** `fn(x: int): int { … }` in expression position is
  a parse error ("Expected a function name after 'fn'"). Function *values*
  exist (`fn(T): R` types, passing named functions) but they cannot capture.
  Callback-style APIs (the `http.tr` router, `List.find(pred)`) must use
  named top-level functions and thread state through parameters.
- **Why it was deferred.** A closure environment must outlive the frame that
  created it, which needed a collected heap (MEM-002) — "building one before
  the memory model exists means building it twice" (§27). After 2.0 the
  prerequisites exist: typed GC allocation with descriptors (FDN-001,
  OVH-001), `mem::root` for environments held by C (OVH-010), escape rules
  (FDN-002).
- **Function-value representation (the central decision).** Today a function
  value is a thin code pointer, which is what makes C callbacks (`qsort`
  comparators, LANG-002 F1) work. Recommended: a Turf function value becomes
  a two-word `{code, env}` for Turf-to-Turf calls (named functions get
  `env = null` and a thunk or an ignored hidden parameter); a parameter of an
  `extern fn` whose type is a function type stays a **thin C function
  pointer** and only accepts a named function or a capture-free lambda,
  checked at the call site. Rejected alternative: two user-visible function
  types (`fn` vs `closure`) — doubles the type surface for one ABI detail.
- **Environment.** A compiler-synthesized struct per lambda, GC-allocated
  with its own `TurfTypeDesc` (precise tracing), holding the captured values.
- **Capture semantics.** By value (a snapshot at creation). Capturing a
  `mut` local is allowed; *assigning* to a captured variable inside the
  lambda is an error with a hint to share state through a `heap()`-allocated
  struct (`&T` captured by value). This avoids implicit shared mutable
  cells, and makes the lifetime story trivial.
- **Ownership/lifetime.** The environment is an ordinary GC object; a closure
  value on the stack is found by conservative scanning; a closure stored in C
  memory must be `mem::root`ed. Capturing a `&local` is governed by FDN-002's
  escape rules (the environment has Heap provenance, so storing a
  Stack-provenance reference into it is already an error).
- **Escaping vs non-escaping.** First version: every environment is
  heap-allocated. Stack-allocating provably non-escaping environments is a
  later optimization (needs the same provenance analysis).
- **Nested closures.** A lambda capturing a closure value copies the
  two-word value; nothing special.
- **Recursion.** A lambda cannot name itself; recursive logic uses a named
  function. (A `self`-style name for the lambda is not proposed.)
- **Generics.** No generic lambdas; a lambda inside a generic function is
  monomorphized with its enclosing function (F-GEN-1), so captured `T`s are
  concrete.
- **`defer`/`try`/`recover` inside a lambda.** Block-scoped as everywhere;
  `try` requires the lambda's own return type to be `!R`.
- **Syntax.** `fn(x: int): int { return x + 1; }` as an expression — the
  exact form users already try. No arrow form, no implicit returns, no
  capture lists (by-value capture needs none).
- **Diagnostics.** assignment to a captured variable; capturing lambda passed
  to an `extern fn`; escaping stack reference captured (FDN-002).
- **Phases.** (1) capture-free lambdas (thin pointers, no env) — parser and
  lowering only; (2) `{code, env}` representation + captures + descriptors;
  (3) stdlib adoption (`List.map/filter`, a closure-based `http` router);
  (4) non-escaping environments on the stack.
- **Testing.** capture correctness; closures surviving `TURF_GC_STRESS=1` at
  `-O0`; closures stored in collections; FFI rejection; nested closures;
  every `-O` level identical.
- **Performance.** one extra word per function value, an indirect call with a
  hidden argument, one allocation per closure creation — measure against
  ROADMAP REL-005's baseline.

### F-2 — Iteration over collections (§27 D2)

- **Complexity** M · **Excluded from 2.0:** yes (additive; OVH-003's
  bounds-checked `get(i)` makes index loops safe).
- **Limitation.** `for x in …` works for fixed arrays and numeric ranges only.
- **Design sketch.** No trait system (F-GEN-3). Recommended: a compile-time,
  monomorphized *structural* protocol — `for x in e` is legal when `e`'s type
  has `length(): int` and `get(i: int): E` methods, lowering to an index loop;
  `Map` iterates keys via a `keys()` method. An iterator-object protocol
  (`next(): !E`) or closure-based `each(f)` (needs F-1) are the alternatives;
  decide after F-1 exists, as §27 advised.
- **Prerequisites** none hard; nicer after F-1.

### F-3 — `Slice<T>`: a non-owning view (§27 D3)

- **Complexity** M · **Excluded from 2.0:** yes (additive). Its soundness
  half — `T[]` parameters escaping the caller's frame (D23) — **is** 2.0
  (ROADMAP FDN-002).
- **Design sketch.** A stdlib generic `{ptr: T*, len: int}` with bounds-checked
  `get`/`set`/`sub(from, to)`; views into GC buffers stay valid through
  interior-pointer retention (MEM-002's block table); views into stack arrays
  carry Stack provenance under FDN-002 and cannot escape. A `string` is
  NUL-terminated, so a byte view of a string is a separate type
  (`Bytes`/`StrView`), consistent with F-STD-3.

### F-6c — Expression-position generic type arguments

- **Complexity** S–M · **Excluded:** yes (a workaround is natural: declare the
  type on the left, `mut l: List<int> = List::default();`).
- **Limitation.** `List<int>::default()` does not parse (SEM-004 pitfall);
  with `guess`, `mut l = List::default();` is an error by design (F-GUESS-3).
- **Design.** Parse `Name<Args>::member` in expression position when `Name`
  is a known generic type (the parser does not consult semantic state today —
  §1.3 — so this needs a syntactic rule, e.g. only before `::`). Explicit type
  arguments on generic *function* calls (`f<int>(…)`) likewise.

### F-7a / F-7b / F-7c — The rest of D7

2.0 ships a source-based project model (ROADMAP LEC-001, Part IX). Not in 2.0:
- **F-7a Separate compilation** (L–XL). Per-module caching of non-generic
  code; generic templates still travel as source. Trigger: a real project's
  build time. The monomorphized mangling scheme is already a pure function of
  its inputs (§9.5) so caches can key on it.
- **F-7b A stable Turf-to-Turf binary ABI** (XL). Only needed for binary
  packages; source distribution makes it unnecessary. The stable interop ABI
  is the C ABI.
- **F-7c Registry, semver ranges, publishing, workspaces** (L). The 2.0
  CLI (`cli/`, the former `turf-cli` repository, `FREEZE.md` §6.16) reserves
  `turf publish`. The pre-2.0 semver resolver, checksum, archive and registry
  client code was deleted in LEC-001 stage 6; it is in git history (the
  subtree import `350acd8`, and the parent of the stage 6 deletion commit)
  for whoever builds F-7c. The registry server stays in the old `turf-cli`
  repository. Its
  `turf_modules/` install layout is gone: 2.0 fetches into the
  `~/.cache/turf/src/` cache.

### F-11 / F-11b / F-11c — Tooling beyond 2.0's (§27 D11, old item 1)

2.0 ships `turf test`, `turf fmt` (indentation-only, idempotent), and an LSP
with diagnostics, hover and go-to-definition on the SMP-001 facade (ROADMAP
REL-002). Later:
- **F-11** LSP completion, find-references, rename, code actions (the
  `--smart` deterministic fixes are natural code actions), incremental
  re-analysis (needs F-S2/F-S3).
- **F-11b** A real pretty-printer (needs a trivia-preserving lexer, F-S0).
- **F-11c** A documentation generator over `pub` items (needs LEC-002's
  visibility and doc-comment conventions).
- In-source `@test` functions (language surface; `turf test` runs whole
  programs in 2.0).

### F-14 / F-14b — Pattern matching beyond 2.0's (§27 D14 remainder)

2.0 ships destructuring, bindings, guards, error-payload patterns (ROADMAP
LEC-003/004). Later: **F-14** `compare` as an expression, range patterns,
nested or-patterns; **F-14b** payloads on plain `enum`s (sum types) —
LEC-004's payload representation is designed to be reused.

### F-15 — Error handling beyond 2.0's (§27 D15 remainder)

Error stack/return traces (record the `@caller_location` of each `try`),
closed error sets in signatures, error conversion. None is needed for
coherence; the open union (`!T`) is the frozen design.

### F-20 / F-20b — Cross-linking and more platforms (§27 D20)

2.0 (owner decision 2026-09-30, ROADMAP DEC-2 amended, DEC-14, REL-008):
Linux x86-64, Windows x86-64 and macOS arm64, all Tier 1 and
self-contained. Object-only cross-compilation keeps its link-time diagnostic
(REL-003). **F-20b and F-20c below were pulled into 2.0 as REL-008.** Their
text stays here as the design notes REL-008 was written from; F-20 itself
(cross-linking) and other platforms stay FUTURE.
- **F-20** Per-target `libturfrt` archives and a cross-linker story (LLD or
  a target `cc`).
- **F-20b** (now REL-008 stage 5) Windows x86-64: the GC's register capture for the Win64
  callee-saved set (`rbx rbp rdi rsi r12–r15 xmm6–15`), stack bounds via
  `GetCurrentThreadStackLimits`, toolchain choice (MinGW was used for v1.0.0
  archives). Other architectures need their own register-capture code
  (MEM-002 *Known pitfalls*).
  - **Install:** an `install.ps1` for `irm https://…/install.ps1 | iex`,
    which is how rustup, deno and bun install on Windows. It mirrors
    REL-003's `install.sh`: detect the architecture, download the zip,
    verify its SHA-256, unpack under `%LOCALAPPDATA%\turf`, add `bin` to the
    user `PATH`.
  - **"Unzip and run" needs F-20c:** today `turfc.exe` links through MinGW
    `gcc`, which a normal Windows machine does not have.
  - **Shell:** nothing in `turf.exe`/`turfc.exe` needs a Unix shell. They
    run from PowerShell or cmd once their DLLs are beside them (the release
    zip bundles them) and a linker is available. MSYS2 is only the
    *from-source* build environment (LLVM 22, MinGW, CMake packages; bash
    build scripts).
- **F-20c** (now REL-008 stages 2–4) **A bundled linker and C runtime** (M–L). Release `turfc` links with
  an embedded **lld** instead of running the system `cc`, and the archive
  carries a C runtime to link against: musl's `libc.a` and start-up objects
  on Linux, the MinGW-w64 CRT import libraries on Windows, the macOS SDK
  stubs (which is its own licensing question).
  - Result: installing TurfLang needs nothing else installed, and every
    compiled program is fully static. That is how Zig ships.
  - It is also F-20's cross-linking story: per-target runtimes plus one
    linker.
  - Trade-offs: archive size (tens of MB), and a libc to keep patched.
    Programs `extern from` a system C library still need that library at
    link time.
  - Pulled forward on 2026-09-30 (ROADMAP DEC-14), with **hybrid**
    linking: only programs that name a system C library use the system
    toolchain.

---

## B. Architecture after 2.0 (Architecture Simplification)

`docs/planning/ARCHITECTURE_SIMPLIFICATION.md` is the source; ROADMAP Part III
is the assessment. Only SMP-001 (query facade) and SMP-002 (one generic type
walker) happen before 2.0. After 2.0 these become a tracked initiative with
IDs, freeze entries and change records:

| ID | Phase | Complexity | Notes |
|---|---|---|---|
| F-S0 | `LexerState` (18 lexer globals → an owned struct) | S–M | Enables concurrent tokenization and F-11b. |
| F-S1 | `ParserState` | M | Concurrent parsing of editor buffers. |
| F-S2 | Remove the `GlobalSymbolTable` singleton | M–L | `turf-lsp` plan item F; behind SMP-001's facade, invisible to tools. |
| F-S3 | A real `SemanticModel`, separate from AST mutation | L–XL | Folds in §27 **D18** (two type-inference engines: `NodeTypes_` vs Codegen's `getExprType()`; INV-TYPE-2 makes them agree for monomorphized nodes only). Needs a freeze decision: mutate-in-place vs immutable trees (Simplification §9). Revisit trigger from §27: a second wrong-answer bug of the `modules_and_scope_resolution.md` §2.10 kind. |
| F-S4 | Promote the CFG to a lowering IR; Codegen emits from it | L–XL | Folds in §27 **D19** (the double-parse pre-pass: every file lexed/parsed twice, pre-pass diagnostics discarded; load-bearing for struct-layout ordering; TYPE-002's U-4 record is the best description of it) and **INV-CG-1** (GEP against an un-laid-out struct is prevented only by pass ordering). OPT-002 already builds a second CFG before Codegen — evidence for this. |
| F-S5 | Dependency layering (frontend library independent of Codegen/LLVM IR) | M | Includes HYG-001's unresolved question: every `Parser*.cpp` includes `Codegen.h`, so parser TUs still pull in the LLVM IR headers. |
| — | Comments | — | Folded into whichever phase touches a file (Simplification §8); never a standalone sweep. |

---

## C. Other deferred work

### F-5 — Full pointer-nullability analysis (§27 D5, old item 6)
- **Complexity** M. LANG-002's `NullabilityPass` is deliberately
  intraprocedural and narrows only from a bare variable compared to `null`.
  Not narrowed today: fields (`if (p.next != null)`), function arguments and
  `return` of a narrowed `&?T` as `&T` (must go through a local cast — LANG-002
  *Known pitfalls*), anything across calls. `DESIGN_NOTES.md` §1 specifies
  more. Additive precision; the current pass is sound.

### F-6 — Manual-heap leak detection (§27 D6, old item 6)
- **Complexity** M. "malloc'd and never freed" warnings, distinct from the
  use-after-free/double-free tracking that already ships
  (`AliasAnalysisPass`). Scope is the manual heap only, since the GC heap no
  longer leaks — as §27 predicted.

### F-6b — Panic backtraces
- **Complexity** M. Symbolized backtraces on panic (DWARF is already
  emitted with `-g`; `libturfrt` would need an unwinder or `backtrace()`).
  Out of 2.0 (FDN-006 keeps panics message + location only).

### F-8 — Concurrency, threads, async (§27 D8)
- **Complexity** XL. Nothing is designed for it; the GC needs per-thread root
  scanning, a stop-the-world handshake, allocation locks or per-thread
  buffers (Overhaul §14.11 records exactly what changes — the *choice* of a
  non-moving mark-sweep collector survives). No consumer yet. §25 item 20
  forbade it *for the Overhaul*; it is backlog, not rejected.

### F-9 — SLM: `@slm { }` blocks, LoRA fine-tuning, corpus, categories (§27 D9, old items 2–3)
- **`@slm { }` in-source blocks** (L) — Prompt-Oriented Programming: `@slm
  { "prompt" }` parsed to `SLMBlockExprAST`; `serializeSymbolContext()`
  builds the prompt from the resolver's visible symbols; the block's
  generated source goes through Lexer→…→TypeChecker with up to three retries
  on TypeChecker failure, sharing `--smart`'s retry scaffolding
  (`SLMIntegration.cpp`'s `kMaxSlmAttempts` loop). Must keep F-DIAG-1/2
  (sandbox verification, confidence gate). Build it against the post-2.0
  architecture (§25 item 18's reason for waiting).
- **LoRA fine-tuning** (L) — Adapter A (syntax & instant fixes; improves
  today's `--smart`) and Adapter B (AST generation for `@slm` blocks); full
  plan in `DESIGN_NOTES.md` §6. SLM-001's benchmark corpus is the
  prerequisite for judging either.
- **Corpus growth** (S) — SLM-001's corpus is 6 hand-picked cases; add
  mutations via `mutate.py`'s `MUTATIONS` list.
- **SLM diagnostic categories / `docs/slm/` expansion** — deliberately
  deferred and **user-initiated only** (F-DIAG-3). Listed so it is not
  forgotten, not scheduled.

### F-10 — Self-hosting (§27 D10, previous "2.0.0" section)
- **Complexity** XL. Writing TurfLang in TurfLang, binding LLVM's C API via
  `extern fn` (`showcase/llvm_ffi_selfhost_demo.tr` proves the binding works)
  and `llama.cpp`'s C API for the SLM. Bootstrap cycle: write new compiler
  logic with existing features → compile with the current compiler → use the
  new feature. Prerequisites now: F-1 (closures, for callback-heavy code), a
  stable 2.x, ideally F-7a. The previous version of this file's reasoning
  still holds: self-hosting a compiler that is still gaining basic features
  means doing that work twice.

### F-12 — Networking: TLS/HTTPS, UDP, non-blocking I/O (§27 D12)
- **Complexity** L. TLS is large and security-sensitive (bind a vetted C
  library through FFI rather than writing one); non-blocking I/O needs F-8.

### F-16 — Circular imports (§27 D16)
- **Complexity** M. A real module-system change. Today's only cost:
  `mem::Pool<T>` is a deliberate duplicate of `List<T>`'s growth logic
  because `list.tr` imports `mem.tr` (`mem.tr:155-214`; STD-003 item 7 says do
  not work around it).

### F-19 — Standard library expansion (old item 4)
- **Complexity** varies. `DESIGN_NOTES.md` §3/§4's tiered gap analysis
  (`strings::split` exists now; the rest: richer `os`, `time` formatting,
  regex, CSV, compression, crypto hashes, a process API, …). Everything added
  after 2.0 is additive and goes through LEC-007's API conventions (fallibility
  rule, `pub` review, API-lock test). `docs/planning/LIMITATIONS.md` is closed
  (all items fixed by TYPE-002/LANG-001/STD-003); new gaps get change records,
  not new LIMITATIONS rows.

### F-22 — Opt-in lint for `int → double` precision loss (§27 D22)
- **Complexity** S. Above 2^53 the implicit conversion loses precision
  silently; §10.5 argued keeping it implicit. A future opt-in lint is the
  intended answer — **never** an error.

### F-24 — Checked arithmetic vs auto-vectorization (§27 D24)
- **Complexity** M. Measured 2026-09-20 on the `structs` benchmark: the
  per-operation overflow branch (`EmitCheckedIntArith`) prevents LLVM loop/SLP
  vectorization; removing only that branch let `-O3` vectorize (`paddq`) and
  closed a 53%-slower-than-`clang -O3` gap to parity. Two undecided
  directions: (a) widen the static range-proof elision OPT-002 built (no
  semantic change); (b) an explicit opt-in wrapping construct for hot loops
  (Swift `&+`, Rust `wrapping_add`) — default stays checked. A semantics-
  changing global flag stays forbidden without this kind of evidence and an
  explicit F-OPT-1 exemption (§25 item 8).

### F-25 — Speculative paradigms (old item 7)
- `DESIGN_NOTES.md` §7 (self-healing compiler, semantic interface matching)
  and §8 (extreme compile-time execution via ORC JIT, region-based memory as
  an alternative to GC, reversible execution). Design sketches only, no code;
  revisit much later. Note that §25 rejects user-facing `comptime` and macros
  — any compile-time execution idea must respect that decision or argue for
  reversing it explicitly.

---

## D. Superseded — closed, with the reason

| Item | Source | Closed by |
|---|---|---|
| ARC / reference counting as the memory model | previous FUTURE item 5, `DESIGN_NOTES.md` §5 | Overhaul §14.3 chose a tracing collector (F-MEM-1): ARC needs retain/release at every assignment, does not collect cycles, and fights the FFI-heavy stdlib. Implemented as MEM-001/MEM-002. The *other* parts of that item — safe references as the default, heap-allocated strings/collections — landed as `&T`/`&?T` (LANG-002) and GC-heap strings/collections (MEM-001). |
| Bounds-check elimination on `ValueRangePass` | previous FUTURE item 6, `DESIGN_NOTES.md` §1 | OPT-002 (proven elision, INV-OPT-2). |
| `CallGraphPass::runGlobal()` stub | previous FUTURE item 6 | OPT-002 deleted `CallGraphPass`. |
| `Map`/`Set` hashing (§27 D4) | §27 | STD-003 item 6. |
| Postfix `++`/`--` and overloads (§27 D21) | §27 | LANG-001 (decided and implemented). |
| `List.get`/`set`, generic `Map`/`Set` equality, generic sort | `LIMITATIONS.md` | LANG-001, TYPE-002, STD-003 (and OVH-003 for generic `get`/`set`). |
| "Turflang 2.0.0 = self-hosting" | previous FUTURE | Renamed: 2.0.0 is the first public release; self-hosting is F-10. |

**Moved into 2.0** (for traceability; see ROADMAP Part II): D7 (scoped), D11
(partly), D13, D14, D15, D17 (decision), D20 (decision), D23, D25.

---

## E. Rejected — not backlog

Everything in `ARCHITECTURE_OVERHAUL.md` §25 and `docs/development/FREEZE.md`
remains rejected; moving any of it here requires an explicit decision and a
change record (§27.4). In particular: no trait/constraint system, no hybrid
generics, no moving or generational collector, no write barriers "in
preparation", no finalizers, no merged heaps, no unsigned integer types, no
new integer widths or literal suffixes, no fat `string`, no Model B
inference, no parameter/return-type inference, no macros or user-facing
`comptime`, no inheritance or dynamic dispatch, no runtime written in
TurfLang, operator resolution stays exact-match, `&&`/`||`/`=`/`()`/`.`/`to`
are never overloadable, no LTO/PGO (§17.5 — revisit only if F-7a creates
something for them to act on).
