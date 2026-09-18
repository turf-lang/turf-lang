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
#include "llvm/ADT/SmallString.h"
#include <string>

// Helper: structural AST fingerprinting (for identical-branch detection)
static std::string Fingerprint(const ExprAST *E) {
  if (!E)
    return "<null>";

  if (auto *N = dynamic_cast<const NumberExprAST *>(E)) {
    if (N->isInteger()) {
      // Exact accessor, not getIntVal() -- this fingerprint feeds
      // identical-BRANCH/duplicate-arm detection (SEM-001,
      // docs/planning/ARCHITECTURE_OVERHAUL.md §8's own Risks note calls
      // this file out by name), so two DIFFERENT wide literals that
      // happen to share the same low 64 bits must not collide into one
      // fingerprint.
      llvm::SmallString<32> Digits;
      N->getIntValExact().toString(Digits, 10);
      return "int:" + Digits.str().str();
    }
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

  if (auto *DW = dynamic_cast<const DoWhileExprAST *>(E)) {
    if (ContainsLoopExit(DW->getBody()))
      return true;
  }

  if (auto *F = dynamic_cast<const ForExprAST *>(E)) {
    if (ContainsLoopExit(F->getBody()))
      return true;
  }

  if (auto *FE = dynamic_cast<const ForEachExprAST *>(E)) {
    if (ContainsLoopExit(FE->getBody()))
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

/**
 * @brief Conservatively determine whether @p E might assign a new value to
 *        the variable named @p VarName, anywhere in its subtree.
 *
 * Used by LintFor()'s "loop variable never advances" check (see
 * StuckForLoopVariableWarning's doc comment in Errors.h). This function MUST
 * be conservative in the "return true" direction: a false negative (failing
 * to notice a real modification, so the warning wrongly fires) is a
 * correctness bug -- a false positive suppression (returning true when
 * nothing actually modifies the variable) just silently skips the warning,
 * which is safe. Concretely: taking the variable's address anywhere
 * (`&i`) unconditionally returns true, since a function receiving that
 * pointer could mutate it in a way this purely-structural check can't trace.
 */
static bool MayModifyVariable(const ExprAST *E, const std::string &VarName) {
  if (!E)
    return false;

  auto IsTargetVar = [&](const ExprAST *Candidate) {
    auto *V = dynamic_cast<const VariableExprAST *>(Candidate);
    return V && V->getName() == VarName;
  };

  if (auto *Assign = dynamic_cast<const AssignmentExprAST *>(E)) {
    if (IsTargetVar(Assign->getLHS()))
      return true;
    return MayModifyVariable(Assign->getLHS(), VarName) ||
           MayModifyVariable(Assign->getRHS(), VarName);
  }
  if (auto *CAssign = dynamic_cast<const CompoundAssignmentExprAST *>(E)) {
    if (IsTargetVar(CAssign->getLHS()))
      return true;
    return MayModifyVariable(CAssign->getLHS(), VarName) ||
           MayModifyVariable(CAssign->getRHS(), VarName);
  }
  if (auto *Post = dynamic_cast<const PostfixExprAST *>(E)) {
    if (IsTargetVar(Post->getOperand()))
      return true;
    return MayModifyVariable(Post->getOperand(), VarName);
  }
  if (auto *Addr = dynamic_cast<const AddressOfExprAST *>(E)) {
    // See doc comment: unconditionally conservative once the address of ANY
    // variable is taken inside the loop, not just VarName -- a pointer to
    // some OTHER variable could, in principle, alias VarName's storage in
    // ways this structural check has no way to rule out (e.g. passed to an
    // extern/FFI function). Narrowing this to only IsTargetVar(Operand)
    // would be more precise but riskier; erring conservative here is cheap
    // since it only costs a missed warning, never a wrong one.
    (void)Addr;
    return true;
  }
  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions())
      if (MayModifyVariable(Child.get(), VarName))
        return true;
    return false;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    for (const auto &B : If->getBranches()) {
      if (MayModifyVariable(B.Cond.get(), VarName) ||
          MayModifyVariable(B.Body.get(), VarName))
        return true;
    }
    return MayModifyVariable(If->getElseBody(), VarName);
  }
  if (auto *W = dynamic_cast<const WhileExprAST *>(E)) {
    return MayModifyVariable(W->getCond(), VarName) ||
           MayModifyVariable(W->getBody(), VarName);
  }
  if (auto *F = dynamic_cast<const ForExprAST *>(E)) {
    // A nested for-loop reusing VarName as ITS OWN induction variable would
    // shadow the outer one -- assignments inside it wouldn't actually touch
    // the outer VarName. Not worth special-casing: treating it as "might
    // modify" here is the conservative (safe) direction anyway.
    return MayModifyVariable(F->getStart(), VarName) ||
           MayModifyVariable(F->getEnd(), VarName) ||
           MayModifyVariable(F->getStep(), VarName) ||
           MayModifyVariable(F->getBody(), VarName);
  }
  if (auto *FE = dynamic_cast<const ForEachExprAST *>(E)) {
    // Same shadowing note as ForExprAST just above -- a nested for-each
    // reusing VarName as its own value/index var would shadow the outer
    // one, but treating it as "might modify" is the conservative-safe
    // direction anyway.
    return MayModifyVariable(FE->getIterable(), VarName) ||
           MayModifyVariable(FE->getBody(), VarName);
  }
  if (auto *Ret = dynamic_cast<const ReturnExprAST *>(E))
    return MayModifyVariable(Ret->getVal(), VarName);
  if (auto *Call = dynamic_cast<const FuncCallExprAST *>(E)) {
    for (const auto &Arg : Call->getArgs())
      if (MayModifyVariable(Arg.get(), VarName))
        return true;
    return false;
  }
  if (auto *MCall = dynamic_cast<const MethodCallExprAST *>(E)) {
    if (MayModifyVariable(MCall->getBase(), VarName))
      return true;
    for (const auto &Arg : MCall->getArgs())
      if (MayModifyVariable(Arg.get(), VarName))
        return true;
    return false;
  }
  if (auto *Bin = dynamic_cast<const BinaryExprAST *>(E))
    return MayModifyVariable(Bin->getLHS(), VarName) ||
           MayModifyVariable(Bin->getRHS(), VarName);
  if (auto *Un = dynamic_cast<const UnaryExprAST *>(E))
    return MayModifyVariable(Un->getOperand(), VarName);

  return false;
}

// Forward declarations
static void LintBinaryExpr(const BinaryExprAST *Bin);
static void LintAssignment(const AssignmentExprAST *Assign);
static void LintWhile(const WhileExprAST *While);
static void LintDoWhile(const DoWhileExprAST *DoWhile);
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

  if (auto *DoWhile = dynamic_cast<DoWhileExprAST *>(E)) {
    LintDoWhile(DoWhile);
  }

  if (auto *For = dynamic_cast<ForExprAST *>(E)) {
    LintFor(For);
  }

  if (auto *ForEach = dynamic_cast<ForEachExprAST *>(E)) {
    // No LintForEach() helper needed: there's no "loop variable never
    // advances" analog for for-each (the value/index vars are never
    // manually advanced by design -- that check is ForExprAST-specific by
    // construction), so a bare recursion into children is sufficient.
    LintExpression(const_cast<ExprAST *>(ForEach->getIterable()));
    LintExpression(ForEach->getBody());
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

  if (auto *CAssign = dynamic_cast<CompoundAssignmentExprAST *>(E)) {
    LintExpression(const_cast<ExprAST*>(CAssign->getLHS()));
    LintExpression(const_cast<ExprAST*>(CAssign->getRHS()));
  }

  if (auto *Post = dynamic_cast<PostfixExprAST *>(E)) {
    LintExpression(const_cast<ExprAST*>(Post->getOperand()));
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
  // Skip nodes ParseUnary() synthesized to desugar a literal `!x` (see
  // BinaryExprAST::IsDesugaredNot's doc comment) -- those aren't a
  // comparison the user wrote at all, so warning "remove the comparison"
  // would be nagging about syntax that doesn't exist in their source.
  if ((Op == TOK_EQ || Op == TOK_NEQ) && !Bin->IsDesugaredNot) {
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
    if (LFP == RFP && LFP != "<null>") {
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

        // '==' and '!=' are NOT unconditionally reflexive: if the value is
        // a double holding NaN, `x == x` is false and `x != x` is true --
        // the opposite of the claim below -- and `x != x` is in fact the
        // standard idiom for isnan(x). Lint.cpp has no type information
        // (it runs structurally, before the TypeChecker's results are
        // available here), so it can't tell whether `x` might be a double.
        // Rather than silence the check for variables entirely (which
        // would lose its main value: catching copy-paste mistakes like
        // `a.val == a.val`), hedge the wording so it's never technically
        // wrong. '<', '>', '<=', '>=' don't need this: every IEEE-754
        // ordered comparison against NaN is false, so "always true/false"
        // is correct for those regardless of type.
        if (Op == TOK_EQ || Op == TOK_NEQ)
          result += " (unless the value can be NaN)";

        // Extract a readable name from the fingerprint
        std::string readableName = LFP;
        if (LFP.substr(0, 4) == "var:")
          readableName = LFP.substr(4);
        else if (LFP.substr(0, 4) == "int:")
          readableName = LFP.substr(4);
        else if (LFP.substr(0, 4) == "dbl:")
          readableName = LFP.substr(4);
        else if (LFP.substr(0, 4) == "str:")
          readableName = LFP.substr(4);
        else if (LFP.substr(0, 5) == "bool:")
          readableName = LFP.substr(5);

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

static void LintDoWhile(const DoWhileExprAST *DoWhile) {
  if (!DoWhile)
    return;

  const ExprAST *Cond = DoWhile->getCond();

  // Check for do { } while(true) — condition is a BoolExprAST with value true
  if (auto *BoolCond = dynamic_cast<const BoolExprAST *>(Cond)) {
    if (BoolCond->getVal()) {
      // Condition is literally 'true'
      if (!ContainsLoopExit(DoWhile->getBody())) {
        SuspiciousInfiniteLoopWarning(DoWhile->getLoc()).warn();
      }
    }
  }

  LintExpression(const_cast<ExprAST *>(DoWhile->getCond()));
  // Recurse into body
  LintExpression(DoWhile->getBody());
}

static void LintFor(const ForExprAST *For) {
  if (!For)
    return;

  // Loop variable never advances: neither 'step' nor the body ever assigns
  // to it. See StuckForLoopVariableWarning's doc comment in Errors.h for why
  // this is deterministically knowable (not a general halting-problem
  // question) and MayModifyVariable()'s doc comment above for the
  // conservative-false-negative-only guarantee that keeps it false-positive
  // free.
  const std::string &VarName = For->getVarName();
  bool Advances = MayModifyVariable(For->getStep(), VarName) ||
                  MayModifyVariable(For->getBody(), VarName);

  if (!Advances) {
    // Try to say whether this specifically runs forever or never runs, by
    // comparing literal start/end bounds -- purely a wording nicety; when
    // start/end aren't both literals (the common case), fall back to the
    // honest "one or the other, can't tell which" phrasing.
    std::string Outcome =
        "this loop's fate was already decided before it started -- it "
        "either runs forever or never runs at all, depending on how '" +
        VarName + "' compares to the end value.";

    auto *StartLit = dynamic_cast<const NumberExprAST *>(For->getStart());
    auto *EndLit = dynamic_cast<const NumberExprAST *>(For->getEnd());
    if (StartLit && EndLit) {
      double StartV = StartLit->isInteger()
                          ? static_cast<double>(StartLit->getIntVal())
                          : StartLit->getDoubleVal();
      double EndV = EndLit->isInteger()
                        ? static_cast<double>(EndLit->getIntVal())
                        : EndLit->getDoubleVal();
      if (StartV < EndV)
        Outcome = "it will loop forever: '" + VarName +
                  "' starts below the end value and never changes.";
      else if (StartV > EndV)
        Outcome = "it will loop forever: '" + VarName +
                  "' starts above the end value and never changes "
                  "(descending range).";
      else
        Outcome = "the body will never execute even once: '" + VarName +
                  "' already equals the end value at the start.";
    }

    StuckForLoopVariableWarning(For->getLoc(), VarName, Outcome).warn();
  }

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

  // Recurse into for-each loop children
  if (auto *ForEach = dynamic_cast<ForEachExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(ForEach->getIterable()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(ForEach->getBody(), ChangedNodeIDs);
    return;
  }

  // Recurse into compound assignment children
  if (auto *CAssign = dynamic_cast<CompoundAssignmentExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(CAssign->getLHS()),
                                 ChangedNodeIDs);
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(CAssign->getRHS()),
                                 ChangedNodeIDs);
    return;
  }

  // Recurse into postfix expression operand
  if (auto *Post = dynamic_cast<PostfixExprAST *>(E)) {
    LintChangedNodesOnlyInternal(const_cast<ExprAST *>(Post->getOperand()),
                                 ChangedNodeIDs);
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
