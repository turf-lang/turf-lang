// AN-3 (a): see MutualRecursion.h.

#include "analysis/MutualRecursion.h"

#include "diagnostics/Errors.h"
#include "frontend/Parser.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace MutualRecursion {

void report(const std::vector<std::unique_ptr<CFG>> &Cfgs) {
  // One node per mangled name; a function codegenned twice (it should not
  // be) keeps its first CFG.
  std::unordered_map<std::string, size_t> Index;
  std::vector<const CFG *> Nodes;
  for (const auto &C : Cfgs)
    if (C && !C->getMangledFunctionName().empty() && Index.emplace(C->getMangledFunctionName(), Nodes.size()).second)
      Nodes.push_back(C.get());
  const size_t N = Nodes.size();
  std::vector<std::vector<size_t>> Edges(N);
  for (size_t I = 0; I < N; ++I)
    for (const std::string &Callee : Nodes[I]->calledFunctions())
      if (auto It = Index.find(Callee); It != Index.end() && It->second != I)
        Edges[I].push_back(It->second);

  // Tarjan's strongly connected components.
  std::vector<int> Order(N, -1), Low(N, 0);
  std::vector<bool> OnStack(N, false);
  std::vector<size_t> Stack;
  std::vector<std::vector<size_t>> Components;
  int Counter = 0;
  std::function<void(size_t)> Visit = [&](size_t V) {
    Order[V] = Low[V] = Counter++;
    Stack.push_back(V);
    OnStack[V] = true;
    for (size_t W : Edges[V]) {
      if (Order[W] < 0) {
        Visit(W);
        Low[V] = std::min(Low[V], Low[W]);
      } else if (OnStack[W]) {
        Low[V] = std::min(Low[V], Order[W]);
      }
    }
    if (Low[V] == Order[V]) {
      std::vector<size_t> Comp;
      size_t W;
      do {
        W = Stack.back();
        Stack.pop_back();
        OnStack[W] = false;
        Comp.push_back(W);
      } while (W != V);
      if (Comp.size() > 1)
        Components.push_back(std::move(Comp));
    }
  };
  for (size_t V = 0; V < N; ++V)
    if (Order[V] < 0)
      Visit(V);

  for (auto &Comp : Components) {
    std::unordered_set<std::string> Names;
    for (size_t V : Comp)
      Names.insert(Nodes[V]->getMangledFunctionName());
    bool NoBase = true;
    for (size_t V : Comp)
      NoBase = NoBase && Nodes[V]->allPathsCallAnyOf(Names);
    if (!NoBase)
      continue;
    // Report at the cycle's first function in source order, naming them all
    // in that order.
    std::sort(Comp.begin(), Comp.end(), [&](size_t A, size_t B) {
      const SourceLocation &LA = Nodes[A]->getFuncLoc(), &LB = Nodes[B]->getFuncLoc();
      return LA.Line != LB.Line ? LA.Line < LB.Line : LA.Col < LB.Col;
    });
    const CFG *First = Nodes[Comp.front()];
    if (IsLibStdWarningLocation(First->getFuncLoc().Filename))
      continue;
    std::string Cycle;
    for (size_t V : Comp)
      Cycle += (Cycle.empty() ? "`" : ", `") + Nodes[V]->getFunctionName() + "`";
    MutualRecursionWarning W(First->getFuncLoc(), Cycle);
    for (size_t I = 1; I < Comp.size(); ++I)
      W.withRelated(Nodes[Comp[I]]->getFuncLoc(),
                    "`" + Nodes[Comp[I]]->getFunctionName() + "` is part of the cycle");
    W.emit();
  }
}

} // namespace MutualRecursion
