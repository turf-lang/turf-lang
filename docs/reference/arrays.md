# Arrays

An array is a fixed number of elements of one type, stored in a row. Its size is
part of its type: `int[5]` and `int[6]` are different types. An array is a
value: assigning it, or passing it to a function, copies it. For a collection
that grows, use [`List<T>`](stdlib/collections.md); [the last section](#arrays-and-listt)
compares the two.

## Declaring arrays and writing literals

An array type is written `T[N]`, where `N` is a positive integer literal. A
literal is a comma-separated list in square brackets:

```turflang
import io;

fn main(): int {
    mut numbers: int[5] = [0, 0, 0, 0, 0];
    numbers[0] = 10;
    numbers[1] = 20;

    immut words: string[2] = ["alpha", "beta"];
    mut ratios: double[3] = [0.5, 1.5, 2.5];
    io::printline(numbers[1] + 1);   // 21
    io::printline(words[1]);         // beta
    io::printline(ratios[2]);        // 2.5
    return 0;
}
```

The size is checked against the literal. Too few or too many elements is
`E003` ("Array size mismatch!"), an element of the wrong type is `E004`
("Array element type mismatch!"), and an array of one size cannot be assigned to
a variable of another (`E066`, "Type mismatch!"):

<!-- doc-example: fails E003 -->
```turflang
fn main(): int {
    mut a: int[3] = [1, 2];
    return a[0];
}
```

<!-- doc-example: fails E004 -->
```turflang
fn main(): int {
    mut a: int[3] = [1, 2, "three"];
    return a[0];
}
```

<!-- doc-example: fails E066 -->
```turflang
fn main(): int {
    mut a: int[3] = [1, 2, 3];
    mut b: int[4] = [1, 2, 3, 4];
    a = b;
    return 0;
}
```

An array size of zero is `E123`. Arrays cannot be compared with `==` (`E124`);
compare their elements in a loop.

### Letting the literal pick the size

The size can be left out when a literal initializes the declaration. `T[]` with
a literal takes the literal's length, and the variable is then exactly `T[N]`:
an owned array with value semantics and a fixed length. `immut x = [1, 2]`
infers the same way.

```turflang
fn main(): int {
    mut nums: int[] = [10, 20, 30];     // exactly int[3]
    immut guessed = [1, 2];             // int[2]
    mut g: int[][2] = [[1, 2], [3, 4]]; // only the outermost size is inferred: int[2][2]
    nums = [7, 8, 9];                   // fine: same length
    return nums[0] + guessed[0] + g[1][1];
}
```

Assigning a literal of another length to `nums` is `E066`. An explicit `T[N]`
is worth writing when the size is a promise: the compiler then rejects a
literal of any other length (`E003`). An empty literal has no size to give
(`mut a: int[] = [];` is `E123`), and an inner dimension cannot be left out
(`int[2][] = ...` is `E140`). Initialized from an existing *array or slice*
instead (`mut v: int[] = arr;`, `= arr[1..]`), a `T[]` is not sized by anything:
it is a [view](#a-slice-shares-the-array-it-views).

### Multidimensional arrays

Nest the size suffix for more dimensions: `int[2][3]` is 2 rows of 3 columns.
Literals nest to match, and a row can be assigned on its own:

```turflang
fn get_3d_array(): int[2][2][2] {
    return [
        [[1, 2], [3, 4]],
        [[5, 6], [7, 8]]
    ];
}

fn main(): int {
    mut arr: int[2][3] = [[1, 2, 3], [4, 5, 6]];
    arr = [[10, 20, 30], [40, 50, 60]];
    arr[0] = [100, 200, 300];           // one row
    return arr[0][2] + arr[1][1] + get_3d_array()[1][0][1];   // 300 + 50 + 6
}
```

## Initialization

A fixed-size array `T[N]` is assigned from its declaration. Declared without an
initializer, every element reads as the zero value of the element type: `0`,
`0.0`, `false`, `""` for a `string`, `null` for a `&?T`, a zero-filled struct,
and rows of zeros for a multidimensional array.

```turflang
import io;

fn main(): int {
    mut a: int[5];
    io::printline(a[3]);      // 0: nothing wrote it
    a[0] = 10;
    a[4] = 50;
    io::printline(a[2]);      // 0
    mut s: string[2];
    s[0] = "x";
    io::printline(s[1]);      // an empty line
    mut g: int[2][3];
    g[1][2] = 4;
    io::printline(g[0][0]);   // 0
    return 0;
}
```

Elements are not tracked one by one, so filling an array in a loop and then
reading it needs no special form. A scalar, a struct or a string is different:
reading one before it is definitely assigned is `E008`; see
[definite assignment](variables_and_types.md#definite-assignment).

An `immut` array needs an initializer, since it cannot be assigned afterwards.
So does an unsized local `T[]` (`E128`), which has no length to fill.

An element type that holds a non-null reference has no zero value. An array
of such elements must be initialized, or every element is `E128`; see
[arrays of references](#arrays-of-references-tn).

## Indexing

`a[i]` reads or writes one element; `i` is an `int` and the first element is
`a[0]`. A negative index counts back from the end, as in Python: `a[-1]` is the
last element and `a[-N]` the first of an `N`-element array. Compound assignment
works on an element.

```turflang
import io;

fn main(): int {
    mut a: int[4] = [10, 20, 30, 40];
    io::printline(a[-1]);   // 40, the same element as a[3]
    io::printline(a[-4]);   // 10, the same element as a[0]
    a[-1] = 41;             // writes a[3]
    a[-2] += 5;             // compound assignment works too
    io::printline(a[2]);    // 35
    return 0;
}
```

The valid range is `-N <= i < N`. It does not wrap: `a[4]` and `a[-5]` are both
out of bounds on four elements. Strings follow the same rule
([indexing a string](variables_and_types.md#indexing-a-string)) as do
`List<T>` elements ([`List` indexing](stdlib/collections.md#operatort-l-listt-i-int-t--operatort-l-listt-i-int-value-t-void)).
A raw pointer (`T*`) carries no length, so `p[-1]` there is plain pointer
arithmetic and is not checked.

## Bounds checking

Every array index is checked, at compile time when the compiler can prove the
index is wrong and otherwise at run time. See also
[Checks that panic](memory_and_safety.md#array-string-and-collection-indexing).

### Caught at compile time

A constant index out of range does not compile (`E001`, "Array index out of
bounds!"). The compiler follows constants through locals, so an index held in a
variable whose value is known is caught too, and so is one declared in a nested
block. This also holds for a sized `T[N]` parameter, and for each dimension of a
multidimensional array.

<!-- doc-example: fails E001 -->
```turflang
fn main(): int {
    mut arr: int[3] = [1, 2, 3];
    return arr[3];       // E001: 'arr' has 3 elements (indices 0 to 2, or -1 to -3)
}
```

<!-- doc-example: fails E001 -->
```turflang
fn main(): int {
    mut arr: int[3] = [10, 20, 30];
    mut i: int = 5;      // provably 5 at the use below
    return arr[i];       // E001
}
```

### Panics at run time

When the index is not a known constant (a loop counter, a function argument, a
call result), the compiler emits a check. A negative index first has the length
added to it; the result must satisfy `0 <= index < length`, so `idx < -N` fails
as well as `idx >= N`. On failure the program prints a panic and aborts:

```text
panic: runtime check: Array index out of bounds for 'arr' (prog.tr:7)
```

The name is the array being indexed, and the location is the index expression;
the process exits abnormally (signal `SIGABRT`).

```turflang
fn at(arr: int[3], i: int): int {
    return arr[i];
}

fn main(): int {
    mut a: int[3] = [1, 2, 3];
    return at(a, 3);     // panics: Array index out of bounds for 'arr'
}
```

Writes are checked the same way as reads. An index into an unsized `T[]`
parameter is always a run-time check, even for a constant, because the length
is the caller's.

## Arrays as parameters and return values

A parameter of type `T[N]` receives a **copy** of the caller's array: writing
to it inside the function does not change the caller's. A function can return
an array of any declared size, and the result can be indexed directly. A literal
may be passed where an array is expected.

```turflang
import io;

fn sum_array(arr: int[4]): int {
    arr[0] = 999;                // changes the function's own copy
    return arr[0] + arr[1] + arr[2] + arr[3];
}

fn make(): int[3] {
    mut r: int[3] = [7, 8, 9];
    return r;
}

fn main(): int {
    mut a: int[4] = [1, 2, 3, 4];
    io::printline(sum_array(a));         // 1008
    io::printline(a[0]);                 // 1: the caller's array is unchanged
    io::printline(sum_array([1, 2, 3, 4]));
    io::printline(make()[2]);            // 9
    mut copy = a;                        // assignment copies too
    copy[0] = 50;
    io::printline(a[0]);                 // 1
    return 0;
}
```

An array of structs works the same way, and a function can return one:

```turflang
import io;

struct Point {
    x: int,
    y: int
}

fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

fn get_points(): Point[2] {
    return [Point::new(10, 20), Point::new(30, 40)];
}

fn main(): int {
    mut points: Point[2] = [Point::new(1, 2), Point::new(3, 4)];
    points[1].y = 40;
    io::printline(points[0].x);          // 1
    io::printline(points[1].y);          // 40
    io::printline(get_points()[1].x);    // 30: indexing a call result
    return 0;
}
```

A struct may also have an array field (`struct S { a: int[3] }`); it is copied
along with the struct. The struct may not contain itself by value through its
array fields ([self-referential structs](structs_and_enums.md#self-referential-structs)).

### Unsized parameters: `T[]`

A parameter written `T[]` (nothing inside the brackets) accepts an array of
**any** length. Inside the function it knows that length: for-each visits every
element, `arr.length()` is the caller's element count, and `arr[i]` is
bounds-checked against it (negative indices included).

```turflang
import io;

fn sum(arr: int[]): int {
    mut total: int = 0;
    for value in arr {
        total += value;
    }
    return total;
}

fn<T> display(arr: T[]): void {   // T is the element type
    for item in arr {
        io::printline(item);
    }
}

fn main(): int {
    mut a: int[3] = [1, 2, 3];
    mut b: int[10] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    io::printline(sum(a));        // 6
    io::printline(sum(b));        // 55: any length
    io::printline(sum([4, 5]));   // 9: a literal works too
    io::printline(a.sum());       // 6: UFCS, with your own function
    display(["x", "y"]);
    return 0;
}
```

Any array expression can be passed: a variable, a literal, a call that returns
an array, a struct field, one row of a 2D array, or another `T[]` parameter.
Every call form takes one the same way: ordinary calls, UFCS, methods,
constructors and module functions. A function may have both a `T[N]` and a
`T[]` overload.

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

fn main(): int {
    mut g: int[2][3] = [[1, 2, 3], [4, 5, 6]];
    return sum_grid(g);
}
```

#### A `T[]` parameter shares the caller's array

Unlike a sized parameter, a `T[]` parameter works on the caller's array
directly, so writing an element changes the caller's array:

```turflang
fn fill(arr: int[], value: int): void {
    for (i, _old) in arr {
        arr[i] = value;
    }
}

fn main(): int {
    mut a: int[3] = [1, 2, 3];
    fill(a, 0);          // a is now [0, 0, 0]
    return a[1];
}
```

An `immut` array cannot be passed to a function that writes through its `T[]`
parameter, whether it writes directly, by calling a mutating method on an
element, or by handing the array on to a function that does. That is `E141`.
A function that only reads the array can be called with an `immut` one.

<!-- doc-example: fails E141 -->
```turflang
fn fill(arr: int[]): void {
    arr[0] = 1;
}

fn main(): int {
    immut a: int[2] = [1, 2];
    fill(a);
    return 0;
}
```

#### Where `T[]` may appear

A `T[]` is only safe for as long as the array it points into exists. That holds
inside one function, so a `T[]` may be a function parameter's type or a
**local variable's** type (a view, see [Range slicing](#range-slicing-arrab)).
A slice can never leave its function. Everywhere that could keep it longer
is `E140`: a struct field or error payload, a return type, an inner dimension
(`int[3][]`, also for a local `int[2][]`), behind `&` or `*`, a generic type
argument (`List<int[]>`), a function type (`fn(int[]): int`), a cast target, an
`extern fn` signature (C has no matching type; take a `T*` and a length), and a
generic call that would bind a type parameter `T` to `int[]`. Declare the
generic parameter as `T[]` instead, as `display` does above.

<!-- doc-example: fails E140 -->
```turflang
fn make(): int[] {
    mut a: int[2] = [1, 2];
    return a;
}

fn main(): int {
    return 0;
}
```

Return a sized array (`int[2]`) or a `List<int>` instead.

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
    io::printline(nums[1..3][0]);    // 20: a slice expression can be indexed
    io::printline(nums[2..].sum());  // 120: and used with UFCS
    for v in nums[3..] {             // 40, 50
        io::printline(v);
    }
    return 0;
}
```

Any array can be sliced: a `T[N]` variable, field or row, a `T[]` parameter, or
another slice. A slice can be indexed, passed to a `T[]` parameter, used in a
for-each, and asked for its `.length()`. Slicing a 2D array slices its rows:
`grid[1..]` of an `int[3][3]` is an `int[][3]`.

#### Bounds

A negative bound counts from the end: `nums[-2..]` is the last two elements and
`nums[1..-1]` drops the first and the last. The length is added to each negative
bound, and the result must satisfy `0 <= start <= end <= length`. `nums[5..]` on
five elements and `nums[i..i]` are valid, **empty** slices; `nums[4..2]` is an
error, not an empty slice.

A range that is provably wrong is `E143` ("Slice range out of bounds!") at
compile time: constant bounds on a `T[N]`, or for a `T[]` a constant start after
a constant end of the same sign. Any other bad range panics at run time with
`Slice range out of bounds for 'nums'` and the program aborts. Bounds must be
ints (`a[1.5..]` is `E002`).

<!-- doc-example: fails E143 -->
```turflang
fn main(): int {
    mut a: int[3] = [1, 2, 3];
    immut v = a[2..5];
    return v.length();
}
```

#### A slice shares the array it views

Writing through a `mut` view changes the original array, and a `mut` view can
be pointed at another array later. An `immut` view is read-only (writing
through it is `E060`).

```turflang
fn main(): int {
    mut arr: int[5] = [1, 2, 3, 4, 5];
    mut view: int[] = arr[1..3];
    view[0] = 20;                 // arr is [1, 20, 3, 4, 5]
    view = arr[3..];              // rebound: the view now looks at arr[3] and arr[4]
    view[1] = 50;                 // arr[4] is now 50

    immut lits: int[] = [10, 20, 30];   // a literal sizes it: this is an int[3], not a view
    immut whole: int[] = arr;           // an array initializer gives a view
    immut again = whole[1..];           // and so does a slice of a view
    return arr[4] + lits[0] + again.length();
}
```

A local `T[]` needs an initializer (`E128`). A slice is a view and not a place,
so `a[0..2] = b` is `E061`. A `mut` view of an `immut` array would let you
change the immutable array, so it is `E144`, both at declaration
(`mut t: int[] = a[1..];`) and when rebinding (`view = fixed[1..];`); declare
the view `immut` or make the array `mut`. Writing through a slice expression of
an immutable array (`a[1..][0] = 5`) is `E060`. Passing a slice of an `immut`
array to a function that writes through its `T[]` is `E141`, as is writing
through a local view of a parameter (`mut rest: int[] = arr[1..]; rest[0] = 0;`).

<!-- doc-example: fails E144 -->
```turflang
fn main(): int {
    immut a: int[3] = [1, 2, 3];
    mut v: int[] = a[1..];
    return v.length();
}
```

#### What can't be sliced, and what a slice can't become

Only arrays can be sliced. A `string` (use `strings::substring(s, start, end)`),
a `List<T>`, a number or a struct is `E142`.

A `T[]` cannot become a `T[N]`, because its length is only known at run time:
`mut b: int[3] = view;` is `E003`, and passing a slice to an `int[3]` parameter
matches no overload (`E056`). Copy the elements in a loop instead.

### Copying: `arr.copy()`

A slice shares storage; `arr.copy()` is how you get your own. It returns a new,
independent `T[]` whose elements live in a fresh block on the GC heap (see
[Memory and Safety](memory_and_safety.md)), so editing the copy never changes
the original, and the original's later edits do not reach the copy. It works on
any array or slice: a `T[N]`, a `T[]` parameter or view, a range slice, a 2D
array and an array of structs. Each element is copied by value, exactly as
assigning it would: a 2D array's rows are copied along with it, but an element
that *refers* to something (a `List`, a `&T`) still refers to the same thing in
the copy.

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
    io::printline(a.copy()[1..].length());   // 2: the result can be sliced
    return 0;
}
```

Because the copy is not a view, an `immut` source is fine: `mut c = fixed.copy();`
is not `E144`, and `fill(fixed.copy(), 0)` is not `E141` (the writes land in the
copy). The result can be indexed, sliced, used in a for-each, asked for
`.length()`, copied again, and passed to a `T[]` parameter. An empty slice copies
to an empty array (`a[i..i].copy()`). Like any `T[]` it is a local-only value: it
cannot be returned or stored in a struct (`E140`), and it cannot initialize a
`T[N]` (`E003`).

`copy()` takes no arguments (`a.copy(1)` is `E131`), and `length()` and `copy()`
are the only methods an array has (`a.reverse()` is `E131`). Strings and
`List<T>` have no `copy()` in this sense; a `List` has its own
[`List.copy()`](stdlib/collections.md#listcopy-listt).

## Iterating with `for`

A for-each loop visits the elements in order; the `(index, value)` form also
binds the position. It works on any array expression: a `T[N]`, a `T[]`
parameter, a slice, a literal, a call result, and on a multidimensional array
each row is itself an array. See
[`for`: for-each form](control_flow.md#for-for-each-form) for the full rules.

```turflang
import io;

fn main(): int {
    mut a: int[4] = [10, 20, 30, 40];
    for v in a {
        io::printline(v);
    }
    for (i, v) in a {
        io::printline(i * 100 + v);      // 10, 120, 230, 340
    }

    mut m: int[2][3] = [[1, 2, 3], [4, 5, 6]];
    mut total: int = 0;
    for row in m {                        // row: int[3]
        for cell in row {
            total += cell;
        }
    }
    io::printline(total);                 // 21

    for i in 0..a.length() step i++ {     // the range form, when you need a custom stride
        a[i] = a[i] + 1;
    }
    return 0;
}
```

The loop variable is a copy of the element: assigning to it, or to a field of a
struct element, does not change the array. Write through the index instead
(`a[i] = ...`). A `List<T>`, a map, a struct or a scalar cannot be iterated with
for-each (`E005`, "for-each requires an array!"); iterate a list by index with a
range `for` and `l[i]`.

## Arrays of references: `(&T)[N]`

`&` takes the whole type after it, so `&Node[2]` is a *reference to* an array of
two nodes. Indexing one, `r[i]` with `r: &int[3]`, indexes the array it refers
to, with the array's bounds check. Any other `&T` indexes as one element, so
`r[0]` is the value it refers to and any other index is out of bounds (`E001`,
or a panic at run time). A `&?T` is checked for null and converted to a `&T`
before it is indexed (`E126`). An array *of* references puts the reference
type in parentheses:

```turflang
import io;

struct Node { val: int, next: &?Node }

fn make(v: int): &Node {
    mut n: Node = Node::default();
    n.val = v;
    return heap(n);
}

fn total(nodes: (&Node)[]): int {
    mut t: int = 0;
    for n in nodes {
        t = t + n.val;
    }
    return t;
}

fn main(): int {
    mut pair: (&Node)[2] = [make(1), make(2)];
    pair[1] = make(5);
    io::printline(total(pair));  // 6

    mut slots: (&?Node)[3];      // every element starts null
    slots[0] = make(7);
    mut first: &?Node = slots[0];
    if (first != null) {
        io::printline(first.val);  // 7
    }
    return 0;
}
```

- A `(&T)[N]` must be initialized (`E128`): a reference cannot be null, and an
  element nothing wrote would be. The same holds for an array whose element
  type holds a `&U` anywhere inside it. A `(&?T)[N]` needs no initializer; its
  elements start as `null`, and an element is checked like any `&?T` that is
  not a variable (store it in one, then check it).
- An array literal whose elements are all `null`, or all `&N` for a node `N`,
  takes the type `(&?N)[n]` wherever that type is expected: a declaration, an
  assignment (`slots = [null, null];`), a function argument, a `return` or a
  struct field assignment. A view or another variable keeps its exact element
  type.
- Copying the array copies the references, not the nodes they point to.
- `(&T)[]` is a slice parameter like any `T[]`.
- Parentheses are only for this and for an array of function values,
  `(fn(int): int)[2]`: `(int)[2]`, and `(&T)` with no `[` after them, are
  `E109`.

## Escape rules

A local array lives in its function's frame, like any local. Its elements can be
returned, copied or passed down freely, because an array is a value. What cannot
leave the function is a *reference into* the array's storage. The same
[escape rules](memory_and_safety.md#4-escape-rules) as for any local apply, and
a violation is `E086`:

- `&a[0]` of a local array points into the frame and cannot be returned, stored
  into a struct that outlives the call, or pushed into a collection.
- An array of references that holds a reference to a local (`(&N)[2]` with an
  element `&n`) cannot be returned, for the same reason; nor can a reference
  to a local be stored into an element of a `(&N)[]` parameter, since the
  caller's array would be left pointing at a dead frame.

<!-- doc-example: fails E086 -->
```turflang
struct N { val: int }

fn make(): &N {
    mut a: N[2] = [N::default(), N::default()];
    a[0].val = 42;
    return &a[0];            // E086: points into this frame
}

fn main(): int {
    return make().val;
}
```

Copy the value onto the GC heap to hand it back (`return heap(a[0]);`, see
[`heap`](memory_and_safety.md#heapexpr-a-value-on-the-gc-heap)), or return the
element itself (`return a[0];`). A reference you *received* as a parameter,
including one into a `T[]` parameter's element, is fine to return:

```turflang
struct N { val: int }

fn first(a: N[]): &N {
    return &a[0];            // points into the caller's array
}

fn main(): int {
    mut a: N[2] = [N::default(), N::default()];
    mut r: &N = first(a);
    r.val = 3;
    return a[0].val;         // 3
}
```

A `T[]` slice cannot itself escape: it is only a parameter or local type
(`E140` anywhere else, see [Where `T[]` may appear](#where-t-may-appear)).

A generic function can take or return a `T[N]` of any length by declaring `N`
as a [size parameter](generics_and_operators.md#size-parameters):
`fn<T, N: size> reversed(a: T[N]): T[N]`. Each call fixes `N`, from an array
argument or from the type the result is stored into.

## The `array` module

The `array` module is part of the prelude: always in scope, with no import. It
provides helpers for arrays of `int`, `double` and `string`, called as UFCS
methods (`arr.sum(n)`) or qualified (`array::sum(arr, n)`). There is one
overload per element type, not a generic version; for another element type
write your own `fn<T>` over `T[]`, or use a [`List<T>`](stdlib/collections.md).

Every function takes the number of elements to look at, `len`, after the array,
and works on `arr[0..len)`. Pass `arr.length()` to cover the whole array. A
`len` past the end of the array panics with `Array index out of bounds for
'arr'`. The full reference is in [Core: `array`](stdlib/core.md#array).

| Function | Element types | Result |
|---|---|---|
| `sum(arr, len)` | `int[]`, `double[]` | the sum of the first `len` elements |
| `max(arr, len)` | `int[]`, `double[]` | the largest of the first `len` elements |
| `min(arr, len)` | `int[]`, `double[]` | the smallest of the first `len` elements |
| `contains(arr, len, val)` | `int[]`, `double[]`, `string[]` | `bool`: is `val` among the first `len` |
| `index_of(arr, len, val)` | `int[]`, `double[]`, `string[]` | the index of the first match, or `-1` |

`max` and `min` start from `arr[0]` without checking `len`, so an empty range
panics with the same index message.

```turflang
import io;

fn main(): int {
    mut a: int[5] = [3, 1, 4, 1, 5];
    mut d: double[3] = [1.5, 0.5, 2.5];
    mut s: string[2] = ["x", "y"];

    io::printline(a.sum(5));                  // 14
    io::printline(array::sum(a, a.length())); // 14
    io::printline(a.max(5));                  // 5
    io::printline(a.min(5));                  // 1
    io::printline(a[1..].sum(4));             // 11: works on a slice
    io::printline(d.max(3));                  // 2.5
    if s.contains(2, "y") {
        io::printline(s.index_of(2, "y"));    // 1
    }
    io::printline(a.index_of(5, 7));          // -1
    return 0;
}
```

To build a whole array rather than look at one, the [`arrays`
module](stdlib/arrays.md) (`import arrays;`) has `fill`, `generate`, `reversed`
and `map`: `mut zeros: int[1000] = arrays::fill(0);`.

## Arrays and `List<T>`

| | `T[N]` | `List<T>` |
|---|---|---|
| Length | fixed, part of the type | grows with `push` |
| Assignment | copies the elements | shares the same list (`l.copy()` for a copy) |
| Storage | in place, in the frame or the struct | a handle to GC-heap storage |
| Index | `a[i]`, negative from the end, checked | `l[i]`, negative from the end, checked |
| Initial value | zero-filled when declared without a value | `List::default()` |
| Iteration | `for v in a` | by index: `for i in 0..l.length() step i++` |
| Element types | any | any |

Use an array when the size is known and small, or when you want value
semantics; use a `List` when the size changes or the collection is shared.
They convert by copying the elements:

```turflang
import io;
import list::List;

fn main(): int {
    immut src: int[3] = [4, 5, 6];
    mut l: List<int> = List::default();
    for v in src {                         // array into list
        l.push(v);
    }
    l.push(7);

    mut out: int[3] = [0, 0, 0];
    for i in 0..3 step i++ {               // list into array
        out[i] = l[i];
    }
    io::printline(out[2]);                 // 6
    io::printline(l.length());             // 4
    return 0;
}
```

A `List<int[2]>` (a list of fixed-size arrays) is fine; a `List<int[]>` is not
(`E140`). The collections documentation covers the rest:
[Collections](stdlib/collections.md).
