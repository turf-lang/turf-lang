# Error Handling

TurfLang separates two kinds of failure.

- A **recoverable error** is something the caller can reasonably handle: a file that is not there, text that is not a number. A function reports it by returning an `error` value in a `!T` result; the caller resolves it with `recover` or passes it on with `try`.
- A **panic** is a bug: an index out of range, a division by zero, a precondition the caller broke. It prints a message to stderr and aborts the process. There is no way to catch it.

This page covers the first kind in full, then panics, caller-located panic messages, the `test` helpers and the convention the standard library follows. For how the compiler implements all this, see [errors and recover](../architecture/errors_and_recover.md) and [caller location](../architecture/caller_location.md).

---

## Declaring an error type

An `error` declaration lists variants, like an `enum`:

```turflang
error IOError {
    FileNotFound,
    PermissionDenied,
    InvalidEncoding
}

error MathError {
    Overflow,
    InvalidInput,
}

fn main(): int {
    return 0;
}
```

A trailing comma after the last variant is accepted. A variant is named `Type::Variant`, and is a plain value, not a function: `MathError::Overflow` is complete, and `MathError::Overflow()` is rejected (E135, "Not callable").

### The bare form

```turflang
error GeneralError;

fn fail_always(): !int {
    return GeneralError::DEFAULT;
}

fn main(): int {
    immut n: int = fail_always() recover produce 7;
    io::printline(n);
    return 0;
}
```

`error GeneralError;` declares one implicit variant, `DEFAULT`. The bare form and a braced variant list cannot be mixed on one declaration: `error GeneralError; { Something, }` is E117.

<!-- doc-example: fails E117 -->
```turflang
error GeneralError; { Something, }

fn main(): int {
    return 0;
}
```

### Returning an error

A function that can fail returns [`!T`](#t-fallible-return-types). `return` an error value to fail, or an ordinary `T` to succeed:

```turflang
error MathError {
    DivisionByZero
}

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}

fn main(): int {
    io::printline(divide(10, 2) recover produce -1);
    io::printline(divide(10, 0) recover produce -1);
    return 0;
}
```

```text
5
-1
```

An unknown variant (`MathError::Nope`) is E039.

### Attaching a message

Any variant can carry one `string` **message**, written as a single argument:

```turflang
error MathError {
    DivisionByZero
}

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero("dividing " + (a to string) + " by zero");
    }
    return a / b;
}

fn safe_divide(a: int, b: int): int {
    return divide(a, b) recover as err run {
        io::printline("failed: " + err.message);
        return -1;
    };
}

fn main(): int {
    io::printline(safe_divide(10, 0));
    return 0;
}
```

```text
failed: dividing 10 by zero
-1
```

`err.message` reads the message back inside a [`recover as err run`](#recover-as-err-run-binding-the-error) block. It is `""` when the error was built without one. A message that is not a `string` is E096; calling a variant with no arguments, or with two or more, is E135.

---

## Payloads: data on a variant

A variant can declare typed fields, written like a function's parameters:

```turflang
error IOError {
    NotFound(path: string),
    Denied(path: string, mode: int),
    Timeout
}

fn main(): int {
    return 0;
}
```

Any sized type is allowed as a field: primitives, enums, structs, arrays, generic instances such as `List<string>`. A field cannot be an error type, a `!T` or `void` (an error value describes what went wrong; it does not contain another failure; E096). Field names must be unique within a variant. A field's type follows the same visibility rules as a struct field's.

**Constructing.** Arguments are positional, one per field, in order: `IOError::Denied("a.txt", 7)`. A trailing `string` after the fields is the message: `IOError::NotFound("a.txt", "no such file")`. A variant with *N* fields takes *N* or *N+1* arguments; one with no fields takes none, or exactly one (the message). Any other count for a variant with fields, or an argument of the wrong type, is **E096**, and the message names the declared fields.

<!-- doc-example: fails E096 -->
```turflang
error IOError {
    Denied(path: string, mode: int)
}

fn open(): !int {
    return IOError::Denied("a.txt");
}

fn main(): int {
    return 0;
}
```

The payload is copied to the heap, so it outlives the function that built it. A reference field may therefore hold a parameter or a `heap()` value, but not the address of a local variable (`&local` is E086).

**A variant with fields is not a value on its own.** `return IOError::NotFound;` is E096: there is no `path` to fill in. A bare reference is legal in only two places, where just *which* variant occurred is read: as an operand of `==` / `!=`, and as the head of a pattern.

<!-- doc-example: fails E096 -->
```turflang
error IOError {
    NotFound(path: string)
}

fn open(): !int {
    return IOError::NotFound;
}

fn main(): int {
    return 0;
}
```

### Reading a payload

A payload is read by destructuring it in a `compare`, and only that. There is no `err.path`, because which fields exist depends on which variant occurred:

```turflang
error IOError {
    NotFound(path: string),
    Denied(path: string, mode: int),
    Timeout
}

fn open(p: string, m: int): !int {
    if m == 0 { return IOError::NotFound(p); }
    if m == 1 { return IOError::Denied(p, 7); }
    if m == 2 { return IOError::Timeout; }
    return m;
}

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

fn main(): int {
    io::printline(describe("a.txt", 0));
    io::printline(describe("b.txt", 1));
    io::printline(describe("c.txt", 2));
    io::printline(describe("d.txt", 5));
    return 0;
}
```

```text
not found: a.txt
1
2
3
5
```

`matches IOError::Denied(path, mode):` tests that the error is that variant and binds its fields positionally. The sub-patterns are the ones a struct pattern takes (see [Control Flow](control_flow.md)): a name binds, `_` ignores, a literal or enum variant tests, a nested struct pattern destructures. As with a struct pattern, the list names every field or ends in `..` (`IOError::Denied(path, ..)`, or `IOError::NotFound(..)` to ignore everything), so a pattern cannot silently change meaning when a field is added. Leaving a field out is E092. A variant with no fields is written bare: `matches IOError::Timeout:`; writing a variant that has fields bare (`matches IOError::NotFound:`) is E092 too. Bindings, guards, stacked headers and source order work exactly as in any `compare`. The message is not bindable in a pattern; read it with `err.message`.

Because `return` inside a `recover` block returns from the *enclosing function* (see [below](#return-break-and-continue-inside-a-recover-block)), the usual shape is a small helper that turns the payload into that function's return value, as `describe` does above.

### Telling errors apart

`err == IOError::NotFound` compares the variant only, never the payload or the message. Two error types may share a variant name; `NetError::Timeout` and `DiskError::Timeout` are different values.

```turflang
error NetError { Timeout, Refused(port: int) }
error DiskError { Timeout, Full }

fn either(n: int): !int {
    if n == 0 { return NetError::Refused(80); }
    if n == 1 { return DiskError::Full; }
    if n == 2 { return DiskError::Timeout; }
    return n;
}

fn which(r: int): int {
    immut v: int = either(r) recover as err run {
        if err == NetError::Timeout { return 10; }
        if err == DiskError::Timeout { return 11; }
        if err == DiskError::Full { return 12; }
        if err == NetError::Refused { return 13; }
        return 99;
    };
    return v;
}

fn main(): int {
    io::printline(which(0));
    io::printline(which(1));
    io::printline(which(2));
    return 0;
}
```

```text
13
12
11
```

**Exhaustiveness.** The `err` of a `recover as err` is an *open* union: the call might fail with any error type, so a `compare` over it always needs an `otherwise:` (or `matches _:`), or it is E093.

<!-- doc-example: fails E093 -->
```turflang
error ParseError { Bad(at: int) }

fn parse(n: int): !int {
    if n < 0 { return ParseError::Bad(n); }
    return n;
}

fn main(): int {
    immut v: int = parse(1) recover as err run {
        compare err {
            matches ParseError::Bad(a):
                return a;
            done
        }
    };
    return v;
}
```

A value of a *declared* error type has a closed set of variants. Covering every one with an unguarded pattern whose fields all match anything is exhaustive without `otherwise`:

```turflang
error E { A(x: int), B(y: int, z: int), C }

fn main(): int {
    mut e: E = E::A(1);
    e = E::B(2, 3);
    compare e {
        matches E::A(_):
            io::printline("a");
        done
        matches E::B(_, z):
            io::printline(z);
        done
        matches E::C:
            io::printline("c");
        done
    }
    return 0;
}
```

```text
3
```

---

## `!T`: fallible return types

`!T` marks a function as returning either a `T` on success or an error value on failure. `T` can be any type, including `void` (`!void` for a function that returns nothing but can fail) and a generic function's own type parameter:

```turflang
error EmptyError { Empty }

fn<T> first_or_fail(items: T[], len: int): !T {
    if len == 0 {
        return EmptyError::Empty;
    }
    return items[0];
}

fn check(ok: bool): !void {
    if !ok {
        return EmptyError::Empty;
    }
    return;
}

fn main(): int {
    mut a: int[3] = [4, 5, 6];
    io::printline(first_or_fail(a, 3) recover produce -1);
    io::printline(first_or_fail(a, 0) recover produce -1);
    check(false) recover run {
        io::printline("check failed");
    };
    return 0;
}
```

```text
4
-1
check failed
```

`!T` is also legal in other type positions (a parameter `x: !int`, a variable `mut a: !int;`).

**The union is open.** A signature says only that the function can fail, not which error types it can fail with: any error type visible to it may be returned. The caller discovers which one with `==` or a `compare`, as above.

**A `!T` is not a `T`.** It has to be resolved with `recover` (or passed on with `try`) before it can be used as a plain `T`. There is no implicit unwrapping, and a `!int` cannot be returned from a function declared to return a plain `int`:

<!-- doc-example: fails E066 -->
```turflang
error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}

fn main(): int {
    mut x: int = divide(10, 2);
    return x;
}
```

<!-- doc-example: fails E064 -->
```turflang
error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}

fn foo(): int {
    return divide(10, 0);
}

fn main(): int {
    return 0;
}
```

The compiler warns about a function declared `!T` that never returns an error.

---

## `recover`: resolving a fallible value

`recover` converts a `!T` expression into a plain `T`, in one of three forms.

| Form | Syntax | On success | On failure | Result type |
|---|---|---|---|---|
| produce | `expr recover produce value` | the success value | `value` | `T` |
| run | `expr recover run { ... }` | the success value | the block runs and must leave (see below), unless `T` is `void` | `T` |
| as-run | `expr recover as err run { ... }` | the success value | the block runs with the error bound to `err` | `T` |

`recover` can only follow a fallible expression (E132 otherwise), and must be followed by `produce`, `run` or `as` (E110). It binds more loosely than any operator: the fallback of `f() recover produce 10 + 1` is `11`, and `1 + f() recover produce 0` is a type error because `1 + f()` is added before `recover` is reached, so parenthesize to combine a recovered value with something else (`1 + (f() recover produce 0)`). It works anywhere an expression does, including as a call argument:

```turflang
import test;

error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}

fn main(): int {
    test::assert_eq(divide(20, 4) recover produce -1, 5, "20 / 4");
    io::printline(1 + (divide(9, 0) recover produce 0));
    return 0;
}
```

```text
1
```

### `recover produce`

```turflang
error MyError { Failed }

struct Point {
    x: int,
    y: int
}

fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

fn make_point(fail: bool): !Point {
    if fail {
        return MyError::Failed;
    }
    return Point::new(1, 2);
}

fn main(): int {
    immut p: Point = make_point(true) recover produce Point::new(0, 0);
    io::printline(p.x);
    return 0;
}
```

```text
0
```

The fallback is checked against `T` exactly: a `string` fallback for a `!int` call, or an `int` for a `!Point`, is E066, and so is an `int` for a `!double`, since the fallback is not widened (write `0.0`, or convert with `to double`). An integer literal for a `!int32` is an `int32`. `recover produce` on a `!void` call is E066 as well, since there is no value to produce.

<!-- doc-example: fails E066 -->
```turflang
error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}

fn main(): int {
    immut x: int = divide(1, 0) recover produce "none";
    return x;
}
```

### `recover run`

The block runs only on failure. When `T` is `void`, it may simply finish:

```turflang
error IOError { WriteFailed }

fn write_file(ok: bool): !void {
    if !ok {
        return IOError::WriteFailed;
    }
    return;
}

fn main(): int {
    mut failed: bool = false;
    write_file(false) recover run {
        failed = true;
    };
    if failed {
        io::printline("the write failed");
    }
    return 0;
}
```

```text
the write failed
```

For any other `T` there is no value to fall back on, so every path through the block must leave with `return`, `break` or `continue`. A block that can fall off its end is E132, "Non-diverging `recover run`". (Calling a function that panics does not count as leaving; the compiler does not know it never returns.)

<!-- doc-example: fails E132 -->
```turflang
error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }
    return a / b;
}

fn main(): int {
    mut x: int = divide(1, 0) recover run {
        io::printline("failed");
    };
    return x;
}
```

Use `break` and `continue` to handle a failure inside a loop:

```turflang
error ParseError { Bad(at: int) }

fn parse(n: int): !int {
    if n % 3 == 0 { return ParseError::Bad(n); }
    return n * 10;
}

fn main(): int {
    mut total: int = 0;
    for i in 1..8 {
        immut v: int = parse(i) recover run { continue; };
        total = total + v;
    }
    io::printline(total);
    return 0;
}
```

```text
190
```

### `return`, `break` and `continue` inside a `recover` block

> **They act on the enclosing function or loop, never on the `recover` expression, and a `return` is checked against the enclosing function's return type, not against `T`.**

```turflang
error NetworkError { Timeout, Refused }

fn connect(n: int): !int {
    if n == 1 { return NetworkError::Timeout; }
    if n == 2 { return NetworkError::Refused; }
    return n;
}

fn try_timeout(n: int): int {
    return connect(n) recover as err run {
        if err == NetworkError::Timeout {
            return 1;   // returns from try_timeout, not from the recover expression
        }
        return 0;
    };
}

fn main(): int {
    io::printline(try_timeout(1));
    io::printline(try_timeout(2));
    io::printline(try_timeout(7));
    return 0;
}
```

```text
1
0
7
```

So a block recovering a call whose success type is `Point`, inside a function returning `int`, must `return` an `int`. Returning something else is E064:

<!-- doc-example: fails E064 -->
```turflang
error MyError { Failed }

struct Point {
    x: int,
    y: int
}

fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

fn make_point(fail: bool): !Point {
    if fail {
        return MyError::Failed;
    }
    return Point::new(42, 7);
}

fn main(): int {
    mut p: Point = make_point(true) recover run {
        return "not an int";
    };
    return 0;
}
```

When failure handling gets involved, put it in a small helper function that turns the error into that function's return value.

### `recover as err run`: binding the error

```turflang
error MathError { DivisionByZero }

fn divide(a: int, b: int): !int {
    if b == 0 {
        return MathError::DivisionByZero("b was zero");
    }
    return a / b;
}

fn safe_divide(a: int, b: int): int {
    return divide(a, b) recover as err run {
        if err == MathError::DivisionByZero {
            io::printline("division by zero: " + err.message);
        }
        return -1;
    };
}

fn main(): int {
    io::printline(safe_divide(10, 0));
    return 0;
}
```

```text
division by zero: b was zero
-1
```

The block has the same leave-or-finish rules as `recover run`, and binds the failing error to `err`, a value that can be any error type. With it you can:

- compare it with `==` / `!=` against an `ErrorType::Variant` reference;
- read `err.message`, the attached string (`""` if none);
- destructure it in a `compare` to read a variant's payload ([Payloads](#payloads-data-on-a-variant)).

A `compare` whose arms all leave, and that has an `otherwise` (or provably covers every value), counts as leaving, so reading a payload needs no dead `return` after it.

Both the name and the trailing `run` are required: `recover as run { }` is E108 and `recover as err { }` is E110.

**Bare-form errors cannot be bound.** If every error type the call can fail with is a bare `error Name;`, there is nothing for `err` to tell apart, and `recover as err run` is E016. Use `recover produce` or `recover run` for those.

<!-- doc-example: fails E016 -->
```turflang
error GeneralError;

fn gen(): !int {
    return GeneralError::DEFAULT;
}

fn main(): int {
    immut y: int = gen() recover as err run { return 1; };
    return y;
}
```

---

## `try`: passing an error on

`try` hands a failure to your own caller instead of handling it:

```turflang
error ParseError {
    Empty,
    Bad(at: int)
}

fn read_text(n: int): !string {
    if n == 0 { return ParseError::Empty; }
    return "text" + (n to string);
}

fn parse(text: string): !int {
    if text.length() > 5 { return ParseError::Bad(text.length()); }
    return text.length();
}

fn load(n: int): !int {
    immut text: string = try read_text(n);
    immut cfg: int = try parse(text);
    return cfg;
}

fn main(): int {
    io::printline(load(1) recover produce -1);
    io::printline(load(0) recover produce -1);
    io::printline(load(12345) recover produce -1);
    return 0;
}
```

```text
5
-1
-1
```

`try e` evaluates to the success value of `e`. If `e` failed, `try` **returns the error unchanged** (same variant, same payload, same message) from the function it is in, running that function's pending `defer`s exactly as a `return` would. It is shorthand for `e recover as err run { return err; }`.

- It is a prefix operator, so `try a() + try b()` is `(try a()) + (try b())`.
- The enclosing function must return `!V`, for any `V`. `V` need not match the operand's success type: the union is open, so nothing is converted. `try` on a `!void` works as a statement (`try write(fd);`).
- It works in any expression position, in a loop, and in a generic function.
- It is error **E095** in `main` (which returns `int`; handle the error there with `recover`), in a function that returns a plain type, inside a `defer`, and on an operand that is not a `!T`.
- `try` is a reserved word.

<!-- doc-example: fails E095 -->
```turflang
error IOError { NotFound }

fn open(): !int {
    return IOError::NotFound;
}

fn main(): int {
    immut n: int = try open();
    return n;
}
```

### Returning a fallible call directly

`return f();`, where `f(): !T` and the function itself returns exactly `!T`, hands the result back unchanged, success or error. It never unwraps and never converts: a `!string` cannot be returned from a function declared `!int` (E064), and a `!T` cannot be returned from a function that returns a plain `T` (handle it with `recover`, or declare the function `!T`).

```turflang
error ParseError { Bad(at: int) }

fn parse(n: int): !int {
    if n < 0 { return ParseError::Bad(n); }
    return n * 2;
}

fn via_parse(n: int): !int {
    return parse(n);
}

fn main(): int {
    io::printline(via_parse(4) recover produce -1);
    io::printline(via_parse(-3) recover produce -1);
    return 0;
}
```

```text
8
-1
```

<!-- doc-example: fails E064 -->
```turflang
error IOError { NotFound }

fn name(): !string {
    return IOError::NotFound;
}

fn number(): !int {
    return name();
}

fn main(): int {
    return 0;
}
```

---

## `defer` and errors

A `defer` runs when its enclosing block is left, by any route: falling off the end, `return`, `break`, `continue`, and the early return of a `try` or a `recover` block. A failure therefore cleans up after itself:

```turflang
error StepError { Failed(index: int) }

fn check(n: int): !int {
    if n == 3 { return StepError::Failed(n); }
    return n;
}

fn work(limit: int): !int {
    io::printline("open");
    defer io::printline("close");
    mut sum: int = 0;
    for i in 1..limit {
        defer io::printline("end of iteration");
        sum = sum + try check(i);
    }
    return sum;
}

fn report(limit: int): int {
    immut total: int = work(limit) recover as err run {
        compare err {
            matches StepError::Failed(s):
                io::printline("failed at step " + (s to string));
                return -1;
            done
            otherwise:
                return -2;
        }
    };
    return total;
}

fn main(): int {
    io::printline(report(3));
    io::printline(report(6));
    return 0;
}
```

```text
open
end of iteration
end of iteration
close
3
open
end of iteration
end of iteration
end of iteration
close
failed at step 3
-1
```

Two limits:

- A deferred statement cannot use `try` (E095), because a deferred statement may not return. If the cleanup itself can fail, resolve it inside the `defer`: `defer close_file(f) recover run { io::printline("cleanup failed"); };`.
- A **panic does not run deferred statements**: nothing unwinds. See [Control Flow](control_flow.md) for `defer` in general.

---

## Panics

A panic prints a message to stderr and aborts the process; deferred statements do not run, and nothing can intercept it. The language raises one for a failed runtime check (an array index out of bounds, division by zero, signed integer overflow, an invalid `string` to `int` conversion), and library or user code raises one with `sys::panic(message)`.

A failed runtime check names the line of the operation that failed:

```text
panic: runtime check: Division by zero (main.tr:2)
```

Use a panic for a bug (a precondition the *caller* broke), and an error for something a correct caller can meet and handle. The standard library follows the same rule; see [below](#the-standard-librarys-failure-convention).

### Reporting the caller: `@caller_location`

A panic inside a helper that says "line 2 of the helper" tells you nothing about which call was wrong. A function can ask for the location of whoever called it. Write `@caller_location` on the line before `fn` (or before `operator`), then read the bare keyword `caller_location`, a `string` of the form `file:line`, inside the body:

```turflang
@caller_location
fn require_positive(n: int): int {
    if n <= 0 {
        sys::panic(caller_location + ": require_positive: got " + (n to string));
    }
    return n;
}

fn twice(n: int): int {
    return require_positive(n) * 2;
}

fn main(): int {
    immut ok: int = require_positive(3);
    io::printline(twice(ok - 3));
    return 0;
}
```

```text
main.tr:10: require_positive: got 0
```

The panic names line 10, the call inside `twice`, not line 4 inside `require_positive`. The file is spelled as it was given to the compiler.

The rules:

- `caller_location` is legal only inside a function marked `@caller_location` (E084 otherwise); `@caller_location` must be followed by `fn` or `operator` (E110).
- It works on free functions, methods (including a call in method form, `c.take(2)`), generic functions and operators; an operator reports the line of the expression that used it.
- A marked function can only be called directly; it can't be used as a function value (`mut f: fn(): string = where;` is E137), because an indirect call has no caller location to pass. To hand one around, write an unmarked function that calls it and use that.
- **A call from an unmarked function stamps the call's own line.** A call from one `@caller_location` function to another **forwards** the location it was itself given, so a chain of marked helpers reports the original call site, not an intermediate one. Nothing needs to be written to forward.

```turflang
@caller_location
fn where_am_i(): string {
    return caller_location;
}

@caller_location
fn outer(): string {
    return where_am_i();
}

fn plain(): string {
    return where_am_i();
}

fn main(): int {
    io::printline(where_am_i());
    io::printline(outer());
    io::printline(plain());
    return 0;
}
```

```text
main.tr:16
main.tr:17
main.tr:12
```

`outer()` is marked, so its call to `where_am_i()` reports line 17, where `main` called `outer`. `plain()` is not, so the call inside it reports its own line, 12.

<!-- doc-example: fails E084 -->
```turflang
fn not_marked(): string {
    return caller_location;
}

fn main(): int {
    io::printline(not_marked());
    return 0;
}
```

---

## `test::assert` and `test::fail`

`assert` and `fail` are ordinary functions in the `test` module, not builtins; they need `import test;` and a qualified call. Both are `@caller_location`, so a failure prints the line of *your* call:

```turflang
import test;

fn check_age(age: int): void {
    test::assert(age >= 0, "age must not be negative");
}

fn main(): int {
    check_age(5);
    check_age(-1);
    return 0;
}
```

```text
main.tr:4: age must not be negative
```

`test::fail(msg)` panics with the same prefix, `test::assert(cond)` uses a default message, and `assert_eq` / `assert_neq` compare two values of the same type but name a line inside the library rather than yours. The full list of helpers is in [Testing](stdlib/testing.md).

---

## The standard library's failure convention

The standard library picks the mechanism by *why* a call can fail:

| Why it fails | Shape | Example |
|---|---|---|
| The input or the environment: a missing file, text that is not a number, a refused connection | returns `!T`, with a variant that carries what failed | `io::read_to_string(path)`, `strings::parse_int(s)`, `json::parse(text)`, `Map.try_get(key)` |
| The caller broke a precondition: popping an empty stack, an index out of range | **panics**, and the message starts with the caller's `file:line:` | `List.pop()`, `Map.get(key)`, `strings::substring` |
| A question: is it there, does it start with this | returns `bool` or a sentinel (`-1`, `""`) | `io::exists(path)`, `Set.contains(x)`, `strings::index_of` |

Where a call has both a panicking and a recoverable form they are a pair, with a predicate to ask first (`Map.get` / `Map.try_get` / `Map.contains`). A `!void` result left unhandled discards the failure silently, so check anything whose failure matters. The full convention, and which function follows which shape, is in [the standard library overview](stdlib/README.md#2-failure-three-kinds-three-shapes).

```turflang
import map::Map;

fn main(): int {
    mut m: Map<string, int> = Map::default();
    m.put("a", 1);
    io::printline(strings::parse_int("abc") recover produce -1);
    io::printline(m.try_get("b") recover produce -1);
    if !m.contains("b") {
        io::printline("no b");
    }
    return 0;
}
```

```text
-1
-1
no b
```

When you write your own library, follow the same split: `!T` for what the outside world can get wrong, a `@caller_location` panic for what only a buggy caller can do, `bool` for questions.

---

## Related reading

- [Control Flow](control_flow.md): `compare` patterns, `defer`.
- [Testing](stdlib/testing.md): the `test` module.
- [The standard library overview](stdlib/README.md): the failure convention in full.
- [Errors and recover](../architecture/errors_and_recover.md) and [caller location](../architecture/caller_location.md): how the compiler implements them.
