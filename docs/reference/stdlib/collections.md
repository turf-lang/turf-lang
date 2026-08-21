# Collections: `List`, `Stack`, `Queue`, `Map`

Generic collection types. None of these are prelude — each requires an
explicit import naming the type, e.g. `import list::List;`, or a plain
`import list;` combined with the fully-qualified spelling `list::List<T>`.

```turflang
import list::List;
import stack::Stack;
import queue::Queue;
import map::Map;
```

All four are default-constructible with no field initialization required —
`mut l: List<int>;` or `List::default()` both give you an empty, valid
collection.

## Shared design: type erasure and growth policy

Every collection in this module — `List<T>`, `Stack<T>`, `Queue<T>`,
`Map<K, V>`, and [`mem::Pool<T>`](./mem.md) — is built on the identical
underlying scheme, so it's worth understanding once instead of per-type:

- **8-byte-per-element type erasure.** Generic storage is a raw pointer
  (`T*`) sized in bytes as `cap * 8` — every `T` occupies exactly one 64-bit
  slot, whether it's an `int`, a `double`, or any pointer type. This is what
  makes the generic storage possible without real monomorphization, but it
  also means **`T` can't practically be a struct passed by value larger than
  8 bytes** — such a struct wouldn't fit in one slot. Use a pointer type
  (`MyStruct*`) as the element type instead if your struct is larger.
- **Doubling-from-8 growth.** Every push-like operation follows the same
  rule: if the backing array is at capacity, grow it — `cap == 0` becomes
  `cap = 8`, otherwise `cap *= 2` — via `mem::malloc`/`mem::realloc`. This
  is the same growth logic in `List.push`, `Stack.push`, `Queue.enqueue`
  (when it can't compact instead — see below), `Map.put`, and
  `mem::Pool.track`.

Bugs to know about: none reported for `List`/`Stack`; `Queue` and `Map` each
have one documented behavioral quirk below (compaction order, and
non-insertion-ordered removal, respectively).

---

## `List<T>` (`collections/list.tr`)

```turflang
struct List<T> { data: T*, len: int, cap: int }
```

A dynamically-growing array — the general-purpose sequence type. Internally
`import mem; import sys;`.

### `List.push(item: T): void`

Appends `item` to the end, growing per the shared policy above.

```turflang
mut l: List<int> = List::default();
l.push(10);
l.push(20);
l.push(30);
test::assert(l.length() == 3, "");
```

### `List.pop(): T`

Removes and returns the last element.

> **Panics, doesn't return `!T`.** Calls `sys::panic("List.pop: pop on empty
> list")` and aborts the process if `len == 0`. There's no recoverable
> variant — check `.length() > 0` first if the list might be empty.

```turflang
test::assert(l.pop() == 30, "");
```

### `List.length(): int`

Returns the current element count (`self.len`) — not the backing capacity.

`List<T>` has no random-access `get`/`set` — `push`/`pop`/`length` are the
entire public surface. To visit every element, pop in a loop until the list
is empty (this is the idiom the standard library and its own tests use).

### `list::list_sort(l: List<int>, cmp: fn(int, int): int): void` / `list::list_binary_search(l: List<int>, target: int, cmp: fn(int, int): int): int` / `list::list_find(l: List<int>, predicate: fn(int): bool): int`

Sort and search, as free functions in `list.tr` rather than generic
`List<T>` methods.

> **Why free functions, and why `List<int>` only.** This isn't a stylistic
> choice — it's a real, confirmed compiler limitation. A comparator
> parameter typed `fn(T, T): int` inside a generic method never
> type-checks correctly (`FuncType` parameters are never substituted
> through generic instantiation anywhere in the compiler), and the
> fallback of comparing `T` values with a bare `<`/`>` operator inside a
> generic body doesn't work either (ordering operators aren't resolved
> generically the way `==`/`!=` are). Both dead ends are documented
> directly in `lib/std/collections/list.tr` so they aren't "fixed" back to
> the broken shape later. Plain functions taking a concrete `List<int>`
> sidestep the problem entirely, and `int` is the only element type this
> could be numerically meaningful for anyway — `List<double>` sorted this
> way would compare raw IEEE-754 bit patterns as integers.

- **`list_sort`** — in-place insertion sort. `cmp(a, b)` should return
  negative/zero/positive, matching C's `qsort` convention.
- **`list_binary_search`** — only meaningful on an already-sorted list
  (not checked/enforced); returns the index if found, `-1` otherwise.
- **`list_find`** — linear search with an arbitrary predicate, for
  unsorted lists or non-equality lookups; returns the first matching
  index or `-1`.

```turflang
import list::List;

fn byValue(a: int, b: int): int { return a - b; }
fn isEven(n: int): bool { return n % 2 == 0; }

mut nums: List<int> = List::default();
nums.push(3);
nums.push(1);
nums.push(2);

list::list_sort(nums, byValue);
test::assert(list::list_find(nums, isEven) >= 0, "found an even number");
```

---

## `Stack<T>` (`collections/stack.tr`)

```turflang
struct Stack<T> { data: T*, len: int, cap: int }
```

LIFO. Structurally identical growth logic to `List<T>` — same
doubling-from-8 scheme, same 8-byte-slot type erasure.

### `Stack.push(item: T): void`

Same growth/append logic as `List.push`.

```turflang
mut s: Stack<int> = Stack::default();
s.push(1);
s.push(2);
s.push(3);
```

### `Stack.pop(): T`

Removes and returns the top element. **Panics**
(`"Stack.pop: pop on empty stack"`) if the stack is empty.

```turflang
test::assert(s.pop() == 3, "");
```

### `Stack.peek(): T`

Returns the top element without removing it. **Panics**
(`"Stack.peek: peek on empty stack"`) if empty.

### `Stack.length(): int`

Current element count.

---

## `Queue<T>` (`collections/queue.tr`)

```turflang
struct Queue<T> { data: T*, len: int, cap: int, head: int }
```

FIFO, implemented as a dynamic array plus a `head` offset — **not** a ring
buffer.

`enqueue` appends at `data[head + len]`. When the buffer is exhausted
(`head + len == cap`):

- If `head > 0`, the queue **compacts**: it shifts all live elements to the
  front of the buffer (an `O(len)` loop-based shift) and resets `head` to
  `0`, reusing existing capacity instead of allocating more.
- Only if `head == 0` (truly full — nothing to reclaim) does it actually
  double `cap` via `malloc`/`realloc`, per the shared growth policy.

### `Queue.enqueue(item: T): void`

As described above.

```turflang
mut q: Queue<int> = Queue::default();
q.enqueue(1);
q.enqueue(2);
q.enqueue(3);
q.dequeue();
q.dequeue(); // head is now 2, len is 1

// enqueue 4..10 past capacity to exercise "compact to front" then "grow"
q.enqueue(4);
// ...
q.enqueue(10);

test::assert(q.length() == 8, "");
test::assert(q.dequeue() == 3, ""); // FIFO order preserved across compaction/growth
```

### `Queue.dequeue(): T`

Reads `data[head]`, advances `head`, decrements `len`. **Panics**
(`"Queue.dequeue: dequeue on empty queue"`) if `len == 0`.

### `Queue.peek(): T`

Returns `data[head]` without dequeuing. **Panics**
(`"Queue.peek: peek on empty queue"`) if empty.

### `Queue.length(): int`

Current element count (`self.len` — not `cap`, and not `head + len`).

---

## `Map<K, V>` (`collections/map.tr`)

```turflang
struct Map<K, V> { keys: K*, values: V*, len: int, cap: int }
```

A **linear-scan association list, not a hash table.** There's no generic
`hash(T)` in the language yet, so this is the honest, simple version — fine
for small maps. A hashed `HashMap<K, V>` is a distinct, deferred future
addition. Storage is two parallel type-erased arrays (`keys`, `values`),
grown exactly like `List<T>.push()`.

Because lookups use `==` on `K`, your key type must support equality.

> **Known limitation: `K = string` lookups compare by identity, not
> content.** `indexOf`'s `self.keys[i] == key` is a raw pointer/bit
> comparison here, not a call into `strings.tr`'s real content-comparing
> `operator==(string, string)` — a generic method body is type-checked and
> codegenned exactly once, shared across every instantiation (every `K`/`V`
> erases to one uniform 8-byte slot), so `self.keys[i]`/`key` never carry
> the concrete type `string` for overload resolution to match against. This
> is correct (bit-exact identity genuinely is `==`) for `K` = `int`/
> `double`/`bool`/pointer, but wrong for `K` = `string`: two content-equal
> keys built from different allocations (e.g. one from `strings::trim()`,
> the other a literal) compare unequal. A same-file literal-to-literal
> comparison (`m.put("a", 1); m.contains("a")`) can appear to work by
> coincidence — the linker merges identical string-literal constants into
> one address — which is why this went unnoticed until a real, non-literal
> key surfaced it. No general fix exists without per-instantiation codegen
> or a trait/constraint system, neither of which this compiler has. If your
> struct embedding a `Map<string, V>` is itself non-generic, you can write
> your own plain (non-generic) method on it doing the linear scan directly
> instead of calling `.get()`/`.contains()`/`.tryGet()` — a field access
> from a non-generic method resolves fully concretely, so `==` there
> correctly reaches the real string operator. See `http.tr`'s
> `Request.get_header()`/`has_header()` and `Server.route_index()` for a
> real, working example of exactly this pattern.

### `Map.indexOf(key: K): int`

Linear scan; returns the index of `key`, or `-1` if not present.

### `Map.contains(key: K): bool`

`indexOf(key) >= 0`.

```turflang
mut m: Map<string, int>;
m.put("a", 1);
test::assert(m.contains("a") == true, "");
test::assert(m.contains("z") == false, "");
```

### `Map.put(key: K, value: V): void`

If `key` already exists, **overwrites the value in place** and does *not*
grow the map (`len` unchanged). Otherwise appends a new key/value pair,
growing both parallel arrays together.

```turflang
m.put("a", 1);
m.put("a", 100); // overwrite, doesn't grow
test::assert(m.length() == 3, ""); // still 3 after the overwrite
test::assert(m.get("a") == 100, "");
```

### `Map.get(key: K): V`

Returns the value for `key`.

> **Panics, doesn't return `!V`.** Calls `sys::panic("Map.get: no such
> key")` if `key` is missing. Check `.contains()` first, or use `tryGet`
> below if a missing key is an expected, recoverable case.

```turflang
test::assert(m.get("a") == 1, "");
```

### `error MapError { KeyNotFound }`

The error type `tryGet` produces on a miss.

### `Map.tryGet(key: K): !V`

Recoverable alternative to `get()` — produces `MapError::KeyNotFound`
instead of panicking on a missing key.

```turflang
import test;
import map::Map;

fn tryGetExisting(): int {
    mut m: Map<string, int> = buildMap();
    return m.tryGet("b") recover as err run {
        return -1;
    };
}

fn tryGetMissing(): int {
    mut m: Map<string, int> = buildMap();
    return m.tryGet("missing") recover as err run {
        if err == map::MapError::KeyNotFound {
            return -3;
        }
        return -4;
    };
}

// Or, with a plain default value instead of distinguishing the error:
mut x: int = m.tryGet("a") recover produce -1;      // succeeds -> 1
mut y: int = m.tryGet("nope") recover produce -1;   // recovers -> -1
```

### `Map.remove(key: K): void`

Removes `key` in O(1) by **swapping the removed entry with the last one** in
the backing arrays, then shrinking `len`. No-op if `key` is not present.

> **Gotcha: does not preserve insertion order.** Unlike `List`/`Stack`/
> `Queue`, which all preserve strict insertion/access order end to end,
> `Map<K, V>` stops being insertion-ordered the moment any `remove()` call
> happens — the entry that used to be last silently moves into the removed
> slot. Don't rely on iteration order after a removal.

```turflang
m.remove("b");
test::assert(m.contains("b") == false, "");
```

### `Map.length(): int`

Current key/value pair count.

---

## `Set<T>` (`collections/set.tr`)

Same underlying need as `Map<K, V>` — same type-erasure/linear-scan model
(see the top of this page), but with a single array instead of two, since a
set only stores presence, not an associated value.

### `Set.add(item: T): void`

Adds `item` if it isn't already present (checked via `indexOf`, a linear
scan); a no-op if it is — no duplicate entries.

### `Set.contains(item: T): bool`

### `Set.remove(item: T): bool`

Removes `item` if present (swap-with-last, same O(1)-removal/order-breaking
tradeoff as `Map.remove` above). Returns whether it was actually present
and removed.

### `Set.length(): int`

```turflang
import set::Set;

mut seen: Set<int> = Set::default();
seen.add(1);
seen.add(2);
seen.add(1); // no-op, already present
test::assert(seen.length() == 2, "");
test::assert(seen.contains(2), "");
```

> **Known limitation: dedup doesn't work correctly for `T = string`.**
> Root cause fully identified — the identical `K = string` gap `Map<K, V>`
> documents above, on `Set<T>`'s own `indexOf`. `self.items[i] == item`
> inside `Set.indexOf`'s single, shared generic-method body compares by raw
> pointer identity for `T = string`, not content, so adding `"hello"`,
> then a *separately-allocated* string that's content-equal to `"hello"`
> (e.g. via `strings::trim(" hello ")` — not a second occurrence of the
> same string literal, which the linker can merge to the same address by
> coincidence and mask the bug) produces a length one higher than
> expected — the second add never found the first. Correct for `T` =
> `int`/`double`/`bool`/pointer. No general fix exists without per-
> instantiation codegen or a trait/constraint system, neither of which this
> compiler has yet — prefer `Set<int>`/`Set<double>`, and don't rely on
> `Set<string>` deduplicating correctly. If you need one today, embed
> `items: T*`-shaped storage as a field of your own concrete (non-generic)
> struct and write a plain (non-generic) method doing the linear scan
> yourself (see `Map<K, V>`'s callout above for why that sidesteps the
> issue). Tracked as an accepted, deferred limitation in
> `tests/99_known_bugs/`, not a live regression.
