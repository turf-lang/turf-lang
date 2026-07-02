# Turflang Phase 3 RFC: Modern Error Handling (Errors as Values)

## The Problem
Turflang originally relied on a `try/raise` exception model (similar to Python or Java). However, exception-based control flow is often considered an anti-pattern in modern systems programming because:
1. **Hidden Control Flow**: Exceptions jump up the call stack invisibly, making code hard to reason about.
2. **Performance Overhead**: Stack unwinding during an exception is slow and complex.
3. **Type Safety Holes**: A function's signature does not definitively state whether it might throw an error, forcing developers to guess or read documentation.

For Phase 2, Issue #19 removes the `try/raise` syntax entirely. For the immediate future, Turflang will use `assert()` and `fail()` (which compile to `abort()`) for unrecoverable errors. 

However, for *recoverable* errors (like "File not found" or "Network disconnected"), we need a modern, type-safe approach.

## Proposed Solution: Error Unions (Zig Style)

We propose introducing **Error Unions** inspired by Zig. Instead of throwing an error, a function that can fail will return a Union type containing either a success value OR an error value.

### Syntax Design

An error union type is denoted by the `!` operator preceding a standard type.
```turflang
// This function returns an `int` if successful, or an `Error` if it fails.
fn divide(a: int, b: int): !int {
    if a == 0 {
        return error.DivisionByZero;
    }
    return a / b;
}
```

### Handling the Error

The Type Checker will explicitly forbid you from treating a `!int` as a normal `int`. You must "unwrap" it using one of three methods:

**1. The `catch` operator** (Fallback)
If the expression returns an error, evaluate the right-hand side instead.
```turflang
mutable x: int = divide(10, 0) catch 0;
```

**2. The `?` operator** (Propagation)
If the expression returns an error, immediately return that error to the caller function.
```turflang
fn do_math(): !int {
    mutable x: int = divide(10, 2)?; // If divide fails, do_math immediately returns the error
    return x * 10;
}
```

**3. Pattern Matching** (Explicit handling)
If you need to perform logic based on the error.
```turflang
match divide(10, 0) {
    Ok(val) => { print(val); }
    Err(e)  => { print("Error occurred: ", e); }
}
```

## Implementation Roadmap (Phase 3)
1. **Parser Changes**: Allow `!` in front of types (e.g. `!int`). Add `catch`, `?`, and `error.XYZ` keywords.
2. **Type Checker**: Create a new `UnionType` or `ErrorUnionType` subclass. Enforce unwrapping before use.
3. **Codegen**: When compiling a `!T`, LLVM should generate a struct `{ i8 tag, T value_or_error }`.
4. **Standard Library**: Update modules (like `io.tr`) to return `!String` instead of throwing exceptions.
