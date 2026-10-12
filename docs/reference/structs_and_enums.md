# Structs and Enums

## Structs

### Declaring a struct

```turflang
struct User {
  age: int,
  name: string
}
```

A struct is a name followed by a brace-enclosed, comma-separated list of
`field: type` pairs. There is no trailing-comma requirement in either
direction.

- `void` cannot be used as a field's type.
- Two fields with the same name in one struct is a parse-time error.
- A struct with the same name declared twice (even if both declarations are
  empty) is rejected.

Empty structs are fully supported:

```turflang
struct MathUtil {}
```

This is a common pattern for a "namespace" struct that exists only to hold
[static methods](#static-methods) — see below.

### Generic structs

A struct can declare type parameters in `< >` after its name:

```turflang
struct Box<T> {
    val: T*
}
```

See [Generics and Operators](generics_and_operators.md) for the full
generics model, including why generic fields are usually declared as
pointers (`T*`) rather than by value.

### Self-referential structs

A field may hold a reference to the struct's own type — this is how you
build linked structures. Use `&?T`, the **nullable safe reference**: no
`unsafe { }` needed anywhere, for the field, the struct declaration, or any
function that takes or returns one.

```turflang
struct Node {
    val: int,
    next: &?Node
}
```

This works with no special forward-declaration step; a struct name may be
referenced by its own fields (or by any struct declared earlier or later in
the file) freely.

`null` is assignable to `&?Node` (it's the only safe reference type that
accepts it), and a plain `&Node` widens into it implicitly:

```turflang
fn Node.new(val: int): void {
    self.val = val;
    self.next = null;   // plain field write, no unsafe { } needed
}
```

**Reading a `&?T` field requires a null check first** — the compiler tracks
whether you've proven it non-null on the path to each use:

```turflang
fn sum(head: &?Node): int {
    mut total: int = 0;
    mut current: &?Node = head;
    while (current != null) {          // narrows `current` to non-null...
        total = total + current.val;   // ...so this compiles
        current = current.next;
    }
    return total;
}
```

A method call (`current.grow()`) is a use like a field read and needs the
same check. The check works on a **variable** only: a `&?Node` that is a
field, a call result or a list element (`current.next.val`) has to be stored
in a variable and checked there (`mut n: &?Node = current.next; if (n != null)
{ ... }`).

Accessing `current.val` with no preceding `current != null` (or
`current == null` with an early return, or a `&&` chain, or any other
direct comparison) is a compile error — see
`docs/architecture/pointers_and_references.md` §1.8 for the full narrowing
rules, and `tests/39_nullable_references/` for a complete linked-list and
binary-tree example.

> [!NOTE]
> A raw `T*` field (rather than `&?T`) is still legal, but requires the
> entire `struct` declaration to be wrapped in `unsafe { }`, and — unlike
> `&?T` — no function may ever take a raw `T*` parameter, even inside
> `unsafe { }`. There is no longer a good reason to reach for `T*` here:
> `&?T` is the safe, checked, ergonomic way to write a self-referential
> struct. See `docs/architecture/pointers_and_references.md` §1.2 for when
> `T*`/`ptr` are still the right choice (FFI bindings, mostly).

### Construction: `::new()` and `::default()`

There is exactly one way to construct a struct value with custom logic —
declaring a `.new` instance method — and one way to get a zero-initialized
value for free.

**`StructName::new(args...)`** is not special syntax on its own; it is an
ordinary instance method named `new`, declared with the same `.` syntax as
any other method, and `self` is auto-injected exactly like any other
instance method. The only thing special about it is that it can **only** be
invoked through `::`, never through `.`:

```turflang
struct User {
  age: int,
  name: string
}

fn User.new(age: int, name: string): void {
  self.age = age;
  self.name = name;
}

fn main(): int {
  mut u: User = User::new(10, "Alice");
}
```

Constructor parameters are positional to the constructor's **own**
signature — they are not matched against field names or field declaration
order:

```turflang
struct Point {
    x: int,
    y: int
}

// Params intentionally NOT in field-declaration order.
fn Point.new(y: int, x: int): void {
    self.x = x;
    self.y = y;
}

fn main(): int {
    mut p: Point = Point::new(20, 10); // p.x == 10, p.y == 20
}
```

A constructor may also validate or transform its inputs rather than just
copy them straight into fields — it's an ordinary function body.

**`StructName::default()`** is always available for every struct, is
compiler-synthesized, takes no arguments, and recursively defaults every
field (including nested struct fields and array elements): numbers are 0,
strings `""`, a `&?T` null. It can never be manually declared, and it never
takes arguments — both are compile errors.

```turflang
mut u2: User = User::default(); // age == 0, name == ""
```

A `&T` field cannot be null, so its default is a new `T::default()` on the
heap, one per field (FREEZE F-COLL-2). Two copies of the struct then point at
the same `T`, which is how a collection field is shared:

```turflang
struct Counter { n: int }
struct Holder { c: &Counter }

fn main(): int {
    mut a: Holder = Holder::default();   // a.c points at a fresh Counter
    mut b: Holder = a;
    b.c.n = 5;
    return a.c.n;                        // 5: one Counter
}
```

A type that leads back to itself through `&` fields (`struct Node { next:
&Node }`) has no default, since each value would need another one: that is
**E147**. Make a field on the cycle `&?Node`, or construct with `::new(...)`.

If a struct declares no `.new` method, `::new(...)` is simply not available
for it — `::default()` remains the only construction path.

> [!NOTE]
> There is no other construction syntax. The legacy brace-literal form
> (`User { age: 1, name: "x" }`) has been fully removed from the language —
> attempting to use it is a hard parse error telling you to use `::new(...)`
> or `::default()` instead.

Calling `new` or `default` through `.` on an existing instance
(`u.new(...)`) is rejected — construction is always via `::`. `new` can
never be declared `static` either, since it always constructs through
`self`.

#### Constructors must assign every field on every path

The compiler performs a real control-flow-sensitive check on every `.new`
method: every declared field must be assigned on **every** path from entry
to exit, not merely "somewhere in the body." Both branches of an `if`/`else`
must cover every field:

```turflang
struct Point {
    x: int,
    y: int
}

// Every field assigned in BOTH branches -- compiles.
fn Point.new(x: int, y: int, negate: bool): void {
    if (negate) {
        self.x = 0 - x;
        self.y = 0 - y;
    } else {
        self.x = x;
        self.y = y;
    }
}
```

A field assigned unconditionally before a loop is considered covered even
if the loop body only reassigns it conditionally:

```turflang
struct Choice {
    picked: int
}

fn Choice.new(n: int): void {
    self.picked = 0;        // covers the field unconditionally
    mut i: int = 0;
    while (i < n) {
        self.picked = i;    // conditional re-assignment is fine
        i = i + 1;
    }
}
```

A constructor that misses a field on some path (e.g. only assigns it inside
one branch of an `if` with no `else`) is a compile error.

### Instance methods and `self`

Instance methods are declared with `.`:

```turflang
fn User.grow(): void { self.age = self.age + 1; }
fn User.rename(new_name: string): void { self.name = new_name; }
```

`self` is automatically injected as the first parameter whenever a method
is declared with `.` and has no trailing `static`. `self` is a pointer to
the instance, so mutating `self.field` inside a method is visible to the
caller after the call returns — it is not a by-value copy.

```turflang
fn main(): int {
    mut u: User = User::new(10, "Alice");
    u.grow();
    // u.age is now 11
    u.rename("Bob");
}
```

### Static methods

A **static** method has no `self` at all. It is declared with the same `.`
syntax as an instance method, plus a trailing `static` keyword placed right
after the parameter list, before the return type:

```turflang
struct MathUtil {}

fn MathUtil.square(x: int) static: int {
    return x * x;
}

fn main(): int {
    test::assert(MathUtil::square(5) == 25, "");
}
```

A static method is called **only** as `StructName::method(args)`.

> [!NOTE]
> `::` is exclusively a call/reference form now, never a declaration form.
> The older `fn StructName::method(...)` declaration syntax has been
> removed entirely and produces a dedicated parser error pointing you at
> the `fn StructName.method(...) static: T` form shown above.

Rules worth knowing:

- Calling a static method through `.` on an instance (`u.square(5)`) is
  rejected — it is treated as an arity mismatch, since `.` always implies
  an implicit `self` argument that a static method's signature doesn't
  have.
- Calling a static method through `.` on the bare struct type name
  (`MathUtil.square(5)`, no instance) is also rejected — only `::` works
  for static calls.
- Using `::` on an **instance variable** rather than a type name
  (`p::square(5)`) is rejected with a "you meant `.`" style error.
- `static` outside of a struct-bound function declaration is meaningless
  and rejected.
- Static methods are reachable across module boundaries, both fully
  qualified (`Module::StructName::method()`) and bare after an on-demand
  import of the defining module.

An empty "namespace" struct combining static and instance methods is a
common and fully supported pattern:

```turflang
struct MathUtil {}

fn MathUtil.square(x: int) static: int {
    return x * x;
}

fn MathUtil.describe(): string {
    return "a MathUtil instance";
}

fn main(): int {
    test::assert(MathUtil::square(5) == 25, "static method call works");
    mut mu: MathUtil = MathUtil::default();
    // mu.describe() works too -- an ordinary instance method
}
```

### Struct equality

There is no built-in `==`/`!=` for struct values. Comparing two struct
instances with no `operator==` overload declared for that type is a clean
compile error. To opt in, declare `operator ==(a: T, b: T): bool` (and
`operator !=` if needed) for the struct — see
[Generics and Operators](generics_and_operators.md#operator-overloading).

---

## Enums

### Declaration and access

```turflang
enum Direction {
    North,
    East,
    South,
    West
}

immut d: Direction = Direction::North;
```

A body is a comma-separated list of bare variant names. A variant's
ordinal is its 0-based declaration order (`North` = 0, `East` = 1, ...).
Variants are accessed with `::`. Declaring the same variant name twice in
one enum is a parse-time error.

### Built-in pseudo-properties and casts

Every enum type gets these for free — no declarations needed:

| Form | Meaning |
|---|---|
| `(v to int)` | The variant's ordinal. |
| `(v to string)` | The variant's declared name, as a string. |
| `("Name" to EnumType)` | Parses a name back into the matching variant. |
| `EnumType::COUNT` | Number of declared variants. |
| `EnumType::VALUES` | A `T*` array of every variant value, in declaration order. |
| `EnumType::NAMES` | A `string*` array of every variant's name, in declaration order. |

```turflang
enum Direction { North, East, South, West }

fn main(): int {
    immut d: Direction = Direction::North;
    test::assert((d to int) == 0, "North should have value 0");
    test::assert((d to string) == "North", "North to string");

    immut parsed: Direction = "South" to Direction;
    test::assert((parsed to int) == 2, "");

    test::assert(Direction::COUNT == 4, "");
    return 0;
}
```

`VALUES` and `NAMES` are raw-pointer-typed (`T*`/`string*`), so reading them
requires `unsafe { }`:

```turflang
unsafe {
    immut vals: Direction* = Direction::VALUES;
    immut names: string* = Direction::NAMES;

    mut i: int = 0;
    while i < Direction::COUNT {
        immut d: Direction = vals[i];
        immut n: string = names[i];
        i = i + 1;
    }
}
```

Casting an out-of-range compile-time constant integer literal to an enum
type (e.g. `(99 to Direction)` for a 4-variant enum) is a compile error. A
cast from a value known only at run time is checked when it runs: outside
`0 <= v < number of variants` it panics with `Invalid int to enum
conversion`, so every enum value is one of its variants and an exhaustive
`compare` stays exhaustive.

### Constraints

- Assigning a variant of one enum type to a variable declared as a
  *different* enum type is a type error.
- Ordering operators (`<`, `>`, `<=`, `>=`) are not defined for enum values
  — cast to `int` first if you need to compare ordinals.
- `==`/`!=` on two values of the **same** enum type work natively, with no
  operator overload required — unlike structs (see above).

### Exhaustiveness in `compare`/`matches`

A `compare` over an enum subject must either cover every variant, or
include an `otherwise:` arm. A non-exhaustive `compare` with no
`otherwise` is a compile error.

```turflang
enum Season { Spring, Summer, Autumn, Winter }

fn season_name(s: Season): string {
    mut name: string = "";
    compare s {
        matches Season::Spring:
            name = "spring";
        done
        matches Season::Summer:
            name = "summer";
        done
        matches Season::Autumn:
            name = "cold";
        done
        matches Season::Winter:
            name = "cold";
        done
    }
    return name;
}

// Equivalently, with otherwise covering the rest:
fn season_name_with_otherwise(s: Season): string {
    mut name: string = "";
    compare s {
        matches Season::Spring:
            name = "spring";
        done
        matches Season::Summer:
            name = "summer";
        done
        otherwise:
            name = "cold";
    }
    return name;
}
```

Declaring the same pattern (the same variant, or the same integer literal)
in two different `matches` arms of the same `compare` is rejected as a
duplicate.

> [!NOTE]
> Enums are also the foundation for Turflang's [state machine](state_machines.md)
> feature (`machine`), which extends enum-like variants with a
> compiler-checked transition graph. Everything on this page — `::`-access,
> `==`/`!=`, `COUNT`, and `compare`/`matches` exhaustiveness — applies
> identically to `machine` types, since a `machine` is implemented as a
> subtype of an enum.
