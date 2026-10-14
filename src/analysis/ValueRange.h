#ifndef VALUE_RANGE_H
#define VALUE_RANGE_H

#include "analysis/CFG.h"
#include "resolve/SymbolTable.h"
#include <optional>
#include <unordered_map>
#include <vector>

// Simple value-range (constant) propagation pass. Tracks per-symbol constant
// values when they can be determined; otherwise marks them as unknown.
class ValueRangePass {
public:
  // Run the pass on the CFG. `InitialAssigned` contains symbol IDs considered
  // initialized (e.g. parameters). The pass populates an internal map of
  // per-block constant maps (not exposed). Currently used for diagnostics
  // and later passes.
  void runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned);

  // OPT-002 (ARCHITECTURE_OVERHAUL.md §8, DESIGN_NOTES.md §1): the SAME
  // constant-propagation dataflow as runOnCFG(), run a second time on a
  // throwaway CFG built BEFORE Body->codegen() (FuncDefExprAST::codegen(),
  // CodegenFunc.cpp -- see that call site's own comment for why a second,
  // earlier CFG is unavoidable: runOnCFG() itself only ever runs AFTER
  // Body->codegen() has already emitted the LLVM IR for every runtime
  // check, so its results arrive too late to suppress anything this
  // compile emits). Never raises a diagnostic -- out-of-range constant
  // indices are still caught only by the existing, untouched runOnCFG()
  // call, at the same point it always has. The only effect here is setting
  // ArrayAccessExprAST::ProvenInBounds / {Binary,Unary,CompoundAssignment,
  // Postfix}ExprAST::ProvenNoOverflow on nodes this dataflow proves safe, so
  // Codegen can skip emitting the corresponding runtime check -- F-OPT-2:
  // removal by proof only, independent of `-O` level.
  void markElidableChecks(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned);
};

#endif
