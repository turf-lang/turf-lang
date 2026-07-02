# Turflang Future Architecture: Generics & Operator Overloading

This roadmap details how to fully implement Frontend Monomorphization (for generic types like `list<T>`) and Operator Overloading (e.g., `Matrix + Matrix`) by leveraging the new multi-pass compiler architecture.

---

## Phase 1: Frontend Monomorphization (Generics)
*Goal: Allow users to write generic functions/structs once, and have the compiler automatically clone and type-check them for every utilized type.*

### Current State
Currently, `Codegen.cpp` supports creating unique LLVM struct types via name mangling (e.g., `list_int`). However, the AST relies on users manually defining or importing explicitly mangled implementations.

### Step 1: Generic Type Definitions in the AST
1. Update the Parser to accept `fn myfunc<T>(T val)` and `struct list<T>`.
2. Introduce a new AST node wrapper: `GenericTemplateAST`. This node does **not** get resolved or type-checked on its own. It acts as a factory/blueprint.
3. The `SymbolTable` stores these blueprints under their base name (e.g., `list`).

### Step 2: The Monomorphizer Sub-Pass (Inside the Resolver)
When the `Resolver` encounters a usage of a generic type or function (e.g., `list<int> myList;`):
1. **Lookup Blueprint:** It finds the `GenericTemplateAST` for `list`.
2. **Clone & Substitute:** It deeply clones the blueprint AST. During the clone, it replaces every instance of the generic type token `T` with the concrete type `int`.
3. **Mangle:** It assigns the cloned AST a mangled name: `list_int`.
4. **Register:** It inserts the newly cloned `list_int` AST into the `TopLevelASTs` list and registers `list_int` in the `SymbolTable`.

### Step 3: TypeCheck & Codegen
Because the Monomorphizer runs during the Resolution phase, the `TypeChecker` and `Codegen` phases don't even know generics exist! They simply see a fully concrete struct called `list_int` and compile it normally. 

---

## Phase 2: Operator Overloading
*Goal: Allow structs to define magic methods that intercept and overload standard mathematical operators.*

### Current Architecture Advantage
Because `Codegen` is decoupled from semantics, operator overloading requires **zero** changes to LLVM IR generation. It is entirely an AST transformation handled by the `TypeChecker`.

### Step 1: Define Magic Methods
Decide on a naming convention for operators. Python's double-underscore convention is highly standard and prevents accidental naming collisions:
- `+`  -> `__add__(self, other)`
- `-`  -> `__sub__(self, other)`
- `*`  -> `__mul__(self, other)`
- `==` -> `__eq__(self, other)`
- `[]` -> `__index__(self, index)`

### Step 2: AST Mutation in the TypeChecker
Modify `TypeChecker::check(BinaryExprAST* AST)`:
1. Identify the types of the Left and Right operands.
2. **Primitive Check:** If both are `int` or `double`, proceed as normal (validate compatibility).
3. **Struct Overload Check:** If the Left operand is a Struct type (e.g., `Matrix`):
   - Query the `SymbolTable` for a method named `__add__` on the `Matrix` struct.
   - If the method does not exist, throw a `TypeError: Struct 'Matrix' does not overload operator '+'`.
   - If the method *does* exist, validate its arity and parameter types against the Right operand.

### Step 3: AST Transformation (Desugaring)
If the magic method is successfully validated, the `TypeChecker` physically transforms the AST:
1. It replaces the `BinaryExprAST` node with a `MethodCallExprAST`.
2. `Target` = Left operand (`Matrix` instance).
3. `MethodName` = `__add__`.
4. `Arguments` = [Right operand].

### Step 4: Codegen
When the AST finally reaches `Codegen`, the original `BinaryExprAST` no longer exists. `Codegen` simply sees a standard `MethodCallExprAST` and generates a normal function call. No LLVM-level operator overloading logic is required!
