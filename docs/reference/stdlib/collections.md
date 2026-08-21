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

Because lookups use `==` on `K`, your key type must support equality — a
primitive, or `string` via the operator overload in `strings.tr`.

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
