# Overhaul Remaining Work — implementation handoff (TEMPORARY)

> **Status: temporary handoff document.** Written 2026-09-26 against
> `refactor/architecture` @ `cde1d30`, by the final closure audit
> (`docs/development/OVERHAUL_CLOSURE_AUDIT.md` — read its §3 findings table
> first; this file is the *what to do*, that file is the *evidence*).
>
> It exists so an implementation session (primarily Sonnet) can close the
> Architecture Overhaul without re-deriving the architecture. **Delete this
> file** (and remove it from `scripts/session_preflight.sh`'s candidate list)
> in the commit that lands the last item, once every item's change record is
> in `docs/development/changes/` and the closure gate (§4 below) has passed.
> Nothing here is a permanent design record — the change records are.

---

## 0. How to use this file

1. Every item is an ordinary feature group for the purposes of
   `docs/development/SESSION_PROTOCOL.md`. Run
   `scripts/session_preflight.sh OVH-00N` — it finds the section below.
2. Read `AGENTS.md`, then `docs/development/FREEZE.md`, then the item, then
   the architecture docs the item names. Same order as every overhaul group.
3. Each item ends with **Non-goals**. Those are this file's "Must NOT
   reinterpret" lists. Do not widen an item's scope; if an item turns out to
   need a decision that is not written here, stop and ask (FREEZE.md §6.0 rule).
4. One change record per item: `docs/development/changes/<ID>.md` (e.g. OVH-003), from
   `docs/development/TEMPLATE.md`, plus an `INDEX.md` row. Items marked
   **DECISION** must not start until the user has answered the question they
   name — record the answer in the change record.
5. Commit format, test conventions, and the `_module.tr` trap are `AGENTS.md`'s.
   No `Co-Authored-By` trailer.

### Item index (dependency order)

| ID | Title | Cat. | Severity | Size | Depends on |
|---|---|---|---|---|---|
| OVH-001 | GC type-descriptor bitmap silently truncated at 64 words | A | **critical** (use-after-free) | S–M | — |
| OVH-002 | GC pointers hidden in untraced manual-heap memory | B | **critical** (use-after-free) | M | OVH-001 |
| OVH-003 | Raw-pointer indexing is ungated in safe code; no safe generic element access | A | **critical** (memory safety, F-PTR-1) | M | — |
| OVH-004 | By-value cycle through a generic instantiation segfaults the compiler | A | high (crash) | S–M | — |
| OVH-005 | `int32`: `++`/`--` ICE; untyped literal rejected; wrong-reason test | A | high (ICE) | S | — |
| OVH-006 | Generic type-parameter names captured by user type names | A | high (wrong rejection) | M | — |
| OVH-007 | Chained field access through a nested generic struct field fails | A | medium | S–M | OVH-006 (same area) |
| OVH-008 | `&T` → `&?T` widening missing in generic candidate matching | A | medium | S | — |
| OVH-009 | `>>` closing nested generic arguments is a parse error | A | medium | S | — |
| OVH-010 | `mem::root` / `mem::unroot` never built | A | medium | S–M | — |
| OVH-011 | Direct compile of `lib/std/collections/list.tr` fails (E034) | A | medium | S–M | — |
| OVH-012 | Lint false positives + stdlib warnings on every import | B | medium (first-run experience) | S | — |
| OVH-013 | Release workflow ships platforms the runtime cannot support | B, **DECISION** | high (release) | S (or L) | — |
| OVH-014 | FREEZE conformance: disclosed narrowings | C, **DECISION** | low | XS–S | — |
| OVH-015 | Documentation and bookkeeping drift | C | medium | M | all of the above |

Suggested session grouping (each line one Sonnet session, in order):
**[001 → 002]**, **[003]**, **[004 + 009]**, **[005]**, **[006 → 007 + 008]**,
**[010 + 011]**, **[012]**, **[013 + 014]** (after the user's decisions),
**[015]** last.

---

## 1. Background every item assumes

- **Two heaps, one rule (F-MEM-1/F-MEM-3).** The collector traces the GC heap
  precisely via `TurfTypeDesc` and scans stack + callee-saved registers
  conservatively. It **never** looks inside `malloc`/`mem::alloc` memory. So
  *any* GC pointer (a `string` produced at runtime, a collection's backing
  buffer) whose only reference lives in manual-heap memory is garbage to the
  collector. `docs/architecture/memory_model.md` Part 1 §1.7 and
  `docs/development/changes/MEM-002.md` *Known pitfalls* describe this.
- **Conservative stack scanning masks bugs at `-O2`.** A GC regression test
  must (a) build the GC object in a *deeper* frame than the one that later
  reads it, (b) churn the stack with a recursive function writing real data
  (see `tests/41_gc/01_retention_across_collection.tr`'s `churn()`), and (c)
  **run at `-O0` as well as the default** — several repros below pass at
  `-O2` only by register luck. `test_runner.sh -O0` is part of CI already.
- **Force a collection** with the bare intrinsic `gc_collect()`
  (`include/Intrinsics.h`). Allocate a few thousand throwaway strings between
  two `gc_collect()` calls so freed blocks are reused.

---

## 2. Items

### OVH-001 — GC type-descriptor bitmap silently truncated at 64 words

- **Category / severity:** A · **critical** — silent use-after-free in safe code.
- **Subsystem:** runtime (`libturfrt`), Codegen descriptor emission.
- **Prerequisite:** none.
- **Frozen decisions:** F-MEM-1 ("precise on the heap"), F-MEM-4, F-STD-2.
- **Invariants:** INV-RT-3 (descriptor built only from a laid-out struct);
  new **INV-MEM-2** (below).

**Current behaviour (verified live).** `TurfTypeDesc.pointer_word_bitmap` is a
single `uint64_t` (`runtime/turfrt.h:64`). `turf_rt_typedesc_is_pointer_word()`
returns 0 for any `word_index >= 64` (`runtime/turfrt_types.c:15`), and the mark
loop caps `max_word` at 64 (`runtime/turfrt_gc.c:~374`). RT-001 disclosed the
limit as "a future session must widen it first"; MEM-002 then shipped a
collector that depends on it. Any struct element of a GC-heap collection whose
pointer field lies at byte offset ≥ 512 is **never traced**.

```turf
// 70 int fields, then a string: `name` is word 70.
struct Big { f0: int, f1: int, /* ... */ f69: int, name: string }
// fill a List<Big> with runtime-built names, gc_collect(), churn, gc_collect()
// -> reading l.data[k].name: every string was freed and reused.
```
Full reproducer: generate it with the Python snippet in the audit
(`OVERHAUL_CLOSURE_AUDIT.md` §3, finding F-01). Observed: `test::assert`
failure, exit 134. Control with 60 int fields (61 words): passes.

**Required behaviour.** Every pointer-shaped word of every descriptor-described
type is traced, for any struct size. No size limit, no silent truncation.

**Why it is wrong.** F-MEM-1 promises precise heap tracing. A collector that
frees reachable objects is the worst possible GC failure mode.

**Strategy.**
1. Change the descriptor to a variable-length bitmap:
   `typedef struct { uint64_t size; const char *name; uint64_t word_count;
   const uint64_t *pointer_words; } TurfTypeDesc;` (bitmap as an array of
   `ceil(word_count/64)` words; `word_count = size/8`). Keep the field order
   documented in `turfrt.h`; this is a compiler↔runtime ABI change, lock-step
   by F-MEM-4, no compatibility shim.
2. `emitTurfTypeDesc()` (`src/CodegenStruct.cpp:~681`) emits a private
   constant `[N x i64]` global for the bitmap and points at it. It must still
   run only from `EmitAllTurfTypeDescs()` (INV-RT-3 — read RT-001's *Known
   pitfalls* first: moving emission inline crashes).
3. The per-element descriptor path used by `gc_realloc(...) to T*`
   (`CodegenStruct.cpp:~726-760`, MEM-002) must emit the same wide form.
4. `turf_rt_typedesc_is_pointer_word()` bounds-checks against `word_count`;
   the mark loop drops its `64` cap.
5. Keep a hard assertion in the emitter: `word_count * 8 >= size` and the
   array length matches, so a future truncation is loud.

**Files likely to change.** `runtime/turfrt.h`, `runtime/turfrt_types.c`,
`runtime/turfrt_gc.c`, `src/CodegenStruct.cpp`, `scripts/runtime_smoke.sh`
(its descriptor check greps the emitted IR shape — update it), possibly
`include/Intrinsics.h` doc text.

**Tests.**
- `tests/41_gc/05_large_struct_element_tracing.tr` — the reproducer above
  (≥ 70 words, pointer field last, plus a second pointer field in the first
  64 words so both halves are covered). Must pass at `-O0` and default.
- `tests/41_gc/06_struct_exactly_64_and_65_words.tr` — boundary: pointer at
  word 63 and word 64.
- `scripts/runtime_smoke.sh`: add a descriptor check for a type > 64 words
  (dump with `--emit-llvm`, assert the bitmap array length).
- Negative: none (this is a runtime property); the existing
  `tests/41_gc/02_precise_element_tracing.tr` must stay green.

**Stdlib / showcase impact.** None directly. **Docs:** `docs/architecture/runtime.md`
(descriptor layout), `memory_model.md` §2.6, `INVARIANTS.md` INV-RT-3 row (drop
the 64-word caveat), new **INV-MEM-2** "every pointer-shaped word of a
descriptor-described object is traced, at any size — enforced by the
variable-length bitmap and the emitter's length assertion".

**Completion criteria.** Both new tests pass at every `-O` level; the full
suite, `gc_smoke.sh`, `gc_rss_smoke.sh`, `runtime_smoke.sh` are green; `grep -n
"64" runtime/turfrt_gc.c runtime/turfrt_types.c` shows no word cap.

**Pitfalls.** The descriptor is referenced from every allocation site
(OPT-001 forward hazard: `GlobalDCE` must not drop it) — the new bitmap global
must be reachable from the descriptor, not a separate unreferenced global.

**Non-goals.** No moving/generational features, no descriptor for manual-heap
memory, no change to conservative stack scanning.

---

### OVH-002 — GC pointers hidden in untraced manual-heap memory

- **Category / severity:** B (regression introduced by MEM-002's collector over
  code that was correct while nothing was ever freed) · **critical**.
- **Subsystem:** stdlib (`json`), showcase, Codegen (error values, `!T` boxes).
- **Prerequisite:** OVH-001 (the Codegen fixes need wide descriptors for
  arbitrary boxed `T`).
- **Frozen decisions:** F-MEM-1, F-MEM-3 (heaps never merge — so the fix is
  to allocate these objects **on the GC heap**, never to trace manual memory).

**Current behaviour (verified live).** Four places store GC pointers inside
`malloc`/`mem::alloc` memory, which the collector never scans:

| # | Where | What it holds | Evidence |
|---|---|---|---|
| 1 | `lib/std/json/json.tr:149, 166` — `mem::alloc(sizeof(JsonValue))` boxes | `str_val: string`, `arr_val`/`obj_keys`/`obj_vals` list buffers | live: parse 40 objects, collect, read → `JsonValue.get: no such key 'name'`, exit 134 |
| 2 | `showcase/kv_store_server.tr:142` — `mem::alloc(16) to Entry*` | `key`/`value` strings | by inspection (same shape as #1) |
| 3 | `src/CodegenCore.cpp:~385` — `CodegenErrorVariantConstructWithMessage()` mallocs the `TurfErrorValue` | the runtime-built `message` string | live: `err.message` read after a collection inside `recover as err run` → wrong value, exit 134 at `-O0` (passes at `-O2` by register luck) |
| 4 | `src/CodegenCore.cpp:~299` — `BoxToI64Slot()` mallocs a box for a `!T` success value wider than 64 bits | any GC pointer inside `T` (e.g. `struct Rec { name: string }`) | by inspection; same mechanism as #3, window is any collection between `return` and `recover` |

Reproducer for #1 (abbreviated; full text in the audit, F-02):
```turf
mut v: json::JsonValue = build();   // json::parse of 40 {"name": "<runtime string>"} objects, in a deeper frame
churn(30); gc_collect(); /* 2000 junk strings */ gc_collect();
v.at(k).get_string("name")          // -> "JsonValue.get: no such key 'name'"
```

**Required behaviour.** No GC pointer is ever reachable *only* through manual
memory in compiler-emitted code, the standard library, or `showcase/`.

**Strategy.**
- **#1 json:** drop the boxing entirely. Post-TYPE-002, `List<JsonValue>`
  by value is legal (a `List<T>` holds `data: T*`, so `JsonValue` containing
  `List<JsonValue>` is not a by-value cycle — confirm with TYPE-001's check,
  it must compile). Change `arr_val`/`obj_vals` to `List<JsonValue>`, delete
  both `mem::alloc` sites and every `boxed[0]`/`.data[i]` dereference of a
  box. Update the header comment (lines ~24-40) that explains the old
  `List<T*>` workaround. Observable behaviour (value semantics of
  `at()`/`get()`) must not change — `tests/34_http_and_json/` is the guard.
- **#2 showcase:** same — `List<Entry>` by value; delete the `mem::alloc`.
  Keep the program's protocol/output byte-identical (`opt_levels_smoke.sh`
  diffs it).
- **#3/#4 Codegen:** replace both `getOrCreateMallocFn()` calls with
  `turf_rt_alloc(size, desc)`, passing a real descriptor: for
  `TurfErrorValue` a two-pointer-word descriptor (emit once, like the
  `%TurfErrorValue` type itself); for the `!T` box the descriptor of `T`
  (emit via the existing `EmitAllTurfTypeDescs()` path — do not emit inline,
  INV-RT-3). Primitive `T` never boxes, so only aggregates need this.
- Add a **mechanical guard**: extend `scripts/check_boundary.sh` with a check
  that `getOrCreateMallocFn()` has no remaining caller in `src/Codegen*.cpp`
  except ones explicitly allowlisted with a reason (there should be none).
  For `lib/std`, add a documented rule to `boundary.md`: *a `mem::alloc`
  result may never hold a GC pointer*; the grep check is best-effort
  (`mem::alloc(sizeof(` of a type with a `string`/collection field).

**Files likely to change.** `lib/std/json/json.tr`, `showcase/kv_store_server.tr`,
`src/CodegenCore.cpp`, `src/CodegenStruct.cpp` (descriptor emission hook),
`scripts/check_boundary.sh`, `docs/architecture/boundary.md`,
`docs/architecture/errors_and_recover.md` §2.8 (no longer "mallocs"),
`docs/architecture/memory_model.md`, `docs/reference/stdlib/json.md` (only if
it documents the boxing).

**Tests.** All must pass at `-O0` **and** default:
- `tests/41_gc/07_json_values_survive_collection.tr` — reproducer #1.
- `tests/41_gc/08_error_message_survives_collection.tr` — reproducer #3
  (build the error in a helper, collect inside the `recover as err run`
  block, compare `err.message` to a freshly rebuilt expected string).
- `tests/41_gc/09_boxed_fallible_success_survives_collection.tr` — #4: hold
  a `!Rec` (struct with a runtime-built `string`) in a local across a
  collection, then `recover`.
- Existing `tests/34_http_and_json/*` unchanged and green.

**Showcase impact.** `kv_store_server.tr` rewritten (#2); run the
`http_server`/`kv_store` client-server pairs manually as MEM-002 did.

**Change record.** Must list every compiler-emitted and stdlib allocation site
that can hold a GC pointer, classified — the table MEM-001 was supposed to
contain. `Removed assumptions`: "a `!T` box / error value is plain malloc
memory".

**Completion criteria.** Three new tests green at `-O0` and default; the
boundary check passes and fails on a deliberately re-added `malloc` call;
`grep -rn "mem::alloc" lib/std showcase` shows no allocation whose type holds
a `string` or collection.

**Pitfalls.** (a) `turf_rt_alloc` memory must never reach `mem::free`
(INV-MEM-1 panics — that is correct, it proves the classification).
(b) The error-value global constants for *bare* variants
(`BuildErrorTag()`) are static `.rodata` — leave them alone; only the
message-carrying construction allocates. (c) Do not "fix" #1 by tracing
manual memory — F-MEM-3.

**Non-goals.** No general typed `gc_alloc(T)` user intrinsic here (that is
roadmap FDN-001); no change to `mem::Pool<T>` (its manual heap is correct:
it never stores GC pointers by design — re-verify, and if it can, record it).

---

### OVH-003 — Raw-pointer indexing is ungated in safe code; no safe generic element access

- **Category / severity:** A · **critical** (out-of-bounds write from safe code).
- **Subsystem:** TypeChecker (unsafe gate), stdlib collections, tests, showcase.
- **Architecture docs:** `docs/architecture/pointers_and_references.md`, `docs/architecture/boundary.md`
- **Prerequisite:** none. (Coordinate with OVH-006 only if its fix changes
  `visitArrayAccess`.)
- **Frozen decisions:** **F-PTR-1** ("`T*` raw-and-gated; raw pointers are
  FFI-only"), FREEZE §6.6 table, F-OP-1 (operator exact match).
- **Invariants:** new **INV-CAP-2** (below).

**Current behaviour (verified live).**
```turf
import list::List;
fn main(): int {
    mut l: List<int> = List::default(); l.push(1);
    l.data[100000] = 42;        // compiles, no `unsafe`, no capability -> SIGSEGV
    return 0;
}
```
and `io::printline(stringList.data[1000]);` reads out of bounds (prints
`(null)`). Binding the raw pointer (`mut p = l.data;`) **is** rejected, and
field access through a raw pointer is gated (`TypeCheckerCall.cpp:~1621`), but
`TypeChecker::visitArrayAccess()` (`src/TypeCheckerCall.cpp:1969`) never calls
`checkUnsafeGate()` on a raw-pointer base. `l.data[i]` is simultaneously the
**documented** way to read a `List<T>` element for any `T` other than `int`,
because `operator[]` exists only for the concrete `List<int>` (operators
cannot be generic — `lib/std/collections/list.tr:84-100`).

Users of `.data[` today: 12 files under `tests/`, `showcase/kv_store_server.tr`,
`lib/std/{core/strings, io/io, http/http, json/json}.tr` (the last two declare
**no** raw-pointer capability), plus the collection modules themselves.

**Required behaviour.**
1. Indexing an expression of raw pointer type (`T*`, `ptr`, `void*`) is gated
   exactly like field access through one: legal inside `unsafe { }` or in a
   module holding the raw-pointer capability; a compile error otherwise.
2. Every collection with positional access gets **bounds-checked, generic**
   element accessors usable from safe code: `fn<T> List.get(i: int): T`,
   `fn<T> List.set(i: int, value: T): void` (panic through the existing
   `CodegenPanic`/`sys::panic` path with an `index out of bounds` message and
   `@caller_location`, matching `operator[]`'s existing message). Methods can
   be generic today; no parser change is needed.
3. All safe-code uses of `.data[i]` outside capability modules migrate to
   `get`/`set` (or `l[i]` where `T = int`).

**Strategy.** (a) Add the gate call in `visitArrayAccess()` for a base whose
type `isRawPointer()`, with the same `self`-exemption reasoning the field
access site documents (read its comment first). Strings are **not** raw
pointers (`string` indexing stays legal; its bounds are roadmap FDN-004).
(b) Add `get`/`set` to `List`; `Stack`/`Queue` gain `get` only if they expose
positional reads today (check; don't invent API). (c) Migrate callers.
(d) Leave `data` public — making it private is D13's job (roadmap LEC-002).

**Files likely to change.** `src/TypeCheckerCall.cpp`,
`lib/std/collections/list.tr` (+ stack/queue if applicable),
`lib/std/json/json.tr`, `lib/std/http/http.tr`, the 12 test files,
`showcase/kv_store_server.tr` (coordinate with OVH-002 — do OVH-002's rewrite
first or together), `docs/reference/stdlib/collections.md`.

**Tests.**
- `tests/05_arrays_pointers_ffi/fail_raw_pointer_index_outside_unsafe.tr` —
  the reproducer; must be **rejected at compile time**.
- `tests/05_arrays_pointers_ffi/raw_pointer_index_inside_unsafe_ok.tr` — the
  same write inside `unsafe { }` in a module with `unsafe module;` compiles
  (location rule: see LANG-002's pitfall about install-root testing).
- `tests/32_set_and_sort_stdlib/` (or `07_functions_modules/`):
  `list_get_set_generic.tr` over `int`, `string`, a 24-byte struct.
- `fail_list_get_out_of_bounds.tr` with `// RUNTIME_ABORT_EXPECTED`.

**Completion criteria.** The reproducer is a compile error; `grep -rn '\.data\['
tests showcase lib/std/json lib/std/http` returns nothing; the full suite,
stdlib sweep and showcase are green.

**Pitfalls.** `self.data[i]` inside collection methods is legal because those
modules hold the capability — confirm the exemption covers it before
migrating anything inside the collections. `ArrayAccessExprAST` also handles
`string` and real arrays — only the raw-pointer branch is gated.

**Non-goals.** No generic `operator` declarations (roadmap LEC-006); no field
privacy; no change to string indexing.

---

### OVH-004 — By-value cycle through a generic instantiation segfaults the compiler

- **Category / severity:** A · high — compiler crash (exit 139, no output) on
  plausible input; violates INV-LAYOUT-1 and §23 DoD item 1.
- **Subsystem:** Resolver (`checkStructLayoutAcyclicity*`), Monomorphizer.
- **Architecture docs:** `docs/architecture/compiler_pipeline.md`, `docs/architecture/memory_model.md`
- **Frozen decisions:** F-INIT-3; §9.5 step 3 ("run TYPE-001's acyclicity
  check on any resulting struct layout").

**Current behaviour (verified live).**
```turf
struct A<T> { val: T }
struct B { back: A<B> }
fn main(): int { mut x: A<B> = A::default(); return 0; }   // turf: exit 139, no output
```
TYPE-002's change record (*Known pitfalls*, last bullet) diagnosed exactly
this gap — `byValueEdgeTarget()` (`src/ResolverCore.cpp:846`) resolves a
`GenericType` field to its **base** name, so the per-clone DFS
(`checkStructLayoutAcyclicityForClone()`) walks into the *template*'s
`val: T` and never closes the `A<B> → B → A<B>` cycle — but classified it as
"a coverage gap, not a live correctness bug". It is a live segfault.

**Required behaviour.** A `StructLayoutCycleError` (E069) naming the cycle
`A<B> -> B -> A<B>` with field notes, exactly like the non-generic case.

**Strategy.** Give `byValueEdgeTarget()` a way to resolve a `GenericType`
field to the **specific registered clone** (mangled name) when one exists —
the monomorphizer already registers clones under their mangled names — and
run the per-clone check after the monomorphizer's worklist has registered
every clone reachable from the current one (so `B`'s field `A<B>` can be
followed to the clone). Keep one DFS core (the shared function at
`ResolverCore.cpp:~888`); do not write a second checker.

**Tests.** `tests/38_monomorphization/fail_generic_instantiation_by_value_cycle.tr`
(the reproducer, rejected with E069); a positive twin where `B` holds
`A<B>*`/`&?A<B>` (compiles and runs); a two-parameter variant
(`struct P<X, Y> { x: X, y: Y }`, `struct Q { p: P<int, Q> }`).

**Completion criteria.** Reproducer rejected with a readable cycle; no
compiler input from the new tests crashes; full suite green.

**Pitfalls.** The pre-pass runs the Resolver twice (TYPE-001 pitfall) — both
runs must agree. Watch recursion: the clone lookup must not trigger new
instantiations.

**Non-goals.** No depth-limited `GetDefaultValue()` workaround (§8 TYPE-001's
"must NOT").

---

### OVH-005 — `int32`: `++`/`--` ICE; untyped literal rejected; wrong-reason test

- **Category / severity:** A · high (ICE on plausible input) + frozen-spec
  deviation (F-NUM-1).
- **Subsystem:** TypeChecker, Codegen (`PostfixExprAST`).
- **Architecture docs:** `docs/architecture/compiler_pipeline.md`
- **Frozen decisions:** F-NUM-1, F-NUM-2, F-NUM-5, FREEZE §6.2 rows 3–4.

**Current behaviour (verified live).**
```turf
fn main(): int { mut c: int32 = 5 to int32; c++; return c to int; }
// Internal compiler error: generated LLVM IR is invalid.
//   call { i64, i1 } @llvm.sadd.with.overflow.i64(i32 %postfix.load, i64 1)
```
Same ICE for `++c`, `c--`. `c += 1` is instead a *type* error ("Cannot assign
value of type 'int' to variable of type 'int32'"), and `mut n: int32 = 5;` is
rejected ("Cannot initialise 'int32' variable 'n' with value of type 'int'").
`tests/36_numeric_semantics/fail_int32_literal_out_of_range.tr` therefore
passes for the **wrong reason** (E066 type mismatch, never the E076
representability check it claims to test).

**Required behaviour.**
1. An **untyped** integer literal whose destination is `int32` acquires type
   `int32` and is checked for representability (F-NUM-1/§6.2 row 3):
   `mut n: int32 = 5;` compiles; `mut n: int32 = 99999999999;` is **E076**.
   Typed `int → int32` stays an error (row 4) — only untyped literals change.
   Apply at every type-fixing site `checkLiteralRepresentable()` already
   covers (`TypeCheckerCore.cpp:321`): init, assignment, argument, return.
2. `++`/`--`/compound assignment on an `int32` lvalue: `int32` "is never itself
   an arithmetic result type" (`docs/reference/variables_and_types.md`
   §`int32`), so these are **compile errors** with a dedicated message and a
   hint showing `c = (c + 1) to int32;` — never an ICE. (Making them legal
   32-bit trapping operations would be a new semantic rule; that is a
   decision this item does not make.)

**Files likely to change.** `src/TypeCheckerCore.cpp`,
`src/TypeCheckerExpr.cpp` (`visitPostfixExpr`, `visitCompoundAssignment`),
`src/CodegenVar.cpp` (`PostfixExprAST::codegen()`, line 565 — add an
assertion that its operand is `int`, so the next gap is loud),
`include/ErrorCodes.h` + `docs/errors/codes/` if a new code is added (use an
existing generic SLM category — F-DIAG-3).

**Tests.** `tests/19_int32_type/int32_untyped_literal_init.tr` (init, assign,
argument, return); rewrite `fail_int32_literal_out_of_range.tr`'s comment and
assert it now fails with **E076** (check the code in the output — the runner
only checks "failed", so verify manually and say so in the record);
`fail_int32_increment.tr`, `fail_int32_compound_assign.tr` (clean error, no
ICE).

**Completion criteria.** No input in the tests produces an ICE; `turf
--explain` works for any new code; full suite green.

**Non-goals.** No int32 arithmetic result type, no int32 overflow traps, no
literal suffixes (F-NUM-5, §10.4).

---

### OVH-006 — Generic type-parameter names captured by user type names

- **Category / severity:** A · high — correct programs rejected; generics
  correctness (F-GEN-1).
- **Subsystem:** Resolver (type-parameter substitution), TypeChecker
  (`isOrWrapsGenericParam`), Parser (`RegisterType` of generic structs),
  Monomorphizer.
- **Architecture docs:** `docs/architecture/compiler_pipeline.md`, `docs/architecture/modules_and_scope_resolution.md`
- **Relation to `ARCHITECTURE_SIMPLIFICATION.md`:** this is that document's
  §7 phase **0★**, now with a user-visible reproducer. Its evidence section
  (§1, 2026-09-26 STD-003 entry) is required reading.

**Current behaviour (verified live).**
```turf
import list::List;
struct T { x: int }
fn main(): int { mut l: List<int> = List::default(); l.push(5); return l.pop() - 5; }
// error at line 106, col 3 [E066], in file lib/std/collections/list.tr
```
Likewise `struct T` + `fn<T> id(v: T): T` in the same file, and a user
`struct V` breaks `Map<K, V>` ("In this instantiation: Return type mismatch:
function declares 'int' but returns 'V'", `map.tr:310`). Root cause: a
generic struct's `GlobalSymbolTable`-registered `TurfStructType` keeps its
field types as stale `NamedType("T")` (the Resolver rewrites only the AST
node), and `isOrWrapsGenericParam()` (`src/TypeCheckerDecl.cpp:68`) decides
"is this a type parameter?" by *"a NamedType whose name is not a registered
type"* — which becomes false the moment the user registers a type with that
name. The same stale copy is why `fn<T> Pool.free_all()` (`free(self.items[i])`)
fails (STD-003 Fix 3, *What this fix does NOT reach*).

**Required behaviour.** A type parameter is identified by **declaration
scope**, never by the absence of a same-named global type. User type names
never interfere with any generic declaration in any module. `self.<field>`
of a generic struct inside its own generic method has type
`TurfGenericParamType` wherever it is observed.

**Strategy.**
1. Make the registered `TurfStructType` for a generic struct carry
   `TurfGenericParamType` fields: apply the same substitution the Resolver
   applies to the AST node (`Resolver::substituteTypeParams`, `ResolverCore.cpp`)
   to the registered type at the same moment, or register the struct type
   after the rewrite. One representation, not two.
2. Delete the name-lookup heuristic in `isOrWrapsGenericParam()` once (1)
   makes it unnecessary; if a `NamedType` still reaches it, that is a bug to
   fix at the source, not to classify by name. Keep SEM-003's bounded
   recursion (its pitfall: do not recurse into nested struct fields).
3. While there, route `typesCompatible()` Rule 1 and `visitCast()`'s STD-003
   bypass through one shared predicate (Simplification §7 0★'s suggestion) —
   only if it falls out naturally; the full "one TypeWalker" unification is
   roadmap SMP-002.

**Tests.** `tests/38_monomorphization/user_type_named_like_type_param.tr`:
user structs named `T`, `K`, `V`, `U` in one program that uses `List<int>`,
`Map<string, int>`, `Set<int>`, `Stack`, `Queue`, a generic free function and
a generic method — all compile and run. `pool_free_all_generic.tr`: the
`Pool<T>.free_all()` shape from STD-003 compiles (add the method to `mem.tr`
only if the user wants it; otherwise keep the test self-contained).

**Completion criteria.** The reproducers compile and run; SEM-003's
`fail_guess_uninstantiated_generic.tr` still fails for the right reason;
full suite green at `-O0` and default.

**Pitfalls.** SEM-003's `containsUninstantiatedGenericParam()` depends on the
current heuristic to reject `mut l = List::default();` — re-verify that
diagnostic after the change. The pre-pass registers types from a throwaway
AST (D19) — make sure the registered type the main pass sees is also fixed.

**Non-goals.** Not the full SemanticModel / "remove GlobalSymbolTable"
refactor (Simplification phases 2–3).

---

### OVH-007 — Chained field access through a nested generic struct field fails

- **Category / severity:** A · medium (TYPE-002 testing scope names nested
  generics).
- **Subsystem:** Codegen `FieldAccessExprAST` / `getExprType()` (likely), or
  TypeChecker field-type resolution.
- **Architecture docs:** `docs/architecture/compiler_pipeline.md`, `docs/architecture/memory_model.md`

**Current behaviour (verified live).**
```turf
struct Pair<A, B> { a: A, b: B }
struct Box<T> { inner: Pair<T, T> }
fn main(): int { mut b: Box<int> = Box::default(); b.inner.a = 3; return b.inner.a - 3; }
// error at line 3: Cannot access field on non-struct type   (src/CodegenStruct.cpp:1024)
```
Going through a local works: `mut p: Pair<int, int> = b.inner; p.a = 3;` compiles
and runs. So the failure is in resolving the type of `b.inner` when it is
used as the *base* of another field access (lvalue or rvalue).

**Required behaviour.** Arbitrary chains of field access through generic
struct fields work for reads and writes.

**Strategy.** Reproduce with `--dump-typechecked` to see whether the
TypeChecker already assigns `Pair<int,int>` to `b.inner`; if it does, the bug
is Codegen's independent `getExprType()`/`codegenLValue()` path resolving the
outer field's `GenericType` to the base-name template (INV-TYPE-2 says the
monomorphized AST must carry concrete types — check whether the field type
inside `Box<int>`'s clone was substituted). Fix at the substitution, not by a
special case in Codegen. Likely touches the same code as OVH-006; do OVH-006
first.

**Tests.** `tests/38_monomorphization/nested_generic_field_chain.tr`: read,
write, compound-assign (`b.inner.a += 1`), three levels deep, and through a
method's `self` (`self.inner.a`).

**Non-goals.** No change to value/aliasing semantics of struct fields.

---

### OVH-008 — `&T` → `&?T` widening missing in generic candidate matching

- **Category / severity:** A · medium (LANG-002 × TYPE-002 interaction).
- **Subsystem:** `typesCompatible()` / `collectGenericSubstitution()`.
- **Architecture docs:** `docs/architecture/pointers_and_references.md`, `docs/architecture/compiler_pipeline.md`
- **Frozen decisions:** FREEZE §6.6 ("`&T` → `&?T` → `T*` widens implicitly").

**Current behaviour (verified live).**
```turf
struct Node<T> { val: T, next: &?Node<T> }
fn<T> len(n: &?Node<T>): int { /* walk */ }
fn main(): int { mut a: Node<int> = Node::default(); return len(&a); }
// No overload of 'len' matches argument types (&Node<int>).
//   candidate: len(&?Node<T> n) -> int
```
The non-generic equivalent (`fn len(n: &?Node)`, `len(&a)`) compiles.

**Required behaviour.** The implicit `&T → &?T` widening applies when the
pointee is (or contains) a generic parameter, and substitution infers
`T = int` through it.

**Strategy.** Find the branch in `typesCompatible()` (`src/Types.cpp`) and in
`collectGenericSubstitution()` (`src/TypeCheckerCall.cpp`) that matches
reference-to-generic shapes; add the nullable-reference target case mirroring
the non-generic rule. STD-003's Fix 2 is the precedent: the same case was
missing from several walkers at once — check all five it names.

**Tests.** `tests/39_nullable_references/generic_nullable_widening.tr`
(positive) and a negative: `&?Node<int>` → `&Node<T>` must still be rejected
(INV-NREF-2).

**Non-goals.** No `&?T → &T` implicit narrowing, ever.

---

### OVH-009 — `>>` closing nested generic arguments is a parse error

- **Category / severity:** A · medium (§8 TYPE-002 testing scope lists
  `List<List<int>>`; the existing test works around it with `List<List<Big> >`,
  `tests/38_monomorphization/16_nested_generic_struct_field.tr:11`).
- **Subsystem:** Parser (`ParseType`, `src/ParserCore.cpp:~408`).
- **Architecture docs:** `docs/architecture/compiler_pipeline.md`

**Current behaviour (verified live).** `mut ll: List<List<string>> = ...;` →
`Expected '>' or ',' in generic type` (plus a misleading cascade
"Statements are not allowed outside of a function"). The lexer's maximal
munch produces `TOK_RSHIFT` (`include/Lexer.h:378`).

**Required behaviour.** In type-argument context, `>>` closes two argument
lists and `>>=` closes two and continues as `=` — the standard C++11/Java
treatment. INV-LEX-1 (uniform maximal munch) stays true: the **lexer** does
not change; the parser splits the token when it is expecting `>`.

**Strategy.** In the generic-argument loop of `ParseType()`, when the current
token is `TOK_RSHIFT` (or `TOK_RSHIFT_ASSIGN`) and a `>` is expected, consume
one `>` logically by replacing `CurTok` with `'>'` (or `TOK_ASSIGN`/`>=`
respectively) without advancing. Document the one exception next to
INV-LEX-1 in `INVARIANTS.md`.

**Tests.** `tests/38_monomorphization/nested_generic_no_space.tr`:
`List<List<int>>`, `Map<string, List<int>>`, `List<List<List<int>>>` (three
closers: `>>` then `>`), and `mut m: Map<string, List<int>>= Map::default();`
(the `>>=` case). Keep the existing spaced test unchanged.

**Non-goals.** Expression-position generic arguments
(`List<int>::default()`) — that is roadmap LEC-006.

---

### OVH-010 — `mem::root` / `mem::unroot` never built

- **Category / severity:** A · medium — §14.3 and §14.6 specify the API as the
  answer to F-MEM-3's documented unsoundness, and §22 **S22** schedules it
  ("sweep, collection triggering, `mem::root`/`unroot`, …"). MEM-002's record
  states it was not built because "nothing in MEM-002's roadmap entry …
  calls for it" — S22 does.
- **Subsystem:** runtime, intrinsics, `lib/std/mem/mem.tr`.
- **Architecture docs:** `docs/architecture/memory_model.md`, `docs/architecture/runtime.md`
- **Frozen decisions:** F-MEM-3, F-STD-2 (adding an intrinsic is an explicit,
  change-recorded act — this item is that record).

**Current behaviour.** A GC pointer stored only in manual memory (a C
library's struct, a `mem::alloc` buffer) is collected. The only root API is
`turf_rt_add_global_root(void **slot)` (slot-based, no Turf-level surface).

**Required behaviour.** `mem::root(p: void*)` pins the GC object containing
`p` (interior pointers allowed, like stack roots) until a matching
`mem::unroot(p)`; pins nest (a count per object); `unroot` of an unpinned
object panics with a named message; `root` of a non-GC pointer is a no-op
(documented) or a panic — choose one and record it (recommendation: no-op,
because C memory is legitimately passed around).

**Strategy.** Runtime: a pinned-object table consulted as a precise root set in
the mark phase (`turfrt_gc.c`), `turf_rt_pin/unpin` in `turfrt.h`.
Intrinsics: `gc_pin`/`gc_unpin` in `include/Intrinsics.h` + `Builtins.cpp`
(same pattern as `gc_is_managed`), wrapped by `mem::root`/`mem::unroot`.
`scripts/check_boundary.sh` check 3 must stay green (document both names).

**Tests.** `tests/41_gc/10_mem_root_pins_manual_heap_reference.tr` — store a
runtime-built string's pointer only inside a `mem::alloc` buffer, `mem::root`
it, churn + collect, read it back; then `unroot`, collect, and assert via
`gc_is_managed` that it was reclaimed (or via stats, like `gc_smoke.sh`).
`fail_mem_unroot_unpinned.tr` (`// RUNTIME_ABORT_EXPECTED`).

**Docs.** `docs/reference/stdlib/mem.md`, `docs/architecture/memory_model.md`
§1.7, `runtime.md`.

**Non-goals.** No automatic rooting of manual memory; no finalizers.

---

### OVH-011 — Direct compile of `lib/std/collections/list.tr` fails (E034)

- **Category / severity:** A (§23 DoD item 26: "`lib/std` … compile clean
  directly") · medium.
- **Subsystem:** Parser (`ParsePreludeImports()`, `src/ParserModule.cpp:887`),
  module import de-duplication.
- **Architecture docs:** `docs/architecture/modules_and_scope_resolution.md`

**Current behaviour (verified live).** `turf --resolve lib/std/collections/list.tr`
→ `E034 A struct named 'List' is already declared`. The prelude module
`strings` does `import list::List`, so compiling `list.tr` as the entry file
imports itself back through the prelude. (`mem.tr`, previously affected, now
resolves clean.) `session_postflight.sh` special-cases "E034 and nothing
else" for these files, and three change records (ARCH-001, TYPE-001,
OPT-002) each re-discovered it. The existing self-import guard only covers a
prelude module *being* the entry file, not reaching it *transitively*.

**Required behaviour.** When any import (direct or transitive, prelude or
not) resolves to the entry file's own path, it binds to the entry module
instead of re-parsing and re-hoisting it. Direct `--resolve` of every
`lib/std` module reports nothing but "No entry point found".

**Strategy.** The import cache is keyed by `ResolvedPath`
(`modules_and_scope_resolution.md` §2.4); register the entry file's resolved
path in it before prelude injection, so every later import of the same path
is a cache hit. Then delete the E034 special case from
`scripts/session_postflight.sh` and the corresponding note in `AGENTS.md`.

**Tests.** Direct-compile coverage is script-level (the test runner cannot do
it): extend `session_postflight.sh`'s stdlib sweep to fail on *any* error.
Add `tests/22_prelude/entry_file_reimported_transitively_module.tr` +
companion to prove a user entry file imported back by one of its own imports
does not duplicate its declarations (mind the `_module.tr` naming rule for the
companion).

**Non-goals.** Circular imports as a feature (D16, FUTURE).

---

### OVH-012 — Lint false positives + stdlib warnings on every import

- **Category / severity:** B/C · medium — every `import io;` (i.e. hello
  world) prints a warning from `lib/std/io/io.tr`; `import json;` prints six.
- **Subsystem:** Resolver (`NeverReturnsErrorWarning`), Lint/Liveness,
  stdlib.
- **Architecture docs:** `docs/architecture/errors_and_recover.md`, `docs/architecture/compiler_pipeline.md`

**Current behaviour (verified live).**
1. `NeverReturnsErrorWarning` (`src/ResolverDecl.cpp:209`) fires on
   `io::read_lines` and `json::parse`, both of which **do** return errors —
   via `return` inside a `recover … run { }` block. The check does not look
   inside recover blocks. False positive.
2. `json.tr:511` `immut numStr` is reported both "declared but never used"
   **and** "dead store", yet it is read on the next line inside a `recover`
   operand. False positive in the unused-variable/liveness path (same
   blind spot: uses inside `RecoverExprAST`). Also: two warnings for one
   unused variable is a duplicate-diagnostic issue (seen on any unused
   `immut`).
3. `json.tr:541, 577` `return arr;`/`return obj;` after `while true` —
   genuinely unreachable (true positive, E074); the compiler accepts the
   functions without them (verified).
4. `http.tr:340` `Server.listen` — "contains a loop with no exit path" —
   intentional (an accept loop).

**Required behaviour.** Items 1–2 fixed in the compiler (with regression
tests); item 3 fixed in the stdlib; item 4: stdlib code must compile
warning-free, so either restructure `listen()` or suppress the class for it.
**Recommendation (needs a one-line user OK, record it):** also treat warnings
whose primary location is inside the install root's `lib/std` as
system-module warnings — not shown unless `--stdlib-warnings` is passed —
matching `-isystem` practice. Errors are never suppressed.

**Tests.** `tests/25_errors/never_returns_error_via_recover_block.tr` (no
warning; assert via a `// WARNINGS_EXPECTED: 0`-style check if the runner
supports one — if not, document the manual check in the record);
`tests/14_cfg_analysis/use_inside_recover_operand_is_a_read.tr`; hello world
with `import io; import json; import http;` produces **no** warning (script
check in `session_postflight.sh`'s stdlib sweep).

**Non-goals.** No new SLM categories (F-DIAG-3); no change to warning
*conditions* beyond the two false positives.

---

### OVH-013 — Release workflow ships platforms the runtime cannot support — **DECISION**

- **Category / severity:** B (RT-001/MEM-002 regressed the platforms v1.0.0
  shipped) · high for release.
- **Subsystem:** CI (`.github/workflows/release.yml`), runtime, README.
- **Architecture docs:** `docs/architecture/runtime.md`, `docs/architecture/memory_model.md`

**Current behaviour.** `release.yml` builds `linux-x86_64`, `linux-arch-x86_64`,
`macos-arm64`, `windows-x86_64` (MinGW). `runtime/turfrt_gc.c` has
`#error` unless `__x86_64__` (so **macOS arm64 fails to build**), captures
only the **System V** callee-saved set (Win64 also treats `rdi`/`rsi`/`xmm6-15`
as callee-saved — **Windows builds but its root scanning is unsound**), and
finds the stack base via `/proc/self/maps` (Linux only; falls back to a
call-depth-dependent guess elsewhere). README still advertises macOS and
Windows builds.

**Decision required (user):** which platforms does the next release claim?
- **Option A (recommended for overhaul closure):** Linux x86-64 (glibc) only.
  Remove/disable the macOS and Windows release jobs, add `#error` for
  `_WIN32` and non-Linux in `turfrt_gc.c` with a pointer to the porting
  notes, update README's Installation section. Porting becomes roadmap
  REL-003 (in or out of 2.0 per its own decision).
- **Option B:** port now (arm64 AAPCS64 callee-saved set `x19–x28`, `fp`,
  `d8–d15`; macOS stack bounds via `pthread_get_stackaddr_np`; Win64 set +
  `GetCurrentThreadStackLimits`). That is roadmap-sized (L), not a closure fix.

**Completion criteria (Option A).** A tag build of the workflow can only
produce artifacts for platforms where `scripts/gc_smoke.sh` and
`runtime_smoke.sh` pass; README matches.

---

### OVH-014 — FREEZE conformance: disclosed narrowings — **DECISION**

- **Category / severity:** C · low. All four only *reject* or *trap* — none
  miscompiles.

| # | FREEZE text | Implemented | Source |
|---|---|---|---|
| 1 | §6.2 row 6: "Compile-time on typed constants: **error**" | `immut a: int = INT64_MAX; immut b = a + 1;` compiles and **traps at runtime** | SEM-002 *Known pitfalls*; verified live |
| 2 | §6.2 row 6: untyped literal arithmetic, "only the final value is checked" | a >int64 literal *leaf* inside a larger literal expression is rejected immediately | SEM-002 *Known pitfalls* |
| 3 | §6.2 row 9 / §10.5: an untyped literal assigned to `double` must be exactly representable | a literal > int64 is rejected even when exactly representable as `double` | SEM-002 *Known pitfalls* |
| 4 | §6.1: depth-cap diagnostic "naming the instantiation chain" | E079 dumps the whole instantiation cache, not the chain | TYPE-002 *Known pitfalls*; D25 |

**Decision required (user):** for each row, *implement* or *record as an
accepted narrowing in `FREEZE.md`* (FREEZE edits need an explicit request —
this is it). **Recommendation:** record 2 and 3 as accepted narrowings;
implement 1 (fold `immut` locals with a literal-foldable initializer in
`tryFoldUntypedIntConstant()`, SEM-002's own suggested starting point — S
sized); move 4 to roadmap LEC-005 (it is D25's fix).

---

### OVH-015 — Documentation and bookkeeping drift

- **Category / severity:** C · medium. Do this **last**; it absorbs every
  doc change the other items owe.

**Owed (each verified stale on 2026-09-26):**
1. `docs/architecture/compiler_pipeline.md` §7 "Generic type erasure" and
   `docs/architecture/memory_model.md` §2.7 describe erasure as the current
   model (both PENDING.md TYPE-002 rows). These are *architecture* docs,
   which `AGENTS.md` says are kept accurate alongside the code — rewrite now,
   do not wait for the reference-docs rewrite.
2. `docs/planning/ARCHITECTURE_OVERHAUL.md` header still says "Nothing in this
   document has been implemented" — replaced by the closure-audit status
   note (done by the audit commit; verify it is still accurate at closure).
3. `INVARIANTS.md`: INV-RT-3's 64-word caveat (OVH-001), new INV-MEM-2,
   INV-CAP-2 (raw-pointer indexing gated, OVH-003), INV-LEX-1's `>>`
   exception (OVH-009); INV-LINT-1 and INV-CG-1 remain `planned` — say so in
   the closure record rather than implying otherwise.
4. `tests/07_functions_modules/export_qualified_cross_module.tr` is silently
   excluded by the runner (`_module.tr` substring) — rename it (e.g.
   `export_qualified_across_modules.tr`); it passes when run directly.
5. `README.md`: "Current version: v1.0.0", platform list (OVH-013), `&?T`
   (LANG-002's PENDING row), `defer`, `guess`, the GC (it still says leaks
   are a tradeoff in places — check).
6. `AGENTS.md`: remove the list/mem E034 caveat once OVH-011 lands; mention
   `test_runner.sh -O0` for GC tests (OVH §1).
7. `docs/development/handbook-sync/PENDING.md`: add rows for every
   user-visible change in OVH-001..014. **Do not** perform the
   `docs/reference/` rewrite here — that is roadmap REL-006.
8. The closure change record `docs/development/changes/OVH-CLOSE.md` (§4).

---

## 3. What is deliberately NOT in this file

- **DoD item 8 in its full sense** (a *dynamically sized* linked list / tree /
  graph in safe TurfLang). The tests satisfy the letter with a fixed set of
  stack nodes (`tests/39_nullable_references/01_linked_list.tr`: "there's no
  heap allocator in this test"); safe code has no way to heap-allocate a
  user struct, and `return &local;` compiles silently into a dangling
  reference. Closing it needs new language surface (a typed GC allocation
  primitive) and escape rules — a design decision, not a fix. It is roadmap
  **FDN-001/FDN-002**, reclassified by explicit decision in the audit (§5).
- Everything classified **D** (deferred) or **E** (rejected) in the audit.
- The `docs/reference/` rewrite (roadmap REL-006).

---

## 4. Overhaul closure gate

Run after the last item lands. Every line must be demonstrated (command
output looked at), not asserted:

1. `./scripts/test_runner.sh` green at default, `-O0`, `-O1`, `-O2`, `-O3`, `-Os`.
2. `turf --resolve` of every `lib/std/**/*.tr` shows no error and no warning
   other than "No entry point found".
3. Every `showcase/*.tr` compiles warning-free; `opt_levels_smoke.sh` clean;
   the two client/server pairs exercised manually.
4. `runtime_smoke.sh`, `mem_smoke.sh`, `gc_smoke.sh`, `gc_rss_smoke.sh`,
   `debuginfo_smoke.sh`, `std002_caller_location_smoke.sh`,
   `slm_deterministic_fix_smoke.sh`, `check_boundary.sh`, `check_doc_links.sh`
   all clean.
5. Every reproducer in this file is now a passing test (positive) or a
   correctly rejected test (negative), at `-O0` and default.
6. Every FREEZE.md decision is either enforced (with the test that would
   prove it wrong named in the closure record) or recorded as an accepted
   narrowing (OVH-014).
7. `INVARIANTS.md` has no row whose status is wrong.
8. Every OVH item has a change record; `INDEX.md` rows exist.
9. `docs/architecture/**` matches the code (OVH-015 item 1).
10. `grep -rn "TurfGenericParamType" src/Codegen*.cpp` shows only the INV-GEN-1
    assertions; no erasure path is reachable.
11. No known regression, ICE, or compiler segfault from any input in this
    file or the audit.
12. `docs/development/changes/OVH-CLOSE.md` states the Overhaul is closed,
    lists the gate evidence, and records the DoD-8 reclassification. Then
    delete this file and its `session_preflight.sh` candidate line.
