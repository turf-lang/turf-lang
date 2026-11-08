#ifndef ERRORS_FLOW_H
#define ERRORS_FLOW_H

#include "diagnostics/Errors.h"

// Control Flow Misuse : break/continue/return in wrong context
class ControlFlowError : public TurfError {
public:
  ControlFlowError(SourceLocation Loc, const std::string &Msg)
      : TurfError(
            Loc,
            Msg,
            "Move `break` and `continue` inside a loop, and `return` inside "
            "a function.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Semantic Error : General semantic violations
class SemanticError : public TurfError {
public:
  SemanticError(SourceLocation Loc, const std::string &Msg,
                const std::string &Hint = "")
      : TurfError(Loc, Msg, Hint) {
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
            Colors::BRIGHT_RED + "I found a path through `" + Colors::CYAN +
                FunctionName + Colors::BRIGHT_RED +
                "` that ends without returning a result." +
                Colors::RESET,
            "Add a `return` statement to every path in `" + FunctionName +
                "`, including each `if` and `else` branch.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
class UnreachableBlockWarning : public TurfWarning {
public:
  UnreachableBlockWarning(SourceLocation Loc, const std::string &FunctionName,
                          const std::string &BlockName)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "I can never reach the code in `" +
                        BlockName + "` inside `" + FunctionName + "`." +
                        Colors::RESET,
                    "Check whether an `if` condition can never be true, or "
                    "whether a `return` comes before this block.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
class DeadBranchWarning : public TurfWarning {
public:
  DeadBranchWarning(SourceLocation Loc, const std::string &FunctionName)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "A part of `" +
                        FunctionName + "` can never run." +
                        Colors::RESET,
                    "Remove the code after a `return`, `break` or `continue`, "
                    "or move it before that statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
class StatementAfterTerminatorError : public TurfError {
public:
  StatementAfterTerminatorError(SourceLocation Loc,
                                const std::string &Terminator)
      : TurfError(Loc, Colors::BRIGHT_RED + "This code comes after a " +
                           Terminator + ", so I can never reach it." +
                           Colors::RESET,
                  "Delete this code or move it before the `" +
                  Terminator + "` statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// LANG-003 (docs/archive/ARCHITECTURE_OVERHAUL.md §8): "a deferred
// statement may not return, break, or continue out of itself" -- there is
// no way for such a jump to target only the defer statement and stop, it
// always escapes to the enclosing function/loop, which a reader would never
// expect from something spelled as ordinary cleanup code. Raised by
// Resolver::visitDefer() (ResolverStmt.cpp) via FindEscapingControlFlow().
class DeferControlFlowError : public TurfError {
public:
  DeferControlFlowError(SourceLocation Loc, const std::string &Kind)
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot let a deferred statement `" +
                           Kind + "`." + Colors::RESET,
                  "Move the `" + Kind + "` out of the `defer`: it would jump "
                  "out of the enclosing function or loop, not just the "
                  "deferred statement.") {
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
                  Colors::BRIGHT_RED + "I cannot use `" + Colors::CYAN + "caller_location" +
                      Colors::BRIGHT_RED +
                      "` outside a function declared `" +
                      Colors::CYAN + "@caller_location" + Colors::BRIGHT_RED +
                      "`." + Colors::RESET,
                  "Add `@caller_location` immediately before this "
                  "function's `fn` keyword, or remove this use.") {
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
                    TurfWarning::WarningLabel() + "`" + Colors::BRIGHT_CYAN + Expr +
                        Colors::BRIGHT_YELLOW + "` is the same as `" +
                        Colors::BRIGHT_CYAN + Simplified +
                        Colors::BRIGHT_YELLOW + "`." + Colors::RESET,
                    "Write `" + Simplified + "` instead of the comparison.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Self-assignment: x = x
class SelfAssignmentWarning : public TurfWarning {
public:
  SelfAssignmentWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "`" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "` is assigned to itself, so this does nothing." +
                        Colors::RESET,
                    "Remove this assignment, or assign the value you meant; it "
                    "may be left over from an edit.") {
    Category_ = SLM::ErrorCategory::GENERAL;
  }
};
// Infinite loop: while(true) with no break/continue/return in the body
class SuspiciousInfiniteLoopWarning : public TurfWarning {
public:
  SuspiciousInfiniteLoopWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "This loop looks infinite: its condition is always true and its body has no " +
                        Colors::CYAN + "`break`" + Colors::BRIGHT_YELLOW + ", " +
                        Colors::CYAN + "`continue`" + Colors::BRIGHT_YELLOW + " or " +
                        Colors::CYAN + "`return`" + Colors::BRIGHT_YELLOW + "." +
                        Colors::RESET,
                    "Add a `break` or a real exit condition. If the loop is "
                    "meant to run forever, pass `--allow E157` to silence this.") {
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
            TurfWarning::WarningLabel() + "Function `" + Colors::CYAN + FuncName +
                Colors::BRIGHT_YELLOW + "` has cyclomatic complexity " +
                Colors::CYAN + std::to_string(M) + Colors::BRIGHT_YELLOW +
                ", above the threshold of 15." +
                Colors::RESET,
            "Split `" + FuncName + "` into smaller, focused functions. To "
            "silence this on purpose, pass `--allow E158`.") {
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
            TurfWarning::WarningLabel() + "Function `" + Colors::CYAN + FuncName +
                Colors::BRIGHT_YELLOW +
                "` contains a loop with no exit path, so it will run forever." +
                Colors::RESET,
            "Add a `break`, a `return` or a condition that can become false, "
            "so the loop can exit.") {
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
            TurfWarning::WarningLabel() + "The value assigned to `" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "` is never read." +
                Colors::RESET,
            "Remove the assignment, or read `" + VarName + "` before it is "
            "overwritten or the function returns. To silence this on "
            "purpose, pass `--allow E160`.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// The dead store is a declaration's initializer: the variable is read, but
// only after a later assignment on every path (`mut x: int = 0;` then
// `x = i;` before each read in a loop).
class DeadInitializerWarning : public TurfWarning {
public:
  DeadInitializerWarning(SourceLocation Loc, const std::string &VarName,
                         const std::string &TypeName)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + "The initial value of `" +
                Colors::CYAN + VarName + Colors::BRIGHT_YELLOW +
                "` is never read, because every path assigns `" + VarName +
                "` again before reading it." + Colors::RESET,
            TypeName.empty()
                ? "Remove the initial value, or read it before the next "
                  "assignment."
                : "Declare it without a value, `mut " + VarName + ": " +
                      TypeName + ";` (every read still has to come after an "
                      "assignment), or read the initial value before the "
                      "next assignment.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// A `for` range whose constant bounds and step say the loop variable moves
// away from the end, so the loop runs until something in the body leaves it
// (`for i in 5..1 { }`: the default step counts up). A warning: the body may
// well `break` or `return`.
class RangeNeverEndsWarning : public TurfWarning {
public:
  RangeNeverEndsWarning(SourceLocation Loc, long long Start, long long End,
                        bool StepCountsUp, const std::string &Var)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + "This loop never reaches its end: " +
                std::to_string(Start) + ".." + std::to_string(End) + " counts " +
                (Start > End ? "down" : "up") + ", but the step counts " +
                (StepCountsUp ? "up" : "down") + "." + Colors::RESET,
            Start > End
                ? "Add a decreasing step: `for " + Var + " in " +
                      std::to_string(Start) + ".." + std::to_string(End) +
                      " step " + Var + "-- { ... }`."
                : "Use an increasing step, such as the default.") {
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
            TurfWarning::WarningLabel() + "Variable `" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "` is declared but never used." + Colors::RESET,
            "Remove `" + VarName + "`, or use it. If it is unused on purpose, "
            "rename it `_" + VarName + "`, or pass `--allow E163`.") {
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
                  Colors::BRIGHT_RED + "I found infinite recursion in `" +
                      Colors::CYAN + FuncName + Colors::BRIGHT_RED + "`." +
                      Colors::RESET,
                  "Add a base case, a path that returns without calling "
                  "itself.") {
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
                "The loop variable `" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "` is never changed by this loop's `step` or its body, so " +
                Outcome + Colors::RESET,
            "Make `step`, or something in the body, update `" +
                VarName +
                "`. If a different variable is meant to control when the "
                "loop ends, use `while` instead of `for`, which iterates "
                "only its own variable.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Empty block in if/else branch
class EmptyBranchWarning : public TurfWarning {
public:
  EmptyBranchWarning(SourceLocation Loc, const std::string &BranchKind)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "The `" + Colors::CYAN + BranchKind +
                        Colors::BRIGHT_YELLOW + "` branch is empty." +
                        Colors::RESET,
                    "Write the code for it, or add a comment such as `// TODO` "
                    "if it is not written yet. If the branch is not needed, "
                    "simplify the `if` statement, or pass `--allow E165` to "
                    "silence this.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Issue #362: E195-E197, the empty bodies E165 does not cover. A body that
// holds a comment is deliberate and does not warn (Lint.cpp, HasComment()).
class EmptyLoopBodyWarning : public TurfWarning {
public:
  EmptyLoopBodyWarning(SourceLocation Loc, const std::string &LoopKind)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "The body of this `" + Colors::CYAN +
                        LoopKind + Colors::BRIGHT_YELLOW + "` loop is empty." +
                        Colors::RESET,
                    "Write what the loop should do, or remove it. If it is meant "
                    "to do nothing, put a comment in the body saying why.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

class EmptyFunctionBodyWarning : public TurfWarning {
public:
  EmptyFunctionBodyWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "The body of `" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "` is empty." + Colors::RESET,
                    "Write the function, or remove it. If it is meant to do "
                    "nothing, put a comment in the body saying why.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

class EmptyRecoverBlockWarning : public TurfWarning {
public:
  explicit EmptyRecoverBlockWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() +
                        "This `recover` block is empty, so the error is dropped "
                        "without a trace." + Colors::RESET,
                    "Handle the error in the block: report it, return it, or "
                    "use a default. If ignoring it is right, put a comment in "
                    "the block saying why.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// Self-comparison: x == x, x != x, x < x, etc.
class SelfComparisonWarning : public TurfWarning {
public:
  SelfComparisonWarning(SourceLocation Loc, const std::string &Name,
                        const std::string &Result)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "`" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "` is compared with itself, so the result is always " +
                        Colors::BRIGHT_CYAN + Result +
                        Colors::BRIGHT_YELLOW + "." + Colors::RESET,
                    "Compare `" + Name + "` with a different variable.") {
    Category_ = SLM::ErrorCategory::GENERAL;
  }
};
// Division / modulo by zero literal
class DivisionByZeroLiteralWarning : public TurfWarning {
public:
  DivisionByZeroLiteralWarning(SourceLocation Loc, const std::string &Op)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "I found `" + Colors::CYAN + Op + Colors::BRIGHT_YELLOW +
                        "` used with zero, which will crash at runtime." + Colors::RESET,
                    "Change the right-hand side: division or modulo by zero "
                    "is undefined behavior.") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
};
// Identical if/else branches
class IdenticalBranchesWarning : public TurfWarning {
public:
  IdenticalBranchesWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "Both branches of this `if` do the same thing." +
                        Colors::RESET,
                    "Remove the `if` and `else` and keep one body, or change "
                    "one of the branches if they were meant to differ.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Use-before-assignment error
class DefiniteAssignmentError : public TurfError {
  static std::string buildHint(const std::string &Name, const std::string &TypeName,
                                bool SuggestDefault) {
    std::string Hint = "Assign to `" + Name + "` on every path before it is used.";
    if (SuggestDefault) {
      // SEM-004 (§8, FREEZE.md §6.4): the mechanical migration off the
      // removed eager-default mechanism is always the same shape, so name
      // it directly rather than making every caller rediscover it.
      Hint += " Or give it an explicit default right away: `mut " + Name +
              ": " + TypeName + " = " + TypeName + "::default();`.";
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
                  Colors::BRIGHT_RED + "I cannot use `" + Colors::CYAN + Name +
                      Colors::BRIGHT_RED + "`" +
                      (TypeName.empty() ? "" : " (type `" + TypeName + "`)") +
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
                    TurfWarning::WarningLabel() + "`" + FuncName +
                        "` is declared to return a fallible type (`!T`) but "
                        "never actually returns an error." + Colors::RESET,
                    "Drop the `!` from the return type and return `T` "
                    "directly if the function can never fail, or pass "
                    "`--allow E169` to silence this.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// AN-3 (a) (TDP-2-007, issue #332): functions that call each other, where
// every path through every one of them calls back into the cycle. Like E010
// for one function, but a warning (allow by default) through 2.x.
class MutualRecursionWarning : public TurfWarning {
public:
  MutualRecursionWarning(SourceLocation Loc, const std::string &Cycle)
      : TurfWarning(Loc,
                    "The functions " + Cycle + " call each other on every path, "
                    "so none of them can ever return.",
                    "Give one of them a path that returns without calling back "
                    "into the cycle: a base case, such as `if n == 0 { return 0; }`.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// AN-3 (b) (TDP-2-007, issue #333): a recursive function's base case that
// the value-range analysis proves is never reached, so every path that runs
// calls the function again. Like E010, which has no base case at all, but a
// warning (allow by default) through 2.x.
class UnreachableBaseCaseWarning : public TurfWarning {
public:
  UnreachableBaseCaseWarning(SourceLocation Loc, const std::string &Fn)
      : TurfWarning(Loc,
                    "This base case of `" + Fn + "` can never run: the values that "
                    "reach it rule it out, so every call of `" + Fn +
                    "` calls it again and never returns.",
                    "Check the condition in front of this base case against the "
                    "values that reach it, such as an earlier guard or the loop "
                    "that bounds them.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// AN-2 (TDP-2-007, issue #350): loops that never end. Warnings only, in
// every version: termination is undecidable.
class LoopTestNeverChangesWarning : public TurfWarning {
public:
  LoopTestNeverChangesWarning(SourceLocation Loc, const std::string &Vars)
      : TurfWarning(Loc,
                    "This loop's test reads " + Vars + ", which nothing in the "
                    "loop changes, and the test is its only way out: once the "
                    "loop starts, it never ends.",
                    "Change " + Vars + " inside the loop, or leave it with "
                    "`break` or `return`.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

class LoopExitsNeverTakenWarning : public TurfWarning {
public:
  explicit LoopExitsNeverTakenWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    "This loop never ends: every way out of it is guarded by a "
                    "test that the values reaching it make false.",
                    "Check the loop's test and each `break` condition against "
                    "the values the loop computes.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

class LoopExitsBehindNoReturnWarning : public TurfWarning {
public:
  explicit LoopExitsBehindNoReturnWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    "This loop never finishes: every way out of it comes after a "
                    "call that ends the program (`panic`, `abort` or `exit`).",
                    "If the loop is meant to stop, let it leave before that call; "
                    "if ending the program is the way out, write the loop as "
                    "`while true`.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// AN-5 (TDP-2-007, F-6, issue #352): manual-heap memory (an
// `@ownership(acquires)` call such as mem::alloc) that nothing frees and
// nothing lets out of the function. Garbage-collected memory is never
// reported.
class ManualHeapLeakWarning : public TurfWarning {
public:
  ManualHeapLeakWarning(SourceLocation Loc, const std::string &Var)
      : TurfWarning(Loc,
                    Var.empty()
                        ? std::string("This allocation is never freed: its result is "
                                      "thrown away, so nothing can free it.")
                        : "The memory allocated into `" + Var + "` is never freed, and "
                          "nothing passes it out of this function.",
                    Var.empty()
                        ? std::string("Keep the pointer and free it with `mem::free` "
                                      "when you are done.")
                        : "Free it with `mem::free(" + Var + ")` when you are done "
                          "(`defer mem::free(" + Var + ");` right after the allocation "
                          "frees it on every path), or use a garbage-collected value "
                          "instead.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};

#endif
