# Building arrays: `arrays`

Functions that build a whole fixed-size array: `fill`, `generate`, `reversed`
and `map`. Not prelude: it needs `import arrays;`, and the functions are called
qualified (`arrays::fill(0)`).

```turflang
import arrays;
```

This is not the `array` module. `array` (singular) is in the prelude and has
`sum`, `max`, `min`, `contains` and `index_of` over `int[]`, `double[]` and
`string[]` ([Core: `array`](core.md#array)). `arrays` (plural) makes arrays.

## The length

Each function returns a `T[N]`, where `N` is a [size
parameter](../generics_and_operators.md#size-parameters). The length is never
written at the call. A call gets it from an array argument, or else from the
type the result is stored into: a declaration with a written type, an
assignment target, a parameter of a non-generic function, or a `return` in a
function whose return type is written.

```turflang
import arrays;
import io;

fn ends(a: int[4]): int {
    return a[0] + a[3];
}

fn four_nines(): int[4] {
    return arrays::fill(9);
}

fn main(): int {
    mut zeros: int[1000] = arrays::fill(0);   // 1000 zeros
    zeros = arrays::fill(7);                  // an assignment target
    io::printline(zeros[999]);                // 7
    io::printline(ends(arrays::fill(2)));     // 4: from the parameter type
    io::printline(four_nines()[3]);           // 9: from the return type
    return 0;
}
```

When nothing gives the length, it is E129: `mut what = arrays::fill(0);` and a
bare `arrays::fill(0);` have no type to read it from. A wrong length or element
type at the target is an ordinary mismatch: `mut bad: int[3] = arrays::fill("x");`
is E004, since the call returns a `string[3]`.

A length is always positive (`int[0]` does not parse, E123).

Every function is pure: it returns a new array and changes nothing it is given.

## Functions

### `fill(value: T): T[N]`

An array of `N` copies of `value`. `T` can be any type that has a zero value:
`int`, `double`, `bool`, `string`, a struct, another array.

```turflang
import arrays;
import io;

struct Point {
    x: int,
    y: int
}

fn main(): int {
    mut p: Point = Point::default();
    p.x = 3;
    mut pts: Point[2] = arrays::fill(p);
    mut row: int[3] = arrays::fill(1);
    mut grid: int[2][3] = arrays::fill(row);   // T is int[3], N is 2
    io::printline(pts[1].x);                   // 3
    io::printline(grid[1][2]);                 // 1
    return 0;
}
```

### `generate(f: fn(int): T): T[N]`

The array `f(0)`, `f(1)`, ..., `f(N - 1)`, in that order.

```turflang
import arrays;
import io;

fn square(i: int): int {
    return i * i;
}

fn main(): int {
    mut squares: int[5] = arrays::generate(square);
    io::printline(squares[4]);   // 16
    return 0;
}
```

### `reversed(a: T[N]): T[N]`

The elements of `a`, last first. `a` is unchanged.

### `map(a: T[N], f: fn(T): U): U[N]`

`f` applied to each element of `a`, in order. The result has the same length
and may have another element type.

```turflang
import arrays;
import io;

fn square(i: int): int {
    return i * i;
}

fn label(i: int): string {
    return "n" + (i to string);
}

fn main(): int {
    mut squares: int[5] = arrays::generate(square);
    mut back: int[5] = arrays::reversed(squares);
    mut texts: string[5] = arrays::map(squares, label);
    io::printline(back[0]);     // 16
    io::printline(texts[2]);    // n4
    return 0;
}
```

## Arrays of references

An element type that holds a non-null reference has no zero value, so an array
of it can't be filled: `(&Node)[2]` is E128. Fill a `(&?Node)[2]` from a
`&?Node` value instead.

<!-- doc-example: fails E128 -->
```turflang
import arrays;

struct Node {
    v: int
}

fn main(): int {
    mut n: Node = Node::default();
    mut a: (&Node)[2] = arrays::fill(&n);
    return 0;
}
```

```turflang
import arrays;
import io;

struct Node {
    v: int
}

fn main(): int {
    mut n: Node = Node::default();
    n.v = 4;
    mut first: &?Node = &n;
    mut refs: (&?Node)[3] = arrays::fill(first);
    io::printline(refs[2] != null);   // true
    return 0;
}
```
