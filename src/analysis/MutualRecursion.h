#ifndef MUTUAL_RECURSION_H
#define MUTUAL_RECURSION_H

// AN-3 (a) (TDP-2-007, issue #332): mutual recursion with no base case on any
// path. A small call graph is built from the functions' CFGs (one node per
// function, an edge per resolved direct call); in every cycle of two or more
// functions where each member's every path calls back into the cycle
// (CFG::allPathsCallAnyOf()), no call can ever return: E182, once, naming the
// whole cycle. A call through a function value or to an `extern fn` is not an
// edge (nothing is known about where it leads), so it never closes a cycle.
// Self-recursion stays E010 (runFunctionFlowPasses()).

#include "analysis/CFG.h"

#include <memory>
#include <vector>

namespace MutualRecursion {

void report(const std::vector<std::unique_ptr<CFG>> &Cfgs);

} // namespace MutualRecursion

#endif
