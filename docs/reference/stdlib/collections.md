# Collections: `List`, `Stack`, `Queue`, `Map`, `Set`

Five generic collection types, and `hashing`, the hash function behind `Map` and
`Set`. None of them is in the prelude: import the type, or the module and spell
the type with its module name.

| Type | Module | What it is |
|---|---|---|
| `List<T>` | `list` | growable array: index, push, pop, sort, search |
| `Stack<T>` | `stack` | last in, first out |
| `Queue<T>` | `queue` | first in, first out |
| `Map<K, V>` | `map` | keys to values, hashed |
| `Set<T>` | `set` | unique values, hashed |
| `hash(x)` | `hashing` | the hash `Map` and `Set` use |

```turflang
import list;
import list::List;
import map::Map;

fn main(): int {
    mut a: List<int> = List::default();   // after `import list::List;`
    a.push(1);

    mut b: list::List<int> = list::List::default();   // or after `import list;`
    b.push(2);

    mut m: Map<string, int> = Map::default();
    m.put("one", 1);
    return a.length() + b.length() + m.length() - 3;
}
```

The conventions for names, failure and the 2.x guarantee are in
[the standard library overview](README.md). How the collections relate to the
two heaps is in [Memory and Safety](../memory_and_safety.md).

## Creating a collection

`T::default()` of any collection is an empty, ready collection. A declaration
with no initializer is not: the variable has no value yet, and using it is
**E008** ("You're using 'l' before it has a value").

<!-- doc-example: fails E008 -->
```turflang
import list::List;

fn main(): int {
    mut l: List<int>;
    l.push(1);
    return 0;
}
```

Element types can be anything the language can store: `int`, `double`, `bool`,
`char`, `string`, an enum, a struct of any size, another collection, a reference.
Keys of a `Map` and elements of a `Set` have a further requirement, described in
[Keys: equality and hashing](#keys-equality-and-hashing).

```turflang
import list::List;

struct Reading {
    id: int,
    value: double,
    label: string,
    flag: bool
}

fn Reading.new(id: int, label: string): void {
    self.id = id;
    self.value = (id to double) * 1.5;
    self.label = label;
    self.flag = id % 2 == 0;
}

fn main(): int {
    mut readings: List<Reading> = List::default();
    mut i: int = 0;
    while i < 20 {
        readings.push(Reading::new(i, "r"));
        i = i + 1;
    }
    mut grid: List<List<int>> = List::default();
    grid.push(List::default());
    grid[0].push(7);
    return readings[19].id + grid[0][0] - 26;
}
```

## Shared by default, copied on demand

A collection value is a **handle** to one collection. `=`, passing it to a
function, returning it, and storing it in a field or in another collection all
share that one collection, its length included. `.copy()` makes an independent
one.

```turflang
import list::List;
import test;

fn fill(l: List<int>): void {
    l.push(1);   // the caller's list grows
}

fn main(): int {
    mut l: List<int> = List::default();
    fill(l);
    mut m: List<int> = l;     // same list, two names
    m.push(2);
    test::assert(l.length() == 2, "m and l are one list");

    mut c: List<int> = l.copy();   // a new list with the same elements
    c.push(3);
    test::assert(l.length() == 2 && c.length() == 3, "the copy is independent");
    return 0;
}
```

The same holds for `Stack`, `Queue`, `Set` and `Map`. A struct that holds a
collection is copied field by field, so both copies of the struct name the same
collection; copy the field with `.copy()` when you want them apart. `copy()`
copies the elements as values: a copy of a `List<&Node>` holds the same nodes.

Binding a collection with `immut` forbids the calls that change it
(`l.push(1)` on an `immut` list is **E060**, "You can't change the value of 'l'
because it's immutable"); reading calls such as `length()` and `get()` are fine.

<!-- doc-example: fails E060 -->
```turflang
import list::List;

fn main(): int {
    immut l: List<int> = List::default();
    l.push(1);
    return 0;
}
```

## Memory

A collection and its storage live on the **garbage-collected heap**. You never
free one: when nothing reachable refers to it any more, the collector reclaims
it, exactly like a `string`. There is no `free()`, `drop()` or `close()`. A loop
that builds and discards a thousand-element list a hundred thousand times runs
in a few megabytes.

- **Growth.** Storage starts empty. The first `push`, `enqueue`, `put` or `add`
  allocates room for 8 elements; whenever it is full, the next one doubles it.
  Element size is the real size of the element type, so a struct of several
  fields costs what it contains. Storage never shrinks: `pop`, `dequeue` and
  `remove` give the slot back for reuse, not to the collector.
- **What the collector sees.** The elements of a collection are traced: a
  `List<string>` keeps its strings alive, a `Map<string, List<int>>` keeps the
  inner lists alive, and a `List<&Node>` keeps the nodes alive.
- **Manual memory is not covered.** Only the collection's own storage is on the
  GC heap. A `List<void*>` of `mem::alloc` blocks does not make the collector
  responsible for those blocks: you still free them
  ([`mem`](mem.md), [the manual heap](../memory_and_safety.md#8-the-manual-heap)).
- **The fields are private.** `list.h` and the rest of a collection's layout
  are not part of the language: reading one is **E088**
  ("Field 'List.h' is private to module 'list'"). Use the methods and operators
  below. Raw pointer indexing into a collection's buffer is not possible; if you
  want a growable buffer of your own, declare your own struct.

## Failure: which calls panic, which return `!T`

A call that cannot be given a sensible answer is a bug in the caller, and
panics. Each panic message starts with the **caller's** `file:line:`, so it
names the call that was wrong, not a line inside the library. There is exactly
one recoverable lookup, `Map.try_get`.

| Call | Fails when | Message (after `file:line: `) |
|---|---|---|
| `List.pop()` | the list is empty | `List.pop: pop on empty list` |
| `List.get(i)`, `List.set(i, v)` | `i < 0` or `i >= length()` | `List.get: index out of bounds`, `List.set: index out of bounds` |
| `l[i]`, `l[i] = v` | `i` outside `-length() <= i < length()` | `List[]: index out of bounds`, `List[]=: index out of bounds` |
| `Stack.pop()` | the stack is empty | `Stack.pop: pop on empty stack` |
| `Stack.peek()` | the stack is empty | `Stack.peek: peek on empty stack` |
| `Queue.dequeue()` | the queue is empty | `Queue.dequeue: dequeue on empty queue` |
| `Queue.peek()` | the queue is empty | `Queue.peek: peek on empty queue` |
| `Map.get(k)`, `m[k]` | `k` is not in the map | `Map.get: no such key` |
| `s[i]` on a `Set` | `i` outside `0 <= i < length()` | `Set[]: index out of bounds` |
| `Map.try_get(k)` | `k` is not in the map | returns `!V` holding `MapError::KeyNotFound` |

Everything else succeeds: `Map.remove` of an absent key does nothing, `Set.add`
of a present value does nothing, `Set.remove` and the `contains`/`index_of`
family report absence through their result (`false`, `-1`).

```turflang
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    l.push(1);
    l.pop();
    l.pop();   // panics: List.pop: pop on empty list
    return 0;
}
```

Ask first with `length()` or `contains()`, or use `try_get` for a map lookup
that may miss. [Error handling](../error_handling.md) covers `recover`.

## Indexing with `[]` and `[]=`

`List` and `Map` support `x[i]` and `x[i] = v`; `Set` supports `s[i]` to read
only. The operators are ordinary generic `operator` declarations, so they work
for every element type. The rules for writing your own are in
[Generics and operators](../generics_and_operators.md#indexing-operator--and-operator-).

| Expression | Meaning |
|---|---|
| `l[i]` | the element at `i`; bounds-checked |
| `l[i] = v` | overwrite the element at `i`; bounds-checked |
| `l[-1]` | a negative index counts from the end: `-length() <= i < length()` is valid, and `l[-length() - 1]` panics |
| `m[k]` | `m.get(k)`: panics if `k` is missing |
| `m[k] = v` | `m.put(k, v)`: inserts or overwrites |
| `s[i]` | the `i`-th element of a set in its stored order (see [Set](#set)) |

`l.get(i)` and `l.set(i, v)` are the same operations as methods, with one
difference: they accept only `0 <= i < length()`, so a negative index to `get`
or `set` panics.

```turflang
import list::List;
import map::Map;
import test;

fn main(): int {
    mut l: List<int> = List::default();
    l.push(1);
    l.push(2);
    l.push(3);
    test::assert(l[0] == 1 && l[-1] == 3, "from the front and from the back");
    l[1] = 20;
    l.set(2, 30);
    test::assert(l.get(1) == 20 && l[-1] == 30, "writes through [] and set");

    mut names: List<string> = List::default();
    names.push("ada");
    names[0] = "grace";
    test::assert(names[0] == "grace", "any element type");

    mut ages: Map<string, int> = Map::default();
    ages["ada"] = 36;
    ages["ada"] = 37;
    test::assert(ages["ada"] == 37 && ages.length() == 1, "m[k] = v inserts or overwrites");
    return 0;
}
```

Two limits follow from `x[i]` returning the element **by value**:

- `l[i].field` and `l[i].method()` read through the element, but a write into
  a field of that copy (`l[i].f = v`, `l[i].f += 1`) or a call to a method that
  assigns to `self` is **E098** ("Cannot assign to part of the value an
  'operator[]' read returned"). Read the element into a variable, change it, and
  store it back: `mut e: T = l[i]; e.f = v; l[i] = e;`.
  A collection element is a handle, so `grid[i][j] = v` and `outer[i].push(v)`
  change the inner collection itself and are fine.
- `m[k] += 1` is **E126** ("Compound assignment through 'operator[]' is not
  supported"). Write `m[k] = m[k] + 1`.

<!-- doc-example: fails E098 -->
```turflang
import list::List;

struct Counter { n: int }

fn main(): int {
    mut l: List<Counter> = List::default();
    l.push(Counter::default());
    l[0].n = 5;
    return 0;
}
```

<!-- doc-example: fails E126 -->
```turflang
import map::Map;

fn main(): int {
    mut m: Map<string, int> = Map::default();
    m["a"] = 1;
    m["a"] += 1;
    return 0;
}
```

`Set` has no `[]=`: a set's elements are unique, and overwriting a position
could break that (`s[0] = 1` is **E056**, "No `operator[]=` overload matches").

## Iteration

A collection is not a built-in array, so `for x in l` does not work on it
(**E005**, "for-each requires an array"). Walk a `List` by index, a `Stack` or
`Queue` by emptying it, and a `Map` through `keys()` or `values()`.

<!-- doc-example: fails E005 -->
```turflang
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    l.push(1);
    for x in l {
        printline(x);
    }
    return 0;
}
```

```turflang
import list::List;
import map::Map;
import io;

fn main(): int {
    mut l: List<string> = List::default();
    l.push("a");
    l.push("b");
    mut i: int = 0;
    while i < l.length() {
        io::printline(l[i]);
        i = i + 1;
    }

    mut m: Map<string, int> = Map::default();
    m.put("x", 1);
    m.put("y", 2);
    immut ks: List<string> = m.keys();
    mut j: int = 0;
    while j < ks.length() {
        io::printline(ks[j] + " = " + (m.get(ks[j]) to string));   // x = 1, then y = 2
        j = j + 1;
    }
    return 0;
}
```

`Map.keys()` and `Map.values()` return **copies**, so changing the map while you
walk a result cannot invalidate it. The `i`-th key and the `i`-th value belong to
one entry as long as the map is not changed between the two calls. The order is
insertion order until the first `remove`; see [Map](#map).

## Sorting and searching

`List<T>` has three methods that take a function. The comparator has the shape
`fn(T, T): int` and returns a negative number, zero or a positive number as the
first argument is less than, equal to or greater than the second. A named
function is passed by name.

| Method | Does |
|---|---|
| `sort(cmp)` | sorts in place, ascending by `cmp`. Insertion sort: O(n^2), and **stable** (elements that compare equal keep their order) |
| `binary_search(target, cmp)` | the index of an element equal to `target` under `cmp`, or `-1`. O(log n). The list **must already be sorted** by the same `cmp`; it is not checked |
| `find(predicate)` | the index of the first element for which `predicate` (`fn(T): bool`) is true, or `-1`. O(n), any order |

```turflang
import list::List;
import io;

struct Person {
    name: string,
    age: int
}

fn Person.new(name: string, age: int): void {
    self.name = name;
    self.age = age;
}

fn by_age(a: Person, b: Person): int {
    return a.age - b.age;
}

fn by_name(a: string, b: string): int {
    if a < b {
        return -1;
    }
    if a > b {
        return 1;
    }
    return 0;
}

fn is_adult(p: Person): bool {
    return p.age >= 18;
}

fn main(): int {
    mut people: List<Person> = List::default();
    people.push(Person::new("ann", 30));
    people.push(Person::new("bob", 10));
    people.push(Person::new("cy", 30));
    people.push(Person::new("di", 5));

    people.sort(by_age);                 // di, bob, ann, cy: ann stays before cy
    io::printline(people[0].name + " " + people[2].name + " " + people[3].name);

    immut first_adult: int = people.find(is_adult);   // 2
    io::printline(first_adult to string);

    mut words: List<string> = List::default();
    words.push("pear");
    words.push("apple");
    words.push("fig");
    words.sort(by_name);                           // apple, fig, pear
    io::printline(words.binary_search("fig", by_name) to string);    // 1
    io::printline(words.binary_search("plum", by_name) to string);   // -1
    return 0;
}
```

Passing something that is not a function of that shape is **E056** ("No
matching overload"). There is no sort for `Stack`, `Queue`, `Map` or `Set`;
sort a `List` copy of the data.

## Keys: equality and hashing

A `Map` finds a key, and a `Set` finds an element, with two operations: a hash
to pick where to look, and `==` to confirm the match. Both come from the key's
type, field by field:

- **`int`, `char`, `bool`, enums, `double`**: by value. `-0.0` and `0.0` are one
  key. `NaN` is never equal to anything, itself included, so a `NaN` key is
  never found: each `put` of one adds a new entry.
- **`string`**: by **content**. Two different string values with the same text
  are one key, wherever they came from.
- **A struct**: you declare `operator ==` for it (comparing its fields), and the
  hash is computed from the fields, never from padding. A struct without an
  `operator ==` cannot be a key: the compiler reports **E011** ("Cannot compare struct values
  with '=='") at the `put` or `add`. A struct with `string`
  fields compares and hashes those by content.
- **A fixed array** cannot be a key: arrays do not support `==` (**E011**,
  "Cannot compare arrays with '=='"). Wrap it in a struct with its own
  `operator ==`.

The one rule you must keep: `a == b` has to imply the same hash. That holds
automatically as long as your `operator ==` compares field by field. An
`operator ==` that equates values whose fields differ (case-insensitive text,
say) breaks it, and keys that "should" match are missed. Normalize such a key
(lower-case it) before using it.

```turflang
import map::Map;
import set::Set;
import strings;
import test;

struct Point {
    x: int,
    y: int
}

fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

operator ==(a: Point, b: Point): bool {
    return a.x == b.x && a.y == b.y;
}

struct Tag {
    name: string,
    id: int
}

fn Tag.new(name: string, id: int): void {
    self.name = name;
    self.id = id;
}

operator ==(a: Tag, b: Tag): bool {
    return a.name == b.name && a.id == b.id;
}

fn main(): int {
    mut by_name: Map<string, int> = Map::default();
    by_name.put(strings::repeat("ab", 2), 1);                 // built at run time
    test::assert(by_name.get("abab") == 1, "string keys compare by content");

    mut by_point: Map<Point, string> = Map::default();
    by_point.put(Point::new(1, 2), "here");
    test::assert(by_point.get(Point::new(1, 2)) == "here", "struct key");

    mut tags: Set<Tag> = Set::default();
    tags.add(Tag::new(strings::repeat("z", 3), 1));
    test::assert(tags.contains(Tag::new("zzz", 1)), "string field by content");

    mut by_double: Map<double, int> = Map::default();
    by_double.put(0.0, 1);
    by_double.put(-0.0, 2);
    test::assert(by_double.length() == 1, "-0.0 and 0.0 are one key");
    return 0;
}
```

### `hashing::hash(x: T): int`

The hash `Map` and `Set` use, public so your own tables can use it. There is a
`string` form and a generic form for every other type; both return the value of
the built-in `hash_of` ([Variables and types](../variables_and_types.md#hash_ofexpr)):
equal values give equal hashes, and the result is an `int` of no particular
range. Do not store it; it can change between compiler versions.

```turflang
import hashing;
import strings;
import test;

fn main(): int {
    test::assert(hashing::hash(strings::repeat("ab", 2)) == hashing::hash("abab"), "same text, same hash");
    test::assert(hashing::hash(7) == hashing::hash(7), "same value, same hash");
    return 0;
}
```

## Complexity

`n` is the length of the collection.

| Operation | Cost |
|---|---|
| `List.push`, `Stack.push`, `Queue.enqueue` | amortised O(1); the occasional doubling copies the storage. A `Queue.enqueue` into a full buffer that has dequeued elements instead moves the live elements to the front, O(n) |
| `pop`, `peek`, `dequeue`, `length`, `get`, `set`, `l[i]`, `s[i]` | O(1) |
| `List.find` | O(n) |
| `List.sort` | O(n^2), stable |
| `List.binary_search` | O(log n) |
| `Map.put`, `get`, `try_get`, `contains`, `index_of`, `remove`, `m[k]` | O(1) average |
| `Set.add`, `contains`, `index_of`, `remove` | O(1) average |
| `copy`, `Map.keys`, `Map.values` | O(n) |

---

## `List<T>`

A growable array, the general-purpose sequence. Elements are stored in order; an
index is a position from 0.

### `List.push(item: T): void`

Appends `item` at the end. Amortised O(1).

### `List.pop(): T`

Removes and returns the last element. Panics on an empty list (see
[Failure](#failure-which-calls-panic-which-return-t)).

### `List.length(): int`

The number of elements (not the capacity).

### `List.get(i: int): T` / `List.set(i: int, value: T): void`

Read and write position `i`, `0 <= i < length()`; any other `i` panics. Same as
`l[i]` and `l[i] = v` for a non-negative index; use them where a named call
reads better or when the index may not be negative.

```turflang
import list::List;
import test;

fn main(): int {
    mut l: List<string> = List::default();
    l.push("a");
    l.push("b");
    l.set(0, "c");
    test::assert(l.get(0) == "c" && l.length() == 2, "get and set");
    test::assert(l.pop() == "b" && l.length() == 1, "pop");
    return 0;
}
```

### `operator<T> [](l: List<T>, i: int): T` / `operator<T> []=(l: List<T>, i: int, value: T): void`

`l[i]` and `l[i] = v`, with negative indices from the end. See
[Indexing](#indexing-with--and-).

### `List.copy(): List<T>`

A new, independent list with the same elements in the same order.

### `List.sort(cmp: fn(T, T): int): void`

### `List.binary_search(target: T, cmp: fn(T, T): int): int`

### `List.find(predicate: fn(T): bool): int`

See [Sorting and searching](#sorting-and-searching).

---

## `Stack<T>`

Last in, first out. Push and pop at the same end.

### `Stack.push(item: T): void`

Puts `item` on top.

### `Stack.pop(): T`

Removes and returns the top element. Panics if empty.

### `Stack.peek(): T`

Returns the top element without removing it. Panics if empty.

### `Stack.length(): int`

The number of elements.

### `Stack.copy(): Stack<T>`

A new, independent stack with the same elements in the same order.

```turflang
import stack::Stack;
import test;

fn main(): int {
    mut s: Stack<int> = Stack::default();
    s.push(1);
    s.push(2);
    s.push(3);
    test::assert(s.peek() == 3 && s.length() == 3, "top is the last pushed");
    test::assert(s.pop() == 3 && s.pop() == 2, "last in, first out");
    return 0;
}
```

---

## `Queue<T>`

First in, first out. `enqueue` at the back, `dequeue` from the front. The queue
reuses the space of elements already dequeued before it asks for more, so a
queue that is filled and drained at a steady length does not grow.

### `Queue.enqueue(item: T): void`

Adds `item` at the back.

### `Queue.dequeue(): T`

Removes and returns the front element. Panics if empty.

### `Queue.peek(): T`

Returns the front element without removing it. Panics if empty.

### `Queue.length(): int`

The number of waiting elements.

### `Queue.copy(): Queue<T>`

A new, independent queue holding the waiting elements in the same order.

```turflang
import queue::Queue;
import test;

fn main(): int {
    mut q: Queue<int> = Queue::default();
    mut i: int = 0;
    while i < 100 {
        q.enqueue(i);
        i = i + 1;
    }
    i = 0;
    while i < 90 {
        test::assert(q.dequeue() == i, "first in, first out");
        i = i + 1;
    }
    i = 100;
    while i < 300 {
        q.enqueue(i);
        i = i + 1;
    }
    test::assert(q.length() == 210 && q.peek() == 90, "order survives reuse and growth");
    return 0;
}
```

---

## `Map<K, V>`

Keys to values. Each key appears once; `put` of an existing key overwrites its
value. Keys are found by hash and `==` (see
[Keys: equality and hashing](#keys-equality-and-hashing)).

### `Map.put(key: K, value: V): void`

Inserts `key` with `value`, or replaces the value if `key` is already present
(the length does not change).

### `Map.get(key: K): V`

The value for `key`. Panics (`Map.get: no such key`) if absent.

### `Map.try_get(key: K): !V`

The recoverable lookup: a miss is the error `MapError::KeyNotFound`, not a
panic.

### `error MapError { KeyNotFound }`

The error `try_get` returns.

```turflang
import map::Map;
import io;

fn lookup(m: Map<string, int>, key: string): int {
    return m.try_get(key) recover as err run {
        if err == map::MapError::KeyNotFound {
            return -1;
        }
        return -2;
    };
}

fn main(): int {
    mut m: Map<string, int> = Map::default();
    m.put("a", 1);
    m.put("a", 100);                                    // overwrite: still one entry
    immut x: int = m.try_get("a") recover produce -1;   // 100
    immut y: int = m.try_get("zz") recover produce -1;  // -1
    io::printline((x to string) + " " + (y to string) + " " + (lookup(m, "zz") to string));
    return m.length() - 1;
}
```

### `Map.contains(key: K): bool`

True if `key` is present.

### `Map.index_of(key: K): int`

The position of `key` among `keys()`, or `-1` if absent. Positions change when
an entry is removed.

### `Map.remove(key: K): void`

Removes `key` and its value; does nothing if `key` is absent. Removal fills the
hole with the entry that was last, so **the order of `keys()` and `values()` is
insertion order only until the first `remove`**. Do not rely on order after a
removal.

```turflang
import list::List;
import map::Map;
import strings;
import io;

fn main(): int {
    mut m: Map<string, int> = Map::default();
    m.put("a", 1);
    m.put("b", 2);
    m.put("c", 3);
    m.put("d", 4);
    m.remove("a");
    m.remove("zz");                                  // absent: nothing happens
    immut ks: List<string> = m.keys();
    io::printline(strings::join(ks, ","));           // d,b,c
    return m.length() - 3;
}
```

### `Map.length(): int`

The number of entries.

### `Map.keys(): List<K>` / `Map.values(): List<V>`

Copies of the keys and of the values, in the same order. See
[Iteration](#iteration).

### `Map.copy(): Map<K, V>`

A new, independent map with the same entries in the same order.

### `operator<K, V> [](m: Map<K, V>, key: K): V` / `operator<K, V> []=(m: &Map<K, V>, key: K, value: V): void`

`m[key]` is `get`, and `m[key] = v` is `put`. See
[Indexing](#indexing-with--and-).

---

## `Set<T>`

A collection of unique values. Membership is decided by hash and `==`, like a
`Map` key.

### `Set.add(item: T): void`

Adds `item`; does nothing if it is already present.

### `Set.contains(item: T): bool`

True if `item` is present.

### `Set.index_of(item: T): int`

The position of `item` in the set's stored order (what `s[i]` indexes), or `-1`.

### `Set.remove(item: T): bool`

Removes `item`. Returns `true` if it was present, `false` if not. As with
`Map.remove`, the stored order changes after a removal: the last element takes
the freed position.

### `Set.length(): int`

The number of elements.

### `Set.copy(): Set<T>`

A new, independent set with the same elements in the same order.

### `operator<T> [](s: Set<T>, i: int): T`

`s[i]` reads the element at position `i`, `0 <= i < length()` (no negative
indices). Positions are insertion order until the first `remove`. Panics out of
range.

```turflang
import set::Set;
import test;

fn main(): int {
    mut seen: Set<string> = Set::default();
    seen.add("x");
    seen.add("y");
    seen.add("z");
    seen.add("x");                                   // already present
    test::assert(seen.length() == 3, "no duplicates");
    test::assert(seen.remove("x") && !seen.remove("x"), "remove reports presence");
    test::assert(seen[0] == "z" && seen[1] == "y", "z moved into x's place");
    test::assert(seen.contains("y") && seen.index_of("y") == 1, "lookup");
    return 0;
}
```
