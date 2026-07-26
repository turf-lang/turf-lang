/**
 * @file ASTVisitor.h
 * @brief Defines the ASTVisitor interface for the multi-pass architecture.
 *
 * This is the core infrastructure for Issue #1 of the monolith → multi-pass
 * migration.  Every AST node class gains an `accept(ASTVisitor &)` method,
 * and concrete passes (Resolver, TypeChecker, ASTPrinter, …) derive from
 * ASTVisitor and override only the visit methods they care about.
 *
 * Design notes:
 *  - Default implementations in the base class are no-ops so that a pass
 *    only needs to override the nodes it actually cares about.
 *  - The visitor pattern separates traversal logic from node data, which is
 *    exactly what we need for the multi-pass pipeline.
 *  - Forward-declare every node type here to keep compile-time dependencies
 *    minimal; include AST.h only in .cpp files that need the full definition.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef AST_VISITOR_H
#define AST_VISITOR_H

// ─── Forward declarations (all AST node types) ───────────────────────────────
class NumberExprAST;
class BoolExprAST;
class StringExprAST;
class NullExprAST;
class VariableExprAST;
class BinaryExprAST;
class UnaryExprAST;
class AssignmentExprAST;
class CompoundAssignmentExprAST;
class PostfixExprAST;
class VarDeclExprAST;
class CastExprAST;
class BlockExprAST;
class IfExprAST;
class WhileExprAST;
class ForExprAST;
class BreakExprAST;
class ContinueExprAST;
class ReturnExprAST;

class FuncDefExprAST;
class FuncCallExprAST;
class MethodCallExprAST;
class FieldAccessExprAST;
class ArrayDeclExprAST;
class ArrayLiteralExprAST;
class ArrayAccessExprAST;
class FieldAccessExprAST;
class EnumDeclExprAST;
class ScopeAccessExprAST;
class StructDeclExprAST;
class StructInitExprAST;
class ModuleImportExprAST;
class SizeOfExprAST;
class ExternFuncDeclExprAST;
class AddressOfExprAST;
class DefaultInitExprAST;
class CompareExprAST;


// ─── ASTVisitor base class ────────────────────────────────────────────────────

/**
 * @brief Abstract visitor interface for the Turflang AST.
 *
 * Concrete passes (e.g. Resolver, TypeChecker, ASTPrinter) subclass this and
 * override the visit* methods they need.  Methods that are not overridden
 * default to a no-op, which is safe for passes that only care about a subset
 * of node types.
 *
 * Usage:
 * @code
 *   class MyPass : public ASTVisitor {
 *   public:
 *     void visitVarDecl(VarDeclExprAST &Node) override { … }
 *   };
 *
 *   MyPass pass;
 *   someNode->accept(pass);
 * @endcode
 */
class ASTVisitor {
public:
  virtual ~ASTVisitor() = default;

  // ── Literals ──────────────────────────────────────────────────────────────
  virtual void visitNumber(NumberExprAST &Node) {}
  virtual void visitBool(BoolExprAST &Node) {}
  virtual void visitString(StringExprAST &Node) {}
  virtual void visitNull(NullExprAST &Node) {}
  virtual void visitDefaultInit(DefaultInitExprAST &Node) {}

  // ── Variables & Declarations ──────────────────────────────────────────────
  virtual void visitVariable(VariableExprAST &Node) {}
  virtual void visitVarDecl(VarDeclExprAST &Node) {}

  // ── Expressions ───────────────────────────────────────────────────────────
  virtual void visitBinary(BinaryExprAST &Node) {}
  virtual void visitUnary(UnaryExprAST &Node) {}
  virtual void visitAddressOf(AddressOfExprAST &Node) {}
  virtual void visitAssignment(AssignmentExprAST &Node) {}
  virtual void visitCompoundAssignment(CompoundAssignmentExprAST &Node) {}
  virtual void visitPostfixExpr(PostfixExprAST &Node) {}
  virtual void visitCast(CastExprAST &Node) {}

  // ── Control flow ──────────────────────────────────────────────────────────
  virtual void visitBlock(BlockExprAST &Node) {}
  virtual void visitIf(IfExprAST &Node) {}
  virtual void visitWhile(WhileExprAST &Node) {}
  virtual void visitFor(ForExprAST &Node) {}
  virtual void visitBreak(BreakExprAST &Node) {}
  virtual void visitContinue(ContinueExprAST &Node) {}
  virtual void visitReturn(ReturnExprAST &Node) {}
  virtual void visitCompare(CompareExprAST &Node) {}


  // ── Functions & Calls ─────────────────────────────────────────────────────
  virtual void visitFuncDef(FuncDefExprAST &Node) {}
  virtual void visitFuncCall(FuncCallExprAST &Node) {}
  virtual void visitMethodCall(MethodCallExprAST &Node) {}

  // ── Structs & Field access ────────────────────────────────────────────────
  virtual void visitFieldAccess(FieldAccessExprAST &Node) {}
  virtual void visitEnumDecl(EnumDeclExprAST &Node) {}
  virtual void visitScopeAccess(ScopeAccessExprAST &Node) {}
  virtual void visitStructDecl(StructDeclExprAST &Node) {}
  virtual void visitStructInit(StructInitExprAST &Node) {}

  // ── Arrays ────────────────────────────────────────────────────────────────
  virtual void visitArrayDecl(ArrayDeclExprAST &Node) {}
  virtual void visitArrayLiteral(ArrayLiteralExprAST &Node) {}
  virtual void visitArrayAccess(ArrayAccessExprAST &Node) {}
  // ── Modules & misc ────────────────────────────────────────────────────────
  virtual void visitModuleImport(ModuleImportExprAST &Node) {}
  virtual void visitSizeOf(SizeOfExprAST &Node) {}

  // ── FFI ────────────────────────────────────────────────────────────────────
  virtual void visitExternFuncDecl(ExternFuncDeclExprAST &Node) {}
};

#endif // AST_VISITOR_H
