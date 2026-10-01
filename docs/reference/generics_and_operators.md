# Generics and Operator Overloading

## Generics

### Declaring a generic struct

A struct declares its type parameters in `< >` after its name:

```turflang
struct Box<T> {
    val: T*
}
```

Multiple type parameters are comma-separated:

```turflang
struct Map<K, V> {
    keys: K*,
    values: V*,
    len: int,
    cap: int
}
```

### Generic methods

A method with its own type parameters puts the `<T>` block right after
`fn`, before the struct-dot method name:

```turflang
unsafe {
    struct Box<T> {
        val: T*
    }
}

fn<T> Box.set(v: &T): void {
    self.val = v;
}

fn<T> Box.get(): &T {
    unsafe {
        return self.val to &T;
    }
}
```

### Constructing a generic type

Construction goes through the exact same `::new(...)`/`::default()`
machinery as an ordinary struct. There is **no** explicit-type-argument
construction syntax — you never write `Box<int>::new(...)` or a generic
brace literal. The concrete type argument is carried entirely by the
surrounding declared variable's type:

```turflang
mut b: Box<int> = Box::default();  // OK -- Box::default() itself produces a bare Box
mut b2: Box<int> = Box::new(42);   // also OK, same principle
```

> [!NOTE]
> `<` after an identifier in expression position is always parsed as the
> less-than operator — there is no explicit generic-type-argument call
> syntax like `Box<int>::new(...)`. The type argument is inferred entirely
> from context (the declared type of the variable you're assigning into).

A full example, including a generic method whose return type resolves to
the concrete instantiation type (not the bare type parameter `T`):

```turflang
struct Box<T> {
    val: T
}

fn<T> Box.new(v: T): void {
    self.val = v;
}

fn<T> Box.get(): T {
    return self.val;
}

fn main(): int {
    mut b: Box<int> = Box::new(42);
    test::assert(b.get() == 42, "Box<int>.get() should return 42");
    test::assert((b.get() to string) == "42", "generic return type resolves to int, not 'T'");
    return 0;
}
```

Two independent instantiations of the same generic struct coexist without
interference in the same file:

```turflang
mut b: Box<int> = Box::default();
mut n: int = 42;
b.set(&n);

mut sb: Box<string> = Box::default();
mut s: string = "hello";
sb.set(&s);
```

### Type-erasure model

Every instantiation of a generic struct shares the exact same underlying
layout: a field typed with the struct's own type parameter is a pointer,
which erases to an opaque pointer regardless of what it actually points to.
The standard library's own collections describe this directly: *"In Type
Erasure, all T are 64 bits (pointers, int, double)."*

This is why field access and method calls on a generic-struct-typed value
still resolve to the correct concrete type at each use site — the compiler
substitutes the type parameter back to the concrete instantiation type
wherever it's used, so `Box<int>.get()` is understood to return `int`, not
a bare, unresolved `T`.

### The `List<T*>` workaround for large element types

`List<T>` (and similarly `Map<K, V>`) stores each element in a fixed
8-byte slot — one pointer/`int`/`double` width. A struct **larger than 8
bytes** cannot be stored directly as `List<StructName>`'s element type.

The established workaround is to instantiate the collection with a
**pointer** element type instead (`List<StructName*>`), heap-allocating
each element and storing a pointer to it rather than the struct by value:

```turflang
import mem;
import list::List;

struct Entry {
    key: string,
    value: string
}

fn main(): int {
    mut store: List<Entry*> = List::default();
    unsafe {
        mut e: Entry* = mem::alloc(16) to Entry*;
        e.key = "language";
        e.value = "Turf";
        store.push(e);
    }

    unsafe {
        test::assert(store.data[0].key == "language", "");
        test::assert(store.data[0].value == "Turf", "");
    }
    return 0;
}
```

> [!NOTE]
> This is an accepted architectural limitation, not something planned to be
> silently fixed underneath you — if `StructName` is larger than 8 bytes,
> `List<StructName>` is not a supported instantiation; use
> `List<StructName*>` and manage the heap allocation yourself (typically
> via `mem::alloc()`), inside `unsafe { }`.

---

## Operator Overloading

### Syntax

Operators are declared at the top level with the `operator` keyword — never
`.`- or `::`-qualified, never `static`. They may take a type-parameter list
right after the keyword (see [Generic operators](#generic-operators)):

```turflang
struct Vec2 {
    x: int,
    y: int
}

operator +(a: Vec2, b: Vec2): Vec2 {
    mut r: Vec2 = Vec2::default();
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    return r;
}

fn main(): int {
    mut a: Vec2 = Vec2::default();
    a.x = 1;
    a.y = 2;
    mut b: Vec2 = Vec2::default();
    b.x = 10;
    b.y = 20;
    mut c: Vec2 = a + b;

    test::assert(c.x == 11, "Vec2 + Vec2 should add x components");
    test::assert(c.y == 22, "Vec2 + Vec2 should add y components");
    return 0;
}
```

An `operator` declaration is, structurally, just an ordinary function with
a specially-generated internal name — every other part of the compiler
treats it exactly like any other overloaded function.

### The overloadable set

Overloadable:

```
binary   +  -  *  /  %  **  &  |  ^  <<  >>  ==  !=  <  >  <=  >=
unary    -  !  ~
indexing []  (read, `x[i]`)   []=  (write, `x[i] = v`)
```

A one-parameter `operator -(a: Vec2): Vec2` is unary negation and a
two-parameter one is subtraction; arity tells them apart. `operator []`
takes `(container, index)` and `operator []=` takes `(container, index,
value)`. The container of `[]=` may be declared `&T` so the write reaches the
caller's value (`Map` does this); the other operands are never taken by
reference implicitly.

Deliberately **not** overloadable:

- Logical operators: `&& ||` (they short-circuit, which a call cannot)
- Assignment `=`, call `()`, member access `.`, and `to` conversions
- Compound assignment as its own operator: `a += b` is exactly
  `a = a + b` and uses `operator +`; there is no `operator +=`

Declaring an operator outside this set is a parse-time error.

### Both operands already builtin

An operator declaration is rejected if **both** operand types are already
one of the built-in primitive types (`int`, `int32`, `double`, `bool`,
`char`, or a pointer type):

```turflang
operator +(a: int, b: int): int { return a; }     // ERROR: both operands builtin
operator +(a: char, b: char): char { return a; }  // ERROR: both operands builtin
```

The rationale (for `char` specifically): character arithmetic is not meant
to be reintroduced through a user overload — cast to `int` explicitly if
you need it.

> [!NOTE]
> `string` is **not** in this builtin-operand list — this is deliberate.
> `string`'s own `+`, `==`, `!=`, `<`, `>`, `<=`, `>=` are themselves
> ordinary `operator` overloads, declared in the standard library
> (`lib/std/core/strings.tr`), not compiler builtins:
>
> ```turflang
> // lib/std/core/strings.tr
> operator +(a: string, b: string): string { ... }
> operator ==(a: string, b: string): bool { ... }
> // != < > <= >= similarly
> ```
>
> Using them requires an explicit `import strings;`:
>
> ```turflang
> import strings;
>
> fn main(): int {
>     mut a: string = "foo";
>     mut b: string = "bar";
>
>     test::assert(a + b == "foobar", "string concatenation via operator+");
>     test::assert(a == "foo", "string equality via operator==");
>     test::assert(a != b, "string inequality via operator!=");
>     test::assert(b < a, "string ordering via operator< (bar < foo)");
>     return 0;
> }
> ```

### Overload selection

Selection is on **exact** operand types — there is no numeric coercion for
operators, unlike ordinary function overload resolution (which does permit
`int` → `double` widening). A declared `operator +(a: Vec2, b: double)`
does **not** match a call site `a + 5` where `5` is an `int` literal.

Multiple overloads of the same symbol differing by operand type are
supported and selected correctly:

```turflang
operator +(a: Vec2, b: Vec2): Vec2 {
    mut r: Vec2 = Vec2::default();
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    return r;
}

operator +(a: Vec2, b: int): Vec2 {
    mut r: Vec2 = Vec2::default();
    r.x = a.x + b;
    r.y = a.y + b;
    return r;
}

fn main(): int {
    mut a: Vec2 = Vec2::default();
    a.x = 1; a.y = 2;
    mut b: Vec2 = Vec2::default();
    b.x = 10; b.y = 20;

    mut sum1: Vec2 = a + b;   // Vec2 + Vec2 overload
    mut sum2: Vec2 = a + 5;   // Vec2 + int overload
    return 0;
}
```

### Visibility

An operator overload is visible anywhere its defining module is imported
in the compile — the same visibility rule that applies to qualified
`Module::func()` calls. This makes sense given operators have no qualifier
syntax at all (`a + b`, never `module::+(a, b)`) — there's no way to write
a "qualified" operator call even if you wanted to.

Note that the struct or enum **type** itself still needs its own separate
on-demand import to be usable bare — that's a pre-existing, unrelated
visibility gate independent of the operator overload's own visibility.

Using an operator on a type with no matching overload declared anywhere is
a clean type error, not an internal compiler error.

### Compound assignment

`a += b` desugars to `a = a + b`, so it uses `operator +` and the result type
must be exactly the type of `a`. `x++` / `x--` use `operator +` / `operator -`
with a right operand of `int`. Compound assignment and `++`/`--` **through an
indexing operator** (`m[k] += 1`, `l[i]++`) are not supported; write
`m[k] = m[k] + 1`.

### Generic operators

An operator may declare type parameters right after the keyword, exactly where
`fn<T>` puts them. The standard collections index this way:

```turflang
// lib/std/collections/list.tr
operator<T> [](l: List<T>, i: int): T { ... }
operator<T> []=(l: List<T>, i: int, value: T): void { ... }

// lib/std/collections/map.tr
operator<K, V> [](m: Map<K, V>, key: K): V { ... }
```

so `l[i]`, `l[i] = v`, `m[k]` and `s[i]` work for **every** `List<T>`,
`Map<K, V>` and `Set<T>`, including a struct element of any size and nested
containers (`m["k"][1]` on a `Map<string, List<int>>`).

A user generic struct gets the same, in any operator form:

```turflang
struct Cell<T> { v: T, n: int }

operator<T> +(a: Cell<T>, k: int): Cell<T> { ... }   // Cell<int> + int, Cell<string> + int, ...
operator<T> <(a: Cell<T>, b: Cell<T>): bool { ... }  // `<` right after `operator<T>` is the operator
```

How a call is resolved:

1. The type parameters are **inferred from the operands** (there is no way to
   write them at a use).
2. Each declared operand type is then substituted with that binding and must
   match its operand **exactly** — the same rule as a concrete operator. A
   generic operator adds inference and nothing else: `Cell<string> + 1.5`
   still fails against `operator<T> +(a: Cell<T>, k: int)`, and
   `Cell<int> == Cell<string>` fails against `operator<T> ==(a: Cell<T>, b:
   Cell<T>)` because `T` is `int` from the left operand.
3. One copy of the operator is compiled per distinct set of type arguments,
   like any generic function.

Two rules apply to the declaration (both error **E097**):

- Every type parameter must appear in some operand. One that appears only in
  the return type (`operator<T> [](p: Cell<int>, i: int): T`) could never be
  inferred.
- At least one operand must be a struct or enum **declared in the same
  module** — looked for through `&`/`*` and through a generic type's base, so
  `List<T>` counts for the module that declares `List`. `operator<T> +(a: T,
  b: T): T` would match every type, including types from modules that never
  imported yours, so it is rejected; declared in another module's type it is the
  ordinary orphan rule (**E080**).

### Gotcha: `x[i]` is a value, not a place (E098)

An index operator returns the element **by value**. Reading through it works —
`l[i].field`, `l[i].method()`, `outer[a][b]`. Writing into *part of* the
returned value would only change a copy, so each of these is a compile error
(**E098**) instead of a silently lost write:

```turflang
bs[0].a = 50;          // assign to a field of the copy
bs[0].a += 1;          // ... or update it
bs[0].a++;
bs[0].bump();          // a method that assigns to self
outer[0].push(5);      // push bumps `len` in the copy of the inner list's header
mm["a"]["b"] = 1;      // a Map's []= mutates the Map header, which is the copy
```

This is the rule C# calls CS1612 and Go applies to a struct stored in a map.
What is *not* rejected, because the write reaches real storage:

```turflang
mut e: Big = bs[0];  e.a = 50;  bs[0] = e;   // read, change, store back (the fix)
grid[i][j] = v;      // List's []= takes the header by value but writes through
                     // the shared buffer, so the row you wrote to is the real row
nodes[0].v = 10;     // an element that is a `&T`/`T*`: the write goes through it
```

The test is whether the destination is a *field or `self` of the returned
copy*: `grid[i][j] = v` stores through a pointer inside the copy, `outer[0].push(v)`
assigns `len` inside it. "Changes its receiver" means the method assigns to
`self.<field>` itself, or calls (through any chain of `self.m()` calls) a method
that does — the same answer the immutable-variable check uses, so
`bs[0].wrapper()` is rejected when `wrapper` only calls `self.bump()`. Two
limits: the check is conservative about overloads (a call counts only if every
same-name, same-arity method it could mean mutates), and an assignment counts
only if it lands in the struct's own value storage — `self.n = …`, `self.arr[i]
= …`, `self.inner.f = …`, including inside `if`, `compare`, `defer` and loops.
A write through a pointer or reference field does not (it changes the pointee),
which is why `list.set(i, v)` stays callable on a copy. One case is not seen: a
mutating method called on a value field (`self.inner.bump()`).
