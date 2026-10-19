# The standard library: conventions and the 2.x surface

This page says how the standard library is shaped: how it names things, how its
functions report failure, how it handles resources, what is public, and what a
2.x program can rely on. The other pages in this folder document one module
each.

## Modules

| Module | For | Import | Page |
|---|---|---|---|
| `int`, `double`, `bool`, `char` | helpers on the primitive types: `n.gcd(m)`, `x.round()`, `c.is_digit()` | none (prelude) | [core.md](core.md) |
| `strings` | string search, slicing, trimming, splitting, parsing, `StringBuilder` | none (prelude) | [core.md](core.md#strings) |
| `array` | `sum`, `max`, `min`, `contains`, `index_of` over `int[]`, `double[]`, `string[]` | none (prelude) | [core.md](core.md#array) |
| `io` | printing, reading input, reading and writing files, `exists` | none (prelude) | [io_and_net.md](io_and_net.md) |
| `math` | `abs`, `min`, `max`, `clamp`, `power`, libm functions, `divide`, constants | none (prelude) | [math_and_os.md](math_and_os.md) |
| `sys` | `panic`, `exit`, `abort` | none (prelude) | [io_and_net.md](io_and_net.md) |
| `arrays` | build a whole array: `fill`, `generate`, `reversed`, `map` | `import arrays;` | [arrays.md](arrays.md) |
| `list` | `List<T>`, a growable array | `import list::List;` | [collections.md](collections.md) |
| `stack` | `Stack<T>`, last in first out | `import stack::Stack;` | [collections.md](collections.md) |
| `queue` | `Queue<T>`, first in first out | `import queue::Queue;` | [collections.md](collections.md) |
| `map` | `Map<K, V>`, a hash map | `import map::Map;` | [collections.md](collections.md) |
| `set` | `Set<T>`, a hash set | `import set::Set;` | [collections.md](collections.md) |
| `hashing` | `hash(x)`, the hash `Map` and `Set` use | `import hashing;` | [collections.md](collections.md) |
| `net` | TCP client and server sockets | `import net;` | [io_and_net.md](io_and_net.md) |
| `fs` | path strings and directories | `import fs;` | [fs.md](fs.md) |
| `os` | environment variables and command-line arguments | `import os;` | [math_and_os.md](math_and_os.md) |
| `time` | the clock, elapsed time, sleeping | `import time;` | [time.md](time.md) |
| `random` | pseudo-random numbers | `import random;` | [random.md](random.md) |
| `mem` | manual-heap allocation and raw memory | `import mem;` | [mem.md](mem.md) |
| `json` | parse and build JSON | `import json;` | [json.md](json.md) |
| `http` | a small HTTP/1.1 server and request/response types | `import http;` | [http.md](http.md) |
| `test` | `assert`, `assert_eq`, `fail` | `import test;` | [testing.md](testing.md) |

A module outside the prelude must be imported in each file that writes its
qualifier: `import list;` lets a file write `list::List<int>`, while
`import list::List;` lets it write `List<int>` but not `list::List`. A file that
needs both writes both. See [Functions and modules](../functions_and_modules.md#5-modules).

## The prelude

Nine modules are loaded into every program, with no `import`:

```text
int  double  bool  char  strings  array      reached as methods, operators and strings::length(s)
io   math    sys                              also callable by their bare names
```

- The six on the first line are reached through method-call syntax (`c.is_digit()`,
  `s.trim()`), through operators (`a + b` on strings), and through the `::`
  qualifier for `strings` and `array` (`strings::length(s)`). They are not
  callable by a bare name: `is_digit(c)` is E041.
- Every public function of `io`, `math` and `sys` is also callable bare:
  `printline("hi")`, `abs(-3)`, `max(a, b)`, `sqrt(2.0)`, `exit(1)`,
  `panic("message")`. Constants stay qualified (`math::PI`).
- A bare name is the last thing looked up, so a function of your own with the
  same name hides the prelude's; `math::max(...)` still reaches it.

The full lookup order and the one trade-off it carries are in
[Functions and modules: the prelude](../functions_and_modules.md#7-the-prelude).

## 1. Naming

Every public function, method and field is `snake_case` (`index_of`, `try_get`,
`is_digit`, `to_upper`). Types are `PascalCase` (`List`, `StringBuilder`,
`TcpConnection`) and constants `UPPER_CASE` (`PI`, `CLOCKS_PER_SEC`). The
library has one spelling for each name, with no aliases. A script checks this
on every CI run (`scripts/check/stdlib_surface_check.sh`).

## 2. Failure: three kinds, three shapes

A function reports failure in the way that matches *why* it can fail.

| Why it fails | Shape | Examples |
|---|---|---|
| **The input or the environment**: a file that is not there, text that is not a number, a refused connection. Not a bug in the caller. | returns `!T`, with an error variant that carries what failed (the path, the offset, the name) and a message saying so (`err.message`) | `io::read_to_string`, `io::write_string`, `fs::mkdir`, `os::set_env`, `strings::parse_int`, `json::parse`, `math::divide`, `net::TcpConnection::connect`, `Map.try_get` |
| **A violated precondition**: popping an empty stack, an index out of range, `random_int(7, 3)`. A bug in the caller that no recovery makes correct. | panics, and the message starts with the **caller's** `file:line:`, so you learn which of your calls was wrong, not which line of the library noticed | `List.pop`, `Stack.pop`, `Queue.dequeue`, `Map.get`, `strings::substring`, `JsonValue.get`, `random::random_int` |
| **A question**: is it there, is it a directory, does it start with this? Absence is a normal answer. | returns `bool`, or a sentinel (`-1` from `index_of`, `""` from `os::env` for an unset variable) | `io::exists`, `fs::is_dir`, `Set.contains`, `strings::index_of` |

Some failures inside a function, such as an array index past the end or an
integer overflow, are panics reported by the language; see
[Memory and safety: checks that panic](../memory_and_safety.md#5-checks-that-panic).

Where a call has both a panicking and a recoverable form, they are a pair, and
a predicate lets you ask first:

| Panics on a miss | Recoverable twin | Ask first |
|---|---|---|
| `Map.get(k)` | `Map.try_get(k): !V` | `Map.contains(k)` |
| `JsonValue.get(k)` | none | `JsonValue.has(k)` |
| `http::Request.get_header(n)` | none | `Request.has_header(n)` |
| `http::Server.get_route(p)` | none | `Server.has_route(p)` |

A `!void` result is handled like any other `!T`: `fs::mkdir(p) recover run { ... };`
runs the block only on failure ([Error handling](../error_handling.md)).
Leaving the result unhandled discards the failure silently, so check anything
whose failure matters.

```turflang
import fs;

fn main(): int {
    mut dir: string = "/tmp/turf_stdlib_readme_dir";
    fs::mkdir(dir) recover run {
        printline("could not create the directory");
        return 1;
    };
    fs::remove_dir(dir) recover run { printline("could not remove it"); };
    printline(fs::is_dir(dir));       // false: a question, so a bool
    return 0;
}
```

## 3. Resources and `defer`

Turf has no finalizers: the collector reclaims memory, not file descriptors. A
value that owns an operating-system resource has an explicit `close()`, and the
idiom is `defer`, which runs on every way out of the block, including each pass
of a loop:

```turflang
import net;

fn main(): int {
    mut conn: net::TcpConnection = net::TcpConnection::connect(127, 0, 0, 1, 8080) recover run {
        printline("could not connect");
        return 1;
    };
    defer conn.close();
    conn.write("ping");
    return 0;
}
```

`TcpConnection.close()` and `TcpListener.close()` are idempotent (a second call
does nothing), so an early explicit close alongside the `defer` is safe.
`raw_fd()` returns the descriptor (`-1` once closed) for passing to FFI.

## 4. Visibility: what is public

A module's declarations are private unless marked `pub`
([Functions and modules](../functions_and_modules.md#8-visibility-pub)). The library follows
this rule:

- **A function that does a job is public**, whether or not the library itself
  calls it. `io::input`, `test::assert_neq` and `strings::last_index_of` are
  public because a program may want them.
- **A struct's internals are public only if a real program does something with
  them.** `Request.method` is public (a handler reads it); `List`'s buffer,
  `Map`'s hash index and `Server`'s route table are not.
- **Raw C bindings are private.** The library wraps `fopen`, `getenv`, `socket`
  and the rest in checked Turf functions; the wrappers are the interface. The
  exceptions are the bindings that are public as they stand (`mem::malloc`,
  `time::usleep`, `sys::exit`, the libm functions in `math`), which are part of
  the surface.

A program that needs a C function the library does not wrap declares its own
`extern fn` for it ([FFI and unsafe](../ffi_and_unsafe.md)).

## 5. Stability: what 2.x guarantees

The public items of the standard library are the 2.x compatibility surface: a
`pub` function, method, type, error, constant or field is not removed, renamed,
or given a different signature or failure shape before 3.0. Additions are
allowed in a minor release. This is checked, not just intended:

- `tests/46_stdlib_surface/` compiles and runs a use of every public item, so a
  removal or rename breaks a test.
- `scripts/check/stdlib_surface_check.sh` fails if a public item exists that no
  file there mentions (a new name added without being locked) or that is not
  `snake_case`. It runs in CI.
- `scripts/check/prelude_shadow_check.sh NAME` is run before a name is added to
  `io`, `math` or `sys`, whose public functions are callable bare: it lists the
  programs in `showcase/`, `examples/`, `tests/` and `lib/std` that already
  declare a function of that name.
- Every module has its own behaviour tests.

Three things are not part of the surface: the behaviour of `json::parse` for
`\uXXXX` escapes (a known limitation, see [json.md](json.md)), the iteration
order of `Map.keys()` after a `remove()`, and `mem::malloc_usable_size` and
`mem::usable_size`, which depend on the C library and may be unavailable on a
platform.
