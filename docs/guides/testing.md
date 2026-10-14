# Testing a TurfLang project

This guide takes a small library and grows a test suite for it: checks on
values, on fallible code, on code the compiler must refuse, on panics, on
warnings, and on code that touches files, random numbers and the clock. It ends
with the suite running in CI.

You need a project (`turf new`) and a way to run `turf test`. The rules behind
each step are in
[Projects and packages](../reference/projects_and_packages.md#tests-turf-test)
and [the `test` module](../reference/stdlib/testing.md); this page shows how
they fit together.

Every output below is from a real run, trimmed. The paths `turf` prints are
shortened to `~/code/temperature`.

## The project under test

```text
$ turf new temperature --lib
Created library package 'temperature' in temperature/
  temperature/turf.toml
  temperature/src/main.tr
  temperature/src/temperature.tr
  ...
```

The library's root module, `src/temperature.tr`, is what the tests exercise. It
has a conversion, a function that fails with a recoverable error (`!double`),
a function that panics when it is misused, a function that reads a file, and a
function that takes the current time as an argument:

```turflang
// The root module of the package `temperature`: conversions between scales.

fn absolute_zero(): double {
    return -273.15;
}

pub error TempError {
    BelowAbsoluteZero,
}

pub struct Reading {
    pub label: string,
    pub celsius: double,
}

pub fn celsius_to_fahrenheit(c: double): double {
    return c * 9.0 / 5.0 + 32.0;
}

pub fn fahrenheit_to_celsius(f: double): double {
    return (f - 32.0) * 5.0 / 9.0;
}

// Whether a reading taken at `taken_at` (seconds since the epoch) is older
// than `max_age` seconds at time `now`. The caller supplies the clock.
pub fn is_stale(taken_at: int, now: int, max_age: int): bool {
    return now - taken_at > max_age;
}

// Fails for a temperature that cannot exist.
pub fn to_kelvin(c: double): !double {
    if c < absolute_zero() {
        return TempError::BelowAbsoluteZero(
            (c to string) + " C is below absolute zero");
    }
    return c - absolute_zero();
}

// The mean of `values`. An empty array is a bug in the caller, not an
// error to recover from, so it panics.
pub fn average(values: double[]): double {
    if values.length() == 0 {
        panic("average: no values");
    }
    mut total: double = 0.0;
    mut i: int = 0;
    while i < values.length() {
        total = total + values[i];
        i = i + 1;
    }
    return total / (values.length() to double);
}

// Reads the number in the file `path` as a temperature in Celsius.
pub fn load_celsius(path: string): !double {
    immut text: string = try io::read_to_string(path);
    return try strings::parse_double(text.trim());
}

pub fn warmer(a: Reading, b: Reading): Reading {
    if a.celsius >= b.celsius {
        return a;
    }
    return b;
}

operator ==(a: Reading, b: Reading): bool {
    return a.label == b.label && a.celsius == b.celsius;
}
```

## Your first test

A test is an ordinary program. It makes assertions, and it passes if it
compiles, runs and exits with status `0`. Tests live in the `tests/` directory
at the project root, beside `src/`. Create `tests/convert_test.tr`:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;
import test;

fn main(): int {
    test::assert_eq(temperature::celsius_to_fahrenheit(100.0), 212.0, "boiling point");
    test::assert_eq(temperature::celsius_to_fahrenheit(0.0), 32.0, "freezing point");
    return 0;
}
```

A test imports the package's own modules the same way `src/main.tr` does, and
`import test;` for the assertions (a bare `assert(...)` without it is E041).
Run it from anywhere inside the project:

```text
$ turf test
Running Turf Test Suites...

[tests] convert_test.tr ... PASS

=== Test Summary ===
Total Tests: 1
Passed:      1
Failed:      0

All tests passed successfully!
```

`turf test` compiles and runs every `.tr` file under `tests/`, at any depth,
several at a time. The exit status is `0` when every test passed and `1`
otherwise. The `[tests]` label is the directory a file is in, and the order of
the lines is whatever finishes first, so do not depend on it.

A project with no `tests/` directory, or one with no tests in it, is an error,
not a pass.

When a check fails, the report shows what the program printed. Change the
expected value to `213.0` and run again:

```text
$ turf test
Running Turf Test Suites...

[tests] convert_test.tr ... RUNTIME FAILED (Exit code 134)

=== Test Summary ===
Total Tests: 1
Passed:      0
Failed:      1

Failed Tests Details:

--- tests/convert_test.tr (Runtime Error exit 134):
<lib>/testing/test.tr:176: assert_eq failed: 212 != 213 (boiling point)
 ---
$ echo $?
1
```

Exit status 134 is the program aborting: a failed assertion prints its message
and aborts, which is all a failure is. There is no framework, no test discovery
inside a file and no test attributes. One file is one test, so a file should
check one thing, or a few closely related things: the first failed assertion
ends the file, and the later ones do not run.

## Assertions

The module has `assert`, `assert_eq`, `assert_neq` and `fail`. The full table is
in [the `test` reference](../reference/stdlib/testing.md#the-functions). Here is
a test that uses each form:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;
import test;

fn main(): int {
    // A condition, with and without a message.
    test::assert(temperature::celsius_to_fahrenheit(-40.0) == -40.0);
    test::assert(temperature::celsius_to_fahrenheit(37.0) > 98.0, "body temperature");

    // Two values that must differ.
    test::assert_neq(temperature::celsius_to_fahrenheit(1.0), 1.0, "scales differ");

    // Doubles are compared exactly; use a tolerance for computed ones.
    immut f: double = temperature::celsius_to_fahrenheit(36.6);
    test::assert(math::abs(f - 97.88) < 0.000001, "normal body temperature in F");

    test::assert_eq(temperature::average([10.0, 20.0, 30.0]), 20.0);
    return 0;
}
```

Give every assertion a message. It is what you read when a test fails in a long
file. The failures look like this:

```text
a.tr:4: Oops! An assertion failed here.
Hint: Assertions are checks that make sure your code is running exactly as you expect. This one didn't pass.

a.tr:4: one is not above two

<lib>/testing/test.tr:185: assert_neq failed: both sides equal 3 (should differ)

<lib>/testing/test.tr:176: assert_eq failed: 0.3 != 0.3
```

The first two name the line of **your** call. `assert_eq` and `assert_neq` name a
line inside the library, so their message is the only thing that tells two of
them apart.

### Doubles

`assert_eq` compares doubles exactly, and prints them with six significant
digits. `celsius_to_fahrenheit(36.6)` is not exactly `97.88`, and
`test::assert_eq(f, 97.88, "...")` fails with `97.88 != 97.88`. Compare a
computed double with a tolerance, as above.

### Structs

`assert_eq` and `assert_neq` exist for `int`, `double`, `bool` and `string`
only. A struct has no `==` either, unless its module declares one:

<!-- doc-example: fails E124 -->
```turflang
import test;

struct Point {
    x: int,
    y: int,
}

fn main(): int {
    mut a: Point = Point::default();
    mut b: Point = Point::default();
    test::assert(a == b, "same point");
    return 0;
}
```

There are two ways to compare structs. The first is to check the fields:

```turflang
import test;

struct Point {
    x: int,
    y: int,
}

fn main(): int {
    mut a: Point = Point::default();
    mut b: Point = Point::default();
    a.x = 3;
    b.x = 3;
    test::assert_eq(a.x, b.x, "x");
    test::assert_eq(a.y, b.y, "y");
    return 0;
}
```

The second is to declare `operator ==` for the type, so that
`assert(a == b, "message")` works:

```turflang
import test;

struct Point {
    x: int,
    y: int,
}

operator ==(a: Point, b: Point): bool {
    return a.x == b.x && a.y == b.y;
}

fn main(): int {
    mut a: Point = Point::default();
    mut b: Point = Point::default();
    test::assert(a == b, "same point");
    return 0;
}
```

`temperature` does this for `Reading` (the last declaration of the library
above). The operator has to be declared in the module that declares the struct.
A test cannot add one for a type it imports (E080). `operator !=` is a separate
operator, so write `!(a == b)` for "different". If you want the failure to read
like `assert_eq`'s, write a helper that calls `test::assert_fail_eq`; the
[reference](../reference/stdlib/testing.md#assert_fail_eq-and-assert_fail_neq)
has an example.

Here is a test that uses `Reading`'s `==`. It also calls a helper, `make_reading`,
which comes from a shared module (see [Shared helpers](#shared-helpers)):

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import readings_module;
import temperature::Reading;
import temperature;
import test;

fn main(): int {
    immut kitchen: Reading = readings_module::make_reading("kitchen", 21.5);
    immut garage: Reading = readings_module::make_reading("garage", 9.0);

    immut warmest: Reading = temperature::warmer(kitchen, garage);
    test::assert_eq(warmest.label, "kitchen", "the warmer reading wins");
    test::assert_eq(warmest.celsius, 21.5, "and keeps its value");

    test::assert(warmest == kitchen, "the result is the kitchen reading");
    test::assert(!(warmest == garage), "and not the garage reading");
    return 0;
}
```

## Testing fallible code

A function that returns `!T` has two paths, and a test covers both. The tool is
`recover`; [Error handling](../reference/error_handling.md) describes it.

For success, resolve the value with `recover as err run { ... }` and fail the
test inside the block. A `!double` cannot be passed to `assert_eq` as it is, and
`recover produce 0.0` would hide a failure behind a plausible number:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
immut k: double = temperature::to_kelvin(0.0) recover as err run {
    test::fail("to_kelvin(0.0) failed: " + err.message);
    return 1;
};
test::assert_eq(k, 273.15, "0 C in kelvin");
```

For the error path, remember that a `return` inside a `recover` block returns
from the **function you are in**, here the test's `main`. So the block can make
its checks and end the test, and a `fail` after the statement catches a call
that did not fail at all:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;
import test;

fn main(): int {
    immut k: double = temperature::to_kelvin(-300.0) recover as err run {
        test::assert_eq(err.message, "-300 C is below absolute zero");
        return 0;
    };
    test::fail("to_kelvin(-300.0) should have failed, gave " + (k to string));
    return 0;
}
```

That covers one input per test file. To check several inputs in one file, put
the error path in a small helper that returns what you want to check, and
assert on its result:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature::TempError;
import temperature;
import test;

// "" when the call succeeds, otherwise the failure's message.
fn failure_message(c: double): string {
    immut _kelvin: double = temperature::to_kelvin(c) recover as err run {
        test::assert(err == TempError::BelowAbsoluteZero, "wrong error variant");
        return err.message;
    };
    return "";
}

fn main(): int {
    immut k: double = temperature::to_kelvin(0.0) recover as err run {
        test::fail("to_kelvin(0.0) failed: " + err.message);
        return 1;
    };
    test::assert_eq(k, 273.15, "0 C in kelvin");

    test::assert_eq(failure_message(-300.0), "-300 C is below absolute zero", "below zero");
    test::assert_eq(failure_message(-273.15), "", "absolute zero itself is allowed");
    return 0;
}
```

If `to_kelvin` stops failing, `failure_message` returns `""`, and the
`assert_eq` reports the difference. The `return` in the block leaves
`failure_message`, not the test. `err == TempError::BelowAbsoluteZero`
compares the variant only; the message is read with `err.message`. For a
variant with fields, use a `compare` with `matches`, as the
[reference](../reference/error_handling.md#reading-a-payload) describes. The
`_` in `_kelvin` marks a variable that is deliberately never read.

## Testing that misuse is rejected

A language with types and visibility makes some mistakes impossible to compile,
and a library can promise that. A test for such a promise passes when the
compiler **refuses** the program. Name the file `fail_<something>.tr`: it passes
when it fails to compile (a compiler crash does not count).

`temperature::to_kelvin` returns `!double`, so assigning it to a plain `double`
must not compile. `tests/compile/fail_unhandled_error.tr`:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
// EXPECT-DIAG: E066
import temperature;

fn main(): int {
    immut k: double = temperature::to_kelvin(20.0);
    return 0;
}
```

The helper `absolute_zero` has no `pub`, so another module cannot call it.
`tests/compile/fail_private_helper.tr`:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
// EXPECT-DIAG: E088
import temperature;

fn main(): int {
    printline(temperature::absolute_zero());
    return 0;
}
```

An index the compiler can see is out of range is rejected too, and that is a
compile-time test, not a runtime one. `tests/compile/fail_constant_index.tr`:

<!-- doc-example: fails E001 -->
```turflang
// EXPECT-DIAG: E001
fn main(): int {
    mut readings: double[3] = [1.0, 2.0, 3.0];
    mut i: int = 3;
    printline(readings[i]);
    return 0;
}
```

A test does not need to import the project: this one has nothing to do with it.

### Pin the error code

The `// EXPECT-DIAG: E066` line says **why** the compiler must refuse the
program. Without it, a `fail_` test passes for any compile error at all,
including a typo in the test itself:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;

fn main(): int {
    immut k: double = temperature::to_kelvn(20.0);
    return 0;
}
```

With no `EXPECT-DIAG` line, `turf test` reports:

```text
[tests] fail_typo.tr ... PASS (Rejected properly)
```

`to_kelvn` is misspelled, the compiler refuses the file, and the test "passes"
while checking nothing about `to_kelvin`. With `// EXPECT-DIAG: E066` as its
first line, the same file fails:

```text
[tests] fail_typo.tr ... FAILED (Rejected, but EXPECT-DIAG not met)

--- tests/fail_typo.tr (Rejected, but EXPECT-DIAG not met):
EXPECT-DIAG missed: no [E066] in the compiler output
fail_typo.tr (1 error)

── error at line 5, col 44 [E026 · general]

  Oops! Module 'temperature' has no function named 'to_kelvn'.
  ...
```

Keep a `fail_` test to one mistake. The compiler may report only some of the
errors in a file, so a second mistake is not a reliable thing to pin.

### Finding the code

You do not need to know the code in advance. Write the test without the
`EXPECT-DIAG` line and without the `fail_` prefix, so it is an ordinary test,
and run it. It fails to compile, and the report shows the diagnostic:

```text
$ turf test
[compile] mixed_up.tr ... COMPILE FAILED

--- tests/compile/mixed_up.tr (Compile Error, exit 1):
mixed_up.tr (1 error)

── error at line 4, col 5 [E066 · type mismatch]

  Type mismatch!
  Cannot initialise 'double' variable 'k' with value of type '!double'.
  ...
  Run `turf --explain E066` for more details.
```

The code is in the header, in square brackets. Check that it is the error you
meant to provoke. `turf explain E066` says what the error means:

```text
$ turf explain E066
E066  TypeError (TYPE_MISMATCH)

You used a value where a different, incompatible type was expected
...
```

Then rename the file to `fail_<something>.tr` and add the line. If you
add `// EXPECT-DIAG: E000` instead (a code nothing produces), the failure message
prints the whole diagnostic and tells you the code, so you can also fill the
line in from there.

A `fail_` file that compiles is a failure, so a guard that stops working is
noticed:

```text
[compile] fail_ok.tr ... FAILED (Should have failed compilation)
```

## Testing panics and runtime checks

A panic is a bug in the caller, and it aborts the program. A test for one
passes when the program **aborts**. The file is `fail_<something>.tr` again, so
it must also carry a marker comment that says it is expected to compile and
abort: `// RUNTIME_ABORT_EXPECTED`. `// EXPECT-RUNTIME:` names text the program
must print, so the test checks why it aborted. `tests/runtime/fail_average_empty.tr`:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
// RUNTIME_ABORT_EXPECTED
// EXPECT-RUNTIME: average: no values
import temperature;

fn main(): int {
    mut readings: double[3] = [1.0, 2.0, 3.0];
    printline(temperature::average(readings[0..0]));
    return 0;
}
```

`readings[0..0]` is an empty slice, which `average` rejects. A runtime check in
the standard library is tested the same way, with the message the
[collections reference](../reference/stdlib/collections.md#failure-which-calls-panic-which-return-t)
lists. `tests/runtime/fail_list_out_of_range.tr`:

```turflang
// RUNTIME_ABORT_EXPECTED
// EXPECT-RUNTIME: List.get: index out of bounds
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    l.push(1);
    printline(l.get(5));
    return 0;
}
```

A panic message begins with the `file:line:` of the call that was wrong, so
pin the part after it. If the text does not match, the report shows what the
program did print:

```text
[tests] fail_average_empty.tr ... FAILED (Aborted, but EXPECT-RUNTIME not met)

--- tests/runtime/fail_average_empty.tr (Aborted at runtime, but EXPECT-RUNTIME not met):
EXPECT-RUNTIME missed: the program's output does not contain 'average: zzz'
average: no values
```

A program that ends without aborting fails too (`Expected a runtime abort, got
exit 0`).

### A silent abort

`RUNTIME_ABORT_EXPECTED` passes on **any** non-zero exit, so `exit(3)` counts
as an abort. When there is no message to pin, pin the exit status with
`// EXPECT-EXIT:`. A call to `abort()` exits with 134:

```turflang
// RUNTIME_ABORT_EXPECTED
// EXPECT-EXIT: 134
fn main(): int {
    abort();
    return 0;
}
```

With another status the test fails. Here the program called `exit(3)`:

```text
[tests] fail_exit3.tr ... FAILED (Aborted, but EXPECT-EXIT not met)
EXPECT-EXIT missed: the program exited with 3, not 134
```

## Pinning warnings, and their absence

A warning does not stop a compile, so an ordinary test (a file that is not
`fail_`) compiles, runs, and passes. `EXPECT-DIAG` with text instead of a code
pins that the compiler said something. Warnings have no code, so match their
words. `tests/warnings/unused_variable_warns.tr`:

```turflang
// EXPECT-DIAG: is declared but never used
import test;

fn main(): int {
    immut spare: int = 5;
    test::assert(1 + 1 == 2, "arithmetic");
    return 0;
}
```

`EXPECT-NO-DIAG` pins the other direction: the compiler's output must not have
the text. This is a way to keep a library free of warnings.
`tests/warnings/clean_library_test.tr` imports `temperature`, so the warnings of
`src/temperature.tr` are part of its output:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
// EXPECT-NO-DIAG: warning
import temperature;
import test;

fn main(): int {
    test::assert_eq(temperature::celsius_to_fahrenheit(0.0), 32.0, "freezing point");
    return 0;
}
```

If someone leaves an unused variable in `absolute_zero`, the test fails and
shows the warning:

```text
[warnings] clean_library_test.tr ... FAILED (EXPECT-DIAG)

--- tests/warnings/clean_library_test.tr (EXPECT-DIAG):
EXPECT-NO-DIAG violated: the compiler output contains 'warning'
~/code/temperature/src/temperature.tr (1 warning)

── warning at line 4, col 5

  Warning: Variable 'spare' is declared but never used.
  ...
```

Only the modules a test imports are part of its output, so a library with
several modules needs a test that imports each of them.

## Shared helpers

Tests repeat setup. A file named `<name>_module.tr` in `tests/` is a helper: it
is never run as a test, and any test can import it by name. This is
`tests/support/readings_module.tr`:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
// A helper shared by the tests. The name ends in _module, so `turf test`
// compiles it only when a test imports it.
import temperature::Reading;

pub fn make_reading(label: string, celsius: double): Reading {
    mut r: Reading = Reading::default();
    r.label = label;
    r.celsius = celsius;
    return r;
}
```

The test above imports it with `import readings_module;`. The directory it is
in does not matter, so `tests/support/` keeps helpers apart from tests.

Two things go wrong:

- **Forgetting `pub`.** A helper is a module like any other, so what a test
  can call must be `pub` (E088 otherwise):

  ```text
  Function 'make_reading' is private to module 'readings_module'.
  ```

- **A name that does not end in `_module`.** Any other `.tr` file under
  `tests/` is a test. A helper called `tests/helpers.tr` has no `main`, and is
  run, and fails:

  ```text
  [tests] helpers.tr ... COMPILE FAILED

  --- tests/helpers.tr (Compile Error, exit 1):
  helpers.tr (1 error)

  ── error [E119 · declaration]

    No entry point found. Please define 'fn main(): int' in your file.
  ```

A test can import the package's own modules under `src/`, its direct
dependencies and the standard library, and nothing else. It cannot import another
package's internal modules; the rules are in
[Projects and packages](../reference/projects_and_packages.md).

## Organising and running the suite

### Subdirectories

`turf test` finds tests at any depth, so group them as you like. The suite so far:

```text
temperature/
  turf.toml
  src/
    main.tr
    temperature.tr
  tests/
    convert_test.tr
    assert_forms_test.tr
    reading_test.tr
    compile/          fail_*.tr: programs the compiler must refuse
    errors/           kelvin_test.tr
    runtime/          fail_*.tr: programs that must abort
    warnings/         warnings pinned and absent
    files/            tests that read a data file
    determinism/      tests with random numbers and time
    support/          readings_module.tr
```

The label in front of each line of the report is the directory name.

### Running part of the suite

Name a test file or a directory of tests, and `turf test` runs only those. A
path is written from where you are, so from the project root it starts with
`tests/`, and from inside `tests/` it does not:

```text
$ turf test tests/convert_test.tr
Running Turf Test Suites...

[tests] convert_test.tr ... PASS

=== Test Summary ===
Total Tests: 1
Passed:      1
Failed:      0

All tests passed successfully!
```

`turf test tests/determinism` runs that directory, and several paths can be
given at once. A path that does not exist, or that is outside `tests/`, is an
error (exit status 2), and so is one that holds no test, such as a
`_module.tr` companion.

`--exclude <path>` does the opposite: it skips a file or a directory, relative
to `tests/`, and can be repeated. To skip one slow test, exclude it:

```text
$ turf test --exclude compile --exclude runtime/fail_silent_abort.tr
Running Turf Test Suites...

[tests] assert_forms_test.tr ... PASS
[warnings] unused_variable_warns.tr ... PASS
[tests] reading_test.tr ... PASS
[errors] kelvin_test.tr ... PASS
[runtime] fail_list_out_of_range.tr ... PASS (Aborted at runtime as expected)
[warnings] clean_library_test.tr ... PASS
[runtime] fail_average_empty.tr ... PASS (Aborted at runtime as expected)
[tests] convert_test.tr ... PASS
...
```

There is no filter that selects tests by name inside a file: the unit is the
file.

Running one test file with `turf run` works only for a test that imports
nothing but the standard library:

```text
$ turf run tests/determinism/random_range_test.tr
$ echo $?
0
```

A test that imports `temperature` has no access to `src/` when run that way, even
from the project root, and fails with `Could not find module 'temperature'`
(E027). The message says why: a project's modules are visible only to commands
that build with the package graph, such as `turf test`, `turf build` and
`turf run` with no file. For a single test of a project, exclude the others.

### Parallelism and time limits

By default the tests run all at once on every processor. Two flags change that.
Both have an environment variable that serves as the default:

| Flag | Environment variable | Default |
|---|---|---|
| `--jobs <n>` | `TURF_TEST_JOBS` | every processor |
| `--timeout <seconds>` | `TURF_TEST_TIMEOUT_SEC` | 5 |

Use `--jobs 1` to read a failing test's output without the others running
beside it, or on a machine that is too busy for the default. A test that runs
longer than the timeout is stopped and fails; this test sleeps for three
seconds, and the suite runs with `--timeout 1`:

```text
$ turf test --timeout 1
[slow] sleepy_test.tr ... TIMEOUT (Likely infinite loop)
...
--- tests/slow/sleepy_test.tr (Timed out after 1s) ---
```

Raise the timeout for a test that is meant to be slow, and keep it low for the
rest, so that a loop that never ends fails quickly.

### Optimisation levels

`-O0` through `-Os` are passed to every test's compile, as are `-g` and
`--link <lib>`:

```text
$ turf test -O0
$ turf test -O2
```

Optimisation must not change what a program does. A suite that passes at `-O0`
and fails at `-O2`, or the other way round, points at a bug in the program or
in the compiler. Every test in this guide passes at `-O0`, `-O2` and `-Os`.

## Tests that read files, and no input

Each test runs with **its own directory** as the working directory, so a data
file beside the test is opened by its bare name, wherever you ran `turf test`.
`tests/files/sample.txt` holds `18.5`, and `tests/files/load_test.tr` reads it:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;
import test;

fn main(): int {
    // The working directory is tests/files, so the path is relative to this file.
    immut c: double = temperature::load_celsius("sample.txt") recover as err run {
        test::fail("load_celsius failed: " + err.message);
        return 1;
    };
    test::assert_eq(c, 18.5, "the value in sample.txt");

    immut missing: double = temperature::load_celsius("nope.txt") recover produce -1000.0;
    test::assert_eq(missing, -1000.0, "a missing file is an error");
    return 0;
}
```

A path written from the project root, such as `tests/files/sample.txt`, does not
exist for the test:

```text
[files] load_test.tr ... RUNTIME FAILED (Exit code 134)

--- tests/files/load_test.tr (Runtime Error exit 134):
load_test.tr:7: load_celsius failed: file not found: 'tests/files/sample.txt'
```

A test that writes a file writes it into that directory, beside the sources.
Remove it at the end of the test, or write to a name you ignore in version
control.

A test has **no standard input**. `input()` returns `""`, as it does at end of
input, and `input(msg)` prints its prompt first. A test of code that reads a
line passes the line as an argument instead, or asserts on the behaviour for
empty input:

```turflang
import test;

fn main(): int {
    test::assert_eq(input("name? "), "", "no standard input: input() returns an empty string");
    return 0;
}
```

## Keeping tests deterministic

A test that fails one run in a hundred is worse than none. Two sources of that
are random numbers and the clock.

### Random numbers

The generator in `random` seeds itself from the operating system, and there is
no function to set the seed. Two runs of the same test get different numbers
(`reseed()` draws another fresh seed; it does not make a sequence repeatable).
There are two ways to test code that uses it.

**Assert a property that holds for every draw**, not a value. This test is
correct whatever the dice show:

```turflang
import random;
import test;

fn main(): int {
    mut i: int = 0;
    while i < 1000 {
        immut die: int = random::random_int(1, 6);
        test::assert(die >= 1 && die <= 6, "a die roll is between 1 and 6");
        i = i + 1;
    }
    return 0;
}
```

**Supply your own generator**, a few lines of arithmetic on an `int` the test
keeps, when you want many varied inputs that are the same on every run. Put
the seed in the failure message, so that a failure is reproducible.
`tests/determinism/round_trip_test.tr` checks that converting to Fahrenheit and
back returns the starting value:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;
import test;

// A small linear congruential generator: the same seed gives the same
// sequence on every machine and every run.
fn next(state: int): int {
    return (state * 1103515245 + 12345) % 2147483648;
}

fn main(): int {
    immut seed: int = 20260101;
    mut state: int = seed;
    mut i: int = 0;
    while i < 1000 {
        state = next(state);
        immut c: double = ((state % 20000) to double) / 100.0 - 100.0;
        immut back: double = temperature::fahrenheit_to_celsius(
            temperature::celsius_to_fahrenheit(c));
        test::assert(math::abs(back - c) < 0.000001,
            "round trip of " + (c to string) + " (seed " + (seed to string) + ", draw " + (i to string) + ")");
        i = i + 1;
    }
    return 0;
}
```

If the conversion were wrong, the failure names the input, the seed and the
draw:

```text
round_trip_test.tr:19: round trip of 20.26 (seed 20260101, draw 0)
```

The generator multiplies a value below 2^31 by about 2^30, which stays inside
`int`, so the arithmetic does not overflow. It is not a good generator, and it
is not for security; it is enough to vary test inputs.

### Time

`time::now_seconds()` is the wall clock, and `time::clock_ticks()` is the CPU
time of the process, which varies from run to run. A test must not compare
either against a fixed number. Design the code so that the time is an argument,
as `is_stale` does, and the test supplies it:

<!-- doc-example: skip imports the project's temperature module -->
```turflang
import temperature;
import test;

fn main(): int {
    immut taken_at: int = 1700000000;
    test::assert(!temperature::is_stale(taken_at, taken_at + 60, 60), "exactly at the limit is fresh");
    test::assert(temperature::is_stale(taken_at, taken_at + 61, 60), "one second past the limit is stale");
    return 0;
}
```

The edges (`+ 60` and `+ 61`) are checked exactly, with no sleep. A test that has
to sleep (`time::sleep_seconds`) is slow and is the first to hit the timeout on a
busy CI machine.

## Running the suite in CI

CI runs the same commands you do. Everything below is what you run locally;
`turf test` exits `1` if any test failed, which makes the CI step fail.
`--locked` makes `turf` refuse to change `turf.lock`, so commit that file with
the project, and `--offline` stops it from fetching anything. `turf fmt --check`
exits `1` when a file in `src/` or `tests/` would be reformatted.

A GitHub Actions workflow, `.github/workflows/test.yml`:

```yaml
name: test
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      # install turf (see the README's install section)
      - run: turf version
      - run: turf fmt --check
      - run: turf test --locked
      - run: turf test --locked -O2
```

Install `turf` first, in the way that [the README's install
section](../../README.md#install) describes. The `turf version` line makes the
log say which compiler it used. See
[Projects and packages](../reference/projects_and_packages.md) for dependencies
and the lock file.

The second `turf test` runs the suite at `-O2`, to check that optimisation does
not change a result.

## See also

- [Projects and packages: `turf test`](../reference/projects_and_packages.md#tests-turf-test):
  the file-naming rules, every `EXPECT-` comment and the flags.
- [Testing: the `test` module](../reference/stdlib/testing.md): every assertion,
  its message, and `assert_fail_eq`.
- [Error handling](../reference/error_handling.md): `!T`, `recover`, `try` and
  panics.
- [CLI and diagnostics](../reference/cli_and_diagnostics.md): `turf check`,
  `turf explain`, `turf fmt` and how to read a diagnostic.
- [Standard library conventions](../reference/stdlib/README.md): which calls
  panic and which return `!T`.
- [Random numbers](../reference/stdlib/random.md) and
  [Time](../reference/stdlib/time.md).
