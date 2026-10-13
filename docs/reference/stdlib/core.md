# Core: `int`, `double`, `bool`, `char`, `array`, `strings`

These six modules are part of the **prelude**: they are loaded into every
program, and nothing needs importing to use them. Together with `io`, `math`
and `sys` they make up the nine prelude modules listed in the
[standard library overview](README.md#the-prelude). `io` and `sys` are
documented in [I/O, networking and `sys`](io_and_net.md); `math` is in
[`math` and `os`](math_and_os.md).

How you reach a function depends on the module:

- Every function of `int`, `double`, `bool`, `char`, `array` and `strings` is
  called as a **method** on its first argument (this is
  [UFCS](../functions_and_modules.md#3-ufcs-calling-a-function-as-a-method)):
  `n.is_even()`, `c.is_digit()`, `s.trim()`, `a.sum(3)`.
- `strings::name(...)` and `array::name(...)` also work, with no import,
  because the prelude modules are always in scope for the `::` qualifier. `int`,
  `double`, `bool` and `char` are type keywords, so their functions are reached
  as methods only (`n.gcd(m)`, never `int::gcd(n, m)`).
- They are not callable by a bare name: `is_digit(c)` is E041 (unknown
  function). Only `io`, `math` and `sys` are bare-callable.

A method call on a literal works the same way: `4.gcd(6)`, `'a'.is_upper()`,
`"hi".length()`. A negative literal needs parentheses: `(-12).gcd(18)`.

---

## `int`

Small helpers that the arithmetic and comparison operators do not cover.
`abs`, `min`, `max`, `clamp` and `power` are in [`math`](math_and_os.md).

| Function | Result |
|---|---|
| `is_even(n: int): bool` | `n % 2 == 0` |
| `is_odd(n: int): bool` | `n % 2 != 0` (so `true` for negative odd numbers too) |
| `is_positive(n: int): bool` | `n > 0` |
| `is_negative(n: int): bool` | `n < 0` |
| `is_zero(n: int): bool` | `n == 0` |
| `gcd(a: int, b: int): int` | greatest common divisor of `abs(a)` and `abs(b)`; `gcd(0, 0)` is `0` |
| `lcm(a: int, b: int): int` | least common multiple, never negative; `0` if either argument is `0` |

`gcd` and `lcm` panic with `Integer overflow` when the result does not fit in
an `int` (for example `lcm` of two large coprime numbers, or `gcd` of the
smallest `int`).

```turflang
fn main(): int {
    mut n: int = 12;
    printline(n.is_even());      // 1
    printline(n.gcd(18));        // 6
    printline(4.lcm(-6));        // 12
    printline((-12).gcd(18));    // 6
    printline(0.gcd(0));         // 0
    printline(0.lcm(6));         // 0
    return 0;
}
```

(`printline` prints a `bool` as `1` or `0`.)

---

## `double`

| Function | Result |
|---|---|
| `sign(x: double): double` | `1.0`, `-1.0` or `0.0` by the sign of `x`; `0.0` for zero and for NaN |
| `is_positive(x: double): bool` | `x > 0.0` |
| `is_negative(x: double): bool` | `x < 0.0` |
| `is_zero(x: double): bool` | `x == 0.0` (true for `-0.0`) |
| `is_integer(x: double): bool` | `true` if `x` has no fractional part |
| `round(x: double): double` | nearest whole number, halves away from zero |
| `floor(x: double): double` | largest whole number not above `x` |
| `ceil(x: double): double` | smallest whole number not below `x` |

`round` is half-away-from-zero, not banker's rounding: `2.5.round()` is `3.0`
and `(-2.5).round()` is `-3.0`.

`is_integer`, `round`, `floor` and `ceil` work by converting through `int`, so
they panic with `Integer overflow` for a NaN, an infinity, or a magnitude
beyond `int`'s range (about 9.2e18). For such values use the `math` versions,
`math::floor`, `math::ceil` and `math::round`, which are libm functions that
return the value unchanged; they are separate functions from the methods on
this page and live in [`math`](math_and_os.md). There is no `is_nan`; test for
NaN with `x != x`.

```turflang
fn main(): int {
    mut x: double = -2.5;
    printline(x.sign());         // -1
    printline(x.round());        // -3
    printline(x.floor());        // -3
    printline(x.ceil());         // -2
    printline(4.0.is_integer()); // 1
    printline(4.5.is_integer()); // 0
    printline(2.7.floor());      // 2
    return 0;
}
```

---

## `bool`

Turf has no `xor` or implication operator (`^` is bitwise and takes `int`).

| Function | Result |
|---|---|
| `logical_xor(a: bool, b: bool): bool` | `a != b` |
| `logical_nand(a: bool, b: bool): bool` | `!(a && b)` |
| `logical_nor(a: bool, b: bool): bool` | `!(a \|\| b)` |
| `implies(a: bool, b: bool): bool` | `!a \|\| b`: "if `a` then `b`", true whenever `a` is false |

Like `&&` and `||`'s operands, all four evaluate both arguments (they are
ordinary functions).

```turflang
fn main(): int {
    mut t: bool = true;
    mut f: bool = false;
    printline(t.logical_xor(f)); // 1
    printline(t.logical_xor(t)); // 0
    printline(f.implies(t));     // 1
    printline(t.implies(f));     // 0
    printline(f.logical_nor(f)); // 1
    return 0;
}
```

---

## `char`

ASCII-only helpers for the one-byte `char`. A `char` has no arithmetic of its
own; convert with `c to int` and `n to char` (see
[casting](../variables_and_types.md#casting-with-to)).

| Function | Result |
|---|---|
| `is_digit(c: char): bool` | `'0'` to `'9'` |
| `is_upper(c: char): bool` | `'A'` to `'Z'` |
| `is_lower(c: char): bool` | `'a'` to `'z'` |
| `is_alpha(c: char): bool` | `is_upper` or `is_lower` |
| `is_alphanumeric(c: char): bool` | `is_alpha` or `is_digit` |
| `is_punctuation(c: char): bool` | printable ASCII (33 to 126) that is neither a letter nor a digit: `.`, `,`, `!`, `(` |
| `is_whitespace(c: char): bool` | space, tab, `\n` or `\r` |
| `to_upper(c: char): char` | the uppercase letter for a lowercase one; any other `char` unchanged |
| `to_lower(c: char): char` | the lowercase letter for an uppercase one; any other `char` unchanged |

`char.to_upper` and `strings.to_upper` have the same name; the receiver picks
one: `'a'.to_upper()` is a `char`, `"abc".to_upper()` a `string`.

```turflang
fn main(): int {
    mut c: char = 'q';
    printline(c.to_upper());              // Q
    printline('9'.is_alphanumeric());     // 1
    printline(' '.is_alphanumeric());     // 0
    printline('!'.is_punctuation());      // 1
    printline('\t'.is_whitespace());      // 1

    // Counting digits in a string with s[i], which is a char.
    mut s: string = "a1b22c333";
    mut digits: int = 0;
    mut i: int = 0;
    while i < s.length() {
        if s[i].is_digit() {
            digits++;
        }
        i++;
    }
    printline(digits);                    // 6
    return 0;
}
```

---

## `array`

Helpers over `int[]`, `double[]` and `string[]` parameters (an unsized array
parameter accepts an array of any length; see [Arrays](../arrays.md)). There
is one overload per element type rather than one generic function. For any
other element type, use [`List<T>`](collections.md), which is generic.

Every function takes a `len: int` after the array and looks only at the first
`len` elements: `a.sum(3)` or `array::sum(a, 3)`. Pass `a.length()` to cover the
whole array. A `len` larger than the array panics with `Array index out of
bounds`. (Functions you write yourself need no `len` parameter: `a.length()` and
`for v in a` work on any `T[]`.)

| Function | Result |
|---|---|
| `sum(arr: int[], len: int): int` | sum of `arr[0..len)` |
| `sum(arr: double[], len: int): double` | sum of `arr[0..len)` |
| `max(arr: int[], len: int): int` | the largest of `arr[0..len)` |
| `max(arr: double[], len: int): double` | the largest of `arr[0..len)` |
| `min(arr: int[], len: int): int` | the smallest of `arr[0..len)` |
| `min(arr: double[], len: int): double` | the smallest of `arr[0..len)` |
| `contains(arr: int[], len: int, val: int): bool` | whether `val` is in `arr[0..len)` |
| `contains(arr: double[], len: int, val: double): bool` | same, for `double` |
| `contains(arr: string[], len: int, val: string): bool` | same, comparing string contents |
| `index_of(arr: int[], len: int, val: int): int` | index of the first match, or `-1` |
| `index_of(arr: double[], len: int, val: double): int` | same, for `double` |
| `index_of(arr: string[], len: int, val: string): int` | same, comparing string contents |

`sum` of `len <= 0` is `0`, `contains` is `false` and `index_of` is `-1`.
`max` and `min` start from `arr[0]`, so they have no meaning for `len < 1`:
`len == 0` returns `arr[0]`, and an array with no elements (such as an empty
slice, `a[0..0]`) panics with `Array index out of bounds`.

```turflang
fn main(): int {
    mut ints: int[5] = [3, 1, 4, 1, 5];
    printline(ints.sum(5));            // 14
    printline(ints.max(5));            // 5
    printline(ints.min(ints.length())); // 1
    printline(ints.contains(5, 4));    // 1
    printline(ints.index_of(5, 1));    // 1
    printline(ints.index_of(5, 9));    // -1
    printline(array::sum(ints, 3));    // 8: only the first three

    mut names: string[3] = ["ann", "bob", "cy"];
    printline(names.contains(3, "bob")); // 1
    printline(names.index_of(3, "cy"));  // 2

    printline(ints[1..4].sum(3));      // 6: a slice is an array too
    return 0;
}
```

---

## `strings`

A `string` is a pointer to NUL-terminated bytes; every function here counts
and indexes **bytes**, not Unicode characters. `"héllo".length()` is `6`.
Ordering is byte order, with no locale or Unicode collation.

Every function that returns a new string returns a fresh string on the
garbage-collected heap (see [Memory and safety](../memory_and_safety.md)); the
argument is never modified.

### Operators

`+` concatenates; `==`, `!=`, `<`, `>`, `<=` and `>=` compare contents, byte by
byte. They are ordinary `operator` overloads defined in this module and work in
every file. `"id " + 5` is E057: write `"id " + (5 to string)`. See
[Operators: string operators](../operators.md#string-operators).

### Indexing: `s[i]`

`s[i]` is the byte at `i` as a `char`, bounds-checked: valid indices are `-n` to
`n - 1`, a negative index counts from the end, and anything else panics with
`String index out of bounds`. Convert with `s[i] to int` for the number. A
write `s[i] = c;` is checked only below zero. The rules, with examples, are in
[Variables and types: indexing a string](../variables_and_types.md#indexing-a-string).

Each `s[i]` read measures the string, so a loop over every byte of a long
string spends time proportional to the length on every pass. When the loop
has already established the index is in range, use `byte_at_unchecked`.

### Length and raw access

| Function | Result |
|---|---|
| `length(s: string): int` | the number of bytes in `s`; `s.length()` |
| `strlen(s: string): int` | the C `strlen` binding that `length` wraps; prefer `length` |
| `byte_at_unchecked(s: string, i: int): char` | the byte at `i` with **no bounds check**; an out-of-range `i` is undefined behaviour |

### Searching

| Function | Result |
|---|---|
| `contains(s: string, needle: string): bool` | whether `needle` occurs in `s`; an empty `needle` always matches |
| `index_of(s: string, needle: string): int` | byte offset of the first occurrence, or `-1`; `index_of(s, "")` is `0` |
| `last_index_of(s: string, needle: string): int` | byte offset of the last occurrence, or `-1`; an empty `needle` matches at `s.length()`; overlapping matches count (`"aaaa".last_index_of("aa")` is `2`) |
| `starts_with(s: string, prefix: string): bool` | whether `s` begins with `prefix` (`false` if `prefix` is longer than `s`) |
| `ends_with(s: string, suffix: string): bool` | whether `s` ends with `suffix` |
| `count_char(s: string, ch: int): int` | how many bytes of `s` equal the byte code `ch` (an `int`: `s.count_char('l' to int)`) |

### Building new strings

| Function | Result |
|---|---|
| `substring(s: string, start: int, end: int): string` | the bytes `[start, end)`; `start` is included, `end` is not |
| `repeat(s: string, n: int): string` | `n` copies of `s` joined; `""` when `n <= 0` |
| `replace(s: string, old: string, new_str: string): string` | `s` with every non-overlapping `old` replaced, scanning left to right; `s` unchanged if `old` is `""` |
| `to_upper(s: string): string` | ASCII letters uppercased; every other byte unchanged |
| `to_lower(s: string): string` | ASCII letters lowercased |
| `trim(s: string): string` | without leading and trailing whitespace (space, tab, `\n`, `\r`); `""` if all whitespace |
| `trim_start(s: string): string` | without leading whitespace only |
| `trim_end(s: string): string` | without trailing whitespace only |
| `pad_left(s: string, target_len: int, pad_char: char): string` | `s` preceded by `pad_char` until it is `target_len` bytes; never truncates |
| `pad_right(s: string, target_len: int, pad_char: char): string` | the same, padding after `s` |
| `split(s: string, delimiter: string): List<string>` | the pieces of `s` between occurrences of `delimiter`, empty pieces included; see below |
| `join(items: List<string>, sep: string): string` | the elements of `items` with `sep` between them; `""` for an empty list |

**`substring` panics on a bad range.** If `start < 0`, `end < start` or `end >
s.length()`, the program aborts with `<file>:<line>: strings.substring: index
out of bounds`, naming the call in *your* code. A range you computed wrongly is
a bug in the caller, not an outcome to recover from (see the
[failure rule](README.md#2-failure-three-kinds-three-shapes)). Unlike `s[i]`,
`substring` does not accept negative indices.

**`split`** returns a single-element list holding all of `s` when `delimiter`
is `""` or does not occur in `s`, so `s.split(d).length() == 1` means "no
delimiter found". `"a,,b".split(",")` is `["a", "", "b"]`, and `join` with the
same separator rebuilds `"a,,b"`. Naming the result type needs `import
list::List;` ([collections](collections.md)); a method can also be called on
the result directly, as in `s.split(d).length()`:

```turflang
import list::List;

fn main(): int {
    mut parts: List<string> = "a,,b".split(",");
    printline(parts.length());              // 3
    printline(parts.get(1) == "");          // 1
    printline(strings::join(parts, ";"));   // a;;b

    mut none: List<string> = "abc".split("x");
    printline(none.length());               // 1
    return 0;
}
```

```turflang
fn main(): int {
    printline("hello world".contains("world"));      // 1
    printline("hello".index_of("l"));                // 2
    printline("a/b/c".last_index_of("/"));           // 3
    printline("abc".starts_with("abcd"));            // 0
    printline("abc".ends_with("bc"));                // 1
    printline("hello".count_char('l' to int));       // 2

    printline("hello".substring(1, 3));              // el
    printline("ab".repeat(3));                       // ababab
    printline("aababaa".replace("a", "x"));          // xxbxbxx
    printline("Hello, World".to_upper());            // HELLO, WORLD
    printline("[" + "  hi \t\n".trim() + "]");       // [hi]
    printline("[" + "  hi  ".trim_start() + "]");    // [hi  ]
    printline("[" + "  hi  ".trim_end() + "]");      // [  hi]
    printline("7".pad_left(3, '0'));                 // 007
    printline("hi".pad_right(5, '.'));               // hi...
    printline("toolong".pad_left(3, '0'));           // toolong
    return 0;
}
```

### Parsing: `parse_int` and `parse_double`

| Function | Result |
|---|---|
| `parse_int(s: string): !int` | `s` as a base-10 integer with an optional leading `-` |
| `parse_double(s: string): !double` | `s` as a decimal number, with an optional exponent |

Both fail with `error StringParseError { InvalidInt, InvalidDouble }`
(`InvalidInt` from `parse_int`, `InvalidDouble` from `parse_double`) because
text that is not a number is ordinary input, not a bug in the caller. They
reject an empty string, a bare `-`, a leading `+` or space, and any other
trailing or embedded byte. `"007"` parses as `7`.

`parse_double` takes an optional `-`, digits with at most one `.` (at least one
digit in all), and an optional exponent: `e` or `E`, an optional sign, digits
(`"1e10"`, `"2.5E-3"`, `".5"`, `"5."`). There is no `inf`, `nan` or hex form.
The result is the correctly rounded `double`, the same value the compiler gives
the matching literal; past the `double` range it is `inf` or `-inf`.

`parse_int` also fails with `InvalidInt` for a number outside `int`'s range;
`"-9223372036854775808"` and `"9223372036854775807"` parse.

```turflang
fn describe(text: string): void {
    immut n: int = strings::parse_int(text) recover as err run {
        if err == strings::StringParseError::InvalidInt {
            printline("not an integer: [" + text + "]");
        }
        return;
    };
    printline(n * 2);
}

fn main(): int {
    describe("21");        // 42
    describe("-7");        // -14
    describe("12a");       // not an integer: [12a]
    describe("");          // not an integer: []

    immut x: double = strings::parse_double("2.5E-3") recover produce 0.0;
    printline(x);          // 0.0025
    immut bad: int = strings::parse_int("nope") recover produce -1;
    printline(bad);        // -1
    return 0;
}
```

`recover` is described in [Error handling](../error_handling.md#recover-resolving-a-fallible-value).

### Converting to and from other types

The `to` operator converts between strings and the numeric types
([casting](../variables_and_types.md#casting-with-to) has the full table).

| Expression | Result |
|---|---|
| `n to string`, `x to string`, `b to string` | the text of an `int`, `double` (shortest `%g`-style form: `3.5`, `1e+20`, `0.333333`) or `bool` (`true` / `false`) |
| `s to int` | parses an integer; **panics** (`Invalid string to int conversion`) if `s` is not one |
| `s to double` | parses a number like C `strtod`; text that is not a number gives `0.0`, and trailing text is ignored (`"12abc"` is `12.0`), so it never fails |
| `c to int`, `n to char` | the byte code of a `char`, and back |

Use `parse_int` / `parse_double` when the text is input you do not control,
since they report failure as a value; `s to int` is for text you know is a
number.

```turflang
fn main(): int {
    printline(65 to string);       // 65
    printline(3.5 to string);      // 3.5
    printline(true to string);     // true
    printline("42" to int);        // 42
    printline("3.25" to double);   // 3.25
    printline(65 to char);         // A
    printline('A' to int);         // 65
    return 0;
}
```

### `StringBuilder`

`s = s + piece` in a loop copies the whole accumulated string every pass, so
building a long string that way takes time proportional to the square of its
length. A `StringBuilder` appends in amortised constant time per byte. `repeat`,
`replace` and `join` use one, and so should any loop of yours that builds a
string.

```turflang
fn main(): int {
    mut sb: strings::StringBuilder = strings::StringBuilder::default();
    sb.append("hello");
    sb.append_char(' ');
    sb.append("world");
    immut s: string = sb.to_string();
    sb.append("!");                     // s is unchanged
    printline(s);                       // hello world
    printline(sb.to_string());          // hello world!
    printline(sb.length());             // 12

    sb.clear();
    mut i: int = 0;
    while i < 5 {
        sb.append(i to string);
        sb.append_char(',');
        i++;
    }
    printline(sb.to_string());          // 0,1,2,3,4,
    return 0;
}
```

| Method | Behaviour |
|---|---|
| `append(s: string): void` | adds every byte of `s` |
| `append_char(c: char): void` | adds one byte |
| `reserve(extra: int): void` | makes room for `extra` more bytes in one allocation, an optimisation only; panics (`StringBuilder.reserve: negative size`, at the calling line) for a negative `extra` |
| `length(): int` | bytes appended so far |
| `clear(): void` | empties the builder but keeps its storage, for reuse in a loop |
| `to_string(): string` | a **copy** of the contents, `""` when empty; appending afterwards never changes a string already returned |

A `StringBuilder` is a struct and is passed by value like any struct: a
function that should append to the caller's builder takes `&strings::StringBuilder`
and is called with `&sb`. Its fields are private.
