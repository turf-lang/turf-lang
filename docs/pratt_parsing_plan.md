# TurfLang Expression Composability Roadmap (Pratt Parsing)

This document provides a detailed, technical roadmap for refactoring TurfLang's expression parsing and code generation to support infinitely composable expressions (like `get_data()[0].method()`).

---

## 1. Phase 1: AST and Parser Refactoring (Completed)
We moved away from "Identifier-based operations" to "Expression-based operations" using Pratt Parsing concepts.
* **AST Node Updates**: Replaced hardcoded `std::string Name` fields in nodes like `ArrayAccessExprAST`, `FuncCallExprAST`, `ListMethodCallExprAST`, and `AssignmentExprAST` with recursive `std::unique_ptr<ExprAST> Base/LHS` fields.
* **Postfix Parsing**: Eliminated monolithic identifier parsing by introducing `ParsePostfixExpression(std::unique_ptr<ExprAST> Base)` to allow operations like `[index]`, `(args)`, and `.method()` to chain onto *any* evaluated expression indefinitely.

## 2. Phase 2: L-Value vs. R-Value Code Generation (Completed)
Because assignments now accept an `ExprAST` for the `LHS` (instead of just a string name), the LLVM code generator needs a way to extract the memory address (L-Value) of an expression, rather than just reading its data value (R-Value).
* **The L-Value Interface**: Added `virtual llvm::Value* codegenLValue()` to the AST so we can extract raw memory addresses instead of just data values.
* **LLVM Pointer Generation**: Implemented `codegenLValue()` for `VariableExprAST` and `ArrayAccessExprAST` (emitting `GEP` without `Load`) so that assignments like `get_data()[0] = 100` can store data recursively into evaluated pointer addresses.

## 3. Phase 3: Multi-dimensional Arrays (Pending)
Currently, TurfLang supports `int[N]` but not `int[N][M]`. We need to generalize array types.
* **Type System**: Extend `ParseType()` to handle recursive array definitions (e.g., parsing multiple `[N]` tokens into nested `ArrayType`s).
* **Code Generation**: Refactor `ArrayDeclExprAST` and `ArrayAccessExprAST` to correctly compute multi-dimensional offsets (either as flat arrays using row-major offsets or arrays-of-arrays) and perform nested bounds checking.

## 4. Phase 4: Chained Method Calls (Pending)
While the syntax tree now supports chaining method calls (e.g. `list.method1().method2()`), the compiler's semantic type checking needs to support it.
* **Method Return Types**: Ensure `ListMethodCallExprAST` and future method calls accurately report their return types (currently methods like `push` return `void`).
* **Builder API**: Consider allowing methods to return the object itself (i.e. returning an L-Value reference or pointer) so that true method chaining is semantically valid.

## 5. Phase 5: Type-System and Semantic Polish (In Progress)
The final polish to ensure the type-system is robust enough to handle deep nesting without false positives.
* **LLVM Type Erasure Fix**: Fix bugs where `getTurfTypeFromLLVM` erroneously defaults LLVM pointer types to `string` (which breaks returning `list<T>` from functions).
* **Semantic Inference**: Migrate more of the type-checking out of the LLVM `Codegen` phase and into the AST parsing phase (`getExprType()`) so that type compatibility is verified using the semantic AST nodes before LLVM lowers them into opaque pointers.
