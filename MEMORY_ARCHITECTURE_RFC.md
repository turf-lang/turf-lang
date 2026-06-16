# Turflang Phase 4 RFC: Memory Architecture & Garbage Collection

This document outlines the current state of Turflang's memory management and proposes the architectural design for Phase 4, transitioning from unsafe manual memory management to a safe, modern, compiler-managed system.

---

## 1. Current State (Phase 1 & 2)

Turflang currently operates as a low-level, unsafe systems language:
- **`malloc`, `realloc`, and `free`**: These are compiler intrinsics that generate LLVM calls directly to the C Standard Library (`libc`). Memory is entirely manually managed. If the user forgets to call `free`, memory leaks.
- **Pointers**: The language exposes raw `i8*` pointers.
- **Strings**: String literals (`"hello"`) compile to `CreateGlobalStringPtr`, meaning they are raw C-strings pointing to the read-only `.rodata` section of the binary. They cannot be mutated, resized, or easily concatenated.
- **Primitive Arrays**: Arrays like `int[5]` compile to LLVM `AllocaInst`, allocating them purely on the Call Stack. Attempting to return an array from a function results in a dangerous "dangling pointer" because the stack frame is destroyed upon return.

---

## 2. Proposed Future Architecture (Phase 4)

In Phase 4, Turflang will abstract manual memory management away from the user, prioritizing safety and developer experience while maintaining high performance.

### A. Automatic Reference Counting (ARC) Garbage Collection
Instead of a heavy background tracing garbage collector (like Java or Go), Turflang will implement **ARC** (like Swift or Rust's `Rc`):
1. Every heap-allocated object (Strings, Arrays, Structs) contains a hidden `ref_count` integer.
2. The compiler automatically injects LLVM instructions to increment the count when the object is assigned or passed to a function.
3. When variables go out of scope, the compiler injects instructions to decrement the count.
4. When `ref_count == 0`, the compiler injects a `free()` call.
5. **Result:** The user never writes `malloc` or `free`, and there is no heavy runtime environment slowing down the program.

### B. Safe References over Raw Pointers
Raw pointers (`*int`) will be deprecated from the safe, user-facing language. They will be replaced by **Safe References** (`&int`), which the compiler guarantees point to valid memory. 

### C. Heap-Allocated Dynamic Strings and Arrays
Primitive stack-arrays and `.rodata` C-strings will be replaced by dynamic, heap-allocated primitives managed by ARC.
- **Strings** will become built-in structs containing `{ i8* data, int length, int capacity, int ref_count }`. They will be fully mutable and dynamically resizable.
- **Arrays** will become heap-allocated "Slices", allowing them to be safely returned from functions without dangling pointers.

---

## 3. Foreign Function Interface (FFI) Interoperability

A critical requirement of hiding raw pointers is maintaining the ability to talk to C libraries and the Operating System. FFI will be handled via an explicit `unsafe` bridge.

1. **The `unsafe_ptr<T>` type:** Turf will retain raw pointers, but they will be strictly isolated.
2. **C Function Declarations:** External C functions will be declared using an `@extern` keyword and can accept `unsafe_ptr`.
   ```turflang
   @extern fn write(fd: int, buf: unsafe_ptr<i8>, count: int): int;
   ```
3. **Safe-to-Unsafe Bridging:** Safe objects will provide compiler intrinsics to temporarily expose their raw memory addresses to C. During this call, ARC is instructed not to free the memory.
   ```turflang
   mutable my_str: string = "hello";
   write(1, my_str.get_raw_pointer(), my_str.length());
   ```

---

## 4. Integration into the Roadmap

This overhaul is highly disruptive and depends heavily on a robust type system. It should be scheduled for **Phase 4**:

- **Phase 2 (Current):** Syntax Overhaul (Functions, Structs, Variables). Memory remains manual.
- **Phase 3:** Type System & Generics (Implementation of Error Unions `!T`, strict type checking).
- **👉 Phase 4:** Memory & Standard Library (Implementation of this RFC. ARC is built, Arrays/Strings move to the Heap, `io.tr` is rewritten to use safe memory).
- **Phase 5:** Optimization & 1.0.0 Release.
