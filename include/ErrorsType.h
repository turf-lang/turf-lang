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
class TypeError : public TurfError {
public:
  TypeError(SourceLocation Loc, const std::string &Msg,
           const std::string &CustomHint = "",
           const std::string &CustomBanner = "")
      : TurfError(Loc,
                  Colors::BRIGHT_RED +
                      (CustomBanner.empty() ? "Type mismatch!" : CustomBanner) +
                      "\n" + Colors::RESET + "  " + Msg,
                  CustomHint.empty()
                      ? "Check that both sides have compatible types."
                      : CustomHint) {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
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
// abs() Argument Type Error : non-int/double passed to abs
class AbsArgTypeError : public TurfError {
public:
  AbsArgTypeError(SourceLocation Loc, const std::string &GotTypeName)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED +
                "Oops! 'abs' only works with int or double, but you gave "
                "it a '" +
                Colors::CYAN + GotTypeName + Colors::BRIGHT_RED + "'.\n" +
                Colors::RESET + "  " + Colors::BRIGHT_GREEN + "Valid uses:\n" +
                "    abs(int)    → int\n" + "    abs(double) → double" +
                Colors::RESET,
            "abs gives the absolute (positive) value of a number.") {
    Category_ = SLM::ErrorCategory::BUILTIN_FUNCTION;
  }
};
// power() Argument Type Error : non-int/double passed to power
class PowerArgTypeError : public TurfError {
public:
  PowerArgTypeError(SourceLocation Loc, const std::string &GotType1,
                    const std::string &GotType2)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED +
                "Oops! 'power' only works with int or double, but you gave it "
                "'" +
                Colors::CYAN + GotType1 + Colors::BRIGHT_RED + "' and '" +
                Colors::CYAN + GotType2 + Colors::BRIGHT_RED + "'.\n" +
                Colors::RESET + "  " + Colors::BRIGHT_GREEN +
                "Valid overloads:\n" + "    power(int, int)       → int\n" +
                "    power(int, double)    → double\n" +
                "    power(double, int)    → double\n" +
                "    power(double, double) → double" + Colors::RESET,
            "power raises the first number to the power of the second.") {
    Category_ = SLM::ErrorCategory::BUILTIN_FUNCTION;
  }
};
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
class ArithmeticError : public TurfError {
public:
  ArithmeticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Math trouble! " + Colors::RESET + Msg,
            "Are you trying to divide by zero? Or combining things "
            "that can't be added together like a number and a word?") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
};
// Argument type mismatch in function call
class ArgumentTypeError : public TurfError {
public:
  ArgumentTypeError(SourceLocation Loc, const std::string &FuncName,
                    const std::string &ParamName, unsigned ArgIndex,
                    const std::string &ExpectedType, const std::string &GotType)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Type mismatch in call to '" + Colors::CYAN +
                FuncName + Colors::BRIGHT_RED + "'!\n" + Colors::RESET +
                "  Parameter '" + Colors::CYAN + ParamName + Colors::RESET +
                "' (argument " + std::to_string(ArgIndex) + ") expects '" +
                Colors::CYAN + ExpectedType + Colors::RESET + "', but got '" +
                Colors::CYAN + GotType + Colors::RESET + "'.",
            "You might need an explicit cast like " + ExpectedType +
                "(value), or pass a different value.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};
// Return type mismatch
class ReturnTypeMismatchError : public TurfError {
public:
  ReturnTypeMismatchError(SourceLocation Loc, const std::string &FuncName,
                          const std::string &ExpectedType,
                          const std::string &GotType)
      : TurfError(Loc, Colors::BRIGHT_RED + "Wrong return type in '" +
                           Colors::CYAN + FuncName + Colors::BRIGHT_RED +
                           "'!\n" + Colors::RESET + "  The function returns '" +
                           Colors::CYAN + ExpectedType + Colors::RESET +
                           "', but you're trying to return '" + Colors::CYAN +
                           GotType + Colors::RESET + "'.",
                  "Use an explicit cast like " + ExpectedType +
                  "(value), or change the function's return type.") {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
  }
};
// Using the result of a void function as a value
class VoidValueError : public TurfError {
public:
  VoidValueError(SourceLocation Loc, const std::string &Context)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Oops! You can't use a void value here!\n" +
                Colors::RESET + "  " + Context,
            "A void function doesn't produce a value. You can "
            "call it as a statement, but not use its result in an "
            "expression or assignment.") {
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
  }
};
class ImmutableAssignmentError : public TurfError {
public:
  ImmutableAssignmentError(SourceLocation Loc, const std::string &VarName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You can't change the value of '" +
                      Colors::CYAN + VarName + Colors::BRIGHT_RED +
                      "' because it's immutable.\n" + Colors::RESET,
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
