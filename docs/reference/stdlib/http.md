# HTTP: `http`

A minimal HTTP/1.1 layer built directly on `net`'s raw TCP transport — no
raw sockets, `ptr`, or manual memory management is ever visible to a
caller of this module. Not prelude — needs an explicit `import`.

```turflang
import http;
import net;
```

Two ways to use it: **manual routing** (parse a request yourself, branch on
`req.path`, build a response by hand — the same shape
`showcase/http_server.tr` hand-writes directly against `net`) or
**callback routing** (register `fn(Request, Response): void` handlers per
path on a `Server`, then call `.listen(port)`). Both are fully supported;
pick whichever fits your program.

---

## `struct Request { method: string, path: string, headers: Map<string, string>, body: string }`

### `parse_request(conn: net::TcpConnection): Request`

Reads one request off `conn` (a single `conn.read(8192)` call) and parses
the request line + headers into a `Request`. Accepts both `\r\n\r\n`
(spec-correct) and a looser `\n\n` as the header/body separator, and trims
a trailing `\r` off every header line either way — both real-world line-
ending styles just work without the caller choosing one. A malformed or
empty request line yields a `Request` with an empty `method`/`path` rather
than failing outright (there's no natural way for a request-per-connection
accept loop to act on a `!Request` differently than just falling through
to a 404 anyway).

```turflang
mut req: http::Request = http::parse_request(conn);
```

### `Request.get_header(name: string): string` / `Request.has_header(name: string): bool`

Look up a header by name (byte-exact match). `get_header` panics
(`sys::panic`) if `name` isn't present — check `has_header` first, or use
it when you know the header is guaranteed present (like a required
`Content-Type`).

```turflang
if req.has_header("Content-Type") {
    io::printline(req.get_header("Content-Type"));
}
```

> **Why these exist as their own methods, not just `req.headers.get(name)`/
> `.contains(name)` inline at the call site.** `headers` is a real,
> directly-accessible `Map<string, string>` field, and since STD-003
> (`docs/development/changes/STD-003.md`) `Map<K, V>`'s own generic
> `.get()`/`.contains()` are content-correct for `K = string` too — see
> [`collections.md`](collections.md)'s `Map<K, V>` section. `get_header`/
> `has_header` are kept as thin, named wrappers over `self.headers.tryGet(name)`/
> `.contains(name)` purely for readability (`req.get_header(name)` reads
> better than `req.headers.get(name)`) and for `get_header`'s own more
> specific panic message (naming the missing header).

---

## `struct Response { conn: net::TcpConnection }`

### `Response.send_text(status: int, body: string): void`

Sends a complete `Content-Type: text/plain` HTTP/1.1 response over
`self.conn` in one `write()` call — status line, `Content-Type`,
`Content-Length`, `Connection: close`, blank line, body.

```turflang
mut res: http::Response;
res.conn = conn;
res.send_text(200, "hello from turf");
```

### `Response.send_json(status: int, body: string): void`

Identical to `send_text`, except `Content-Type: application/json`. `body`
is the raw JSON text — build it yourself (e.g. via `json::stringify`) or
hand-write it for a small, fixed shape.

```turflang
res.send_json(404, "{\"error\":\"not found\"}");
```

### `Response.send_raw(status: int, contentType: string, body: string): void`

The shared building block `send_text`/`send_json` are both one-line
wrappers over, for any other content type.

### Status text

A small, deliberately incomplete table (200/201/400/404/500 only — every
other code becomes `"<code> Unknown"`) covers exactly what this module's
own callers need. Not part of the public API surface (no separate
function exported for it); it's applied automatically inside `send_raw`.

---

## `struct Server { listener: net::TcpListener, routes: Map<string, fn(Request, Response): void> }`

A route table keyed by exact request path, storing a first-class function
handler per route. Route matching is **exact-path only** — no `:id`-style
parameter segments, no wildcards.

### `Server.get(path: string, handler: fn(Request, Response): void): void`

Registers `handler` to run for an exact-match request to `path`.
Re-registering the same path replaces the previous handler.

```turflang
fn home(req: http::Request, res: http::Response): void {
    res.send_text(200, "Welcome to Turflang!");
}

mut app: http::Server;
app.get("/", home);
```

### `Server.has_route(path: string): bool` / `Server.get_route(path: string): fn(Request, Response): void`

Look up the handler registered for `path`. `get_route` panics if `path`
isn't registered — check `has_route` first. Both are thin wrappers around
`self.routes.indexOf(path)` (see `Request.get_header`'s identical note,
above, for why these exist as named methods at all rather than inlining
`Map<K, V>`'s own accessors at every call site).

### `Server.listen(port: int): void`

Binds `port` and serves forever: accepts a connection, parses the
request, dispatches to the registered handler for `req.path` (or sends a
built-in 404 and moves on if no route matches), closes the connection,
repeats. A bind failure is fatal (prints an error and `sys::exit(1)`); a
single bad `accept()` is logged and skipped, not fatal.

```turflang
import http;

fn home(req: http::Request, res: http::Response): void {
    res.send_text(200, "Welcome to Turflang!");
}

fn main(): int {
    mut app: http::Server;
    app.get("/", home);
    app.listen(8080);
    return 0;
}
```

---

## Full example: manual routing

```turflang
import io;
import net;
import http;
import sys;

fn main(): int {
    mut listener: net::TcpListener = net::TcpListener::bind(8080) recover run {
        sys::exit(1);
        return 1;
    };
    while true {
        mut conn: net::TcpConnection = listener.accept() recover run { continue; };
        mut req: http::Request = http::parse_request(conn);
        mut res: http::Response;
        res.conn = conn;
        if req.path == "/" {
            res.send_text(200, "hello");
        } else {
            res.send_text(404, "not found");
        }
        conn.close();
    }
    return 0;
}
```

See `tests/34_http_and_json/03_http_manual_routing_roundtrip.tr` for a
full, real two-request round trip (status line, headers, and body all
asserted) over an actual `net::TcpConnection` pair, and
`04_http_callback_routing.tr` for the equivalent proof on the
callback-routing (`Server`) path.
