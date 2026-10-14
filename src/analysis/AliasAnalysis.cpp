#include "analysis/AliasAnalysis.h"
#include "frontend/AST.h"
#include "diagnostics/Errors.h"
#include "diagnostics/SuggestionIndex.h"
#include "resolve/SymbolTable.h"

#include <functional>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <optional>

using AllocSiteID = size_t;

namespace OwnershipAnnotation {

bool acquires(const ResolverFunctionSymbol &Callee) {
  for (const Annotation *A : Callee.annotationsNamed(Name))
    if (A->hasWord("acquires"))
      return true;
  return false;
}

namespace {

// calledFunction(), plus the call's arguments in parameter order when `Args`
// is given: a UFCS call's receiver fills the first parameter.
const ResolverFunctionSymbol *callTarget(const ExprAST &Call,
                                         std::vector<const ExprAST *> *Args) {
  if (const auto *FC = dynamic_cast<const FuncCallExprAST *>(&Call)) {
    if (Args)
      for (const auto &A : FC->getArgs())
        Args->push_back(A.get());
    return FC->getResolvedCallTarget();
  }
  const auto *MC = dynamic_cast<const MethodCallExprAST *>(&Call);
  if (!MC || !MC->ResolvedViaUFCS)
    return nullptr;
  if (Args) {
    Args->push_back(MC->getBase());
    for (const auto &A : MC->getArgs())
      Args->push_back(A.get());
  }
  return MC->getResolvedCallTarget();
}

} // namespace

const ResolverFunctionSymbol *calledFunction(const ExprAST &Call) {
  return callTarget(Call, nullptr);
}

bool releases(const ExprAST &Call) {
  const ResolverFunctionSymbol *Target = calledFunction(Call);
  if (!Target)
    return false;
  for (const Annotation *A : Target->annotationsNamed(Name))
    if (A->findKeyed("releases"))
      return true;
  return false;
}

const ExprAST *releasedArgument(const ExprAST &Call) {
  std::vector<const ExprAST *> Args;
  const ResolverFunctionSymbol *Target = callTarget(Call, &Args);
  if (!Target)
    return nullptr;
  for (const Annotation *A : Target->annotationsNamed(Name)) {
    const AnnotationArg *Released = A->findKeyed("releases");
    if (!Released)
      continue;
    for (size_t I = 0; I < Target->Params.size() && I < Args.size(); ++I)
      if (Target->Params[I].Name == Released->Value)
        return Args[I];
  }
  return nullptr;
}

std::string validate(const Annotation &Ann,
                     const std::vector<std::pair<std::string, TurfTypePtr>> &Params,
                     const TurfTypePtr &ReturnType, bool /*IsExtern*/,
                     const std::vector<Annotation> & /*All*/) {
  if (Ann.Args.empty())
    return "'@ownership' needs an argument: 'acquires' or 'releases: <parameter>'.";

  // The pass reads the annotation off function CALLS; an instance method is
  // reached through `x.method()`, which it does not look at, so the claim
  // would silently check nothing. A static method (`T::m()`) is a call.
  if (!Params.empty() && Params[0].first == "self")
    return "'@ownership' cannot go on an instance method yet: the double-free "
           "check reads it only from calls to plain functions and static "
           "methods.";

  const std::string WordsNotStrings =
      "The arguments of '@ownership' are words, not strings: 'acquires' or "
      "'releases: <parameter>'.";

  for (const AnnotationArg &Arg : Ann.Args) {
    if (Arg.Key.empty()) {
      if (Arg.IsString)
        return WordsNotStrings;
      if (Arg.Value == "acquires") {
        if (!ReturnType || !ReturnType->isPointer())
          return "'@ownership(acquires)' says the function returns a fresh "
                 "allocation, but it does not return a pointer.";
        continue;
      }
      if (Arg.Value == "releases")
        return "'releases' needs the parameter it frees: "
               "'@ownership(releases: p)'.";
      std::string Msg = "Unknown '@ownership' argument '" + Arg.Value + "'.";
      auto Near = SuggestionIndex::suggestVisible(Arg.Value, {"acquires", "releases"});
      if (!Near.empty())
        Msg += " Did you mean '" + Near.front() + "'?";
      return Msg + " Use 'acquires' or 'releases: <parameter>'.";
    }

    if (Arg.Key != "releases")
      return "Unknown '@ownership' argument '" + Arg.Key + ":'. The only keyed "
             "argument is 'releases: <parameter>'.";
    if (Arg.IsString)
      return WordsNotStrings;
    const std::pair<std::string, TurfTypePtr> *Param = nullptr;
    for (const auto &P : Params)
      if (P.first == Arg.Value)
        Param = &P;
    if (!Param)
      return "'@ownership(releases: " + Arg.Value + ")' names '" + Arg.Value +
             "', which is not a parameter of this function.";
    if (!Param->second || !Param->second->isPointer())
      return "'@ownership(releases: " + Arg.Value + ")' says the call frees '" +
             Arg.Value + "', but that parameter is not a pointer.";
    // A `&T`/`&?T` is GC-managed: nothing frees it, so the claim cannot be true.
    if (Param->second->isReference() || Param->second->isNullableReference())
      return "'@ownership(releases: " + Arg.Value + ")' says the call frees '" +
             Arg.Value + "', but that parameter is a reference ('&T'), which "
             "the garbage collector owns; only a raw pointer is released.";
  }
  return "";
}

} // namespace OwnershipAnnotation

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

  // The dataflow fact at one program point.
  //
  // Freed state is tracked twice, because the two answers mean different
  // things to the user. MustFreed holds the sites released on EVERY path
  // reaching this point (intersection at a join): a second release or a use
  // is certainly wrong, so it is an error. MayFreed holds the sites released
  // on SOME path (union at a join): whether a second release is wrong depends
  // on a condition this pass cannot evaluate, e.g.
  //
  //     if flag { mem::free(p); freed = true; }
  //     if !freed { mem::free(p); }
  //
  // which is correct code, so that is a warning, never an error.
  struct State {
    std::unordered_map<VarKey, std::optional<AllocSiteID>> Points;
    std::unordered_set<AllocSiteID> MayFreed;
    std::unordered_set<AllocSiteID> MustFreed;
    bool operator==(const State &) const = default;
  };
  std::vector<State> IN(N), OUT(N);
  // A block's OUT means something only once it has been processed; a join
  // ignores the predecessors that have not been (the standard optimistic
  // start for an intersection), so a loop's back edge does not empty
  // MustFreed before the loop body has even been looked at.
  std::vector<bool> Visited(N, false);

  // An allocation site is the acquiring call itself, so it keeps one ID however
  // often the worklist revisits its block and the fixpoint is reached.
  std::unordered_map<const ExprAST *, AllocSiteID> SiteIDs;
  auto siteFor = [&SiteIDs](const ExprAST *Call) -> AllocSiteID {
    return SiteIDs.try_emplace(Call, SiteIDs.size() + 1).first->second;
  };

  // Where each AllocSiteID was first released, so DoubleFreeError /
  // UseAfterFreeError (and their "may" warnings) can pin that call site as a
  // RelatedLocation ("freed here") instead of leaving the user to search the
  // function for it. Filled during the reporting pass, in block order.
  std::unordered_map<AllocSiteID, SourceLocation> FreeLoc;

  // Worklist
  std::queue<size_t> work;
  for (size_t i = 0; i < N; ++i) work.push(i);

  // Helper: get the resolved identity of a VariableExprAST (or nullptr)
  auto getVarSymbol = [](const VariableExprAST *V) -> VarKey {
    if (!V) return nullptr;
    return V->getResolvedSymbol();
  };

  // Helper: does this call's resolved target declare `@ownership(acquires)`?
  // Asked of the callee's declaration, never of its name (Article V).
  // A call is a FuncCallExprAST or a UFCS method call (calledFunction()).
  auto isAcquire = [](const ExprAST *E) -> bool {
    const ResolverFunctionSymbol *Target =
        E ? OwnershipAnnotation::calledFunction(*E) : nullptr;
    return Target && OwnershipAnnotation::acquires(*Target);
  };

  // Helper: the variable a releasing call frees, or null if the call releases
  // nothing, or releases something that is not a plain variable.
  auto releasedVariable = [](const ExprAST *E) -> const VariableExprAST * {
    return dynamic_cast<const VariableExprAST *>(OwnershipAnnotation::releasedArgument(*E));
  };

  // Where a call is, for a diagnostic (ExprAST has no virtual getLoc()).
  auto callLoc = [](const ExprAST *E) -> SourceLocation {
    if (auto *MC = dynamic_cast<const MethodCallExprAST *>(E))
      return MC->getLoc();
    return static_cast<const FuncCallExprAST *>(E)->getLoc();
  };

  // `V`'s allocation is released at `Loc`. With `Report`, a release of
  // something already released is reported first (an error if it was
  // released on every path here, a warning if only on some).
  auto release = [&](State &S, const VariableExprAST *V, SourceLocation Loc,
                     bool Report) {
    VarKey key = getVarSymbol(V);
    if (!key) return;
    auto it = S.Points.find(key);
    if (it == S.Points.end() || !it->second.has_value()) return;
    AllocSiteID aid = *(it->second);
    if (Report) {
      auto FreeIt = FreeLoc.find(aid);
      if (S.MustFreed.count(aid)) {
        DoubleFreeError err(Loc, V->getName());
        if (FreeIt != FreeLoc.end())
          err.withRelated(FreeIt->second, "first freed here");
        err.raise();
      } else if (S.MayFreed.count(aid)) {
        PossibleDoubleFreeWarning W(Loc, V->getName());
        if (FreeIt != FreeLoc.end())
          W.withRelated(FreeIt->second, "freed here, on some paths");
        W.emit();
      } else {
        FreeLoc.try_emplace(aid, Loc);
      }
    }
    // Keep the mapping so later accesses can detect use-after-free
    S.MayFreed.insert(aid);
    S.MustFreed.insert(aid);
  };

  // `V`'s memory is read or written at `Loc` (only checked when reporting).
  auto use = [&](const State &S, const VariableExprAST *V, SourceLocation Loc,
                 bool Report) {
    if (!Report) return;
    VarKey key = getVarSymbol(V);
    if (!key) return;
    auto it = S.Points.find(key);
    if (it == S.Points.end() || !it->second.has_value()) return;
    AllocSiteID aid = *(it->second);
    auto FreeIt = FreeLoc.find(aid);
    if (S.MustFreed.count(aid)) {
      UseAfterFreeError err(Loc, V->getName());
      if (FreeIt != FreeLoc.end())
        err.withRelated(FreeIt->second, "freed here");
      err.raise();
    } else if (S.MayFreed.count(aid)) {
      PossibleUseAfterFreeWarning W(Loc, V->getName());
      if (FreeIt != FreeLoc.end())
        W.withRelated(FreeIt->second, "freed here, on some paths");
      W.emit();
    }
  };

  // `key` now holds a fresh allocation from the acquiring call `Call`. Whatever
  // that site handed out earlier (a previous loop iteration) is a different
  // block, so the site is no longer freed.
  auto acquire = [&](State &S, VarKey key, const ExprAST *Call) {
    AllocSiteID aid = siteFor(Call);
    S.MayFreed.erase(aid);
    S.MustFreed.erase(aid);
    S.Points[key] = aid;
  };

  // IN of block `b`: the join of its processed predecessors' OUT.
  auto join = [&](size_t b) -> State {
    State In;
    bool first = true;
    for (auto *P : Blocks[b]->getPredecessors()) {
      size_t pi = index[P];
      if (!Visited[pi]) continue;
      const State &PO = OUT[pi];
      if (first) { In = PO; first = false; continue; }
      // points-to: keep a value only if equal across preds, else unknown
      for (auto &kv : In.Points) {
        auto it = PO.Points.find(kv.first);
        if (it == PO.Points.end() || it->second != kv.second)
          kv.second = std::nullopt;
      }
      for (AllocSiteID aid : PO.MayFreed) In.MayFreed.insert(aid);
      for (auto It = In.MustFreed.begin(); It != In.MustFreed.end();)
        It = PO.MustFreed.count(*It) ? std::next(It) : In.MustFreed.erase(It);
    }
    return In;
  };

  // The effect of block `b`'s statements on `S`; diagnostics only with
  // `Report`, which is set once, after the fixpoint (below), so nothing is
  // reported from a fact a later iteration would have changed.
  auto transfer = [&](size_t b, State S, bool Report) -> State {
    for (ExprAST *Stmt : Blocks[b]->getStatements()) {
      // Detect assignment of malloc: x = malloc(n) or alias: p = q
      if (auto A = dynamic_cast<AssignmentExprAST *>(Stmt)) {
        if (auto L = dynamic_cast<const VariableExprAST *>(A->getLHS())) {
          VarKey lhsKey = getVarSymbol(L);
          if (lhsKey) {
            bool handled = false;
            if (isAcquire(A->getRHS())) {
              acquire(S, lhsKey, A->getRHS());
              handled = true;
            } else if (auto RHSVar = dynamic_cast<const VariableExprAST *>(A->getRHS())) {
              VarKey rhsKey = getVarSymbol(RHSVar);
              if (rhsKey && S.Points.count(rhsKey)) {
                S.Points[lhsKey] = S.Points.at(rhsKey);
              } else {
                S.Points.erase(lhsKey);
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
          if (isAcquire(Vd->getInitializer())) {
            acquire(S, key, Vd->getInitializer());
            handled = true;
          } else if (auto InitVar = dynamic_cast<const VariableExprAST *>(Vd->getInitializer())) {
            VarKey rhsKey = getVarSymbol(InitVar);
            if (rhsKey && S.Points.count(rhsKey)) {
              S.Points[key] = S.Points.at(rhsKey);
            } else {
              S.Points.erase(key);
            }
            handled = true;
          }
          if (handled) continue;
        }
      }

      // A call to a function declared `@ownership(releases: p)`, e.g. free(x)
      // or x.free()
      if (OwnershipAnnotation::releases(*Stmt)) {
        if (auto V = releasedVariable(Stmt))
          release(S, V, callLoc(Stmt), Report);
        continue;
      }

      // Array access -> check if base points to a freed alloc site
      if (auto AA = dynamic_cast<ArrayAccessExprAST *>(Stmt)) {
        if (auto BaseV = dynamic_cast<const VariableExprAST *>(AA->getBase()))
          use(S, BaseV, AA->getLoc(), Report);
      }

      // Recurse into statement children for nested calls / accesses
      std::function<void(const ExprAST*)> recurse = [&](const ExprAST *N) {
        if (!N) return;
        // a releasing call inside an expression
        if (auto V = releasedVariable(N))
          release(S, V, callLoc(N), Report);
        if (auto AA2 = dynamic_cast<const ArrayAccessExprAST*>(N)) {
          if (auto BaseV = dynamic_cast<const VariableExprAST*>(AA2->getBase()))
            use(S, BaseV, AA2->getLoc(), Report);
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
    return S;
  };

  while (!work.empty()) {
    size_t b = work.front(); work.pop();
    IN[b] = join(b);
    State Out = transfer(b, IN[b], /*Report=*/false);
    if (!Visited[b] || Out != OUT[b]) {
      Visited[b] = true;
      OUT[b] = std::move(Out);
      for (auto *Succ : Blocks[b]->getSuccessors()) work.push(index[Succ]);
    }
  }

  // Report once, from the converged facts, in block order (so "first freed
  // here" names the earlier call where both are in straight-line code).
  for (size_t b = 0; b < N; ++b)
    if (Visited[b])
      transfer(b, IN[b], /*Report=*/true);
}
