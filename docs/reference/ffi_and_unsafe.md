# FFI, `unsafe {}`, and Pointers

This page covers calling into C libraries with `extern fn`, the `unsafe {}`
block, safe references (`&T`), and raw pointers (`ptr` / `T*`) — Turflang's
whole low-level/interop surface.

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
// lib/std/math/math.tr
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

`&T` can't represent a genuinely-nullable "next" pointer, which is exactly
why `T*` still exists alongside it:

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
| Opaque pointer | `ptr` | yes | no | inside `unsafe {}`, or anywhere under the standard library |
| Typed raw pointer | `T*` | yes | yes | inside `unsafe {}`, or anywhere under the standard library |
| Safe reference | `&T` | no | yes | anywhere |

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

1. **Anywhere under the standard library** (`lib/std`) — the standard
   library is exempt from the gate entirely, since it's where the raw
   FFI/pointer plumbing has to live.
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
