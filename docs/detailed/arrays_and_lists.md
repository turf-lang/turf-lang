# Arrays and Lists

Turf-Lang offers two primary ways to store collections of data: **Fixed-size Arrays** and **Generic Lists**. 

---

## Arrays

Arrays in Turf-Lang have a fixed compile-time size and provide highly efficient memory storage directly on the stack (via LLVM allocas).

### Declaration and Initialization

You can declare an array with a specific size and initialize it using an array literal block `[...]`.

```turf
int[3] arr = [1, 2, 3];
string[2] names = ["Alice", "Bob"];
```

### Empty Array Declarations

Unlike primitive variables, arrays **can** be declared empty (without an initializer block). 

```turf
int[5] numbers;
bool[10] flags;
```

**What are the values of an empty declared array?**
When you declare an array without an initializer, Turf-Lang automatically zero-initializes the entire block of memory. 
- For `int` and `double` arrays, all elements are set to `0` or `0.0`.
- For `bool` arrays, all elements are set to `false`.
- For pointers/strings, they are initialized to `null`.

### Array Operations

- **Access and Assignment:** Use standard zero-based indexing `arr[index]`.
- **Length:** Use the `.length` property to get the size of the array at runtime.

```turf
int[5] numbers;
numbers[0] = 42;
int size = numbers.length; // size == 5
```

> [!WARNING]
> Array bounds are strictly checked. Accessing an index outside `0` to `size - 1` will result in an `OutOfBoundsException`.

---

## Generic Lists

When you need a dynamically sized collection, use a Generic List (`list<T>`). Lists manage their own memory on the heap and can grow or shrink.

### Declaration

Lists can be declared without an initializer. They start empty.

```turf
list<int> my_list;
list<string> words;
```

### List Methods

Turf-Lang natively supports the following methods on `list<T>` objects:

- **`.push(value)`**: Appends a new element to the end of the list.
- **`.pop()`**: Removes and returns the last element from the list.
- **`.length`**: Property that returns the current number of elements in the list.

```turf
list<int> queue;
queue.push(10);
queue.push(20);

int len = queue.length; // len == 2
int last_val = queue.pop(); // last_val == 20
```

### Index Access

Just like arrays, you can access and assign elements in a list using index brackets `[ ]`.

```turf
queue[0] = 15;
printline(queue[0]);
```

---

## Frequently Asked Questions (FAQs)

**Q: Can arrays be empty declared, and what will be the values?**
A: Yes. `int[5] arr;` is completely valid. Every element is guaranteed to be zero-initialized (`0` for numbers, `false` for booleans).

**Q: Are multi-dimensional arrays supported?**
A: Yes! You can chain brackets for multi-dimensional arrays, e.g., `int[5][10] matrix;`.

**Q: What happens if I try to access `arr[10]` on an `int[5]` array?**
A: Turf-Lang enforces bounds checking. If the index is a constant, you will get a compile-time error. If it is computed at runtime, it will raise an `OutOfBoundsException` when executed.
