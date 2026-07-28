#ifndef ANALYSIS_H
#define ANALYSIS_H

#include "CFG.h"
#include "SymbolTable.h"
#include <unordered_set>
#include <vector>

struct VariableSymbol;

// Definite assignment analysis pass.
// Runs a forward dataflow analysis on the provided CFG and emits diagnostics
// via the existing Errors API when it finds uses of variables that are not
// definitely assigned on all paths.
class DefiniteAssignmentPass {
public:
  // Run the pass on the given CFG. `InitialAssigned` contains symbol IDs
  // that should be considered assigned on function entry (parameters).
  void runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned);
};

// CFG Phase 2 (Issue #186): Liveness analysis pass.
// Runs a BACKWARD data-flow analysis on the provided CFG. Emits:
//   - DeadStoreWarning  when a local is written but the value is never read
//                       before it is overwritten or the function exits.
//   - UnusedVariableWarning when a declared local is never read on any path.
//
// Uses VariableSymbol* as the identity key (same as DefiniteAssignmentPass)
// to handle shadowed variables with the same name correctly.
//
// Parameters (from InitialAssigned) are never flagged — they are considered
// live on function entry by definition.
class LivenessAnalysisPass {
public:
  // Run the pass on the given CFG.
  // `ParamSymbols` contains VariableSymbol* for every function parameter;
  // they are unconditionally treated as live-in and excluded from warnings.
  void runOnCFG(CFG &Cfg,
                const std::unordered_set<const VariableSymbol *> &ParamSymbols);
};

#endif
