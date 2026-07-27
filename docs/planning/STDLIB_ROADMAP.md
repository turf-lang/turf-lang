# Turflang Standard Library Roadmap

What's actually in `lib/std/` today, module by module, followed by what's
genuinely missing to make Turf usable for real programs rather than
language-feature demos. This is a planning/wishlist document — nothing
here is implemented yet unless explicitly marked as current baseline.

Every module in `lib/std/` follows the same convention (see any of them for
the pattern): raw C/POSIX functions bound via `extern fn` (never
`@intrinsic`, which no longer exists as a language feature at all), with
ordinary Turf functions/structs layered on top so calling code never sees
`void*`/FFI details. Any new module proposed below should follow the exact
same shape.

---

## 1. Current baseline (what actually exists right now)

| Module | Exports |
|---|---|
| `io` | `print`/`printline` (int, double, bool, string overloads), `input()` (one line from stdin) |
| `mem` | `alloc`, `alloc_zeroed`, `grow`, `dup`, `zero` (checked, fail-fast wrappers) + raw `malloc`/`calloc`/`realloc`/`free`/`memcpy`/`memmove`/`memset`/`memcmp` |
| `math` | `abs`, `power`, `min`, `max`, `clamp` (int/double overloads) + raw libm bindings (`sqrt`/`sin`/`cos`/`tan`/`floor`/`ceil`/`round`/`log`/`log2`/`log10`/`exp`/`pow`/`fabs`, all currently only reachable as the *wrapped* names above except where noted in §2.4) |
| `strings` | `length`, `contains`, `index_of`, `starts_with`, `ends_with`, `repeat`, `count_char`, `substring`, `replace`, `to_upper`, `to_lower`, `trim` |
| `list` / `stack` / `queue` | `List<T>` / `Stack<T>` / `Queue<T>` — push/pop/peek/length/enqueue/dequeue as applicable, dynamic-array-backed |
| `net` | *(added this session)* `TcpListener`/`TcpConnection` — `bind`/`accept`/`connect`/`read`/`write`/`close`. TCP only. |

That's the entire standard library. No `os`, `time`, `json`, `random`,
`path`/`fs`, `http`, or `map`/`set`/`dict` module exists at all.

---

## 2. Per-module gaps

### 2.1 `io` — bare minimum today

Only console I/O exists; there is no file I/O whatsoever.

**Missing, high-value:**
- File I/O: `open`/`read`/`write`/`append`/`close`, `read_to_string(path)` (whole file in one call — the single most commonly needed operation), `read_lines(path)`, `exists(path)`, `write_string(path, content)`.
  ```turflang
  import io;
  immut contents: string = io::read_to_string("config.txt");
  io::write_string("out.txt", "hello");
  ```
- `eprint`/`eprintline` — stderr output, currently impossible to distinguish from stdout at all.
- `flush()` — no way to force a flush of buffered stdout (relevant for interactive prompts, and for anything piping output to another process — the `dangerouslyDisableSandbox`/backgrounded-process testing done this session hit real stdio-buffering surprises that a `flush()` would have made controllable from Turf code itself).

**Missing, medium value:**
- Typed input: `input_int()`, `input_double()` — right now every `input()` result is a raw string that must be `to int`/`to double` cast manually, which will abort ungracefully on bad input rather than giving a clean re-prompt path.
- Formatted output beyond individual `printf`-style overloads — no `sprintf`-equivalent (build a string from mixed types without manual `+` concatenation of `to string` casts).

### 2.2 `strings` — solid basics, missing the connective tissue

**Missing, high priority — genuinely blocking, not speculative:** `split(s, delimiter): List<string>`. Discovered directly this session: `net.tr`'s client API had to take an IP address as 4 separate integer octets (`connect(127, 0, 0, 1, 8080)`) specifically *because* there was no way to parse `"127.0.0.1"` into parts. `split` is a small, self-contained addition (loop + `substring`, the same shape `replace`/`trim` already use) and it unblocks real string-processing programs immediately.
```turflang
mut parts: List<string> = strings::split("127.0.0.1", ".");
```

**Missing, high value:**
- `join(items: List<string>, sep: string): string` — the natural inverse of `split`.
- `trim_start(s)` / `trim_end(s)` — `trim` only does both ends at once.
- `pad_left(s, width, ch)` / `pad_right(s, width, ch)` — needed for any tabular/aligned console output.
- Numeric parsing/formatting beyond the bare `to int`/`to double` casts: `parse_int(s): int` / `parse_double(s): double` that can report failure explicitly (ties into whatever error-handling story lands — see §3, error unions) instead of the cast's current behavior on bad input.

**Missing, lower priority:**
- A string-builder type for efficient multi-piece concatenation. Every `+` on two strings today allocates a fresh heap buffer and leaks the operands (`EmitStringConcat`'s own `TURF-MEMLEAK` comment in `Codegen.cpp`) — fine for small numbers of concatenations, real cost in a loop building up a large string piece by piece. Worth flagging as a known cost, not neccessarily worth solving before the memory model itself changes (Phase 4, `docs/planning/MEMORY_ARCHITECTURE_RFC.md`).
- Char-level helpers beyond raw `s[i]` byte indexing (e.g. `is_digit(ch)`, `is_alpha(ch)`, `is_whitespace(ch)` — trivial wrappers, but currently every caller reimplements the ASCII-range check by hand, as `strings.tr`'s own `trim()` does inline for whitespace).

### 2.3 `mem` — reasonably complete for a low-level module

The checked wrappers (`alloc`/`alloc_zeroed`/`grow`/`dup`/`zero`) cover the common cases well. What's missing is higher-level and explicitly **not urgent** — it's Phase-4-memory-model-adjacent (`docs/planning/MEMORY_ARCHITECTURE_RFC.md`), not a gap in today's manual-memory story:
- Arena/pool allocators (bulk-allocate, free all at once on scope exit) — most valuable *after* there's a language-level hook (scope-exit/`defer`) to drive the "free all at once" trigger automatically.
- Reference counting helpers — redundant work if/when ARC lands as a language feature per the RFC; don't build a parallel stdlib-level ref-counting scheme in the meantime.

### 2.4 `math` — missing constants, blocked on a real language gap

Every libm binding needed (`sin`/`cos`/`tan`/`sqrt`/`floor`/`ceil`/`round`/`log`/`exp`/etc.) is already declared via `extern from "m" fn` — genuinely nothing to add there, they're just not yet re-exposed as ordinary Turf-named wrappers the way `abs`/`power` are (a small, mechanical addition: `fn sqrt(x: double): double { return sqrt(x); }`-shaped one-liners for each, since the raw `extern fn` names are currently only reachable if the caller somehow already knows they exist and imports them directly, which defeats the point of a clean module surface).

**Blocked on a language gap, not a stdlib gap:** `PI`/`E` constants. Turf has **no way to declare a top-level module constant at all** — `immut NAME: int = value;` outside a function is a parse error (confirmed directly this session, hit while writing `net.tr`). Every module that wants a named constant today must either inline the literal at every use site (what `net.tr` does for `AF_INET`/`SOCK_STREAM`/etc., with a comment explaining why) or expose a zero-argument getter function (`fn PI(): double { return 3.14159265358979; }`, called as `math::PI()`) as the only current workaround. Worth fixing at the language level (see `docs/planning/CODE_REVIEW_GAPS_2026-07.md`) before `math` grows many constants, since a function-call spelling for a mathematical constant is a permanent, mildly awkward wart otherwise.

### 2.5 `list` / `stack` / `queue` — the missing type is a map

**Missing, high value — nothing like this exists at all:** a `map`/dictionary/hash-table generic type (`Map<K, V>`). This is arguably the single highest-value addition across the entire stdlib — almost every non-trivial real program needs key-value lookup somewhere (routing tables, caches, counting/grouping, config), and today there is no way to build one without hand-rolling parallel `List<K>`/`List<V>` arrays and linear-scanning them.
```turflang
struct Map<K, V> {
    keys: List<K>,
    vals: List<V>
    // or a real hash-table layout once one exists; linear-scan parallel
    // lists is a legitimate, honest FIRST implementation given List<T>
    // already exists and works
}
fn<K, V> Map.get(key: K): V { ... }
fn<K, V> Map.set(key: K, val: V): void { ... }
fn<K, V> Map.contains(key: K): bool { ... }
```

**Missing, medium value:**
- A `Set<T>` type — same underlying need (fast membership test) as `Map`, could plausibly share implementation once `Map` exists.
- Sort (`List<T>.sort()` or a free `sort(list, comparator)`) and search (`binary_search`, or even just a linear `find`/`index_of` mirroring `strings::index_of`'s shape) helpers — currently entirely absent; any program needing sorted output must hand-roll a sort.
- Iteration: checked whether the language has any `for x in someList` form beyond numeric ranges (`for i in 0..10 step i++`) — it does not currently; iterating a `List<T>` today means a manual `while i < l.length() { ... l.pop()-or-index ... i = i + 1; }` loop. This is really a language-level gap (needs some notion of an iterator protocol or at minimum indexable-range sugar for collections), not purely a stdlib one, but it directly limits how usable `list`/`stack`/`queue` feel in practice.

**Missing, lower priority, overlapping with language work already tracked elsewhere:**
- An `Optional<T>`/`Result<T, E>`-style generic wrapper type. Meaningfully overlaps with the still-unimplemented `!T` error-union proposal (`docs/planning/ERROR_HANDLING_RFC.md`) — worth waiting for that decision rather than building a parallel ad-hoc convention now that would likely need throwing away.

### 2.6 `net` — TCP-only, blocking-only, IPv4-only, no DNS, no TLS

Everything here is a natural "next" rather than an urgent gap — the module is genuinely useful as-is for anything that fits synchronous, single-connection-at-a-time TCP:

- **DNS resolution.** `TcpConnection::connect()` only takes 4 raw integer octets, no hostname lookup (`getaddrinfo`/`gethostbyname`) — can't currently connect to anything by name, only by literal IP. High value: almost every real client program wants to connect by hostname.
- **UDP.** `net.tr` is TCP-only (`SOCK_STREAM`); no `UdpSocket`-equivalent exists.
- **Non-blocking / concurrent connections.** Every call in `net.tr` blocks the calling thread; `TcpListener.accept()` can only ever serve one client at a time (loop-and-handle-sequentially, as `examples`/`showcase` server code does today). No `select`/`poll`/`epoll` wrapper, no threading primitives in the language at all to build a thread-per-connection model even if the socket layer supported it.
- **TLS/HTTPS.** No encryption at any layer — a real "connect to the internet" story needs this eventually (likely via `extern fn` bindings to a system TLS library, OpenSSL or the platform equivalent), but it's a large, security-sensitive undertaking, not a small addition.
- **A real HTTP layer on top.** `net.tr` gives raw bytes in and out; there is no request/response parsing, no routing, no headers-as-a-map. This was literally the original vision in `docs/planning/STDLIB_AND_WEB_RFC.md` (`http.tr` wrapping sockets the same way `net.tr` now does) — with real TCP now working, building `http.tr` on top of `net.tr` (parse the raw request line + headers into a struct, provide a `Response.send_text()`/`.send_json()`-shaped API, maybe basic path routing) is a genuinely achievable near-term win, not speculative.

### 2.7 Modules that don't exist at all yet

- **`os`** — environment variables (`getenv`/`setenv`), command-line arguments (`argc`/`argv`), process exit codes beyond `main()`'s own return value, process spawning (`fork`/`exec`/`system`-equivalent). Already flagged as unwritten in `docs/planning/ROADMAP_v1.0.0.md`. High value: no Turf program can currently read its own CLI arguments or environment at all.
- **`time`** — current time (wall clock and/or monotonic), `sleep(seconds)`, duration arithmetic, formatting/parsing timestamps. Doesn't exist in any form right now — even a trivial "how long did this take" measurement is currently impossible from Turf code.
- **`json`** — parse/serialize. Proposed in `docs/planning/STDLIB_AND_WEB_RFC.md` (originally sketched as a thin wrapper over `cJSON` via FFI) but never built. Pairs naturally with a future `http.tr` for any real API client/server work.
- **`path` / `fs`** — path manipulation (join, basename, extension, absolute/relative) and directory operations (list directory, create/remove directory, stat a path), distinct from raw file *content* I/O (§2.1). Also entirely absent today.
- **`random`** — no random-number generation exists in any form (not even a raw `extern fn rand()` binding anywhere in the stdlib). Needed for anything from simple simulations to cryptographically-irrelevant test-data generation (note: *not* suitable for security-sensitive randomness without a dedicated CSPRNG binding, a separate, later concern).
- **`testing`** — there's a bare `assert()`/`fail()` intrinsic already (language-level, not stdlib), but no structured test-declaration/test-runner concept. `docs/planning/ROADMAP_v1.0.0.md` Phase 6 already earmarks a compiler-level `turf test` command (`test "name" { ... }` blocks, hidden generated `main()`) — that's a *compiler* feature, not a stdlib module, but it's the same underlying need; don't build a competing stdlib-only test convention if the compiler-level one is still on the table.

---

## 3. Priority tiering

**Blocking / near-term (small, concrete, already-identified gaps with real programs depending on them):**
1. `strings::split` — directly blocked real code this session (`net.tr`'s IP handling).
2. `Map<K, V>` — the single most obviously-missing data structure; unblocks a huge fraction of "real" programs.
3. `io` file I/O (`read_to_string` especially) — no file access at all today is a hard ceiling on usefulness.
4. `os` (env vars + argv) — no Turf program can currently be configured or invoked with arguments.
5. Top-level module constants (language-level fix, not stdlib) — currently forces every stdlib module needing a named constant into an awkward workaround; unblocks `math::PI`/`math::E` and cleans up `net.tr`'s existing inline-literal workaround.

**High-value, natural next steps (larger but well-understood, no open design questions):**
- `http.tr` built on the now-working `net.tr`.
- `strings::join`, `pad_left`/`pad_right`, `trim_start`/`trim_end`.
- `time` (even just current-time + sleep).
- DNS resolution for `net`.
- `math`'s libm functions re-exposed as ordinary wrapped names (mechanical, just needs doing).

**Valuable but larger / has open design questions:**
- `Set<T>`, sort/search helpers, some iteration story for collections (language-level question: does Turf ever get a `for x in collection` form, or does everything stay index-loop-based?).
- `json` (natural, but depends on deciding the FFI-vs-hand-rolled-parser question `STDLIB_AND_WEB_RFC.md` already raises).
- `Optional<T>`/`Result<T, E>` — deliberately deferred pending the `!T` error-union decision (`ERROR_HANDLING_RFC.md`) so the stdlib doesn't ship a convention that gets thrown away.

**Large undertakings, not near-term:**
- TLS/HTTPS.
- Non-blocking/concurrent networking (needs a concurrency story in the language first — none exists today).
- Arena allocators / ref-counting helpers in `mem` (Phase-4-memory-model-adjacent; wait for that RFC's direction).
- `turf test` (compiler-level, tracked separately in `docs/planning/ROADMAP_v1.0.0.md` Phase 6).

---

## 4. RFC Coverage Plan

What it would concretely take to deliver on each of the three standing RFCs (`STDLIB_AND_WEB_RFC.md`, `ERROR_HANDLING_RFC.md`, `MEMORY_ARCHITECTURE_RFC.md`), given what's real today vs. what's still blocked on a language feature that doesn't exist yet. Planning only — nothing in this section is implemented.

### 4.1 `STDLIB_AND_WEB_RFC.md` — buildable NOW, in layers

The RFC's own example code (`app.get("/", fn(req, res): void {...})`, callback-based routing) needs **first-class functions as values**, which don't exist yet (`ROADMAP_v1.0.0.md` Issue #187, still open) — so the *literal* syntax sketched in that RFC isn't reachable yet. But the underlying capability — a real HTTP layer — doesn't need to wait for that. A realistic build order, each layer genuinely shippable on its own:

1. **`http.tr`, request/response structs + manual if/elseif routing** (buildable today, on top of the now-working `net.tr`):
   ```turflang
   struct Request { method: string, path: string, headers: string, body: string }
   struct Response { conn: net::TcpConnection }

   fn parse_request(conn: net::TcpConnection): Request { /* split conn.read() on \n\n into headers/body, first line into method/path */ }
   fn Response.send_text(status: int, body: string): void { /* build + conn.write() the raw HTTP response, exactly like showcase/http_server.tr does inline today */ }
   fn Response.send_json(status: int, body: string): void { /* same, Content-Type: application/json */ }
   ```
   This is a mechanical extraction of exactly what `showcase/http_server.tr` already hand-writes inline — the showcase file IS the reference implementation for what `http.tr` needs to wrap. Depends on `strings::split` (§3, already tier-1) to cleanly parse headers into something queryable, rather than the showcase's simpler `starts_with`-only routing.
2. **Callback-based routing** (`app.get(path, handler)`, matching the RFC's actual example) — blocked on first-class functions (Issue #187). Until then, `http.tr` route dispatch has to stay a manual `if request.path == "/" { ... } elseif ...` chain inside the caller's own loop, same shape `showcase/http_server.tr` uses — a real ergonomic loss vs. the RFC's vision, but not a blocker to shipping layer 1.
3. **`json.tr`** — the RFC itself proposes wrapping `cJSON` via FFI rather than hand-rolling a parser (`extern from "cjson" fn cJSON_Parse(...): ptr;` etc., exactly the `extern from "libname" fn` mechanism `math.tr`'s `extern from "m" fn sqrt(...)` already proves out). Concretely: confirm `libcjson` (or equivalent) is available/vendorable in the build environment first — this session's `llvm-c` FFI proof-of-concept (`showcase/llvm_ffi_selfhost_demo.tr`) is the template for "confirm the .so exports what you need via `nm -D`, then bind it with `extern from`." A hand-rolled pure-Turf JSON parser is the fallback if no C library is available, but is meaningfully more work and duplicates a solved problem.

### 4.2 `ERROR_HANDLING_RFC.md` — entirely blocked on the `!T` language decision

Nothing stdlib-buildable here yet. The RFC's `!T` error unions, `catch`, `?`, and `error.XYZ` are a **parser/type-system feature** (`ROADMAP_v1.0.0.md` Issue #188, still open, explicitly called out there as "last -- heaviest"), not a stdlib module — there's no FFI angle to build ahead of that decision. The one stdlib-adjacent risk already flagged in §2.5 above: don't build an ad-hoc `Optional<T>`/`Result<T, E>` generic struct in `list`/`map` land as a stopgap, since it would almost certainly need to be thrown away or awkwardly reconciled once `!T` actually lands. Wait for the language decision first.

### 4.3 `MEMORY_ARCHITECTURE_RFC.md` — the FFI-facing pieces are stdlib-buildable now; the safety pieces are not

The RFC bundles two genuinely separate things:
- **FFI interop primitives** (`unsafe_ptr<T>`, an explicit "get the raw pointer out of a safe type" escape hatch) — these are really just naming/convention work once `unsafe {}` blocks exist (Issue not yet filed as its own roadmap line item beyond the RFC itself; `ROADMAP_v1.0.0.md` Phase 4 lists `unsafe {}` as its own still-open item). Not stdlib work per se — `mem.tr` already exposes the raw allocator surface this needs; the RFC's ask is a compiler-level *restriction* (raw pointer types illegal outside `unsafe {}`), not a new capability.
- **ARC / safe references (`&T`) / heap-managed strings and arrays** — a memory-MODEL change (how every existing type is represented and who frees it), not something `mem.tr` (or any stdlib module) can deliver by itself; this is compiler + Codegen work (automatic retain/release insertion at every assignment/scope-exit), fundamentally different in kind from every other item in this document. `mem.tr`'s current `Pool<T>`/`free_pool()` (this session's addition, see §2.3) is the honest, purely-stdlib-level stopgap available in the meantime — bulk-free tracking, not automatic lifetime management — and is explicitly NOT a step toward ARC, just a smaller, immediately-useful convenience that doesn't require any language change to deliver.

Bottom line: of the three RFCs, only `STDLIB_AND_WEB_RFC.md` has a real "start building stdlib code today" path. The other two are gated on specific, already-tracked language decisions (`!T` error unions; ARC/safe-references) rather than stdlib engineering effort.
