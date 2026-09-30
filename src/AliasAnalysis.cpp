#include "AliasAnalysis.h"
#include "AST.h"
#include "Errors.h"
#include "SymbolTable.h"

#include <unordered_map>
#include <queue>
#include <optional>

using AllocSiteID = size_t;

void AliasAnalysisPass::runOnCFG(CFG &Cfg, const std::vector<SymbolID> & /*InitialAssigned*/) {

  const auto &Blocks = Cfg.getBlocks();
  size_t N = Blocks.size();
  if (N == 0) return;

  std::unordered_map<const TurfBasicBlock *, size_t> index;
  for (size_t i = 0; i < N; ++i) index[Blocks[i].get()] = i;

  // For simplicity, we perform a forward traversal per-block maintaining a
  // points-to map for symbols (VariableSymbol* -> optional AllocSiteID) and
  // a set of freed alloc sites.
  //
  // NOTE: this map used to be keyed by std::string (the variable's raw
  // name) rather than its resolved identity. That conflates two distinct
  // variables that happen to share a name across a shadowing boundary --
  // e.g. an outer `p` that gets free()'d, and an unrelated inner `p` in a
  // nested block that shadows it. A shadowing VarDecl whose initializer is
  // neither a malloc()/realloc() call nor a plain variable alias (e.g.
  // `mut p: int* = null;`) hit neither branch below and so never
  // overwrote/cleared the stale name->AllocSiteID entry left by the OUTER
  // `p` -- so freeing the INNER (never-allocated) `p` was misread as
  // freeing the outer one a second time, raising a false "Double free
  // detected for 'p'" for perfectly safe code. Fixed by keying on each
  // VariableExprAST/VarDeclExprAST's ResolvedSymbol (a VariableSymbol* set
  // once by the Resolver, already correctly scope-and-shadowing-aware,
  // since it resolves each name to whichever declaration is lexically
  // visible at that exact point) instead of the bare name -- the same
  // identity-based fix already applied to DefiniteAssignmentPass and
  // ValueRangePass (see Analysis.cpp / ValueRange.cpp).
  using VarKey = const VariableSymbol *;
  std::vector<std::unordered_map<VarKey, std::optional<AllocSiteID>>> IN(N), OUT(N);
  std::vector<std::unordered_set<AllocSiteID>> Freed(N);

  AllocSiteID NextAllocID = 1;

  // Records where each AllocSiteID was first free()'d, so DoubleFreeError /
  // UseAfterFreeError can pin that call site as a RelatedLocation ("freed
  // here") instead of leaving the user to search the function for it.
  // Shared across the whole worklist pass (like NextAllocID); re-setting the
  // same aid's location to the same value on a later dataflow iteration is
  // harmless -- it's always the same free() call site.
  std::unordered_map<AllocSiteID, SourceLocation> FreeLoc;

  // Worklist
  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  // Helper: get the resolved identity of a VariableExprAST (or nullptr)
  auto getVarSymbol = [](const VariableExprAST *V) -> VarKey {
    if (!V) return nullptr;
    return V->getResolvedSymbol();
  };

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    TurfBasicBlock *BB = Blocks[b].get();

    // IN = merge of preds (conservative: prefer existing mapping, do not try to join alloc sites)
    std::unordered_map<VarKey, std::optional<AllocSiteID>> newIN;
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
          std::unordered_map<VarKey, std::optional<AllocSiteID>> tmp;
          for (const auto &kv : newIN) {
            VarKey key = kv.first;
            auto it = OUT[pi].find(key);
            if (it != OUT[pi].end() && it->second == kv.second) tmp[key] = kv.second;
            else tmp[key] = std::nullopt;
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
      // Detect assignment of malloc: x = malloc(n) or alias: p = q
      if (auto A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
        if (auto L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
          VarKey lhsKey = getVarSymbol(L);
          if (lhsKey) {
            bool handled = false;
            if (auto RHS = dynamic_cast<const FuncCallExprAST *>(A->getRHS())) {
              if (auto CalleeVar = dynamic_cast<const VariableExprAST *>(RHS->getCallee())) {
                std::string cname = CalleeVar->getName();
                if (cname == "malloc" || cname == "realloc") {
                  AllocSiteID aid = NextAllocID++;
                  cur[lhsKey] = aid;
                  handled = true;
                }
              }
            } else if (auto RHSVar = dynamic_cast<const VariableExprAST *>(A->getRHS())) {
              VarKey rhsKey = getVarSymbol(RHSVar);
              if (rhsKey && cur.count(rhsKey)) {
                cur[lhsKey] = cur.at(rhsKey);
              } else {
                cur.erase(lhsKey);
              }
              handled = true;
            }
            if (handled) continue;
          }
        }
      }

      // VarDecl with initializer = malloc() or alias: mut p: ptr<int> = q;
      if (auto Vd = dynamic_cast<VarDeclExprAST *>(Stmt)) {
        VarKey key = Vd->getResolvedSymbol();
        if (key) {
          bool handled = false;
          if (auto Init = dynamic_cast<const FuncCallExprAST *>(Vd->getInitializer())) {
            if (auto CalleeVar = dynamic_cast<const VariableExprAST *>(Init->getCallee())) {
              std::string cname = CalleeVar->getName();
              if (cname == "malloc" || cname == "realloc") {
                AllocSiteID aid = NextAllocID++;
                cur[key] = aid;
                handled = true;
              }
            }
          } else if (auto InitVar = dynamic_cast<const VariableExprAST *>(Vd->getInitializer())) {
            VarKey rhsKey = getVarSymbol(InitVar);
            if (rhsKey && cur.count(rhsKey)) {
              cur[key] = cur.at(rhsKey);
            } else {
              cur.erase(key);
            }
            handled = true;
          }
          if (handled) continue;
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
                VarKey key = getVarSymbol(V);
                if (key) {
                  auto it = cur.find(key);
                  if (it != cur.end() && it->second.has_value()) {
                    AllocSiteID aid = *(it->second);
                    if (curFreed.count(aid)) {

                      DoubleFreeError err(FC->getLoc(), V->getName());
                      auto FreeIt = FreeLoc.find(aid);
                      if (FreeIt != FreeLoc.end())
                        err.withRelated(FreeIt->second, "first freed here");
                      err.raise();
                    } else {

                      curFreed.insert(aid);
                      FreeLoc[aid] = FC->getLoc();
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
          VarKey key = getVarSymbol(BaseV);
          if (key) {
            auto it = cur.find(key);
            if (it != cur.end() && it->second.has_value()) {
              AllocSiteID aid = *(it->second);
              if (curFreed.count(aid)) {

                UseAfterFreeError err(AA->getLoc(), BaseV->getName());
                auto FreeIt = FreeLoc.find(aid);
                if (FreeIt != FreeLoc.end())
                  err.withRelated(FreeIt->second, "freed here");
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
                VarKey key = getVarSymbol(V);
                if (key) {
                  auto it = cur.find(key);
                  if (it != cur.end() && it->second.has_value()) {
                    AllocSiteID aid = *(it->second);
                    if (curFreed.count(aid)) {
                      DoubleFreeError err(F->getLoc(), V->getName());
                      auto FreeIt = FreeLoc.find(aid);
                      if (FreeIt != FreeLoc.end())
                        err.withRelated(FreeIt->second, "first freed here");
                      err.raise();
                    } else {
                      curFreed.insert(aid);
                      FreeLoc[aid] = F->getLoc();
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
            VarKey key = getVarSymbol(BaseV);
            if (key) {
              auto it = cur.find(key);
              if (it != cur.end() && it->second.has_value()) {
                AllocSiteID aid = *(it->second);
                if (curFreed.count(aid)) {
                  UseAfterFreeError err(AA2->getLoc(), BaseV->getName());
                  auto FreeIt = FreeLoc.find(aid);
                  if (FreeIt != FreeLoc.end())
                    err.withRelated(FreeIt->second, "freed here");
                  err.raise();
                }
              }
            }
          }
        }

        // descend
        if (auto B = dynamic_cast<const BinaryExprAST*>(N)) { recurse(B->getLHS()); recurse(B->getRHS()); }
        else if (auto U = dynamic_cast<const UnaryExprAST*>(N)) recurse(U->getOperand());
        else if (auto Tr = dynamic_cast<const TryExprAST*>(N)) recurse(Tr->getOperand()); // LEC-004
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
