# FFI and `unsafe`

This page covers the part of TurfLang where the compiler stops checking for
you: calling C with `extern fn`, raw pointers (`ptr`, `T*`), `unsafe { }`,
`unsafe module;`, and the capability a project grants to its dependencies.

Safe references (`&T`, `&?T`) are not unsafe and are not covered here. They
are described in [Memory and safety](memory_and_safety.md), which also
explains what the language guarantees in code that never uses anything on
this page. Everything below is outside those guarantees.

Contents: [1. `extern fn`](#1-extern-fn) · [2. Linking C libraries](#2-linking-c-libraries)
· [3. Raw pointers](#3-raw-pointers) · [4. `unsafe { }`](#4-unsafe--)
· [5. `unsafe module;`](#5-unsafe-module) · [6. Capabilities in a project](#6-capabilities-in-a-project)
· [7. Converting between pointer types](#7-converting-between-pointer-types)
· [8. Callbacks](#8-callbacks) · [9. What stays your responsibility](#9-what-stays-your-responsibility)
· [10. Which pointer type do I use?](#10-which-pointer-type-do-i-use)

---

## 1. `extern fn`

An `extern fn` declares a function that is defined in C. It has a signature
and no body, and ends in `;`:

```turflang
extern fn strlen(s: string): int;
extern fn getpid(): int32;
extern fn abs(n: int32): int32;
extern from "m" fn sqrt(x: double): double;
extern fn dprintf(fd: int32, fmt: string, ...): int32;   // variadic

fn main(): int {
    printline(strlen("hello"));          // 5
    printline(sqrt(2.0));                // 1.41421
    printline(abs(-5 to int32));         // 5
    printline(getpid() > 0);             // true
    dprintf(1 to int32, "%d and %s\n", 42, "text");
    return 0;
}
```

- The name you declare is the C symbol that is linked. To bind a Turf name
  to a differently named symbol on some platform, use
  [`@ffi`](functions_and_modules.md#ffi).
- The return type is mandatory, as for any function.
- `from "lib"` right after `extern` adds `-llib` to the link (see
  [section 2](#2-linking-c-libraries)). Plain `extern fn` resolves against
  the C library.
- `...` marks a C variadic function and must be the last parameter.

### Types across the boundary

Declare each parameter and return type with the type the C function really
has:

| C type | Turf type |
|---|---|
| `long`, `long long`, `size_t`, `ssize_t`, `int64_t` | `int` (64-bit) |
| `int`, `int32_t` | `int32` |
| `double` | `double` |
| `char *` (a NUL-terminated string) | `string` |
| `void *`, any other pointer | `ptr`, `void*` or `T*` |
| `void` (return only) | `void` |
| a callback | a function type such as `fn(ptr, ptr): int32` ([section 8](#8-callbacks)) |

Getting this wrong is not diagnosed: C does not tell the compiler its
signature. A C `int` declared as Turf `int` reads garbage from the upper half
of the return register; that is the reason `int32` exists. Pass a struct to C
by pointer (`&s` widens to a pointer parameter, [section 7](#7-converting-between-pointer-types)).

A parameter can never be bare `void`; use `void*` for an opaque pointer:

<!-- doc-example: fails E112 -->
```turflang
extern fn bogus(x: void): void;
```

An unknown type in the signature is `E042`, like anywhere else.

### Redeclaring a function

Two declarations of the same C function are fine if their signatures are
identical, and an error (`E020`, "conflicts with an earlier declaration of the
same name") if they differ in arity, a parameter type or the return type. This
includes the functions the standard library already declares: `strlen` above
is also declared by `sys`, with the same signature, so it is accepted;
declaring `strlen` with a different return type is `E020`.

<!-- doc-example: fails E020 -->
```turflang
extern fn strlen(s: string): int;
extern fn strlen(s: string): int32;
```

### Raw pointers in an `extern fn`

An `extern fn` signature may use `ptr`, `T*` and `void*` freely, with no
`unsafe` around the declaration. What is gated is using the result in
ordinary code: binding it to a variable of raw-pointer type needs `unsafe`
([section 3](#3-raw-pointers)).

```turflang
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;

fn main(): int {
    unsafe {
        mut p: ptr = malloc(64);
        free(p);
    }
    return 0;
}
```

At a call to an `extern fn` only, a `string` argument is accepted where the
parameter is `ptr`, `T*` or `void*`, so a C function that takes a `void *` can
be handed text without a cast. Everywhere else a `string` is not a `ptr`.

```turflang
extern fn puts(s: ptr): int32;

fn main(): int {
    puts("hi");
    return 0;
}
```

### Declared inside a function

An `extern fn` may also be written inside a function body. It is registered
at that point, so it must come before its first use, like a local variable.
It is **not** scoped to the function: once the declaration has been reached
by the compiler, the name can be called from code that follows it in other
functions too. Declare the functions you share at the top of the file.

```turflang
fn main(): int {
    extern fn puts(s: string): int32;
    puts("local extern");
    return 0;
}
```

### Annotations on an `extern fn`

Annotations are written before the declaration (before `pub`, if any). Two
matter here, and both are described in
[Functions and modules: Annotations](functions_and_modules.md#4-annotations):

- [`@ownership(acquires)` and `@ownership(releases: p)`](functions_and_modules.md#ownership)
  tell the double-free and use-after-free check which of your `extern fn`s
  allocate and free. The check knows allocators only through this
  annotation: an unannotated `extern fn free` is not tracked.
- [`@ffi(linux: symbol)`, `@ffi(macos: ...)`, `@ffi(windows: ...)`](functions_and_modules.md#ffi)
  bind the declaration to a different C symbol per platform. They are valid
  on an `extern fn` only.

An annotation the declaration cannot carry (an unknown name, a bad argument)
is `E146`.

With `@ownership`, mistakes with raw memory become compile errors:

<!-- doc-example: fails E017 -->
```turflang
@ownership(acquires)
extern fn malloc(size: int): ptr;
@ownership(releases: p)
extern fn free(p: ptr): void;

fn main(): int {
    unsafe {
        mut p: ptr = malloc(8);
        free(p);
        free(p);          // E017: Double free detected for 'p'
    }
    return 0;
}
```

<!-- doc-example: fails E018 -->
```turflang
@ownership(acquires)
extern fn malloc(size: int): int*;
@ownership(releases: p)
extern fn free(p: int*): void;

fn main(): int {
    unsafe {
        mut p: int* = malloc(8);
        free(p);
        p[0] = 1;         // E018: you're using memory from 'p' after it was freed
    }
    return 0;
}
```

The check follows one function at a time. It does not see a pointer that
is freed by a callee, stored somewhere, or freed on only some paths (that last
case is a warning, "may already have been freed").

---

## 2. Linking C libraries

`extern from "name"` (and `turfc --link name`, see
[CLI](cli_and_diagnostics.md)) adds `-lname` to the link. What that needs on
the machine depends on the library:

| The program names | How it is linked | What the machine needs |
|---|---|---|
| nothing, or only the C runtime (`c`, `m`; on Linux also `pthread dl rt util`; on Windows also `ucrt kernel32 user32 advapi32 shell32 ws2_32`; on macOS `System`) | by the linker built into `turfc`, statically, against the C runtime in the release archive | nothing: no C compiler, no libc development files |
| any other library (`"z"`, `"sqlite3"`, `"LLVM"`) | by the **system C compiler** (`cc`; `gcc` on Windows) | a C compiler **and** that library's development files |

The release archive bundles no third-party library. A program that binds one
is a normal C-FFI program: you install the library, as you would for any C
program that uses it.

<!-- doc-example: skip needs the zlib development files on the machine -->
```turflang
extern from "z" fn zlibVersion(): string;

fn main(): int {
    printline(zlibVersion());     // for example 1.3.2
    return 0;
}
```

**Linux and macOS.** Install the library's `-dev`/`-devel` package (or
`brew install`), which provides the `.so`/`.dylib`/`.a` the linker searches
for. A library in a non-default directory is found by adding that directory
to `LIBRARY_PATH`.

**Windows.** Install a MinGW-w64 `gcc` (for example through MSYS2) and put it
on `PATH`; `turfc` runs the command `gcc`. The library must be one MinGW's
linker can link: an import library or static archive that the name resolves to
(`-lfoo` finds `libfoo.a`, `libfoo.dll.a` or `foo.lib`), with the matching DLL
beside the program or on `PATH` when it runs. Point `LIBRARY_PATH` at its
directory if it is not in the compiler's own. The name in
`extern from "..."` must be the name the import library actually has, which a
vendor may spell differently from the library's Linux name.

`turfc` says which of these is wrong:

- *`Error: linking needs a C compiler, and 'gcc' was not found on PATH.`*:
  the program names a library from the second row (the message lists it) and
  there is no compiler. Install one, or remove the declarations that name the
  library.
- *`Linking failed ... This program links against "foo", which Turf does not
  bundle.`*: the compiler ran and the linker could not use the library. The
  linker's own line above it (`cannot find -lfoo`) is the specific cause; the
  fix is the library's development files, or `LIBRARY_PATH`.

`turfc --version` shows which path an install takes (`linker embedded-lld`
with a `bundled-runtime` line is the first row; `linker system-cc` means every
program is linked by `cc`). `TURF_LINKER=system` forces the second row for any
program.

---

## 3. Raw pointers

| Type | Spelling | Nullable | Pointee known | Checked by the compiler |
|---|---|---|---|---|
| Opaque pointer | `ptr` | yes | no | no |
| Typed raw pointer | `T*` (`int*`, `Node*`, `void*`) | yes | yes | no |
| Reference | `&T` | no | yes | non-null, escape rules |
| Nullable reference | `&?T` | yes | yes | must be null-tested |

`ptr` and `T*` are plain machine addresses. Nothing checks that one is non-null,
points at something alive, is correctly typed, or stays inside the object it
points into. You read a field of one with `.`, and index it with `p[i]`;
there is no `*p`.

A raw pointer type can be used only where the compiler has been told you
accept that: inside an `unsafe { }` block ([section 4](#4-unsafe--)), in a
file that begins `unsafe module;` ([section 5](#5-unsafe-module)), or in an
`extern fn` signature. Anywhere else it is `E130` ("Unsafe pointer use!"):

```text
Raw pointer type 'ptr' cannot be used outside an 'unsafe' block.
Hint: Wrap this in 'unsafe { }', or use a safe reference ('&T') instead if you
don't actually need a possibly-null raw pointer.
```

Four kinds of use are gated:

<!-- doc-example: fails E130 -->
```turflang
extern fn malloc(size: int): ptr;

fn main(): int {
    mut p: ptr = malloc(8);       // a variable of raw-pointer type
    return 0;
}
```

<!-- doc-example: fails E130 -->
```turflang
fn main(): int {
    mut v: int = 1;
    mut r: &int = &v;
    mut raw: int* = r to int*;    // a cast to or from a raw pointer
    return 0;
}
```

<!-- doc-example: fails E130 -->
```turflang
struct Node {
    val: int,
    next: Node*                   // a raw-pointer field: the whole struct
}                                 // declaration must be inside unsafe { }
```

The fourth is reading or writing a field through a raw pointer
(`n.next.val` where `n.next` is a `Node*`), or calling a method on one
(`n.next.total()`), which needs the `unsafe` block around the access.

A struct with a raw-pointer field is written inside `unsafe { }`, wrapping the
whole declaration, whether or not the field is ever dereferenced:

```turflang
unsafe {
    struct Node {
        val: int,
        next: Node*
    }
}

fn main(): int {
    unsafe {
        mut n: Node = Node::default();
        mut m: Node = Node::default();
        m.val = 5;
        n.next = &m;               // a &Node widens to Node* by itself
        printline(n.next.val);     // 5
    }
    return 0;
}
```

For a link in a data structure that may be absent, prefer `&?Node`, which
needs no `unsafe`: see [Memory and safety](memory_and_safety.md#3-references-t-and-t).

### Raw pointers never appear in an ordinary function's signature

A function that has a body may not take or return a raw pointer, and `unsafe`
does not change that, because the signature is not inside the block. Use `&T`
and convert inside the body. The only exceptions are `extern fn` declarations,
the standard library, and a function in a file that declares
`unsafe module;` ([section 5](#5-unsafe-module)).

<!-- doc-example: fails E130 -->
```turflang
unsafe {
    fn takes(p: ptr): int {       // E130: Raw pointer in function signature!
        return 0;
    }
}

fn main(): int { return 0; }
```

```text
Function 'takes' cannot use raw pointer type 'ptr' for parameter 'p'.
Hint: Raw pointer types are never allowed in an ordinary function's signature,
even inside 'unsafe { }' -- use '&T' instead. Only 'extern fn' bindings and
files that begin with 'unsafe module;' may use raw pointers here.
```

### `self`

Inside a method, `self` is a raw pointer under the hood and may be read
through without `unsafe`. To hand it out as a `&T` (for chaining, say) you
convert it, and the conversion needs `unsafe`:

```turflang
struct Counter { n: int }

fn Counter.me(): &Counter {
    unsafe { return self to &Counter; }
}

fn main(): int {
    mut c: Counter = Counter::default();
    c.me().n = 3;
    printline(c.n);               // 3
    return 0;
}
```

---

## 4. `unsafe { }`

`unsafe { }` is a block that tells the compiler to allow the gated uses of
section 3 inside it. It does nothing else: no run-time effect, and no checking
is added or removed beyond that gate. Everything inside is still compiled by
the same rules, and the compiler still rejects type errors.

It can appear:

- as a statement in a function body, nested freely inside `if`, `while` and
  other blocks (nesting is harmless);
- around a top-level declaration (`struct`, `fn`, `enum`, `machine`), as in the
  `Node` example above;
- as an expression, whose value is its last statement, like any block:

```turflang
extern fn strlen(s: string): int;

fn main(): int {
    return unsafe { strlen("abcd"); };     // exit status 4
}
```

A `return`, `break` or `continue` inside `unsafe { }` leaves the enclosing
function or loop as it would anywhere else: `unsafe { }` is not a scope of its
own.

Inside the block, nothing is checked for you:

```turflang
fn main(): int {
    unsafe {
        mut a: int[3] = [1, 2, 3];
        mut p: int* = &a[0];
        printline(p[1]);          // 2
        printline(p[7]);          // compiles and runs: reads whatever is there
    }
    return 0;
}
```

An index through a raw pointer is not bounds-checked, a null raw pointer
is not diagnosed (reading through one is a crash, or at higher optimisation
levels anything at all), and the collector knows nothing about what you do
with a pointer after you cast it.

---

## 5. `unsafe module;`

A file whose first top-level statement is

```text
unsafe module;
```

has the raw-pointer capability: every gated use of section 3 is allowed in
that whole file without an `unsafe { }` around it, and its own functions may
take and return raw pointers. This is how the standard library is written,
and what a C callback written in Turf needs ([section 8](#8-callbacks)).

<!-- doc-example: skip a file with unsafe module; needs --allow-unsafe-modules -->
```turflang
unsafe module;
import mem;

struct Pair { a: int, next: Pair* }

fn main(): int {
    mut p: int* = mem::alloc(sizeof(int) * 4) to int*;
    p[0] = 7;
    p[3] = 9;
    printline(p[0] + p[3]);       // 16
    mem::free(p to void*);
    return 0;
}
```

Because it removes the compiler's checks for a whole file, the declaration is
honored only when someone has agreed to it:

- Files under the compiler's install root (the standard library) are always
  trusted.
- In a single-file compile, any other file needs `--allow-unsafe-modules` on
  the command line. Without it the file above fails with `E130`, at its first
  raw-pointer use:

  ```text
  $ turfc pair.tr
  ── error at line 4, col 1 [E130 · memory error]
    Unsafe pointer use!
    Raw pointer type 'Pair*' cannot be used outside an 'unsafe' block.
  $ turfc pair.tr --allow-unsafe-modules
  ```

  The flag is a compile-time flag; a compiled binary needs nothing.
- In a project, the flag does not apply and the manifest decides
  ([section 6](#6-capabilities-in-a-project)).

A file with `unsafe module;` that uses no raw pointer compiles with or without
the flag. If you never write `unsafe module;`, you never need the flag.

---

## 6. Capabilities in a project

In a project built with `turf build`, your own package's modules may declare
`unsafe module;` without any listing: you wrote them. A **dependency's**
`unsafe module;` is honored only if the root `turf.toml` lists that dependency:

```toml
[dependencies]
ffi = { path = "../ffi" }

[unsafe]
allow = ["ffi"]
```

Without the `[unsafe]` table the build fails with `E130` in the dependency's
file. Listing is not transitive (the root names every package that needs it),
a dependency cannot allow itself, and `--allow-unsafe-modules` does not apply
in a project. The details, including the errors for a bad name, are in
[Projects and packages](projects_and_packages.md#8-unsafe-module-in-dependencies).

Granting a dependency `[unsafe] allow` is a statement that you trust its
author to get raw memory right: such a package is outside every guarantee in
[Memory and safety](memory_and_safety.md).

---

## 7. Converting between pointer types

References widen to raw pointers for free; going the other way is always
explicit and, where the compiler cannot check it, `unsafe`:

```text
&T  ──implicit──▶ &?T     a non-null reference is a valid "maybe null"
&T  ──implicit──▶ T*      (and to ptr): a reference can never be null
&?T ──implicit──▶ T*      (and to ptr)
&?T ──`to &T`───▶ &T      only where a null test has narrowed it, else E082
T*  ──`to &T`───▶ &T      inside unsafe { }: the compiler cannot prove non-null
ptr ──`to T*`───▶ T*      inside unsafe { }
```

```turflang
struct Box { n: int }

fn main(): int {
    mut b: Box = Box::default();
    b.n = 3;
    unsafe {
        mut r: &Box = &b;
        mut raw: Box* = r;              // widening: no cast
        mut back: &Box = raw to &Box;   // narrowing: cast, inside unsafe
        printline(back.n);              // 3
    }
    return 0;
}
```

A `&T` can also be passed straight to an `extern fn` parameter of pointer type
(`memset(&x, 0 to int32, 8)`).

Making a reference out of something that is not already a reference to that
type forges an address and is `E130` outside `unsafe { }`: `0 to &N`,
`s to &N` for a string, `r to &M` for an `r: &N`.

<!-- doc-example: fails E130 -->
```turflang
fn main(): int {
    mut x: int = 5;
    mut q: &int = x to &int;      // E130: Unsafe reference conversion!
    return 0;
}
```

A reference is never null (`mut r: &int = null;` is `E062`), and a raw pointer
cannot be turned into a number: `p to int` is `E134`. Integer to pointer
(`4096 to int*`) is allowed, inside `unsafe`.

---

## 8. Callbacks

A C function that takes a function pointer, like `qsort`, is declared with
a function type for that parameter. The callback itself takes raw pointers,
and a function with a body may only do that in a file with
`unsafe module;`:

<!-- doc-example: skip a file with unsafe module; needs --allow-unsafe-modules -->
```turflang
unsafe module;

extern fn qsort(base: ptr, n: int, size: int, cmp: fn(ptr, ptr): int32): void;

fn cmpints(a: ptr, b: ptr): int32 {
    mut x: int = (a to int*)[0];
    mut y: int = (b to int*)[0];
    if x < y { return -1 to int32; }
    if x > y { return 1 to int32; }
    return 0 to int32;
}

fn main(): int {
    mut v: int[5] = [5, 2, 9, 1, 3];
    qsort(&v[0] to ptr, 5, 8, cmpints);
    mut i: int = 0;
    while i < 5 {
        printline(v[i]);          // 1 2 3 5 9
        i = i + 1;
    }
    return 0;
}
```

(Compile with `--allow-unsafe-modules`, or put the file in a project package.)

---

## 9. What stays your responsibility

Once you use `extern fn`, a raw pointer, `unsafe { }` or `unsafe module;`, the
guarantees in [Memory and safety](memory_and_safety.md) stop applying to that
code. The compiler has not verified, and you must:

- **The C signature.** The compiler trusts your `extern fn`. A wrong type,
  wrong arity or wrong calling convention is undefined behaviour, not an error.
- **Validity of every pointer you use:** non-null, aligned, pointing at a live
  object of the type you cast it to. A null or dangling raw pointer is a crash
  or silent corruption.
- **Bounds.** `p[i]` on a raw pointer is unchecked, and so is whatever a C
  function does with the buffer you give it.
- **Lifetime of the manual heap.** Memory from `mem::alloc` or `malloc` is yours
  to free, exactly once, and never to use afterwards. `@ownership` lets the
  compiler catch the simple cases of double free and use after free in one
  function; it catches nothing else.
- **Never freeing GC memory.** A `string` or a collection's buffer is on the
  garbage-collected heap. `mem::free` on one panics at run time (the
  [cross-heap free](memory_and_safety.md#the-cross-heap-free-panic)); calling
  C's `free` on one through your own `extern fn` is memory corruption.
- **Keeping GC objects alive when only C can see them.** A pointer into the
  garbage-collected heap that is stored *only* in manual memory or inside a C
  library is invisible to the collector. Pin it with `mem::root`
  ([`mem::root`](memory_and_safety.md#memroot-pinning-an-object-the-collector-cannot-see)).
- **Strings.** A `string` handed to C is the address of NUL-terminated bytes;
  C must not write past them or keep the pointer past the lifetime you gave it
  (the collector does not move strings, but it does free them).
- **Threads and signals.** The runtime is single-threaded. Do not call into
  Turf code from another thread.

---

## 10. Which pointer type do I use?

| Type | Nullable | Checked | Needs `unsafe`/capability | Reach for it when |
|---|---|---|---|---|
| `&T` | no | yes | no | The default: any reference you pass or return that is never "nothing". |
| `&?T` | yes | yes (null test) | no | A link or optional reference that may be absent: a list's `next`, a tree's children. |
| `T*` | yes | no | yes | Wrapping a C API, or a callback's parameters, where the type of the pointee is known. |
| `ptr` | yes | no | yes | The untyped edge of a C API (`void *`): cast to a `T*` as soon as you know what it is. |

Start with `&T`. When "or nothing" is part of the meaning, use `&?T`. Use
`T*` or `ptr` only for FFI, or where you need unchecked access and have
accepted the cost.

All four are the same machine value; the differences are enforced by the
compiler and cost nothing at run time. For how the compiler implements these
rules, see [`docs/architecture/pointers_and_references.md`](../architecture/pointers_and_references.md).
