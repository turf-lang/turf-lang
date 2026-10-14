# Pointers, Safe References, and `unsafe { }`

How TurfLang's pointer system works: the four pointer-shaped types (`ptr`, `T*`,
`&T`, `&?T`), the `unsafe { }` block, the `unsafe module;` capability
declaration, and the two flow analyses that make the safe references safe
(`NullabilityPass` and `EscapePass`). Like
`docs/architecture/modules_and_scope_resolution.md`, this describes both **what
a TurfLang programmer observes** (Part 1) and **how the compiler implements it,
pass by pass** (Part 2). Frozen decisions: `docs/development/FREEZE.md` §6.6
(pointers and references), §6.12 (`heap()`), §6.13 (escape rules), §6.8 (the
standard library holds no capability a user cannot request).

Related docs:
- `docs/reference/ffi_and_unsafe.md` — the user-facing version of Part 1.
- `docs/architecture/memory_model.md` — where the memory behind a `&T` lives
  (stack, GC heap, manual heap) and how `heap()` allocates.
- `docs/architecture/boundary.md` — which standard-library modules declare the
  raw-pointer capability and why.
- `docs/architecture/compiler_pipeline.md` — the pass order this document slots
  into.

---

## Part 1 — Language semantics

### 1.1 The four pointer-shaped types

| Type | Spelling | Nullable? | Pointee known? | Where it is legal |
|---|---|---|---|---|
| Opaque pointer | `ptr` | yes | no (an untyped `ptr` in LLVM IR) | `extern fn` signatures; inside `unsafe { }`; in a file that declares the raw-pointer capability (1.7) |
| Typed raw pointer | `T*` | yes | yes | same as `ptr` |
| Safe reference | `&T` | **no**, compiler-guaranteed | yes | anywhere, no `unsafe { }` |
| Nullable safe reference | `&?T` | **yes**, but every use is checked (1.8) | yes | anywhere, no `unsafe { }` |

`ptr` is the untyped FFI escape hatch (what `extern fn malloc(size: int): ptr;`
returns); cast it to a concrete `T*` with `to` before using it as anything
struct-shaped. `T*`, `&T` and `&?T` all know their pointee and all support field
and method access with no explicit dereference: the language has no `*p`
operator at all. The value itself is copied out of a `&T` (or a narrowed `&?T`,
or `self`) with `to`: `r to T`. What separates them is a compile-time promise (or its absence):
a `&T` is never null; a `&?T` may be null and the compiler checks every use; a
`T*` may be null and nothing checks it. All four lower to the same LLVM pointer
value (2.6), so the distinction costs nothing at run time.

A struct field of type `&T` is defaulted by `S::default()` to a fresh
`T::default()` rather than to null; a struct that reaches itself through `&`
fields therefore has no default value (E147, hint: make one field on the cycle a
`&?T`).

### 1.2 The gate: where a raw pointer type may be used

Four kinds of use of a raw pointer type (`ptr` or `T*`; **not** `&T` or `&?T`,
which are exempt from all of this) need an `unsafe { }`:

| Use | Example |
|---|---|
| **Declaration** | `mutable p: ptr = ...;` |
| **Cast**, either direction | `raw to int*`, `ref to int*` |
| **Dereference**: field access, indexing or a method call through a raw base | `p.field` with `p: Node*`; `w.data[i]` with `data: int*`; `p.method()` |
| **Struct field** (and error-payload field) | `struct Node { next: Node* }`: the *entire* declaration must be inside `unsafe { }` |

A fifth rule is checked **unconditionally** and `unsafe { }` does not fix it:

| Use | Example | Fix |
|---|---|---|
| **Plain function signature**: a parameter or return type that is a raw pointer | `fn get(p: Node*): int` | use `&T`; or declare the capability (1.7) |

All five report E130 (`Raw pointer outside unsafe`) with the titles
`Unsafe pointer use!` (the first four) and `Raw pointer in function signature!`
(the fifth). A related check, titled `Unsafe reference conversion!` (also E130),
applies when a *reference* is forged out of something that is not already a
reference to the same pointee: `0 to &int`, `someString to &Node`,
`&A to &B`. Converting `&T` to `&?T` and a narrowed `&?T` to `&T` keep the
pointee and stay safe.

Four things are exempt from the gate:

1. **A file that declares the raw-pointer capability** (`unsafe module;`, 1.7).
   Every raw-pointer use in it is legal without wrapping, and the signature rule
   does not apply either.
2. **`extern fn` declarations.** An FFI binding's parameters and return type may
   be `ptr`/`void*`/`T*` freely. Only *binding the result to a variable* in your
   own code is a gated declaration:
   ```turflang
   extern fn malloc(size: int): ptr;
   unsafe {
       mutable p: ptr = malloc(64);
   }
   ```
3. **The implicit `self` parameter.** `self` is always non-null, and its type is
   still an ordinary raw `StructName*` (2.5.3), so a method reads and writes
   `self.field` freely and `self` does not count against the signature rule.
4. **Code lexically inside an `unsafe { }`**, including a struct, `fn`, `enum`
   or `machine` declaration wrapped in a top-level `unsafe { }` (1.4).

Two things are *not* gated, and are known gaps rather than design: a **method
call** through a raw-pointer receiver (`a.next.getval()` with `next: Node*`)
needs no `unsafe { }`, and a field *read* of a raw-pointer-typed field is only a
dereference if something is then accessed through it.

### 1.3 `&T`, in detail

`&expr` (address-of) always produces a `&T`, never a raw pointer. The operand
must be an addressable lvalue (a named variable, a struct field, an array
element); `&(x + 1)` is rejected.

```turflang
mutable val: int = 42;
mutable ref: &int = &val;
```

`&expr` points into the stack storage of whatever it names, so it must not
outlive the frame (1.9). **`heap(expr)` is the other source of a `&T`**, and the
only one whose pointee outlives the current frame: it copies `expr` onto the GC
heap (with the type's real descriptor, so the collector traces it) and returns a
`&T` to that copy. It needs no `unsafe { }` and rejects `void`:

```turflang
struct Node { val: int }

fn make(): &Node {
    mut n: Node = Node::default();
    n.val = 42;
    return heap(n);   // n's stack copy dies with the frame; this one does not
}
```

`heap()` is a closed-list intrinsic (`src/sema/Intrinsics.h`,
`BareIntrinsic::Heap`); its allocation is `HeapAllocateValue()` in
`src/codegen/CodegenCore.cpp`, described in `memory_model.md` §2.6.1.

**Widening is one-way.** `&T` widens to `&?T` and to `T*`/`ptr`; nothing widens
back implicitly:

```turflang
mutable ref: &int = &val;
unsafe {
    mutable raw: int* = ref;            // &T -> T*: implicit
    mutable back: &int = raw;           // T* -> &T: REJECTED even inside unsafe
    mutable back2: &int = raw to &int;  // explicit cast, needs unsafe { }
}
```

Declaring `raw` is itself a gated declaration, which is why it sits in the
`unsafe { }`; the *widening* needs no cast. Storing a `&Node` into a `Node*`
struct field (`a.next = &b;`) compiles outside `unsafe { }` for the same reason:
the assignment names no raw type. A raw pointer might be null, so turning one
into a `&T` is an explicit `to` cast, and that cast is a gated "cast" use.

`null` is never compatible with `&T` (E062, `Cannot assign 'null' to variable
... of type '&int'`).

### 1.4 `unsafe { }`, in detail

`unsafe { }` is a **lexical gate**, not a control-flow construct. A `return`,
`break` or `continue` inside it behaves as if the braces were absent:

```turflang
struct Counter { n: int }

fn Counter.increment(): &Counter {
    self.n = self.n + 1;
    unsafe {
        return self to &Counter;   // returns from increment()
    }
}
```

(The `unsafe { }` is required: `self` is a raw `Counter*`, and converting it to
the declared `&Counter` is the raw-to-safe cast.)

It is usable in three positions:

1. **As a statement** in a function body.
2. **Nested** in another block (`if`, `while`, another `unsafe { }`); nesting
   stays authorized throughout.
3. **At the top level, wrapping a `struct`/`fn`/`enum`/`machine` declaration**,
   which is required for any struct with a raw-pointer field.

   ```turflang
   unsafe {
       struct Node {
           val: int,
           next: Node*
       }
   }
   ```

It is an **expression**, valued as its last statement, the same rule an ordinary
`{ }` block follows:

```turflang
mutable v: int = unsafe { b.next.val; };
```

### 1.5 Worked examples

**A linked list with `&?T`** (the safe, checked choice; no `unsafe { }` anywhere):

```turflang
struct Node {
    val: int,
    next: &?Node
}

fn sum(head: &?Node): int {
    mutable total: int = 0;
    mutable current: &?Node = head;
    while (current != null) {
        total = total + current.val;
        current = current.next;
    }
    return total;
}

fn main(): int {
    mutable a: Node = Node::default();
    mutable b: Node = Node::default();
    a.val = 1;
    b.val = 2;
    a.next = &b;
    return sum(&a) - 3;
}
```

**The same list with a raw pointer** (what an FFI-shaped structure looks like).
A plain function may not take or return a raw pointer, so the walker takes a
`&Node` and widens it into a local raw pointer inside its own `unsafe { }`:

```turflang
unsafe {
    struct Node {
        val: int,
        next: Node*
    }
}

fn sum_list(head: &Node): int {
    unsafe {
        mutable current: Node* = head;      // &Node -> Node*: no cast
        mutable total: int = 0;
        while current != null {
            total = total + current.val;
            current = current.next;
        }
        return total;
    }
}
```

**A generic container over a raw field:**

```turflang
unsafe {
    struct Box<T> {
        val: T*
    }
}

fn<T> Box.set(v: &T): void {
    self.val = v;                  // &T -> T*: widening
}

fn<T> Box.get(): &T {
    unsafe {
        return self.val to &T;     // T* -> &T: explicit cast
    }
}
```

**FFI:**

```turflang
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;

fn main(): int {
    unsafe {
        mutable buf: ptr = malloc(64);   // binding the result: gated
        free(buf);                        // the call itself: not gated
    }
    return 0;
}
```

### 1.6 What does not compile

```turflang
// (1) a raw pointer declared outside unsafe          -- E130 Unsafe pointer use!
fn main(): int {
    mutable p: ptr = null;
    return 0;
}

// (2) a raw-pointer struct field outside unsafe      -- E130 Unsafe pointer use!
struct Node { next: Node* }

// (3) a raw-to-safe cast outside unsafe              -- E130 Unsafe pointer use!
fn g(r: &int): void {
    mutable q: int* = r to int*;
}

// (4) a raw pointer in a plain function's signature  -- E130 Raw pointer in function signature!
unsafe {
    fn takesRaw(p: ptr): int { return 0; }   // unsafe does not fix this
}

// (5) null is never a &T                             -- E062
fn h(): int {
    mutable r: &int = null;
    return 0;
}

// (6) a reference forged from a non-reference        -- E130 Unsafe reference conversion!
fn k(): int {
    mutable r: &int = 0 to &int;
    return 0;
}
```

Regression tests: `tests/05_arrays_pointers_ffi/` (`fail_ptr_outside_unsafe.tr`,
`fail_cast_raw_pointer_outside_unsafe.tr`,
`fail_struct_raw_ptr_field_outside_unsafe.tr`,
`fail_fn_signature_raw_pointer_disallowed.tr`,
`fail_raw_pointer_index_outside_unsafe.tr`,
`fail_cast_int_to_reference_outside_unsafe.tr`,
`fail_cast_string_to_reference_outside_unsafe.tr`,
`fail_cast_reference_to_other_reference_outside_unsafe.tr`,
`18_unsafe_block_and_safe_reference.tr`), and
`tests/39_nullable_references/fail_null_to_safe_reference.tr`.

### 1.7 The raw-pointer capability declaration

A file's own first top-level statement may be

```turflang
unsafe module;
```

which grants the file the raw-pointer capability: every raw-pointer use in it
(declarations, casts, dereferences, struct fields **and plain function
signatures**) is exempt from the gate, without wrapping each in `unsafe { }`.
That is what lets a user file write a C callback such as
`fn cmpints(a: ptr, b: ptr): int32` (what `qsort` wants), which no `unsafe { }`
can legalise.

The capability is **declared in source, never inferred from where a file lives**
(F-STD-1, INV-CAP-1; `boundary.md` §3). A declaration is *honored* when:

1. the file is under the resolved install root (`TurfInstallRoot()`), which is
   how the standard library's own modules use it; or
2. in a **package build**, the file's package is allowed: the root package, and
   each dependency the root manifest lists under `[unsafe] allow` (the
   capability is per package and not transitive, FREEZE.md §6.16); or
3. otherwise, `turfc`/`turf` was run with `--allow-unsafe-modules`.

In a package build the global flag is not consulted at all, so one flag cannot
grant every dependency the capability at once. An undeclared file gets nothing
regardless of location. Tests: `tests/05_arrays_pointers_ffi/21_unsafe_module_capability_and_multi_extern.tr`
and `tests/39_nullable_references/fail_raw_pointer_signature_no_capability.tr`.

### 1.8 `&?T`, in detail

`&?T` is a nullable reference whose uses the compiler checks. It is the type for
"a node, or nothing" — a linked list's next pointer, a tree's child — without
`unsafe { }`:

```turflang
struct Node {
    val: int,
    next: &?Node
}
```

`null` is assignable to `&?T`. Widening is implicit and one-way, with the same
shape as `&T`:

```turflang
mutable a: Node = Node::default();
mutable p: &?Node = &a;        // &Node -> &?Node: always allowed
unsafe {
    mutable raw: Node* = p;    // &?Node -> Node*: always allowed
}
```

**Using a `&?T` requires a null check first.** `p.val`, `p.next` and `p.method()`
compile only where `NullabilityPass` (2.8) has proven `p` non-null on every path
reaching the use. Without it, E081 (`Possible null dereference`). Narrowing
sources: a direct `p != null` / `p == null` comparison, flowing through `if` /
`else`, an early `return`, a `while` condition, a `&&` / `||` chain (not `!`), and
assignment from `&x` or from a `&T` value (both non-null by construction).

```turflang
fn sum(head: &?Node): int {
    mutable total: int = 0;
    mutable current: &?Node = head;
    while (current != null) {          // narrows `current`...
        total = total + current.val;   // ...so this is legal
        current = current.next;
    }
    return total;
}
```

**`&?T -> &T` is never implicit.** Passing a `&?T` where a `&T` is expected is a
type error (E056, no matching overload; E066 for an initializer). The only way is
an explicit `p to &T`, which needs no `unsafe { }` but is rejected (E082,
`Unnarrowed nullable-to-safe-reference cast`) unless `p` is proven non-null at
that cast:

```turflang
fn takesSafeRef(n: &Node): int { return n.val; }

fn maybeVal(p: &?Node): int {
    if (p != null) {
        return takesSafeRef(p to &Node);   // legal: narrowed right above
    }
    return -1;
}
```

**Every use is checked, not just `variable.field`:**

- A **method call** is a use: `q.bump()` passes `q` as `self` and needs the same
  proof as `q.val` (E081). A UFCS call whose callee takes the `&?T` itself
  (`fn count(p: &?Node)` called as `q.count()`) reads nothing and needs none.
- A **`&?T` that is not a variable cannot be used at all.** Narrowing is
  per-variable, so nothing can prove `p.next`, `find()` or `list[i]` non-null:
  `p.next.val`, `find().val`, `list[i].val` and a method call on one are E081,
  and `(p.next to &Node)` is E082. Both are rejected by the TypeChecker
  (`TypeChecker::rejectNullableNonVariableUse()`), with a hint to store the value
  in a variable and check that. Comparing such a value with `null`, storing it
  and passing it to a `&?T` parameter stay legal.
- A **loop keeps a proof it does not disturb.** `if (p == null) { return; }`
  followed by a loop that never assigns `p` leaves `p` non-null inside the loop
  and after it; a loop that may assign `p` ends the proof.
- A **deferred use is checked where it runs.** `defer use(q.val);` runs when its
  block exits, so a later statement of that block that may set `q` to null
  (anything but `&x` or a `&T` value; a later `defer` too) makes it E081
  (`TypeChecker::rejectDeferredNullableUse()`).

Tests: `tests/39_nullable_references/` (`03_narrowing_shapes.tr`,
`04_narrowed_cast.tr`, `08_narrowing_survives_loops.tr`,
`09_method_call_after_check.tr`, `10_nullable_field_through_a_variable.tr`,
`11_narrowing_inside_recover_block.tr`, and the `fail_deref_*`, `fail_method_call_*`,
`fail_cast_*` files beside them).

**Limits, by design.** Narrowing is **per variable**: `p.field != null` narrows
nothing; only a bare variable compared directly to `null` does. It is
**intraprocedural**: a null check in one function says nothing about another
function's parameter, which starts as "not proven" (the caller may pass `null`).

### 1.9 Reference and array escape rules

Neither `&T`'s non-null guarantee nor `&?T`'s null check says anything about
*where* the pointee lives. The remaining hazard is a reference into the current
function's own stack frame, smuggled out past the point where the frame is
popped (FREEZE.md §6.13, F-MEM-5, INV-MEM-6):

```turflang
fn make(): &Node {
    mut n: Node = Node::default();
    n.val = 42;
    return &n;              // E086: dangles the moment make() returns
}
```

`EscapePass` (2.9) tracks, for every reference-typed, raw-pointer or
unsized-array (`T[]`) parameter or local, a **provenance**:

- **Stack**: points into this frame's own storage (`&local`, `&local.f`,
  `&localArray[i]`).
- **Param**: came in from the caller, `self` included.
- **Heap**: from `heap()`, from a call whose result cannot reach this frame, from
  an element of a standard collection, or reached through a Heap base.
- **MayStack**: a merge where at least one path holds Stack. Flagged like Stack,
  because that path is a real escape (`p = heap(n); if c { p = &n; } return p;`).
- **Unknown**: anything the pass cannot classify. Never flagged.

A Stack or MayStack value that reaches one of these sinks is E086
(`Dangling reference escape`):

- a `return`;
- the right-hand side of an assignment to a field (or array element) reached
  through a Param or Heap base, at any depth (`p.next.f = &local`);
- an argument of a `List`/`Map`/`Set`/`Stack`/`Queue` mutator
  (`push`, `set`, `put`, `add`, `enqueue`) or a real `operator[]=` dispatch;
- `heap()`'s own operand, and an error variant's payload (`E::P(&local)`), both
  of which copy the value to the heap;
- an element of a `(&N)[]` parameter.

The check looks inside values: a struct or array local that holds a Stack
reference at a tracked path (`h.head` after `h.head = &n`, `a[0].next`) is
Stack-carrying at every sink above. A `defer`'s statement counts where it runs, at
every exit of its block, last-in-first-out, so `p = heap(n); defer p = &n;
return p;` is an error and `p = &n; defer p = heap(n); return p;` is accepted.

The fix is the intrinsic of 1.3:

```turflang
fn make(): &Node {
    mut n: Node = Node::default();
    n.val = 42;
    return heap(n);          // OK: a fresh, GC-heap-owned copy
}
```

A reference reached through an already-reference-typed parameter is Param
provenance and never flagged, however far it is passed along:

```turflang
fn firstFieldAddr(param: &Node): &int {
    return &param.val;      // OK: Param provenance
}
```

**Limits, by design.** There is no borrow checker, no lifetime syntax and no
general interprocedural summary: the pass is intraprocedural, plus one call fact
(a function whose every `return` is one reference parameter, unchanged, passes
that argument's provenance to its call: `return id(&local)` is an error). It
never reports a false positive: a cast into the raw-pointer world (`to ptr`,
`to T*`) leaves its tracking (`unsafe` is the programmer's own opt-out), an
element written at a run-time index is not looked into, and a call result whose
arguments may reach this frame is Unknown. Tests: `tests/39_nullable_references/`
(`fail_escape_*`, `fail_return_address_of_local.tr`,
`fail_push_local_ref_into_list.tr`, and the `escape_*_ok.tr` files).

### 1.10 Manual memory: use after free and double free

A raw pointer obtained from an allocator can be released, and `AliasAnalysisPass`
(`src/analysis/AliasAnalysis.cpp`, run before the nullability and escape passes
in the per-function sequence) rejects, within one function, a use of a pointer
after it was freed (E018) and a second free of the same allocation (E017). Which
functions allocate and which release is declared, never guessed from a name: a
function says `@ownership(acquires)` (its result is a fresh allocation) or
`@ownership(releases: p)` (the call frees what its parameter `p` holds).
`lib/std/mem/mem.tr` declares `malloc`/`calloc`/`realloc`/`alloc` and `free` this
way, and a user's own arena functions are understood identically. Tests:
`tests/05_arrays_pointers_ffi/fail_use_after_free.tr`, `fail_double_free.tr`,
`16_alias_analysis_shadowing.tr`.

---

## Part 2 — Internal reference (compiler implementation)

Pipeline position: `Lexer → Parser → Resolver → TypeChecker → Codegen`, with the
CFG-based flow passes (`src/analysis/`) run per function from
`FuncDefExprAST::codegen()` (`src/codegen/CodegenFunc.cpp`) and from the query
facade (`runFunctionFlowPasses()`, `src/analysis/Analysis.cpp`), after the
TypeChecker and before real code generation. The `unsafe { }` gate is enforced
entirely in the **TypeChecker**; the Resolver and Codegen barely know `unsafe`
exists. Reference safety (null and escape) is enforced in the flow passes.

### 2.1 The type hierarchy

`src/sema/Types.h`:

```
TurfType
 ├─ OpaquePointerType           — `ptr`
 ├─ TurfPointerType             — `T*`
 │   ├─ TurfReferenceType       — `&T`
 │   └─ TurfNullableReferenceType — `&?T`
 └─ ... (everything else)
```

Both safe-reference types are **subclasses of `TurfPointerType`**. At the LLVM
level all three are the same opaque pointer, so every
`dynamic_cast<const TurfPointerType *>` in Codegen (auto-deref field access,
argument passing, address-of caching, pointer casts) matches a `&T` or `&?T` the
way it matches a `T*`, and `isPointer()` is true for all of them. The safe/raw
distinction lives in two predicates on the `TurfType` base:

```cpp
virtual bool isPointer() const { return false; }
virtual bool isReference() const { return false; }          // true only for &T
virtual bool isNullableReference() const { return false; }  // true only for &?T
```

`TurfNullableReferenceType` is a **sibling** of `TurfReferenceType`, not a
subclass: `&?T` does not carry `&T`'s non-null contract, so `isReference()` stays
false for it. That one choice makes most pointer-widening rules in
`typesCompatible()` apply to `&?T` unchanged (2.5.4). `getName()` prints `&T`,
`&?T`, and `&(N[2])` for a reference to an array (so it never reads like the
array of references `(&N)[2]`).

### 2.2 Lexing and parsing

**Lexer.** One keyword token for the gate, `TOK_UNSAFE`. (`module` after it is an
ordinary identifier, 2.8.)

**`&T` and `&?T` as a type.** `ParseType()` (`src/frontend/ParserCore.cpp`) has a
leading-`&` branch, symmetric to the leading-`!` branch for error unions: consume
`&`, look for a `?` (one token of lookahead; `?` has no other meaning in the
grammar), recurse for the pointee, wrap in `TurfReferenceType` or
`TurfNullableReferenceType`. It is unambiguous with the expression-level `&`
(bitwise AND and address-of) because `ParseType()` is only ever called from type
positions. An array of references needs parentheses, `(&N)[4]`, because `&N[4]`
is a reference to an array; `ParseType()` handles that form too.

**`unsafe { }` as a block.** `ParseUnsafeExpr()` (`src/frontend/ParserStmt.cpp`)
consumes `unsafe` and delegates to `ParseUnsafeBlockBody()`, which requires `{`
and calls the ordinary `ParseBlock()`. `ParseBlock()` already accepts nested
`struct`/`fn`/`enum`/`machine` declarations as statements, which is what makes
`unsafe { struct Node { ... } }` legal inside a function with no extra grammar.
It is reached from `ParsePrimary()`, from `ParseBlock()`'s statement-keyword list,
and from the top-level loops below.

**Top-level `unsafe { struct ... }` is flattened at parse time.** Several places
treat a file's top level as a **flat list** of declaration-shaped nodes found by
`dynamic_cast`, not by recursive walk: `Resolver::hoistTopLevel()`
(`src/resolve/ResolverCore.cpp`) registers every struct/enum/machine/error name
before bodies are resolved; the multi-pass driver in `src/driver/main.cpp` runs
separate passes over the list for struct types, function signatures and function
bodies; and the TypeChecker's resumable per-declaration recovery
(`checkAll(Nodes, RecoverPerDeclaration)`). A real `UnsafeBlockExprAST` wrapper
in that list would hide the structs inside it from all three. So
`ParseFile()` (`src/frontend/ParserModule.cpp`) and the driver's two mirrored
top-level parse loops (the pre-pass and the main pass) call
`AppendFlattenedTopLevelDecl()`, declared in `src/frontend/Parser.h`:

```cpp
void AppendFlattenedTopLevelDecl(std::unique_ptr<ExprAST> AST,
                                 std::vector<std::unique_ptr<ExprAST>> &Target,
                                 bool RecordForSandbox);
```

If `AST` is an `UnsafeBlockExprAST`, its children (`takeChildren()`) are moved
out and pushed onto `Target` individually; the wrapper is discarded. Each
spliced child is registered by pointer identity in a per-compile set
(`UnsafeTopLevelDecls`, cleared with the other per-compile parser state), and
`WasDeclaredInsideTopLevelUnsafe(const ExprAST *)` is the read side the
TypeChecker's gate consults. Inside a function body or ordinary nested block,
`unsafe { }` stays a real nested node, because those contexts are walked
recursively by every pass.

### 2.3 AST

**`UnsafeBlockExprAST`** (`src/frontend/ASTStmt.h`) is shaped like `IfExprAST`
rather than plain `BlockExprAST`: it carries the `unsafe` keyword's
`SourceLocation` and a single `Body` child (always a `BlockExprAST`). `accept()`
dispatches to `ASTVisitor::visitUnsafeBlock()`. Two accessors exist to support
2.2: `BlockExprAST::takeExpressions()` and `UnsafeBlockExprAST::takeChildren()`.

**`AddressOfExprAST`** (`src/frontend/ASTExpr.h`) is parsed in `ParseUnary()`;
its addressability check is `codegenLValue()` in Codegen, and the TypeChecker
types the result `&T` (2.5).

### 2.4 Resolver

`Resolver::visitUnsafeBlock()` (`src/resolve/ResolverStmt.cpp`) is a one-line
delegation to the body, reusing `visitBlock()`'s scope bracket. The Resolver has
no unsafe-tracking state at all; `hoistTopLevel()` needs no special case because
the parse-time flattening (2.2) means it never sees an `UnsafeBlockExprAST` at
the top level.

**Generic substitution must rebuild the right pointer kind.** A generic
declaration's `NamedType("T")` placeholders become `TurfGenericParamType`s
(`substituteTypeParams()`, `src/resolve/ResolverCore.cpp`), and a monomorphization
replaces them with concrete types (`Monomorphize::substituteConcreteTypes()`,
`src/sema/Monomorphize.cpp`; the TypeChecker's `substituteReturnType()` in
`src/sema/TypeCheckerCall.cpp` is a thin wrapper over it). All three call the one
shared walker `TypeWalk::substitute()` (`src/sema/TypeWalk.cpp`), which checks
`TurfReferenceType` and `TurfNullableReferenceType` **before** the plain
`TurfPointerType` branch. Because both are subclasses of `TurfPointerType`, a
combined check would match them and silently rebuild a plain `T*`, turning a
generic `&T` or `&?T` parameter, field or return type into a raw pointer. Any new
code that reconstructs a `TurfPointerType` from a `dynamic_cast` match must make
the same distinction. `scripts/check/check_boundary.sh` check 6 enforces that
generic-parameter queries go through `TypeWalk`.

### 2.5 TypeChecker — the gate

State and helpers (`src/sema/TypeChecker.h`, `src/sema/TypeCheckerCore.cpp`):

```cpp
int UnsafeDepth_ = 0;

static bool isRawPointer(const TurfType *T) {
  return T && T->isPointer() && !T->isReference() && !T->isNullableReference();
}

bool isUnsafeGateExempt(const SourceLocation &Loc, const ExprAST *Node = nullptr) const;
bool checkUnsafeGate(const TurfType *T, SourceLocation Loc, const ExprAST *Node = nullptr);
void checkNoRawPointersInSignature(const FuncDefExprAST &Node);
```

#### 2.5.1 Exemption and enforcement

`isUnsafeGateExempt()` is true when any of:

1. `UnsafeDepth_ > 0`: lexically inside a (nested) `unsafe { }`.
2. `Node` was spliced from a top-level `unsafe { }`
   (`WasDeclaredInsideTopLevelUnsafe(Node)`).
3. The declaring file has the raw-pointer capability:
   `HasRawPointerCapability(Loc.Filename)` (`src/frontend/ParserModule.cpp`,
   exported through `Parser.h` so the LLVM-free TypeChecker header need not link
   `llvm::sys::fs`). It requires the file to have parsed a leading
   `unsafe module;` (recorded in a set keyed by canonical path) *and* that the
   declaration be honored: under `TurfInstallRoot()`, or allowed for its package
   in a package build (`PackageInfo::AllowUnsafe`), or `--allow-unsafe-modules`
   (`g_AllowUnsafeModules`) otherwise.

`checkUnsafeGate()` raises E130 (`Raw pointer type '...' cannot be used outside an
'unsafe' block.`, banner `Unsafe pointer use!`) when `T` is a raw pointer and the
location is not exempt.

`UnsafeDepth_` is incremented and decremented around the body in
`visitUnsafeBlock()`. It is an `int` so nested blocks stay authorized, and it is
reset to `0` in `resetForRecovery()`: a `TypeError` raised inside an `unsafe { }`
unwinds the C++ stack by throwing `RecoverUnwind` (`src/frontend/Lexer.h`), which
skips the matching `--UnsafeDepth_`, and without the reset every later top-level
declaration in the file would be wrongly exempt. `visitUnsafeBlock()` also types
the node as its body's type, which is what makes `unsafe { expr; }` usable as a
value.

#### 2.5.2 The gate call sites

| Gate | Site | What is checked |
|---|---|---|
| Declaration | `visitVarDecl` (`TypeCheckerDecl.cpp`) | the declared type |
| Cast | `visitCast` (`TypeCheckerExpr.cpp`) | destination type first, then operand type |
| Dereference (field access) | `visitFieldAccess` (`TypeCheckerCall.cpp`) | the base expression's type; skipped for a base named `self` |
| Dereference (indexing) | `visitArrayAccess` (`TypeCheckerCall.cpp`) | the base expression's type; same `self` skip |
| Dereference (method call) | `visitMethodCall` (`TypeCheckerCall.cpp`) | the receiver's type, once the call resolves to a method (or a UFCS callee that loads the receiver); same `self` skip |
| Struct field | `visitStructDecl` (`TypeCheckerDecl.cpp`) | the first raw field; the struct's own `Loc` is checked for exemption |
| Error payload field | `visitErrorDecl` (`TypeCheckerDecl.cpp`) | the first raw field, as for a struct |

The cast site is the one place a raw/safe conversion happens:

```cpp
if (!checkUnsafeGate(Node.getDestTypePtr().get(), Node.getLoc(), &Node))
  checkUnsafeGate(OperandTy.get(), Node.getLoc(), &Node);
```

The destination is checked first, so `raw to &T` (safe destination, raw operand)
is gated on the operand, and `ref to T*` (raw destination) on the destination.
`visitCast` also carries two reference-specific rules: an operand that is a
`&?T` cast to `&T` must be a plain variable (otherwise E082, 1.8), and making a
reference out of anything that is not a reference to a compatible pointee is
gated as `Unsafe reference conversion!` (E130, 1.2). The struct-field site stops
at the first raw field: the fix (wrap the declaration) is the same whichever
field triggered it.

#### 2.5.3 `self` and the signature rule

`checkNoRawPointersInSignature()` is called unconditionally from `visitFuncDef()`,
not gated by `UnsafeDepth_` (a signature is not inside the block's lexical scope,
even when the whole declaration is wrapped). It returns early only for a file with
the capability; otherwise it iterates the parameters and the return type and
raises `Raw pointer in function signature!` for any `isRawPointer()` match.
`extern fn` goes through `visitExternFuncDecl()` and is unaffected.

This check and `visitFieldAccess`/`visitArrayAccess` special-case `self` by
**name**:

```cpp
if (Param.Name == "self") continue;                       // signature rule
bool IsSelfBase = BaseVar && BaseVar->getName() == "self"; // dereference gate
```

`self` is still synthesized as an ordinary raw `TurfPointerType` to the struct
(`ParserDecl.cpp`) and is not retyped to `&Self`; FREEZE.md §6.6 says to leave it.
Retyping would disturb method-mangled names and the many `dynamic_cast`
assumptions in tested code for a purely cosmetic gain. Name-based exemption is
safe because `self` is a reserved token (`TOK_SELF`): a user cannot declare a
variable called `self`.

It is also why a method returning `self` by reference (the chaining example in
1.4) needs its own `unsafe { }`: converting `self`'s raw type to the declared
`&Counter` return type is the raw-to-safe cast, whatever `self`'s exemption from
the dereference gate.

#### 2.5.4 `typesCompatible()` rules (`src/sema/Types.cpp`)

The pointer rules, in order:

```cpp
// Rule 1 (null): compatible with ptr, void*, T* and &?T; NOT with &T
auto isPointerLikeStrict = [](const TurfType *T) {
  return (T->isPointer() && !T->isReference()) ||
         T->isNamed("ptr") || T->isNamed("void*");
};

// Rule 2 (opaque ptr/void* <-> T*): ptr -> &T excluded; &T -> ptr allowed
if (isOpaquePtr(From) && To->isPointer() && !To->isReference()) return true;
if (isOpaquePtr(To) && From->isPointer()) return true;

// Rule 2.5: &T -> a concrete T2* or &?T2 with the same pointee
if (const auto *FromRef = dynamic_cast<const TurfReferenceType *>(From))
  if (!To->isReference())
    if (const auto *ToPtr = dynamic_cast<const TurfPointerType *>(To))
      if (referencePointeeGenericMatch(FromRef->getPointee().get(),
                                       ToPtr->getPointee().get()))
        return true;

// Rule 2.6: &?T -> a concrete T2* (not &T, not &?T)
if (const auto *FromNRef = dynamic_cast<const TurfNullableReferenceType *>(From))
  if (!To->isReference() && !To->isNullableReference())
    ...same pointee test...
```

The `!isReference()` guards matter because `&T` inherits `isPointer()`: without
them `null` and a bare `ptr` would satisfy the older rules by accident, which is
backwards for a type whose purpose is a non-null guarantee. The pointee test is
`referencePointeeGenericMatch()` (`isEquivalent()` plus the narrow
generic-parameter leniency the candidate-matching rules grant), not a recursive
`typesCompatible()`, so `&int -> double*` cannot ride the int-to-double numeric
widening rule. No rule exists for raw `T*`/`ptr -> &T` or for `&?T -> &T`: both
fall through to `false`, which forces the explicit `to` cast (inside `unsafe { }`
for a raw source, narrowing-checked for a `&?T` source). A generic parameter
`T` accepts a `&T` argument by the existing pointer-to-generic rule, since `&T`
casts to `TurfPointerType` by inheritance.

### 2.6 Codegen

Nearly nothing. `UnsafeBlockExprAST::codegen()` (`CodegenControlFlow.cpp`) is a
pass-through to its body, since the TypeChecker has already rejected anything the
gate disallows. `getLLVMType()`'s existing `TurfPointerType` branch covers both
reference types, so `T*`, `&T` and `&?T` all lower to an opaque `ptr`;
`AddressOfExprAST::codegen()` returns the operand's lvalue address through
`codegenLValue()` whichever label the TypeChecker gave the result.

The control-flow analysis (`src/analysis/CFG.cpp`) has an explicit case for
`UnsafeBlockExprAST` that delegates to the body like `BlockExprAST`. Without it a
`return` inside `unsafe { }` is invisible to the "does every path return?" check,
which falls into its generic "ordinary statement" bucket and reports a false
missing return.

### 2.7 Known gaps

- **`self` stays a raw pointer internally** (2.5.3).
- **Codegen's own `getExprType()`** (`CodegenCore.cpp`), a second, parallel type
  inference used for Codegen bookkeeping, rebuilds a plain `TurfPointerType` in
  at least two places (its generic-struct-field substitution and its
  `AddressOfExprAST` type cache). Every consumer of its result asks only whether
  the type is pointer-shaped (`isPointer()`, still true), never whether it is a
  safe reference, so the divergence is inert. A change that makes Codegen ask
  `isReference()` would have to fix both.
- `ptr` and `T*` are the only raw-pointer spellings; there is no
  `unsafe_ptr<T>`.

### 2.8 Nullable references and the capability, by pass

- **Types** (`src/sema/Types.h`, `Types.cpp`): `TurfNullableReferenceType` as in
  2.1; Rule 2.6 in 2.5.4. `isRawPointer()` excludes `isNullableReference()`, so
  `&?T` is exempt from the raw gate and its safety is a separate mechanism.
- **Parser**: `ParseType()` as in 2.2. The `unsafe module;` pragma is parsed by
  `ParseLeadingUnsafeModuleDecl()` (`ParserModule.cpp`) at the places a file's
  first token is seen: `ParseFile()` for imported files, and the driver's
  pre-pass and main-pass entry-file loops. With one token of lookahead it must
  consume `unsafe` before it can tell the pragma from a genuine top-level
  `unsafe { }` block, so on the second case it finishes parsing the block itself
  (`ParseUnsafeBlockBody()`) and hands it back for flattening.
- **TypeChecker**: `HasRawPointerCapability()` is the single capability query,
  called from `isUnsafeGateExempt()` and `checkNoRawPointersInSignature()`.
  `rejectNullableNonVariableUse()` and `rejectDeferredNullableUse()` implement the
  non-variable and deferred rules of 1.8.
- **`NullabilityPass`** (`src/analysis/Analysis.h`, `Analysis.cpp`): a forward
  dataflow over the CFG, structurally the same as `DefiniteAssignmentPass`: one
  `std::unordered_set<const VariableSymbol *>` of "proven non-null" variables per
  block, meet = intersection, and two states per variable (proven, or not). It
  tracks `&?T` locals and parameters (a parameter starts "not proven"). The
  per-statement work is `NarrowingVisitor`, which computes the sets that hold when
  a condition is true or false (`==`/`!=` against `null`, threaded through `&&` and `||` the way they short-circuit; `!` narrows nothing),
  updates a tracked variable on assignment (`null` clears it; `&expr` and a `&T`
  variable set it; another tracked variable copies its state; any other shape
  clears it), and, on the reporting walk, raises E081 for a field access or method
  call through an unproven `&?T` and E082 for an unproven `to &T` cast. The
  dataflow runs silently to a fixed point first and reports only afterwards, so an
  unconverged set never produces a user-facing diagnostic. A `recover run { }`
  block is analysed with its own entry set.
- **Generics**: `TypeWalk::substitute()` as in 2.4.
- **Codegen**: nothing (2.6).

### 2.9 Escape analysis, by pass

- **`EscapePass`** (`src/analysis/Analysis.h`, `Analysis.cpp`) is a forward
  dataflow beside `NullabilityPass`. Its state per block is a map from a *place*
  (a variable, or a path inside a value variable: `.field`, `[k]`, `[*]`) to a
  `RefProvenance` (`Stack`, `Param`, `Heap`, `Unknown`, `MayStack`). The meet keeps
  an entry where every reached predecessor agrees, except that `Stack` on any path
  makes the place `MayStack` (`mergeProv()`); a predecessor not yet computed is
  skipped, not met as empty, since an empty map would turn every provenance
  Unknown around a loop and silently disable the check. The transfer function runs
  each statement through `EscapeCheckVisitor`, an `ASTChildWalker` (so a node it
  does not special-case is recursed into rather than skipped: this pass enforces
  F-MEM-5, and a skipped node is a dangling-reference hole). Provenance of an
  expression comes from `classifyProvenance()`, a deliberately small closed set of
  shapes: `&expr`, a variable, `heap()`, a call (via
  `ResolverFunctionSymbol::ReturnsParam`, or long-lived when no argument can reach
  this frame, or a standard-collection element), a field or element read, a cast
  (a raw-pointer destination leaves tracking), and a `give` expression (its
  branches merged). Standard collections are recognised by the receiver's struct
  name (`List`, `Map`, `Set`, `Stack`, `Queue`), stripped of its module
  qualifier, together with the mutator names `push`, `set`, `put`, `add`,
  `enqueue`; **a new collection or mutator needs an entry added by hand** or the
  pass will not check it. A statement list includes each `defer` where it runs,
  at its scope's exits (`getStatementsWithDeferredRuns()`, built by the CFG
  builder's `emitDeferredRuns()`), and the dataflow reports only after reaching a
  fixed point.
- **Diagnostics** (`src/diagnostics/ErrorsMemory.h`, `ErrorCodes.h`):
  `DanglingReferenceEscapeError` (E086, memory-error category), parameterised by
  a short sink description so one class covers every sink.
- Nothing upstream changed for this pass: every fact it needs (`IsParameter`, a
  type's pointer or array shape, `ArrayAccessExprAST::ResolvedIndexSetTarget` for
  an operator `[]=`, `FuncCallExprAST::isIntrinsic(BareIntrinsic::Heap)`) is on the
  AST already.
