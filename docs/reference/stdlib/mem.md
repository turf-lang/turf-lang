# Memory: `mem`

The manual heap: raw allocation, the checked wrappers around it, pinning, and
`Pool`. This page is the API. What the two heaps are, which one you want and
how they interact is in [Memory and Safety](../memory_and_safety.md).

```turflang
import mem;
```

Not prelude: it needs `import mem;`. Most `mem` code deals in `void*`, so it
runs inside `unsafe { }` ([FFI and unsafe](../ffi_and_unsafe.md)). Using a raw
pointer type outside `unsafe` is **E130** ("Unsafe pointer use").

## Which storage do I want

| You need | Use |
|---|---|
| a local value, a small struct, a fixed array | an ordinary variable |
| a value that outlives its function, or a linked structure | [`heap(expr)`](../memory_and_safety.md#heapexpr-a-value-on-the-gc-heap), on the GC heap |
| a growable sequence, set or map | [`List`, `Map`, ...](collections.md), on the GC heap |
| memory for a C library, or a buffer whose lifetime you manage | `mem::alloc`, on the manual heap |

The two heaps never merge. Memory from `mem::alloc` is never traced or freed by
the collector: you free it, exactly once. A `string`, a `heap(...)` object or a
collection's storage belongs to the collector and must never be passed to
`mem::free`.

```turflang
import mem;
import io;

fn main(): int {
    unsafe {
        mut buf: int* = mem::alloc(sizeof(int) * 4) to int*;
        buf[0] = 12;
        buf[3] = 30;
        io::printline((buf[0] + buf[3]) to string);   // 42
        mem::free(buf to void*);
    }
    return 0;
}
```

The compiler checks a pointer from `mem::alloc` (and every function declared
`@ownership(acquires)`) for a second `mem::free` of it, **E017** ("Double free
detected"), and for a use after the free, **E018** ("You're using memory from
'buf' after it was freed"), when it can see both on the same path.

<!-- doc-example: fails E017 -->
```turflang
import mem;

fn main(): int {
    unsafe {
        mut p: void* = mem::alloc(8);
        mem::free(p);
        mem::free(p);
    }
    return 0;
}
```

<!-- doc-example: fails E018 -->
```turflang
import mem;

fn main(): int {
    unsafe {
        mut buf: int* = mem::malloc(40);
        mem::free(buf);
        buf[0] = 5;
    }
    return 0;
}
```

## Checked allocation

The wrappers below call the C allocator and **panic** if it returns null, so a
returned pointer is never null. None of them returns `!T`: running out of memory
is not a condition a program recovers from. Parameters are named `p`, not
`ptr`, because `ptr` is a type keyword.

### `alloc(size: int): void*`

`size` bytes of uninitialised manual memory. Panics `mem.alloc: out of memory`.

### `alloc_zeroed(count: int, elem_size: int): void*`

`count * elem_size` bytes, all zero. Panics `mem.alloc_zeroed: out of memory`.

### `grow(p: void*, new_size: int): void*`

Resizes the block at `p` to `new_size` bytes, keeping the contents that fit, and
returns the new address: use that, not `p`, afterwards. Panics
`mem.grow: out of memory`.

### `dup(src: void*, n: int): void*`

A new block of `n` bytes holding a copy of the `n` bytes at `src`. Panics
`mem.alloc: out of memory`.

### `zero(p: void*, n: int): void`

Sets `n` bytes starting at `p` to zero.

### `alloc_aligned(alignment: int, size: int): void*`

`size` bytes whose address is a multiple of `alignment`. `alignment` must be a
power of two and a multiple of the pointer size (8). Any failure, including a
bad `alignment`, panics with
`mem.alloc_aligned: allocation failed (bad alignment or out of memory)`.

### `usable_size(p: void*): int`

How many bytes are really usable at `p`: at least what was asked for, often
more. Not available on every platform (it wraps `malloc_usable_size`, a glibc
function, spelled `malloc_size` on macOS).

```turflang
import mem;
import test;

fn main(): int {
    unsafe {
        mut a: int* = mem::alloc(8) to int*;
        a[0] = 12345;
        mut copy: int* = mem::dup(a to void*, 8) to int*;
        test::assert(copy[0] == 12345 && copy != a, "dup is a distinct block");

        mut z: void* = mem::alloc_zeroed(4, 8);
        mut zi: int* = z to int*;
        test::assert(zi[3] == 0, "zeroed");
        zi[3] = 9;
        mem::zero(z, 32);
        test::assert(zi[3] == 0, "zero clears");
        z = mem::grow(z, 64);

        mut aligned: void* = mem::alloc_aligned(64, 128);
        test::assert(mem::usable_size(aligned) >= 128, "usable_size");

        mem::free(a to void*);
        mem::free(copy to void*);
        mem::free(z);
        mem::free(aligned);
    }
    return 0;
}
```

### `free(p: void*): void`

Releases a block that came from `alloc`, `alloc_zeroed`, `alloc_aligned`,
`grow`, `dup`, or any other pointer you own. Rules:

- Free each block once, and do not use it afterwards. `free` of `null` does
  nothing.
- **Never pass it a pointer from the GC heap**: a `string`, a `heap(...)`
  object, or a collection's storage. The runtime checks every pointer against
  the collector's records, and panics instead of corrupting memory:

  ```text
  panic: cross-heap free: mem::free() was called on a pointer that belongs to the GC heap (a string, a collection buffer, a heap(...) value, anything the collector owns), not the manual heap -- the two heaps do not merge
  ```

  The compiler cannot see this, since `s to void*` is a legal cast; the check
  is at run time.
- The check lives in `mem::free`. If you declare your own `extern fn free` and
  call the C function directly, there is no check.

```turflang
import mem;
import strings;

fn main(): int {
    mut s: string = strings::repeat("z", 10);   // a GC-heap string
    unsafe {
        mem::free(s to void*);                  // panics: cross-heap free
    }
    return 0;
}
```

See [the cross-heap free panic](../memory_and_safety.md#the-cross-heap-free-panic).

## Pinning: `root` and `unroot`

### `root(p: void*): void`

### `unroot(p: void*): void`

The collector finds objects by following pointers from the stack, registers and
other GC objects. A GC pointer stored **only** in manual memory, or inside a C
library's data, is invisible to it, and the object can be collected while you
still hold it there. `root(p)` pins the GC object that contains `p` so that no
collection reclaims it, until a matching `unroot(p)`.

- `p` may point into the middle of the object.
- Pins nest: two `root(p)` calls need two `unroot(p)` calls.
- `root` of a pointer that is not a GC pointer (a `mem::alloc` block, a C buffer)
  does nothing, and so does a later `unroot` of it.
- `unroot(p)` of a GC object with no outstanding pin panics:
  `unpin of unpinned object: mem::unroot() was called on an object with no matching mem::root() pin outstanding`.

```turflang
import mem;
import strings;
import io;

fn main(): int {
    unsafe {
        mut holder: string* = mem::alloc(sizeof(string)) to string*;
        holder[0] = strings::repeat("kept", 3);   // the only reference is in manual memory
        mem::root(holder[0] to void*);
        gc_collect();
        io::printline(holder[0]);                 // keptkeptkept
        mem::unroot(holder[0] to void*);
        mem::free(holder to void*);
    }
    return 0;
}
```

When you need it, and when you do not, is in
[`mem::root`](../memory_and_safety.md#memroot-pinning-an-object-the-collector-cannot-see).

## `Pool`: free a batch together

```text
pub struct Pool         // the fields are private
```

A `Pool` remembers a set of allocations so that one call frees them all, for
example everything allocated while serving one request. It stores the pointers
you hand it; it does not allocate for you.

`Pool` is not generic and holds `void*` only. Its storage is manual memory that
the collector never scans, so a GC value kept only there would be freed under
it; the signature keeps one out. `mem::Pool<string>` is **E148** ("Wrong number
of type arguments"), and `pool.track(s)` with a `string`, a collection or a
struct is **E056** ("No matching overload"). Any `T*` converts to `void*` on its
own.

### `Pool.track(item: void*): void`

Registers `item`, normally a pointer you already got from `alloc`. It allocates
nothing for the item itself. The pool's own storage is manual memory and grows
by doubling, starting at 8.

### `Pool.length(): int`

The number of tracked items.

### `free_pool(pool: Pool): void`

Calls `mem::free` on every tracked pointer, then releases the pool's own storage.
Every tracked pointer must therefore follow `free`'s rules: a GC pointer cast to
`void*` and tracked anyway panics with the cross-heap free message. The pool is
left empty, so freeing it again does nothing and it can track a new batch.

A `Pool` is a **shared handle**, like a `List`: assigning it, or passing it to a
function, shares the one set of tracked items, so a callee's `track` is seen by
its caller. There is no `copy()`, since two pools freeing the same pointers
would free them twice.

```turflang
import mem;
import io;

fn track_two(pool: mem::Pool): void {
    unsafe {
        pool.track(mem::alloc(16));
        pool.track(mem::alloc(16));
    }
}

fn main(): int {
    unsafe {
        mut pool: mem::Pool = mem::Pool::default();
        pool.track(mem::alloc(16));
        track_two(pool);
        io::printline(pool.length() to string);   // 3
        mem::free_pool(pool);
    }
    return 0;
}
```

## C bindings

The C functions underneath are public as well, for code that wants the exact C
behaviour with no null check. Prefer the wrappers above.

| Binding | Notes |
|---|---|
| `malloc(size: int): void*`, `calloc(count: int, size: int): void*`, `realloc(p: void*, new_size: int): void*` | return null on failure; declared as allocators for the double-free check |
| `posix_memalign(memptr: void*, alignment: int, size: int): int` | `memptr` is the address of a pointer-sized slot the C function writes into; returns 0 on success |
| `malloc_usable_size(p: void*): int` | glibc; `malloc_size` on macOS |
| `memcpy(dst, src, n)`, `memmove(dst, src, n): void*`, `memset(p, val, n): void*`, `memcmp(a, b, n): int` | the C functions of those names |

Memory from `malloc`, `calloc` and `realloc` is released with `mem::free`.

```turflang
import mem;
import test;

fn main(): int {
    unsafe {
        mut r: void* = mem::malloc(8);
        mut z: void* = mem::calloc(1, 8);
        r = mem::realloc(r, 16);
        mem::memset(r, 7, 16);
        mem::memcpy(z, r, 8);
        test::assert(mem::memcmp(z, r, 8) == 0, "copied bytes compare equal");
        mem::free(r);
        mem::free(z);
    }
    return 0;
}
```
