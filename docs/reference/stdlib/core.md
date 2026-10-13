# Core: `int`, `double`, `bool`, `char`, `array`, `strings`

These six modules, together with `io`, `math` and `sys` (documented in
[`io_and_net.md`](./io_and_net.md) and [`math_and_os.md`](./math_and_os.md)),
make up **the entire Turflang prelude** — the compiler synthesizes `import int;
import double; import bool; import char; import strings; import array; import io;
import math; import sys;` as the first lines of whatever top-level file you
compile, so every symbol documented on this page is available with **zero
explicit imports**. This is the complete list; nothing else is prelude. The
six modules on this page are reached through UFCS (`c.is_digit()`), operators
and `strings::length(s)`; only `io`, `math` and `sys` can also be called by
their bare names (`printline("hi")`, `abs(-3)`) — see
[Functions and Modules §4.5](../functions_and_modules.md).

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

> **Renamed in 2.0 (LEC-007).** These were `isDigit`, `isUpper`, `isLower`,
> `isAlpha`, `isAlphaNumeric`, `isPunctuation`, `isWhitespace`, `toUpper` and
> `toLower`. Every public name in the standard library is now `snake_case`.
> `char.to_upper` and `strings.to_upper` share a name and are told apart by
> their receiver (`'a'.to_upper()` vs `"abc".to_upper()`).

### `is_digit(c: char): bool`

`'0'..'9'` (ASCII 48–57).

### `is_upper(c: char): bool`

`'A'..'Z'` (ASCII 65–90).

### `is_lower(c: char): bool`

`'a'..'z'` (ASCII 97–122).

### `is_alpha(c: char): bool`

`is_upper() || is_lower()`.

### `is_alphanumeric(c: char): bool`

`is_alpha() || is_digit()`.

```turflang
mut a: char = 'a';
mut nine: char = '9';
mut space: char = ' ';
test::assert(a.is_alphanumeric(), "'a' is alphanumeric");
test::assert(nine.is_alphanumeric(), "'9' is alphanumeric");
test::assert(!space.is_alphanumeric(), "' ' is not alphanumeric");
```

### `is_punctuation(c: char): bool`

Printable ASCII (33–126) that is neither alpha nor digit — e.g. `.`, `,`,
`!`, `(`.

```turflang
mut bang: char = '!';
test::assert(bang.is_punctuation(), "'!' is punctuation");
```

### `is_whitespace(c: char): bool`

Space (32), tab (9), `\n` (10), or `\r` (13).

### `to_upper(c: char): char`

Subtracts 32 if `c` is lowercase; otherwise returns `c` unchanged
(non-letters pass through).

### `to_lower(c: char): char`

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
2. **Every function takes an explicit `len: int`.** These helpers predate
   unsized parameters knowing their length (issue #260) and keep their
   signatures. They look at `arr[0..len)`. A `len` larger than the array is
   now a runtime panic ("Array index out of bounds"), where it used to read
   past the end. Call as `arr.sum(len)` or `array::sum(arr, len)`; passing
   `arr.length()` as `len` covers the whole array. Your own functions don't
   need a `len` parameter: `for v in arr` and `arr.length()` work inside any
   function taking `arr: T[]`.

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

### `last_index_of(s: string, needle: string): int`

Byte offset of the **last** occurrence of `needle` in `s`, or `-1`. An empty
`needle` matches at `strlen(s)`, the mirror of `index_of("")` being `0`.
Overlapping matches count: `last_index_of("aaaa", "aa")` is `2`. Added in 2.0
(it was `fs`'s private helper).

```turflang
test::assert(strings::last_index_of("a/b/c", "/") == 3, "");
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

Concatenates `n` copies of `s`. `n <= 0` returns `""`. Linear in the result
(it was a quadratic `result = result + s` loop before 2.0).

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

> **Panics instead of returning `!string` — on purpose.** If
> `start < 0 || end < start || end > strlen(s)`, `substring` aborts with
> `<your file>:<your line>: strings.substring: index out of bounds`. A range
> your own code computed wrongly is a bug, not input that can be
> recovered from (see the [failure rule](README.md#2-failure-three-kinds-three-shapes)):
> `math::divide` and `parse_int` return `!T` because *their input* can be
> bad. Validate indices before calling, or use `trim`, which never passes an
> out-of-range slice.

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

### `trim(s: string): string` / `trim_start(s: string): string` / `trim_end(s: string): string`

Strips leading/trailing ASCII whitespace (space, tab, `\n`, `\r`) by
locating the trimmed range and delegating to `substring`. `trim` strips both
ends; `trim_start`/`trim_end` strip only one. An all-whitespace input
returns `""`.

```turflang
mut all_space: string = strings::trim("     ");
test::assert(all_space == "", "trim: all-whitespace string -> empty");
test::assert(strings::trim("  hello world  ") == "hello world", "trim: interior space preserved");
```

### `split(s: string, delimiter: string): List<string>`

Splits `s` on every occurrence of `delimiter`. An empty `delimiter`, or a
`delimiter` that never occurs in `s`, both return a single-element list
containing all of `s` unchanged (treated as "no real delimiter given," not
an error). Spelling the result type needs `import list::List;` (or
`import list;` and `list::List<string>` written out fully) since it returns a
generic collection; just using the returned value needs neither.

```turflang
import list::List;

mut octets: List<string> = strings::split("127.0.0.1", ".");
test::assert(octets.length() == 4, "split: 4 octets");
```

### `join(items: List<string>, sep: string): string`

The inverse of `split` — concatenates every element of `items` with `sep`
between them (no leading/trailing separator). An empty list returns `""`.

```turflang
import list::List;

mut parts: List<string> = List::default();
parts.push("a");
parts.push("b");
parts.push("c");
test::assert(strings::join(parts, "-") == "a-b-c", "join: dash-separated");
```

### `pad_left(s: string, target_len: int, pad_char: char): string` / `pad_right(s: string, target_len: int, pad_char: char): string`

Pads `s` with `pad_char` until it reaches `target_len` bytes — on the left
or right respectively. If `s` is already `target_len` bytes or longer, it's
returned unchanged (never truncated).

```turflang
test::assert(strings::pad_left("7", 3, '0') == "007", "pad_left: zero-pad");
test::assert(strings::pad_right("hi", 5, '.') == "hi...", "pad_right: dot-fill");
```

### `parse_int(s: string): !int` / `parse_double(s: string): !double`

Parses `s` as a number. Returns the recoverable
`error StringParseError { InvalidInt, InvalidDouble }` on malformed input —
an empty string, a bare `-`, a non-digit byte, or (for `parse_double`) a
malformed decimal point or exponent — rather than panicking, since a bad
parse is an ordinary, expected outcome (e.g. user input, a config file), not
a programming bug.

`parse_double` accepts an optional `-`, digits with at most one `.` (at
least one digit), and an optional exponent: `e` or `E`, an optional sign,
digits (`1e10`, `2.5E-3`). Nothing else: no leading `+` or space, no `inf`,
`nan` or hex. The value is the correctly rounded `double`, the same one
`s to double` and the compiler's literal give; past the `double` range it is
`inf` or `-inf`.

```turflang
immut n: int = strings::parse_int("42") recover produce -1;
test::assert(n == 42, "parse_int: ordinary case");

immut bad: int = strings::parse_int("not a number") recover produce -1;
test::assert(bad == -1, "parse_int: malformed input recovers to -1");
```

### `StringBuilder`

Incremental string construction. A `string` is an immutable NUL-terminated
pointer, so `s = s + piece` in a loop copies the whole accumulated string on
every pass — quadratic. (Measured: 200,000 pieces took 1.9 s, 600,000 took
23 s.) A `StringBuilder` appends in amortised constant time per byte; `repeat`,
`replace` and `join` are built on it, and so should any loop of yours that
builds a string.

```turflang
mut sb: strings::StringBuilder = strings::StringBuilder::default();
sb.append("hello");
sb.append_char(' ');
sb.append("world");
immut s: string = sb.to_string();   // "hello world"
```

| Method | |
|---|---|
| `append(s: string): void` | adds every byte of `s` |
| `append_char(c: char): void` | adds one byte |
| `reserve(extra: int): void` | makes room for `extra` more bytes up front (panics on a negative `extra`); an optimisation only |
| `length(): int` | bytes appended so far |
| `clear(): void` | forgets the contents, keeps the buffer — reuse one builder per loop iteration |
| `to_string(): string` | a **copy** of the contents, `""` when empty. Appending afterwards never changes a string already returned |

The buffer, length and capacity are private: the buffer's NUL terminator is an
invariant only the methods keep.
