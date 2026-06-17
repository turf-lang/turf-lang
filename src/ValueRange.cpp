#include "ValueRange.h"
#include "AST.h"
#include "SymbolTable.h"
#include "Errors.h"

#include <optional>
#include <queue>
#include <unordered_map>

using ConstMap = std::unordered_map<SymbolID, std::optional<long long>>;

static std::optional<long long> evalConstExpr(const ExprAST *E, const ConstMap &M) {
  if (!E) return std::nullopt;
  if (auto N = dynamic_cast<const NumberExprAST *>(E)) {
    if (N->isInteger()) return N->getIntVal();
    return std::nullopt;
  }
  if (auto V = dynamic_cast<const VariableExprAST *>(E)) {
    if (!GlobalSymbolTable) return std::nullopt;
    Symbol *S = GlobalSymbolTable->LookupSymbol(V->getName());
    if (!S) return std::nullopt;
    auto it = M.find(S->ID);
    if (it != M.end()) return it->second;
    return std::nullopt;
  }
  if (auto B = dynamic_cast<const BinaryExprAST *>(E)) {
    auto L = evalConstExpr(B->getLHS(), M);
    auto R = evalConstExpr(B->getRHS(), M);
    if (!L.has_value() || !R.has_value()) return std::nullopt;
    long long lv = *L, rv = *R;
    switch (B->getOp()) {
    case '+': return lv + rv;
    case '-': return lv - rv;
    case '*': return lv * rv;
    case '/': if (rv == 0) return std::nullopt; return lv / rv;
    default: return std::nullopt;
    }
  }
  if (auto A = dynamic_cast<const AssignmentExprAST *>(E)) {
    return evalConstExpr(A->getRHS(), M);
  }
  // For other nodes, conservatively unknown
  return std::nullopt;
}

void ValueRangePass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned) {
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  std::vector<ConstMap> IN(N), OUT(N);

  TurfBasicBlock *Entry = Cfg.getEntryBlock();
  if (Entry) {
    size_t ei = index[Entry];
    for (auto id : InitialAssigned) IN[ei][id] = std::nullopt; // parameters unknown values
  }

  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    // IN = meet of preds: for constants, require all preds agree on same const
    ConstMap newIN;
    const auto &Preds = BB->getPredecessors();
    if (Preds.empty()) {
      newIN = IN[b];
    } else {
      bool first = true;
      for (auto *P : Preds) {
        size_t pi = index[P];
        if (first) { newIN = OUT[pi]; first = false; }
        else {
          // join: keep value only if equal in both maps, else unknown
          ConstMap tmp;
          for (const auto &kv : newIN) {
            SymbolID sid = kv.first;
            auto it = OUT[pi].find(sid);
            if (it != OUT[pi].end() && kv.second.has_value() && it->second.has_value() && kv.second == it->second) {
              tmp[sid] = kv.second;
            } else {
              tmp[sid] = std::nullopt;
            }
          }
          newIN.swap(tmp);
        }
      }
    }

    if (newIN != IN[b]) IN[b] = newIN;

    // Transfer: propagate constants through statements
    ConstMap cur = IN[b];
    for (ExprAST *Stmt : BB->getStatements()) {
        // Check for array accesses inside this statement and emit OOB diagnostics
        std::function<void(const ExprAST *)> checkAccesses = [&](const ExprAST *N) {
          if (!N) return;
          if (auto AA = dynamic_cast<const ArrayAccessExprAST *>(N)) {
            auto idxVal = evalConstExpr(AA->getIndex(), cur);
            if (idxVal.has_value()) {
              if (auto BaseVar = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
                if (GlobalSymbolTable) {
                  Symbol *S = GlobalSymbolTable->LookupSymbol(BaseVar->getName());
                  if (S && S->Type && S->Type->isArray()) {
                    const ArrayType *AT = dynamic_cast<const ArrayType *>(S->Type.get());
                    if (AT) {
                      int sz = AT->getSize();
                      if (sz > 0) {
                        long long iv = *idxVal;
                        if (iv < 0 || iv >= sz) {
                          ArrayBoundsError err(AA->getLoc(), BaseVar->getName(), iv, sz);
                          err.raise();
                        }
                      }
                    }
                  }
                }
              }
            }
          }

          // Recurse into children (basic traversal)
          if (auto B = dynamic_cast<const BinaryExprAST *>(N)) { checkAccesses(B->getLHS()); checkAccesses(B->getRHS()); }
          else if (auto U = dynamic_cast<const UnaryExprAST *>(N)) checkAccesses(U->getOperand());
          else if (auto A = dynamic_cast<const AssignmentExprAST *>(N)) { checkAccesses(A->getRHS()); checkAccesses(A->getLHS()); }
          else if (auto Vd = dynamic_cast<const VarDeclExprAST *>(N)) checkAccesses(Vd->getInitializer());
          else if (auto If = dynamic_cast<const IfExprAST *>(N)) {
            for (const auto &Br : If->getBranches()) { checkAccesses(Br.Cond.get()); checkAccesses(Br.Body.get()); }
            checkAccesses(If->getElse());
          } else if (auto Bl = dynamic_cast<const BlockExprAST *>(N)) {
            for (const auto &E2 : Bl->getExpressions()) checkAccesses(E2.get());
          } else if (auto F = dynamic_cast<const FuncCallExprAST *>(N)) {
            for (const auto &Arg : F->getArgs()) checkAccesses(Arg.get());
            checkAccesses(F->getCallee());
          } else if (auto ArrA = dynamic_cast<const ArrayAccessExprAST *>(N)) {
            checkAccesses(ArrA->getBase()); checkAccesses(ArrA->getIndex());
          } else if (auto ArrAs = dynamic_cast<const ArrayAssignExprAST *>(N)) {
            checkAccesses(ArrAs->getIndex()); checkAccesses(ArrAs->getRHS());
          } else if (auto R = dynamic_cast<const ReturnExprAST *>(N)) checkAccesses(R->getVal());
        };
        checkAccesses(Stmt);

      // Handle var decl
      if (auto Vd = dynamic_cast<VarDeclExprAST *>(Stmt)) {
        auto val = evalConstExpr(Vd->getInitializer(), cur);
        Symbol *S = GlobalSymbolTable ? GlobalSymbolTable->LookupSymbol(Vd->getName()) : nullptr;
        if (S) cur[S->ID] = val;
        continue;
      }
      if (auto A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
        // LHS variable assign
        if (auto L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
          auto val = evalConstExpr(A->getRHS(), cur);
          Symbol *S = GlobalSymbolTable ? GlobalSymbolTable->LookupSymbol(L->getName()) : nullptr;
          if (S) cur[S->ID] = val;
        }
        continue;
      }
      // For other statements, we attempt to evaluate but don't change state
      // (could be extended later)
    }

    if (cur != OUT[b]) {
      OUT[b] = cur;
      for (auto *S : BB->getSuccessors()) {
        size_t si = index[S];
        work.push(si);
      }
    }
  }

  // Currently no diagnostics emitted; results are available for later passes.
}
