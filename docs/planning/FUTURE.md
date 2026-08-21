# Turflang: Beyond 1.0.0

`ROADMAP_v1.0.0_COMPLETE.md` is done — every phase it tracks is complete (Phase 7
excepted, see below). This document picks up where it leaves off: everything
real that's still open, in the order it's worth tackling, split by which
major version it belongs to.

For long-form design rationale behind the stdlib/memory/CFG/paradigm items
below, see `docs/planning/DESIGN_NOTES.md` — that document is not duplicated
here, only sequenced and summarized. This document owns two things
`DESIGN_NOTES.md` doesn't already cover in full: the concrete `@slm { }`
in-source syntax implementation sketch, and self-hosting.

---

## Turflang v1.x (post-1.0.0, pre-self-hosting)

Suggested order. Items 1–2 have no open design questions and are safe to
start immediately; items 3 onward either depend on earlier items or carry
real open design questions worth resolving before implementation starts (the
same discipline `FINAL_SPRINT.md` applied to Phase 4's `unsafe {}` track).

### 1. Developer Tooling (`ROADMAP_v1.0.0_COMPLETE.md` §8, unchanged)

LSP server (`turf --lsp`), `turf test`, `turf fmt`, `tpm`. Purely mechanical,
no dependencies on anything below. Do this first — it's what makes a tagged
1.0.0 pleasant to actually build things with day to day, and nothing else in
this document blocks on it or is blocked by it.

### 2. `@slm { }` In-Source Syntax (Prompt-Oriented Programming core)

The 1.0.0 final sprint deliberately narrowed Phase 5 to just the `--smart`
retry loop (done). This is the rest of Phase 5 as originally specced —
letting a Turf program itself contain a natural-language block the compiler
turns into real AST nodes, not just SLM-assisted error healing:

- **`@slm` Syntax & Parser** — new `TOK_SLM_BLOCK`, `SLMBlockExprAST` node,
  parsed from `@slm { "prompt" }`.
- **Structured Context Serialization** — `serializeSymbolContext()` extracts
  the current function signature and all visible variables/types from
  `ResolverSymbolTable` to build the prompt prefix the model sees.
- **The SLM Query Loop** — `SLMBlockExprAST::codegen()` invokes `SLMEngine`,
  the returned string passes through `Lexer → Parser → Resolver →
  TypeChecker`, retrying up to 3× on TypeChecker failure by feeding the
  error back into the prompt. This is the same retry *shape* the `--smart`
  path already ships (`SLMIntegration.cpp`'s `kMaxSlmAttempts` loop,
  `docs/planning/ROADMAP_v1.0.0_COMPLETE.md` §7) but operating on a block-codegen
  target instead of a single suggested line — expect to share the retry
  scaffolding, not duplicate it.

See `DESIGN_NOTES.md` §7 ("Prompt-Oriented Programming") for the
higher-level pitch and how this composes with the other two SLM-driven
paradigm sketches there (self-healing compiler, semantic interface
matching) — those two are further out and speculative; this one has a
concrete implementation path already scoped above.

### 3. LoRA Fine-Tuning (Adapter A/B)

Full dataset/training/tooling plan already written up in `DESIGN_NOTES.md`
§6 — not repeated here. Sequencing note: Adapter A (syntax & instant fixes)
already improves today's `--smart` path and doesn't depend on item 2 above;
Adapter B (AST generation) is specifically for `@slm` block codegen and only
becomes useful once item 2 ships. Fine reasonable to start Adapter A's
dataset pipeline in parallel with item 2's implementation.

### 4. Standard Library Expansion

Full module-by-module gap analysis and priority tiering already in
`DESIGN_NOTES.md` §3 (`strings::split`, file I/O, `os` expansion, `time`,
`Set<T>`, sort/search, etc.) and §4 (`http.tr`/`json.tr` on top of the now-
real `net.tr` transport). Not repeated here — that tiering already reflects
what's actually blocking real programs vs. speculative.

`docs/planning/LIMITATIONS.md` (new, 2026-08-22) tracks the confirmed,
still-open generics gaps found while finishing that stdlib work for
1.0.0 — `Map<K, V>`/`Set<T>` generic equality not dispatching to
`operator==` for `string` keys/items, the same gap blocking a generic
`List<T>` comparator/sort, and `List<T>` missing `get`/`set` indexing
methods. Prioritized there; start with `List.get`/`set` (no blocker) and
`Map`/`Set` string equality (highest value, probably wants a trait/
constraint system as its real fix).

### 5. Memory Architecture (ARC)

`DESIGN_NOTES.md` §5 in full: automatic reference counting, safe references
replacing raw pointers as the default, heap-allocated mutable strings/
arrays, and the `unsafe_ptr<T>` FFI bridge. Explicitly a memory-*model*
change (compiler + Codegen work, not a stdlib addition) — the narrow
`unsafe {}`/`&T` track that shipped for 1.0.0 (`ROADMAP_v1.0.0_COMPLETE.md` §6) was a
deliberate subset of this, not a first step that makes the rest smaller.
Gate real design work on items 1–4 above landing first; nothing here is
urgent enough to preempt them.

### 6. CFG Static Analysis Remainder

`DESIGN_NOTES.md` §1: pointer nullability tracking, real memory-leak
detection (distinct from the already-shipped use-after-free/double-free
tracking), bounds-check elimination on top of `ValueRangePass`, and
`CallGraphPass::runGlobal()` (still a literal stub). Independent of
everything else in this document — safe to interleave whenever.

### 7. SLM-Driven & Other Language Paradigms (speculative)

`DESIGN_NOTES.md` §7 (self-healing compiler, semantic interface matching —
POP itself moved to item 2 above, concrete) and §8 (extreme compile-time
execution via ORC JIT, region-based memory management as an ARC
alternative, reversible execution). Pure design sketches, no code behind
any of them yet, no urgency — revisit once items 1–6 are further along.

---

## Turflang 2.0.0: Self-Hosting & Bootstrapping

*Decided 2026-08-21: big enough, and independent enough of everything above,
to be its own major-version milestone rather than a phase tacked onto the
1.0.0 roadmap — moved here from that document's former Phase 7 section
verbatim in substance.*

The final milestone: writing Turflang in Turflang.

**Requirements for Self-Hosting** (both already satisfied today):
- Phase 3 complete — specifically **`extern fn` FFI**, to bind LLVM's C API.
- Phase 4 complete — specifically **`std/io.tr`**, to read/write source
  files and object files.

**Bootstrap cycle:**
1. Write new compiler logic using *existing* Turflang features.
2. Compile using the *current* compiler binary.
3. Output is a new binary that understands the new feature.
4. Use the new feature in everyday Turflang programs.

**SLM in the Self-Hosted Compiler:** The `@slm { }` syntax (item 2 above) is
for user programs. The compiler's own SLM calls become ordinary `extern fn`
FFI calls binding `llama.cpp`'s C API directly:

```turflang
extern fn llama_backend_init(): bool;
extern fn llama_model_load_from_file(path: string, params: unsafe_ptr<void>): unsafe_ptr<void>;
extern fn llama_decode(ctx: unsafe_ptr<void>, batch: unsafe_ptr<void>): int;
```

Realistically this waits until most of the v1.x list above has landed —
self-hosting a compiler that's still missing basic dev tooling, a stable
stdlib, and its own headline language feature (`@slm {}`) would mean
re-doing that work twice.
