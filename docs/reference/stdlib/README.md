# The standard library: conventions and the 2.x surface

This page is the contract behind every other file in this folder. The
reviewed, frozen public surface of the standard library is what a 2.x program
can rely on; the rules below say how it is shaped and how to read it.

| Module | Page |
|---|---|
| `int`, `double`, `bool`, `char`, `strings`, `array` (the **prelude**, always in scope) | [core.md](core.md) |
| `list`, `stack`, `queue`, `map`, `set`, `hashing` | [collections.md](collections.md) |
| `io`, `net`, `sys` | [io_and_net.md](io_and_net.md) |
| `fs` | [fs.md](fs.md) |
| `math`, `os` | [math_and_os.md](math_and_os.md) |
| `time` | [time.md](time.md) |
| `random` | [random.md](random.md) |
| `mem` | [mem.md](mem.md) |
| `json` | [json.md](json.md) |
| `http` | [http.md](http.md) |
| `test` | [testing.md](testing.md) |

## 1. Naming

Every public function, method and field is `snake_case` (`index_of`,
`try_get`, `is_digit`, `to_upper`). Types are `PascalCase` (`List`,
`StringBuilder`, `TcpConnection`) and constants `UPPER_CASE` (`PI`,
`CLOCKS_PER_SEC`). Until 2.0 the library mixed in a dozen camelCase names
(`isDigit`, `indexOf`, `tryGet`); they were renamed once, without aliases, so
the surface does not carry two spellings forever. A script checks this on every
CI run (`scripts/check/stdlib_surface_check.sh`).

## 2. Failure: three kinds, three shapes

A function reports failure in the way that matches *why* it can fail.

| Why it fails | Shape | Examples |
|---|---|---|
| **The input or the environment** — a file that is not there, text that is not a number, a refused connection. Not a bug in the caller. | returns `!T`, with an error variant that carries what failed (the path, the offset, the name) | `io::read_to_string`, `io::write_string`, `fs::mkdir`, `os::set_env`, `strings::parse_int`, `json::parse`, `math::divide`, `net::TcpConnection::connect`, `Map.try_get` |
| **A violated precondition** — popping an empty stack, an index out of range, `random_int(7, 3)`. A bug in the caller that no recovery makes correct. | panics, and the message starts with the **caller's** `file:line:` (`@caller_location`), so you learn which of your calls was wrong, not which line of the library noticed | `List.pop`, `Stack.pop`, `Queue.dequeue`, `Map.get`, `strings::substring`, `JsonValue.get`, `random::random_int` |
| **A question** — is it there, is it a directory, does it start with this? Absence is a normal answer. | returns `bool`, or a sentinel (`-1` from `index_of`, `""` from `os::env` for an unset variable) | `io::exists`, `fs::is_dir`, `Set.contains`, `strings::index_of` |

Where a call has both a panicking and a recoverable form, they are a pair, and
a predicate lets you ask first:

| Panics on a miss | Recoverable twin | Ask first |
|---|---|---|
| `Map.get(k)` | `Map.try_get(k): !V` | `Map.contains(k)` |
| `JsonValue.get(k)` | — | `JsonValue.has(k)` |
| `http::Request.get_header(n)` | — | `Request.has_header(n)` |
| `http::Server.get_route(p)` | — | `Server.has_route(p)` |

Handling an `!void` result: `fs::mkdir(p) recover run { ... };` runs the block
only on failure. Leaving the result unhandled discards the failure silently
(the same as ignoring a `bool` did), so check anything whose failure matters.

## 3. Resources and `defer`

Turf has no finalizers (a collector reclaims memory, not descriptors). A value
that owns an operating-system resource has an explicit `close()`, and the idiom
is `defer`, which runs on every way out of the block, including each pass of a
loop:

```turflang
mut conn: net::TcpConnection = net::TcpConnection::connect(127, 0, 0, 1, 8080) recover run { return 1; };
defer conn.close();
```

`TcpConnection.close()` and `TcpListener.close()` are idempotent — a second call
does nothing — so an early explicit close alongside the `defer` is safe.
`raw_fd()` returns the descriptor (`-1` once closed) for passing to FFI.

## 4. Visibility: what is public

Everything a module declares is private unless it says `pub`. The rule the
library follows is:

- **A function that does a job is public**, whether or not the library itself
  calls it. `io::input`, `test::assert_neq` and `strings::last_index_of` are
  public because a program may want them, not because the library needs them.
- **A struct's internals are public only if a real program does something with
  them.** `Request.method` is public (a handler reads it); `List`'s buffer,
  `Map`'s hash index and `Server`'s route table are not.
- **Raw C bindings are private.** The library wraps `fopen`, `getenv`, `socket`
  and the rest in checked Turf functions; the wrappers are the interface.

A program that needs a C function the library does not wrap declares its own
`extern fn` for it.

## 5. Stability: what 2.x guarantees

The public items of `lib/std/**` are the 2.x compatibility surface: they will
not be removed, renamed, or have their signature or failure shape changed before
3.0. Additions are allowed in a minor release. Three things make that a
checked promise, not an intention:

- `tests/46_stdlib_surface/` compiles and runs a use of every public item, so a
  removal or rename breaks a test.
- `scripts/check/stdlib_surface_check.sh` fails if a public item exists that no file
  there mentions (a new name added without being locked) or is not
  `snake_case`. It runs in CI and in `scripts/dev/session_postflight.sh`.
- Every module has its own behaviour tests under `tests/`.

Exceptions, stated plainly: `mem::malloc_usable_size` and `mem::usable_size`
are glibc functions and are not available on every platform 2.0 ships to
(tracked with the platform work); the behaviour of `json::parse` for `\uXXXX`
escapes is a documented limitation, not a promise.

## 6. Changed in 2.0

Each of these was deliberate and is a breaking change from 1.x (LEC-007); the
change record is `docs/development/changes/LEC-007.md`.

| Was | Is |
|---|---|
| `char`: `isDigit` `isUpper` `isLower` `isAlpha` `isAlphaNumeric` `isPunctuation` `isWhitespace` `toUpper` `toLower` | `is_digit` `is_upper` `is_lower` `is_alpha` `is_alphanumeric` `is_punctuation` `is_whitespace` `to_upper` `to_lower` |
| `Map.indexOf`, `Set.indexOf`, `Map.tryGet` | `index_of`, `index_of`, `try_get` |
| `list::list_sort`, `list_binary_search`, `list_find` | removed — `l.sort(cmp)`, `l.binary_search(x, cmp)`, `l.find(pred)` |
| `io::write_string(p, s): bool` | `: !void`, fails with `FileError::WriteFailed(path)` |
| `fs::mkdir`, `remove_file`, `remove_dir`: `bool` | `!void`, fail with `FsError::{MkdirFailed, RemoveFileFailed, RemoveDirFailed}(path)` |
| `os::set_env(...): bool` | `!void`, fails with `OsError::SetEnvFailed(name)` |
| `http::Server.listener`, `.routes` public fields | private |
| `strings::repeat` / `replace` / `join` quadratic | linear, built on the new `StringBuilder` |

Added: `strings::StringBuilder`, `strings::last_index_of`, `Map.keys()`,
`Map.values()`, `json::escape_string` (public), `TcpConnection.raw_fd()`,
`TcpListener.raw_fd()`.
