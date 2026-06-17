#include "AliasAnalysis.h"
#include "AST.h"
#include "Errors.h"
#include "SymbolTable.h"

#include <unordered_map>
#include <queue>
#include <optional>

using AllocSiteID = size_t;

void AliasAnalysisPass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned) {
  std::cerr << "[AliasAnalysis] running on function: " << Cfg.getFunctionName() << "\n";
  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  // For simplicity, we perform a forward traversal per-block maintaining a
  // points-to map for symbols (SymbolID -> optional AllocSiteID) and a set
  // of freed alloc sites.

  std::vector<std::unordered_map<std::string, std::optional<AllocSiteID>>> IN(N), OUT(N);
  std::vector<std::unordered_set<AllocSiteID>> Freed(N);

  AllocSiteID NextAllocID = 1;

  // Worklist
  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  // Helper: get variable name from VariableExprAST (or empty)
  auto getVarName = [&](const VariableExprAST *V) -> std::string {
    if (!V) return std::string();
    return V->getName();
  };

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    // IN = merge of preds (conservative: prefer existing mapping, do not try to join alloc sites)
    std::unordered_map<std::string, std::optional<AllocSiteID>> newIN;
    std::unordered_set<AllocSiteID> newFreed;
    const auto &Preds = BB->getPredecessors();
    if (Preds.empty()) {
      newIN = IN[b];
      newFreed = Freed[b];
    } else {
      bool first = true;
      for (auto *P : Preds) {
        size_t pi = index[P];
        if (first) { newIN = OUT[pi]; newFreed = Freed[pi]; first = false; }
        else {
          // merge maps: keep value only if equal across preds, else unknown
          std::unordered_map<std::string, std::optional<AllocSiteID>> tmp;
          for (const auto &kv : newIN) {
            const std::string &name = kv.first;
            auto it = OUT[pi].find(name);
            if (it != OUT[pi].end() && it->second == kv.second) tmp[name] = kv.second;
            else tmp[name] = std::nullopt;
          }
          newIN.swap(tmp);
          // merge freed sets: union
          for (auto fs : Freed[pi]) newFreed.insert(fs);
        }
      }
    }

    if (newIN != IN[b] || newFreed != Freed[b]) {
      IN[b] = newIN;
      Freed[b] = newFreed;
    }

    // Transfer: scan statements
    auto cur = IN[b];
    auto curFreed = Freed[b];

    for (ExprAST *Stmt : BB->getStatements()) {
      // Detect assignment of malloc: x = malloc(n)
      if (auto A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
        // RHS is func call?
        if (auto RHS = dynamic_cast<const FuncCallExprAST *>(A->getRHS())) {
          if (auto CalleeVar = dynamic_cast<const VariableExprAST *>(RHS->getCallee())) {
            std::string cname = CalleeVar->getName();
            if (cname == "malloc" || cname == "realloc") {
              // allocate new site
              AllocSiteID aid = NextAllocID++;
              // LHS variable
              if (auto L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
                std::string name = getVarName(L);
                if (!name.empty()) {
                  cur[name] = aid;
                  std::cerr << "[AliasAnalysis] allocated site " << aid << " for var '" << name << "'\n";
                }
              }
              continue;
            }
          }
        }
      }

      // VarDecl with initializer = malloc()
      if (auto Vd = dynamic_cast<VarDeclExprAST *>(Stmt)) {
        if (auto Init = dynamic_cast<const FuncCallExprAST *>(Vd->getInitializer())) {
          if (auto CalleeVar = dynamic_cast<const VariableExprAST *>(Init->getCallee())) {
            std::string cname = CalleeVar->getName();
            if (cname == "malloc" || cname == "realloc") {
              std::string name = Vd->getName();
                if (!name.empty()) {
                  AllocSiteID aid = NextAllocID++;
                  cur[name] = aid;
                  std::cerr << "[AliasAnalysis] allocated site " << aid << " for var '" << name << "' (decl)\n";
                }
              continue;
            }
          }
        }
      }

      // free(x)
      if (auto FC = dynamic_cast<FuncCallExprAST *>(Stmt)) {
        if (auto CalleeVar = dynamic_cast<const VariableExprAST *>(FC->getCallee())) {
          std::string cname = CalleeVar->getName();
          if (cname == "free") {
            if (!FC->getArgs().empty()) {
              const ExprAST *Arg0 = FC->getArgs()[0].get();
              if (auto V = dynamic_cast<const VariableExprAST *>(Arg0)) {
                std::string name = getVarName(V);
                if (!name.empty()) {
                  auto it = cur.find(name);
                  if (it != cur.end() && it->second.has_value()) {
                    AllocSiteID aid = *(it->second);
                    if (curFreed.count(aid)) {
                      std::cerr << "[AliasAnalysis] double-free candidate for var '" << name << "'\n";
                      DoubleFreeError err(FC->getLoc(), name);
                      err.raise();
                    } else {
                      std::cerr << "[AliasAnalysis] freeing alloc site " << aid << " from var '" << name << "'\n";
                      curFreed.insert(aid);
                      // Keep mapping so later accesses can detect use-after-free
                    }
                  }
                }
              }
            }
            continue;
          }
        }
      }

      // Array access -> check if base points to a freed alloc site
      if (auto AA = dynamic_cast<ArrayAccessExprAST *>(Stmt)) {
        if (auto BaseV = dynamic_cast<const VariableExprAST *>(AA->getBase())) {
          std::string name = getVarName(BaseV);
          if (!name.empty()) {
            auto it = cur.find(name);
            if (it != cur.end() && it->second.has_value()) {
              AllocSiteID aid = *(it->second);
              if (curFreed.count(aid)) {
                std::cerr << "[AliasAnalysis] use-after-free detected for var '" << name << "'\n";
                UseAfterFreeError err(AA->getLoc(), name);
                err.raise();
              }
            }
          }
        }
      }

      // Recurse into statement children for nested calls / accesses
      std::function<void(const ExprAST*)> recurse = [&](const ExprAST *N) {
        if (!N) return;
        if (auto F = dynamic_cast<const FuncCallExprAST*>(N)) {
          // free inside expression
          if (auto CalleeVar = dynamic_cast<const VariableExprAST *>(F->getCallee())) {
            if (CalleeVar->getName() == "free" && !F->getArgs().empty()) {
              if (auto V = dynamic_cast<const VariableExprAST *>(F->getArgs()[0].get())) {
                std::string name = getVarName(V);
                if (!name.empty()) {
                  auto it = cur.find(name);
                  if (it != cur.end() && it->second.has_value()) {
                    AllocSiteID aid = *(it->second);
                    if (curFreed.count(aid)) {
                      DoubleFreeError err(F->getLoc(), name);
                      err.raise();
                    } else {
                      curFreed.insert(aid);
                      // Preserve mapping for later detection
                    }
                  }
                }
              }
            }
          }
        }
        if (auto AA2 = dynamic_cast<const ArrayAccessExprAST*>(N)) {
          if (auto BaseV = dynamic_cast<const VariableExprAST*>(AA2->getBase())) {
            std::string name = getVarName(BaseV);
            if (!name.empty()) {
              auto it = cur.find(name);
              if (it != cur.end() && it->second.has_value()) {
                AllocSiteID aid = *(it->second);
                if (curFreed.count(aid)) {
                  UseAfterFreeError err(AA2->getLoc(), name);
                  err.raise();
                }
              }
            }
          }
        }

        // descend
        if (auto B = dynamic_cast<const BinaryExprAST*>(N)) { recurse(B->getLHS()); recurse(B->getRHS()); }
        else if (auto U = dynamic_cast<const UnaryExprAST*>(N)) recurse(U->getOperand());
        else if (auto As = dynamic_cast<const AssignmentExprAST*>(N)) { recurse(As->getLHS()); recurse(As->getRHS()); }
        else if (auto Vd = dynamic_cast<const VarDeclExprAST*>(N)) recurse(Vd->getInitializer());
        else if (auto Bl = dynamic_cast<const BlockExprAST*>(N)) for (const auto &e : Bl->getExpressions()) recurse(e.get());
        else if (auto If = dynamic_cast<const IfExprAST*>(N)) { for (const auto &Br : If->getBranches()) { recurse(Br.Cond.get()); recurse(Br.Body.get()); } recurse(If->getElse()); }
      };

      recurse(Stmt);
    }

    if (cur != OUT[b] || curFreed != Freed[b]) {
      OUT[b] = cur;
      Freed[b] = curFreed;
      for (auto *S : BB->getSuccessors()) work.push(index[S]);
    }
  }
}
