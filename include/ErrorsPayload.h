#ifndef ERRORS_PAYLOAD_H
#define ERRORS_PAYLOAD_H

#include "Errors.h"

// LEC-004 (docs/development/changes/LEC-004.md, FREEZE.md §6.18 / F-ERR-1):
// the diagnostics of error-variant payloads and of `try`. Two classes, each
// typed and coded (FREEZE.md §6.9); the wording of one rule differs by WHAT
// went wrong through the message and hint the raising site supplies, not by an
// error class per kind. No new SLM::ErrorCategory (§6.9).
//
// A destructuring pattern over an error value reuses E092 (InvalidPatternError)
// and E093 (NonExhaustiveCompareError) rather than growing a third class here.

/**
 * @brief An error variant's payload declared or constructed wrongly.
 *
 * One class for every payload rule: a field type an error cannot hold (another
 * error type, a `!T`, `void`), a field named twice, empty parentheses, a
 * construction with the wrong number of arguments or an argument of the wrong
 * type, and a variant that has fields used as a bare value (there is no
 * payload to fill in). Every raising site passes a hint naming the concrete
 * fix: a silent "invalid payload" is how a typo becomes a puzzle.
 */
class ErrorPayloadError : public TurfError {
public:
  ErrorPayloadError(SourceLocation Loc, const std::string &Problem,
                    const std::string &Hint)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Invalid error payload: " +
                      Colors::RESET + Problem,
                  Hint) {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

/**
 * @brief `try` used where it cannot work.
 *
 * `try e` returns `e`'s error from the enclosing function, so it needs a
 * function that returns `!V`, a place a `return` is allowed (not a `defer`),
 * and an operand that can fail. The hint says how to handle the error
 * instead -- for `main`, which can never be fallible, that is the point of
 * the message.
 */
class TryMisuseError : public TurfError {
public:
  TryMisuseError(SourceLocation Loc, const std::string &Problem,
                 const std::string &Hint)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Misplaced 'try': " + Colors::RESET +
                      Problem,
                  Hint) {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

#endif
