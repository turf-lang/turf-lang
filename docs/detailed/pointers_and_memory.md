# Pointers and Memory Management

Turf-Lang provides explicit pointers and manual memory management capabilities, allowing developers to write high-performance systems code or handle complex reference-based data structures.

---

## Pointer Types

Pointers are denoted by appending an asterisk (`*`) to any type.

```turf
int* ptr;
double* d_ptr;
void* raw_memory;
```

*Note: String literals natively decay into `string` types, which act as underlying byte pointers.*

---

## Memory Allocation

Turf-Lang provides standard library intrinsics for heap memory management. These must be imported (e.g. from `mem.tr` if available in the standard library, or they are just globally available as `@intrinsic` functions).

### `malloc(size)`

Allocates a block of memory of the specified `size` (in bytes) and returns a `void*` pointer.

```turf
import mem
// Allocates 8 bytes of memory
void* ptr = malloc(8);
```

### `sizeof(type)`

Because `malloc` requires a byte size, Turf-Lang provides a native `sizeof` operator to dynamically determine the size of a type.

```turf
import mem

// Allocate exactly enough memory for one integer
int size = sizeof(int);
void* ptr = malloc(size);
```

### `realloc(ptr, size)`

Resizes a previously allocated memory block to a new `size`. 

```turf
// Double the allocation
ptr = realloc(ptr, sizeof(int) * 2);
```

### `free(ptr)`

Releases memory allocated by `malloc` or `realloc` back to the operating system. Failing to call `free` will result in a memory leak.

```turf
free(ptr);
```

---

## Type Casting

Often, when dealing with `void*` from `malloc`, you will need to cast it back to a usable pointer type using Turf-Lang's function-style casting.

*(Note: Type-casting pointers is an advanced feature. For now, rely on structural boundaries or explicitly defined struct pointers).*

## Frequently Asked Questions (FAQs)

**Q: Is Turf-Lang garbage collected?**
A: No. Turf-Lang uses manual memory management via `malloc` and `free` for native pointers, similar to C. However, generic structures like `list<T>` handle their own underlying buffers.

**Q: Can I use pointer arithmetic?**
A: Pointer arithmetic is currently abstracted. Arrays handle standard offset math via indexing (`ptr[index]`).

**Q: What happens if I `free` a pointer twice?**
A: Like C, this leads to undefined behavior (a double-free error) at the system level. Always ensure memory is managed properly.
