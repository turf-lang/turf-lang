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
| `string` from `+`/`to_upper`/etc. | Heap | Aliased (a `string` is a pointer) | **Nobody.** Never freed automatically, and nothing in the standard library frees it either |
| Fixed-size array `T[N]` | Stack | Copied (value semantics) | Automatic |
| Struct (local, or via `::new`/`::default`) | Stack, at the point of construction | Copied on assignment/pass/return | Automatic, *if* it never leaves the stack — see §1.5 |
| `ptr` / `T*` (raw pointer) | Wherever it was given — usually heap, via `mem::alloc`/`malloc` | Aliased | **You.** Manual `mem::free`/`free`, or nothing |
| `&T` (safe reference) | Same representation as `T*` | Aliased | Same as `T*` — the only difference is a compile-time non-null guarantee, not a lifetime guarantee (see `pointers_and_references.md`) |
| `List<T>`/`Stack<T>`/`Queue<T>`/`Map<K,V>`/`mem::Pool<T>`'s backing buffer | Heap | Aliased, grown via `realloc` | **You** — nothing frees it automatically either |

The one-sentence version of Turflang's safety story: **there is no
automatic memory management of any kind today.** No garbage collector, no
reference counting, no borrow checker. `&T` (§1.6) buys you a compile-time
non-null guarantee and nothing else — it does not prevent a dangling
reference, and it does not free anything. Every heap allocation in a Turf
program, whether written by hand or reached through a stdlib collection, is
freed only if some line of code explicitly calls `free`/`mem::free` — and,
as of today, the standard library itself never calls `free` on the buffers
it hands you responsibility for (§1.4, §1.7).

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
`repeat`, ...) is a **fresh heap allocation**, sized exactly for its
contents and obtained via the same C allocator FFI everything else in this
document goes through (`malloc`, `lib/std/core/strings.tr:57`). Every one of
these functions mallocs a new buffer, copies bytes into it, and returns it —
**and none of them ever frees the operand(s) they were given.** The
`strings.tr` source is explicit about this being a known, deliberate
tradeoff:

> `operator +`'s body mirrors exactly what used to be Codegen's hardcoded
> EmitStringConcat, including the same TURF-MEMLEAK tradeoff: the malloc'd
> buffer is intentionally never freed (Turf has no GC yet); future work is
> an arena allocator or a Boehm GC sweep.
> — `lib/std/core/strings.tr:50-53`

Concretely: `result = result + s;` inside a loop (as `strings::repeat` and
`strings::replace` both do internally) leaks one buffer's worth of memory
per iteration, on top of whatever the original operands already cost. A
`string`'s representation is pointer-identical to `ptr`/`void*`
(`CodegenCore.cpp`'s `isPointerLike()`, §2.4), so a leaked buffer *can* in
principle be freed manually via `mem::free` once cast — but nothing in the
standard library does this today, and there's no reference count or any
other bookkeeping that would tell you it's safe to.

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

### 1.7 What manages any of this? Nothing, automatically.

To say it as plainly as possible: **Turflang has no automatic memory
management today** — no garbage collector, no reference counting, no
ownership/borrow checker, no destructors run on scope exit. Every stack
allocation is reclaimed automatically the instant its function returns (that
part is free, and unavoidable, the way it is in any compiled language) —
but every *heap* allocation, anywhere in a Turf program, is freed only if
some line of code explicitly calls `free`/`mem::free` on it, and nothing
currently does that on your behalf:

- `mem::alloc`/`mem::alloc_zeroed`/`mem::grow`/`mem::dup`/`mem::alloc_aligned`
  (`lib/std/mem/mem.tr`) are thin, fail-fast wrappers around
  `malloc`/`calloc`/`realloc`/`posix_memalign` — you own whatever they
  return, exactly as in C.
- `List<T>`, `Stack<T>`, `Queue<T>`, `Map<K,V>`, and `mem::Pool<T>` each grow
  a `malloc`/`realloc`-backed buffer internally (`lib/std/collections/list.tr:31-33`,
  and the shared growth policy documented in `docs/reference/stdlib/collections.md`)
  — but **none of them frees that buffer for you**, ever, including when the
  collection variable itself goes out of scope. There is no destructor
  mechanism in the language that could even hook that. `mem::Pool<T>` exists
  specifically to make *manual* bulk-freeing less tedious (track every
  allocation, then `mem::free_pool()` them all in one call) — it is a
  bookkeeping convenience, not automatic management.
- String concatenation and most `strings::*` functions heap-allocate and
  leak by design today (§1.4).

A future automatic-reference-counting (ARC) design exists only as a
proposal — `docs/planning/FUTURE.md` §5 — and is explicitly gated behind
other, unrelated work; nothing described in this document should be read as
already having that safety net.

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
representation — this is the single fact that makes §1.4's "a leaked
string buffer could in principle be `mem::free`d like any other pointer"
claim true at the representation level, independent of whether any
call site in the standard library actually exercises that path today.

**Concatenation moved out of Codegen entirely.** `src/CodegenExpr.cpp:82-94`
documents that `EmitStringConcat`/`EmitStrcmp`/`EmitStrcmpRelational` used to
be hand-rolled directly in this file; `+`/`==`/`!=`/`<`/`>`/`<=`/`>=` on two
`string`s are now ordinary `operator` overloads defined in Turf itself
(`lib/std/core/strings.tr:55-85`), resolved through the exact same
operator-overload machinery any user-defined `operator` would go through.
`operator +`'s body is what actually performs the heap allocation
(`strings.tr:55-61`):

```turf
operator +(a: string, b: string): string {
    mut n: int = strlen(a) + strlen(b) + 1;
    mut buf: string = malloc(n) to string;
    strcpy(buf, a);
    strcat(buf, b);
    return buf;
}
```

`malloc(n)` — sized exactly for the concatenated result plus a NUL
terminator, nothing more — is the entire allocation; there is no
over-allocation/capacity headroom the way a growable collection has. The
function's own doc comment (`strings.tr:50-53`, quoted in full in §1.4)
names this the "TURF-MEMLEAK tradeoff" explicitly: the returned buffer is
never freed by this function, by its caller, or by anything else in the
standard library. Every other buffer-returning function in the same file
(`to_upper`, `to_lower`, `substring`, `trim`, `replace`, `repeat`) follows
the identical shape — `malloc`, fill, return, never free — `replace` and
`repeat` additionally call `operator +` (via plain `+`) in a loop, so each
one leaks once per concatenation performed internally, on top of its own
final result.

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

### 2.6 Heap allocation: what exists, and what's built on it

Every heap allocation reachable from a Turf program funnels through one of
a small number of C allocator FFI bindings, all declared with `extern fn` —
never a compiler intrinsic, never hidden Codegen logic:

```turf
// lib/std/mem/mem.tr:31-34
extern fn malloc(size: int): void*;
extern fn calloc(count: int, size: int): void*;
extern fn realloc(p: void*, new_size: int): void*;
extern fn free(p: void*): void;
```

`mem::alloc`/`mem::alloc_zeroed`/`mem::grow`/`mem::dup`/`mem::alloc_aligned`
(`mem.tr:64-116`) are thin, fail-fast (`sys::panic` on a null/OOM result)
wrappers around these — no additional bookkeeping, no metadata prepended to
the allocation, nothing beyond what raw `malloc`/`calloc`/`realloc` already
give you.

Every generic collection in `lib/std/collections/` is, at the storage
level, the identical three-field shape:

```turf
// lib/std/collections/list.tr:13-17
struct List<T> {
  data: T*,
  len: int,
  cap: int
}
```

`data` is a heap pointer, grown via `malloc`/`realloc` exactly like a
hand-written dynamic array in C (`list.tr:19-39`'s `List.push`: doubling
growth starting at capacity 8, `mem::malloc`/`mem::realloc` when the backing
buffer needs to grow). The **struct itself** — the three-word header
`{data, len, cap}` — lives wherever the `List<T>` variable holding it lives
(stack, by default, per §1.5); only the buffer `data` points at is ever on
the heap. `mem::Pool<T>` (`mem.tr:155-214`) is a deliberately independent,
minimal reimplementation of the identical growth logic — its own comment
explains why it can't just `import list` and reuse `List<T>` (this
language doesn't support circular imports, and `list.tr` already imports
`mem.tr`) — used specifically to batch-free a set of unrelated allocations
in one call (`mem::free_pool()`) rather than tracking each `free()`
individually; it is a manual-bookkeeping convenience, not a different
memory-management model.

**Nothing frees any of this automatically.** There is no destructor concept
in the language — no code runs when a `List<T>` (or any struct) goes out of
scope, so a `List<T>`'s `data` buffer is leaked unless some line of code
explicitly frees it (which, notably, `List<T>` itself provides no method to
do — the closest analog is `mem::free_pool()`, and that only works for a
`Pool<void*>` specifically, per its own doc comment in `mem.tr:188-204`).

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
| `src/CodegenStruct.cpp` | `StructDeclExprAST::codegen()` — inline field layout (§2.5.1) |
| `lib/std/mem/mem.tr` | The C allocator FFI bindings and their checked Turf wrappers; `Pool<T>` (§2.6) |
| `lib/std/collections/list.tr` | The canonical `data: T*, len: int, cap: int` collection shape and its type-erasure comment (§2.6, §2.7.1) |
| `lib/std/core/strings.tr` | String `operator` overloads, including the documented `+` heap-allocation/leak tradeoff (§2.4) |
| `docs/reference/arrays.md` | User-facing array semantics, verified against implementation in §2.3 |
| `docs/reference/stdlib/collections.md` | The shared collection design writeup, including the `List<T*>` >8-byte workaround (§2.7.3) |
| `docs/architecture/pointers_and_references.md` | The full `ptr`/`T*`/`&T`/`unsafe` reference this document cross-references rather than duplicates (§1.6, §2.8) |
| `docs/planning/FUTURE.md` | §5, the proposed (unimplemented) ARC design mentioned in §1.7 |
