# Turf

Turf is an experimental programming language and compiler written in C++. The goal is simple: fast compilation with helpful, human-friendly error messages. Instead of plain errors, Turf explains what went wrong and suggests fixes. 

Turf has matured beyond its early phases and is now a fully programmable language. It has been packaged into a standalone cross-platform compiler that handles parsing, code generation, and producing the binary seamlessly.

## Installation

You don't need to build from source to start using Turf. You can download the pre-compiled binaries for your OS and architecture directly from the release assets:

- **macOS (Apple Silicon / ARM64):**
  Download `turf-v0.4.0-macos-arm64.tar.gz` and extract it:
  ```bash
  tar -xzf turf-v0.4.0-macos-arm64.tar.gz
  ```

- **Linux (x86_64):**
  Download `turf-v0.4.0-linux-x86_64.tar.gz` and extract it:
  ```bash
  tar -xzf turf-v0.4.0-linux-x86_64.tar.gz
  ```

- **Linux (Arch x86_64):**
  Download `turf-v0.4.0-linux-arch-x86_64.tar.gz` and extract it:
  ```bash
  tar -xzf turf-v0.4.0-linux-arch-x86_64.tar.gz
  ```

- **Windows (x86_64):**
  Download `turf-v0.4.0-windows-x86_64.zip` and extract it using your file explorer or terminal.

Once extracted, you can compile and run Turf programs directly:
```bash
# Compile the Turf source code into an executable
./turf example.tr -o program

# Run the generated executable
./program
```

## Features

- **Type System:** Built-in types `int`, `double`, `bool`, and `string` with implicit type promotion (bool → int → double) during operations. 
- **Typed Variable Declarations:** Variables can be declared with explicit types (`int x = 5`, `double pi = 3.14`, `bool flag = true`, `string name = "Turf"`).
- **String Support:** First-class string type with string literals (`"hello"`). Supports escape sequences (`\n`, `\t`, `\\`, `\"`).
- **Type Casting:** Explicit type casting allows conversions like `int("100")` or `double(42)`.
- **Variables:** Support for variable assignment, compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`), and increment/decrement operators (`++`, `--`).
- **Math:** Full support for arithmetic operators (`+`, `-`, `*`, `/`, `%`, `^`) with operator precedence, including exponentiation.
- **Logical Operators:** Evaluate complex conditions with `&&` (AND) and `||` (OR).
- **Unary Operators:** Support for unary negation (`-x`).
- **Comparison Operators:** Full set of comparison operators (`<`, `>`, `==`, `!=`, `<=`, `>=`).
- **Control Flow:** `if`/`else` expressions and `while` loops with block syntax (`{ ... }`), including `break` and `continue` statements.
- **Block Expressions:** Group multiple expressions using `{ ... }` syntax.
- **Comments:** Single-line comments using `//` syntax.
- **Print:** Built-in `print()` function for output.
- **LLVM Backend:** Compiles source code directly to a native executable via LLVM.
- **Smart Compiler:** Detailed error messages including typo detection and context-aware suggestions (using Damerau-Levenshtein distance).

## Build from Source (Manual)

If you'd like to build the Turf compiler from source, ensure you have LLVM and a modern C++ compiler (like `clang++`) installed.

You can then use the included build scripts for quick setup and testing:
```bash
# Compile the compiler itself
./scripts/update_compiler.sh

# Compile a Turf file and run the executable
./scripts/compile_and_run.sh tests/test.tr
```

## Example Code

A taste of what's possible with Turf right now:

```turf
// Typed variable declarations
int width = 10
int height = 5
double pi = 3.14
string greeting = "Hello, Turf!"

// Variables are mutable
int area = width * height
width = 20
int new_area = width * height

// Complex expressions with proper precedence
int result = (width + height) * 2

// Exponentiation and modulo
double squared = 3.0 ^ 2.0
int rem = 17 % 5

// Boolean values and logical operators
bool flag = true
bool check = result > height && flag == true || width < 5

// Type casting
int parsed_int = int("42")
double cast_val = double(10)

// Control flow with if/else
int max = if width > height then width else height

// While loops with break/continue
int counter = 0
int sum = 0
while counter < 5 {
  counter++
  if counter == 3 {
    continue
  } else {}
  sum += counter
}

// Built-in print supports int, double, bool, and string ranges
print(greeting)
print(result)
printline(flag)
```

## Status

**Active Development.** The compiler is fully capable of taking code to native executables. It currently supports typed variables (`int`, `double`, `bool`, `string`), arithmetic, exponentiation, modulo, logical short-circuiting, type casting, control flow (`if`/`else`, `while`, `break`, `continue`), comparison operators, booleans, single-line comments, and output via `print()` and `printline()`.
