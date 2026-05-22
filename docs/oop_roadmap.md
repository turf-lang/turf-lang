# TurfLang OOP & Standard Library Roadmap

To transform TurfLang from a primitive compiler into a modern, generic language, we need a massive architectural shift. Attempting to build everything at once will break the compiler. We will execute this in systematic phases.

## The Roadmap

1. **Phase 1: The Struct Foundation (Current Focus)**
   - Introduce user-defined `struct` types.
   - Implement dot-access parsing (`user.age`) generically.
   - Register structs in the Symbol Table and compile them to LLVM structs.
2. **Phase 2: Methods and Generic Dispatch**
   - Allow functions to be defined inside/attached to structs.
   - Replace the hardcoded `ListMethodCallExprAST` with a fully generic `MethodCallExprAST` that dynamically looks up the method in the Symbol Table.
3. **Phase 3: The Prelude and Standard Library**
   - Create `lib/std/` folder.
   - Write `list.tr` in pure TurfLang (wrapping `malloc`/`realloc` via intrinsics).
   - Update `main.cpp` to auto-import the Prelude before user code.
4. **Phase 4: Modules and Exceptions**
   - Implement file-based imports (`import math`).
   - Implement `try ... onError ... raise` exception handling.

---

# Phase 1 Implementation Plan: The Struct Foundation

This plan covers the immediate next steps to lay the groundwork.

## Proposed Changes

### src/AST.h & src/AST.cpp
#### [NEW] `StructDeclExprAST`
Defines a structure and its fields.
#### [NEW] `FieldAccessExprAST`
Replaces generic dot-access for fields (e.g., `user.name`).
#### [NEW] `StructInitExprAST`
Allows initialization of a struct in memory.

### src/Parser.cpp
#### [MODIFY] Parser Core
- Add parsing for the `struct` keyword: `struct User { int age, string name }`.
- Update `ParsePostfixExpression` to handle generic field access. When it sees a `.` followed by an identifier that isn't a method call `()`, it will emit a `FieldAccessExprAST`.

### src/SymbolTable.h & src/SymbolTable.cpp
#### [MODIFY] Type Registry
- Expand the symbol table to store User-Defined Types (Structs). When `StructDeclExprAST` is parsed, register the struct's name and its ordered field types so the Semantic Analyzer knows the memory layout.

### src/Codegen.cpp
#### [MODIFY] LLVM Code Generation
- Emit `llvm::StructType::create` for struct definitions.
- For `FieldAccessExprAST`, implement `Builder->CreateStructGEP` (GetElementPtr) to calculate the memory offset of the requested field dynamically based on the symbol table layout.

## Verification Plan
We will write tests in a new `tests/oop/` folder:
- Define a struct, instantiate it, assign values to its fields, and print them.
- Verify LLVM IR outputs proper `getelementptr` instructions.

---

> [!IMPORTANT]
> ## User Review Required
> Because this is a massive shift, I strongly recommend we tackle this one phase at a time. Does Phase 1 (The Struct Foundation) look good to you? Also, what syntax do you prefer for struct initialization? (e.g., C-style `User u = {10, "Alice"}` or constructor-style `User u = User(10, "Alice")`?). Let me know your thoughts so we can begin!
