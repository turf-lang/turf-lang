#ifndef ERRORS_FLOW_H
#define ERRORS_FLOW_H

#include "Errors.h"

// Control Flow Misuse : break/continue/return in wrong context
class ControlFlowError : public TurfError {
public:
  ControlFlowError(SourceLocation Loc, const std::string &Msg)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Control flow error!\n" + Colors::RESET +
                "  " + Msg,
            "Make sure 'break' and 'continue' are inside a loop, "
            "and 'return' is inside a function.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Semantic Error : General semantic violations
class SemanticError : public TurfError {
public:
  SemanticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! Something doesn't add up:\n" + Colors::RESET +
                           "  " + Msg) {
    // Intentionally left as the default GENERAL: this class is a genuine
    // catch-all for miscellaneous semantic violations that don't fit any
    // other bucket, not an oversight.
  }
};
// (UnreachableCodeError, E013, was deleted in issue #263: the same mistake as
// StatementAfterTerminatorError / E012, raised only from a Codegen backstop
// that E012 always pre-empted. The backstop now raises E012.)
// Control Flow Errors
class MissingReturnError : public TurfError {
public:
  MissingReturnError(SourceLocation Loc, const std::string &FunctionName)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "The function '" + Colors::CYAN +
                FunctionName + Colors::BRIGHT_RED +
                "' promises to give back a result, but sometimes it doesn't!" +
                Colors::RESET,
            "Make sure every path in your function (like inside 'if' "
            "and 'else') has a 'return' statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
class UnreachableBlockWarning : public TurfWarning {
public:
  UnreachableBlockWarning(SourceLocation Loc, const std::string &FunctionName,
                          const std::string &BlockName)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "The code in '" + BlockName +
                        "' inside '" + FunctionName +
                        "' can never be reached!" + Colors::RESET,
                    "Check your 'if' conditions. Are they impossible to be "
                    "true, or did you 'return' before this block?") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
class DeadBranchWarning : public TurfWarning {
public:
  DeadBranchWarning(SourceLocation Loc, const std::string &FunctionName)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "There's a part of '" +
                        FunctionName + "' that will never be executed!" +
                        Colors::RESET,
                    "Check if you have code after a 'return', 'break', or "
                    "'continue' that can't be reached.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
class StatementAfterTerminatorError : public TurfError {
public:
  StatementAfterTerminatorError(SourceLocation Loc,
                                const std::string &Terminator)
      : TurfError(Loc, Colors::BRIGHT_RED + "This code comes after a " +
                           Terminator + ", so it's impossible to reach it!" +
                           Colors::RESET,
                  "Delete this code or move it before the '" +
                  Terminator + "' statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// LANG-003 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): "a deferred
// statement may not return, break, or continue out of itself" -- there is
// no way for such a jump to target only the defer statement and stop, it
// always escapes to the enclosing function/loop, which a reader would never
// expect from something spelled as ordinary cleanup code. Raised by
// Resolver::visitDefer() (ResolverStmt.cpp) via FindEscapingControlFlow().
class DeferControlFlowError : public TurfError {
public:
  DeferControlFlowError(SourceLocation Loc, const std::string &Kind)
      : TurfError(Loc, Colors::BRIGHT_RED + "A deferred statement can't '" +
                           Kind + "'!" + Colors::RESET,
                  "'" + Kind + "' inside a 'defer' would jump out of the "
                  "enclosing function or loop, not just the deferred "
                  "statement -- move it out of the 'defer'.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(a)): `caller_location` reads a
// hidden value that only exists because the ENCLOSING function was declared
// `@caller_location` (Codegen appends it as a real extra LLVM parameter --
// see CallerLocationExprAST's own doc comment, ASTExpr.h). Used anywhere
// else, there is nothing to read. Raised by
// TypeChecker::visitCallerLocation() (TypeCheckerDecl.cpp).
class CallerLocationOutsideMarkedFunctionError : public TurfError {
public:
  explicit CallerLocationOutsideMarkedFunctionError(SourceLocation Loc)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "'" + Colors::CYAN + "caller_location" +
                      Colors::BRIGHT_RED +
                      "' can only be used inside a function declared '" +
                      Colors::CYAN + "@caller_location" + Colors::BRIGHT_RED +
                      "'." + Colors::RESET,
                  "Add '@caller_location' immediately before this "
                  "function's 'fn' keyword, or remove this use.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
// Lint Warnings - Reviewer-style heuristic pattern warnings
// Redundant comparison: x == true, x != false, etc.
class RedundantComparisonWarning : public TurfWarning {
public:
  RedundantComparisonWarning(SourceLocation Loc, const std::string &Expr,
                             const std::string &Simplified)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "This comparison is redundant!" + Colors::RESET + "\n  " +
                        Colors::BRIGHT_CYAN + Expr + Colors::RESET +
                        " is the same as just writing " + Colors::BRIGHT_CYAN +
                        Simplified + Colors::RESET + ".",
                    "You can simplify this by removing the comparison "
                    "entirely.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Self-assignment: x = x
class SelfAssignmentWarning : public TurfWarning {
public:
  SelfAssignmentWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "You're assigning '" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "' to itself — this doesn't do anything!" +
                        Colors::RESET,
                    "Did you mean to assign a different value, or is this "
                    "left over from an edit?") {
    Category_ = SLM::ErrorCategory::GENERAL;
  }
};
// Infinite loop: while(true) with no break/continue/return in the body
class SuspiciousInfiniteLoopWarning : public TurfWarning {
public:
  SuspiciousInfiniteLoopWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "This looks like an infinite loop!" + Colors::RESET +
                        " The condition is always true and the body has no " +
                        Colors::CYAN + "'break'" + Colors::BRIGHT_YELLOW + ", " +
                        Colors::CYAN + "'continue'" + Colors::BRIGHT_YELLOW + ", or " +
                        Colors::CYAN + "'return'" + Colors::BRIGHT_YELLOW + ".",
                    "If this is intentional, consider adding a comment. "
                    "Otherwise, add a 'break' or a proper exit condition.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// CFG Phase 1 (Issue #185): Cyclomatic complexity exceeds threshold (M > 15).
// M = E - N + 2P, computed over the function's CFG. High complexity correlates
// with functions that are difficult to test, review, and maintain.
class CyclomaticComplexityWarning : public TurfWarning {
public:
  CyclomaticComplexityWarning(SourceLocation Loc, const std::string &FuncName,
                              int M)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + "Function '" + Colors::CYAN + FuncName +
                Colors::BRIGHT_YELLOW + "' has cyclomatic complexity " +
                Colors::CYAN + std::to_string(M) + Colors::BRIGHT_YELLOW +
                " (threshold: 15). Consider breaking it into smaller functions." +
                Colors::RESET,
            "High cyclomatic complexity makes code harder to test and maintain. "
            "Try splitting this function into smaller, focused helpers.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// CFG Phase 1 (Issue #185): A loop whose CFG cycle has no reachable exit.
// Triggered when DFS finds a back-edge where no block in the cycle has a
// Break/Return terminator AND no cycle block has a successor outside the cycle.
// The second condition is what separates while(true) from while(cond): a
// while(cond) loop's condition block always has an edge to the loop-exit block
// (outside the cycle), so it never triggers this warning.
class InfiniteLoopWarning : public TurfWarning {
public:
  InfiniteLoopWarning(SourceLocation Loc, const std::string &FuncName)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + "Function '" + Colors::CYAN + FuncName +
                Colors::BRIGHT_YELLOW +
                "' contains a loop with no exit path. "
                "This will run forever." + Colors::RESET,
            "Add a 'break', 'return', or a condition that can become false "
            "to allow the loop to exit.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// CFG Phase 2 (Issue #186): A variable is written to but the value is never
// subsequently read before the next write or the function exits. The assigned
// value is immediately discarded. Non-fatal — compilation continues.
class DeadStoreWarning : public TurfWarning {
public:
  DeadStoreWarning(SourceLocation Loc, const std::string &VarName)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + "Variable '" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "' is assigned but the value is never read (dead store)." +
                Colors::RESET,
            "The assigned value is overwritten or the function returns before "
            "this variable is ever used. Remove the assignment or use the "
            "value.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// CFG Phase 2 (Issue #186): A local variable is declared but its value is
// never read on any path through the function. Non-fatal.
class UnusedVariableWarning : public TurfWarning {
public:
  UnusedVariableWarning(SourceLocation Loc, const std::string &VarName)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + "Variable '" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "' is declared but never used." + Colors::RESET,
            "If the variable is intentionally unused, prefix its name with "
            "'_' by convention. Otherwise, remove the declaration or use "
            "its value.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// CFG Phase 2 (Issue #186): Every path from the function's entry block to
// its exit block passes through at least one self-call, meaning no base case
// exists. This guarantees a stack overflow at runtime. Raised as a fatal
// error (uses raise()) because the program is guaranteed to crash — there is
// no safe way to proceed.
class RecursionWithoutBaseError : public TurfError {
public:
  RecursionWithoutBaseError(SourceLocation Loc, const std::string &FuncName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Infinite recursion detected in '" +
                      Colors::CYAN + FuncName + Colors::BRIGHT_RED + "'." +
                      Colors::RESET,
                  "Every recursive function needs a base case — "
                  "a path that returns without calling itself.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
/**
 * @brief A `for` loop's induction variable is never modified by its `step`
 *        clause or its body.
 *
 * `for i in start..end step <expr> { body }` desugars to
 * `init; while (ascending ? i < end : i > end) { body; step }` (see
 * ForExprAST::codegen()'s doc comment in Codegen.cpp) -- nothing re-checks
 * `start`/`end` each iteration, and nothing forces `step` to actually touch
 * `i`. If neither `step` nor `body` ever assigns to `i` (directly, via
 * `+=`/`-=`/etc., or via `++`/`--`), `i`'s value is frozen at `start` for
 * the entire loop, so the loop's fate was already decided before it began:
 * either the `start`-vs-`end` comparison was true and it runs forever, or it
 * was false and the body never executes even once. Concretely reproduced
 * via `for i in 0..10 step j += 10 { }` (step touches an unrelated variable
 * `j`, never `i`) -- `i` stays 0 forever, `0 < 10` stays true forever, and
 * the loop never terminates at runtime.
 *
 * Deliberately conservative to avoid false positives: suppressed if `i`'s
 * address is ever taken anywhere in `step`/`body` (some function could still
 * be mutating it through a pointer, which this check can't trace) -- see
 * MayModifyVariable()'s doc comment in Lint.cpp.
 */
class StuckForLoopVariableWarning : public TurfWarning {
public:
  StuckForLoopVariableWarning(SourceLocation Loc, const std::string &VarName,
                              const std::string &Outcome)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() +
                "The loop variable '" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "' is never changed by this loop's 'step' or its body, so " +
                Outcome + Colors::RESET,
            "Make sure 'step' (or something in the body) actually updates '" +
                VarName +
                "'. If a DIFFERENT variable is meant to control when the "
                "loop ends, use 'while' instead of 'for' -- 'for' always "
                "iterates its own induction variable, it doesn't watch "
                "other variables for you.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Empty block in if/else branch
class EmptyBranchWarning : public TurfWarning {
public:
  EmptyBranchWarning(SourceLocation Loc, const std::string &BranchKind)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "The '" + Colors::CYAN + BranchKind +
                        Colors::BRIGHT_YELLOW + "' branch is empty — it doesn't do anything!" +
                        Colors::RESET,
                    "If you haven't written the code yet, add a comment "
                    "like '// TODO'. Otherwise, you may be able to simplify this "
                    "'if' statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Self-comparison: x == x, x != x, x < x, etc.
class SelfComparisonWarning : public TurfWarning {
public:
  SelfComparisonWarning(SourceLocation Loc, const std::string &Name,
                        const std::string &Result)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "You're comparing '" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "' with itself!" + Colors::RESET +
                        " This is always " + Colors::BRIGHT_CYAN + Result +
                        Colors::RESET + ".",
                    "Did you mean to compare with a different variable?") {
    Category_ = SLM::ErrorCategory::GENERAL;
  }
};
// Division / modulo by zero literal
class DivisionByZeroLiteralWarning : public TurfWarning {
public:
  DivisionByZeroLiteralWarning(SourceLocation Loc, const std::string &Op)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "You're using '" + Colors::CYAN + Op + Colors::BRIGHT_YELLOW +
                        "' with zero — this will crash at runtime!" + Colors::RESET,
                    "Check the right-hand side. Division or modulo by zero "
                    "is undefined behavior.") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
};
// Identical if/else branches
class IdenticalBranchesWarning : public TurfWarning {
public:
  IdenticalBranchesWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "Both branches of this 'if' do the same thing!" +
                        Colors::RESET,
                    "The 'if'/'else' can be removed — just keep the body. "
                    "Or did you forget to change one of the branches?") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Use-before-assignment error
class DefiniteAssignmentError : public TurfError {
  static std::string buildHint(const std::string &Name, const std::string &TypeName,
                                bool SuggestDefault) {
    std::string Hint = "Make sure every path assigns to '" + Name + "' before you use it.";
    if (SuggestDefault) {
      // SEM-004 (§8, FREEZE.md §6.4): the mechanical migration off the
      // removed eager-default mechanism is always the same shape, so name
      // it directly rather than making every caller rediscover it.
      Hint += " Or give it an explicit default right away: 'mut " + Name +
              ": " + TypeName + " = " + TypeName + "::default();'.";
    }
    return Hint;
  }

public:
  // TypeName/SuggestDefault are optional so every pre-existing call site
  // (this error long predates SEM-004) keeps compiling unchanged; SEM-004's
  // own call site (Analysis.cpp) is the only one that passes them.
  DefiniteAssignmentError(SourceLocation Loc, const std::string &Name,
                           const std::string &TypeName = "",
                           bool SuggestDefault = false)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You're using '" + Colors::CYAN + Name +
                      Colors::BRIGHT_RED + "'" +
                      (TypeName.empty() ? "" : " (type '" + TypeName + "')") +
                      " before it has a value." + Colors::RESET,
                  buildHint(Name, TypeName, SuggestDefault)) {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
  }
};
// ─── `error`/`!T`/`recover` diagnostics ──────────────────────────────────────
// (docs/architecture/errors_and_recover.md)
// Named `...Error : public TurfError` / `...Warning : public TurfWarning`
// like every other diagnostic in this file -- this is the COMPILER's own
// diagnostic machinery reporting on the user-facing `error` keyword feature,
// not to be confused with that feature's own types (TurfUserErrorType/
// TurfErrorUnionType in Types.h, deliberately named to stay clear of
// `TurfError` itself).

// A `!T`-declared function whose body never actually returns a value of any
// declared error type -- a warning (not a hard error, per
// errors_and_recover.md), since `!T` doesn't OBLIGE a function to fail, only
// permits it to.
class NeverReturnsErrorWarning : public TurfWarning {
public:
  NeverReturnsErrorWarning(SourceLocation Loc, const std::string &FuncName)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "'" + FuncName +
                        "' is declared to return a fallible type ('!T') but "
                        "never actually returns an error." + Colors::RESET,
                    "If this function can never really fail, its return "
                    "type doesn't need the '!' -- drop it and return T "
                    "directly.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

#endif
