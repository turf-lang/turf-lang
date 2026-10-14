# Math and the operating system: `math`, `os`

| Module | For | Import |
|---|---|---|
| `math` | constants, `abs`, `min`, `max`, `clamp`, `power`, the libm functions, `divide` | none (prelude) |
| `os` | environment variables and command-line arguments | `import os;` |

`math` is part of the [prelude](README.md#the-prelude): `abs(-3)`, `max(a, b)` and
`sqrt(2.0)` work with no import, and so do `math::abs` and `math::sqrt`. Its
constants stay qualified (`math::PI`). `os` needs `import os;`.

---

## `math`

### Constants

| Constant | Value |
|---|---|
| `PI: double` | `3.14159265358979323846` |
| `E: double` | `2.71828182845904523536` |

### Functions from the C math library

These take a `double` and return a `double` (`pow` takes two). They are the C
library's own functions, so they follow its rules for the cases that have no
real answer: a result that is not a number is `nan`, one that is too large is
`inf` or `-inf`. None of them panics or returns an error.

| Function | Result |
|---|---|
| `pow(base: double, exp: double): double` | `base` raised to `exp` |
| `sqrt(val: double): double` | the square root; `sqrt(-1.0)` is `nan` |
| `sin(val)`, `cos(val)`, `tan(val)` | trigonometry, with the angle in radians |
| `floor(val)`, `ceil(val)` | round down, round up: `floor(-2.5)` is `-3`, `ceil(-2.5)` is `-2` |
| `round(val)` | round to the nearest whole number, halves away from zero: `round(2.5)` is `3`, `round(-2.5)` is `-3` |
| `fabs(val)` | the absolute value of a `double` |
| `log(val)`, `log2(val)`, `log10(val)` | natural, base-2 and base-10 logarithm; `log(0.0)` is `-inf`, `log(-1.0)` is `nan` |
| `exp(val)` | e raised to `val` |

`math::floor`, `math::ceil` and `math::round` are separate from the methods
`x.floor()`, `x.ceil()` and `x.round()` that [`double`](core.md) provides. Both
round the same way, but the methods panic for a NaN, an infinity or a value
beyond the range of an `int`, where the `math` functions return the value
unchanged.

```turflang
fn main(): int {
    immut radius: double = 2.5;
    printline("area " + (math::PI * math::power(radius, 2) to string));
    printline(math::sqrt(2.0));
    printline(math::floor(-2.5));
    printline(math::round(2.5));
    printline(math::log2(8.0));
    printline(math::sqrt(-1.0));
    return 0;
}
```

```text
area 19.635
1.41421
-3
3
3
nan
```

### `abs`

```text
abs(val: int): int
abs(val: double): double
```

The absolute value. `abs` of the smallest `int` (`-9223372036854775807 - 1`) has
no positive counterpart in an `int`, so it **panics** with `Integer overflow`.

### `min`, `max`

```text
min(a: int, b: int): int         max(a: int, b: int): int
min(a: double, b: double): double   max(a: double, b: double): double
```

The smaller and the larger of two values of the same type.

### `clamp`

```text
clamp(val: int, min_val: int, max_val: int): int
clamp(val: double, min_val: double, max_val: double): double
```

`val` limited to the range from `min_val` to `max_val`: `min_val` if `val` is
below it, `max_val` if above, else `val`. Pass `min_val <= max_val`; with the
bounds reversed the result is not meaningful.

### `power`

```text
power(base: int, exp: int): int
power(base: double, exp: double): double
power(base: int, exp: double): double
power(base: double, exp: int): double
```

`base` to the power `exp`. The overload follows the argument types.

All four are computed with floating-point `pow`. The `int, int` form converts the
result back to an `int`, which has consequences:

- It **truncates toward zero**. `power(2, -1)` is `0`, not `0.5`; use a `double`
  argument for fractional results.
- It is exact only while the result fits in a `double`'s 53 bits of precision.
  `power(10, 18)` is exact, but `power(7, 22)` is `3909821048582988288` where the
  true value is `3909821048582988049`. For exact large powers, multiply in a loop.
- A result that does not fit in an `int` **panics** with `Integer overflow`:
  `power(2, 63)` and `power(3, 40)` both do.

```turflang
fn main(): int {
    printline(math::power(2, 10));       // 1024
    printline(math::power(2.0, 0.5));    // 1.41421
    printline(math::power(2, -1));       // 0
    printline(math::power(2.0, -1));     // 0.5
    printline(math::min(3, 2));          // 2
    printline(math::max(1.5, 2.5));      // 2.5
    printline(math::clamp(15, 0, 10));   // 10
    printline(math::abs(-4));            // 4
    return 0;
}
```

### `error MathError` and `divide`

```text
error MathError { DivisionByZero }

divide(a: int, b: int): !int
divide(a: double, b: double): !double
```

The `/` operator on integers **panics** when the divisor is zero, which is right
when a zero divisor is a bug. `divide` is the form for a divisor that comes from
input: it returns `MathError::DivisionByZero` when `b` is `0` (or `0.0`) and `a / b`
otherwise. Integer division truncates toward zero, as `/` does:
`divide(-7, 2)` is `-3`.

```turflang
fn average(total: int, count: int): int {
    return math::divide(total, count) recover produce 0;
}

fn main(): int {
    printline(average(10, 4));   // 2
    printline(average(10, 0));   // 0
    immut ratio: double = math::divide(1.0, 3.0) recover as err run {
        eprintline("division by zero");
        return 1;
    };
    printline(ratio);            // 0.333333
    return 0;
}
```

### Failure summary

| Call | On a problem |
|---|---|
| `divide` | returns `MathError::DivisionByZero` |
| `abs(int)`, `power(int, int)` | **panic** (`Integer overflow`) when the result does not fit |
| the C math functions | return `nan` or `inf`; never fail |
| `min`, `max`, `clamp`, `abs(double)`, the other `power` forms | never fail |

---

## `os`

The program's environment and its command line. Not prelude.

```turflang
import os;
```

### `env(name: string): string`

The value of the environment variable `name`, or `""` if it is not set. A
variable that is set to the empty string also reads as `""`, so `env` cannot tell
"unset" from "empty". It never fails: reading the environment is a question
whose answer can be "nothing".

### `error OsError` and `set_env`

```text
error OsError { SetEnvFailed(name: string) }

set_env(name: string, value: string, overwrite: int): !void
```

Sets an environment variable for the current process and for any program it
starts afterwards. It does not change the shell that launched your program.

`overwrite` is a flag: nonzero replaces an existing value; `0` leaves an existing
value alone and **still succeeds**. It fails with `OsError::SetEnvFailed(name)`
when the system rejects the name: an empty name, or one that contains `=`.

```turflang
import os;

fn main(): int {
    os::set_env("GREETING", "hello", 1) recover run {
        eprintline("could not set GREETING");
        return 1;
    };
    os::set_env("GREETING", "ignored", 0) recover run {
        return 1;
    };
    printline(os::env("GREETING"));          // hello

    os::set_env("GREETING", "updated", 1) recover run {
        return 1;
    };
    printline(os::env("GREETING"));          // updated
    printline(os::env("NOT_SET_ANYWHERE"));  // (an empty line)

    os::set_env("BAD=NAME", "x", 1) recover as err run {
        compare err {
            matches os::OsError::SetEnvFailed(name):
            printline("rejected: " + name);
            done
            otherwise:
            printline("failed");
        }
    };
    return 0;
}
```

```text
hello
updated

rejected: BAD=NAME
```

### `argc(): int`

The number of command-line arguments **including** the program itself, like C's
`argc`. A program run with no arguments has `argc() == 1`.

### `argv(index: int): string`

The argument at `index`. `argv(0)` is the program's own invocation path, and
`argv(1)` is the first argument the user typed. Arguments are not parsed: a
quoted argument with spaces arrives as one string, and a flag such as `--name=x`
arrives as that text. An index that is negative or not less than `argc()` returns
`""`; it never panics.

`argc` and `argv` work in a program whose `main` is the entry point, which is
every program the compiler builds.

```turflang
import os;

fn main(): int {
    if os::argc() < 2 {
        eprintline("usage: " + os::argv(0) + " NUMBER...");
        return 2;
    }

    mut total: int = 0;
    mut i: int = 1;
    while i < os::argc() {
        immut n: int = strings::parse_int(os::argv(i)) recover run {
            eprintline("not a number: " + os::argv(i));
            return 1;
        };
        total = total + n;
        i = i + 1;
    }
    printline(total);
    return 0;
}
```

```text
$ ./sum 10 20 12
42
$ ./sum 10 x
not a number: x
$ ./sum
usage: ./sum NUMBER...
```

### Failure summary

| Call | On a problem |
|---|---|
| `set_env` | returns `OsError::SetEnvFailed(name)` |
| `env`, `argc`, `argv` | never fail |
