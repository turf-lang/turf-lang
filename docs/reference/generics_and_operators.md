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
`.`- or `::`-qualified, never generic, never `static`:

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

Only ten binary symbols can be overloaded:

```
+  -  *  /  ==  !=  <  >  <=  >=
```

That's it. Deliberately **not** overloadable:

- `%`, `**`
- Bitwise operators: `& | ^ << >>`
- Logical operators: `&& ||`
- Any assignment form, including compound assignment (`+=`, etc.)

Declaring an operator outside the whitelist (e.g. `operator &(...)`) is a
parse-time error.

> [!NOTE]
> There is **no indexing-operator overloading** (`operator [](...)` /
> `operator []=(...)`) anywhere in the language. If you need
> container-style indexing behavior on your own type, expose it as a named
> method instead.
>
> There is also **no unary operator overloading**. A one-parameter
> `operator` declaration (e.g. `operator -(a: Vec2): Vec2` for negation)
> is explicitly rejected with *"Unary operator overloading is not
> supported yet."* Every `operator` declaration must take exactly two
> parameters.

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

### Gotcha: compound assignment isn't routed through operators

`s += t` is **not** routed through `operator +` resolution. This is a
known, deliberate gap — using compound assignment on a type that only has
an `operator +` overload (and no native `+=` support) is a clean compile
error today, not a crash, but it is also not silently rewritten to
`s = s + t` for you.
