# RFC: Web Servers, JSON, and the Standard Library on C FFIs

This RFC outlines the architectural strategy for Turflang's future standard library, specifically focusing on how the newly stabilized FFI system will power high-level abstractions like web servers and JSON parsers.

The primary goal is to completely hide raw C primitives (`ptr`, `malloc`, `bind`, `recv`, etc.) behind memory-safe, ergonomic Turflang syntax.

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

During the development of the FFI real-world showcase (`tests/ffi_realworld.tr`), several language and parser limitations were identified. These document current edge cases that should be addressed in future compiler phases to improve developer ergonomics.

### The Identifier Lexing Bug (The "Two-Letter" Bug)
- **Issue:** Using standard C-style variable names like `fp`, `fd`, `wfile`, `t0`, `t1`, and `shell` causes the parser to fail when these variables are used in an expression position.
- **Root Cause:** A lexer/parser edge case where identifiers sharing prefixes with keywords (e.g., `fn`, `while`, `to`) confuse the token stream, resulting in errors like `Hmm, I don't know the word 'fp'. Hint: Did you mean: 'fn'?`.
- **Workaround:** Renamed variables to unambiguous names (`whandle`, `clk1`, `sockval`, etc.).

### Passing `null` to `ptr` Arguments
- **Issue:** Calling a C function with a null pointer (e.g., `time(null)`) is rejected by the TypeChecker.
- **Root Cause:** While `ptr` variables can be assigned `null`, the TypeChecker's function argument matching does not currently allow `null` to be passed implicitly as a `ptr` parameter.
- **Workaround:** Allocate a dummy pointer using `malloc`, pass it, and immediately `free` it.

### Implicit `string` to `ptr` Casting in Function Calls
- **Issue:** Passing a Turflang `string` literal to a C function expecting a `ptr` buffer (e.g., `fwrite(msg, ...)` where `msg` is a `string`) fails type checking.
- **Root Cause:** Even though `string` and `ptr` are identical at the LLVM IR level (opaque pointers), the TypeChecker's strict argument-matching loop for function calls does not implicitly cast a `string` argument to a `ptr` parameter.
- **Workaround:** Change the `extern fn` declaration in Turflang so the specific argument is explicitly typed as a `string` (e.g., `extern fn fwrite(buf: string, ...)`). The C linker accepts this perfectly.

### Local `extern fn` Declarations
- **Issue:** Declaring an `extern fn` locally inside a function block to limit scope fails with a syntax error.
- **Root Cause:** The parser only expects `extern fn` declarations at the top level of the file (handled by `hoistTopLevel` in the Resolver) and does not know how to parse them as statements inside a block.
- **Workaround:** Move all `extern fn` declarations to the global scope at the top of the file.

### `for` Loop Syntax Strictness
- **Issue:** Writing a busy-wait loop using `for i in 0..1000000 { ... }` causes a parser error.
- **Root Cause:** Turflang's `for` loop syntax strictly requires an explicit `step` clause and does not default to `step 1`.
- **Workaround:** Must explicitly declare the step (e.g., `step 1`) or use a `while` loop equivalent.
