# `math` and `os`

Math functions (constants, libm bindings, and checked wrappers) and process
environment access (env vars, argv/argc). Neither module is prelude —
both require an explicit `import`.

```turflang
import math;
import os;
```

---

## `math`

### Constants

| Constant | Value |
|---|---|
| `PI: double` | `3.14159265358979323846` |
| `E: double` | `2.71828182845904523536` |

```turflang
test::assert(math::PI > 3.14159 && math::PI < 3.14160, "...");
immut circumference: double = 2.0 * math::PI * 10.0;
```

### Raw libm bindings

Declared `extern from "m"` (auto-links `-lm`), all `(val: double): double`
except `pow(base, exp)`:

`pow`, `sqrt`, `sin`, `cos`, `tan`, `floor`, `ceil`, `round`, `fabs`, `log`,
`log2`, `log10`, `exp`.

Directly callable, e.g. `math::sqrt(2.0)`.

> **Note: two independent `floor`/`ceil`/`round` implementations.**
> `math::floor`, `math::ceil`, `math::round` here are the *raw libm*
> versions — a completely separate namespace and code path from
> `double.tr`'s pure-Turf, prelude, UFCS-callable `x.floor()` /
> `x.ceil()` / `x.round()` (see [`core.md`](./core.md)). They should behave
> identically (libm's `round()` is also half-away-from-zero) but are two
> deliberately separate implementations, not the same function reached two
> ways.

### `abs(val: double): double` / `abs(val: int): int`

`double` overload is `fabs(val)`; `int` overload is a manual
`if val < 0 { 0 - val }` (avoids a double round-trip for integers).

```turflang
mut abs1: int = math::abs(-10);
mut abs2: double = math::abs(-3.2);
```

### `power(base, exp): int`

Four overloads: `(int, int) -> int`, `(double, double) -> double`,
`(int, double) -> double`, `(double, int) -> double`. All route through
`pow()`, casting to/from `double` as needed; the `(int, int)` overload casts
the final result back to `int`.

> **Gotcha: not exact integer exponentiation.** Because the `(int, int)`
> overload truncates a floating-point `pow()` result back to `int`, results
> are libm's floating-point `pow`, truncated — not computed via exact
> integer multiplication. This is fine for the small values typically
> exercised (`2^8`) but could silently lose precision for larger bases or
> exponents due to floating-point rounding inside `pow()`.

```turflang
mut p: int = math::power(2, 8);
test::assert(p == 256, "...");
```

### `min(a, b)` / `max(a, b)`

Overloaded for `(int, int) -> int` and `(double, double) -> double`.
Ordinary `a < b ? a : b` / `a > b ? a : b`.

### `clamp(val, min_val, max_val)`

Overloaded for `(int, int, int) -> int` and
`(double, double, double) -> double`. Clamps `val` into `[min_val, max_val]`.

### `error MathError { DivisionByZero }`

### `divide(a: int, b: int): !int` / `divide(a: double, b: double): !double`

Recoverable alternative to the raw `/` operator, which hard-panics (aborts
the whole process) on division by zero. Produces `MathError::DivisionByZero`
when `b` is `0`/`0.0`, else returns `a / b`.

```turflang
mut a: int = math::divide(10, 2) recover produce -1;   // 5
mut b: int = math::divide(10, 0) recover produce -1;   // -1, recovered
mut d: double = math::divide(1.0, 0.0) recover produce -1.0;
```

Also usable directly as a function-call argument:

```turflang
test::assert_eq(math::divide(20, 4) recover produce -1, 5, "...");
```

---

## `os`

Environment variables and argv/argc, following the same
raw-binding-plus-wrapper split as the rest of the stdlib.

### `env(name: string): string`

Wraps `getenv`. **Normalizes libc's `NULL`-on-unset return to `""`**, so
callers never have to null-check — matching the convention `strings::
contains`/`index_of` use for `strstr`'s result.

```turflang
test::assert(os::env("TURF_TEST_DEFINITELY_UNSET_VAR_239") == "",
             "reading an unset environment variable returns \"\", not a null crash");
```

### `error OsError { SetEnvFailed(name: string) }` / `set_env(name: string, value: string, overwrite: int): !void`

Wraps `setenv`. `overwrite` is a C-style int flag: `0` means do not clobber
an existing value (and still succeeds), nonzero means overwrite it. Fails
with `OsError::SetEnvFailed(name)` for a name `setenv` rejects (empty, or
containing `=`). **Changed in 2.0 (LEC-007):** this returned `bool`.

```turflang
os::set_env("TURF_TEST_VAR_239", "hello", 1) recover run { test::fail("set_env"); };
os::set_env("TURF_TEST_VAR_239", "should_not_overwrite", 0); // no-op, value stays "hello"
os::set_env("TURF_TEST_VAR_239", "updated", 1);              // clobbers -> "updated"
```

### `argc(): int`

Counts like C's `argc` — includes `argv(0)` (the program's own invocation
path). Backed by `TurfRuntimeGetArgc()`, a C-ABI helper the compiler
synthesizes directly in LLVM IR at codegen time for `main`'s real
argc/argv (there's no Turf syntax to declare `main(argc, argv)` parameters
directly). Only valid inside a `main()` the compiler itself compiled as the
entry point.

```turflang
test::assert(os::argc() >= 1, "argc() includes at least argv(0)");
```

### `argv(index: int): string`

Backed by `TurfRuntimeGetArgv(index)`. Already bounds-checked by the
synthesized helper — **returns `""` for an out-of-range index, never
crashes.**

```turflang
test::assert(os::argv(0) != "", "argv(0) is the program's own invocation path, never empty");
test::assert(os::argv(999999) == "", "an out-of-range argv index returns \"\", not a crash");
```
