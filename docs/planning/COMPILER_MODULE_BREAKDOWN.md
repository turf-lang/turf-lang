# Compiler Module Breakdown Plan

Status: **in progress**. Batches 1 (`Errors.h`), 2 (`AST.h`), 3
(`Parser.cpp`), 4 (`Resolver.cpp` + `ResolverSymbolTable`), 5
(`TypeChecker.cpp`), and 6 (`Codegen.cpp`) are done (2026-08-03); batches
7–9 below are still proposed, not yet executed. This document is the map +
plan for
splitting the monolithic files under `src/`/`include/` into smaller,
single-concern files, matching how production compilers (Clang, rustc, Swift)
organize their AST/Parse/Sema/CodeGen layers. Nothing in this document has
been applied to the tree yet — it's the reconnaissance pass before the actual
refactor, done in batches (see "Batch sequencing" at the end).

`external/llama.cpp` is a vendored third-party dependency and is out of scope
entirely — this plan only touches `src/` and `include/`.

## Why this split, and why this order

Turf's pipeline is `Lexer -> Parser -> Resolver -> TypeChecker -> Codegen`,
and four of those five stages (Parser, Resolver, TypeChecker, Codegen) are
implemented as one method/function per AST node kind, all living in a single
`.cpp` file per stage. That's the same shape Clang split years ago:
`ParseExpr.cpp` / `ParseStmt.cpp` / `ParseDecl.cpp`, `SemaExpr.cpp` /
`SemaStmt.cpp` / `SemaDecl.cpp`, `CGExpr.cpp` / `CGStmt.cpp` / `CGDecl.cpp` /
`CGClass.cpp` / `CGCall.cpp`. The category boundary (expressions / control
flow / declarations / calls-and-member-access) is the load-bearing one across
every stage, so it's used consistently everywhere below — once `AST.h` is
split by that boundary, the same boundary applies almost mechanically to
Parser, Resolver, TypeChecker, and Codegen, which is also why `AST.h` is
split before them, not after.

## Current state (line counts, `src/` + `include/` only)

| File | Lines | Shape |
|---|---:|---|
| `src/Codegen.cpp` | 5287 | one `codegen()`/`codegenLValue()` impl per AST node class (44 methods) + shared LLVM-type helpers |
| `src/Parser.cpp` | 3542 | one `Parse*()` free function per grammar production (~50 functions) + module/import resolution + precedence engine |
| `include/AST.h` | 2128 | 45 AST node classes, all flat under one `ExprAST` base |
| `src/TypeChecker.cpp` | 2862 | one `visit*()` per node kind (46 methods) + shared type-inference helpers |
| `src/Resolver.cpp` | 2221 | one `visit*()` per node kind (~40 methods) + a distinct two-pass hoisting/registration engine |
| `include/Errors.h` | 1695 | ~75 flat `TurfError`/`TurfWarning` subclasses, one per diagnostic |
| `src/main.cpp` | 1072 | `main()` alone is ~700 lines: the entire pre-pass/main-pass driver inlined |
| `src/CFG.cpp` / `include/CFG.h` | 1050 / 218 | `CFGBuilder` (AST -> CFG construction) and `CFG` (reachability/complexity/dead-code analysis) both in one class+file |
| `include/ResolverSymbolTable.h` / `.cpp` | 922 / 649 | 3 distinct registries (lexical scopes, module registry, struct/enum/error type registry) in one class |
| `src/Lexer.cpp` / `include/Lexer.h` | 919 / 292 | tokenizer + a separately-conceived `DiagnosticEngine` bundled together |
| `include/Types.h` / `src/Types.cpp` | 743 / 316 | `TurfType` hierarchy (18 concrete types) — reasonably sized already |
| `src/Analysis.cpp` / `include/Analysis.h` | 791 / 66 | 3 independent dataflow passes (definite-assignment, liveness, constructor-field) in one file |
| `src/SLMIntegration.cpp` | 722 | suggestion fetch/validate entrypoints + JSON/prompt/confidence helpers |
| `src/Sandbox.cpp` / `include/Sandbox.h` | 704 / 182 | one cohesive incremental-fix-validation state machine |
| `src/Lint.cpp` | 714 | heuristic lint checks, one function per node kind (some names duplicated as fwd-decl + impl) |
| `include/ASTPrinter.h` | 531 | debug AST-dump visitor, already isolated |
| everything else (`Builtins`, `SymbolTable`, `Types`, `CallGraph`, `AliasAnalysis`, `ValueRange`, `Algorithms`, `SLMEngine`, `Colors`, `SuggestionIndex`, `ASTVisitor`, `ErrorCategory`) | ≤650 each | already single-concern, no action needed |

## Per-file breakdown

### 1. `include/AST.h` (2128 lines, 45 classes) → 5 headers

All node classes are thin (mostly data + `codegen()`/`accept()` declarations),
so this is a pure declaration split, zero logic change. New files:

- **`AST.h`** (kept) — `ExprAST` base class only, plus shared small structs
  used across categories (`SourceLocation` stays in `Errors.h`/`Lexer.h` as
  today). Every other header below includes this one.
- **`ASTExpr.h`** — literal & operator expressions: `NumberExprAST`,
  `BoolExprAST`, `CharExprAST`, `StringExprAST`, `NullExprAST`,
  `DefaultInitExprAST`, `BinaryExprAST`, `UnaryExprAST`, `CompareExprAST`
  (+ `MatchesArm`), `CastExprAST`, `AddressOfExprAST`, `PostfixExprAST`,
  `AssignmentExprAST`, `CompoundAssignmentExprAST`, `VariableExprAST`,
  `SizeOfExprAST`.
- **`ASTStmt.h`** — control flow: `BlockExprAST`, `IfExprAST` (+
  `CondBranch`), `WhileExprAST`, `DoWhileExprAST`, `ForExprAST`,
  `ForEachExprAST`, `BreakExprAST`, `ContinueExprAST`, `ReturnExprAST`,
  `RecoverExprAST`.
- **`ASTDecl.h`** — declarations: `VarDeclExprAST`, `FuncDefExprAST` (+
  `ParamDecl`), `ExternFuncDeclExprAST`, `StructDeclExprAST`,
  `EnumDeclExprAST`, `ErrorDeclExprAST`, `ArrayDeclExprAST`,
  `ModuleImportExprAST` (+ `ImportedSymbolRef`).
- **`ASTAccess.h`** — calls & member/module access: `FuncCallExprAST`,
  `MethodCallExprAST`, `FieldAccessExprAST`, `ArrayAccessExprAST`,
  `ArrayLiteralExprAST`, `ScopeAccessExprAST`.

`AST.h` becomes an umbrella that includes all 5, so existing
`#include "AST.h"` call sites elsewhere in the tree don't need to change.

**Discovered during execution (2026-08-03), not part of the original recon:**
the last ~700 lines of `AST.h` (past the 45th node class) aren't a node class
at all — they're the sandbox's AST-diffing/traversal helpers
(`CollectAllNodeIDsInternal`, `CollectChangedNodeIDsInternal`,
`CollectChangedNodeIDs`, `AreNodesStructurallyEqual`), each `dynamic_cast`ing
across every one of the 45 concrete node types. This can't belong to any one
of the 4 category headers above — it structurally depends on all of them —
so it became a 6th file, **`ASTDiff.h`**, included last from the `AST.h`
umbrella (after `ASTExpr.h`/`ASTStmt.h`/`ASTDecl.h`/`ASTAccess.h`). Note for
a future pass: `CollectAllNodeIDsInternal` and `CollectChangedNodeIDsInternal`
are two parallel ~45-branch `dynamic_cast` chains walking the same tree
shape for different purposes, and they already disagree on which node types
they even handle (some types are silently skipped by one but not the other).
Unifying them behind a shared child-enumerator would reduce real duplication
but is a behavioral redesign, not a mechanical extraction — deliberately
left alone during the split.

### 2. `include/Errors.h` (1695 lines, ~75 classes) → base + 8 category headers

Same pure-declaration split, grouped by which pass raises each diagnostic
(this already matches how Clang splits `Diagnostic*Kinds.td` by group):

- **`Errors.h`** (kept, trimmed) — `SourceLocation`, `TurfError`,
  `TurfWarning` base classes only.
- **`ErrorsLex.h`** — `SyntaxError`, `KeywordError`.
- **`ErrorsType.h`** — `TypeError`, `StringConversionError`,
  `AbsArgTypeError`, `PowerArgTypeError`, `CompareError`/`CompareTypeError`/
  `CompareWarning`, `ArgumentTypeError`, `ReturnTypeMismatchError`,
  `VoidValueError`, `ArithmeticError`, `NullAssignmentError`,
  `ImmutableAssignmentError`.
- **`ErrorsScope.h`** — `LookupError`, `UnknownFunctionError`,
  `ReferenceError`, `UseBeforeDeclarationError`, `DuplicateDeclarationError`,
  `DuplicateTypeNameError`, `ShadowingWarning`, `UnknownTypeError`,
  `WrongArityError`, `NestedFunctionError`.
- **`ErrorsStruct.h`** — `StructMethodNotFoundError`,
  `NoConstructorDefinedError`, `ReservedConstructorNameError`,
  `StaticOutsideStructError`, `ConstructorCannotBeStaticError`,
  `DefaultConstructorArityError`, `ConstructorCalledOnInstanceError`,
  `IncompleteConstructorError`, `DotOperatorOnNonInstanceError`.
- **`ErrorsModule.h`** — `ModuleNotFoundError`, `CircularImportError` (+
  `ImportChainEntry`), `UnknownScopeQualifierError`,
  `ScopeOperatorOnInstanceError`, `UnknownModuleSymbolError`,
  `ImportSymbolCollisionError`, `UnqualifiedTypeNotVisibleError`,
  `ModuleMethodNotFoundError`, `ModuleArityOrTypeError`,
  `ExportOutsideTopLevelError`, `ExportMutNotSupportedError`,
  `ExportNonConstantInitializerError`, `OperatorBuiltinPairError`,
  `ConflictingExternDeclarationError`.
- **`ErrorsFlow.h`** — `ControlFlowError`, `SemanticError`,
  `UnreachableCodeError`, `MissingReturnError`, `UnreachableBlockWarning`,
  `DeadBranchWarning`, `StatementAfterTerminatorError`,
  `RedundantComparisonWarning`, `SelfAssignmentWarning`,
  `SuspiciousInfiniteLoopWarning`, `CyclomaticComplexityWarning`,
  `InfiniteLoopWarning`, `DeadStoreWarning`, `UnusedVariableWarning`,
  `RecursionWithoutBaseError`, `StuckForLoopVariableWarning`,
  `EmptyBranchWarning`, `SelfComparisonWarning`,
  `DivisionByZeroLiteralWarning`, `IdenticalBranchesWarning`,
  `NeverReturnsErrorWarning`, `DefiniteAssignmentError`.
- **`ErrorsArray.h`** — `ArrayBoundsError`, `ArraySizeMismatchError`,
  `ArrayNonIntegerIndexError`, `ArrayTypeMismatchError`,
  `ForEachNonArrayError`, `ForEachUnsizedArrayError`.
- **`ErrorsMemory.h`** — `UseAfterFreeError`, `DoubleFreeError`,
  `BareErrorAsBindingError`.

`Errors.h` becomes an umbrella including all 8, same non-breaking approach
as `AST.h`.

### 3. `src/Parser.cpp` (3542 lines) → 5 files, mirrors the AST split

- **`ParserCore.cpp`** — `getNextToken`, `InitializePrecedence`,
  `GetTokPrecedence`, `ParseType`/`isDisallowedVoidType`, `Parse()`,
  `ParsePrimary`/`ParsePrimaryBase`, `ParseBinOpRHS`, `ParseExpression`,
  `ParseUnary`, `ParsePostfixExpression`, `ParseParenExpr` — the precedence-
  climbing engine every other file below calls into.
- **`ParserLiteral.cpp`** — `ParseNumberExpr`, `ParseBoolExpr`,
  `ParseCharExpr`, `ParseStringExpr`, `ParseNullExpr`, `SynthesizeDefault`.
- **`ParserStmt.cpp`** — `ParseBlock`, `ParseIfExpr`, `ParseWhileExpr`,
  `ParseDoWhileExpr`, `ParseForExpr`, `ParseCompareExpr`, `ParseReturnExpr`,
  `ParseLetStatement`.
- **`ParserDecl.cpp`** — `ParseStructDecl`, `ParseEnumDecl`,
  `ParseErrorDecl`, `ParseExternDecl`, `ParseExportDecl`, `ParseFuncDef`,
  `ParseOperatorDecl`.
- **`ParserModule.cpp`** — `ParseFile`, `ParseImport`,
  `ParsePreludeImports`, `ParseModuleImportByName`, `IndexModuleDir`,
  `TurfInstallRoot`, `BuildCircularImportChain`, plus the module-level state
  (`ImportStack`, `ImportedFiles`) and the top-level-AST cache
  (`ClearParsedTopLevelASTs`/`SwapParsedTopLevelASTs`/
  `RecordParsedTopLevelAST`/`BuildProgramASTFromCache`). This is
  self-contained enough (own state, own ~500-line subsystem) to earn its own
  file even though it has no Clang analogue.

`Parser.h`'s public surface doesn't change — all 5 `.cpp` files implement
declarations that stay in the one header.

**Executed 2026-08-03.** Unlike `AST.h`/`Errors.h` (header-only, pure
declaration splits), `Parser.cpp` is free functions over file-scope `static`
state, not a class — splitting it across 5 translation units meant anything
`static` (internal linkage) that a function in a *different* new file needed
to call had to lose its `static` and gain a declaration visible to every
file. Added **`src/ParserInternal.h`** (private, not part of the public
`Parser.h` surface) for exactly that: declares every function called across
the 5 files' category boundaries, built by tracing the actual call graph
(not guessed) — `ParsePrimaryBase()` alone, the primary-expression
dispatcher, turned out to call into all 4 other categories directly (every
literal, every statement keyword, every declaration keyword, `ParseImport`).
3 previously-`static` helpers (`ParseType`, `isDisallowedVoidType`,
`ParseNumberExpr`) lost their `static` for this reason. One extra wrinkle:
`ParseBinOpRHS` calls `ParseUnary`, both Core-category and landing in the
same new file, but in *file order* `ParseUnary`'s definition comes after
`ParseBinOpRHS`'s — needed a same-file forward declaration too (mirroring
what the original monolith's top-of-file forward-declaration block was
already doing, which the split removed). Verified the whole split against
the compiler, not just eyeballing: two custom brace-counting scripts (like
AST.h's) initially both had subtle bugs (didn't account for a default
argument's `= {}` embedding a brace pair in a function *signature*, and one
function's return type sitting alone on the line before its name+params)
that gave wrong end-of-function lines; caught by diffing computed line
counts against manual spot-checks before trusting them, not after.

### 4. `src/Resolver.cpp` (2221 lines) + `include/ResolverSymbolTable.h`/`.cpp` (922/649 lines)

Resolver mirrors TypeChecker's node-kind shape but has one extra concern —
a two-pass hoisting engine with no TypeChecker equivalent — so it gets one
extra file:

- **`ResolverCore.cpp`** — `resolve`, `resolveAll`,
  `registerFunctionSignature`, `validateOperatorDecl`,
  `hoistExportedConstant`, `hoistTopLevel` (both overloads),
  `hoistModuleBody`, `registerOnDemandImports` — pass 1, the forward-
  declaration/registration engine that makes out-of-order top-level
  definitions work.
- **`ResolverExpr.cpp`** — `visitVariable`, `visitBinary`, `visitUnary`,
  `visitAddressOf`, `visitAssignment`, `visitCast`,
  `visitCompoundAssignment`, `visitPostfixExpr`, `visitCompare`.
- **`ResolverStmt.cpp`** — `visitVarDecl`, `visitBlock`, `visitIf`,
  `visitWhile`, `visitDoWhile`, `visitFor`, `visitForEach`, `visitReturn`.
- **`ResolverDecl.cpp`** — `visitFuncDef`, `visitStructDecl`,
  `visitArrayDecl`, `visitArrayLiteral`, `visitArrayAccess`,
  `visitEnumDecl`, `visitErrorDecl`, `visitExternFuncDecl`,
  `visitModuleImport`, `validateNamedType`, `validateModuleQualifier`.
- **`ResolverCall.cpp`** — `visitFuncCall`, `visitMethodCall`,
  `visitFieldAccess`, `visitRecoverExpr`, `visitScopeAccess`.

`ResolverSymbolTable` bundles 3 registries that are conceptually independent
(only sharing the outer class for convenience of a single injected
dependency) — split into:

- **`ResolverScopeTable.h`/`.cpp`** — lexical scoping: `enterScope`,
  `exitScope`, `resetToDepth`, `declareVariable`, `lookupVariable`,
  `isDeclaredInCurrentScope`, `getAllVisibleNames`.
- **`ResolverModuleTable.h`/`.cpp`** — `ResolverModule` class + module/
  constant/function registration and lookup: `registerModule`,
  `registerModuleFunction`, `registerModuleConstant`, `findModuleConstant`,
  `hasModule`, `getModuleFunctionNames`, `getModuleConstantNames`,
  `registerCurrentFileFunction`, `registerCurrentFileConstant`,
  `lookupCurrentFileConstant`, `isTypeVisibleUnqualified`,
  `getTypeOwningModule`.
- **`ResolverTypeRegistry.h`/`.cpp`** — struct/enum/error-type registration
  and lookup: `isKnownStruct`, `getAllStructNames`, `lookupEnumType`,
  `isEnumName`, `getAllEnumNames`, `lookupErrorType`, `isErrorTypeName`,
  `getAllErrorTypeNames`, plus the `dump`/`dumpModules` debug helpers.

`ResolverSymbolTable` itself becomes a thin composite holding one instance of
each of the 3 above, keeping `Resolver.cpp`'s call sites unchanged.

**Executed 2026-08-03.** `Resolver.cpp`'s split was closer in shape to
`AST.h`/`Errors.h` than to `Parser.cpp`'s, since `Resolver` is an actual
class -- every `Resolver::` method is already visible across all 5 new
`.cpp` files via the single class declaration in `Resolver.h`, no
`ParserInternal.h`-style forward-declaration header needed for those. Only 2
genuinely file-scope `static` free functions (`toSourceLocation`,
`substituteTypeParams` -- NOT `Resolver::` methods) turned out to be called
from more than one of the 5 new files, so a small `ResolverInternal.h` was
still needed for just those two.

The `ResolverSymbolTable` split hit a real circular-`#include` problem the
plan didn't anticipate: `ResolverScopeTable`/`ResolverModuleTable`/
`ResolverTypeRegistry` all need the shared data types (`VariableSymbol`,
`ResolverFunctionSymbol`, ...), which used to live directly in
`ResolverSymbolTable.h` alongside the composite class -- but the composite
class needs all 3 sub-table classes fully defined first. Two headers both
needing content from each other is not solvable by `#include` guards alone
(whichever file is entered first ends up missing the other's content, since
re-entering an already-guarded header is silently skipped). Fixed by pulling
the shared data types into their own `ResolverSymbolBase.h`, which the 3
sub-tables + the composite `ResolverSymbolTable.h` all include, breaking the
cycle -- the general fix for "an umbrella and its own leaves need each
other's content," worth remembering for the `TypeChecker`/`Codegen` batches
if the same shape recurs there.

Two more real complications, both handled by deviating from the plan
sketch's rough method list once the actual code was read: (1)
`TypeOwningModule_` (which module owns each struct/enum/error name) is
written exclusively by the 3 type-registration methods, so it — and
`getTypeOwningModule()`/`getModuleTypeNames()` — moved to
`ResolverTypeRegistry`, not `ResolverModuleTable` as originally sketched.
(2) `isTypeVisibleUnqualified()` genuinely needs data from BOTH
`ResolverTypeRegistry` (`getTypeOwningModule`) and `ResolverModuleTable` (a
new `hasFlatTypeImport` query, extracted from what used to be inline private
member access) — a cross-cutting query like that belongs on the composite
`ResolverSymbolTable`, the one layer that actually holds both sub-tables,
not shoehorned into either one alone. Also preserved
`ResolverSymbolTable::FlatImportedSymbol` / `::ModuleFunctionMatch` as
type-alias re-exports (`using FlatImportedSymbol = ResolverModuleTable::
FlatImportedSymbol;`) since external call sites (`ResolverCore.cpp`,
`ResolverCall.cpp`) spell them fully qualified through the composite name —
canonical definitions moved to `ResolverModuleTable`, zero call-site changes
needed. `dump()`/`dumpModules()` were split into one section-emitting method
per sub-table (e.g. `ResolverTypeRegistry::dumpStructNames()`,
`ResolverModuleTable::dumpFlatImports()`), with the composite calling them
in the original exact order — verified byte-for-byte against real
`--dump-resolver-symbols`/`--dump-modules` output, not just code review,
since this was the riskiest part of the split (hand-reassembling one
function's output from pieces spread across 3 new files).

### 5. `src/TypeChecker.cpp` (2862 lines) → 5 files, same boundary as Resolver

- **`TypeCheckerCore.cpp`** — `checkAll`, `check`, `getType`/`hasType`/
  `setType`, `reportError`, `isNumericType`, `areArithCompatible`,
  `inferArithmeticType`, `inferRelationalType` — shared type-inference
  helpers every `visit*` below calls into.
- **`TypeCheckerExpr.cpp`** — `visitNumber`/`Bool`/`Char`/`String`/`Null`/
  `DefaultInit`, `visitBinary`, `visitUnary`, `visitAddressOf`,
  `visitAssignment`, `visitCompoundAssignment`, `visitPostfixExpr`,
  `visitCast`, `visitCompare`.
- **`TypeCheckerStmt.cpp`** — `visitBlock`, `visitIf`, `visitWhile`,
  `visitDoWhile`, `visitFor`, `visitForEach`, `visitBreak`, `visitContinue`,
  `visitReturn`.
- **`TypeCheckerDecl.cpp`** — `visitVarDecl`, `visitFuncDef`,
  `visitExternFuncDecl`, `visitStructDecl`, `visitEnumDecl`,
  `visitErrorDecl`, `visitArrayDecl`, `visitArrayLiteral`,
  `visitModuleImport`, `visitSizeOf`.
- **`TypeCheckerCall.cpp`** — `visitFuncCall`, `tryUFCSFallback`,
  `visitMethodCall`, `visitFieldAccess`, `visitScopeAccess`,
  `visitArrayAccess`, `visitRecoverExpr`.

**Executed 2026-08-03.** `visitVariable` was missing from this plan's
original method list (an oversight, not a real recategorization) — it
landed in `TypeCheckerExpr.cpp`, matching where `Resolver`'s `visitVariable`
went. Like `Resolver`, `TypeChecker` is a real class, so no
`ParserInternal.h`-scale forward-declaration header was needed for its own
`visit*`/private methods — only 1 genuinely file-scope helper
(`sameTypeExact`, used by both `visitBinary` in Expr and the UFCS matching
logic in Call) needed a tiny `TypeCheckerInternal.h`.

The file also had a top section the recon pass's method-name grep had
missed entirely: an anonymous namespace holding the default error handler
(`productionErrorHandler`/`unitTestErrorHandler`, used only by the 2
`TypeChecker` constructors) and a `findRootVariable` helper (used only by
`visitAssignment`/`visitCompoundAssignment`/`visitPostfixExpr`, so it moved
to `TypeCheckerExpr.cpp`, re-wrapped in its own anonymous namespace there),
plus the constructors themselves and one more file-scope helper,
`toSourceLocation` (also present, separately, in `Resolver.cpp`).

That last one caused a real link error: batch 4 had already dropped
`static` off `Resolver.cpp`'s own `toSourceLocation` (byte-identical body:
`return {R.Line, R.Col};`) for the same cross-file reason. Two
translation units each defining an identically-named, externally-linked
`toSourceLocation(ResolverSourceLocation)` is a genuine ODR violation, only
caught at final link time (`multiple definition of toSourceLocation`), not
at either file's own compile time. Fixed per the standing instruction to
deduplicate rather than paper over: moved the single definition to be
`inline` in `ResolverSymbolBase.h` (which both passes' splits already
include transitively), removed both out-of-line copies and both
`*Internal.h` re-declarations entirely. Worth remembering for the
`Codegen.cpp` batch: check any `*Internal.h` helper's name against
`ResolverInternal.h`/`TypeCheckerInternal.h` before assuming a
similarly-named cross-file helper is safe to make non-`static` — a name
collision across passes only surfaces at link time, not compile time, so
`cmake --build` succeeding per-file is not sufficient evidence; the full
link has to actually complete.

### 6. `src/Codegen.cpp` (5287 lines) → 7 files, same boundary again

The biggest file, and the one with the most Clang precedent
(`CGExpr.cpp`/`CGExprScalar.cpp`/`CGStmt.cpp`/`CGDecl.cpp`/`CGClass.cpp`/
`CGCall.cpp`):

- **`CodegenCore.cpp`** — `InitializeModule`, `getLLVMType`,
  `CreateEntryBlockAlloca`, and the shared low-level helpers currently
  sitting at the top of the file: `getFallibleValueType`,
  `getOrCreateMallocFn`, `BoxToI64Slot`/`UnboxFromI64Slot`,
  `getOrCreateStrcmpFn`, `IsErrorTagExpr`/`GetErrorTagValue`/
  `IsErrorConstructionExpr`/`WrapFallibleValue`, `getTurfTypeFromLLVM`,
  `getTypeRank`/`getCommonType`, `isPointerLike`, `getExprType`,
  `GetDefaultValue`. Every other Codegen file depends on this one.
- **`CodegenExpr.cpp`** — `NumberExprAST`, `BoolExprAST`, `CharExprAST`,
  `StringExprAST`, `NullExprAST`, `DefaultInitExprAST`, `BinaryExprAST`,
  `UnaryExprAST`, `AddressOfExprAST`, `CompareExprAST`, `CastExprAST`,
  `PostfixExprAST`, `SizeOfExprAST`.
- **`CodegenVar.cpp`** — `VarDeclExprAST`, `VariableExprAST` (+lvalue),
  `AssignmentExprAST`, `CompoundAssignmentExprAST`.
- **`CodegenControlFlow.cpp`** — `IfExprAST`, `BlockExprAST`,
  `WhileExprAST`, `DoWhileExprAST`, `ForExprAST`, `ForEachExprAST`,
  `BreakExprAST`, `ContinueExprAST`, `ReturnExprAST`.
- **`CodegenFunc.cpp`** — `FuncDefExprAST`, `ExternFuncDeclExprAST`,
  `FuncCallExprAST`, `RecoverExprAST`, `ErrorDeclExprAST`.
- **`CodegenArray.cpp`** — `ArrayDeclExprAST`, `ArrayLiteralExprAST`,
  `ArrayAccessExprAST` (+lvalue).
- **`CodegenStruct.cpp`** — `StructDeclExprAST`, `FieldAccessExprAST`
  (+lvalue), `MethodCallExprAST`, `ScopeAccessExprAST`, `EnumDeclExprAST`,
  `ModuleImportExprAST`.

**Executed 2026-08-03.** Like `Parser.cpp`, `Codegen.cpp` is not a class —
every `NodeType::codegen()`/`codegenLValue()` method needed zero
cross-file scaffolding (already declared as part of its AST node class in
`AST.h`, visible everywhere via `#include "AST.h"`), but the ~20 file-scope
`static` helpers and global state variables at the top of the file did.
Added `CodegenInternal.h` for the ones genuinely called across categories
after tracing actual usage (not guessing): `getTurfTypeFromLLVM` and
`CastToType` turned out to be needed by every single one of the 6 non-Core
categories (the two most pervasively-used helpers in the entire file),
`getExprType` by all but Core itself, plus `UnboxFromI64Slot`/
`BuildErrorTag`/`getCommonType`/`GetDefaultValue`/`CodegenConstantValue` for
narrower cross-category sets — 9 functions total lost their `static`. Also
promoted `LLVMTypeCache` (a `static` map in the original) to `extern`:
`CodegenStruct.cpp`'s `StructDeclExprAST` pre-populates an entry in it
directly (not just through `getLLVMType()`) to support self-referential
struct types, a genuine direct cross-file data dependency, not just a
function call. 3 real bugs caught before the build was trusted clean: (1) a
first pass at the cross-category usage scan missed 5 functions
(`getOrCreateStrcmpFn`, `IsErrorTagExpr`, `GetErrorTagValue`,
`IsErrorConstructionExpr`, `WrapFallibleValue`) because the automated
call-graph script's category boundaries were right but I'd only added the
functions that scan flagged as multi-category to the declared/non-`static`
set — these 5 are each called from exactly ONE category, just a *different*
category than the one their definition physically sits in, which
`cmake --build` caught immediately as "not declared in this scope" on the
very next build (not a silent miss); (2) `CastToType`'s `TurfTypePtr`
overload had `SourceLocation Loc = {0, 0}` on both its `CodegenInternal.h`
declaration AND its `.cpp` definition — a duplicate-default-argument error,
fixed by dropping the default from the `.cpp` side, same rule as any
ordinary C++ default argument declared in a header; (3) global state
(`FuncParamTypes`/`FuncReturnTypes`/`LLVMTypeCache`/`LoopBlocks`/
`ExternDeclLocations`/`g_RequiredLibs`) was placed by checking which
category's methods actually touch each one (`LoopBlocks` turned out to be
100% `ControlFlow`-only despite sitting in the file's shared "global state"
block at the top — moved there instead of Core). Also verified beyond the
usual full-rebuild + 544-test-suite + stdlib/showcase-compile checks: ran a
compiled showcase binary and confirmed correct *runtime* output (LLVM-C FFI
self-hosting demo, exit code 0) — Codegen is the stage most likely to
produce code that compiles fine but misbehaves at runtime, so a compile-only
check isn't as reassuring here as it was for the earlier passes.

### 7. `src/CFG.cpp`/`include/CFG.h` (1050/218 lines) → construction vs. analysis

`CFGBuilder` (AST → CFG) and `CFG`'s own analysis methods are different
concerns wearing one file:

- **`CFGBuild.cpp`** — all `CFGBuilder::*` methods: `buildCFG`,
  `createBlock`, `finishBlock`, `visitExpr`/`visitBlock`/`visitIf`/
  `visitCompare`/`visitWhile`/`visitDoWhile`/`visitFor`/`visitForEach`/
  `visitReturn`/`visitBreak`/`visitContinue`.
- **`CFGAnalysis.cpp`** — all `CFG::*` methods: `createBlock`, `addEdge`,
  `computeReachability`, `getUnreachableBlocks`, `allPathsReturn`,
  `getDeadBranches`, `calculateCyclomaticComplexity`,
  `hasUnbreakableLoop`, `blockContainsSelfCall`, `allPathsRecurse`,
  `reportFlowDiagnostics`, `print`.

`CFG.h` stays as one header (both classes are small enough there), only the
`.cpp` splits.

### 8. `src/Analysis.cpp` (791 lines) → one file per dataflow pass

`DefiniteAssignmentPass`, `LivenessAnalysisPass`, and
`ConstructorFieldAssignmentPass` are three independent dataflow passes
(forward/backward, different lattices) that happen to share a file:

- **`AnalysisDefiniteAssignment.cpp`**
- **`AnalysisLiveness.cpp`**
- **`AnalysisConstructorFields.cpp`**

Lower priority than 1–7 (791 lines split 3 ways is already a modest file),
but worth doing in the same batch as `CFG.cpp` since it's the same
"analysis passes" neighborhood and reviewers will be in that code anyway.

### 9. `src/main.cpp` (1072 lines, `main()` alone ≈700 lines) → driver extraction

`main()` currently inlines the entire two-pass compile pipeline (hoisting
pre-pass, main parse, lint, resolve, typecheck, codegen, object emission,
linking, SLM/sandbox retry wiring). Split into:

- **`main.cpp`** (kept, shrinks to <100 lines) — arg parsing dispatch +
  calling into the driver.
- **`CompilerOptions.h`/`.cpp`** — the `CompilerOptions` struct + `parseArgs`.
- **`CompilerDriver.h`/`.cpp`** — the full pre-pass/main-pass pipeline
  currently inlined in `main()`: this is the biggest single lift in this
  plan in terms of *risk* (not line count) because it's one long imperative
  function with real control-flow dependencies between phases (setjmp-based
  recovery, shared `TopLevelASTs`/`FnASTs` lists) — needs the most careful
  line-by-line extraction, not a mechanical cut.
- **`ObjectEmission.h`/`.cpp`** — `emitObjectFile`, `linkObject`,
  `SkipToNextTopLevelBoundary`.

### 10. Lower-priority / optional

These are large-ish but each already has one clear responsibility; splitting
them is a smaller win and can be deferred or skipped:

- **`src/Sandbox.cpp`** (704) — could split into `SandboxCore.cpp`
  (lifecycle: ctor, capture/clone/commit/reject/log) and
  `SandboxValidation.cpp` (`validate`/`validateIncremental`/
  `isOriginalErrorResolved`/the two `run*Pass` helpers/`applyFix`/
  `classifyEditKind`/`parseDeclarationLine`/`isLocalizedEdit`/the
  `updateAST*`/`updateSymbolTable*` helpers), but it's one cohesive state
  machine and 704 lines isn't yet painful.
- **`src/SLMIntegration.cpp`** (722) — could split prompt/JSON/confidence
  helpers (`getCodeContext`, `loadDocFile`, `contextSizeForCategory`,
  `stripAnsiCodes`, `escapeJsonString`, `extractJsonFieldString`,
  `computeStructuralPrior`, `finalizeConfidence`) out of the 3 main
  entrypoints (`fetchSuggestion`, `verifySuggestion`,
  `validateAndApplySuggestion`) into `SLMPrompting.cpp`, but avoid
  fragmenting further than 2 files — this subsystem is already the least
  load-bearing part of the compiler (gated behind `--smart`).
- **`src/Lint.cpp`** (714) — one function per node kind already (mirrors the
  Resolver/TypeChecker/Codegen shape) but small enough, and has a wrinkle
  (some `Lint*` names appear as forward-decl stubs before their real
  definition later in the file) that needs a careful read before splitting,
  not a mechanical cut. Defer until after batches 1–9 prove out the pattern.

**No action needed** — already single-concern and reasonably sized:
`include/Types.h`/`src/Types.cpp` (743/316), `src/Builtins.cpp` (158),
`src/SymbolTable.cpp` (349), `src/CallGraph.cpp` (26),
`src/AliasAnalysis.cpp` (287), `src/ValueRange.cpp` (209),
`src/Algorithms.cpp` (126), `src/SLMEngine.cpp` (343),
`include/ASTPrinter.h` (531), `include/ASTVisitor.h`, `include/Colors.h`,
`include/SuggestionIndex.h`, `include/ErrorCategory.h`.

### Bonus (surfaced during recon, not required by the split)

`include/Lexer.h`/`src/Lexer.cpp` bundles the tokenizer with
`DiagnosticEngine`, which is conceptually a cross-cutting service (every
pass reports through it), not lexer-specific. Worth a follow-up
`Diagnostics.h`/`.cpp` extraction after the main split lands, not blocking.

## Batch sequencing

Batches are ordered so each one only depends on batches already done, and
each batch groups files that share the same category boundary so one
reviewer pass covers all of it:

1. ✅ **`Errors.h`** category split (base + 8 headers). Pure declarations, zero
   logic change, everything downstream includes it — doing this first means
   later batches never touch it again.
2. ✅ **`AST.h`** category split (base + 4 headers, + an unplanned 5th,
   `ASTDiff.h`, see above). Also pure declarations. Same reasoning:
   Parser/Resolver/TypeChecker/Codegen all key off this boundary, so it goes
   second.
3. ✅ **`Parser.cpp`** → 5 files + an unplanned `ParserInternal.h` (see
   above — Parser isn't a class, so cross-file calls needed real forward
   declarations, not just a header split).
4. ✅ **`Resolver.cpp`** → 5 files + a small `ResolverInternal.h` (2
   functions only, since Resolver IS a class — see above). **
   `ResolverSymbolTable`** → 3 sub-tables + an unplanned `ResolverSymbolBase.h`
   (see above — genuine circular-`#include` fix) + the composite kept as
   `ResolverSymbolTable.h`/`.cpp`.
5. ✅ **`TypeChecker.cpp`** → 5 files + a small `TypeCheckerInternal.h` (1
   function). Also fixed a cross-pass `toSourceLocation` link collision with
   batch 4's `Resolver.cpp` split (see above) by consolidating both into one
   `inline` definition in `ResolverSymbolBase.h`.
6. ✅ **`Codegen.cpp`** → 7 files + `CodegenInternal.h` (9 functions + 3
   globals — the largest of the 4 `*Internal.h` headers, matching this
   being the largest and most cross-referenced file in the plan).
7. **`CFG.cpp`** (construction/analysis split) + **`Analysis.cpp`** (3 passes)
   — same "analysis passes" neighborhood, one batch.
8. **`main.cpp`** driver extraction (`CompilerOptions` + `CompilerDriver` +
   `ObjectEmission`). Done last since it's the highest-risk lift and
   benefits from every other stage's public surface already being stable.
9. *(optional, only if requested)* `Sandbox.cpp`, `SLMIntegration.cpp`,
   `Lint.cpp` light splits.

Every batch: update `CMakeLists.txt`'s source list, rebuild
(`cmake --build build -j$(nproc)`), run `./scripts/test_runner.sh` full green
before moving to the next batch. No behavior changes in any batch — this is
a pure file-organization refactor; if a diff in any batch does anything
other than move code and adjust includes, that's a bug to back out, not a
drive-by fix to keep.
