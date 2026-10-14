# Testing: `test`

Assertions for test programs, and how `turf test` runs them. Not prelude: it
needs `import test;`, and a bare `assert(...)` without it is E041 ("Unknown
function 'assert'").

```turflang
import test;
```

A test in Turf is an ordinary program. Its `main` makes assertions and returns
`0`. An assertion that fails prints a message to standard error and aborts the
program (exit status 134 on Linux and macOS), which `turf test` reports as a
failed test. There is no test framework, no test discovery inside a file, and no
test attributes: one file is one test.

## The functions

| Function | Fails when |
|---|---|
| `assert(cond: bool): void` | `cond` is false |
| `assert(cond: bool, msg: string): void` | `cond` is false |
| `assert_eq(left, right): void` | `left != right` |
| `assert_eq(left, right, msg: string): void` | `left != right` |
| `assert_neq(left, right): void` | `left == right` |
| `assert_neq(left, right, msg: string): void` | `left == right` |
| `fail(msg: string): void` | always |
| `assert_fail_eq(left: string, right: string, msg: string): void` | always |
| `assert_fail_neq(left: string, right: string, msg: string): void` | always |

`assert_eq` and `assert_neq` exist for four types: `int`, `double`, `bool` and
`string`. Both sides must be the same type. Other types have no overload (`char`
is E025, "No matching overload!"); compare those with
`assert(a == b, "message")`.

### `assert`

`assert(cond)` fails with a canned message:

```text
te.tr:2: Oops! An assertion failed here.
Hint: Assertions are checks that make sure your code is running exactly as you expect. This one didn't pass.
```

`assert(cond, msg)` fails with `msg` instead. Both messages start with the
`file:line:` of **your** call, so a failure in a long test says which check it
was.

### `assert_eq` and `assert_neq`

On failure, `assert_eq` prints `assert_eq failed: <left> != <right>`, with
` (<msg>)` added when you pass a message; `assert_neq` prints
`assert_neq failed: both sides equal <value>`.

```text
<lib>/testing/test.tr:176: assert_eq failed: 14 != 15 (perimeter of a 3 by 4 rectangle)
```

Two things to know:

- The `file:line:` prefix of these two names a line **inside the library**, not
  your call. Give each `assert_eq` a message, as above, to tell checks apart.
- `double` values are compared exactly, and printed with six significant digits.
  `assert_eq(0.1 + 0.2, 0.3)` fails, and its message reads `0.3 != 0.3`. Compare
  computed doubles with a tolerance:
  `assert(math::abs(a - b) < 0.000001, "close enough")`.

### `fail`

`fail(msg)` ends the test with `file:line: msg` (your call's location). Use it
for a path that must not be reached.

### `assert_fail_eq` and `assert_fail_neq`

The functions `assert_eq` and `assert_neq` call to report a failure. They take the
two sides already converted to text and always fail, with the messages above.
Call them from your own comparison helper, for a type `assert_eq` does not cover,
so that its failures read the same:

```turflang
import test;

struct Point {
    x: int,
    y: int,
}

fn assert_same_point(a: Point, b: Point, msg: string): void {
    if a.x != b.x || a.y != b.y {
        test::assert_fail_eq(
            "(" + (a.x to string) + ", " + (a.y to string) + ")",
            "(" + (b.x to string) + ", " + (b.y to string) + ")",
            msg);
    }
}

fn main(): int {
    mut p: Point = Point::default();
    p.x = 3;
    p.y = 4;
    mut q: Point = Point::default();
    q.x = 3;
    q.y = 4;
    assert_same_point(p, q, "same coordinates");
    return 0;
}
```

---

## Example: tests for a module, run with `turf test`

A project keeps its tests in `tests/`. A test imports the project's own modules
(from `src/`) and the standard library. This project has a module `shapes`:

```text
geometry/
  turf.toml
  src/
    main.tr
    shapes.tr
  tests/
    shapes_test.tr
    fail_wrong_argument_type.tr
    fail_assertion_aborts.tr
```

`src/shapes.tr`:

```turflang
pub fn area_of_rect(width: int, height: int): int {
    return width * height;
}

pub fn perimeter_of_rect(width: int, height: int): int {
    return 2 * (width + height);
}

pub fn half(n: int): double {
    return (n to double) / 2.0;
}
```

`tests/shapes_test.tr` is a plain test: it passes if it compiles, runs and exits
with status `0`.

<!-- doc-example: skip needs the project's shapes module (src/shapes.tr above) -->
```turflang
import shapes;
import test;

fn main(): int {
    test::assert_eq(shapes::area_of_rect(3, 4), 12, "area of a 3 by 4 rectangle");
    test::assert_eq(shapes::perimeter_of_rect(3, 4), 14, "perimeter of a 3 by 4 rectangle");
    test::assert_eq(shapes::half(5), 2.5, "half of 5");
    test::assert(shapes::area_of_rect(0, 9) == 0, "an empty rectangle has no area");
    return 0;
}
```

A file whose name begins with `fail_` is a test that is **expected to be rejected**
or to abort, and comments say why. `tests/fail_wrong_argument_type.tr` checks that
the compiler refuses a mistake, and that it is refused for the reason intended
(`// EXPECT-DIAG:` takes an error code):

<!-- doc-example: skip needs the project's shapes module (src/shapes.tr above) -->
```turflang
// EXPECT-DIAG: E025
import shapes;

fn main(): int {
    return shapes::area_of_rect("3", 4);
}
```

A test that is expected to abort at run time carries the marker comment
`// RUNTIME_ABORT_EXPECTED`, and `// EXPECT-RUNTIME:` pins the message it must
print, as in `tests/fail_assertion_aborts.tr`:

<!-- doc-example: skip needs the project's shapes module (src/shapes.tr above) -->
```turflang
// RUNTIME_ABORT_EXPECTED
// EXPECT-RUNTIME: assert_eq failed: 12 != 13
import shapes;
import test;

fn main(): int {
    test::assert_eq(shapes::area_of_rect(3, 4), 13);
    return 0;
}
```

From the project directory, `turf test` compiles and runs every file under
`tests/` and exits `0` only if all of them pass:

```text
$ turf test
Running Turf Test Suites...

[tests] fail_wrong_argument_type.tr ... PASS (Rejected properly)
[tests] fail_assertion_aborts.tr ... PASS (Aborted at runtime as expected)
[tests] shapes_test.tr ... PASS

=== Test Summary ===
Total Tests: 3
Passed:      3
Failed:      0

All tests passed successfully!
```

When an assertion fails, the report shows the program's message, and the exit
status is `1`:

```text
[tests] shapes_test.tr ... RUNTIME FAILED (Exit code 134)

--- tests/shapes_test.tr (Runtime Error exit 134):
<lib>/testing/test.tr:176: assert_eq failed: 14 != 15 (perimeter of a 3 by 4 rectangle)
```

The file-naming rules, the full list of `EXPECT-` comments, the flags and the
rule that a project with no tests is an error are in
[Projects and packages: `turf test`](../projects_and_packages.md#tests-turf-test).
Outside a project, the same runner is `turfc --test <dir>`.

## Example: a single-file check

A test does not need a project. Any program that asserts and returns `0` is one,
and you can run it directly:

```turflang
import list::List;
import test;

fn main(): int {
    mut words: List<string> = strings::split("a,b,c", ",");
    test::assert_eq(words.length(), 3, "three fields");
    test::assert_eq(words.get(1), "b", "the middle field");
    test::assert_neq(words.get(0), words.get(2));
    test::assert(strings::contains("turf", "ur"), "substring found");
    printline("all checks passed");
    return 0;
}
```

```text
all checks passed
```
