# I/O: `io`, `net`, `sys`

Console I/O, TCP networking, and low-level process control. None of these
are prelude — each requires an explicit `import`.

```turflang
import io;
import net;
import sys;
```

Like the rest of the standard library, these modules are a thin
Turf-level wrapper layer over raw C/POSIX bindings declared with
`extern fn` — there's no compiler-intrinsic magic involved (the old
`@intrinsic("print"/"printline"/"input")` mechanism has been fully removed).
`io.tr` delegates to libc `printf`/`scanf`/`getchar`/`calloc`; `net.tr`
wraps raw POSIX `socket`/`bind`/`listen`/`accept`/`connect`/`recv`/`send`/
`close`/`setsockopt`; `sys.tr` wraps `write`/`abort`/`exit` syscalls.

---

## `io`

### `print(val: int|double|bool|char|string): void`

Writes `val` with no trailing newline. Format per type: `%lld` for `int`,
`%g` for `double`, `%d` for `bool` (prints `0`/`1`, not `"true"`/`"false"`),
`%c` for `char`, `%s` for `string`. Dispatch to the right format happens via
ordinary function overload resolution, not runtime type inspection.

### `printline(val: int|double|bool|char|string): void`

Same as `print`, plus a trailing `\n`. Used pervasively throughout the test
suite and every showcase program.

```turflang
io::printline("hello world");
```

### `input(): string`

Reads one line from stdin (up to 4095 characters). Internally: `calloc`s a
zeroed 4096-byte buffer, reads with `scanf("%4095[^\n]", buf)` (stops at
`\n`, leaving the newline itself unconsumed in the stream), then calls
`getchar()` once to consume that leftover `\n` so it doesn't leak into the
*next* `input()` call.

```turflang
io::print("Enter your name: ");
immut name: string = io::input();
io::printline("Hello, " + name + "!");
```

### File I/O: `error FileError`, `read_to_string`, `read_lines`, `write_string`, `exists`

```turflang
error FileError { NotFound(path: string), ReadFailed(path: string) }
```

Each variant carries the path it failed on: `matches io::FileError::NotFound(path):`
in a `compare` over `err`. `err == io::FileError::NotFound` still works and
compares the variant only.

- **`read_to_string(path: string): !string`** — reads an entire file into
  one string (`fopen`/`fseek`/`ftell`/`fread`). `FileError::NotFound` if
  the file can't be opened, `FileError::ReadFailed` on a short read.
- **`read_lines(path: string): !List<string>`** — `read_to_string` + a
  newline split, with exactly one spurious trailing empty element (caused
  by the file's own final `\n`) dropped; a genuine blank line before EOF is
  preserved.
- **`write_string(path: string, content: string): bool`** — opens `"wb"`,
  writes, closes. `true` iff the write was complete.
- **`exists(path: string): bool`** — `true` if the path can be opened for
  reading.

```turflang
io::write_string("notes.txt", "hello\nworld\n");
immut content: string = io::read_to_string("notes.txt") recover produce "";
test::assert(io::exists("notes.txt"), "...");
```

### `eprint(val: string): void` / `eprintline(val: string): void` / `flush(): void`

`eprint`/`eprintline` write to stderr via a raw `write(2, ...)` syscall
(not a buffered `FILE*`), mirroring `sys::panic`'s own approach rather than
risking a leaked or double-closed stream handle. `flush()` calls
`fflush(NULL)`, which glibc documents as flushing every open output stream.

```turflang
io::eprintline("this goes to stderr, not stdout");
io::flush();
```

---

## `net`

TCP networking over two structs: `TcpListener` (server-side, listening) and
`TcpConnection` (either an accepted inbound connection or a dialed outbound
one — the same struct either way, since a connected socket is symmetric).

Addresses are four separate `int` octets — `connect(127, 0, 0, 1, 8080)` —
not a `"127.0.0.1"` string. `struct sockaddr_in` needs a raw 32-bit address
in a specific byte layout, and `strings.tr` has no dotted-decimal parser
(yet). Internally, `buildSockAddr` packs family/port/address into one
integer written through a `void* to int*` indexed store.

Exported constants, useful if you're building further raw-socket code
against the same primitives (e.g. a hypothetical future `UdpSocket`):

| Constant | Value |
|---|---|
| `AF_INET` | `2` |
| `SOCK_STREAM` | `1` |
| `SOL_SOCKET` | `1` |
| `SO_REUSEADDR` | `2` |

### `error NetworkError`

```turflang
error NetworkError {
    SocketCreationFailed,
    ConnectionRefused,
    BindFailed,
    ListenFailed,
    AcceptFailed,
    ResolutionFailed,
}
```

One variant per distinct syscall failure point, kept separate rather than a
single generic `Failed`, so a `recover as err run { ... }` block can
distinguish e.g. `ConnectionRefused` from a local `socket()` failure.

### `resolve_hostname(host: string): !string`

Resolves a hostname to a dotted-decimal IPv4 string, so a caller can dial
by name instead of by raw octets — `net::resolve_hostname("localhost")`
returns `"127.0.0.1"`. `NetworkError::ResolutionFailed` if the name doesn't
resolve.

Implemented via the classic (deprecated but ABI-stable) glibc
`gethostbyname`, read back through manual byte-offset pointer arithmetic
rather than a Turf struct mirroring `struct hostent`'s C layout (this
compiler can't bind an arbitrary C struct's field layout — see
[`docs/architecture/pointers_and_references.md`](../../architecture/pointers_and_references.md)
for why). Verified end-to-end: resolving `"localhost"` and dialing the
result with `TcpConnection::connect` reaches a real listening socket.

```turflang
immut ip: string = net::resolve_hostname("localhost") recover produce "127.0.0.1";
mut conn: net::TcpConnection = net::TcpConnection::connect(127, 0, 0, 1, 8080) recover run {
    io::printline("could not connect");
    return 1;
};
```

### `struct TcpConnection { fd: int }`

#### `TcpConnection::connect(a: int, b: int, c: int, d: int, port: int) static: !TcpConnection`

Dials `a.b.c.d:port`. Produces `NetworkError::SocketCreationFailed` if the
local `socket()` call fails, or `NetworkError::ConnectionRefused` if
`connect()` fails (e.g. nothing listening on the other end).

```turflang
mut conn: net::TcpConnection = net::TcpConnection::connect(127, 0, 0, 1, 8080) recover as err run {
    if err == net::NetworkError::ConnectionRefused {
        io::printline("Connection refused -- is showcase/http_server.tr running on port 8080?");
    } else {
        io::printline("Failed to connect (socket() error).");
    }
    return 1;
};
```

#### `TcpConnection.read(maxBytes: int): string`

Allocates a `maxBytes + 1`-byte zeroed buffer and issues **one** `recv()`
call into it (blocks until *some* data arrives, or the peer closes), then
returns whatever arrived as a `string`. Possibly shorter than `maxBytes`,
possibly `""` on a closed connection.

> **Note: does not loop to fill `maxBytes`.** One `recv()` call only — it's
> up to the caller to loop if they need to guarantee reading a specific
> amount. Also note the returned buffer is deliberately never freed before
> return: `string` and `void*` are the same opaque pointer at the LLVM
> level, so `buf to string` doesn't copy — the returned string *is* the
> buffer, and there's no GC yet to reclaim it (a documented, small,
> intentional leak per call).

```turflang
immut request: string = conn.read(4096);
```

#### `TcpConnection.write(data: string): void`

Sends the entirety of `data` (`strlen(data)` bytes) in one `send()` call.

```turflang
conn.write("hello back");
```

#### `TcpConnection.close(): void`

Closes the file descriptor. Safe to call once; the fd is not reusable
afterward.

### `struct TcpListener { fd: int }`

#### `TcpListener::bind(port: int) static: !TcpListener`

Binds all interfaces (`INADDR_ANY`) on `port`. Sets `SO_REUSEADDR` first, so
a server restart doesn't fail with "address already in use" from a
lingering `TIME_WAIT` socket, then calls `listen(fd, 128)` (backlog 128).
Produces `NetworkError::SocketCreationFailed`, `BindFailed`, or
`ListenFailed` on the respective failure.

```turflang
mut server: net::TcpListener = net::TcpListener::bind(8080) recover run {
    io::printline("Could not start the server -- is port 8080 already in use?");
    return 1;
};
```

#### `TcpListener.accept(): !TcpConnection`

Blocks until a client connects; produces `NetworkError::AcceptFailed` on
error. The idiomatic recovery inside a serve loop is `recover run {
continue; }`, so one bad `accept()` doesn't take down the whole server.

```turflang
mut conn: net::TcpConnection = server.accept() recover run {
    io::printline("accept() failed, skipping.");
    continue;
};
```

#### `TcpListener.close(): void`

Stops listening and closes the socket.

### Full example: an HTTP server and client, entirely in Turf

`showcase/http_server.tr` and `showcase/http_client.tr` are a real
HTTP/1.1 server and client pair, both written in Turf against the `net`
module directly (no raw sockets, no `extern fn`, no manual memory
management in either file). The server:

```turflang
import io;
import net;
import strings;

fn handle(conn: net::TcpConnection): void {
    immut request: string = conn.read(4096);

    mut body: string = "";
    mut status: string = "200 OK";

    if strings::starts_with(request, "GET / ") {
        body = "Hello from Turf!";
    } elseif strings::starts_with(request, "GET /time") {
        body = "the Turf web server is alive";
    } else {
        status = "404 Not Found";
        body = "not found";
    }

    immut bodyLen: int = strings::length(body);
    immut response: string =
        "HTTP/1.1 " + status + "\nContent-Type: text/plain\nContent-Length: "
        + (bodyLen to string) + "\nConnection: close\n\n" + body;
    conn.write(response);
    conn.close();
}

fn main(): int {
    mut server: net::TcpListener = net::TcpListener::bind(8080) recover run {
        io::printline("Could not start the server -- is port 8080 already in use?");
        return 1;
    };

    mut requestCount: int = 0;
    while requestCount < 1000 {
        mut conn: net::TcpConnection = server.accept() recover run {
            io::printline("accept() failed, skipping.");
            continue;
        };
        handle(conn);
        requestCount = requestCount + 1;
    }

    server.close();
    return 0;
}
```

And the matching client:

```turflang
import io;
import net;
import strings;

fn main(): int {
    immut path: string = "/";
    immut request: string = "GET " + path + " HTTP/1.1\nHost: 127.0.0.1:8080\nConnection: close\n\n";

    mut conn: net::TcpConnection = net::TcpConnection::connect(127, 0, 0, 1, 8080) recover as err run {
        if err == net::NetworkError::ConnectionRefused {
            io::printline("Connection refused -- is showcase/http_server.tr running on port 8080?");
        } else {
            io::printline("Failed to connect (socket() error).");
        }
        return 1;
    };
    conn.write(request);

    immut response: string = conn.read(4096);
    conn.close();
    io::printline(response);
    return 0;
}
```

`showcase/kv_store_server.tr` / `showcase/kv_store_client.tr` are a second,
larger real-world consumer of `net` — a key-value store protocol over the
same API, combined with `mem::alloc`/pointer manipulation.

---

## `sys`

Low-level process control, and the shared `panic` primitive that every
other stdlib module's internal guard rails build on.

### `abort(): void`

Raw `extern fn`, called directly as `sys::abort()`. There's deliberately no
Turf-level wrapper of the same name — a same-signature `fn abort(): void {
abort(); }` wouldn't be an overload of the extern declaration, it would be a
duplicate declaration (a compile error), and would recurse into itself even
if it weren't.

### `exit(code: int): void`

Raw `extern fn`. `sys::exit(code)` terminates the process immediately with
the given exit code and never returns.

```turflang
test::assert(true, "reached before exiting");
sys::exit(0);
// unreachable: sys::exit never returns
```

### `STDERR_FD: int = 2`

Exported constant for the stderr file descriptor.

### `panic(msg: string): void`

Writes `msg` plus a trailing `\n` to stderr via two separate raw `write(2,
...)` syscalls, then calls `abort()`. **Never returns.**

Every stdlib module's internal "this should be impossible" guard ultimately
calls this — OOM checks in `mem.tr`, socket failures in `net.tr`,
`List.pop()`/`Map.get()` on empty/missing, `strings::substring` bounds
checks.

> **Design note.** The two `write()` calls are deliberately not merged into
> one `msg + "\n"` write. String `+` is an `operator` overload defined in
> `strings.tr`, and `strings.tr` itself needs `sys.tr` for its own panics
> (`substring`'s bounds check) — `sys.tr` importing `strings.tr` back would
> be a circular import, which the language doesn't support. Avoiding string
> arithmetic entirely inside `sys.tr` breaks that cycle.

```turflang
sys::panic("something went wrong");
```
