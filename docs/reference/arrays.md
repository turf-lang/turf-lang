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

#### Letting the literal pick the size

The size can be left out when a literal initializes the declaration. `T[]`
with a literal takes the literal's length, and the variable is then exactly
`T[N]`: an owned array with value semantics (assigning copies it) and a fixed
length. `guess` infers the same way.

```turflang
mut nums: int[] = [10, 20, 30];     // exactly int[3]
immut guessed = [1, 2];             // int[2]
mut g: int[][2] = [[1, 2], [3, 4]]; // only the outermost size is inferred: int[2][2]
nums = [7, 8, 9];                   // fine: same length
// nums = [1, 2];                   // E066: 'int[2]' to 'int[3]'
```

An explicit `T[N]` is still worth writing when the size is a promise: the
compiler then rejects a literal of any other length (`E003`). An empty
literal has no size to give (`mut a: int[] = [];` is `E123`), and an inner
dimension can't be left out (`int[2][] = ...` is `E140`). Initialized from an
existing *array or slice* instead (`mut v: int[] = arr;`, `= arr[1..]`), a
`T[]` is not sized by anything: it is a [view](#a-slice-shares-the-array-it-views).

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

A parameter written `T[]` (nothing inside the brackets) accepts an array of
**any** length. Inside the function it knows that length: for-each visits
every element, `arr.length()` is the caller's element count, and `arr[i]` is
bounds-checked against it (negative indices included).

```turflang
fn sum(arr: int[]): int {
    mut total: int = 0;
    for value in arr {
        total += value;
    }
    return total;
}

fn<T> display(arr: T[]): void {   // T is the element type
    for item in arr {
        printline(item);
    }
}

fn main(): int {
    mut a: int[3] = [1, 2, 3];
    mut b: int[10] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    sum(a);              // 6
    sum(b);              // 55 -- any length
    sum([4, 5]);         // 9  -- a literal works too
    display(["x", "y"]);
    return 0;
}
```

Any array expression can be passed: a variable, a literal, a call that
returns an array, a struct field, one row of a 2D array, or another `T[]`
parameter. Every call form takes one the same way: ordinary calls, UFCS
(`a.sum()`), methods, constructors and module functions.

Only the **outermost** dimension may be unsized. `int[][3]` is "any number of
rows of three", like C's `int arr[][3]`:

```turflang
fn sum_grid(rows: int[][3]): int {
    mut total: int = 0;
    for row in rows {        // row: int[3]
        for cell in row {
            total += cell;
        }
    }
    return total;
}
```

#### A `T[]` parameter shares the caller's array

Unlike a sized parameter (`arr: int[4]`, which receives a copy), a `T[]`
parameter works on the caller's array directly, so writing an element changes
the caller's array:

```turflang
fn fill(arr: int[], value: int): void {
    for (i, _old) in arr {
        arr[i] = value;
    }
}

mut a: int[3] = [1, 2, 3];
fill(a, 0);          // a is now [0, 0, 0]
```

An `immut` array cannot be passed to a function that writes through its
`T[]` parameter, whether it writes directly, by calling a mutating method on
an element, or by handing the array on to a function that does. That is
`E141`. A function that only reads the array can be called with an `immut`
one.

#### Where `T[]` may appear

A `T[]` is only safe for as long as the array it points into exists. That
holds inside one function, so a `T[]` may be a function parameter's type or
a **local variable's** type (a view, see [Range slicing](#range-slicing-arrab)).
A slice can never leave its function. Everywhere that could keep it longer
is `E140`: a struct field or error payload, a return type, an inner
dimension (`int[3][]`, also for a local `int[2][]`), behind `&`/`*`, a
generic type argument (`List<int[]>`), a function type (`fn(int[]): int`), a
cast target, an `extern fn` signature (C has no matching type; take a `T*`
and a length), and a generic call that would bind a type parameter `T` to
`int[]`. Declare the generic parameter as `T[]` instead.

### Range slicing: `arr[a..b]`

`arr[start..end]` is a **view** of the elements from `start` up to, but not
including, `end` (the same exclusive end as `0..n`). It is never a copy, so
taking one is O(1), and its type is `T[]`. Either bound may be left out:
`arr[start..]`, `arr[..end]` and `arr[..]` (the whole array).

```turflang
import io;

fn sum(arr: int[]): int {
    mut t: int = 0;
    for v in arr {
        t += v;
    }
    return t;
}

fn main(): int {
    immut nums: int[5] = [10, 20, 30, 40, 50];
    immut tail = nums[2..];          // [30, 40, 50]
    immut head = nums[..2];          // [10, 20]
    io::printline(head[1]);          // 20
    io::printline(tail.length());    // 3
    io::printline(sum(nums[1..4]));  // 90
    io::printline(nums[1..3][0]);    // 20 again, a slice expression can be indexed
    io::printline(nums[2..].sum());  // 120, and used with UFCS
    for v in nums[3..] {             // 40, 50
        io::printline(v);
    }
    return 0;
}
```

Any array can be sliced: a `T[N]` variable, field or row, a `T[]` parameter,
or another slice. A slice can be indexed, passed to a `T[]` parameter, used
in a for-each, and asked for its `.length()`. Slicing a 2D array slices its
rows: `grid[1..]` of an `int[3][3]` is an `int[][3]`.

#### Bounds

A negative bound counts from the end: `nums[-2..]` is the last two elements
and `nums[1..-1]` drops the first and the last. The length is added to each
negative bound, and the result must satisfy `0 <= start <= end <= length`.
`nums[5..]` on five elements and `nums[i..i]` are valid, **empty** slices;
`nums[4..2]` is an error, not an empty slice.

A range that is provably wrong is `E143` at compile time (constant bounds on
a `T[N]`; for a `T[]`, a constant start after a constant end of the same
sign). Any other bad range panics at run time with
`Slice range out of bounds for 'nums'` and the program aborts. Bounds must be
ints (`a[1.5..]` is `E002`).

#### A slice shares the array it views

Writing through a `mut` view changes the original array, and a `mut` view can
be pointed at another array later. An `immut` view is read-only (writing
through it is `E060`).

```turflang
mut arr: int[5] = [1, 2, 3, 4, 5];
mut view: int[] = arr[1..3];
view[0] = 20;                 // arr is [1, 20, 3, 4, 5]
view = arr[3..];              // rebound: the view now looks at arr[3] and arr[4]
view[1] = 50;                 // arr[4] is now 50

immut lits: int[] = [10, 20, 30];   // a literal sizes it: this is an int[3], not a view
immut whole: int[] = arr;            // an array initializer gives a view
immut again = whole[1..];            // and so does a slice of a view
```

A local `T[]` needs an initializer (`E128`). A slice is a view and not a
place, so `a[0..2] = b` is `E061`. A `mut` view of an `immut` array would let
you change the immutable array, so it is `E144`, both at declaration
(`mut t: int[] = a[1..];`) and when rebinding (`view = fixed[1..];`); declare
the view `immut` or make the array `mut`. Writing through a slice expression
of an immutable array (`a[1..][0] = 5`) is `E060`. Passing a slice of an
`immut` array to a function that writes through its `T[]` is `E141`, as
is writing through a local view of a parameter (`mut rest: int[] = arr[1..];
rest[0] = 0;`).

#### What can't be sliced, and what a slice can't become

Only arrays can be sliced. A `string` (use `strings::substring(s, start,
end)`), a `List<T>`, a number or a struct is `E142`.

A `T[]` can't become a `T[N]`, because its length is only known at run time:
`mut b: int[3] = view;` is `E003`, and passing a slice to an `int[3]`
parameter matches no overload (`E056`). Copy the elements in a loop instead.

### Copying: `arr.copy()`

A slice shares storage; `arr.copy()` is how you get your own. It returns a
new, independent `T[]` whose elements live in a fresh block on the GC heap
(see [the memory model](../architecture/memory_model.md)), so editing the
copy never changes the original, and the original's later edits don't reach
the copy. It works on any array or slice: a `T[N]`, a `T[]` parameter or
view, a range slice, a 2D array and an array of structs. Each element is
copied by value, exactly as assigning it would: a 2D array's rows are copied
along with it, but an element that *refers* to something (a `List`, a `&T`)
still refers to the same thing in the copy.

```turflang
import io;

fn main(): int {
    mut a: int[3] = [1, 2, 3];
    immut fixed: int[3] = [4, 5, 6];

    mut c = a[1..].copy();       // an independent int[] of 2 elements
    c[0] = 99;
    io::printline(a[1]);         // 2, unchanged
    io::printline(c[0]);         // 99

    mut e = fixed.copy();        // fine: a copy is not a view of 'fixed'
    e[2] = 0;
    io::printline(fixed[2]);     // 6
    io::printline(a.copy()[1..].length());   // 2, the result can be sliced
    return 0;
}
```

Because the copy is not a view, an `immut` source is fine: `mut c =
fixed.copy();` is not `E144`, and `fill(fixed.copy(), 0)` is not `E141`
(the writes land in the copy). The result can be indexed, sliced, used in a
for-each, asked for `.length()`, copied again, and passed to a `T[]`
parameter. An empty slice copies to an empty array (`a[i..i].copy()`).
Like any `T[]` it is a local-only value: it can't be returned or stored in a
struct (`E140`), and it can't initialize a `T[N]` (`E003`).

`copy()` takes no arguments (`a.copy(1)` is `E131`), and `length()` and
`copy()` are the only methods an array has (`a.reverse()` is `E131`). Strings
and `List<T>` have no `copy()`.

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
/ `l[i] = v` ([stdlib](stdlib/collections.md)), and to an unsized `T[]`
parameter, whose length is the caller's array's (checked at run time, even
for a constant index). It does **not** apply to a raw pointer (`T*`), which
carries no length to count back from — there `p[-1]` stays plain pointer
arithmetic and is unchecked.

Range-slice bounds follow the same rule: `a[-2..]` is the last two elements
(see [Range slicing](#range-slicing-arrab)).

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
