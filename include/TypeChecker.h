/**
 * @file TypeChecker.h
 * @brief Turflang TypeChecker pass — Issue #7 (monolith → multi-pass migration).
 *
 * Implements `class TypeChecker : public ASTVisitor`.
 *
 * Pipeline position
 * ─────────────────
 *   Parser  →  Resolver  →  **TypeChecker**  →  Codegen
 *
 * The TypeChecker runs *after* the Resolver.  It walks the AST bottom-up,
 * infers a TurfTypePtr for every expression node, and raises Type Mismatch
 * errors for illegal operations (e.g. `string + int`).
 *
 * Design constraints (Shadow Mode — Phase 2 of migration plan)
 * ─────────────────────────────────────────────────────────────
 *  - LLVM-free at the header level.  No LLVM headers are included here.
 *  - The TypeChecker does NOT replace Codegen yet; it runs in parallel and its
 *    output (inferred types stored in NodeTypes_) will gradually replace the
 *    equivalent monolithic type logic in Codegen.
 *  - Error reporting uses a pluggable callback so the class can be unit-tested
 *    without linking against the full LLVM error machinery (Errors.h pulls in
 *    LLVM via Codegen.h).  Production code installs a real TypeError callback;
 *    unit tests install a std::runtime_error thrower.
 *
 * ForExprAST prerequisite (issue #135 note)
 * ─────────────────────────────────────────
 *  The issue requires that `for` loop variables are properly typed before the
 *  TypeChecker body traversal.  This is handled entirely inside visitFor():
 *  the loop variable is inserted into LoopVarTypes_ as `int` (inferred from
 *  the integer range bounds), visible to visitVariable() during the body walk,
 *  and removed when the for-loop node is fully visited.
 *
 * Usage
 * ─────
 * @code
 *   // 1. Build the symbol table (Resolver pass fills it).
 *   ResolverSymbolTable rst;
 *   // ... Resolver runs here ...
 *
 *   // 2. Create the TypeChecker (production — real errors).
 *   TypeChecker tc(rst);
 *
 *   // 3. Walk the AST.
 *   rootNode->accept(tc);
 *
 *   // 4. Query any node's inferred type.
 *   TurfTypePtr ty = tc.getType(someNode);
 * @endcode
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef TYPE_CHECKER_H
#define TYPE_CHECKER_H

#include "ASTVisitor.h"
#include "ResolverSymbolTable.h"
#include "Types.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declared; full definition not required here (LLVM-free header).
struct SourceLocation;

// ExprAST is the base of all AST nodes.  We only need a forward declaration
// here because TypeChecker.h stores ExprAST* as map keys and exposes getType /
// hasType queries.  The full definition lives in AST.h (which is LLVM-dependent)
// and is included only in TypeChecker.cpp.
class ExprAST;

// ─── TypeChecker ─────────────────────────────────────────────────────────────

/**
 * @brief Type-inference and type-checking pass for the Turflang compiler.
 *
 * Concrete ASTVisitor that walks the AST and annotates every expression node
 * with its inferred TurfType.  Raises a type-mismatch error if an illegal
 * binary or unary operation is encountered.
 *
 * Thread safety: NOT thread-safe.  One TypeChecker instance per compilation.
 */
class TypeChecker : public ASTVisitor {
public:
  // ── Error callback type ───────────────────────────────────────────────────
  //
  // Signature: (SourceLocation, error message string)
  // The callback is expected to terminate the current compilation path (either
  // via longjmp / std::exit in production, or by throwing in unit tests).
  // After the callback returns the TypeChecker inserts an 'unknown' type for
  // the offending node and continues so that further errors can still be found.
  using ErrorHandler =
      std::function<void(SourceLocation, const std::string &)>;

  // ── Constructors ──────────────────────────────────────────────────────────

  /**
   * @brief Construct with a pre-populated ResolverSymbolTable.
   *
   * Installs the *production* error handler by default, which calls
   * TypeError::raise() from Errors.h.  Override with setErrorHandler() for
   * unit tests.
   *
   * @param RST  Symbol table populated by the Resolver pass.  Not owned; must
   *             outlive this TypeChecker.
   */
  explicit TypeChecker(const ResolverSymbolTable &RST);

  /**
   * @brief Construct with a custom error handler (for unit tests / fuzz).
   *
   * Installs @p Handler instead of the default TypeError callback.
   */
  TypeChecker(const ResolverSymbolTable &RST, ErrorHandler Handler);

  // ── Error handler replacement ─────────────────────────────────────────────

  /// Replace the error handler after construction (useful in tests).
  void setErrorHandler(ErrorHandler Handler) {
    ErrorHandler_ = std::move(Handler);
  }

  // ── Type query ────────────────────────────────────────────────────────────

  /**
   * @brief Return the inferred type for a node visited earlier.
   *
   * Returns nullptr if @p Node has not been visited or if inference failed.
   */
  TurfTypePtr getType(const ExprAST *Node) const;

  /**
   * @brief Return true if the given node has a recorded type.
   */
  bool hasType(const ExprAST *Node) const;

  // ── ASTVisitor overrides ──────────────────────────────────────────────────

  // ── Literals ─────────────────────────────────────────────────────────────
  void visitNumber(NumberExprAST &Node) override;
  void visitBool(BoolExprAST &Node) override;
  void visitString(StringExprAST &Node) override;
  void visitNull(NullExprAST &Node) override;

  // ── Variables & declarations ──────────────────────────────────────────────
  void visitVariable(VariableExprAST &Node) override;
  void visitVarDecl(VarDeclExprAST &Node) override;

  // ── Expressions ───────────────────────────────────────────────────────────
  void visitBinary(BinaryExprAST &Node) override;
  void visitUnary(UnaryExprAST &Node) override;
  void visitAssignment(AssignmentExprAST &Node) override;
  void visitCast(CastExprAST &Node) override;

  // ── Control flow ──────────────────────────────────────────────────────────
  void visitBlock(BlockExprAST &Node) override;
  void visitIf(IfExprAST &Node) override;
  void visitWhile(WhileExprAST &Node) override;
  void visitFor(ForExprAST &Node) override;
  void visitBreak(BreakExprAST &Node) override;
  void visitContinue(ContinueExprAST &Node) override;
  void visitReturn(ReturnExprAST &Node) override;
  void visitRaise(RaiseExprAST &Node) override;
  void visitTry(TryExprAST &Node) override;

  // ── Functions & calls ─────────────────────────────────────────────────────
  void visitFuncDef(FuncDefExprAST &Node) override;
  void visitFuncCall(FuncCallExprAST &Node) override;
  void visitMethodCall(MethodCallExprAST &Node) override;

  // ── Structs & field access ────────────────────────────────────────────────
  void visitFieldAccess(FieldAccessExprAST &Node) override;
  void visitStructDecl(StructDeclExprAST &Node) override;
  void visitStructInit(StructInitExprAST &Node) override;

  // ── Arrays ────────────────────────────────────────────────────────────────
  void visitArrayDecl(ArrayDeclExprAST &Node) override;
  void visitArrayAccess(ArrayAccessExprAST &Node) override;
  void visitArrayAssign(ArrayAssignExprAST &Node) override;
  void visitArrayLength(ArrayLengthExprAST &Node) override;

  // ── Modules & misc ────────────────────────────────────────────────────────
  void visitModuleImport(ModuleImportExprAST &Node) override;
  void visitSizeOf(SizeOfExprAST &Node) override;

private:
  // ── Internal helpers ──────────────────────────────────────────────────────

  /// Record the inferred type for @p Node.
  void setType(const ExprAST *Node, TurfTypePtr Ty);

  /// Fire the error handler with the given location and message.
  void reportError(SourceLocation Loc, const std::string &Msg);

  /**
   * @brief Infer the result type of a binary arithmetic/string operation.
   *
   * Rules (in priority order):
   *  1. string OP string  →  string  (only for '+')
   *  2. string OP numeric →  TypeError
   *  3. numeric OP string →  TypeError
   *  4. double OP double  →  double
   *  5. int    OP int     →  int
   *  6. int    OP double  →  double  (numeric promotion)
   *  7. double OP int     →  double  (numeric promotion)
   *  8. bool   OP bool    →  int     (bool promotes to int for arithmetic)
   *
   * @param Loc        Source location for error reporting.
   * @param Op         Operator character ('+', '-', '*', '/', '%').
   * @param LhsTy      Inferred LHS type (may be nullptr → treated as unknown).
   * @param RhsTy      Inferred RHS type (may be nullptr → treated as unknown).
   * @param ResultOut  Populated with the result type on success.
   * @return true if inference succeeded without errors.
   */
  bool inferArithmeticType(SourceLocation Loc, int Op,
                           const TurfType *LhsTy, const TurfType *RhsTy,
                           TurfTypePtr &ResultOut);

  /**
   * @brief Infer the result type of a comparison or logical operator.
   *
   * Comparison operators (< > <= >= == !=) and logical operators (&& ||)
   * always produce bool, provided both operands are of compatible types.
   *
   * @return Always TurfBool() on success; fires an error and returns nullptr
   *         on mismatch.
   */
  TurfTypePtr inferRelationalType(SourceLocation Loc, int Op,
                                  const TurfType *LhsTy,
                                  const TurfType *RhsTy);

  /// True if @p T is a numeric primitive (int, double, or bool).
  static bool isNumericType(const TurfType *T);

  /// True if two types are compatible for arithmetic (both numeric or both
  /// string — string + string is valid; mixed is not).
  static bool areArithCompatible(const TurfType *A, const TurfType *B);

  // ── State ─────────────────────────────────────────────────────────────────

  /// Reference to the symbol table filled by the Resolver.
  const ResolverSymbolTable &RST_;

  /// Map from AST node pointer to its inferred type.
  /// Pointer stability: we only ever store nodes whose lifetime is managed
  /// by the AST owning the unique_ptrs, so raw pointers are safe here.
  std::unordered_map<const ExprAST *, TurfTypePtr> NodeTypes_;

  /// Pluggable error reporter (default = TypeError::raise()).
  ErrorHandler ErrorHandler_;

  /// Stack of expected return types for nested function definitions.
  /// Empty when we are at the top level.
  std::vector<TurfTypePtr> ReturnTypeStack_;

  /// Loop variable types: populated by visitFor(), consulted by visitVariable().
  /// Maps loop variable name → inferred type (always int for range-for).
  std::unordered_map<std::string, TurfTypePtr> LoopVarTypes_;

  /// Whether we had at least one error during this pass.
  bool HadError_ = false;
};

#endif // TYPE_CHECKER_H
