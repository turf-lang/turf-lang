# Memory and Safety

This page explains where a TurfLang program's data lives, who frees it, which
mistakes the compiler and the runtime stop, and which they do not. It ends with
the exact statement of what the language guarantees in code that uses no
`unsafe`, and what it does not.

Contents: [1. Two heaps](#1-two-heaps) · [2. The garbage-collected heap](#2-the-garbage-collected-heap)
· [3. References: `&T` and `&?T`](#3-references-t-and-t) · [4. Escape rules](#4-escape-rules)
· [5. Checks that panic](#5-checks-that-panic) · [6. When a program panics](#6-when-a-program-panics)
· [7. What is guaranteed](#7-what-is-guaranteed-and-what-is-not)
· [8. The manual heap](#8-the-manual-heap) · [9. Where `unsafe` begins](#9-where-unsafe-begins)

---

## 1. Two heaps

A TurfLang program has three places to keep data.

| Place | What lives there | Who frees it |
|---|---|---|
| **The stack** | `int`, `int32`, `double`, `bool`, `char`; struct values; fixed arrays `T[N]`; the handle of a collection | The function's return, automatically |
| **The garbage-collected (GC) heap** | every `string` an operation produces; the storage of `List`, `Stack`, `Queue`, `Set` and `Map`; everything you put there with `heap(expr)` | The collector, once nothing reachable refers to it |
| **The manual heap** | what `mem::alloc` (and `malloc`) return, and the bookkeeping of `mem::Pool<T>` | You, with `mem::free` |

A string literal (`"hello"`) is a fourth thing: static read-only data in the
program, never allocated and never freed.

The two heaps are **disjoint**. A pointer never moves from one to the other,
the collector never looks inside manual memory, and the manual heap's `free`
refuses to free GC memory ([section 8](#the-cross-heap-free-panic)).

```turflang
import list;

struct Point { x: int, y: int }

fn main(): int {
    mut p: Point = Point::default();                  // stack
    mut grid: int[4] = [1, 2, 3, 4];                  // stack
    mut lit: string = "static";                       // read-only data
    mut text: string = lit + " text";                 // GC heap
    mut l: list::List<int> = list::List::default();   // handle: stack; storage: GC heap
    l.push(1);
    mut boxed: &Point = heap(p);                      // GC heap
    return 0;
}
```

Everyday programs never touch the manual heap. Strings, collections and
`heap(...)` are all on the GC heap and need no cleanup. Deterministic cleanup of
an operating-system resource (a file, a socket) is `defer`, not the collector:
there are no finalizers (see [Control flow](control_flow.md)).

### Which storage do I want?

| You need | Use |
|---|---|
| a local value, a small struct, a fixed array | an ordinary variable (stack) |
| a value that must outlive the function that made it, or a linked structure whose size is not known at compile time | `heap(expr)` |
| a growable sequence, set or map | [`List`, `Map`, ...](stdlib/collections.md) |
| memory for a C library, or a buffer whose lifetime you manage | `mem::alloc` ([section 8](#8-the-manual-heap)) |

### Collections are handles

A `List`, `Stack`, `Queue`, `Set` or `Map` value names one collection.
Assigning it, passing it and returning it share that collection, length
included; `.copy()` makes an independent one.

```turflang
import list;

struct Box { n: int }

fn main(): int {
    mut b: Box = Box::default();
    b.n = 3;
    mut h: &Box = heap(b);          // a copy of b on the GC heap
    h.n = 4;
    printline(b.n);                 // 3: b itself is unchanged
    printline(h.n);                 // 4

    mut l: list::List<int> = list::List::default();
    mut m: list::List<int> = l;     // the same collection
    m.push(7);
    printline(l.length());          // 1
    mut c: list::List<int> = l.copy();
    c.push(8);
    printline(l.length());          // 1
    printline(c.length());          // 2
    return 0;
}
```

---

## 2. The garbage-collected heap

The collector is automatic. It runs when the program has allocated enough since
the last collection; you never call it, and nothing has to be freed. It has
properties that matter when you cross into C:

- **It does not move objects.** The address of a string or a buffer is stable
  for the object's whole life, so you can hand it to a C function that reads
  it during the call.
- **It is exact about the heap and conservative about the stack.** It finds
  every pointer stored in a GC object by that object's type, and treats any
  word on the stack or in a register that looks like a pointer as one. The
  consequence: an object is never freed while your code can still reach it, and
  now and then a dead object lives a little longer than needed.
- **It stops the program while it runs** and the runtime is single-threaded.
- **It does not see manual memory.** A GC pointer kept only inside a `mem::alloc`
  block, or inside a C library's own structures, is invisible to it
  ([`mem::root`](#memroot-pinning-an-object-the-collector-cannot-see)).

### `heap(expr)`: a value on the GC heap

`heap(expr)` copies the value of `expr` onto the GC heap and gives you a `&T`
to the copy. It is a built-in, needs no import and no `unsafe`. `T` is any type
with a size: a struct, a fixed array or a primitive. `heap` of a `void`
expression is `E067` ("Cannot heap-allocate 'void'").

A reference from `&local` points into a stack frame that ends when the function
returns; a reference from `heap(local)` points at a copy that lives as long as
anything refers to it. That makes `heap` the way to build a structure whose
size is decided at run time:

```turflang
struct Node { val: int, next: &?Node }

fn cons(v: int, next: &?Node): &Node {
    mut n: Node = Node::default();
    n.val = v;
    n.next = next;
    return heap(n);
}

fn sum(head: &?Node): int {
    mut total: int = 0;
    mut cur: &?Node = head;
    while (cur != null) {
        total = total + cur.val;
        cur = cur.next;
    }
    return total;
}

fn first_or(n: &?Node, fallback: int): int {
    if n == null { return fallback; }
    return n.val;
}

fn main(): int {
    mut list: &Node = cons(1, cons(2, cons(3, null)));
    printline(sum(list));               // 6
    printline(first_or(null, -1));      // -1
    return 0;
}
```

---

## 3. References: `&T` and `&?T`

`&T` is a reference to a `T` that is **never null**. `&expr` makes one from a
variable, a field or an array element, and `heap(expr)` makes one to a fresh
GC-heap copy. You use it with `.` for fields and methods, as if it were the
value; there is no `*r`. Assigning `null` to a `&T` is `E062`, and a `&T`
declared without a value must be assigned before use like any variable.

`&?T` is a reference that **may be null**. The compiler makes you test it
before you use it. After `x != null` (or `x == null` with an early exit), `x`
counts as non-null on that path; using it where it might be null is `E081`:

<!-- doc-example: fails E081 -->
```turflang
struct Node { val: int, next: &?Node }

fn first_val(n: &?Node): int {
    return n.val;       // E081: Possible null dereference: 'n' might be 'null' here
}
```

```text
Possible null dereference: 'n' might be 'null' here, but you're accessing its 'val' field.
Hint: Add a null check first, e.g. 'if (n != null) { ... }', and access '.val' only inside it.
```

To turn a `&?T` into a `&T` you may cast with `to &T`, but only where the
compiler has seen the null test; elsewhere it is `E082`:

<!-- doc-example: fails E082 -->
```turflang
struct Node { val: int, next: &?Node }

fn force(n: &?Node): &Node {
    return n to &Node;  // E082: Cannot cast 'n' to a non-null reference here -- it might be 'null'
}
```

A `&T` converts to a `&?T` by itself. The list example in
[section 2](#heapexpr-a-value-on-the-gc-heap) is the usual pattern: `&?Node`
for the link that may be absent, a loop that tests it.

A reference into the GC heap keeps its target alive. A reference to a local is
a plain address into the stack, which leads to the next section.

---

## 4. Escape rules

A local variable lives until its function returns, then its stack slot is
reused. A `&T` into it would then point at something else. TurfLang refuses
the common ways of letting such a reference leave the function: it is
`E086`, "A reference into this function's own local storage can't be ..."

<!-- doc-example: fails E086 -->
```turflang
struct Counter { n: int }

fn make(): &Counter {
    mut c: Counter = Counter::default();
    c.n = 1;
    return &c;          // E086: ... can't be returned
}
```

```text
A reference into this function's own local storage can't be returned -- it would
dangle the moment this function returns.
Hint: heap(value) copies it onto the GC heap first and gives you back a reference
that outlives this function; or return/store a reference you received as a
parameter instead of a local's.
```

The fix the diagnostic suggests is `heap(c)`. A reference you **received** as a
parameter is fine to return or store, because it already outlives your function:

```turflang
struct Counter { n: int }

fn make(): &Counter {
    mut c: Counter = Counter::default();
    c.n = 1;
    return heap(c);
}

fn bump(c: &Counter): &Counter {
    c.n = c.n + 1;
    return c;           // a parameter's reference: allowed
}

fn main(): int {
    mut k: &Counter = make();
    bump(k);
    printline(k.n);     // 2
    return 0;
}
```

A reference to a local may not be:

| Sink | Message ends |
|---|---|
| returned | `... can't be returned` |
| stored into a field of something that outlives the call (a parameter's, or a `heap` object's) | `... can't be stored into a field that outlives this function` |
| pushed into a `List`, `Stack`, `Queue`, `Set` or `Map`, or stored by an indexed assignment | `... can't be pushed into a collection` |
| boxed with `heap()` | `... can't be boxed with heap()` |
| carried in an error's payload | `... can't be stored in an error payload` |

The same holds when the reference is inside a struct or array value that is then
returned, stored, pushed or boxed (`... inside a struct or array value`), and
when the reference is reached through a variable that holds it: a reference
assigned on any path that reaches the sink counts, even if other paths assign
a `heap` reference. Each of these is `E086`:

<!-- doc-example: fails E086 -->
```turflang
struct Node { val: int, next: &?Node }

fn link(head: &Node): void {
    mut tmp: Node = Node::default();
    tmp.val = 7;
    head.next = &tmp;   // E086: can't be stored into a field that outlives this function
}
```

<!-- doc-example: fails E086 -->
```turflang
import list;

struct Item { v: int }

fn fill(into: list::List<&Item>): void {
    mut it: Item = Item::default();
    into.push(&it);     // E086: can't be pushed into a collection
}
```

<!-- doc-example: fails E086 -->
```turflang
struct Item { v: int }
struct Holder { item: &Item }

fn wrap(): Holder {
    mut it: Item = Item::default();
    mut h: Holder = Holder::default();
    h.item = &it;
    return h;           // E086: can't be returned inside a struct or array value
}
```

In each case the fix is to put the value on the GC heap first (`head.next = heap(tmp)`,
`into.push(heap(it))`, `h.item = heap(it)`), or to use a reference that was passed in.

### What this does not catch

This is a check on each function by itself, not a borrow checker. There are no
lifetime annotations. It never rejects a program that was safe, and it does not
find every dangling reference. A reference to a local that is passed **into
another function** and stored there is not followed:

```turflang
struct Item { v: int }
struct Holder { item: &Item }

fn store(h: &Holder, r: &Item): void {
    h.item = r;
}

fn remember(h: &Holder): void {
    mut local: Item = Item::default();
    store(h, &local);   // accepted: h.item now dangles once remember() returns
}

fn main(): int {
    mut shared: &Holder = heap(Holder::default());
    remember(shared);
    return 0;
}
```

The compiler also does not see through a function that returns one of two
reference parameters (`pick(&x, &y, true)`), or through a reference whose
origin it cannot classify. If you pass `&local` to a function that keeps its
argument, you are responsible for what that function does.

---

## 5. Checks that panic

These mistakes stop the program with a panic ([section 6](#6-when-a-program-panics)),
or are compile errors when the compiler can see them coming.

### Array, string and collection indexing

An array index is checked. When the index is a constant, or provably constant,
and out of range, the program does not compile (`E001`). Otherwise the check
runs at run time:

<!-- doc-example: fails E001 -->
```turflang
fn main(): int {
    mut a: int[3] = [1, 2, 3];
    printline(a[3]);        // E001: Array index out of bounds!
    return 0;
}
```

```turflang
fn at(a: int[3], i: int): int {
    return a[i];
}

fn main(): int {
    mut a: int[3] = [1, 2, 3];
    printline(at(a, -1));   // 3: a negative index counts from the end
    printline(at(a, 3));    // panic: runtime check: Array index out of bounds for 'a'
    return 0;
}
```

A slice `v[lo..hi]` out of range panics (`Slice range out of bounds`). The
standard collections check too: `l[i]` on a `List` panics with
`List[]: index out of bounds`. Indexing is described in [Arrays](arrays.md#bounds-checking).

Reading a string byte, `s[i]`, is checked (`String index out of bounds`), with
negative indices counting from the end. **Writing** a string byte, `s[i] = c`,
is checked against the string's allocation rather than its length: string-building
code writes bytes before the terminator exists, so the terminator cannot bound
a write, but the heap block the string lives in can. A write past the block's
end panics with `String index out of bounds`, and a write into a string that
is not on the heap at all (a string literal, which is read-only data, or memory
from C) panics with `Cannot write into this string`:

```turflang
import strings;

fn poke(s: string, i: int): void {
    s[i] = 'x';
}

fn main(): int {
    mut s: string = strings::repeat("ab", 2);   // 4 bytes and the terminator
    poke(s, 3);             // fine
    poke(s, 4);             // fine: the terminator's byte, which this overwrites
    poke(s, 103);           // panics: String index out of bounds
    return 0;
}
```

The bound is the whole allocation, so a write can still overwrite a string's
terminator, after which reading it runs into whatever follows, up to the end
of its block. Build strings with the string operations and `+`, and write
bytes by index only into a buffer you allocated for it.
`strings::byte_at_unchecked(s, i)` is the read that skips the check, for a
caller that has already established the bound.

### Integer overflow, division, conversions

Integer arithmetic is checked. `+`, `-`, `*`, unary `-`, `<<`, `**` and the
quotient `INT_MIN / -1` panic on overflow; `/` and `%` by zero panic; a `double`
that is NaN, infinite or out of range does not convert to an `int`. This does
not depend on the optimisation level. The full table is in
[Variables and types](variables_and_types.md#integer-overflow). A constant
expression that overflows is a compile error (`E076`) instead.

```turflang
fn add(a: int, b: int): int { return a + b; }
fn div(a: int, b: int): int { return a / b; }

fn main(): int {
    printline(add(1, 2));                    // 3
    printline(add(9223372036854775807, 1));  // panic: runtime check: Integer overflow
    printline(div(1, 0));                    // not reached
    return 0;
}
```

`double` arithmetic is IEEE: `inf` and `NaN` do not panic.

### Reading a variable before it has a value

A local is zero-filled, but reading one before it is definitely assigned on
every path is `E008`, so you never observe the zero ([Variables and types](variables_and_types.md#definite-assignment)).

---

## 6. When a program panics

A failed check, a stack overflow, and running out of memory all end the program
the same way. The runtime writes one line to standard error and aborts:

```text
panic: runtime check: Integer overflow (example.tr:3)
```

The exit status is 134 on Linux. There is no unwinding and no recovery by the
program itself: **deferred statements do not run**, and nothing after the failing
operation executes. Output the program already wrote to standard output is
flushed before the panic message, so it is not lost even when standard output
is a pipe.

```turflang
import io;

fn main(): int {
    defer io::eprintline("never printed");
    io::printline("printed before the panic message");
    mut zero: int = 0;
    return 5 / zero;        // panic: runtime check: Division by zero
}
```

### Stack overflow

Unbounded recursion exhausts the stack. This is a panic, not a crash:

```text
panic: stack overflow: the call stack exceeded its limit -- check for unbounded or missing-base-case recursion
```

```turflang
fn recurse(n: int): int {
    mut buf: int[8] = [1, 2, 3, 4, 5, 6, 7, 8];
    if n > 2000000000 {
        return buf[0];
    }
    return recurse(n + 1) + buf[0];     // not a tail call: the stack grows
}

fn main(): int {
    return recurse(0);                  // panic: stack overflow
}
```

The size of the stack is set by the platform.
Data that is large or whose depth is unbounded belongs in a collection or on
the GC heap, not in recursion.

### Out of memory

If the GC heap cannot get memory, the program panics with
`panic: out of memory`; there is no null to check and nothing to catch.
A failed `mem::alloc` panics too, with `mem.alloc: out of memory`
(`mem.alloc_zeroed`, `mem.grow`, ... for the others). Code does not need to
check what the allocators return.

```turflang
import strings;

fn main(): int {
    mut s: string = strings::repeat("abc", 4000000000000);   // panic: out of memory
    printline(strings::length(s));
    return 0;
}
```

---

## 7. What is guaranteed, and what is not

In code that has **no `unsafe { }` block** and lives in a **module without
`unsafe module;`**, TurfLang guarantees the six statements below. Each is stated
exactly, with the program that shows it.

**1. A program cannot read or write outside an object, except as named
here.** Array, slice, `List`, collection and string-read indexing are
checked (section 5), and raw-pointer operations, which are not, cannot be written
in such code. A string byte write `s[i] = c` is checked against the string's
heap allocation. Standard-library functions that are documented as unchecked
(`strings::byte_at_unchecked`) skip the check on request.

**2. A reference cannot outlive its stack frame, in the cases section 4
classifies.** Returning, storing, pushing and boxing a reference to a local are
`E086`. A reference handed to another function and kept there is not followed
(section 4).

**3. A GC object cannot be used after it is collected.** The collector keeps
every object reachable from the stack, registers, other GC objects and
collections. In code with no raw pointers there is no way to hold a GC
pointer anywhere the collector cannot see, and no way to free one.

**4. GC memory cannot be freed by hand.** `mem::free` takes a raw pointer, so
`mem::free(some_string)` does not type-check, and the cast to `void*` that
would make it type-check is `E130` outside `unsafe`:

<!-- doc-example: fails E130 -->
```turflang
import mem;
import strings;

fn main(): int {
    mut s: string = strings::repeat("z", 10);
    mem::free(s to void*);      // E130
    return 0;
}
```

(Inside `unsafe { }`, the same call compiles and panics at run time, see
[section 8](#the-cross-heap-free-panic).)

**5. A `&?T` cannot be dereferenced without a null check:** `E081`, section 3.
A `&T` is never null by construction.

**6. Integer overflow and division by zero panic** rather than wrapping or
being undefined (section 5). So does stack overflow (section 6).

### What is not guaranteed

- **Anything inside `unsafe { }`, in a file with `unsafe module;`, in a package
  you listed in `[unsafe] allow`, or behind an `extern fn`.** That code can read
  and write anywhere, hold a pointer to freed memory, free twice, and hide GC
  pointers from the collector. A bug there can corrupt the rest of the program,
  including its safe parts. See [FFI and unsafe](ffi_and_unsafe.md).
- **That a string keeps its terminator.** A byte write is bounded by the
  string's allocation, not by its length, so it can overwrite the terminator
  (statement 1).
- **Dangling references the compiler does not classify** (statement 2's
  limit).
- **That the manual heap is freed.** Safe code can call `mem::alloc` and never
  free the result; nothing reports a leak. The compiler's double-free and
  use-after-free check covers only functions you annotate with `@ownership`,
  and only within one function ([Functions and modules](functions_and_modules.md#ownership)).
- **That memory is returned promptly.** The collector runs when it decides to;
  a dead object may be retained for a while, and a program that allocates
  without bound still runs out of memory.
- **Deterministic cleanup.** There are no finalizers. Use `defer`.
- **That a program keeps running.** Every check above ends in a panic, not an
  error you can handle.

Reading through a null or wild raw pointer is therefore on you, not the
runtime: it is a segmentation fault reported as
`panic: segmentation fault (this is a bug in unsafe code or in TurfLang)`, or,
at higher optimisation levels, any value at all.

---

## 8. The manual heap

`mem::alloc(n)` returns `n` bytes of manual memory as a `void*`, and
`mem::free(p)` returns them. `mem::alloc_zeroed`, `grow`, `dup`, `alloc_aligned`
and `mem::Pool<T>` are in [Memory](stdlib/mem.md). A raw pointer needs
`unsafe` to declare ([FFI and unsafe](ffi_and_unsafe.md)):

```turflang
import mem;

fn main(): int {
    unsafe {
        mut p: int* = mem::alloc(sizeof(int) * 4) to int*;
        p[0] = 12;
        p[3] = 30;
        printline(p[0] + p[3]);       // 42
        mem::free(p to void*);
    }
    return 0;
}
```

The collector never sees this memory. You free it once, and not afterwards.

### The cross-heap free panic

A `string` and a collection's storage look like pointers, but they belong to the
GC heap. Giving one to `mem::free` is a mistake that does not corrupt memory
silently: the runtime checks every pointer against the collector's records and
panics.

```turflang
import strings;
import mem;

fn main(): int {
    mut s: string = strings::repeat("z", 10);
    unsafe {
        mem::free(s to void*);
    }
    return 0;
}
```

```text
panic: cross-heap free: mem::free() was called on a pointer that belongs to the GC heap (a string, a collection buffer, a heap(...) value, anything the collector owns), not the manual heap -- the two heaps do not merge
```

(Calling the C `free` directly through your own `extern fn` has no such check.)

### `mem::root`: pinning an object the collector cannot see

The collector finds an object by following pointers from the stack, registers
and other GC objects. A GC pointer stored **only** in manual memory (a
`mem::alloc` block) or inside a C library's data is not found by that, and the
object can be collected while you still hold it there.

`mem::root(p)` pins the GC object that contains `p` until a matching
`mem::unroot(p)`:

```turflang
import mem;
import strings;
import io;

fn main(): int {
    unsafe {
        mut holder: string* = mem::alloc(sizeof(string)) to string*;
        holder[0] = strings::repeat("kept", 3);   // the only reference is in manual memory
        mem::root(holder[0] to void*);            // pin it
        io::printline(holder[0]);                 // keptkeptkept
        mem::unroot(holder[0] to void*);
        mem::free(holder to void*);
    }
    return 0;
}
```

- `p` may point into the middle of the object.
- Pins nest: two `mem::root(p)` calls need two `mem::unroot(p)` calls.
- `mem::root` of a pointer that is not a GC pointer at all (a `mem::alloc` block,
  a C buffer) does nothing.
- `mem::unroot(p)` of an object with no pin panics:
  `panic: unpin of unpinned object: mem::unroot() was called on an object with no matching mem::root() pin outstanding`.

You need `mem::root` when a program puts a `string`, a collection buffer or a
`heap` object somewhere only manual memory or C will hold it, for example a
callback's context that a C library stores for later. You do not need it for
anything on the stack, in a struct, or in a collection, and never in code
without `unsafe`.

---

## 9. Where `unsafe` begins

The guarantees in section 7 hold up to the line where code does one of these:

| You write | You have left the guarantees because |
|---|---|
| `unsafe { ... }` | raw pointers can be declared, cast and read through; raw indexing is unchecked |
| `unsafe module;` at the top of a file | the whole file may do the same without a block (needs `--allow-unsafe-modules`, or `[unsafe] allow` for a dependency) |
| `extern fn` | the compiler cannot check what the C function does with its arguments, and trusts the signature |
| `mem::alloc` and the pointers it returns | the memory is yours to free once, in time, and not to use afterwards |

Everything else on this page applies to the rest of the program. The page that
covers these is [FFI and unsafe](ffi_and_unsafe.md), including what remains
the programmer's responsibility, and
[Projects and packages](projects_and_packages.md#8-unsafe-module-in-dependencies)
for the `[unsafe] allow` list. For how the runtime and the collector are
built, see [`docs/architecture/memory_model.md`](../architecture/memory_model.md).
