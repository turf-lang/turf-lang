# Variables and Scoping

This document details the rules for variable declarations, initialization requirements, and scoping rules within Turf-Lang.

## Variable Declaration

Variables in Turf-Lang are statically typed and must be explicitly declared. Primitive variables (like `int`, `double`, `bool`, and `string`) **must be initialized at the time of declaration**.

```turf
// Correct Declarations
int x = 10;
double pi = 3.1415;
bool flag = true;
string greeting = "Hello, Turf!";
```

If you attempt to declare a variable without an initialization value, the compiler will raise a syntax error.

```turf
// INCORRECT
int y; // Error: Expected '=' after variable name 'y'. Variables must be initialised.
```

### Exception: Generic Lists

The only standard exception to the immediate-initialization rule is generic lists (`list<T>`). When a list is declared without an initializer, the compiler safely handles the memory allocation underneath.

```turf
list<int> numbers; // Valid! Automatically initialized as an empty list.
```

---

## Scoping Rules

Turf-Lang uses standard **lexical block scoping**. Scopes are created by blocks (`{ ... }`), such as function bodies, `if` statements, and loops.

### Visibility

A variable is visible from the point of its declaration until the end of the block in which it was declared.

```turf
fn int test() {
    int outer = 5;
    if outer > 0 {
        int inner = 10;
        printline(inner); // Valid
    }
    // printline(inner); // Error: 'inner' is out of scope here
    return 0;
}
```

### Shadowing

Turf-Lang allows variable shadowing. You can declare a variable in an inner scope with the same name as a variable in an outer scope. However, the built-in **Linter will emit a warning** when you do this, helping prevent unintended logic errors.

```turf
int x = 5;
if true {
    int x = 10; // Valid, but will trigger a compiler shadowing warning
    printline(x); // Prints 10
}
printline(x); // Prints 5
```

---

## Frequently Asked Questions (FAQs)

**Q: Can variables be empty declared? What will be the value?**
A: No, primitive variables (`int`, `double`, `bool`, `string`) **cannot** be empty declared. Turf-Lang enforces explicit initialization to prevent uninitialized memory bugs. If you try, the compiler will error out. The only exception is `list<T>` which handles its own initialization to an empty state.

**Q: Can I declare multiple variables on one line?**
A: No. Turf-Lang requires each variable declaration to have its own type keyword and initializer statement (e.g., `int x = 5; int y = 10;`).

**Q: Are there global variables?**
A: Currently, all executable code and variable declarations must reside inside a function (such as `main`). Top-level variables outside of functions are strictly disallowed and will throw an error.
