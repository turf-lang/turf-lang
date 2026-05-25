# Structs

Turf-Lang supports user-defined data structures via the `struct` keyword, allowing you to encapsulate related variables together into a single, cohesive type.

---

## Declaration

Structs must be declared at the top-level of the file. A struct definition contains a name and a block of fields, where each field consists of a type and a name.

```turf
struct Point {
    int x
    int y
}

struct User {
    string username
    bool is_active
}
```

*Note: You do not need to use commas or semicolons to separate fields inside the struct block.*

---

## Instantiation

To create an instance of a struct, you call the struct name as if it were a function, passing in arguments for each field in the exact order they were declared.

```turf
fn int main() {
    Point p = Point(10, 20);
    User u = User("Alice", true);
    
    return 0;
}
```

---

## Field Access and Modification

You can access or modify the fields of an instantiated struct using the dot (`.`) operator.

```turf
fn int main() {
    Point p = Point(0, 0);
    
    // Modify fields
    p.x = 100;
    p.y = 50;
    
    // Read fields
    printline(p.x); // Prints 100
    
    return 0;
}
```

---

## Methods

While Turf-Lang currently focuses on data-structs, methods can be simulated or directly attached via compiler features depending on the module. Currently, methods like `.length`, `.push()`, and `.pop()` are natively integrated into the compiler for built-in structs like `list`. For user-defined structs, development on native Object-Oriented methods is ongoing.

---

## Frequently Asked Questions (FAQs)

**Q: Can a struct contain another struct as a field?**
A: Yes, structs can be nested, as long as the nested struct has been declared earlier in the compilation unit.

**Q: Can I have a pointer to a struct?**
A: Yes. If you have `struct Node`, you can declare `Node* next;`. This is essential for building linked lists or trees.

**Q: Do I have to initialize all fields when creating a struct?**
A: Yes. The instantiation syntax `StructName(arg1, arg2...)` requires exactly the same number of arguments as there are fields, and their types must match exactly.
