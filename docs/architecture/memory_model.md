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

**The unsized spelling `T[]` is a parameter-only escape hatch, and it is
*not* a value type.** It exists so one function can accept an array
argument of any length (`fn sum(arr: int[]): int` called with either an
`int[5]` or an `int[10]`). Passing an array to a `T[]` parameter does **not**
copy it — the callee receives the actual address of the caller's
stack-allocated array (verified in §2.3.2). This is the one place a fixed
array's storage is genuinely shared rather than copied, and the compiler
performs **no escape analysis or lifetime checking** on it at all: nothing
stops you from casting that address to a `ptr`/`T*` inside the callee and
handing it further outward (returning it, storing it in a struct field,
etc.). If you do, and the value outlives the caller's own stack frame, you
have a dangling pointer — the exact same hazard as returning the address of
any other local variable in a language with manual memory management.
`T[]` cannot be used as a local variable type, a struct field type, or a
return type (`docs/reference/arrays.md` §"Unsized parameters") — precisely
because there's no way to give it a well-defined lifetime once it's
detached from "the argument I was just handed."

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
(verified in §2.5.2). There is no way to construct a struct directly on the
heap; if you want one there, you allocate raw bytes yourself
(`mem::alloc(sizeof-equivalent-byte-count)`, cast to `T*`) and manage it
exactly like any other manual allocation from that point on — the struct
*type* itself carries no opinion about where its bytes live.

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
- **Neither `T*` nor `&T` carries any lifetime information.** A `&T` being
  non-null tells you nothing about whether the memory it points to is still
  valid — it is entirely possible (and not gated by anything) to construct
  a `&T` that outlives the stack frame it pointed into, exactly as with a
  raw pointer in C. "Safe" here means "can't be null," not "can't dangle."

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
  record, *Known pitfalls*).
- **The GC heap.** `List<T>`, `Stack<T>`, `Queue<T>`, `Set<T>`, and
  `Map<K,V>` each grow a backing buffer internally via the
  `gc_alloc(n)`/`gc_realloc(p, n)` bare intrinsics (`include/Intrinsics.h`
  category 1, `lib/std/collections/*.tr`, and the shared growth policy
  documented in `docs/reference/stdlib/collections.md`); string
  concatenation and most `strings::*` functions allocate here too (§1.4).
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
registered global roots and for a GC-heap block's own declared pointer
fields (a compiler-emitted `TurfTypeDesc` bitmap), and conservative for the
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
a bare opaque pointer — there is no array-length metadata carried at
runtime for the unsized form at all.

#### 2.3.2 Passing an array: copy for `T[N]`, address for `T[]`

The distinction that produces §1.3's aliasing hazard is made explicit, with
its own comment, at every call site (`src/CodegenFunc.cpp:955-971`):

```cpp
// Unsized array params: the function expects a pointer to the array data.
// We must pass the raw AllocaInst of the caller's array, not a loaded
// value. TypeChecker has already verified the caller is passing an array
// variable with a compatible element type.
// Note: Fixed-size arrays (Size > 0) are passed by value, so V remains the
// evaluated argument (the loaded array value).
if (isArrayType(DeclaredType.get())) {
  const auto *ArrTy = dynamic_cast<const ::ArrayType *>(DeclaredType.get());
  if (ArrTy && ArrTy->getSize() == -1) {
    if (auto *VE = dynamic_cast<VariableExprAST *>(Args[Idx].get())) {
      V = VE->codegenLValue();   // the caller's own AllocaInst, not a copy
      ...
```

For a `T[N]` argument, `V` is left as whatever the argument expression's
ordinary `codegen()` already produced — for a plain array variable, that's
`VariableExprAST::codegen()`'s `CreateLoad` of the *entire* aggregate
(`src/CodegenVar.cpp`, the same load path every scalar variable read goes
through) — i.e. a genuine bit-for-bit copy of every element gets passed by
value. For a `T[]` argument, the special-cased branch above instead passes
the caller's own stack address directly, with **no copy anywhere in this
path** — which is exactly why a `T[]` parameter aliases the caller's frame
rather than owning independent storage, and why the TypeChecker enforces
that a `T[]` argument must be a plain variable (`assert(V && ...)`
above; a `T[]` argument that isn't literally a named variable has no
addressable storage for this branch to take the address of in the first
place).

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
(OVH-001, `docs/planning/OVERHAUL_REMAINING_WORK.md`) — `word_count`
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

### 2.7 Generics and type erasure

#### 2.7.1 Every type parameter erases to one 64-bit slot

`getLLVMType()`'s handling of an open generic parameter is unconditional and
has nothing to do with what the parameter is eventually instantiated with:

```cpp
// Type Erasure: Generic Type Parameters are compiled as void*
if (dynamic_cast<const TurfGenericParamType *>(T)) {
  return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);   // CodegenCore.cpp:352-354
}
```

A generic function/struct is compiled **once**, generically — there is no
per-instantiation monomorphization the way C++ templates or Rust generics
work. `List<int>` and `List<string>` are, at the machine level, the
*identical* compiled code and the *identical* struct layout; the only
difference between them is which concrete type the TypeChecker validated at
each call site before Codegen ever ran. This is precisely what `list.tr`'s
own comment states directly:

> // In Type Erasure, all T are 64 bits (pointers, int, double)
> — `lib/std/collections/list.tr:27`

An opaque pointer is one machine word — 8 bytes on the 64-bit targets this
compiler builds for — which is why `List<T>.push()` computes its buffer
size as `self.cap * 8` (`list.tr:28`) rather than anything derived from a
real `sizeof(T)`: there is no such thing as `sizeof(T)` here, because `T`'s
erased representation is always exactly one word, regardless of what `T`
actually is.

#### 2.7.2 How a scalar `T` fits into a pointer-sized slot

Storing an `int`/`double`/`bool` value into a generic-parameter-typed slot
(e.g. `self.data[self.len] = item;` in `List<T>.push`, where `item: T`) has
to reconcile a genuinely numeric value with a slot whose LLVM type is an
opaque pointer. `CastToType()`'s type-erasure branch does this by
**reinterpreting the value's raw bits as a pointer**, not by boxing it in a
separate heap allocation:

```cpp
// CodegenCore.cpp:1177-1187
if (dynamic_cast<const TurfGenericParamType *>(DestType)) {
  Type *VoidPtrTy = PointerType::get(*TheContext, 0);
  if (ValTy->isPointerTy())
    return Val; // already a pointer
  if (ValTy->isDoubleTy())
    Val = Builder->CreateBitCast(Val, Type::getInt64Ty(*TheContext), "dbl_to_i64");
  if (Val->getType()->isIntegerTy())
    return Builder->CreateIntToPtr(Val, VoidPtrTy, "erased_ptr");
  return Builder->CreateBitCast(Val, VoidPtrTy, "erased_ptr");
}
```

A `double` is first reinterpreted as a 64-bit integer bit pattern (not
converted numerically — the same 8 bytes, just relabeled), and any integer
width (`int`, `int32` after its own widening, `bool`, `char`) is then
turned into a pointer value carrying that same bit pattern as its address —
`CreateIntToPtr` performs no dereference, it's purely a type-level
relabeling at the LLVM level with zero runtime cost. Reading a value back
out of a generic-parameter slot (`CodegenCore.cpp:1208-1221`) reverses this
exactly: `CreatePtrToInt` extracts the pointer's bit pattern back into an
integer, then bitcasts to `double` if that's what the concrete
instantiation calls for. **No heap allocation, no wrapper object, and no
per-instantiation code exists anywhere in this mechanism** — a `List<int>`
storing the value `5` literally stores the pointer value `0x5` in its
backing array, never dereferenced as an address, always immediately
reinterpreted back into an `int` on read.

#### 2.7.3 Why this breaks down above 8 bytes, precisely

This mechanism only works because every scalar Turflang currently exposes
to generics — `int`, `int32` (widened), `double`, `bool`, any pointer type —
fits in 64 bits with room to spare. A **struct value larger than 8 bytes**
categorically cannot be reinterpreted this way: there is no single 64-bit
bit pattern that round-trips back into (for example) a 16-byte
two-`double`-field struct, and `CastToType()`'s generic-erasure branch above
has no case at all for a struct-typed source or destination — a struct
passed by value into a generic-parameter-typed slot simply isn't a shape
this mechanism was built to handle.

The practical consequence, stated in `docs/reference/stdlib/collections.md`
and consistent with every collection sharing the `list.tr` design (§2.6):
**a struct larger than 8 bytes can't practically be used as `T` in
`List<T>` (or `Stack<T>`/`Queue<T>`/`Map<K,V>`/`mem::Pool<T>`) directly.**
The documented workaround is to instantiate with a *pointer* to the struct
instead — `List<MyStruct*>` — since a pointer, whatever it points at, is
always exactly one word and fits the erasure scheme regardless of the
pointee's size. This is a real, currently-unfixed architectural gap (not a
deliberate language restriction with a principled justification) — closing
it would require either real per-instantiation monomorphization (a much
larger compiler change) or a redesigned erasure scheme that boxes
oversized values on the heap instead of inlining their bits, and no such
work is scheduled.

### 2.8 Pointers and `&T`: representation only

`getLLVMType()`'s pointer branch (`CodegenCore.cpp:340-341`) maps every
`TurfPointerType` — which `TurfReferenceType` (`&T`) inherits from, per
`pointers_and_references.md` §2.1 — to the same bare opaque pointer as
`ptr`/`string`/an unsized array parameter. There is no separate "reference"
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
| `src/CodegenCore.cpp` | `getLLVMType()` (§2.2, §2.3.1, §2.7.1, §2.8), `CastToType()`'s generic-erasure box/unbox branches (§2.7.2), `GetDefaultValue()` (§2.5.2) |
| `src/CodegenExpr.cpp` | Literal codegen for every primitive and `string` (§2.2, §2.4); the removed-`EmitStringConcat` comment |
| `src/CodegenArray.cpp` | `ArrayDeclExprAST`/`ArrayLiteralExprAST`/`ArrayAccessExprAST` codegen (§2.3.1, §2.3.3) |
| `src/CodegenVar.cpp` | `VarDeclExprAST::codegen()` — the general copy-on-assignment path shared by every type, plus the struct-construction-alloca-reuse optimization (§2.5.2, §2.5.3) |
| `src/CodegenFunc.cpp` | Parameter passing (§2.3.2, §2.5.3), `::new`/`::default` construction (§2.5.2) |
| `src/CodegenControlFlow.cpp` | `ReturnExprAST::codegen()` — the generic return-value path (§2.3.3, §2.5.3) |
| `src/CodegenStruct.cpp` | `StructDeclExprAST::codegen()` — inline field layout (§2.5.1); `EmitAllTurfTypeDescs()`/`getOrCreateElementTurfTypeDesc()` — struct-instance and per-element `TurfTypeDesc` emission (§2.6) |
| `runtime/turfrt_gc.c` | The collector itself — block table, mark, sweep, root discovery (§2.6). Full design in `docs/development/changes/MEM-002.md` |
| `runtime/turfrt.h` | The public `libturfrt` C ABI: `turf_rt_alloc`/`turf_rt_collect`/`turf_rt_add_global_root`/`turf_rt_is_gc_pointer`/`turf_rt_checked_free`, `TurfTypeDesc` (§2.6) |
| `lib/std/mem/mem.tr` | The C allocator FFI bindings and their checked Turf wrappers; `Pool<T>`; `free()`'s cross-heap `gc_checked_free` check (§2.6) |
| `lib/std/collections/list.tr` | The canonical `data: T*, len: int, cap: int` collection shape (§2.6, §2.7.1) |
| `lib/std/core/strings.tr` | String `operator` overloads and their GC-heap allocations (§2.4) |
| `docs/reference/arrays.md` | User-facing array semantics, verified against implementation in §2.3 |
| `docs/reference/stdlib/collections.md` | The shared collection design writeup, including the `List<T*>` >8-byte workaround (§2.7.3) |
| `docs/architecture/pointers_and_references.md` | The full `ptr`/`T*`/`&T`/`unsafe` reference this document cross-references rather than duplicates (§1.6, §2.8) |
| `docs/planning/FUTURE.md` | §5, the proposed (unimplemented) ARC design mentioned in §1.7 |
