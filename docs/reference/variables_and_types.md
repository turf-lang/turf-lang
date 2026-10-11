# Variables and Types

This page covers variable declarations, default initialization, the primitive
type system, literals, explicit casting with `to`, and the `sizeof`/`typeof`
operators.

## Variable declarations

Turf has four declaration keywords, but really only two distinct forms:

```turflang
mut a: int = 10;
mutable b: int = 20;
immut x: int = 3;
immutable y: int = 4;
```

`mut` and `mutable` are **exact synonyms** — they lex to the identical token
and are handled by identical code. The same is true of `immut` and
`immutable`. There is no semantic difference between the short and long
spellings; use whichever you prefer, even inconsistently within a file.

### Syntax

```
(mut | mutable | immut | immutable) <name> : <Type> [= <expr>] ;
```

> **Note — no type inference.** The `: Type` annotation is mandatory. Turf has
> no `mut x = 5;` form that infers the type from the initializer. Omitting the
> colon is a parse error ("Expected ':' after variable name"). The
> initializer expression's type must still be *compatible* with the declared
> type, but the type itself is always written out explicitly.

`void` cannot be used as a variable's type — `mut x: void;` is a dedicated
compile error.

### `immut`/`immutable` require an initializer

An immutable binding can never be assigned to after its declaration, so it
must receive its one and only value at the declaration site:

```turflang
immut x: int;   // ERROR
```

```
Immutable variable 'x' requires an explicit initializer.
Hint: 'immutable'/'immut' variables can never be assigned to later, so they
must get their only value right here.
```

This applies to *every* type: `immut c: char;` fails with the exact same
error as `immut x: int;`, even though `mut c: char;` compiles fine.

## Default initialization

A `mut` variable declared without an initializer follows **one rule for every
type** (`FREEZE.md` §6.4): it must be definitely assigned before it is read.

### Definite assignment

Declaring without an initializer leaves the variable **unassigned**. The
compiler statically tracks whether a variable has been assigned on every
path, and rejects any *read* that occurs before an assignment (E008):

```turflang
mut x: int;
mut y: int = x;   // ERROR: definite-assignment violation, x read before assignment
```

Declare-then-assign-then-use is fine:

```turflang
mut x: int;
x = 5;
mut y: int = x;   // OK
```

This is a purely static, compile-time check — it does not mean the variable
holds garbage memory underneath (see the note at the end of this section).

The same holds for `char`, `int32`, structs and arrays; an older design gave
those a readable default at the declaration, and that mechanism is gone:

<!-- doc-example: fails E008 -->
```turflang
mut c: char;
mut n: int = c to int;   // ERROR: E008, c read before assignment
```

For a struct, assign the whole value, most simply its zero value with
[`T::default()`](structs_and_enums.md#construction-new-and-default). Writing
one field of an unassigned struct (`mut p: Point; p.x = 4;`) is E008 too:
struct *fields* are never tracked on their own, since a struct value is
either fully constructed by `::new` or fully zeroed by `::default`.

An **array** is tracked as a whole. It is unassigned until it is assigned
whole or **any one element** is written; from then on every element can be
read, and the ones never written are zero:

```turflang
struct Point { x: int, y: int }

fn main(): int {
    mut p: Point = Point::default();   // every field zeroed, readable at once

    mut arr: int[3];
    // mut first: int = arr[1];        // ERROR: E008, no element written yet
    arr[0] = 10;
    mut x: int = arr[1];               // OK: 0
    return p.x + x;
}
```

### Default value table

The value storage holds before its first assignment. Definite assignment
keeps a whole variable from being read in that state, but it is what an
array's unwritten elements and `T::default()`'s fields read as.

| Type | Zero value |
|---|---|
| `int`, `int32` | `0` |
| `double` | `0.0` |
| `bool` | `false` |
| `char` | byte `0` / `'\0'` |
| `string` | `""` |
| pointer / generic-param / function type | null pointer |
| array | every element its own zero value |
| struct | every field its own zero value, recursively |

> **Note.** The memory Turf allocates for a bare declaration is always
> zero-filled. Definite assignment is a static safety net over memory that
> already reads as zero, not protection against garbage.

## Primitive types

Turf has six true primitive types plus `void`:

| Type | Size | Numeric for coercion purposes? | Notes |
|---|---|---|---|
| `int` | 64-bit | yes | Signed. |
| `int32` | 32-bit | yes | Exists mainly to describe real C ABI types in `extern fn` declarations. |
| `double` | 64-bit float | yes | |
| `bool` | 1 bit | yes (by classification) but see below — no real numeric coercion | |
| `char` | 1 byte | **no** | A raw byte (0–255), not a Unicode codepoint. |
| `string` | pointer to bytes | no | C-style string pointer. |
| `void` | — | no | Only legal as a function return type. |

### `int`

A 64-bit signed integer. Widens implicitly to `double`. Arithmetic (`+ - * /
%`) between two `int`s (or `int`/`bool`) produces `int`; if either operand is
`double`, the whole expression promotes to `double`.

### `int32`

`int32` is **not** a second parallel arithmetic type — it exists so that
`extern fn` declarations can honestly describe a real C ABI 32-bit `int`
parameter or return value (the motivating bug: a C `strcmp` declared with a
Turf `int` return type silently misread the return register, because a real C
`int` is 32 bits, not 64).

```turflang
extern fn strcmp(a: string, b: string): int32;
```

- **Widens implicitly** to `int` and to `double` — `mut w: int = an_int32_value;`
  just works.
- **Narrowing `int → int32` requires an explicit cast.** `mut n: int32 = 5;`
  is a compile error; write `5 to int32` instead.
- Any arithmetic or comparison involving `int32` — with another `int32` or
  with a plain `int` — **promotes straight to `int`**. `int32` is never
  itself an arithmetic result type.
- A declared-but-uninitialized `int32` local is auto-defaulted and
  immediately readable (Mechanism B above), unlike a plain `int`.

### `double`

IEEE double precision. `int → double` widens implicitly; `double → int`
requires an explicit `to int` cast, which truncates toward zero.

### `bool`

> **Note — `bool` has almost no cast surface at all.** `bool` is classified
> as "numeric" internally (this is what lets `&&`/`||` share the same
> operand-checking path as other numeric comparisons), but it is deliberately
> isolated from real numeric coercion:
>
> - There is **no implicit** `bool ↔ int`/`double` coercion. `mut x: int =
>   true;` is rejected.
> - There is **no explicit** cast either. `b to int` where `b: bool` fails
>   at compile time with "Cannot explicitly cast 'bool' to 'int'" — the
>   explicit `to` cast does not bridge this gap, unlike most type pairs where
>   `to` can force a conversion implicit coercion won't do.
> - The **only** supported cast is `bool → string`, producing `"true"` or
>   `"false"`. There is no `string → bool` cast in the other direction.

```turflang
mut flag: bool = true;
mut s: string = flag to string;   // "true" — OK, the one supported cast
mut n: int = flag to int;          // ERROR — no such cast exists
```

### `char`

A 1-byte primitive — not sugar over `int` or `string`. `char` deliberately
does not participate in numeric coercion at all, which is what keeps it out
of every arithmetic/comparison path that other numeric types share.

```turflang
mut c: char = 'a';
```

- Supports `== != < > <= >=` as raw byte-value comparison (no locale or
  Unicode awareness).
- **No arithmetic whatsoever.** `'a' + 'b'` and `'a' + 1` are both compile
  errors — you must cast to `int` first.
- **No implicit coercion to/from `int`.** Even `'a' == 1` is rejected.
- **Explicit casts exist only between `char` and `int`, in both directions:**

  ```turflang
  mut n: int = 'a' to int;   // zero-extend, unsigned byte 0-255
  mut c2: char = 97 to char; // truncate
  ```

  `char → int` zero-extends (so it always yields an unsigned value 0–255,
  never a signed C `char`). `int → char` truncates.

> **Note.** There is **no cast between `char` and `string`, `double`, or
> `bool`** — none of the six combinations exist. `c to string` fails with a
> "cannot convert to string" error rather than silently doing something
> unexpected.

A declared-but-uninitialized `char` gets an immediate default of byte `0`
(Mechanism B above) and is readable right away, unlike `int`/`double`/`bool`/
`string`.

### `string`

A `string` is a pointer to a C-style byte buffer. Unlike most languages'
string operators, `+ == != < > <= >=` on `string` are **not hardcoded in the
compiler** — they are ordinary `operator` overloads defined in the standard
library and brought into scope automatically because the module that defines
them is one of the auto-imported prelude modules. See
[Operators § String operators](operators.md#string-operators) for details.

Indexing a string with `s[i]` *is* a compiler-level construct — see the same
section.

### `void`

Legal only as a function's return type. `mut x: void;` is a dedicated
compile-time error.

## Literals

### Integer literals

A run of decimal digits with no decimal point: `42`, `0`, `1000`. There is no
hex/octal/binary literal syntax, no digit-group separators (`_`), and no
integer literal type suffixes.

### Double literals

A run of digits containing exactly one `.`, e.g. `3.14`, `0.5`. A leading
bare `.` only starts a number if immediately followed by a digit — `.5`
parses as the double `0.5`, but a lone `.` elsewhere is the field-access/
method-call operator. `..` and `...` (range/ellipsis) are recognized before
the number scanner runs, so `1..5` doesn't get misparsed as a double.

### Bool literals

`true` and `false` are keywords.

### Char literals

Exactly one character, or one escape sequence, between single quotes:

```turflang
mut c: char = 'a';
mut newline: char = '\n';
```

- `''` — "Empty char literal."
- `'ab'` — "Char literal must contain exactly one character."
- `'a` (no closing quote before end of line/EOF) — "Unterminated char literal"

### String literals

Double-quoted, and **cannot span multiple physical lines** — the scanner
stops at a closing `"`, EOF, or a raw newline, so a forgotten closing quote
is caught immediately instead of silently swallowing the rest of the file:

```
Unterminated string literal
Hint: Add a closing '"' before the end of the line -- Turf string literals
can't span multiple lines. Need an actual line break inside the string? Use
the \n escape instead.
```

#### Escape sequences

The following escapes are recognized in both string and char literals:

| Escape | Byte | Escape | Byte |
|---|---|---|---|
| `\n` | newline (10) | `\a` | bell (7) |
| `\t` | tab (9) | `\b` | backspace (8) |
| `\r` | CR (13) | `\f` | form feed (12) |
| `\0` | NUL (0) | `\v` | vertical tab (11) |
| `\\` | backslash (92) | `\'` | single quote (39) |
| `\"` | double quote (34) | | |

Any other escape sequence (e.g. `\q`) is a compile-time error naming the
recognized set. This is a real, single-byte substitution — `\r` produces one
CR byte, not the two characters `\` and `r`:

```turflang
immut withCR: string = "a\rb";
test::assert(strings::length(withCR) == 3, "\\r must be exactly one byte");
test::assert(withCR != "arb", "\\r must not silently become a literal 'r'");
```

### `null`

`null` is only compatible with pointer-like types (`ptr`, `void*`, `T*`).
It's never compatible with plain numeric types, `bool`, or `string`, and it
is explicitly excluded from safe reference types (`&T`), since a `&T`'s
entire purpose is a non-null guarantee.

## Casting with `to`

`to` is the sole explicit-cast mechanism in Turf:

```turflang
mut i: int = 65;
mut d: double = i to double;
mut s: string = i to string;
```

> **Note.** There is no `int(x)`/`double(x)` function-call cast syntax —
> `to` is the only way to convert between types explicitly.

`to` binds tighter than assignment but looser than every other real binary
operator (see the [precedence table](operators.md#precedence) in the
operators reference), so `y = x to int;` parses as `y = (x to int)`.

### Legal conversions

| From → To | Behavior |
|---|---|
| `int ↔ double` | Standard int/float conversion (also implicit `int → double`) |
| `int32 ↔ int` | Sign-extend / truncate (`int32 → int` also implicit) |
| `int32 ↔ double` | Standard conversion (`int32 → double` also implicit) |
| `char ↔ int` | Zero-extend / truncate |
| `enum ↔ int` | Zero-extend / reinterpret |
| `string ↔ int` | Parses/formats the number; **runtime panic on an unparseable `string → int`** |
| `string ↔ double` | Parses/formats the number; no runtime validation on `string → double` |
| `int → string` | Formats the number as text |
| `double → string` | Formats the number as text |
| `bool → string` | `"true"` / `"false"` |
| `enum ↔ string` | Matches by variant name; **runtime panic on an unrecognized `string → enum`** |
| `int ↔ pointer` | Reinterprets the integer as an address and vice versa |
| pointer ↔ pointer (incl. `ptr`/`string`) | Reinterpret cast |
| pointer-to-struct → struct (by value) | Implicit dereference |

### Not supported at all

`bool ↔ int`, `bool ↔ double`, `char ↔ string`, `char ↔ double`,
`char ↔ bool`, `string → bool`, and any other pair not listed above. These
fail at compile time — `to` does not bypass type-system restrictions beyond
the conversions the compiler explicitly implements.

> **Note.** The type checker itself does essentially no legality checking on
> `to` — a cast expression always type-checks. All real enforcement of which
> conversions are legal happens later, during code generation, which is why
> an illegal cast still produces a clear compile-time error rather than
> succeeding silently or crashing.

Raw-pointer casts performed via `to` inside `unsafe { }` blocks, and
`sizeof`/enum-literal-range checks, are enforced as part of the same cast
machinery — see [Pointers and references](../architecture/pointers_and_references.md)
for the `unsafe` rules.

## `sizeof` and `typeof`

These look like a matched pair but have fundamentally different signatures.

### `sizeof(T)` — takes a type, returns its size in bytes

`sizeof` takes a **type**, not a value expression, and evaluates to a
compile-time `int` constant giving that type's size in bytes:

```turflang
unsafe {
    mut p: Node* = malloc(sizeof(Node)) to Node*;
    mut q: int* = malloc(sizeof(int) * 5);
}
```

### `typeof(expr)` — takes an expression, returns its static type name

`typeof` takes a **value expression** and returns a `string` naming its
*compile-time* type — `"int"`, `"double"`, `"string"`, `"bool"`, or e.g.
`"int[2]"` for a sized array. Turf has no runtime type information, so this
is always the type the compiler inferred for the expression at compile time,
not anything computed dynamically:

```turflang
test::assert(typeof(10) == "int", "");
test::assert(typeof("hello") == "string", "");
test::assert(typeof(3.14) == "double", "");
test::assert(typeof(true) == "bool", "");
test::assert(typeof([1, 2]) == "int[2]", "");
```

`typeof` is not a keyword — it's a built-in function callable without a
module prefix, evaluated directly against the argument's static AST type.

---

See also: [Operators](operators.md) for the full operator reference,
including `+`/`==`/etc. on `string` and the complete precedence table.
