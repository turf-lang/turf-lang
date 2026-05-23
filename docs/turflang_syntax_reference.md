# Turf-Lang Syntax Reference

This document serves as the comprehensive syntax reference for Turf-Lang, covering its data types, control structures, and standard library following its architectural overhaul.

## Table of Contents
1. [Hello World](#hello-world)
2. [Types](#types)
3. [Literals](#literals)
4. [Variables & Arrays](#variables--arrays)
5. [Structs](#structs)
6. [Operators & Expressions](#operators--expressions)
7. [Control Flow](#control-flow)
8. [Functions](#functions)
9. [Error Handling](#error-handling)
10. [Special Expressions](#special-expressions)
11. [Standard Library (Built-ins)](#standard-library-built-ins)

---

## Hello World

A simple Hello World program in Turf-Lang:

```c
fn int main() {
    printline("Hello, World!");
    return 0;
}
```

---

## Types

Turf-Lang uses strong static typing. The type system supports primitives, arrays, generics, and user-defined structs.

### Primitive Types
- `int`: 64-bit signed integer
- `double`: 64-bit floating-point number
- `bool`: Boolean type (`true` or `false`)
- `string`: String literal or byte pointer
- `void`: Used only as a function return type to indicate no value is returned

### Pointer Types
You can create a pointer to any type by appending `*`:
- `int*`, `double*`, `string*`, etc.

### Arrays
Arrays have a specific element type and optionally a compile-time fixed size.
- `int[5]` represents an array of 5 integers.
- Multi-dimensional arrays are supported (e.g., `int[5][10]`).

### Generic Lists
Turf-Lang provides a built-in generic list type.
- `list<int>`, `list<string>`, `list<double>`, etc.

---

## Literals

- **Integer**: `42`, `-10`
- **Double**: `3.14`, `-0.001`
- **Boolean**: `true`, `false`
- **String**: `"Hello, World\n"` (Escape sequences like `\n`, `\t`, `\\`, `\"` are supported)

---

## Variables & Arrays

### Variable Declaration
Variables must be declared with a type and must be initialized immediately.

```c
int x = 10;
double pi = 3.1415;
bool flag = true;
string name = "Turf";
```

### Array Declaration
Arrays can be initialized with an array literal.

```c
int[3] arr = [1, 2, 3];
```
Array elements are accessed using zero-based index `[ ]`:
```c
int first = arr[0];
arr[1] = 42;
```

### Lists
Lists are declared using the `list<T>` generic type.

```c
list<int> numbers;
```
*(Note: standard list methods like `.length`, `.push()`, `.pop()` are accessible depending on the standard library implementation).*

---

## Structs

You can define custom data structures using the `struct` keyword.

### Declaration
```c
struct Point {
    int x
    int y
}
```

### Initialization
To instantiate a struct, call it like a function with arguments matching the order of its fields:
```c
Point p = Point(10, 20);
```

### Field Access
Access struct fields using the `.` operator:
```c
int x_coord = p.x;
p.y = 30;
```

---

## Operators & Expressions

### Arithmetic
- `+` (Addition), `-` (Subtraction)
- `*` (Multiplication), `/` (Division), `%` (Modulo)

### Assignment & Compound Assignment
- `=` (Assignment)
- `+=`, `-=`, `*=`, `/=`, `%=` (Compound Assignment)

### Increment / Decrement
Both prefix and postfix operators are supported:
- `++` (Increment)
- `--` (Decrement)

### Relational
- `==` (Equal), `!=` (Not equal)
- `<` (Less than), `>` (Greater than)
- `<=` (Less than or equal), `>=` (Greater than or equal)

### Logical
- `&&` (Logical AND)
- `||` (Logical OR)

---

## Control Flow

### If / Elseif / Else
Turf-Lang supports both an expression-based single-line `if...then...else` and a block-based `if...elseif...else`.

**Expression Form:**
```c
if x > 0 then x = 1 else x = 0;
```

**Block Form:**
```c
if x > 0 {
    printline("Positive");
} elseif x < 0 {
    printline("Negative");
} else {
    printline("Zero");
}
```

### While Loop
```c
while x < 10 {
    x++;
}
```

### For Loop
Turf-Lang uses a range-based `for` loop with a customizable step expression.
```c
for i in 0 .. 10 step 1 {
    printline(i);
}
```

### Break and Continue
Control loop execution using `break` and `continue`.
```c
while true {
    if should_skip {
        continue;
    }
    if should_stop {
        break;
    }
}
```

---

## Functions

Functions are defined using the `fn` keyword, followed by the return type, function name, typed parameters, and a block body.

```c
fn int add(int a, int b) {
    return a + b;
}

fn void greet() {
    printline("Hello!");
    return;
}
```

### Intrinsic Decorator
To bind a function to an LLVM intrinsic or built-in, use the `@intrinsic("name")` decorator before the `fn` keyword.
```c
@intrinsic("print") fn void my_print(int val);
```

---

## Error Handling

Turf-Lang supports structured exception handling.

### Raise
Throw an exception payload using the `raise` keyword.
```c
raise "An error occurred";
```

### Try / OnError
Catch exceptions using `try { ... } onError (err) { ... }`.
```c
try {
    // Potentially failing code
    raise 404;
} onError (err) {
    print("Caught error code: ");
    printline(err);
}
```

---

## Special Expressions

### Type Casting
Convert values between compatible types using function-style casting:
```c
int(3.14)      // casts double to int (3)
double(42)     // casts int to double (42.0)
string(1)      // casts bool to string
```

### Sizeof
Get the size of a specific type (useful for memory allocation).
```c
int ptr_size = sizeof(int*);
```

### Length property
Retrieve the number of elements in an array or list using `.length`.
```c
int count = arr.length;
```

---

## Standard Library (Built-ins)

Turf-Lang comes with several built-in functions available globally:

### I/O
- `print(expr)`: Prints the evaluated expression to standard output.
- `printline(expr)`: Prints the evaluated expression followed by a newline.
- `input()`: Reads a line of text from standard input and returns it as a `string`.

### Math
- `power(a, b)`: Returns `a` raised to the power of `b`. Handles both `int` and `double`.
- `abs(val)`: Returns the absolute value of `val`.

### Memory Management
*(Requires using `int*` or other pointer types)*
- `malloc(size)`: Allocates `size` bytes of memory and returns a pointer.
- `realloc(ptr, size)`: Reallocates `ptr` to a new `size`.
- `free(ptr)`: Frees the previously allocated memory.

### Utilities
- `lengthof(str)`: Returns the string length of `str`. *(Note: For arrays and lists, use the `.length` property instead).*
- `typeof(expr)`: Returns a `string` representing the static type of the evaluated expression (e.g., `"int"`, `"double"`, `"bool"`, `"string"`).
