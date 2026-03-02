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

  // For complex nodes, return a unique tag that won't match anything else.
  // This prevents false positives at the cost of missing some patterns.
  return "<complex>";
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
    if (ContainsLoopExit(If->getThen()))
      return true;
    if (ContainsLoopExit(If->getElse()))
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

// Main entry point
void LintExpression(ExprAST *E) {
  if (!E)
    return;

  if (auto *Bin = dynamic_cast<BinaryExprAST *>(E))
    LintBinaryExpr(Bin);

  if (auto *Assign = dynamic_cast<AssignmentExprAST *>(E))
    LintAssignment(Assign);

  if (auto *While = dynamic_cast<WhileExprAST *>(E))
    LintWhile(While);

  if (auto *If = dynamic_cast<IfExprAST *>(E))
    LintIf(If);

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

  const std::string &Name = Assign->getName();
  const ExprAST *RHS = Assign->getRHS();

  if (auto *Var = dynamic_cast<const VariableExprAST *>(RHS)) {
    if (Var->getName() == Name) {
      SelfAssignmentWarning(Assign->getLoc(), Name).warn();
    }
  }
}

// Check: infinite loop (while(true) without break/continue/return)
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

  // Recurse into body
  LintExpression(const_cast<ExprAST *>(While->getBody()));
}

// Check: empty branches, identical branches
static void LintIf(const IfExprAST *If) {
  if (!If)
    return;

  ExprAST *Then = If->getThen();
  ExprAST *Else = If->getElse();

  // Empty then or else branch
  if (IsEffectivelyEmpty(Then)) {
    EmptyBranchWarning(If->getLoc(), "then").warn();
  }
  if (IsEffectivelyEmpty(Else)) {
    EmptyBranchWarning(If->getLoc(), "else").warn();
  }

  // Identical then/else branches
  if (Then && Else && !IsEffectivelyEmpty(Then) && !IsEffectivelyEmpty(Else)) {
    std::string ThenFP = Fingerprint(Then);
    std::string ElseFP = Fingerprint(Else);
    if (ThenFP == ElseFP && ThenFP != "<complex>") {
      IdenticalBranchesWarning(If->getLoc()).warn();
    }
  }

  // Recurse into branches
  LintExpression(Then);
  LintExpression(Else);
}
