# Core: `int`, `double`, `bool`, `char`, `array`, `strings`

These six modules make up **the entire Turflang prelude** — the compiler
synthesizes `import int; import double; import bool; import char; import
strings; import array;` as the first lines of whatever top-level file you
compile, so every symbol documented on this page is available with **zero
explicit imports**. This is the complete list; nothing else is prelude.

> **Note.** `strings` is prelude even though its own header comment doesn't
> say so and its usage example shows `import strings;`. Every other prelude
> module (`int`, `double`, `bool`, `char`, `array`) explicitly documents
> itself as "part of the prelude" in a header comment; `strings.tr` doesn't,
> but it's synthesized into scope exactly the same way, confirmed directly
> in the compiler's `PreludeModuleNames` list. Writing `import strings;`
> anyway is harmless and arguably clearer — it's just never required.

All functions on this page are **UFCS-callable**: `n.is_even()` and
`int::is_even(n)` call the same function. Examples below mix both styles to
match how the standard library itself and the test suite use them.

---

## `int`

Small helpers that Turf's built-in arithmetic and comparison operators don't
cover directly. Deliberately does **not** duplicate `abs`/`min`/`max`/`clamp`/
`power` — those live in [`math`](./math_and_os.md), which requires an
explicit `import math;`.

### `is_even(n: int): bool`

`n % 2 == 0`.

```turflang
mut four: int = 4;
test::assert(four.is_even(), "4 is even");
```

### `is_odd(n: int): bool`

`n % 2 != 0`.

```turflang
test::assert(three.is_odd(), "3 is odd");
```

### `is_positive(n: int): bool`

`n > 0`. Zero is not positive.

### `is_negative(n: int): bool`

`n < 0`. Zero is not negative.

### `is_zero(n: int): bool`

`n == 0`.

### `gcd(a: int, b: int): int`

Greatest common divisor via the Euclidean algorithm, computed on `abs(a)`/
`abs(b)`. `gcd(0, 0)` returns `0`.

```turflang
test::assert_eq(twelve.gcd(18), 6, "gcd(12, 18) == 6");
```

### `lcm(a: int, b: int): int`

Least common multiple: `0` if either argument is `0`, else
`abs(a / gcd(a, b) * b)`.

```turflang
test::assert_eq(four.lcm(6), 12, "lcm(4, 6) == 12");
```

---

## `double`

Also deliberately minimal — `abs`/`min`/`max`/`clamp`/`power` live in
`math`. This module has no `is_nan` helper: `Lint.cpp`'s self-comparison
check (`x != x`) has no type information, so it would flag the standard
`x != x` NaN idiom as a copy-paste bug, and `double.tr` is compiled into
*every* program via the prelude — the false positive would show up
universally. There is currently no way to test for NaN through this module.

### `sign(x: double): double`

`1.0` if `x > 0.0`, `-1.0` if `x < 0.0`, else `0.0` (covers `x == 0.0`, and
incidentally NaN too, since neither comparison holds for NaN).

```turflang
test::assert_eq(five.sign(), 1.0, "sign(5.0) == 1.0");
test::assert_eq(negFive.sign(), 0.0 - 1.0, "sign(-5.0) == -1.0");
```

### `is_positive(x: double): bool` / `is_negative(x: double): bool` / `is_zero(x: double): bool`

Straightforward `> 0.0` / `< 0.0` / `== 0.0` comparisons, mirroring `int`'s helpers.

### `is_integer(x: double): bool`

`true` only if `x` has no fractional part, checked by truncating to `int`
and back: `((x to int) to double) == x`.

```turflang
test::assert(four.is_integer(), "4.0 is an integer");
test::assert(!fourFive.is_integer(), "4.5 is not an integer");
```

### `round(x: double): double`

Nearest integer, **half-away-from-zero** (not banker's rounding):
`x >= 0.0` rounds via `((x + 0.5) to int) to double`, negative `x` via
`((x - 0.5) to int) to double`.

```turflang
test::assert_eq(twoFive.round(), 3.0, "round(2.5) == 3.0");
test::assert_eq(negTwoFive.round(), 0.0 - 3.0, "round(-2.5) == -3.0");
```

### `floor(x: double): double`

Truncates toward zero (`to int` / `to double`), then subtracts `1.0` if `x`
was negative and truncation changed the value — this corrects for `int`
truncation rounding toward zero rather than down.

```turflang
test::assert_eq(negTwoSeven.floor(), 0.0 - 3.0, "floor(-2.7) == -3.0");
```

### `ceil(x: double): double`

Mirror image of `floor`: adds `1.0` if `x` was positive and truncation
changed the value.

```turflang
test::assert_eq(twoTwo.ceil(), 3.0, "ceil(2.2) == 3.0");
```

> **Note: two independent `floor`/`ceil`/`round` implementations exist.**
> `math.tr` also exports `floor`/`ceil`/`round` as raw `extern from "m"`
> libm bindings, under `math::floor`, `math::ceil`, `math::round` — a
> completely separate namespace from these UFCS methods (`x.floor()`, bare
> `floor(x)`). Both exist simultaneously without conflicting. They should
> behave identically (libm's `round()` is also half-away-from-zero) but
> they are genuinely separate code paths, not the same function reached two
> ways — don't assume a fix to one applies to the other. See
> [`math_and_os.md`](./math_and_os.md) for the libm versions.

---

## `bool`

Fills gaps Turf's built-in `&&`/`||`/`!` don't cover: there's no built-in
XOR (`^` is bitwise, `int`-only) and no implication operator.

### `logical_xor(a: bool, b: bool): bool`

`a != b`.

```turflang
test::assert(t.logical_xor(f), "true xor false == true");
```

### `logical_nand(a: bool, b: bool): bool`

`!(a && b)`.

### `logical_nor(a: bool, b: bool): bool`

`!(a || b)`.

### `implies(a: bool, b: bool): bool`

Material implication, `!a || b` ("if `a` then `b`"; vacuously `true` when
`a` is `false`).

```turflang
test::assert(f.implies(t), "false implies true == true");
test::assert(!t.implies(f), "true implies false == false");
```

---

## `char`

ASCII-only helpers, matching `char`'s 1-byte, no-Unicode design. Turf has no
built-in arithmetic on `char` — every helper below internally casts to `int`
(`c to int`) before comparing.

### `isDigit(c: char): bool`

`'0'..'9'` (ASCII 48–57).

### `isUpper(c: char): bool`

`'A'..'Z'` (ASCII 65–90).

### `isLower(c: char): bool`

`'a'..'z'` (ASCII 97–122).

### `isAlpha(c: char): bool`

`isUpper() || isLower()`.

### `isAlphaNumeric(c: char): bool`

`isAlpha() || isDigit()`.

```turflang
mut a: char = 'a';
mut nine: char = '9';
mut space: char = ' ';
test::assert(a.isAlphaNumeric(), "'a' is alphanumeric");
test::assert(nine.isAlphaNumeric(), "'9' is alphanumeric");
test::assert(!space.isAlphaNumeric(), "' ' is not alphanumeric");
```

### `isPunctuation(c: char): bool`

Printable ASCII (33–126) that is neither alpha nor digit — e.g. `.`, `,`,
`!`, `(`.

```turflang
mut bang: char = '!';
test::assert(bang.isPunctuation(), "'!' is punctuation");
```

### `isWhitespace(c: char): bool`

Space (32), tab (9), `\n` (10), or `\r` (13).

### `toUpper(c: char): char`

Subtracts 32 if `c` is lowercase; otherwise returns `c` unchanged
(non-letters pass through).

### `toLower(c: char): char`

Adds 32 if `c` is uppercase; otherwise returns `c` unchanged.

---

## `array`

UFCS helpers over unsized array parameters. Two hard constraints shape every
function here:

1. **UFCS is not generic.** `TypeChecker::tryUFCSFallback()`'s receiver
   matching is a plain, non-generic exact-element-type match, so there's one
   concrete overload per element type (`int[]`, `double[]`, `string[]`), not
   a single `fn<T>` version. Need another element type? Reach for
   [`List<T>`](./collections.md) instead — a real generic.
2. **An unsized array parameter erases its length.** `arr: int[]` (accepting
   an argument of any concrete length) loses its size entirely — calling
   `arr.length()` *inside* such a function returns `-1`, not the caller's
   real length, because `Size == -1` is baked into the parameter's static
   type and there's no runtime way to recover it. Every function below
   therefore takes an explicit `len: int` alongside the array. Call as
   `arr.sum(len)` or `array::sum(arr, len)`.

### `sum(arr: int[], len: int): int` / `sum(arr: double[], len: int): double`

Linear-scan sum over `arr[0..len)`.

```turflang
mut ints: int[5] = [3, 1, 4, 1, 5];
mut ilen: int = 5;
test::assert_eq(ints.sum(ilen), 14, "sum([3,1,4,1,5]) == 14");
```

### `max(arr: int[], len: int): int` / `max(arr: double[], len: int): double`

Starts `best = arr[0]`, then scans `i = 1..len` keeping the running maximum.

> **Gotcha: no zero-length guard.** Unlike `sum`/`contains`/`index_of`
> below, `max` and `min` seed their result by reading `arr[0]` before any
> length check. Calling `.max(0)` on an empty slice reads out of bounds
> instead of failing cleanly — always ensure `len >= 1` before calling.

```turflang
test::assert_eq(ints.max(ilen), 5, "max([3,1,4,1,5]) == 5");
```

### `min(arr: int[], len: int): int` / `min(arr: double[], len: int): double`

Mirror of `max`; same `len >= 1` assumption applies.

### `contains(arr, len: int, val): bool`

Overloaded for `int[]`, `double[]`, `string[]`. Linear scan for equality;
returns `false` when `len == 0` (the loop simply never runs — no special
case needed).

```turflang
test::assert(ints.contains(ilen, four), "[3,1,4,1,5] contains 4");
```

### `index_of(arr, len: int, val): int`

Overloaded for `int[]`, `double[]`, `string[]`. Linear scan, returns the
first matching index or `-1` if not found.

```turflang
test::assert_eq(ints.index_of(ilen, four), 2, "index_of(4) == 2");
test::assert_eq(ints.index_of(ilen, 0 - 1), 0 - 1, "index_of(missing) == -1");
```

---

## `strings`

`string` is an ordinary compiler-recognized type with **zero hardcoded
operator behavior** — `+`, `==`, `!=`, `<`, `>`, `<=`, `>=` on strings are
all just `operator` overloads defined in this file, resolved through the
same mechanism a user's own `operator` overload would use. Because they're
defined *in* `strings.tr`, every other function in this file is careful
never to use string `+`/`==` internally (that would recurse into itself) —
hence the manual byte-indexed loops you'll see if you read the source.

String ordering is byte-by-byte `strcmp` — there's no Unicode-aware sorting
(explicitly deferred to a hypothetical future `unicode` module).

### Operators: `+`, `==`, `!=`, `<`, `>`, `<=`, `>=`

- `+` concatenates: mallocs `strlen(a) + strlen(b) + 1` bytes and
  `strcpy`+`strcat`s into it. **The result is never freed** — there's no
  GC yet, so long-running programs that concatenate strings in a loop leak
  memory a bit at a time. This is a documented, deliberate tradeoff, not an
  oversight.
- `==`, `!=`, `<`, `>`, `<=`, `>=` are thin wrappers over `strcmp(a, b)`
  compared against `0`.

```turflang
immut greeting: string = "Hello, " + "world!";
if greeting == "Hello, world!" {
    io::printline("matched");
}
```

### `length(s: string): int`

Returns `strlen(s)`. This is now the **only** way to get a string's length
— the old bare `lengthof(s)` intrinsic has been removed entirely.

```turflang
test::assert(strings::length(r50) == 100, "repeat ab x50: length must be 100");
```

### `contains(s: string, needle: string): bool`

`strstr(s, needle) != null`. An empty `needle` always matches.

```turflang
test::assert(strings::contains("hello world", "world"), "...");
test::assert(strings::contains("hello world", "xyz") == false, "...");
test::assert(strings::contains("", "x") == false, "...");
test::assert(strings::contains("abc", "") == true, "contains: empty needle always true");
```

### `index_of(s: string, needle: string): int`

Byte offset of the first occurrence of `needle`, or `-1` if not found.

```turflang
test::assert(strings::index_of("hello world", "world") == 6, "index_of: world at 6");
test::assert(strings::index_of("", "a") == 0 - 1, "index_of: empty string");
```

### `starts_with(s: string, prefix: string): bool`

Byte-compares the first `strlen(prefix)` bytes of `s`; `false` if `prefix`
is longer than `s`. Equal strings count as a match.

```turflang
test::assert(strings::starts_with("abc", "abc"), "starts_with: full string");
test::assert(strings::starts_with("abc", "abcd") == false, "starts_with: longer prefix");
```

### `ends_with(s: string, suffix: string): bool`

Mirror of `starts_with`; compares the tail of `s`.

### `repeat(s: string, n: int): string`

Concatenates `n` copies of `s`. `n <= 0` returns `""`.

```turflang
mut r0: string = strings::repeat("hello", 0);
test::assert(r0 == "", "repeat hello x0 = empty");

mut r50: string = strings::repeat("ab", 50); // 100-char stress case
```

### `count_char(s: string, ch: int): int`

Counts occurrences of the byte `ch` (an `int` byte code, not a `char`) in
`s`.

### `substring(s: string, start: int, end: int): string`

Slices `[start, end)` — `start` inclusive, `end` exclusive.

> **Gotcha: hard-panics instead of returning `!string`.** If
> `start < 0 || end < start || end > strlen(s)`, `substring` calls
> `sys::panic("strings.substring: index out of bounds")` and aborts the
> process. This is inconsistent with the rest of the standard library's
> error-handling story — `math::divide`, `Map.tryGet`, and every `net`
> connection function return a recoverable `!T` for "ordinary expected
> failure," but a bad substring range is instead treated as a programming
> bug. Validate your indices before calling, or use `trim`, which never
> passes an out-of-range slice.

### `replace(s: string, old: string, new_str: string): string`

Replaces every non-overlapping occurrence of `old` with `new_str`. If `old`
is `""`, returns `s` unchanged (guards against matching everywhere and
looping forever).

```turflang
test::assert(strings::replace("aababaa", "a", "x") == "xxbxbxx", "replace: all a -> x");
test::assert(strings::replace("hello world", "o", "") == "hell wrld", "replace: delete all 'o'");
```

### `to_upper(s: string): string` / `to_lower(s: string): string`

Byte-by-byte case conversion via libc `toupper`/`tolower`. Non-alphabetic
bytes pass through unchanged.

```turflang
test::assert(strings::to_upper("hello123!") == "HELLO123!", "digits and punct unchanged");
```

### `trim(s: string): string`

Strips leading/trailing ASCII whitespace (space, tab, `\n`, `\r`) by
locating the trimmed range and delegating to `substring`. An all-whitespace
input returns `""`.

```turflang
mut all_space: string = strings::trim("     ");
test::assert(all_space == "", "trim: all-whitespace string -> empty");
test::assert(strings::trim("  hello world  ") == "hello world", "trim: interior space preserved");
```
