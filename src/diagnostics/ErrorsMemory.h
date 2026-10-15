#ifndef ERRORS_MEMORY_H
#define ERRORS_MEMORY_H

#include "diagnostics/Errors.h"

// Use-after-free error
class UseAfterFreeError : public TurfError {
public:
  UseAfterFreeError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You're using memory from '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "' after it was freed." + Colors::RESET,
                  "Don't use memory after it has been released (by free() or any function declared '@ownership(releases: ...)'). Consider setting the pointer to null after releasing it.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};
// Double free error
class DoubleFreeError : public TurfError {
public:
  DoubleFreeError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Double free detected for '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "'." + Colors::RESET,
                  "Release a pointer once: free() and every function declared '@ownership(releases: ...)' release what they are given.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};

// The "may" halves of the two errors above. The alias pass reports an ERROR
// only when the pointer was released on EVERY path reaching this point; when
// it was released on SOME paths only, whether this is a bug depends on a
// condition the pass cannot evaluate (a `freed` flag, say), so it warns.
class PossibleUseAfterFreeWarning : public TurfWarning {
public:
  PossibleUseAfterFreeWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "'" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW +
                        "' may already have been freed on some path reaching this use." +
                        Colors::RESET,
                    "It is released on at least one path that leads here. If a condition "
                    "rules that path out, this is fine; otherwise don't use it after "
                    "releasing it.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};
class PossibleDoubleFreeWarning : public TurfWarning {
public:
  PossibleDoubleFreeWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "'" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW +
                        "' may already have been freed on some path reaching this call." +
                        Colors::RESET,
                    "It is released on at least one path that leads here. If a condition "
                    "rules that path out, this is fine; otherwise release it only once.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};

// LANG-002: NullabilityPass's dereference gate -- a `&?T`-typed variable was
// field-accessed (`p.field`) without first being proven non-null on every
// path reaching this point (a direct `p != null` / `p == null` comparison,
// possibly through `&&`, an early return, or a `while` condition -- see
// NullabilityPass's own doc comment, src/analysis/Analysis.h). Deliberately in
// the MEMORY_ERROR category, alongside UseAfterFreeError/DoubleFreeError
// just above: a missed null check is the same class of mistake (using a
// reference whose validity was never established) as a missed free-state
// check, and both are raised by a CFG dataflow pass running at the same
// point in the pipeline (FuncDefExprAST::codegen(), CodegenFunc.cpp).
class PossibleNullDereferenceError : public TurfError {
public:
  /// What is reached through the reference. A method call dereferences its
  /// receiver exactly as a field read does (`self` is the receiver).
  enum class Member { Field, Method };
  /// Selects the constructor for a base that is not a variable.
  struct NotAVariable {};

  PossibleNullDereferenceError(SourceLocation Loc, const std::string &Name,
                               const std::string &MemberName,
                               Member Kind = Member::Field)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Possible null dereference: '" +
                      Colors::CYAN + Name + Colors::BRIGHT_RED +
                      "' might be 'null' here, but you're " + verb(Kind) +
                      " its '" + Colors::CYAN + MemberName +
                      Colors::BRIGHT_RED + "' " + noun(Kind) + "." +
                      Colors::RESET,
                  "Add a null check first, e.g. 'if (" + Name +
                      " != null) { ... }', and " + hintVerb(Kind) + " '." +
                      MemberName + (Kind == Member::Method ? "()" : "") +
                      "' only inside it.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }

  /// A `&?T` value that is not a variable: `p.next.val`, `get().val`,
  /// `list[i].val`. Only a variable compared with `null` is ever narrowed
  /// (NullabilityPass), so no check written around such an expression can
  /// prove it; the fix is always to name the value first.
  PossibleNullDereferenceError(SourceLocation Loc, NotAVariable,
                               const std::string &TypeName,
                               const std::string &MemberName,
                               Member Kind = Member::Field)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Possible null dereference: this '" +
                      Colors::CYAN + TypeName + Colors::BRIGHT_RED +
                      "' value might be 'null', but you're " + verb(Kind) +
                      " its '" + Colors::CYAN + MemberName +
                      Colors::BRIGHT_RED + "' " + noun(Kind) + "." +
                      Colors::RESET,
                  "A null check only works on a variable. Store the value in "
                  "one and check that: 'mut r: " + TypeName +
                      " = ...; if (r != null) { ... r." + MemberName +
                      (Kind == Member::Method ? "()" : "") + " ... }'.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }

private:
  static const char *verb(Member K) {
    return K == Member::Method ? "calling" : "accessing";
  }
  static const char *noun(Member K) {
    return K == Member::Method ? "method" : "field";
  }
  static const char *hintVerb(Member K) {
    return K == Member::Method ? "call" : "access";
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
  /// Selects the constructor for an operand that is not a variable.
  struct NotAVariable {};

  /// @p CopyOut: the cast copies the value out (`r to T`) rather than making
  /// a `&T`; the message says which.
  UnnarrowedNullableCastError(SourceLocation Loc, const std::string &Name,
                              bool CopyOut = false)
      : TurfError(Loc,
                  Colors::BRIGHT_RED +
                      (CopyOut ? "Cannot copy the value out of '" + Colors::CYAN + Name +
                                     Colors::BRIGHT_RED + "' here"
                               : "Cannot cast '" + Colors::CYAN + Name +
                                     Colors::BRIGHT_RED + "' to a non-null reference here") +
                      " -- it might be 'null'." + Colors::RESET,
                  "Narrow it first with a direct null check, e.g. 'if (" +
                      Name + " != null) { ... " + Name +
                      (CopyOut ? " to T" : " to &T") +
                      " ... }', so the cast only runs where it's "
                      "known to be safe.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }

  /// `(h.next to &Node)`: the operand is not a variable, so nothing can have
  /// narrowed it (see PossibleNullDereferenceError's second constructor).
  UnnarrowedNullableCastError(SourceLocation Loc, NotAVariable,
                              const std::string &TypeName, bool CopyOut = false)
      : TurfError(Loc,
                  Colors::BRIGHT_RED +
                      (CopyOut ? "Cannot copy the value out of this '" + Colors::CYAN +
                                     TypeName + Colors::BRIGHT_RED + "' value"
                               : "Cannot cast this '" + Colors::CYAN + TypeName +
                                     Colors::BRIGHT_RED + "' value to a non-null reference") +
                      " -- it might be 'null'." + Colors::RESET,
                  "A null check only works on a variable. Store the value in "
                  "one, check it, and cast inside the check: 'mut r: " +
                      TypeName + " = ...; if (r != null) { ... r " +
                      (CopyOut ? "to T" : "to &T") + " ... }'.") {
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
// FDN-002 (docs/development/ROADMAP_v2.0.0.md; FREEZE.md §6.13, F-MEM-5):
// EscapePass's one diagnostic -- raised at each of the five sinks
// (EscapePass's own doc comment, Analysis.h) when the value flowing in has
// been POSITIVELY proven Stack-provenance (points into the current
// function's own frame). `SinkDescription` names the specific sink in the
// message ("returned", "stored into a field that outlives this function",
// "pushed into a collection", "boxed with heap()") so the same class covers
// every sink without a diagnostic subclass per sink -- mirrors how
// UseAfterFreeError/DoubleFreeError just above share one MEMORY_ERROR
// category for the same class of mistake (using a reference whose validity
// this function cannot vouch for past its own return).
class DanglingReferenceEscapeError : public TurfError {
public:
  DanglingReferenceEscapeError(SourceLocation Loc,
                               const std::string &SinkDescription)
      : TurfError(Loc,
                  Colors::BRIGHT_RED +
                      "A reference into this function's own local storage "
                      "can't be " + Colors::CYAN + SinkDescription +
                      Colors::BRIGHT_RED +
                      " -- it would dangle the moment this function returns."
                      + Colors::RESET,
                  "heap(value) copies it onto the GC heap first and gives "
                  "you back a reference that outlives this function; or "
                  "return/store a reference you received as a parameter "
                  "instead of a local's.") {
    Category_ = SLM::ErrorCategory::MEMORY_ERROR;
  }
};

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
