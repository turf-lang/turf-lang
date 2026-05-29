/**
 * @file Resolver.h
 * @brief The Resolver pass: name resolution before type checking or codegen.
 *
 * Issue #4 of the monolith → multi-pass migration.
 *
 * Implements `class Resolver : public ASTVisitor`.  A single call to
 * `Resolver::resolve(root)` walks the entire AST and enforces that:
 *
 *  1. Every variable is declared before it is used (visitVariable).
 *  2. Block boundaries introduce a new lexical scope (visitBlock).
 *  3. VarDecl nodes register their name in the current scope (visitVarDecl).
 *
 * Error handling strategy:
 *  - `visitVarDecl` — raises DuplicateDeclarationError if the same name is
 *    re-declared in the *same* scope.  Shadowing an outer scope is allowed
 *    (outer-scope duplicate check is done via lookupVariable, not
 *    isDeclaredInCurrentScope).
 *  - `visitVariable` — raises ReferenceError (with Damerau-Levenshtein
 *    suggestions) if the variable name is not found in any enclosing scope.
 *
 * The pass runs in "shadow mode" for now (Issue #4 acceptance criterion):
 * Codegen still performs its own symbol lookups.  The Resolver merely
 * validates and pre-populates the ResolverSymbolTable so that Issues #5+
 * can attach resolved symbols to AST nodes.
 *
 * Design notes:
 *  - The Resolver owns its ResolverSymbolTable instance.
 *  - All child dispatch is done via `child->accept(*this)` so that every
 *    node type is recursed automatically without duplicating traversal code.
 *  - Only the three visit methods listed in Issue #4 contain logic; all
 *    other nodes are dispatched to children and nothing else.
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

  void visitBinary(BinaryExprAST &Node) override;
  void visitUnary(UnaryExprAST &Node) override;
  void visitAssignment(AssignmentExprAST &Node) override;
  void visitCast(CastExprAST &Node) override;

  void visitIf(IfExprAST &Node) override;
  void visitWhile(WhileExprAST &Node) override;
  void visitFor(ForExprAST &Node) override;
  void visitBreak(BreakExprAST &Node) override { (void)Node; }
  void visitContinue(ContinueExprAST &Node) override { (void)Node; }
  void visitReturn(ReturnExprAST &Node) override;
  void visitRaise(RaiseExprAST &Node) override;
  void visitTry(TryExprAST &Node) override;

  void visitFuncDef(FuncDefExprAST &Node) override;
  void visitFuncCall(FuncCallExprAST &Node) override;
  void visitMethodCall(MethodCallExprAST &Node) override;

  void visitFieldAccess(FieldAccessExprAST &Node) override;
  void visitStructDecl(StructDeclExprAST &Node) override { (void)Node; }
  void visitStructInit(StructInitExprAST &Node) override;

  void visitArrayDecl(ArrayDeclExprAST &Node) override;
  void visitArrayAccess(ArrayAccessExprAST &Node) override;
  void visitArrayAssign(ArrayAssignExprAST &Node) override;
  void visitArrayLength(ArrayLengthExprAST &Node) override { (void)Node; }

  void visitModuleImport(ModuleImportExprAST &Node) override;
  void visitSizeOf(SizeOfExprAST &Node) override { (void)Node; }

  // ── Accessors ──────────────────────────────────────────────────────────────

  /// Read-only access to the symbol table (e.g. for unit tests).
  const ResolverSymbolTable &symbolTable() const { return SymTable_; }

private:
  ResolverSymbolTable SymTable_;

  // ── Internal helpers ───────────────────────────────────────────────────────

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
