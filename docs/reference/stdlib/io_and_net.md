# I/O and networking: `io`, `sys`, `net`

Three modules for talking to the outside world:

| Module | For | Import |
|---|---|---|
| `io` | printing, reading a line of input, reading and writing whole files | none (prelude) |
| `sys` | ending the program: `exit`, `abort`, `panic` | none (prelude) |
| `net` | TCP client and server sockets | `import net;` |

`io` and `sys` are part of the [prelude](README.md#the-prelude): their functions
are callable bare (`printline("hi")`, `exit(1)`) or qualified (`io::printline`,
`sys::exit`), with no `import`. `net` needs `import net;`. The conventions that
every function below follows (which failures return `!T`, which panic, how
`defer` closes a resource) are on the [standard library overview](README.md).

---

## `io`

### Printing

```text
print(val: int): void          printline(val: int): void
print(val: double): void       printline(val: double): void
print(val: bool): void         printline(val: bool): void
print(val: char): void         printline(val: char): void
print(val: string): void       printline(val: string): void
```

`print` writes `val` to standard output; `printline` writes it and a newline.
The overload is chosen by the argument's type:

| Type | Output |
|---|---|
| `int` | the decimal digits: `-7` |
| `double` | the shortest `%g`-style form: `3.5`, `0.333333`, `1e+08`. A NaN is always `nan` |
| `bool` | `1` or `0`, not `true` and `false`. To print the words, convert first: `printline(flag to string)` |
| `char` | the character |
| `string` | the text |

```turflang
fn main(): int {
    print(42);
    print(" ");
    print(3.5);
    print(" ");
    print(true);
    print(" ");
    print('x');
    printline("!");
    printline(1.0 / 3.0);
    printline(false);
    printline(true to string);
    return 0;
}
```

```text
42 3.5 1 x!
0.333333
0
true
```

Any other type has no overload: `printline(some_struct)` is E025 ("No matching
overload!"). Convert it to a `string` first. To print several values, build one
string with `+` and `to string`.

### Buffering: `flush(): void`

Standard output is buffered. `flush()` pushes out everything printed so far.
Call it before a prompt that waits for input, and before anything that can end
the program abruptly:

- The buffer is written out when `main` returns and when the program calls
  `sys::exit`.
- It is **not** written out when the program panics or calls `sys::abort`:
  text printed with `print` or `printline` that is still in the buffer can be
  lost. A panic message itself goes to standard error, which is not buffered.

### Standard error: `eprint(val: string): void`, `eprintline(val: string): void`

Like `print` and `printline` for a `string`, but to standard error, and
unbuffered: the text appears at once, in order with any panic message. Only
`string` is accepted; convert other values with `to string`.

```turflang
fn main(): int {
    eprint("warning: ");
    eprintline("using the default configuration");
    return 0;
}
```

### `input(): string`

Reads one line from standard input and returns it **without** its newline.

- At end of input (no more lines, or no input at all) it returns `""`, so an
  empty result means either an empty line or no input; it cannot tell you
  which.
- It reads at most 4095 characters. A longer line is returned in pieces: the
  first 4095 characters, then (after one character is dropped between them) the
  rest on the next call.
- It does not trim anything: a line from a file with Windows line endings keeps
  its `\r`.

```turflang
fn main(): int {
    print("Your name: ");
    flush();
    immut name: string = input();
    if name == "" {
        printline("no name given");
        return 1;
    }
    printline("Hello, " + name + "!");
    return 0;
}
```

### Files: `FileError`, `read_to_string`, `read_lines`, `write_string`, `exists`

`io` reads and writes **whole files**. There is no file handle: each call opens
the file, does its work and closes it before returning, so there is nothing to
`close()` and no `defer` is needed. For a file you read or write piece by piece,
use `extern fn` ([FFI and unsafe](../ffi_and_unsafe.md)).

```turflang
error FileError {
    NotFound(path: string),
    ReadFailed(path: string),
    WriteFailed(path: string),
}
```

Each variant carries the path it failed on. Read it with a pattern inside a
`compare` (`matches io::FileError::NotFound(path):`); `err == io::FileError::NotFound`
compares the variant alone; `err.message` is the message text, empty here
([Error handling](../error_handling.md)).

| Function | Returns | Behaviour |
|---|---|---|
| `read_to_string(path: string): !string` | the whole file as one `string` | `FileError::NotFound(path)` if the file cannot be opened for reading, whatever the reason (missing, no permission); `FileError::ReadFailed(path)` if fewer bytes were read than the file's size. |
| `read_lines(path: string): !List<string>` | the file split at `\n` | The same error cases as `read_to_string`, except that every failure is reported as `NotFound(path)`. A final newline does not produce a final empty element: `"a\nb\n"` is `["a", "b"]`. A blank line before the end is kept: `"a\n\n"` is `["a", ""]`. An empty file is an empty list. `\r` is kept: `"a\r\nb"` is `["a\r", "b"]`. |
| `write_string(path: string, content: string): !void` | nothing | Creates the file or **overwrites** it. `FileError::WriteFailed(path)` if it cannot be opened for writing (missing directory, no permission) or the write is short. |
| `exists(path: string): bool` | whether the path can be opened for reading | A question, so a `bool`. It is also `true` for a directory on Linux; use [`fs::is_dir`](fs.md) to tell the two apart. |

Files are read and written as bytes, with no newline translation on any
platform. A `string` ends at its first NUL byte, so a binary file that contains
NULs reads in full but its `length()` stops at the first one; `write_string`
likewise writes up to the first NUL.

```turflang
import list::List;

fn main(): int {
    io::write_string("notes.txt", "first\nsecond\n") recover as err run {
        compare err {
            matches io::FileError::WriteFailed(path):
            eprintline("cannot write " + path);
            done
            otherwise:
            eprintline("write failed");
        }
        return 1;
    };

    immut lines: List<string> = io::read_lines("notes.txt") recover run {
        eprintline("cannot read notes.txt");
        return 1;
    };
    mut i: int = 0;
    while i < lines.length() {
        printline((i + 1 to string) + ": " + lines.get(i));
        i = i + 1;
    }

    immut text: string = io::read_to_string("missing.txt") recover produce "(none)";
    printline(text);
    printline(io::exists("notes.txt"));
    return 0;
}
```

```text
1: first
2: second
(none)
1
```

---

## `sys`

Ending the program, and the primitive every library panic is built on.

### `exit(code: int): void`

Ends the program at once with status `code`. It never returns, and it does not
run pending `defer` statements. Standard output is flushed first.

### `abort(): void`

Ends the program at once as an abnormal termination. On Linux and macOS the
process dies with `SIGABRT`, which a shell reports as status 134; on Windows the
status is `0xE0545254`. Standard output is not flushed.

### `panic(msg: string): void`

Writes `msg` and a newline to standard error, then calls `abort`. It never
returns. Unlike a library panic, it adds no `file:line:` prefix: include your
own context in `msg`. Use it for a condition that no caller can recover from;
for one a caller might want to handle, return a `!T` instead.

### `STDERR_FD: int`

The constant `2`, the standard error descriptor, for code that calls a C
function such as `write` through `extern fn`.

```turflang
fn parse_port(text: string): int {
    return strings::parse_int(text) recover as err run {
        eprintline("bad port: " + text);
        exit(2);
        return 0;
    };
}

fn main(): int {
    immut port: int = parse_port("8080");
    if port < 1024 {
        panic("refusing to use a privileged port");
    }
    printline("port " + (port to string));
    return 0;
}
```

---

## `net`

TCP over IPv4, in two types: `TcpListener` (a server's listening socket) and
`TcpConnection` (one connected socket, whether the program dialed it or
accepted it; both ends read and write the same way). All calls block, and there
is no timeout.

An address is four `int` octets, not a `"127.0.0.1"` string:
`connect(127, 0, 0, 1, 8080)`. To dial a host name, resolve it first with
`resolve_hostname` and split the dotted text, or dial an address you already
know. A listener always binds every local interface.

```turflang
import net;

fn main(): int {
    immut ip: string = net::resolve_hostname("localhost") recover produce "127.0.0.1";
    printline(ip);
    return 0;
}
```

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

The variants carry no fields. Which call produces which:

| Call | Failure variants |
|---|---|
| `TcpConnection::connect` | `SocketCreationFailed`, `ConnectionRefused` (the connection could not be made for any reason: nothing listening, no route, unreachable) |
| `TcpListener::bind` | `SocketCreationFailed`, `BindFailed` (the port is in use, or needs privileges), `ListenFailed` |
| `TcpListener.accept` | `AcceptFailed` |
| `resolve_hostname` | `ResolutionFailed` |

`read`, `write` and `close` do not report failure: `read` returns `""` when the
peer has closed the connection or the read failed, and a `write` that fails is
silent.

### `resolve_hostname(host: string): !string`

Looks `host` up and returns its first IPv4 address as dotted text:
`resolve_hostname("localhost")` is `"127.0.0.1"`. A name that is already a
dotted address resolves to itself. `NetworkError::ResolutionFailed` if the name
does not resolve (a typo, no DNS record, no network) or is empty. IPv6 addresses
are not returned.

### `TcpConnection::connect(a: int, b: int, c: int, d: int, port: int) static: !TcpConnection`

Dials `a.b.c.d:port`. See the table above for the errors. Each octet is taken
modulo 256.

### `TcpConnection.read(max_bytes: int): string`

Waits until the peer sends something, then returns what arrived from **one**
receive: at most `max_bytes` characters, and often fewer, because a TCP message
can arrive in several pieces. It returns `""` if the peer closed the connection.
To read until a known end, call it in a loop. The text ends at the first NUL
byte, so `read` is for text protocols.

### `TcpConnection.write(data: string): void`

Sends `data` in one call. Like `read`, it moves bytes, not messages: the peer
may receive them in several reads.

### `TcpConnection.close(): void`

Closes the socket. It is **idempotent**: a second call does nothing, so
`defer conn.close();` and an explicit early close together are safe. (Closing
the same descriptor number twice is dangerous in general, because the system may
have given that number to a different file in between.)

### `TcpConnection.raw_fd(): int`

The underlying descriptor, for passing to FFI; `-1` once the connection is
closed.

### `TcpConnection::new(fd: int)`

Wraps a descriptor you already own, so that `read`, `write` and `close` work on
it. The connection takes responsibility for closing it.

### `TcpListener::bind(port: int) static: !TcpListener`

Listens on every local interface at `port`, with a backlog of 128. It sets
`SO_REUSEADDR` first, so restarting a server does not fail with "address in use"
while the previous socket lingers. Errors: see the table above.

### `TcpListener.accept(): !TcpConnection`

Waits for a client and returns the new connection. `AcceptFailed` is usually
transient, so a serve loop recovers with `continue`, as below.

### `TcpListener.close(): void`, `TcpListener.raw_fd(): int`, `TcpListener::new(fd: int)`

As for `TcpConnection`: `close` is idempotent, `raw_fd` is `-1` once closed, and
`new` wraps a descriptor you own.

### Constants

`AF_INET` (`2`), `SOCK_STREAM` (`1`), `SOL_SOCKET` (`1`) and `SO_REUSEADDR`
(`2`) are the Linux values of the C constants, for a program that makes its own
socket calls with `extern fn`. The first two are the same on every platform;
`SOL_SOCKET` and `SO_REUSEADDR` differ on Windows and macOS, where a program
calling `setsockopt` itself must use its platform's numbers. `net` itself does
not use these constants.

### Platform notes

The same Turf source works on Linux, macOS and Windows. The runtime handles the
differences:

- **Windows:** Winsock is started automatically on the first socket call and
  shut down when the program exits. A socket is closed with `closesocket`, which
  `close()` does for you. `raw_fd()` returns the Winsock `SOCKET` handle, which is
  **not** a C runtime file descriptor: pass it to Winsock calls, never to
  `read`, `write` or `close`.
- **macOS and BSD:** the address structure has a length byte that Linux's does
  not; the runtime builds it for the platform.
- **Everywhere:** only IPv4 and TCP. `resolve_hostname` returns the first IPv4
  address.

### Example: an echo server and its client

The server answers each connection with its message prefixed by `echo: `, and
stops when it receives `quit`. `defer conn.close();` closes each connection on
every path out of `serve`, and `defer listener.close();` closes the listener when
`main` returns.

```turflang
import net;

fn serve(conn: net::TcpConnection): bool {
    defer conn.close();
    immut request: string = conn.read(1024);
    if request == "quit" {
        conn.write("bye");
        return false;
    }
    if request.length() > 0 {
        conn.write("echo: " + request);
    }
    return true;
}

fn main(): int {
    mut listener: net::TcpListener = net::TcpListener::bind(9091) recover run {
        eprintline("cannot listen on port 9091");
        return 1;
    };
    defer listener.close();
    printline("listening on port 9091");
    flush();

    mut running: bool = true;
    while running {
        mut conn: net::TcpConnection = listener.accept() recover run {
            continue;
        };
        running = serve(conn);
    }
    return 0;
}
```

The client tells apart a server that is not running from a local failure:

```turflang
import net;

fn main(): int {
    mut conn: net::TcpConnection = net::TcpConnection::connect(127, 0, 0, 1, 9091) recover as err run {
        if err == net::NetworkError::ConnectionRefused {
            eprintline("nothing is listening on 127.0.0.1:9091");
        } else {
            eprintline("could not create a socket");
        }
        return 1;
    };
    defer conn.close();

    conn.write("hello");
    printline(conn.read(1024));
    return 0;
}
```

With the server running, the client prints `echo: hello`. Without it, the
client prints `nothing is listening on 127.0.0.1:9091` to standard error and
exits with status 1.

For an HTTP layer over `net`, see [http.md](http.md).
