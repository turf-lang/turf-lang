#ifndef ERRORS_PATTERN_H
#define ERRORS_PATTERN_H

#include "diagnostics/Errors.h"

// LEC-003 (docs/development/changes/LEC-003.md, FREEZE.md §6.17 / F-PAT-1):
// the diagnostics of destructuring patterns and guards. Three classes, each
// typed and coded (FREEZE.md §6.9); the wording of one rule differs by WHAT
// went wrong through the message and hint the raising site supplies, not by an
// error class per kind. No new SLM::ErrorCategory (§6.9).

/**
 * @brief A `matches` header that is not a legal pattern, or a legal pattern
 *        used where it cannot work.
 *
 * One class for every structural rule of the pattern language: a struct
 * pattern over a non-struct subject, a field the struct does not have, a
 * field left unmentioned without `..`, the same name bound twice, two stacked
 * headers binding different names, a sub-pattern that is neither a literal,
 * an enum variant, `_`, a binding nor a nested struct pattern. Every raising
 * site passes a hint that names the concrete fix -- a silent "invalid pattern"
 * is how a typo becomes a puzzle.
 */
class InvalidPatternError : public TurfError {
public:
  InvalidPatternError(SourceLocation Loc, const std::string &Problem,
                      const std::string &Hint)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Invalid pattern: " + Colors::RESET +
                      Problem,
                  Hint) {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

/**
 * @brief A `compare` with no `otherwise` whose arms do not cover every value
 *        of an enum, machine, `bool` or error subject.
 *
 * Before LEC-003 this was an untyped reportError() that knew only enums. The
 * rule now also covers `bool`, and an open error union (`AnyErrorType`), whose
 * variants cannot be listed at all, so it always needs `otherwise`; a guarded
 * arm never counts toward coverage (its guard may fail). `Missing` is what the
 * message lists; empty means "the set is open or unknowable".
 */
class NonExhaustiveCompareError : public TurfError {
public:
  /// @p Kind completes "Non-exhaustive <Kind> on '<Subject>'." -- "enum
  /// comparison" keeps the wording the enum-only check always used (the SLM
  /// docs quote it). @p MissingLabel heads the list ("Missing variants:").
  NonExhaustiveCompareError(SourceLocation Loc, const std::string &Kind,
                            const std::string &SubjectType,
                            const std::string &MissingLabel,
                            const std::vector<std::string> &Missing,
                            const std::string &Hint)
      : TurfError(Loc, "", Hint) {
    std::string Msg = Colors::BRIGHT_RED + "Non-exhaustive " + Kind + " on '" +
                      Colors::CYAN + SubjectType + Colors::BRIGHT_RED + "'." +
                      Colors::RESET;
    if (!Missing.empty()) {
      Msg += "\n" + MissingLabel;
      for (const auto &M : Missing)
        Msg += "\n    " + M;
    }
    Message = Msg;
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

/**
 * @brief An arm (or the `otherwise` body) that can never run, because an
 *        earlier unguarded header already matches everything.
 *
 * A warning, not an error: the dead arm is harmless, often left behind while
 * editing, and rejecting it would only force a deletion the writer may be
 * about to undo. `WhyLoc` points at the header that shadows it.
 */
class UnreachableArmWarning : public TurfWarning {
public:
  UnreachableArmWarning(SourceLocation Loc, const std::string &What,
                        SourceLocation WhyLoc)
      : TurfWarning(
            Loc,
            TurfWarning::WarningLabel() + What + " can never run." +
                Colors::RESET,
            "An earlier arm without a guard already matches every value, so "
            "arms are never tried past it. Delete this arm, or move it above "
            "the catch-all (arms are tried in source order).") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
    withRelated(WhyLoc, "this header matches everything");
  }
};

#endif
