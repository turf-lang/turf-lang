# Error Handling

Reference for Turflang's recoverable-error system: `error` type declarations, `!T` fallible return types, the three `recover` forms, and the `test::assert`/`test::fail` helpers used in tests.

See also: `docs/architecture/errors_and_recover.md` for how the compiler implements this system pass by pass. This document only covers how to write the code.

---

## Declaring an error type

```turflang
error IOError {
    FileNotFound,
    PermissionDenied,
    InvalidEncoding
}
```

Structurally similar to a C-style `enum` — a fixed set of bare variant names. A trailing comma after the last variant is accepted:

```turflang
error MathError {
    Overflow,
    InvalidInput,
}
```

A variant can also carry typed data — see [Payloads](#payloads-data-on-a-variant) below.

### Bare form

```turflang
error GeneralError;
```

Sugar for a single implicit variant: `error GeneralError { DEFAULT }`, constructed as `GeneralError::DEFAULT`.

> **Note:** you cannot mix the two forms on one declaration — `error GeneralError; { Something, }` is rejected. Pick either the bare `;` form or a braced variant list, not both.

### Referencing a variant

```turflang
MathError::DivisionByZero
```

A bare, **non-callable** value reference — exactly like an enum variant. Even `MathError::DivisionByZero()` (call syntax with zero arguments) is rejected: an error variant is a value, not a function.

```turflang
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}
```

A variant that has no fields can also carry one `string` **message**, attached at construction and read back with `err.message` (`""` when none was given):

```turflang
return MathError::DivisionByZero("dividing " + (a to string) + " by zero");
```

### Payloads: data on a variant

A variant may declare typed fields, written like a function's parameters:

```turflang
error IOError {
    NotFound(path: string),
    Denied(path: string, mode: int),
    Timeout
}
```

Any sized type is allowed — primitives, enums, structs, arrays, generic instances such as `List<string>` — except an error type, a `!T` or `void` (an error value describes what went wrong; it does not contain another failure). A field's type follows the same visibility rules as a struct field's.

**Constructing.** Arguments are positional, one per field, in order: `IOError::Denied("a.txt", 7)`. A trailing `string` after the fields is the message: `IOError::NotFound("a.txt", "no such file")`. A variant with *N* fields takes *N* or *N+1* arguments; one with no fields takes none, or exactly one (the message). Anything else is error **E096**, which names the declared fields.

**A variant with fields is not a value on its own.** `return IOError::NotFound;` is E096 — there is no `path` to fill in. A bare reference is still legal in two places, where only *which* variant occurred is read: as an operand of `==`/`!=`, and as the head of a pattern.

**Reading a payload** is done by destructuring, and only that — there is no `err.path`, because which fields exist depends on which variant occurred:

```turflang
fn describe(p: string, m: int): int {
    immut v: int = open(p, m) recover as err run {
        compare err {
            matches IOError::NotFound(path):
                io::printline("not found: " + path);
                return 1;
            done
            matches IOError::Denied(path, mode) if mode == 7:
                return 2;
            done
            matches IOError::Timeout:
                return 3;
            done
            otherwise:
                return 99;
        }
    };
    return v;
}
```

`matches IOError::Denied(path, mode):` tests that the error is that variant and binds its fields positionally. The sub-patterns are the ones a struct pattern takes (see [Control Flow](control_flow.md)): a name binds, `_` ignores, a literal or enum variant tests, a nested struct pattern destructures. Like a struct pattern, it lists every field or ends in `..` (`IOError::Denied(path, ..)`, or `IOError::NotFound(..)` to ignore everything), so a pattern cannot keep compiling, meaning something different, after a field is added. A variant with no fields is written bare: `matches IOError::Timeout:`. Bindings, guards, stacked headers and source order work exactly as in any `compare`.

An `err` from `recover as err` is an open union, so such a `compare` always needs an `otherwise:` (or `matches _:`) — E093 otherwise. A value of a *declared* error type (`mut e: IOError = ...`) has a closed set of variants: covering every one with an unguarded pattern whose fields all match anything (`IOError::Denied(_, mode)`) is exhaustive without `otherwise`.

`err == IOError::NotFound` compares the variant only — never the payload or the message.

---

## `try`: passing an error on

```turflang
fn load(path: string): !Config {
    immut text: string = try io::read_to_string(path);
    immut cfg: Config = try parse(text);
    return cfg;
}
```

`try e` is `e` if it succeeded. If it failed, it **returns the error, unchanged** — same variant, same payload, same message — from the function it is in, after running that function's pending `defer`s exactly as a `return` would. It is shorthand for `e recover as err run { return err; }`.

- It is a prefix operator: `try a() + try b()` is `(try a()) + (try b())`.
- The enclosing function must return `!V`. `V` is unrelated to the operand's success type: the error union is open, so nothing is converted.
- It works in any expression position, in a loop, in a generic function, and on a `!void` (`try write(fd);` as a statement).
- It is error **E095** in `main` (which returns `int`; handle the error there with `recover`), in a function that returns a plain type, inside a `defer`, at the top level, and on an operand that is not a `!T`.
- `try` is a reserved word.

### Returning a fallible call directly

`return f();`, where `f(): !T` and the function returns exactly `!T`, hands the value back unchanged — success or error. It never unwraps, and it does not convert: a `!string` can't be returned from a `!int` function, and a `!T` can't be returned from a function that returns a plain `T` (handle it with `recover`, or declare the function `!T`).

---

## `!T`: fallible return types

`!T` marks a function (or any type position — parameters, variable declarations) as returning either a value of type `T` on success, or a value of **any** declared error type on failure:

```turflang
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}
```

`T` can be a primitive, `void`, a user struct, or a generic function's own type parameter:

```turflang
fn<T> firstOrFail(arr: T[], len: int): !T { ... }
```

`!T` is also legal outside of return-type position:

```turflang
fn unusedHelper(x: !int, y: !string): void { ... }
mut a: !int;
```

**This is an open union**, not a closed, named set of possible error types (unlike, say, Rust's `Result<T, E>`). A function declared `!int` doesn't say *which* error types it can fail with in its signature — it may fail with any error type visible to it.

A `!T` value can't be used as a plain `T` directly — it must go through `recover` (below) at the call site:

```turflang
mut x: int = divide(10, 2);                     // ERROR: divide(10, 2) is !int, not int
mut x: int = divide(10, 2) recover produce 0;    // OK
```

There is no shorthand auto-forwarding (no `?`-style operator) — even returning a `!int` bare from a function declared to return plain `int` is rejected:

```turflang
fn foo(): int {
    return divide(10, 0);   // ERROR: divide(10, 0) is !int, foo() returns int
}
```

---

## `recover` — resolving a fallible value

`recover` converts a `!T` expression into a plain `T`, in one of three forms.

| Form | Syntax | On success | On failure | Result type |
|---|---|---|---|---|
| produce | `expr recover produce value` | the success value | `value` | `T` |
| run | `expr recover run { ... }` | the success value | block runs, must diverge unless `T` is `void` | `T` |
| as-run | `expr recover as err run { ... }` | the success value | block runs, `err` bound | `T` |

`recover` binds at the loosest precedence level, so it works as a call argument, not just a variable initializer:

```turflang
test::assert_eq(math::divide(20, 4) recover produce -1, 5, "...");
```

The result of a `recover` expression is always `T`, never `!T`.

### `recover produce`

```turflang
mut ok: int = divide(10, 2) recover produce -1;
mut p: Point = makePoint(false) recover produce Point::new(0, 0);
```

The fallback value is type-checked **exactly** against `T` — no coercion. Producing an `int` fallback against a `!Point` call, or a `string` against `!int`, is rejected. `recover produce` on a `!void` call is also rejected — there's no value slot to produce into.

### `recover run { ... }`

```turflang
writeFile("bad.txt", true) recover run {
    failed = true;
};
```

The block runs only on failure. When `T` is `void`, the block may simply fall off the end (as above). For any **non-`void`** `T`, every path through the block must end in `return`, `break`, or `continue` — falling off the end is a compile error:

```turflang
// against a !int call:
divide(1, 0) recover run {
    mut y: int = 5;   // ERROR: this path doesn't return
};
```

> **Gotcha — `return`/`break`/`continue` inside the block target the *enclosing function or loop*, not the `recover` expression itself, and are type-checked against that enclosing context — never against `T`.**
>
> ```turflang
> fn tryTimeout(): int {
>     return connect(1) recover as err run {
>         if (err == NetworkError::Timeout) {
>             return 1;   // returns from tryTimeout(), NOT from the recover expression
>         }
>         return 0;
>     };
> }
> ```
>
> This means a recover-run block used for a call whose success type is `Point` inside a function that returns `int` must `return` an `int`, not a `Point` — the block's `return` is checked against `int`. This is the single most common source of confusion with `recover run`/`recover as ... run`, so it's worth structuring failure-handling logic in small, single-purpose helper functions when it matters.

### `recover as err run { ... }`

```turflang
mut x: int = divide(10, 0) recover as err run {
    if err == MathError::DivisionByZero {
        io::printline("division by zero");
    }
    return -1;
};
```

Same divergence rules as `recover run`, but binds the failing error value to `err`, whose type is a type-erased "any error type." `err` supports `==`/`!=` against a bare `ErrorType::Variant` reference for variant-identity comparison, `err.message` for the attached message, and destructuring in a `compare` for a variant's payload (see [Payloads](#payloads-data-on-a-variant)). A `compare` counts as diverging when it has an `otherwise` (or provably covers every value) and every arm diverges, so reading a payload in a block needs no dead `return` after it.

Both the bound name and the trailing `run` are required — `recover as run { }` (missing name) and `recover as err { }` (missing `run`) are parse errors.

> **Note — bare-form errors can't be bound with `as err`.** If *every* error type a wrapped call can possibly fail with is a bare-form declaration (`error Name;`, i.e. only ever `Name::DEFAULT`), there's nothing for `err ==` to usefully discriminate between, and `recover as err run` is rejected for that call. Bare-form errors still work fine with `recover produce` and plain `recover run` — the restriction is specific to `as err`.

### Other `recover` constraints

- `recover` can only be applied to a fallible (`!T`) expression — using it on an already-plain expression is rejected.
- `recover` must be followed by `produce`, `run`, or `as` — anything else is a parse error.

---

## `test::assert` / `test::fail`

`assert` and `fail` are **not** compiler builtins and are **not** called bare. They're ordinary functions defined in `lib/std/testing/test.tr`, requiring `import test;` and module-qualified calls:

```turflang
import test;

test::assert(x == 5);                      // 1-arg: default failure message
test::assert(x == 5, "x should be 5");      // 2-arg: custom message
test::assert_eq(x, 5);                      // also test::assert_eq / test::assert_neq
test::fail("some message");                 // prints to stderr and aborts — never returns
```

- `fail(msg)` writes `msg` to stderr and aborts the process; it never returns.
- `assert(cond)` delegates to a default failure message; `assert(cond, msg)` calls `fail(msg)` if `cond` is false.
- `assert_eq`/`assert_neq` are overloaded per-type (`int`, `double`, `bool`, `string`), each with a 2-arg (default message) and 3-arg (custom message) form — the same arity-overload pattern used by `io::print`/`io::printline` elsewhere in the standard library. They are not generic over arbitrary `T`, since equality isn't well-defined for arbitrary types without operator-overload support.

> **Note:** if you write code after a `test::fail(...)` call within the same block, that code must still be reachable/valid as far as the compiler is concerned, even though `fail` never actually returns at runtime — the compiler treats `fail` calls as terminating that control-flow path only when it can prove it, so keep fail-then-more-statements code the same shape you'd write for a function call that returns normally.

---

## Related reading

- `docs/architecture/errors_and_recover.md` — internals: AST representation, resolver/type-checker passes, and codegen for `error`, `!T`, and `recover`.
