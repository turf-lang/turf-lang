# Turflang Migration: Monolith to Multi-Pass Architecture

## Overview
This epic tracks the architectural migration of the Turflang compiler from a monolithic, codegen-heavy architecture into a modern, multi-pass pipeline (`Parser -> Resolver -> TypeChecker -> Codegen`). 

**Goal:** Decouple semantics and name resolution from LLVM Codegen.
**Constraints:** The compiler currently works. We must migrate incrementally. Do not break the main branch.

---

## 👥 Contributors (Proposed Ownership)
- **[A] Architecture Lead:** Core infrastructure, ASTVisitor, Symbol Table, and Integration.
- **[B] Resolver Engineer:** Scopes, Name Resolution, Module Loading.
- **[C] Type System Engineer:** Type inference, arity checking, stripping logic from Codegen.

---

## 📌 Epic 1: AST Infrastructure & Symbol Table (Foundation)
*Establishing the data structures before touching compiler logic.*

### [Issue #1] Implement `ASTVisitor` Interface
**Assignee:** [A] | **Labels:** `architecture`, `infrastructure` | **Depends on:** None
**Description:**
We need a standard way to walk the AST without embedding traversal logic inside the nodes.
- Define `class ASTVisitor` with virtual methods for every AST node (`visitVarDecl`, `visitFunctionCall`, `visitIfStmt`, etc.).
- Update all AST nodes to accept the visitor via an `accept(ASTVisitor& v)` method.
- **Acceptance Criteria:** A dummy `ASTPrinter` visitor successfully traverses and prints the AST without crashing.

### [Issue #2] Implement Hierarchical `SymbolTable` Data Structures
**Assignee:** [A] | **Labels:** `architecture`, `data-structures` | **Depends on:** None
**Description:**
Implement the dictionaries that will eventually track our state.
- Create `VariableSymbol`, `FunctionSymbol` (with arity/overload tracking), and `Module` structs.
- Create `class Scope` with a pointer to its parent and unordered maps for symbols.
- Create `class SymbolTable` to manage the active scope and loaded modules.
- *Note:* Do not hook this into the compiler yet. Just write the isolated classes and unit tests.

### [Issue #3] Extract `Type` Hierarchy from LLVM Types
**Assignee:** [C] | **Labels:** `type-system` | **Depends on:** None
**Description:**
Currently, types are heavily coupled with LLVM `Type*`. We need an abstract frontend representation.
- Create a polymorphic `class Type` (e.g., `IntType`, `StringType`, `GenericType`).
- **Acceptance Criteria:** The compiler frontend can represent a `List<int>` natively without asking LLVM what a pointer is.

---

## 📌 Epic 2: The Resolver Pass (Decoupling Name Resolution)
*Making sure every variable and function actually exists before we check its type.*

### [Issue #4] Create `Resolver` Pass & Variable Tracking
**Assignee:** [B] | **Labels:** `resolver`, `core` | **Depends on:** #1, #2
**Description:**
Implement `class Resolver : public ASTVisitor`.
- Walk `VarDecl` nodes: Add variables to the current `SymbolTable` scope.
- Walk `Block` nodes (`{...}`): Call `symTable.enterScope()` and `symTable.exitScope()`.
- Walk `VarExpr` nodes: Look up the variable in the `SymbolTable`. If it doesn't exist, throw an "Unknown Variable" error. (Our Damereau-Levenshtein Errors)

### [Issue #5] Modify AST Nodes to Store Resolved `Symbol` Pointers
**Assignee:** [A] | **Labels:** `architecture`, `refactor` | **Depends on:** #4
**Description:**
Currently, Codegen does string lookups to find functions/variables.
- Add `std::shared_ptr<Symbol> ResolvedSymbol;` to relevant AST nodes (e.g. `VariableExprAST`, `VarDeclExprAST`).
- Update the `Resolver` to attach the found symbol to the AST node during its traversal.
- **Risk:** AST nodes will become slightly fatter, but this eliminates O(N) string lookups in Codegen.
- **Critical Caveat (Clone Semantics):** The Sandbox (`Sandbox.cpp`) and AST diffing machinery clone trees heavily. A shallow copy of `ResolvedSymbol` will point to a stale scope. *Decision:* Ensure `clone()` nulls out the `ResolvedSymbol` pointer, meaning cloned ASTs must be re-resolved before TypeChecking/Codegen.

### [Issue #6] Migrate Module & Function Resolution to `Resolver`
**Assignee:** [B] | **Labels:** `resolver`, `modules` | **Depends on:** #4, #5
**Description:**
Stop global string-prefix guessing for modules (e.g., `turf_io_printline`).
- When `import io` is parsed, the Resolver creates an `io` Module in the Symbol Table.
- When `io.printline` is called, the Resolver validates it against the `io` Module and attaches the exact mangled LLVM name to the AST node.
- **Note:** The `ResolverSymbolTable` will also need functionality similar to `GetRankedSuggestions()` from the legacy table to ensure error messages don't degrade.
- **Acceptance Criteria:** Cross-namespace bugs (Note 12) are impossible because the Resolver strictly checks module boundaries.

---

## 📌 Epic 3: The TypeChecker Pass (Decoupling Semantic Analysis)
*Validating that operations are legal.*

### [Issue #7] Create `TypeChecker` Pass & Primitive Inference
**Assignee:** [C] | **Labels:** `type-system`, `core` | **Depends on:** #1, #3, #5
**Description:**
Implement `class TypeChecker : public ASTVisitor`.
- This runs *after* the `Resolver`.
- Walk the AST and infer types bottom-up. Visit binary operations (`+`, `-`) and assign a `ResolvedType` to the AST node.
- Throw Type Mismatch errors here (e.g., `string + int` -> Note 9).
- **Prerequisite:** Ensure `ForExprAST` loop variables are properly typed in the Resolver (e.g., inferring `int` from the range) before this pass runs.

### [Issue #8] Implement Intrinsic Arity & Overload Resolution in `TypeChecker`
**Assignee:** [C] | **Labels:** `type-system`, `builtins` | **Depends on:** #6, #7
**Description:**
Intrinsic functions (`math.abs`, `typeof`) currently crash LLVM if arguments are missing (Note 10, Note 13).
- Implement arity checking in the TypeChecker. Check `args.size()` against the `FunctionSymbol`.
- Resolve the correct function overload based on argument types.
- **Note:** Make sure the validation logic *moves* to the TypeChecker rather than just getting deleted, as `Builtins.cpp` is still needed for the suggestion index.
- **Acceptance Criteria:** `typeof()` with 0 or 2 arguments throws a clean compiler error instead of an LLVM assertion.

---

## 📌 Epic 4: Codegen Detox & Stabilization (Ripping out the monolith)
*Making Codegen "dumb" again.*

### [Issue #9] Strip Name Resolution from Codegen
**Assignee:** [A] | **Labels:** `codegen`, `cleanup` | **Depends on:** #5, #6
**Description:**
- Delete all `if (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')` fallback loops.
- Codegen must exclusively use the `ResolvedSymbol->MangledName` provided by the AST.
- **Risk:** High chance of breaking existing code if the Resolver missed an edge case. Run full test suite.

### [Issue #10] Strip Semantic Error Throwing from Codegen
**Assignee:** [C] | **Labels:** `codegen`, `cleanup` | **Depends on:** #7, #8
**Description:**
- Delete all type checks inside LLVM generation (e.g., checking if `BIsDouble` or `BIsInt` inside `math.power` codegen).
- Remove error-throwing logic from Codegen entirely. Codegen should use raw `llvm::Value*` blindly, trusting the TypeChecker.

### [Issue #11] Pipeline Integration & Monolith Teardown
**Assignee:** [A, B, C] | **Labels:** `integration`, `milestone` | **Depends on:** All above
**Description:**
- Wire up `main.cpp` to run the passes sequentially: `Parser -> Resolver -> TypeChecker -> Codegen`.
- Delete dead legacy code from the old monolith. (Recommendation: Keep the legacy `SymbolTable.cpp` as a read-only reference for at least one release before deleting).
- Final benchmarking and integration tests.

---

## 🚦 Suggested Implementation Order & Blockers
1. **Phase 1 (Non-breaking):** Merge #1, #2, #3. (These are isolated classes and won't affect the current compiler).
2. **Phase 2 (Shadow Mode):** Merge #4, then add `--resolve` flag in `main.cpp`. 
3. **Phase 2b (Shadow Mode):** Merge #5 (attach ResolvedSymbol, nulled on clone) and #7 (TypeChecker expression type inference in shadow mode). This allows testing passes against real code without breaking compilation.
4. **Phase 3a (First Cutover):** Merge #6 (module/function Resolver) and #8 (TypeChecker arity live checks).
5. **Phase 3b (Codegen Detox):** Merge #9 (strip name resolution from Codegen) and #10 (strip type checks from Codegen). Splitting Phase 3 into 3a and 3b significantly reduces regression risk.
6. **Phase 4:** #11 (Cleanup).
