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

// LANG-002: NullabilityPass's dereference gate -- a `&?T`-typed variable was
// field-accessed (`p.field`) without first being proven non-null on every
// path reaching this point (a direct `p != null` / `p == null` comparison,
// possibly through `&&`, an early return, or a `while` condition -- see
// NullabilityPass's own doc comment, include/Analysis.h). Deliberately in
// the MEMORY_ERROR category, alongside UseAfterFreeError/DoubleFreeError
// just above: a missed null check is the same class of mistake (using a
// reference whose validity was never established) as a missed free-state
// check, and both are raised by a CFG dataflow pass running at the same
// point in the pipeline (FuncDefExprAST::codegen(), CodegenFunc.cpp).
class PossibleNullDereferenceError : public TurfError {
public:
  PossibleNullDereferenceError(SourceLocation Loc, const std::string &Name,
                               const std::string &FieldName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Possible null dereference: '" +
                      Colors::CYAN + Name + Colors::BRIGHT_RED +
                      "' might be 'null' here, but you're accessing its '" +
                      Colors::CYAN + FieldName + Colors::BRIGHT_RED +
                      "' field." + Colors::RESET,
                  "Add a null check first, e.g. 'if (" + Name +
                      " != null) { ... }', and access '." + FieldName +
                      "' only inside it.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};

// LANG-002: the companion check for the explicit `p to &T` narrowing cast
// (the only way to convert a `&?T` value into a `&T`-typed one -- see
// TurfNullableReferenceType's own doc comment, Types.h, for why this is
// never granted implicitly). Syntactically this cast is always legal (both
// sides are pointer-shaped, neither is a raw pointer, so it needs no
// `unsafe`) -- ITS soundness is exactly the same "proven non-null on this
// path" question the dereference gate above answers, just applied to a
// cast expression instead of a field access, so NullabilityPass checks both
// with the same dataflow state.
class UnnarrowedNullableCastError : public TurfError {
public:
  UnnarrowedNullableCastError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Cannot cast '" + Colors::CYAN + Name +
                      Colors::BRIGHT_RED +
                      "' to a non-null reference here -- it might be 'null'."
                      + Colors::RESET,
                  "Narrow it first with a direct null check, e.g. 'if (" +
                      Name + " != null) { ... " + Name +
                      " to &T ... }', so the cast only runs where it's "
                      "known to be safe.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
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
