#ifndef ERRORS_MEMORY_H
#define ERRORS_MEMORY_H

#include "Errors.h"

// Use-after-free error
class UseAfterFreeError : public TurfError {
public:
  UseAfterFreeError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You're using memory from '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "' after it was freed." + Colors::RESET,
                  "Don't use memory after calling free(). Consider setting the pointer to null after freeing.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};
// Double free error
class DoubleFreeError : public TurfError {
public:
  DoubleFreeError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Double free detected for '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "'." + Colors::RESET,
                  "Avoid calling free() on the same pointer more than once.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};
// `recover as err run { ... }` on a call whose only possible failure(s) are
// bare-form error types (`error Name;`, sugar for a single, informationless
// DEFAULT variant) -- errors_and_recover.md explicitly calls this out as a
// diagnostic error: a bare error carries nothing to discriminate on, so
// binding `err` to compare it against anything is pointless. Determined by scanning the
// callee's own `return` statements for which declared error types it can
// actually produce (Resolver::CollectReturnedErrorTypeNames).
class BareErrorAsBindingError : public TurfError {
public:
  BareErrorAsBindingError(SourceLocation Loc, const std::string &BoundName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "'recover as " + Colors::CYAN + BoundName +
                      Colors::BRIGHT_RED +
                      " run' can't be used here -- every error this call can "
                      "fail with is a bare 'error Name;' declaration." +
                      Colors::RESET,
                  "A bare error type carries no information to discriminate "
                  "on (it only ever has a single DEFAULT variant) -- use "
                  "'recover run { ... }' or 'recover produce value' instead, "
                  "since they don't need to inspect which error happened.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

#endif
