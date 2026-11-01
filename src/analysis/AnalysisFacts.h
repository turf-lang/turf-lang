#ifndef ANALYSIS_FACTS_H
#define ANALYSIS_FACTS_H

// Facts the semantic passes record about AST nodes for the passes after them,
// keyed by ExprAST::getNodeID() instead of flags on the nodes (F-S4a's side
// table, docs/planning/ARCHITECTURE_SIMPLIFICATION.md). Owned by a compilation
// like its symbol table (F-S2): the driver's compile and each query-facade
// analysis own one and make it current with an AnalysisFactsScope, so a nested
// analysis (a compiler fix's re-check) never sees the outer one's facts.

#include <cstdint>
#include <unordered_set>

struct AnalysisFacts {
  // AN-6 (E181): an `int` expression converted to `double`, explicitly (`x to
  // double`) or implicitly (stored into, passed as, or returned as a double).
  // The value-range walk warns at each one it cannot prove within +-2^53.
  std::unordered_set<uint64_t> IntToDouble;

  // NM-2: the expressions the TypeChecker gave type `int` every time it
  // checked them. The value-range analysis trusts a range only for these: a
  // comparison against a `double` or an operand of unknown type narrows
  // nothing. A node also seen with another type (a generic body checked once
  // per instantiation) is never in IntExprs.
  std::unordered_set<uint64_t> IntExprs;
  std::unordered_set<uint64_t> NotIntExprs;

  void noteType(uint64_t Id, bool IsInt) {
    if (IsInt && !NotIntExprs.count(Id)) {
      IntExprs.insert(Id);
    } else if (!IsInt) {
      IntExprs.erase(Id);
      NotIntExprs.insert(Id);
    }
  }
  bool isInt(uint64_t Id) const { return IntExprs.count(Id) > 0; }

  // OPT-002 / NM-2: checks ValueRangePass::markElidableChecks() proved can
  // never fire, so Codegen emits none (F-OPT-2: removal by proof only, never
  // by `-O` level). ProvenNoOverflow: a checked `int` `+`, `-`, `*`, `<<`,
  // unary `-`, `op=` or `++`/`--`, emitted as plain `nsw` arithmetic.
  // ProvenInBounds: an index into a fixed-size array, emitted without its
  // bounds check. A clone (a monomorphized body) has new node ids and is
  // marked by its own analysis.
  std::unordered_set<uint64_t> ProvenNoOverflow;
  std::unordered_set<uint64_t> ProvenInBounds;

  // F-S4a: the functions (FuncDefExprAST node ids) whose flow passes the
  // phase before Codegen already ran (runProgramFlowPasses()).
  std::unordered_set<uint64_t> FlowChecked;
};

// The current compilation's facts, or null outside any.
AnalysisFacts *Facts();

// Whether the node with this id was proven to need no overflow / bounds
// check (false outside a compilation).
inline bool provenNoOverflow(uint64_t Id) {
  const AnalysisFacts *F = Facts();
  return F && F->ProvenNoOverflow.count(Id);
}
inline bool flowChecked(uint64_t Id) {
  const AnalysisFacts *F = Facts();
  return F && F->FlowChecked.count(Id);
}
inline bool provenInBounds(uint64_t Id) {
  const AnalysisFacts *F = Facts();
  return F && F->ProvenInBounds.count(Id);
}

class AnalysisFactsScope {
public:
  explicit AnalysisFactsScope(AnalysisFacts *F);
  ~AnalysisFactsScope();
  AnalysisFactsScope(const AnalysisFactsScope &) = delete;
  AnalysisFactsScope &operator=(const AnalysisFactsScope &) = delete;

private:
  AnalysisFacts *Saved;
};

#endif
