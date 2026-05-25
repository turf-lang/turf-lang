# Intrinsics

Turf-Lang allows developers to bridge directly into native LLVM functions or external C standard library functions without writing complex wrappers. This is achieved using the `@intrinsic` decorator.

---

## The `@intrinsic` Decorator

When defining a function, placing `@intrinsic("internal_name")` before the `fn` keyword tells the compiler:
*"Do not compile a body for this function. Instead, map calls to this function directly to the LLVM or C function named `internal_name`."*

### Syntax

```turf
@intrinsic("llvm_or_c_name")
fn ReturnType local_name(Type arg1, Type arg2)
```

Notice that there are no braces `{}`. The function is effectively a forward-declaration linking to a native binary.

---

## Examples

### Binding C Functions

Many of the standard I/O functions in Turf-Lang's standard library are implemented by binding to C functions.

```turf
// mem.tr essentially does this to bind to C's malloc and free
@intrinsic("malloc")
fn void* malloc(int size)

@intrinsic("free")
fn void free(void* ptr)
```

### Binding LLVM Intrinsics

LLVM has highly optimized, architecture-specific intrinsic functions for math. You can bind directly to them by using their internal LLVM identifier.

```turf
// math.tr
@intrinsic("llvm.sin.f64")
fn double sin(double val)

@intrinsic("llvm.sqrt.f64")
fn double sqrt(double val)
```

---

## How It Works

During Code Generation (`Codegen.cpp`), when the compiler sees a function call, it checks if the function was declared with an intrinsic string. 
- If the string starts with `llvm.`, it generates a native LLVM intrinsic call node.
- Otherwise, it assumes it is a standard external C-linkage function (like `printf`, `malloc`, `strlen`) and emits an external call. The system linker (`cc` or `gcc`) will resolve these when generating the final executable.

## Frequently Asked Questions (FAQs)

**Q: Can I use `@intrinsic` in my own code, or is it just for `lib/std/`?**
A: Yes, you can use `@intrinsic` anywhere in your code to link against external C functions. Just ensure the linked C library is available when the compiler invokes the system linker.

**Q: What if the intrinsic name doesn't exist?**
A: The LLVM compilation phase will succeed (as it assumes external linkage), but the final linking step will fail with an "undefined reference" error from your system's linker (e.g., `cc` or `gcc`).
