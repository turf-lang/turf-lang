# Turf-Lang Syntax Reference

This reference is generated directly by validating the parsing rules, lexer tokens, and built-in function bindings found in the compiler source code (`Parser.cpp`, `Lexer.cpp`, `AST.h`, `Builtins.cpp`).

## Table of Contents
1. [Types](#types)
2. [Literals](#literals)
3. [Variables](#variables)
4. [Arrays](#arrays)
5. [Operators & Expressions](#operators--expressions)
6. [Type Casting](#type-casting)
7. [Control Flow](#control-flow)
8. [Functions](#functions)
9. [Built-in Functions](#built-in-functions)

---

## Types
Turf-Lang uses strong static typing. The supported types are:
- `int` (64-bit integer)
- `double` (64-bit float). The `float` keyword is also accepted as an alias.
- `bool` (boolean)
- `string` (string / byte pointer)
- `void` (Allowed **only** as a function return type)

## Literals
- **Integers**: `42`, `-10`
- **Doubles**: `3.14`, `-0.001`
- **Booleans**: `true`, `false`
- **Strings**: `"Hello, World\n"` (Supports escape sequences: `\n`, `\t`, `\\`, `\"`)

## Variables
Variables must be declared with their static type and require an initialization value at the time of declaration.

```c
// Correct Declarations
int x = 5;
double pi = 3.14;
bool is_active = true;
string greeting = "hello";
```

### Assignments
Variables can be mutated using standard assignment operators:
```c
x = 10;
x += 5;
x -= 2;
x *= 3;
x /= 2;
x %= 4;

// Increment / Decrement
x++;
x--;
++x;
--x;
```

## Arrays
Arrays in Turf-Lang have a fixed compile-time size and can be declared with or without an initializer list.

### Declaration
```c
int[5] numbers;                     // Uninitialized array of size 5
int[3] primes = [2, 3, 5];          // Initialized array of size 3
```

### Access and Mutation
Array elements are accessed using zero-based indexing.
```c
numbers[0] = 42;                    // Standard assignment
int first = primes[0];              // Accessing element
```
*Note: Compound assignments (`arr[0] += 5`) are explicitly unsupported on array elements. You must use `arr[0] = arr[0] + 5`.*

### Array Properties
You can get the length of an array using the `.length` property.
```c
int l = primes.length;              // Evaluates to 3
```

## Operators & Expressions

### Arithmetic
`+` (Add), `-` (Subtract), `*` (Multiply), `/` (Divide), `%` (Modulo), `^` (Exponentiation)
*(Unary `-` is also supported for negation).*

### Relational
`==` (Equal), `!=` (Not equal)
`<` (Less than), `>` (Greater than)
`<=` (Less than or equal), `>=` (Greater than or equal)

### Logical
`&&` (Logical AND), `||` (Logical OR)
*Note: Logical NOT (`!`) is reserved in the lexer but not fully implemented as a unary operator in the parser.*

## Type Casting
Explicit, function-style type casting is supported between base types:
```c
int(3.14)       // Casts double to int
double(42)      // Casts int to double
string(1)       // Casts bool/int to string
```

## Control Flow

### If / Else
Turf-Lang supports two forms of `if` statements.

**Form 1: Expression / Single-line**
```c
if x > 0 then x = 1 else x = 0;
```

**Form 2: Block Form (with elseif)**
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
A range-based for loop where `start`, `end`, and `step` can be *any expression*.
```c
int step_size = 2;
for i in 0 .. (5 * 10) step (step_size + 1) {
    printline(i);
}
```

### Break and Continue
Used inside loop bodies to control execution flow.
```c
while true {
    if condition1 {
        continue;
    }
    if condition2 {
        break;
    }
}
```

## Functions
Functions are declared using the `fn` keyword, followed by the return type, name, explicitly typed parameters, and a block body.

```c
fn int add(int a, int b) {
    return a + b;
}

fn void do_nothing() {
    return;
}
```

## Built-in Functions
Functions inherently registered in the language's global namespace:

- **`print(expr)`**: Prints a single expression to standard output.
- **`printline(expr)`**: Prints a single expression followed by a newline.
- **`input()`**: Reads input from stdin and returns it as a `string`.
- **`lengthof(string_ptr)`**: Evaluates the character length of a string. Returns an `int`. *(Note: This is strictly for strings; arrays use `.length`)*.
- **`typeof(expr)`**: Returns a `string` representing the static type of the expression (e.g., `"int"`, `"double"`, `"bool"`).
