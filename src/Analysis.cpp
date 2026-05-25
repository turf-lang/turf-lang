#include "Analysis.h"
#include "Errors.h"
#include "AST.h"
#include "SymbolTable.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>

using AssignSet = std::unordered_set<SymbolID>;

static void collectReadsAndDefs(const ExprAST *E, AssignSet &Current,
                                const std::function<void(SourceLocation,const std::string&)> &onRead,
                                const std::function<void(SourceLocation,const std::string&)> &onDef) {
  if (!E) return;

  // Lambdas for recursion
  std::function<void(const ExprAST *)> visit = [&](const ExprAST *N) {
    if (!N) return;
    // Handle node kinds via RTTI
    if (auto V = dynamic_cast<const VariableExprAST *>(N)) {
      onRead(V->getLoc(), V->getName());
    } else if (auto A = dynamic_cast<const AssignmentExprAST *>(N)) {
      // Visit RHS first (reads), then LHS becomes assigned
      visit(A->getRHS());
      // LHS may be a variable or more complex; if variable, mark def
      if (auto L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
        onDef(A->getLoc(), L->getName());
      } else {
        visit(A->getLHS());
      }
    } else if (auto Vd = dynamic_cast<const VarDeclExprAST *>(N)) {
      // initializer reads then def
      visit(Vd->getInitializer());
      onDef(Vd->getLoc(), Vd->getName());
    } else if (auto B = dynamic_cast<const BinaryExprAST *>(N)) {
      visit(B->getLHS());
      visit(B->getRHS());
    } else if (auto U = dynamic_cast<const UnaryExprAST *>(N)) {
      visit(U->getOperand());
    } else if (auto If = dynamic_cast<const IfExprAST *>(N)) {
      for (const auto &Branch : If->getBranches()) {
        visit(Branch.Cond.get());
        visit(Branch.Body.get());
      }
      visit(If->getElse());
    } else if (auto Bl = dynamic_cast<const BlockExprAST *>(N)) {
      for (const auto &E2 : Bl->getExpressions())
        visit(E2.get());
    } else if (auto R = dynamic_cast<const ReturnExprAST *>(N)) {
      visit(R->getVal());
    } else if (auto W = dynamic_cast<const WhileExprAST *>(N)) {
      visit(W->getCond());
      visit(W->getBody());
    } else if (auto F = dynamic_cast<const ForExprAST *>(N)) {
      visit(F->getStart());
      visit(F->getEnd());
      visit(F->getStep());
      visit(F->getBody());
    } else if (auto C = dynamic_cast<const CastExprAST *>(N)) {
      visit(C->getOperand());
    } else if (auto S = dynamic_cast<const SizeOfExprAST *>(N)) {
      // nothing
    } else if (auto Str = dynamic_cast<const StringExprAST *>(N)) {
      // nothing
    } else if (auto Num = dynamic_cast<const NumberExprAST *>(N)) {
      // nothing
    } else if (auto Try = dynamic_cast<const TryExprAST *>(N)) {
      visit(Try->getTryBlock());
      visit(Try->getCatchBlock());
    } else {
      // Fallback: no-op for nodes we don't explicitly handle
    }
  };

  visit(E);
}

void DefiniteAssignmentPass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  std::vector<AssignSet> IN(N), OUT(N);

  // Initialize OUT[entry] with parameters
  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  if (Entry) {
    size_t ei = index[Entry];
    for (auto id : InitialAssigned) OUT[ei].insert(id);
  }

  // Worklist algorithm
  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    // IN = intersection of predecessor OUTs (for entry, use OUT[entry])
    AssignSet newIN;
    const auto &Preds = BB->getPredecessors();
    if (Preds.empty()) {
      // entry block: IN = OUT[entry] (already contains params)
      newIN = IN[b];
    } else {
      bool first = true;
      for (auto *P : Preds) {
        size_t pi = index[P];
        if (first) { newIN = OUT[pi]; first = false; }
        else {
          // intersection
          AssignSet tmp;
          for (auto s : newIN) if (OUT[pi].count(s)) tmp.insert(s);
          newIN.swap(tmp);
        }
      }
    }

    if (newIN != IN[b]) {
      IN[b] = std::move(newIN);
    }

    // Transfer: process statements sequentially
    AssignSet cur = IN[b];
    for (ExprAST *Stmt : BB->getStatements()) {
      collectReadsAndDefs(Stmt, cur,
                          [&](SourceLocation Loc, const std::string &Name){
                            // on read: check assigned
                            if (!GlobalSymbolTable) return;
                            Symbol *S = GlobalSymbolTable->LookupSymbol(Name);
                            if (!S) return; // unresolved var handled elsewhere
                            if (!cur.count(S->ID)) {
                              DefiniteAssignmentError err(Loc, Name);
                              err.raise();
                            }
                          },
                          [&](SourceLocation Loc, const std::string &Name){
                            if (!GlobalSymbolTable) return;
                            Symbol *S = GlobalSymbolTable->LookupSymbol(Name);
                            if (!S) return;
                            cur.insert(S->ID);
                          });
    }

    if (cur != OUT[b]) {
      OUT[b] = std::move(cur);
      // push successors
      for (auto *S : BB->getSuccessors()) {
        size_t si = index[S];
        work.push(si);
      }
    }
  }
}
