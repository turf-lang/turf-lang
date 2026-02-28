# Symbol Table Implementation - Phase 2

## Overview

This phase introduces lexical scope tracking with a stack-based symbol table infrastructure to enable semantic name validation and provide better error diagnostics.

## Architecture

### Core Components

1. **SymbolTable** (`include/SymbolTable.h`, `src/SymbolTable.cpp`)
   - Stack-based scope management
   - Each symbol has a unique stable ID
   - Tracks declaration location for error reporting
   - Supports nested scopes (functions, blocks, control flow)

2. **Symbol Structure**

   ```cpp
   struct Symbol {
     SymbolID ID;              // Unique identifier
     std::string Name;         // Variable name
     TurfType Type;            // Variable type
     SourceLocation DeclLoc;   // Where declared
     llvm::AllocaInst *Alloca; // LLVM memory location
     size_t ScopeLevel;        // Nesting depth
   };
   ```

3. **Scope Structure**
   ```cpp
   struct Scope {
     size_t Level;                           // Nesting depth
     std::map<std::string, SymbolID> Symbols; // Name -> ID mapping
     bool HasEarlyExit;                       // Tracks return statements
   };
   ```

## Semantic Checks Implemented

### 1. Use-Before-Declaration

- **Error**: `UseBeforeDeclarationError`
- **Detected in**: `VariableExprAST::codegen()`
- **Example**:
  ```turf
  print(x);  // Error: Use of undeclared identifier 'x'
  int x = 5;
  ```

### 2. Duplicate Declaration

- **Error**: `DuplicateDeclarationError`
- **Detected in**: `VarDeclExprAST::codegen()`
- **Shows**: Both declaration locations
- **Example**:
  ```turf
  int x = 5;
  int x = 10;  // Error: Redeclaration of variable 'x'
               // note: previous declaration was at 1:4
  ```

### 3. Shadowing Detection

- **Warning**: `ShadowingWarning` (non-fatal)
- **Detected in**: `VarDeclExprAST::codegen()`
- **Shows**: Both declaration locations
- **Example**:
  ```turf
  int x = 5;
  if x > 0 {
    int x = 10;  // warning: Declaration of 'x' shadows variable in outer scope
                 // note: previous declaration was at 1:4
  }
  ```

### 4. Unreachable Code

- **Error**: `UnreachableCodeError`
- **Detected in**: `VarDeclExprAST::codegen()`
- **Tracks**: Return statements via `MarkEarlyExit()`
- **Example**:
  ```turf
  fn int test() {
    int x = 5;
    return x;
    int y = 10;  // Error: Unreachable declaration after return statement
  }
  ```

## Scope Management

### Scope Entry Points

1. **Global scope**: Created on `InitializeSymbolTable()`
2. **Function scope**: Entered in `FuncDefExprAST::codegen()` before body
3. **Block scope**: Entered in `BlockExprAST::codegen()`

### Scope Lifetime

- Scopes are automatically exited when:
  - Blocks complete execution
  - Functions return to caller
  - Control flow exits

## Integration Points

### Modified Files

1. **src/Codegen.cpp**
   - `VarDeclExprAST::codegen()` - Declaration validation
   - `VariableExprAST::codegen()` - Lookup validation
   - `BlockExprAST::codegen()` - Scope management
   - `FuncDefExprAST::codegen()` - Function scope and parameters
   - `ReturnExprAST::codegen()` - Early exit tracking

2. **include/Errors.h**
   - Added 4 new error types for semantic validation

3. **src/main.cpp**
   - Added `InitializeSymbolTable()` call

## Testing

Run the semantic test suite:

```bash
./scripts/test_semantics.sh
```

### Test Files

- `scope_test_use_before_decl.tr` - Use-before-declaration
- `scope_test_duplicate.tr` - Duplicate declarations
- `scope_test_shadowing.tr` - Shadowing warnings
- `scope_test_unreachable.tr` - Unreachable code
- `scope_test_valid.tr` - Valid nested scopes
- `scope_test_control_flow.tr` - Scopes in control flow

## Success Criteria ✓

- [x] All identifiers resolve to a single symbol or produce precise error
- [x] Errors point to both use-site and declaration-site when relevant
- [x] Use-before-declaration detected
- [x] Duplicate declarations in same scope detected
- [x] Shadowing of outer-scope symbols detected (warning)
- [x] Unreachable declarations detected (after returns)
- [x] Stack-based scope management implemented
- [x] Stable symbol IDs for future phases
- [x] No type inference (correct binding only)

## Future Work

This infrastructure enables:

- **Context-aware diagnostics**: Better error messages with full scope context
- **Scope-filtered identifier suggestions**: Only suggest variables in scope
- **Control-flow-aware semantic checks**: Track variable initialization state
- **Type inference**: Build on stable symbol IDs
- **Static analysis**: Unused variable detection, constant propagation

## Technical Notes

### Backward Compatibility

- `NamedValues` (old flat map) is maintained for LLVM codegen
- Symbol table provides semantic checks only
- Both updated simultaneously during declarations

### Performance

- O(1) symbol lookup in current scope
- O(n) lookup across scope chain (n = nesting depth, typically small)
- Minimal overhead - only active during compilation

### Limitations

- Standalone blocks `{ }` not supported at statement level
  - Use control flow: `if cond { ... }` or `while cond { ... }`
- Function-level scoping only (no class/module scopes yet)
