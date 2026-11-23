#ifndef ERRORS_TYPE_H
#define ERRORS_TYPE_H

#include "diagnostics/Errors.h"
#include <cctype>

// Type Error : Errors generated due to incompatible types
//
// TypeChecker::reportError() is the single production path every
// TypeChecker diagnostic funnels through, so this is the single
// constructor every one of them -- from a genuine "int vs string" clash to
// a duplicate `matches` pattern, a non-exhaustive enum, an out-of-range
// cast, or a missing initializer -- goes through. Two knobs let a call
// site say something accurate instead of settling for the generic
// default, each independently, without stacking alongside it:
//
//   CustomHint:   the actionable "what to do about it" line. Several call
//                 sites used to embed their own much more specific hint
//                 directly IN THE MESSAGE TEXT, before this parameter
//                 existed, producing a redundant, sometimes literally
//                 doubled ("Hint: ... \n\n Hint: ...") diagnostic.
//   CustomBanner: not printed. A diagnostic has no banner line above its
//                 message (docs/development/DIAGNOSTICS_STYLE.md rule 4): the
//                 heading already names the code. Kept so the sites that
//                 pass one need no change.
//
// Code (issue #263): REQUIRED. This class used to be the one code, E066, for
// every diagnostic that went through it, with the banner as free text -- a
// wrong argument count, a missing initializer and a raw pointer outside
// `unsafe` all told a beginner "type mismatch". The code is now the caller's
// (an ErrCode::), and it also decides the defaults the two knobs above fall
// back to: the hint is typeErrorDefaultHint() -- "make both sides the same
// type" is advice for a type mismatch and nothing else.
// "an `int`", "a `string`": a type's name in backticks with its article
// (as SpanLabels::aType() writes it for the TypeChecker). `u...` (`uint8`)
// takes "a".
inline std::string aTypeNamed(const std::string &Name) {
  const char C = Name.empty() ? 'x' : static_cast<char>(std::tolower(static_cast<unsigned char>(Name[0])));
  const bool An = C == 'a' || C == 'e' || C == 'i' || C == 'o';
  return std::string(An ? "an" : "a") + " `" + Name + "`";
}

inline const char *typeErrorDefaultHint(const std::string &Code) {
  if (Code == ErrCode::ArithmeticOperand)
    return "Use numbers with + - * / % and whole numbers with & | ^ << >>; "
           "convert an operand with `to`, e.g. `n + (x to int)`.";
  if (Code == ErrCode::InvalidComparison)
    return "Compare two operands of the same type, such as two `int`s or two "
           "`string`s. To compare structs, compare the fields you care about.";
  if (Code == ErrCode::AmbiguousCall)
    return "Remove one of the conflicting definitions or imports, or pass "
           "arguments only one of them accepts.";
  if (Code == ErrCode::UnsupportedOperation)
    return "Write the operation out in full instead, e.g. `x = x + 1;`.";
  if (Code == ErrCode::MissingInitializer)
    return "Give the variable its value on the spot: `immut X: int = 5;`, "
           "`mut x: guess = 5;`.";
  if (Code == ErrCode::MethodOnNonStruct)
    return "Check the spelling and the import: on a type that is not a struct, "
           "`x.f()` calls an imported function `f` that takes that type.";
  if (Code == ErrCode::ArgumentType || Code == ErrCode::ModuleArityOrType)
    return "Pass each argument as the type its parameter declares, converting "
           "with `to` where they differ (`x to int`); the candidates are "
           "listed above.";
  if (Code == ErrCode::ReturnTypeMismatch)
    return "Return a value of the declared type (convert with `to`, e.g. "
           "`return x to int;`), or change the type after `):` in the "
           "function's declaration.";
  if (Code == ErrCode::ArrayTypeMismatch)
    return "Give every element the array's element type, converting one with "
           "`to` where they differ.";
  if (Code == ErrCode::WrongArity)
    return "Pass exactly as many arguments as the function declares "
           "parameters.";
  if (Code == ErrCode::VoidValue)
    return "Call the function as a statement of its own, or declare it to return "
           "the type you need.";
  if (Code == ErrCode::InvalidCast)
    return "`to` converts numbers, chars and text: `5 to double`, `3.9 to int`, "
           "`42 to string`, `65 to char`. To test a number for zero write "
           "`n != 0`; `turf --explain E134` lists the rest.";
  if (Code == ErrCode::StringConversion)
    return "Convert an `int`, a `double`, a `bool` or an enum variant with "
           "`to string`, e.g. `n to string`.";
  if (Code == ErrCode::NotCallable)
    return "Remove the `(...)`: the expression is complete without a call.";
  if (Code == ErrCode::NotAValue)
    return "Call it directly where you need its result.";
  if (Code == ErrCode::InvalidModuleQualifier)
    return "Use one module name, e.g. `io::printline(...)`: modules do not "
           "nest.";
  if (Code == ErrCode::MissingTypeArguments)
    return "Write the type arguments, e.g. `List<int>`.";
  if (Code == ErrCode::NothingToInfer)
    return "Write the type yourself instead of relying on inference.";
  return nullptr;
}

class TypeError : public TurfError {
  static std::string hintFor(const char *Code, const std::string &Custom) {
    if (!Custom.empty())
      return Custom;
    if (std::string(Code) != ErrCode::TypeMismatch)
      if (const char *Default = typeErrorDefaultHint(Code))
        return Default;
    return "Make both sides the same type: convert one with `to` (`x to int`), "
           "or change a declaration so the types agree.";
  }

public:
  TypeError(SourceLocation Loc, const char *Code, const std::string &Msg,
            const std::string &CustomHint = "",
            const std::string &CustomBanner = "")
      : TurfError(Loc, Colors::BRIGHT_RED + Msg + Colors::RESET,
                  hintFor(Code, CustomHint)) {
    (void)CustomBanner; // DX-9: no banner; the heading names the code
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
    withCode(Code);
    // The registry row is the audited answer for what KIND of mistake the code
    // is: a wrong argument count reaches `--smart` as FUNCTION_SIGNATURE, not
    // as the TYPE_MISMATCH this class used to default everything to.
    if (const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(Code))
      Category_ = Row->Category;
  }
};
// String Conversion Error : Unsupported type cast with `... to string`
class StringConversionError : public TurfError {
public:
  StringConversionError(SourceLocation Loc, const std::string &SrcTypeName)
      : TurfError(
            Loc, Colors::BRIGHT_RED + "I cannot convert " + aTypeNamed(SrcTypeName) +
                     " to a `string`." +
                     Colors::RESET,
            "Convert an `int`, a `double` or a `bool` with `to string`, e.g. "
            "`n to string`.") {
    Category_ = SLM::ErrorCategory::STRING_CONVERSION;
  }
};
// (AbsArgTypeError / PowerArgTypeError, E055 / E063, were deleted in issue
// #263: `abs` and `power` stopped being compiler intrinsics long ago, nothing
// constructed these, and their hint text described a world that no longer
// exists. A bad argument to either is now an ordinary E056/E025.)
// Compare Statement Structural Error : parse-time violations in compare blocks
class CompareError : public TurfError {
public:
  CompareError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + Msg + Colors::RESET,
                  "Write each arm as `matches value: ... done` and put "
                  "`otherwise: ...` last, once.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Compare Type Error : subject and pattern type mismatch in compare
class CompareTypeError : public TurfError {
public:
  CompareTypeError(SourceLocation Loc, const std::string &SubjectType,
                   const std::string &PatternType)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I cannot match " + aTypeNamed(SubjectType) +
                      " subject against " + aTypeNamed(PatternType) + " arm." +
                      Colors::RESET,
                  "Write each `matches` arm with the subject's type, `" +
                      SubjectType + "`.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};
// Compare Warning : emitted for empty compare {} blocks (no arms, no otherwise)
class CompareWarning : public TurfWarning {
public:
  CompareWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    Colors::BRIGHT_YELLOW +
                        "This `compare` has no `matches` arm and no "
                        "`otherwise`, so it does nothing." +
                        Colors::RESET,
                    "Add a `matches` arm or an `otherwise` arm, or remove the "
                    "`compare`.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// SEM-002 (docs/archive/ARCHITECTURE_OVERHAUL.md §8, FREEZE.md §6.2 row 3):
// an untyped integer literal (or a compile-time constant expression built
// only from untyped literals via unary '-' and binary + - *, FREEZE.md
// §6.2 row 6) whose EXACT value doesn't fit the type it's acquiring --
// `int`'s or `int32`'s signed range, or (for an implicit int-literal ->
// double conversion, row 9) int64 at all (see TypeChecker::
// checkLiteralRepresentable()'s own doc comment for why that's the actual
// gating condition Codegen can honor today, not the pure "exactly
// representable as a double" ideal FREEZE.md states). `RangeDescription`
// is pre-formatted by the caller (e.g. "-9223372036854775808 ..
// 9223372036854775807") rather than derived here, since the two
// destination types this fires for have different widths and this class
// has no reason to know either literally.
class IntegerLiteralOutOfRangeError : public TurfError {
public:
  IntegerLiteralOutOfRangeError(SourceLocation Loc, const std::string &ExactValue,
                                const std::string &DestTypeName,
                                const std::string &RangeDescription)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "The literal " + Colors::CYAN + ExactValue +
                Colors::BRIGHT_RED + " does not fit in " + aTypeNamed(DestTypeName) +
                ", which holds " + RangeDescription + "." + Colors::RESET,
            DestTypeName == "int"
                ? "Write a smaller number: `int` is the widest integer type."
                : "Use `int` instead, or convert with `to " + DestTypeName +
                      "` if you want the value truncated.") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
};
// SEM-002, FREEZE.md §6.2 row 9: an untyped integer literal directly
// initializing a 'double' whose exact value can't be rounded to that
// double WITHOUT loss (e.g. 9223372036854775807 -- INT64_MAX itself isn't
// exactly representable in a double). Distinct from
// IntegerLiteralOutOfRangeError above: the literal DOES fit int64 (that's
// checked first, separately -- see checkLiteralRepresentable()'s own
// comment), it just can't be rounded exactly.
class IntegerLiteralNotExactAsDoubleError : public TurfError {
public:
  IntegerLiteralNotExactAsDoubleError(SourceLocation Loc, const std::string &ExactValue)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "The literal " + Colors::CYAN + ExactValue +
                Colors::BRIGHT_RED + " has no exact `double`, so I would lose "
                "its last digits converting it." + Colors::RESET,
            "Use `int` instead, or write `" + ExactValue +
                ".0` if the rounded number is what you want.") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
};
// SEM-002, FREEZE.md §6.2 row 2/3/8: a float literal whose decimal->binary
// rounding overflowed to +/-inf (NumberExprAST::overflowed(), set by
// ParseNumberExpr -- SEM-001). Unlike the two classes above, this is
// destination-independent: an inf-rounded literal is never valid,
// regardless of what it's being assigned to (row 8: "In a literal: hard
// error" -- no destination check needed first).
class FloatLiteralOverflowError : public TurfError {
public:
  FloatLiteralOverflowError(SourceLocation Loc, const std::string &Spelling)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "The literal `" + Colors::CYAN + Spelling +
                Colors::BRIGHT_RED + "` is too large for a `double`, which "
                "holds up to about 1.8e308." + Colors::RESET,
            "Write a smaller number, or measure in a larger unit.") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
};
// (ArithmeticError E057, ArgumentTypeError E056, ReturnTypeMismatchError E064
// and VoidValueError E067 were deleted in issue #263. Each was documented and
// registered and none was ever constructed -- every such mistake was reported
// as E066 "Type mismatch!" instead -- and their hint texts were stale ("an
// explicit cast like int(value)" is not Turf syntax). The codes live on:
// TypeChecker::reportError() names them per site and keeps the curated
// messages, see ErrorCodes.h's ErrCode:: block.)
class ImmutableAssignmentError : public TurfError {
public:
  ImmutableAssignmentError(SourceLocation Loc, const std::string &VarName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I cannot change `" + Colors::CYAN +
                      VarName + Colors::BRIGHT_RED + "`: it is immutable." +
                      Colors::RESET,
                  "Declare it `mut` instead of `immut` to change it later.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
class NullAssignmentError : public TurfError {
public:
  NullAssignmentError(SourceLocation Loc, const std::string &VarName,
                      const std::string &TypeName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I cannot store `null` in `" +
                      Colors::CYAN + VarName + Colors::BRIGHT_RED +
                      "`, which is " + aTypeNamed(TypeName) + "." + Colors::RESET,
                  "Declare it as a pointer or a `&?` reference to hold `null`, "
                  "or give it " + aTypeNamed(TypeName) + ".") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};
// Invalid Assignment Target Error : the LHS of '=' isn't an lvalue (a
// variable, struct field, or array element) -- e.g. `true = 5;` or
// `break = 5;`. Without this check, visitAssignment() type-checked both
// sides and reported a generic TypeError as if the LHS were a real,
// declared variable of whatever type it happened to have (even inventing
// nonsense like "variable of type void" for `break`, which has no type at
// all) -- confusing and wrong about the actual rule being violated.
class InvalidAssignmentTargetError : public TurfError {
public:
  InvalidAssignmentTargetError(SourceLocation Loc, const std::string &What)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I cannot assign to " +
                      Colors::CYAN + What + Colors::BRIGHT_RED +
                      ": only a variable, a struct field or an array element "
                      "can be assigned." + Colors::RESET,
                  "Assign to a variable, a field or an element instead.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

// B-17 / D-27 (issue #330): `mut l: List<int> = List::default();` -- the
// construction's type arguments come from where its value goes, not from the
// call. Written where it is built, `List<int>::default()`, the call says what
// it builds on its own; 3.0 requires that (B-17).
class TypeArgsFromContextWarning : public TurfWarning {
public:
  TypeArgsFromContextWarning(SourceLocation Loc, const std::string &Call,
                             const std::string &Args, const std::string &Written)
      : TurfWarning(Loc,
                    "`" + Call + "` takes its type arguments, `" + Args +
                        "`, from where its value goes.",
                    "Write them where it is built: `" + Written + "`.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};

// AN-6 (TDP-2-007, F-22, issue #353): an `int` past 2^53 has no exact
// `double`, so converting one may lose its last digits. Opt-in (starts at
// `allow` and stays a warning: Overhaul 10.5).
class IntToDoublePrecisionWarning : public TurfWarning {
public:
  explicit IntToDoublePrecisionWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    "This `int` becomes a `double`, which holds every `int` "
                    "only up to 2^53. A larger `int` loses its last digits.",
                    "Keep it an `int`, or keep it within +-9007199254740992 "
                    "(2^53) before converting it.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};

#endif
