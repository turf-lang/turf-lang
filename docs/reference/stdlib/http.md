# HTTP: `http`

A small HTTP/1.1 layer over [`net`](io_and_net.md#net): parse a request,
write a response, and an optional `Server` that dispatches requests to handler
functions by path. Not prelude: it needs `import http;`, and a program that
accepts connections itself also needs `import net;`.

```turflang
import http;
```

It is meant for small services, tools and tests, not for the open internet:

- **One request per connection.** Every response carries `Connection: close`;
  there is no keep-alive.
- **One read per request.** `parse_request` reads up to 8192 bytes in a single
  receive. A request line and headers beyond that, or a body that arrives in a
  later packet, are not seen. The body is whatever arrived with the headers;
  `Content-Length` is not consulted.
- **One request at a time.** `Server.listen` serves a connection to the end
  before it accepts the next.
- **No TLS**, no chunked encoding, no query-string or form parsing.

There are two ways to use it, and both use the same `Request` and `Response`:

- **Manual routing**: you run the accept loop, call `parse_request`, and
  branch on `req.path` yourself.
- **A `Server`**: register a handler per path with `get`, then call `listen`.

---

## `Request`

```text
struct Request {
    pub method: string,
    pub path: string,
    pub headers: Map<string, string>,
    pub body: string,
}
```

What the handler reads. `method` is the request line's first word (`"GET"`),
and `path` is its second, **exactly as sent**, query string included:
`/search?q=a` is the path `"/search?q=a"`. `headers` maps each header name to its
value, with surrounding spaces trimmed; names are case-sensitive, so
`Content-Type` and `content-type` are different keys. A header sent twice keeps
its last value. `body` is whatever followed the blank line.

A `Request` you create yourself starts with empty strings and no headers:
`http::Request::default()`.

### `parse_request(conn: net::TcpConnection): Request`

Reads one request from `conn` with a single `conn.read(8192)` and parses it. The
header block ends at the first blank line, either `\r\n\r\n` (what clients send)
or `\n\n`; a trailing `\r` is removed from every line either way. It never
fails: an empty or malformed request line gives a `Request` whose `method` and
`path` are `""`, which falls through to your not-found branch.

### `Request.has_header(name: string): bool`

Whether the request has a header called exactly `name`.

### `Request.get_header(name: string): string`

The header's value. It **panics** if the header is not there, with a message that
starts with your call's `file:line:` and names the header. Ask with `has_header`
first unless you know the header is required.

To walk all the headers, use the map: `req.headers.keys()` returns a
`List<string>` of the names ([collections.md](collections.md#mapk-v)).

---

## `Response`

```text
struct Response {
    pub conn: net::TcpConnection,
}
```

A response writes to its connection. Create one with
`http::Response::default()` and set `conn`; a `Server` does this for you and
hands the handler a ready `Response`. Each `send_*` call writes a complete
response and nothing else, so call exactly one per request. The connection is
not closed by `send_*`; that is the caller's `defer conn.close();`.

### `Response.send_text(status: int, body: string): void`

Sends `status` with `Content-Type: text/plain`.

### `Response.send_json(status: int, body: string): void`

Sends `status` with `Content-Type: application/json`. `body` is JSON text you
have already made, for example with [`json::stringify`](json.md#stringifyv-jsonvalue-string).

### `Response.send_raw(status: int, content_type: string, body: string): void`

The function the other two call: any content type. The response is, with `\r\n`
line endings:

```text
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 15
Connection: close

Welcome to Turf
```

`Content-Length` is the length of `body` in bytes.

### `status_text(status: int): string`

The status line text for a code: `"200 OK"`, `"201 Created"`,
`"400 Bad Request"`, `"404 Not Found"`, `"500 Internal Server Error"`. Any other
code gives its number and the word `Unknown` (`status_text(418)` is
`"418 Unknown"`), so a response with any other status is still well formed, just
without a reason phrase.

---

## `Server`

```text
struct Server { /* private fields */ }
```

A table from exact path to handler, and the loop that serves it. Create one with
`http::Server::default()`; a declaration without a value
(`mut app: http::Server;`) is E008. The fields are private; `get`, `has_route`,
`get_route` and `listen` are the interface.

A handler is any function of type `fn(http::Request, http::Response): void`.
Matching is **exact on the path** and ignores the method: `/users` does not match
`/users/7` or `/users?page=2`, and a `POST /` goes to the same handler as
`GET /`. A handler that needs the method reads `req.method`.

### `Server.get(path: string, handler: fn(Request, Response): void): void`

Registers `handler` for `path`. Registering the same path again replaces the
handler.

### `Server.has_route(path: string): bool`

Whether a handler is registered for exactly `path`.

### `Server.get_route(path: string): fn(Request, Response): void`

The handler registered for `path`. It **panics** if there is none, with a message
that starts with your call's `file:line:` and names the path. Use `has_route`
first.

### `Server.listen(port: int): void`

Binds `port` on every interface and serves forever. For each connection it
parses the request and calls the handler for `req.path`; if no handler matches
it answers `404` with the text `Not Found`. It closes the connection when the
handler returns.

- If the port cannot be bound, it prints
  `http::Server.listen: could not bind port 8080.` to standard output and ends the
  program with status 1.
- A failed `accept` is skipped and the server keeps serving.
- It does not return. There is no way to stop it except a handler calling
  `exit`, or ending the process from outside. For a server with its own
  shutdown, use manual routing.

---

## Example: a `Server` with a JSON handler

```turflang
import http;
import json;

fn home(req: http::Request, res: http::Response): void {
    res.send_text(200, "Welcome to Turf");
}

fn greet(req: http::Request, res: http::Response): void {
    mut reply: json::JsonValue = json::JsonValue::new_object();
    reply.set("greeting", json::JsonValue::from_string("hello"));
    reply.set("method", json::JsonValue::from_string(req.method));
    if req.has_header("User-Agent") {
        reply.set("agent", json::JsonValue::from_string(req.get_header("User-Agent")));
    }
    res.send_json(200, json::stringify(reply));
}

fn main(): int {
    mut app: http::Server = http::Server::default();
    app.get("/", home);
    app.get("/greet", greet);
    app.listen(8080);
    return 0;
}
```

A request `GET /greet` with a `User-Agent: curl/8` header is answered with
status 200, `Content-Type: application/json` and the body
`{"greeting":"hello","method":"GET","agent":"curl/8"}`. `GET /missing` gets
`404` and `Not Found`.

## Example: manual routing with your own accept loop

Manual routing is the choice when the server needs its own rules: a shutdown
request, per-method handling, or a different not-found response. This one
answers three requests and stops. `defer conn.close();` runs at the end of
each pass of the loop, on every path.

```turflang
import http;
import net;

fn main(): int {
    mut listener: net::TcpListener = net::TcpListener::bind(8080) recover run {
        eprintline("cannot listen on port 8080");
        return 1;
    };
    defer listener.close();

    mut served: int = 0;
    while served < 3 {
        mut conn: net::TcpConnection = listener.accept() recover run {
            continue;
        };
        defer conn.close();
        served = served + 1;

        mut req: http::Request = http::parse_request(conn);
        mut res: http::Response = http::Response::default();
        res.conn = conn;

        if req.method == "GET" && req.path == "/" {
            res.send_text(200, "hello");
        } elseif req.method == "POST" && req.path == "/echo" {
            res.send_text(200, req.body);
        } elseif req.path == "/teapot" {
            res.send_raw(418, "text/plain", "short and stout");
        } else {
            res.send_text(404, "not found");
        }
    }
    return 0;
}
```

The `/teapot` response starts `HTTP/1.1 418 Unknown`, because 418 is not in the
status table.

## Escaping text you put in a response

A handler that builds JSON by hand must escape any text it did not write itself,
or a quote in a user's name breaks the document. `json::escape_string` does it
([json.md](json.md#escape_strings-string-string)):

```turflang
import http;
import json;

fn hello(req: http::Request, res: http::Response): void {
    res.send_json(200, "{\"path\":\"" + json::escape_string(req.path) + "\"}");
}

fn main(): int {
    mut app: http::Server = http::Server::default();
    app.get("/hello", hello);
    return 0;
}
```
