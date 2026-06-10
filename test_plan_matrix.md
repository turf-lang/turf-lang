# Turflang Comprehensive Test Matrix

This matrix provides a structured breakdown of every feature, phase, and edge case that needs to be covered to ensure a robust and exhaustive test suite for Turflang.

## 1. Lexer & Parser (Syntax)
*Goal: Ensure the language grammar is strictly adhered to and syntax errors are caught gracefully.*
- **Basic Tokens:** Integers, floats, booleans (`true`/`false`), strings, identifiers.
- **Keywords:** Rejecting keywords used as variable/function names.
- **Comments:** Single-line `//` and multi-line `/* */` comments (including nested/unclosed comments).
- **Operators:** Precedence and associativity (e.g., `a + b * c` vs `(a + b) * c`).
- **Statements vs Expressions:** Missing semicolons, trailing semicolons, empty blocks `{}`.
- **Edge Cases:**
  - Extra whitespace/tabs/newlines anywhere.
  - Extremely deeply nested parentheses or blocks (stack overflow prevention).
  - Malformed strings (missing closing quote, unknown escape sequences).
  - Unterminated arrays or struct initializations.

## 2. Resolver (Name Resolution & Scoping)
*Goal: Ensure the compiler connects identifiers to the correct declarations and handles scoping correctly.*
- **Scope Hierarchy:** Global scope vs Function scope vs Block scope (`if`/`while`).
- **Variable Shadowing:** Inner blocks correctly shadowing outer block variables.
- **Duplicate Declarations:** Catching multiple variables with the same name in the *same* scope.
- **Undeclared Variables:** Using a variable or function before it's defined.
- **Forward References:** Main function calling a helper function defined further down the file.
- **Early Exits:** Dead code detection after `return`, `break`, or `continue`.
- **Edge Cases:**
  - Calling a variable as if it were a function (first-class function prevention).
  - Shadowing a module name with a local variable.
  - Complex shadowing (Global -> Function -> If Block -> While Block).

## 3. TypeChecker (Type System & Safety)
*Goal: Ensure type safety, valid assignments, and correct overload resolution.*
- **Type Compatibility:** Assigning `int` to `int`, preventing `string` to `int` without cast.
- **Binary Operators:** `int + int`, `double * double`, `bool == bool`.
- **Unary Operators:** `-int`, `-double`, `!bool`.
- **Function Arity:** Calling functions with too few, too many, or correct number of arguments.
- **Overload Resolution:** Correctly picking `add(int, int)` vs `add(double, double)`.
- **Return Types:**
  - `return <val>` in a `void` function.
  - `return;` in a non-`void` function.
  - Missing return statement in non-`void` functions.
- **Void Values:** Preventing `int x = voidFunc()`, `voidFunc1() + voidFunc2()`.
- **Edge Cases:**
  - Implicit upcasting (`int` -> `double`) in expressions and function arguments.
  - Overload ambiguity (if multiple signatures match an implicit cast).
  - Array elements of `void` type.

## 4. Control Flow (Codegen & Logic)
*Goal: Ensure loops and branches execute the correct paths.*
- **If/Else:** `if` alone, `if/else`, chained `if/else if/else`. Condition must be `bool`.
- **While Loops:** Basic loop execution, condition updates.
- **For Loops:** Initialization, condition checking, incrementing.
- **Break & Continue:** 
  - Valid usage inside `while`/`for`.
  - Catching `break`/`continue` used outside of loops.
  - Nested loops (break only exits the innermost loop).
- **Edge Cases:**
  - `if (true)` with an empty block.
  - Infinite loops that are `break`'d out of conditionally.

## 5. Arrays & Pointers
*Goal: Validate memory structures, bounds checking, and pointer arithmetic.*
- **Array Declarations:** `int[5] arr;`, array initialization lists `[1, 2, 3]`.
- **Bounds Checking:** Out-of-bounds access on local arrays (both compile-time constants and runtime variables).
- **Array Length:** The `.length` property on arrays.
- **Pointers:** Taking address (`@var`), dereferencing, passing pointers to functions.
- **Edge Cases:**
  - Negative array indices.
  - Indexing with a non-integer (e.g., `arr["hello"]` or `arr[1.5]`).
  - Size mismatches in initialization lists (e.g., `int[2] arr = [1, 2, 3];`).

## 6. Structs & Custom Types
*Goal: Validate composite data structures and their instantiations.*
- **Declaration:** Defining structs with various member types.
- **Initialization:** Default initialization vs member-wise initialization.
- **Field Access:** `obj.field`, nested field access `obj.innerObj.field`.
- **Methods:** Calling custom constructors (e.g., `Obj.init()`).
- **Edge Cases:**
  - Accessing non-existent fields.
  - Passing structs by value vs pointer to functions.
  - Type mismatch during member-wise initialization.

## 7. Modules & Standard Library (Prelude)
*Goal: Ensure the prelude imports correctly and module methods behave.*
- **Imports:** `import io;`, `import math;`, `import string;`.
- **Module Method Calls:** `io.printline()`, `math.pow()`.
- **String Handling:** `string.concat()`, `string.length()`, `string.substr()`.
- **Edge Cases:**
  - Calling a method on an unimported module.
  - Module method names that conflict with local variables.
  - Passing `null` to string library functions.

## 8. End-to-End & Integration (The "Real World" Tests)
*Goal: Complex combinations of features to ensure the passes don't trip over each other.*
- **Fibonacci Sequence:** Recursive and Iterative implementations.
- **Sorting Algorithms:** Bubble sort or Quick sort using arrays, loops, and conditional logic.
- **Linked Lists:** Structs containing pointers to themselves.
- **Math Heaviness:** Complex BEDMAS expressions with deep nesting and mixed types (`int` + `double`).
- **Shadowing Extravaganza:** A test purely dedicated to reusing the same variable name in 10 different nested scopes and functions.
