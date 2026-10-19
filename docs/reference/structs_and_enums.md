# Structs and Enums

A **struct** groups named fields into one value and can have methods. An
**enum** is a closed set of named variants. This page covers both; state
machines, which are enums with a checked transition graph, have
[their own page](state_machines.md).

## Structs

### Declaring a struct

```turflang
struct User {
    age: int,
    name: string,
}

fn main(): int {
    mut u: User = User::default();
    u.age = 30;
    u.name = "Alice";
    return u.age;
}
```

A struct is a name followed by a brace-enclosed, comma-separated list of
`field: type` pairs. A trailing comma is allowed and not required.

- A field cannot be `void`. That is `E112`: `'void' cannot be a struct field's type`.
- Two fields with the same name are `E114`: `Struct 'A' already has a field named 'x'`.
- A struct name can be declared only once **per file** (`E034`: `A struct named 'A' is already declared`).
  Another module may declare a struct with the same name; they are different
  types. See [Types belong to a module](#types-belong-to-a-module).

An empty struct is legal. It is useful as a namespace for
[static methods](#static-methods):

```turflang
struct MathUtil {}

fn MathUtil.square(x: int) static: int {
    return x * x;
}

fn main(): int {
    return MathUtil::square(5);
}
```

### Fields and `pub`

A field is read and written with `.`; fields of fields chain
(`line.start.x`). Reading or writing a field that does not exist is `E068`
(`Struct 'P' has no field named 'nope'`), and assigning a value of the wrong
type is `E066`.

A struct declared in an imported module has private fields unless each field is marked `pub`, even if the struct itself is `pub`:

```turflang
pub struct Account {
    pub owner: string,
    balance: int,
}

fn main(): int {
    mut a: Account = Account::default();
    a.owner = "Mira";
    a.balance = 10;
    return a.balance;
}
```

Inside the declaring file nothing needs `pub`. From another module,
`account.balance` is `E088` (`Field 'Account.balance' is private to module
'account'`) while `account.owner` is fine. `Account::default()` is always
allowed, whatever the field visibility: it fills every field with its zero
value, which cannot break an invariant that zero does not. Methods, types and
the rest of the visibility rules are in
[Visibility: `pub`](functions_and_modules.md#8-visibility-pub).

### Structs are values

A struct variable holds its fields directly. Assigning a struct, passing it
to a function, or returning it **copies** it, fields and nested structs and
fixed-size arrays included:

```turflang
struct Cell { v: int }
struct Grid {
    cells: Cell[4],
    origin: Cell,
}

fn reset(g: Grid): void {
    g.origin.v = 0;       // changes the callee's copy only
}

fn main(): int {
    mut g: Grid = Grid::default();
    g.cells[2].v = 9;
    g.origin.v = 1;

    mut h: Grid = g;      // a full copy
    h.cells[2].v = 5;
    reset(g);
    return g.cells[2].v + g.origin.v;   // 10: neither change reached g
}
```

To let a function change the caller's struct, take a reference, `&T`, and
pass `&value` (see [Parameters: by value and by
reference](functions_and_modules.md#parameters-by-value-and-by-reference)):

```turflang
struct Counter { n: int }

fn Counter.bump(): void { self.n = self.n + 1; }

fn twice(c: &Counter): void {
    c.bump();
    c.bump();
}

fn main(): int {
    mut c: Counter = Counter::default();
    twice(&c);
    return c.n;   // 2
}
```

An `immut` struct variable cannot be changed: assigning to a field, or
calling a method that assigns to one of its own fields, is `E060` (`Oops! You
can't change the value of 'c' because it's immutable`).

### Constructing a struct

There are exactly two ways to make a struct value, and both use `::`.

**`T::default()`** exists for every struct. It takes no arguments and gives
every field its zero value: numbers are 0, `bool` is `false`, `string` is
`""`, a `&?T` or raw pointer is `null`, an enum is its first variant, and
nested structs and array elements are defaulted recursively. It cannot be
declared by hand (`E051`: `'default' is reserved and can't be declared by
hand`) and takes no arguments (`E047`).

**`T::new(args...)`** is a constructor you write. It is an ordinary
instance method called `new`; `self` is the value being built, and the call
`T::new(...)` evaluates to that value:

```turflang
struct Point {
    x: int,
    y: int,
}

// The parameters need not follow the field order.
fn Point.new(y: int, x: int): void {
    self.x = x;
    self.y = y;
}

fn main(): int {
    mut p: Point = Point::new(20, 10);   // p.x == 10, p.y == 20
    return p.x;
}
```

A struct with no `.new` method has no `::new(...)`: calling it is `E050`
(`'Point' has no constructor -- 'Point::new(...)' can't be called`). A
constructor can validate or transform its inputs like any function.

These rules apply to constructors:

- `new` is reached only through `::`. `p.new(...)` on an existing value is
  `E045` (`'new' can't be called on an instance with '.'`), and so is
  `p.default()`.
- `new` cannot be `static` (`E046`).
- A call with the wrong number of arguments is `E044` (`'Point::new' was called with 1 argument, but no overload accepts that many`).
- Every field must be assigned on **every** path through `new`. Missing one
  on any path is `E049` (`'Point.new' doesn't assign every field on all
  paths`, with `Not definitely assigned on every path: y`).

```turflang
struct Point {
    x: int,
    y: int,
}

// Both branches assign both fields, so this compiles.
fn Point.new(x: int, y: int, negate: bool): void {
    if (negate) {
        self.x = 0 - x;
        self.y = 0 - y;
    } else {
        self.x = x;
        self.y = y;
    }
}

fn main(): int {
    mut p: Point = Point::new(3, 4, true);
    return p.x;
}
```

A field assigned unconditionally before a loop counts as covered, even if
the loop only reassigns it conditionally.

There is no brace-literal form. `Point { x: 1, y: 2 }` is `E113`
(`'Point { ... }' is no longer valid syntax for constructing a struct`);
write `Point::new(...)`, or `Point::default()` and assign the fields.

A `&T` field cannot be null, so its default is a fresh `T::default()` on the
heap, one per field. Two copies of the struct then point at the same `T`:

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

A type that reaches itself through `&` fields has no default, since each
value would need another one inside it. That is `E147`; see
[Self-referential structs](#self-referential-structs).

### Instance methods and `self`

A method is a function declared with `fn Type.name(...)`. Inside it, `self`
is the receiver, passed by reference: assigning `self.field` changes the
caller's value.

```turflang
struct User {
    age: int,
    name: string,
}

fn User.grow(): void { self.age = self.age + 1; }
fn User.rename(new_name: string): void { self.name = new_name; }

fn main(): int {
    mut u: User = User::default();
    u.grow();
    u.rename("Bob");
    return u.age;   // 1
}
```

A method may be declared before or after its struct in the file. Calling a
method that does not exist is `E054` (`'User' has no method named 'nope'`).
A function that is not a method of the struct can also be called as `u.f()`
when its first parameter is a `User`; see
[UFCS](functions_and_modules.md#3-ufcs-calling-a-function-as-a-method).

### Static methods

A **static** method has no `self`. It is declared like an instance method,
with `static` after the parameter list and before the return type, and is
called with `::`:

```turflang
struct Temperature {}

fn Temperature.to_fahrenheit(c: double) static: double {
    return c * 9.0 / 5.0 + 32.0;
}

fn main(): int {
    immut f: double = Temperature::to_fahrenheit(100.0);
    return f to int;   // 212
}
```

The ways to get this wrong:

- `u.to_fahrenheit(1.0)` through an instance is `E052` (`'to_fahrenheit' is a
  static method -- it can't be called through an instance with '.'`).
- `Temperature.to_fahrenheit(1.0)` on the type name is `E048` (`'.' can't be
  used here -- 'Temperature' is a struct type, not an instance`).
- `t::to_fahrenheit(1.0)` with `::` on a variable is `E029` (`'t' is a
  variable, not a type or module -- '::' doesn't apply to it`).
- `static` on a function that is not a method is `E053` (`'static' can only
  be used on a struct-bound method`).
- The old declaration form `fn Temperature::to_fahrenheit(...)` is `E113`;
  `::` is only for calling.

Static methods are reachable from other modules both fully qualified
(`module::Type::method()`) and, after an on-demand import, bare
(`Type::method()`).

### Methods on generic structs

A struct can take type parameters:

```turflang
struct Box<T> {
    val: T,
}
```

Every method on a generic struct repeats the struct's type parameters
after `fn`. Omitting them is `E138` (`Missing type parameters!`: declare it
as `fn<T> Box.get(...)`):

```turflang
struct Box<T> {
    val: T,
}

fn<T> Box.get(): T { return self.val; }
fn<T> Box.put(v: T): void { self.val = v; }

// A static method repeats them too when it mentions `T`.
fn<T> Box.of(v: T) static: Box<T> {
    mut b: Box<T> = Box::default();
    b.val = v;
    return b;
}

// A static method that does not mention `T` needs none.
fn Box.empty_label() static: string { return "box"; }

fn main(): int {
    mut a: Box<int> = Box::of(41);
    a.put(a.get() + 1);
    mut s: Box<string> = Box::default();
    s.put("hi");
    return a.get();   // 42
}
```

The same rule holds for a method you declare on a generic struct imported
from a module (`fn<T> List.last_or(...)`). The type arguments come from the
declared type of the variable: write `mut b: Box<int> = Box::default();`.
Naming a generic struct with no arguments, `mut b: Box = ...`, is `E138`
(`Missing type arguments!`); giving a type the wrong number of them,
`Box<int, int>`, or any at all to a struct that declares none, is `E148`
(`Wrong number of type arguments!`). Generics are covered in full in [Generics and
operator overloading](generics_and_operators.md#generic-structs-and-their-methods).

### Local type declarations

`struct`, `enum`, `machine` and `error` types may be declared inside a
function body. They are visible from that point to the end of the
function:

```turflang
fn classify(n: int): int {
    struct Span { lo: int, hi: int }
    enum Size { Small, Large }

    mut s: Span = Span::default();
    s.lo = 0;
    s.hi = n;
    immut size: Size = Size::Large;
    return s.hi + (size to int);
}

fn main(): int {
    return classify(4);   // 5
}
```

A local type has no methods (functions cannot nest), so it is built with
`::default()` and field assignments. Type names are still unique per
file:

- Two functions cannot each declare a local `P`: the second is `E034`.
- A local `P` cannot reuse the name of a top-level `P` in the same file: `E034`.
- A local struct in a generic function cannot use that function's type
  parameter; `T` is `E042` (`Unknown type 'T'`) there.
- A local struct cannot have type parameters of its own (`struct Box<T>`
  inside a function is `E151`); declare a generic struct at the top level.

<!-- doc-example: fails E034 -->
```turflang
struct P { x: int }

fn f(): int {
    struct P { y: int }     // E034: 'P' is already declared in this file
    mut p: P = P::default();
    return p.y;
}

fn main(): int { return f(); }
```

### Layout cycles: a struct cannot contain itself by value

A struct's size must be computable from its declaration. A field that holds
another struct **by value** (or an array of one) puts that whole struct
inside this one, so a chain of by-value fields that comes back to the start
would need infinite space. The compiler rejects it with `E069`
(`struct 'A' cannot contain itself by value`) and names the whole cycle:

<!-- doc-example: fails E069 -->
```turflang
struct A { b: B }
struct B { a: A }       // E069  cycle: A -> B -> A

fn main(): int { return 0; }
```

```text
error [E069]: struct 'A' cannot contain itself by value.
  cycle: A -> B -> A
  Note: field 'A::b' has type 'B'
  Note: field 'B::a' has type 'A'
```

The direct form (`struct Node { me: Node }`) and the array form
(`struct B { items: B[2] }`, two copies of `B` inside `B`) are the same
mistake. A generic instantiation can close a cycle too, even though neither
declaration looks circular on its own:

<!-- doc-example: fails E069 -->
```turflang
struct A<T> { val: T }
struct B { back: A<B> }     // A<B> holds a B by value, and B holds that A<B>

fn main(): int { return 0; }
```

A generic struct that mentions itself by value in its own declaration
(`struct Box<T> { inner: Box<T> }`) is rejected for any `T`.

**To break a cycle, make at least one field on it a reference or pointer.**
A reference is a fixed size no matter what it points to. Use `&?T` where the
link can be absent, `&T` where it never is, and a raw `T*` only inside
`unsafe` (see below):

```turflang
struct A { b: &?B }        // A refers to a B, or to nothing
struct B { a: A }          // B holds an A by value

struct Wrapper<T> { val: T }
struct Holder { back: &?Wrapper<Holder> }

fn main(): int {
    mut a: A = A::default();
    mut h: Holder = Holder::default();
    return 0;
}
```

### Self-referential structs

Linked structures (lists, trees, graphs) are built from fields that refer
to the struct's own type. A struct may name itself, or any struct declared
earlier or later in the file, in its fields; no forward declaration is
needed. The field must be a reference or pointer, not a by-value field
(see above).

#### With `&?T`

`&?T` is the **nullable safe reference**. It needs no `unsafe` anywhere,
and `null` is assignable to it (a plain `&T` also widens into it):

```turflang
struct Node {
    val: int,
    next: &?Node,
}

fn Node.new(val: int): void {
    self.val = val;
    self.next = null;
}

fn sum(head: &?Node): int {
    mut total: int = 0;
    mut current: &?Node = head;
    while (current != null) {          // narrows `current` to non-null...
        total = total + current.val;   // ...so this compiles
        current = current.next;
    }
    return total;
}

fn main(): int {
    mut a: Node = Node::new(1);
    mut b: Node = Node::new(2);
    a.next = &b;
    return sum(&a);   // 3
}
```

**Reading through a `&?T` requires a null check first.** A field read or
a method call on it without a proven check is `E081` (`Possible null
dereference: this '&?Node' value might be 'null'`). The check works on a
**variable** only: a `&?Node` that is a field, a call result or a list
element (`a.next.val`) has to be stored in a variable and checked there:

```turflang
struct Node {
    val: int,
    next: &?Node,
}

fn second_value(a: &Node): int {
    mut n: &?Node = a.next;
    if (n != null) {
        return n.val;
    }
    return -1;
}

fn main(): int {
    mut a: Node = Node::default();
    mut b: Node = Node::default();
    b.val = 7;
    a.next = &b;
    return second_value(&a);   // 7
}
```

`if (x == null) { return ...; }`, a `&&` chain and `while (x != null)` all
count as checks. The narrowing rules are in
[pointers and references](../architecture/pointers_and_references.md).

A binary tree uses two links, and a method may recurse through them:

```turflang
struct Tree {
    v: int,
    left: &?Tree,
    right: &?Tree,
}

fn Tree.new(v: int): void {
    self.v = v;
    self.left = null;
    self.right = null;
}

fn Tree.insert(v: int): void {
    if (v < self.v) {
        if (self.left == null) {
            self.left = heap(Tree::new(v));
        } else {
            mut l: &?Tree = self.left;
            if (l != null) { l.insert(v); }
        }
    } else {
        if (self.right == null) {
            self.right = heap(Tree::new(v));
        } else {
            mut r: &?Tree = self.right;
            if (r != null) { r.insert(v); }
        }
    }
}

fn size(t: &?Tree): int {
    if (t == null) { return 0; }
    return 1 + size(t.left) + size(t.right);
}

fn main(): int {
    mut root: Tree = Tree::new(5);
    root.insert(3);
    root.insert(8);
    root.insert(4);
    return size(&root);   // 4
}
```

`heap(expr)` allocates the value on the garbage-collected heap and returns
a `&T`, so the node outlives the call that made it.

#### With `&T`

A non-nullable `&T` link is legal, but a type that reaches itself through
`&` fields has no `::default()`, because defaulting would have to build
another one inside it forever. `Node::default()` on `struct Node { v: int,
next: &Node }` is `E147` (`'Node' has no default value: ... it reaches itself
through '&' fields`). Make a field on the cycle `&?Node`, or build the value
with `::new(...)`.

#### With raw pointers

A raw `T*` field is also legal, but a struct that has one must be declared
inside `unsafe { }`, and so must every use of the field (see [FFI, `unsafe
{}`, and Pointers](ffi_and_unsafe.md#4-raw-pointers-ptr-and-t)). Without
the `unsafe`, the declaration is `E130` (`Raw pointer type 'Node*' cannot be
used outside an 'unsafe' block`). Even inside `unsafe`, no ordinary function
may take or return a raw pointer in its signature (`E130`, `Raw pointer in
function signature!`):

```turflang
unsafe {
    struct RawNode {
        val: int,
        next: RawNode*,
    }
}

fn main(): int {
    mut a: RawNode = RawNode::default();
    mut b: RawNode = RawNode::default();
    a.val = 1;
    b.val = 2;
    unsafe {
        a.next = &b;
        b.next = null;
        return a.next.val;   // 2
    }
}
```

There is rarely a reason to prefer `T*` for this: `&?T` is checked and needs
no `unsafe`. Raw pointers are for FFI bindings.

### Types belong to a module

A struct, enum, machine or `error` type is identified by the module that
declares it and its name, not by its name alone. Two modules may each
declare `Node`, and your program may declare its own `List` or `Parser`
whatever it imports; they are separate types, written `a::Node` and
`b::Node`. Bare `Node` means your own file's type or one you imported by
name. If a bare name could mean both, the use is `E085` (`'Node' is
ambiguous here -- it could mean more than one type`) naming both; qualify the
imported one and leave it out of the import. `typeof(x)` prints
`shapes::Node` for a module's type and the bare name for one in the main
file.

<!-- doc-example: skip needs a second file, shapes.tr -->
```turflang
// shapes.tr declares `pub struct Node { pub id: int }`.
import shapes;

struct Node { n: int }          // this file's own Node

fn main(): int {
    mut mine: Node = Node::default();
    mut theirs: shapes::Node = shapes::Node::default();
    mine.n = 1;
    theirs.id = 2;
    return mine.n + theirs.id;
}
```

The rules for imports and visibility are in [Types belong to their
module](functions_and_modules.md#9-types-belong-to-their-module).

### Struct equality

`==` and `!=` are not defined for struct values. Comparing two structs
with no `operator ==` for that type is `E124` (`Values can't be compared!
Cannot compare struct values with '=='`). Compare the fields you care
about, or declare `operator ==(a: T, b: T): bool` for the struct; see
[Operator overloading](generics_and_operators.md#operator-overloading).

---

## Enums

An enum is a closed set of named variants. A plain enum carries no data;
a variant that holds values belongs to an `error` type (see
[Payloads](error_handling.md#payloads-data-on-a-variant)).

### Declaration and access

```turflang
enum Direction {
    North,
    East,
    South,
    West,
}

fn main(): int {
    immut d: Direction = Direction::East;
    return d to int;   // 1
}
```

The body is a comma-separated list of bare variant names. A variant's
ordinal is its 0-based declaration order. Variants are reached with `::`.
There is no explicit value syntax (`A = 5`) and no payload syntax
(`A(int)`); both are `E105` parse errors.

- A repeated variant name is `E114` (`Enum 'E' already has a variant named 'A'`).
- A name that is not a variant is `E039` (`Enum 'E' has no variant 'C'`).
- `E::default()` does not exist (`E135`: an enum variant is not callable);
  name the variant you want. A **struct field** of enum type defaults to
  the first variant.
- Assigning a variant of another enum is `E066`.
- `==` and `!=` work on two values of the same enum type with no overload.
  Ordering operators (`<`, `>`, `<=`, `>=`) do not (`E124`: `Cannot use
  ordering operators on enum type 'E'. Cast to int first`).

### Casts and built-in members

Every enum gets these without declaring anything:

| Form | Meaning |
|---|---|
| `(v to int)` | The variant's ordinal. |
| `(v to string)` | The variant's declared name. |
| `("Name" to EnumType)` | The variant with that name. |
| `(n to EnumType)` | The variant with ordinal `n`. |
| `EnumType::COUNT` | Number of variants, an `int`. |
| `EnumType::VALUES` | A `T*` array of every variant, in declaration order. |
| `EnumType::NAMES` | A `string*` array of every variant name, in declaration order. |

```turflang
enum Direction { North, East, South, West }

fn main(): int {
    immut d: Direction = Direction::North;
    test::assert((d to int) == 0, "North has ordinal 0");
    test::assert((d to string) == "North", "North to string");

    immut parsed: Direction = "South" to Direction;
    test::assert((parsed to int) == 2, "South has ordinal 2");

    test::assert(Direction::COUNT == 4, "four variants");
    return 0;
}
```

A cast from a value known only at run time is checked when it runs: an
integer outside `0 <= n < COUNT`, or a string that names no variant, aborts
the program (`Runtime Error: Invalid int to enum conversion.` or `Invalid
string to enum conversion.`). So a value of an enum type is always one of its
variants. A compile-time constant out of range, `(7 to E)`, is `E127`
(`Value 7 is out of range for enum 'E'`).

`VALUES` and `NAMES` are raw pointers, so reading them needs `unsafe { }`
(`E130` otherwise):

```turflang
enum Direction { North, East, South, West }

fn main(): int {
    mut total: int = 0;
    unsafe {
        immut vals: Direction* = Direction::VALUES;
        immut names: string* = Direction::NAMES;
        mut i: int = 0;
        while (i < Direction::COUNT) {
            immut d: Direction = vals[i];
            immut n: string = names[i];
            total = total + (d to int) + n.length();
            i = i + 1;
        }
    }
    return total;
}
```

### Matching with `compare`

`compare` over an enum must cover every variant or end in `otherwise:`;
otherwise it is `E093` (`Non-exhaustive enum comparison on 'E'`, listing
the missing variants). Matching the same variant in two arms is `E133`
(`Duplicate pattern!`).

```turflang
enum Season { Spring, Summer, Autumn, Winter }

fn season_name(s: Season): string {
    compare s {
        matches Season::Spring:
            return "spring";
        done
        matches Season::Summer:
            return "summer";
        done
        matches Season::Autumn:
            return "autumn";
        done
        matches Season::Winter:
            return "winter";
        done
    }
}

fn is_cold(s: Season): bool {
    compare s {
        matches Season::Autumn:
        matches Season::Winter:
            return true;
        done
        otherwise:
            return false;
    }
}

fn main(): int {
    test::assert(season_name(Season::Summer) == "summer", "name");
    test::assert(is_cold(Season::Winter), "cold");
    return 0;
}
```

Stacked `matches` lines share one body. Struct patterns, guards and
bindings, and exhaustiveness in full, are in
[Control Flow](control_flow.md#compare--matches).

### Enums in structs, and `pub`

An enum is a plain value and can be a struct field or array element. Like
structs, enums are private to their module unless `pub`; every variant is
public exactly when the enum is.

```turflang
pub enum Level { Low, Mid, High }

struct Sensor {
    level: Level,
    id: int,
}

fn main(): int {
    mut s: Sensor = Sensor::default();      // level is Level::Low
    s.level = Level::High;
    return s.level to int;                  // 2
}
```

A `machine` is an enum with a declared graph of legal moves between its
variants; see [State Machines](state_machines.md).
