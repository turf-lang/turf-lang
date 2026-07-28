#ifndef ANALYSIS_H
#define ANALYSIS_H

#include "CFG.h"
#include "SymbolTable.h"
#include <string>
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

// Struct-constructor definite-field-assignment analysis: `fn StructName.new(
// ...): void { ... }` must assign every one of StructName's declared fields
// via `self.<field> = ...` on EVERY path from entry to the constructor's
// exit -- a real, control-flow-sensitive check, not just "is it assigned
// SOMEWHERE in the body" (an `if` that only assigns a field in one branch
// must still be caught). Mirrors DefiniteAssignmentPass's forward dataflow
// exactly (same meet-as-intersection-at-merge-points shape), just tracking
// field names instead of VariableSymbol* identities, and checking the
// fixed-point set at the CFG's EXIT block rather than at each use site --
// there is no "use" of a field to check here, only whether it was ever
// written on every path that reaches the end of the function.
class ConstructorFieldAssignmentPass {
public:
  // Run the pass on `Cfg` (a `StructName.new` constructor's own CFG).
  // `RequiredFields` is every field `StructName` declares, in any order.
  // Raises IncompleteConstructorError (naming every field missed on at
  // least one path) if the meet-over-all-paths set at the CFG's exit
  // doesn't cover all of them.
  void runOnCFG(CFG &Cfg, const std::string &StructName,
                const std::vector<std::string> &RequiredFields,
                SourceLocation ConstructorLoc);
};

#endif
