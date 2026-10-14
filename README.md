# Turf

Turf (TurfLang) is a statically typed language that compiles ahead of time to native executables through LLVM. It has structs, enums, real generics, state machines, pattern matching, error unions, a garbage-collected heap with checked references, a standard library (strings, collections, files, JSON, HTTP, sockets), a project and package tool, and an optional diagnostics assistant that runs a language model on your own machine.

**Version 2.0.** The exact number is in the [`VERSION`](VERSION) file (`2.0.0-dev` on an untagged checkout); a release is tagged `v2.0.0`.

Coming from 1.x? Read [`docs/MIGRATING_TO_2.md`](docs/MIGRATING_TO_2.md) first.

## Contents

1. [Platforms](#platforms)
2. [Install](#install)
3. [Quick start](#quick-start)
4. [Language tour](#language-tour)
5. [The standard library](#the-standard-library)
6. [`--smart`: diagnostics with a local model](#--smart-diagnostics-with-a-local-model)
7. [Building from source](#building-from-source)
8. [Where to go next](#where-to-go-next)

## Platforms

| Platform | Tier | Notes |
|---|---|---|
| Linux x86-64 | 1 | needs glibc 2.34 or newer (Ubuntu 22.04, Debian 12, RHEL 9, Amazon Linux 2023 and later) |
| Windows x86-64 | 1 | native; no MSYS2 or other toolchain to install |
| macOS arm64 (Apple Silicon) | 1 | Intel Macs are not supported |

Tier 1 means the platform is tested on every change and a release is blocked on it. Each release is self-contained on all three: the archive carries the compiler, its LLVM, the linker and the runtime, so nothing else needs installing to compile and run programs. The programs it builds need no Turf files at run time.

Every release has two archives per platform: the plain one, and a `-smart` one, which is the same compiler with the runtime for [`--smart`](#--smart-diagnostics-with-a-local-model) added. Choose plain unless you want `--smart`.

`turf` can compile for another machine's architecture (`--target`), but it writes the object file and stops: linking for another platform is not supported in 2.0.

## Install

**Linux and macOS:**

```sh
curl -fsSL https://raw.githubusercontent.com/turf-lang/turf-lang/master/scripts/install/install.sh | sh
```

**Windows (PowerShell):**

```powershell
irm https://raw.githubusercontent.com/turf-lang/turf-lang/master/scripts/install/install.ps1 | iex
```

The installer finds the archive for your machine, checks its SHA-256 (and installs nothing if the check fails or cannot run), unpacks it into `~/.turf` (Windows: `%LOCALAPPDATA%\turf`) and adds the `bin` folder to your `PATH` with one marked block, so `turf` works in any **new** terminal. Both scripts are also attached to every release.

Options, `install.sh` / `install.ps1`: `--version v2.0.0` / `-Version` (a release other than the latest), `--prefix DIR` / `-Prefix` (another folder), `--smart` / `-Smart` (the `-smart` build), `--no-modify-path` / `-NoModifyPath` (leave start-up files alone), `--uninstall` / `-Uninstall`.

A piped `irm | iex` cannot take options; use the environment variables `TURF_INSTALL_VERSION`, `TURF_INSTALL_DIR` and `TURF_INSTALL_SMART=1`, or save the script and run it with the options above. Uninstalling removes exactly what the installer added and never your projects, the package cache or a model you put in `models/`.

**By hand:** download the archive from the [Releases page](../../releases), unpack it anywhere, and run `bin/turf` or put `bin/` on your `PATH`. It is one folder: `bin/` (`turf`, `turfc`), `lib/` (runtime and standard library), `docs/`, `share/turf/examples/`. No `TURF_HOME` is needed: the compiler finds `lib/` relative to its own location, through symlinks too. (`TURF_HOME` is only for binaries copied out of that folder.)

**From source:** see [Building from source](#building-from-source).

Check the install with `turf version`, which prints the versions of `turf` and of the compiler `turfc` behind it. Paste it into a bug report.

## Quick start

`turf new` creates a project, `turf run` builds and runs it:

```text
$ turf new hello
Created program package 'hello' in hello/
  hello/turf.toml
  hello/src/main.tr
  hello/README.md
  hello/.vscode/extensions.json
  hello/.gitignore
$ cd hello
$ turf build
Compiled to build/hello.o
Successfully compiled to build/hello
$ turf run
Hello, TurfLang! This is hello.
```

The project is a `turf.toml` manifest and `src/main.tr`:

```toml
[package]
name = "hello"
version = "0.1.0"
turf = "2.0"

[dependencies]
```

```turflang
// src/main.tr
import io;

fn main(): int {
    io::printline("Hello, TurfLang! This is hello.");
    return 0;
}
```

`turf test` runs every file in the project's `tests/` folder as a program; a test passes when it exits 0. Put this in `tests/basic.tr`:

```turflang
import test;

fn main(): int {
    test::assert(1 + 1 == 2, "arithmetic still works");
    return 0;
}
```

```text
$ turf test
Running Turf Test Suites...

[tests] basic.tr ... PASS

=== Test Summary ===
Total Tests: 1
Passed:      1
Failed:      0

All tests passed successfully!
```

Other commands you will use: `turf check` (report every diagnostic, write nothing), `turf fmt`, `turf explain E081` (what an error code means, with an example), `turf add` / `turf remove` / `turf update` (dependencies, by `path` or `git` tag), `turf clean`, and `turf help <command>`. `turf` exits 0 on success, 1 when it reports a problem, and 2 when the command line itself is wrong.

A single file needs no project: `turf run hello.tr`, or `turf hello.tr -o hello` and then `./hello`.

[`examples/`](examples/README.md) holds a library and a program that depends on it, ready to run. [`showcase/`](showcase/) has larger programs: an HTTP server and client talking over a real socket, a key-value store, a text-analysis tool. The project layout, the manifest, dependencies and the lock file are in [`docs/reference/projects_and_packages.md`](docs/reference/projects_and_packages.md). For walkthroughs of working on a project, writing a library and testing it, see the [guides](docs/guides/README.md).

## Language tour

Every snippet below is a complete program, compiled and run before it went into this page. They use the prelude (`printline`, `math`, `strings` and the primitive-type helpers need no `import`); see [The standard library](#the-standard-library).

### Variables, `guess` and expressions

A declaration is `mut` (can be reassigned) or `immut`. The type is written, or left out and inferred from the initializer; `: guess` is the explicit spelling of the same thing. An inferred type is fixed at the declaration. Conversions are explicit, with `to`; `give ... otherwise` is the conditional expression.

```turflang
fn main(): int {
    mut count = 0;                       // inferred int; same as `count: guess = 0`
    immut ratio: double = 7 to double / 2.0;
    immut label = "turf";                // inferred string

    for i in 1..6 step i += 1 {
        if i % 2 == 0 { continue; }
        count = count + i;
    }

    mut size: string = count > 5 give "big" otherwise "small";
    printline(label + ": " + size + " " + (count to string));
    printline(ratio to string);
    return 0;
}
```

```text
turf: big 9
3.5
```

Integer arithmetic is checked: signed overflow and division by zero panic at run time rather than wrapping, and a literal that does not fit its type is a compile error. Loops are `while`, `do { } while (cond);` and the range `for`. Details: [Variables and types](docs/reference/variables_and_types.md), [Operators](docs/reference/operators.md), [Control flow](docs/reference/control_flow.md).

### Patterns with `compare`

`compare` takes a value, an enum, or a struct taken apart by a pattern. Arms are tried in order and the first match wins. A name inside a destructuring pattern binds the field, `..` ignores the rest, `if` adds a guard, and `_` matches anything. Each arm ends in `done`; `otherwise` is the arm of last resort. For an enum, leaving a variant out is a compile error (E093) unless `otherwise` is there.

```turflang
struct Point { x: int, y: int }

enum Color { Red, Green, Blue }

fn describe(p: Point): string {
    compare p {
        matches Point { x: 0, y: 0 }:
            return "origin";
        done
        matches Point { x, y } if x == y:
            return "diagonal at " + (x to string);
        done
        matches Point { x: 0, .. }:
            return "on the y axis";
        done
        otherwise:
            return "somewhere else";
    }
}

fn name(c: Color): string {
    compare c {
        matches Color::Red: return "red"; done
        matches Color::Green: return "green"; done
        matches Color::Blue: return "blue"; done
    }
}

fn main(): int {
    mut p: Point = Point::default();
    printline(describe(p));
    p.x = 3;
    p.y = 3;
    printline(describe(p));
    p.y = 4;
    printline(describe(p));
    printline(name(Color::Green));
    return 0;
}
```

```text
origin
diagonal at 3
somewhere else
green
```

A struct is built by a constructor you write (`fn Point.new(...)`, called as `Point::new(...)`) or zeroed by `Point::default()`; methods are declared `fn Type.method(...)` with an implicit `self`. See [Structs and enums](docs/reference/structs_and_enums.md).

### Errors, `recover` and `try`

A function that can fail returns `!T`. The result cannot be used as a plain `T` until you deal with it: `recover produce v` supplies a fallback, `recover run { ... }` runs a block, and `recover as err run { ... }` binds the error. An `error` variant can carry data, which a `compare` reads back with a pattern. `try` hands a failure to your own caller unchanged.

```turflang
error ParseError {
    Empty,
    BadDigit(text: string)
}

fn parse(text: string): !int {
    if text == "" {
        return ParseError::Empty;
    }
    for i in 0..text.length() step i += 1 {
        if !text[i].is_digit() {
            return ParseError::BadDigit(text);
        }
    }
    return text to int;
}

fn total(a: string, b: string): !int {
    immut x: int = try parse(a);     // on failure, returns parse's error unchanged
    immut y: int = try parse(b);
    return x + y;
}

fn totalOrZero(a: string, b: string): int {
    return total(a, b) recover as err run {
        compare err {
            matches ParseError::BadDigit(t):
                printline("not a number: " + t);
            done
            otherwise:
                printline("empty input");
        }
        return 0;
    };
}

fn main(): int {
    printline(totalOrZero("20", "22") to string);
    printline(totalOrZero("20", "4x") to string);
    printline(totalOrZero("", "1") to string);
    return 0;
}
```

```text
42
not a number: 4x
0
empty input
0
```

`try` is not allowed in `main` (it returns `int`, not `!V`); handle the error there with `recover`. [Error handling](docs/reference/error_handling.md) has the rest.

### `defer`

A `defer`red statement runs when the enclosing block is left, on every path out of it, last deferred first. It is how you release a file or a socket, which the garbage collector does not do for you. A panic does not run deferred statements.

```turflang
struct Log { text: string }

fn work(log: &Log): void {
    defer log.text = log.text + " cleanup";   // runs when work returns
    log.text = log.text + " working";
}

fn main(): int {
    mut log: Log = Log::default();
    work(&log);
    printline(log.text);
    return 0;
}
```

```text
 working cleanup
```

### References, `&T` and `&?T`, and `heap`

`&T` is a reference that is never null. `&?T` is a reference that may be `null`, and the compiler only lets you read through it where it has proved it is not: an unchecked access is error E081. That makes a linked list or a tree possible in safe code. `heap(value)` copies a value onto the garbage-collected heap and gives you a `&T` to it, so the list can grow while the program runs and is freed when nothing refers to it. The compiler also rejects a reference that would outlive the stack variable it points to.

```turflang
struct Node {
    val: int,
    next: &?Node       // a node, or nothing
}

fn push(head: &?Node, v: int): &Node {
    mut n: Node = Node::default();
    n.val = v;
    n.next = head;
    return heap(n);    // copy to the GC heap, get a &Node
}

fn sum(head: &?Node): int {
    mut total = 0;
    mut cur: &?Node = head;
    while cur != null {
        total = total + cur.val;   // legal: the check proves cur is not null
        cur = cur.next;
    }
    return total;
}

fn main(): int {
    mut list: &?Node = null;
    for i in 1..5 step i += 1 {
        list = push(list, i);
    }
    printline(sum(list) to string);
    return 0;
}
```

```text
10
```

[Memory and safety](docs/reference/memory_and_safety.md) explains the two heaps and exactly what the language guarantees outside `unsafe`.

### Generics

Structs, functions and methods take type parameters, written `<T>` after `fn` for functions and methods. Each instantiation is compiled to code specialised for its types, so the standard collections are ordinary generic types.

```turflang
import list::List;
import map::Map;

struct Box<T> {
    val: T
}

fn<T> Box.new(v: T): void {
    self.val = v;
}

fn<T> Box.get(): T {
    return self.val;
}

fn<T> largest(items: List<T>, less: fn(T, T): bool): T {
    mut best: T = items.get(0);
    for i in 1..items.length() step i += 1 {
        if less(best, items.get(i)) {
            best = items.get(i);
        }
    }
    return best;
}

fn lessInt(a: int, b: int): bool { return a < b; }

fn main(): int {
    mut b: Box<string> = Box::new("generic");
    printline(b.get());

    mut nums: List<int> = List::default();
    nums.push(3);
    nums.push(9);
    nums.push(4);
    printline(largest(nums, lessInt) to string);

    mut ages: Map<string, int> = Map::default();
    ages.put("ada", 36);
    printline(ages.get("ada") to string);
    return 0;
}
```

```text
generic
9
36
```

Functions are values (`fn(int): int`), and `operator` overloads work on your own types. See [Generics and operators](docs/reference/generics_and_operators.md).

### State machines

`machine` declares states and the transitions allowed between them. A state is a value (`Door::Open`), and `.transition(State)` moves a variable along an edge. Where the compiler can prove the edge exists it costs nothing; otherwise it is checked when the program runs, and an edge the graph does not declare panics.

```turflang
machine Door {
    Closed -> Open,
    Open -> Closed,
    Closed -> Locked,
    Locked -> Closed
}

fn main(): int {
    mut door: Door = Door::Closed;
    door.transition(Door::Open);
    door.transition(Door::Closed);
    door.transition(Door::Locked);
    printline(door == Door::Locked give "locked" otherwise "not locked");
    return 0;
}
```

```text
locked
```

Writing `door.transition(Door::Open)` next, from `Locked`, panics: `Door::transition(Open) -- no such transition from the current state.` See [State machines](docs/reference/state_machines.md).

### Modules and `pub`

Every file is a module. A declaration is private to its module unless it is marked `pub` (`public` is a synonym); that applies to functions, methods, types, `error`s, `machine`s, `extern fn`s and struct fields, and a field is private even in a `pub struct` unless it is marked too. Another module reaches a name with `::` after `import`. Packages (a `turf.toml` plus `src/`) work the same way: a library's root module is the only thing another package can import, and what it makes `pub` is its API.

```turflang
// src/geometry.tr: the root module of a library package
pub struct Point {
    pub x: int,
    pub y: int
}

pub fn Point.new(x: int, y: int): void {
    self.x = x;
    self.y = y;
}

pub fn manhattan(a: Point, b: Point): int {
    return (a.x - b.x).abs() + (a.y - b.y).abs();
}
```

A program package then writes `import geometry;` and `geometry::manhattan(...)`. Upgrading a 1.x module is mostly this keyword; `turf migrate pub` adds it for you (see the [migration guide](docs/MIGRATING_TO_2.md)). More in [Functions and modules](docs/reference/functions_and_modules.md).

### Calling C: `extern`, `unsafe` and raw pointers

`extern fn` declares a C function, and `extern from "m" fn` links the library it lives in. Raw pointers (`ptr`, `T*`) exist for this boundary and are allowed only inside an `unsafe { }` block. Everything outside it keeps the guarantees in [Memory and safety](docs/reference/memory_and_safety.md).

```turflang
extern fn strlen(s: string): int;
extern from "m" fn sqrt(x: double): double;
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;

fn main(): int {
    unsafe {
        mut buf: ptr = malloc(64);
        free(buf);
    }
    printline(strlen("verify") to string);
    printline(sqrt(2.0) to string);
    return 0;
}
```

```text
6
1.41421
```

An ordinary function may not take or return a raw pointer, even inside `unsafe { }` (error E130): use `&T`, or `&?T` when it may be absent. A file that needs raw pointers throughout, such as a wrapper around a C API, can begin with `unsafe module;`, which lifts the per-use gate for that file. Outside the install folder the compiler honours it only when you pass `--allow-unsafe-modules`, so a plain `turf file.tr` never grants it silently. See [FFI and `unsafe`](docs/reference/ffi_and_unsafe.md).

## The standard library

The standard library is ordinary Turf source under `lib/std/`. The prelude (nine modules, no `import` needed) is `int`, `double`, `bool`, `char`, `strings`, `array`, `io`, `math` and `sys`; the rest are imported by name.

| Module | For |
|---|---|
| `strings`, `array`, `math` | string search, slicing, splitting and parsing; `int[]` helpers; `abs`, `min`, `clamp`, libm functions |
| `io`, `fs`, `os` | printing and input, reading and writing files; paths and directories; environment and arguments |
| `list`, `stack`, `queue`, `map`, `set` | `List<T>`, `Stack<T>`, `Queue<T>`, `Map<K, V>`, `Set<T>` |
| `json`, `http`, `net` | a JSON parser and writer; an HTTP/1.1 server and request/response types; TCP sockets and DNS |
| `time`, `random`, `mem` | clocks and sleeping; pseudo-random numbers (not cryptographic); manual memory and `Pool` |
| `sys`, `test` | `panic`, `exit`; `assert`, `assert_eq`, `fail` |

Names are `snake_case`; an operation that can fail returns `!T` and never a sentinel. The conventions, the full list and every function are in [`docs/reference/stdlib/`](docs/reference/stdlib/README.md).

## `--smart`: diagnostics with a local model

Turf's diagnostics are the compiler's own and need nothing else. With `--smart`, an error that the compiler cannot fix by itself is passed to a small language model running locally through `llama.cpp`; no network is involved, and nothing runs on a clean compile. A suggestion is never trusted: it is applied to an isolated copy of your program and shown only if it removes the error without introducing another. Otherwise you see the compiler's diagnostic, unchanged. An error with a single unambiguous fix, such as a misspelled variable, is fixed without the model at all.

```sh
turf my_program.tr --smart -o my_program
```

`--smart` needs two things: the **smart build** (the `-smart` archive, `install.sh --smart` / `install.ps1 -Smart`, or a source build without `--no-smart`), and **one model** that you choose: put exactly one `.gguf` file in the `models/` folder of your install (`~/.turf/models`, `%LOCALAPPDATA%\turf\models`, or `models/` in a checkout). No model is shipped or downloaded. [`qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`](https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF) (about 1 GB on disk, about 1.5 GB of RAM while it works) is what Turf is developed with. With the plain build, no model, or more than one, `--smart` says so in one line and shows the compiler's diagnostics. The details are in [CLI and diagnostics](docs/reference/cli_and_diagnostics.md#--smart).

## Building from source

You need LLVM 22, CMake 3.20 or newer and a C++20 compiler (Clang recommended); Go 1.26 or newer builds the `turf` front end (without it you get `build/turfc` alone). `--smart`'s runtime is built from the vendored `llama.cpp` submodule.

```sh
git clone --recursive https://github.com/turf-lang/turf-lang
cd turf-lang
./scripts/cmake_build.sh --release              # compiler, turf, and the --smart runtime
./scripts/cmake_build.sh --release --no-smart   # same, without llama.cpp and its slow first build
./scripts/install/dev_install.sh                # link turf into ~/.local/bin (--prefix DIR for elsewhere)
```

The binaries are `build/turf` and `build/turfc`. A `--no-smart` build is the same compiler; only `--smart` is missing. Running from a checkout needs no `TURF_HOME`. A source build links your programs with the system `cc`, which the self-contained releases do not need. Contributor setup, the test suites and conventions are in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Where to go next

- [`docs/reference/`](docs/reference/README.md): the language reference, one topic per page, with the standard library in [`docs/reference/stdlib/`](docs/reference/stdlib/README.md).
- [`docs/MIGRATING_TO_2.md`](docs/MIGRATING_TO_2.md): what changed from 1.x and what to do about it.
- [`docs/architecture/`](docs/architecture/): how the compiler and runtime work, for contributors.
- [`CONTRIBUTING.md`](CONTRIBUTING.md): building, testing and sending changes.
- `turf explain <code>`: every error code has a written explanation.
- [`LICENSE`](LICENSE): Apache License 2.0.
