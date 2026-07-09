#include "Analysis.h"
#include "Errors.h"
#include "AST.h"
#include "SymbolTable.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>

using AssignSet = std::unordered_set<SymbolID>;

namespace {

class AnalysisVisitor : public ASTVisitor {
  const std::function<void(SourceLocation, const std::string &)> &onRead;
  const std::function<void(SourceLocation, const std::string &)> &onDef;

  void visitChild(const ExprAST *Node) {
    if (Node)
      const_cast<ExprAST *>(Node)->accept(*this);
  }

public:
  AnalysisVisitor(
      const std::function<void(SourceLocation, const std::string &)> &onRead,
      const std::function<void(SourceLocation, const std::string &)> &onDef)
      : onRead(onRead), onDef(onDef) {}

  void visitVariable(VariableExprAST &Node) override {
    onRead(Node.getLoc(), Node.getName());
  }

  void visitVarDecl(VarDeclExprAST &Node) override {
    visitChild(Node.getInitializer());
    if (Node.getInitializer())
      onDef(Node.getLoc(), Node.getName());
  }

  void visitAssignment(AssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      onDef(Node.getLoc(), L->getName());
    } else {
      visitChild(Node.getLHS());
    }
  }

  void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
    visitChild(Node.getRHS());
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getLHS())) {
      onRead(Node.getLoc(), L->getName());
      onDef(Node.getLoc(), L->getName());
    } else {
      visitChild(Node.getLHS());
    }
  }

  void visitPostfixExpr(PostfixExprAST &Node) override {
    if (auto *L = dynamic_cast<const VariableExprAST *>(Node.getOperand())) {
      onRead(Node.getLoc(), L->getName());
      onDef(Node.getLoc(), L->getName());
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
    onDef(Node.getLoc(), Node.getName());
  }

  void visitStructInit(StructInitExprAST &Node) override {
    for (const auto &Field : Node.getNamedFields())
      visitChild(Field.second.get());
  }

  void visitDefaultInit(DefaultInitExprAST &Node) override {
    (void)Node;
  }
};

} // namespace

static void collectReadsAndDefs(
    const ExprAST *E, AssignSet &Current,
    const std::function<void(SourceLocation, const std::string &)> &onRead,
    const std::function<void(SourceLocation, const std::string &)> &onDef) {
  if (!E)
    return;
  AnalysisVisitor Visitor(onRead, onDef);
  const_cast<ExprAST *>(E)->accept(Visitor);
}

void DefiniteAssignmentPass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  // Collect all symbols visible in the current function scope.
  // Used to initialise non-entry IN sets to the "universal" set so the
  // intersection is correct from the very first worklist iteration.
  AssignSet AllSymbols;
  if (GlobalSymbolTable) {
    for (const auto &Name : GlobalSymbolTable->GetAllVisibleNames()) {
      Symbol *S = GlobalSymbolTable->LookupSymbol(Name);
      if (S) AllSymbols.insert(S->ID);
    }
  }

  // Also include the parameters themselves (they may not be in the visible
  // names if the table has been modified).
  for (auto id : InitialAssigned) AllSymbols.insert(id);

  std::vector<AssignSet> IN(N), OUT(N);

  // Entry block: IN = params, OUT will be computed by transfer.
  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  size_t entryIdx = 0;
  if (Entry) {
    entryIdx = index[Entry];
    for (auto id : InitialAssigned) {
      IN[entryIdx].insert(id);
    }
  }

  // All other blocks: initialise IN to the universal set so that intersection
  // starts from "everything" and gets tightened by the dataflow.
  for (size_t i = 0; i < N; ++i) {
    if (i != entryIdx) IN[i] = AllSymbols;
  }

  // ── Phase 1: Compute fixed-point dataflow (NO error reporting) ──────────
  // Transfer function: OUT[b] = IN[b] ∪ Defs(b)
  // Meet function:     IN[b]  = ∩ OUT[p] for all predecessors p
  auto computeOUT = [&](size_t b) -> AssignSet {
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt, cur,
        [](SourceLocation, const std::string &) { /* no-op during propagation */ },
        [&](SourceLocation, const std::string &Name) {
          if (!GlobalSymbolTable) return;
          Symbol *S = GlobalSymbolTable->LookupSymbol(Name);
          if (!S) return;
          cur.insert(S->ID);
        });
    }
    return cur;
  };

  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) {
    OUT[i] = computeOUT(i);
    work.push(i);
  }

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    // Compute new IN via meet (intersection) of predecessor OUTs
    AssignSet newIN;
    const auto &Preds = BB->getPredecessors();
    if (Preds.empty()) {
      // Entry block: IN is always the parameter set (never shrinks)
      newIN = IN[b];
    } else {
      bool first = true;
      for (auto *P : Preds) {
        size_t pi = index[P];
        if (first) { newIN = OUT[pi]; first = false; }
        else {
          AssignSet tmp;
          for (auto s : newIN) if (OUT[pi].count(s)) tmp.insert(s);
          newIN.swap(tmp);
        }
      }
      // Ensure parameters are always in IN (they are always defined on entry)
      for (auto id : InitialAssigned) newIN.insert(id);
    }

    if (newIN != IN[b]) {
      IN[b] = newIN;
      AssignSet newOUT = computeOUT(b);
      if (newOUT != OUT[b]) {
        OUT[b] = std::move(newOUT);
        for (auto *S : BB->getSuccessors()) {
          work.push(index[S]);
        }
      }
    }
  }

  // ── Phase 2: Report errors using the fixed-point IN sets ─────────────────
  for (size_t b = 0; b < N; ++b) {
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      collectReadsAndDefs(Stmt, cur,
        [&](SourceLocation Loc, const std::string &Name) {
          if (!GlobalSymbolTable) return;
          Symbol *S = GlobalSymbolTable->LookupSymbol(Name);
          if (!S) return;
          if (!cur.count(S->ID)) {
            DefiniteAssignmentError err(Loc, Name);
            err.raise();
          }
        },
        [&](SourceLocation, const std::string &Name) {
          if (!GlobalSymbolTable) return;
          Symbol *S = GlobalSymbolTable->LookupSymbol(Name);
          if (!S) return;
          cur.insert(S->ID);
        });
    }
  }
}

