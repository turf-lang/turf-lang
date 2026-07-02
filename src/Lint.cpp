/**
 * @file Lint.cpp
 * @brief Static analysis and code linting checks.
 * @details Traverses the AST to find redundant operations, dead branches,
 *          infinite loops, self-assignments, and semantic warnings prior
 *          to code generation.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Lint.h"
#include "AST.h"
#include "Errors.h"
#include "Lexer.h"
#include "Types.h"
#include <string>

// Helper: structural AST fingerprinting (for identical-branch detection)
static std::string Fingerprint(const ExprAST *E) {
  if (!E)
    return "<null>";

  if (auto *N = dynamic_cast<const NumberExprAST *>(E)) {
    if (N->isInteger())
      return "int:" + std::to_string(N->getIntVal());
    return "dbl:" + std::to_string(N->getDoubleVal());
  }

  if (auto *B = dynamic_cast<const BoolExprAST *>(E)) {
    return B->getVal() ? "bool:true" : "bool:false";
  }

  if (auto *S = dynamic_cast<const StringExprAST *>(E)) {
    return "str:" + S->getValue();
  }

  if (auto *V = dynamic_cast<const VariableExprAST *>(E)) {
    return "var:" + V->getName();
  }

  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    std::string S = "block{";

    for (const auto &Child : Blk->getExpressions()) {
      S += Fingerprint(Child.get()) + ";";
    }
    S += "}";
    return S;
  }

  if (auto *Bin = dynamic_cast<const BinaryExprAST *>(E)) {
    return "binop:" + std::to_string(Bin->getOp()) + "(" +
           Fingerprint(Bin->getLHS()) + "," + Fingerprint(Bin->getRHS()) + ")";
  }

  // For unsupported complex nodes, return a unique instance-based fingerprint
  // using the pointer address. This prevents false-positive "identical
  // branches" warnings when both branches contain structurally different
  // complex statements.
  return "<unique@" + std::to_string(reinterpret_cast<uintptr_t>(E)) + ">";
}

// Helper: check if an AST subtree contains break/continue/return
static bool ContainsLoopExit(const ExprAST *E) {
  if (!E)
    return false;

  if (dynamic_cast<const BreakExprAST *>(E))
    return true;

  if (dynamic_cast<const ContinueExprAST *>(E))

    return true;
  if (dynamic_cast<const ReturnExprAST *>(E))
    return true;

  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions()) {
      if (ContainsLoopExit(Child.get()))
        return true;
    }
  }

  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    for (const auto &B : If->getBranches()) {
      if (ContainsLoopExit(B.Body.get()))
        return true;
    }
    if (ContainsLoopExit(If->getElseBody()))
      return true;
  }

  if (auto *W = dynamic_cast<const WhileExprAST *>(E)) {
    if (ContainsLoopExit(W->getBody()))
      return true;
  }

  if (auto *F = dynamic_cast<const ForExprAST *>(E)) {
    if (ContainsLoopExit(F->getBody()))
      return true;
  }

  return false;
}

// Helper: check if a block is effectively empty
static bool IsEffectivelyEmpty(const ExprAST *E) {
  if (!E)
    return true;

  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    if (Blk->getExpressions().empty())
      return true;
    for (const auto &Child : Blk->getExpressions()) {

      if (!IsEffectivelyEmpty(Child.get()))
        return false;
    }
    return true;
  }

  return false;
}

// Helper: check if a NumberExprAST is a zero literal
static bool IsZeroLiteral(const ExprAST *E) {
  if (!E)
    return false;
  if (auto *N = dynamic_cast<const NumberExprAST *>(E)) {
    if (N->isInteger())
      return N->getIntVal() == 0;
    return N->getDoubleVal() == 0.0;
  }
  return false;
}

// Forward declarations
static void LintBinaryExpr(const BinaryExprAST *Bin);
static void LintAssignment(const AssignmentExprAST *Assign);
static void LintWhile(const WhileExprAST *While);
static void LintIf(const IfExprAST *If);

static void LintFor(const ForExprAST *For);

// Main entry point
/**
 * @brief Recursively lints an AST expression or statement.
 *
 * Evaluates the node and its children for potential semantic warnings
 * (e.g., redundant comparisons, infinite loops, empty branches). Warnings
 * are emitted via the DiagnosticEngine.
 *
 * @param E The AST node to check.
 */
void LintExpression(ExprAST *E) {
  if (!E)
    return;

  if (auto *Bin = dynamic_cast<BinaryExprAST *>(E)) {
    LintBinaryExpr(Bin);
    LintExpression(const_cast<ExprAST*>(Bin->getLHS()));
    LintExpression(const_cast<ExprAST*>(Bin->getRHS()));
  }

  if (auto *Assign = dynamic_cast<AssignmentExprAST *>(E)) {
    LintAssignment(Assign);
    LintExpression(const_cast<ExprAST*>(Assign->getLHS()));
    LintExpression(const_cast<ExprAST*>(Assign->getRHS()));
  }

  if (auto *While = dynamic_cast<WhileExprAST *>(E)) {
    LintWhile(While);
  }

  if (auto *For = dynamic_cast<ForExprAST *>(E)) {
    LintFor(For);
  }

  if (auto *If = dynamic_cast<IfExprAST *>(E)) {
    LintIf(If);
  }

  if (auto *Ret = dynamic_cast<ReturnExprAST *>(E)) {
    LintExpression(const_cast<ExprAST*>(Ret->getVal()));
  }

  if (auto *FD = dynamic_cast<FuncDefExprAST *>(E)) {
    LintExpression(FD->getBodyMut());
  }

  // Recurse into block children
  if (auto *Blk = dynamic_cast<BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions()) {
      LintExpression(Child.get());
    }
  }
}

// Check: redundant boolean comparison, self-comparison, division by zero
static void LintBinaryExpr(const BinaryExprAST *Bin) {
  if (!Bin)
    return;

  int Op = Bin->getOp();
  const ExprAST *LHS = Bin->getLHS();
  const ExprAST *RHS = Bin->getRHS();

  // Redundant boolean comparison: x == true, x != false, etc.
  if (Op == TOK_EQ || Op == TOK_NEQ) {
    bool RHSIsBool = dynamic_cast<const BoolExprAST *>(RHS) != nullptr;
    bool LHSIsBool = dynamic_cast<const BoolExprAST *>(LHS) != nullptr;

    if (RHSIsBool || LHSIsBool) {
      if (Op == TOK_EQ) {
        RedundantComparisonWarning(
            Bin->getLoc(), "comparing with 'true' or 'false' using '=='",
            "just the value itself (drop the '== true' / '== false')")
            .warn();
      } else {
        RedundantComparisonWarning(
            Bin->getLoc(), "comparing with 'true' or 'false' using '!='",
            "just the value itself, or negate it (drop the '!= true' / '!= "
            "false')")
            .warn();
      }
    }
  }

  // Self-comparison: x == x, x < x, etc.
  if (Op == TOK_EQ || Op == TOK_NEQ || Op == '<' || Op == '>' ||
      Op == TOK_LEQ || Op == TOK_GEQ) {
    std::string LFP = Fingerprint(LHS);
    std::string RFP = Fingerprint(RHS);

    // Only flag for variable references and literals, not complex expressions
    if (LFP == RFP && LFP != "<complex>" && LFP != "<null>") {
      // Don't double-report if we already flagged a redundant bool comparison
      bool AlreadyFlagged =
          (Op == TOK_EQ || Op == TOK_NEQ) &&
          (dynamic_cast<const BoolExprAST *>(LHS) != nullptr ||
           dynamic_cast<const BoolExprAST *>(RHS) != nullptr);

      if (!AlreadyFlagged) {
        std::string result;
        if (Op == TOK_EQ || Op == TOK_LEQ || Op == TOK_GEQ)
          result = "true";
        else
          result = "false";

        // Extract a readable name from the fingerprint
        std::string readableName = LFP;
        if (LFP.substr(0, 4) == "var:")
          readableName = LFP.substr(4);
        else if (LFP.substr(0, 4) == "int:")
          readableName = LFP.substr(4);
        else if (LFP.substr(0, 4) == "dbl:")
          readableName = LFP.substr(4);

        SelfComparisonWarning(Bin->getLoc(), readableName, result).warn();
      }
    }
  }

  // Division or modulo by zero literal
  if (Op == '/' || Op == '%') {
    if (IsZeroLiteral(RHS)) {
      std::string OpStr = (Op == '/') ? "/" : "%";
      DivisionByZeroLiteralWarning(Bin->getLoc(), OpStr).warn();
    }
  }
}

// Check: self-assignment (x = x)
static void LintAssignment(const AssignmentExprAST *Assign) {
  if (!Assign)
    return;

  const ExprAST *LHS = Assign->getLHS();
  const ExprAST *RHS = Assign->getRHS();

  if (auto *LVar = dynamic_cast<const VariableExprAST *>(LHS)) {
    if (auto *RVar = dynamic_cast<const VariableExprAST *>(RHS)) {
      if (LVar->getName() == RVar->getName()) {
        SelfAssignmentWarning(Assign->getLoc(), LVar->getName()).warn();
      }
    }
  }
}

static void LintWhile(const WhileExprAST *While) {
  if (!While)
    return;

  const ExprAST *Cond = While->getCond();

  // Check for while(true) — condition is a BoolExprAST with value true
  if (auto *BoolCond = dynamic_cast<const BoolExprAST *>(Cond)) {
    if (BoolCond->getVal()) {
      // Condition is literally 'true'
      if (!ContainsLoopExit(While->getBody())) {
        SuspiciousInfiniteLoopWarning(While->getLoc()).warn();
      }
    }
  }

  LintExpression(const_cast<ExprAST *>(While->getCond()));
  // Recurse into body
  LintExpression(const_cast<ExprAST *>(While->getBody()));
}

static void LintFor(const ForExprAST *For) {
  if (!For)
    return;

  LintExpression(const_cast<ExprAST *>(For->getStart()));
  LintExpression(const_cast<ExprAST *>(For->getEnd()));
  LintExpression(const_cast<ExprAST *>(For->getStep()));
  LintExpression(const_cast<ExprAST *>(For->getBody()));
}

static void LintIf(const IfExprAST *If) {
  if (!If)
    return;

  const auto &Branches = If->getBranches();

  for (size_t i = 0; i < Branches.size(); ++i) {
    // Empty then/elseif body
    if (IsEffectivelyEmpty(Branches[i].Body.get())) {
      std::string Kind = (i == 0) ? "then" : "elseif";
      EmptyBranchWarning(Branches[i].Loc, Kind).warn();
    }
    // Recurse into branch condition and body
    LintExpression(const_cast<ExprAST*>(Branches[i].Cond.get()));
    LintExpression(Branches[i].Body.get());
  }

  // Empty else body
  ExprAST *Else = If->getElseBody();
  if (Else && IsEffectivelyEmpty(Else)) {
    EmptyBranchWarning(If->getElseLoc(), "else").warn();
  }

  // Identical branches (only for simple if/else, not chains)
  if (Branches.size() == 1 && Else &&
      !IsEffectivelyEmpty(Branches[0].Body.get()) &&
      !IsEffectivelyEmpty(Else)) {
    std::string ThenFP = Fingerprint(Branches[0].Body.get());
    std::string ElseFP = Fingerprint(Else);
    if (ThenFP == ElseFP && ThenFP != "")
      IdenticalBranchesWarning(If->getLoc()).warn();
  }

  LintExpression(Else);
}

// Semantic Pruning: Only lint nodes that have changed (by node ID)
// Internal recursive helper for semantic pruning
static void
LintChangedNodesOnlyInternal(ExprAST *E,
                             const std::set<uint64_t> &ChangedNodeIDs) {
  if (!E)
    return;

  // Only lint this node if it's in the changed set
  // (or if ChangedNodeIDs is empty, lint everything)
  uint64_t NodeID = E->getNodeID();
  bool ShouldLintThisNode =
      ChangedNodeIDs.empty() || ChangedNodeIDs.count(NodeID) > 0;

  if (ShouldLintThisNode) {
    // Run normal lint checks on this node
    if (auto *Bin = dynamic_cast<BinaryExprAST *>(E))
      LintBinaryExpr(Bin);

    if (auto *Assign = dynamic_cast<AssignmentExprAST *>(E))
      LintAssignment(Assign);

    if (auto *While = dynamic_cast<WhileExprAST *>(E))
      LintWhile(While);

    if (auto *If = dynamic_cast<IfExprAST *>(E))
      LintIf(If);
  }

  // Recurse into block children regardless (they might have changed nodes)
  if (auto *Blk = dynamic_cast<BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions()) {
      LintChangedNodesOnlyInternal(Child.get(), ChangedNodeIDs);
    }
    return;
  }

  // Recurse into binary expression children
  if (auto *Bin = dynamic_cast<BinaryExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(Bin->getLHS()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(Bin->getRHS()),
                                 ChangedNodeIDs);
    return;
  }

  // Recurse into assignment children
  if (auto *Assign = dynamic_cast<AssignmentExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(Assign->getLHS()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(Assign->getRHS()),
                                 ChangedNodeIDs);
    return;
  }

  // Recurse into if expression children
  if (auto *If = dynamic_cast<IfExprAST *>(E)) {
    const auto &Branches = If->getBranches();
    for (const auto &Branch : Branches) {
      LintChangedNodesOnlyInternal(Branch.Cond.get(), ChangedNodeIDs);
      LintChangedNodesOnlyInternal(Branch.Body.get(), ChangedNodeIDs);
    }
    LintChangedNodesOnlyInternal(If->getElseBody(), ChangedNodeIDs);
    return;
  }

  // Recurse into while loop children
  if (auto *While = dynamic_cast<WhileExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(While->getCond()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(While->getBody(), ChangedNodeIDs);
    return;
  }

  // Recurse into for loop children
  if (auto *For = dynamic_cast<ForExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(For->getStart()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(For->getEnd()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(For->getStep()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(For->getBody(), ChangedNodeIDs);
    return;
  }
}

// Public API: Semantic pruning for incremental validation
void LintChangedNodesOnly(ExprAST *E,
                          const std::set<uint64_t> &ChangedNodeIDs) {
  if (!E)
    return;
  LintChangedNodesOnlyInternal(E, ChangedNodeIDs);
}
