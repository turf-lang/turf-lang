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
