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

// LANG-002: a forward dataflow analysis over the CFG, structurally the same
// shape as DefiniteAssignmentPass just above (a std::unordered_set<const
// VariableSymbol*> per block, meet = intersection at merge points), tracking
// which `&?T`-typed local/parameter variables are PROVEN non-null at each
// program point. Two-state, not the three-state (NonNull/Nullable/Null)
// lattice DESIGN_NOTES.md sketches: "proven non-null, or not" is all either
// check below needs, and the simpler set-membership shape lets this reuse
// DefiniteAssignmentPass's exact meet/fixed-point code, unchanged in
// structure. (A symbol absent from the current set means "not proven" --
// covers both "still nullable" and "just compared equal to null", which
// this pass never needs to tell apart.)
//
// Narrowing sources (docs/architecture/pointers_and_references.md-adjacent,
// this group's own change record has the full list): a direct comparison
// `var != null` / `var == null` against a tracked variable, threaded through
// `&&`/`||` short-circuit chains the same way the language actually
// evaluates them (see EvalNarrow() in Analysis.cpp). Deliberately
// intraprocedural and direct-comparison-only -- a field access, a method
// call, or a comparison against anything other than the literal `null`
// keyword narrows nothing (§8 LANG-002's own "start deliberately local"
// guidance). Re-assigning a tracked variable resets its state from the
// assigned expression's own shape (an address-of always narrows to
// non-null; a bare `null` literal or an opaque call result never does).
//
// Two independent checks share this one dataflow, because both ask the
// identical question ("is this &?T expression proven non-null right here")
// of a different AST node kind:
//   1. Field access through a `&?T` base not proven non-null --
//      PossibleNullDereferenceError (E081).
//   2. An explicit `p to &T` cast whose operand is a `&?T` not proven
//      non-null -- UnnarrowedNullableCastError (E082). This is the ONLY
//      way a `&?T` ever becomes a `&T` (typesCompatible() never grants it
//      implicitly -- see TurfNullableReferenceType's doc comment, Types.h),
//      so this is where "&?T -> &T only after narrowing" is actually
//      enforced.
class NullabilityPass {
public:
  // Run the pass on `Cfg`. Parameters need no separate initial-set argument
  // (unlike DefiniteAssignmentPass's InitialAssigned) -- every `&?T`-typed
  // parameter starts life correctly absent from the entry set (i.e.
  // "not proven non-null"), which is exactly right: the caller might have
  // passed `null`.
  void runOnCFG(CFG &Cfg);
};

// FDN-002 (docs/planning/ROADMAP_v2.0.0.md; FREEZE.md §6.13, F-MEM-5): an
// intraprocedural, conservative FORWARD dataflow pass -- structurally the
// same shape as NullabilityPass just above (one std::unordered_map per
// block, meet = "keep an entry only where every predecessor agrees",
// transfer = process a block's statements in order) -- that tracks, for
// every reference-typed (&T/&?T/self/a raw pointer) or unsized-array (T[])
// PARAMETER/local, WHERE the value it currently holds points to: its
// provenance. A reference into THIS function's own stack storage must never
// be smuggled out through a return, a field of a longer-lived (Param- or
// Heap-owned) object, a collection mutator/`operator[]=`, or heap()'s own
// operand -- each would silently dangle the moment this frame pops (the
// audit's F-16).
//
// Three tracked states, in `RefProvenance` below; a symbol ABSENT from the
// current block's map is the fourth, permissive default -- Unknown, exactly
// like NonNullSet's "absence means not proven" convention. Unknown is NEVER
// flagged: this pass only ever raises DanglingReferenceEscapeError for a
// value it has POSITIVELY proven is Stack-provenance, never for one it
// merely failed to classify (see EscapePass.cpp's classifyProvenance() doc
// comment for the exact, deliberately small set of expression shapes this
// covers, and docs/development/changes/FDN-002.md's "Known pitfalls" for
// the residual gap this leaves: a reference smuggled out inside a by-value
// struct copy, or through an intermediate function call, is NOT caught).
//
// A Param- or Heap-provenance value is always allowed to flow anywhere,
// including a return -- this pass has no interprocedural summaries (a
// deliberate Non-goal, Overhaul §0.6's "small language" constraint), so it
// extends the SAME trust ordinary reference-parameter semantics already
// grant (the caller is responsible for what it hands in) rather than trying
// to verify further up the call chain.
enum class RefProvenance { Stack, Param, Heap };

class EscapePass {
public:
  void runOnCFG(CFG &Cfg);
};

// Every flow check a function gets once its CFG is built, in order: missing
// return / unreachable code (CFG::reportFlowDiagnostics()), the complexity and
// infinite-loop warnings, definite assignment, value range, alias (E017/E018),
// nullability, escape, liveness, recursion without a base case, and, for a
// `fn S.new()`, the constructor's field assignment (needs GlobalSymbolTable to
// know S). The one sequence both FuncDefExprAST::codegen() and the query
// facade's AnalysisOptions::RunFlowAnalysis run, so they cannot drift apart.
// An error raises, ending the sequence for this function.
//
// @param Name     The function's name as declared (`S.new` for a constructor).
// @param Loc      Where the warnings and the recursion error point.
// @param ParamIDs Parameter symbols, for the passes that take them (none of
//                 them reads it today).
void runFunctionFlowPasses(CFG &Cfg, const std::string &Name, SourceLocation Loc,
                           const std::vector<SymbolID> &ParamIDs);

#endif
