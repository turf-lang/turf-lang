# Writing a TurfLang library

A library is a package that other packages import. This guide builds one,
`interval`, which handles closed ranges of integers such as `3..7`. It covers
what a library exposes and what it keeps private, how its functions should
report failure, how to test it, how to hand it to other projects, and how to
wrap a C library. Another package uses it at the end.

If you have not built a program with `turf` yet, read [Working on a TurfLang
project](projects.md) first. The rules this guide relies on are stated in full in
[Projects and packages](../reference/projects_and_packages.md).

## Create the package

```text
$ turf new interval --lib
Created library package 'interval' in interval/
  interval/turf.toml
  interval/src/main.tr
  interval/src/interval.tr
  interval/README.md
  interval/.vscode/extensions.json
  interval/.gitignore
```

`--lib` adds the library's root module, `src/interval.tr`, to what `turf new`
makes for a program. The name matters: it is what other packages write in
`import`, so it must be a plain identifier, and it may not be the name of a
standard-library module (`turf new list --lib` is refused).

The generated `src/main.tr` imports the library and calls it, so `turf run`
works at once:

```text
$ cd interval
$ turf run
Compiled to build/interval.o
Successfully compiled to build/interval
Hello, TurfLang! interval::answer() is 42.
```

Keep it. It is a handy place to try your own API. Packages that depend on yours
ignore it.

## The root module is the API

A package's modules are the `.tr` files under `src/`. Of those, exactly one can
be imported by another package: the **root module**, `src/<name>.tr`. Every
other module is internal. A dependent that tries to import one is refused,
whatever the module marks `pub`:

```text
── error at line 3, col 8 [E091 · declaration]

  Module 'numbers' is internal to package 'interval'.
    /home/me/code/interval/src/numbers.tr

  Hint: Only a package's root module can be imported from outside it. Import 'interval' and use what its root module makes `pub`; if 'interval' should offer this, its root module has to expose it.

  3 │ import numbers;
    │        ^~~~~~~ Here!
```

So you have two levels of privacy to work with. Inside a module, everything is
private unless it says `pub`. Across modules of the same package, `pub` means
"visible to the rest of the package". Only a `pub` item *in the root module*
reaches your users.

That suggests a layout. Put the types and functions you want to support in the
root module, and put the machinery behind them in internal modules. Here is
`interval`'s internal module, `src/numbers.tr`:

```turflang
// An internal module: only this package's own modules can import it.

pub fn read(text: string): !int {
    return strings::parse_int(text.trim());
}

pub fn bounded(n: int, low: int, high: int): int {
    if n < low {
        return low;
    }
    if n > high {
        return high;
    }
    return n;
}
```

The root module imports it by its bare name. `turf build` and `turf check`
always check the root module, even when nothing imports it, so a mistake in your
library shows up before any user meets it.

## Design the public surface

Start `src/interval.tr` with the things users will name: the types and the
errors.

<!-- doc-example: skip imports the package's internal module numbers -->
```turflang
// The root module of the package 'interval'. Another package writes
// `import interval;` and sees what is marked `pub` here, and nothing else.

import numbers;

pub error IntervalError {
    Inverted(low: int, high: int),
    BadText(text: string),
    Outside(n: int)
}

// A closed range of integers, from `low` to `high` inclusive.
pub struct Interval {
    pub low: int,
    pub high: int
}

// A value that applies to one interval.
pub struct Tagged<T> {
    pub range: Interval,
    pub value: T
}
```

Three rules shape what you write.

**Fields are private unless marked `pub`**, even in a `pub` struct. `Interval`
lets users read `low` and `high`. A type that must keep its layout to itself
leaves the fields private and offers methods instead.

**A dependent that touches something private is refused.** Suppose the root
module has a helper without `pub`:

<!-- doc-example: skip uses the Interval type of the package -->
```turflang
fn checksum(r: Interval): int {
    return r.low * 31 + r.high;
}
```

and a dependent calls `interval::checksum(r)`:

```text
── error at line 9, col 29 [E088 · declaration]

  Function 'checksum' is private to module 'interval'.
  Note: 'checksum' is declared in module 'interval' without `pub` (line 87, col 4)

  Hint: If 'checksum' is meant to be used from other modules, mark it `pub` in interval.tr: `pub fn checksum(...)`. Otherwise, 'interval' offers these public functions: parse.

  9 │     io::printline(interval::checksum(r));
    │                             ^~~~~~~~ Here!
```

The fix is on your side: either make it `pub` and own it as part of your API, or
leave it private. Private items are the part of the library you can change
without telling anyone.

**A `pub` signature may not mention a private type.** If a user would receive
a value they cannot name, the library itself is refused, in `turf check` of the
library:

<!-- doc-example: skip uses the Cache type declared beside it -->
```turflang
struct Cache {
    hits: int
}

pub fn warm(): Cache {
    return Cache::default();
}
```

```text
── error at line 91, col 8 [E089 · declaration]

  Public function 'warm' exposes the private type 'Cache'.
  Note: 'Cache' is declared in module 'interval' without `pub` (line 87, col 1)

  Hint: Anything `pub` is visible to other modules, but 'Cache' is not, so they would receive values of a type they cannot name. Either make the type public -- `pub struct Cache` in interval.tr -- or keep the function internal by removing its `pub`.

  91 │ pub fn warm(): Cache {
     │        ^~~~ Here!
```

**Declare the types of your API in the root module.** A `pub` type of an
internal module is visible to the rest of the package only, so the root module
may not put it in a `pub` signature either. Users would receive a value whose
type they cannot write, since they cannot import the module it lives in:

<!-- doc-example: skip imports the package's internal module numbers -->
```turflang
import numbers;

pub fn stats(values: int[]): numbers::Stats {
    return numbers::summarize(values);
}
```

```text
── error at line 3, col 8 [E089 · declaration]

  Public function 'stats' of package 'interval' exposes 'Stats', a type of its internal module 'numbers'.
  Note: 'Stats' is declared in module 'numbers', which only package 'interval' can import (src/numbers.tr, line 3, col 5)

  Hint: Other packages import only 'interval', its root module, so they would receive values of a type they cannot name. Either move `pub struct Stats` into the root module, interval.tr, or keep the function internal by removing its `pub`.

  3 │ pub fn stats(values: int[]): numbers::Stats {
    │        ^~~~~ Here!
```

Define `Stats` in `src/interval.tr` and the error goes away. An internal
module's own `pub` items may use its types freely: only the package imports
them.

## Follow the standard library's conventions

Users learn one set of habits from the standard library. If your library
follows them, they do not have to learn another. The full statement is in
[the standard library overview](../reference/stdlib/README.md); these are the
parts a library author needs.

**Naming.** Functions, methods and fields are `snake_case` (`index_of`,
`value_at`). Types and errors are `PascalCase`; constants are `UPPER_CASE`.
Give each thing one name, with no aliases.

**Failure has three shapes, chosen by why a call can fail.**

| Why it fails | Shape | In `interval` |
|---|---|---|
| The input or the environment is wrong: bad text, a missing file, a refused connection. Not a bug in the caller. | returns `!T` with an error that says what failed | `Interval.of`, `parse`, `Tagged.value_at` |
| The caller broke a precondition: an index out of range. | panics, and the message starts with the **caller's** `file:line:` | `Interval.nth` |
| A question: is it inside, is it there. | returns `bool`, or a sentinel such as `-1` | `Interval.contains` |

Here they are in the root module:

<!-- doc-example: skip uses the types and the internal module of the package -->
```turflang
// Input that can be wrong returns `!Interval`.
pub fn Interval.of(low: int, high: int) static: !Interval {
    if low > high {
        immut why: string = "low " + (low to string) + " is above high " + (high to string);
        return IntervalError::Inverted(low, high, why);
    }
    mut r: Interval = Interval::default();
    r.low = low;
    r.high = high;
    return r;
}

// Reads text such as "3..7".
pub fn parse(text: string): !Interval {
    immut cut: int = text.index_of("..");
    if cut < 0 {
        return IntervalError::BadText(text, "expected low..high");
    }
    immut low: int = numbers::read(text.substring(0, cut)) recover run {
        return IntervalError::BadText(text, "the lower bound is not a number");
    };
    immut high: int = numbers::read(text.substring(cut + 2, text.length())) recover run {
        return IntervalError::BadText(text, "the upper bound is not a number");
    };
    return Interval::of(low, high);
}

// A question returns a `bool`.
pub fn Interval.contains(n: int): bool {
    return n >= self.low && n <= self.high;
}

pub fn Interval.width(): int {
    return self.high - self.low + 1;
}

pub fn Interval.clamp(n: int): int {
    return numbers::bounded(n, self.low, self.high);
}

// A broken precondition is a bug in the caller: panic, and name the caller's line.
@caller_location
pub fn Interval.nth(i: int): int {
    if i < 0 || i >= self.width() {
        sys::panic(caller_location + ": Interval.nth: index " + (i to string) + " is outside 0.." + (self.width() to string));
    }
    return self.low + i;
}
```

`@caller_location` makes `caller_location` available inside `nth`: the line of
the code that called it. If a public method calls a private helper that
panics, mark both with `@caller_location`, or the message names a line inside
your library instead of the user's call. A marked function can only be called
directly, not through a function value.

When a function has both a panicking and a recoverable form, the standard
library makes them a pair: `Map.get` panics on a miss, `Map.try_get` returns
`!V`, and `Map.contains` lets you ask first. Do the same only where both forms
are useful.

**A value that owns an operating-system resource has `close()`**, and users
write `defer x.close();` right after they create it. Make `close()` safe to call
twice. The C-wrapping example at the end does this.

## Define error types

An `error` declaration lists variants, like an `enum`. A variant can carry
fields, and a trailing `string` argument is its message. A function returns one
the way it returns a value, from a function whose result is `!T`, and the caller
reads it back in a `recover as err` block:

```turflang
error IntervalError {
    Inverted(low: int, high: int),
    BadText(text: string),
    Outside(n: int)
}

fn check(low: int, high: int): !int {
    if low > high {
        return IntervalError::Inverted(low, high, "low " + (low to string) + " is above high " + (high to string));
    }
    return high - low;
}

fn main(): int {
    immut width: int = check(7, 3) recover as err run {
        printline(err.message);
        printline(err == IntervalError::Inverted);
        printline(err == IntervalError::Outside);
        return 0;
    };
    printline(width);
    return 1;
}
```

```text
low 7 is above high 3
true
false
```

Make the error `pub` in your root module so a user can name it. Carry what
failed (the text, the number) in the fields, and put a sentence a person can
read in the message. A user who only wants to report the problem prints
`err.message`; one who wants to react compares variants with `==`, which ignores
the payload. The fares program below does both.

One error type per library is usually enough. A `!T` does not say which errors a
function can return, so name the variants in your documentation.

## Generic types and functions

A generic is compiled from source in the package that uses it, with that
package's types. That is why a library can offer `Tagged<T>` and a user can
instantiate it with a struct only the user declares. A method on a generic
struct repeats the struct's type parameters after `fn`:

<!-- doc-example: skip uses the types of the package -->
```turflang
pub fn<T> Tagged.new(range: Interval, value: T): void {
    self.range = range;
    self.value = value;
}

// Looking a value up by number can miss, so it returns `!T`.
pub fn<T> Tagged.value_at(n: int): !T {
    if !self.range.contains(n) {
        immut why: string = (n to string) + " is outside " + (self.range.low to string) + ".." + (self.range.high to string);
        return IntervalError::Outside(n, why);
    }
    return self.value;
}
```

Generics follow the ordinary visibility rules: a generic is usable from another
package only if it is `pub`, and a generic struct's fields are private unless
marked `pub`. The body of a `pub` generic can call the module's private helpers.
There are no trait constraints; the body is checked against whatever type the
caller supplies. See [Generics and operator
overloading][generics] for the rules.

[generics]: ../reference/generics_and_operators.md#generic-structs-and-their-methods

## Try it and test it

Replace `src/main.tr` to exercise the API:

<!-- doc-example: skip imports the interval package, which is not in this file -->
```turflang
// src/main.tr: this package's entry point. `turf run` builds and runs it.
// The library itself is src/interval.tr; other packages import that, never this.
import io;
import interval;

fn main(): int {
    immut r: interval::Interval = interval::parse("3..7") recover run {
        io::printline("bad interval");
        return 1;
    };
    io::printline(r.contains(5));
    io::printline(r.clamp(100));
    io::printline(r.width());
    return 0;
}
```

```text
$ turf run
Compiled to build/interval.o
Successfully compiled to build/interval
true
7
5
```

For repeatable checks, put tests under `tests/`. A test imports the library the
way a user would, plus anything internal, since it belongs to the package.
`turf test` runs every file and passes a file when it compiles and exits 0. A
file named `fail_*.tr` must instead abort (when it carries
`// RUNTIME_ABORT_EXPECTED`) or fail to compile. Here is a passing test and one
that pins the panic of `nth`:

<!-- doc-example: skip imports the interval package -->
```turflang
import interval;
import test;

fn main(): int {
    immut r: interval::Interval = interval::parse("3..7") recover run {
        test::fail("3..7 should parse");
        return 1;
    };
    test::assert_eq(r.width(), 5, "width of 3..7");
    test::assert(r.contains(7), "the upper bound is inside");
    test::assert(!r.contains(8), "8 is outside");
    test::assert_eq(r.clamp(100), 7, "clamp to the top");
    test::assert_eq(r.nth(2), 5, "third element");

    immut fallback: interval::Interval = interval::parse("7..3") recover produce r;
    test::assert_eq(fallback.low, 3, "inverted text takes the fallback");
    return 0;
}
```

<!-- doc-example: skip imports the interval package -->
```turflang
// RUNTIME_ABORT_EXPECTED
// EXPECT-RUNTIME: Interval.nth: index 9 is outside 0..5
import interval;

fn main(): int {
    immut r: interval::Interval = interval::Interval::of(3, 7) recover run {
        return 1;
    };
    r.nth(9);
    return 0;
}
```

```text
$ turf test
Running Turf Test Suites...

[tests] fail_nth_out_of_range.tr ... PASS (Aborted at runtime as expected)
[tests] basic.tr ... PASS

=== Test Summary ===
Total Tests: 2
Passed:      2
Failed:      0

All tests passed successfully!
```

Save them as `tests/basic.tr` and `tests/fail_nth_out_of_range.tr`. Do not name a
test after a module in `src/` (a `tests/interval.tr` would make `import
interval;` ambiguous, E090). The rest of the testing workflow is in
[Testing](testing.md).

## Use it from another project

Make a program that depends on the library. A path dependency is the quickest
way while you are still changing the library, because `turf` reads it live:

```text
$ turf new fares
$ cd fares
$ turf add interval --path ../interval
Added dependency 'interval' (../interval) to /home/me/code/fares/turf.toml
```

The program prices a ticket by age. It uses `Tagged<Fare>`, a generic instance
over a struct that only this package declares, and it reads the library's errors
through `err.message`:

<!-- doc-example: skip imports the interval package -->
```turflang
import io;
import interval;

struct Fare {
    name: string,
    cents: int
}

fn main(): int {
    immut ages: interval::Interval = interval::parse("5..12") recover as err run {
        io::printline("cannot read the interval: " + err.message);
        return 1;
    };
    mut child: Fare = Fare::default();
    child.name = "child";
    child.cents = 250;
    mut row: interval::Tagged<Fare> = interval::Tagged::new(ages, child);

    for age in [9, 30] {
        immut fare: Fare = row.value_at(age) recover as err run {
            io::printline(err.message);
            continue;
        };
        io::printline("age " + (age to string) + ": " + fare.name + " pays " + (fare.cents to string));
    }
    return 0;
}
```

```text
$ turf run
Compiled to build/fares.o
Successfully compiled to build/fares
age 9: child pays 250
30 is outside 5..12
```

A path dependency's `turf.lock` entry records the path and no hash.

## Release it as a git tag

There is no registry to upload to. A library is published by tagging a commit in
a git repository that others can reach. In `interval/`:

```text
$ git init -b main
$ git add -A
$ git commit -m "interval 0.1.0"
$ git tag v0.1.0
```

Commit everything `turf new` made, including `turf.lock`, which for a library
with no dependencies is two lines. A dependent ignores your `turf.lock`; it uses
its own. Push the repository and the tag to wherever you host it (`git push
origin main v0.1.0`).

A project then names the repository and the tag. Here `/home/me/code/interval`
stands for the repository's URL (an `https://` address, for a hosted one):

```text
$ turf remove interval
Removed dependency 'interval' from /home/me/code/fares/turf.toml
$ turf add interval --git /home/me/code/interval --tag v0.1.0
turf: fetching interval (/home/me/code/interval)
Added dependency 'interval' (git /home/me/code/interval, tag=v0.1.0, commit 534ea21001c3) to /home/me/code/fares/turf.toml
```

```toml
[dependencies]
interval = { git = "/home/me/code/interval", tag = "v0.1.0" }
```

Use exactly one of `tag` (a release), `rev` (a commit) or `branch`. The next
build fetches the repository and pins it in `turf.lock`:

```text
$ turf run
Compiled to build/fares.o
Successfully compiled to build/fares
age 9: child pays 250
30 is outside 5..12
$ cat turf.lock
# This file is generated by turf. Commit it; do not edit it.
version = 1

[[package]]
name = "interval"
source = "git+/home/me/code/interval"
ref = "tag=v0.1.0"
commit = "534ea21001c3d0aeb1a0fd76efffbda440e0d9e8"
hash = "sha256:af96dcd16d4bf4eeec70024ad1dee67b9352cc616861814b5051eb296fe3d342"
```

The lock records the tag you asked for, the commit it named, and a hash of the
fetched files. Your users commit their lock, and it keeps their builds on that
commit even if you later move the tag.

### Versions are tags

`version` in `turf.toml` is a string that `turf` does not interpret. A
dependent's pin is the tag or commit, not that field. Bump `version` when you
tag, so the two agree, and tag every release. To ship a new method, add it to
the root module:

<!-- doc-example: skip uses the Interval type of the package -->
```turflang
pub fn Interval.overlaps(other: Interval): bool {
    return self.low <= other.high && other.low <= self.high;
}
```

Then change `version` to `"0.2.0"` in `turf.toml`, commit and tag:

```text
$ git diff --stat
 src/interval.tr | 4 ++++
 turf.toml       | 2 +-
 2 files changed, 5 insertions(+), 1 deletion(-)
$ git commit -am "Interval.overlaps"
$ git tag v0.2.0
```

A dependent upgrades on purpose, by naming the new tag. If it forgets to
commit the lock, a CI step with `--locked` says so:

```text
$ sed -i 's/v0.1.0/v0.2.0/' turf.toml
$ turf build --locked
turf: fetching interval (/home/me/code/interval)
turf: turf.lock is out of date, and --locked forbids changing it (nothing was written):
  changed 'interval': ref tag=v0.1.0 -> tag=v0.2.0; commit 534ea21001c3d0aeb1a0fd76efffbda440e0d9e8 -> ae406e9d79646f5274ac4fa94d4396688fcbb93f; hash sha256:af96dcd16d4bf4eeec70024ad1dee67b9352cc616861814b5051eb296fe3d342 -> sha256:539abb92396fbb8cfb5bb60deed8045e6937b4eee3502041a7b21a08b82f60a3
hint: run `turf build` or `turf fetch` without --locked, and commit the new turf.lock
$ turf build
turf: fetching interval (/home/me/code/interval)
Compiled to build/fares.o
Successfully compiled to build/fares
```

A tag that does not exist is an error that names it:

```text
turf: /home/me/code/interval has no tag named 'v9.9.9' (dependency 'interval' in /home/me/code/fares/turf.toml)
```

Treat a tag as final. A dependent that already locked `v0.1.0` keeps the old
commit until it runs `turf update`, so re-pointing the tag splits your users
between two versions of one name. Fix a mistake with another tag.

### One version per package name

A build has one package per name. If your library is used by two packages that
need it at different tags, the build stops and names both:

```text
turf: cannot add 'rates' (nothing was written): package 'interval' is required at two different versions of git /home/me/code/interval, and a build has one package per name:
  package 'fares' (/home/me/code/fares/turf.toml) needs it at git /home/me/code/interval (tag=v0.2.0, commit ae406e9d7964)
  package 'rates' (/home/me/code/rates/turf.toml) needs it at git /home/me/code/interval (tag=v0.1.0, commit 534ea21001c3)
```

There is no way for a user to ask for both. The user fixes it by moving one of
the two to the other's tag, and that needs the author of `rates` to cut a
release. For you, as the author, this means:

- Keep a library's dependencies few, and pin each to a tag.
- Treat each tag as something every dependent has to adopt together. Prefer
  adding to the API over changing it.
- A dependent can import its own dependencies and the standard library, not your
  dependencies. If your public API returns a type from one, a user who wants to
  name that type adds the package to their `turf.toml` too.

## Wrap a C library

A library can bind a C library and give users a safe API over it. Take a small
C library that keeps a running total, `libtally`:

```c
#include <stdlib.h>

typedef struct { long total; long count; } tally;

tally *tally_new(void) {
    return calloc(1, sizeof(tally));
}

void tally_add(tally *t, long n) {
    t->total += n;
    t->count += 1;
}

long tally_total(const tally *t) {
    return t->total;
}

long tally_count(const tally *t) {
    return t->count;
}

void tally_free(tally *t) {
    free(t);
}
```

For this walkthrough, build it as a static archive in a directory of its own.
Normally you install the library's development package instead:

```text
$ cc -c -O2 tally.c -o tally.o
$ ar rcs libtally.a tally.o
```

The package `counter` declares the C functions with `extern fn` and wraps them.
An `extern fn` takes the C types: `ptr` for a pointer, `int` for a C `long`
(see the [type table](../reference/ffi_and_unsafe.md#1-extern-fn)). `extern from
"tally"` on one declaration adds `-ltally` to the link of every program that
uses the package. The raw bindings stay private; the library's API is the
`Counter` type:

<!-- doc-example: skip a file with unsafe module; needs the libtally archive -->
```turflang
unsafe module;

// The root module of the package 'counter'. It wraps the C library libtally.
// The raw bindings are private; only the `Counter` type below is public.

extern from "tally" fn tally_new(): ptr;
extern fn tally_add(t: ptr, n: int): void;
extern fn tally_total(t: ptr): int;
extern fn tally_count(t: ptr): int;
extern fn tally_free(t: ptr): void;

pub error CounterError {
    OutOfMemory
}

// A running total kept by the C library. Call `close()` when you are done.
pub struct Counter {
    handle: ptr,
    open: bool
}

pub fn Counter.start() static: !Counter {
    mut c: Counter = Counter::default();
    c.handle = tally_new();
    if c.handle == null {
        return CounterError::OutOfMemory("tally_new returned null");
    }
    c.open = true;
    return c;
}

@caller_location
fn Counter.require_open(): void {
    if !self.open {
        sys::panic(caller_location + ": Counter used after close()");
    }
}

@caller_location
pub fn Counter.add(n: int): void {
    self.require_open();
    tally_add(self.handle, n);
}

@caller_location
pub fn Counter.total(): int {
    self.require_open();
    return tally_total(self.handle);
}

@caller_location
pub fn Counter.count(): int {
    self.require_open();
    return tally_count(self.handle);
}

// Safe to call twice, like the standard library's close() methods.
pub fn Counter.close(): void {
    if self.open {
        tally_free(self.handle);
        self.open = false;
    }
}
```

The file begins with `unsafe module;` because `Counter` holds a raw `ptr` in a
field, and a raw pointer is only allowed in an `unsafe module;` file or inside
an `unsafe { }` block (a struct declaration with a raw-pointer field has to sit
in one of them). Keep the field private so a user cannot reach the pointer.

The API follows the standard library's habits. `start()` returns `!Counter`
for the failure the environment can cause. `close()` frees the handle and is
safe to call twice. Using a counter after `close()` is a caller bug, so it
panics at the caller's line. The full rules for `extern fn`, raw pointers and
linking are in [FFI and unsafe](../reference/ffi_and_unsafe.md).

`turf.toml` has no link settings: the library name travels with the `extern
from` declaration. `turf check` works without the C library installed. A build
that links a program needs it, and the system C compiler does the linking when a
program names a library beyond the C runtime:

```text
$ turf run
/usr/bin/ld: cannot find -ltally: No such file or directory
collect2: error: ld returned 1 exit status
Compiled to build/counter.o
Linking failed (exit code 1)
  This program links against "tally", which Turf does not bundle. If the linker could not find it, install that library's development files (the import library or .so/.dylib/.a, not only the runtime DLL) and, if they are not in a default location, add the directory to LIBRARY_PATH.
$ LIBRARY_PATH=/home/me/code/tally-c turf run
Compiled to build/counter.o
Successfully compiled to build/counter
total 12 over 2 adds
```

Every user of `counter` needs the C library installed too. Say so in the
README.

### The dependent grants the consent

A package that uses `unsafe module;` removes compiler checks for a whole file,
so a dependent has to agree to it. A dependency's `unsafe module;` is honored
only if the dependent's own `turf.toml` lists the package under `[unsafe]`. Here
is a program that depends on `counter`:

<!-- doc-example: skip uses the counter package -->
```turflang
import io;
import counter;

fn main(): int {
    mut c: counter::Counter = counter::Counter::start() recover run {
        io::printline("could not start a counter");
        return 1;
    };
    defer c.close();
    c.add(40);
    c.add(2);
    io::printline(c.total());
    return 0;
}
```

Without the consent, the build fails in the library's file:

```text
$ turf build
/home/me/code/counter/src/counter.tr (1 error)

── error at line 17, col 5 [E130 · memory error]

  Unsafe pointer use!
  Raw pointer type 'ptr' cannot be used outside an 'unsafe' block.

  Hint: Wrap this in 'unsafe { }', or use a safe reference ('&T') instead if you don't actually need a possibly-null raw pointer.

  17 │ pub struct Counter {
     │     ^~~~~~ Here!
```

The dependent adds the table, and the same build compiles:

```toml
[dependencies]
counter = { path = "../counter" }

[unsafe]
allow = ["counter"]
```

```text
$ turf run
Compiled to build/sums.o
Successfully compiled to build/sums
42
```

The consent has limits you should document for your users. It is not
transitive: a program that depends on a package that depends on `counter` lists
`counter` itself. A dependency cannot allow itself, and
`--allow-unsafe-modules` does not apply in a project. Only list a package whose
author you trust to get raw memory right.

Because `defer c.close();` runs when `main` ends, the C allocation is freed on
every way out. If a user does call a method after `close()`, the panic names
their line:

```text
src/main.tr:13: Counter used after close()
```

If a binding only passes numbers and strings, with no stored pointer, it needs no
`unsafe module;` and no consent from anyone. Prefer that design when the C API
allows it.

## There is no registry

A library is a git repository and a tag, and that is the whole distribution
story. `turf publish` exists only to say so:

```text
$ turf publish
turf: the package registry is not part of TurfLang 2.0; publish a library by pushing a git tag
```

There are no version ranges either: a dependent names one tag, one commit or one
branch. Put the repository URL and the tag you recommend in your README, and
tag every release.

## Where next

- [Working on a TurfLang project](projects.md): the programs that use your
  library.
- [Testing](testing.md): the test workflow in depth.
- [Projects and packages](../reference/projects_and_packages.md): dependencies,
  the lock file, the cache and the import rules.
- [FFI and unsafe](../reference/ffi_and_unsafe.md): everything about `extern fn`
  and raw pointers.
