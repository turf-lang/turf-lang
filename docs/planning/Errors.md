For our langauge, User-defined errors are: something which they describe themselves.

It is described with a new keyword: `error`.

A basic syntax of an `error` type is:

```turf
error IOError {
    FileNotFound(path: string),
    PermissionDenied(path: string),
    InvalidEncoding(path: string, line: int)
}

error NetworkError {
    Timeout(seconds: int),
    ConnectionRefused(host: string, port: int)
}

error MathError {
    DivisionByZero,
    Overflow,
    InvalidInput(message: string),
}
```

This means that the user can define their own errors just like they define their own types. They are allowed to form errors with parameters of however they like, checked by the compiler later on. That means, it is not necessary to put in parameters itself. This would allow stuff like (taking formed error types from above):

```turf
NetworkError::Timeout(30); // This is allowed
MathError::InvalidInput(33.33); // This is not allowed and will be rejected in the typecheck phase just like every other typecheck
```

Just how `StructType` and `EnumType` are present in the language, the above three error types, starting with the `error` keyword would then become `ErrorType`, and will be validated in the pipeline and form a node in the AST just like anything else.

Now, since errors are being introduced, the following syntax:

```turf
fn divide(a: int, b: int): !int {
    if (b == 0) {
        return MathError::DivisionByZero;
    }

    return a / b;
}
```

The above syntax will be allowed. That means, `!T` is allowed where T can be anything, a primitive type, `List<T>`, `Map<K, V>` from the standard library or even any user-defined datatype, say `User` or `Animal`. This would help in the scaling of the language itself, as well as a better standard library!

Thus, now, in the Analysis phase, any function returning `T` but not returning something of the `ErrorType` in any of its paths will be warned. That means, they are not obliged to return a node of the `ErrorType` but they may or may not return a node of the error type. They will get a helpful diagnostic warning after it is validated by the Analysis phase and is found that the function has not returned a node of `ErrorType`. (`error` keyword forms a node of the `ErrorType`)

Now, introduction of a new keyword: `recover`. This keyword is situated where the function is returning either an `ErrorType` or an object of type `T`.

That means, if someone were to use the function `divide`, they would need to recover from the error. If they don't recover from the error themselves, they will get an error, because `divide(10, 0)` will not be of the type `int`. Anything of the type `!T` will need `recover` on the call site to handle the failure. So basically `recover` is what makes the code to proceed to the next step from here. 

The below three are the ways in which we can use `recover`:

1. `mut x: !int = divide(10, 2) recover produce 0;`
2. ```turf
    mut x: !int = divide(10, 2) recover run {
        io::printline("Division failed");
        return;
    };
    ```
3. ```turf
    mut x: !int = divide(10, 0) recover as err run {
        if err == MathError::DivisionByZero {
            io::printline("Division by zero");
        }
    };```

This means: we have 3 variants of recover. They do the following things:

1. `recover produce value`: If the function is successful, the value of the function is given, and if the value of the function is an error, it will produce the value of `value`. 
2. `recover run { ... }`: This would just run the code inside the curly brackets if the function is unsuccessful, this is extremely useful for logging and debugging purposes, and even running side-effects. The previous one and the current one is useful if we do not care about the side effect name.
3. `recover as err run { ... }`: This would just run the code inside the curly brackets if the function is unsuccessful, this is extremely useful when you want to run specific blocks of code or side effects for a specific type of error.

To reduce the hassle of always filling in the `error` type, we have the following also allowed:

`error GeneralError;` -> Here, `GeneralError` is not a keyword but an error type defined by the user themselves. Since we allowed them to sometimes forget the exact error types, such as in the case above where we just use `recover produce value` or `recover run {}`, they do not necessarily need to have error names inside the blocks. However, this would prevent them from doing `recover as err run { ... }`, and will be given a diagnostic error during compilation phase.

That means, they can do `return GeneralError::DEFAULT;` and the error would be caught by the appropriate `recover` circuit later when the function is called. This `DEFAULT` is attached to every error with no subtypes inside.

On `!T`, we expand now. When `T` is `void` in this case, we would want them to not return anything from the block and give an appropriate error like we do with functions of the return type void, but returning any value.

Thus, if a function signature is like this:

```turf
fn writeFile(...): !void {
    ...
}
```

The following is valid:

```turf
writeFile("a.txt") recover run {
    io::printline("Write failed.");
};
```

```turf
writeFile("a.txt") recover run {
   sys::exit(1);
};
```

But the below is invalid:

```turf
writeFile("a.txt") recover produce 42;
```

Thus an extremely simple rule to understand from the document: "A recover block must evaluate to the function's success type."

If the success type is:

1. `int` → recover must produce an `int`.
2. `string` → recover must produce a `string`.
3. `void` → recover produces nothing (`void`).

The type checker naturally enforces it.

Again, an explicit forbidden code:

```turf
fn foo(): int {
    return divide(10,0);
}
```

This is because the return types do not match, and the TypeChecker should catch it. Good if tested.

Anyway, you need to test extensively always!