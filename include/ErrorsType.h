#ifndef ERRORS_TYPE_H
#define ERRORS_TYPE_H

#include "Errors.h"

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
//   CustomBanner: the bold header line above the message. Left at the
//                 default "Type mismatch!" for genuine type clashes;
//                 overridden for anything that ISN'T actually a mismatch
//                 between two types (e.g. "Non-exhaustive match!",
//                 "Missing initializer!", "Duplicate pattern!") so the
//                 header doesn't misdescribe what's actually wrong.
//
// Code (issue #263): REQUIRED. This class used to be the one code, E066, for
// every diagnostic that went through it, with the banner as free text -- a
// wrong argument count, a missing initializer and a raw pointer outside
// `unsafe` all told a beginner "type mismatch". The code is now the caller's
// (an ErrCode::), and it also decides the defaults the two knobs above fall
// back to: the heading is the code's registry description (E066 alone is "Type
// mismatch!"), the hint is defaultHintFor() -- "Check that both sides have
// compatible types." is advice for a type mismatch and nothing else.
inline const char *typeErrorDefaultHint(const std::string &Code) {
  if (Code == ErrCode::ArithmeticOperand)
    return "Check that both sides of the operator are values it works on: "
           "numbers for + - * / %, whole numbers for & | ^ << >>.";
  if (Code == ErrCode::InvalidComparison)
    return "Compare values of the same kind (two ints, two strings, ...). To "
           "compare structs, compare the fields you care about.";
  if (Code == ErrCode::AmbiguousCall)
    return "More than one definition fits equally well. Remove one of the "
           "conflicting definitions or imports, or call it in a way only one "
           "of them accepts.";
  if (Code == ErrCode::UnsupportedOperation)
    return "Write the operation out in full instead, e.g. `x = x + 1;`.";
  if (Code == ErrCode::MissingInitializer)
    return "Give the variable its value on the spot: `immut X: int = 5;`, "
           "`mut x: guess = 5;`.";
  if (Code == ErrCode::MethodOnNonStruct)
    return "Only structs have methods. `x.f()` on another type works only when "
           "a function `f` that takes that type is imported -- check the "
           "spelling and the import.";
  if (Code == ErrCode::ArgumentType || Code == ErrCode::ModuleArityOrType)
    return "Check the type of each argument against the function's "
           "parameters; the candidates are listed above.";
  if (Code == ErrCode::ReturnTypeMismatch)
    return "Return a value of the declared type, or change the function's "
           "declared return type.";
  if (Code == ErrCode::ArrayTypeMismatch)
    return "Every value in the list must be the array's element type.";
  if (Code == ErrCode::WrongArity)
    return "Check the number of arguments.";
  if (Code == ErrCode::VoidValue)
    return "A function declared ': void' produces no value, so there is "
           "nothing to use here.";
  if (Code == ErrCode::InvalidCast)
    return "Only some conversions are allowed with `to`; see `turf --explain "
           "E134` for the list.";
  if (Code == ErrCode::StringConversion)
    return "Only these convert to text: `intExpr to string`, `doubleExpr to "
           "string`, `boolExpr to string` (and enum values).";
  if (Code == ErrCode::NotCallable)
    return "Drop the '(...)': it is already a complete value on its own.";
  if (Code == ErrCode::NotAValue)
    return "Call it directly where you need its result.";
  if (Code == ErrCode::InvalidModuleQualifier)
    return "Modules aren't nested inside each other: use one module name, "
           "e.g. `io::printline(...)`.";
  if (Code == ErrCode::MissingTypeArguments)
    return "Write the type arguments, e.g. `List<int>`.";
  if (Code == ErrCode::NothingToInfer)
    return "Say the type yourself instead of relying on inference.";
  return nullptr;
}

class TypeError : public TurfError {
  static std::string bannerFor(const char *Code, const std::string &Custom) {
    if (!Custom.empty())
      return Custom;
    if (std::string(Code) != ErrCode::TypeMismatch)
      if (const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(Code))
        return std::string(Row->Description) + "!";
    return "Type mismatch!";
  }
  static std::string hintFor(const char *Code, const std::string &Custom) {
    if (!Custom.empty())
      return Custom;
    if (std::string(Code) != ErrCode::TypeMismatch)
      if (const char *Default = typeErrorDefaultHint(Code))
        return Default;
    return "Check that both sides have compatible types.";
  }

public:
  TypeError(SourceLocation Loc, const char *Code, const std::string &Msg,
            const std::string &CustomHint = "",
            const std::string &CustomBanner = "")
      : TurfError(Loc,
                  Colors::BRIGHT_RED + bannerFor(Code, CustomBanner) + "\n" +
                      Colors::RESET + "  " + Msg,
                  hintFor(Code, CustomHint)) {
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
            Loc, Colors::BRIGHT_RED + "Oops! I can't convert '" + Colors::CYAN +
                     SrcTypeName + Colors::BRIGHT_RED + "' to a string!\n" +
                     Colors::RESET,
            "Only these conversions are supported:\n"
            "    intExpr to string    - turns a whole number into text\n"
            "    doubleExpr to string - turns a decimal number into text\n"
            "    boolExpr to string   - gives you \"true\" or \"false\"") {
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
                  Colors::BRIGHT_RED + "compare error!\n" + Colors::RESET +
                      "  " + Msg,
                  "Check the structure of your compare statement: matches "
                  "arms need 'done', and 'otherwise' must be the last arm.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// Compare Type Error : subject and pattern type mismatch in compare
class CompareTypeError : public TurfError {
public:
  CompareTypeError(SourceLocation Loc, const std::string &SubjectType,
                   const std::string &PatternType)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Type mismatch in compare!\n" +
                      Colors::RESET +
                      "  Subject has type '" + Colors::CYAN + SubjectType +
                      Colors::RESET + "' but a matches arm has type '" +
                      Colors::CYAN + PatternType + Colors::RESET + "'.",
                  "All matches arm values must have the same type as the "
                  "compare subject (int or string).") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};
// Compare Warning : emitted for empty compare {} blocks (no arms, no otherwise)
class CompareWarning : public TurfWarning {
public:
  CompareWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    Colors::BRIGHT_YELLOW +
                        "Warning: Empty compare statement has no arms and no "
                        "'otherwise' — this is a no-op." +
                        Colors::RESET,
                    "Add at least one 'matches' arm or an 'otherwise' arm.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};
// SEM-002 (docs/planning/ARCHITECTURE_OVERHAUL.md §8, FREEZE.md §6.2 row 3):
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
            Colors::BRIGHT_RED + "Integer literal is not representable in type '" +
                Colors::CYAN + DestTypeName + Colors::BRIGHT_RED + "'.\n" +
                Colors::RESET + "  The literal's value is " + Colors::CYAN +
                ExactValue + Colors::RESET + ".\n  '" + DestTypeName +
                "' holds " + RangeDescription + ".",
            DestTypeName == "int"
                ? "'int' is TurfLang's only general-purpose integer type; "
                  "there is no wider one. If you need arbitrary precision, "
                  "use a library type."
                : "Use 'int' instead, or an explicit 'to " + DestTypeName +
                      "' cast if truncation is actually what you want.") {
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
            Colors::BRIGHT_RED +
                "Integer literal cannot be represented EXACTLY as a 'double'.\n" +
                Colors::RESET + "  The literal's value is " + Colors::CYAN +
                ExactValue + Colors::RESET +
                "; converting it to 'double' would silently lose precision.",
            "Use 'int' instead, or write the value as a float literal "
            "('" + ExactValue + ".0') if the rounded value is what you "
            "actually want.") {
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
            Colors::BRIGHT_RED +
                "Float literal is too large to represent -- it rounds to infinity.\n" +
                Colors::RESET + "  '" + Colors::CYAN + Spelling + Colors::RESET +
                "' overflows 'double's range.",
            "'double' (IEEE-754) can represent roughly up to 1.8e308. "
            "Did you mean a smaller value, or a different unit?") {
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
                  Colors::BRIGHT_RED + "Oops! You can't change the value of '" +
                      Colors::CYAN + VarName + Colors::BRIGHT_RED +
                      "' because it's immutable." + Colors::RESET,
                  "If you need to change its value later, declare it "
                  "with 'mutable' instead of 'immutable'.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};
class NullAssignmentError : public TurfError {
public:
  NullAssignmentError(SourceLocation Loc, const std::string &VarName,
                      const std::string &TypeName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Cannot assign 'null' to variable '" +
                      Colors::CYAN + VarName + Colors::BRIGHT_RED +
                      "' of type '" + Colors::CYAN + TypeName +
                      Colors::BRIGHT_RED + "'." + Colors::RESET,
                  "'null' is only valid for pointer types. "
                  "If you want a zero value, use the default initializer "
                  "(declare without '= null').") {
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
                  Colors::BRIGHT_RED + "Cannot assign to " +
                      Colors::CYAN + What + Colors::BRIGHT_RED +
                      " -- it isn't a variable, struct field, or array "
                      "element." + Colors::RESET,
                  "Only those can appear on the left of '='.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

#endif
