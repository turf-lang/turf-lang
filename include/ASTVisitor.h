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
 *  - A pass whose correctness depends on seeing EVERY node (a lint, a
 *    liveness walk -- anything where a skipped node is a silent false
 *    negative or false positive) derives from ExhaustiveASTVisitor instead
 *    (LEC-005, INV-LINT-1): there, "I did not think about this node" is a
 *    compile error rather than a no-op.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef AST_VISITOR_H
#define AST_VISITOR_H

// ─── Forward declarations (all AST node types) ───────────────────────────────
class NumberExprAST;
class BoolExprAST;
class CharExprAST;
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
class UnsafeBlockExprAST;
class IfExprAST;
class WhileExprAST;
class DoWhileExprAST;
class ForExprAST;
class ForEachExprAST;
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
class SliceExprAST;
class FieldAccessExprAST;
class EnumDeclExprAST;
class MachineDeclExprAST;
class ScopeAccessExprAST;
class ErrorDeclExprAST;
class RecoverExprAST;
class TryExprAST;
class DeferExprAST;
class StructDeclExprAST;
class ModuleImportExprAST;
class SizeOfExprAST;
class CallerLocationExprAST;
class ExternFuncDeclExprAST;
class AddressOfExprAST;
class CompareExprAST;
class WildcardPatternExprAST;
class BindingPatternExprAST;
class StructPatternExprAST;
class ErrorVariantPatternExprAST;


// ─── The node list (single source of truth) ───────────────────────────────────

/**
 * @brief Every AST node kind a visitor can be dispatched on, as
 *        `X(<method suffix>, <node class>)` -- one row per `accept()`.
 *
 * ASTVisitor's no-op methods AND ExhaustiveASTVisitor's pure-virtual ones are
 * both generated from this one list, so the two cannot drift: a node kind
 * added here (and given an `accept()`) becomes a compile error in every
 * exhaustive pass until that pass decides what to do with it. Keep it in
 * step with the `accept()` methods in AST*.h -- a node whose `accept()` calls
 * a method that is not listed here does not compile, which is the check.
 */
#define TURF_AST_NODE_LIST(X)                                                  \
  /* Literals */                                                               \
  X(Number, NumberExprAST)                                                     \
  X(Bool, BoolExprAST)                                                         \
  X(Char, CharExprAST)                                                         \
  X(String, StringExprAST)                                                     \
  X(Null, NullExprAST)                                                         \
  /* Variables & declarations */                                               \
  X(Variable, VariableExprAST)                                                 \
  X(VarDecl, VarDeclExprAST)                                                   \
  /* Expressions */                                                            \
  X(Binary, BinaryExprAST)                                                     \
  X(Unary, UnaryExprAST)                                                       \
  X(AddressOf, AddressOfExprAST)                                               \
  X(Assignment, AssignmentExprAST)                                             \
  X(CompoundAssignment, CompoundAssignmentExprAST)                             \
  X(PostfixExpr, PostfixExprAST)                                               \
  X(Cast, CastExprAST)                                                         \
  /* Control flow */                                                           \
  X(Block, BlockExprAST)                                                       \
  X(UnsafeBlock, UnsafeBlockExprAST)                                           \
  X(If, IfExprAST)                                                             \
  X(While, WhileExprAST)                                                       \
  X(DoWhile, DoWhileExprAST)                                                   \
  X(For, ForExprAST)                                                           \
  X(ForEach, ForEachExprAST)                                                   \
  X(Break, BreakExprAST)                                                       \
  X(Continue, ContinueExprAST)                                                 \
  X(Return, ReturnExprAST)                                                     \
  X(Compare, CompareExprAST)                                                   \
  X(Defer, DeferExprAST)                                                       \
  /* Destructuring patterns (LEC-003): children of a CompareExprAST arm       \
     header. A visitor that cares what a header READS or DEFINES must          \
     implement these -- the no-op default is exactly how a pattern's bindings  \
     would go unseen. */                                                       \
  X(WildcardPattern, WildcardPatternExprAST)                                   \
  X(BindingPattern, BindingPatternExprAST)                                     \
  X(StructPattern, StructPatternExprAST)                                       \
  X(ErrorVariantPattern, ErrorVariantPatternExprAST)                           \
  /* Functions & calls */                                                      \
  X(FuncDef, FuncDefExprAST)                                                   \
  X(FuncCall, FuncCallExprAST)                                                 \
  X(MethodCall, MethodCallExprAST)                                             \
  /* Structs, enums, errors & field access */                                  \
  X(FieldAccess, FieldAccessExprAST)                                           \
  X(EnumDecl, EnumDeclExprAST)                                                 \
  X(MachineDecl, MachineDeclExprAST)                                           \
  X(ScopeAccess, ScopeAccessExprAST)                                           \
  X(ErrorDecl, ErrorDeclExprAST)                                               \
  X(RecoverExpr, RecoverExprAST)                                               \
  X(Try, TryExprAST)                                                           \
  X(StructDecl, StructDeclExprAST)                                             \
  /* Arrays */                                                                 \
  X(ArrayDecl, ArrayDeclExprAST)                                               \
  X(ArrayLiteral, ArrayLiteralExprAST)                                         \
  X(ArrayAccess, ArrayAccessExprAST)                                           \
  X(Slice, SliceExprAST)                                                       \
  /* Modules & misc */                                                         \
  X(ModuleImport, ModuleImportExprAST)                                         \
  X(SizeOf, SizeOfExprAST)                                                     \
  X(CallerLocation, CallerLocationExprAST)                                     \
  /* FFI */                                                                    \
  X(ExternFuncDecl, ExternFuncDeclExprAST)

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

#define TURF_DECLARE_VISIT_NOOP(Name, Class) \
  virtual void visit##Name(Class &) {}
  TURF_AST_NODE_LIST(TURF_DECLARE_VISIT_NOOP)
#undef TURF_DECLARE_VISIT_NOOP
};

// ─── ExhaustiveASTVisitor ─────────────────────────────────────────────────────

/**
 * @brief An ASTVisitor that must say something about EVERY node kind.
 *
 * LEC-005 / INV-LINT-1. The base class's no-op defaults are the right
 * trade-off for a pass that only cares about a few nodes, and exactly the
 * wrong one for a pass whose job is to look at everything: `LintExpression`
 * was a chain of independent `dynamic_cast`s, so a node kind nobody wrote a
 * branch for -- a call argument, an `unsafe` block, a `defer`, an array
 * literal -- was skipped without a trace, and `mut x: int = 5 / 0;` once
 * warned about nothing. Here every `visit*` is pure virtual, so adding a
 * node kind to TURF_AST_NODE_LIST breaks the build of each exhaustive pass
 * until its author decides what the pass does with it (a leaf writes `{}`
 * and says why; a container recurses).
 *
 * Opt a pass in only when a skipped node is a defect; a partial pass (the
 * Resolver, the TypeChecker, a printer for a few kinds) should stay on
 * ASTVisitor.
 */
class ExhaustiveASTVisitor : public ASTVisitor {
public:
#define TURF_DECLARE_VISIT_PURE(Name, Class) \
  void visit##Name(Class &) override = 0;
  TURF_AST_NODE_LIST(TURF_DECLARE_VISIT_PURE)
#undef TURF_DECLARE_VISIT_PURE
};

#endif // AST_VISITOR_H
