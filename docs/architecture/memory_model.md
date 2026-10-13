# The Memory Model: Storage, Lifetime, and Ownership

Internal reference for what happens to memory in a compiled Turflang program:
which values live on the stack, which live on the heap, which are baked into the
binary as static read-only data, what is copied and what is aliased, and what
manages any of it. Like the other docs in this directory it covers both **what a
Turflang programmer observes** (Part 1) and **how the compiler and runtime
implement it** (Part 2).

It explains things in ordinary computer-science terms (stack frames, heap
allocations, static data, value versus reference semantics), not LLVM
instruction names. Nothing here depends on knowing LLVM IR.

Related docs:
- `docs/architecture/pointers_and_references.md`: the full reference for `ptr`,
  `T*`, `&T`, and `unsafe { }`. This document assumes its Part 1 and
  cross-references it.
- `docs/architecture/modules_and_scope_resolution.md`: `::new`/`::default`
  construction, which the struct section depends on.
- `docs/architecture/runtime.md`: `libturfrt` as a library (entry points, type
  descriptors, build, the fault and OOM contract). This document covers the
  collector's design and root discovery.
- `docs/architecture/boundary.md`: which heap a standard-library allocation may
  use, and the one disclosed gap (`mem::Pool<T>`).
- `docs/architecture/compiler_pipeline.md` §7: the monomorphizer.
- `docs/reference/arrays.md`: the user-facing array reference.
- `docs/planning/FUTURE.md` §5: a proposed automatic-reference-counting design.
  It is not implemented and is not discussed further here.

---

## Part 1: The memory model as observed

### 1.1 The short version

| Type | Where it lives | Copied or aliased? | Who manages its lifetime |
|---|---|---|---|
| `int`, `int32`, `double`, `bool`, `char` | Stack (or a register) | Copied (value semantics) | Automatic: gone when its stack frame pops |
| `string` **literal** | Static, read-only, baked into the binary | N/A: only the pointer is copied | The program's whole lifetime; never freed |
| `string` from `+`, `to_upper`, `int to string`, etc. | **GC heap** | Aliased (a `string` is a pointer) | **The collector**: reclaimed once nothing reachable points to it |
| Fixed-size array `T[N]` | Stack | Copied (value semantics) | Automatic |
| Slice `T[]` (parameter or local view) | The slice `{ address, length }` is on the stack; the elements are the original array's | Aliased | The array it views |
| Struct (local, or via `::new`/`::default`) | Stack, at the point of construction | Copied on assignment/pass/return | Automatic, *if* it never leaves the stack (§1.5) |
| `ptr` / `T*` (raw pointer) | Wherever it was given, usually the manual heap via `mem::alloc` | Aliased | **You**: `mem::free`, or nothing |
| `&T` (safe reference) | Same representation as `T*` | Aliased | Same as `T*`; the difference is a compile-time non-null guarantee, not a lifetime guarantee (§1.6) |
| `List<T>`/`Stack<T>`/`Queue<T>`/`Set<T>`/`Map<K,V>`: the value is a one-pointer handle; its header and backing buffer(s) | Handle: wherever the variable is. Header and buffers: **GC heap** | Aliased: `=`/pass/return share one collection, `.copy()` is independent (F-COLL-1) | **The collector** |
| `mem::Pool<T>`'s backing buffer | Manual heap | Aliased, grown via `realloc` | **You**, via `mem::free_pool()`; deliberately not collected (§1.7) |

The one-sentence version of the safety story: **there are two disjoint heaps,
and only one of them is automatically managed** (F-MEM-3).

- The **manual heap** (`mem::alloc`/`malloc` and everything built on them,
  including `mem::Pool<T>`) has no automatic management of any kind: no
  collector, no reference counting, no borrow checker.
- The **GC heap** (every `string` result, every `List`/`Stack`/`Queue`/`Set`/
  `Map` header and buffer, every `heap(expr)` value) is reclaimed by a
  non-moving mark-sweep collector, `turf_rt_collect()` in `libturfrt`. It runs
  automatically on allocation pressure, or on demand through the `gc_collect()`
  bare intrinsic.

`&T` (§1.6) buys a compile-time non-null guarantee, not a lifetime guarantee:
it does not prevent a dangling reference into the manual heap. A `&T` into the
GC heap is protected by the collector like any other GC pointer, as long as it
(or something that reaches it) is itself reachable. The two heaps never merge:
`mem::free()` on a GC-heap pointer is a diagnosable runtime panic, not undefined
behaviour (§1.4, §1.7).

### 1.2 Primitives: `int`, `int32`, `double`, `bool`, `char`

All five are stored directly in a variable's own stack slot (or promoted to a
register by the optimizer) and have ordinary value semantics: assigning one
variable to another copies the bits, and two variables never alias one `int`.

- `int` is a 64-bit signed integer, the only general-purpose integer type.
- `int32` is a genuinely 32-bit signed integer. It exists so an `extern fn`
  binding can describe a C function whose real ABI is a 32-bit `int` (`strcmp`,
  `toupper`, `tolower`); declaring those as the 64-bit `int` reads garbage out
  of the upper half of the return register. Arithmetic on an `int32` promotes to
  `int` automatically.
- `double` is a 64-bit IEEE-754 floating-point number.
- `bool` is a genuine 1-bit boolean, kept out of every implicit numeric-coercion
  path (`typesCompatible()`, `src/sema/Types.cpp`), so it never silently widens
  to or from `int`/`double`.
- `char` is a single byte, **not** a Unicode code point and **not numeric**:
  comparisons work, but `'a' + 1` is rejected (E057, "You used an operator on a
  kind of value it doesn't work for"); cast to `int` first. `CharType` does not
  override `isNumeric()` (`src/sema/Types.h`), which keeps it out of every
  arithmetic and widening rule without exclusion logic anywhere else.

### 1.3 Fixed-size arrays: `T[N]`

A declared `int[5]` is a stack-allocated block of 5 contiguous `int`s, not a
pointer and not a heap allocation. **Arrays are value types**
(`docs/reference/arrays.md`; implementation in §2.3): assigning one array to
another copies every element, and passing one to a function or returning one
from a function is a full independent copy.

A declaration with no initializer zero-fills the storage (and gives every
`string` element `""`, not a null pointer). Reading such an array before
assigning to it is still a compile-time error (E008, "You're reading a variable
that might not have a value yet"): the zero fill is a runtime guarantee, not a
static one.

**The unsized spelling `T[]` is a slice, and it is not a value type.** It lets
one function accept an array of any length (`fn sum(arr: int[]): int` called
with an `int[5]` or an `int[10]`). Passing an array to a `T[]` parameter does
**not** copy it: the callee receives the address of the caller's array plus its
element count (§2.3.2). This is the one place a fixed array's storage is shared,
so its lifetime is bounded by construction: `T[]` may only be a parameter's own
type or a local variable's (a view, from a range slice `arr[a..b]`, or an array
or another slice; a literal does not make a view, it sizes the declaration,
§2.3.1), and a local lives no longer than its function. Everywhere that could
outlive the frame is E140 (field, return type, element type, generic argument,
function type, behind `&`/`*`; no cast produces one), and `arr to ptr` on one is
rejected (E134). A write through a slice changes the original array, so passing
an `immut` array (or a slice of one) to a parameter the callee writes through is
E141, and a `mut` view of an `immut` array is E144. A slice into a `heap()`
block's array field holds an interior pointer, which the collector's
interior-pointer retention already keeps alive
(`tests/41_gc/03_interior_pointer_retention.tr`,
`tests/41_gc/12_slice_into_heap_struct_survives_collection.tr`).

### 1.4 `string`

A `string` is, at the machine level, a raw pointer to a sequence of bytes, the
same representation as `ptr`/`T*` (§2.4). What differs is *how a given string
value came to exist*.

**A string literal** (`"hello"`) is static, read-only data in the binary, present
for the whole life of the program. It is never allocated and never freed.
Nothing in the type system stops indexed assignment through a string-literal
pointer (`s[0] = 'j'` compiles, since element writes are permitted on any
`string`-typed lvalue), but it targets read-only memory and is undefined
behaviour, not a supported operation.

**A string produced by concatenation (`a + b`), by a numeric-to-string
conversion (`n to string`), or by almost any standard-library string function**
(`to_upper`, `to_lower`, `substring`, `trim`, `replace`, `repeat`, `pad_left`,
`pad_right`, `StringBuilder.to_string`, ...) is a **fresh GC-heap allocation**,
sized exactly for its contents. Every buffer-returning function in
`lib/std/core/strings.tr` calls the bare `gc_alloc(n)` intrinsic
(`src/sema/Intrinsics.h`, category 1) directly or through `+`/`substring`, and
that lowers to `turf_rt_alloc()` (`runtime/turfrt.h`), never `malloc`.
`io::input()`, `io::read_to_string()` and `net::TcpConnection.read()` use the
zero-filling `gc_alloc_zeroed(n)`, because they rely on the byte past what was
read already being the NUL terminator.

None of these functions frees what it returns or what it was given. The
collector reclaims each buffer once nothing reachable points to it, so
`result = result + s;` in a loop (as `strings::repeat` and `strings::replace` do
internally) allocates a fresh buffer per iteration and the previous iteration's
buffer is collected the next time allocation pressure triggers a cycle.
`tests/13_string_stdlib/17_stress_10k_concat.tr` and
`tests/08_e2e_stress/06_gc_bounded_allocation.tr` (peak RSS, measured by
`scripts/smoke/gc_rss_smoke.sh`) cover it.

A `string` is pointer-identical to `ptr`/`void*` (`isPointerLike()`, §2.4), and
that identity is **not** license to treat it like a manual-heap pointer.
`mem::free()` checks every pointer against the collector's block table
(`gc_checked_free`, which becomes `turf_rt_checked_free`) and panics with a named
diagnostic (`cross-heap free`) if it is the start of a live GC block, instead of
corrupting either heap's bookkeeping (`tests/41_gc/fail_free_gc_pointer.tr`,
INV-MEM-1).

### 1.5 Structs

Struct values have ordinary **value semantics**, the same story as fixed arrays:

- **Assignment copies.** `mut b: Point = a;` copies every field into `b`'s own
  stack storage; mutating `b` never touches `a` (§2.5.3).
- **Passing a struct to a function copies it.** There is no implicit
  pass-by-reference for struct-typed parameters (contrast `self`, §1.5.1).
- **Returning a struct by value copies it out.** Nothing is left aliased to the
  callee's popped frame.
- **A struct field that is itself a struct is embedded inline**, not stored as a
  pointer to a separate sub-object. `struct Line { a: Point, b: Point }` is one
  contiguous block containing both `Point`s (§2.5.1).

**Where does a struct live?** Wherever it was declared. An ordinary local
(`mut p: Point = Point::new(1, 2);`) is on the stack, including one built by
`Point::new(...)`/`Point::default()`, which allocate their temporary on the
stack and return a copy (§2.5.2). The struct *type* carries no opinion about
where its bytes live. Two other placements exist: `mem::alloc(byteCount)` cast
to `T*` (the **manual** heap, your bookkeeping from then on, §1.7), and
`heap(structValue): &T` (the **GC** heap, §1.7, §2.6.1), which is the only safe
way to get a `&T` to a heap-resident value, since the other source of a `&T`,
`&localVar`, points into a stack frame.

#### 1.5.1 `self` is the one exception to "structs are copied"

A struct method's implicit `self` parameter is a raw pointer (`StructName*`) to
the *original* struct, not a copy: `self.field = value;` mutates the caller's
struct. See `pointers_and_references.md` §2.5.3 for why `self` stays a raw
pointer internally.

### 1.6 Pointers (`ptr`, `T*`) and safe references (`&T`)

Covered in full in `docs/architecture/pointers_and_references.md`. The
memory-model-relevant facts:

- `ptr` (opaque) and `T*` (typed) are raw, unmanaged pointers: whatever address
  they hold, the compiler trusts you. They point at whatever gave rise to them,
  a `mem::alloc` result, an address-of expression, or a cast from an integer.
- `&T` is a **compiler-checked non-null guarantee over the same representation
  as `T*`**. At the machine level they are indistinguishable. `&expr` can never
  produce `null`, and a `&T` widens to `T*` for free but never the reverse
  without an `unsafe`-gated cast.
- Neither carries lifetime information as a *type*. Separately, `EscapePass`
  (`src/analysis/Analysis.h`; F-MEM-5, INV-MEM-6) rejects, at compile time, a
  `&T` provably pointing into the current function's own stack frame that is
  returned, stored into a longer-lived object, pushed into a collection, or
  boxed with `heap()` (E086). It is intraprocedural and conservative (never a
  false positive on safe code), and it is not a general lifetime system: a
  reference smuggled through an intermediate call, or via raw pointer
  arithmetic, is still uncaught. `pointers_and_references.md` §1.9 has the full
  rule and its residual gap. "Safe" means "can't be null and can't, in the cases
  above, provably dangle", never "carries a lifetime".

### 1.7 What manages any of this? Two heaps, one of them automatic.

- **The manual heap.** `mem::alloc`/`mem::alloc_zeroed`/`mem::grow`/`mem::dup`/
  `mem::alloc_aligned` (`lib/std/mem/mem.tr`) are fail-fast wrappers around
  `malloc`/`calloc`/`realloc`/`posix_memalign`. You own what they return, as in
  C: no collector, no reference counting, no ownership checker, no destructor on
  scope exit. `mem::Pool<T>` grows its own `malloc`/`realloc`-backed array the
  same way; it exists to make *manual* bulk-freeing less tedious (track every
  allocation, `mem::free_pool()` them all), so its bookkeeping stays on the
  manual heap beside what it tracks.

  **The collector never traces into this heap** (F-MEM-3): a pointer that lives
  only inside manually managed memory is not a root, even if it points at a
  GC-heap object. This is a deliberate, disclosed unsoundness
  (`boundary.md` §5 states the rule that standard-library code must not do it,
  and §6 the one known gap, `mem::Pool<T>`). The escape hatch is explicit:
  `mem::root(p)` / `mem::unroot(p)` (`gc_pin`/`gc_unpin`, `turf_rt_pin`/
  `turf_rt_unpin`) mark the GC object *containing* `p` (an interior pointer is
  fine) as an always-retained root until a matching `unroot`. Pins nest (a
  per-object count), a `p` that is not a GC pointer is a no-op, and `unroot` on
  an object with no outstanding pin panics. Nothing pins anything
  automatically; the collector still has no block-table entry for manual memory.
- **The GC heap.** `List<T>`, `Stack<T>`, `Queue<T>`, `Set<T>` and `Map<K,V>`
  each allocate a header and grow their backing buffer(s) through the
  `gc_alloc(n)`/`gc_realloc(p, n)` bare intrinsics (`lib/std/collections/*.tr`);
  string results allocate here too (§1.4). `heap(expr): &T` copies `expr`'s value
  (a struct, a fixed array, or a primitive) into a fresh `turf_rt_alloc()` block
  and returns its address as a `&T`; it is what makes a dynamically sized linked
  structure (list, tree, graph) writable in safe Turf. The compiler itself
  allocates here for a `!T` success value too wide for its 64-bit slot, for a
  constructed error value and its payload, and for a `&U` field's default
  (§2.6.1). A non-moving mark-sweep collector reclaims every one of these once
  nothing reachable points to it. There are no destructors and no finalizers
  (F-MEM-2); `defer` is Turf's answer for deterministic OS-resource cleanup.

Root discovery (§2.6.3) is precise for registered global roots, pinned objects
and a GC block's own declared pointer fields (a compiler-emitted `TurfTypeDesc`
bitmap), and conservative for the stack and callee-saved registers, the
standard Boehm-style technique, which needs no Codegen cooperation to find
roots. Conservative scanning can over-retain (a stale word that merely looks
like a pointer keeps a block alive) but cannot free something genuinely
reachable.

---

## Part 2: How the compiler and runtime implement this

### 2.1 Pipeline recap

```
source.tr
   │
   ▼
 Lexer ─▶ Parser ─▶ Resolver ─▶ TypeChecker ─▶ Monomorphizer ─▶ Codegen (LLVM IR) ─▶ link with libturfrt
```

Everything in this document is a **Codegen**-layer or runtime story. The
Resolver and TypeChecker decide what is legal; the decision "where do this
value's bits live, and are they copied or aliased" is made in
`src/codegen/Codegen*.cpp` by which LLVM construct each AST node lowers to. The
type hierarchy behind it is `src/sema/Types.h`. The monomorphizer (§2.7) runs
before Codegen, so Codegen only ever sees concrete types.

### 2.2 Primitives

`getLLVMType()` (`src/codegen/CodegenCore.cpp`) maps each primitive `TurfType` to
a fixed-width machine type with no indirection:

| Turf type | LLVM type |
|---|---|
| `int` | `i64` |
| `int32` | `i32` |
| `double` | `double` |
| `bool` | `i1` |
| `char` | `i8` |
| `string`, `ptr`, `T*`, `&T`, function values | opaque `ptr` |
| enums | `i64` |

A local of any of these gets a stack slot made by `CreateEntryBlockAlloca`,
always placed at the top of the function's entry block wherever the declaration
appears in the source, so a loop or recursion reuses one slot per frame instead
of growing the stack. `NumberExprAST`, `BoolExprAST` and `CharExprAST`
(`src/codegen/CodegenExpr.cpp`) each yield an immediate constant, so reading a
literal touches no memory until it is stored.

### 2.3 Fixed-size and unsized arrays

#### 2.3.1 Declaration: a real stack allocation, zero-filled

`ArrayDeclExprAST::codegen()` (`src/codegen/CodegenArray.cpp`) creates an LLVM
array type of the declared element count and allocates it on the stack in the
entry block, then zero-fills the whole block with a `memset` before any
initializer is applied. When an element is left unwritten and the element type
holds a `string` (`typeHoldsString()`), a loop then stores the element type's
default value (`EmitDefaultValue()`, `""` for a string) into every element, so
a never-written `string` element reads as an empty string, not null. An
initializer (`InitExpr`, or the `InitList` stores) is applied last.

`getLLVMType()`'s array branch mirrors the split the type carries: `Size > 0` (a
concrete `T[N]`) is a real fixed-length array type; the unsized `T[]` is the
slice `{ ptr, i64 }` (`getSliceLLVMType()`), the first element's address and the
element count. For-each, `arr.length()` and the bounds check on `arr[i]` read
the count from the second field. A **local** `T[]` is a `{ ptr, i64 }` slot too,
built from its initializer (`CodegenStoredView()`). An array literal
initializer does not take this path: the Parser
(`src/frontend/ParserStmt.cpp`) gives an unsized declaration with a literal
initializer `ArraySize` equal to the literal's length, so
`mut nums: int[] = [10, 20, 30];` is simply `int[3]`, an owned stack array (only
the outermost dimension is inferred; `int[][2]` becomes `int[2][2]`). An empty
literal has no length to give (E123). Only an array or slice initializer yields
a `{ ptr, i64 }` view. When a view's source is a temporary (a call result, say),
`CodegenSliceArgument()` allocates the temporary with `HeapAllocateValue()` while
a variable keeps the view, so the storage outlives the statement and the
collector traces its elements.

**`arr.copy()`** is the way to get an independent `T[]`. In
`MethodCallExprAST::codegen()` (`src/codegen/CodegenStruct.cpp`) the receiver
becomes `{ data, len }` through `CodegenSliceArgument()`, then
`len * elemSize` bytes are allocated with `turf_rt_alloc`, passing
`getOrCreateElementTurfTypeDesc(elem)` so the collector traces the elements
precisely (a `string[]` copy keeps its strings alive,
`tests/41_gc/13_array_copy_elements_survive_collection.tr`), and the elements
are `memcpy`'d. The result is the slice `{ block, len }`; the only reference to
the block is that slice on the stack, which the conservative stack scan finds.
It is a fresh block, not a view of the receiver, so it is never an E144/E141
concern and an `immut` receiver is fine.

#### 2.3.2 Passing an array: copy for `T[N]`, slice for `T[]`

For a `T[N]` parameter the argument is whatever the argument expression's
`codegen()` produces. For a plain array variable that is
`VariableExprAST::codegen()`'s `CreateLoad` of the entire aggregate
(`src/codegen/CodegenVar.cpp`, the same path every scalar read takes), so a
bit-for-bit copy of every element is passed by value.

For a `T[]` parameter, every call form (ordinary, UFCS receiver and arguments,
method, static method, constructor, module function) evaluates that argument
with `CodegenSliceArgument()` (`src/codegen/CodegenArray.cpp`) instead, which
builds `{ address, N }`:

- an argument with storage of its own (a variable, a struct field, a row of a 2D
  array) contributes its own address, with **no copy**, which is why a `T[]`
  parameter aliases the caller's array;
- an argument without (a literal, a call returning `T[N]`) is first stored in a
  temporary in the caller's frame, which outlives the call;
- an argument that is already a `T[]` (a parameter handed on) is passed as the
  slice it is.

A range slice `arr[a..b]` is built by `SliceExprAST::codegen()`
(`src/codegen/CodegenArray.cpp`). It takes the base's `{ data, length }` from
`CodegenSliceArgument()` (so a `T[N]` is viewed in place and a `T[]` passes
through), evaluates each present bound once and adds the length to a negative
one, then checks `0 <= start <= end <= length`. A failing check calls
`CodegenPanic()` with `Slice range out of bounds for 'NAME'`; the TypeChecker
has already turned the provable cases into E143. The result is
`{ data + start, end - start }`, so the view aliases the original array exactly
as a `T[]` parameter does.

#### 2.3.3 Assignment and return: whole-aggregate copy

`ArrayLiteralExprAST::codegen()` builds a literal by inserting each evaluated
element into an initially undefined aggregate value (`CreateInsertValue`); that
in-register aggregate is stored into a real alloca by the same
`VarDeclExprAST::codegen()` path any initializer uses (§2.5.3). Reading an
array-typed variable, or returning one (`ReturnExprAST::codegen()`,
`src/codegen/CodegenControlFlow.cpp`, which has no array-specific branch), loads
the entire array as one aggregate value. That is what makes
`fn make3(): int[3]` (`tests/05_arrays_pointers_ffi/11_array_return_and_assign.tr`)
a safe independent copy rather than a pointer into the popped frame: a
fixed-size array return never produces a raw address, only a value.

### 2.4 Strings

`StringExprAST::codegen()` (`src/codegen/CodegenExpr.cpp`) is
`Builder->CreateGlobalString(Val, "str")`: the literal's bytes become static
data and the result is a pointer to them. No allocation instruction runs, which
matches §1.4. `getLLVMType()` maps `string` to a bare opaque pointer, identical
to `ptr`: there is no length, capacity or reference count in a string's
representation. It is a NUL-terminated `char*`, and every standard-library
function that builds one leaves room for and writes the terminator.

`isPointerLike()` (`src/codegen/CodegenCore.cpp`, file-local, used throughout
`CastToType()`) groups `string` with `ptr`/`T*`/`void*`/function values as one
LLVM representation. That is why `gc_alloc(n)` (returning `void*`) can be cast
`to string` with no representation change, and why a GC string's bytes are
physically indistinguishable from a manual-heap `void*`'s; §1.4 explains why
that does not make them interchangeable.

`+`, `==`, `!=`, `<`, `>`, `<=`, `>=` on two strings are ordinary `operator`
overloads in Turf (`lib/std/core/strings.tr`), resolved by the same machinery as
a user-defined operator. (`src/codegen/CodegenExpr.cpp` keeps a comment where
the old hand-written `EmitStringConcat`/`EmitStrcmp` helpers lived.) `operator +`
does the allocation:

```turf
operator +(a: string, b: string): string {
    mut n: int = strlen(a) + strlen(b) + 1;
    mut buf: string = gc_alloc(n) to string;
    strcpy(buf, a);
    strcat(buf, b);
    return buf;
}
```

`gc_alloc(n)` is sized exactly for the result plus the NUL; there is no capacity
headroom (`strings::StringBuilder` is the growable form and uses
`gc_alloc`/`gc_realloc` with doubling growth). The intrinsic lowers to
`turf_rt_alloc(n, NULL)`: a null descriptor means "opaque, no outgoing pointers",
which is correct for a byte run (§2.6.2). The other buffer-returning functions
(`to_upper`, `to_lower`, `substring`, `pad_left`, `pad_right`,
`StringBuilder.to_string`) follow the same shape, and `repeat`, `replace` and
`join` build on `+`. `io::input()`/`io::read_to_string()` (`lib/std/core/io.tr`)
and `net::TcpConnection.read()` (`lib/std/io/net.tr`) use `gc_alloc_zeroed(n)`.
`n to string` for a number is built in `CastExprAST::codegen()` with a 32-byte
`turf_rt_alloc(32, NULL)` block, so the result survives its function's return.

### 2.5 Structs

#### 2.5.1 Layout: fields embedded inline, not pointer-chased

`StructDeclExprAST::codegen()` (`src/codegen/CodegenStruct.cpp`) builds an LLVM
aggregate type and calls `setBody()` with each field's own type. A field whose
declared type is itself a struct gets that struct's actual LLVM type embedded in
place. Construction is two-step (create an opaque named type, then `setBody()`)
so a self-referential field such as `next: Node*` can name the type before it is
complete. That is also why a recursive field must be a raw *pointer* or a
reference, never an embedded field of the struct's own type, which would be
infinitely sized. A monomorphized generic instantiation (§2.7) is a
`StructDeclExprAST` clone appended to the pre-pass list, so it can be generated
after a struct that embeds it by value; this is the ordering that makes type
descriptor emission a separate late pass (`runtime.md` §4.1).

`List<T>`/`Pool<T>`-shaped structs hold a raw `T*` so element storage lives on
the heap and grows independently of the struct's small fixed header (§2.6.2).

#### 2.5.2 Construction: always a stack allocation

`StructName::default()` and `StructName::new(...)` both allocate their
result/`self` storage with `TmpB.CreateAlloca` in the entry block, never on a
heap:

- `::default()` allocates `<Name>_default`, stores `EmitDefaultValue()` into it
  and loads it back. `EmitDefaultValue()` (`src/codegen/CodegenCore.cpp`)
  recurses through fields and fixed-array elements, and gives every `&U` field a
  fresh `heap(U::default())`, which is why an empty `List` already owns a header
  (F-COLL-2). A `&` chain that reaches itself is a compile-time error rather
  than endless allocation. `GetDefaultValue()` is the constant-folded fallback
  for types with no `&` fields (a `string` field defaults to `""`).
- `::new(args)` allocates `<Name>_new`, passes its address as the implicit
  `self` to the constructor, then loads it.

Both end by loading the alloca back into a plain value; that load-then-return is
what makes `mut p: Point = Point::new(1, 2);` an independent copy rather than an
alias of the constructor's temporary. `VarDeclExprAST::codegen()`
(`src/codegen/CodegenVar.cpp`) detects exactly this "load from a just-built
construction alloca" shape and renames and reuses that alloca for the variable
instead of allocating and copying again. It is more than an allocation-count
optimization: a second alloca would let LLVM's SROA pass promote each alloca
independently and lose the alias between the variable and the pointer passed to
`self` inside a method.

#### 2.5.3 Assignment, parameter passing, and return: whole-aggregate copy

`VarDeclExprAST::codegen()`'s ordinary path applies to every type: the
initializer is evaluated to a plain value (for a struct-typed source, a full
`CreateLoad` of the aggregate), converted with `CastToType()`, and stored whole
into a new entry-block alloca. Nothing in that path makes a struct assignment
share storage, so the aggregate-load/aggregate-store shape that gives arrays
value semantics (§2.3.3) gives structs value semantics with no per-type logic.
Parameter passing (`FuncDefExprAST::codegen()`, `src/codegen/CodegenFunc.cpp`)
and `return` (`ReturnExprAST::codegen()`) are the same generic paths for structs
as for `int`s and `int[5]`s.

`self` is the exception and never goes through this value path: the implicit
`self` parameter is created as a raw `TurfPointerType(NamedType(StructName))` at
parse time (`ParseFuncDef()`, `src/frontend/ParserDecl.cpp`; cross-referenced in
`pointers_and_references.md` §2.5.3), so a method call always passes the
receiver's *address*, which is what makes `self.field = value;` mutate the
caller's struct.

### 2.6 Heap allocation: two heaps, one of them collected

The **manual heap** is `mem::alloc` and friends over `extern fn`-declared
`malloc`/`calloc`/`realloc`/`posix_memalign` (`lib/std/mem/mem.tr`; the
declarations are `pub extern fn` carrying an `@ownership(acquires)` annotation
the double-free analysis reads). `mem::alloc`, `alloc_zeroed`, `grow`, `dup` and
`alloc_aligned` `sys::panic` on a null result and add no bookkeeping. `mem::Pool<T>`
stays entirely on this heap, including its `items: T*` array, so it can be freed
the ordinary way and is never traced. `mem::free()` is an ordinary Turf function
that calls the `gc_checked_free(p)` bare intrinsic: Turf's `extern fn` has no
alias syntax, so a checked wrapper cannot share the name `free` with a raw
extern in the same file. `turf_rt_checked_free()` frees `p` on the manual heap
or panics if `p` is a live GC block's start address.

The **GC heap** is reached through the bare intrinsics `gc_alloc`,
`gc_alloc_zeroed`, `gc_realloc`, `gc_collect`, `gc_is_managed`, `gc_checked_free`,
`gc_pin`, `gc_unpin` and `heap` (`src/sema/Intrinsics.h`, category 1), dispatched
in `FuncCallExprAST::codegen()` (`src/codegen/CodegenFunc.cpp`) to
`turf_rt_alloc`, `turf_rt_alloc_zeroed`, `turf_rt_realloc`, `turf_rt_collect`,
`turf_rt_is_gc_pointer`, `turf_rt_checked_free`, `turf_rt_pin`/`turf_rt_unpin`
(`runtime/turfrt.h`). They are the standard library's allocation surface, not
documented user API. `gc_alloc`'s byte count must be an `int`
(`tests/41_gc/fail_gc_alloc_non_int_size.tr`).

#### 2.6.1 `heap(expr)`: a user value, GC-heap-allocated with a real descriptor

`heap(e: T): &T` is a bare intrinsic with no grammar of its own. The allocation
is `HeapAllocateValue()` (`src/codegen/CodegenCore.cpp`, declared in
`CodegenInternal.h`): given `e`'s evaluated LLVM value,

1. compute the size from the value's LLVM type (`DataLayout::getTypeAllocSize()`);
2. pick the real descriptor: `getOrForwardDeclareTurfTypeDescGlobal()` (the same
   whole-instance `TurfTypeDesc` `EmitAllTurfTypeDescs()` emits for that struct)
   if `T` is a named struct, otherwise `getOrCreateElementTurfTypeDesc()`'s
   per-LLVM-type descriptor (a fixed array or a primitive);
3. call `turf_rt_alloc(size, desc)`, store `e` whole into the block, return the
   pointer.

`BoxToI64Slot()` (`!T` success values too wide for the 64-bit slot) and
`CodegenErrorVariantConstruct()` (error values and payloads) share this
size-and-descriptor choice, and `EmitDefaultValue()` calls `HeapAllocateValue()`
for each `&U` field. The descriptor is never null here: the operand always has a
static type, so the collector traces a `heap()` value's own pointer fields
exactly as it would the same value on the stack (INV-MEM-5, INV-MEM-3).
`tests/39_nullable_references/05_dynamic_linked_list.tr`, `06_dynamic_binary_tree.tr`
and `07_graph_adjacency.tr` cover correctness across a forced collection;
`tests/08_e2e_stress/07_heap_bounded_allocation.tr` with
`scripts/smoke/gc_rss_smoke.sh` covers reclamation.

#### 2.6.2 The collector

`runtime/turfrt_gc.c` is a stop-the-world, non-moving, single-threaded
mark-sweep collector with no finalizers (F-MEM-1, F-MEM-2): precise on the heap
(per-type and per-element pointer bitmaps), conservative on the stack and
callee-saved registers.

**Collection shapes.** Every collection in `lib/std/collections/` has the same
storage shape: the value is one pointer, `h`, to a header on the GC heap that
holds the buffer pointer(s) and counters.

```turf
struct ListHeader<T> {
  data: T*,
  len: int,
  cap: int
}

pub struct List<T> {
  h: &ListHeader<T>
}
```

`List::default()` allocates the header (a `&U` field's default is
`heap(U::default())`, F-COLL-2). `data` is a GC pointer grown with
`gc_alloc`/`gc_realloc` and byte size `cap * sizeof(T)` (`List.push`: doubling
growth from capacity 8). `Stack`, `Queue`, `Set` and `Map` follow the pattern:
`Map` keeps `keys` and `values` arrays plus an `int*` hash index (`buckets`);
`Set` an `items` array plus the same index. Copying a collection value copies
one pointer, so both copies name one collection (F-COLL-1); `.copy()` builds an
independent one.

**The block table.** Every live GC block is a `GcBlock { start, size, desc,
marked, dead }`. New blocks are appended in O(1) to an unsorted `g_pending`
array indexed by a hash table (`g_pidx`, start address to position), so
`turf_rt_realloc` finds the block it moves without scanning. At the top of each
collection `gc_rebuild_index()` drops `dead` entries (a realloc that moved a
block out of the sorted array marks its entry dead instead of shifting the
array), merges `g_pending` into the sorted `g_blocks` and re-sorts. Exact-start
lookups (`turf_rt_is_gc_pointer`, `turf_rt_checked_free`) binary-search
`g_blocks` after a hash probe of `g_pending`, without forcing a rebuild. The
*containing-block* query the mark phase needs (`gc_find_containing()`: the last
block whose start is `<=` the address, then a bounds check) runs only against
the freshly rebuilt array; pin and unpin additionally scan `g_pending`
(`gc_find_containing_any()`).

**Allocation and the trigger.** Each of `turf_rt_alloc`/`turf_rt_alloc_zeroed`/
`turf_rt_realloc` obtains the memory, **registers the block first**, then calls
`turf_gc_note_alloc()`. The collection that may run there therefore finds the
about-to-be-returned pointer in a register or stack slot of the allocator's
caller and retains it. The threshold is heap-doubling: collect when bytes
requested since the last collection reach the threshold, which is twice the live
bytes after the last sweep with a 1 MiB floor (and 1 MiB initially).
`gc_collect()` forces a cycle.

**Mark.** `gc_mark()` rebuilds the index, clears the mark bits, captures the
callee-saved registers (§2.6.3), then considers as roots: those registers, every
word of the stack range `[current SP, g_stack_base)`, the slot value of every
registered global root (read fresh each cycle), and every pinned block. Each
candidate word goes through `gc_find_containing()`, so an **interior pointer
retains its containing block** (`&list.data[i]`-style pointers are common, since
raw pointer arithmetic does not exist in the language;
`tests/41_gc/03_interior_pointer_retention.tr`). A newly marked block is pushed
on an explicit worklist (not recursion, so a long chain cannot overflow the C
stack) and traced by `gc_scan_block_precise()`.

**Precise tracing.** A block's `TurfTypeDesc` (`runtime/turfrt.h`; layout and
emission in `runtime.md` §4) says which 8-byte words may hold pointers. The
mark loop walks the block in `desc->size` strides, re-applying the bitmap at
each, so one descriptor serves both a single struct and an array of elements. A
collection's backing buffer gets a descriptor for **one element** of its
concrete `T`, built by `getOrCreateElementTurfTypeDesc()` where
`CastExprAST::codegen()` recognises a `gc_alloc(...) to T*` site: a
`List<int>` element descriptor has an all-zero bitmap (nothing to trace), a
`List<string>` one has bit 0 set, so each element is followed to its string
buffer (`tests/41_gc/02_precise_element_tracing.tr`). The bitmap covers the
type's whole layout at any size, with no fixed cap (INV-MEM-2;
`tests/41_gc/05_large_struct_element_tracing.tr`,
`06_struct_exactly_64_and_65_words.tr`). A `NULL` descriptor means opaque, which
is right for a string buffer or a `void*` run.

**Sweep.** `gc_sweep()` `free()`s every unmarked block, compacts the table
preserving order, records the live count and bytes (`turf_rt_stats()`), and sets
the next threshold.

**Non-moving.** A block's address never changes for its lifetime (`realloc`
aside), so a GC pointer can be handed straight to `strlen`, `write` or any other
C call and stays valid for the call (`tests/41_gc/04_ffi_crossing.tr`) with no
pinning.

**What is not reclaimed automatically:** the manual heap (F-MEM-3), and a GC
object reachable only through manual-heap memory unless pinned with
`mem::root()`.

#### 2.6.3 Root discovery: registers, stack bounds, per platform

The scan has two platform-dependent inputs, each chosen at compile time in
`runtime/turfrt_gc.c` and each a hard `#error` on an unported target, so an
unported platform fails to build instead of silently under-scanning
(INV-RT-4).

**Callee-saved registers** (`gc_capture_callee_saved()`). A `noinline` leaf
function reads the current contents of exactly the target ABI's callee-saved
registers into an array, using inline asm. It is not `setjmp`: glibc XOR-mangles
the pointer-valued registers it saves, so they would not look like heap
addresses. The technique relies on the calling convention: any frame between the
true root and this read that needed one of these registers for itself has
already spilled its caller's value into its own stack frame, which the stack
scan covers. The sets (`TURF_GC_REGCAP_*`):

| ABI | Targets | Captured | Words |
|---|---|---|---|
| SysV x86-64 (`TURF_GC_REGCAP_SYSV64`) | Linux x86-64 | `rbx`, `rbp`, `r12`-`r15` | 6 |
| Win64 (`TURF_GC_REGCAP_WIN64`) | Windows x86-64 (MinGW) | the SysV set, plus `rdi`, `rsi` (callee-saved here) and the full 128 bits of `xmm6`-`xmm15` | 28 |
| AAPCS64 (`TURF_GC_REGCAP_AAPCS64`) | macOS arm64 (the only macOS target), Linux arm64 | `x19`-`x28`, `x29`, and the low 64 bits of `d8`-`d15` (AAPCS64 preserves only that half) | 19 |

The Win64 `xmm` captures are one `asm` statement per register: a single
statement with 20 memory outputs does not assemble at `-O0`. Linux on any
architecture other than x86-64 and AArch64, macOS x86-64, and every other OS
(the BSDs, for instance, which have no `/proc` and no `__libc_stack_end`) are
`#error`.

**Stack top** (`turf_gc_record_stack_base()`, called once from `turf_rt_init()`
before any Turf code runs). It asks the OS for the thread's stack region, which
is fixed at thread creation:

| Platform | Source |
|---|---|
| Linux | the high address of the `[stack]` mapping in `/proc/self/maps` |
| Linux, no usable procfs (a container or sandbox) | `__libc_stack_end` on glibc (the stack pointer at process entry, an ancestor of every Turf frame); `pthread_getattr_np()` on other libcs (musl, bionic) |
| macOS | `pthread_get_stackaddr_np(pthread_self())` |
| Windows | the high limit from `GetCurrentThreadStackLimits()` |

If no source yields an address the runtime panics (`cannot locate the stack`);
there is no under-scanning fallback. In particular it must not capture "a local
in `turf_rt_init()`": `turf_rt_init()` returns before the user's `main` runs
(`WrapMainWithRuntimeBootstrap()` calls them as siblings), so `main`'s frames
can sit at addresses above that local and be excluded from `[sp, base)`.
`TURF_GC_NO_PROCFS=1` makes the Linux path skip procfs so the fallback runs on
an ordinary machine (`scripts/smoke/gc_no_procfs_smoke.sh`).

**Stack low end.** The current SP is approximated per collection by the address
of a local in `gc_approx_sp()`, a tiny `noinline` helper. Everything below that
address is the collector's own C frames, never Turf data, so excluding it is
correct. The scan reads every aligned word of `[sp, g_stack_base)`.

**Under ASan.** `libturfrt_asan.a` is compiled with `-fsanitize=address`
(`TURF_RT_SANITIZE`). Two things change. `gc_approx_sp()` is exempted from
instrumentation, because ASan's fake-stack pass may relocate an ordinary local
onto the heap and the SP approximation would then be a heap address. And the
conservative scan skips words ASan has poisoned (the redzones around
stack-allocated locals in frames that have already returned), via
`__asan_region_is_poisoned()`. The precise heap reads are not exempted, so a
genuine freed-block-reuse bug through precise tracing still aborts. The runtime
also installs no fault handler under ASan (`runtime.md` §7.1).

#### 2.6.4 Testing aids

All are runtime environment variables read by every compiled program, combinable
with each other and any `-O` level. A value counts as set when it is non-empty
and not `0`.

- **`TURF_GC_STRESS=1`** collects on every allocation instead of at the
  heap-doubling threshold. A root the conservative scan barely misses can go
  unnoticed for a long time under normal pressure and is caught at once when
  every allocation is a collection point. The whole `tests/` suite is run this
  way. `tests/custom/gc_stress_corpus/` holds three long-running programs (a
  `heap()` graph rewired at 1,000,000 live nodes, a `Map<string, List<string>>`
  churned for 10,000,000 operations, the `http_server` showcase shape driven by
  a scripted client for 100,000 requests), run by
  `scripts/smoke/gc_stress_corpus_smoke.sh` and deliberately not under
  `TURF_GC_STRESS` (an `O(n)`-allocation workload becomes quadratic-ish when
  every allocation collects).
- **`TURF_GC_VALIDATE=1`** (Linux, glibc, x86-64 only; elsewhere a one-time
  notice and no effect) cross-checks the production scan against roots derived
  independently. (1) A second register snapshot from `getcontext()`, taken
  adjacent to the primary read, is checked for *caller-saved* registers
  pointing at an otherwise-unmarked block; this is report-only, since such a
  register may hold coincidental stale data. (2) A full second mark phase takes
  its roots only from sources the primary scan never uses: a `getcontext()`
  snapshot taken in `turf_rt_collect()` (an ancestor of `gc_mark()`) for the
  callee-saved registers and the stack's low end, and `pthread_getattr_np()` for
  the whole `[stack]` mapping's high end. Its range is a subset of the
  production range whenever the production stack base is right, so a block only
  the independent pass marks is a real missed root, and the collector panics
  (`GC root missed`) rather than sweep a live object. This is the check that
  catches a wrong stack bound, which the register check cannot. (`pthread_*` are
  weak references, so the comparison reports itself unavailable instead of
  failing the link on a libc without them.) Pair it with `TURF_GC_STRESS=1` to
  compare mark sets on every allocation.
- **`TURF_GC_NO_PROCFS=1`**: see §2.6.3.
- **The ASan build** (§2.6.3): configure a separate build tree with
  `-DTURF_RT_SANITIZE=ON`; `turfc` from that tree links every program against
  `libturfrt_asan.a`, which turns the whole suite into freed-block-reuse
  validation. Both trees write the plain `lib/libturfrt.a`, so do not build one
  while a suite runs against the other.
- **`TURF_RT_STATS_DUMP=1`** prints the allocation and collection counters to
  stderr at shutdown.

`tests/41_gc/` must also be run at `-O0` (`./scripts/test_runner.sh -O0`):
conservative scanning can mask a real tracing bug at `-O2` through
register-allocation luck.

### 2.7 Generics and monomorphization

Generics are monomorphized, not type-erased (F-GEN-1). The monomorphizer
(`src/sema/Monomorphize.cpp`; `compiler_pipeline.md` §7 has the pipeline
position and the drain functions) clones each generic declaration once per
distinct concrete type-argument tuple the program uses, substitutes the types,
re-resolves and re-typechecks the clone, and registers it under a mangled name.
The templates themselves never reach Codegen.

#### 2.7.1 Every distinct instantiation gets its own real layout

`getLLVMType()`'s `GenericType` branch (`List<int>`, `Box<Player>`, ...) looks
up the per-instantiation LLVM struct by its mangled name
(`Monomorphize::instantiatedDeclName()`, which inserts a `_gen<N>_` marker, e.g.
`List_gen1_int`; the LLVM struct is module-qualified, `list.List_gen1_int`).
`List<int>` and `List<string>` are two distinct `llvm::StructType`s with two sets
of compiled methods: `List<int>.push` takes a genuine `i64`, `List<string>.push`
a genuine string pointer. Field access resolves the field per instantiation
(`FieldAccessExprAST`, `src/codegen/CodegenStruct.cpp`, using the same name),
and `GlobalSymbolTable` holds one registered `TurfStructType` per instantiation.

Reaching Codegen with an unsubstituted `TurfGenericParamType` is a compiler bug
(F-GEN-2), asserted in a debug build in three places: `finishInstantiation()` in
`Monomorphize.cpp` (a clone's own parameters and return type), and
`getLLVMType()` and `CastToType()` in `CodegenCore.cpp` (INV-GEN-1). There is
no code that erases a generic parameter to a pointer-sized slot or reinterprets
a scalar through one.

#### 2.7.2 Fields carry their real, substituted types

Because a clone's fields are substituted, a `T`-typed field's storage is
whatever `T` actually is: a real `i64` for `int`, a pointer for `string`, an
inline aggregate for a struct, laid out exactly as if the struct were written
for that one type. `Resolver::checkStructLayoutAcyclicityForClone()` runs per
clone before it is registered, so a clone whose by-value layout would be
infinite is rejected the same way a non-generic struct's would be.
`self.data[i] = item;` where `item: T` is an ordinary typed store.

#### 2.7.3 `sizeof(T)` and struct-typed `T` of any size

`sizeof(T)` inside a generic body reports the true size per instantiation, for
free functions and for struct methods alike. Every standard-library collection's
growth computes its buffer size as `cap * sizeof(T)` (`List`, `Stack`, `Queue`,
`Set`, `Map` in `lib/std/collections/`, and `Pool<T>` in `lib/std/mem/mem.tr`),
so a struct element of any size works directly: `List<Big>` with a multi-word
`Big` compiles `push`/`pop` clones that take and return a genuine `%Big`
aggregate by value and grows its buffer correctly
(`tests/38_monomorphization/12_list_growth_over_8_bytes.tr`, `17_` through
`20_..._growth_over_8_bytes.tr`). Because each element is a real aggregate, the
buffer's element descriptor (§2.6.2) traces its pointer fields.

`List<MyStruct*>` (instantiating with a pointer to the struct) is still legal and
is an ordinary choice about aliasing, not a workaround for size. For a
collection of structs that hold `string` or collection fields, storing the
struct by value is the form the collector traces precisely; boxing each in
manual memory hides its GC fields from the collector (`boundary.md` §5). A
`List<T>`-shaped module cannot simply switch `data` from `T*` to `&T`, because
`checkNoRawPointersInSignature` rejects a signature that substitutes to a raw
pointer parameter when `T` is itself a pointer type; that is unrelated to
monomorphization and is documented in `lib/std/collections/list.tr`.

### 2.8 Pointers and `&T`: representation only

`getLLVMType()`'s pointer branch maps every `TurfPointerType` (which
`TurfReferenceType`, `&T`, inherits from, `pointers_and_references.md` §2.1) to
the same opaque pointer as `ptr` and `string`. An unsized array parameter is the
`{ ptr, i64 }` slice (§2.3). There is no separate reference representation:
`&T`'s entire contribution is a compile-time non-null contract enforced in the
TypeChecker (`pointers_and_references.md` §2.5). A pointer or reference is never
copied together with its pointee the way a struct or array is: assigning one to
another copies the *address*, and both then alias the same memory.

---

## Part 3: File map

| File | Role in this document |
|---|---|
| `src/sema/Types.h` | The type hierarchy: `IntType`/`Int32Type`/`DoubleType`/`BoolType`/`CharType`, `ArrayType`, `TurfPointerType`/`TurfReferenceType`, `TurfGenericParamType`/`GenericType`, `TurfStructType`, `TypeIdentity::llvmName()` |
| `src/sema/Types.cpp` | `typesCompatible()`: array-size and element-type compatibility, the string/void/bool exclusions from numeric coercion |
| `src/sema/Intrinsics.h` | The closed list of bare intrinsics, including the `gc_*` family and `heap` (§2.6) |
| `src/sema/Monomorphize.cpp` | Clone, substitute, re-resolve, register; `instantiatedDeclName()`, `finishInstantiation()` (§2.7) |
| `src/codegen/CodegenCore.cpp` | `getLLVMType()` (§2.2, §2.3.1, §2.7.1, §2.8), `CastToType()`, `isPointerLike()`, `GetDefaultValue()`/`EmitDefaultValue()` (§2.5.2), `HeapAllocateValue()`/`BoxToI64Slot()` (§2.6.1) |
| `src/codegen/CodegenExpr.cpp` | Literal codegen (§2.2, §2.4); `CastExprAST::codegen()`'s element-descriptor injection and numeric-to-string (§2.4, §2.6.2) |
| `src/codegen/CodegenArray.cpp` | `ArrayDeclExprAST`, `ArrayLiteralExprAST`, `SliceExprAST`, `CodegenSliceArgument()` (§2.3) |
| `src/codegen/CodegenVar.cpp` | `VarDeclExprAST::codegen()`: the copy-on-assignment path and the construction-alloca reuse (§2.5.2, §2.5.3) |
| `src/codegen/CodegenFunc.cpp` | Parameter passing (§2.3.2, §2.5.3), construction (§2.5.2), the `gc_*`/`heap` dispatch (§2.6), `WrapMainWithRuntimeBootstrap()` |
| `src/codegen/CodegenControlFlow.cpp` | `ReturnExprAST::codegen()`: the generic return path (§2.3.3, §2.5.3) |
| `src/codegen/CodegenStruct.cpp` | `StructDeclExprAST::codegen()` (§2.5.1); `EmitAllTurfTypeDescs()`, `buildTurfTypeDescInit()`, `getOrCreateElementTurfTypeDesc()` (§2.6.2); `arr.copy()` (§2.3.1) |
| `runtime/turfrt_gc.c` | The collector: block table, root discovery per platform, mark, sweep, pins, testing aids (§2.6.2-§2.6.4) |
| `runtime/turfrt_alloc.c` | `turf_rt_alloc`/`_zeroed`/`turf_rt_realloc`, `turf_rt_checked_free`, `turf_rt_stats` |
| `runtime/turfrt.h` | The public `libturfrt` ABI and `TurfTypeDesc` (§2.6, §1.7) |
| `lib/std/mem/mem.tr` | Manual-heap wrappers, `Pool<T>`, `free()`'s `gc_checked_free` check, `root()`/`unroot()` (§1.7, §2.6) |
| `lib/std/collections/list.tr` (and `stack`, `queue`, `set`, `map`) | The header-plus-buffer collection shape (§2.6.2, §2.7.3) |
| `lib/std/core/strings.tr` | String operator overloads, `StringBuilder`, and their GC-heap allocations (§2.4) |
| `docs/reference/arrays.md` | User-facing array semantics (§2.3) |
| `docs/reference/stdlib/collections.md` | The shared collection design |
| `docs/architecture/pointers_and_references.md` | The `ptr`/`T*`/`&T`/`unsafe` reference (§1.6, §2.8) |
