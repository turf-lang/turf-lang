# Turflang Architectural Review & Innovation Strategy

## 1. Executive Summary
Turflang is an Ahead-Of-Time (AOT) compiled programming language that bypasses virtual machines and interpreters. Leveraging the LLVM infrastructure, the Turflang compiler parses source code into an Abstract Syntax Tree (AST), translates it to LLVM Intermediate Representation (IR), and utilizes the LLVM `TargetMachine` backend to emit native object files (`.o`). These are subsequently linked into standalone, bare-metal executable binaries, placing Turflang in the same performance and compilation family as C, C++, and Rust.

## 2. Current Architectural State

### 2.1. Structs and Generics
The type system represents structs via `TurfStructType` and generics via `GenericType` and `TurfGenericParamType`.
- **Parsing**: The parser constructs the struct body and, upon completion, registers the `TurfStructType` into the `GlobalSymbolTable`. Generic parameters temporarily enter the scope during parsing to allow field type resolution.
- **Codegen**: Structs are materialized as `llvm::StructType`. Instantiation checks for custom constructors (mangled with `.init` prefixes) and falls back to member-wise LLVM `CreateStructGEP` and `CreateStore` instructions if none are found.
- **Monomorphization**: Generic types rely heavily on name mangling (e.g., `list_int`) to route method calls to the correct standard library module implementations.

### 2.2. Function Scoping and Closures
While Turflang syntactically permits defining functions inside other functions, it does not support closures or lexical scoping for nested functions.
- **Module-Level Hoisting**: During code generation, inner functions are hoisted to the global module scope (`TheModule`) and registered with internal linkage.
- **State Isolation**: The compiler explicitly clears the `NamedValues` (local variables) map before generating the inner function body. Consequently, nested functions cannot capture or access variables from their parent scope.

### 2.3. String Operations
Currently, strings are treated strictly as raw `i8*` memory pointers.
- **Binary Operations**: Arithmetic operations (`+`, `-`, etc.) enforce numeric type constraints, rendering string concatenation via `+` invalid at compile-time.
- **Comparisons**: While the `==` operator is permitted on strings, it currently translates to an LLVM `ICmpEQ` instruction, which performs a pointer address comparison rather than evaluating string content equality.

## 3. Identified Limitations & Proposed Solutions

### 3.1. Recursive and Self-Referential Structs
**Limitation**: Defining a struct that contains a pointer to itself (e.g., linked list nodes) fails with an `Unknown type` error.
**Root Cause**: `ParseStructDecl()` evaluates the entire struct body before registering the struct name in the `GlobalSymbolTable`. When a self-reference is encountered, the type lookup fails.
**Proposed Solution**: Implement forward-registration in the parser. The struct's identifier must be injected into the symbol table prior to parsing its fields. Furthermore, self-references must strictly use pointer semantics (`Node*`) to prevent LLVM infinite sizing errors.

### 3.2. Universal Method Dispatch ("Everything is an Object")
**Limitation**: `MethodCallExprAST` restricts method invocation strictly to `TurfStructType` and `GenericType`. Primitive types like `string` cannot utilize method chaining (e.g., `"hello".length()`).
**Proposed Solution**: Remove the struct-specific constraint and utilize the base type's name for method resolution. By standardizing method lookup to `turf_[type]_[method]`, Turflang can support standard library extensions for primitives (e.g., `lib/std/string.tr`), enabling a modern, unified syntax.

### 3.3. Compiler Intrinsics for String Operations
**Limitation**: Implementing a generalized user-facing operator overloading system requires significant parser and type-checker restructuring.
**Proposed Solution**: Utilize compiler intrinsics for standard types. 
- **Equality**: Intercept the `TOK_EQ` operator in `BinaryExprAST` for string operands and inject a call to `strcmp`, comparing the result against zero.
- **Concatenation**: Intercept the `+` operator for strings and route it to a standard library function (e.g., `turf_string.concat`).

## 4. Strategic Innovation Opportunities

By fully controlling the AST-to-LLVM pipeline and integrating a local Small Language Model (SLM), Turflang is uniquely positioned to adopt paradigms inaccessible to legacy languages.

### 4.1. SLM Integration
- **Prompt-Oriented Programming (POP)**: Introduce `@slm` blocks that allow developers to write natural language intent. The compiler queries the SLM with the prompt and current symbol table, retrieves generated AST nodes, injects them into the compilation pipeline, and caches the LLVM IR output.
- **The Self-Healing Compiler**: Utilize AST diffing (`CollectChangedNodeIDs`) to automatically resolve syntax or type errors. The SLM proposes an AST patch, which the compiler validates against the type checker before seamlessly updating the source.
- **Semantic Interface Matching**: Introduce a `semantic_cast<T>` operator. The SLM analyzes two distinct structs and automatically generates the LLVM memory mapping instructions to convert between them based on conceptual field similarities, bypassing boilerplate adapters.

### 4.2. Core Language Paradigms
- **Extreme Compile-Time Execution**: Extend `comptime` capabilities using the LLVM ORC JIT. Allow the compiler to execute network requests or file IO during the build process, embedding the resulting data directly into the binary to eliminate runtime configuration overhead.
- **Region-Based Memory Management**: Achieve the memory safety of Rust and the simplicity of Python without a Garbage Collector. The compiler tracks block scopes and automatically allocates/deallocates grouped memory arenas, dropping them in O(1) time upon scope exit.
- **Native State Machines**: Implement a `machine` syntax where state transitions are validated by the Type Checker. Invalid topological transitions result in compile-time failures, bringing mathematical reliability to application logic.
- **Reversible Execution**: Introduce a `@reversible` decorator that instructs the compiler to generate inverted mathematical and memory assignments in the LLVM IR, natively supporting "undo" functionality and time-travel debugging.
