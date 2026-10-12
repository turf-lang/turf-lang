# FFI, `unsafe {}`, and Pointers

This page covers calling into C libraries with `extern fn`, the `unsafe {}`
block, safe references (`&T` / `&?T`), and raw pointers (`ptr` / `T*`) —
Turflang's whole low-level/interop surface. In a hurry? Jump straight to
[§5, "Which pointer type do I use?"](#5-which-pointer-type-do-i-use) for a
quick decision guide.

See also: `docs/architecture/pointers_and_references.md` for the
compiler-internals view (TypeChecker gate implementation, LLVM
representation, file:line references) if you need to go deeper than "how
do I write this."

---

## 1. `extern fn` — calling C functions

### Syntax

```turflang
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;
extern fn printf(fmt: string, ...): int;      // variadic, `...` must be last
extern from "m" fn sqrt(val: double): double; // links against libm (-lm)
```

- `extern fn name(param: type, ...): returnType;` — always terminated by
  `;`, never a body.
- An optional `from "libname"` goes right after `extern`, before `fn`. It
  tells the linker to add `-l<libname>` (`extern from "m"` → `-lm`). Plain
  `extern fn` needs no extra linker flag — it's assumed to resolve against
  libc.
- The return type annotation is mandatory, same as an ordinary `fn`.
- A parameter's type can never be bare `void`, but **`void*` is allowed**
  — it's the standard opaque-pointer FFI idiom:

  ```turflang
  extern fn free(ptr: void*): void; // OK
  extern fn bogus(x: void): void;   // ERROR — bare void parameter
  ```

### A realistic example — libm bindings wrapped by Turf code

```turflang
// lib/std/core/math.tr
extern from "m" fn sqrt(val: double): double;
extern from "m" fn pow(base: double, exp: double): double;

export immut PI: double = 3.14159265358979323846;

fn abs(val: double): double {
    // ...
}
```

Calling code then uses `math::sqrt(2.0)`, `math::abs(-5)` — the raw extern
bindings are typically wrapped by ordinary Turf functions in the same
module rather than exposed directly, though nothing stops you from calling
an `extern fn` straight from user code.

### Signature checking

Extern signatures are validated like any other declared type — an unknown
or misspelled type in an `extern fn` signature is a compile error, not
silently accepted.

**Re-declaring the same extern is fine; conflicting re-declarations are
not.** It's common for two different modules to independently declare the
same C function (e.g. `strlen` declared in both a strings module and a net
module) — as long as the signatures are identical, this compiles cleanly:

```turflang
// module A
extern fn strlen(s: string): int;
// module B
extern fn strlen(s: string): int;
// both imported by the same file — fine, identical signatures
```

But a different arity, parameter type, or return type for the same extern
name across declarations is a hard error.

### `extern fn` and the raw-pointer gate

An `extern fn`'s own signature is **exempt** from the raw-pointer `unsafe`
gate — this is exactly what lets FFI bindings use `ptr`/`T*`/`void*`
freely:

```turflang
extern fn malloc(size: int): ptr; // fine, no unsafe needed for the declaration itself
```

What's still gated is *using the result* in ordinary code — binding it to
a variable is a "declaration" use (see [§3](#3-the-four-gated-uses)):

```turflang
unsafe {
    mutable p: ptr = malloc(64); // binding the result to a variable IS gated
}
```

### Gotcha: `extern fn` only parses at file/top level

> **Note:** `extern fn` cannot appear inside a function body. Writing:
>
> ```turflang
> fn main(): int {
>     extern fn puts(s: string): int;
>     return 0;
> }
> ```
>
> does **not** produce a clear "extern not allowed here" error. Instead,
> the parser consumes the `extern fn puts(s: string): int;` declaration and
> its trailing `;` as if it had ended the enclosing statement, and then
> chokes on whatever comes next — the actual diagnostic is
> `Expected ';' after statement.`, pointing at the *next* line (here,
> `return 0;`), not at the `extern` line itself. If you see this error
> immediately after a line that looks fine, check whether an `extern fn`
> snuck into a function body — move it to file/top level instead.

### Linking external libraries

`extern from "name"` (and `turfc --link name`) adds `-lname` to the link. What that
needs on the machine depends on the library:

| The program names | How it is linked | What the machine needs |
|---|---|---|
| nothing, or only the C runtime (`c`, `m`; on Linux also `pthread dl rt util`; on Windows also `ucrt kernel32 user32 advapi32 shell32 ws2_32`; on macOS `System`) | by the linker built into `turfc`, statically, against the C runtime in the release archive (`lib/rt/<platform>/`) | nothing. No C compiler, no libc development files |
| any other library (`"z"`, `"sqlite3"`, `"LLVM"`) | by the **system C compiler** (`cc`; `gcc` on Windows) | a C compiler **and** that library's development files |

The release archive bundles no third-party library. A program that binds one is
a normal C-FFI program: you install the library, as you would for any C program
that uses it.

**Linux and macOS.** Install the library's `-dev`/`-devel` package (or `brew install`),
which provides the `.so`/`.dylib`/`.a` the linker searches for. A library in a
non-default directory is found by adding that directory to `LIBRARY_PATH`.

**Windows.** Install a MinGW-w64 `gcc` (for example through MSYS2) and put it on
`PATH`; `turfc` runs the command `gcc`. The library must be one MinGW's linker can
link: an import library or static archive that the name resolves to (`-lfoo` finds
`libfoo.a`, `libfoo.dll.a` or `foo.lib`), with the matching DLL beside the program or
on `PATH` when it runs. Point `LIBRARY_PATH` at its directory if it is not in the
compiler's own. The library name you write in `extern from "..."` must be the name
that import library actually has, which a vendor may spell differently from the
library's Linux name. (`showcase/llvm_ffi_selfhost_demo.tr` writes `from "LLVM"`, the
Linux `libLLVM`; on Windows check the name of the import library in the LLVM you
installed before expecting it to link.)

`turfc` says which of these is wrong:

- *`Error: linking needs a C compiler, and 'gcc' was not found on PATH.`*: the
  program names a library from the second row (the message lists it) and there is no
  compiler. Install one, or remove the declarations that name the library.
- *`Linking failed ... This program links against "foo", which Turf does not
  bundle.`*: the compiler ran and the linker could not use the library. The
  linker's own line above it (`cannot find -lfoo`) is the specific cause; the fix is
  its development files, or `LIBRARY_PATH`.

`turfc --version` shows which path a given install takes (`linker embedded-lld` with a
`bundled-runtime` line is the first row; `linker system-cc` means every program is
linked by `cc`). `TURF_LINKER=system` forces the second row for any program.

---

## 2. `unsafe { }`

Raw-pointer operations (see [§3](#3-the-four-gated-uses)) must be
lexically wrapped in an `unsafe {}` block.

### Where it's legal

**As a statement inside a function body** — the common case:

```turflang
fn readIntoLocal(): int {
    unsafe {
        mutable p: ptr = null;
        return 0;
    }
}
```

**Nested inside other blocks** (`if`, `while`, another `unsafe {}`) — this
is harmless; nesting stays authorized throughout.

**Wrapping a top-level declaration** (`struct`, `fn`, `enum`, `machine`):

```turflang
unsafe {
    struct Node {
        val: int,
        next: Node*
    }
}
```

This is required for any struct with a raw-pointer field — the **entire**
declaration must be inside `unsafe {}`, regardless of whether the field is
ever actually dereferenced anywhere.

### It's an expression

`unsafe {}` takes on the value of its last statement, exactly like an
ordinary `{}` block, so it can be used inline inside a larger expression:

```turflang
test::assert(unsafe { n1.next.val; } == 2, "field access through a raw pointer base");
```

### `unsafe {}` doesn't change control flow

A `return` written inside `unsafe {}` still returns from the *enclosing
function* — `unsafe {}` is not its own control-flow scope, only a
type-checking gate.

---

## 3. `&T` — safe references

```turflang
mutable val: int = 42;
mutable ref: &int = &val; // &expr always produces &T
```

- `&expr` (address-of) **always** produces a `&T`, never a raw `T*`. The
  operand must be an addressable lvalue (a named variable, struct field, or
  array element) — `&(x + 1)` is rejected.
- `&T` supports `.field` / `.method()` directly, with no separate
  dereference syntax needed — same as `T*`. Turflang has no `*p`
  dereference operator at all, for either pointer kind.
- **`null` is never compatible with `&T`**, in either direction — not as an
  initializer, not as a comparison target. This is the entire reason `&T`
  exists: it's a compiler-guaranteed non-null pointer.

  ```turflang
  mutable r: &int = null; // ERROR — 'null' is never assignable to a '&T'
  ```

- `&T` never needs `unsafe {}` to declare, pass, or return — only genuinely
  raw pointer types are gated.

### Widening is one-way

- `&T → T*` (or `ptr`) is **always implicit**, no `unsafe {}` needed — a
  reference is non-null by construction, so widening it to a raw pointer
  can't lose anything:

  ```turflang
  mutable ref: &int = &val;
  mutable raw: int* = ref; // implicit, no cast needed
  ```

- `T* → &T` is **never implicit** — it requires an explicit `to` cast,
  which itself must be inside `unsafe {}` (a raw pointer might genuinely be
  null, so converting it into a type that promises non-null is the unsafe
  direction):

  ```turflang
  unsafe {
      mutable back: &int = raw to &int;
  }
  ```

### A realistic example — linked list

`&T` can't represent a genuinely-nullable "next" pointer — that's what
`&?T` (LANG-002) is for, and it needs no `unsafe {}` at all:

```turflang
struct Node { val: int, next: &?Node } // next is genuinely nullable

fn Node.new(val: int): void {
    self.val = val;
    self.next = null;   // plain field write
}

fn sum_list(head: &?Node): int {
    mutable current: &?Node = head;
    mutable total: int = 0;
    while (current != null) {          // narrows `current` to non-null...
        total = total + current.val;   // ...so this compiles
        current = current.next;
    }
    return total;
}
```

`raw`/`T*` still work for this too (see the `Node*` version just below), but
`&?T` is the safe, checked, idiomatic choice — reach for `T*` only when a
raw, unchecked pointer is genuinely what you need (usually: FFI).

### Raw-pointer version, for comparison

```turflang
unsafe {
    struct Node { val: int, next: Node* } // next is genuinely nullable
}

fn Node.new(val: int): void {
    self.val = val;
    unsafe { self.next = null; }
}

// A plain function may never take/return a raw pointer -- &Node instead.
fn sum_list(head: &Node): int {
    unsafe {
        mutable current: Node* = head;      // &Node -> Node*: no cast needed
        mutable total: int = 0;
        while current != null {
            total = total + current.val;
            current = current.next;
        }
        return total;
    }
}
```

---

## 4. Raw pointers: `ptr` and `T*`

| Type | Spelling | Nullable | Pointee known | Legal where |
|---|---|---|---|---|
| Opaque pointer | `ptr` | yes | no | inside `unsafe {}`, or in a file that declares the raw-pointer capability (§4a) |
| Typed raw pointer | `T*` | yes | yes | inside `unsafe {}`, or in a file that declares the raw-pointer capability (§4a) |
| Safe reference | `&T` | no | yes | anywhere |
| Nullable safe reference | `&?T` | yes, checked | yes | anywhere |

### §4a. The raw-pointer capability (LANG-002/STD-001)

A file's own first top-level statement may be:

```turflang
unsafe module;
```

This grants the file every raw-pointer use §4's table lists, without
wrapping each one in `unsafe {}` — exactly what `lib/std`'s own modules do.
It's honored unconditionally for files under the compiler's resolved
install root (that's how `lib/std` gets it for free); anywhere else, it
also needs the `--allow-unsafe-modules` CLI flag, so a plain `turf
myfile.tr` never silently grants a capability nobody asked for. This is
also what makes a C callback possible from user code — `fn cmpints(a: ptr,
b: ptr): int32` (what `qsort`'s comparator needs) is otherwise rejected in
*any* non-`extern` function's signature, unconditionally, `unsafe {}` or
not.

### The four gated uses

Each of these requires the raw pointer type to be lexically inside
`unsafe {}`:

**Declaration:**

```turflang
mutable p: ptr = null; // ERROR outside unsafe {}
```

**Cast** (either direction):

```turflang
mutable asRaw: int* = ref to int*; // ERROR outside unsafe {}
```

**Dereference** (field access through a raw base):

```turflang
n1.next.val; // ERROR outside unsafe {} when n1.next: Node*
```

**Struct field** — the whole declaration, not just later uses of the field:

```turflang
struct Node {
    val: int,
    next: Node* // ERROR: this whole struct decl must be inside unsafe {}
}
```

The diagnostic for all four is the same shape:

```
Raw pointer type '<T>' cannot be used outside an 'unsafe' block.
hint: Wrap this in 'unsafe { }', or use a safe reference ('&T') instead
      if you don't actually need a possibly-null raw pointer.
```

### The one thing `unsafe {}` can never fix

**A plain function's parameter or return type can never be a raw pointer —
`unsafe {}` cannot make this legal**, because a function's *signature*
isn't inside the block's lexical scope at all, even if the whole
declaration is textually wrapped:

```turflang
unsafe {
    fn takesRaw(p: ptr): int {   // still an ERROR
        return 0;
    }
}
```

```
Function 'takesRaw' cannot use raw pointer type 'ptr' for parameter 'p'.
hint: Raw pointer types are never allowed in an ordinary function's
      signature, even inside 'unsafe { }' — use '&T' instead. Only
      'extern fn' bindings and the standard library may use raw pointers
      here.
```

Use `&T` in the signature instead, and convert to/from a raw pointer inside
the function body under `unsafe {}` if you need to.

### The three exemptions

Raw pointers are allowed with no `unsafe {}` at all in exactly three
places:

1. **A file that has declared the raw-pointer capability** (§4a) —
   `lib/std`'s own modules all do this now (`unsafe module;` as their first
   line), which is why they read as if raw pointers were unconditionally
   exempt there; the exemption is really the declaration, not the path.
2. **`extern fn` declarations** — an FFI binding's own parameter/return
   types can freely use `ptr`/`T*`/`void*` (see [§1](#1-extern-fn--calling-c-functions)).
   Only *binding a call's result to a variable* in ordinary user code is
   still gated.
3. **The implicit `self` parameter** — `self` is exempt from both the
   signature check and the dereference gate. It's still an ordinary raw
   pointer under the hood, but it's a reserved keyword, so no user code can
   shadow it, making the exemption safe. If you want to *return* `self` as
   a `&T` (for method chaining, say), that conversion still needs its own
   `unsafe {}` around the `return` — the exemption only covers reading
   through `self`, not casting it to a safe type.

### Known gaps (not bugs, just not gated yet)

- Indexing through a raw pointer (`p[0]` where `p: int*`) is not gated.
- Calling a method through a raw pointer receiver is not gated — only
  reading/writing a *field* through one is.

---

## 5. Which pointer type do I use?

A quick decision guide, now that there are four. All four compile to the
*identical* LLVM value (an opaque pointer) — the differences below are
purely frontend bookkeeping, zero runtime cost.

| Type | Nullable? | Checked? | Needs `unsafe`/capability? | Reach for it when... |
|---|---|---|---|---|
| `&T` | no, ever | — (guaranteed) | no | **Default choice.** Any "pass by reference" where you know it's never "nothing" — ordinary reference parameters, `self`, returning a reference into something the caller owns. |
| `&?T` | yes | yes | no | Something might legitimately be "nothing" — a linked list's `next`, a tree's `left`/`right`, an optional back-reference. This is almost always the right answer for "I need a nullable pointer" now. |
| `T*` | yes | no | yes | Genuine low-level/unchecked work: wrapping a C API directly (`lib/std/mem.tr`, `net.tr`), or a C callback's own parameters (`fn cmpints(a: ptr, b: ptr): int32` for `qsort`). Reach for it only when you specifically need *unchecked*, not just *nullable*. |
| `ptr` | yes | no | yes | The untyped boundary of a C API that itself deals in `void*` (`malloc`'s return, a generic handle) — cast to a concrete `T*` with `to` as soon as you know what it actually is. |

**Rule of thumb:** start with `&T`. The moment you need "or nothing," reach
for `&?T`, not `T*` — `&?T` gives you the same nullability with compile-time
checking and no `unsafe {}` ceremony anywhere. Only drop to `T*`/`ptr` for
real FFI/C-interop, or a rare case where you need something unchecked for
performance and know exactly what you're doing.

### How they convert into each other

Things only ever get **more permissive** (safer → rawer) for free; going
the other way (rawer → safer) always needs an explicit `to` cast, gated by
*something* — `unsafe {}` if the source is genuinely raw, or
`NullabilityPass` proving you narrowed it if the source is `&?T`:

```
&T  ──implicit──▶ &?T   (a non-null ref is trivially valid as "maybe null")
&T  ──implicit──▶ T*    (widening to raw is always sound)
&?T ──implicit──▶ T*    (same — the LLVM value never changes)
&?T ──implicit──▶ ptr   (same)
&?T ──explicit, checked──▶ &T   (`p to &T`, but ONLY where narrowed — E082 otherwise)
T*  ──explicit, unsafe──▶ &T    (`p to &T` inside `unsafe {}` — the compiler can't prove it)
ptr ──explicit, unsafe──▶ T*    (`p to SomeType*` inside `unsafe {}`)
int, string, &U ──explicit, unsafe──▶ &T   (forges an address — E130 outside `unsafe {}`)
```

Any other cast that produces a reference, from something that is not
already a reference to the same type (`0 to &N`, `s to &N`, `r to &M` with
`r: &N`), makes an address the compiler knows nothing about, so it is
E130 outside `unsafe { }` as well.

### Do I need `--allow-unsafe-modules`?

**No, almost certainly not.** It's only relevant to §4a's capability
declaration, and only in one specific case:

| What you're doing | Need the flag? |
|---|---|
| Using `&T` / `&?T` anywhere | No, never |
| Wrapping a raw-pointer use in `unsafe {}` locally | No |
| Writing code that lives under the compiler's install root (e.g. `lib/std`) | No — the capability is free there |
| **Your own file** declares `unsafe module;` to get raw pointers file-wide | **Yes** |
| Running an already-compiled binary | No — this is a compile-time flag only |

If you're not writing `unsafe module;` yourself, this flag simply never
comes up.
