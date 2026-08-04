# RFC: Web Servers, JSON, and the Standard Library on C FFIs

This RFC outlines the architectural strategy for Turflang's future standard library, specifically focusing on how the newly stabilized FFI system will power high-level abstractions like web servers and JSON parsers.

The primary goal is to completely hide raw C primitives (`ptr`, `malloc`, `bind`, `recv`, etc.) behind memory-safe, ergonomic Turflang syntax.

> **2026-07-27: confirmed live, twice.** First cut: a real, working HTTP server (persistent, multi-request, basic path routing, real `socket`/`bind`/`listen`/`accept`/`recv`/`send` calls) written with raw FFI directly in the example file. The `\r`-string-escape lexer gap hit while building it is now fixed (was: `\n`/`\t`/`\\`/`\"` only, `\r` silently became a literal `r` character; see git history for the old writeup, previously in the now-removed `docs/planning/CODE_REVIEW_GAPS_2026-07.md`).
>
> Second cut, and the one that actually matters for this RFC's premise: the raw-FFI/`sockaddr_in`-byte-packing part described below is no longer something every caller has to do — it's now wrapped in a real stdlib module, `lib/std/net.tr` (`TcpListener`/`TcpConnection`, `bind`/`accept`/`connect`/`read`/`write`/`close`). `showcase/http_server.tr` and `showcase/http_client.tr` (a full Turf-to-Turf HTTP round trip, both ends verified live) use ONLY `import net;` — no `extern fn`, no `ptr`, no manual memory management anywhere in either showcase file. This is exactly the "hide raw C primitives behind memory-safe, ergonomic Turflang syntax" goal stated at the top of this RFC, just delivered as a TCP-level module first rather than jumping straight to the `http`/`json` abstraction sketched in Section 1 below — a natural next layer to build now that the transport underneath is solid. See `docs/planning/STDLIB_ROADMAP.md` for the tracked next steps (an `http.tr` request/response/routing layer on top of `net.tr`, UDP, TLS, DNS resolution, etc).

---

## 1. The Developer Experience (What the user writes)

A user building a web backend in Turflang should not interact with C code. They will import the `http` and `json` modules and write code that looks like a modern, high-level language:

```turflang
import http;
import json;
import io;

fn main(): int {
    // 1. Create a new server
    mutable app: http.Server = http.Server();

    // 2. Define a GET route
    app.get("/", fn(req: http.Request, res: http.Response): void {
        res.send("Welcome to Turflang!");
    });

    // 3. Define a POST route with JSON parsing
    app.post("/api/users", fn(req: http.Request, res: http.Response): void {
        // Parse the raw string body into a JSON object
        mutable payload: json.Value = json.parse(req.body);
        
        // Extract fields safely
        mutable username: string = payload.get_string("username");
        mutable age: int = payload.get_int("age");

        io.printline("New user: " + username);

        // Build a JSON response
        mutable response_data: json.Object = json.Object();
        response_data.set("status", "success");
        response_data.set("id", 101);

        res.json(201, response_data);
    });

    // 4. Start listening
    io.printline("Server listening on port 8080...");
    app.listen(8080);
    
    return 0;
}
```

Notice what is **missing** here:
- No `@intrinsic` compiler magic.
- No `malloc` or `free`.
- No `ptr` handling or manual casts.
- No `sockaddr_in` or `bind` syscalls.
- No manual HTTP string parsing.

---

## 2. Under the Hood: How the Abstractions Work

How does Turflang hide the raw C code? By wrapping opaque pointers (`ptr`) inside Turflang `struct`s and attaching methods to them (requiring Phase 4 Unsafe Blocks and Phase 5 Magic Methods/Struct Methods).

### HTTP Server Abstraction

Deep in `lib/std/http.tr`, the standard library developer writes the POSIX C interop code.

```turflang
// lib/std/http.tr

// 1. Raw C Syscalls (Hidden from the end-user)
extern fn socket(domain: int, type: int, proto: int): int;
extern fn bind(fd: int, addr: ptr, len: int): int;
extern fn listen(fd: int, backlog: int): int;
extern fn accept(fd: int, addr: ptr, len: ptr): int;
extern fn recv(fd: int, buf: ptr, len: int, flags: int): int;
extern fn send(fd: int, buf: ptr, len: int, flags: int): int;
extern fn close(fd: int): int;

// 2. Native Turflang Structs
struct Request {
    method: string,
    path: string,
    body: string
    // headers: map<string, string> (Phase 4 Generics)
}

struct Response {
    client_fd: int
}

// 3. Methods attached to the structs
fn (res: Response) send(content: string): void {
    // Manually format the HTTP response string
    mutable http_msg: string = "HTTP/1.1 200 OK\r\nContent-Length: " + 
                               to_string(strlen(content)) + "\r\n\r\n" + 
                               content;
    
    // Call raw C send() using the stored client socket descriptor
    send(res.client_fd, http_msg, strlen(http_msg), 0);
    close(res.client_fd);
}

struct Server {
    server_fd: int
    // routes map goes here
}

fn (s: Server) listen(port: int): void {
    s.server_fd = socket(2, 1, 0); // AF_INET, SOCK_STREAM
    
    // Phase 4 unsafe block: Build the 16-byte sockaddr_in struct in memory
    unsafe {
        mutable addr: ptr = mem.alloc(16);
        mem.set(addr, 0, 16);
        // ... byte layout for AF_INET (2) and port ...
        bind(s.server_fd, addr, 16);
        mem.free(addr);
    }
    
    listen(s.server_fd, 10);
    
    // Accept loop
    while true {
        mutable client_fd: int = accept(s.server_fd, null, null);
        
        // Read raw HTTP request
        mutable buf: ptr = mem.alloc(4096);
        recv(client_fd, buf, 4096, 0);
        
        // Parse the raw string into a Request struct
        mutable req: Request = parse_http_request(buf);
        mutable res: Response = Response { client_fd: client_fd };
        
        // Route it (pseudo-code)
        // execute_handler(req, res);
        
        mem.free(buf);
    }
}
```

### JSON Parsing Abstraction

Writing a fast JSON parser from scratch in a new language is hard. Instead, Turflang can use its library annotation FFI to link against a battle-tested C library like `cJSON`, completely hiding the C library behind a clean Turflang API.

```turflang
// lib/std/json.tr

// 1. Link against the external C library automatically!
extern from "cjson" fn cJSON_Parse(value: string): ptr;
extern from "cjson" fn cJSON_GetObjectItemCaseSensitive(object: ptr, string: string): ptr;
extern from "cjson" fn cJSON_GetStringValue(item: ptr): string;
extern from "cjson" fn cJSON_Delete(item: ptr): void;

// 2. The Turflang Wrapper Struct
// Holds the raw C pointer safely inside a native struct
struct Value {
    raw_node: ptr
}

// 3. The Turflang API
fn parse(json_string: string): Value {
    // Call C parsing
    mutable raw: ptr = cJSON_Parse(json_string);
    if raw == null {
        // Handle error...
    }
    return Value { raw_node: raw };
}

fn (v: Value) get_string(key: string): string {
    // Walk the C tree safely
    mutable item: ptr = cJSON_GetObjectItemCaseSensitive(v.raw_node, key);
    if item != null {
        return cJSON_GetStringValue(item);
    }
    return ""; // or raise an Error
}

// Memory Management:
// When the 'Value' struct goes out of scope, a future memory management 
// phase (like Destructors or Defer) will automatically call cJSON_Delete(v.raw_node).
```

---

## 3. Summary

The power of the C FFI is that **Turflang doesn't have to reinvent the wheel**. 
- You want an HTTP server? Wrap POSIX sockets.
- You want JSON? Wrap `cJSON`.
- You want a database driver? Wrap `libpq` (PostgreSQL) or `libsqlite3`.
- You want 2D graphics? Wrap `SDL2` or `Raylib`.

By building clean Turflang `struct`s and attaching methods to them, you trap all the ugly pointers, manual memory management, and `unsafe` blocks inside the standard library files (`lib/std/*.tr`). 

The end-user writing Turflang only ever sees safe, native types, garbage-collected (or RAII) memory, and highly readable syntax.

---

## 4. Known Compiler Limitations Encountered (Phase 3)

> **Re-verified 2026-07-27** against the current compiler (`tests/ffi_realworld.tr` itself no longer exists in the tree, but every limitation below was independently re-tested with a fresh scratch `.tr` file). Two of the five are now fixed; three are still real. Note also that sections 1-3 above use syntax from BEFORE the "Grand Syntactic Overhaul" (`mutable`-only variables, paren-based `fn (res: Response) send(...)` method syntax) — the current language uses `mut`/`immut` and `fn StructName.method(...)` / `fn StructName::method(...)` (see README.md and docs/architecture/). The *concept* here (http/json modules wrapping raw FFI behind clean structs) is still unimplemented and still the right target; only the illustrative code samples are stale.

### ~~The Identifier Lexing Bug (The "Two-Letter" Bug)~~ — FIXED
- Re-tested: `mut fp: int = 5; mut fd: int = 10; mut t0: int = 1; mut t1: int = 2;` used in expressions compiles and runs correctly now. No special-casing needed.

### ~~Passing `null` to `ptr` Arguments~~ — FIXED
- Re-tested: `extern fn time(t: ptr): int; ... time(null);` compiles cleanly now — `null` is accepted directly as a `ptr` argument.

### Implicit `string` to `ptr` Casting in Function Calls — still open
- Re-tested: `extern fn puts(s: ptr): int; ... puts("hello");` still fails: `No overload of 'puts' matches argument types (string).` The documented workaround (declare the FFI parameter as `string` instead of `ptr`) still works and is still necessary.

### Local `extern fn` Declarations — still open
- Re-tested: declaring `extern fn abs(x: int): int;` inside a function body still fails with `Expected ';' after statement.` — the parser still only accepts `extern fn` at the top level. Workaround unchanged: declare all `extern fn`s at file scope.

### `for` Loop Syntax Strictness — still open
- Re-tested: `for i in 0..1000000 { ... }` (no `step` clause) still fails: `Expected 'step' after the range end value.` Workaround unchanged: `for i in 0..1000000 step i++` explicitly, or use `while`.
