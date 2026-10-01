# Collections: `List`, `Stack`, `Queue`, `Map`, `Set`

Generic collection types. None of these are prelude — each requires an
explicit import naming the type, e.g. `import list::List;`, or a plain
`import list;` combined with the fully-qualified spelling `list::List<T>`.

```turflang
import list::List;
import stack::Stack;
import queue::Queue;
import map::Map;
import set::Set;
```

All five are default-constructible via `T::default()` — `mut l: List<int> =
List::default();` gives you an empty, valid collection. (A bare `mut l:
List<int>;` with no initializer no longer works, since SEM-004 removed the
old eager-zero-init mechanism that used to make it equivalent — see
`docs/reference/variables_and_types.md`'s initialization chapter.)

## Shared design: real per-instantiation storage, real growth

Every collection in this module — `List<T>`, `Stack<T>`, `Queue<T>`, `Map<K,
V>`, `Set<T>`, and [`mem::Pool<T>`](./mem.md) — shares the same underlying
growth scheme, so it's worth understanding once instead of per-type:

- **Real per-instantiation element sizes.** Generic storage is a raw
  pointer (`T*`) sized in bytes as `cap * sizeof(T)` — `sizeof(T)` is a
  genuine, monomorphized per-instantiation byte count (TYPE-002), not a
  hardcoded assumption. **`T` can be any type, including a struct larger
  than one machine word** — `List<Big>` (a real, multi-field struct, by
  value) works directly; see `tests/38_monomorphization/
  06_generic_struct_over_8_bytes.tr`. Before TYPE-002 landed, every `T` was
  assumed to fit in one 8-byte slot and a struct over 8 bytes had to be
  worked around by instantiating with a pointer instead (`List<MyStruct*>`)
  — that workaround is no longer *necessary*, though it's still legal if
  you want it (a pointer element is always exactly one word regardless of
  what it points at).
- **Doubling-from-8 growth.** Every push-like operation follows the same
  rule: if the backing array is at capacity, grow it — `cap == 0` becomes
  `cap = 8`, otherwise `cap *= 2` — via `gc_alloc`/`gc_realloc`. This is the
  same growth logic in `List.push`, `Stack.push`, `Queue.enqueue` (when it
  can't compact instead — see below), `Map.put`, and `Set.add`.

Bugs to know about: none reported for `List`/`Stack`; `Queue` has one
documented behavioral quirk below (compaction order); `Map`/`Set` no longer
preserve insertion order once a `remove()` happens (see below — true since
before this group too, just for a different underlying reason now).

---

## `List<T>` (`collections/list.tr`)

```turflang
struct List<T> { data: T*, len: int, cap: int }
```

A dynamically-growing array — the general-purpose sequence type. Internally
`import sys;`.

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

### `operator<T> [](l: List<T>, i: int): T` / `operator<T> []=(l: List<T>, i: int, value: T): void`

`l[i]` / `l[i] = v` for **every** element type (LEC-006) — bounds-checked
(`sys::panic` out of range). `l.get(i)` / `l.set(i, v)` are the same
operations as named methods. Indexing a `List<T>` is never `unsafe`; the raw
`l.data[i]` is, and is unchecked.

```turflang
mut l: List<int> = List::default();
l.push(1);
test::assert(l[0] == 1, "");
l[0] = 99;
test::assert(l[0] == 99, "");

mut names: List<string> = List::default();
names.push("ada");
names[0] = "grace";
test::assert(names[0] == "grace", "");
```

`l[i]` yields the element **by value**, so `l[i].field` and `l[i].method()`
read through it (for a struct element of any size), but you cannot assign
into a field of it (`l[i].f = v`), or call a method that changes it
(`outer[i].push(v)`) — those are E098; `grid[i][j] = v` is fine. See
[Generics and Operators](../generics_and_operators.md#gotcha-xi-is-a-value-not-a-place-e098).

### `List.sort(cmp: fn(T, T): int): void` / `List.binary_search(target: T, cmp: fn(T, T): int): int` / `List.find(predicate: fn(T): bool): int`

Genuinely generic `List<T>` methods (STD-003) — sort, search, and find,
over any element type, not just `int`.

> **This used to be `List<int>`-only.** A comparator parameter typed
> `fn(T, T): int` used to fail to type-check correctly at the call site,
> because the compiler's generic substitution machinery didn't recurse
> into a `FuncType`'s own parameter/return types. Fixed directly in the
> compiler (STD-003, `docs/development/changes/STD-003.md`) — not a
> stdlib workaround, a real fix across five call sites in the type
> checker/resolver/monomorphizer, since a comparator callback is ordinary
> code any Turf program might write, not something specific to this file.

- **`sort`** — in-place insertion sort. `cmp(a, b)` should return
  negative/zero/positive, matching C's `qsort` convention.
- **`binary_search`** — only meaningful on an already-sorted list
  (not checked/enforced); returns the index if found, `-1` otherwise.
- **`find`** — linear search with an arbitrary predicate, for
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

nums.sort(byValue);
test::assert(nums.find(isEven) >= 0, "found an even number");
```

> **Removed in 2.0 (LEC-007).** The concrete `list::list_sort`, `list::list_binary_search` and `list::list_find` free functions (deprecated wrappers over the `List<int>` case since STD-003) no longer exist. Call the methods above: `l.sort(cmp)`, `l.binary_search(target, cmp)`, `l.find(predicate)` — they work for every element type, not just `int`.

---

## `Stack<T>` (`collections/stack.tr`)

```turflang
struct Stack<T> { data: T*, len: int, cap: int }
```

LIFO. Structurally identical growth logic to `List<T>` — same
doubling-from-8 scheme, same real per-instantiation element sizes.

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
  double `cap`, per the shared growth policy.

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
struct Map<K, V> { keys: K*, values: V*, len: int, cap: int, buckets: int*, bcap: int }
```

A **real hash table, O(1) average lookup** — not the linear scan this type
used to be. Two parts:

- `keys`/`values`/`len`/`cap` are a dense, insertion-ordered pair of arrays
  (exactly as before) — grown the same way `List<T>.push()` is.
- `buckets`/`bcap` are a separate open-addressing hash INDEX layered on top
  — `hash(key)` picks a bucket, which stores the DENSE ARRAY INDEX of that
  key (or an empty/tombstone sentinel). `index_of()`/`get()`/`contains()`/
  `put()`/`remove()` all probe this index instead of scanning `keys`
  linearly.

This split (rather than storing entries directly in hash-bucket order) is
deliberate: `Set<int>`'s own `operator[]` (below) needs INSERTION-ORDER
positional access, which a pure open-addressing table can't provide at all
— see `docs/development/changes/STD-003.md` for the full investigation.
`hash(K)` is `hashing::hash()` (`collections/hashing.tr`) — a real, general
`hash(T)` only became possible with monomorphization (TYPE-002); see that
file's own header for its design (one concrete `string` overload, one
generic byte-hash fallback covering everything else).

Because lookups need both `==` (to disambiguate a hash collision) and
`hash()` (to pick a bucket) on `K`, your key type must support both. `int`
and `string` are covered directly by `hashing.tr`'s own overloads; any other
struct key needs its own `operator==` (as before) and gets `hash()` for free
via the generic byte-hash fallback — see that file's header for the one
caveat (a struct whose `operator==` compares a pointer-shaped field, e.g.
another string, by content rather than identity, isn't guaranteed to hash
consistently with it; not a concern for any plain-data key).

### `Map.index_of(key: K): int`

O(1) average; returns the dense-array index of `key`, or `-1` if not
present.

### `Map.contains(key: K): bool`

`index_of(key) >= 0`.

```turflang
mut m: Map<string, int>;
m.put("a", 1);
test::assert(m.contains("a") == true, "");
test::assert(m.contains("z") == false, "");
```

### `Map.put(key: K, value: V): void`

If `key` already exists, **overwrites the value in place** and does *not*
grow the map (`len` unchanged). Otherwise appends a new key/value pair,
growing both the dense arrays and (independently, on its own load factor)
the bucket index as needed.

```turflang
m.put("a", 1);
m.put("a", 100); // overwrite, doesn't grow
test::assert(m.length() == 3, ""); // still 3 after the overwrite
test::assert(m.get("a") == 100, "");
```

### `Map.get(key: K): V`

Returns the value for `key`.

> **Panics, doesn't return `!V`.** Calls `sys::panic("Map.get: no such
> key")` if `key` is missing. Check `.contains()` first, or use `try_get`
> below if a missing key is an expected, recoverable case.

```turflang
test::assert(m.get("a") == 1, "");
```

### `error MapError { KeyNotFound }`

The error type `try_get` produces on a miss.

### `Map.try_get(key: K): !V`

Recoverable alternative to `get()` — produces `MapError::KeyNotFound`
instead of panicking on a missing key.

```turflang
import test;
import map::Map;

fn tryGetExisting(): int {
    mut m: Map<string, int> = buildMap();
    return m.try_get("b") recover as err run {
        return -1;
    };
}

fn tryGetMissing(): int {
    mut m: Map<string, int> = buildMap();
    return m.try_get("missing") recover as err run {
        if err == map::MapError::KeyNotFound {
            return -3;
        }
        return -4;
    };
}

// Or, with a plain default value instead of distinguishing the error:
mut x: int = m.try_get("a") recover produce -1;      // succeeds -> 1
mut y: int = m.try_get("nope") recover produce -1;   // recovers -> -1
```

### `Map.remove(key: K): void`

Removes `key` in O(1) average by **swapping the removed entry with the
last one** in the dense arrays, then shrinking `len`, and retargeting the
hash index for whichever key just moved. No-op if `key` is not present.

> **Gotcha: does not preserve insertion order.** `Map<K, V>` stops being
> insertion-ordered the moment any `remove()` call happens — the entry that
> used to be last silently moves into the removed slot. Don't rely on
> iteration order after a removal.

```turflang
m.remove("b");
test::assert(m.contains("b") == false, "");
```

### `Map.length(): int`

Current key/value pair count.

### `operator<K, V> [](m: Map<K, V>, key: K): V` / `operator<K, V> []=(m: &Map<K, V>, key: K, value: V): void`

`m[key]` / `m[key] = v`, wrapping `get()`/`put()`, for **every** `Map<K, V>`
(LEC-006). `m[key]` on a missing key panics, like `get()`; use `contains()`
first, or the recoverable lookup above.

```turflang
mut m: Map<string, List<int>> = Map::default();
mut inner: List<int> = List::default();
inner.push(10);
m["k"] = inner;
test::assert(m["k"][0] == 10, "");
```

---

## `Set<T>` (`collections/set.tr`)

```turflang
struct Set<T> { items: T*, len: int, cap: int, buckets: int*, bcap: int }
```

Same underlying design as `Map<K, V>` — a real hash-index lookup layered
over a dense, insertion-ordered `items` array — but with a single array
instead of two, since a set only stores presence, not an associated value.

### `Set.add(item: T): void`

Adds `item` if it isn't already present (checked via the hash index, O(1)
average); a no-op if it is — no duplicate entries.

### `Set.contains(item: T): bool`

### `Set.remove(item: T): bool`

Removes `item` if present (swap-with-last on the dense array, same O(1)-
average-removal/insertion-order-breaking tradeoff as `Map.remove` above).
Returns whether it was actually present and removed.

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

### `operator<T> [](s: Set<T>, i: int): T`

`s[i]` — positional read of the i-th element **in insertion order** (the
dense `items` array is unaffected by hash-bucket order; only a `remove()`
breaks strict insertion order, exactly like `Map`'s own gotcha above).
Bounds-checked (`sys::panic` out of range). Works for every `Set<T>`
(LEC-006). Deliberately no `operator[]=` — a set's invariant is
uniqueness, and positional overwrite could silently violate it; `add()`/
`remove()` stay the only way to mutate a `Set`'s contents.

```turflang
mut s: Set<int> = Set::default();
s.add(10);
s.add(20);
test::assert(s[0] == 10, "");
test::assert(s[1] == 20, "");
```
