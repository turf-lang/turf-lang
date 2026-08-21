# Turf

Turf is a statically-typed, LLVM-backed programming language. It compiles directly to native binaries, supports structs, enums, generics, modules, pointers, and native FFI, and ships with an integrated compiler-diagnostics system built around a locally-run language model.

Current version: **pre-1.0**, actively developed (past `v0.8.0`).

## Table of Contents

1. [Why Turf](#why-turf)
2. [Installation](#installation)
3. [Quick Start](#quick-start)
4. [Language Tour](#language-tour)
5. [Examples](#examples)
6. [Building from Source](#building-from-source)
7. [Roadmap](#roadmap)

## Why Turf

Turf's compiler pipeline is a standard deterministic front-to-back: lexer → parser → resolver → type checker → codegen, LLVM backend. Nothing about the compilation itself is probabilistic. What Turf adds on top is a diagnostics layer that treats the compiler's own internal state as the
source of truth for AI-assisted suggestions, rather than pattern-matching against formatted error strings — and every part of that pipeline exists specifically to close off the ways this class of feature usually goes wrong.

### The diagnostic pipeline, precisely

When a diagnostic fires with `--smart` enabled, this is what actually happens, in order:

1. **The error itself carries structural context, not just a message.**
  Every diagnostic class that has something meaningful to point to (a variable's declaration site on a type mismatch, every candidate overload on an arity mismatch, the array's declaration on an out-of-bounds access, the prior declaration on a duplicate) attaches a `RelatedLocation` — `{SourceLocation, Description}` — at the exact point in the Resolver/TypeChecker/Codegen where that information is already known, because real symbol-table resolution already happened there. This is threaded through `TurfError`/`TurfWarning` via a chainable `.withRelated(loc, "declared here")`, into `DiagnosticEngine::add()`, and stored on the diagnostic itself. Nothing is recovered later by regex over an error string or by re-scanning source text for a keyword — the data is real from the moment the compiler pass produces it.

2. **Context is built from that data, not from guessing.**
  The old approach (used by an earlier iteration of this feature) scanned raw source lines for patterns like `fn name(` or a declaration keyword to guess where something relevant might be. That approach is gone. Context assembly now just reads the `RelatedLocation` list off the diagnostic and pins those exact lines into the model's prompt, labeled with why each one matters. The size of the surrounding sliding window (how many lines before/after the error line itself) is sized per error category — tighter for self-contained errors like a syntax mistake, wider for errors like a type mismatch where the relevant declaration may be far away in the file.

3. **Category classification is resolved deterministically first.**
  A rule-based classifier (not the model) assigns each error to a category before any model call happens. Only when that fails to produce a specific category does the category get folded into the same model call that produces the fix — via a `category` field in the same JSON response, rather than spending a second, separate model invocation on classification alone.

4. **The model's output is grammar-constrained at the token level.**
  A GBNF grammar is loaded into the sampler chain via `llama.cpp`'s grammar-masking sampler, fixing the JSON schema exactly: six required fields in a fixed order, `fix_mode` restricted to the literal tokens `MODIFY` or `NONE`, the `category` field restricted to the exact set of category names the compiler understands. This isn't a validation step run after the fact — it's enforced during sampling, so it is not merely unlikely for the model to emit a malformed response, it is not sampleable in the first place.
  The sampler chain also applies a repetition penalty ahead of greedy decoding, specifically to counter the tendency of small models to loop under purely deterministic sampling.

5. **Confidence is computed, never asked for.**
  The model is never prompted to self-report a probability — small models anchor to whatever example values appear nearby in the prompt and echo them back regardless of actual certainty, making a self-reported number meaningless. Instead, confidence is computed after parsing from measurable properties of the actual response: whether the category was resolved deterministically, whether `fix_mode` is `MODIFY`, whether a concrete single-line replacement was actually produced, whether that replacement is clean single-line syntax rather than something structurally suspicious, and whether it's actually on-topic for the symbol involved.

6. **The suggested fix is never trusted — it's tested.**
  Before anything is shown to you, the fix is applied to an isolated snapshot of the compiler state — a real sandboxed copy of the source, symbol table, and AST, not the live one — and speculatively re-parsed and re-validated. Only a fix that demonstrably resolves the original error, without introducing new ones, is surfaced. Anything else is silently discarded and you see the compiler's own original diagnostic, unmodified.

### `--smart`

Runs the pipeline above on error only — no cost, no model inference, nothing happens on a clean compile — entirely locally via a small quantized model loaded through `llama.cpp`. No network call, no external dependency.

```bash
turf my_program.tr --smart -o my_program
```

Useful compiler introspection flags, beyond ordinary compilation: `--dump-ast`, `--dump-resolved`, `--dump-typechecked`, `--dump-modules` (everything the compiler knows about the current program's modules — every imported module's functions, struct/enum type ownership, and every on-demand import), `--emit-llvm`.

## Installation

No package-manager distribution yet. Build from source (see [Building from Source](#building-from-source)), or use a pre-built binary from the [Releases page](../../releases) if one exists for your platform — CI currently builds for Linux (x86_64, Arch x86_64), macOS (Apple Silicon
and Intel), and Windows (x86_64). Release archives bundle `lib/` alongside the binary, so extracting one and running the binary from inside the extracted folder works with no further setup.

### Setting up `TURF_HOME`

The compiler resolves every `import` (its own standard library under `lib/std/`, and — once a package format exists — third-party packages under `lib/usr/`) against the `TURF_HOME` environment variable: it should point at the Turf installation root, i.e. the folder that directly contains `lib/`. Running the binary from right inside an extracted release archive, or a source checkout you've just built, needs no setup at all — with `TURF_HOME` unset, the compiler falls back to looking for `lib/` next to its own binary. You only need to set `TURF_HOME` once you move the binary somewhere else: a system-wide install, a symlink on `PATH`, a shortcut, etc.

**Linux / macOS** (bash or zsh) — add one line to your shell profile (`~/.bashrc`, `~/.zshrc`, or `~/.profile`, whichever your shell reads) so it's set in every new terminal:

```bash
export TURF_HOME="/path/to/turf-installation"   # e.g. the extracted release folder, or this repo's root
```

Then either open a new terminal, or run `source ~/.zshrc` (or your equivalent file) to pick it up in the current one.

**Windows — PowerShell** — set it once, persistently, for your user account (a new PowerShell/cmd window is required for it to take effect):

```powershell
setx TURF_HOME "C:\path\to\turf-installation"
```

Or, for the current session only (lost when the window closes):

```powershell
$env:TURF_HOME = "C:\path\to\turf-installation"
```

**Windows — Command Prompt (cmd.exe)** — same idea:

```bat
setx TURF_HOME "C:\path\to\turf-installation"
:: or, current session only:
set TURF_HOME=C:\path\to\turf-installation
```

**Windows — GUI, if you'd rather not use a terminal**: Start Menu → search "Environment Variables" → *Edit environment variables for your account* → *New...* under "User variables" → Variable name `TURF_HOME`, Variable value the path to your Turf installation → OK. Log out/in (or just open a fresh terminal) for it to apply.

Once set, verify it from a fresh terminal: `cd` somewhere completely unrelated to your Turf installation (e.g. your home directory) and compile the `hello.tr` from [Quick Start](#quick-start) below. If `TURF_HOME` is wired up correctly, `import io;` resolves regardless of your current directory or where the `turf` binary itself lives.

## Quick Start

```
// hello.tr
import io;

fn main(): int {
    io::printline("Hello, Turf!");
    return 0;
}
```

```bash
turf hello.tr -o hello
./hello
```

I/O lives in the standard library's `io` module. `::` is Turf's namespace-resolution operator — `io::printline`, not a bare global and not `io.printline` (`.` is reserved for instance-member access; see [Modules & Imports](#modules--imports)).

## Language Tour

### Variable declarations

Every declaration is explicitly typed and explicitly `mut` or `immut`:

```
mut x: int = 10;
immut pi: double = 3.14159;
mut flag: bool = true;
mut name: string = "Turf";
```

### Casting

Explicit conversions use the `to` operator:

```
mut a: double = 42 to double;
mut b: int = 3.75 to int;      // truncates: 3
mut c: int = "100" to int;     // parses the string
mut s: string = 42 to string;
```

### Control flow

Block form:

```
if x > 0 {
    io::printline("positive");
} elseif x < 0 {
    io::printline("negative");
} else {
    io::printline("zero");
}
```

`compare` is Turf's switch statement — each `matches` arm ends with `done`, and an optional `otherwise` arm must come last:

```
compare x {
    matches 1:
        io::printline("one");
    done
    matches 2:
        io::printline("two");
    done
    otherwise:
        io::printline("something else");
}
```

Ternary (expression) form uses `give` / `otherwise`:

```
mut max: int = a > b give a otherwise b;
```

Loops:

```
while i < 10 {
    i = i + 1;
    if i % 2 == 0 { continue; }
}

for j in 1..10 step j += 1 {
    if j % 2 == 0 { continue; }
    sum = sum + j;
}

do {
    i = i - 1;
} while (i > 0);   // body always runs at least once
```

Bitwise operators: `&`, `|`, `~`, `^` (XOR), `<<`, `>>`.

```
mut flags: int = 0b1010 & 0b1100;
mut shifted: int = 1 << 4;
```

### Structs

Fields are `name: Type`, comma-separated. There is no brace-literal construction syntax — every struct is built via exactly one of two reserved, compiler-recognized calls:

```
struct User {
    age: int,
    name: string
}

fn User.new(age: int, name: string): void {
    self.age = age;
    self.name = name;
}

mut u: User = User::new(10, "Alice");
u.age = 25;

mut u2: User = User::default();   // every field recursively zero-initialized
```

`new` and `default` are reserved struct-method names, not ordinary functions:

- **`StructName::new(args...)`** calls a user-defined constructor — declared exactly like any other instance method (`.`, `self` implicit), just named `new`. It must assign every field of the struct on every path before returning (checked with a real control-flow-sensitive analysis, not just "assigned somewhere in the body") — a field missed on even one branch is a compile error. A struct with no `.new` declared can't be constructed with `::new(...)` at all.
- **`StructName::default()`** is always compiler-synthesized — it recursively zero-initializes every field (including nested structs) and takes no arguments. It can never be declared by hand (`fn StructName.default(...)` is a compile error), and it's always available, even for a struct with no methods at all.
- Neither can be called via `.` on an instance (`x.new(...)`/`x.default()`) — only `StructName::new(...)`/`StructName::default()`.

Other instance methods are declared the same way as `new`, called via `.`, with `self` implicit — not a declared parameter, but available inside the method body:

```
fn User.grow(): void {
    self.age = self.age + 1;
}

fn User.rename(new_name: string): void {
    self.name = new_name;
}

u.grow();
u.rename("Bob");
```

Static methods use the exact same `.` declaration form, distinguished only by a trailing `static` keyword (no implicit `self` at all) — but are *called* on the type itself via `::`, never through an instance:

```
fn User.describe(prefix: string) static: string {
    return prefix + " user";
}

io::printline(User::describe("a"));
```

`::` never appears in a `fn` declaration — it's reserved for calling/referencing through a namespace (enum variants, module-qualified functions, static methods, and `new`/`default`), while `.` is used for every method *declaration*, static or not.

A struct with no fields at all (`struct Empty {}`) is fully supported — the natural shape for a "namespace" struct that only ever holds static methods. `Empty::default()` works even with zero fields to initialize.

### Arrays

Fixed-size, typed, and index directly — including arrays of structs, with field access chaining through the index:

```
mut users: User[5] = [
    User::default(),
    User::default(),
    User::default(),
    User::default(),
    User::default()
];

users[0] = User::new(25, "Alice");
assert(users[0].age == 25, "");
```

### Enums

`::` addresses a variant, the same operator used for every other namespace-qualified reference in the language:

```
enum Color { Red, Green, Blue }

mut c: Color = Color::Green;
if c == Color::Green {
    io::printline("green");
}
```

`Color::COUNT`, `Color::VALUES`, and `Color::NAMES` are built in for every enum — variant count, the array of underlying values, and the array of variant name strings, respectively.

### Error handling

Recoverable errors are named `error` types — bare-name variants only, no payload fields:

```
error MathError {
    DivisionByZero
}

fn divide(a: int, b: int): !int {
    if b == 0 {
        return MathError::DivisionByZero;
    }
    return a / b;
}
```

A function returning `!T` can't be used as a plain `T` at the call site — it must be handled with `recover`, in one of three forms:

```
mut x: int = divide(10, 0) recover produce 0;   // fallback value on failure

fn tryDivide(a: int, b: int): int {
    return divide(a, b) recover as err run {     // bind the specific error variant
        if err == MathError::DivisionByZero {
            io::printline("division by zero");
        }
        return -1;
    };
}
```

`recover run`/`recover as err run` blocks must produce the success type on every path (usually via `return`) — the one exception is a `!void` success type, where the block can just run side effects and fall through:

```
extern fn writeFile(path: string, contents: string): !void;

writeFile("a.txt", "hi") recover run {
    io::printline("write failed");
};
```

`recover as err run { ... }` requires the callee to declare a real `error` type — a bare `error GeneralError;` type (no variants beyond the implicit `GeneralError::DEFAULT`) can still use `recover produce`/`recover run`, just not `recover as`. See `docs/architecture/errors_and_recover.md` for the full spec.

### First-class functions

An unqualified `fn` name is a value — it can be passed as an argument, stored in a variable, and called indirectly:

```
fn apply(f: fn(int): int, x: int): int {
    return f(x);
}

fn double_it(n: int): int { return n * 2; }

mut result: int = apply(double_it, 5); // 10
```

Only bare, unqualified function names are first-class today — `Module::func` and `StructName::method` referenced as a value (not called) still raise a compile error.

### Generics

Turf uses real generics (`Name<T1, T2>`) rather than a single hardcoded collection type baked into the language — both user-defined generic structs and the standard library's own collections work the same way:

```
import io;
import list::List;

struct Box<T> {
    val: T
}

fn Box.new(v: T): void {
    self.val = v;
}

fn Box.get(): T {
    return self.val;
}

fn main(): int {
    mut b: Box<int> = Box::new(42);
    assert(b.get() == 42, "");

    mut nums: List<int> = List::default();
    nums.push(10);
    nums.push(20);
    assert(nums.length() == 2, "");

    return 0;
}
```

### Modules & Imports

`::` is the general namespace-resolution operator — enum variants, module-qualified functions, module-qualified types, and static methods all use it. `.` stays reserved for instance-member access only. Writing `io.printline(...)` or calling a static method as `StructName.method(...)` is a compile error, with a message pointing at the correct `::` form.

Every source file — the standard library's own modules included — is an ordinary `.tr` file with no compiler special-casing. `import` has four forms:

```
import io;                       // qualified access only: io::printline(...)
import io::printline;            // + bare access to just that one symbol
import io::[printline, print];   // + bare access to exactly these symbols
import io::[*];                  // + bare access to everything the module exports
```

A plain `import Module;` always grants qualified access (`Module::thing`), regardless of form — the on-demand forms only add *bare*, unqualified access to specific symbols on top of that. This applies uniformly to functions, structs, and enums: a struct or enum declared in another module is not usable unqualified unless it (or `*`) was explicitly on-demand imported, exactly like a bare function call. Circular imports are detected and reported with the full import chain, not a silent truncation or a hang. See `docs/architecture/modules_and_scope_resolution.md` for the complete internal and external reference.

Functions, structs, and enums are exportable automatically — anything declared at a file's top level is visible to importers with no keyword needed. A top-level *constant* needs `export`, since bare `mut`/`immut` only makes sense inside a function otherwise:

```
export immut PI: double = 3.14159;
export immut MAX_USERS: int = 100;
```

`export` only works on `immut` declarations at the true top level (a compile error either inside a function or on `export mut`), and the initializer must be a literal — an exported constant has no real storage; every reference to it is replaced by its value directly at compile time. Everything else (typos, duplicate names, immutability) behaves exactly like any other variable.

### FFI

```
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;
extern fn strlen(s: string): int;
extern from "m" fn sqrt(x: double): double;   // links -lm automatically

fn main(): int {
    unsafe {
        mut buf: ptr = malloc(64);
        free(buf);
    }
    return 0;
}
```

### `unsafe { }` and safe references (`&T`)

Raw pointer types (`ptr`, `T*`) are gated behind `unsafe { }` everywhere outside `lib/std` — declaring a raw-pointer-typed variable, casting to/from one, or accessing a field through one (there's no explicit dereference operator; field access through a raw pointer base is the closest thing) all require lexically being inside `unsafe { }`. A struct with a raw pointer field needs its *entire declaration* wrapped, not just the code that dereferences it later. An ordinary (non-`extern`) function's parameters and return type may never be raw pointers at all, even inside `unsafe { }` — `extern fn` bindings (the FFI surface itself) and anything under `lib/std` are exempt from all of this.

`&T` is a compiler-guaranteed non-null reference — `&expr` (address-of) always produces one:

```
mutable val: int = 42;
mutable ref: &int = &val;   // compiler-guaranteed non-null
```

A `&T` auto-derefs on field/member access exactly like a raw pointer does, and always safely widens into a raw `T*`/`ptr` (assigning or passing it needs no cast or `unsafe { }`). The reverse — turning a raw pointer into a `&T` — always needs an explicit `to` cast inside `unsafe { }`, since the raw pointer might be null:

```
unsafe {
    mutable p: Node* = ...;
    mutable safe: &Node = p to &Node;   // asserts p isn't null
}
```

### Assertions

```
assert(condition, "message shown on failure");
```

## Examples

**Structs, constructors, instance methods, and static methods:**

```
extern fn strlen(s: string): int;

struct User { age: int, name: string }

fn User.new(age: int, name: string): void { self.age = age; self.name = name; }
fn User.grow(): void { self.age = self.age + 1; }
fn User.rename(new_name: string): void { self.name = new_name; }
fn User.describe() static: string { return "a user"; }

fn main(): int {
    mut u: User = User::new(0, "unnamed");
    u.grow();
    assert(u.age == 1, "");

    u.rename("Bob");
    assert(User::describe() == "a user", "");
    return 0;
}
```

**Enums and modules:**

```
import io;

enum Status { Pending, Active, Closed }

fn describe(s: Status): string {
    compare s {
        matches Status::Pending:
            return "pending";
        done
        matches Status::Active:
            return "active";
        done
        otherwise:
            return "closed";
    }
}

fn main(): int {
    immut s: Status = Status::Active;
    io::printline(describe(s));
    assert(Status::COUNT == 3, "");
    return 0;
}
```

**Arrays of structs:**

```
extern fn strlen(s: string): int;

struct User {
    age: int,
    name: string
}

fn User.new(age: int, name: string): void {
    self.age = age;
    self.name = name;
}

fn main(): int {
    mut users: User[5] = [
        User::default(),
        User::default(),
        User::default(),
        User::default(),
        User::default()
    ];

    users[0] = User::new(25, "Alice");
    users[1] = User::new(30, "Bob");
    users[2] = User::new(35, "Charlie");

    assert(users[0].age == 25, "");
    assert(strlen(users[0].name) == 5, "");

    assert(users[1].age == 30, "");
    assert(strlen(users[1].name) == 3, "");

    assert(users[2].age == 35, "");
    assert(strlen(users[2].name) == 7, "");

    return 0;
}
```

**Casting and type conversion:**

```
fn main(): int {
    mut a: double = 42 to double;
    assert(a > 41.9 && a < 42.1, "");

    mut b: int = 3.75 to int;
    assert(b == 3, ""); // truncates the decimal part

    mut c: int = "100" to int;
    assert(c == 100, "");

    mut x: int = 10;
    mut s9: string = x to string;

    return 0;
}
```

**Loops, `break`, `continue`, range-`for`:**

```
fn main(): int {
    mut i: int = 0;
    mut sum: int = 0;

    while i < 10 {
        i = i + 1;
        if i % 2 == 0 {
            continue;
        }
        sum = sum + i;
    }
    assert(sum == 25, "");

    mut for_sum: int = 0;
    for j in 1..10 step j += 1 {
        if j % 2 == 0 {
            continue;
        }
        for_sum = for_sum + j;
    }
    assert(for_sum == 25, "");

    return 0;
}
```

**FFI — pointers and memory:**

```
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;
extern fn memset(p: ptr, c: int, n: int): ptr;
extern fn memcpy(dst: ptr, src: ptr, n: int): ptr;
extern fn strlen(s: string): int;

// An ordinary function may never take or return a raw `ptr`/`T*` (even
// inside `unsafe { }`) -- only `extern fn` bindings and lib/std can, so
// buffer allocation stays inlined in main() below instead of its own helper.

fn main(): int {
    unsafe {
        mut b: ptr = malloc(32);
        memset(b, 0, 32);
        free(b);
    }

    mut len: int = strlen("verify");
    assert(len == 6, "");

    return 0;
}
```

A larger FFI showcase (file I/O, `libm`, environment variables, sockets, variadic `printf` via `extern fn printf(fmt: string, ...): int;`, and linking against arbitrary system libraries via `extern from "libname" fn`) lives in the test suite alongside the rest of the examples above.

**`showcase/`** has larger, real-world-shaped programs — an HTTP server and a companion Turf HTTP client that talks to it over a real TCP socket (a full request/response round trip, both ends written in Turf, using nothing but standard-library modules — no `extern fn`/`ptr` in the showcase files themselves), plus more. See that directory's own files for details on running each one.

## Building from Source

### Requirements

- **LLVM 22** (via `LLVMConfig.cmake`)
- **CMake 3.20+**
- **C++20** compiler (Clang recommended)
- `llama.cpp` (vendored as a submodule under `external/llama.cpp`; built   automatically as a static library on first configure if not already built)

### Build

```bash
git clone --recursive <this-repo>
cd turf
chmod +x scripts/cmake_build.sh
./scripts/cmake_build.sh --release
```

`cmake_build.sh` detects available CPU cores and builds in parallel; the resulting binary is placed at `build/turf`. On first run, if `llama.cpp`'s static libraries aren't already built, CMake builds them automatically before building Turf itself — this can take a while the first time.

### Compile and run a file directly

```bash
./scripts/compile_and_run.sh path/to/file.tr
```

Compiles, runs, and cleans up the produced binary.

## Roadmap

Turf is pre-1.0. The full, actively-maintained tracker — with each item marked done or open against the real codebase — is `docs/planning/ROADMAP_v1.0.0_COMPLETE.md`. Phases 1–3 (the language backbone: FFI, `do`/`while`, `compare`, enums, bitwise/string ops, first-class functions, error handling, for-each loops) and operator overloading are complete. Highlights of what's still open:

- `unsafe { }` blocks and safe references (`&T`) — the remaining memory-model work from Phase 4.
- `std/os.tr` (env vars, argv) — the remaining Phase 4 stdlib module; see `docs/planning/STDLIB_ROADMAP.md`.
- Prompt-Oriented Programming (`@slm` syntax, structured context serialization, the SLM query loop, LoRA fine-tuning) — Phase 5. The `--smart` error-healing context pipeline is already live; the in-language `@slm { }` block is not.
- Diagnostics polish (source spans, error codes + `turf --explain`, broader "did you mean?") — see `docs/planning/ROADMAP_v1.0.0_COMPLETE.md`'s Diagnostics Polish section.
- `turf-cli`: a standalone package manager (written in Go) for Turf projects
  and dependencies — currently a working showcase, not yet linked into the
  main `turf` binary's workflow.
- An LSP server, `turf fmt`, and `turf test` — none exist yet (Phase 6).