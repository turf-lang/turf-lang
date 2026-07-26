/**
 * @file Resolver.h
 * @brief The Resolver pass: name resolution before type checking or codegen.
 *
 * Issue #18 — Two-Pass Resolver for Forward Declarations.
 *
 * Implements `class Resolver : public ASTVisitor`. A single call to
 * `Resolver::resolveAll(nodes)` walks the AST in two distinct passes:
 *
 *  **Pass 1 — Hoisting** (`hoistTopLevel`):
 *    Walk only top-level AST nodes (no recursion into bodies).
 *    For each `StructDeclExprAST`: call `SymTable_.registerStruct(Name)`.
 *    For each `FuncDefExprAST`: call `SymTable_.registerCurrentFileFunction(sym)`
 *      — only the signature (name, params, returnType), NOT the body.
 *    For each `ModuleImportExprAST`: register all functions in the module.
 *
 *  **Pass 2 — Full Resolution** (existing visitor behavior):
 *    Walk the entire AST, resolve all variable references, function calls,
 *    and type annotations. Because Pass 1 already registered all structs and
 *    functions, forward references resolve correctly.
 *
 * This two-pass design is why the compiler no longer needs
 * `GlobalSymbolTable->LookupType()` in `ParseType()`: the parser creates a
 * `NamedType("MyStruct")` placeholder, and the Resolver's Pass 2 validates
 * it by looking up the name that was registered in Pass 1.
 *
 * Error handling strategy:
 *  - `visitVarDecl` — raises DuplicateDeclarationError if the same name is
 *    re-declared in the *same* scope. Shadowing an outer scope is allowed.
 *  - `visitVariable` — raises ReferenceError (with Damerau-Levenshtein
 *    suggestions) if the variable name is not found in any enclosing scope.
 *  - `visitStructDecl` — validates that all NamedType field references
 *    (including pointer-to-named-type fields) refer to structs registered in
 *    Pass 1. Primitive types and void are always allowed. Type-level
 *    validation (field types, initialization) is the TypeChecker's job.
 *
 * Design notes:
 *  - The Resolver owns its ResolverSymbolTable instance.
 *  - All child dispatch is done via `child->accept(*this)` so that every
 *    node type is recursed automatically without duplicating traversal code.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef RESOLVER_H
#define RESOLVER_H

#include "AST.h"
#include "ASTVisitor.h"
#include "ResolverSymbolTable.h"

#include <memory>
#include <vector>

/**
 * @brief Name-resolution pass for the Turflang multi-pass compiler pipeline.
 *
 * Usage:
 * @code
 *   Resolver resolver;
 *   resolver.resolve(programRoot);   // throws on the first error found
 * @endcode
 *
 * The Resolver walks the whole AST once before Codegen runs.  In the current
 * "shadow mode" (Phase 2 of the migration plan) its output is not yet fed into
 * Codegen; the pass is merely run for early error detection.
 */
class Resolver : public ASTVisitor {
public:
  Resolver() = default;

  // ── Public entry points ────────────────────────────────────────────────────

  /**
   * @brief Resolve a single top-level expression / statement.
   *
   * Accepts any ExprAST node and dispatches via the visitor pattern.
   * Typically called once per top-level node produced by the parser.
   */
  void resolve(ExprAST *Node);

  /**
   * @brief Resolve an entire program (list of top-level nodes).
   *
   * Convenience wrapper that iterates over a vector of unique_ptr<ExprAST>
   * and calls resolve() on each one in order.
   */
  void resolveAll(const std::vector<std::unique_ptr<ExprAST>> &Nodes);

  /**
   * @brief Reset internal scope tracking after a longjmp recovery.
   *
   * Must be called before resuming resolution on subsequent top-level nodes
   * if a previous node raised an error caught by `setjmp`.
   * Warning: Do not loop-with-recovery over multiple top-level nodes without
   * resetting scope depth and FunctionNestingDepth_ between iterations.
   */
  void resetForRecovery() {
    FunctionNestingDepth_ = 0;
    SymTable_.resetToDepth(0);
  }

  // ── ASTVisitor overrides ───────────────────────────────────────────────────

  // ── Variables & Declarations ──────────────────────────────────────────────

  /**
   * @brief Register the declared variable in the current scope.
   *
   * Also recurses into the initializer expression (if present) so that
   * uses of variables inside the initializer are validated.
   *
   * Raises DuplicateDeclarationError if the name is already declared in the
   * *current* (innermost) scope.
   */
  void visitVarDecl(VarDeclExprAST &Node) override;

  /**
   * @brief Check that the referenced variable exists in an enclosing scope.
   *
   * Raises ReferenceError with Damerau-Levenshtein suggestions if the name
   * cannot be found in any scope from innermost to outermost.
   */
  void visitVariable(VariableExprAST &Node) override;

  // ── Control flow ──────────────────────────────────────────────────────────

  /**
   * @brief Walk a block, wrapping it in enterScope() / exitScope().
   *
   * Every statement inside the block is recursed via accept(*this).
   */
  void visitBlock(BlockExprAST &Node) override;

  // ── All other node types — just recurse into children ─────────────────────

  void visitNumber(NumberExprAST &Node) override { (void)Node; }
  void visitBool(BoolExprAST &Node) override { (void)Node; }
  void visitString(StringExprAST &Node) override { (void)Node; }
  void visitNull(NullExprAST &Node) override { (void)Node; }
  void visitDefaultInit(DefaultInitExprAST &Node) override { (void)Node; }

  void visitBinary(BinaryExprAST &Node) override;
  void visitUnary(UnaryExprAST &Node) override;
  void visitAddressOf(AddressOfExprAST &Node) override;
  void visitAssignment(AssignmentExprAST &Node) override;
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override;
  void visitPostfixExpr(PostfixExprAST &Node) override;
  void visitCast(CastExprAST &Node) override;

  void visitIf(IfExprAST &Node) override;
  void visitWhile(WhileExprAST &Node) override;
  void visitFor(ForExprAST &Node) override;
  void visitBreak(BreakExprAST &Node) override { (void)Node; }
  void visitContinue(ContinueExprAST &Node) override { (void)Node; }
  void visitReturn(ReturnExprAST &Node) override;
  void visitCompare(CompareExprAST &Node) override;

  void visitFuncDef(FuncDefExprAST &Node) override;
  void visitFuncCall(FuncCallExprAST &Node) override;
  void visitMethodCall(MethodCallExprAST &Node) override;

  void visitFieldAccess(FieldAccessExprAST &Node) override;
  void visitStructDecl(StructDeclExprAST &Node) override;
  void visitEnumDecl(EnumDeclExprAST &Node) override;
  void visitScopeAccess(ScopeAccessExprAST &Node) override;
  void visitStructInit(StructInitExprAST &Node) override;

  void visitArrayDecl(ArrayDeclExprAST &Node) override;
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override;
  void visitArrayAccess(ArrayAccessExprAST &Node) override;
  void visitModuleImport(ModuleImportExprAST &Node) override;
  void visitSizeOf(SizeOfExprAST &Node) override { (void)Node; }

  // ── FFI ────────────────────────────────────────────────────────────────────
  void visitExternFuncDecl(ExternFuncDeclExprAST &Node) override;

  // ── Accessors ──────────────────────────────────────────────────────────────

  /// Read-only access to the symbol table (e.g. for unit tests).
  const ResolverSymbolTable &symbolTable() const { return SymTable_; }

private:
  ResolverSymbolTable SymTable_;
  int FunctionNestingDepth_ = 0;
  std::vector<std::string> ModuleStack_;
  // The enclosing function's own `fn<...>` type parameter names, set for the
  // duration of visitFuncDef()'s body traversal and cleared on exit (a plain
  // member, not a stack, is safe since FunctionNestingDepth_ already forbids
  // nested function definitions -- see NestedFunctionError). Consulted by
  // visitVarDecl()'s validateNamedType() call so a bare generic type
  // parameter used as a local variable's declared type (`fn<T> f() { mut x:
  // T = ...; }`) isn't wrongly flagged as an unknown type.
  std::vector<std::string> CurrentFuncTypeParams_;

  // ── Internal helpers ───────────────────────────────────────────────────────

  std::string currentModuleName() const {
    return ModuleStack_.empty() ? std::string() : ModuleStack_.back();
  }

  ResolverFunctionSymbol buildFunctionSymbol(const FuncDefExprAST &Node,
                                             const std::string &ModuleName) const;

  /**
   * @brief Validate that a type reference (field, parameter, or return type)
   *        names either a known primitive, a struct/enum registered in
   *        Pass 1, or one of the enclosing declaration's own generic type
   *        parameters, raising UnknownTypeError otherwise.
   *
   * Shared by visitStructDecl (field types), visitFuncDef (parameter and
   * return types), and visitExternFuncDecl (parameter and return types) so
   * all four positions get identical, complete validation instead of only
   * struct fields being checked — see the doc comment on the .cpp definition
   * for the concrete gap this closes (an unknown struct name in a function
   * signature previously compiled with zero diagnostics).
   *
   * @param Ty         The parsed type to validate. No-op if null.
   * @param Loc        Source location to attach to any raised diagnostic.
   * @param TypeParams Names declared in the enclosing `fn<...>`/`struct<...>`
   *                   parameter list (e.g. {"T"} for `fn<T> identity(x: T)`).
   *                   ParseType() has no way to know at parse time that a
   *                   bare identifier like `T` names a generic parameter
   *                   rather than a struct, so it always produces a plain
   *                   NamedType("T") -- never a TurfGenericParamType -- for
   *                   type ANNOTATIONS (as opposed to the GlobalSymbolTable
   *                   registration ParseFuncDef/ParseStructDecl perform for
   *                   Codegen's benefit, which is a separate, LLVM-aware
   *                   table this LLVM-free pass does not consult). Without
   *                   this exemption list, every generic function or struct
   *                   using its own type parameter as a field/parameter/
   *                   return type (e.g. `fn<T> identity(x: T): T`) is
   *                   incorrectly rejected as "Unknown type 'T'". Defaults to
   *                   empty for call sites with no type parameters of their
   *                   own (currently visitExternFuncDecl).
   */
  void validateNamedType(const TurfTypePtr &Ty, SourceLocation Loc,
                         const std::vector<std::string> &TypeParams = {}) const;

  /// Validates an explicit `Module::TypeName` qualifier (from NamedType::
  /// getModule()/GenericType::getModule(), or a qualified struct-literal's
  /// ModuleQualifier) once @p TypeName is already confirmed to be a real,
  /// known struct/enum. A no-op (returns true) if @p Qualifier is empty --
  /// an unqualified reference has nothing to validate. Raises
  /// UnknownScopeQualifierError if @p Qualifier isn't a known module at
  /// all, or UnknownModuleSymbolError if it IS a real module but doesn't
  /// own @p TypeName (e.g. `mem::List` when List actually lives in
  /// `list`). Returns false in either error case (already raised), true
  /// if the qualifier is empty or correct.
  bool validateModuleQualifier(const std::string &Qualifier,
                               const std::string &TypeName,
                               SourceLocation Loc) const;

  // ── Pass 1: Hoisting ───────────────────────────────────────────────────────
  // Walk only the top-level list, registering struct names and function
  // signatures.  No bodies are visited in this pass.

  /// Hoist a single top-level node (struct name or function signature).
  void hoistTopLevel(const ExprAST *Node);

  /// Hoist an entire list of top-level nodes (convenience overload for
  /// resolveAll).
  void hoistTopLevel(const std::vector<std::unique_ptr<ExprAST>> &Nodes);

  /// Register all function signatures found inside an imported module body.
  /// Called by hoistTopLevel when it encounters a ModuleImportExprAST.
  void hoistModuleBody(const ExprAST *Body, const std::string &ModuleName);

  /// Processes an on-demand import's requested symbols (ImportMode::Symbol/
  /// SymbolList/Wildcard) against the already-hoisted target module,
  /// registering each into ContextLabel's flat namespace ("" = main file,
  /// else the enclosing module doing the importing). No-op for
  /// ImportMode::Bare. Called right after hoistModuleBody() has fully
  /// populated the target module's own registrations, from both
  /// hoistTopLevel (ContextLabel="") and hoistModuleBody's own nested-
  /// import case (ContextLabel=the enclosing module's name).
  void registerOnDemandImports(const ModuleImportExprAST &Import,
                               const std::string &ContextLabel);

  /// Register one function's signature (name, params, return type) in
  /// SymTable_ under the given module namespace ("\'" = current file).
  void registerFunctionSignature(const FuncDefExprAST &Node,
                                 const std::string &ModuleName);

  /// Dispatch to @p Node if non-null (no-op otherwise).
  void visitChild(ExprAST *Node) {
    if (Node)
      Node->accept(*this);
  }

  /// Convenience: dispatch a const child.
  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }
};

#endif // RESOLVER_H
