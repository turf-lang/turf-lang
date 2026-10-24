# TurfLang 2.1 → 3.0.0 — Roadmap

> **Status: plan of record**, finalized 2026-10-15 on `release/post-2.0.0`
> (`VERSION` = `2.0.1-dev`) from the proposal of the same date. It replaces
> `ROADMAP_v2.0.0.md` (now in `docs/development/`, kept as the record of 2.0) as
> the forward plan. `FUTURE.md` keeps only what this document leaves
> unscheduled. `FREEZE.md`, `INVARIANTS.md` and the 2.0 change records describe
> what 2.0.0 shipped; they are background here, not gates. The 2.x compatibility
> promise is enforced by the 2.0 test corpus (V-4), not by a document.
>
> **Item ids are stable.** `F-` ids come from `FUTURE.md`; `V-`, `AN-`, `RT-`,
> `CC-`, `DX-`, `SL-`, `NM-`, `LG-` and `B-` ids from the proposal; `PC-`, `LB-`,
> `PK-` and `IF-` are added here for the 3.0 aims. Ids are never renumbered:
> moving an item to another version moves its row, not its id.
>
> **Reading order:** Part 1 (the aims) → Part 2 (what ships in which version) →
> Part 3 (the TDPs) → Part 4 (rules every change follows: syntax, Constitution,
> repositories, documentation) → Part 5 (the versioning contract) → Part 6 (the
> item catalogue) → Part 7 (3.0.0 and the end of 2.x) → Part 8 (decisions).

---

## Part 1 — The aims

### 1.1 2.x: grow without breaking

Every 2.x minor is additive: a program that compiled and ran on 2.0.0 compiles
and runs the same on every 2.x (Part 5). Small, non-breaking changes come first
(2.1), larger features follow (2.2 … 2.7), and the last minor (2.8) prepares
the break. Everything that cannot be done without breaking a program goes into
Part 7's register and waits for 3.0.0.

### 1.2 3.0.0, aim A: a pure compiler

`turfc` today is a compiler and several tools in one binary (`turfc help`):
it compiles, but it also formats (`--format`), migrates (`--migrate=pub`), runs
test suites (`--test`), explains codes (`--explain`) and hosts a language model
(`--smart`, `--smart-write-fix`, the vendored llama.cpp and the separate
`-smart` release flavour). `turf-lsp` builds the compiler from a source checkout
(`add_subdirectory` of `turf-lang`), and global state in the lexer, the parser
and the symbol table forces the `--smart` sandbox to save and restore it.

At 3.0.0, `turfc` is a compiler and nothing else:

- **One job.** Source plus a package graph in; an executable, object or LLVM IR
  and diagnostics out. Formatting, migration, test running, code explanations
  and model assistance live in their own tools, which `turf` invokes (PC-3, PC-2).
- **One library.** The front end is `libturf` (F-S5, PC-1): reentrant (F-S0 …
  F-S2), with a `SemanticModel` (F-S3) and a lowering IR (F-S4) between the
  syntax tree and LLVM. `turfc`, `turf-lsp`, the formatter and the assistant
  link it through a versioned API instead of compiling the compiler's sources.
- **Deterministic.** The same inputs give byte-identical output (PC-4); no model
  and no network is ever in the compilation path (the Constitution's
  Non-Goal).
- **One release flavour.** With the model runtime out of `turfc`, the plain and
  `-smart` builds become one; the assistant is a separate download.

The 2.x minors do every additive step (the internal F-S stages, `libturf`, the
new tools beside the old flags, deprecation notices). 3.0.0 only removes the old
flags and the `-smart` flavour (B-14, B-15).

### 1.3 3.0.0, aim B: libraries you can trust, and a way to ship them

A Turf library should be able to wrap C, keep every `unsafe` line inside itself,
present a safe Turf API, and ship as source through a registry, with the build
and the project managed by `turf`. What exists today: `extern fn`, `unsafe { }`,
the per-file `unsafe module;` capability honoured per package through
`[unsafe] allow` in `turf.toml`, `--link <libname>`, path and git
dependencies, `turf.lock` with tree hashes, `turf graph`, and a reserved
`turf publish`. What is missing:

| Need | Items | Version |
|---|---|---|
| See where the unsafe code is | LB-2: per-package report of `unsafe module` files, `unsafe { }` blocks and `extern fn`s | 2.1 |
| Write fewer unsafe lines | LB-3: boundary contracts on `extern fn` parameters (nullability, length pairs, ownership) that the compiler turns into checks, so many bindings need no hand-written `unsafe` | 2.6 |
| Build C with the package | LB-1: a `[native]` table in `turf.toml` for C sources and system libraries | 2.6 |
| Catch a wrapper that leaks unsafety | LB-4: a raw pointer in a package's `pub` API; LB-5: method calls through a raw receiver (a known gap in `pointers_and_references.md` §1.2). Warnings in 2.x | 2.2, 2.6 |
| Test the unsafe core | RT-5: opt-in runtime checks for the manual heap; `turf test --debug-checks` | 2.5 |
| Ship and depend | PK-1 … PK-4: registry, semver ranges, `turf publish` with checks, workspaces, generated package docs | 2.6 |
| Make it the rule | B-12, B-13: LB-4 and LB-5 become errors; B-2: RT-5 on by default at `-O0` | 3.0 |

**"Guarantee safety" means this, and no more.** The compiler cannot prove C
code correct. The guarantee 3.0 makes is: *safe Turf code cannot corrupt memory,
including through a library's API; only code marked `unsafe` (in Turf or in C)
can, and that code is confined to packages that declare it, counted by `turf`,
allowed by name in the consumer's manifest, and checked at run time in test
builds.* A memory error in a program therefore always traces back to a named
unsafe region in a named package. This is the Constitution's Principle I
(safe by default, unsafe by declaration) and Principle IV (nothing crosses a
boundary implicitly) applied to packages; no amendment is needed.

"The purest way possible" to ship is **source**: a published package is a
content-hashed source tree (Turf and C) plus its manifest, built by the consumer
with the consumer's compiler. No prebuilt binaries are published; a
Turf-to-Turf binary ABI (F-7b) stays unscheduled.

### 1.4 The end of 2.x

3.0.0 is followed by bug-fix and soundness patches to the last 2.x minor for six
months (D-8), and then 2.x is discontinued: no further 2.x releases, the
package recipes (Homebrew, Scoop, AUR) carry 3.x only, the website's 2.x
documentation moves to an archived section, and the registry accepts new
publishes only from packages whose `turf` requirement includes 3.x (7.3).

---

## Part 2 — What ships in which version

**How versions are assigned.** Each 2.N line has a theme. Items ship in that
line's minor `2.N.0`; `2.N.P` patches carry bug fixes and documentation only
(5.1). If an item is not ready when its line is tagged, the line ships without
it and the item's row moves to a later line in this document (its id does not
change). Stdlib additions (F-19) and supported platforms (F-20) are continuous:
any line may carry them under the stdlib rules of 5.2.

**The architecture track** (F-S0 … F-S5, PC-1, PC-4) is internal and
unobservable (5.2's internals row), so it runs alongside the user-visible
themes and is listed in each line's "Internal" row.

| Line | Theme | Size of change | TDPs |
|---|---|---|---|
| **2.0.x** | Bug fixes found in 2.0.0 | patches | — |
| **2.1.x** | Contract and quick wins | small, additive | 2-003, 2-004, 2-005, 2-006 (2-002 in part) |
| **2.2.x** | Diagnostics you can trust; analysis I | medium, warn-first | 2-001, 2-002 (rest), 2-007, 2-008 |
| **2.3.x** | Closures and generics | large | 2-009, 2-010 |
| **2.4.x** | Iteration, views, patterns, errors | large | 2-011, 2-012, 2-013, 2-014 |
| **2.5.x** | Runtime and memory checks | large, internal | 2-015 |
| **2.6.x** | Packages and native code (aim B) | large | 2-016, 2-017, 2-018 |
| **2.7.x** | Tooling and the pure compiler (aim A, additive half) | large | 2-019 |
| **2.8.x** | Ready for 3.0 (last 2.x minor) | small | 2-020 |
| **3.0.0** | The breaking register (Part 7) | breaking | 2-020 |
| **3.x** | Concurrency, then what follows | large | 3-001, 3-002 |

### 2.1.x — Contract and quick wins

Everything later relies on this line: lint levels before the first new warning,
the compatibility corpus before the first feature, the JSON diagnostics before
any change to default output.

| Area | Items |
|---|---|
| Contract tooling | V-1 lint levels, V-2 `@deprecated`, V-3 prelude-shadow check, V-4 `tests/compat/2.0/`, V-5 diagnostic-code registry |
| Diagnostics | SL-0 provenance labels (the `[Model Suggestion]` label landed), DX-1 `Fix:` lines (landed), DX-3 hint backfill, DX-2 `--diagnostics=json` (open) |
| Numeric speed | NM-1 baseline benchmark, NM-3 `math::wrapping_*`, NM-4 trap-block measurement |
| Generics at use sites | LG-1 `List<int>::default()`, LG-2 `f::<int>(…)`, LG-7 hints that point at them |
| Small language and runtime | F-11d `@test` functions with their outcomes (`compile_error`, `warning`, `no_warnings`, `panic`, `exit`, `error`), RT-6 stack-overflow depth hint, SY-1 `for c in s`, SY-2 (a) integer literal receivers documented and tested |
| Libraries | LB-2 unsafe-surface report in `turf` |
| Repositories | IF-1 interface manifest, IF-5 consumer fixtures in the compiler's CI |
| Documentation | `docs/development/DOCS_BACKLOG.md` and the per-minor documentation gate (4.4) in force |
| Internal | F-S0 `LexerState`, F-S1 `ParserState` |

### 2.2.x — Diagnostics you can trust; analysis I

| Area | Items |
|---|---|
| Fixes without a model | SL-7 rule-engine coverage beyond the 18 codes DX-1 shipped with in 2.1 |
| Measured `--smart` | SL-3 corpus, SL-2 scoreboard, SL-1 Fix Score v2, SL-5 suggestion checks, SL-4 model checks, SL-6 prompt context (in that order) |
| Analysis (warnings only) | AN-1 `unsafe` checks, AN-2 loops, AN-3 (a)(b) recursion, AN-4 nullability, AN-5 manual-heap leaks, AN-6 precision lint, AN-7 dead code, LB-5 raw-receiver method calls |
| Numeric speed | NM-2 proof-based elision over ranges |
| Repositories | IF-2 versioned schemas for diagnostics and package-graph JSON; IF-3 website error pages generated from V-5 |
| Syntax | SY-2 (b) decimal-literal receiver warning, SY-3 `function`, SY-4 `static fn`, SY-5 `pub immut`, each old spelling with a warning and a `Fix:` |
| Internal | F-S2 no global symbol table, F-S4a analysis leaves Codegen, F-11e the compiler's own tests move to `@test` expectations |
| Tests | F-11f test options (TDP-2-026) |

The AN warnings and LB-5 land here so that each has at least two minors in the
field before 3.0 can promote it (B-1, B-13).

### 2.3.x — Closures and generics

| Area | Items |
|---|---|
| Closures | F-1 phases 1–2 (capture-free, then `{code, env}` with captures), G-1, B-7 decided in phase 1 |
| Inference | LG-3 struct type arguments from constructor arguments, LG-4 return-only type parameters, LG-6 `List::from` / `Set::from` |
| Internal | F-S3 `SemanticModel` |

### 2.4.x — Iteration, views, patterns, errors

| Area | Items |
|---|---|
| Closures in use | F-1 phase 3 (stdlib adoption), LG-5 lambda types from context |
| Collections | F-2 iteration over collections, F-3 storable `Slice<T>` |
| Patterns | F-14 `compare` expressions, range and nested or-patterns; F-14b enum payloads |
| Errors | AN-9 error return traces, F-15b closed error sets |
| Modules | F-16 circular imports |
| Internal | F-S4b one CFG, F-S4c TIR |

### 2.5.x — Runtime and memory checks

| Area | Items |
|---|---|
| Collector | RT-1 better algorithms inside the non-moving design, RT-3 per-thread allocation buffers |
| Checks | RT-5 opt-in runtime checks (`--debug-checks`, `turf test --debug-checks`), AN-8 panic backtraces, AN-3 (c) opt-in `pedantic` recursion heuristic (uses TIR's call graph) |
| Closures | F-1 phase 4 (stack-allocated non-escaping environments, on TIR) |
| Internal | F-S4d Codegen from TIR begins |

### 2.6.x — Packages and native code

| Area | Items |
|---|---|
| Packages | PK-1 registry, semver ranges and `turf publish`; PK-2 workspaces; PK-3 publish checks; PK-4 package documentation (with F-11c) |
| Native code | LB-1 `[native]` C sources and libraries; LB-3 checked boundary contracts on `extern fn` |
| Safe APIs | LB-4 raw pointers in a package's `pub` API (warning) |
| Build speed | F-7a separate compilation |
| Internal | F-S4d continues |

### 2.7.x — Tooling and the pure compiler (additive half)

| Area | Items |
|---|---|
| Library | PC-1 `libturf` (F-S5) with a versioned API; IF-4 `turf-lsp` links it from a release |
| Tools | PC-2 the assistant leaves `turfc`; PC-3 format, migrate, test and explain as their own tools; the matching `turfc` flags print a deprecation note |
| Determinism | PC-4 reproducible output, checked in CI |
| Editor | F-11 LSP completion, references, rename, code actions; F-11b trivia-preserving formatter; DX-4 `turf fix` applies validated fixes |
| Internal | F-S4e the AST code generator is retired |

### 2.8.x — Ready for 3.0

The last 2.x minor adds no feature. It makes the upgrade a non-event:
`lint-set = "3.0"` reports every B-item as an error on a 2.8 compiler; `turf
migrate 3.0` performs every mechanical migration; every deprecation that 3.0
removes has warned for at least two minors; the 3.0 migration guide is
published on the website; TDP-2-020 is accepted.

### 3.0.0

The register in Part 7: the pure compiler's removals (B-14, B-15), the
safe-library rules (B-12, B-13), the promoted warnings (B-1), debug checks on at
`-O0` (B-2), deprecated items removed (B-3), old lint-set pins retired (B-10).
The others (B-4 … B-9, B-11) are expected to be empty.

### 3.x

3.0.x patches; then additive minors again under Part 5, with `3.N` in place of
`2.N`.

**Concurrency is the first 3.x theme** (D-22). It is additive, so it does not
need a major, and it builds on the reentrant `libturf` and the runtime work of
2.5 instead of racing them. Its design must stay additive inside 3.x: a rule
that would reject a program which compiles on 3.0 waits for 4.0 (B-8), so
TDP-3-001's checks apply only to code that starts a thread.

| Line | Items |
|---|---|
| 3.1 | CC-0 design accepted (TDP-3-001); CC-1 thread-aware runtime with a single-thread fast path; CC-2 `thread` module |
| 3.2 | CC-3 `sync` module, CC-4 async (library first), CC-5 non-blocking I/O, CC-6 TLS and UDP (TDP-3-002) |

Other candidates: RT-2 GC tier 2 (after threads), F-10 self-hosting, the
assistant's own roadmap (SL-* beyond 2.2, F-9's LoRA adapters), more platforms.

**On no line, by design:** F-6c is LG-1/LG-2, F-7c is PK-1 and RT-4 is NM-1 …
NM-6 (aliases); NM-5 waits for a measurement of its own and NM-6 for an LLVM
that vectorizes it (measured 2026-10-25); RT-2′ is chosen only if RT-2 is
measured not to work (it would be B-4, so 4.0); SL-8 is dropped (4.2); F-7b and
F-25 stay unscheduled.

---

## Part 3 — Turf Design Proposals

A TDP is the public design for anything a user can see: syntax, semantics, a
stdlib module, a CLI command or a file format. TDPs live on the website
(`website/src/content/tdp/M-NNN-slug.md`, from `_template.md`), which is their
only copy; this repository cites them by number. Internal items (kind `I`)
need a change record (`docs/development/changes/<ID>.md`), not a TDP. A new
function in an existing stdlib module needs a change record and the API lock
(5.2), not a TDP; a new module needs a TDP.

A TDP is accepted before its line opens and set to `implemented` in the release
that ships it.

**Numbering: `TDP-<major>-<NNN>`.** `<major>` is the major version line the
proposal first ships in; `<NNN>` counts from 001 within that major, assigned
when the TDP is written, with no gaps and never reused. A proposal that ships in
2.x and finishes in 3.0 (TDP-2-018, TDP-2-019, TDP-2-020) keeps its 2-number.
The website lists proposals in ascending order (TDP-2-001, TDP-2-002, …,
TDP-3-001, …) at `/tdp/<major>-<NNN>/`. The numbers below assume the TDPs are
written in this order; this table is updated if they are not.

| TDP | Title | Items | Line |
|---|---|---|---|
| TDP-2-001 | Fixes in diagnostics without `--smart` (landed in 2.1) | DX-1, SL-0, SL-7 | 2.1 (DX-1, SL-0 label), 2.2 (SL-7) |
| TDP-2-002 | Checked arithmetic and auto-vectorization (draft) | NM-1 … NM-6 | 2.1, 2.2 |
| TDP-2-003 | Lint levels, deprecation and the 2.x compatibility contract | V-1, V-2, V-4, V-5, B-10 | 2.1 |
| TDP-2-004 | Machine-readable diagnostics and applying fixes | DX-2, IF-2 (diagnostics schema), DX-4 | 2.1, 2.7 |
| TDP-2-005 | Type arguments at the use site | LG-1, LG-2, LG-7 | 2.1 |
| TDP-2-006 | Tests in source files | F-11d | 2.1 |
| TDP-2-007 | Analysis warnings for unsafe code, loops, recursion and leaks | AN-1 … AN-7, LB-5 (warning) | 2.2 |
| TDP-2-008 | Measured suggestions: the Fix Score and the benchmark | SL-1 … SL-6, DX-3 | 2.2 |
| TDP-2-009 | Closures and function values | F-1, G-1, B-7, LG-5 | 2.3, 2.4, 2.5 |
| TDP-2-010 | Generic inference from arguments and return types | LG-3, LG-4, LG-6 | 2.3 |
| TDP-2-011 | Iteration over collections and storable slices | F-2, F-3 | 2.4 |
| TDP-2-012 | Richer patterns and enum payloads | F-14, F-14b | 2.4 |
| TDP-2-013 | Error traces, closed error sets and panic backtraces | AN-8, AN-9, F-15b | 2.4, 2.5 |
| TDP-2-014 | Circular imports | F-16 | 2.4 |
| TDP-2-015 | Runtime debug checks | RT-5, B-2 | 2.5, 3.0 |
| TDP-2-016 | Native code in packages and checked boundaries | LB-1, LB-3 | 2.6 |
| TDP-2-017 | The package registry, publishing and workspaces | PK-1 … PK-4, F-7c, B-9 | 2.6 |
| TDP-2-018 | Safe library interfaces | LB-2, LB-4, LB-5, B-12, B-13 | 2.6, 3.0 |
| TDP-2-019 | A pure compiler: `turfc`, `libturf` and the assistant | PC-1 … PC-4, IF-4, B-14, B-15 | 2.7, 3.0 |
| TDP-2-020 | TurfLang 3.0.0 and the end of 2.x | Part 7, B-1, B-3, D-8 | 2.8, 3.0 |
| TDP-2-021 | Iterating over a string | SY-1 | 2.1 |
| TDP-2-022 | Methods on number literals | SY-2, B-18 | 2.1, 2.2, 3.0 |
| TDP-2-023 | `function` as a spelling of `fn` | SY-3, B-19 | 2.2, 3.0 |
| TDP-2-024 | `static` before `fn` | SY-4, B-20 | 2.2, 3.0 |
| TDP-2-025 | `pub immut` replaces `export immut` | SY-5, B-21 | 2.2, 3.0 |
| TDP-2-026 | Test options | F-11f | 2.2 |
| TDP-3-001 | Threads and shared state | CC-0 … CC-3, B-8 | 3.1, 3.2 |
| TDP-3-002 | Async and non-blocking I/O | CC-4, CC-5, CC-6 | 3.2 |

---

## Part 4 — Rules every change follows

### 4.1 New syntax: easy to read, safe to run

A TDP that adds syntax must show both, and a spelling that fails either is
rejected however convenient it is:

- **Easy to read.** A reader who has not seen the feature can tell what the
  code does and where each part ends. One spelling per idea (LG-1 reuses the
  type-position `<…>` rather than adding `List:int`); words over symbols where
  a symbol would be new; no spelling whose grouping is undecidable by eye. A new
  word is a contextual keyword (5.2), never a new reserved word.
- **Safe to run.** The new form cannot introduce undefined behaviour or a path
  around a check in safe code; every check the long-hand spelling has, the new
  spelling has (LG's "same monomorphized instance" gate, 6.10). A form that can
  bypass a check is spelled with `unsafe` or is not added.

The TDP's Design section shows the feature next to the long-hand program it
replaces, and its Verification section shows that the two compile to the same
checks.

### 4.2 The Constitution

Every TDP has the template's Constitution check, principle by principle, plus
the three Non-Goals. The Constitution is amended only when a feature that is
clearly needed cannot be designed within it, and then through a TDP of its own;
the default answer to a conflict is to reshape or drop the feature. Nothing in
this roadmap needs an amendment. One item changes because of it: SL-8 (`@slm { }`
blocks, model-generated code entering a program at compile time) is dropped,
since it puts a model in the compilation path, which the Non-Goals rule out; a
model may suggest source through the assistant, and a person commits it.

### 4.3 Repositories become more independent with every version

The projects are `turflang` (compiler, runtime, stdlib), `turf-cli` (`turf`),
`turf-lsp`, `turf-fuzz` and `website`. The goal is that a change in one rarely
needs a change in another, so development runs in parallel. Today
`turf-lsp` compiles the compiler from a sibling checkout, while `turf-cli` and
`turf-fuzz` already depend only on a `turfc` binary (the fuzzer pins it in
`compiler.lock`).

Every line must leave the coupling no tighter than it found it, and these items
loosen it:

| ID | Item | Line |
|---|---|---|
| **IF-1** | **Interface manifest.** `turfc --version --json` reports the driver interface major/minor, the diagnostics-JSON schema version, the package-graph schema version and a feature list. `turf`, `turf-lsp` and `turf-fuzz` check it instead of assuming a compiler revision. | 2.1 |
| **IF-2** | **Formats are versioned documents.** Diagnostics JSON (DX-2), the package graph (`--package-graph`), `turf graph`'s output and the registry index (PK-1) each have a published schema and fixture files. Consumers test against the fixtures; a producer may add fields in a minor and changes a meaning only with a schema major. | 2.2 onward |
| **IF-3** | **Generated reference.** The website's error pages come from V-5's registry and its stdlib reference from F-11c, both shipped in the release archive, so the website builds from a released compiler instead of a compiler checkout. | 2.2 (errors), 2.6 (stdlib) |
| **IF-4** | **`turf-lsp` links `libturf`** (PC-1) from a release through its versioned API, not by `add_subdirectory` of the compiler's sources. | 2.7 |
| **IF-5** | **Consumer fixtures live in the producer's CI.** The compiler's CI runs each consumer's fixture tests (`turf`'s driver expectations, the LSP's API tests, the fuzzer's oracles against the IF-2 schemas), so a compiler change that would break a consumer fails in the compiler repository, and consumers test against the latest release plus a nightly. | 2.1 onward |

A change that still needs coordinated edits in two repositories says so in its
change record and lands in both in the same line, with an IF item proposed to
remove the need next time.

### 4.4 Documentation ships with the change

1. **Right away.** A change that alters behaviour updates the documentation in
   the same pull request: `docs/reference/`, `docs/architecture/`,
   `docs/errors/` here, and the matching website page in a pull request opened
   the same day. A change that needs internal work in `turf-lsp`, `turf-fuzz` or
   `turf-cli` updates that repository's documentation (`README.md`,
   `ARCHITECTURE.md`, `DESIGN.md`, the website's tooling pages) when the work
   lands, not later.
2. **The backlog.** Anything that could not be documented in the same change
   gets a row in `docs/development/DOCS_BACKLOG.md` (what, where, which
   repository, target line) at once. A row is deleted when the page is written;
   the history is in git.
3. **Every minor is a documentation release.** `2.N.0` is not tagged while the
   backlog has a row targeting `2.N`. The release checklist (5.5) covers: this
   repository's reference pages, the website's language, stdlib, tooling and
   error pages, each shipped TDP set to `implemented`, a release post on the
   website's blog, the `turf`, `turf-lsp` and `turf-fuzz` READMEs where their
   behaviour changed, and the migration guide for any deprecation.
4. **Documents describe the present.** Reference pages say how things are; what
   changed and when belongs in git, change records and release posts.

## Part 5 — The versioning contract

### 5.1 What a number means

| Bump | Contents | Compatibility promise |
|---|---|---|
| **2.N.P** (patch) | Bug fixes, documentation, packaging. No new language surface, no new stdlib API, no new diagnostics except to fix a wrong one. | A 2.N.0 program behaves identically. |
| **2.N.0** (minor) | Additive features, new warnings, new stdlib API, performance, internal re-architecture. Anything on the list in 5.2. | Every program that compiled and ran correctly on 2.(N-1) compiles and runs with the same observable behaviour, apart from the exceptions in 5.3. |
| **3.0.0** (major) | Anything that fails the minor promise (Part 7's register). | Shipped with a `turf --migrate=…` for every mechanical break, as `--migrate=pub` was for 2.0. |

The lines 2.1 … 2.8 and their themes are fixed in Part 2; there is no
calendar. An item that misses its line moves to a later one. `3.0.0` follows
2.8, or comes earlier only if a soundness hole cannot be closed within the
limits of 5.3 (a).

### 5.2 What a minor may do (verified additive)

| Change | Allowed in a minor if |
|---|---|
| New **syntax** | It cannot change the parse of any existing program. A new word is a **contextual keyword** (meaningful only in one grammatical position, so an identifier called `async` or `await` keeps working); it is never a new reserved word. Checked 2026-10-15 with `turfc --check`: a program declaring locals named `async`, `await`, `spawn`, `thread`, `yield`, `select` and `lock` compiles, while the same program with `try` (a real reserved word) is rejected with E108. Users may already be using those names. |
| New **stdlib API** | It is a new `pub` item in an existing module or a new module, recorded in `tests/46_stdlib_surface/` + `scripts/check/stdlib_surface_check.sh` (F-API-1: existing items are never removed, renamed or re-signatured before 3.0). A **new module is not added to the prelude** (§6.25). Adding a bare-callable name to one of the three bare-callable prelude modules can shadow or collide with a user name, so it needs the check in V-3. |
| New **warning** | See 5.4. |
| New **error** | **Never** for a program that compiled, except 5.3. |
| **Runtime** change | It is unobservable by a correct program: same output, same exit statuses (§6.21), same panic text class, stable object addresses (§6.5). The GC algorithm, allocator, thread-safety of internals, and speed are all changeable under this rule. |
| New **diagnostic output** (a `Fix:` line, a `--diagnostics=json` form, a changed `--smart` label) | It is additive text or a new flag. Tools should read DX-2's JSON, not the text, once it lands; DX-1's `Fix:` lines shipped first (2.1.0, owner decision 2026-10-16). |
| **Compiler internals** | Diagnostic **codes** (`E###`, warnings included) and exit statuses are the contract; message text and diagnostic order are not (decision D-6). |
| **Driver interface** | `TurfcInterfaceMinor` bumps for a new flag or input; `TurfcInterfaceMajor` only in a major . `turf` and `turfc` still version independently. |

### 5.3 The only two exceptions to "never reject a program that compiled"

(a) **Soundness.** A minor may reject (or change the behaviour of) a program
only to close a hole where *safe* code corrupts memory, crashes the compiler
or silently computes a wrong answer (the R2 rule of the 2.0 roadmap).
Requirements: a change record, owner sign-off (Part VI already escalates "a bug
whose only fix changes frozen semantics"), a warning in the previous minor when
the hole can be detected conservatively, and a line in the release notes.
(b) **Programs that relied on undefined behaviour in `unsafe` code.** A newly
added check may panic where the program previously corrupted memory silently,
but only under an opt-in mode (RT-5) in a minor. Default-on is a 3.0 item (B-2).

### 5.4 The warn → deny ladder (how analysis improves without breaking)

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

### 5.5 Mechanics each minor must satisfy

1. **Compatibility corpus.** The 2.0.0 test tree is kept as an immutable
   `tests/compat/2.0/` (copy, not move) and must pass byte-identically on every
   2.x compiler. A minor that edits a test to make it pass is a 3.0 change and
   CI says so (extends `tests/46_stdlib_surface/`'s idea from API to behaviour).
2. **API lock diff** in the release notes: added `pub` items, added diagnostic
   codes, added flags, new `TurfcInterfaceMinor`.
3. **Performance floor.** REL-005's baseline (`docs/development/BENCHMARKS.md`)
   is not regressed for single-threaded programs by more than the noise band,
   even when the line adds threads (CC-1's single-thread fast path).
4. **Every full suite gate** the 2.0 release used: `test_runner.sh`,
   `TURF_GC_STRESS` + ASan, the Windows/macOS smokes, `release_deps_check.sh`.
5. **Release procedure:** branch `release/vX.Y.Z`, `VERSION` and `turf`'s
   `version.go`, tag `v*` (publishes via `release.yml`), `gen_recipes.sh` for
   Homebrew/Scoop/AUR, then `VERSION` → next `-dev`, as 140ba12 did.
6. **Records:** an accepted TDP for every item that adds user-visible surface
   (Part 3), one **change record** per item
   (`docs/development/changes/<ID>.md`), an **INVARIANTS.md** row for every new
   guarantee.
7. **Documentation gate** (4.4): no `DOCS_BACKLOG.md` row targets this line;
   website pages, TDP statuses, the release post and the other repositories'
   READMEs are current.
8. **Repository gate** (4.3): the IF-5 consumer fixtures pass, and the change
   records name any coordinated change in another repository.
9. **Support window.** After 3.0.0 the last 2.x minor gets bug-fix patches for
   six months (D-8), then 2.x is discontinued (7.3).

---

---

## Part 6 — The item catalogue

Each item's version line is in Part 2 and its TDP in Part 3; this part says
what each item is.

**Kind:** `A` additive · `W` warning-first (ladder 5.4) · `I` internal
(unobservable, ships under 5.2's runtime/internals rows) · `D` decision needed
before it can be classified · `B` breaking candidate (register, Part 7).
Complexity uses `FUTURE.md`'s scale (XS < S < M < L < XL).

### 6.1 Versioning infrastructure (V — prerequisites for the rest)

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **V-1** | **Lint levels.** Every diagnostic code is `allow`/`warn`/`deny`; set by CLI (`--allow E###`, `--deny …`), by `turf.toml [lints]`, and a `lint-set = "2.N"` pin. **No source syntax.** | A | M | Must land before the first new warning. The manifest lives in the `turf-cli` repository. |
| **V-2** | **Deprecation.** A `@deprecated("use X")` form and a warning on use, honoured by `turf doc` / LSP. Removal only in 3.0 (B-3). | A | S | `@` is already the spelling of `@caller_location`. Contextual, no new reserved word. |
| **V-3** | **Prelude-shadow check.** A script that, for any proposed addition to a bare-callable prelude module, lists every `showcase/`, `examples/`, `tests/` and `lib/std` program that declares the same name. | A | XS | Guards 5.2's stdlib row. |
| **V-4** | **`tests/compat/2.0/` corpus and its CI job** (5.5.1). | A | S | |
| **V-5** | **Diagnostic-code registry.** One file listing every code, its kind (error/warning), the version it appeared in and its level. Generated into `docs/reference/cli_and_diagnostics.md`. | A | S | Without it the API-lock diff of 5.5.2 cannot be automated. |

### 6.2 Analysis and diagnostics (AN)

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

### 6.3 Runtime and GC (RT)

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
| **RT-4** | **Checked arithmetic vs vectorization** (F-24). Specified as NM-1 … NM-6 in 6.9 (draft TDP-2-002). The default stays checked. | I / A | M | `INV-OPT-2` (elision only when proven) is not weakened. |
| **RT-5** | **Opt-in runtime check mode** (`turf build --debug-checks`, building on the existing `TURF_RT_SANITIZE`): red zones around `mem::alloc` blocks, poison on `mem::free`, double-free/invalid-free, `mem::copy`/`mem::set` bounds when the block size is known, uninitialised-read of a manual-heap block. Panics with a named diagnostic, exit 134. | A | M–L | Default off in 2.x. Default on at `-O0` is B-2. |
| **RT-6** | **Stack-overflow headroom and depth reporting.** A recursion-depth hint in the `panic: stack overflow` text. | I | S | |

### 6.4 Language features (from `FUTURE.md` §A)

| ID | Item | Kind | Cx | Depends on |
|---|---|---|---|---|
| **F-1** | **Closures/lambdas**, in the four phases of `FUTURE.md` F-1: capture-free (thin pointers; parser + lowering) → `{code, env}` + captures + type descriptors → stdlib adoption (`List.map/filter`, closure `http` router) → stack-allocated non-escaping environments. | A | L | FDN-001/002 (done); one risk, see B-7 |
| **G-1** | Array-of-function-values spelling. Decide **with** F-1 (element size changes). The *additive* fix is a parenthesised type or a type alias; changing what `fn(int): int[2]` means is B-6. | A / B | S | F-1 |
| **F-2** | **Iteration over collections**: structural protocol (`length()` + `get(i)`) lowering to an index loop; `Map` via `keys()`. Decide protocol vs `next(): !E` vs `each(f)` after F-1 exists. | A | M | F-1 (soft) |
| **F-3** | **Storable `Slice<T>`** (struct fields, returns, collections of views, views into `List<T>` buffers). | A | M | FDN-002's escape analysis; RT-1 block table |
| **F-6c** | **Expression-position type arguments.** Specified as LG-1 (`List<int>::default()`) and LG-2 (`f::<int>(…)`) in 6.10; `f<int>(…)` cannot be made unambiguous. | A | S–M | none |
| **F-14** | `compare` as an expression, range patterns, nested or-patterns. | A | M | none |
| **F-14b** | Payloads on plain `enum`s (sum types), reusing LEC-004's representation. | A | M | none |
| **F-15b** | Closed error sets in signatures, error conversion. | A | M | AN-9 |
| **F-16** | Circular imports. A module-system change, but it only *accepts* programs that are rejected today. | A | M | none |
| **F-11d** | In-source `@test` functions and their outcomes (TDP-2-006): none (returns), `compile_error, E<code>, "text"` (optionally `fix: "text"`), `warning, E<code>, "text"`, `no_warnings`, `panic, "text"`, `exit, <status>`, `error, T::V` (a `!void` test). `compile_error`, not `Error::E041`, so a program's own error type named `Error` is never shadowed (Principle V). | A | S–M | `turf test` (§6.22) |
| **F-11f** | **Test options** (TDP-2-026): `skip: "reason"`, `timeout: <ms>`, `levels: all \| [O0, …]`, `stdout: "…"`, `tag: "…"`, `serial`, after the outcome as `name: value`. Literals only; no retries, no setup hooks. | A | S–M | F-11d |
| **F-11e** | **Test-suite cleanup.** Move the compiler's own `tests/**/fail_*.tr` whose expected error lies inside one function body, and the `EXPECT-RUNTIME` panics, to `@test(Error::…)` / `@test(panic, …)` functions, several to a file; the file-name and `// EXPECT-*` conventions stay for what a function cannot express (syntax errors, declaration-level errors, exit statuses, `EXPECT-FIX`). `tests/compat/2.0/` is never touched. | I | M | F-11d |

### 6.5 Concurrency and async (CC)

`F-8` is XL because nothing is designed for it and because it presses on three
frozen things at once: the collector (§6.5), **safe code cannot corrupt
memory** (the R2 guarantee 2.0 ships on), and **collections are shared, copied
on demand** (§6.27). Threads that share a `List<T>` without a rule would break
the second guarantee in safe code. The traits/constraints system that would
express `Send`/`Sync` is rejected (§E), so the rule has to be something else.
This roadmap therefore writes down **constraints and gates**, and leaves the
design to TDP-3-001, written and decided at CC-0 (3.1).

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **CC-0** | **Concurrency design plan** with an owner decision on the sharing model (D-3). Recommended default: **share nothing by default.** `spawn` takes a function value and a *transferable* argument (a compiler-known structural predicate: primitives, strings, value structs/arrays of transferable parts; a collection or `&T` is deep-copied or rejected); shared state only through stdlib `Mutex<T>`/atomics/channels whose unsafe core is inside `lib/std`. | D | M | Written before any implementation. |
| **CC-1** | **Thread-aware runtime**, no user-visible threads yet: per-thread stack bounds and register capture (extends INV-RT-4 to N threads); stop-the-world handshake (safepoints or signal-based); allocation under RT-3; `panic` and signal handlers thread-safe (INV-RT-5); the **single-thread fast path**: locks and handshakes are compiled out until the first thread starts. | I | XL | RT-1, RT-3, F-S0/S1/S2 (reentrant compiler pipeline is separate, but the same global-state audit applies to `libturfrt`) |
| **CC-2** | `thread` stdlib module: `spawn`, `join`, `sleep`, thread-local storage. Uses F-1 for the entry function; before F-1, a named function plus a transferable argument. | A | L | CC-0, CC-1 |
| **CC-3** | `sync` module: `Mutex<T>`, `RwLock<T>`, atomics (a closed intrinsic list, like the GC's), channels. | A | L | CC-2 |
| **CC-4** | **Async.** Two shapes: (a) a *library* of stackful tasks on the CC-1 runtime, so no new syntax; (b) stackless `async fn` / `await` as **contextual keywords** (5.2). Recommended: (a) first, (b) only if a measured need appears. | A | XL | CC-3, F-1, F-2 |
| **CC-5** | Non-blocking I/O and an event loop (F-12, first half). | A | L | CC-4 |
| **CC-6** | TLS/HTTPS by binding a vetted C library through FFI (F-12, second half); UDP. | A | L | none hard; CC-5 for async use |

### 6.6 Architecture (F-S, from `ARCHITECTURE_SIMPLIFICATION.md`)

Invisible to users by 5.2's internals row: diagnostic codes, exit statuses,
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
This roadmap keeps them separate in *purpose* and recommends **not** adding a
third tree: AST + `SemanticModel` lower directly into TIR, and TIR is where operator
identity, conversions and checks become explicit, so it plays the Bound IR's role.
Revisit only if lowering code is seen re-deriving the same decision in several
places. rustc's HIR → THIR → MIR is the larger precedent; this repository is
smaller than rustc and a third representation is a third thing to keep consistent.

**Interaction with the near-term plan.** NM-2 (range-based elision) and AN-3 (a)
are written to work on today's AST-flag mechanism and migrate to F-S4a's side
table and F-S4c's instructions later; they do not wait for F-S4. F-S4a is the
cheapest first step and is sequenced in 2.2.

### 6.7 Packaging, tooling, platforms, stdlib

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
| **F-9** | SLM: LoRA adapters and corpus growth (SL-3), judged by SL-2's scoreboard; part of the assistant after PC-2. `@slm { }` blocks are dropped (SL-8). F-DIAG-1/2/3 keep their gates. | A | L | PC-2 |
| **F-10** | Self-hosting. | A | XL | F-1, F-7a, a stable 2.x |
| **F-25** | Speculative paradigms (`DESIGN_NOTES.md` §7–8). | D | — | Revisit later; §25 rejects user-facing `comptime`/macros. |

### 6.8 Diagnostics and the SLM (DX, SL)

**What existed before TDP-2-001 landed** (the rule layer is now `src/diagnostics/CompilerFix.*`; this is the 2026-10-15 snapshot; read from `src/slm/`, `scripts/slm/`, and
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
| **DX-1** | **Fixes in diagnostics without `--smart`** (draft **TDP-2-001**). A `Fix:` line with the corrected line, printed in every build with no flag and no model, only when the compiler computed exactly one edit **and** a re-check shows the original error gone and no new one. An error class declares its fix beside its hint; nothing matches an error's name or text. `--smart` keeps its place: rule fix first, model only when there is none. | A | M–L | **Landed in 2.1.0** (planned for 2.2): 18 codes, the checked `Fix:` line in every build including `--no-smart`, `EXPECT-FIX` for tests. See "Findings on TDP-2-001" below. |
| **DX-2** | **Machine-readable diagnostics** (`--diagnostics=json`, SARIF-shaped): code, severity, spans, hint, related locations, and the DX-1 fix as a list of `{span, replacement}`. TDP-2-001 lists this as an open question. | A | M | `TurfcInterfaceMinor` bump. Gives `turf-lsp` quick fixes (F-11), the benchmark (SL-2) and tools a contract other than scraping text, which D-6 puts outside the compatibility promise. **Owner decision (2026-10-16):** DX-2 lands after DX-1, in a later 2.x minor. 2.1.0 prints `Fix:` lines in default output with no JSON form for tools to read instead; the release notes tell tool authors that diagnostic text is outside the promise. |
| **DX-3** | **Hint backfill.** TDP-2-001 counts 20 codes whose hint is only general advice and 5 with none (E105, E106, E115, E116, E118). A fix builds on a concrete hint, so these come first. | A | S–M | The count is the TDP's; recompute it with the checked-in script DX-1 requires. |
| **DX-4** | **`--fix` apply mode** (later proposal). Applies only validated edits, atomically, on explicit request, with the same verified-result guarantee `turf fmt` has (§6.23). | A | M | Not before DX-1 and DX-2. |

#### The SLM: scoring and checks on the model itself

The aim is a model-assisted path whose answers are *measured* trustworthy,
cheaper where a rule suffices, and honest about what produced them. Every SL
item preserves F-DIAG-1 (sandbox verification is never removed) and F-DIAG-2
(an unvalidated suggestion never clears the display gate), and respects F-DIAG-3
(no new `SLM::ErrorCategory` values without an explicit request).

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **SL-0** | **Provenance.** Every suggestion carries `rule | model | cache`; output says `Compiler fix` for a rule, `Model suggestion` for a model, never `[AI Suggestion]` for a rule. | A | XS | Prerequisite for SL-1 and DX-1. The label is output text (D-6). **Landed in 2.1.0 in part:** a model suggestion is labelled `[Model Suggestion]` and a rule fix is a `Fix:` line, so `[AI Suggestion]` is no longer printed for a rule. The per-suggestion `rule | model | cache` field is not recorded. |
| **SL-1** | **Fix Score v2: a decomposed score replaces the binary one.** See below. | A | M | |
| **SL-2** | **Benchmark scoring: a utility score with a confidence interval**, per-code and per-category tables, and a rules-only baseline. See below. | A | M | |
| **SL-3** | **Corpus growth and a held-out split.** From 22 to several hundred cases. See below. | A | M | F-9's "corpus growth" item. |
| **SL-4** | **Checks on the model itself.** Qualification, drift, determinism, resource bounds. See below. | A | M–L | |
| **SL-5** | **Checks on what a suggestion does** (beyond "it compiles"): capability diff, deletion detection, API-surface stability, warning delta. See below. | A | M | Feeds SL-1. |
| **SL-6** | **Richer prompt context.** `known_types` covers one call site today (SLM-001 *Known pitfalls*: variable-initializer mismatches and operator mismatches have none). Add: the declaration of each struct a diagnostic names, the signature of a called function, the enclosing function's parameter types. | A | M | Easier after F-S3 (a queryable `SemanticModel`). |
| **SL-7** | **Coverage of the rule layer.** Today 11 rename classes. Add the classes DX-1 identifies (E060 make `mut`, E102 insert `;`, arity, missing return type …), each re-derived (not assumed) the way SLM-001 excluded `UseBeforeDeclarationError`. | A | M | Shared with DX-1: one rule engine (`src/diagnostics/CompilerFix.*`, outside `src/slm/`). DX-1 shipped E060 and E102 and 16 more codes; arity and missing return type remain. |
| **SL-8** | ~~`@slm { }` blocks~~ **Dropped** (4.2): model-generated code entering a program at compile time puts a model in the compilation path, which the Constitution's Non-Goals rule out. The assistant may *suggest* source that a person commits. | — | — | |

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

#### Findings on TDP-2-001 (checked against the repository, 2026-10-15; outcome 2026-10-16)

1. **Part of the mechanism existed.** The rule layer, its sandbox validation and
   the model-free speed were already in `--smart`; the model is not needed for
   renames. DX-1 was therefore "move the rule engine out of `src/slm/`, show it
   by default, extend the classes, relabel", not a new mechanism. It landed that
   way in 2.1.0: the engine is `src/diagnostics/CompilerFix.{h,cpp}`, the edits
   are attached where an error is raised (`withFixIt()` in
   `src/diagnostics/Errors.h`), `tryDeterministicFix()` and the
   `[Deterministic Fix]` status line are gone, and `--smart` asks the model
   only when the compiler has no fix.
2. **No build dependency on the model runtime.** Checked: a `--no-smart` build
   (`turfc --version` ends `smart not-built`) prints `Fix:` lines.
3. **The re-check** re-analyses an edited in-memory copy through the query
   facade (`include/turf/Analysis.h`, lint and flow analysis included) and
   runs without touching the original compile. One re-check per fix,
   capped at `CompilerFixes::MaxChecksPerCompile` (10) per compile. F-S2 is
   still the real fix for the cost.
4. **Spelling.** The footer and the docs keep `turf --explain E0NN`.
5. **Numbers**, from the checked-in `scripts/dev/diagnostic_fix_coverage.py`:
   791 `fail_*` tests, 150 codes in the registry, 138 exercised by the suite;
   of those, 120 print a hint every time, 13 sometimes and 5 never (the TDP's
   "136 of 149" used another method); 722 errors reported by the suite, 43 with
   a `Fix:`, across 18 codes: E014, E026, E029, E030, E031, E039, E040, E041,
   E042, E046, E048, E052, E054, E060, E068, E087, E102, E138.
6. **`EXPECT-FIX` exists.** `turfc --test` requires each marker to match a
   printed `Fix:` exactly and every printed `Fix:` to be pinned, then applies
   the fixes via `--smart-write-fix` and analyses the fixed program, requiring
   fewer errors and none the original lacked. E102 (a missing `;`) prints with
   its code and has a fix. `tests/55_compiler_fixes/` holds the new fixtures,
   including one for an ambiguous typo that gets no fix.
7. **Text compatibility.** `Fix:` lines are output text, outside the
   compatibility promise (D-6), and the 2.1.0 release notes say so. TDP-2-004
   planned DX-2 (`--diagnostics=json`) first; the owner decided (2026-10-16)
   that it lands later, so until it does a tool that reads default output sees
   the `Fix:` lines and has no JSON to read instead. DX-3 (hint backfill, 5
   codes with no hint) is still open.

### 6.9 Numeric speed (NM) — draft **TDP-2-002**, checked arithmetic and vectorization

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
| **NM-6** | Batch-then-check: emit the operation unchecked with an overflow bit, flush the bits once per straight-line run (stage 1) or per loop strip (stage 2). | I | L | Proposal and applicability rule in `docs/development/changes/TDP-2-002.md` NM-6. Unobservable because a panic cannot be caught and memory after it is not observable (`FREEZE.md` §6.30, F-PANIC-1 … 3); no check is removed (INV-OPT-2). Stage 2 needs NM-2. Measured 2026-10-25 (`BENCHMARKS.md`): stage 1 does not pay, stage 2 pays only as hand-written vector code, which §6.10 leaves to LLVM. Not implemented; on no line. |

`RT-4` in 6.3 is replaced by NM-1 … NM-6.

#### Findings on TDP-2-002 (checked against the repository, 2026-10-15)

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
   closed-list change (F-STD-2), so it needs a change record, which with the
   TDP is its "deliberate, published change".
4. **`-O0`.** A call to a wrapping function must lower to a plain `add`/`sub`/`mul`
   with no `nsw`, even at `-O0`, never an actual call; otherwise "same result at
   every level" holds but the speed advantage is lost where people debug.
5. **`int32` forms** are left out here: the TDP says `int32` exists only for ABI
   declarations, and this was not re-verified.
6. **The Constitution (Principles I–VIII) is not in this repository** (it
   appears only in `docs/development/handbook-sync/DONE.md`). Both TDPs cite it.
   This roadmap treats it as external; decision D-14 says where TDPs live.

### 6.10 Generics at use sites and wider `guess` (LG)

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
  LG-3 narrows this one clause, decided in D-19 and specified in TDP-2-010.

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
| **LG-7** | **Inference you can see** (no language change): E129/E110 point at LG-1/LG-3 spellings through DX-1's `Fix:` where one candidate exists; LSP **inlay hints** for `guess`-inferred types and a code action "write the inferred type" (the compiler already records `WasGuessInferred`); `typeof` already prints a type at run time. | A | S–M | Readability without losing the short form. Compiler hints in 2.1; LSP inlay hints with F-11. |

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


### 6.11 The pure compiler (PC) — aim A

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **PC-1** | **`libturf`.** F-S5's front-end library with a versioned API (lex, parse, resolve, type-check, `SemanticModel` queries, diagnostics, TIR, and lowering to LLVM as a separate layer). Shipped in the release archive as headers plus a static library. `turfc` is a thin driver over it. | I / A | L | Needs F-S2, F-S3, F-S4e. The API version is in IF-1's manifest. |
| **PC-2** | **The assistant leaves `turfc`.** The model runtime (llama.cpp, `src/slm/SLMEngine.cpp`, grammars, the cache) moves into a separate binary (name: D-24) that reads DX-2 JSON and the source, re-checks candidates through `libturf`'s sandbox, and prints suggestions. `turf check --smart` invokes it. DX-1's rule fixes stay in the compiler, since they involve no model. In 2.7 `turfc --smart` forwards to the assistant and prints a deprecation note. | A (2.7), B-14 (3.0) | M–L | The `-smart` release flavour ends at 3.0 (B-15); F-DIAG-1/2/3 move with the assistant. |
| **PC-3** | **Non-compile modes leave `turfc`.** `--format`, `--migrate`, `--test` and `--explain` get their own home (D-25): `explain` becomes data (V-5's registry) that `turf` reads directly; the rest move to a helper linked against `libturf`. `turf fmt`, `turf migrate`, `turf test` and `turf explain` keep their spelling throughout. In 2.7 the `turfc` flags print a deprecation note. | A (2.7), B-14 (3.0) | M | `turf` users see no change. |
| **PC-4** | **Reproducible output.** The same source, flags and compiler give byte-identical objects and executables; build paths are normalized in debug info. CI builds the corpus twice and compares. | I | S–M | |

### 6.12 Libraries over C (LB) — aim B

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **LB-1** | **Native code in packages.** A `[native]` table in `turf.toml`: C sources compiled with the system C compiler (D-26) and linked into the package's consumers, system libraries to link (generalizing `--link`), include paths. The C sources are part of the package's tree hash, so `turf.lock` pins them. | A | M–L | `turf-cli` work plus a driver input (`TurfcInterfaceMinor`). |
| **LB-2** | **Unsafe-surface report.** `turf graph` (and a readable `turf audit`) lists, per package: files declaring `unsafe module;`, `unsafe { }` blocks, `extern fn` declarations, native sources, and whether the consumer's `[unsafe] allow` grants the package. | A | S | The compiler supplies the counts through the package-graph output (IF-2). |
| **LB-3** | **Checked boundary contracts on `extern fn`.** Let a binding state, in safe types the compiler lowers, what it would otherwise check by hand: a non-null pointer (`&T`), a nullable one (`&?T`), an array view lowered to pointer plus length, and who frees a returned pointer. The compiler emits the checks and conversions at the call, so the call needs no `unsafe { }`. Spelling is TDP-2-016's, under 4.1. | A | M–L | Removes most hand-written unsafe code from typical bindings. |
| **LB-4** | **Safe library APIs.** A raw pointer type (`ptr`, `T*`) in a package's `pub` API is a warning. The 3.0 rule (B-12) is TDP-2-018's to choose: forbidden outright, or usable only from code that itself holds the raw-pointer capability. | W | S–M | Warning from 2.6. |
| **LB-5** | **Raw-receiver method calls.** A method call through a raw-pointer receiver (`a.next.getval()` with `next: Node*`) needs no `unsafe { }` today (`pointers_and_references.md` §1.2's known gap). Warning in 2.2, error in 3.0 (B-13). | W | S | |

### 6.13 Packages and project management (PK) — aim B

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **PK-1** | **Registry** (this is F-7c). A package index (hosting: D-23) mapping name and version to a source archive and its tree hash; semver ranges in `[dependencies]` (`name = "^1.2"`); `turf add <name>`, `turf update` within ranges, `turf outdated`; `turf publish`; yanking. `turf.lock` records the resolved version and hash and carries a format version, so an older `turf` rejects a newer lock with a message. | A | L | Index format is an IF-2 schema. |
| **PK-2** | **Workspaces.** Several packages in one tree with one lock and one build directory. | A | M | |
| **PK-3** | **Publish checks.** `turf publish` refuses unless: the package builds and its tests pass; its `pub` API, compared with the previous published version, matches the version bump (removing or changing a `pub` item needs a major; the stdlib's API-lock tooling, generalized); its LB-2 report and `[native]` table are recorded in the index entry. | A | M | Semver enforced by the tool, not by trust. |
| **PK-4** | **Package documentation.** F-11c generates reference pages from `pub` items and doc comments; the registry shows them. | A | M | Same generator as the stdlib reference (IF-3). |

### 6.14 Syntax (SY) — TDP-2-021 … TDP-2-025

Owner requests, 2026-10-25. Each new spelling is additive in a minor; each old
spelling it retires warns first (5.4) with a `Fix:`, and is removed in 3.0 with
`turf migrate 3.0` doing the edit.

| ID | Item | Kind | Cx | Notes |
|---|---|---|---|---|
| **SY-1** | **`for c in s` over a `string`** (TDP-2-021): one iteration per byte, `c` a `char`, `s` and its length evaluated once, each read through `s[i]`'s check. Today E005. | A | S | Byte iteration, as `s[i]`; TDP-2-011's protocol later makes `string` one more instance. |
| **SY-2** | **Methods on number literals** (TDP-2-022): (a) `1.twice()` already compiles; document and test it, and that `-1.abs()` is `-(1.abs())`. (b) A decimal literal receiver (`1.5.floor()`, also compiling today) warns, with a `Fix:` writing `(1.5).floor()`. | A | S | (b) becomes B-18. |
| **SY-3** | **`function` as a spelling of `fn`** (TDP-2-023): a contextual keyword where a function declaration starts; an identifier elsewhere, as it is today. | A | S | Reserved in 3.0: B-19. |
| **SY-4** | **`[pub] static fn T.m(...): R`** (TDP-2-024), modifiers in front; `fn T.m(...) static: R` warns with a moving `Fix:`. | A | S | Old place removed in 3.0: B-20. |
| **SY-5** | **`[pub] immut` module constants** (TDP-2-025): a top-level `immut`, private unless `pub`/`public`; `export immut` warns with a `Fix:` writing `pub immut`. Top-level `mut` stays E087. | A | S–M | `export` removed in 3.0: B-21. The stdlib moves in the same minor. |

---

## Part 7 — 3.0.0 and the end of 2.x

### 7.1 The breaking register

Nothing is added to 3.0 because it is on this list; it is on this list because
it could not go in a minor. Each entry names *why* it fails the minor promise
and what the migration is. `lint-set = "3.0"` on 2.8 reports every
source-level entry as an error, so a project can be ready before it upgrades.

| ID | Item | Why it is breaking | Migration | Expected |
|---|---|---|---|---|
| **B-1** | **Promote proven warnings to errors:** AN-3 (a)/(b) (consistent with today's self-recursion error), selected AN-1 `unsafe` checks, any warning with ≥ 2 minors of field time and no false positive reported. | Rejects programs that compile in 2.x. | `lint-set = "3.0"` (V-1) first; `turf migrate 3.0` annotates the sites. | yes |
| **B-2** | **Runtime debug checks (RT-5) on by default at `-O0`.** | A program with latent memory corruption starts panicking. | `--no-debug-checks`. | yes |
| **B-3** | **Removal of items deprecated by V-2.** | Removal is not allowed before 3.0. | The `@deprecated` message names the replacement; `turf migrate` renames where the replacement has the same signature. | yes |
| **B-10** | **Retire `lint-set` pins older than 2.8.** | Pinned builds change. | Documented. | yes |
| **B-12** | **Safe library APIs** (LB-4): the TDP-2-018 rule for raw pointers in a package's `pub` API. | Rejects packages that compile in 2.x. | Wrap the pointer in a struct or `&T`; LB-3 contracts for bindings. | yes |
| **B-13** | **Raw-receiver method calls need `unsafe`** (LB-5). | Rejects programs that compile in 2.x. | `turf migrate 3.0` wraps the call in `unsafe { }`. | yes |
| **B-14** | **`turfc` is a compiler only** (PC-2, PC-3): `--smart`, `--smart-write-fix`, `--format`, `--migrate`, `--test` and `--explain` are removed from `turfc`; `TurfcInterfaceMajor` becomes 3. | Scripts calling those `turfc` flags fail. | The `turf` commands are unchanged; the deprecation notes have named them since 2.7. | yes |
| **B-15** | **One release flavour.** The `-smart` archives end; the assistant is its own download. | Install recipes and download URLs change. | Package recipes and the download page updated in 3.0.0. | yes |
| **B-16** | **`recover` binds tighter than `=` and `+=`.** In 2.x `recover` binds more loosely than every operator, assignment included, so `v = f() recover run { ... };` parses as `(v = f()) recover ...` and is E066 (`Cannot assign value of type '!int'`): only a declaration (`mut v = f() recover ...`) or a parenthesized right side (`v = (f() recover run { ... });`) works. In 3.0 `recover` takes the whole right-hand side of an assignment, the way it already takes a declaration's initializer. | Changes the meaning of a program that compiles in 2.x: with `mut v: !int`, `v = f() recover produce 0;` is `(v = f()) recover produce 0` (v holds the error when f fails; checked 2026-10-15), and in 3.0 it would store 0 instead. | `turf migrate 3.0` adds the parentheses where the meaning would change; nothing to do for code that already parenthesizes. Until then the 2.x reference documents the parentheses as the way to write it. | yes |
| **B-17** | **A generic struct's type arguments are written where it is built** (D-27). `List::default()` and `Box::new(x)` no longer take their type arguments from the type they are stored into, returned as or passed to; the call writes them, `List<int>::default()` (LG-1). `mut l: List<int> = List::default();` and `return Box::new("x");` in a function returning `Box<string>` are errors. Annotations themselves stay (`mut l: List<int> = List<int>::default();`, `mut n: int = 5;`), and so do type arguments inferred from the constructor's own arguments (LG-3), which take nothing from the context. | Rejects programs that compile in 2.x: every generic construction whose type arguments come from the context. | A warning in 2.3.0 (allow-by-default first, per the 5.4 ladder) at each such call, with a `Fix:` writing the arguments in; `turf migrate 3.0` applies them all, reading each one from the declared type exactly as 2.x does. | yes |
| **B-18** | **A decimal literal receiver needs parentheses** (SY-2 b): `1.5.floor()` is an error; `(1.5).floor()` and `1.twice()` are unaffected. | Rejects programs that compile in 2.x. | The 2.3 warning's `Fix:`; `turf migrate 3.0` adds the parentheses. | yes |
| **B-19** | **`function` is a reserved word** equal to `fn` (SY-3), usable wherever `fn` is, function types included. | An identifier named `function` stops parsing (a B-5 case, taken deliberately). | `lint-set = "3.0"` lists the names; `turf migrate 3.0` renames them. | yes |
| **B-20** | **`static` only before `fn`** (SY-4): `fn T.m(...) static: R` is an error. | Rejects programs that compile in 2.x. | The 2.3 warning's `Fix:`; `turf migrate 3.0` moves the word. | yes |
| **B-21** | **`export` removed** (SY-5): `export immut` is an error; `pub immut` is the spelling. `export` stays reserved. | Rejects programs that compile in 2.x. | The 2.3 warning's `Fix:`; `turf migrate 3.0` writes `pub immut`. | yes |
| **B-4** | **GC write-barrier contract** (only if RT-2′ is chosen over RT-2). | `unsafe` and C code storing a GC pointer into a GC object must call a barrier. | `mem::` helper plus an RT-5 check that finds missing barriers. | no (RT-2 is 3.x) |
| **B-5** | **New reserved words**, only if a contextual keyword cannot express a feature (CC-4(b)). | An identifier with that name stops parsing. | `turf migrate` renames it. | no |
| **B-6** | **Meaning of `fn(T): R[N]`** (G-1, "fix the grammar"). | Changes the parse of an existing type. | Not needed if G-1's additive spelling suffices. | no |
| **B-7** | **Function values in `extern` struct fields** change size with F-1's `{code, env}`. | Layout change. | Decided in F-1 phase 1 so the fields stay thin. | no |
| **B-8** | **Concurrency soundness rules that reject existing code.** | Concurrency ships in 3.x, so such a rule would need 4.0; TDP-3-001 is designed so it never does (its checks apply only to code that starts a thread). | — | no |
| **B-9** | **`turf.toml` / `turf.lock` format major**, only if PK-1 needs it. | Old manifests rejected. | A converter in `turf`. | no |
| **B-11** | **A different spelling for type arguments in type positions.** | Every program with generics. | Not planned: LG-1 removes the reason. | no |

B-17's migration is mechanical because 2.x already knows the answer at every
site it changes: it is the type 2.x reads the arguments from.

### 7.2 What adds nothing to the register

DX-1, DX-2, SL-0 … SL-7, NM-1 … NM-6, LG-1 … LG-7, LB-1 … LB-3, PK-1 … PK-4, SY-1,
PC-1 and PC-4 are additive or unobservable. The designs TDP-2-002 rejects (a
global wrapping flag, wrapping by default in optimized builds, undefined
overflow) would be breaking and stay rejected. If NM-3 and RT-5 together show
that default-checked arithmetic is still too costly, the answer is more NM-2
proof, not a changed default.

### 7.3 Releasing 3.0.0 and discontinuing 2.x

1. **2.8.0** ships `lint-set = "3.0"`, `turf migrate 3.0` and the migration
   guide (website, beside "Migrating to 2"). TDP-2-020 is accepted.
2. **3.0.0-rc.N** tags are cut from `master`, as for 2.0; the package registry's
   most-used packages are built against each release candidate and their
   authors told of failures.
3. **3.0.0** ships with the register applied, `turf` updated, and the website's
   current documentation switched to 3.0. The 2.x documentation moves to an
   archived section.
4. **Maintenance:** the last 2.8.x receives bug-fix and soundness patches for
   six months after 3.0.0 (D-8). No features, no new warnings.
5. **Discontinuation:** six months after 3.0.0, 2.x is end of life. No further
   2.x releases; Homebrew, Scoop and AUR carry 3.x only; the registry keeps
   serving existing 2.x-only versions but accepts new publishes only from
   packages whose `turf` requirement includes 3.x.

### 7.4 What 3.0.0 is for

3.0.0 is defined by its two aims (1.2, 1.3), not by an accumulation of
breaks. It is cut when PC-1 … PC-4 and the aim-B items have shipped in 2.x and
the register's "yes" rows are ready to switch on. An entry marked "no" that
turns out to be needed waits for 4.0 unless it closes a soundness hole.

---

## Part 8 — Decisions

### 8.1 Decided, 2026-10-15

The proposal's recommendations, adopted. D-1, D-8, D-11 and D-17 … D-19 were
put to the owner and confirmed on 2026-10-15; the rest are internal design
calls that any TDP touching them may reopen.

| ID | Question | Decision |
|---|---|---|
| **D-1** | Rust-style editions instead of a 3.0? | **No.** 3.0 plus `turf migrate`. |
| **D-2** | What does `--deny-warnings` mean across minors? | Added only with V-1's `lint-set = "2.N"` pin; documented as upgrade-unsafe without it. |
| **D-3** | Concurrency sharing model. | Share nothing by default; transferable arguments; shared state only through stdlib `Mutex`/atomics/channels. TDP-3-001 specifies it. |
| **D-4** | May RT-2 use OS dirty-page tracking while staying non-moving? | Yes, with RT-1 measured first. |
| **D-5** | May RT-1's parallel mark use collector-owned worker threads? | Yes: the *mutator* is single-threaded until CC-1. |
| **D-6** | Are diagnostic message text and ordering outside the contract? | Yes. Codes and exit statuses are the contract; tools read DX-2's JSON. |
| **D-7** | Contextual keywords vs new reserved words. | Contextual only (4.1). |
| **D-8** | Support window for the last 2.x after 3.0. | Six months of bug-fix and soundness patches, then end of life (7.3). |
| **D-9** | Runtime before closures? | Closures first (2.3), runtime in 2.5. |
| **D-10** | Where the proposal goes. | This document is the plan of record; `FUTURE.md` keeps only what it leaves unscheduled. |
| **D-11** | Does a rule-based `Fix:` print by default? | Yes, after DX-2. `--smart` means model assistance only. |
| **D-12** | SL-2's penalty and the model's shipping bar. | `K = 3`; a model path ships only if its mean utility beats the rules-only baseline with separated 95% intervals and tier `proven` has zero false confidence on the held-out split. Revisited once SL-3 gives real numbers. |
| **D-13** | Does `--smart` warn about an unqualified model? | Warn once, never block. |
| **D-14** | Where TDPs live. | On the website only (`website/src/content/tdp/`); this repository cites them by number. |
| **D-15** | How NM-3's compiler-known lowering is declared. | A fourth kind in `src/sema/Intrinsics.h`, keyed by the resolved declaration's (module, function). |
| **D-16** | May SL-5's Codegen recheck run inside `--smart`? | Yes: compile to an in-memory module and verify; never link or run. |
| **D-17** | LG-4: type parameters only in the return type? | Yes, with an LG-2 argument or an expected type; E129 at a call that has neither. |
| **D-18** | LG-5: relax F-GUESS-3 for lambdas? | Yes, lambdas only. |
| **D-19** | LG-3: inference of struct type arguments from constructor arguments? | Yes, that clause only; Model B stays rejected. |
| **D-20** | A Bound/Typed tree between `SemanticModel` and TIR? | No. AST + `SemanticModel` lower into TIR. |
| **D-21** | F-S4a before F-S3? | Yes (2.2 and 2.3). |

### 8.2 Decided by the owner, 2026-10-15

| ID | Question | Decision | Specified in |
|---|---|---|---|
| **D-22** | Concurrency before 3.0 or after it? | After. It is additive, so it is the first 3.x theme (3.1, 3.2), and 3.0 follows 2.8. | Part 2 |
| **D-23** | Registry hosting. | A static index in a git repository plus source archives on release storage; publishing is a pull request that CI checks and merges. No server. A sparse HTTP read path may be added later without changing the index format. | TDP-2-017 |
| **D-24** | How the assistant reaches users. | A separate binary and archive (working name `turf-assist`), found by `turf` on the `PATH` or beside `turf`. The main release carries no model runtime. | TDP-2-019 |
| **D-25** | Where format, migrate, test and explain go. | `explain` is data from V-5's registry, read by `turf`; format, migrate and test are one helper binary linked against `libturf`. The `turf` commands keep their spelling. | TDP-2-019 |
| **D-26** | Which C compiler LB-1 uses. | The system C compiler (`CC`, then `cc`/`clang`/`gcc`, MSVC on Windows), with a clear error when none is found. Pure-Turf packages never need one. | TDP-2-016 |

### 8.3 Decided by the owner, 2026-10-24

| ID | Question | Decision | Specified in |
|---|---|---|---|
| **D-27** | With LG-1 in, does a generic construction keep taking its type arguments from the declared type? | No, in 3.0: the type arguments are written where the value is built (`List<int>::default()`), which also works with `guess`. Annotations stay; only the context supplying a constructor's type arguments is retired. No style guide is needed to choose between the two spellings, since one of them goes. | B-17 (7.1) |

No decision is open. A new one is added here with a recommendation and asked
of the owner before the item it blocks starts.

---

## Appendix — Traceability to the sources

- `FUTURE.md` §A (F-1, F-2, F-3, F-6c, F-7a/b/c, F-11/b/c/d, F-14/b, F-15,
  F-20), §B (F-S0 … F-S5), §C (F-5, F-6, F-6b, F-8, F-9, F-10, F-12, F-16, F-19,
  F-22, F-24, F-25) and Known gaps (G-1) are each placed once in Part 6 and
  given a line in Part 2 (or 3.x). §D (superseded) and §E (rejected) are not
  reopened, apart from D-4 and D-5's readings of §E.
- `ARCHITECTURE_SIMPLIFICATION.md` phases 0★, 0–5 map to F-S3's note, F-S0, F-S1,
  F-S2, F-S3, F-S4, F-S5; PC-1 builds on F-S5.
- `FUTURE.md` F-9 is split: corpus growth is SL-3, LoRA adapters stay in F-9
  under the assistant, and `@slm` blocks (SL-8) are dropped (4.2). F-24 is NM-1 …
  NM-6 (TDP-2-002). F-11's "`--smart` fixes as code actions" is DX-1 + DX-2.
  F-7c is PK-1.
- `SLM-001.md` items 2–5 are the base for SL-0 … SL-7; its *Known pitfalls* are
  SL-6 and SL-7.
- `docs/architecture/pointers_and_references.md` §1.2's known gap is LB-5;
  §1.7's per-package capability is the base of LB-2 and LB-4.
- New in the proposal: LG-1 … LG-7, V-1 … V-5, AN-1 … AN-9, RT-1 … RT-6,
  CC-0 … CC-6, DX-1 … DX-4, SL-0 … SL-8, NM-1 … NM-6, B-1 … B-11, D-1 … D-21.
  New in this document: PC-1 … PC-4, LB-1 … LB-5, PK-1 … PK-4, IF-1 … IF-5,
  B-12 … B-15, D-22 … D-26.
