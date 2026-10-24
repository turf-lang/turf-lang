# Functions and Modules

This page covers how to declare and call functions, how to pass functions
around as values, uniform function call syntax (UFCS), annotations, and the
module system: `import`, `::`, the prelude, visibility (`pub`) and the way a
type belongs to the module that declares it.

Packages (a directory with a `turf.toml`, dependencies, `turf build`) are in
[Projects and packages](projects_and_packages.md). For how the compiler
implements modules, see
[`docs/architecture/modules_and_scope_resolution.md`](../architecture/modules_and_scope_resolution.md).

---

## 1. Functions

### Declaring a function

```turflang
fn add(a: int, b: int): int {
    return a + b;
}

fn greet(name: string): void {
    printline("hello, " + name);
}

fn main(): int {
    greet("turf");
    printline(add(2, 3));
    return 0;
}
```

The shape is `fn name(param: Type, ...): ReturnType { body }`.

- The return type is always written. A function that returns nothing says
  `void`; leaving the `: type` out is `E106` ("Expected ':' followed by return
  type after parameter list").
- A function whose return type is not `void` must return on every path
  (`E009`: "The function 'g' promises to give back a result, but sometimes it
  doesn't!"). A returned value must match the declared type (`E064`).
- A parameter's type can never be `void` (`E112`), and two parameters cannot
  share a name (`E033`).
- A function is declared at the top level of a file. A function inside a
  function body is `E038` ("Nested function definitions are not allowed").
- Order does not matter: a function can call one declared further down the
  file, and two functions can call each other.
- `main` takes no parameters and returns `int` (the exit status) or `void`
  (exit status 0).
- A function that can fail returns `!T`; see [Error handling](error_handling.md).
- A function can be generic, `fn<T> ident(x: T): T`; see
  [Generics and operators](generics_and_operators.md).

A call that passes the wrong number of arguments is `E044`; one whose argument
types match nothing is `E056` ("No matching overload!"); calling a name that is
not a function is `E041`.

### Parameters: by value and by reference

A parameter is a **copy** of the argument. Assigning to the parameter, or to a
field of a struct or an element of an array it holds, changes the copy and
never the caller's variable. This holds for numbers, strings, structs and
sized arrays alike:

```turflang
struct P { x: int }

fn bump_int(n: int): int { n = n + 1; return n; }
fn bump_struct(p: P): void { p.x = p.x + 1; }
fn bump_array(a: int[3]): void { a[0] = 99; }

fn main(): int {
    mut n: int = 1;
    bump_int(n);
    printline(n);      // 1

    mut p: P = P::default();
    bump_struct(p);
    printline(p.x);    // 0

    mut a: int[3] = [1, 2, 3];
    bump_array(a);
    printline(a[0]);   // 1
    return 0;
}
```

To let a function change the caller's value, take a **reference**, `&T`, and
pass the address of the variable with `&`:

```turflang
struct P { x: int }

fn bump_ref(p: &P): void { p.x = p.x + 1; }

fn main(): int {
    mut p: P = P::default();
    bump_ref(&p);
    bump_ref(&p);
    printline(p.x);    // 2
    return 0;
}
```

`bump_ref(p)` without the `&` is `E056`: a `P` is not a `&P`. A `&T` can never
be null; a `&?T` parameter may be, and the body must check it before use:

```turflang
struct P { x: int }

fn show(p: &?P): int {
    if p == null { return -1; }
    return p.x;
}

fn main(): int {
    mut p: P = P::default();
    p.x = 7;
    printline(show(&p));    // 7
    printline(show(null));  // -1
    return 0;
}
```

An **unsized array parameter**, `a: int[]`, is a view of the caller's array of
any length: it is not a copy, and writes through it reach the caller. Arrays
are covered in [Arrays](arrays.md); references in
[FFI, `unsafe` and pointers](ffi_and_unsafe.md).

Numeric widening applies to arguments and return values: an `int` is accepted
where a `double` is expected (`to_double(42)`), but never the other way round.

### Methods

A function declared with a `.` between a struct name and a method name is a
method of that struct:

```turflang
struct Counter { n: int }

fn Counter.new(start: int): void { self.n = start; }
fn Counter.bump(by: int): void { self.n += by; }
fn Counter.get(): int { return self.n; }
fn Counter.zero() static: Counter { return Counter::default(); }

fn main(): int {
    mut c: Counter = Counter::new(5);
    c.bump(2);
    printline(c.get());              // 7
    mut z: Counter = Counter::zero();
    printline(z.get());              // 0
    return 0;
}
```

- An **instance method** gets `self` implicitly and is called with `.` on a
  value: `c.bump(2)`. Inside it, `self.field` reads and writes the receiver.
- A **static method** puts `static` after the parameter list, before the
  return type, has no `self`, and is called as `Counter::zero()`. Calling it
  through a value is `E052`.
- `new` and `default` are the construction names. `Counter::default()`
  zero-fills; `Counter::new(args)` runs the method named `new` (without a
  `fn T.new`, `T::new(...)` is `E050`). `new` is never `static`, and `default`
  cannot be declared. There is no brace-literal form.

More on structs: [Structs and enums](structs_and_enums.md).

### Overloading

Several functions may share a name when their parameter lists differ, in
number or in type:

```turflang
fn show(x: int): string { return "int"; }
fn show(x: double): string { return "double"; }
fn show(x: string): string { return "string"; }
fn show(x: int, y: int): string { return "two ints"; }

fn main(): int {
    printline(show(1));        // int
    printline(show(1.5));      // double
    printline(show("s"));      // string
    printline(show(1, 2));     // two ints
    return 0;
}
```

How a call picks its overload:

1. Only overloads with the right number of parameters are considered
   (`E044` if there are none).
2. Of those, the ones whose parameter types accept the argument types are
   kept, with the ordinary widening rules (an `int` argument fits a `double`
   parameter). If none is left, the call is `E056`, and the message lists
   every candidate.
3. If an overload matches every argument exactly, it wins over one that needs
   a widening. Otherwise, the first of the remaining overloads in the file
   wins.

So with `fn only(x: double)` alone, `only(3)` is fine; with both `show(int)` and
`show(double)`, `show(3)` is the `int` one. Two functions with the same name
and the same parameter types are `E011` ("Duplicate function definition"),
whatever their return types.

The **return type is not part of the choice**; the compiler never picks an
overload from the type the result is assigned to.

Operators overload the same way and have their own rules:
[Generics and operators](generics_and_operators.md#operator-overloading).

---

## 2. First-class functions

A function is a value. Its type is written `fn(T1, T2): R`: parameter types
only, no names, and the `: R` is required.

```turflang
fn apply(f: fn(int): int, x: int): int { return f(x); }
fn double_it(n: int): int { return n * 2; }
fn triple_it(n: int): int { return n * 3; }

fn add(a: int, b: int): int { return a + b; }
fn sub(a: int, b: int): int { return a - b; }

fn choose(use_add: bool): fn(int, int): int {
    if use_add { return add; }
    return sub;
}

struct Op { f: fn(int, int): int }

fn main(): int {
    printline(apply(double_it, 5));      // 10

    mut f: fn(int): int = double_it;     // stored in a variable
    f = triple_it;                       // and reassigned
    printline(f(3));                     // 9

    mut op: fn(int, int): int = choose(true);
    printline(op(3, 4));                 // 7

    mut o: Op = Op::default();           // stored in a struct field
    o.f = sub;
    immut h: fn(int, int): int = o.f;
    printline(h(9, 4));                  // 5
    return 0;
}
```

Two function types are the same type when their parameter types and return
type are the same. Assigning a function of another shape is a type error
(`E066`), as is passing one where a different shape is expected (`E056`), and
calling a function value with the wrong number of arguments is `E044`.

An array of function values puts the function type in parentheses, since in
`fn(int): int[2]` the `[2]` belongs to the return type. An element is called
directly, and so is any other expression of function type, such as a call
that returns a function (`choose(true)(3, 4)`):

```turflang
fn add(a: int, b: int): int { return a + b; }
fn mul(a: int, b: int): int { return a * b; }

fn main(): int {
    mut ops: (fn(int, int): int)[2] = [add, mul];
    for f in ops {
        printline(f(3, 4));              // 7, then 12
    }
    printline(ops[1](2, 5));             // 10
    return 0;
}
```

A `reduce` is the usual example of a higher-order function:

```turflang
fn reduce(arr: int[5], f: fn(int, int): int, init: int): int {
    mut acc: int = init;
    for i in 0..5 step i += 1 {
        acc = f(acc, arr[i]);
    }
    return acc;
}

fn add(a: int, b: int): int { return a + b; }
fn mul(a: int, b: int): int { return a * b; }

fn main(): int {
    mut nums: int[5] = [1, 2, 3, 4, 5];
    printline(reduce(nums, add, 0));   // 15
    printline(reduce(nums, mul, 1));   // 120
    return 0;
}
```

What can be used as a value:

- **A function by its plain name**, when that name is visible to a bare call
  (declared in this file, brought in by `import m::name;`, or a prelude
  function such as `sqrt`, which comes last as it does for a call). The variable's
  function type can be written or inferred: `mut g = double_it;` gives `g` the
  type `fn(int): int`.
- **Not an overloaded name.** If more than one overload has the name, there is
  no single signature to take, and it is `E125` ("'combine' is overloaded"):

  <!-- doc-example: fails E125 -->
  ```turflang
  fn combine(a: int, b: int): int { return a + b; }
  fn combine(a: double, b: double): double { return a - b; }

  fn main(): int {
      mut f: fn(int, int): int = combine;
      return 0;
  }
  ```

- **A generic function where a function type is expected.** Its type
  arguments come from that type: a declared variable, an assignment, a return
  type, or a parameter of function type. Each type gets its own copy, the same
  one a call at that type uses:

  ```turflang
  fn<T> id(x: T): T { return x; }
  fn apply(f: fn(int): int, x: int): int { return f(x); }

  fn main(): int {
      immut fi: fn(int): int = id;
      immut fs: fn(string): string = id;
      printline(fs("hi"));
      return apply(id, fi(0));
  }
  ```

  With nothing to take them from (`mut g = id;`) it is `E137`, and a type no
  instantiation fits (`fn(int): string = id`) is `E066`.
- **Not a `@caller_location` function.** It needs a location that only a
  direct call can pass (`E137`); write a plain function that calls it and use
  that.
- **Not a qualified name.** `io::printline` and `MathUtil::square` can be
  called but not used as values (`E137`, "Not a value!"). Wrap the call in a
  function of your own, or import the plain name:

  <!-- doc-example: fails E137 -->
  ```turflang
  struct MathUtil {}
  fn MathUtil.square(x: int) static: int { return x * x; }

  fn main(): int {
      mut g = MathUtil::square;
      return 0;
  }
  ```

  ```turflang
  import math::sqrt;

  fn main(): int {
      immut f: fn(double): double = sqrt;
      printline(f(16.0));   // 4
      return 0;
  }
  ```

---

## 3. UFCS: calling a function as a method

`receiver.name(args)` calls the free function `name(receiver, args)` when the
receiver has no method of that name. It is how `int`, `double`, `bool`,
`char`, `string` and arrays, which have no methods of their own, get methods
(`n.is_even()`, `s.length()`), and you can use it with your own functions:

```turflang
struct User { tag: int }

fn User.print(): int { return 1; }                   // a method
fn print(u: User, verbose: bool): int { return 2; }  // a free function

fn double_it(x: int): int { return x * 2; }
fn is_long(s: string): bool { return s.length() > 3; }
fn scale(x: int, factor: double): double { return x * factor; }
fn sum(a: int[]): int {
    mut t: int = 0;
    for v in a { t += v; }
    return t;
}

fn main(): int {
    mut u: User = User::default();
    printline(u.print());            // 1: the method wins
    printline(u.print(true));        // 2: no method takes an argument, so UFCS

    mut n: int = 21;
    printline(n.double_it());        // 42
    printline(21.double_it().double_it());   // 84: calls chain
    printline("hello".is_long());    // true
    printline(n.scale(2));           // 42: the 2 widens to double

    mut a: int[4] = [1, 2, 3, 4];
    printline(a.sum());              // 10: an `int[]` parameter takes any length
    return 0;
}
```

How the call is resolved:

1. **A method first.** If the receiver's type has an instance method of that
   name taking that many arguments, it is called, and UFCS is not considered.
2. **Then the free functions.** Every free function named `name` in the
   program whose first parameter is the receiver's type is a candidate,
   provided the function is visible to the caller (it is `pub`, or in the
   caller's own module; section 8). This is wider than a bare call: the
   function does not have to be imported by the calling file, only part of
   the program, because dot syntax follows the receiver's type rather than the
   file's imports. This is how the prelude's `char`, `strings` and other
   modules reach every file.

The two sides of a UFCS call are matched differently:

- **The receiver must match exactly.** A `char` receiver does not reach a
  function declared for `int`, though `char` converts to `int` elsewhere. An
  unsized array parameter (`a: int[]`) is the exception: it takes an array of
  any length. A raw pointer receiver is matched as the pointer it is, never
  as what it points to.
- **The other arguments convert as in any call**, so `n.scale(2)` widens `2`.

Failures: no candidate for the receiver is `E056` ("No visible 'describe'
overload accepts a 'char' receiver via UFCS"):

<!-- doc-example: fails E056 -->
```turflang
fn describe(x: int): string { return "int"; }

fn main(): int {
    mut c: char = 'a';
    printline(c.describe());
    return 0;
}
```

If two candidates from different modules match exactly and are equally good, the call is
`E125` ("Ambiguous UFCS call"); name the module's function with `::` to choose.
One tie is broken for you: when the exact matches include one declared in the
receiver type's own prelude module (`double` for a `double`, `strings` for a
`string`), that one wins.

---

## 4. Annotations

An annotation is a line written before a function declaration that states a
fact about that function to the compiler:

```text
@name
@name(argument, key: argument, ...)
```

An argument is a word (a keyword counts as a word) or a string; a key is a
word. Anything richer, such as `@ownership(1 + 2)`, is a syntax error.
Several annotations may be stacked, and they come **before** `pub`:

```text
@ownership(acquires)
pub extern fn my_alloc(size: int): void*;
```

`pub @ownership(...)` is `E087`. An annotation can only precede a `fn` or an
`extern fn`; before anything else it is `E110` ("Expected 'fn' or 'extern fn'
after an annotation"). A name the compiler does not know, or an argument that
does not fit the declaration, is `E146`; a misspelling gets a "Did you mean"
suggestion. An annotation is never silently ignored.

The annotations are `@caller_location`, `@ownership`, `@ffi` and `@deprecated`.

### `@caller_location`

Inside a function marked `@caller_location`, the keyword `caller_location` is
a `string` holding the file and line of the call that reached it, as
`file.tr:12`. It is how a library function reports a misuse at the user's line
rather than inside the library (the standard library's `List.pop` on an empty
list names the line that called `pop`).

```turflang
@caller_location
fn where(): string { return caller_location; }

@caller_location
fn check(ok: bool): void {
    if !ok { printline("failed at " + caller_location); }
}

fn main(): int {
    printline(where());   // the line of this call
    check(false);         // failed at <file>:<line of this call>
    return 0;
}
```

- A function marked `@caller_location` that calls another marked function
  *forwards* the original caller's location rather than naming its own call
  site. A wrapper that is not marked reports its own line.
- It works for a call written as a method (`s.where(1)`), and `@caller_location`
  may also precede an `operator` declaration; the location is then the line of
  the expression that used the operator.
- Using `caller_location` in a function that is not marked is `E084`:

  <!-- doc-example: fails E084 -->
  ```turflang
  fn not_marked(): string {
      return caller_location;
  }

  fn main(): int {
      printline(not_marked());
      return 0;
  }
  ```

- It may precede only `fn` and `operator`, not `extern fn` or a type
  (`E110`).

### `@ownership`

`@ownership` tells the memory-safety checks which functions allocate and which
release raw memory. The double-free and use-after-free check recognises an
allocator only through this annotation, never through a function's name:
an unannotated `extern fn free` is not tracked.

- `@ownership(acquires)` says the function returns a fresh allocation. The
  return type must be a pointer.
- `@ownership(releases: p)` says a call frees what its raw-pointer parameter
  `p` holds. `p` must be a parameter of the function and a raw pointer, not a
  `&T` (the garbage collector owns those).

The standard library's `mem` module declares its allocators this way, and you
declare your own the same way:

```turflang
@ownership(acquires)
@ffi(linux: malloc)
@ffi(macos: malloc)
@ffi(windows: malloc)
extern fn arena_take(size: int): int*;

@ownership(releases: block)
@ffi(linux: free)
@ffi(macos: free)
@ffi(windows: free)
extern fn arena_give_back(block: int*): void;

fn main(): int {
    unsafe {
        mut buf: int* = arena_take(sizeof(int) * 4);
        buf[0] = 1;
        printline(buf[0]);   // 1
        arena_give_back(buf);
    }
    return 0;
}
```

(The `@ffi` lines, described next, bind these names to C's `malloc` and
`free`; the checks care only about the `@ownership` lines.) Releasing `buf` twice is then `E017` ("Double free detected for 'buf'"), and
using it after the release is `E018`. A release on only some paths leaves a
warning, "may already have been freed", instead of an error. `@ownership`
cannot yet be put on an instance method (`E146`). See
[FFI, `unsafe` and pointers](ffi_and_unsafe.md) for `extern fn` and
`unsafe`.

### `@ffi`

`@ffi(platform: symbol)` goes on an `extern fn` only. It says that on that
platform the C symbol the declaration binds to has a different name. The
platforms are `linux`, `macos` and `windows`, the one named is the platform
being compiled for (`--target`, otherwise the host), and the symbol is a plain
word, not a string. A platform with no entry binds to the declared name.

```turflang
@ffi(linux: strlen)
@ffi(macos: strlen)
@ffi(windows: strlen)
extern fn text_length(s: string): int;

fn main(): int {
    printline(text_length("hello"));   // 5
    return 0;
}
```

Each platform may be named once for a declaration (twice is `E146`); an
unknown platform, a positional argument and `@ffi` on a function with a body
are all `E146`.

### `@deprecated`

`@deprecated("message")` marks a `fn` or `extern fn` obsolete. Every call to
it then warns (`E172`) with that exact message, naming what to use instead;
the item itself keeps working exactly as before -- this is advance notice,
not a removal. Turf never removes anything on its own; a deprecated item is
only ever removed across a major version.

```turflang
@deprecated("use add() instead")
fn add_old(a: int, b: int): int {
    return a + b;
}

fn add(a: int, b: int): int {
    return a + b;
}

fn main(): int {
    printline(add_old(1, 2));   // WARNING: 'add_old' is deprecated: use add() instead
    return 0;
}
```

The message is required and must be a non-empty string, written positionally
(`@deprecated("...")`, not `@deprecated(reason: "...")`) -- anything else is
`E146`. The warning has a [lint level](cli_and_diagnostics.md#lint-levels)
like any other, so a build that already knows about a deprecation and isn't
ready to migrate yet can set `E172` to `allow`.

---

## 5. Modules

A **module is one file**. A file's module name is its file name without the
`.tr`: `shapes.tr` is the module `shapes`. The file you compile is the
program (its module is `main`, a name you never write); every file it imports
is another module.

A module is made of top-level declarations: functions, structs, enums, errors,
machines, `extern fn`s, constants (`export immut`) and its own `import`s. Code
other than declarations does not run at the top level.

### Import forms

```text
import io;                       // the module: use io::printline(...)
import io::printline;            // one name, bare: printline(...)
import io::[print, printline];   // a list of names, bare
import math::[*];                // every public name of math, bare
```

Each form grants exactly one thing. `import m;` grants the qualifier `m::`
and makes nothing bare. `import m::name;`, `import m::[a, b];` and
`import m::[*];` make those names bare and do **not** grant the qualifier. A
file that wants both writes both:

```text
import list;           // list::List<int>, list::List::default()
import list::List;     // List<int>, List::default()
```

A file may write `m::` only for a module it imports itself. Importing `json`
(which imports `list` for its own use) does not let your file write
`list::List`: that is `E145` ("Module 'list' is not imported in this file"),
and the message gives the line to add. The prelude modules (section 7) need no
import. The rule applies only to spelling `m::name`: you can still hold, pass
and return a value whose type comes from a module you did not import, call its
methods, call functions on it through the dot (UFCS), and use its operators.

A bare import of the same name from two modules is an error at the second
import (`E024`: "'Node' is imported from two different modules"); qualify one
of them instead. A name that does not exist in the module, or a module that
cannot be found (`E027`), is reported at the import.

Imports affect only the file that writes them and are never re-exported
(`pub import` is `E087`).

### A first multi-file example

The examples that span several files show each file under a `// name.tr`
comment, and were compiled together in one directory.

<!-- doc-example: skip multi-file -->
```turflang
// shapes.tr
pub struct Point { pub x: int, pub y: int }

pub fn Point.len(): int { return self.x + self.y; }
pub fn origin(): Point { return Point::default(); }
export immut LIMIT: int = 10;

// main.tr
import shapes;

fn main(): int {
    mut p: shapes::Point = shapes::origin();
    p.x = 3;
    printline(p.len());          // 3
    printline(shapes::LIMIT);    // 10
    return 0;
}
```

### `::` and `.`

`::` names things that belong to a namespace; `.` reaches into a value you
hold. The compiler enforces the split:

```text
Status::Failed                 // an enum variant
io::printline("hi")            // a module's function
math::PI                       // a module's constant
MathUtil::square(5)            // a static method
Counter::new(1)                // construction
shapes::Point::default()       // module, type, then function
```

- `io.printline("x")`: `E048`, "'.' can't be used here -- 'io' is a module".
- `MathUtil.square(5)` on a type name: `E048`, "'MathUtil' is a struct type,
  not an instance".
- `p::get()` where `p` is a variable: `E029`, "'p' is a variable, not a type
  or module".
- Calling a static method through a value, `m.square(2)`: `E052`.

A local variable named like a module shadows it, by the ordinary scoping
rules, so `mut io: Thing = ...; io.show();` calls the variable's method.

<!-- doc-example: fails E048 -->
```turflang
fn main(): int {
    io.printline("x");
    return 0;
}
```

### Constants: `export immut`

A module shares a constant with `export immut`:

```turflang
export immut LIMIT: int = 10;
export immut RATIO: double = 0.5;

fn main(): int {
    printline(LIMIT * 2);      // 20
    return 0;
}
```

An `export` constant is always public (it is its own `pub`, so `pub export` is
`E087`); other modules reach it as `m::LIMIT` or by name after an import. It is
`immut` only (`export mut` is an error), appears only at the top level, and
its value must be a literal the compiler can fold: every use is replaced by the
value, and there is no storage behind it. A number may start with one `-`
(`export immut FLOOR: int = -40;`), and an integer literal is accepted for a
`double` constant (`export immut ONE: double = 1;` is `1.0`); anything more is
`E022`.

### Circular imports

If module `a` imports `b` and `b` imports `a`, or any longer loop does, the
program is rejected with `E019` ("Circular import detected"), and the message
draws the loop. Two modules importing a third is not a cycle.

---

## 6. Where `import <name>;` looks

`import <name>;` names a module, not a path, and must match **exactly one**
file. The compiler looks in every place it is allowed to and never takes a
"first" match: two different files answering to the name is `E090`, listing
both. A local `json.tr` therefore cannot silently take the place of the
standard library's, or be replaced by it. The error fires only when the name is
actually imported by a file that can see both.

```text
Module 'list' is ambiguous: 2 different files answer to that name.
    standard library  <install>/lib/std/collections/list.tr
    next to main.tr   ./list.tr
  Hint: A module may not share its name with a standard-library module. Rename
  ./list.tr (for example to 'my_list.tr') and import it by the new name.
```

**For a single file** (`turf run file.tr`, `turfc file.tr`), the places are:

1. the standard library, `lib/std/` of the installation;
2. installed packages, `lib/usr/`;
3. the directory of the file that contains the `import`;
4. the current directory.

The same file reached twice (when you compile from the file's own directory,
places 3 and 4 are one place) counts once. A module can sit next to the program
and be found from any working directory.

**In a project** (a `turf.toml`; `turf build`, `turf run`, `turf check`), a
module can import its own package's modules (a `<name>.tr` anywhere under
`src/`), the packages listed under `[dependencies]` by package name, and the
standard library; nothing else is searched. A module that is in the build but
out of reach (a dependency of a dependency, another package's internal module)
is `E091`. The rules are in
[Projects and packages](projects_and_packages.md#4-how-imports-resolve).

An `import` written inside the standard library, and the prelude's own
imports, look only in the standard library, so a `list.tr` of your own never
disturbs the library's internal use of its `list`; it only matters when you
write `import list;` yourself.

**The standard library's subfolders are invisible.** Files in
`lib/std/core/`, `lib/std/collections/` and so on are organised for browsing.
A module name is never a path, so you write `import list;`, not
`import collections::list;`, and moving a file between folders breaks no
import.

---

## 7. The prelude

Nine standard-library modules are available in every file, program and module
alike, with no `import`:

```text
int  double  bool  char  strings  array      reached by UFCS, operators and  strings::length(s)
io  math  sys                                also callable by their bare names
```

### Bare names

Every `pub` free function of `io`, `math` and `sys` can be called without a
qualifier: `printline("hi")`, `print(3)`, `input()`, `abs(-5)`, `max(a, b)`,
`sqrt(2.0)`, `exit(1)`, `panic("message")`. The qualified forms
(`io::printline`, `math::max`) work too, still with no import. Constants stay
qualified (`math::PI`; a bare `PI` is `E040`), and methods are never bare.

The other six modules are not flattened into bare names: their functions are
reached as methods (`c.is_digit()`, `n.is_even()`, `s.length()`) or qualified
(`strings::length(s)`). Calling `is_digit(c)` bare is `E041` ("Unknown
function"); `int`, `double`, `bool` and `char` are also type keywords, so their
functions are reached by UFCS only, never as `int::is_even(n)`.

```turflang
fn main(): int {
    printline(abs(-5));             // 5   (math, bare)
    printline(math::max(1.5, 0.5)); // 1.5 (math, qualified)
    printline(sqrt(16.0));          // 4
    printline(math::PI);            // 3.14159

    mut c: char = '7';
    printline(c.is_digit());        // true: UFCS on a char, from the char module
    printline("abc".length());      // 3
    printline(strings::length("abc"));
    return 0;
}
```

### A bare name is the last resort

A bare call `f(...)` looks, in order, at: (1) the functions this file declares;
(2) for code inside a module, that module's own functions; (3) the names
you imported (`import m::f;`); and only if none of those has any function
called `f`, (4) the prelude. So the prelude can never make a program
ambiguous: declaring your own `max(a: int, b: int)` hides math's `max`, and
`math::max` still reaches it. A bare function name used as a value
(`mut f = sqrt;`) is looked up in the same order.

The price is that once a file declares *any* function named `max`, a bare
`max(1.5, 0.5)` is checked against yours alone and reports `E056` naming your
function; write `math::max` for the prelude's:

```turflang
fn max(a: int, b: int): int {
    if a > b { return a; }
    return b;
}

fn main(): int {
    printline(max(3, 7));             // 7: this file's max
    printline(math::max(1.5, 0.5));   // 1.5: the prelude's
    return 0;
}
```

Importing a prelude module yourself (`import char;`, `import io;`) is
harmless.

---

## 8. Visibility: `pub`

Everything a module declares at its top level is **private to that module**
unless it is written with `pub` (or `public`: the two words are synonyms, like
`mut` and `mutable`; both are reserved words). Another module can use only
what is `pub`.

`pub` may precede:

- a function, a method (instance or `static`) and an `extern fn`:
  `pub fn`, `pub fn Point.len()`, `pub extern fn`;
- a `struct`, `enum`, `error` or `machine`;
- a field inside a struct body: `pub x: int`.

Rules:

- **Fields are private unless marked `pub`, even in a `pub struct`.** A
  module can hide the layout of a type it shares.
- **Variants and states follow their type.** The variants of a `pub enum` and
  the states of a `pub machine` are public; there is no per-variant `pub`.
- **Operators follow their operand types.** An `operator` is usable wherever
  its operand types are, so `pub operator` is `E087`: make the type `pub`.
- **`export immut` is its own `pub`.**
- `T::default()` is allowed from any module for any visible type: it only
  zero-fills.
- A module's own code always sees its own private names, and a program's main
  file needs no `pub` at all (visibility is checked across modules only).

```text
// shapes.tr
pub struct Point { pub x: int, pub y: int, cache: int }
pub fn Point.len(): int { return self.x + self.y; }
fn Point.refresh(): void { self.cache = 0; }
pub fn origin(): Point { return Point::default(); }
fn helper(): int { return 41; }
pub fn api(): int { return helper() + 1; }
```

Used from another file, `p.x`, `p.len()`, `shapes::origin()` and
`shapes::api()` work. The private parts are rejected with `E088`, and the
message names the declaration, the file to edit and the public alternatives:

```text
shapes::helper()            // E088: Function 'helper' is private to module 'shapes'.
p.cache                     // E088: Field 'Point.cache' is private to module 'shapes'.
p.refresh()                 // E088: Method 'Point.refresh' is private to module 'shapes'.
shapes::Node::default()     // E088: Type 'Node' is private to module 'shapes'.
import shapes::helper;      // E088, at the import
```

The private name is also off limits through every other path: a type
annotation, a variant of a private enum, a `static` method, and UFCS (a
private free function is not a candidate for `x.f()` outside its module). A
wildcard import, `import shapes::[*];`, simply skips the private names, so it
never raises `E088` and they never clash with names of your own. The fix is
either `pub` in the module that owns the declaration, or to use what the
module does export; the hint lists those.

**A `pub` item cannot expose a private type.** A `pub` function's parameter or
return type, a `pub` method's, or a `pub` field's type may not mention a type
of the same module that is not `pub`, anywhere in it (`!Hidden`, `&Hidden`,
`List<Hidden>`): an importer would receive values it cannot name. That is
`E089` ("Public function 'make' exposes the private type 'Hidden'"), reported
in the module that declares it. Make the type `pub`, or drop `pub` from the
item. A private type may be used freely inside a `pub` function's body, and
in a single-file program (nobody imports it) the rule never applies.

`pub` where it could change nothing is an error rather than being ignored
(`E087`): on an operator, an `import`, a local declaration inside a function,
a parameter, a variant, a machine transition, or a top-level `immut`. Its
hint says what to write instead.

### Migrating a 1.x module

Before 2.0 everything a module declared was importable. `turf migrate pub`
adds `pub` to every top-level declaration and every struct field, which keeps
that behaviour; without `--write` it prints what it would change:

```text
$ turf migrate pub oldmod.tr
would migrate oldmod.tr (5 `pub`)
  oldmod.tr:1:
    - struct Pt { x: int, y: int }
    + pub struct Pt { pub x: int, pub y: int }
  oldmod.tr:2:
    - fn Pt.len(): int { return self.x; }
    + pub fn Pt.len(): int { return self.x; }
  oldmod.tr:3:
    - fn helper(): int { return 1; }
    + pub fn helper(): int { return 1; }
Would migrate 5 declarations/fields in 1 file. Re-run with --write to apply.
```

`turf migrate pub --write <module.tr>...` applies it. The same command is
`turfc --migrate=pub`; see [CLI and diagnostics](cli_and_diagnostics.md). After
migrating, narrow each module's surface by removing `pub` from what is
internal.

---

## 9. Types belong to their module

A struct, enum, machine or `error` type is identified by the module that
declares it and its name, never by the name alone. Two modules may each declare
a `Node`, and a program may declare its own `List` or `Parser` without
importing `list` or `parser`: they are different types, `shapes::Node` and
`other::Node`.

`typeof` and diagnostics print the qualified name; a type declared in the main
file prints bare:

<!-- doc-example: skip multi-file -->
```turflang
// other.tr
pub struct Node { pub n: int }

// shapes.tr
pub struct Node2 { pub v: int }

// main.tr
import shapes;
import other;

struct Node { z: int }

fn main(): int {
    mut a: shapes::Node2 = shapes::Node2::default();
    mut b: other::Node = other::Node::default();
    mut c: Node = Node::default();
    printline(typeof(a));   // shapes::Node2
    printline(typeof(b));   // other::Node
    printline(typeof(c));   // Node
    return 0;
}
```

A bare name `Node` means this file's own type, or one it brought in by name
(`import m::Node;`, `m::[...]`, `m::[*]`); `m::Node` always names the module's
type. If a bare name could mean two types, the use is `E085`
("'Node' is ambiguous here -- it could mean more than one type"), naming both.
There is no rule that the local one wins. Write the imported one qualified and
leave it out of the import (list the names you need instead of `[*]`):

```text
import other::[*];            // other declares Node...
struct Node { z: int }        // ...and so does this file
mut c: Node = Node::default() // E085
```

Declaring the same type name twice in **one** file is `E034`; two modules
declaring it is fine. Generic types behave the same way
(`list::List<other::Node>`), and a generic type written without its type
arguments is an error.

---

## 10. Errors you will meet

| Situation | Code | Message |
|---|---|---|
| Missing `: type` after the parameter list | `E106` | Expected ':' followed by return type |
| `void` parameter | `E112` | 'void' cannot be a parameter's type |
| Function declared inside a function | `E038` | Nested function definitions are not allowed |
| Same name and parameters declared twice | `E011` | Duplicate function definition |
| Non-void function without a return on every path | `E009` | promises to give back a result |
| Wrong number of arguments | `E044` | called with N arguments |
| No overload accepts the argument types | `E056` | No matching overload! |
| Unknown function | `E041` | Unknown function |
| Overloaded name used as a value | `E125` | is overloaded |
| `m::f` or `T::method` used as a value | `E137` | Not a value! |
| Two UFCS candidates match equally | `E125` | Ambiguous UFCS call |
| `io.printline()`, `Type.static()` | `E048` | '.' can't be used here |
| `variable::member` | `E029` | is a variable, not a type or module |
| Static method called through a value | `E052` | is a static method |
| `T::new()` with no `new` declared | `E050` | has no constructor |
| `caller_location` in an unmarked function | `E084` | can only be used inside a function declared '@caller_location' |
| Bare type name could mean two types | `E085` | is ambiguous here |
| `pub` where it does nothing | `E087` | 'pub' can't be applied |
| Private function, type, field or method used from another module | `E088` | is private to module |
| `pub` item exposes a private type | `E089` | exposes the private type |
| Two files answer to one module name | `E090` | is ambiguous |
| Module exists but this package cannot import it | `E091` | not a dependency / internal to package |
| Import cycle | `E019` | Circular import detected |
| Module not found | `E027` | Could not find module |
| `m::` written without `import m;` | `E145` | is not imported in this file |
| Same name imported from two modules | `E024` | imported from two different modules |
| Unknown annotation, or one that does not fit | `E146` | Unknown annotation |
| Annotation before something other than a function | `E110` | Expected 'fn' or 'extern fn' after an annotation |
| Double free of an `@ownership` pointer | `E017` | Double free detected |
| Use after release | `E018` | using memory after it was freed |

`turf explain E0NN` (for example `turf explain E088`) prints the long explanation of any code.
