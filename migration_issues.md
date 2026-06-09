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

### [Issue #6] Migrate Function Resolution to `Resolver`
**Assignee:** [B] | **Labels:** `resolver`, `functions` | **Depends on:** #4, #5
**Description:**
Wire `FuncDefExprAST` and `FuncCallExprAST` into the Resolver so that all function-level name resolution moves out of Codegen.

**Infrastructure already in place (completed as Issue #2 gap-fill):**
- `FunctionParam { string Name; TurfTypePtr Type; }` — parameter with name + type for rich diagnostics.
- `ResolverFunctionSymbol` — has `BaseName`, `SourceModule` (`""` = current file, `"io"` = imported), `Params`, `ReturnType`, `DeclLoc`, `IsIntrinsic`. **No `MangledName` — Codegen derives that on demand** (see "Late Mangling" below).
- `ResolverSymbolTable::registerCurrentFileFunction()` / `lookupCurrentFileFunctions()` / `getAllCurrentFileFunctionNames()` — Tier-1 registry (current file).
- `ResolverSymbolTable::findInModules()` / `getAllModuleFunctionNames()` — Tier-2 search (imported modules).

**Step 1 — AST node additions (same pattern as Issue #5):**
Add to `FuncDefExprAST`:
```cpp
std::shared_ptr<ResolverFunctionSymbol> ResolvedDef; // null until Resolver runs; nulled by clone()
```
Add to `FuncCallExprAST`:
```cpp
// All overloads matching the call name — TypeChecker (Issue #8) narrows to one.
std::vector<std::shared_ptr<ResolverFunctionSymbol>> ResolvedCandidates;
```

**Step 2 — `visitFuncDef` in `Resolver.cpp`:**
```
1. If FunctionNestingDepth_ > 0:
     emit NestedFunctionError (new Diagnostic kind) — nested fns are not allowed.
     return.
2. Increment FunctionNestingDepth_.
3. enterScope() for the function body.
4. Register each parameter as a variable via declareVariable().
5. Build a ResolverFunctionSymbol:
     BaseName     = Node.getName()
     SourceModule = ""  (current file)
     Params       = [{param.Name, param.Type} for each param]
     ReturnType   = Node.getReturnType()
     DeclLoc      = {Node.getLoc().Line, Node.getLoc().Col}
6. Call SymTable_.registerCurrentFileFunction(sym).
7. Attach sym to Node.ResolvedDef.
8. Recurse into body.
9. exitScope(), decrement FunctionNestingDepth_.
```
Note: `FunctionNestingDepth_` is a new `int` field on `Resolver`, initialised to 0.

**Step 3 — `visitFuncCall` in `Resolver.cpp`:**
```
1. Recurse into arguments first (so arg expressions are resolved).
2. Extract the callee name (from the VariableExprAST inside the call, or the method base).
3. Tier-1 search: SymTable_.lookupCurrentFileFunctions(name)
4. Tier-2 search: SymTable_.findInModules(name)
5. If both empty → emit two-tier UnknownFunctionError:

     Error: Unknown function 'pritn'
       Current file:   print(int x)      — line 12
       From io module: io.printline(str)  — try 'io.printline(x)'

   Suggestions are Damerau-Levenshtein ranked using:
     getAllCurrentFileFunctionNames()  → Tier-1 candidates
     getAllModuleFunctionNames()        → Tier-2 candidates
6. Otherwise: populate Node.ResolvedCandidates with all matching shared_ptrs.
   TypeChecker (Issue #8) will narrow to one based on argument types.
```

**Step 4 — `visitModuleImport` in `Resolver.cpp`:**
When `import io` is parsed, read the module's function manifest and call
`SymTable_.registerModuleFunction("io", sym)` for each exported function.
This populates the Tier-2 registry so it is searchable at call sites.

**Late Mangling — contract with Codegen (Issue #9):**
`ResolverFunctionSymbol` deliberately has **no `MangledName` field**.
Codegen derives the LLVM symbol name on demand using a deterministic rule:
```
mangle("",   "foo",       [int, double]) → "foo"      (user-defined, no prefix)
mangle("io", "printline", [int])         → "turf_io_printline_int"
```
The exact mangling algorithm is specified in Issue #9. For now, Codegen
continues to use its existing string-lookup approach; Issue #9 will switch
it to read from `ResolvedCandidates` (narrowed by TypeChecker) and call the mangle function.

**Acceptance Criteria:**
- `turf myfile.tr --resolve` correctly reports "Unknown function" with two-tier suggestions.
- Nested function definitions emit a clear error instead of silently compiling.
- `turf myfile.tr --dump-resolver-symbols` shows both Tier-1 and Tier-2 function registries.
- No regression on existing programs (shadow mode — Codegen still runs normally).

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

**Background & Current State:**
The arity crash for `typeof()` / `power(x)` etc. is **already patched in Codegen** as a Phase 1 bandaid:
- `FuncCallExprAST::codegen()` has a hardcoded `EarlyArityMap` (Codegen.cpp ~line 1949).
- `CodegenIntrinsic()` has a hardcoded `IntrinsicArity` table (Builtins.cpp lines 43–62).
These are duplications of truth that belong in the TypeChecker. This issue *moves* that logic forward
into the proper pass, so that Codegen can eventually trust the TypeChecker and delete its own guards
(Issue #10).

**What to implement:**

**Step 1 — Add a narrowed-target field to `FuncCallExprAST` and `MethodCallExprAST` (AST.h):**
```cpp
// On FuncCallExprAST  — TypeChecker writes this after narrowing ResolvedCandidates to 1.
std::shared_ptr<ResolverFunctionSymbol> ResolvedCallTarget; // null until TypeChecker runs

// On MethodCallExprAST — same, for module-qualified calls like io.printline(...)
std::shared_ptr<ResolverFunctionSymbol> ResolvedCallTarget;
```
Both must be nulled by `clone()` (same clone-semantics contract as `ResolvedSymbol`).

**Step 2 — Fill in `TypeChecker::visitFuncCall` (TypeChecker.cpp):**
Replace the current stub `setType(&Node, nullptr)` with:
```
1. Visit all argument expressions first (bottom-up), collect their inferred types.
2. Special-case pure intrinsics that have no ResolvedCandidates entry:
     - typeof(x)   → always TurfString(); arity must be 1.
     - lengthof(x) → always TurfInt();    arity must be 1.
     Use LookupBuiltin(calleeName)->Arity (from Builtins.h) for the arity check.
     If arity wrong → reportError with a clear "expects N argument(s), got M" message.
3. For user-defined / Resolver-registered calls, read Node.ResolvedCandidates:
     a. Filter to candidates where Params.size() == Args.size().
        If none → arity error ("'foo' called with N args; known overloads: ...").
     b. Among arity-matching candidates, filter by type compatibility
        (isTypeCompatible per parameter, same logic as Codegen's fallback loop).
        If none → overload error ("no overload of 'foo' matches argument types (int, string)").
        If exactly one → set Node.ResolvedCallTarget = that candidate.
        If multiple → pick the most specific (fewest implicit casts); set ResolvedCallTarget.
4. setType(&Node, Node.ResolvedCallTarget->ReturnType) if a target was found.
   Otherwise setType(&Node, nullptr) and HadError_ = true.
```

**Step 3 — Fill in `TypeChecker::visitMethodCall` (TypeChecker.cpp):**
Replace the current stub with:
```
1. Visit argument expressions bottom-up, collect types.
2. Detect the module base: if Base is a VariableExprAST with non-empty ResolvedModuleName,
   this is a module-qualified call (e.g. io.printline, math.abs).
3. Look up candidates: RST_.findInModules(methodName, moduleName).
   (ResolverSymbolTable already has this from Issue #6 Step 4.)
4. Apply the same arity → type-match → narrowing logic as visitFuncCall Step 3.
5. Set Node.ResolvedCallTarget and setType accordingly.
```

**Step 4 — Add type-badge support for the new fields in `ASTPrinter.h`:**
- `visitFuncCall`: show `[→ returnType ✓]` in green when `ResolvedCallTarget` is set,
  or keep the existing `[unresolved]` yellow badge when null.
- `visitMethodCall`: same badge pattern.
- This makes `--dump-typechecked` show inferred return types for all calls.

**Data sources to use (do NOT re-duplicate):**
- `LookupBuiltin(name)->Arity` from `Builtins.h` — canonical arity for bare intrinsics.
- `GetBuiltinNames()` from `Builtins.h` — untouched; still used for suggestion index.
- `RST_.findInModules(name, module)` — module-qualified candidate lookup.
- `Node.ResolvedCandidates` — Resolver-populated overload set for user-defined functions.

**What this unlocks in downstream issues:**
- **Issue #9:** Codegen's `EarlyArityMap` (Codegen.cpp ~line 1949) can be deleted once TypeChecker
  catches arity before Codegen runs.
- **Issue #10:** `CodegenIntrinsic`'s `IntrinsicArity` guard (Builtins.cpp lines 122–136) and the
  type checks inside `abs`/`power` in Codegen can be deleted. Codegen reads `ResolvedCallTarget`
  directly and trusts its types blindly.

**Acceptance Criteria:**
- `typeof()` and `typeof(a, b)` produce a TypeChecker-level error (not Codegen SemanticError).
  Verify with `--typecheck`: `[TypeChecker] ✗` appears *before* Codegen runs.
- `power(1)` (wrong arity) produces a TypeChecker arity error.
- `abs("hello")` produces a TypeChecker type-mismatch error (string is not numeric).
- `--dump-typechecked` shows `[→ string ✓]` on `typeof(x)` calls and `[→ int ✓]` on `lengthof(x)`.
- All existing programs compile without regression (shadow mode — Codegen still runs in parallel).

---

## 📌 Epic 4: Codegen Detox & Stabilization (Ripping out the monolith)
*Making Codegen "dumb" again.*

### [Issue #9] Strip Name Resolution & Arity Checks from Codegen
**Assignee:** [A] | **Labels:** `codegen`, `cleanup` | **Depends on:** #5, #6, #8
**Description:**
- Delete the `EarlyArityMap` from `FuncCallExprAST::codegen()` (Codegen.cpp ~line 1949).
- Delete the `if (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')` fallback overload resolution loops in Codegen.
- Codegen must exclusively use `Node.ResolvedCallTarget->mangledName()` for function/method calls and `Node.ResolvedSymbol->MangledName` for variables.
- Delete the `LookupError` emission from Codegen; if a target wasn't found, the TypeChecker would have aborted the pipeline.
- **Risk:** High chance of breaking existing code if the Resolver missed an edge case. Run prog.tr.

### [Issue #10] Strip Semantic Error Throwing from Codegen
**Assignee:** [C] | **Labels:** `codegen`, `cleanup` | **Depends on:** #7, #8
**Description:**
- Delete all type checks inside LLVM generation (e.g., checking if `BIsDouble` or `BIsInt` inside `power`/`abs` codegen).
- Strip fallback module method overload resolution loops from `MethodCallExprAST::codegen()` (Codegen.cpp lines 2550-2598). It must exclusively use `Node.ResolvedCallTarget->mangledName()`, mirroring what was done for `FuncCallExprAST` in Issue #9.
- Remove dead code in `TypeChecker::visitFuncCall` that tries to typecheck `abs`, `power`, `print`, etc. as intrinsics (they are now standard library functions and `LookupBuiltin` returns null for them).
- *Note: The `IntrinsicArity` map was already stripped of stdlib functions in Issue #9. You only need to delete the remaining guard for `typeof`/`lengthof` in `CodegenIntrinsic` if TypeChecker takes full responsibility.*
- Remove error-throwing logic from Codegen entirely (`SemanticError`, `TypeError`, `VoidValueError`, `ArgumentTypeError`, etc.).
- Codegen should blindly trust the TypeChecker's inferred types (`Node.ResolvedCallTarget->ReturnType`, etc.) and directly generate the corresponding LLVM IR.

### [Issue #11] Pipeline Integration & Monolith Teardown
**Assignee:** [A, B, C] | **Labels:** `integration`, `milestone` | **Depends on:** All above
**Description:**
- Turn off "shadow mode". The pipeline in `main.cpp` must strictly be: `Parser -> Resolver -> TypeChecker -> Codegen`.
- Codegen should only execute if `TypeChecker` succeeds.
- Remove legacy `SymbolTable.cpp` and `GlobalSymbolTable` references from Codegen.
- The single source of truth for scope and types should now reside in the AST nodes themselves, populated by the previous passes.
- Final benchmarking and integration tests to ensure the compiler is stable.

---

## 🚦 Suggested Implementation Order & Blockers
1. **Phase 1 (Non-breaking):** Merge #1, #2, #3. (These are isolated classes and won't affect the current compiler).
2. **Phase 2 (Shadow Mode):** Merge #4, then add `--resolve` flag in `main.cpp`. 
3. **Phase 2b (Shadow Mode):** Merge #5 (attach ResolvedSymbol, nulled on clone) and #7 (TypeChecker expression type inference in shadow mode). This allows testing passes against real code without breaking compilation.
4. **Phase 3a (First Cutover):** Merge #6 (module/function Resolver) and #8 (TypeChecker arity live checks).
5. **Phase 3b (Codegen Detox):** Merge #9 (strip name resolution from Codegen) and #10 (strip type checks from Codegen). Splitting Phase 3 into 3a and 3b significantly reduces regression risk.
6. **Phase 4:** #11 (Cleanup).
