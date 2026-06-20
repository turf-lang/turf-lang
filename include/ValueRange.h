#ifndef VALUE_RANGE_H
#define VALUE_RANGE_H

#include "CFG.h"
#include "SymbolTable.h"
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
};

#endif
