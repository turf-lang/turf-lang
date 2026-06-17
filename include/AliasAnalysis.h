#ifndef ALIAS_ANALYSIS_H
#define ALIAS_ANALYSIS_H

#include "CFG.h"
#include "SymbolTable.h"
#include <unordered_map>

// Simple intra-procedural alias/lifetime analysis.
// Tracks allocation sites and freed state to detect use-after-free and
// double-free within a function.
class AliasAnalysisPass {
public:
  void runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned);
};

#endif
