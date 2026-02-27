# Turf

Turf is an experimental programming language and compiler written in C++. The goal is simple: fast compilation with helpful, human-friendly error messages. Instead of plain errors, Turf explains what went wrong and suggests fixes.

- The project is in early development.
- Current focus is the frontend: lexer, parser, and diagnostics.
- LLVM is used for code generation.

## Features (Implemented)

- **Type System:** Built-in types `int`, `float`/`double`, `bool`, and `string` with implicit type promotion (bool → int → double) during operations. Strings are treated as a distinct type and cannot be cast to/from numeric types.
- **Typed Variable Declarations:** Variables can be declared with explicit types (`int x = 5`, `double pi = 3.14`, `bool flag = true`, `string name = "Turf"`).
- **String Support:** First-class string type with string literals (`"hello"`), string variables, assignment, and printing. Supports escape sequences (`\n`, `\t`, `\\`, `\"`).
- **Variables:** Support for variable assignment and lookups.
- **Boolean Literals:** Support for `true` and `false` boolean values.
- **Math:** Full support for arithmetic operators (`+`, `-`, `*`, `/`, `%`, `^`) with operator precedence, including exponentiation.
- **Unary Operators:** Support for unary negation (`-x`).
- **Comparison Operators:** Full set of comparison operators (`<`, `>`, `==`, `!=`, `<=`, `>=`).
- **Control Flow:** `if`/`else` expressions and `while` loops with block syntax (`{ ... }`).
- **Block Expressions:** Group multiple expressions using `{ ... }` syntax.
- **Comments:** Single-line comments using `//` syntax.
- **Print:** Built-in `print()` function for output (supports int, double, bool, and string types).
- **Memory Management:** Automatic stack allocation using LLVM `alloca`, `store`, and `load`.
- **LLVM Backend:** Compiles source code directly to optimized LLVM IR (`output.ll`).
- **Smart Compiler (Phase 1):** Support for smart compiler error enhancements, where it suggests you what changes to make (using Levenshtein distance for variable name suggestions).

## Build and Run

Requires LLVM and a C++ compiler.

### Quick Start

Use the included helper script to compile the compiler, generate the IR, link it, and run the executable in one go:

```bash
chmod +x update_compiler.sh
./update_compiler.sh
chmod +x compile_and_run.sh
./compile_and_run.sh test.tr
```

### Manual Build

If you want to build the compiler binary manually:

```bash
clang++ main.cpp Lexer.cpp Parser.cpp Codegen.cpp Algorithms.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o turf
```

Rest steps will be the same from the Quick Start section.

## Example Code

```turf
// Typed variable declarations
int width = 10
int height = 5
double pi = 3.14
string greeting = "Hello, Turf!"

// Variables are mutable and memory-managed
int area = width * height
width = 20
int new_area = width * height

// Complex expressions with precedence
int result = (width + height) * 2

// Exponentiation (result is double due to type promotion)
int x = 3
double squared = x ^ 2

// Boolean values
bool flag = true
bool check = x > height

// Control flow with if/else
int max = if x > height then x else height

// While loops
int counter = 0
while counter < 5 {
  print(counter)
  counter = counter + 1
}
bool, and string)
print(result)
print(pi)
print(flag)
print(greetin
print(flag)
```

## Roadmap

- [x] Basic Arithmetic
- [x] Variables & Memory Assignment
- [x] Type System (int, float/double, bool)
- [x] Typed Variable Declarations
- [x] Better Error Diagnostics
- [x] Control Flow (if / else)
- [x] Comparison Operators (<, >, ==, !=, <=, >=)
- [x] While Loops
- [x] String Type (literals, variables, printing)
- [x] Print Function
- [x] Comments (single-line)
- [ ] Functions

## Status

**Active Development.**, `string`), arithmetic (including exponentiation), control flow (`if`/`else`, `while`), comparison operators, boolean literals, string literals with escape sequence

The compiler supports typed variables (`int`, `double`, `bool`), arithmetic (including exponentiation), control flow (`if`/`else`, `while`), comparison operators, boolean literals, single-line comments, and output via `print()`.

> After a certain phase of development, both the 'Build' and 'Run' phase will be packaged in a separate "Turf Compiler" package.
