# Modules and Standard Library

Turf-Lang provides a module system that lets you organize code across multiple files and utilize a built-in standard library for common tasks.

---

## The Module System

Modules are simply other `.tr` (Turf-Lang) source files. You can include them into your current scope using the `import` keyword.

### Import Resolution

When you type `import module_name`, the Turf-Lang compiler searches in two locations:
1. First, it looks in the global standard library path: `lib/std/module_name.tr`.
2. If not found, it looks for a local file relative to the execution: `module_name.tr`.

```turf
import math     // Checks lib/std/math.tr first
import my_file  // Checks lib/std/my_file.tr, then my_file.tr locally
```

*(Note: Module imports currently pull the global declarations of the target file directly into the compilation context.)*

---

## The Standard Library (`lib/std/`)

Turf-Lang includes a small but powerful standard library.

### `io.tr`

Handles input and output.
- `print(val)`: Prints the value to the console without a newline. Supports `int`, `double`, `bool`, and `string`.
- `printline(val)`: Prints the value followed by a newline.
- `input()`: Reads user input from standard input and returns it as a `string`.

### `math.tr`

Provides common mathematical operations.
- `power(base, exp)`: Raises `base` to the `exp`. Overloaded for combinations of `int` and `double`.
- `abs(val)`: Returns the absolute value. Overloaded for `int` and `double`.
- Various LLVM floating-point bindings: `pow`, `sqrt`, `sin`, `cos`, `floor`, `ceil`, `round`.

### `mem.tr`

Provides manual memory management.
- `malloc(size)`: Allocates `size` bytes, returns `void*`.
- `realloc(ptr, size)`: Reallocates `ptr` to `size` bytes, returns `void*`.
- `free(ptr)`: Frees the memory at `ptr`.

### `prelude.tr`

The prelude is a special module. **It is automatically imported by the compiler** before your code is processed. You do not need to `import prelude` explicitly.
It provides standard exception structs that the language infrastructure relies on:
- `Exception`
- `ArithmeticException`
- `OutOfBoundsException`

---

## Frequently Asked Questions (FAQs)

**Q: Do I need to compile the modules separately?**
A: No, the `import` keyword recursively parses the target file and includes its AST directly into the compilation unit. You just run `turf main.tr`.

**Q: Can I resolve nested modules like `import folder/module`?**
A: Currently, imports resolve strict file names in the immediate directories or `lib/std`. Nested paths require string interpolation which is not natively bound to the `import` keyword yet.
