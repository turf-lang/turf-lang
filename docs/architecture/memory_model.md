# The Memory Model: Storage, Lifetime, and Ownership

Complete internal + external reference for what actually happens to memory in
a compiled Turflang program: which values live on the stack, which live on
the heap, which are baked into the binary as static/read-only data, what's
copied and what's aliased, and what manages any of it. Like the other docs in
this directory, this describes both **what a Turflang programmer observes**
(Part 1) and **how the compiler actually implements it, pass by pass**
(Part 2), with file:line references throughout.

This document deliberately explains everything in ordinary computer-science
terms — stack frames, heap allocations, static/read-only data, value vs.
reference semantics — not LLVM instruction names. Turflang compiles down
through LLVM to native code, but nothing here depends on knowing LLVM IR.

Related docs:
- `docs/architecture/pointers_and_references.md` — the full reference for
  `ptr`, `T*`, `&T`, and `unsafe { }`. This document assumes you've read at
  least its Part 1 and cross-references it rather than repeating it.
- `docs/architecture/modules_and_scope_resolution.md` — `::new`/`::default`
  construction, which this document's struct section depends on.
- `docs/reference/arrays.md` — the user-facing array reference; this
  document's array section verifies and extends it with the internals.
- `docs/planning/FUTURE.md` §5 — a proposed future automatic-reference-
  counting (ARC) memory model. Not implemented; mentioned once in §1.7 below
  and not otherwise discussed, since this document describes the compiler as
  it exists today.

---

## Part 1 — The memory model as observed

### 1.1 The short version

| Type | Where it lives | Copied or aliased? | Who manages its lifetime |
|---|---|---|---|
| `int`, `int32`, `double`, `bool`, `char` | Stack (or a register) | Copied (value semantics) | Automatic — freed when its stack frame pops |
| `string` **literal** | Static, read-only, baked into the binary | N/A — never copied, only the pointer is | The program's whole lifetime; never freed |
| `string` from `+`/`to_upper`/etc. | **GC heap** | Aliased (a `string` is a pointer) | **The collector** (MEM-002) — reclaimed once nothing reachable still points to it |
| Fixed-size array `T[N]` | Stack | Copied (value semantics) | Automatic |
| Struct (local, or via `::new`/`::default`) | Stack, at the point of construction | Copied on assignment/pass/return | Automatic, *if* it never leaves the stack — see §1.5 |
| `ptr` / `T*` (raw pointer) | Wherever it was given — usually the manual heap, via `mem::alloc`/`malloc` | Aliased | **You.** Manual `mem::free`/`free`, or nothing |
| `&T` (safe reference) | Same representation as `T*` | Aliased | Same as `T*` — the only difference is a compile-time non-null guarantee, not a lifetime guarantee (see `pointers_and_references.md`) |
| `List<T>`/`Stack<T>`/`Queue<T>`/`Map<K,V>`'s backing buffer | **GC heap** | Aliased, grown via `gc_realloc` | **The collector** (MEM-002) |
| `mem::Pool<T>`'s backing buffer | Manual heap | Aliased, grown via `realloc` | **You**, via `mem::free_pool()` — deliberately NOT collected, see §1.7 |

The one-sentence version of Turflang's safety story, as of **MEM-002**
(`docs/planning/ARCHITECTURE_OVERHAUL.md` §8/§14): **there are two
disjoint heaps, and only one of them is automatically managed.** The
**manual heap** (`mem::alloc`/`malloc` and everything built on them,
including `mem::Pool<T>`) still has no automatic memory management of any
kind — no garbage collector, no reference counting, no borrow checker —
exactly as this document always said. The **GC heap** (every `string`
result, every `List`/`Stack`/`Queue`/`Set`/`Map` backing buffer) is now
reclaimed by a real non-moving, mark-sweep collector, `libturfrt`'s
`turf_rt_collect()`, run automatically on allocation pressure or forced
explicitly from Turf source via `gc_collect()`. `&T` (§1.6) still buys you
only a compile-time non-null guarantee, not a lifetime guarantee — it does
not prevent a dangling reference into the MANUAL heap, though a `&T` into
the GC heap is protected by the collector like any other GC pointer as
long as it (or something that reaches it) is itself reachable. The two
heaps never merge (F-MEM-3): `mem::free()`ing a GC-heap pointer is a
diagnosable runtime panic, not undefined behaviour (§1.4, §1.7).

### 1.2 Primitives: `int`, `int32`, `double`, `bool`, `char`

All five are stored directly in a variable's own stack slot (or promoted to
a CPU register by the optimizer) and have ordinary value semantics: assigning
one variable to another copies the bits, and there is no notion of two
variables "aliasing" the same `int`.

- `int` is a 64-bit signed integer — Turflang's only general-purpose integer
  type, and the default for anything that looks like a whole number.
- `int32` is a genuinely 32-bit signed integer, which exists *specifically*
  so `extern fn` bindings can describe a C function whose real ABI is a
  32-bit `int` (glibc's `strcmp`, `toupper`, `tolower` — declaring those as
  Turflang's 64-bit `int` reads garbage out of the upper 32 bits of the
  return register). Ordinary Turf code essentially never needs `int32` —
  arithmetic on an `int32` value promotes it to a 64-bit `int` automatically,
  the same way `bool` arithmetic promotes to `int`.
- `double` is a 64-bit IEEE-754 floating-point number.
- `bool` is a genuine 1-bit boolean, not an integer — it's deliberately kept
  out of every implicit numeric-coercion path (`Types.h:214`), so `bool`
  never silently widens to or from `int`/`double`.
- `char` is a single byte holding an ASCII/byte value, **not** a Unicode
  codepoint, and **not numeric** — comparisons work, but `'a' + 1` is
  rejected outright; you must explicitly cast to `int` first
  (`Types.h:223-233`). This is a deliberate design choice, not a limitation
  that will later be lifted for free — treating `char` as numeric would pull
  it into every implicit-widening rule the way `int32` was deliberately
  designed to.

### 1.3 Fixed-size arrays: `T[N]`

A declared `int[5]` is a genuine stack-allocated block of 5 contiguous
`int`s — not a pointer, not a heap allocation. **Arrays are value types**
(`docs/reference/arrays.md` §"Arrays are value types" — verified against the
implementation in §2.3 below): assigning one array variable to another
copies every element; mutating the copy never touches the original. The
same is true when an array is passed as a function argument or returned
from a function — each of those is a full, independent copy, not a shared
reference.

**The unsized spelling `T[]` is a slice (a parameter or a local view), and it
is *not* a value type.** It exists so one function can accept an array argument of any
length (`fn sum(arr: int[]): int` called with either an `int[5]` or an
`int[10]`). Passing an array to a `T[]` parameter does **not** copy it — the
callee receives a slice: the address of the caller's own array plus its
element count (issue #260, §2.3.2). This is the one place a fixed array's
storage is genuinely shared rather than copied, so its lifetime is kept to
the call by construction rather than by analysis: `T[]` may only be a
parameter's own type or a local variable's (a view, from a range slice
`arr[a..b]`, or an array or another slice; a literal does not make a view,
it sizes the declaration, §2.3.1), and a local lives no longer than its function. Everywhere that could outlive the frame is E140
(field, return type, element type, generic argument, function type, behind
`&`/`*`; no cast produces one), and `arr to ptr` on one is rejected (E134).
A write through a slice changes the original array, so passing an `immut`
array (or a slice of one) to a parameter the callee writes through is E141,
and a `mut` view of an `immut` array is E144. A slice into a `heap()` block's
array field holds an interior pointer, which the conservative collector's
interior-pointer retention (`tests/41_gc/03_interior_pointer_retention.tr`) already keeps alive, so the GC needed no
change (`tests/41_gc/12_slice_into_heap_struct_survives_collection.tr`).

### 1.4 `string`

A `string` is, at the machine level, nothing more than a raw pointer to a
sequence of bytes — the same representation as `ptr`/`T*` (§2.4). What
differs entirely by *how a given string value came to exist*:

**A string literal** (`"hello"`) is baked into the compiled binary as a
static, read-only block of bytes, present for the whole life of the
program. It is never allocated, never freed, and every occurrence of the
same literal text in the same compilation may or may not be deduplicated —
either way, nothing about it is mutable storage you're meant to write
through. (Nothing in the type system currently stops you from writing
through a string-literal pointer via indexed assignment, since `char[]`-style
element writes are permitted on any `string`-typed lvalue — see §2.4 — but
doing so targets memory the linker placed as read-only, which is undefined
behavior/likely to crash at runtime, not a supported operation.)

**A string produced by concatenation (`a + b`), or by almost any stdlib
string function** (`to_upper`, `to_lower`, `substring`, `trim`, `replace`,
`repeat`, ...) is a **fresh GC-heap allocation**, sized exactly for its
contents. Every buffer-returning function in `lib/std/core/strings.tr`
calls the bare `gc_alloc(n)` intrinsic (`include/Intrinsics.h` category 1),
which lowers to `libturfrt`'s `turf_rt_alloc()` (`runtime/turfrt.h`) —
never `malloc` directly. `io::input()`/`io::read_to_string()` and
`net::TcpConnection.read()` use the zero-filling sibling,
`gc_alloc_zeroed(n)`, for the same reason `calloc` used to be chosen over
`malloc` there (see §2.4/§2.6).

As of **MEM-002** (`docs/planning/ARCHITECTURE_OVERHAUL.md` §8/§14), none of
these functions need to free the buffer they return, or the operand(s) they
were given — a real collector (`libturfrt`'s `turf_rt_collect()`) reclaims
each one once nothing reachable still points to it, the same as any other
GC-heap value. `result = result + s;` inside a loop (as `strings::repeat`
and `strings::replace` both do internally) allocates a fresh buffer per
iteration exactly as before — the difference is that the PREVIOUS
iteration's now-unreachable buffer is collected, not leaked, the next time
allocation pressure (or an explicit `gc_collect()`) triggers a cycle.
`tests/13_string_stdlib/17_stress_10k_concat.tr` (10,000 concatenations in
a loop) and `tests/08_e2e_stress/06_gc_bounded_allocation.tr` (peak RSS
measured under `scripts/gc_rss_smoke.sh`) are this claim's own regression
coverage.

A `string`'s representation is still pointer-identical to `ptr`/`void*`
(`CodegenCore.cpp`'s `isPointerLike()`, §2.4), and that identity is still
NOT license to treat it like a manual-heap pointer: a `string` returned by
concatenation or a stdlib string function lives on the **GC heap**, and
`mem::free()`ing a GC-heap pointer is exactly the cross-heap mistake
F-MEM-3 forbids (the two heaps "do not merge"). As of MEM-002 this is no
longer merely unsupported — `mem::free()` checks every pointer it's handed
against the collector's own block table (`gc_is_managed`/
`turf_rt_is_gc_pointer`) and panics with a named diagnostic instead of
silently corrupting the manual heap's own bookkeeping or the collector's;
see `tests/41_gc/fail_free_gc_pointer.tr`.

### 1.5 Structs

Struct values have ordinary **value semantics** — the same story as fixed
arrays, and unlike `class` in most reference-semantics languages:

- **Assignment copies.** `mut b: Point = a;` copies every field's bits into
  `b`'s own, independent stack storage — mutating `b` afterward never
  touches `a` (verified in §2.5).
- **Passing a struct to a function copies it.** The callee receives its own
  independent stack copy; there is no implicit pass-by-reference for
  struct-typed parameters (contrast with `self`, which genuinely is a raw
  pointer under the hood — see §1.5.1).
- **Returning a struct by value copies it out.** `fn make(): Point { ... }`
  hands the caller a fresh copy; nothing is left aliased to the callee's own
  now-popped stack frame.
- **A struct field that is itself a struct is embedded inline** — not
  stored as a pointer to a separately-allocated sub-object. A
  `struct Line { a: Point, b: Point }` is one contiguous block of memory
  containing both `Point`s directly, the same way a C struct-in-struct
  works, verified in §2.5.1.

**Where does a struct actually live?** Wherever it was declared: an ordinary
local (`mut p: Point = Point::new(1, 2);`) lives on the stack, popped
automatically when its enclosing function returns — including a struct
constructed via `Point::new(...)`/`Point::default()`, which allocates its
own temporary on the **stack**, not the heap, before returning a copy of it
(verified in §2.5.2). The struct *type* itself carries no opinion about
where its bytes live — a stack local is the default, but two other
placements exist: `mem::alloc(sizeof-equivalent-byte-count)` cast to `T*`
(the **manual** heap, entirely your own bookkeeping from that point on,
§1.7), or, as of **FDN-001** (`docs/planning/ROADMAP_v2.0.0.md`),
`heap(structValue): &T` (the **GC** heap, §1.7, §2.6) — the only safe way
to get a `&T` pointing at a heap-resident value at all, since the sole
other source of a `&T` is `&localVar`, whose pointee is still exactly the
originating stack frame's storage (§2.5, §1.6 below).

#### 1.5.1 `self` is the one exception to "structs are copied"

A struct method's implicit `self` parameter is a raw pointer
(`StructName*`) to the *original* struct the method was called on, not a
copy — `self.field = value;` inside a method genuinely mutates the caller's
struct. This is deliberately unlike an ordinary struct-typed parameter (see
`pointers_and_references.md` §2.5.3 for why `self` stays a raw pointer
internally even after the `&T`/`unsafe` work).

### 1.6 Pointers (`ptr`, `T*`) and safe references (`&T`)

Covered in full in `docs/architecture/pointers_and_references.md` — this
section only summarizes the memory-model-relevant facts so this document
stands on its own:

- `ptr` (opaque) and `T*` (typed) are both raw, unmanaged pointers: whatever
  address they hold, valid or not, the compiler trusts you. They point at
  whatever gave rise to them — a `mem::alloc`/`malloc` return value, an
  address-of expression, or a cast from an integer.
- `&T` is a **compiler-checked non-null guarantee over the exact same
  underlying representation as `T*`** — at the machine level, a `&T` and a
  `T*` are identical, indistinguishable values. The safe/raw distinction is
  purely a compile-time fiction the TypeChecker enforces (`&expr` can never
  produce `null`, and a `&T` can be widened to `T*` for free but never the
  reverse without an explicit `unsafe`-gated cast).
- **Neither `T*` nor `&T` carries any lifetime information** as a TYPE —
  but as of **FDN-002** (`docs/planning/ROADMAP_v2.0.0.md`;
  `docs/development/changes/FDN-002.md`; `FREEZE.md` §6.13/F-MEM-5,
  `INV-MEM-6`), a `&T` provably pointing into the CURRENT function's own
  stack frame can no longer be returned, stored into a field of a
  longer-lived object, pushed into a collection, or boxed with `heap()` —
  `EscapePass` (`include/Analysis.h`) catches the syntactic shapes above
  intraprocedurally, conservatively (never a false positive on code that
  was actually safe), at compile time. It is **not** a general lifetime
  system: a reference smuggled out inside a by-value struct copy, through
  an intermediate function call, or via raw pointer arithmetic (`arr[i]`'s
  own address, `unsafe`-gated) is still uncaught — see
  `docs/architecture/pointers_and_references.md` §1.9 for the full rule and
  its documented residual gap. "Safe" here still means "can't be null and
  can't (in the cases above) provably dangle," never "carries a lifetime."

### 1.7 What manages any of this? Two heaps, one of them automatic.

As of **MEM-002** (`docs/planning/ARCHITECTURE_OVERHAUL.md` §8/§14), the
heap is split into two genuinely disjoint categories with two different
answers to "who reclaims this":

- **The manual heap.** `mem::alloc`/`mem::alloc_zeroed`/`mem::grow`/
  `mem::dup`/`mem::alloc_aligned` (`lib/std/mem/mem.tr`) are thin, fail-fast
  wrappers around `malloc`/`calloc`/`realloc`/`posix_memalign` — you own
  whatever they return, exactly as in C: no garbage collector, no reference
  counting, no ownership/borrow checker, no destructors run on scope exit.
  `mem::Pool<T>` (also `mem.tr`) grows its own `malloc`/`realloc`-backed
  backing array the same way — it exists specifically to make *manual*
  bulk-freeing less tedious (track every allocation, then
  `mem::free_pool()` them all in one call), so its own bookkeeping array
  stays on the manual heap right alongside what it tracks. **The collector
  never traces into this heap at all** (F-MEM-3, FREEZE.md §6.5 — "the two
  heaps do not merge"); a pointer that lives ONLY inside manually-managed
  memory is not a root, even if it points at a GC-heap object (a real,
  documented, deliberately-accepted unsoundness — see MEM-002's own change
  record, *Known pitfalls*). **OVH-010**
  (`docs/development/changes/OVH-010.md`) gives this its escape hatch:
  `mem::root(p)`/`mem::unroot(p)` (`lib/std/mem/mem.tr`, `gc_pin`/`gc_unpin`
  bare intrinsics, `turf_rt_pin`/`turf_rt_unpin` in `runtime/turfrt_gc.c`)
  let a program mark the GC object *containing* `p` — an interior pointer
  is fine, matching how a stack/register root is already treated — as an
  always-retained root for as long as a matching `unroot()` hasn't released
  it (pins nest: an internal per-object count, not a boolean). This does
  not change the unsoundness above by default: nothing pins anything
  automatically, and a `mem::alloc` buffer holding a GC pointer with no
  `mem::root()` call anywhere is exactly as unsound as before — `root()` is
  an opt-in fix a program applies at the specific spot it needs it, not a
  blanket scan of manual memory (which F-MEM-3 forbids: the two heaps still
  never merge, and the collector still has no block-table entry for
  manually-managed memory at all).
- **The GC heap.** `List<T>`, `Stack<T>`, `Queue<T>`, `Set<T>`, and
  `Map<K,V>` each grow a backing buffer internally via the
  `gc_alloc(n)`/`gc_realloc(p, n)` bare intrinsics (`include/Intrinsics.h`
  category 1, `lib/std/collections/*.tr`, and the shared growth policy
  documented in `docs/reference/stdlib/collections.md`); string
  concatenation and most `strings::*` functions allocate here too (§1.4).
  As of **FDN-001** (`docs/planning/ROADMAP_v2.0.0.md`), ordinary user code
  can put an arbitrary value here directly, too: `heap(expr): &T` copies
  `expr`'s value (a struct, a fixed array, or a primitive) into a fresh
  `turf_rt_alloc()` block and returns its address as a `&T` — the intrinsic
  that makes a *dynamically*-sized linked structure (list, tree, graph)
  writable in safe TurfLang at all, since the only other source of a `&T`
  is `&localVar`, whose pointee is a stack frame that pops on return (§1.6,
  §2.6).
  **A real, non-moving mark-sweep collector reclaims every one of these
  once nothing reachable still points to it** — `libturfrt`'s
  `turf_rt_collect()` (`runtime/turfrt.h`/`runtime/turfrt_gc.c`), run
  automatically on allocation pressure (a heap-doubling heuristic) or
  forced explicitly from Turf source via the `gc_collect()` bare
  intrinsic. There is still no destructor mechanism in the language, and
  none is needed here — the collector's job is exactly "find what's
  unreachable and reclaim it," which needs no per-type cleanup hook the
  way a `class` destructor would (`defer`, LANG-003, is TurfLang's answer
  for deterministic OS-resource cleanup, which a tracing collector
  deliberately does not attempt — no finalizers, F-MEM-2).

Root discovery (§2.6 has the full compiler-side mechanism) is precise for
registered global roots, pinned objects (`mem::root()`, OVH-010, just
above), and a GC-heap block's own declared pointer fields (a
compiler-emitted `TurfTypeDesc` bitmap), and conservative for the
stack and callee-saved registers — the standard Boehm-style technique,
chosen specifically because it needs zero Codegen changes for root finding
(`ARCHITECTURE_OVERHAUL.md` §14.3). Conservative scanning can over-retain
(treat a stale value that merely looks like a pointer as a root) but never
under-retain in a way that would free something genuinely reachable — see
`docs/development/changes/MEM-002.md` for the measured false-retention/
pause-time/throughput numbers.

A future automatic-reference-counting (ARC) design exists only as a
proposal — `docs/planning/FUTURE.md` §5 — explicitly rejected in favour of
the tracing collector actually built (see `ARCHITECTURE_OVERHAUL.md`
§14.3's own three reasons: cycles, retain/release Codegen cost, and
FFI-boundary pinning). Nothing described in this document should be read
as ARC being implemented anywhere.

---

## Part 2 — How the compiler implements this

### 2.1 Pipeline recap

```
source.tr
   │
   ▼
 Lexer  ──tokens──▶  Parser  ──AST──▶  Resolver (2 passes)  ──annotated AST──▶  TypeChecker  ──▶  Codegen (LLVM IR)
```

Everything in this document is a **Codegen**-layer story — the Resolver and
TypeChecker decide what's legal (is this assignment type-compatible, is this
pointer used inside `unsafe { }`, ...), but the actual "where does this
value's bits live, and are they copied or aliased" decisions are made
entirely in `src/Codegen*.cpp`, by which LLVM construct each AST node
lowers to. The type hierarchy that drives all of it is `include/Types.h`
(overview at its own header comment, `Types.h:10-28`).

### 2.2 Primitives

`getLLVMType()` (`src/CodegenCore.cpp:297-479`) maps each primitive
`TurfType` to a fixed-width machine type with no indirection at all:

```cpp
if (T->isNamed("int"))    return llvm::Type::getInt64Ty(*TheContext);   // CodegenCore.cpp:308
if (T->isNamed("int32"))  return llvm::Type::getInt32Ty(*TheContext);   // CodegenCore.cpp:310
if (T->isNamed("double")) return llvm::Type::getDoubleTy(*TheContext);  // CodegenCore.cpp:312
if (T->isNamed("bool"))   return llvm::Type::getInt1Ty(*TheContext);    // CodegenCore.cpp:314
if (T->isNamed("char"))   return llvm::Type::getInt8Ty(*TheContext);    // CodegenCore.cpp:316
```

A local variable of any of these types gets an ordinary stack slot
(`CreateEntryBlockAlloca`, always placed at the top of the function's entry
block regardless of where in the source the declaration textually appears —
this is what makes recursion/loops safe: each call gets its own fresh
frame, not a shared slot). `NumberExprAST::codegen()`/`BoolExprAST::codegen()`/
`CharExprAST::codegen()` (`src/CodegenExpr.cpp:45-62`) each produce a plain
immediate constant with no allocation of any kind — reading a numeric
literal never touches memory until it's stored into a variable's slot.

`Int32Type` and `CharType`'s "no implicit arithmetic promotion" design is
enforced at the type level, not Codegen: `Int32Type::isNumeric()` returns
`true` (`Types.h:196`, so `int32` flows through the ordinary numeric
paths and widens to `int` for free), while `CharType` deliberately does
**not** override `isNumeric()` (`Types.h:234` onward) — this single flag is
what keeps `char` out of every implicit-widening/arithmetic rule in
`typesCompatible()` with no separate exclusion logic needed anywhere else.

### 2.3 Fixed-size and unsized arrays

#### 2.3.1 Declaration: a real stack allocation, memset to zero

`ArrayDeclExprAST::codegen()` (`src/CodegenArray.cpp:50-111`) allocates a
genuine LLVM array type sized to the declared element count directly on the
stack:

```cpp
llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, Size);
AllocaInst *Alloca = TmpB.CreateAlloca(ArrTy, nullptr, Name);   // CodegenArray.cpp:58-62
```

immediately followed by an explicit zero-fill of the whole block
(`Builder->CreateMemSet(...)`, `CodegenArray.cpp:64-71`) before any
initializer is applied — this is what makes `mut arr: int[5];` (no
initializer) a defined, all-zero array rather than reading uninitialized
stack garbage, matching `docs/reference/arrays.md`'s documented behavior.
`getLLVMType()`'s array branch (`CodegenCore.cpp:332-337`) reflects the same
split the type carries: `Size > 0` (a concrete `T[N]`) lowers to a real
fixed-length array type; `Size == -1` (the unsized `T[]` spelling) lowers to
the slice `{ ptr, i64 }` (`getSliceLLVMType()`): the first element's address
and the element count. For-each, `arr.length()` and the bounds check on
`arr[i]` all read the count from the second field. A **local** `T[]` variable
is a `{ ptr, i64 }` slot too: `ArrayDeclExprAST::codegen()` stores the slice
built from its initializer into it. An array literal initializer does not
take this path: the Parser (`src/ParserStmt.cpp`) gives an unsized declaration
with a literal initializer `ArraySize` = the literal's length, so
`mut nums: int[] = [10, 20, 30];` is simply `int[3]`, an ordinary owned stack
array (only the outermost dimension is inferred; `int[][2]` becomes
`int[2][2]`). An empty literal has no length to give (E123). Only an array or
slice initializer still yields a `{ ptr, i64 }` view.

**`arr.copy()`** is the way to get an independent `T[]`. In
`MethodCallExprAST::codegen()` (`src/CodegenStruct.cpp`) the receiver is
turned into `{ data, len }` with the same `CodegenSliceArgument()`, then
`len * elemSize` bytes are allocated with `turf_rt_alloc`, passing the
descriptor from `getOrCreateElementTurfTypeDesc(elem)` so the collector traces
the elements precisely (a `string[]` copy keeps its strings alive,
`tests/41_gc/13_array_copy_elements_survive_collection.tr`), and the elements
are `memcpy`'d. The result is the slice `{ block, len }`; the only reference
to the block is that slice on the stack, which the conservative stack scan
finds. Since the result is a fresh block, not a view of the receiver, it is
never an E144/E141 concern and an `immut` receiver is fine.

#### 2.3.2 Passing an array: copy for `T[N]`, slice for `T[]`

For a `T[N]` parameter, the argument is whatever the argument expression's
ordinary `codegen()` produces — for a plain array variable, that's
`VariableExprAST::codegen()`'s `CreateLoad` of the *entire* aggregate
(`src/CodegenVar.cpp`, the same load path every scalar variable read goes
through) — i.e. a genuine bit-for-bit copy of every element gets passed by
value.

For a `T[]` parameter, every call form (ordinary, UFCS receiver and
arguments, method, static method, constructor, module function) evaluates
that argument with `CodegenSliceArgument()` (`src/CodegenArray.cpp`)
instead, which builds `{ address, N }`:

- an argument with storage of its own (a variable, a struct field, a row of
  a 2D array) contributes its own address — **no copy**, which is why a
  `T[]` parameter aliases the caller's array;
- an argument without (a literal, a call returning `T[N]`) is first stored
  in a temporary in the caller's frame, which outlives the call;
- an argument that is already a `T[]` (a parameter handed on) is passed as
  the slice it is.

A range slice `arr[a..b]` is built by `SliceExprAST::codegen()`
(`src/CodegenArray.cpp`). It takes the base's `{ data, length }` from the same
`CodegenSliceArgument()` (so a `T[N]` is viewed in place and a `T[]` passes
through), evaluates each present bound once and adds the length to a
negative one, then checks `0 <= start <= end <= length`. A failing check
branches to a runtime panic (`Slice range out of bounds for 'NAME'`); the
TypeChecker has already turned the provable cases into E143. The result is
`{ data + start, end - start }`: no element is copied, so the view aliases the
original array exactly as a `T[]` parameter does. Slicing a slice, indexing a
slice expression and passing one to a `T[]` parameter all compose from this.

Before issue #260 this was a bare pointer taken with `codegenLValue()` on a
plain variable only: a literal or call-result argument was an internal
compiler error, and forwarding a `T[]` parameter passed the address of the
slot holding the pointer, so the next callee read garbage.

#### 2.3.3 Assignment and return: whole-aggregate copy

`ArrayLiteralExprAST::codegen()` (`CodegenArray.cpp:114-136`) builds a
literal by inserting each evaluated element into an initially-undefined
aggregate value one at a time (`CreateInsertValue`) — this in-register
aggregate is then stored into a real alloca the same way any other
initializer is (`VarDeclExprAST::codegen()`, §2.5.3's general path applies
identically to arrays, since neither function special-cases arrays there).
Reading an array-typed variable back out, or `return`ing one, goes through
the same generic `CreateLoad`/`CreateRet` path every value type uses
(`ReturnExprAST::codegen()`, `src/CodegenControlFlow.cpp:889` — no
array-specific branch exists there at all) — the entire array's bytes are
loaded as one aggregate value and handed to the caller, which is what makes
`fn make3(): int[3] { ... }` (`tests/05_arrays_pointers_ffi/11_array_return_and_assign.tr:16-19`)
a safe, independent copy rather than a pointer into the callee's popped
stack frame: nothing about a fixed-size array return ever produces a raw
address at all, only a value.

### 2.4 Strings

`StringExprAST::codegen()` (`src/CodegenExpr.cpp:65-68`) is one line:

```cpp
Value *StringExprAST::codegen() {
  return Builder->CreateGlobalString(Val, "str");
}
```

This places the literal's bytes as static data compiled directly into the
binary, and returns a pointer to it — no allocation instruction of any kind
runs when a string literal is evaluated, matching §1.4's "baked into the
binary" description. `getLLVMType()` maps `string` to a bare opaque pointer
(`CodegenCore.cpp:317-318`, identical to `ptr`) — there is no length field,
no capacity, no reference count anywhere in a `string`'s runtime
representation; it is exactly a `char*` in the C sense, NUL-terminated by
convention (every stdlib string function that builds one, e.g.
`strings.tr`'s `operator +`, explicitly leaves room for and writes a
trailing NUL via `strcpy`/`strcat`/manual `buf[len] = 0`).

`isPointerLike()` (`CodegenCore.cpp`, used throughout `CastToType()`)
explicitly groups `string` with `ptr`/`T*`/`void*` as sharing one LLVM
representation — this is why a `gc_alloc(n)`/`gc_alloc_zeroed(n)` call
(returning a raw `void*`) can be cast `to string` with no representation
change at all, just a compile-time relabeling. It's also why a leaked
string buffer's bytes are physically indistinguishable from a manual-heap
`void*`'s — but as of MEM-001 that no longer means they're safe to treat
the same way; see §1.4's note on `mem::free` and the GC heap.

**Concatenation moved out of Codegen entirely.** `src/CodegenExpr.cpp:82-94`
documents that `EmitStringConcat`/`EmitStrcmp`/`EmitStrcmpRelational` used to
be hand-rolled directly in this file; `+`/`==`/`!=`/`<`/`>`/`<=`/`>=` on two
`string`s are now ordinary `operator` overloads defined in Turf itself
(`lib/std/core/strings.tr:55-85`), resolved through the exact same
operator-overload machinery any user-defined `operator` would go through.
`operator +`'s body is what actually performs the heap allocation
(`strings.tr`, `operator +`):

```turf
operator +(a: string, b: string): string {
    mut n: int = strlen(a) + strlen(b) + 1;
    mut buf: string = gc_alloc(n) to string;
    strcpy(buf, a);
    strcat(buf, b);
    return buf;
}
```

`gc_alloc(n)` — sized exactly for the concatenated result plus a NUL
terminator, nothing more — is the entire allocation; there is no
over-allocation/capacity headroom the way a growable collection has. Before
**MEM-001** this called `malloc(n)` directly; the intrinsic lowers to
`libturfrt`'s `turf_rt_alloc(n, NULL)` instead (`runtime/turfrt.h`) — same
underlying `malloc` today (MEM-002 hasn't given the GC heap its own
allocator yet), but now reachable by a real collector once one exists,
which a bare `malloc` call baked into stdlib source never would have been.
The function's own doc comment (quoted in full in §1.4) names this the
"TURF-MEMLEAK tradeoff" explicitly: the returned buffer is never freed by
this function, by its caller, or by anything else in the standard library.
Every other buffer-returning function in the same file (`to_upper`,
`to_lower`, `substring`, `trim`, `replace`, `repeat`, `pad_left`,
`pad_right`) follows the identical shape — `gc_alloc`, fill, return, never
free — `replace` and `repeat` additionally call `operator +` (via plain `+`)
in a loop, so each one leaks once per concatenation performed internally, on
top of its own final result. `io::input()`/`io::read_to_string()`
(`lib/std/io/io.tr`) and `net::TcpConnection.read()`
(`lib/std/io/net.tr`) follow the same shape with `gc_alloc_zeroed(n)`
instead — see §2.6's MEM-001 subsection for the full site-by-site
allocation classification.

### 2.5 Structs

#### 2.5.1 Layout: fields embedded inline, not pointer-chased

`StructDeclExprAST::codegen()` (`src/CodegenStruct.cpp:580-...`) builds a
real LLVM aggregate struct type and calls `setBody()` with each field's
*own* type directly — a field whose declared type is itself another struct
gets that struct's actual LLVM type embedded in place, not a pointer to a
separately-heap-allocated instance. This is why `getLLVMType()`'s struct
branch (`CodegenCore.cpp:345-349`) is the type every nested-struct field
resolves back into, and it's also why a two-step opaque-then-`setBody()`
construction sequence is needed at all (`CodegenStruct.cpp:580-640`'s
detailed comment on self-referential fields like `next: Node*` — this
concern is exactly why a raw *pointer* field, not a raw *embedded* field, is
required for anything self-referential or recursive; an embedded field of
the struct's own type would be an infinitely-sized type and could never be
built).

`Pool<T>`/`List<T>`-shaped structs, by contrast, hold a raw pointer field
(`data: T*`) precisely so their *element* storage lives on the heap and can
grow independently of the struct's own (small, fixed) header — see §2.6.

#### 2.5.2 Construction: always a stack allocation

Both `StructName::default()` and `StructName::new(...)` allocate their
`self`/result storage with `TmpB.CreateAlloca` — the same entry-block stack
allocation every other local uses, never `malloc`:

```cpp
// DefaultConstruct — CodegenFunc.cpp:774-777
AllocaInst *Alloca = TmpB.CreateAlloca(LLVMStructType, nullptr, StructName + "_default");
Builder->CreateStore(GetDefaultValue(T.get()), Alloca);
return Builder->CreateLoad(LLVMStructType, Alloca, "structval");

// ConstructorCall (::new) — CodegenFunc.cpp:817
AllocaInst *SelfAlloca = TmpB.CreateAlloca(LLVMStructType, nullptr, StructName + "_new");
```

`GetDefaultValue()` (`CodegenCore.cpp:1325-1363`) recursively zero-fills
struct fields one at a time — for a nested struct field, it recurses into
that struct's own field list and builds a nested zero-filled constant,
matching the "recursively embedded, recursively zeroed" layout from §2.5.1.
Both construction forms end by **loading** the freshly-built alloca back
into a plain value (`Builder->CreateLoad`) and handing that back to the
caller — this load-then-return is what makes `mut p: Point = Point::new(1, 2);`
produce a real, independent copy in `p`'s own storage rather than leaving
`p` aliased to the constructor's internal temporary. (`VarDeclExprAST::codegen()`
has a specific optimization, `CodegenVar.cpp:113-141`, that detects exactly
this "just-loaded-from-a-just-constructed-alloca" shape and reuses the
constructor's own alloca directly under the destination variable's name
instead of allocating *yet another* slot and copying into it a second time
— purely an allocation-count optimization; the semantics are identical
either way, since nothing else ever gets a handle to the constructor's own
temporary to alias against.)

#### 2.5.3 Assignment, parameter passing, and return: whole-aggregate copy

`VarDeclExprAST::codegen()`'s ordinary (non-construction-shortcut) path
(`CodegenVar.cpp:107-160`) applies uniformly to every type, structs
included: the initializer expression is evaluated to a plain value
(`Init = InitVal->codegen()`, which for a struct-typed source expression is
a full `CreateLoad` of the whole aggregate, exactly like §2.3.3's array
case), then stored whole into a brand-new stack alloca
(`Builder->CreateStore(Init, Alloca)`). There is no special-casing anywhere
in this path that would make a struct-typed assignment share storage
instead of copying it — the same aggregate-load/aggregate-store shape that
gives arrays value semantics (§2.3.3) gives structs value semantics for
free, with no per-type logic required. Function-parameter passing
(`CodegenFunc.cpp:339-348`) and `return` (`CodegenControlFlow.cpp:889-901`)
are the identical generic paths described in §2.3.3 and §2.2 respectively —
neither has a struct-specific branch, so a struct argument/return value is
copied the same mechanical way an `int` or an `int[5]` is.

The one deliberate exception, `self`, is never passed through this generic
value path at all: the implicit `self` parameter is constructed as a raw
`TurfPointerType(NamedType(StructName))` at parse time
(`ParserDecl.cpp`, cross-referenced in `pointers_and_references.md` §2.5.3)
— a method call always passes the *address* of the receiver, never a copy,
which is what makes `self.field = value;` inside a method a real mutation
of the caller's own struct rather than a no-op mutation of a throwaway copy.

### 2.6 Heap allocation: two heaps, one of them collected

MEM-001 drew the line between **two disjoint heaps** (FREEZE.md §6.5): the
**manual heap** (`mem::alloc` and friends, `extern fn`-declared
`malloc`/`calloc`/`realloc`) and the **GC heap** (the
`gc_alloc`/`gc_alloc_zeroed`/`gc_realloc` bare intrinsics, lowering to
`libturfrt`'s `turf_rt_alloc`/`turf_rt_alloc_zeroed`/`turf_rt_realloc` —
`runtime/turfrt.h`). **MEM-002** is what makes that split load-bearing: the
GC heap is now a real, collected heap (`libturfrt`'s
`turf_rt_collect()`, `runtime/turfrt_gc.c`), not just a naming convention
around the same underlying `malloc`.

**The manual heap** — untouched by either group (F-MEM-3):

```turf
// lib/std/mem/mem.tr
extern fn malloc(size: int): void*;
extern fn calloc(count: int, size: int): void*;
extern fn realloc(p: void*, new_size: int): void*;
```

`mem::alloc`/`mem::alloc_zeroed`/`mem::grow`/`mem::dup`/`mem::alloc_aligned`
are thin, fail-fast (`sys::panic` on a null/OOM result) wrappers around
these — no additional bookkeeping beyond what raw `malloc`/`calloc`/
`realloc` already give you. `mem::Pool<T>` also stays entirely on this
heap, including its own `items: T*` bookkeeping array — it exists to batch
a set of *manual* allocations for one-call bulk freeing
(`mem::free_pool()`), so the array holding those pointers has to be
freeable the same ordinary way, never traced by the collector. `mem::free()`
itself is no longer a bare `extern fn` (MEM-002: Turf's `extern fn` has no
alias syntax, so a checked wrapper could not share the name `free` with a
raw extern binding in the same file) — it is an ordinary Turf function that
calls the `gc_checked_free(p)` bare intrinsic
(`turf_rt_checked_free`, below), which frees `p` on the manual heap or
panics if `p` turns out to be a live GC-heap block's own start address.

**The GC heap** — where every generic collection's backing buffer, and
every `strings.tr`/`io.tr`/`net.tr` buffer-returning function's result,
lives. Every collection in `lib/std/collections/` is, at the storage
level, the identical shape:

```turf
// lib/std/collections/list.tr
struct List<T> {
  data: T*,
  len: int,
  cap: int
}
```

`data` is a GC-heap pointer, grown via the `gc_alloc(n)`/`gc_realloc(p, n)`
bare intrinsics exactly like a hand-written dynamic array in C (`list.tr`'s
`List.push`: doubling growth starting at capacity 8) — `Stack<T>`/
`Queue<T>`/`Set<T>`/`Map<K,V>` (the last with *two* backing arrays,
`keys`/`values`) all follow the identical pattern. The **struct itself** —
the small, fixed header — lives wherever the collection variable holding
it lives (stack, by default, per §1.5); only the buffer(s) it points at
are ever on the GC heap.

**The collector, in outline** (`runtime/turfrt_gc.c`; full design in
`docs/development/changes/MEM-002.md`): a **block table** records every
live GC-heap allocation's `{start address, size, TurfTypeDesc}`. A
collection is stop-the-world mark-sweep: **mark** scans the C stack and
callee-saved registers *conservatively* (every word that could plausibly
be a pointer is checked against the block table — Boehm's technique, zero
Codegen cost) plus any registered global root *precisely*, then follows
each reachable block's own declared pointer words (its `TurfTypeDesc`,
below) to find further reachable blocks; **sweep** frees every block that
was never marked. Triggered automatically on allocation pressure
(heap-doubling) or explicitly via the `gc_collect()` bare intrinsic.
**Non-moving** — an object's address never changes for its whole
lifetime, which is why a GC pointer can be handed directly to `strlen`/
`write`/any other C FFI call and remain valid for the call's whole
duration (§1.4, `tests/41_gc/04_ffi_crossing.tr`) with no pinning needed.

**Precise per-type/per-element tracing** reuses RT-001's `TurfTypeDesc`
(§2.7 — one per monomorphized aggregate) two ways: a struct instance's own
descriptor names which of its fields are pointer-shaped, and — new in
MEM-002 — a collection's backing buffer gets a descriptor for ONE ELEMENT
of its concrete `T` (built by `CastExprAST::codegen()` recognizing a
`gc_alloc(...) to T*`-shaped call site and `getOrCreateElementTurfTypeDesc()`,
`src/CodegenStruct.cpp`), which the collector's mark phase repeats across
the block's real length. A `List<int>`'s element descriptor has an
all-zero pointer bitmap (no tracing needed); a `List<string>`'s has bit 0
set per element, so the collector follows each element to the string
buffer it points at (`tests/41_gc/02_precise_element_tracing.tr` is this
mechanism's own regression test).

**Interior pointers retain their containing block.** `List<T>.data + k`
arithmetic (via `&list.data[i]`, since raw pointer arithmetic itself
doesn't exist in the language — see `pointers_and_references.md`) is
common enough in TurfLang specifically (`LIMITATIONS.md` §4) that the
block table answers "which block, if any, contains this address," not
just "is this address exactly a block's start"
(`tests/41_gc/03_interior_pointer_retention.tr`).

**A `TurfTypeDesc`'s bitmap covers the type's ENTIRE layout, at any size**
(OVH-001, `docs/development/changes/OVH-001.md`) — `word_count`
(`ceil(size / 8)`) plus a `pointer_words` array of `ceil(word_count / 64)`
`uint64_t`s, not the single inline `uint64_t` RT-001 originally shipped.
That original version silently stopped tracing past word 63 (byte 512) of
a type's layout, which MEM-002's collector then depended on for
correctness — a pointer field past that point in a GC-heap struct or
collection element was never marked, and got freed out from under a
still-live object the next collection.
`tests/41_gc/05_large_struct_element_tracing.tr` and
`06_struct_exactly_64_and_65_words.tr` are this fix's own regression
tests — see `docs/architecture/runtime.md` §4 for the emitted-IR shape.

**What is NOT reclaimed automatically:** the manual heap, still and
always (F-MEM-3) — `mem::free_pool()`/`mem::free(mem::alloc(...))` work
exactly as before. And, on the GC heap specifically, a pointer that exists
ONLY inside manually-managed memory is not a root — a documented,
deliberate unsoundness (§14.3's own "a GC pointer stored only in manual
memory is not a root"), not a bug; see MEM-002's own change record, *Known
pitfalls*, for how this was rediscovered live while measuring this
group's own performance numbers.

**Hardening and validation (FDN-005,
`docs/development/changes/FDN-005.md`)** answers the two open questions
MEM-002's own record left honestly unresolved — narrow real-world
exercise, and no independent check on the register-scanning technique:

- **`TURF_GC_STRESS=1`** (a testing-only environment variable, read once
  and cached, same shape as `TURF_RT_STATS_DUMP`) forces `turf_rt_collect()`
  on every single allocation instead of waiting for the heap-doubling
  threshold — the single most effective way to surface a root the
  conservative scan barely misses, run across the entire `tests/` suite in
  CI at both `-O0` and `-O2`.
  `tests/custom/gc_stress_corpus/` holds three long-running programs
  (a heap()-allocated graph rewired repeatedly at 1,000,000 live nodes, a
  `Map<string, List<string>>` churned for 10,000,000 operations, and the
  `http_server` showcase shape driven by a real scripted client for
  100,000 requests) that this project's own test suite had never exercised
  at anything near this scale before — run by
  `scripts/gc_stress_corpus_smoke.sh`, deliberately never under
  `TURF_GC_STRESS` (an `O(n)`-allocation workload becomes
  `O(n^2 log n)`-shaped under per-allocation collection; that file's own
  header explains why the two stress dimensions are kept orthogonal).
- **An ASan+UBSan-instrumented `libturfrt_asan.a`** (a separate CMake
  target, `CMakeLists.txt`'s `TURF_RT_SANITIZE` option) that a `turfc`
  binary built with the same option links every COMPILED TURF PROGRAM
  against instead of the ordinary `libturfrt.a` — turning the entire
  existing `tests/` suite into independent validation for freed-block
  reuse, not just the hand-written C harnesses `scripts/gc_smoke.sh`
  already covers. Two real, GC/ASan-specific interactions were found and
  fixed getting this running at all (a conservative stack scan reading
  ASan's own inserted redzones; the "address of a local" idiom for
  approximating the current stack pointer being silently relocated onto
  ASan's heap-backed fake stack — now isolated into its own
  ASan-exempted helper, `gc_approx_sp()`) — see the change record for
  both.
- **`TURF_GC_VALIDATE=1`** (Linux/glibc/x86-64 only, degrading to a
  one-time notice everywhere else) captures a second, independent
  register snapshot via glibc's `getcontext()` at the same instant as the
  primary technique's own inline-asm read, and reports (never aborts) if
  any register outside the documented callee-saved set points at a block
  the production scan did not otherwise mark live — direct empirical
  evidence for or against the "callee-saved registers survive a call" ABI
  assumption the whole design leans on, run across the full suite in CI
  (both opt levels verified live during this group's own session; the
  default level is the standing CI gate) plus the stress corpus above.
  The same variable also runs a **full-stack mark-set comparison**: a
  second, complete mark phase whose conservative roots come only from
  sources the production scan never uses (a `getcontext()` snapshot taken
  in `turf_rt_collect()`, an ancestor of `gc_mark()`, for the
  callee-saved registers and the low end of the stack; glibc's
  `pthread_getattr_np()` for the whole `[stack]` mapping's high end). That
  range is a subset of the production scan's range whenever the
  production stack base is right, so a block only the independent pass
  marks is a real missed root, and it **panics** (`GC root missed`)
  instead of sweeping a live object. It is the check that catches a wrong
  stack bound, which the register check cannot.
- **The stack base without `/proc`.** `turf_gc_record_stack_base()` reads
  the `[stack]` mapping's top from `/proc/self/maps`; when that is
  unreadable (a container or sandbox without procfs) it falls back to
  glibc's `__libc_stack_end` (the stack pointer at process entry), or on
  non-glibc Linux to `pthread_getattr_np()`, and panics if neither is
  available. Before FDN-005's follow-up it fell back to the address of a
  local in `turf_rt_init()`'s frame, a *sibling* of the user's `main`, and
  freed live objects in 11 of 12 `tests/41_gc` programs under
  `TURF_GC_STRESS`. **`TURF_GC_NO_PROCFS=1`** (testing aid) skips the
  `/proc` read so the fallback runs on an ordinary machine;
  `scripts/gc_no_procfs_smoke.sh` is its regression coverage.

#### 2.6.1 `heap(expr)`: a user value, GC-heap-allocated with a real descriptor

**FDN-001** (`docs/planning/ROADMAP_v2.0.0.md`, DEC-5) is the bare
intrinsic `heap(e: T): &T` — no grammar change, `include/Intrinsics.h`
category 1, dispatched in `FuncCallExprAST::codegen()` (`CodegenFunc.cpp`)
alongside `gc_alloc`/`gc_pin`/etc. The actual allocation logic is
`HeapAllocateValue()` (`CodegenCore.cpp`, declared in the internal
`CodegenInternal.h`): given `e`'s already-evaluated LLVM value,

1. compute `sizeof` from the value's own LLVM type
   (`DataLayout::getTypeAllocSize()`);
2. pick the real descriptor — `getOrForwardDeclareTurfTypeDescGlobal()`
   (the SAME whole-instance `TurfTypeDesc` `EmitAllTurfTypeDescs()` emits
   for that struct, §2.6 above) if `T` is a named struct, or
   `getOrCreateElementTurfTypeDesc()`'s general per-LLVM-type descriptor
   otherwise (a fixed array or a primitive — the identical helper
   MEM-002's collection-growth call sites already use for a backing
   buffer's per-element descriptor);
3. call `turf_rt_alloc(size, desc)` and store `e`'s value into the
   returned block whole, then return the pointer.

This is the exact size/descriptor-selection shape `BoxToI64Slot()`
(`CodegenCore.cpp`) already used for an oversized `!T` success value or a
message-carrying error value (OVH-002) — `HeapAllocateValue()` factors it
out as a shared helper rather than duplicating it a third time, differing
only in the return value (a real pointer, not an `i64`-boxed slot).
Never `NULL`/opaque: unlike the bare `gc_alloc(n)` intrinsic (§1.4, no
static type at that call site to build a descriptor from at all),
`heap()`'s operand always has a known static type, so the collector traces
a `heap()`ed value's own pointer fields precisely, exactly as it would the
same value sitting on the stack.
`tests/39_nullable_references/05_dynamic_linked_list.tr`/
`06_dynamic_binary_tree.tr`/`07_graph_adjacency.tr` are the correctness
regression coverage (a dynamically-sized list/tree/cyclic graph, each
surviving a forced collection while retained);
`tests/08_e2e_stress/07_heap_bounded_allocation.tr` +
`scripts/gc_rss_smoke.sh` prove the reclaim side (peak RSS bounded, not
proportional to cumulative `heap()` calls).

### 2.7 Generics and monomorphization

#### 2.7.1 Every distinct instantiation gets its own real layout

`getLLVMType()`'s handling of a `GenericType`-typed reference (`List<int>`,
`Box<Player>`, …) looks up the **per-instantiation** LLVM struct type by its
mangled instantiation name, not a single shared type standing in for every
concrete use:

```cpp
// CodegenCore.cpp:643 — GenericType branch
if (auto *GT = dynamic_cast<const GenericType *>(T)) {
  std::string InstName = Monomorphize::instantiatedDeclName(
      GT->getBaseName(), GT->getTypeArgs());
  // ... look up (or forward-declare) the StructType registered under InstName
}
```

`List<int>` and `List<string>` are, at the machine level, **two entirely
distinct** `llvm::StructType`s with two distinct sets of compiled methods
(`turf_List.push_gen1_int_...` takes a genuine `i64`; `turf_List.push_gen1_string_...`
takes a genuine string handle) — not one shared implementation reached
through a cast. This is produced by the **Monomorphizer**
(`docs/architecture/compiler_pipeline.md` §7): every generic struct
declaration is a template that never itself reaches Codegen; each concrete
type-argument tuple actually used in the program gets its own cloned
`StructDeclExprAST`, substituted field-by-field with concrete types, and
registered in `GlobalSymbolTable` under its own mangled name (e.g.
`List_gen1_int`). Reaching Codegen with an unsubstituted `TurfGenericParamType`
anywhere is a compiler bug, enforced by a debug-build `assert()` at three
sites (`Monomorphize.cpp`'s `finishInstantiation()`, and `CodegenCore.cpp`'s
`getLLVMType()`/`CastToType()`) — `docs/development/INVARIANTS.md` **INV-GEN-1**.

This replaced an earlier type-erasure implementation, in which every open
generic parameter compiled unconditionally to an opaque pointer
(`llvm::PointerType::get`) regardless of what it was eventually instantiated
with, and a scalar value was smuggled through that pointer-shaped slot by
reinterpreting its raw bits (`CreateIntToPtr`/`CreatePtrToInt` — a `double`
first bitcast to its 64-bit integer representation, then to a pointer
carrying that same bit pattern, never dereferenced as an address). That
erasure branch is deleted outright, not merely unused — `CastToType()` has
no "cast to/from generic parameter" case left; the only code that ever
touches a `TurfGenericParamType` post-Monomorphizer is the three assertions
above, confirming one never reached them.

#### 2.7.2 There is no pointer-sized "erased slot" any more

Because a generic struct's fields carry their real, substituted concrete
types after cloning, a `T`-typed field's storage is whatever `T` actually
is — a real `i64` for `int`, a real 8-byte string handle for `string`, a
real inline aggregate for a struct type, laid out exactly as it would be if
the struct had been hand-written for that one concrete type
(`checkStructLayoutAcyclicityForClone()`, run per clone before it is
registered, guarantees the layout is finite the same way §1.5/§2.5's
ordinary acyclicity check does for a non-generic struct — see
`docs/development/changes/TYPE-002.md`'s S13a entry). There is no
reinterpretation step at a `push`/`get` call boundary any more: `self.data[i]
= item;` where `item: T` is an ordinary, type-specific store into a
concretely-typed array, exactly like any other struct field assignment.

#### 2.7.3 A struct-typed `T` larger than one machine word works directly

Under the old erasure scheme, a struct value larger than 8 bytes could not
be losslessly round-tripped through a single erased pointer-sized slot at
all — an unconditional ICE. This is fixed as of `TYPE-002` (S13b/S14a/S14b,
`docs/development/changes/TYPE-002.md`'s own D3 entry): every stdlib
collection's growth arithmetic now computes its buffer size from a genuine
`sizeof(T)` (`self.cap * sizeof(T)` — `lib/std/collections/list.tr`,
mirrored in `stack.tr`/`queue.tr`/`set.tr`/`map.tr`/`mem.tr`'s `Pool<T>`,
each once), which reports the real, concrete size for any instantiation,
including a multi-field struct — verified live: `List<Big>` (a 24-byte,
three-`int`-field struct) compiles `push`/`pop` clones that take/return a
genuine `%Big` aggregate by value (`define internal %Big
@turf_List.pop_gen1_Big...(ptr %self)`), grows its backing buffer correctly
across repeated pushes, and reads every value back correctly
(`tests/38_monomorphization/12_list_growth_over_8_bytes.tr`,
`17_stack_growth_over_8_bytes.tr` through `20_...`).

The `List<MyStruct*>` pattern (instantiating with a *pointer* to the struct
instead of the struct itself) still compiles and is not being removed —
`lib/std/collections/list.tr`'s own header comment documents it as "still
fully supported" — but it is now a caller's ordinary choice about aliasing
and mutation-through-reference, the same choice a Turf programmer has for
any struct-valued collection, not a workaround required to avoid an ICE.
The one genuinely load-bearing reason a `List<T>`-shaped module can't
simply switch `data` from a raw `T*` to a safe `&T` (`checkNoRawPointersInSignature`
rejecting a signature substituted to a raw-pointer parameter when `T` is
itself instantiated as a pointer type) is unrelated to monomorphization and
is documented in that same comment.

### 2.8 Pointers and `&T`: representation only

`getLLVMType()`'s pointer branch (`CodegenCore.cpp:340-341`) maps every
`TurfPointerType` — which `TurfReferenceType` (`&T`) inherits from, per
`pointers_and_references.md` §2.1 — to the same bare opaque pointer as
`ptr`/`string` (an unsized array parameter is a `{ ptr, i64 }` slice since
issue #260, §2.3). There is no separate "reference"
representation anywhere in Codegen; `&T`'s entire contribution to the
compiled program is a compile-time-only non-null contract enforced earlier
in the pipeline (the TypeChecker's `unsafe` gate,
`pointers_and_references.md` §2.5). This document adds nothing to that
account beyond confirming it's consistent with everything else here: a
pointer or reference is never itself copied-with-its-pointee the way a
struct or array is (§2.3, §2.5) — assigning one pointer/reference variable
to another copies the *address*, and both variables alias the same
underlying memory afterward, exactly as in any manual-memory-management
language.

---

## Part 3 — File map

| File | Role in this document |
|---|---|
| `include/Types.h` | The type hierarchy: primitive classes (`IntType`/`Int32Type`/`DoubleType`/`BoolType`/`CharType`), `ArrayType`, `TurfPointerType`/`TurfReferenceType`, `TurfGenericParamType`/`GenericType`, `TurfStructType` |
| `src/Types.cpp` | `typesCompatible()` — the array-size/element-type compatibility rule (§2.3), the string/void/bool exclusions from numeric coercion |
| `src/CodegenCore.cpp` | `getLLVMType()` (§2.2, §2.3.1, §2.7.1, §2.8), `CastToType()`'s generic-erasure box/unbox branches (§2.7.2), `GetDefaultValue()` (§2.5.2), `HeapAllocateValue()` — `heap(expr)`'s allocation logic (§2.6.1) |
| `src/CodegenExpr.cpp` | Literal codegen for every primitive and `string` (§2.2, §2.4); the removed-`EmitStringConcat` comment |
| `src/CodegenArray.cpp` | `ArrayDeclExprAST`/`ArrayLiteralExprAST`/`ArrayAccessExprAST` codegen (§2.3.1, §2.3.3) |
| `src/CodegenVar.cpp` | `VarDeclExprAST::codegen()` — the general copy-on-assignment path shared by every type, plus the struct-construction-alloca-reuse optimization (§2.5.2, §2.5.3) |
| `src/CodegenFunc.cpp` | Parameter passing (§2.3.2, §2.5.3), `::new`/`::default` construction (§2.5.2), `heap(expr)` dispatch (§2.6.1) |
| `src/CodegenControlFlow.cpp` | `ReturnExprAST::codegen()` — the generic return-value path (§2.3.3, §2.5.3) |
| `src/CodegenStruct.cpp` | `StructDeclExprAST::codegen()` — inline field layout (§2.5.1); `EmitAllTurfTypeDescs()`/`getOrCreateElementTurfTypeDesc()` — struct-instance and per-element `TurfTypeDesc` emission (§2.6) |
| `runtime/turfrt_gc.c` | The collector itself — block table, mark, sweep, root discovery (§2.6). Full design in `docs/development/changes/MEM-002.md` |
| `runtime/turfrt.h` | The public `libturfrt` C ABI: `turf_rt_alloc`/`turf_rt_collect`/`turf_rt_add_global_root`/`turf_rt_is_gc_pointer`/`turf_rt_checked_free`/`turf_rt_pin`/`turf_rt_unpin`, `TurfTypeDesc` (§2.6, §1.7) |
| `lib/std/mem/mem.tr` | The C allocator FFI bindings and their checked Turf wrappers; `Pool<T>`; `free()`'s cross-heap `gc_checked_free` check; `root()`/`unroot()`'s `gc_pin`/`gc_unpin` pinning (§2.6, §1.7) |
| `lib/std/collections/list.tr` | The canonical `data: T*, len: int, cap: int` collection shape (§2.6, §2.7.1) |
| `lib/std/core/strings.tr` | String `operator` overloads and their GC-heap allocations (§2.4) |
| `docs/reference/arrays.md` | User-facing array semantics, verified against implementation in §2.3 |
| `docs/reference/stdlib/collections.md` | The shared collection design writeup, including the `List<T*>` >8-byte workaround (§2.7.3) |
| `docs/architecture/pointers_and_references.md` | The full `ptr`/`T*`/`&T`/`unsafe` reference this document cross-references rather than duplicates (§1.6, §2.8) |
| `docs/planning/FUTURE.md` | §5, the proposed (unimplemented) ARC design mentioned in §1.7 |
