# Control Flow

Reference for Turflang's control-flow constructs: `if`/`elseif`/`else`, the `give`/`otherwise` ternary, `while`, `do`/`while`, `for` (range and for-each forms), `break`/`continue`, and `compare`/`matches`/`otherwise`.

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
