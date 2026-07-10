#include "Analysis.h"
#include "Errors.h"
#include "AST.h"
#include "SymbolTable.h"
#include "ResolverSymbolTable.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>

// Use raw VariableSymbol* as the definiteness key.
// VariableSymbol objects are owned by shared_ptrs attached to AST nodes and
// survive the entire compilation of a function — unlike SymbolID from
// GlobalSymbolTable which is erased when a lexical scope exits.
using AssignSet = std::unordered_set<const VariableSymbol *>;

namespace {

// ─── AnalysisVisitor ─────────────────────────────────────────────────────────
// Walks an expression and fires onRead/onDef callbacks with the VariableSymbol*
// that the Resolver attached to each variable reference / declaration.

class AnalysisVisitor : public ASTVisitor {
  const std::function<void(SourceLocation, const VariableSymbol *)> &onRead;
  const std::function<void(SourceLocation, const VariableSymbol *)> &onDef;

  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }

public:
  AnalysisVisitor(
      const std::function<void(SourceLocation, const VariableSymbol *)> &onRead,
      const std::function<void(SourceLocation, const VariableSymbol *)> &onDef)
      : onRead(onRead), onDef(onDef) {}

  void visitVariable(VariableExprAST &Node) override {
    if (Node.ResolvedSymbol)
      onRead(Node.getLoc(), Node.ResolvedSymbol.get());
  }

  void visitVarDecl(VarDeclExprAST &Node) override {
    visitChild(Node.getInitializer());
    // Only mark as "defined" when there is an explicit initializer.
    // Primitive locals declared without an initializer (e.g. `mut x: int;`)
    // are intentionally left out so the DAP catches reads before assignment.
    if (Node.getInitializer() && Node.ResolvedSymbol)
      onDef(Node.getLoc(), Node.ResolvedSymbol.get());
  }

  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      if (L->ResolvedSymbol)
        onDef(Node.getLoc(), L->ResolvedSymbol.get());
    } else {
      visitChild(Node.getLHS());
    }
  }

  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      if (L->ResolvedSymbol) {
        onRead(Node.getLoc(), L->ResolvedSymbol.get());
        onDef(Node.getLoc(),  L->ResolvedSymbol.get());
      }
    } else {
      visitChild(Node.getLHS());
    }
  }

  void visitPostfixExpr(PostfixExprAST &Node) override {
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getOperand())) {
      if (L->ResolvedSymbol) {
        onRead(Node.getLoc(), L->ResolvedSymbol.get());
        onDef(Node.getLoc(),  L->ResolvedSymbol.get());
      }
    } else {
      visitChild(Node.getOperand());
    }
  }

  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS());
    visitChild(Node.getRHS());
  }

  void visitUnary(UnaryExprAST &Node) override {
    visitChild(Node.getOperand());
  }

  void visitCast(CastExprAST &Node) override { visitChild(Node.getOperand()); }

  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions())
      visitChild(E.get());
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

  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart());
    visitChild(Node.getEnd());
    visitChild(Node.getStep());
    visitChild(Node.getBody());
  }

  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }

  void visitFuncCall(FuncCallExprAST &Node) override {
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

  void visitArrayAccess(ArrayAccessExprAST &Node) override {
    visitChild(Node.getBase());
    visitChild(Node.getIndex());
  }

  void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
    for (const auto &Elem : Node.getElements())
      visitChild(Elem.get());
  }

  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    for (const auto &Init : Node.getInitList())
      visitChild(Init.get());
    if (Node.getInitExpr())
      visitChild(Node.getInitExpr().get());
    // Arrays always get storage allocated — treat as defined immediately.
    if (Node.ResolvedSymbol)
      onDef(Node.getLoc(), Node.ResolvedSymbol.get());
  }

  void visitStructInit(StructInitExprAST &Node) override {
    for (const auto &Field : Node.getNamedFields())
      visitChild(Field.second.get());
  }

  void visitDefaultInit(DefaultInitExprAST &Node) override {
    (void)Node;
  }
};

// ─── BodyDeclCollector ────────────────────────────────────────────────────────
// A simple visitor that collects ALL locally-declared VariableSymbol* — both
// those with and without an explicit initializer. This set is used to
// distinguish local variables (which must be assigned before use) from function
// parameters (which are always defined when the function is called).

class BodyDeclCollector : public ASTVisitor {
  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }

public:
  AssignSet LocalDecls; // All locally-declared symbols (with or without init)

  void visitVarDecl(VarDeclExprAST &Node) override {
    if (Node.ResolvedSymbol)
      LocalDecls.insert(Node.ResolvedSymbol.get());
    visitChild(Node.getInitializer());
  }

  void visitArrayDecl(ArrayDeclExprAST &Node) override {
    if (Node.ResolvedSymbol)
      LocalDecls.insert(Node.ResolvedSymbol.get());
    for (const auto &I : Node.getInitList()) visitChild(I.get());
    if (Node.getInitExpr()) visitChild(Node.getInitExpr().get());
  }

  // Recurse into compound statements
  void visitBlock(BlockExprAST &Node) override {
    for (const auto &E : Node.getExpressions()) visitChild(E.get());
  }
  void visitIf(IfExprAST &Node) override {
    for (const auto &Br : Node.getBranches()) {
      visitChild(Br.Cond.get()); visitChild(Br.Body.get());
    }
    visitChild(Node.getElse());
  }
  void visitWhile(WhileExprAST &Node) override {
    visitChild(Node.getCond()); visitChild(Node.getBody());
  }
  void visitFor(ForExprAST &Node) override {
    visitChild(Node.getStart()); visitChild(Node.getEnd());
    visitChild(Node.getStep()); visitChild(Node.getBody());
  }
  void visitReturn(ReturnExprAST &Node) override { visitChild(Node.getVal()); }
  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getLHS()); visitChild(Node.getRHS());
  }
  void visitBinary(BinaryExprAST &Node) override {
    visitChild(Node.getLHS()); visitChild(Node.getRHS());
  }
};

} // namespace

// ─── collectReadsAndDefs ─────────────────────────────────────────────────────

static void collectReadsAndDefs(
    const ExprAST *E,
    const std::function<void(SourceLocation, const VariableSymbol *)> &onRead,
    const std::function<void(SourceLocation, const VariableSymbol *)> &onDef) {
  if (!E) return;
  AssignSet dummy;
  AnalysisVisitor Visitor(onRead, onDef);
  const_cast<ExprAST *>(E)->accept(Visitor);
}

// ─── DefiniteAssignmentPass ──────────────────────────────────────────────────

void DefiniteAssignmentPass::runOnCFG(CFG &Cfg,
                                      const std::vector<SymbolID> &/*InitialAssigned*/) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  // ── Step 0: Collect all locally-declared symbols (the "body" set) ─────────
  // These are symbols introduced by VarDeclExprAST or ArrayDeclExprAST in the
  // function body. Function parameters are NOT in this set — they are always
  // considered pre-initialized and must never be flagged.
  AssignSet AllBodyDeclared;
  for (size_t b = 0; b < N; ++b) {
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      BodyDeclCollector Coll;
      if (Stmt) const_cast<ExprAST *>(static_cast<const ExprAST *>(Stmt))->accept(Coll);
      for (const VariableSymbol *Sym : Coll.LocalDecls)
        AllBodyDeclared.insert(Sym);
    }
  }

  // ── Step 1: Dataflow (no error reporting) ─────────────────────────────────
  // Transfer : OUT[b] = IN[b] ∪ Defs(b)
  // Meet     : IN[b]  = ∩ OUT[pred] for all predecessors p
  // Entry IN starts empty; all other blocks start at the universal set.

  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  size_t entryIdx = 0;
  if (Entry) entryIdx = index[Entry];

  std::vector<AssignSet> IN(N), OUT(N);
  // Non-entry blocks: init to "all locals" (universal set for intersection)
  for (size_t i = 0; i < N; ++i)
    if (i != entryIdx) IN[i] = AllBodyDeclared;

  auto computeOUT = [&](size_t b) -> AssignSet {
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt,
        [](SourceLocation, const VariableSymbol *) { /* no-op */ },
        [&](SourceLocation, const VariableSymbol *Sym) { cur.insert(Sym); });
    }
    return cur;
  };

  for (size_t i = 0; i < N; ++i) OUT[i] = computeOUT(i);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < N; ++b) {
      if (b == entryIdx) continue;

      // Meet: intersect OUT of all predecessors.
      AssignSet newIN = AllBodyDeclared;
      bool hasPred = false;
      for (size_t p = 0; p < N; ++p) {
        bool isPred = false;
        for (const auto *succ : Blocks[p]->getSuccessors()) {
          if (index.count(succ) && index.at(succ) == b) { isPred = true; break; }
        }
        if (!isPred) continue;
        hasPred = true;
        AssignSet intersected;
        for (const VariableSymbol *sym : newIN)
          if (OUT[p].count(sym)) intersected.insert(sym);
        newIN = std::move(intersected);
      }
      if (!hasPred) continue;

      if (newIN != IN[b]) {
        IN[b]  = newIN;
        OUT[b] = computeOUT(b);
        changed = true;
      }
    }
  }

  // ── Step 2: Report errors using fixed-point IN sets ───────────────────────
  // Only flag a read as "uninitialized" if the symbol was locally DECLARED
  // (i.e. it is in AllBodyDeclared) but NOT yet in the current assigned set.
  // Reads of function parameters (not in AllBodyDeclared) are always fine.
  for (size_t b = 0; b < N; ++b) {
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt,
        [&](SourceLocation Loc, const VariableSymbol *Sym) {
          if (AllBodyDeclared.count(Sym) && !cur.count(Sym)) {
            DefiniteAssignmentError err(Loc, Sym->Name);
            err.raise();
          }
        },
        [&](SourceLocation, const VariableSymbol *Sym) {
          cur.insert(Sym);
        });
    }
  }
}
