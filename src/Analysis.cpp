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

