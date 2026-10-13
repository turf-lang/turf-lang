# Turf

Turf is a statically-typed, LLVM-backed programming language. It compiles directly to native binaries, supports structs, enums, generics, state machines, modules, pointers, and native FFI, ships with a real standard library (strings, collections, file I/O, JSON, time, randomness, filesystem paths, networking), and includes an integrated compiler-diagnostics system built around a locally-run language model.

Current version: **v1.0.0** is the last tagged release. This branch
(`refactor/architecture`) is unreleased, in-development work on the
Architecture Overhaul — real generics (monomorphization), a tracing garbage
collector for the heap, `&?T` nullable safe references, and a numeric/memory
semantic freeze — targeting the first public release, **v2.0.0**. See
`docs/planning/ROADMAP_v2.0.0.md` for what's left before that tag.

## Table of Contents

1. [Why Turf](#why-turf)
2. [Installation](#installation)
3. [Quick Start](#quick-start)
4. [Language Tour](#language-tour)
5. [Standard Library](#standard-library)
6. [Examples](#examples)
7. [Building from Source](#building-from-source)
8. [Documentation](#documentation)
9. [Roadmap](#roadmap)

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

**`--smart` needs two things: the smart build, and one model.**

- **The smart build.** Every release has two archives per platform: the plain one, and a `-smart` one that is the same compiler with the model runtime in it. Install it with `install.sh --smart` (Windows: `install.ps1 -Smart`). In the plain build `--smart` prints one note saying it is not included and you get the compiler's own diagnostics. A source build has the runtime unless you pass `--no-smart`.
- **One model, which you choose.** No model is shipped or downloaded for you. Put exactly **one** `.gguf` file in the `models/` folder of your install (`~/.turf/models`, `%LOCALAPPDATA%\turf\models`; in a checkout, `models/` at its root). Its name does not matter.
  - **None there:** `--smart` says so, names the folder, and shows the compiler's own diagnostics.
  - **One:** it is used.
  - **More than one:** `--smart` uses none of them, because it cannot know which you meant; it lists what it found. Leave one.
- **Recommended:** [`qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`](https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF), about 1 GB on disk and about 1.5 GB of RAM at its peak while it works on an error. It is the model TurfLang is developed and continuously tested with. Any GGUF model `llama.cpp` can run will load, so with more memory you can try a larger one; the prompts are tuned on the recommended model, and every suggestion is checked by the compiler before you see it whichever model made it.

Installing a new version or uninstalling leaves your `models/` folder and the model in it alone.

Useful compiler introspection flags, beyond ordinary compilation: `--dump-ast`, `--dump-resolved`, `--dump-typechecked`, `--dump-modules` (everything the compiler knows about the current program's modules — every imported module's functions, struct/enum type ownership, and every on-demand import), `--emit-llvm`.

## Installation

**Linux (x86-64 and arm64) and macOS (Apple Silicon):**

```bash
curl -fsSL https://raw.githubusercontent.com/turf-lang/turf-lang/master/scripts/install/install.sh | sh
```

`install.sh` downloads the release archive for your machine, **checks its SHA-256 (and installs nothing if that fails)**, unpacks it into `~/.turf`, and adds `~/.turf/bin` to your `PATH` with one marked block in your shell's start-up file, so `turf` works in any **new** terminal. `--smart` installs the build that can run a [`--smart`](#--smart) model. Nothing else is needed to compile and run programs (on Linux the compiler needs glibc 2.34 or newer: Ubuntu 22.04, Debian 12, RHEL 9, Amazon Linux 2023 and later; the programs it builds are static and run anywhere); **git dependencies need `git`**, which a Mac does not have until you install it (`xcode-select --install` or `brew install git`; `/usr/bin/git` alone is only a placeholder). `--prefix DIR` installs elsewhere, `--version v2.0.0` picks a release, `--no-modify-path` leaves your start-up files alone, and `--uninstall` removes exactly what it added (never your projects or the package cache). It is also attached to every release.

**Windows (x86-64), from PowerShell:**

```powershell
irm https://raw.githubusercontent.com/turf-lang/turf-lang/master/scripts/install/install.ps1 | iex
```

`install.ps1` does the same: it downloads the Windows archive, **checks its SHA-256 (and installs nothing if that fails)**, unpacks it into `%LOCALAPPDATA%\turf`, and adds `%LOCALAPPDATA%\turf\bin` to your **user** `PATH` (no administrator rights; nothing machine-wide), so `turf` works in any **new** terminal. A piped `irm | iex` cannot take options, so use the environment variables `TURF_INSTALL_VERSION`, `TURF_INSTALL_DIR` and `TURF_INSTALL_SMART=1` (the build that can run a [`--smart`](#--smart) model; `-Smart` as an option), or save the script and run `powershell -ExecutionPolicy Bypass -File install.ps1 -Version v2.0.0 -Prefix D:\tools\turf -NoModifyPath`; `-Uninstall` removes exactly what it added (never your projects or the package cache). It is also attached to every release.

**By hand (any platform):** download the archive from the [Releases page](../../releases) (Linux x86_64 and arm64, macOS Apple Silicon, Windows x86_64; Intel Mac releases were dropped, and the separate Arch archive is gone because the compiler in the Linux archive now carries its own LLVM; see `build-archives.yml`'s comments. each platform has a plain archive and a `-smart` one, see [`--smart`](#--smart)) and unpack it. It is one folder: `bin/` (`turf`, `turfc`), `lib/` (the runtime and standard library), `docs/`, `share/turf/examples/`. Run `bin/turf` from where it is, or put `bin/` on your `PATH`; no `TURF_HOME` is needed.

> A release does not yet bundle everything it needs: the compiler still links LLVM dynamically and links your programs with the system `cc` (a self-contained release is `ROADMAP_v2.0.0.md` REL-008). Homebrew/Scoop/AUR packages (generated by `scripts/release/gen_recipes.sh` and attached to each release) follow that.

### Setting up `TURF_HOME`

The compiler resolves the standard library's `import`s (under `lib/std/`) against the `TURF_HOME` environment variable (dependencies in a project come from `turf.toml`, not from here, and `lib/usr/` is searched only by single-file builds until it is retired): it should point at the Turf installation root, i.e. the folder that directly contains `lib/`. Running `turf` from an installed or extracted release archive, or from a source checkout you've just built, needs no setup at all — with `TURF_HOME` unset, the compiler finds `lib/` from its own real path (`<bin>/..`), through symlinks too. You only need `TURF_HOME` if you copy the binaries out of that folder.

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

**Get `turf`.** Either [install a release](#installation), or build from source. This produces `build/turf` and `build/turfc`, and `scripts/install/dev_install.sh` links `turf` into `~/.local/bin` so it works from any directory:

```bash
./scripts/cmake_build.sh --release --no-smart
./scripts/install/dev_install.sh  # --prefix DIR for elsewhere; put DIR/bin on your PATH
turf version                      # prints both the turf and turfc versions
```

`turf version` (or `turf --version`) prints something like this, and is what to paste into a bug report:

```
turf 1.0.0-dev  (needs turfc interface 1.1+)
turfc 2.0.0-dev  (interface 1.2, /home/you/turf/build/turfc)
  llvm 22.1.8
  runtime turf_rt_abi_2_0
  root /home/you/turf
```

`turf help` lists the commands and `turf help <command>` shows one command's options. `turf` exits 0 on success, 1 when it reports a problem (a compile error, a bad manifest), and 2 when the command line itself is wrong.

(`--no-smart` skips the llama.cpp model runtime for `--smart` and its slow first build; see [Building from Source](#building-from-source).)

**A first project.** `turf new` makes a directory with a `turf.toml`, a starter `src/main.tr` and a `README.md` of the everyday commands, and `turf run` builds and runs it:

```bash
turf new hello
cd hello
turf run                          # prints: Hello, TurfLang! This is hello.
```

**A dependency.** A library is a package too (`turf new geometry --lib`, whose root module `src/geometry.tr` is the only thing another package can import). `turf add` records it in `turf.toml`, and `import geometry;` then reaches its `pub` items:

```bash
turf new geometry --lib           # next to hello/
cd hello
turf add geometry --path ../geometry
```

A dependency can also be `{ git = "…", tag = "…" }`, which `turf` fetches, pins in `turf.lock` and verifies on every build. Manifest keys, dependencies, imports across packages, the lock file and the cache are documented in [`docs/reference/projects_and_packages.md`](docs/reference/projects_and_packages.md), and [`examples/`](examples/README.md) holds a library and a program that depends on it, ready to run.

**A single file** needs no project:

```
// hello.tr
import io;

fn main(): int {
    io::printline("Hello, Turf!");
    return 0;
}
```

```bash
turf hello.tr -o hello     # or `turf run hello.tr` to compile, run and clean up in one step
./hello
```

`turf` is the tool you use. `turf check hello.tr` reports diagnostics without writing anything, `turf explain E090` explains an error code, and `turf help` lists the rest. Behind it is `turfc`, the compiler, which `turf` drives. `turfc` is the interface for tools (editor integrations, build systems); you don't need to call it yourself. The two are versioned independently, and `turf version` shows both. Building from source puts both in `build/`; `scripts/install/dev_install.sh` puts `turf` on your `PATH`.

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

### Type inference (`guess`)

An initializer's type can be inferred instead of written out — either by omitting the `: Type` annotation entirely, or by spelling it explicitly as `guess`:

```
mut a = 5;              // infers int
mut b: guess = 5.5;     // infers double (explicit spelling is equivalent)
immut name = "Turf";    // infers string
```

The inferred type is fixed permanently at the declaration — a `mut` variable can be reassigned, but only with a value of that same inferred type. `typeof(x)` returns the inferred type's name as a string.

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

An error variant can carry typed data, read back by destructuring it in a `compare`, and `try` passes a failure on to your own caller instead of handling it:

```
error IOError {
    NotFound(path: string),
    Timeout
}

fn open(path: string): !int { ... }

fn load(path: string): !int {
    immut fd: int = try open(path);      // on failure: returns open's error, unchanged
    return fd;
}

fn report(path: string): int {
    immut v: int = load(path) recover as err run {
        compare err {
            matches IOError::NotFound(p):
                io::printline("not found: " + p);
                return 1;
            done
            otherwise:
                return 2;
        }
    };
    return v;
}
```

`docs/reference/error_handling.md` has the full rules (payload construction, `err.message`, `try` in `main`, returning a fallible call directly).

`recover as err run { ... }` requires the callee to declare a real `error` type — a bare `error GeneralError;` type (no variants beyond the implicit `GeneralError::DEFAULT`) can still use `recover produce`/`recover run`, just not `recover as`. See `docs/architecture/errors_and_recover.md` for the full spec.

### `defer`

A `defer`'d statement runs when the enclosing function returns, regardless of which `return` triggered it — the deterministic-cleanup answer for OS resources (file handles, sockets, manual-heap buffers) that a tracing garbage collector's mark-sweep pass can't help with, since it has no destructor concept. Multiple `defer`s in the same block run in reverse (LIFO) order:

```
fn appendThree(t: &Trail): void {
    defer t.log = t.log + "1";
    defer t.log = t.log + "2";
    defer t.log = t.log + "3";
    // on return, these run in order 3, 2, 1 -- t.log ends up "321"
}
```

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

A plain `import Module;` grants qualified access (`Module::thing`) and nothing else; the on-demand forms grant *bare*, unqualified access to specific symbols and nothing else — each form needs its own line if you use both spellings. A file may use `Module::` only for a module it imports itself: `import json;` does not let you write `list::List<int>` just because `json` imports `list` (that is error E145, and its hint names the line to add). The exception is the **prelude**: `int`, `double`, `bool`, `char`, `strings`, `array`, `io`, `math` and `sys` need no import in any file, and the functions of `io`, `math` and `sys` are callable by their bare names — `printline("hi")`, `abs(-3)`, `exit(1)` — unless your own file declares or explicitly imports a function of that name, which then wins. This applies uniformly to functions, structs, and enums: a struct or enum declared in another module is not usable unqualified unless it (or `*`) was explicitly on-demand imported, exactly like a bare function call. Circular imports are detected and reported with the full import chain, not a silent truncation or a hang. See `docs/architecture/modules_and_scope_resolution.md` for the complete internal and external reference.

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

A file with several `extern fn`s that all need to pass raw pointers around (a C callback signature, a struct of raw fields) can declare itself exempt from `unsafe { }` once, file-wide, instead of wrapping each use — see the capability declaration below.

### `unsafe { }` and safe references (`&T`, `&?T`)

Raw pointer types (`ptr`, `T*`) are gated behind `unsafe { }` by default — declaring a raw-pointer-typed variable, casting to/from one, or accessing a field through one (there's no explicit dereference operator; field access through a raw pointer base is the closest thing) all require lexically being inside `unsafe { }`. A struct with a raw pointer field needs its *entire declaration* wrapped, not just the code that dereferences it later. An ordinary (non-`extern`) function's parameters and return type may never be raw pointers at all, even inside `unsafe { }` — `extern fn` bindings (the FFI surface itself) are exempt from all of this, and so is any file that declares itself exempt (see the capability declaration below).

A file can grant itself blanket use of raw pointers — the same privilege `lib/std` itself uses — with a declaration as its own first top-level statement:

```
unsafe module;

fn cmpints(a: ptr, b: ptr): int32 {   // legal here, and not gated per-use
    return 0;
}
```

Honored unconditionally for a file under the Turf install root (`lib/std`/`lib/usr`); anywhere else it needs the `--allow-unsafe-modules` CLI flag, so a plain `turf myfile.tr` never silently grants it. See `docs/architecture/pointers_and_references.md` §1.7 for the full mechanism.

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

`&?T` is `&T`'s **nullable** counterpart — a safe reference that can be `null`, checked by a real compile-time nullability analysis instead of trusting the programmer. It's the type that makes a safe self-referential struct (a linked list, a tree) possible without `unsafe { }` anywhere in the declaration:

```
struct Node {
    val: int,
    next: &?Node   // no unsafe { } needed anywhere in this struct decl
}

fn sum(head: &?Node): int {
    mutable total: int = 0;
    mutable current: &?Node = head;
    while (current != null) {          // narrows `current` to non-null...
        total = total + current.val;   // ...so this field access is legal
        current = current.next;
    }
    return total;
}
```

Field access on a `&?T` compiles only where the compiler has proven it non-null on every path reaching that access; an unguarded access is a compile error (`E081`). `&?T -> &T` needs an explicit, narrowing-checked `to` cast — never implicit. See `docs/architecture/pointers_and_references.md` §1.8 for the full narrowing rules.

### State machines

`machine` declares a set of states plus a transition graph in one construct — a `TurfMachineType` is a real `enum` underneath (`::`-access, `==`, `COUNT` all just work), with a `.transition(State)` call the compiler tries to prove safe at compile time:

```
machine TrafficLight {
    Red -> Green,
    Green -> Yellow,
    Yellow -> Red
}

fn main(): int {
    mut light: TrafficLight = TrafficLight::Red;

    compare light {
        matches TrafficLight::Red:
            light.transition(TrafficLight::Green);   // compile-time-proven safe: zero runtime cost
        done
        otherwise:
            io::printline("not red");
    }

    assert(light == TrafficLight::Green, "");
    return 0;
}
```

Inside a single-pattern `matches` arm that's narrowed the receiver to one exact state, `.transition(...)` compiles to a plain store — no runtime check at all. Everywhere else it falls back to a runtime switch that panics on an edge the graph doesn't declare. See `docs/reference/state_machines.md` for the full reference, including exactly which shapes qualify for the compile-time-proven path.

### Assertions

```
assert(condition, "message shown on failure");
```

## Standard Library

Every module is an ordinary `.tr` file under `lib/std/` — no compiler special-casing, `import <module>;` doesn't care which subfolder it lives in. `int`/`double`/`bool`/`char`/`strings`/`array` are auto-imported into every file (the "prelude"); everything else needs an explicit `import`.

| Module | What it gives you |
|---|---|
| `strings` | length/contains/index_of/starts_with/ends_with/repeat/substring/replace/to_upper/to_lower/trim(_start/_end), `split`/`join`, `pad_left`/`pad_right`, `parse_int`/`parse_double` |
| `io` | `print`/`printline`/`input`, plus file I/O: `read_to_string`, `read_lines`, `write_string`, `exists`, `eprint`/`eprintline`, `flush` |
| `list` / `stack` / `queue` / `map` / `set` | `List<T>`/`Stack<T>`/`Queue<T>`/`Map<K,V>`/`Set<T>` (`List<T>` has `sort`/`binary_search`/`find` methods) |
| `mem` | checked `alloc`/`alloc_zeroed`/`grow`/`dup`/`zero` wrappers over `malloc`/`calloc`/`realloc`/`free`, plus `Pool<T>` |
| `math` | `abs`/`power`/`min`/`max`/`clamp`, libm bindings (`sqrt`, `sin`, `pow`, ...), `PI`/`E` |
| `time` | `now_seconds()` (wall clock), `clock_ticks()`/`elapsed_ms()` (CPU time — not wall time, see the module's own doc comment), `sleep_seconds`/`sleep_ms` |
| `random` | `random_int`/`random_double`/`random_bool` — not cryptographically secure |
| `fs` | path manipulation (`join`/`dirname`/`basename`/`extension`/`is_absolute`) and directory ops (`mkdir`/`remove_file`/`remove_dir`/`is_dir`) |
| `net` | `TcpListener`/`TcpConnection`, plus `resolve_hostname` for DNS |
| `http` | `Request`/`Response` HTTP/1.1 parsing/building over `net`, plus a callback-based `Server` router |
| `json` | a hand-rolled, dependency-free `JsonValue` parser/serializer (`json::parse`/`json::stringify`) |
| `os` | `env`/`set_env`, `argc`/`argv` |
| `sys` / `test` | `panic`/`exit`/`abort`; `assert`/`fail`/`assert_eq` |

Full per-function reference for every module: [`docs/reference/stdlib/`](docs/reference/stdlib/).

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

**`showcase/`** has larger, real-world-shaped programs — an HTTP server and a companion Turf HTTP client that talks to it over a real TCP socket (a full request/response round trip, both ends written in Turf, using nothing but standard-library modules — no `extern fn`/`ptr` in the showcase files themselves), a key-value store client/server pair, a self-hosting-adjacent LLVM-C FFI demo, a text-analysis tool exercising `strings`/`fs`/`Set`/sort-and-search/`time`, and a JSON-config-driven dice roller exercising `json`/`random`/`net::resolve_hostname`. See that directory's own files for details on running each one.

## Building from Source

### Requirements

- **LLVM 22** (via `LLVMConfig.cmake`)
- **CMake 3.20+**
- **C++20** compiler (Clang recommended), which also provides the `cc` that `turfc` links every program with
- **Go 1.26+**, optional: only for the `turf` front end (`build/turf`). Without it you get `build/turfc` alone.
- `llama.cpp`, optional: only for `--smart`'s local model. It is vendored as a submodule under `external/llama.cpp` and built automatically as a static library on the first configure.

### Build

```bash
git clone --recursive <this-repo>
cd turf
./scripts/cmake_build.sh --release              # everything, including --smart's model runtime
```

Just the compiler and `turf`, without the llama.cpp submodule or its (slow) first build:

```bash
git clone <this-repo>
cd turf
./scripts/cmake_build.sh --release --no-smart   # same as cmake -DTURF_SMART=OFF
```

A `--no-smart` build is the same compiler: every diagnostic, deterministic fix and feature works. The only difference is that `--smart` says it is not part of this build and shows the compiler's own diagnostics. With the runtime built in, `--smart` runs the one `.gguf` file in `models/` at the checkout's root (see [`--smart`](#--smart)).

`cmake_build.sh` detects available CPU cores and builds in parallel. The compiler is placed at `build/turfc` and, if Go is installed, the front end at `build/turf`. On the first `--smart`-enabled configure, CMake builds `llama.cpp`'s static libraries before building Turf itself, which takes a while the first time.

### Compile and run a file directly

```bash
./scripts/dev/compile_and_run.sh path/to/file.tr
```

Compiles, runs, and cleans up the produced binary.

## Documentation

- **[`docs/reference/`](docs/reference/)** — the full language reference, one topic per page (variables & types, control flow, error handling, functions & modules, FFI & `unsafe`, structs & enums, state machines, generics & operators, arrays, the standard library, projects & packages, CLI & diagnostics). Start at [`docs/reference/README.md`](docs/reference/README.md).
- **[`docs/architecture/`](docs/architecture/)** — deep internals references for contributors: the module/`::` system, pointers/`unsafe`/`&T`, error handling/`recover`, the compiler's memory model (what's heap vs. stack, in plain CS terms, not LLVM internals), and the full Lexer→Parser→Resolver→TypeChecker→Codegen pipeline plus its CFG static-analysis passes.
- **[`docs/planning/`](docs/planning/)** — `archive/ROADMAP_v1.0.0_COMPLETE.md` (the completed v1.0.0 tracker) and `FUTURE.md` (what's next, see below).

## Roadmap

**Turf is `v1.0.0`.** Every phase in `docs/planning/archive/ROADMAP_v1.0.0_COMPLETE.md` is done: the full language backbone (FFI, `do`/`while`, `compare`, enums, bitwise/string ops, first-class functions, error handling, for-each loops, state machines, operator overloading), `unsafe { }`/`&T` safe references, the standard library rewrite via FFI, the `--smart` diagnostics retry loop, and diagnostics polish (source spans, error codes + `turf --explain`, broader "did you mean?").

What's next is tracked in **[`docs/planning/FUTURE.md`](docs/planning/FUTURE.md)**, roughly in priority order:

- Developer tooling: an LSP server that builds against this compiler (REL-002). `turf test` (runs a project's `tests/`), `turf fmt` and a project/dependency tool (`turf new`, `build`, `add`; path and git dependencies) exist today; a package registry does not (`FUTURE.md` F-7c).
- `@slm { }` in-source syntax (Prompt-Oriented Programming) — the `--smart` error-healing pipeline is live today; writing natural-language blocks that generate real code is not.
- LoRA fine-tuning for the local SLM.
- A hashed `HashMap<K, V>` (today's `map`/`set` are linear-scan association lists — see the Standard Library section above; a struct key can supply its own `hash()` now that generics are monomorphized, see `docs/reference/stdlib/collections.md`).
- Two disjoint heaps: `mem::alloc`/`mem::alloc_zeroed`/`mem::grow`/`mem::Pool<T>` (`lib/std/mem/`) are entirely manual, C-style — no automatic reclamation of that memory. Every collection (`List<T>`, `Map<K,V>`, `string` concatenation, ...) allocates on a separate, garbage-collected heap instead: a real non-moving mark-sweep collector (`libturfrt`) traces reachability precisely through declared pointer fields and conservatively through the stack/registers, running automatically on allocation pressure or on demand via `gc_collect()`. `mem::root(p)`/`mem::unroot(p)` pin a GC object reachable only through manual memory. An automatic-reference-counting design was considered and explicitly rejected in favor of this tracing collector (cycles, FFI-boundary pinning, and per-assignment retain/release cost were the deciding factors) — see `docs/architecture/memory_model.md` §1.7 for the full model.

**Turflang 2.0.0** is reserved for self-hosting — writing the compiler in Turf itself, once the above has landed.