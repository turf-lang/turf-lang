# Operators

This page covers every operator category in Turf, their precedence and
associativity, and the operators built on top of `string`.

## Precedence

The table below is ordered from **loosest to tightest** binding (a lower
number binds looser). This is the full, verified precedence chain used by
the parser:

| Precedence | Operators | Associativity |
|---|---|---|
| 10 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` `**=` | right |
| 12 | `to` (cast) | left |
| 15 | `\|\|` | left |
| 20 | `&&` | left |
| 25 | `==` `!=` | left |
| 30 | `<` `>` `<=` `>=` | left |
| 33 | `\|` (bitwise OR) | left |
| 35 | `^` (bitwise XOR) | left |
| 37 | `&` (bitwise AND) | left |
| 38 | `<<` `>>` | left |
| 40 | `+` `-` (binary) | left |
| 50 | `*` `/` `%` | left |
| 60 | `**` (exponentiation) | right |

A few things worth calling out explicitly:

- **Bitwise operators sit between comparisons and arithmetic** — that is,
  arithmetic binds tighter than bitwise operators, and bitwise operators bind
  tighter than comparisons. This differs from C, where bitwise operators bind
  looser than comparisons; double-check parenthesization if you're porting
  C-like expressions.
- **`to` binds tighter than assignment but looser than everything else** —
  deliberately, so that `y = x to int;` parses as `y = (x to int)` and not
  `(y = x) to int`. This also applies inside a compound-assignment RHS:
  `z += w to int;` casts `w` before adding.
- **`**` (exponentiation) is right-associative**, like assignment, and unlike
  every other binary operator (including its lookalike `*`, which is
  left-associative). `2 ** 3 ** 2` groups as `2 ** (3 ** 2) == 512`, not
  `(2 ** 3) ** 2 == 64`.
- **Unary operators bind tighter than every binary operator, uniformly** —
  including `**`. There is no special-case the way Python or JavaScript give
  `**` unusual unary-minus interaction: `-2 ** 2` groups as `(-2) ** 2 == 4`
  in Turf.

> **Note.** There is no C-style `cond ? a : b` ternary syntax. Turf's only
> ternary form is `give`/`otherwise` (§ [Ternary](#ternary-give--otherwise)
> below), and it binds looser than every operator in the table above — even
> looser than assignment.

## Assignment: `=`

Standard assignment. The left-hand side must be a mutable, assignable
location — assigning to an `immut`/`immutable` binding, or to a variable
before its declaration, is a compile error.

## Arithmetic: `+ - * / %`

```turflang
mut a: int = 10;
mut b: int = 20;
mut max_val: int = a + b;
```

Two numeric operands combine to `double` if either side is `double`,
otherwise to `int` (so `int`/`bool`-adjacent arithmetic yields `int`).

`%` (modulo) works on `double` operands too, going through the same generic
arithmetic path as `+ - * /` — it is not restricted to `int` operands the way
the bitwise operators are.

> **Note — runtime division/modulo-by-zero check.** `/` and `%` both insert
> a genuine **runtime** check that panics with `"Division by zero"` if the
> divisor is zero, for both `int` and `double` operands. This is not purely
> a compile-time concern, and it means float division by zero panics rather
> than silently producing `inf`/`NaN`.

## Exponentiation: `**`

```turflang
mut r: double = 2 ** 10;   // 1024
```

`**` is numeric (accepts `int` or `double` operands) and always produces
`double`, regardless of the operand types. It is right-associative (see the
precedence table above).

> **Note.** `^` does **not** mean exponentiation in Turf — it means bitwise
> XOR (below). If you're used to a language or older Turf docs where `^` was
> power, that has been fully migrated to `**`; treat any reference to `^` as
> exponentiation as stale.

## Comparison: `== != < > <= >=`

Standard relational and equality comparisons on numeric types. Enums are
strictly typed with no coercion between enum values and other types. On
`string`, these are stdlib operator overloads, not compiler primitives — see
[String operators](#string-operators) below.

## Logical: `&& || !`

```turflang
mut e: int = 0;
mut ok: bool = (e != 0) && (100 / e > 5);   // RHS never evaluated when e == 0
```

`&&` and `||` are genuine **short-circuiting** operators — the right operand
is not evaluated unless necessary, which is safe to rely on for guard
patterns like the one above. Both operands must be numeric-classified types.

`!x` is desugared to `x != true`, so `!!f == f` and `!!!f == !f` hold for any
boolean-valued `f`.

## Bitwise: `& | ^ ~ << >>`

```turflang
mut m: int = a & b;
mut n: int = a | b;
mut x: int = a ^ b;   // XOR, not exponentiation
mut c: int = ~a;
mut l: int = a << 2;
mut r: int = a >> 2;
```

> **Note — strictly `int`-only.** Every bitwise operator, including unary
> `~`, requires genuine integer operands. Using a `double` operand with any
> bitwise operator is a hard compile error — there is no implicit truncation.

- `>>` is an **arithmetic** (sign-extending) right shift, not a logical one:
  `-8 >> 1 == -4`, `-1 >> 1 == -1`.
- `~x == -(x + 1)` (standard two's-complement bitwise NOT); `~0 == -1`.
- `^` is XOR only. Exponentiation is the separate `**` operator (see above).

## Compound assignment

Eleven forms, all right-associative, all parsed at the same precedence as
plain `=`:

```
+=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=  **=
```

That's every arithmetic operator, every bitwise/shift operator, and `**=`.
There is **no `~=`** — bitwise NOT is unary-only and has no compound form,
same as in C.

The bitwise compound forms (`&= |= ^= <<= >>=`) inherit the same int-only
restriction as their plain counterparts. `**=` is the interesting exception:
unlike the bitwise compound forms, it does **not** require `int` operands —
like `**` itself, it accepts any numeric operand, computes through a
`double` intermediate, and then narrows the result back to the left-hand
side's own declared type for storage:

```turflang
mut b: int = 3;
b **= 4;               // 81 -- double pow() result narrowed back to int for storage

mut d: int = 2;
mut exp: double = 3.0;
d **= exp;              // 8 -- RHS type need not match LHS's
```

## Increment/decrement: `++`/`--`

Both prefix and postfix forms exist, and they are implemented quite
differently.

### Prefix (`++x`, `--x`)

Requires a **bare identifier** immediately after the operator — `++(a)` and
`++arr[0]` are not legal syntax ("Expected identifier after prefix ++ or
--"). Prefix `++`/`--` desugars directly to `x = x + 1` / `x = x - 1`, so it
goes through the exact same immutability checks as any other assignment.

```turflang
mut j: int = 5;
mut k: int = - ++j;   // j becomes 6 first, then negated: k == -6
```

### Postfix (`x++`, `x--`)

Parsed as part of the general postfix chain, alongside `[]`, `()`, and `.`,
so it can syntactically apply to any postfix-chainable expression. Unlike
prefix, it is not desugared to a plain assignment internally.

> **Note — tokenization trap.** `--x` lexes as a single atomic token, not two
> separate unary minuses. This means `---x` is parsed as `--` (prefix
> decrement) followed by `-x` (unary minus) — **not** three chained
> negations. To write three chained unary minuses, put spaces between them:
> `- - - a`.

## Ternary: `give` / `otherwise`

```turflang
mut a: int = 10;
mut b: int = 20;
mut max_val: int = a > b give a otherwise b;   // 20
```

`cond give a otherwise b` is Turf's only ternary form — there is no C-style
`cond ? a : b` syntax. It desugars directly to an if/else used as an
expression, and it is parsed after the entire binary-operator chain, making
it the loosest-binding construct in an expression — looser even than
assignment.

## String operators

`string` has no compiler-hardcoded operator behavior. `+ == != < > <= >=` on
two `string` values are ordinary `operator` overloads defined in the
standard library, brought into scope automatically because that module is
one of the auto-imported prelude modules — practically indistinguishable
from a built-in language feature, but implemented in Turf itself rather than
hardcoded in the compiler:

```turflang
operator +(a: string, b: string): string { /* concatenation */ }
operator ==(a: string, b: string): bool { return strcmp(a, b) == 0; }
// also != < > <= >=
```

- `+` is concatenation.
- `==`/`!=` and the four relational operators are byte-wise (`strcmp`-based)
  comparisons.
- Mixing a non-`string` operand into `+` requires an explicit `to string`
  cast first — `"hi" + 5` and `"hi" + 3.14` are both rejected:

```turflang
mut score_msg: string = "Score: " + (100 to string) + " / " + (200 to string);
mut status_msg: string = "ID_" + (999 to string) + "_Active_" + (true to string);
```

> **Note.** String concatenation heap-allocates its result and never frees
> it (Turf has no garbage collector yet). This is a memory-model detail, not
> a language-level concern for this page — see
> [docs/architecture/pointers_and_references.md](../architecture/pointers_and_references.md)
> for more.

### String indexing `s[i]`

Unlike the other string operators, indexing **is** a genuine compiler-level
construct, not a stdlib overload. It reads one raw byte at offset `i` and
zero-extends it to a full `int` — so `s[i]` has static type `int`, not
`char`, even though it's indexing into byte data:

```turflang
mut s: string = "hello";
mut c0: int = s[0];
test::assert(c0 == 104, "s[0] should be 104 ('h')");   // note: int, not char
test::assert("hello"[1] == 101, "direct literal indexing also works");
```

---

See also: [Variables and types](variables_and_types.md) for the `to` cast
operator's full conversion table, primitive types, and literals.
