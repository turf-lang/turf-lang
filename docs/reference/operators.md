# Operators

This page covers the built-in operators: their precedence, what each accepts
and what it checks at run time. Declaring an operator for your own type is
described in [Operator overloading](generics_and_operators.md#operator-overloading).

## Precedence

From the tightest binding to the loosest:

| Level | Operators | Grouping |
|---|---|---|
| 1 | postfix `[ ]`, `( )`, `.`, `++`, `--` | left |
| 2 | prefix `-`, `!`, `~`, `++`, `--` | right |
| 3 | `**` | right |
| 4 | `*` `/` `%` | left |
| 5 | `+` `-` | left |
| 6 | `<<` `>>` | left |
| 7 | `&` | left |
| 8 | `^` | left |
| 9 | `\|` | left |
| 10 | `<` `>` `<=` `>=` | left |
| 11 | `==` `!=` | left |
| 12 | `&&` | left |
| 13 | `\|\|` | left |
| 14 | `to` | left |
| 15 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` `**=` | right |
| 16 | `cond give a otherwise b` | right |

Things that differ from C:

- **The bitwise operators bind tighter than comparison.** `x & 1 == 1` is
  `(x & 1) == 1`, and `flags | 8 == 15` is `(flags | 8) == 15`. In C the same
  text groups the other way. The order among them is `&`, then `^`, then `|`,
  and the shifts bind tighter than all three but looser than `+` and `-`
  (`1 + 2 << 1` is 6).
- **Unary operators bind tighter than `**`**, with no exception for `-`. So
  `-2 ** 2` is `(-2) ** 2`, which is `4`; write `-(2 ** 2)` for `-4`. `**`
  itself groups to the right: `2 ** 3 ** 2` is `2 ** (3 ** 2)`, which is `512`.
- **`to` is looser than every arithmetic, bitwise, comparison and logical
  operator.** `1 + 2 to double` casts the sum, and `1 < 2 to string` is
  `"true"`. It is tighter than assignment, so `y = x to int;` is
  `y = (x to int)`. To cast one operand, parenthesise: `(x to double) * 2`.
- **Assignment is an expression** whose value is the assigned value, and it
  groups to the right: `a = b = 3;` assigns both.
- **`give`/`otherwise` is the loosest of all**, looser than assignment. In
  `r = c give a otherwise b;` the assignment is the condition. Parenthesise the
  whole conditional: `r = (c give a otherwise b);`.

There is no `cond ? a : b`. The conditional expression is
[`give`/`otherwise`](#conditional-give--otherwise).

## Assignment: `=`

The left side must be a mutable location: a `mut` variable, a struct field or an
array element. Assigning to an `immut` variable is E060, and assigning to
something that is not a location (`5 = 6`) is E061.

The right side is evaluated **first**, then the location's own sub-expressions,
left to right, then the bounds checks. In `a[i()] = v()`, `v` runs before `i`:

```turflang
fn tag(label: string, v: int): int {
    printline(label);
    return v;
}

fn main(): int {
    mut a: int[3] = [0, 0, 0];
    a[tag("index", 1)] = tag("value", 7);   // prints value, then index
    return a[1];
}
```

The same order holds for compound assignment and for a type's own
`operator []=`.

## Arithmetic: `+ - * / %`

```turflang
fn main(): int {
    mut a: int = 7;
    mut b: int = 2;
    printline(a / b);        // 3: integer division truncates toward zero
    printline(-a / b);       // -3
    printline(a % b);        // 1
    printline(-a % b);       // -1: the sign of the dividend
    printline(7.5 % 2.0);    // 1.5: % also works on doubles
    printline(a / 2.0);      // 3.5: an int mixed with a double becomes double
    return 0;
}
```

Two operands combine to a `double` if either is a `double`, and to an `int`
otherwise. An `int32` operand counts as an `int`
([`int32`](variables_and_types.md#int32)). A `char` takes no part in arithmetic:
`'a' + 1` is E057.

**Division and remainder by zero panic**, for `int` and for `double`:
`Division by zero` for `/`, `Modulo by zero` for `%`. Dividing a `double` by
zero is never `inf`.

**Signed overflow panics.** `+`, `-`, `*`, unary `-`, `++`, `--`, `<<` and `**`
on `int` check their result:

```turflang
fn main(): int {
    mut x: int = 9223372036854775807;
    x = x + 1;                          // panics: Integer overflow
    return 0;
}
```

```text
panic: runtime check: Integer overflow
```

`-INT_MIN` and `INT_MIN / -1` have no `int` result and panic the same way.
`INT_MIN % -1` is `0`, which is its exact result. `double` arithmetic never
panics for overflow: `1.7e308 * 10.0` is `inf`.

When every operand is a literal or an `immut` variable with a known value, the
expression is evaluated exactly at compile time, and a result that does not fit
is a compile error instead (E076):

<!-- doc-example: fails E076 -->
```turflang
mut c: int = 9223372036854775807 + 1;
```

`mut` variables are never constants, so `x + 1` above is a run-time check. See
[Integer overflow](variables_and_types.md#integer-overflow) for the full list of
checked operations.

## Exponentiation: `**`

```turflang
fn main(): int {
    mut r: int = 2 ** 10;         // 1024
    mut h: double = 2 ** 0.5;     // 1.41421
    mut e: double = 2 ** -1.0;    // 0.5
    printline(r);
    printline(h);
    printline(e);
    return 0;
}
```

With an `int` (or `int32`) on both sides the result is an exact `int`: every
multiplication is overflow-checked, so `3 ** 62` panics with `Integer
overflow`, and a negative exponent panics with `Negative exponent in integer
'**'`. An `int` cannot hold `2 ** -1`, so write `2 ** -1.0`. A `double` on
either side gives a `double`.

`^` is bitwise XOR and is not exponentiation.

## Comparison: `== != < > <= >=`

Numbers compare by value, and an `int` and a `double` compare numerically
(`1 == 1.0` is true). `char`s compare by byte value, and a `char` is not
comparable with an `int` (`c == 97` is E124). Enums compare with the same
enum. `bool`s can be compared for equality.

`string` comparison is defined in the standard library, see [String
operators](#string-operators). For a **struct**, the comparison operators do
not exist until you declare them, and each one is declared separately:
declaring `operator ==` does not give you `!=`, and `operator <` does not give
you `>`. Using one that is missing is E124 ("Values can't be compared"). Arrays
cannot be compared with `==` either; compare elements.

## Logical: `&& || !`

`&&` and `||` take `bool` operands (an `int` or `double` is E057; write
`n != 0`) and **short-circuit**: the right operand is
evaluated only when the left does not already decide the result. That makes
guard conditions safe:

```turflang
fn main(): int {
    mut e: int = 0;
    mut ok: bool = (e != 0) && (100 / e > 5);   // the division never runs
    mut any: bool = true || (1 / e > 0);        // neither does this one
    printline(ok);
    printline(any);
    return 0;
}
```

`&&` binds tighter than `||`: `true || false && false` is true.

`!x` is `x != true` for a `bool`, so `!!f == f`. Its operand must be a `bool`
(`!5` is E057), unless it is a **struct** that declares its own `operator !`;
then the call replaces the rewriting, for that type.

`&&` and `||` cannot be overloaded, because a function call cannot
short-circuit. `operator &&` is a syntax error (E110). A struct operand is
rejected by `&&` and `||`, whether or not the struct declares `operator !`:
turn it into a `bool` yourself, with a comparison for example.

## Bitwise: `& | ^ ~ << >>`

```turflang
fn main(): int {
    mut a: int = 12;
    mut b: int = 10;
    printline(a & b);       // 8
    printline(a | b);       // 14
    printline(a ^ b);       // 6, XOR
    printline(~a);          // -13
    printline(a << 2);      // 48
    printline(-8 >> 1);     // -4
    return 0;
}
```

All bitwise operators, including unary `~`, take `int` operands. A `double`,
`bool` or `int32` operand is E057 ("Bitwise operator requires integer
operands"); there is no implicit conversion.

- `>>` is an **arithmetic** shift: it copies the sign bit. `-8 >> 1` is `-4`
  and `-1 >> 1` is `-1`.
- Both shifts take a count from 0 to 63. A count outside that range panics with
  `Integer overflow`, and so does a `<<` that shifts a set bit past the sign
  bit (`1 << 63`).
- `~x` is `-(x + 1)`, so `~0` is `-1`.

## Compound assignment

```text
+=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=  **=
```

`a op= b` means `a = a op b`, with the location `a` evaluated once. It has the
same checks as the plain operator, including the overflow and zero-division
panics, and its right side runs first like any assignment. There is no `~=`.

The bitwise forms (`&= |= ^= <<= >>=`) require `int` operands, like the plain
operators. `**=` follows `**`: with an `int` on both sides it is the exact `int`
power, and with a `double` on either side the result is computed as a `double`
and converted back to the variable's own type, trapping if it no longer fits:

```turflang
fn main(): int {
    mut b: int = 3;
    b **= 4;                   // 81, exact
    mut d: int = 2;
    mut exp: double = 3.0;
    d **= exp;                 // 8, the double power converted back to int
    printline(b);
    printline(d);
    return 0;
}
```

A type that declares `operator +` supports `+=` automatically, because the
compound form is rewritten to `a = a + b` first (see [Operators on your own
types](#operators-on-your-own-types)). An `int32` variable supports no compound
assignment at all (E126); write `n = (n + 1) to int32;`.

## Increment and decrement: `++` `--`

Both the prefix and the postfix form work on any assignable location: a
variable, an array element or a struct field. `i++` and `++i` both add one. They
differ only in the value the expression has: **postfix gives the value before the
change, prefix the value after it.**

```turflang
fn main(): int {
    mut i: int = 5;
    printline(i++);       // 5, i is now 6
    printline(++i);       // 7
    printline(i--);       // 7, i is now 6
    printline(--i);       // 5

    mut j: int = 5;
    mut k: int = - ++j;   // j becomes 6, then it is negated: k is -6

    mut a: int[2] = [1, 2];
    ++a[0];               // a[0] is 2
    a[1]--;               // a[1] is 1
    return k + a[0] + a[1];
}
```

Operands are evaluated strictly left to right, so a chain of them has one
defined result. With `x` starting at 1,
`x++ + ++x - ++x - x-- - --x` is `1 + 3 - 4 - 4 - 2`, which is `-6`, every time,
at every optimisation level.

Both forms check for overflow like `+ 1` and `- 1`. They apply to a `double`
too (`1.5` becomes `2.5`), they are E060 on an `immut` variable, and they are
E126 on an `int32`.

> **Tokenisation.** `--x` is one token, so `---x` reads as `--` followed by
> `-x` and is a syntax error (E103). For three negations, separate them:
> `- - - x`.

## Conditional: `give` / `otherwise`

```turflang
fn pick(a: int, b: int): int {
    return a > b give a otherwise b;
}

fn main(): int {
    printline(pick(3, 9));                            // 9
    mut n: int = 5;
    mut size: string = n > 3 give "big" otherwise "small";
    mut grade: int = n > 8 give 3 otherwise n > 4 give 2 otherwise 1;
    printline(size);
    return grade + (n > 0 give 1 otherwise 0);
}
```

`cond give a otherwise b` evaluates only the branch that is chosen. `otherwise`
is required (E110 without it), and the two branches must have the same type
(`give 1 otherwise "x"` is E066). It is the loosest operator, looser than
assignment, so parenthesise it when it is not the whole right side: `r = c give a
otherwise b;` assigns `c` first. Chains nest to the right, as the `grade`
example shows.

## String operators

`string` has no operator behaviour built into the compiler. `+`, `==`, `!=`,
`<`, `>`, `<=` and `>=` on two strings are ordinary `operator` overloads in the
standard library (the `strings` module), which is always loaded, so they work
in every file with no import.

- `+` concatenates and returns a new string.
- `==` and `!=` compare the **contents**, not the pointers. The ordering
  operators compare byte by byte.
- Both operands of `+` must be strings. `"id " + 5` is E057: convert with
  `5 to string`.

```turflang
fn main(): int {
    mut who: string = "ab";
    mut label: string = "Score: " + (100 to string) + " / " + (200 to string);
    mut s: string = who;
    s += "c";                            // s = s + "c"
    printline(label);
    printline(s == "abc");               // true
    printline("Z" < "a");                // true: byte order
    return 0;
}
```

Indexing, `s[i]`, is part of the language and gives a `char`. Its bounds rules
are described in [Variables and types](variables_and_types.md#indexing-a-string).

## Operators on your own types

The built-in rules above apply to the built-in types. For a struct or enum you
declare, the arithmetic, comparison, bitwise, unary and indexing operators can
be defined with `operator` declarations, covered in [Generics and operator
overloading](generics_and_operators.md#operator-overloading). How they meet
the built-ins:

- **The operand types must match exactly.** There is no widening: with
  `operator +(a: V, b: int)` declared, `v + 1` works and `v + 1.0` is E057.
- **`&&`, `||`, `=`, `.`, `()` and `to` cannot be overloaded.**
- **Compound assignment and `++` / `--` use the overload.** `a += b` is
  `a = a + b`, and `c++` on a `Counter` calls `operator +(Counter, int)` with
  `1`. There is no `operator +=`.
- **`!x` calls `operator !` when `x`'s type declares one.** Otherwise it is
  `x != true`.
- **Compound assignment and `++` / `--` through `operator []` are rejected**
  (E126). `box[i] += 1` and `box[i]++` must be written `box[i] = box[i] + 1;`.
- **At least one operand must be a type your own module declares**, and an
  operator whose operands are all built-in numbers is rejected (E028).

```turflang
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

fn main(): int {
    mut c: Counter = Counter::default();
    c++;
    ++c;
    c += 5;
    c--;
    return c.n;                  // 6
}
```

---

See also: [Variables and types](variables_and_types.md) for the primitive types,
literals, overflow rules and the full `to` conversion table.
