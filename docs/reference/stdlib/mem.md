# Memory: `mem`

Low-level memory management, wrapping the C allocator ABI directly. Not
prelude — requires `import mem;`.

```turflang
import mem;
```

Like the rest of the standard library, this module is two layers: raw
`extern fn` bindings onto real libc allocator symbols (`malloc`, `calloc`,
`realloc`, `free`, `posix_memalign`, ...), and Turf-level checked wrappers
built on top of them that fail fast via `sys::panic()` on out-of-memory
rather than returning a null pointer for the caller to forget to check.

> **Naming note.** Helpers name their pointer parameter `p`, not `ptr` —
> `ptr` is Turf's own reserved opaque-pointer *type* keyword, so using it as
> a parameter name fails to parse.

Most `mem` code runs inside an `unsafe { }` block, since it deals directly
in `void*`.

---

## Raw C bindings

These map 1:1 onto real libc symbols and are directly callable as
`mem::malloc(...)`, etc., though most code should prefer the checked
wrappers below.

### Allocator bindings

- `malloc(size: int): void*`
- `calloc(count: int, size: int): void*`
- `realloc(p: void*, new_size: int): void*`
- `free(p: void*): void`
- `posix_memalign(memptr: void*, alignment: int, size: int): int` — the
  first parameter is genuinely a `void**` out-param in C; Turf's `void*` is
  opaque regardless of pointer depth, so it round-trips through a scratch
  slot and an `int*`-cast read-back internally.
- `malloc_usable_size(p: void*): int` — glibc-specific; reports the actual
  usable byte count at `p`, which is often larger than what was requested
  since `malloc` rounds up internally.

### Memory primitive bindings

- `memcpy`, `memmove`, `memset`, `memcmp` — standard C ABI signatures.

---

## Checked wrappers

### `alloc(size: int): void*`

`malloc(size)`. **Aborts** via `sys::panic("mem.alloc: out of memory")` on
OOM (a null result) — not a recoverable `!void*`.

```turflang
unsafe {
    mut buf: void* = mem::alloc(32);
    test::assert(buf != null, "");
}
```

### `alloc_zeroed(count: int, elem_size: int): void*`

`calloc(count, elem_size)`. Aborts (`"mem.alloc_zeroed: out of memory"`) on
OOM.

```turflang
mut zbuf: void* = mem::alloc_zeroed(4, 8);
```

### `grow(p: void*, new_size: int): void*`

`realloc(p, new_size)`. Aborts (`"mem.grow: out of memory"`) on OOM.

```turflang
mut grown: void* = mem::grow(buf, 64);
```

### `dup(src: void*, n: int): void*`

Allocates `n` bytes (via `alloc`, so also abort-on-OOM) and `memcpy`s `n`
bytes from `src` into it, returning a genuinely distinct buffer.

```turflang
mut src: int* = mem::alloc(8) to int*;
src[0] = 12345;

mut copy: int* = mem::dup(src, 8) to int*;
test::assert(copy[0] == 12345, "");
test::assert(copy != src, "");
```

### `zero(p: void*, n: int): void`

`memset(p, 0, n)`.

### `alloc_aligned(alignment: int, size: int): void*`

Wraps `posix_memalign`. `alignment` must be a power of two and a multiple of
`sizeof(void*)`, matching the underlying libc call's requirements. Aborts
(`"mem.alloc_aligned: allocation failed (bad alignment or out of memory)"`)
on any failure, including bad alignment.

```turflang
mut aligned: void* = mem::alloc_aligned(64, 128);
test::assert(mem::usable_size(aligned) >= 128, "...");
```

### `usable_size(p: void*): int`

`malloc_usable_size(p)` — the actual usable byte count at `p` (subject to
the glibc-specific caveat noted above).

---

## `Pool<T>` — bulk-free allocation tracking

```turflang
struct Pool<T> { items: T*, len: int, cap: int }
```

Tracks a set of allocations so they can all be freed with one call.

> **Design note: not a true bump-pointer arena.** Turf has neither
> address-of (`&local`) nor an allowed `ptr`/`void*` → `int` cast (only
> `int` → `ptr` is permitted), so there's no way to hand out sub-pointers
> into one big buffer the way a real arena would. `Pool<T>` instead just
> remembers every allocation made through it. It's deliberately
> self-contained — it does not `import list;`/reuse `List<T>`, which would
> create a circular import since `list.tr` itself imports `mem.tr`. Its
> `track()` is a direct copy of `List.push()`'s growth logic, and shares
> the same real-per-instantiation-`sizeof(T)` / doubling-from-8 policy
> described in [`collections.md`](./collections.md). This duplication is
> deliberate and explicitly out of scope to "fix" — see
> `docs/development/changes/STD-003.md`, item 7.

### `Pool.track(item: T): void`

Registers an already-obtained pointer (e.g. from `mem::alloc()`) with the
pool. Does **not** allocate anything itself.

```turflang
mut pool: mem::Pool<void*> = mem::Pool::default();
mut i: int = 0;
while i < 20 {
    pool.track(mem::alloc(16));
    i = i + 1;
}
test::assert(pool.length() == 20, "...");
```

### `Pool.length(): int`

Number of tracked allocations.

### `free_pool(pool: Pool<void*>): void`

Frees every tracked allocation, then the pool's own backing array.

> **Gotcha: only works for `Pool<void*>`, not a generic `Pool<T>`
> operation.** This is a plain function taking a *concrete* `Pool<void*>`
> parameter, not a generic `fn<T> Pool.free_all()` method. A generic
> method's body is type-checked exactly once against the abstract `T`, so
> `self.items[i] to void*` inside a generic method is rejected ("Cannot
> explicitly cast 'T' to 'void*'"). A plain function taking a concrete
> `Pool<void*>` sidesteps this because the type is fully known at the call
> site. In practice this means `Pool<T>` can only be meaningfully
> bulk-freed when instantiated as `Pool<void*>` (or another single concrete
> pointer type, via an equivalent hand-written free function) — there's no
> way to bulk-free an arbitrary `Pool<T>` generically.

```turflang
mem::free_pool(pool);
```
