# Turflang Design Notes & Open Work

Consolidated 2026-08-17 from seven separate planning docs (`cfg_analysis_roadmap.md`,
`diagnostics_and_errors_review.md`, `lora_training_strategy.md`,
`MEMORY_ARCHITECTURE_RFC.md`, `STDLIB_AND_WEB_RFC.md`, `STDLIB_ROADMAP.md`,
`turflang_architectural_review.md`) into one file, since they'd drifted into
overlapping, partially-stale coverage of the same handful of future-work
areas. Each section below was re-verified against the live codebase at merge
time — items already shipped were cut rather than carried forward as stale
checkboxes; only genuinely open work and durable design rationale survive.

`docs/planning/archive/ROADMAP_v1.0.0_COMPLETE.md` and `docs/planning/FUTURE.md` (the latter
added 2026-08-21, once 1.0.0 shipped and `FINAL_SPRINT.md` was retired) are
**not** part of this merge and remain the authoritative, separately-tracked
planning docs — some items below (e.g. `unsafe {}`, source spans, error
codes) are also tracked there; where that's true it's called out explicitly
rather than duplicated in full. **§2 below predates 1.0.0 shipping and is
now stale in places** — source spans, error codes/`--explain`, and
struct-field "did you mean?" all shipped; see `ROADMAP_v1.0.0_COMPLETE.md`'s
"Diagnostics Polish" section for the current, accurate status. Not
re-verified past that point yet — see that section's own header for why.

---

## 1. Static Analysis / CFG Roadmap

Real CFG infrastructure exists (`src/analysis/CFG.h`/`src/analysis/CFG.cpp`,
`src/analysis/Analysis.h`'s `DefiniteAssignmentPass` and `LivenessAnalysisPass`,
`src/analysis/ValueRange.h`/`src/analysis/ValueRange.cpp`'s `ValueRangePass`,
`src/analysis/AliasAnalysis.cpp`'s `AliasAnalysisPass`), wired into
`CodegenFunc.cpp` per function. Already
shipped: cyclomatic-complexity warnings, unreachable-code/dead-branch
warnings, missing-return errors, statement-after-terminator errors,
use-before-initialization errors (`DefiniteAssignmentPass`), dead-store/
unused-variable warnings (`LivenessAnalysisPass`), infinite-recursion
detection, and use-after-free/double-free tracking (`AliasAnalysisPass`).

Genuinely still open:

### Pointer Nullability State
Track whether a pointer might be null to prevent unsafe dereferencing.
- Create `class NullabilityPass`. Track variable state as `NonNull`,
  `Nullable`, or `Null`.
- If an `IfExprAST` checks `ptr != null`, narrow to `NonNull` in the `then`
  branch and `Null` in the `else` branch.
- Trigger: an `ArrayAccessExprAST` or dereference while state is `Nullable`
  or `Null` raises "Potential null pointer dereference."

### Memory Leak Detection
Ensure every `malloc` is paired with a `free` (distinct from
`AliasAnalysisPass`'s use-after-free/double-free tracking, which already
exists — this is the "never freed at all" case).
- When `malloc` is called, track the returned pointer variable through the
  CFG.
- If the CFG reaches `ExitBlock` without the pointer transitioning to
  `Freed` (via `free(ptr)`) and it isn't being returned to the caller,
  raise a "Memory leak detected" warning.

### Bounds Check Elimination
`ValueRangePass` currently only does constant propagation (per its own doc
comment) — the actual elimination optimization isn't built on top of it.
- When `Codegen.cpp` generates an `ArrayAccessExprAST`, it currently always
  injects `EmitRuntimeBoundsCheck`.
- Before emitting, query `ValueRangePass` for the index variable's proven
  range.
- If `Range.Min >= 0 && Range.Max < ArraySize` is provably true, skip
  emitting the check entirely.

### `CallGraphPass::runGlobal()`
Deleted (OPT-002): it was a literal empty stub, and its `CallGraph.cpp` /
`CallGraph.h` no longer exist. Whatever whole-program call-graph analysis it
was meant to enable (the original roadmap tied it to infinite-recursion
detection, which now ships via a different, CFG-local mechanism) is still
unbuilt.

---

## 2. Diagnostics & DX

The `DiagnosticEngine` + SLM pipeline is already fairly sophisticated:
every error funnels through `DiagnosticEngine::add()`, which caps noise to
one primary error per line and (with `--smart`) hands the error to
`SLMIntegration.cpp`. That gathers a hyper-specific context window
(deterministic error categorization, surrounding lines, a text scan that
pins the original declaration of the broken symbol even when it's far
away), feeds it to a local `qwen2.5-coder-1.5b` model forced into strict
JSON output, then **sandbox-validates** the suggested fix by compiling it
silently before ever showing it to the user — a hallucinated fix that
doesn't actually compile is discarded, never surfaced.

Already shipped from the original DX wishlist: hints as a separate
`Hint_` field on `TurfError` rather than baked into the message string;
`TurfWarning` as a distinct non-halting class; module-method-name
"did you mean?" suggestions (`SuggestionIndex::suggestVisible()`, reused
across struct/enum/module-function/import-symbol lookups); keyword-typo
"did you mean?" (`KeywordError` via `SuggestionIndex::suggestKeywords()`,
e.g. `funtion` → `fn`).

**Superseded — all three items below shipped 2026-08-21. Left in place
below only as the original pre-implementation design record; see
`ROADMAP_v1.0.0_COMPLETE.md`'s "Diagnostics Polish" section for what actually
shipped, which differs in scope from a couple of these (e.g. source spans
now cover every token kind, not just identifiers/keywords).**
- **Source spans instead of single-point locations — partially done.**
  `SourceLocation` (`src/frontend/Lexer.h`) gained `EndLine`/`EndCol` fields
  (default `-1`, meaning "not computed") while building `turf-lsp`
  support (turf-lang/turf-lang#243) — `gettok()` now populates them for
  every identifier/keyword token, which flows for free into every AST
  node's name location (struct/function/variable/field/method/module
  names, per #230's fix) and into any diagnostic whose `Loc` happens to be
  one of those. Still open: (1) `DiagnosticEngine`/`printDiag()`
  (`Lexer.cpp`) doesn't read `EndCol` at all yet — the CLI's own `^` caret
  output is visually unchanged; only library consumers reading
  `Diag.Loc.EndCol` directly (e.g. an LSP building a `Range`) benefit
  today. (2) Other token kinds (operators, string/char/number literals)
  and genuine multi-token spans — a whole sub-expression like
  `add(3, "4")` below, not just one identifier — still have no end
  position. Wiring `printDiag()` to underline using `EndCol` when present,
  and extending coverage to operators/literals, remains open scope:
  ```
    7 |     io.printline(add(5 + 6, add(3, "4")));
      |                             ^^^^^^^^^^^
      |                             Type mismatch: expected 'int', got 'string'
  ```
- **Error codes + `turf --explain`.** No `--explain` flag in `main.cpp`'s
  arg parsing, no per-class error codes. Each error class should get a
  unique code (`E042` for a type mismatch, etc.) that `turf --explain E042`
  can print a full multi-paragraph explanation for, Rust-style.
- **Struct-field "did you mean?".** `user.pasword` should suggest
  `user.password` — no field-not-found suggestion path exists yet in
  `TypeCheckerExpr.cpp`/`ResolverExpr.cpp`.

---

## 3. Standard Library

What's in `lib/std/` today, module by module, followed by what's missing.
Every module follows the same shape — raw C/POSIX bound via `extern fn`,
ordinary Turf functions/structs layered on top so callers never see
`void*`/FFI details — and any new module below should follow it too. Files
live under logical subfolders (`core/`, `collections/`, `io/`, `mem/`,
`math/`, `testing/`); `import <module>;` doesn't reference a path, so this
is purely a browsing convenience.

### 3.1 Current baseline

| Module | Exports |
|---|---|
| `io` | `print`/`printline` (int, double, bool, string overloads), `input()` (one line from stdin) |
| `mem` | `alloc`, `alloc_zeroed`, `grow`, `dup`, `zero`, `alloc_aligned`, `usable_size` (checked, fail-fast wrappers) + raw `malloc`/`calloc`/`realloc`/`free`/`memcpy`/`memmove`/`memset`/`memcmp`, plus `Pool<T>` (bulk-free tracking — an explicit stopgap, not a step toward ARC) |
| `math` | `abs`, `power`, `min`, `max`, `clamp` (int/double overloads) + raw libm bindings (`sqrt`/`sin`/`cos`/`tan`/`floor`/`ceil`/`round`/`log`/`log2`/`log10`/`exp`/`pow`/`fabs`) |
| `strings` | `length`, `contains`, `index_of`, `starts_with`, `ends_with`, `repeat`, `count_char`, `substring`, `replace`, `to_upper`, `to_lower`, `trim` |
| `list` / `stack` / `queue` | `List<T>` / `Stack<T>` / `Queue<T>` — push/pop/peek/length/enqueue/dequeue as applicable, dynamic-array-backed |
| `map` | `Map<K,V>` — linear-scan over two parallel type-erased arrays, `put`/`get`/`contains`/`remove`/`length`; not hashed (no generic `hash(T)` yet), so O(n) lookup |
| `net` | `TcpListener`/`TcpConnection` — `bind`/`accept`/`connect`/`read`/`write`/`close`. TCP-only. |

No `os`, `time`, `json`, `random`, `path`/`fs`, `http`, or `Set<T>` module
exists at all.

### 3.2 Per-module gaps

**`io`** — no file I/O whatsoever. Missing, high value: `read_to_string(path)`
(single most commonly needed op), `read_lines(path)`, `write_string(path,
content)`, `exists(path)`; `eprint`/`eprintline` for stderr; `flush()` to
force a stdout flush. Missing, medium value: typed input (`input_int()`,
`input_double()` with a clean re-prompt path instead of an abort on bad
input); a `sprintf`-equivalent for building a string from mixed types.

**`strings`** — solid basics, missing connective tissue. **Blocking, not
speculative**: `split(s, delimiter): List<string>` — directly blocked real
code (`net.tr`'s client API has to take an IP as 4 raw integer octets
because there's no way to parse `"127.0.0.1"`). High value: `join(items,
sep)`, `trim_start`/`trim_end`, `pad_left`/`pad_right`,
`parse_int`/`parse_double` with explicit failure reporting. Lower priority:
a string-builder type (every `+` concat allocates fresh and leaks the
operands per `EmitStringConcat`'s own `TURF-MEMLEAK` comment — a known
cost, not worth solving before the memory model changes, see §5); char
helpers (`is_digit`/`is_alpha`/`is_whitespace`).

**`mem`** — reasonably complete for a low-level module. What's left is
Phase-4-memory-model-adjacent, not a gap in today's manual story: a real
bump-allocated arena (most valuable once there's a language-level
scope-exit/`defer` hook to drive it), and reference-counting helpers
(redundant if/when ARC lands — don't build a parallel scheme, see §5).

**`math`** — every libm binding needed already exists via `extern from
"m" fn`; just needs mechanical one-line wrappers (`fn sqrt(x: double):
double { return sqrt(x); }`) to be reachable under a clean module surface.
`math::PI`/`math::E` are a trivial addition now that top-level `export
immut` constants exist as a language feature — not yet added since nothing
currently needs them, but no longer blocked on anything.

**`list`/`stack`/`queue`/`map`** — missing: `Set<T>` (same underlying need
as `Map`, could share implementation); sort (`List<T>.sort()` or a free
`sort(list, comparator)`) and search (`binary_search`, linear `find`)
helpers, currently entirely absent; any iteration story beyond numeric
ranges (`for x in someList` doesn't exist — a language-level gap, not
purely stdlib, since it needs some notion of an iterator protocol).
Lower priority: `Optional<T>`/`Result<T,E>` — `!T` error unions already
shipped and cover the common single-function-return case, so a wrapper
struct would mainly matter for storing a fallible result in a
field/collection, a real but now non-urgent gap.

**`net`** — TCP-only, blocking-only, IPv4-only, no DNS, no TLS. Each of
these is a natural next step, not an urgent gap:
- DNS resolution (`getaddrinfo`/`gethostbyname`) — `connect()` only takes
  4 raw octets today, can't connect by hostname.
- UDP — no `UdpSocket`-equivalent.
- Non-blocking/concurrent connections — everything blocks the calling
  thread; no `select`/`poll`/`epoll`, and no threading primitives in the
  language at all yet to build a thread-per-connection model even if the
  socket layer supported it.
- TLS/HTTPS — no encryption at any layer; large, security-sensitive,
  likely via `extern fn` bindings to a system TLS library.
- A real HTTP layer on top — `net.tr` gives raw bytes in/out only, no
  request/response parsing, routing, or headers-as-a-map. See §4 below —
  this is genuinely achievable near-term, not speculative.

**Modules that don't exist at all**: `os` (env vars, argv, process exit
codes beyond `main()`'s own return, process spawning — high value, no Turf
program can currently read its own CLI args or environment); `time`
(wall/monotonic clock, `sleep`, duration arithmetic/formatting — not even
a trivial "how long did this take" is possible today); `json`
(parse/serialize, pairs with `http.tr`, see §4); `path`/`fs` (path
manipulation + directory ops, distinct from raw file *content* I/O);
`random` (no RNG in any form, not even a raw `extern fn rand()` binding —
note: not suitable for security-sensitive randomness without a dedicated
CSPRNG binding later); `testing` (structured `test "name" { ... }`
declarations + runner — `ROADMAP_v1.0.0_COMPLETE.md` Phase 6 already earmarks this
as a *compiler* feature, `turf test`, not a stdlib module; don't build a
competing stdlib-only convention).

### 3.3 Priority tiering

**Blocking / near-term:**
1. `strings::split` — already blocked real code this session.
2. `io` file I/O (`read_to_string` especially).
3. `os` (env vars + argv).

**High-value, well-understood next steps:** `http.tr` on top of `net.tr`
(§4); `strings::join`/`pad_*`/`trim_start`/`trim_end`; `time`; DNS
resolution for `net`; `math`'s libm re-exports.

**Valuable but with open design questions:** `Set<T>`, sort/search
helpers, a real iteration story for collections (does Turf ever get
`for x in collection`, or does everything stay index-loop-based?); `json`
(depends on the FFI-vs-hand-rolled-parser question in §4);
`Optional<T>`/`Result<T,E>` (unblocked by `!T` shipping, still not urgent).

**Large, not near-term:** TLS/HTTPS; non-blocking/concurrent networking
(needs a concurrency story in the language first — none exists); arena
allocators/ref-counting helpers in `mem` (wait for §5's direction);
`turf test` (compiler-level, tracked in `ROADMAP_v1.0.0_COMPLETE.md` Phase 6).

---

## 4. Web Standard Library Design (`http`/`json` on top of `net`)

Goal: hide raw C primitives (`ptr`, `malloc`, `bind`, `recv`, `send`)
behind memory-safe, ergonomic Turflang syntax, so a user building a web
backend never touches C:

```turflang
import http;
import json;

fn main(): int {
    mutable app: http.Server = http.Server();
    app.get("/", fn(req: http.Request, res: http.Response): void {
        res.send("Welcome to Turflang!");
    });
    app.post("/api/users", fn(req: http.Request, res: http.Response): void {
        mutable payload: json.Value = json.parse(req.body);
        mutable username: string = payload.get_string("username");
        res.json(201, json_response);
    });
    app.listen(8080);
    return 0;
}
```

A real TCP transport now exists (`lib/std/net.tr` — `TcpListener`/
`TcpConnection`, verified live via `showcase/http_server.tr` and
`showcase/http_client.tr`, a full Turf-to-Turf HTTP round trip using only
`import net;`, no raw FFI in either showcase file). What's still needed is
the layer above it, buildable now in stages:

1. **`http.tr` with request/response structs + manual if/elseif routing** —
   buildable today. Mechanical extraction of what `showcase/http_server.tr`
   already hand-writes inline:
   ```turflang
   struct Request { method: string, path: string, headers: string, body: string }
   struct Response { conn: net::TcpConnection }
   fn parse_request(conn: net::TcpConnection): Request { /* split conn.read() on \n\n */ }
   fn Response.send_text(status: int, body: string): void { /* build + conn.write() */ }
   fn Response.send_json(status: int, body: string): void { /* same, Content-Type: application/json */ }
   ```
   Depends on `strings::split` (§3.2, already tier-1) for clean header
   parsing.
2. **Callback-based routing** (`app.get(path, handler)`, matching the
   example above) — blocked on first-class functions as values
   (`ROADMAP_v1.0.0_COMPLETE.md` Issue #187, still open). Until then, route dispatch
   stays a manual `if request.path == "/" { ... } elseif ...` chain, same
   shape the showcase files already use.
3. **`json.tr`** — wrap `cJSON` via FFI (`extern from "cjson" fn
   cJSON_Parse(value: string): ptr;`, the same `extern from "libname" fn`
   mechanism `math.tr`'s libm bindings already prove out) rather than
   hand-rolling a parser. Needs confirming `libcjson` is available/vendorable
   in the build environment first; a hand-rolled pure-Turf parser is the
   fallback, meaningfully more work, duplicating a solved problem.

The three smaller FFI/control-flow ergonomics gaps this section used to
list here (implicit `string`→`ptr` coercion in FFI calls, local
in-function-body `extern fn` declarations, and the mandatory range-`for`
`step` clause) were closed by LANG-004
(`docs/development/changes/LANG-004.md`, 2026-09-18) — all three now work
as described in `docs/reference/control_flow.md` §"`for` — range form" and
`docs/reference/ffi_and_unsafe.md` §1.

---

## 5. Memory Architecture (ARC, Safe References, Heap Strings/Arrays)

Current state: `malloc`/`realloc`/`free` are compiler intrinsics calling
straight into libc; pointers are raw `i8*`; string literals compile to
`CreateGlobalStringPtr` (immutable, `.rodata`-backed); fixed-size arrays
are stack `AllocaInst`s, so returning one from a function dangles. All of
this is still true — nothing in this section has shipped.

Proposed future architecture, still fully open:

**A. Automatic Reference Counting.** Not a tracing GC (Java/Go-style) —
ARC like Swift/Rust's `Rc`. Every heap object (strings, arrays, structs)
carries a hidden `ref_count`; the compiler injects increments on
assignment/pass-by-value, decrements on scope exit, and a `free()` call
when the count hits zero. No user-visible `malloc`/`free`, no background
runtime.

**B. Safe references over raw pointers.** Raw `*int` deprecated from the
safe surface language, replaced by `&int` references the compiler
guarantees point to valid memory.

**C. Heap-allocated dynamic strings and arrays.** Strings become a real
struct (`{ i8* data, int length, int capacity, int ref_count }`), mutable
and resizable. Arrays become heap-allocated slices, safely returnable from
functions.

**D. FFI bridge for the above.** A `unsafe_ptr<T>` type retained
specifically for talking to C/the OS, isolated behind an explicit `unsafe`
bridge — `@extern fn write(fd: int, buf: unsafe_ptr<i8>, count: int):
int;`-style declarations, with safe types exposing an intrinsic to
temporarily hand out their raw address (ARC instructed not to free during
the call): `write(1, my_str.get_raw_pointer(), my_str.length());`.

This is a genuine memory-*model* change — automatic retain/release
insertion at every assignment and scope exit — not something any stdlib
module can deliver on its own; it's compiler + Codegen work, gated on
`unsafe {}` blocks existing first (`ROADMAP_v1.0.0_COMPLETE.md` Phase 4 tracks
`unsafe {}` as its own open item). The one already-shipped, purely
stdlib-level stopgap in the meantime is `mem.tr`'s `Pool<T>`/`free_pool()`
(bulk-free tracking) — explicitly *not* a step toward ARC, just a smaller
convenience that needed no language change.

---

## 6. LoRA Training Strategy (SLM fine-tuning)

Fully open, nothing built. Turf-Lang doesn't exist in any base model's
pretraining data, so the SLM (`qwen2.5-coder-1.5b` via `llama.cpp`, see §2)
can't rely on innate knowledge — it needs fine-tuning via two separate LoRA
adapters, which `llama.cpp` can hot-swap/stack on the same base model
without reloading multi-gigabyte weights.

**Adapter 1 — Syntax & Instant Fixes.** Teach the model Turf-Lang's
concrete syntax so it stops hallucinating C/Java/Python shapes in
`code_patch` suggestions.
- *Dataset*: procedurally generate synthetic data (can't scrape GitHub for
  a language with no public corpus) — a code mutator takes passing test
  files, intentionally breaks them (delete a semicolon, swap `int`→
  `string`, drop a closing brace, swap argument order), runs them through
  the real compiler to capture the exact error + context lines, and
  formats the result as ChatML JSONL matching `SLMIntegration.cpp`'s
  existing prompt structure.
- *Training*: Unsloth or Axolotl (GPU-optimized, not raw `llama.cpp` for
  the training phase itself). Rank 16–32 (local pattern matching doesn't
  need more). 2–3 epochs — more risks overfitting to one repeated fix.
- *Integration*: load `syntax_fixer.lora` into the `llama_context`
  specifically when `fetchSuggestion()` is called.

**Adapter 2 — AST Generation (neural parser).** Teach the model to read
raw Turf-Lang text and emit a serialized AST directly.
- *Dataset*: needs an AST serializer (e.g. a new `ASTSerializer.h`) that
  dumps the C++ AST to structured JSON, run over a large corpus of valid
  code (tests + procedurally generated valid programs).
- *Training*: much harder reasoning task than syntax fixing — deep nested
  hierarchical structure. Rank 64–128, targeting *all* linear layers
  (`q_proj`/`k_proj`/`v_proj`/`o_proj`/`gate_proj`/`up_proj`/`down_proj`),
  not just attention.
- *Guardrail*: a strict GBNF grammar for the AST JSON schema, applied via
  `llama_sampler_init_grammar` at inference time, so the model
  mathematically cannot emit malformed JSON the C++ deserializer would
  choke on.

**Tooling pipeline**: procedural C++ generator → JSONL → Unsloth (Colab or
local GPU) → export to GGUF (`export_gguf`) → `llama_model_lora_adapter_set
(model, "syntax_fixer.gguf", 1.0f)` in `SLMEngine.cpp` for hot-swapping.

---

## 7. SLM-Driven Language Paradigms (speculative, not started)

By fully controlling the AST-to-LLVM pipeline and already having a local
SLM integrated, three further paradigms were sketched as uniquely
reachable for Turflang. All three are pure design sketches — none has any
code behind it yet:

- **Prompt-Oriented Programming (POP).** `@slm` blocks where a developer
  writes natural-language intent; the compiler queries the SLM with the
  prompt + current symbol table, retrieves generated AST nodes, injects
  them into the pipeline, and caches the resulting LLVM IR.
- **The Self-Healing Compiler.** AST diffing (`CollectChangedNodeIDs`) to
  auto-resolve syntax/type errors — the SLM proposes an AST patch, the
  compiler validates it against the type checker before applying it back
  to the source. (Distinct from the already-shipped sandboxed
  single-line-suggestion flow in §2 — this would patch the AST directly
  and iterate, not just suggest one replacement line.)
- **Semantic Interface Matching.** A `semantic_cast<T>` operator: the SLM
  compares two structs' fields for conceptual similarity and generates the
  LLVM memory-mapping instructions to convert between them, no boilerplate
  adapter code required.

## 8. Other Core Language Paradigms (speculative, not started)

Three more forward-looking ideas, independent of the SLM work above, none
started:

- **Extreme Compile-Time Execution.** Extend `comptime` using the LLVM ORC
  JIT to let the compiler execute network requests or file IO *during the
  build*, embedding the results directly into the binary — eliminates
  runtime configuration overhead for data that's actually static per-build.
- **Region-Based Memory Management.** An alternative (not a duplicate) of
  §5's ARC proposal: track block scopes and automatically allocate/
  deallocate grouped memory arenas, dropped in O(1) at scope exit, aiming
  for Rust-level safety with Python-level simplicity and no GC pause.
- **Reversible Execution.** A `@reversible` decorator instructing the
  compiler to also generate inverted mathematical/memory instructions in
  the LLVM IR alongside the forward ones, natively supporting "undo" and
  time-travel debugging without a separate logging layer.
