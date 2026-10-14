# Variables and Types

This page covers variable declarations and type inference, definite
assignment, the primitive types, literals and their range rules, integer
overflow, explicit conversion with `to`, string and `char` indexing, and the
`sizeof`, `typeof` and `hash_of` built-ins. The operators that act on these
types are in [Operators](operators.md).

## Variable declarations

A declaration is one of four keywords, in two pairs:

```turflang
fn main(): int {
    mut a: int = 10;
    mutable b: int = 20;
    immut x: int = 3;
    immutable y: int = 4;
    a = a + b + x + y;
    return a;
}
```

`mut` and `mutable` are exact synonyms, and so are `immut` and `immutable`.
Use whichever spelling you like, even both in one file.

```text
(mut | mutable | immut | immutable) <name> [: <Type>] [= <expr>] ;
```

A `mut` variable can be assigned again after its declaration. An `immut`
variable cannot, and assigning to one is an error (E060, "You can't change the
value of ... because it's immutable").

<!-- doc-example: fails E060 -->
```turflang
immut limit: int = 10;
limit = 20;
```

An `immut` variable must be given its value in the declaration, for every
type. `immut x: int;` is E128 ("Missing initializer"): the variable could never
get a value later. A `mut` variable may be declared without one, as described
under [Definite assignment](#definite-assignment).

<!-- doc-example: fails E128 -->
```turflang
immut x: int;
```

`void` is not a type for a variable. `mut x: void;` is E112.

## Type inference with `guess`

Leave the type out, or write `guess` in its place, and the compiler takes the
type from the initializer. The two spellings are identical:

```turflang
fn main(): int {
    mut count = 5;               // int
    mut ratio: guess = 2.5;      // double
    mut name = "turf";           // string
    mut initial = 'x';           // char
    mut ready = true;            // bool
    mut primes = [2, 3, 5, 7];   // int[4]
    immut limit = 100;           // int

    printline(typeof(count));
    printline(typeof(primes));
    count = count + limit;
    return 0;
}
```

An untyped literal takes its default type:

| Initializer | Inferred type |
|---|---|
| integer literal | `int` |
| float literal | `double` |
| `true`, `false` | `bool` |
| `'c'` | `char` |
| `"s"` | `string` |
| `[a, b, c]` | `T[N]` for the element type `T` and `N` elements |
| any other expression | the expression's own type (a call to `fn f(): int32` gives `int32`; it is not widened) |

The default type is chosen first and the value is checked against it, so
`mut big = 999999999999999999999;` is an out-of-range error for `int`
(E076), not a promotion to something wider. There is nothing wider.

The inferred type is fixed at the declaration. A later assignment of another
type is an ordinary type mismatch (E066), and the message says where the type
came from:

<!-- doc-example: fails E066 -->
```turflang
mut x = 5;
x = 5.5;
```

```text
Type mismatch!
Cannot assign value of type 'double' to variable of type 'int'.
Note: 'x' was inferred as 'int' here (line 1, col 18)
```

Inference is local and takes its type from one initializer. It does not apply
to:

- a declaration without an initializer: `mut x: guess;` and `mut x;` are E128
  ("`guess` needs an initializer to infer from");
- a function parameter, a return type or a struct field: `guess` there is E128
  ("can only declare a local variable");
- an initializer with no usable type: `mut x = null;`, or the result of a
  `void` call, is E129 ("Cannot infer a type from ...");
- a generic type given without its type arguments (E129).

## Definite assignment

A `mut` variable declared without an initializer holds no value yet. The
compiler tracks, for every path through the function, whether it has been
assigned, and a read that some path reaches before any assignment is E008
("You're using 'x' before it has a value"). The rule is the same for every type:
no read sees a value nothing assigned. An array is tracked as a whole (see
[Arrays](arrays.md#initialization)).

<!-- doc-example: fails E008 -->
```turflang
mut x: int;
mut y: int = x;
```

Assigning on every path makes the read legal:

```turflang
fn pick(flag: bool): int {
    mut x: int;
    if flag {
        x = 1;
    } else {
        x = 2;
    }
    return x;
}

fn main(): int {
    return pick(true);
}
```

Assigning on only some paths does not. A loop body might run zero times, so
this is E008 as well:

<!-- doc-example: fails E008 -->
```turflang
mut last: int;
mut i: int = 0;
while i < 3 {
    last = i;
    i++;
}
mut copy: int = last;
```

`char`, `int32`, `bool`, `string`, enums, structs and arrays follow the same
rule: declare-then-read is E008. To get a zero-valued struct immediately, use
[`T::default()`](structs_and_enums.md#construction-new-and-default). Writing a
field of an unassigned struct (`mut p: Point; p.x = 4;`) is also E008, because
struct fields are not tracked individually: a struct is either fully assigned
or not.

An array is tracked as a whole. It counts as assigned once it is assigned whole
or **any one element** is written. After that every element can be read, and the
ones never written read as zero:

```turflang
struct Point {
    x: int,
    y: int
}

fn main(): int {
    mut p: Point = Point::default();   // every field zero, readable at once

    mut arr: int[3];
    // mut first: int = arr[1];        // E008: no element written yet
    arr[0] = 10;
    mut second: int = arr[1];          // 0
    return p.x + second;
}
```

### Zero values

The zero value is what `T::default()` gives a field, and what an array element
that was never written reads as:

| Type | Zero value |
|---|---|
| `int`, `int32` | `0` |
| `double` | `0.0` |
| `bool` | `false` |
| `char` | byte `0`, `'\0'` |
| `string` | `""` |
| enum | the first variant |
| pointer, function type | null |
| array | every element its own zero value |
| struct | every field its own zero value, recursively |
| `&?T` | null |
| `&T` | a fresh `T::default()` of its own (a reference is never null) |

A struct that holds a `&T` field can only be defaulted when that gives a finite
structure: a cycle through `&T` fields is a compile error. See
[Structs and enums](structs_and_enums.md#construction-new-and-default).

The storage behind a bare declaration is zero-filled anyway. Definite
assignment is a check on your program's logic, not protection against garbage.

## Primitive types

| Type | Size | Notes |
|---|---|---|
| `int` | 64-bit | Signed. The general-purpose integer. |
| `int32` | 32-bit | Signed. For describing C `int` in `extern fn` signatures. |
| `double` | 64-bit | IEEE 754 floating point. |
| `bool` | 1 byte | `true` or `false`. |
| `char` | 1 byte | One raw byte, 0 to 255. Not a Unicode code point. |
| `string` | pointer | A NUL-terminated byte string. |
| `void` | none | Only a function's return type. |

There are no other integer widths and no unsigned types. `uint` or `unsigned`
is E042 (unknown type).

### `int`

A 64-bit signed integer. It widens implicitly to `double`. `+ - * / %` on two
integers give an `int`; if either operand is a `double` the result is a
`double`.

### `int32`

`int32` exists to describe a C `int` honestly in an `extern fn` declaration. A
function that really returns a 32-bit value must not be declared to return a
64-bit one.

```turflang
extern fn abs(n: int32): int32;

fn main(): int {
    immut r: int32 = abs(-5 to int32);
    return r to int;
}
```

- **Widening is implicit.** An `int32` converts to `int` and to `double`
  wherever one is expected.
- **Narrowing needs a cast, for a value that is already typed `int`.** The cast
  truncates to the low 32 bits:

  ```turflang
  fn main(): int {
      mut existing: int = 5;
      mut n: int32 = existing to int32;
      return n to int;
  }
  ```

  `mut n: int32 = existing;` is E066.
- **An untyped literal becomes an `int32` directly** when that is the
  destination: `mut n: int32 = 5;` is fine, and `mut n: int32 = 99999999999;` is
  E076 because the value does not fit in 32 bits.
- **Arithmetic and comparison involving an `int32` produce an `int`.** An
  `int32` is a storage type, not a second arithmetic type. Assigning the sum
  of two `int32` values to an `int32` variable is E066; cast it back with `to
  int32`.
- **`++`, `--` and every compound assignment on an `int32` variable are E126**
  ("not supported on 'int32'"). Write the operation out:

  ```turflang
  fn main(): int {
      mut n: int32 = 5;
      n = (n + 1) to int32;
      return n to int;
  }
  ```
- The bitwise operators and `~` do not accept an `int32` (E057). Convert to
  `int` first.

### `double`

An IEEE 754 double. `int` and `int32` widen to it implicitly. `double` to `int`
is never implicit: use `to int`, which truncates toward zero (see [Casting with
`to`](#casting-with-to)).

Arithmetic on `double` follows IEEE rules, with one exception: division and
remainder by zero panic rather than producing `inf` or `NaN`. Overflow in
`double` arithmetic gives `inf` or `-inf` and does not panic. A `NaN` prints,
and converts to `string`, as `nan`.

### `bool`

`true` or `false`. A `bool` has no conversion to or from a number, either
implicit or explicit: `mut x: int = true;` is E066, and `true to int` is E134
("That conversion isn't allowed"). To get a number from a condition, say what
you want: `cond give 1 otherwise 0`. The one cast out of `bool` is `bool to
string`, which gives `"true"` or `"false"`. Nor is a `bool` an operand of
arithmetic or ordering: `true + 1` is E057, and `true < false` and `1 < 2 < 3`
(which compares the `bool` `1 < 2` with `3`) are E124. `==` and `!=` compare
two `bool`s.

```turflang
fn main(): int {
    mut flag: bool = true;
    printline(flag to string);                 // true
    mut n: int = flag give 1 otherwise 0;
    return n;
}
```

### `char`

A `char` is one byte. It is its own type, not an integer and not a one-letter
string.

```turflang
fn main(): int {
    mut c: char = 'a';
    mut next: char = (c to int + 1) to char;
    printline(next);                  // b
    return 0;
}
```

- `== != < > <= >=` compare byte values as unsigned numbers, so `200 to char`
  is greater than `'a'`.
- There is **no arithmetic** on a `char`. `'a' + 1` is E057. Convert to `int`
  first.
- There is **no implicit conversion** either way. `mut c: char = 97;` is E066 and
  `c == 97` is E124 ("Values can't be compared"). Write `'a'`, or `c to int ==
  97`.
- The explicit casts are between `char` and `int` only: `char to int`
  zero-extends (always 0 to 255), and `int to char` keeps the low byte
  (`321 to char` is `'A'`). There is no cast from `char` to `string`, `double`,
  `bool` or `int32`.

### `string`

A `string` is a pointer to NUL-terminated bytes. A string literal is static
data. Every string an operation produces lives on the garbage-collected heap.

`+` and the six comparison operators on strings are ordinary operator
overloads defined in the standard library, available without an import (see
[Operators: String operators](operators.md#string-operators)). `s[i]` is built
into the language and is described next.

#### Indexing a string

`s[i]` is the byte at position `i`, and its type is `char`. Compare it with a
`char` literal directly, and convert with `to int` when you want the number:

```turflang
fn main(): int {
    mut s: string = "hello";
    mut first: char = s[0];

    printline(first == 'h');            // true
    printline(s[1] to int);             // 101
    printline(s[-1]);                   // o: negative counts from the end
    printline("hello"[4]);              // o

    mut vowels: int = 0;
    mut i: int = 0;
    while i < s.length() {
        if s[i] == 'e' || s[i] == 'o' {
            vowels++;
        }
        i++;
    }
    return vowels;
}
```

`mut n: int = s[0];` is E066: a `char` is not an `int`.

A **read** is bounds-checked at run time. The valid indices are `-n` to `n - 1`
for a string of `n` bytes, and anything else panics with `String index out of
bounds`. A negative index `i` means `n + i`, the same as for arrays and
`List`. Reading position 0 of `""` panics.

A **write** (`s[i] = c;`) wraps a negative index the same way and panics if the
result is still below zero. Its upper bound is the string's heap allocation,
not its length, since a string being filled byte by byte has no terminator yet
to measure: a write past the allocation panics with `String index out of
bounds`, and a write into a string that is not on the heap (a string literal,
or memory from C) panics with `Cannot write into this string`. The value being
stored must be a `char` (`s[0] = 65;` is E066).

`strings::byte_at_unchecked(s, i)` reads a byte with no check at all, for a
loop that has already established the index is in range. Everything else about
strings is in [the `strings` module](stdlib/core.md).

### `void`

Only a function's return type.

## Literals

### Integer literals

| Form | Example | Value |
|---|---|---|
| decimal | `42`, `1_000_000` | as written |
| hexadecimal | `0xFF`, `0x1_000` | 255, 4096 |
| binary | `0b1010` | 10 |
| octal | `0o17` | 15 |

The prefix letter may be upper or lower case (`0XFF`, `0B1010`, `0O17`). An
underscore `_` is a digit separator: it may sit anywhere **between** two digits
and nowhere else.

A leading `0` is **not** an octal marker. `0755` is the decimal number 755. Use
`0o755` for octal.

```turflang
fn main(): int {
    immut million: int = 1_000_000;
    immut mask: int = 0xFF;
    immut flags: int = 0b1010;
    immut perms: int = 0o755;
    immut decimal: int = 0755;      // 755, not 493

    printline(perms);               // 493
    printline(decimal);             // 755
    return million + mask + flags;
}
```

There are no integer-literal suffixes, and no way to write a negative number
other than the unary `-` operator.

### Floating-point literals

A float literal is either **dotted**, with at least one digit on **each** side
of the point (`1.5`, `0.25`, `3.14_159`), or **dotless with an exponent**
(`1e6`). Either form may take an exponent: `e` or `E`, an optional sign and
digits (`1.5e-3`, `1E+10`, `2e8`). `.5` is accepted and means `0.5`.

```turflang
fn main(): int {
    immut a: double = 1.5;
    immut b: double = 1e6;
    immut c: double = 1.5e-3;
    immut d: double = 1E+10;
    immut e: double = 3.14_159;
    return (a + b + c + d + e) to int;
}
```

A number that ends in a bare point is rejected: `5.` is E139 ("ends in a '.'
with no digit after it"), so write `5.0` or `5`. Hex, binary and octal
literals are whole numbers only: `0x1.8` is E139. A digit sequence cannot hold
two points (`1.5.5`, E139).

The malformed-literal errors:

| Code | Cause | Example |
|---|---|---|
| E070 | radix prefix with no digit | `0x` |
| E071 | digit not valid in the radix | `0b012`, `0o8` |
| E072 | separator at the start or end of a digit run | `5_`, `0x_FF` |
| E073 | exponent marker with no exponent | `1.5e`, `1.5e+` |
| E139 | malformed decimal point | `5.`, `0x1.8`, `1.5.5` |

(A leading underscore, `_5`, is not a number at all: it is the identifier `_5`,
E040.)

### Range rules for literals

An integer literal is held exactly, however large, until its type is known.
Once the destination is fixed (`int`, `int32` or `double`), the value must fit.
**An out-of-range literal is a compile error. It is never truncated or wrapped.**

<!-- doc-example: fails E076 -->
```turflang
mut x: int = 9223372036854775808;
```

```text
Integer literal is not representable in type 'int'.
The literal's value is 9223372036854775808.
'int' holds -9223372036854775808 .. 9223372036854775807.
```

| Destination | Range | Error when outside |
|---|---|---|
| `int` | -9223372036854775808 to 9223372036854775807 | E076 |
| `int32` | -2147483648 to 2147483647 | E076 |
| `double` (from an integer literal) | must convert **exactly** | E077 |
| `double` (from a float literal) | must not round to infinity | E078 |

- A literal written with a leading `-` is checked **with** its sign, so
  `-9223372036854775808` is legal, and `-9223372036854775809` is not.
- `0xFFFFFFFFFFFFFFFF` is not `-1`. There is no wrapping, and it is E076.
- An integer literal assigned straight to a `double` must be exactly
  representable in a `double`. `mut d: double = 5;` is fine, but
  `9223372036854775807` has no exact `double` and is E077. Write
  `9223372036854775807.0` if the rounded value is what you want. Values above
  `int`'s own range are rejected as E076 before this check.
- A float literal too large for a `double` (`1e400`) is E078. A tiny one that
  underflows (`1e-400`) is `0.0`.

The same check applies to a constant expression. When every operand is a
literal, or an `immut` variable whose value is known, the result is computed
exactly and must fit its destination:

<!-- doc-example: fails E076 -->
```turflang
immut top: int = 9223372036854775807;
immut past = top + 1;
```

This is a compile-time error. The same arithmetic on `mut` variables is a
[run-time trap](#integer-overflow).

### `bool`, `char` and `string` literals

`true` and `false` are keywords. A char literal is exactly one character or one
escape between single quotes: `'a'`, `'\n'`, `'\''`.

- `''` is E101 ("Empty char literal").
- `'ab'` is E101 ("Char literal must contain exactly one character").

A string literal is in double quotes and cannot continue onto the next line. A
missing closing quote is E099 ("Unterminated string literal"), reported at the
line where the string began. Use `\n` for a line break inside the text.

Escapes, in both char and string literals:

| Escape | Byte | Escape | Byte |
|---|---|---|---|
| `\n` | newline (10) | `\a` | bell (7) |
| `\t` | tab (9) | `\b` | backspace (8) |
| `\r` | carriage return (13) | `\f` | form feed (12) |
| `\0` | NUL (0) | `\v` | vertical tab (11) |
| `\\` | backslash (92) | `\'` | single quote (39) |
| `\"` | double quote (34) | | |

Any other escape (`\q`, `\x41`) is E100 ("Unknown escape sequence"). Each
escape is one byte: `"a\rb"` is three bytes long.

### `null`

`null` is the null pointer. It is compatible with raw pointer types and `&?T`
only. `mut x: int = null;` and `mut s: string = null;` are E062, and a `&T`
reference can never be null.

## Integer overflow

`int` is 64-bit signed and **overflow is a run-time panic, never a silent wrap.**
`+ - *`, unary `-`, `++`, `--`, `<<` and `**` all check the result, and so does
the quotient `INT_MIN / -1`:

```text
panic: runtime check: Integer overflow
```

```turflang
fn main(): int {
    mut a: int = 9223372036854775807;
    a = a + 1;           // panics: Integer overflow
    return 0;
}
```

The program aborts. There is no unwinding, and deferred statements do not run.

Other checked operations:

| Operation | Panic message |
|---|---|
| `x / 0`, for `int` and `double` | `Division by zero` |
| `x % 0` | `Modulo by zero` |
| shift by a count outside 0 to 63, or `<<` that loses a set bit | `Integer overflow` |
| `int ** negative int` | `Negative exponent in integer '**'` |
| `double to int` of a NaN, an infinity, or a value outside `int` | `Integer overflow` |

`double` arithmetic is the exception: `1.7e308 * 10.0` is `inf`, not a panic.
A cast to a narrower type (`to int32`, `to char`) truncates and does not panic.

These behaviours do not depend on the optimisation level.

## Casting with `to`

`to` is the only explicit conversion. There is no `int(x)` call form.

```turflang
fn main(): int {
    mut i: int = 65;
    mut d: double = i to double;
    mut s: string = i to string;
    printline(d);                   // 65
    printline(s + "!");             // 65!
    return 0;
}
```

`to` binds looser than every arithmetic and comparison operator and tighter
than assignment. `1 + 2 to double` is `(1 + 2) to double`, and `y = x to int;`
is `y = (x to int)`. Parenthesise to cast one operand: `(x to double) * 2`.
Writing `x to double * 2` without parentheses does not work: the `*` after a
type name is read as a pointer type.

An illegal cast is rejected at compile time (E134, "That conversion isn't
allowed", or E065 for the text conversions).

### Conversion table

The row is the source type, the column the target. `ok` is a legal cast,
`-` is rejected. Implicit conversions (no `to` needed) are marked in the notes
below the table.

| from \ to | `int` | `int32` | `double` | `bool` | `char` | `string` | enum |
|---|---|---|---|---|---|---|---|
| `int` | | ok | ok | - | ok | ok | ok |
| `int32` | ok | | ok | - | - | - | - |
| `double` | ok | ok | | - | - | ok | - |
| `bool` | - | - | - | | - | ok | - |
| `char` | ok | - | - | - | | - | - |
| `string` | ok | - | ok | - | - | | ok |
| enum | ok | - | - | - | - | ok | |

What each legal cast does:

- **`int to int32`** keeps the low 32 bits. `4294967297 to int32` is `1`.
- **`int to char`** keeps the low 8 bits. **`char to int`** gives 0 to 255.
- **`int32 to int`** sign-extends. **`int` and `int32 to double`** convert the
  number.
- **`double to int`** truncates toward zero (`-3.9 to int` is `-3`). A NaN, an
  infinity, or a value outside the range of `int` panics with `Integer
  overflow`. **`double to int32`** does the same against `int32`'s range.
- **`int to string`** and **`double to string`** format the number. A `double`
  is written in the shortest `%g`-style form (`0.1`, `1e+20`, `1.23457e+06`),
  and a NaN is `nan`. **`bool to string`** gives `"true"` or `"false"`.
- **`string to int`** accepts the whole string or nothing: an optional `-`
  and decimal digits, in the range of `int`. Anything else panics with
  `Invalid string to int conversion`: `"12abc"`, `"1.5"`, `" 12"`, `"+5"`,
  `""`, or `"9223372036854775808"`.
- **`string to double`** accepts an optional `-`, digits with at most one `.`
  (at least one digit), and an optional exponent (`1e3`, `2.5E-3`). Anything
  else panics with `Invalid string to double conversion`: `"abc"`, `"12abc"`,
  `" 1.5"`, `"inf"`, `"nan"`. A magnitude beyond the `double` range is an
  infinity.
- Both accept exactly what `strings::parse_int` and `strings::parse_double`
  accept. When the text comes from outside your program (`input()`, a file),
  prefer those: they report bad text as an error value you can `recover`
  from instead of panicking (see [the `strings` module](stdlib/core.md)).
- **`enum to int`** gives the variant's position, and **`int to enum`** panics
  with `Invalid int to enum conversion` when no variant has that position.
  **`enum to string`** gives the variant name, and **`string to enum`** panics
  with `Invalid string to enum conversion` for an unknown name.
- A string **literal** that can never convert (`"abc" to int`, `"1.2.3" to
  double`, `"Purple" to Color`) is E149 at compile time, and an integer
  literal that is no variant (`7 to Color`) is E127.

Implicit conversions are exactly `int32` to `int`, `int32` to `double` and
`int` to `double`. Nothing narrows implicitly, and there is no implicit
conversion between `bool`, `char` and the numbers. `mut i: int = 3.7;` is
E066.

A struct or array cannot be cast to a string (E065); build the text with `+`
and `to string` on its fields.

Casts that involve raw pointers or reference types (`int to int*`, `T* to
ptr`, `string to &T`, `&T to string`) are only allowed inside `unsafe { }`
(E130 outside). See [FFI and unsafe](ffi_and_unsafe.md). A `&T` converts to `&?T` implicitly.

## `sizeof` and `typeof`

### `sizeof(T)`

`sizeof` takes a **type** and is an `int` constant: the type's size in bytes.
A variable is not a type: `sizeof(a)` is `E109`.

```turflang
struct Pair {
    a: int,
    b: double,
    c: char
}

fn main(): int {
    printline(sizeof(int));        // 8
    printline(sizeof(int32));      // 4
    printline(sizeof(bool));       // 1
    printline(sizeof(char));       // 1
    printline(sizeof(string));     // 8
    printline(sizeof(int[4]));     // 32
    printline(sizeof(Pair));       // 24, includes padding
    return 0;
}
```

The size of a struct is the one the target platform's layout gives, padding
included. `sizeof(void)` is E126, and `sizeof(void*)` is a pointer's size. A name
that is not a type (`sizeof(x)` for a variable `x`, or a misspelling) is E042.

### `typeof(expr)`

`typeof` takes an **expression** and returns a `string` naming its static,
compile-time type. There is no run-time type information: the string is the
type the compiler gave the expression, wherever the call happens.

```turflang
import list::List;

struct Node {
    v: int
}

fn main(): int {
    mut n: Node = Node::default();
    mut names: List<string> = List::default();

    printline(typeof(10));          // int
    printline(typeof(3.14));        // double
    printline(typeof("hi"));        // string
    printline(typeof(true));        // bool
    printline(typeof([1, 2]));      // int[2]
    printline(typeof(n));           // Node
    printline(typeof(names));       // list::List<string>
    return 0;
}
```

A struct, enum or `error` type declared in the main program file is named by
its bare name (`Node`). One declared in an imported module is named by its
**qualified** name: `list::List<string>`, `json::JsonValue`. The string is the
same wherever `typeof` is called, so compare against the qualified spelling:
`typeof(v) == "json::JsonValue"`, never `"JsonValue"`.

`typeof` needs no import: it is a built-in.

### `hash_of(expr)`

`hash_of` takes any value and returns an `int` such that values that compare
`==` hash equally. It is built from the value's static type: a `string` by its
text, a `double` with `-0.0` taken as `0.0`, a struct field by field (never the
padding between fields), a fixed array or `T[]` element by element, and a
reference or pointer by its address. `Map` and `Set` hash their keys with it,
so a struct key works as long as its `operator ==` compares field by field. A
key compared some other way (case-insensitively, say) needs its fields
normalised before it goes in. A NaN never equals anything, itself included, so
a NaN key is never found.

```turflang
struct P {
    c: char,
    n: int
}

fn mk(n: int): P {
    mut p: P = P::default();
    p.c = 'a';
    p.n = n;
    return p;
}

fn main(): int {
    test::assert(hash_of(mk(5)) == hash_of(mk(5)), "equal structs");
    test::assert(hash_of("ab" + "c") == hash_of("abc"), "strings by text");
    return 0;
}
```

---

See also: [Operators](operators.md) for the operator set and its precedence
table, and [Structs and enums](structs_and_enums.md) for `::default()`.
