# Generics and operator overloading

This page covers two features that let you write code once and use it for many
types: **generics** (functions and structs parameterized by type) and
**operator overloading** (giving `+`, `==`, `[]` and friends a meaning on your
own types). The standard collections use both: `List<T>` is a generic struct,
and `list[i]` is a generic operator.

## Generics

Generics are **monomorphized**. A generic function, method or struct is a
template; each distinct combination of type arguments you use produces its own
fully specialized copy, checked and compiled for exactly those types. `List<int>`
and `List<string>` are two different structs with two different sets of
compiled methods, and nothing is boxed or erased: a `List<int>` stores real
`int`s, a `Box<Big>` holds a real `Big` by value, whatever its size.

What follows from that:

- `sizeof(T)` inside a generic body is the real size of the type argument.
- A generic struct instantiated with a large struct works the same as one
  instantiated with `int`. There is no need to store pointers to large elements
  (`List<Entry*>`) and no reason to; use `List<Entry>`.
- A generic body is type-checked again for every instantiation, so an error
  that depends on the type argument is reported for that instantiation (see
  [Errors inside a generic body](#errors-inside-a-generic-body)).
- Instantiations that no code asks for are never compiled.

```turflang
import list::List;
import test;

struct Entry {
    key: string,
    value: string,
    hits: int
}

fn main(): int {
    mut store: List<Entry> = List::default();
    mut e: Entry = Entry::default();
    e.key = "language";
    e.value = "Turf";
    store.push(e);

    test::assert(store[0].key == "language", "elements are stored by value");
    test::assert(store.get(0).value == "Turf", "whatever their size");
    return 0;
}
```

### Generic functions

Type parameters are declared in `< >` right after `fn`. They can be used
anywhere a type can: parameter types, the return type, local variables, array
types, other generic types and function types.

```turflang
import test;

fn<T> identity(x: T): T {
    return x;
}

fn<T> first(items: T[]): T {
    return items[0];
}

fn<T> sizeOfValue(x: T): int {
    return sizeof(T);
}

fn<T> countDown(x: T, n: int): int {
    if (n == 0) {
        return 0;
    }
    return 1 + countDown(x, n - 1);
}

fn main(): int {
    test::assert(identity(42) == 42, "identity<int>");
    test::assert(identity("hello") == "hello", "identity<string>");
    test::assert(identity(3.5) == 3.5, "identity<double>");

    mut arr: int[3] = [5, 6, 7];
    test::assert(first(arr) == 5, "T[] binds T to the element type");

    test::assert(sizeOfValue(1) == 8, "sizeof(T) is the real size");
    test::assert(sizeOfValue(true) == 1, "...for every instantiation");

    test::assert(countDown("s", 3) == 3, "a generic may call itself");
    return 0;
}
```

A generic function may take a function-typed parameter that mentions its own
type parameters. This is how a generic body uses an operation the caller
chooses rather than one `T` must provide, and how the standard library's
`List.sort(cmp: fn(T, T): int)` is written:

```turflang
import test;

fn<T> largest(a: T, b: T, greater: fn(T, T): bool): T {
    if (greater(a, b)) {
        return a;
    }
    return b;
}

fn greaterInt(a: int, b: int): bool {
    return a > b;
}

fn greaterString(a: string, b: string): bool {
    return a > b;
}

fn<T> applyTwice(x: T, f: fn(T): T): T {
    return f(f(x));
}

fn increment(n: int): int {
    return n + 1;
}

fn main(): int {
    test::assert(largest(3, 9, greaterInt) == 9, "T = int");
    test::assert(largest("pear", "apple", greaterString) == "pear", "T = string");
    test::assert(applyTwice(1, increment) == 3, "fn(T): T");
    return 0;
}
```

Several generic functions may share a name if their parameter lists differ,
exactly like ordinary overloads.

### How type arguments are chosen

**Calls to a generic function infer the type arguments from the arguments.**
`identity(42)` binds `T` to `int` because the argument is an `int`; `first(arr)`
binds `T` to `int` because `arr` is `int[3]`. Every type parameter must appear
in the parameter list, since that is the only place it can be inferred from
(`fn<T> make(): T` is E129 at the declaration).

**Every argument that mentions a type parameter must agree on it.** Inference
never widens: with `fn<T> pair(a: T, b: T): T`, `pair(1, 2.5)` and
`pair(1, "a")` are E056, because `T` would be `int` from the first argument and
something else from the second. Convert one argument (`pair(1 to double, 2.5)`).

**There is no syntax for writing type arguments at a use.** `<` after a name in
an expression is always the less-than operator, so `identity<int>(3)` and
`Box<int>::default()` do not parse (E110). Where a type argument cannot be
inferred, put the type on a declaration instead:

<!-- doc-example: fails E110 -->
```turflang
fn<T> identity(x: T): T {
    return x;
}

fn main(): int {
    immut r: int = identity<int>(3);
    return 0;
}
```

**A generic struct takes its type arguments from the type it is stored into.**
`Box::default()` and `Box::new(...)` have no type arguments of their own; the
declared type of the variable, parameter, field, return type or element they
are assigned to supplies them:

```turflang
import list::List;
import test;

struct Box<T> {
    val: T
}

fn<T> Box.new(v: T): void {
    self.val = v;
}

fn<T> Box.get(): T {
    return self.val;
}

fn makeBox(): Box<string> {
    return Box::new("made");
}

fn take(b: Box<int>, extra: int): int {
    return b.get() + extra;
}

fn main(): int {
    mut a: Box<int> = Box::new(42);
    mut b: Box<int> = Box::default();
    mut boxes: List<Box<int>> = List::default();
    boxes.push(Box::new(7));

    test::assert(a.get() == 42, "annotated variable");
    test::assert(b.get() == 0, "default() zero-fills");
    test::assert(makeBox().get() == "made", "return type");
    test::assert(take(Box::new(1), 2) == 3, "argument");
    test::assert(boxes[0].get() == 7, "collection element");
    return 0;
}
```

The arguments must then have exactly those types: with `Box<string>` declared,
`Box::new(42)` is E056, and with `Box<double>` or `Box<int32>` declared,
`Box::new(1)` is E056 too, because an argument is not converted to fit. Write
the conversion (`Box::new(1 to double)`).

When nothing supplies the type, there is nothing to infer from. `mut l =
List::default();` is rejected with E129 (Nothing to infer! Cannot infer the
type arguments of a generic type):

<!-- doc-example: fails E129 -->
```turflang
import list::List;

fn main(): int {
    mut l = List::default();
    return 0;
}
```

Write `mut l: List<int> = List::default();` instead.

Generic types nest freely, with or without a space between the closing `>`s:

```turflang
import list::List;
import map::Map;
import test;

fn main(): int {
    mut grid: List<List<int>> = List::default();
    mut row: List<int> = List::default();
    grid.push(row);
    grid[0].push(4);
    test::assert(grid[0][0] == 4, "List<List<int>>");

    mut index: Map<string, List<int>> = Map::default();
    mut ids: List<int> = List::default();
    index["a"] = ids;
    index["a"].push(1);
    test::assert(index["a"][0] == 1, "Map<K, List<V>>");
    return 0;
}
```

### Generic structs and their methods

A struct declares its type parameters in `< >` after its name; several are
comma-separated. Its fields may use them anywhere, including inside other
generic types.

A method on a generic struct repeats the struct's type parameters after `fn`:
`fn<T> Box.get()` for `struct Box<T>`, `fn<K, V> Map.put(...)` for
`struct Map<K, V>`. Leaving them off is E138 (Missing type parameters!), because
`self`'s layout depends on the type arguments:

<!-- doc-example: fails E138 -->
```turflang
struct Box<T> {
    val: T
}

fn Box.broken(): int {
    return 1;
}

fn main(): int {
    return 0;
}
```

A method can declare further type parameters of its own alongside the struct's,
and a method on a non-generic struct can be generic too. Each is inferred from
the method's arguments:

```turflang
import test;

struct Pair<A, B> {
    first: A,
    second: B
}

fn<A, B> Pair.setFirst(a: A): void {
    self.first = a;
}

fn<A, B> Pair.swapped(): Pair<B, A> {
    mut r: Pair<B, A> = Pair::default();
    r.first = self.second;
    r.second = self.first;
    return r;
}

fn<A, B, U> Pair.pick(other: U): U {
    return other;
}

struct Printer {
    prefix: string
}

fn<T> Printer.echo(x: T): T {
    return x;
}

fn main(): int {
    mut p: Pair<int, string> = Pair::default();
    p.setFirst(7);
    p.second = "x";

    mut q: Pair<string, int> = p.swapped();
    test::assert(q.first == "x" && q.second == 7, "Pair<string, int>");

    test::assert(p.pick(9) == 9 && p.pick("s") == "s", "a method's own U");

    mut pr: Printer = Printer::default();
    test::assert(pr.echo(5) == 5 && pr.echo("hi") == "hi", "generic method, plain struct");
    return 0;
}
```

Structs are values: assigning a `Box<int>` to another variable copies it.

### What a generic body can do with `T`

There are no traits or constraints. A generic body is checked against whatever
`T` actually is, once per instantiation, so it can use any operation that type
supports:

- **Operators**: `==` and `!=`, ordering (`< <= > >=`), arithmetic
  (`+ - * / % **`), bitwise (`& | ^ << >> ~`), unary `-` and `!`, `++`/`--` and
  compound assignment. Each works when `T` has it, built in (numbers, `bool`,
  `char`, `string` by content, enums, pointers) or declared (`operator <` on a
  struct).
- **Fields and methods**: `x.name` and `x.method()` when `T` is a struct that
  has them.
- **`sizeof(T)`** and **`to` casts** involving `T`.
- **Calls** to functions with `T` values as arguments. Overload resolution runs
  per instantiation, so `hash(x)` inside a generic body picks the `string`
  overload when `T` is `string` and the general one otherwise.
- **Generic structs and functions built from `T`** (`Box<T>`, `List<T>`,
  `helper<T>(...)`).

```turflang
import test;

struct Money {
    cents: int
}

operator <(a: Money, b: Money): bool { return a.cents < b.cents; }

fn<T> largest(a: T, b: T): T {
    if (a < b) {
        return b;
    }
    return a;
}

fn<T> sum(a: T, b: T): T {
    return a + b;
}

fn<T> describe(x: T): string {
    return "value";
}

fn describe(x: string): string {
    return "string";
}

fn<T> viaGeneric(x: T): string {
    return describe(x);
}

fn main(): int {
    test::assert(largest(3, 9) == 9, "T = int");
    test::assert(largest("pear", "apple") == "pear", "T = string");
    mut a: Money = Money::default();
    a.cents = 250;
    mut b: Money = Money::default();
    b.cents = 199;
    test::assert(largest(a, b).cents == 250, "T = Money, through its operator <");
    test::assert(sum(1.5, 2.0) == 3.5, "T = double");
    test::assert(viaGeneric(1) == "value", "T = int uses the generic fallback");
    test::assert(viaGeneric("s") == "string", "T = string picks the string overload");
    return 0;
}
```

The template itself is never rejected for what `T` might lack: whether `a < b`
works depends on what `T` is, so it is decided for each instantiation, and an
instantiation whose `T` lacks the operation is an error at the call that asked
for it, with the code the same mistake has anywhere else (E124 for a
comparison, E057 for arithmetic, E068 for a field, E131 for a method). The
next section shows how it reads. To accept types that lack an operation, take
it as a function parameter, as the `greater` version of `largest` does above.

<!-- doc-example: fails E124 -->
```turflang
struct Point {
    x: int
}

fn<T> largest(a: T, b: T): T {
    if (a < b) {
        return b;
    }
    return a;
}

fn main(): int {
    mut p: Point = Point::default();
    mut q: Point = Point::default();
    return largest(p, q).x;     // E124: Point has no `<`
}
```

### Errors inside a generic body

An operation that exists for some type arguments and not others is reported
when the offending instantiation is requested. The error is anchored at the line
in **your** code that asked for the instantiation. Notes under it give the chain
of instantiations that led to the failing one and the generic declaration it came
from. The error's code is the one the same mistake has outside a generic.

Here `Set<T>` needs `==` on its elements, and a plain struct has none:

<!-- doc-example: fails E124 -->
```turflang
import set::Set;

struct Big {
    a: int,
    b: int,
    c: int
}

fn main(): int {
    mut s: Set<Big> = Set::default();
    mut b: Big = Big::default();
    s.add(b);
    return 0;
}
```

```text
main.tr (1 error)

── error at line 12, col 6 [E124 · type mismatch]

  Oops! Something doesn't add up:
  In this instantiation: Cannot compare struct values with '=='. (Structs have
  no built-in comparison: compare the fields you care about (e.g. `a.x ==
  b.x`), or declare `operator ==` for the type.)
  Note: 'Set<Big>.add' requires 'Set<Big>.contains' here (lib/std/collections/set.tr, ...)
  Note: 'Set<Big>.contains' requires 'Set<Big>.index_of' here (lib/std/collections/set.tr, ...)
  Note: 'Set<Big>.index_of' requires 'Set<Big>.bucketFind' here (lib/std/collections/set.tr, ...)
  Note: the error itself, inside 'Set<Big>.bucketFind' (lib/std/collections/set.tr, ...)
  Note: in generic declaration 'set::Set.bucketFind' (lib/std/collections/set.tr, ...)

  12 │     s.add(b);
     │      ^ Here!
```

Read the notes from top to bottom: `add` needed `contains`, which needed
`index_of`, which needed `bucketFind`, and `bucketFind` is where `==` was applied
to two `Big`s. The fix here is to declare `operator ==(a: Big, b: Big): bool`
(see [Operator overloading](#operator-overloading)).

The same reporting applies to a call that finds no overload for the concrete
type (`No overload of 'len' matches argument types (int).`) or an operation
such as indexing that the type does not support.

Three further limits protect the compiler from a generic that never finishes:

- **E079, generic instantiation depth exceeded.** A generic that asks for a
  strictly bigger instantiation of itself every time it runs (`wrap<int>` calls
  `wrap<&int>` calls `wrap<&&int>` ...) is stopped at a chain depth of 64. The error
  prints the chain. Keep the type fixed across the recursion and let only the
  value recurse, for example by passing a pointer or reference.
- **E079, too many generic instantiations.** A generic that requests two or more new
  instantiations per level (`f<T>` calls `f<Box<T>>` and `f<Pair<T>>`) hits a
  cap on the total number of instantiations in the program.
- **E069, a struct that contains itself by value** through a generic
  instantiation: with `struct A<T> { val: T }` and `struct B { back: A<B> }`, the
  instantiation `A<B>` holds a `B` that holds an `A<B>`. Use a reference or
  pointer for the field.

<!-- doc-example: fails E079 -->
```turflang
fn<T> wrap(x: &T): int {
    return wrap(&x);
}

fn main(): int {
    mut v: int = 1;
    return wrap(&v);
}
```

### Visibility of generics

A generic function or struct follows the ordinary [visibility
rules](functions_and_modules.md): it is usable from another module only when
declared `pub`. The body of a `pub` generic can call its own module's private
helpers; each instantiation is resolved in the module that declares the
template, not in the module that requested it. Generic struct fields are private
unless declared `pub`.

---

## Operator overloading

An `operator` declaration gives a symbol a meaning on your own types. The call
site is the plain operator: `a + b`, `-a`, `list[i]`, `m[k] = v`.

### Declaration syntax

An operator is a top-level declaration: the keyword `operator`, the symbol, a
parameter list and a body. It is not `.`- or `::`-qualified and is never
`static`. The operand names are ordinary parameter names; `self` is a reserved
word and cannot be one (E108).

```turflang
import test;

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
    test::assert(c.x == 11, "x components add");
    test::assert(c.y == 22, "y components add");
    return 0;
}
```

The symbol follows `operator` directly. A type-parameter list, if any, goes
between the keyword and the symbol (see [Generic operators](#generic-operators)).
A declaration with the wrong number of parameters is E115, and a declaration
without a body is E107.

### The overloadable operators

```text
binary    +  -  *  /  %  **  &  |  ^  <<  >>  ==  !=  <  >  <=  >=
unary     -  !  ~
indexing  []   (read, x[i])        []=   (write, x[i] = v)
```

Binary operators take two parameters, unary operators one. `-` has both forms and
the number of parameters tells them apart. `[]` takes `(container, index)` and
`[]=` takes `(container, index, value)`.

```turflang
import test;

struct Num {
    v: int
}

fn num(v: int): Num {
    mut n: Num = Num::default();
    n.v = v;
    return n;
}

operator +(a: Num, b: Num): Num { return num(a.v + b.v); }
operator -(a: Num, b: Num): Num { return num(a.v - b.v); }
operator -(a: Num): Num { return num(-a.v); }
operator *(a: Num, b: Num): Num { return num(a.v * b.v); }
operator /(a: Num, b: Num): Num { return num(a.v / b.v); }
operator %(a: Num, b: Num): Num { return num(a.v % b.v); }
operator &(a: Num, b: Num): Num { return num(a.v & b.v); }
operator |(a: Num, b: Num): Num { return num(a.v | b.v); }
operator ^(a: Num, b: Num): Num { return num(a.v ^ b.v); }
operator <<(a: Num, b: Num): Num { return num(a.v << b.v); }
operator >>(a: Num, b: Num): Num { return num(a.v >> b.v); }
operator ~(a: Num): Num { return num(~a.v); }
operator ==(a: Num, b: Num): bool { return a.v == b.v; }
operator !=(a: Num, b: Num): bool { return a.v != b.v; }
operator <(a: Num, b: Num): bool { return a.v < b.v; }
operator >(a: Num, b: Num): bool { return a.v > b.v; }
operator <=(a: Num, b: Num): bool { return a.v <= b.v; }
operator >=(a: Num, b: Num): bool { return a.v >= b.v; }

fn main(): int {
    immut a: Num = num(12);
    immut b: Num = num(3);
    test::assert((a + b).v == 15 && (a - b).v == 9, "+ -");
    test::assert((a * b).v == 36 && (a / b).v == 4 && (a % b).v == 0, "* / %");
    immut band: Num = a & b;
    immut bor: Num = a | b;
    immut bxor: Num = a ^ b;
    immut shl: Num = a << b;
    immut shr: Num = a >> b;
    test::assert(band.v == 0 && bor.v == 15 && bxor.v == 15, "& | ^");
    test::assert(shl.v == 96 && shr.v == 1, "<< >>");
    test::assert((-a).v == -12 && (~a).v == ~12, "unary - and ~");
    test::assert(a != b && !(a == b) && b < a && a > b && b <= a && a >= b, "comparisons");
    return 0;
}
```

Each comparison is its own operator. Declaring `==` does not give you `!=`, and
declaring `<` does not give you `<=`; declare each one you use.

### Not overloadable

- `&&` and `||`. They short-circuit, which a function call cannot.
- `=`, call `()`, member access `.`, and `to` conversions.
- A compound assignment as an operator of its own. `a += b` is `a = a + b` and
  uses `operator +`; there is no `operator +=`. The same goes for `++` and `--`.

Declaring any of these is a syntax error (E110, Expected an overloadable operator
after 'operator'), and the hint lists the symbols that can be overloaded.

### Operators between built-in types are not allowed

A declaration is rejected (E028) when **every** operand is a built-in primitive
type (`int`, `int32`, `double`, `bool`, `char`, or a pointer): that would change
how ordinary arithmetic means. At least one operand must be a struct, an enum or
a `string`.

<!-- doc-example: fails E028 -->
```turflang
operator +(a: int, b: int): int {
    return a;
}

fn main(): int {
    return 0;
}
```

`string` is not on that list because its own operators (`+`, `==`, `!=`, `<`, `>`,
`<=`, `>=`) are ordinary overloads declared in the standard library's prelude.
They are always available, with no import:

```turflang
import test;

fn main(): int {
    mut a: string = "foo";
    mut b: string = "bar";

    test::assert(a + b == "foobar", "concatenation is operator +");
    test::assert(a == "foo", "equality compares content");
    test::assert(a != b, "inequality");
    test::assert(b < a, "ordering is lexicographic");

    a += "!";
    test::assert(a == "foo!", "a += b uses operator +");
    return 0;
}
```

Redeclaring `operator +(string, string)` yourself is an ambiguity error (E125).
Enums are ordinary operand types: `operator ==(a: Color, b: Color): bool` is
accepted, and is how you give an enum equality.

### Overload selection

A use `a op b` looks for a visible `operator op` whose parameter types match the
operand types **exactly**. Operators never widen: with `operator +(a: Vec2,
b: int)` declared, `v + 1` works and `v + 1.0` is E057. Operand order matters
too; `1 + v` needs its own `operator +(int, Vec2)`.

Overloads of the same symbol are told apart by their operand types:

```turflang
import test;

struct Vec2 {
    x: int,
    y: int
}

fn vec(x: int, y: int): Vec2 {
    mut r: Vec2 = Vec2::default();
    r.x = x;
    r.y = y;
    return r;
}

operator +(a: Vec2, b: Vec2): Vec2 {
    return vec(a.x + b.x, a.y + b.y);
}

operator +(a: Vec2, b: int): Vec2 {
    return vec(a.x + b, a.y + b);
}

operator +(a: int, b: Vec2): Vec2 {
    return vec(a + b.x, a + b.y);
}

fn main(): int {
    immut a: Vec2 = vec(1, 2);
    immut b: Vec2 = vec(10, 20);

    immut s1: Vec2 = a + b;
    immut s2: Vec2 = a + 5;
    immut s3: Vec2 = 5 + a;
    test::assert(s1.x == 11 && s1.y == 22, "Vec2 + Vec2");
    test::assert(s2.x == 6 && s2.y == 7, "Vec2 + int");
    test::assert(s3.x == 6 && s3.y == 7, "int + Vec2");
    return 0;
}
```

<!-- doc-example: fails E057 -->
```turflang
struct Vec2 {
    x: int,
    y: int
}

operator +(a: Vec2, b: int): Vec2 {
    return a;
}

fn main(): int {
    mut v: Vec2 = Vec2::default();
    mut w: Vec2 = v + 1.0;
    return 0;
}
```

The return type is yours to choose. Comparison operators usually return `bool`,
but `operator +(Vec2, int): int` is legal.

### Unary operators

`-`, `~` and `!` take one parameter. A one-parameter `operator -` is negation; the
two-parameter one is subtraction, and both can be declared for the same type.

`!x` on a value whose type declares `operator !` calls that operator. For every
other type, `!x` is the built-in `x != true`. `bool` never declares one, so `!` on
a `bool` is unaffected.

```turflang
import test;

struct Vec2 {
    x: int,
    y: int
}

operator -(a: Vec2): Vec2 {
    mut r: Vec2 = Vec2::default();
    r.x = -a.x;
    r.y = -a.y;
    return r;
}

operator -(a: Vec2, b: Vec2): Vec2 {
    mut r: Vec2 = Vec2::default();
    r.x = a.x - b.x;
    r.y = a.y - b.y;
    return r;
}

struct Slot {
    empty: bool
}

operator !(s: Slot): bool {
    return s.empty;
}

fn main(): int {
    mut v: Vec2 = Vec2::default();
    v.x = 3;
    v.y = -4;

    immut neg: Vec2 = -v;
    test::assert(neg.x == -3 && neg.y == 4, "unary -");

    immut diff: Vec2 = v - neg;
    test::assert(diff.x == 6, "binary - beside unary -");

    mut s: Slot = Slot::default();
    s.empty = true;
    test::assert(!s, "!s calls operator !");
    test::assert(!false, "!bool is unchanged");
    return 0;
}
```

### Compound assignment, `++` and `--`

`a op= b` is `a = a op b`, with the left-hand location evaluated once, so a type
that declares `operator +` supports `+=`. The result of the operator must have
exactly the type of `a`. `x++`, `x--`, `++x` and `--x` use `operator +` and
`operator -` with an `int` right operand of `1`; the prefix forms evaluate to the
new value and the postfix forms to the old one. Any assignable location works:
a variable, a field, an array element.

```turflang
import test;

struct Counter {
    n: int
}

operator +(c: Counter, k: int): Counter {
    mut r: Counter = c;
    r.n = c.n + k;
    return r;
}

operator -(c: Counter, k: int): Counter {
    mut r: Counter = c;
    r.n = c.n - k;
    return r;
}

struct Tally {
    total: Counter
}

fn main(): int {
    mut c: Counter = Counter::default();
    c.n = 10;

    c += 5;
    test::assert(c.n == 15, "+= uses operator +");
    c -= 3;
    test::assert(c.n == 12, "-= uses operator -");

    ++c;
    test::assert(c.n == 13, "prefix ++");

    mut before: Counter = c++;
    test::assert(before.n == 13 && c.n == 14, "postfix ++ yields the old value");

    mut t: Tally = Tally::default();
    t.total += 4;
    test::assert(t.total.n == 4, "a field is a location too");
    return 0;
}
```

If the operator returns another type, `a += b` is E066 (the result "cannot be
assigned back" to `a`); if no operator matches, it is E057.

### Indexing: `operator []` and `operator []=`

`operator []` makes `x[i]` readable and `operator []=` makes `x[i] = v` writable.
They are independent: declare only `[]` and the type is read-only; declare only
`[]=` and it is write-only (a read is then E126, Cannot index a value of type
...). A use with an index or value type that matches no overload is E056 (No
'operator[]' overload matches ...). The operator picked is the one whose
parameter types match exactly, so a type can have several, for instance one
indexed by `int` and one by `string`.

```turflang
import test;

struct Grid {
    a: int,
    b: int,
    c: int
}

operator [](g: Grid, i: int): int {
    if (i == 0) {
        return g.a;
    }
    if (i == 1) {
        return g.b;
    }
    return g.c;
}

operator []=(g: &Grid, i: int, value: int): void {
    if (i == 0) {
        g.a = value;
    } else if (i == 1) {
        g.b = value;
    } else {
        g.c = value;
    }
}

operator []=(g: &Grid, name: string, value: int): void {
    g.a = value;
}

fn main(): int {
    mut g: Grid = Grid::default();
    g[1] = 5;
    g["first"] = 9;
    g[2] = g[1] + 1;

    test::assert(g[1] == 5 && g[0] == 9 && g[2] == 6, "read and write");
    test::assert(g.c == 6, "the write reached g");

    mut arr: int[3] = [1, 2, 3];
    test::assert(arr[0] == 1, "built-in array indexing is unaffected");
    return 0;
}
```

The container of `[]` or `[]=` may be declared by value (`g: Grid`) or by
reference (`g: &Grid`); this is the one place operands are not matched exactly.
Declare the container of `[]=` as `&Grid` so the write reaches the caller's
variable; a by-value container would write into a copy. The call site is always
just `g[i] = v`, and the reference is taken for you.

**An index result is a value, not a place.** `x[i]` calls a function that returns
the element by value. Reading through it is fine (`l[i].field`, `l[i].method()`),
but a write into part of the returned copy would be silently lost, so it is
rejected with E098 instead:

<!-- doc-example: fails E098 -->
```turflang
import list::List;

struct Big {
    a: int,
    b: int
}

fn main(): int {
    mut bs: List<Big> = List::default();
    mut e: Big = Big::default();
    bs.push(e);
    bs[0].a = 50;
    return 0;
}
```

E098 also covers `bs[0].a += 1`, `bs[0].a++`, a call to a method that assigns to
`self` (`bs[0].bump()`), and `[]=` applied to another index's result
(`mm["a"]["b"] = 1` on a `Map<string, Map<string, int>>`). Read the element into a
variable, change it and store it back:

```turflang
import list::List;
import test;

struct Big {
    a: int,
    b: int
}

fn main(): int {
    mut bs: List<Big> = List::default();
    mut e: Big = Big::default();
    bs.push(e);

    mut copy: Big = bs[0];
    copy.a = 50;
    bs[0] = copy;
    test::assert(bs[0].a == 50, "stored back");

    // These land in real storage, so they are accepted:
    mut grid: List<List<int>> = List::default();
    mut row: List<int> = List::default();
    grid.push(row);
    grid[0].push(4);
    grid[0][0] = 8;
    test::assert(grid[0][0] == 8, "a collection element is a shared handle");
    return 0;
}
```

The check is whether the destination is a field of the returned copy (or `self` of
a mutating method called on it). A write through a pointer or reference element,
or into a collection element (a handle to shared storage), is not a write to the
copy and is allowed.

**Compound assignment and `++`/`--` through an index are not supported** (E126,
Compound assignment through 'operator[]' is not supported). `box[i] += 1` and
`box[i]++` must be written `box[i] = box[i] + 1;`.

<!-- doc-example: fails E126 -->
```turflang
struct Cell {
    a: int
}

operator [](c: Cell, i: int): int {
    return c.a;
}

operator []=(c: &Cell, i: int, value: int): void {
    c.a = value;
}

fn main(): int {
    mut c: Cell = Cell::default();
    c[0] += 1;
    return 0;
}
```

### Generic operators

An operator can declare type parameters between the keyword and the symbol,
exactly where `fn<T>` puts them. The standard collections index this way:

```text
operator<T> [](l: List<T>, i: int): T
operator<T> []=(l: List<T>, i: int, value: T): void
operator<K, V> [](m: Map<K, V>, key: K): V
operator<K, V> []=(m: &Map<K, V>, key: K, value: V): void
```

so `l[i]`, `l[i] = v`, `m[k]` and `m[k] = v` work for every `List<T>` and `Map<K, V>`
(and `s[i]` reads a `Set<T>`; a `Set` has no `[]=`). A generic struct of your own
gets the same in any operator form. When `<` follows `operator` and an identifier,
it opens a type-parameter list; `operator<T> <(...)` declares the less-than
operator generically.

```turflang
import test;

struct Cell<T> {
    v: T,
    n: int
}

fn<T> cell(v: T, n: int): Cell<T> {
    mut c: Cell<T> = Cell::default();
    c.v = v;
    c.n = n;
    return c;
}

operator<T> +(a: Cell<T>, k: int): Cell<T> {
    return cell(a.v, a.n + k);
}

operator<T> -(c: Cell<T>): Cell<T> {
    return cell(c.v, -c.n);
}

operator<T> <(a: Cell<T>, b: Cell<T>): bool {
    return a.n < b.n;
}

operator<T> [](c: Cell<T>, i: int): T {
    return c.v;
}

fn main(): int {
    immut a: Cell<int> = cell(1, 5);
    immut b: Cell<string> = cell("s", 2);

    immut a2: Cell<int> = a + 3;
    immut b2: Cell<string> = b + 1;
    test::assert(a2.n == 8 && b2.n == 3, "one operator, two instantiations");
    test::assert((-a).n == -5, "unary");
    test::assert(a < a2 && b < b2, "<");
    test::assert(a[0] == 1 && b[9] == "s", "[]");

    mut c: Cell<int> = a;
    c += 10;
    c++;
    test::assert(c.n == 16, "compound assignment");
    return 0;
}
```

A use resolves in two steps. First the type parameters are **inferred from the
operands** (they cannot be written at a use). Then each declared operand type,
with the parameters substituted, must match its operand **exactly**, as for a
concrete operator. Generics add inference and nothing else:
`Cell<string> + 1.5` still fails against `operator<T> +(a: Cell<T>, k: int)`
(E057), and `Cell<int> == Cell<string>` fails against `operator<T> ==(a: Cell<T>,
b: Cell<T>)` because `T` is already `int`. One copy of the operator is compiled
per distinct set of type arguments, as for a generic function.

Two rules apply to the declaration, both E097:

- **Every type parameter must appear in an operand.** One that appears only in the
  return type could never be inferred.
- **At least one operand must be a struct or enum declared in the same module**,
  looked for through `&`, `*` and the base of a generic type. A bare
  `operator<T> +(a: T, b: T): T` would apply to every type, including types from
  modules that never imported yours.

<!-- doc-example: fails E097 -->
```turflang
operator<T> +(a: T, b: T): T {
    return a;
}

fn main(): int {
    return 0;
}
```

### The orphan rule

Every operator declaration, generic or not, needs at least one operand type that
is a struct or enum declared in **the same module** as the operator (E080, the
operator "does not own any of its operand types"). Only operands made entirely of
built-in types and `string` are exempt, since those have no owner module; they
are the standard library's own `string` operators. You can overload `Vec2 + int`
in the module that declares `Vec2`, but not `List<T> + List<T>`, or
`List<int> + List<int>`, anywhere else (an instance belongs to the module that
declares its struct, and `&`/`*` around an operand do not change its owner): the meaning of `a + b` never depends on which modules happen to be imported.

<!-- doc-example: fails E080 -->
```turflang
import list::List;

operator<T> +(a: List<T>, b: List<T>): List<T> {
    return a;
}

fn main(): int {
    return 0;
}
```

### Visibility

An operator is visible wherever its operand types are. There is no `pub` for an
operator (writing one is E087; the hint says to make the type `pub` instead) and
no qualified call syntax, so `a + b` works in any module that can name the type of
`a`, whether it imported the type by name, the whole module, or only received a
value from one of its functions. Import the module that declares the type and the
operators come with it.

```text
// vecmod.tr
pub struct Vec2 {
    pub x: int,
    pub y: int
}

pub fn make(x: int, y: int): Vec2 { ... }

operator +(a: Vec2, b: Vec2): Vec2 { ... }      // no `pub`

// main.tr
import vecmod::make;

fn main(): int {
    immut b = make(1, 2) + make(3, 4);          // operator + found
    return b.x;
}
```

A type's own fields and methods still follow the usual `pub` rules; the operator's
body in `vecmod` can read private fields.

An operator used on a type with no matching overload is an ordinary type error
(E057 for arithmetic, E124 for a comparison), never an internal one.

### Summary of errors

| Code | Meaning |
|---|---|
| E028 | Every operand is a built-in type |
| E056 | No `operator []`/`[]=` matches the index or value type |
| E057 | No `operator` matches these operand types (including a `+=` with a mismatched operand) |
| E066 | Result of the operator for `a op= b` is not the type of `a` |
| E080 | The operator owns none of its operand types |
| E087 | `pub` on an operator |
| E097 | A generic operator has an unused type parameter, or owns no operand |
| E098 | Writing into part of an index result |
| E107 / E115 | Operator without a body / wrong number of parameters |
| E110 | A symbol that cannot be overloaded |
| E124 | No `operator ==`/`<`/... for these operand types |
| E126 | Indexing without `operator []`, or a compound assignment through one |
