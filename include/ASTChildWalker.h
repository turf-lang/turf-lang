#ifndef AST_CHILD_WALKER_H
#define AST_CHILD_WALKER_H

#include "AST.h"

#include <set>

/**
 * @brief ExhaustiveASTVisitor whose default for every node kind is "visit each
 *        child expression, in source order".
 *
 * LEC-005 / INV-LINT-1. The lint pass and the liveness passes need to look at
 * every node, and each used to keep its own hand-written list of which node
 * kinds contain which children -- so each list had its own gaps (a call
 * argument, an `unsafe` block, `&x`, a `do`/`while` body). This class is the
 * one place that says what a node's children ARE; a pass derives from it and
 * overrides only the nodes it does something with, calling
 * ASTChildWalker::visitX(Node) (or walking the children itself) for the rest.
 * Because it is an ExhaustiveASTVisitor, a new node kind added to
 * TURF_AST_NODE_LIST fails to compile HERE, in the one file that has to
 * decide where the new node's children are, rather than being skipped by
 * every pass that happens to inherit a no-op.
 *
 * A node that holds no child expressions (a literal, a declaration, `break`)
 * is an explicit `{}` below, so "no children" is a recorded decision, not an
 * omission.
 */
class ASTChildWalker : public ExhaustiveASTVisitor {
public:
  /// Visit @p Node if non-null. The const_cast is the visitor pattern's
  /// price: `accept()` is non-const but every AST getter hands out const
  /// pointers.
  void visitChild(const ExprAST *Node) {
    if (!Node)
      return;
    // Optional depth fence (see AstDepthExceeds()): past the limit it records
    // the fact and stops descending, so the walk itself never goes deeper than
    // the limit. Unlimited (0) for every ordinary pass.
    if (DepthLimit_ && Depth_ >= DepthLimit_) {
      TooDeep_ = true;
      return;
    }
    ++Depth_;
    const_cast<ExprAST *>(Node)->accept(*this);
    --Depth_;
  }

protected:
  int DepthLimit_ = 0;
  int Depth_ = 0;
  bool TooDeep_ = false;

public:

  // ── Leaves: no child expressions ──────────────────────────────────────────
  void visitNumber(NumberExprAST &) override {}
  void visitBool(BoolExprAST &) override {}
  void visitChar(CharExprAST &) override {}
  void visitString(StringExprAST &) override {}
  void visitNull(NullExprAST &) override {}
  void visitVariable(VariableExprAST &) override {}
  void visitBreak(BreakExprAST &) override {}
  void visitContinue(ContinueExprAST &) override {}
  void visitWildcardPattern(WildcardPatternExprAST &) override {}
  void visitBindingPattern(BindingPatternExprAST &) override {}
  void visitScopeAccess(ScopeAccessExprAST &) override {}
  void visitSizeOf(SizeOfExprAST &) override {}
  void visitCallerLocation(CallerLocationExprAST &) override {}

  // ── Declarations: not part of any function body's expression tree ─────────
  // A `struct`/`enum`/`error`/`machine`/`extern fn`/`import` is walked by the
  // pass that owns top-level declarations, never as a child of an expression,
  // and a nested `fn` is rejected by the Resolver. `import` carries a parsed
  // module body, deliberately NOT descended into: that module is linted and
  // analysed as its own file.
  void visitEnumDecl(EnumDeclExprAST &) override {}
  void visitMachineDecl(MachineDeclExprAST &) override {}
  void visitErrorDecl(ErrorDeclExprAST &) override {}
  void visitStructDecl(StructDeclExprAST &) override {}
  void visitModuleImport(ModuleImportExprAST &) override {}
  void visitExternFuncDecl(ExternFuncDeclExprAST &) override {}

  // ── Containers ────────────────────────────────────────────────────────────
  void visitFuncDef(FuncDefExprAST &Node) override {
    visitChild(Node.getBody());
  }
  void visitVarDecl(VarDeclExprAST &Node) override {
    visitChild(Node.getInitializer());
  }
  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }
  void visitUnary(UnaryExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitAddressOf(AddressOfExprAST &Node) override {
    visitChild(Node.getOperand());
  }
  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }
  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }
  void visitPostfixExpr(PostfixExprAST &Node) override {
    visitChild(Node.getOperand());
  }
  void visitCast(CastExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions())
      visitChild(E.get());
  }
  void visitUnsafeBlock(UnsafeBlockExprAST &Node) override {
    visitChild(Node.getBody());
  }
  void visitIf(IfExprAST &Node) override {
    for (const auto &Branch : Node.getBranches()) {
      visitChild(Branch.Cond.get());
      visitChild(Branch.Body.get());
    }
    visitChild(Node.getElse());
  }
  void visitWhile(WhileExprAST &Node) override {
    visitChild(Node.getCond());
    visitChild(Node.getBody());
  }
  void visitDoWhile(DoWhileExprAST &Node) override {
    visitChild(Node.getCond());
    visitChild(Node.getBody());
  }
  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart());
    visitChild(Node.getEnd());
    visitChild(Node.getStep());
    visitChild(Node.getBody());
  }
  void visitForEach(ForEachExprAST &Node) override {
    visitChild(Node.getIterable());
    visitChild(Node.getBody());
  }
  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }
  void visitCompare(CompareExprAST &Node) override {
    visitChild(Node.getSubject());
    for (auto &Arm : Node.getArmsMut()) {
      for (auto &Val : Arm.Values)
        visitChild(Val.get());
      for (auto &Guard : Arm.Guards)
        visitChild(Guard.get());
      visitChild(Arm.Body.get());
    }
    visitChild(Node.getOtherBody());
  }
  void visitDefer(DeferExprAST &Node) override { visitChild(Node.getStmt()); }
  void visitStructPattern(StructPatternExprAST &Node) override {
    for (auto &Field : Node.getFieldsMut())
      visitChild(Field.Sub.get());
  }
  void visitErrorVariantPattern(ErrorVariantPatternExprAST &Node) override {
    for (auto &Sub : Node.getSubsMut())
      visitChild(Sub.get());
  }
  void visitFuncCall(FuncCallExprAST &Node) override {
    visitChild(Node.getCallee());
    for (const auto &Arg : Node.getArgs())
      visitChild(Arg.get());
  }
  void visitMethodCall(MethodCallExprAST &Node) override {
    visitChild(Node.getBase());
    for (const auto &Arg : Node.getArgs())
      visitChild(Arg.get());
  }
  void visitFieldAccess(FieldAccessExprAST &Node) override {
    visitChild(Node.getBase());
  }
  /// `Wrapped` always runs; `ProduceExpr` / `Block` only on the failure path,
  /// and exactly one of the two exists per RecoverKind.
  void visitRecoverExpr(RecoverExprAST &Node) override {
    visitChild(Node.getWrapped());
    if (Node.getKind() == RecoverKind::Produce)
      visitChild(Node.getProduceExpr());
    else
      visitChild(Node.getBlock());
  }
  void visitTry(TryExprAST &Node) override { visitChild(Node.getOperand()); }
  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &Init : Node.getInitList())
      visitChild(Init.get());
    if (Node.getInitExpr())
      visitChild(Node.getInitExpr().get());
  }
  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &Elem : Node.getElements())
      visitChild(Elem.get());
  }
  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    visitChild(Node.getBase());
    visitChild(Node.getIndex());
  }
  void visitSlice(SliceExprAST &Node) override {
    visitChild(Node.getBase());
    visitChild(Node.getStart());
    visitChild(Node.getEnd());
  }
};

/// Inserts the node ID of every node it reaches. One override per node kind,
/// generated from TURF_AST_NODE_LIST, so no kind can be left out.
class ASTNodeIDCollectorBase : public ASTChildWalker {
  std::set<uint64_t> &IDs;

public:
  explicit ASTNodeIDCollectorBase(std::set<uint64_t> &IDs) : IDs(IDs) {}

#define TURF_COLLECT_NODE_ID(Name, Class)                                      \
  void visit##Name(Class &Node) override {                                     \
    IDs.insert(Node.getNodeID());                                              \
    ASTChildWalker::visit##Name(Node);                                         \
  }
  TURF_AST_NODE_LIST(TURF_COLLECT_NODE_ID)
#undef TURF_COLLECT_NODE_ID
};

class ASTNodeIDCollector : public ASTNodeIDCollectorBase {
public:
  using ASTNodeIDCollectorBase::ASTNodeIDCollectorBase;

  // The Sandbox's whole-tree fallback has always included an imported module's
  // body (ASTChildWalker deliberately does not descend into one).
  void visitModuleImport(ModuleImportExprAST &Node) override {
    ASTNodeIDCollectorBase::visitModuleImport(Node);
    visitChild(Node.getBody());
  }
};

inline void CollectAllNodeIDsInternal(const ExprAST *Node,
                                      std::set<uint64_t> &NodeIDs) {
  if (!Node)
    return;
  ASTNodeIDCollector Collector(NodeIDs);
  const_cast<ExprAST *>(Node)->accept(Collector);
}


/// Walks an expression tree and reports whether it is deeper than @p Limit
/// levels. The walk is recursive like every other, but fenced: it stops
/// descending at the limit, so it uses at most @p Limit levels of stack
/// however deep the tree is -- the property no ordinary pass has.
///
/// Why this exists: the parser bounds *recursion* (ParseRecursionGuard) and
/// postfix-chain length, not the depth of the tree it builds, and a tree built
/// from finished pieces -- `(((n--…--)--…--)--…--)`, each piece within the
/// limit -- is as deep as their sum. Every later pass recurses over the tree,
/// so such a tree segfaulted the type checker (exit 139) with no diagnostic.
/// The check is made once per top-level declaration, before anything recurses
/// over it (AppendFlattenedTopLevelDecl()).
class AstDepthProbe : public ASTChildWalker {
public:
  explicit AstDepthProbe(int Limit) { DepthLimit_ = Limit; }
  bool exceeded(const ExprAST *Root) {
    visitChild(Root);
    return TooDeep_;
  }
};

inline bool AstDepthExceeds(const ExprAST *Root, int Limit) {
  AstDepthProbe Probe(Limit);
  return Probe.exceeded(Root);
}

#endif // AST_CHILD_WALKER_H
