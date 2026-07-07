# Turf

Turf is a statically-typed, LLVM-backed programming language. It compiles directly to native binaries, supports structs, generics, pointers, and native FFI, and ships with an integrated compiler-diagnostics system built around a locally-run language model.

Current version: **v0.7.0**, pre-1.0, actively developed.

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

When a diagnostic fires with `--smart` or `--online` enabled, this is what actually happens, in order:

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

### `--smart` vs. `--online`

Both flags run this exact same pipeline, on error only — no cost, no model inference, nothing happens on a clean compile. The difference is purely which model answers:

- **`--smart`** runs entirely locally via a small quantized model loaded through `llama.cpp` — no network call, no external dependency.
- **`--online`** routes the identical pipeline (same structural context, same grammar constraints, same sandbox validation) to a larger model hosted online instead, for cases on more complex programs where the extra capability of a bigger model is worth the round-trip and where `--smart`'s local model may not have enough capacity to produce a fix with real confidence.

```bash
turf my_program.tr --smart -o my_program
turf my_program.tr --online -o my_program
```

## Installation

No package-manager distribution yet. Build from source (see [Building from Source](#building-from-source)), or use a pre-built binary from the [Releases page](../../releases) if one exists for your platform — CI currently builds for Linux (x86_64, Arch x86_64), macOS (Apple Silicon
and Intel), and Windows (x86_64).

## Quick Start

```
// hello.tr
fn main(): int {
    io.printline("Hello, Turf!");
    return 0;
}
```

```bash
turf hello.tr -o hello
./hello
```

I/O lives in the standard library's `io` module and is called qualified — `io.printline`, not a bare global.

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
    io.printline("positive");
} elseif x < 0 {
    io.printline("negative");
} else {
    io.printline("zero");
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
```

### Structs

Fields are `name: Type`, comma-separated. Instances are constructed with brace syntax and named fields:

```
struct User {
    age: int,
    name: string
}

mut u: User = User { age: 10, name: "Alice" };
u.age = 25;
```

Methods are declared as `StructName.methodName(...)`. `self` is implicit — it's not a declared parameter, but it's available inside the method body:

```
fn User.grow(): void {
    self.age = self.age + 1;
}

fn User.rename(new_name: string): void {
    self.name = new_name;
}
```

### Arrays

Fixed-size, typed, and index directly — including arrays of structs, with field access chaining through the index:

```
mut users: User[5] = [
    User { age: 0, name: "" },
    User { age: 0, name: "" },
    User { age: 0, name: "" },
    User { age: 0, name: "" },
    User { age: 0, name: "" }
];

users[0] = User { age: 25, name: "Alice" };
assert(users[0].age == 25, "");
```

### Generics

Turf uses real generics (`Name<T1, T2>`) rather than a single hardcoded collection type baked into the language.

### FFI

```
extern fn malloc(size: int): ptr;
extern fn free(p: ptr): void;
extern fn strlen(s: string): int;
extern from "m" fn sqrt(x: double): double;   // links -lm automatically

fn main(): int {
    mut buf: ptr = malloc(64);
    free(buf);
    return 0;
}
```

### Assertions

```
assert(condition, "message shown on failure");
```

## Examples

**Structs and methods:**

```
extern fn strlen(s: string): int;

struct User { age: int, name: string }

fn User.grow(): void { self.age = self.age + 1; }
fn User.rename(new_name: string): void { self.name = new_name; }

fn main(): int {
    mut u: User = User { age: 10, name: "Alice" };
    u.grow();
    assert(u.age == 11, "");

    u.rename("Bob");
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

fn main(): int {
    mut users: User[5] = [
        User { age: 0, name: "" },
        User { age: 0, name: "" },
        User { age: 0, name: "" },
        User { age: 0, name: "" },
        User { age: 0, name: "" }
    ];

    users[0] = User { age: 25, name: "Alice" };
    users[1] = User { age: 30, name: "Bob" };
    users[2] = User { age: 35, name: "Charlie" };

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

fn make_buffer(size: int): ptr {
    mut p: ptr = malloc(size);
    memset(p, 0, size);
    return p;
}

fn main(): int {
    mut b: ptr = make_buffer(32);
    free(b);

    mut len: int = strlen("verify");
    assert(len == 6, "");

    return 0;
}
```

A larger FFI showcase (file I/O, `libm`, environment variables, sockets, variadic `printf` via `extern fn printf(fmt: string, ...): int;`, and linking against arbitrary system libraries via `extern from "libname" fn`) lives in the test suite alongside the rest of the examples above.

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

Turf is pre-1.0. In progress:

- `turf-cli`: a standalone package manager (written in Go) for Turf projects
  and dependencies — currently a working showcase, not yet linked into the
  main `turf` binary's workflow.
- Continued standard library growth.