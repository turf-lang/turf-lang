# Arrays

## Fixed-size syntax and literals

A declared array type is written `T[N]`, where `N` is a compile-time
constant size:

```turflang
mut numbers: int[5] = [0, 0, 0, 0, 0];
numbers[0] = 10;
numbers[1] = 20;

mut arr: int[2] = [1, 2];
```

A declaration with no initializer is legal and zero-initializes every
element:

```turflang
mut arr: int[5];   // all elements start at 0
arr[0] = 10;
arr[1] = 20;
arr[4] = 50;
// arr[2] and arr[3] are still 0
```

This works for any element type, including `double`:

```turflang
mut darr: double[3];   // all elements start at 0.0
darr[1] = 3.5;
```

### Multidimensional arrays

Nest the size suffix for multiple dimensions: `int[2][3]` is 2 rows of 3
columns. Array literals nest to match:

```turflang
mut arr: int[2][3] = [[1, 2, 3], [4, 5, 6]];
arr[1][2]; // 6

arr = [[10, 20, 30], [40, 50, 60]];
arr[1][1]; // 50

// A single row can be reassigned on its own:
arr[0] = [100, 200, 300];
arr[0][2]; // 300
```

Three dimensions work the same way:

```turflang
fn get_3d_array(): int[2][2][2] {
    return [
        [[1, 2], [3, 4]],
        [[5, 6], [7, 8]]
    ];
}

get_3d_array()[1][0][1]; // 6
```

### Arrays are value types

Assigning one array variable to another copies it. Mutating the copy never
affects the original:

```turflang
mut arr: int[2][3] = [[1, 2, 3], [4, 5, 6]];
mut copy_arr: int[2][3] = arr;
copy_arr[1][0] = 999;

// arr is unchanged -- arr[1][0] is still 4, and copy_arr[1][0] is 999.
```

### Arrays as arguments and struct elements

Array literals can be passed directly as function arguments, and structs
can appear as array elements:

```turflang
struct Point {
    x: int,
    y: int
}

fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

fn sum_array(arr: int[4]): int {
    return arr[0] + arr[1] + arr[2] + arr[3];
}

fn get_points(): Point[2] {
    return [Point::new(10, 20), Point::new(30, 40)];
}

fn main(): int {
    sum_array([1, 2, 3, 4]);           // literal passed directly, == 10

    mut points: Point[2] = [Point::new(1, 2), Point::new(3, 4)];
    points[0].x; // 1
    points[1].y; // 4

    get_points()[1].x; // 30 -- inline indexing on a call result
    return 0;
}
```

### Unsized parameters: `T[]`

The unsized spelling `T[]` is valid only in a function parameter position.
It accepts an argument array of **any** concrete length:

```turflang
fn sumFive(arr: int[]): int {
    return arr[0] + arr[1] + arr[2] + arr[3] + arr[4];
}

fn main(): int {
    mut a: int[5] = [1, 2, 3, 4, 5];
    sumFive(a);   // OK

    mut b: int[10] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    sumFive(b);   // also OK -- length doesn't have to match a fixed size
    return 0;
}
```

> [!NOTE]
> `T[]` is a parameter-only spelling. You cannot declare a local variable
> or a struct field with an unsized array type — every array with a
> concrete home (a variable, a field, a return type) must carry a known
> size.

## Type-checking constraints

- An initializer list's length must match the declared array size exactly.
  A mismatched length is a compile error.
- Every element in an initializer list must match the declared element
  type. A mismatched element type is a compile error.
- Assigning an array of one declared size into a variable of a
  **different** concrete size is a type error — sizes are part of an
  array type's identity, not just its element type.

## Negative indices

A negative index counts back from the end, as in Python: `a[-1]` is the last
element, `a[-2]` the one before it, and `a[-N]` the first element of an
`N`-element array.

```turflang
mut a: int[4] = [10, 20, 30, 40];
a[-1];        // 40, the same element as a[3]
a[-4];        // 10, the same element as a[0]
a[-1] = 41;   // writes a[3]
a[-2] += 5;   // compound assignment works too
```

The valid range is therefore `-N <= i < N`. It does **not** wrap around:
`a[-5]` and `a[4]` are both out of bounds on a 4-element array, caught at
compile time when the index is a constant and by the runtime check otherwise.

The same rule applies to `string` indexing (`s[-1]` is the last byte; see
[String indexing](operators.md#string-indexing-si)) and to `List<T>`'s `l[i]`
/ `l[i] = v` ([stdlib](stdlib/collections.md)). It does **not** apply to a raw
pointer (`T*`) or an unsized parameter (`T[]`), which carry no length to count
back from — there `p[-1]` stays plain pointer arithmetic and is unchecked.

## Bounds checking

Turflang catches array out-of-bounds accesses at **compile time** whenever
it can prove the index is out of range, and falls back to a **runtime
panic** only when the index genuinely can't be resolved to a constant.

### Caught at compile time

The compiler performs constant propagation and rejects a provably
out-of-bounds access before the program ever runs, in each of these
shapes:

**A literal index directly out of range:**

```turflang
mut arr: int[3] = [1, 2, 3];
mut x: int = arr[3];   // COMPILE ERROR: index 3 is out of bounds for int[3]
```

**A local variable holding a provably-constant out-of-bounds value:**

```turflang
mut arr: int[3] = [10, 20, 30];
mut i: int = 5;
arr[i];   // COMPILE ERROR: i is provably 5, out of bounds for int[3]
```

This holds even when the offending variable is declared inside a nested
block rather than directly alongside the array.

### Falls back to a runtime panic

When the index genuinely cannot be proven constant at compile time (for
example, it depends on a loop counter, a function argument, or other
runtime-computed value), the compiler instead emits a runtime bounds
check. A negative index first has `size` added to it (see
[Negative indices](#negative-indices)); the result must then satisfy
`0 <= idx < size`, so the check fires for `idx < -size` as well as
`idx >= size`. On violation, the program panics with a message like:

```
Array index out of bounds for '<name>'
```

and aborts. This is the same general panic-and-abort convention used
elsewhere in the compiler (for example, an unnarrowed
[state machine](state_machines.md#runtime-checked-transitions) transition
that turns out to be invalid at runtime uses the identical pattern).

> [!NOTE]
> In practice, most out-of-bounds accesses that show up in straightforward
> code are caught by the compile-time constant-propagation pass described
> above. The runtime guard exists as a safety net for indices the analysis
> genuinely cannot resolve to a constant — it is a real, reachable code
> path, not dead code, but you should expect most bugs of this shape to
> surface as compile errors rather than runtime aborts.
