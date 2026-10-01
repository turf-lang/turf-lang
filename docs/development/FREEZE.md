# FREEZE.md — the Semantic Freeze

> **Read this before writing any code for an overhaul feature group.**
> This file is authoritative. `docs/planning/ARCHITECTURE_OVERHAUL.md` §6 is
> the historical copy it was lifted from; where the two ever disagree, this
> file wins.

**Status**: in force since 2026-09-17 (ARCH-001).
**Scope**: the decisions that independently-run implementation sessions would
otherwise each answer differently, producing an incoherent language.

### 6.0 How to use this document

1. A decision recorded here is **not** open for reinterpretation by an
   implementation session. If a session believes a frozen decision is wrong,
   it stops, writes the argument into
   `docs/development/changes/<ID>.md` under *Unresolved questions*, and asks.
2. A decision **not** recorded here is an ordinary engineering judgement —
   make it, and record it in the change record.
3. Adding a new frozen decision requires an explicit request. Sessions do not
   freeze things on their own initiative.

### 6.1 Generics — FROZEN

- Generics are **monomorphized**: one fully-specialized copy of a generic
  function, method, or struct per distinct tuple of concrete type arguments.
- `TurfGenericParamType` exists **only** inside an uninstantiated template.
  Reaching Codegen with one is a compiler bug; Codegen asserts on it.
- `typesCompatible()`'s Rule 1 ("generic param matches anything") applies
  **only** during candidate matching, never after substitution.
- Instantiation depth is capped (default 64) with a dedicated diagnostic
  naming the instantiation chain.
- There is **no** trait/constraint syntax. A generic body that uses an
  operation the concrete `T` does not support is an error reported **at the
  instantiation site**, with a `withRelated()` note at the generic
  declaration.
- Monomorphized symbol names extend the existing mangling
  (`turf_<module>_<name>_<paramty>…`) with the type arguments; the scheme
  must remain a pure function of (module, base name, type arguments,
  parameter types).

### 6.2 Numeric semantics — FROZEN

**Six distinct concepts, never conflated:**

| # | Concept | Rule |
|---|---|---|
| 1 | Lexical validity | `[0-9_]+` (plain decimal int), `0x…`/`0b…`/`0o…` (radix-prefixed int), `[0-9_]*.[0-9_]+([eE][+-]?[0-9_]+)?` (dotted float) or `[0-9_]+[eE][+-]?[0-9_]+` (dotless-exponent float, e.g. `1e6` — corrected 2026-09-18/SEM-001: the original single dotted-float alternative made a bare exponent lexically invalid, contradicting SEM-001's own testing scope and every mainstream language's float grammar; this row now states both alternatives so it no longer self-contradicts). `_` is a separator, never leading or trailing. |
| 2 | Literal representability | An integer literal is held exactly (arbitrary precision) until typed. A float literal is held as the exactly-rounded `double` **plus a flag for whether rounding overflowed to `inf`**. |
| 3 | Destination representability | Checked when the literal's type is fixed. Out of range → **hard error**, never a clamp. A float literal that overflowed to `inf` → hard error. |
| 4 | Implicit conversion | `int32 → int`, `int32 → double`, `int → double`. Nothing else. No narrowing, no `bool` ↔ numeric, no `char` ↔ numeric. |
| 5 | Explicit conversion (`to`) | Per the table in `docs/reference/variables_and_types.md`, **enforced in the TypeChecker**, not Codegen. `double → int` truncates toward zero. `int → int32` truncates. |
| 6 | Arithmetic overflow | Compile-time on untyped literals: exact, arbitrary precision, error only if the *result* is not representable in the destination. Compile-time on typed constants: **error**. Runtime, signed integer: **trap** (panic). Runtime, floating point: IEEE (`inf`/`NaN`), **no trap**. |

- `-9223372036854775808` is writable: unary `-` applied directly to an
  untyped integer literal folds into the literal *before* representability is
  checked.
- Division and modulo by zero continue to trap at runtime for both `int` and
  `double` (existing behaviour, deliberately preserved — it is why IEEE
  `inf`/`NaN` are largely unreachable in TurfLang, and that asymmetry is now
  a documented decision rather than an accident).
- `int` is 64-bit signed; `int32` is 32-bit signed and exists solely for
  honest `extern fn` ABI declarations. **No further integer widths, and no
  unsigned types, are to be added.**
- Semantics **never** vary with optimization level.
- **Two accepted narrowings** (OVH-014, `docs/development/changes/OVH-014.md`
  — recorded 2026-09-27, per this file's own §6.0 point 3: an explicit
  request, not a session's own initiative). Both only *reject* a program this row's
  literal wording would accept; neither ever accepts something wrong or
  miscompiles:
  - Row 6's "only the *final* value is checked" is not literal: a >int64
    literal *leaf* nested inside a larger untyped-literal-arithmetic
    expression (`BIG - BIG2 + 1`, where `BIG`/`BIG2` individually overflow
    `int` but the true sum does not) is rejected at the leaf, immediately,
    not deferred to the expression's actual final value. Reason: Codegen
    truncates every integer literal through a 64-bit `int` before any
    arithmetic runs (SEM-001) — accepting the fully general reading would
    require Codegen-side exact constant folding this compiler does not
    build, for a shape no test or stdlib/showcase program needs (full
    account: `docs/development/changes/SEM-002.md`, *Known pitfalls*,
    second bullet).
  - An untyped integer literal assigned directly to `double`
    (`docs/planning/ARCHITECTURE_OVERHAUL.md` §10.5's "must be exactly
    representable" as a `double`) is checked against `int64` representability
    FIRST, not double-exactness directly — so a literal wider than `int64`
    but exactly representable as a `double` (one significant bit, e.g. 2^100
    written out in decimal) is rejected, citing `int`, not accepted as the
    literal §10.5 wording would allow. Reason: Codegen converts an integer
    literal to `double` by first truncating it through a 64-bit `int`, so
    accepting such a literal without also teaching Codegen to convert
    directly from the exact arbitrary-precision value would silently produce
    the WRONG `double` (full account:
    `docs/development/changes/SEM-002.md`, *Known pitfalls*, third bullet).

### 6.3 `guess` / inference — FROZEN

- `guess` is a **type-position keyword**: `mut x: guess = 5;`. Omitting the
  annotation entirely (`mut x = 5;`) is defined as exactly `: guess`.
- **Model A only.** The type is inferred from the initializer and is
  **permanently fixed** at the declaration. `mut x = 5; x = 5.5;` is an
  error.
- **Model B is rejected.** A declaration with `guess` and no initializer is a
  compile error. There is no flow-sensitive type assignment anywhere in the
  language.
- Inference is **local, one-directional, and single-expression**. It never
  applies to a function parameter, a return type, a struct field, an
  `export immut` constant, or a generic type argument.
- Defaulting for untyped literals under `guess`: integer → `int`,
  float → `double`, `true`/`false` → `bool`, `'c'` → `char`, `"s"` →
  `string`, `[…]` → `T[N]`. **The default type is chosen first; then
  representability is checked against it.** `mut x = 999999999999999999999;`
  is therefore an error, not a promotion to a wider type (there is no wider
  type).
- `guess` never infers `void`, never infers the bare type of `null`, and
  never infers an uninstantiated generic.

### 6.4 Initialization — FROZEN

- **One** mechanism: **definite assignment**, for every type without
  exception. Reading a body-declared local before it is definitely assigned
  on every path is `DefiniteAssignmentError` (E008).
- Storage is still always zero-filled. Definite assignment is a purely static
  safety net over memory that already reads as zero — this property is
  preserved verbatim from today and is what makes the check removable in
  principle without becoming a memory-safety hazard.
- The eager auto-default (Mechanism B) is **removed**. `T::default()` is the
  explicit replacement and already exists.
- `immut`/`immutable` still require an initializer.
- **A struct may not contain itself by value, transitively.** Checked once in
  the Resolver, before any layout or default value is computed. The
  diagnostic names the full cycle.
- Struct *fields* are not subject to definite assignment; a struct value is
  either fully constructed (`::new` — already checked by
  `ConstructorFieldAssignmentPass`) or fully defaulted (`::default`).

### 6.5 Memory model — FROZEN

- **Two disjoint heaps.** The **GC heap** (reachability-managed) and the
  **manual heap** (`mem::alloc`/`malloc`, entirely the programmer's). A
  pointer never migrates between them, and the collector never traces into
  manual memory.
- The collector is **non-moving** (object addresses are stable for life),
  **mark-sweep**, **precise on the heap** (per-type pointer bitmaps emitted
  by the compiler), **conservative on stack and registers**, **stop-the-
  world**, **single-threaded**.
- **Non-moving is non-negotiable.** A `string` handed to `write(2)` or a
  buffer handed to `send(2)` must not move; TurfLang's stdlib is
  overwhelmingly FFI.
- **No finalizers.** Deterministic cleanup for OS resources is `defer`.
- What lives on the GC heap: `string` values produced by any operation,
  collection backing buffers, closure environments (if closures land), boxed
  values. What does not: stack locals, `T[N]`, struct values, `mem::alloc`
  results, string literals (static `.rodata`).
- The GC entrypoints (`turf_gc_alloc`, `turf_gc_collect`,
  `turf_gc_add_root`, `turf_gc_remove_root`) live in `libturfrt` and are
  reached through the closed intrinsic list, never by an ad-hoc `extern fn`.
- **Precise heap tracing requires monomorphization** (F-GEN-1). GC must not
  be implemented before it.

### 6.6 Pointers and references — FROZEN

| Type | Nullable | Checked | Where legal |
|---|---|---|---|
| `&T` | no | compile-time non-null | anywhere |
| `&?T` | yes | must be null-tested before use (`NullabilityPass`) | anywhere |
| `T*` | yes | none | `extern fn` signatures, and inside `unsafe { }` in a module that declares the raw-pointer capability |
| `ptr` | yes | none | same as `T*` |

- `&T` → `&?T` → `T*` widens implicitly; nothing widens back without an
  explicit `unsafe`-gated `to`-cast.
- `self` remains a raw `StructName*` internally and remains exempt. Do not
  change this.

### 6.7 Operators — FROZEN

- Overload resolution is **exact type match** (`sameTypeExact`), never
  `typesCompatible()`. This is deliberate and long-standing: it is what keeps
  `operator+(string,string)` from being reached by unrelated widened
  operands, and it makes resolution predictable.
- Overloads are stored in the **ordinary name-keyed overload map** under a
  synthesized name (`"operator+"`). No separate operator table.
- Overloadable, after this milestone: binary `+ - * / % ** == != < > <= >=
  & | ^ << >>`; unary `- ! ~`; indexing `[]` (read) and `[]=` (write).
- **Not** overloadable, deliberately: `&&`, `||` (short-circuit cannot
  survive a call), `=` (assignment), `()` (call), `.` (member access), `to`
  (conversion), and any compound assignment.
- Compound assignment **desugars**: `a op= b` ≡ `a = a op b`, and therefore
  uses `operator op`. There is no `operator+=`.
- A declaration whose operands are **all** compiler-primitive
  (`int`/`int32`/`double`/`bool`/`char`/pointer) is rejected —
  `OperatorBuiltinPairError`, existing behaviour, preserved. `string` is
  deliberately **not** in that set, because its operators are stdlib-defined
  and must stay that way.

### 6.8 Standard library, compiler, runtime boundary — FROZEN

- Rules B1/B2/B3 of §3.3, reproduced here so this file stands alone:
  > **B1 — the compiler owns anything whose correctness depends on TurfLang's
  > type system** (monomorphization, GC descriptor emission, overload and
  > operator resolution, bounds-check elimination). Corollary: `to`-cast
  > legality moves out of Codegen and into the TypeChecker.
  >
  > **B2 — the runtime owns anything that must be true across the whole
  > process and cannot be re-derived per translation unit** (the heap, the
  > collector, the root set, panic behaviour, the OOM policy). Written in C,
  > versioned lock-step with the compiler, deliberately *not* in TurfLang.
  >
  > **B3 — everything else belongs in `lib/std` as ordinary TurfLang, and the
  > standard library gets no capability a user cannot request explicitly.**
- The intrinsic list is **closed and enumerated in one header**
  (`include/Intrinsics.h`). Adding an intrinsic is an explicit,
  change-recorded act.
- Standard-library privileges are declared in source (a module-level
  capability declaration), never inferred from a file path.
- `libturfrt` is written in C, versioned lock-step with the compiler, and
  linked into every binary.
- `string` stays a NUL-terminated byte pointer.

### 6.9 Diagnostics — FROZEN

- `DiagnosticEngine` remains the single sink; one primary error per line.
- Every `TurfError` subclass carries its own code and category. **Deriving a
  category by matching text in the message is to be deleted**, not extended.
- SLM: grammar-constrained output and sandbox verification are permanent.
  An unvalidated suggestion is capped below the display gate.
- No new `ErrorCategory` values or `docs/slm/` category files without an
  explicit request.

### 6.10 Optimization — FROZEN

- Optimization levels change compile time, run time, binary size and
  debuggability. They **never** change observable behaviour.
- Language-mandated runtime checks (bounds, division by zero, integer
  overflow trap) may be eliminated **only** by proof, never by flag.
- LLVM owns instruction selection, register allocation, generic scalar and
  loop optimization, object emission, and DWARF encoding. TurfLang owns
  anything requiring TurfLang semantics.

### 6.11 Type identity — FROZEN (FDN-003, 2026-09-27)

Recorded on the project owner's explicit approval of `ROADMAP_v2.0.0.md`
FDN-003 stage 1 ("Roadmap design, stable typeof") — §6.0 rule 3's explicit
request, not a session's own initiative. Full account:
`docs/development/changes/FDN-003.md`.

- A struct/enum/machine/`error` type is identified by **(owning module,
  name)**, never by its bare name. Two modules may each declare `Node`; a
  program may declare `struct List` without importing `list`.
- The main program file's module is named **`main`** (DEC-13). That
  qualifier is never spelled: a main-file type's identity, `typeof()` output,
  LLVM name and diagnostics are its bare name, exactly as before FDN-003.
- **Spelling rules are unchanged.** Bare `Name` means the current file's or
  module's own type, or one it on-demand-imported (`import m::Name;`,
  `m::[...]`, `m::[*]`); `m::Name` is always allowed. The Resolver is the
  single place a spelling becomes an identity.
- A bare spelling that could mean **two** visible types is an error at the
  use (E085), naming both. No "local wins" rule: a too-strict rule can be
  relaxed after 2.0 without breaking anyone; the reverse cannot. Two
  on-demand imports bringing in the same bare type name remain an error at
  the import (E024), as for functions.
- A duplicate declaration is an error only within **one** module (E034).
- `typeof()` and diagnostics print the identity — `m::Name` for a
  module-owned type, bare for a main-file type — the **same string wherever
  `typeof` is called** (not "qualified only outside the declaring file").
- LLVM struct names, GC type-descriptor names and DWARF type names spell a
  module-owned type **`m.Name`**.

### 6.12 Safe heap allocation — FROZEN (FDN-001, 2026-09-27)

Recorded on the project owner's explicit approval of `ROADMAP_v2.0.0.md`
FDN-001's DEC-5 (`heap(expr): &T` intrinsic, over a `new T`-style keyword) —
§6.0 rule 3's explicit request, not a session's own initiative. Full
account: `docs/development/changes/FDN-001.md`.

- `heap(expr)` is a closed-list bare intrinsic (`include/Intrinsics.h`
  category 1), not new grammar: `heap(e: T): &T` for any sized `T` (struct,
  fixed array, primitive) whose static type is known at the call site.
  `void` is rejected (nothing to copy).
- Copies `e`'s evaluated value into a fresh GC-heap block sized
  `sizeof(T)`, allocated with `T`'s own real `TurfTypeDesc` (a named
  struct's whole-instance descriptor, or the general per-LLVM-type
  descriptor for anything else) — never `NULL`/opaque — so the collector
  traces a `heap()`ed value's own pointer fields exactly as it would a
  stack-resident `T`.
- No `unsafe { }` gate: `&T` is legal anywhere (FREEZE.md §6.6), and
  `heap()` produces exactly that — a compiler-checked non-null reference,
  never a raw `T*`.
- Does **not** by itself close the general "a `&U` field inside `T` may
  point at a stack frame that has since popped" hazard — that is FDN-002's
  escape-analysis rule, applied to `heap()`'s own operand like any other
  expression; FDN-001 does not gate on it.

### 6.13 Reference and array escape rules — FROZEN (FDN-002, 2026-09-27)

Recorded on the project owner's explicit approval of `ROADMAP_v2.0.0.md`
FDN-002's own design (no numbered DEC- item — the roadmap states the design
directly rather than a table of alternatives) — §6.0 rule 3's explicit
request, not a session's own initiative. Full account:
`docs/development/changes/FDN-002.md`.

- Every reference-typed value (`&T`, `&?T`, a raw pointer including `self`)
  and every unsized-array (`T[]`) parameter carries one of three tracked
  provenances at each program point, computed by an intraprocedural,
  conservative forward dataflow pass (`EscapePass`, beside `NullabilityPass`
  in the same CFG-pass pipeline stage): **Stack** (points into the current
  function's own frame), **Param** (came in from the caller, including
  `self`), **Heap** (from `heap()`, or reached through a `Heap`-provenance
  base). Anything the pass cannot classify is **Unknown** — never flagged.
- It is an error for a `Stack`-provenance value to flow into: a `return`; the
  RHS of an assignment into a field reached through a `Param`- or
  `Heap`-provenance base; an argument to a `List`/`Map`/`Set`/`Stack`/`Queue`
  mutator (`push`/`set`/`put`/`add`/`enqueue`) or a real `operator[]=`
  dispatch; the operand of `heap()`.
- An unsized `T[]` parameter is `Param` provenance, but converting it to
  `ptr` (or any raw, non-reference pointer type) and returning that cast's
  result is still an error — the cast itself discards the type system's only
  memory of where the pointer aliases (§27 D23).
- No lifetime annotations, no borrow checker, no interprocedural summaries.
  `Param`- and `Heap`-provenance values are always allowed to flow anywhere
  (including a `return`) — this pass trusts a reference the caller already
  handed in for exactly as long as ordinary reference semantics already did,
  and does not attempt to verify further up the call chain.

### 6.14 String indexing — FROZEN (FDN-004, 2026-09-28)

Recorded on the project owner's explicit approval of `ROADMAP_v2.0.0.md`
FDN-004's own design (D17's own "— DECISION —" marker in that document) —
§6.0 rule 3's explicit request, not a session's own initiative. Full
account: `docs/development/changes/FDN-004.md`.

- `s[i]` on a `string` has static type `char`, not `int`. `char` already
  exists and is one byte (F-STD-3); returning `int` was the pre-existing
  inconsistency this closes. Breaking (R1): after 2.0 this can only change
  again in 3.0.
- A **read** (`s[i]` used as a value) is bounds-checked at runtime against
  `strlen(s)`: `i < 0` or `i >= strlen(s)` panics. There is no compile-time
  elision for a string index (unlike a fixed-size array's `ValueRangePass`
  constant folding) — every read pays a fresh `strlen()` call, which LLVM's
  own loop-invariant code motion hoists out of a loop whose string base does
  not change per iteration at `-O2`, but not at `-O0`.
- A **write** (`s[i] = c;`) is checked for `i < 0` only. There is
  deliberately **no** upper-bound check on a write — a `string`'s only
  length signal is `strlen()`, which reads as 0 (or however many bytes
  happen to precede the write cursor) for a buffer still being filled
  before its NUL terminator is written, exactly how every `strings.tr`
  string-building function works. This is a documented, permanent asymmetry
  between the read and write sides, not a gap to close later — there is no
  way to recover a `string` value's real allocated capacity to check
  against instead (F-STD-3).
- `strings::byte_at_unchecked(s, i): char` is the stdlib escape hatch for a
  caller that has already established `i` is in range some other way and
  cannot afford the repeated `strlen()` — unchecked, like any other raw
  pointer read.

### 6.15 Per-declaration visibility — FROZEN (LEC-002, 2026-09-29)

Recorded on the project owner's explicit instruction to start LEC-002 at
stage 1 with `ROADMAP_v2.0.0.md` as the authority (DEC-7's recommendation,
"private by default, `pub`"), plus one owner amendment to the roadmap's
table: the keyword has **two spellings**, exactly as `mut`/`mutable` and
`immut`/`immutable` do. §6.0 rule 3's explicit request, not a session's own
initiative. Full account: `docs/development/changes/LEC-002.md`.

- **Default: private to the declaring module.** A top-level declaration is
  visible to another module only if marked `pub`. Breaking (R1): after 2.0
  this can only change again in 3.0.
- **Keyword: `pub` or `public`, synonyms.** Both lex to one token
  (`TOK_PUB`), the way `mut`/`mutable` share `TOK_MUTABLE`. There is no
  semantic, stylistic or diagnostic difference between them; a diagnostic's
  hint spells it `pub`. Both become reserved words (checked 2026-09-29: no
  identifier named `pub` or `public` exists in `tests/`, `lib/std/` or
  `showcase/`; the word occurs only in prose comments).
- **Position: a prefix modifier** on the declaration it applies to
  (`pub fn f()`, `pub fn Point.len()`, `pub struct S`, `pub x: int` inside a
  struct body).
- **What can be `pub`:** `fn`, methods (instance and `static`), `struct`,
  `enum`, `error`, `machine`, struct **fields**, and `extern fn`
  (engineering judgement, recorded in the change record: `extern fn` is a
  named top-level function, so the uniform rule applies to it).
- **Fields are private unless `pub`, even in a `pub struct`.** This is what
  lets `List<T>.data` (a raw pointer) be hidden. `T::default()` stays legal
  from any module — it zero-fills and cannot break an invariant that zero
  cannot. `T::new()` is an ordinary method with its own visibility.
- **Enum/error variants and machine states are public iff their type is.**
  No per-variant privacy.
- **Operators are visible wherever their operand types are.** `pub` on an
  `operator` declaration is rejected (with a hint saying why): dispatch is
  type-directed (F-OP-3) and the orphan rule already ties an operator to a
  type's module; a private operator would make `a + b` depend on import
  visibility.
- **`export immut` keeps its meaning** — `export` *is* its `pub`. `pub`
  combined with `export` is rejected, with a hint to drop one. A plain
  top-level `immut`/`mut` remains illegal; private top-level constants are
  additive later.
- **Private-in-public is an error:** a `pub` function signature (parameters
  and return type) or `pub` field may not mention a private type, or an
  importer could hold a value of a type it cannot spell.
- **UFCS respects visibility:** a private function is not a UFCS candidate
  outside its module. Call syntax never bypasses visibility.
- **Generics:** a `pub` generic's body may call private helpers; the
  monomorphized clone is resolved in the **template's** module context,
  never the instantiation site's.
- **Packages (LEC-001):** exactly two levels, module-private and `pub`. A
  package's internal modules are not importable from outside the package, so
  their `pub` items are package-visible. `pub(package)` is FUTURE.
- **Where `pub` is meaningless it is an error, not ignored:** on a local
  variable or a statement, on `import`, on `operator`, on a `machine`
  transition line, on a parameter — always with a hint saying what to do
  instead. A silently-ignored modifier is how a typo becomes a policy.
- **Single-file programs are unaffected.** Visibility is only ever checked
  across a module boundary; a main program's own declarations never need
  `pub`, and a stray `pub` on them is accepted.
- **Diagnostic contract (owner-directed emphasis on hints).** Every
  visibility diagnostic is a typed `TurfError` with its own code, carries a
  **hint that names the concrete fix** (which file to edit and which keyword
  to add, or which public function/constructor to call instead), and names
  the declaration site with a related-location note. Diagnostics for the
  same rule differ by *what* is private (function, type, field, method,
  constant) in their wording and hint, not by adding an error class per
  kind. No new `SLM::ErrorCategory` and no `docs/slm/` files (§6.9).
- **Non-goals:** `pub(package)`/`protected`, friend modules, per-variant
  visibility, private top-level constants.

### 6.16 Projects, packages and the toolchain — FROZEN (LEC-001, 2026-09-30)

Recorded on the project owner's explicit instruction to start LEC-001 with
the freeze entry (§6.0 rule 3's explicit request, not a session's own
initiative). `ROADMAP_v2.0.0.md` LEC-001 entries 2–6 are frozen as
recommended. Entry 1 (DEC-8, "one binary: project commands in the compiler
driver, `turf-cli` not shipped") is **overturned by the owner**: the Go CLI
becomes the `turf` front end and the compiler is renamed `turfc`, both in
this repository. The roadmap's "no two programs called `turf`" is kept.
**Amended by the owner the same day**: its "one version, lock-step" was
first kept as "same tree, same version, same archive", and is replaced by
independent versions plus an interface check (the next bullet and *Driver
interface* below). Full account: `docs/development/changes/LEC-001.md`.

**The toolchain (F-PKG-1)**

- **Two programs, independently versioned.** `turf` is the Go program under
  `cli/`; `turfc` is the compiler (`src/`, today's `build/turf`).
  - Each has its own release version and may grow on its own schedule, for
    example TurfLang 2.0.0 with CLI 1.0.0. `turf version` shows both.
  - What must agree is the **driver interface** (below), not the versions.
  - They live in one repository today. The owner plans to move the CLI out
    before the public release, which this rule already allows.
  - A release archive may still bundle both, as a distribution convenience
    (REL-003).
- **`turf` is the user's tool; `turfc` is the tooling interface.**
  - User-facing documentation, help text and diagnostic hints name `turf`
    (`turf explain E090`, `turf check`).
  - `turfc` is documented for tool authors (the LSP, editor plugins, build
    systems) and for the compiler's own test scripts.
  - Users are never asked to type `turfc`.
- **The boundary** — the same kind of rule as §6.8's B1/B2/B3:
  - **The compiler owns everything whose answer depends on TurfLang
    source:** parsing, which file an `import` names (every ambiguity and
    collision error), visibility, the internal-module rule, and the
    `unsafe module;` capability check. `turfc` never touches the network,
    never reads `turf.toml` or `turf.lock`, and never searches for them.
  - **The CLI owns everything about files, network and reproducibility:**
    the manifest, dependency fetching, the cache, the lockfile, hashing,
    `--offline`/`--locked`, and running the built executable. `turf` never
    parses TurfLang source, never decides which module an import names, and
    never re-renders or rewrites a compiler diagnostic.
  - **They meet at an explicit package graph** that `turf` passes to
    `turfc`. For each package it gives the name, the source root, the names
    and roots of its **direct** dependencies, and whether its `unsafe
    module;` declarations are honored. The concrete form (repeated flags or
    a plan file) is an engineering judgement for the stage that implements
    it. `turfc` given no graph is in single-file mode.
- **`turfc` stays a complete compiler on its own.** Every `turf` command is
  one or more `turfc` invocations plus file and network work. The CLI adds
  convenience, never a compiler capability. Test scripts, the SLM Sandbox
  and the LSP keep using the compiler (or the SMP-001 facade) directly.
- **The one exception to "turfc never looks for a manifest":** when an
  `import` fails to resolve in single-file mode and a `turf.toml` exists in
  the source file's directory or an ancestor, the not-found diagnostic adds
  a hint naming `turf build`. The file is only checked for existence, never
  read.
- **Finding the compiler**, first match wins:
  1. `$TURF_COMPILER` (the variable `scripts/test_runner.sh` and CI already
     use for exactly this).
  2. `turfc` in the directory of the running `turf` executable, with
     symlinks resolved. This is the installed layout and the dev layout
     (`build/turf` beside `build/turfc`).
  3. `$TURF_HOME/bin/turfc`.
  4. `turfc` on `PATH`.

  No match is an error with a hint naming `TURF_COMPILER`.
  Step 3 is how an independently installed `turf` and `turfc` find each
  other.
- **Driver interface.** The contract between the two is a versioned
  interface, `major.minor`, that `turfc` reports on line 2 of
  `turfc --version` as `interface <major>.<minor>`. Line 1 stays
  `turfc <release version>`.
  - It covers exactly what `turf` relies on:
    - the flags it passes (`--check`, `--module`, `--dump-*`, `--format`,
      `--write`, `--migrate=`, `--explain`, `-o`, and from stage 4 the
      package-graph input);
    - the `--version` format;
    - the exit statuses (0 success, 1 a reported error);
    - passthrough of every other argument unchanged.
  - An additive change (a new flag or input) bumps the minor. Removing or
    changing the meaning of anything covered bumps the major.
  - `turf` declares the interface it needs. It drives a `turfc` whose major
    is equal and whose minor is at least the one it needs.
  - Before driving the compiler, `turf` checks this and refuses an
    incompatible compiler. The error names both release versions, both
    interface versions, both paths, and which side to upgrade.
    `TURF_COMPILER` does not bypass the check.
  - A differing *release* version is never an error on its own.
- **Passthrough.** When the first argument is not a subcommand name,
  `turf <args…>` runs `turfc <args…>` with the arguments, stdout, stderr
  and exit status all unchanged. So `turf file.tr -o x` keeps working
  exactly as it does today. Exceptions: `--version` and `--help`/`-h` are
  the CLI's own; a bare word that neither starts with `-` nor ends in `.tr`
  is an unknown command, reported with a spelling suggestion rather than
  passed to the compiler as a missing file.
- **Command surface for 2.0** (names frozen; each command's options are
  engineering judgement, recorded in the change record):
  - **Any file, no manifest needed:** `compile`, `run <file>`, `check`,
    `fmt`, `explain`, `migrate`, `dump`, `version`, `help`. Flags `compile`
    does not recognise are forwarded to `turfc` unchanged, so a new compiler
    flag works through `turf` the day it lands.
  - **Project:** `new`, `init`, `build`, `run`, `test` (REL-002), `clean`,
    `fetch`, `add`, `remove`, `update`. `run` without a file argument means
    the project.
  - **`publish`** is reserved. In 2.0 it exits non-zero with a message
    saying the registry is FUTURE F-7c.
  - The pre-2.0 `turf-cli` command `install` is **not** a 2.0 command: what
    it did is `fetch`, and `install` is left free for installing a built
    program later (FUTURE).
- **Exit status.** `turf compile`/`check`/passthrough return `turfc`'s
  status unchanged; `turf run` returns the program's. Codes for the CLI's
  own failures (usage, manifest, fetch) are REL-001's decision.

**Layout, manifest and module resolution (F-PKG-2)**

- **Package layout** (roadmap entry 2):
  ```
  myapp/
    turf.toml          # manifest
    turf.lock          # generated by turf; committed
    src/main.tr        # present => the package builds an executable
    src/<name>.tr      # present => the package is a library; its root module
    src/**.tr          # internal modules
    tests/*.tr         # `turf test` (REL-002)
  ```
  A package may have both entry files. Only a package with a root module
  can be a dependency; depending on one without it is a build-time error.
- **Manifest** (roadmap entry 3): `turf.toml`, read by `turf` with a real
  TOML parser; the accepted keys are documented in
  `docs/reference/projects_and_packages.md`.
  - `[package]` requires `name` and `version`. `turf` is optional and gives
    a minimum compiler version whose major must match.
  - `name` must be a valid TurfLang identifier and not a reserved word,
    because it is spelled in `import`.
  - `[dependencies]` entries are inline tables: `{ path = "…" }`, or
    `{ git = "…", tag = "…" }` with exactly one of `tag`, `rev` or `branch`.
    A registry-style string entry (`math = "^1.2.0"`) is an error whose hint
    points to `path`/`git` (registries are F-7c).
  - `[unsafe] allow` is covered under F-PKG-4 below.
  - An **unknown key or table is an error**, not ignored, for the same
    reason a stray `pub` is (§6.15): a silently-ignored setting is how a
    typo becomes a policy. Pre-2.0 `turf-cli` manifests (flat `name =`,
    string versions) are not accepted; that tool never shipped, so there is
    nothing to migrate.
- **Module identity** (roadmap entry 4, DEC-9) is **(package, path under
  `src/`)**. Two packages' internal `util.tr` are two modules, and their
  types are distinct under F-TYPE-1 (§6.11), whose module component becomes
  package-qualified where needed. In single-file mode the package is
  implicit and identities are exactly as today.
- **Import resolution in a package-graph build.** From a module of package
  P, `import foo;` looks for:
  - (a) P's own module named `foo`: `src/**/foo.tr`. More than one match is
    itself an ambiguity.
  - (b) A **direct** dependency of P named `foo`, meaning its root module.
    Transitive dependencies are not importable; to use one, P lists it
    itself.
  - (c) The stdlib module `foo`.

  Exactly one of (a)/(b)/(c) must exist. **More than one is a hard error
  naming every candidate by path and kind** (own module / dependency /
  stdlib), with a hint naming the fix. There is never silent shadowing and
  never "own wins". `import foo::bar` resolves `foo` by the same rule.
  `lib/usr/**`, the source file's directory and the cwd are **not**
  searched.
- **Internal modules are package-private.** From outside its package, only a
  package's root module is reachable, so an internal module's `pub` items
  are visible within the package only (§6.15's two levels).
- **Names that could never be imported are rejected early:**
  - When the manifest is read, a dependency whose name equals a stdlib
    module name is an error.
  - So is a library package whose own name equals one (`turf new`/`init`
    refuse it, and so does `turf build`).
- **Single-file mode** keeps today's search order (install stdlib →
  `lib/usr` → source directory → cwd) but examines **every** root, and more
  than one distinct file (by canonical path) is the same ambiguity error.
  The silent stdlib shadow (audit F-19) becomes an error (R1, safe). The
  same file reached twice, for example when the source directory is the
  cwd, is one candidate, not two. `lib/usr` stays in single-file mode until
  REL-003 retires it.

**Fetching and reproducibility (F-PKG-3)** (roadmap entry 5)

- **Sources in 2.0 are `path` and `git` only.** Git is invoked through the
  system `git` binary. Fetched trees live under
  `<os.UserCacheDir()>/turf/src/<name>-<commit>/` (Linux:
  `$XDG_CACHE_HOME/turf/…`, default `~/.cache/turf/…`). The cache is
  disposable: deleting it only costs a re-fetch.
- **`turf.lock` belongs to the root package only**; a dependency's own
  `turf.lock` is ignored.
  - A git entry records the package name, the source URL, the requested
    ref, the resolved commit, and the SHA-256 of the fetched tree (VCS
    metadata excluded, paths sorted, independent of mtimes and
    permissions).
  - A path entry records the name and path, with no hash: a path dependency
    is live source.
- **The hash is verified on every build**; a mismatch is a hard error (tamper
  or cache corruption) that names the package and the cache path.
- **`turf build` fetches as needed.** `turf fetch` resolves, fetches and
  locks without compiling.
  - `--offline` never touches the network, and a missing cache entry is an
    error.
  - `--locked` fails if the lockfile would change.
  - `turf update [name]` is the only command that moves a `branch`
    dependency, or re-resolves a `tag` that has moved, to a new commit.
- **One version per package name.** Two routes to the same name with
  different sources or commits (a diamond that disagrees) is a hard error
  naming both requirers. There is no multi-version support.

**Capabilities across packages (F-PKG-4)** (roadmap entry 6)

- A dependency's `unsafe module;` declaration is honored only if the **root**
  manifest's `[unsafe] allow` lists that dependency by name. Listing is not
  transitive: the root names every package, direct or indirect, that needs
  it. A name in `allow` that is not in the graph is an error.
- **The root package's own modules are honored without listing.** The
  manifest's author is the author of that code;
  `--allow-unsafe-modules` exists for the case with no manifest to express
  consent. `lib/std` stays unconditional (install root, INV-CAP-1).
- The CLI never inspects source for `unsafe module;`: it passes each
  package's allow bit in the graph, and the compiler enforces it with its
  existing capability check (`HasRawPointerCapability`), made per-package.

**Non-goals:** registry, `publish`, semver ranges (F-7c); workspaces; build
scripts; separate compilation (F-7a); binary packages and a Turf ABI
(F-7b); per-package compiler flags; dependency renaming; multiple versions
of one package; `pub(package)`; path-qualified imports of internal modules
(`import a::b` keeps its existing meaning, a symbol import).

### 6.17 Pattern matching — FROZEN (LEC-003, 2026-10-01)

Recorded on the project owner's explicit instruction to implement LEC-003 with
`ROADMAP_v2.0.0.md` as the authority (DEC-10's recommendation: a top-level
`matches expr` is unchanged, destructuring forms bind) — §6.0 rule 3's explicit
request, not a session's own initiative. Full account:
`docs/development/changes/LEC-003.md`. The roadmap marks four points for
confirmation; each is settled below.

- **Backward compatibility first.** `matches <expr>:` at the top of a header
  keeps its exact meaning: equality against a value expression (which may be a
  variable, e.g. a string). A bare identifier at the top of a header is still
  that variable, never a binding. Every `compare` that uses only value patterns
  keeps its original `switch`/`strcmp` code generation.
- **Arms are tried in source order; the first header that matches wins.**
  Confirmed against the current lowering (a duplicate value pattern is already
  rejected, so order never mattered for plain values) and frozen: a guard that
  fails falls through to the next header, not out of the `compare`.
- **Reserved word:** `_` alone is the wildcard and is reserved everywhere a name
  is expected (its own token). `_name` — an underscore followed by anything — is
  an ordinary identifier, and by convention silences the unused-variable
  warning (the hint always promised this; it was untrue while `_x` did not lex).
- **Pattern forms.** Wildcard `_`; struct destructuring `Name { field: pattern,
  field, .. }` (also `module::Name`); a value sub-pattern inside a struct
  pattern: an integer (`5`, `-5`), `char`, `string` or `bool` literal, or an
  enum/machine variant. **Inside a destructuring pattern a bare identifier
  binds** (Rust's rule); to compare a field with a variable or any expression,
  bind it and use a guard. A `double` literal is not a pattern.
- **A struct pattern lists every field or ends in `..`.** A field left
  unmentioned without `..` is an error (E092), so a pattern cannot keep
  compiling, meaning something different, after the struct gains a field.
  Fields are read exactly as `p.field` reads them: another module's private
  field is E088 (and must be waved off with `..`).
- **Subjects a struct pattern accepts:** a struct value, a safe `&T`, and
  `self`. A `&?T` (may be null) and a raw `T*` are refused with a hint.
- **Guards:** `matches <pattern> if <bool expression>:`. A guard belongs to its
  header; bindings of that header are visible in it.
- **Alternatives:** existing header stacking only (no nested `|` in 2.0). Every
  stacked header of an arm must bind exactly the same names at the same types
  (the shared body reads one symbol per name), else E092. Each header keeps its
  own guard.
- **Bindings** are immutable copies of the matched value, scoped to the arm's
  guards and body, shadow any outer name for that arm only, and are definitely
  assigned on entry to the arm (the CFG models them as definitions on the edge
  into the arm; each guard is a real two-way branch).
- **The subject is evaluated once** and snapshotted; every header and guard
  tests that one value.
- **Exhaustiveness.** With no `otherwise` and no unguarded catch-all, an enum,
  machine or `bool` subject must have every value covered by an **unguarded**
  header (E093); a guarded arm never counts. An error value (`AnyErrorType`, an
  open union) always needs `otherwise` or an unguarded `_` (E093). An `int`,
  `string` or `char` subject, or a struct with a refutable pattern, may still
  fall through to "no arm ran" (existing behaviour; changing it would be R1
  breakage with no soundness gain). A struct pattern is irrefutable when every
  field pattern is a binding, `_` or an irrefutable struct pattern; an
  unguarded irrefutable header is a **catch-all**.
- **Unreachable arms** (an arm, or `otherwise`, after an unguarded catch-all)
  are warnings (E094), once per site. The pre-existing hard error for a
  duplicate unguarded value pattern is kept (not relaxed to a warning: every
  existing test depends on it, and relaxing it would buy nothing).
- **`compare` stays a statement.** An expression form is FUTURE (F-14).
- **Excluded from 2.0:** range patterns, `double` literal patterns, nested
  or-patterns, enum payloads (plain enums stay ordinal-only), and error-variant
  destructuring, which arrives with LEC-004 on top of the same machinery.
- **Diagnostic contract (owner emphasis on hints, as §6.15).** Typed errors
  with codes (E092 invalid pattern, E093 non-exhaustive compare, E094
  unreachable arm) and a hint that names the concrete fix. No new
  `SLM::ErrorCategory` and no `docs/slm/` files (§6.9).

### 6.18 Error payloads and propagation — FROZEN (LEC-004, 2026-10-01)

Recorded on the project owner's explicit instruction to implement LEC-004 with
`ROADMAP_v2.0.0.md` as the authority (DEC-11: prefix `try`; DEC-12: N fields
or N+1 with a trailing message) — §6.0 rule 3's explicit request, the same
footing §6.17 was frozen on. Full account:
`docs/development/changes/LEC-004.md`.

- **Declaration.** `error IOError { NotFound(path: string), Denied(path: string,
  mode: int), Timeout }`. A variant's fields are named and typed, separated by
  commas; a variant with none is written bare, exactly as before (and
  `error Name;` is unchanged). Field names are unique within a variant. A field
  may have any sized type — a primitive, a struct, a generic instance, a
  pointer, an array — except an error type, a `!T`, or `void` (an error value
  does not contain another error). A raw-pointer field needs `unsafe` exactly
  as a struct field does. A type spelled in a field resolves under FDN-003's
  identity rules, like a struct field's.
- **Construction is positional:** `IOError::NotFound(p)`. A variant with *N ≥ 1*
  fields takes *N* arguments (the fields), or *N+1* where the extra trailing
  argument is the STD-002 `string` message; a variant with no fields takes
  exactly one (the message), unchanged. Any other count is an error naming the
  declared fields. The message stays universal: `err.message` still reads it,
  `""` when absent, on an open-union `err` with no destructuring.
- **A bare `E::V` for a variant that has fields is not a value.** It can be
  neither returned nor stored (there is no payload to fill in). It is still
  legal as an operand of `==`/`!=` (tag identity, as it always was — equality
  never looks at a payload or a message) and as the head of a pattern.
- **Payloads are read by patterns, and only by patterns** — no `err.path`. In a
  `compare`, `matches E::V(p1, p2):` matches an error value whose tag is `E::V`
  and whose fields match `p1`, `p2` **positionally**. A sub-pattern is what
  §6.17 allows inside a struct pattern (`_`, a name that binds, a literal, an
  enum/machine variant, a nested struct pattern). Like a struct pattern, it
  lists every field or ends in `..` (`E::V(path, ..)`, `E::V(..)`). A bare
  `E::V:` header matches a variant that has no fields; for one that has, the
  header is `E::V(..)` (error E092 with that hint), so a pattern cannot keep
  compiling, meaning something different, after a field is added. The message
  is not bindable in a pattern. A variant pattern's subject is an error value
  (`AnyErrorType`, or a declared error type); anything else is E092. Bindings,
  guards, stacking, source order and single evaluation are §6.17's, unchanged.
- **Exhaustiveness.** A variant pattern never makes an arm a catch-all. An
  open error union (`AnyErrorType`) still always needs `otherwise` or an
  unguarded `_` (E093). A subject of a *declared* error type must have every
  one of its variants covered by an unguarded variant pattern whose fields are
  all irrefutable, or end in `otherwise`/`_` (E093).
- **Propagation is the prefix keyword `try`** (DEC-11; `?` was rejected — this
  language spells control flow with words). `try <expr>` has the precedence of
  a prefix operator, so `try a() + try b()` is `(try a()) + (try b())`. The
  operand must be a `!U`; the result is `U`. On failure `try` returns the error
  value **unchanged** from the enclosing function — it *is* a `return`: pending
  `defer`s run exactly as for one. The enclosing function must return `!V` (any
  `V`: the error union is open, so no conversion exists or is needed). `try` is
  an error (E095) outside a function, in a function whose return type is not
  `!V` (`main` returns `int`, so `main` included — the hint says how to handle
  the error there), inside a `defer` (INV-DEFER-2: a deferred statement may not
  return), and on an operand that is not fallible. `try` is a reserved word.
- **Pass-through return.** `return f();` where `f(): !T` and the enclosing
  function returns exactly `!T` is legal and returns the fallible value
  unchanged. It never unwraps, and nothing converts a `!T` into a plain `T`
  (§5.2's rule stays).
- **Runtime representation.** An error value is `%TurfErrorValue = { i8* tag,
  i8* message, i8* payload }`. `payload` is null for a variant with no fields;
  otherwise it points to a GC-allocated per-variant struct with its own
  `TurfTypeDesc`, so the collector traces a `string` or a `List<T>` held in a
  payload exactly as it traces the message. `%FallibleValue` is unchanged, and
  tag equality stays `strcmp` on tag strings.
- **Excluded from 2.0:** closed error sets in signatures (`!IOError int`),
  error-type conversion, stack traces (FUTURE F-15), payloads on plain `enum`s
  (F-14b), binding the message in a pattern, and `try` as a statement-position
  shorthand with a custom handler.
- **Diagnostic contract (owner emphasis on hints, as §6.15).** Typed, coded
  errors with a hint naming the concrete fix: E095 misplaced or ill-typed
  `try`, E096 an error-variant payload declared or constructed wrongly; variant
  patterns reuse E092/E093. No new `SLM::ErrorCategory` and no `docs/slm/`
  files (§6.9).

### 6.19 Generic operators — FROZEN (LEC-006, 2026-10-01)

Recorded on the project owner's instruction to implement LEC-006 as
`ROADMAP_v2.0.0.md` specifies it. Extends §6.7 (operators); nothing there
changes. Full account: `docs/development/changes/LEC-006.md`.

- **Declaration.** `operator<T> [](l: List<T>, i: int): T`, `operator<K, V>
  []=(m: &Map<K, V>, key: K, value: V): void` — a `<...>` type-parameter list
  immediately after the keyword, where `fn<T>` puts it, valid on every
  overloadable operator. After the keyword `<` followed by an identifier opens
  a parameter list; `<` followed by `(` is the less-than operator
  (`operator<T> <(a: C<T>, b: C<T>)` declares `<` generically).
- **Resolution is still exact-match (F-OP-1).** Candidate matching infers the
  parameters by unifying each declared operand type against the concrete
  operand (`TypeWalk::unify`), then **substitutes and requires the same
  `sameTypeExact`** a concrete candidate gets, per operand. Inference never
  widens an operand: unify skips a structural mismatch silently, so the
  substituted re-check is what rejects `Cell<int> == Cell<string>`. Every type
  parameter must be bound by an operand. The one §6.7 relaxation — the
  `operator[]`/`operator[]=` self operand may be declared `&T` — applies to a
  generic candidate too.
- **Instantiation.** A generic operator is a template like `fn<T>`: one clone
  per distinct type-argument tuple, requested at the operator's node and
  pointed at it by the monomorphizer. Request identity includes the exact
  declaration, so overloads sharing a name (`operator-` unary and binary, two
  `operator+`s, and ordinary `fn<T> f(x)` / `fn<T> f(x, y)`, `Box.put(x)` /
  `Box.put(x, y)`, `Box::new(v)` / `Box::new(v, w)`) are separate
  instantiations. Never instantiated for a still-open type parameter.
- **Declaration rules (E097).** (1) Every type parameter appears in some
  operand — they cannot be written at a use. (2) The orphan rule (INV-OP-2) is
  **not vacuous** for a generic operator: at least one operand, looked for
  through `&`/`*` and a generic type's base, must be a struct/enum declared in
  the operator's own module, because a bare `T` operand matches every type.
  An operand owned by another module is the existing E080.
- **An index result is a value, not a place (E098).** An `operator[]` read is a
  call returning the element by value, so it has no address: `l[i].field` and
  `l[i].method()` read it as an r-value. A write whose destination is a field of
  that copy, or `self` of a method that assigns to it, would be silently lost,
  so it is rejected: `bs[0].a = v`, `bs[0].a += v`, `bs[0].a++`,
  `bs[0].bump()` (a method that assigns to `self`, directly or through a chain
  of `self.m()` calls), `outer[0].push(v)`, and a `[]=`
  whose container is declared `&T` applied to another `[]`'s result
  (`mm["a"]["b"] = v`). It stops at a pointer or reference: an element that is
  `&T`/`T*` is written through. `grid[i][j] = v` for a `List` is allowed — its
  `[]=` takes the header by value and stores through the shared buffer, which
  writes no field of the copy. The same rule C# calls CS1612 and Go applies to a
  struct in a map. Index operators that yield a place (Rust's `IndexMut`) are out
  of scope: they conflict with F-MEM-5 and need their own design.
- **Excluded:** explicit type arguments at a use, generic arithmetic on types
  with no owned operand, and compound assignment through an indexing operator
  (§6.7, unchanged).
- **Diagnostic contract.** E097 `GenericOperatorError` (a declaration) and E098
  `OperatorTemporaryWriteError` (a use), typed and coded, each with a hint naming
  the fix. No new `SLM::ErrorCategory`, no
  `docs/slm/` file (§6.9).

### 6.20 The standard library's 2.x surface — FROZEN (LEC-007, 2026-10-01)

Recorded on the project owner's instruction to implement LEC-007 as
`ROADMAP_v2.0.0.md` specifies it ("frozen as the 2.x compatibility surface"),
with the owner's direction on what counts as public (below). Full account and
the per-module review: `docs/development/changes/LEC-007.md`. User-facing
statement: `docs/reference/stdlib/README.md`.

- **The public items of `lib/std/**` are the 2.x surface (F-API-1).** A `pub`
  function, method, type, error, constant or field is not removed, renamed, or
  given a different signature or failure shape before 3.0; additions are
  allowed in a minor release. Enforced by `tests/46_stdlib_surface/` (a use of
  every public item, compiled and run) and `scripts/stdlib_surface_check.sh`
  (fails on a public item no test mentions).
- **Naming (F-API-2).** Public functions, methods and fields are `snake_case`;
  types `PascalCase`; constants `UPPER_CASE`. The twelve camelCase names that
  existed were renamed without aliases (a kept alias would freeze two
  spellings). Checked by the same script.
- **Failure shape (F-API-3).** A function that can fail because of its input or
  environment returns `!T` with a variant carrying what failed; one that fails
  only because the caller broke a precondition panics, and the message starts
  with the caller's `file:line:` (`@caller_location`); a question
  (`exists`, `contains`, `index_of`) returns `bool` or a sentinel. Where a call
  has both a panicking and a recoverable form they are a pair (`Map.get` /
  `Map.try_get`) with a predicate to ask first (`contains`).
- **What is public (F-API-4), the owner's rule.** A function that does a job a
  real program might want is public whether or not the repository itself calls
  it; hiding it because "we" do not use it is not a reason. A struct's
  internals are public only if a real program does something with them
  (`Request.method`: yes; `List`'s buffer, `Map`'s hash index, `Server`'s route
  table: no). Raw C bindings (`fopen`, `getenv`, `socket`, `strstr`, ...) stay
  private: the library wraps them in checked Turf functions, and those are the
  interface. A pre-existing `pub extern` (`mem::malloc`, `time::usleep`,
  `sys::exit`) is part of the surface as it stands; no new one is added by
  this record.
- **Resource handles (F-MEM-2 applied).** Every handle type has an explicit
  `close()`, idempotent, and the documented idiom is `defer x.close();`.
  `raw_fd()` exposes the descriptor; there is no public `fd` field.
- **`string` stays a C pointer (F-STR-1, F-STD-3).** `strings::StringBuilder`
  is the incremental-construction answer that decision relied on; its buffer is
  private and `to_string()` returns a copy.
- **Not part of the surface:** the behaviour of `json::parse` for `\uXXXX`
  escapes (a documented gap), the iteration order of `Map.keys()` after a
  `remove()`, and `mem::malloc_usable_size`/`usable_size`, which are glibc-only
  and may be unavailable on a platform the release adds.

---

## Appendix — frozen-decision IDs

A change record's *Frozen decisions relied on* row cites these IDs
(`docs/development/TEMPLATE.md`). They name the decisions above; the
rationale for each lives in the `ARCHITECTURE_OVERHAUL.md` section named in
the third column. This table is a copy of that document's §4 and adds
nothing to the freeze -- except rows for decisions frozen after the
Overhaul (F-TYPE-1), whose third column points at their section above.

|---|---|---|
| **F-GEN-1** | Generics are **monomorphized**. Type erasure is removed entirely. | §9 |
| **F-GEN-2** | No `TurfGenericParamType` may reach Codegen. Enforced by assertion. | §9 |
| **F-GEN-3** | No trait/constraint system. Requirements are reported at instantiation sites. | §9, §25 |
| **F-NUM-1** | Integer literals are **untyped and arbitrary-precision** until a destination type is known. | §10 |
| **F-NUM-2** | Assigning a literal not representable in its destination type is a **hard error**, never a clamp. | §10 |
| **F-NUM-3** | Signed integer overflow **traps at runtime**; it is never wrapping and never UB. | §10 |
| **F-NUM-4** | Floating-point overflow at runtime produces IEEE `inf` and does **not** trap; a non-representable float *literal* is a hard error. | §10 |
| **F-NUM-5** | No new integer widths, no unsigned types. `int` (64) and `int32` (FFI) only. | §10, §25 |
| **F-NUM-6** | Language semantics never vary with optimization level. | §10, §17 |
| **F-GUESS-1** | `guess` is **Model A**: initializer-only inference, type permanently fixed at the declaration. | §11 |
| **F-GUESS-2** | Model B (declare-then-infer-from-first-assignment) is **rejected and must not be added**. | §11 |
| **F-GUESS-3** | Inference is **local and one-directional**. Never for parameters, return types, or struct fields. | §11 |
| **F-INIT-1** | **One** initialization mechanism: definite assignment, for every type. The eager auto-default (Mechanism B) is removed. | §12 |
| **F-INIT-2** | Storage is still always zero-filled. Definite assignment is a static net over already-zeroed memory. | §12 |
| **F-INIT-3** | A struct may not contain itself by value, transitively. Checked in the Resolver. | §12 |
| **F-MEM-1** | The memory model is a **non-moving, mark-sweep tracing collector**, precise on the heap, conservative on stack and registers. | §14 |
| **F-MEM-2** | No finalizers, ever. Deterministic resource cleanup is `defer`. | §14 |
| **F-MEM-3** | `mem::alloc`/`malloc` memory stays **manual and untraced**. The two heaps never merge. | §14 |
| **F-MEM-4** | A `libturfrt` runtime library exists and is linked into every binary. | §14, §15 |
| **F-PTR-1** | `&T` non-null, `&?T` nullable-checked, `T*` raw-and-gated. Raw pointers are FFI-only. | §14, §13 |
| **F-OP-1** | Operator overload resolution stays **exact-match** (`sameTypeExact`). No widening, ever. | §13 |
| **F-OP-2** | Compound assignment desugars; there is no `operator+=`. | §13 |
| **F-OP-3** | Operator overloads live in the ordinary name-keyed overload map under synthesized names. No separate table. | §13 |
| **F-OP-4** | `&&`, `\|\|`, `=`, `()`, `.`, and conversion operators are **not** overloadable. | §13, §25 |
| **F-OP-5** | `operator` declarations may be generic (`operator<T> [](l: List<T>, i: int): T`). Parameters are inferred from the operands, then every substituted operand must match **exactly** (F-OP-1 holds); each type parameter must appear in an operand, and at least one operand must be a type the declaring module owns (E097). Instantiated like `fn<T>`, one clone per type-argument tuple and per declaration. | §6.19 above (`ROADMAP_v2.0.0.md` LEC-006) |
| **F-STD-1** | Standard-library privileges are **declared in source**, never inferred from a file path. | §15 |
| **F-STD-2** | The intrinsic/builtin list is **closed and enumerated** in one place. | §15 |
| **F-STD-3** | `string` remains a NUL-terminated byte pointer. | §15, §25 |
| **F-DIAG-1** | Sandbox verification of SLM suggestions is **never removed**. | §16 |
| **F-DIAG-2** | An unvalidated suggestion can never clear the display gate. | §16 |
| **F-DIAG-3** | No new `SLM::ErrorCategory` values without an explicit request. | §16 |
| **F-OPT-1** | Optimization levels change speed/size/debuggability only — never observable behaviour. | §17 |
| **F-OPT-2** | Bounds/overflow/div-by-zero checks may be *removed by proof*, never by flag. | §17 |
| **F-TRACK-1** | Every implementation session updates a change record under `docs/development/`. | §18 |
| **F-TYPE-1** | A named type's identity is (owning module, name); the main file's module is `main`, never spelled. Bare-spelling ambiguity is an error, not "local wins". | §6.11 above (post-Overhaul: `ROADMAP_v2.0.0.md` FDN-003, not `ARCHITECTURE_OVERHAUL.md` §4) |
| **F-HEAP-1** | `heap(expr): &T` is the closed-list intrinsic for safe heap allocation — no `new T` keyword, no grammar change. Always allocates with `T`'s real `TurfTypeDesc`, never `NULL`. | §6.12 above (post-Overhaul: `ROADMAP_v2.0.0.md` FDN-001) |
| **F-MEM-5** | A `Stack`-provenance reference (points into the current frame) can never flow into a `return`, a field of a `Param`/`Heap`-owned object, a collection mutator/`operator[]=`, or `heap()`'s own operand — enforced by `EscapePass`, intraprocedurally, with no lifetime syntax. | §6.13 above (post-Overhaul: `ROADMAP_v2.0.0.md` FDN-002) |
| **F-STR-1** | `s[i]` on a `string` is `char`, bounds-checked against `strlen(s)` on read (`i < 0` or `i >= strlen(s)` panics); a write checks `i < 0` only, by design — no upper bound, since a `string` under construction has no recoverable capacity to check against. | §6.14 above (post-Overhaul: `ROADMAP_v2.0.0.md` FDN-004) |
| **F-VIS-1** | A top-level declaration and a struct field are private to their module unless prefixed `pub`/`public` (synonyms, one token); enum/error variants and machine states follow their type; operators are visible wherever their operand types are; `export immut` is its own `pub`; a `pub` signature may not mention a private type; UFCS and monomorphized clones never bypass or leak visibility; every visibility diagnostic carries a fix-naming hint. | §6.15 above (post-Overhaul: `ROADMAP_v2.0.0.md` LEC-002) |
| **F-PKG-1** | Two independently versioned programs: `turf` (Go, `cli/`) is the user's tool and drives; `turfc` compiles and is the tooling interface (users are never asked to type it). They agree on a `major.minor` driver interface reported by `turfc --version` (same major, minor at least what `turf` needs), not on release versions. The compiler owns everything that depends on TurfLang source (import resolution, visibility, capabilities) and never touches the network or the manifest; the CLI owns files, network and reproducibility, never parses TurfLang, and never rewrites a diagnostic; they meet at an explicit package graph. `turf <non-subcommand args>` is `turfc <args>` unchanged. | §6.16 above (owner decisions 2026-09-30: overturning `ROADMAP_v2.0.0.md` LEC-001 entry 1 / DEC-8, then replacing "same version" with the driver interface) |
| **F-PKG-2** | A module is (package, path under `src/`). `import foo;` resolves to exactly one of {own module, **direct** dependency's root module, stdlib module}; more than one is a hard error naming every candidate, never silent shadowing, also in single-file mode. Internal modules are package-private. The manifest rejects unknown keys. | §6.16 above (`ROADMAP_v2.0.0.md` LEC-001 entries 2–4, DEC-9) |
| **F-PKG-3** | Dependencies are `path` or `git` (`tag`/`rev`/`branch`) only; the root's `turf.lock` pins commit + SHA-256, verified on every build; one version per package name; `--offline`, `--locked`. | §6.16 above (`ROADMAP_v2.0.0.md` LEC-001 entry 5) |
| **F-PKG-4** | A dependency's `unsafe module;` is honored only if the root manifest's `[unsafe] allow` names it (not transitive); the root package's own modules need no listing; the compiler enforces, the CLI only passes the bit. | §6.16 above (`ROADMAP_v2.0.0.md` LEC-001 entry 6) |
| **F-PAT-1** | A top-level `matches <expr>:` keeps its meaning (equality; a bare identifier is a variable); destructuring forms (`_`, `Name { field: pattern, .. }`, guards `if`) are new syntax where a bare identifier BINDS. Arms are tried in source order, first matching header wins, the subject is evaluated once. `_` alone is reserved; `_name` is an identifier. A struct pattern lists every field or ends in `..`, and reads fields as `.field` does (visibility included). Bindings are immutable and arm-scoped. A guarded arm never counts toward exhaustiveness; enum/machine/`bool` subjects and open error unions are checked. | §6.17 above (`ROADMAP_v2.0.0.md` LEC-003, DEC-10) |
| **F-ERR-1** | An error variant may declare typed fields (`NotFound(path: string)`), constructed positionally with N arguments (or N+1 with a trailing `string` message) and read back only by patterns (`matches E::V(path, ..):`), never as a field. A bare `E::V` with fields is not a value, only an `==`/`!=` operand or a pattern head. A variant pattern never makes an arm a catch-all; an open error union still needs `otherwise`/`_`. `try <expr>` (prefix keyword, reserved) returns the error unchanged from the enclosing `!V` function, running pending `defer`s, and is an error in `main`, a non-fallible function, a `defer` or on a non-fallible operand. `return f();` of a `!T` from a `!T` function passes the value through. An error value is `{ tag, message, payload }`, the payload a GC-allocated per-variant struct with its own descriptor. | §6.18 above (`ROADMAP_v2.0.0.md` LEC-004, DEC-11, DEC-12) |
| **F-API-1** | The public items of `lib/std/**` are the 2.x compatibility surface: not removed, renamed, or re-signatured before 3.0; additions allowed in a minor. Locked by `tests/46_stdlib_surface/` + `scripts/stdlib_surface_check.sh`. | §6.20 above (`ROADMAP_v2.0.0.md` LEC-007) |
| **F-API-2** | Public stdlib functions, methods and fields are `snake_case`; no aliases are kept for a renamed name. | §6.20 above |
| **F-API-3** | Stdlib failure shapes: input/environment failure → `!T` carrying what failed; violated precondition → panic naming the caller's `file:line:`; a question → `bool` or sentinel. A panicking lookup has a predicate to ask first and, where useful, a `try_` twin. | §6.20 above |
| **F-API-4** | A function that does a job a real program might want is public whether or not the repository uses it; a struct's internals are public only if a real program does something with them; raw C bindings stay private behind checked Turf wrappers. | §6.20 above (owner's rule, 2026-10-01) |

---

## Changing a frozen decision

1. A session that believes one of these is wrong **stops**. It does not
   implement a different interpretation and explain afterwards.
2. It writes the argument into `docs/development/changes/<ID>.md` under
   *Unresolved questions*, and asks.
3. Adding a **new** frozen decision requires an explicit request. Sessions do
   not freeze things on their own initiative (§6.0 rule 3) — an ordinary
   engineering judgement goes in the change record, not here.
4. A decision moving between "rejected" (`ARCHITECTURE_OVERHAUL.md` §25) and
   "deferred" (§27) is itself a decision, and gets a change record.
