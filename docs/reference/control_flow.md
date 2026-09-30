# Control Flow

Reference for Turflang's control-flow constructs: `if`/`elseif`/`else`, the `give`/`otherwise` ternary, `while`, `do`/`while`, `for` (range and for-each forms), `break`/`continue`, `defer`, and `compare`/`matches`/`otherwise`.

Conditions in every construct below are bare expressions — no parentheses required (though parentheses are accepted for grouping, e.g. `while (x > 0)`).

---

## `if` / `elseif` / `else`

```turflang
if x > 10 {
    result = 3;
} elseif x > 5 {
    result = 2;
} elseif x > 0 {
    result = 1;
} else {
    result = -1;
}
```

- Braces around each branch body are **mandatory** — there is no brace-less `if`.
- Chain additional conditions with `elseif` (one word). This is the language's sanctioned spelling; use it rather than a two-word `else if`.
- `else` is optional. When present it can be a block or, in principle, any expression — but idiomatic Turflang code always writes `else { ... }`.
- An empty body is legal: `if cond {} else { ... }` is sometimes used to avoid a dangling `elseif`/`else` chain.

```turflang
if j == 3 {
    continue;
} else {
}
```

> **Note:** If you leave off the opening brace after a condition, the parser's error message points you at `give`/`otherwise` (below) as the single-expression alternative — that construct, not a brace-less `if`, is how Turflang expresses a conditional in expression position.

---

## Ternary expression: `give` / `otherwise`

For a single-expression conditional (as opposed to a multi-statement branch), use `give`/`otherwise`:

```turflang
mut max_val: int = a > b give a otherwise b;
```

Reads as: *if `a > b`, give `a`; otherwise, `b`*. It evaluates to a value and can be used anywhere an expression is expected — inside a variable initializer, a function argument, a return statement, and so on.

> **Note — keyword reuse:** `otherwise` appears in three unrelated places in Turflang: this ternary, the catch-all arm of `compare` (below), and nowhere else. The ternary's `otherwise` and `compare`'s `otherwise:` arm share nothing but the spelling — don't read significance into the overlap. There is no third meaning.

---

## `while`

```turflang
while i < 5 {
    sum = sum + i;
    i = i + 1;
}
```

Standard pre-test loop: the condition is checked before each iteration, and the loop body (braces mandatory) may run zero or more times.

---

## `do { ... } while (cond);`

```turflang
do {
    x = x - 1;
    iterations = iterations + 1;
} while (x > 0);
```

Parentheses around the condition are optional, even for compound conditions:

```turflang
do {
    a = a + 1;
    b = b - 1;
} while a < 5 && b > 0;
```

The body always runs **at least once** — the condition is checked *after* the body, on every pass, including the first.

> **Gotchas:**
> - The body must be braced (`do { ... }`) — there is no brace-less `do`.
> - The `while` keyword is required after the closing `}`.
> - Unlike every other loop form, `do`/`while` requires a **trailing `;`** after the condition, since it doesn't self-delimit with a final `}`. Forgetting it is an error; adding an extra stray `;` after a valid `do`/`while` is also an error.
> - The condition is checked in the *outer* scope, not inside the body's block scope — a variable declared only inside the body is **not visible** to the condition, matching C/C++ `do`/`while` semantics:
>   ```turflang
>   do {
>       mut finished: bool = true;
>   } while finished;   // ERROR: `finished` is not visible here
>   ```
> - `break`/`continue` inside a `do`/`while` target that loop specifically, correctly isolated from any loop it's nested inside or that's nested inside it.

---

## `for` — range form

```turflang
for i in 1..10 step i += 1 {
    sum = sum + i;
}
```

`i` ranges over `1..10` (inclusive of the start, exclusive of the end, evaluated left to right), and `step` runs after each iteration of the body.

> **`step` is currently mandatory.** There is no shorthand that defaults to `+= 1` — every range-based `for` must spell out its own `step` clause. Omitting it is a parse error: `Expected 'step' after the range end value.`

The step expression can be any expression, most commonly one that advances the loop variable:

```turflang
for i in 0..10 step i++ { ... }
for x in 0.0..1.0 step x += 0.1 { ... }   // double-typed range works too
```

The loop variable's type defaults to `int`; if the range's bounds are `double`, the loop variable is `double` instead.

> **Note — `step` doesn't have to touch the loop variable.** This compiles:
> ```turflang
> for i in 0..10 step j += 10 {
>     ...
> }
> ```
> Since `i` itself is never advanced, this is almost certainly a bug (or an infinite loop, unless something else in the body advances or exits the loop) — the compiler emits a `StuckForLoopVariableWarning` for it, but it is not rejected outright. Treat this warning as a real signal.

> **Note:** the `(index, value)` pair form (below) is only for for-each — it is not accepted on a range-based `for`.

---

## `for` — for-each form

Iterates a sized array (`T[N]`) value in order, either binding just the element or the `(index, value)` pair:

```turflang
for value in numbers {
    sum = sum + value;
}

for (index, value) in numbers {
    indexSum = indexSum + index;
    valueSum = valueSum + value;
}
```

- `index` is always `int`, starting at `0`.
- `value`'s type is the array's element type — this works for arrays of primitives, structs, enums, and nested arrays (iterating an `int[2][3]`, each `row` is itself an `int[3]` you can for-each again).

```turflang
for row in matrix {          // matrix: int[2][3]
    for cell in row {        // row: int[3]
        total = total + cell;
    }
}
```

**What you can't for-each over:**

| Value | Result |
|---|---|
| A scalar (e.g. `int`) | rejected — not an array |
| A bare `struct` value | rejected — not an array |
| `List<T>` | rejected — it's a generic struct, not a fixed-size array, even though it behaves like a collection |
| An unsized array parameter (`arr: int[]`) | rejected separately — the array has no compile-time-known length to bound the loop with |

```turflang
fn sumAll(arr: int[]): int {
    for value in arr {   // ERROR: unsized array parameter has no bound
        ...
    }
}
```

Using the same name for both loop variables in the paired form (`for (x, x) in numbers`) is also rejected, as an ordinary duplicate-declaration error.

---

## `break` / `continue`

Bare keywords, no arguments and no labels — there is no way to break or continue an outer loop by name from an inner one.

- `break` exits the innermost enclosing loop entirely.
- `continue` skips to the next iteration. For a range-based `for`, `continue` still runs the loop's `step` clause before re-checking the condition — a `for` loop is, conceptually, a `while` loop with the step folded in after the body.
- Using `break` or `continue` outside any loop is a compile error (`'break' used outside of a loop.`).

```turflang
for i in 0..10 step i += 1 {
    if i == 3 {
        continue;
    }
    if i == 7 {
        break;
    }
    sum = sum + i;
}
```

`break`/`continue` work identically inside for-each loops, including nested combinations of range-`for`, for-each, `while`, and `do`/`while`.

---

## `defer`

`defer <statement>;` registers a statement to run when the **enclosing `{ }` block** exits — not the enclosing function. It runs on every exit path out of that block: falling off the end normally, `return`, `break`, and `continue` alike.

```turflang
fn readConfig(path: string): int {
    mut f: File = fs::open(path);
    defer fs::close(f);

    if (!fs::exists(path)) {
        return -1;   // fs::close(f) still runs before this return completes
    }

    return fs::readInt(f);
}
```

This is Turflang's mechanism for deterministic cleanup of things a garbage collector must never manage on its own — file descriptors, sockets, manual allocations (`mem::free`).

- **Block-scoped, not function-scoped.** A `defer` inside a loop body's `{ }` runs once **per iteration**, not once for the whole function:

  ```turflang
  mut i: int = 0;
  while (i < 3) {
      defer io::printline("iteration end");   // prints 3 times, once per pass through the loop body
      i = i + 1;
  }
  ```

- **Reverse (LIFO) order.** Multiple `defer` statements in the same block run in the opposite order they were registered — the last one written runs first:

  ```turflang
  defer io::printline("1");
  defer io::printline("2");
  defer io::printline("3");
  // prints: 3, 2, 1
  ```

- **Nesting.** Exiting several nested blocks at once (e.g. a `return` three blocks deep) runs every enclosing block's own pending `defer`s, innermost block first.

- **A deferred statement may not itself `return`, `break`, or `continue`.** There is no way for such a jump to target "just the defer" — it would always escape to the enclosing function or loop, which is never what it looks like from the call site. This is rejected at compile time:

  ```turflang
  defer return 5;   // ERROR: a deferred statement can't 'return'
  ```

  A `break`/`continue` **inside a loop that itself begins inside the deferred statement** is fine — it's absorbed by that inner loop and never escapes the `defer` at all:

  ```turflang
  defer while (cond) { if (done) { break; } };   // OK -- the break only exits this inner while
  ```

- **A panic does not run deferred statements.** Turflang has no unwinding — a panic (an out-of-bounds access, a division by zero, an integer overflow trap, a failed `test::assert`, …) aborts the process immediately. This is the main semantic difference from Go's `defer`, where a reader familiar with that language should not assume cleanup runs on a crash path here.

---

## `compare` / `matches` / `otherwise`

`compare` is a multi-way pattern match over a subject value:

```turflang
compare x {
    matches 1:
        result = 10;
    done

    matches 2:
        result = 20;
    done

    otherwise:
        result = 99;
}
```

Multiple `matches` headers can share one body by stacking them (a "fallthrough" group, not chained execution):

```turflang
compare x {
    matches 1:
    matches 2:
    matches 3:
        result = 100;
    done

    otherwise:
        result = 999;
}
```

**Syntax rules:**

- The subject accepts optional parentheses: `compare x { ... }` and `compare (x) { ... }` are both valid.
- Each `matches` value also accepts optional parentheses: `matches 1:` or `matches (1):`.
- Every `matches` arm's body must end with `done`. `otherwise`'s body does not use `done` — it simply runs to the closing `}` of the whole `compare` block, which is why `otherwise` must be the **last** arm.
- `otherwise` is entirely optional. If it's absent and no `matches` arm matches the subject, the whole `compare` is a no-op.
- Duplicate `otherwise` arms are rejected, as is placing `otherwise` anywhere but last.
- An empty `compare x {}` compiles, but emits a warning rather than an error, since it can never do anything.
- The subject and each pattern value must be type-compatible (e.g. don't match a `string` pattern against an `int` subject). Confirmed subject types include `int` and `string`; nothing in the grammar itself restricts the subject's type beyond ordinary type-compatibility checking.
- `break`, `continue`, `return`, nested `if`, and any other ordinary statement are all valid inside a `matches`/`otherwise` body.

> **Note — keyword reuse:** `otherwise` here is unrelated to the ternary's `otherwise` above beyond sharing a spelling — see the note in that section.

### Patterns: `_`, destructuring and guards

A `matches` header is either a **value** (everything above: the subject is compared with it by equality) or one of three newer forms that look inside the subject. A bare identifier at the top of a header is still an ordinary variable to compare against; it never binds.

```turflang
struct Point { x: int, y: int }

fn classify(p: Point): int {
    mut r: int;
    compare p {
        matches Point { x: 0, y: 0 }:     // test both fields
            r = 1;
        done
        matches Point { x, y } if x == y: // bind both, then a guard
            r = 2;
        done
        matches Point { x: 0, .. }:       // test x, ignore every other field
            r = 3;
        done
        matches _:                        // anything at all
            r = -1;
        done
    }
    return r;
}
```

| Form | Meaning |
|---|---|
| `_` | Matches anything, binds nothing. Usable at the top of a header and as a field's pattern (`x: _`). |
| `Name { field: pattern, field, .. }` | Destructures a struct. Also spelled `module::Name { ... }`. A field written alone (`y`) **binds** it under its own name; `field: name` binds it under another; `field: 0` tests it. |
| `<pattern> if <condition>` | A guard: the header matches only if the pattern matches **and** the condition is true. The header's bindings are visible in the condition. |

**What can sit in a field's position** (inside a struct pattern): an integer literal (`5`, `-5`), a `char`, `string` or `bool` literal, an enum or machine variant (`Color::Red`), `_`, a name to bind, or another struct pattern (any depth). Inside a destructuring pattern a bare identifier always **binds**; to compare a field with a variable or any other expression, bind it and use a guard:

```turflang
matches Point { x, .. } if x == limit:    // not `Point { x: limit }`
```

`double` literals are not patterns (exact float equality is almost never meant); bind the field and test it in a guard.

**Every field is listed, or the pattern ends in `..`.** `Point { x }` is an error for a two-field struct (E092) so that adding a field to `Point` later cannot change what an existing pattern means. A pattern reads fields exactly as `p.field` would, so another module's private field cannot be destructured (E088); wave it off with `..`.

**What can be matched with a struct pattern:** a struct value, a `&T` reference, and `self`. A `&?T` (which may be null) and a raw `T*` are refused; test a `&?T` for null first.

**Bindings** are immutable copies of the matched value. They are visible to the header's guard and to the arm's body and to nothing else, and they shadow an outer variable of the same name inside the arm only.

```turflang
mut x: int = 100;
compare p {
    matches Point { x, .. }:   // this x is p.x
        ...
    done
}
// x is 100 again here
```

**Stacked headers** remain the way to write alternatives. They share one body, so each header must bind exactly the same names at the same types (or none); a guard belongs to its own header:

```turflang
compare p {
    matches Point { x: 0, y }:
    matches Point { x: 1, y }:      // either header runs the body, with y bound
        r = y * 10;
    done
}
```

**Error values.** `Type::Variant(sub, sub, ..)` destructures an error value's payload, positionally, with these same sub-patterns (`matches IOError::Denied(path, 0):`). It lists every field or ends in `..`, and a variant with no fields is written bare (`matches IOError::Timeout:`). It is never a catch-all. See [Error Handling](error_handling.md#payloads-data-on-a-variant).

**Order.** Headers are tried top to bottom, and the first one that matches (pattern **and** guard) runs; a guard that fails falls through to the next header, not out of the `compare`. The subject is evaluated **once** and every header tests that one value.

**Exhaustiveness.** With no `otherwise`:

- an enum or machine subject must have every variant covered by an **unguarded** header, and a `bool` subject both `true` and `false` (a guarded arm never counts, because its guard may be false) - otherwise E093;
- an error value (`err` inside `recover as err run`) can be any error a callee returns, so it always needs `otherwise` or an unguarded `_`, even when every arm is an error-variant pattern; a value of a *declared* error type must have each of its variants covered by an unguarded variant pattern whose fields all match anything;
- an `int`, `string` or `char` subject, or a struct with a refutable pattern, may still match nothing, in which case the `compare` does nothing.

`matches _:` (unguarded) counts as covering everything, which is the way to say "and everything else" without `otherwise`.

**Unreachable arms.** An arm - or `otherwise` - after an unguarded header that matches everything (`_`, or a struct pattern whose fields all bind or ignore) can never run and is a warning (E094). A duplicate *value* pattern is still an error, as before.

**`_name` is an identifier.** An underscore followed by anything is an ordinary name (`_unused`, `_why`), and - as the unused-variable warning's hint has always said - a name that starts with `_` is never reported as unused. A lone `_` is reserved for the wildcard and cannot be a variable, parameter or function name.
