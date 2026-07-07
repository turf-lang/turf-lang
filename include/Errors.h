#ifndef ERRORS_H
#define ERRORS_H

#include "Algorithms.h"
#include "Builtins.h"
#include "Colors.h"
#include "Lexer.h"
#include "SuggestionIndex.h"
#include "SLMIntegration.h"
#include <cstdlib>
#include <iostream>
#include <map>

#include <string>
#include <vector>

struct SourceLocation;

// Base Class
class TurfError {
protected:
  SourceLocation Loc;
  std::string Message;
  std::string Hint_;
  std::vector<RelatedLocation> RelatedLocations_; // structural context for SLM

public:
  TurfError(SourceLocation Loc, const std::string &Msg,
            const std::string &Hint = "")
      : Loc(Loc), Message(Msg), Hint_(Hint) {}

  virtual ~TurfError() = default;

  const std::string &getHint() const { return Hint_; }
  const std::string &getMessage() const { return Message; }

  const std::vector<RelatedLocation> &getRelatedLocations() const {
    return RelatedLocations_;
  }

  // Chainable: TurfError(loc, msg).withRelated(declLoc, "declared here").raise();
  TurfError &withRelated(SourceLocation RelLoc, const std::string &Desc) {
    RelatedLocations_.push_back({RelLoc, Desc});
    return *this;
  }

  void raise() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/false, RelatedLocations_);

    if (g_recoverActive)
      longjmp(g_recoverJmp, 1);
    else {
      DiagnosticEngine::flushAll();
      std::exit(1);
    }
  }

  void warn() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/true, RelatedLocations_);
  }

  static std::string formatRankedSuggestions(const RankedSuggestions &Suggestions) {
    std::string result = "";
    bool hasLocal = !Suggestions.Local.empty();
    bool hasModule = !Suggestions.Module.empty();

    if (hasLocal) {
      result += "\n  " + Colors::BRIGHT_GREEN + "It may be from the current file: ";
      for (size_t i = 0; i < Suggestions.Local.size(); ++i) {
        result += Colors::BRIGHT_YELLOW + "'" + Suggestions.Local[i] + "'" + Colors::BRIGHT_GREEN;
        if (i != Suggestions.Local.size() - 1) result += ", ";
      }
    }
    
    if (hasModule) {
      result += "\n  " + Colors::BRIGHT_GREEN + "Or, could be one of these from the imported files:";
      for (const auto &pair : Suggestions.Module) {
        result += "\n    - In module '" + Colors::BRIGHT_CYAN + pair.first + Colors::BRIGHT_GREEN + "': ";
        for (size_t i = 0; i < pair.second.size(); ++i) {
          result += Colors::BRIGHT_YELLOW + "'" + pair.second[i] + "'" + Colors::BRIGHT_GREEN;
          if (i != pair.second.size() - 1) result += ", ";
        }
      }
    }
    
    if (hasLocal || hasModule) {
      result += Colors::RESET;
    }
    return result;
  }

  static std::string suggestionsOrFallback(const RankedSuggestions &Suggestions, const std::string &Fallback, const std::string &Prefix = "") {
    std::string formatted = formatRankedSuggestions(Suggestions);
    if (!formatted.empty()) {
      if (formatted.rfind("\n  ", 0) == 0) {
        return Prefix + formatted.substr(3);
      }
      return Prefix + formatted;
    }
    return Fallback;
  }
};

// Warning Base Class
// Unlike TurfError::raise(), TurfWarning::emit() NEVER interrupts compilation.
// It registers a yellow "Warning:" entry with the DiagnosticEngine and
// returns normally so that every subsequent analysis pass continues.
class TurfWarning {
protected:
  SourceLocation Loc;
  std::string Message;
  std::string Hint_;
  std::vector<RelatedLocation> RelatedLocations_; // structural context for SLM

public:
  TurfWarning(SourceLocation Loc, const std::string &Msg,
              const std::string &Hint = "")
      : Loc(Loc), Message(Msg), Hint_(Hint) {}

  virtual ~TurfWarning() = default;

  const std::string &getHint() const { return Hint_; }
  const std::string &getMessage() const { return Message; }

  const std::vector<RelatedLocation> &getRelatedLocations() const {
    return RelatedLocations_;
  }

  TurfWarning &withRelated(SourceLocation RelLoc, const std::string &Desc) {
    RelatedLocations_.push_back({RelLoc, Desc});
    return *this;
  }

  // NEVER interrupts compilation flow — always returns normally.
  void emit() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/true, RelatedLocations_);
  }

  void warn() const { emit(); }
};

// Syntax Error : Parser issues
class SyntaxError : public TurfError {
public:
  SyntaxError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! There's a tiny mistake here:\n" +
                           Colors::RESET + "  " + Msg,
                  "Check your spelling, punctuation like ';' or "
                  "'}', and make sure everything is closed properly.") {}
};

// Type Error : Errors generated due to incompatible types
class TypeError : public TurfError {
public:
  TypeError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Type mismatch!\n" + Colors::RESET +
                      "  " + Msg,
                  "Check that you're not mixing incompatible types "
                  "(like assigning a string to an int variable).") {}
};

// String Conversion Error : Unsupported type passed to string(...)
class StringConversionError : public TurfError {
public:
  StringConversionError(SourceLocation Loc, const std::string &SrcTypeName)
      : TurfError(
            Loc, Colors::BRIGHT_RED + "Oops! I can't convert '" + Colors::CYAN +
                     SrcTypeName + Colors::BRIGHT_RED + "' to a string!\n" +
                     Colors::RESET,
            "Only these conversions are supported:\n"
            "    string(int)    - turns a whole number into text\n"
            "    string(double) - turns a decimal number into text\n"
            "    string(bool)   - gives you \"true\" or \"false\"") {}
};

// lengthof() Argument Type Error : non-string passed to lengthof
class LengthofTypeError : public TurfError {
public:
  LengthofTypeError(SourceLocation Loc, const std::string &GotTypeName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED +
                      "Oops! 'lengthof' only works with strings, but you gave "
                      "it a '" +
                      Colors::CYAN + GotTypeName + Colors::BRIGHT_RED + "'.\n" +
                      Colors::RESET,
                  "'lengthof' counts the characters in a string.\n"
                  "    Correct usage:  lengthof(\"hello\")  →  5\n"
                  "    Correct usage:  int n = lengthof(myStringVar)") {}
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
            "abs gives the absolute (positive) value of a number.") {}
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
            "power raises the first number to the power of the second.") {}
};

// Control Flow Misuse : break/continue/return in wrong context
class ControlFlowError : public TurfError {
public:
  ControlFlowError(SourceLocation Loc, const std::string &Msg)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Control flow error!\n" + Colors::RESET +
                "  " + Msg,
            "Make sure 'break' and 'continue' are inside a loop, "
            "and 'return' is inside a function.") {}
};

// Semantic Error : General semantic violations
class SemanticError : public TurfError {
public:
  SemanticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! Something doesn't add up:\n" + Colors::RESET +
                           "  " + Msg) {}
};

// Keyword Error : Misspelled keywords
class KeywordError : public TurfError {
public:
  KeywordError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED + "Hmm, I don't know the word '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {

    std::vector<std::string> Candidates = SuggestionIndex::suggestKeywords(Name);

    if (!Candidates.empty()) {
      std::string sug = "Did you mean: ";
      for (size_t i = 0; i < Candidates.size(); ++i) {
        sug += Colors::BRIGHT_YELLOW + "'" + Candidates[i] + "'" +
                   Colors::BRIGHT_GREEN;
        if (i != Candidates.size() - 1)
          sug += ", ";
      }
      sug += "?";
      Hint_ = sug;
    } else {
      Hint_ = "Check if you spelled the keyword correctly.";
    }
  }
};

// Lookup Error : Typo/Unknown function name
class LookupError : public TurfError {
public:
  LookupError(SourceLocation Loc, const std::string &Name,
              const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "Oops! I can't find a function named '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check the spelling against the built-in "
      "functions like 'printline' or 'lengthof'.");
  }
};

// Arithmetic Error : Math issues

class UnknownFunctionError : public TurfError {
public:
  UnknownFunctionError(SourceLocation Loc, const std::string &Name,
                       const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "Error: Unknown function '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check the spelling or import the module that "
      "defines it.");
  }
};

/// Emitted by the Resolver's Pass 2 when a struct field's NamedType reference
/// does not match any struct registered in Pass 1.
class UnknownTypeError : public TurfError {
public:
  UnknownTypeError(SourceLocation Loc, const std::string &TypeName,
                   const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "Error: Unknown type '" +
                           Colors::CYAN + TypeName + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Hint_ = suggestionsOrFallback(Suggestions,
      "Make sure the struct '" + TypeName +
      "' is declared before it is used, or check the spelling.");
  }
};


/// Emitted when the function name is known but the argument count is wrong.
/// Shows every valid overload signature so the user knows what they can write.
class WrongArityError : public TurfError {
public:
  WrongArityError(SourceLocation Loc, const std::string &Name,
                  int Got, const std::vector<std::string> &KnownSignatures)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "'" + Colors::CYAN + Name +
                      Colors::BRIGHT_RED + "' was called with " +
                      std::to_string(Got) + " argument" +
                      (Got == 1 ? "" : "s") +
                      ", but no overload accepts that many." +
                      Colors::RESET) {
    if (!KnownSignatures.empty()) {
      Message += "\n  " + Colors::BRIGHT_GREEN + "Known overloads:" +
                 Colors::RESET;
      for (const auto &Sig : KnownSignatures)
        Message += "\n    " + Colors::CYAN + Sig + Colors::RESET;
    }
    Hint_ = "Check the number of arguments.";
  }
};

class NestedFunctionError : public TurfError {
public:
  NestedFunctionError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Nested function definitions are not allowed: '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'.",
                  "Move the function to the top level of the "
                  "file or module.") {}
};
class ArithmeticError : public TurfError {
public:
  ArithmeticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Math trouble! " + Colors::RESET + Msg,
            "Are you trying to divide by zero? Or combining things "
            "that can't be added together like a number and a word?") {}
};

// Reference Error : Variable lookup issues
class ReferenceError : public TurfError {
public:
  ReferenceError(SourceLocation Loc, const std::string &Name,
                 const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "I can't find anything named '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'.") {
    Hint_ = suggestionsOrFallback(Suggestions,
      "Did you forget to create this variable? You have to "
      "create it before you can use it, like this: `int " +
      Name + " = 5;`");
  }
};

// Semantic Error : Symbol binding issues
class UseBeforeDeclarationError : public TurfError {
public:
  UseBeforeDeclarationError(SourceLocation UseLoc, const std::string &Name,
                            const RankedSuggestions &Suggestions)
      : TurfError(UseLoc, Colors::BRIGHT_RED + "Hold on! You're trying to use '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED +
                           "' before telling me what it is.") {
    Hint_ = suggestionsOrFallback(Suggestions,
      "Did you forget to create this variable? You have to "
      "create it before you can use it, like this: `int " +
      Name + " = 5;`");
  }
};

class DuplicateDeclarationError : public TurfError {
public:
  DuplicateDeclarationError(SourceLocation NewLoc, const std::string &Name,
                            SourceLocation PrevLoc)
      : TurfError(NewLoc,
                  Colors::BRIGHT_RED +
                      "Wait a minute! You already created a variable named '" +
                      Colors::CYAN + Name + Colors::BRIGHT_RED + "'.") {
    Message += "\n  " + Colors::BRIGHT_BLUE +
               "Note: You first created it at line " +
               std::to_string(PrevLoc.Line) + ", column " +
               std::to_string(PrevLoc.Col) + Colors::RESET;
    Hint_ = "If you want to change its value, just do `" + Name +
            " = new_value;` without the type (like 'int' or 'string'). If "
            "you want a new variable, give it a different name!";
    RelatedLocations_.push_back({PrevLoc, "first declared here"});
  }
};

class ShadowingWarning : public TurfWarning {
public:
  ShadowingWarning(SourceLocation NewLoc, const std::string &Name,
                   SourceLocation PrevLoc)
      : TurfWarning(NewLoc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "Just so you know, you're creating a new '" +
                        Colors::CYAN + Name + Colors::BRIGHT_YELLOW +
                        "' that hides an older one!" + Colors::RESET,
                    "It's better to give this new variable a different name "
                    "to avoid confusion later.") {
    Message += "\n  " + Colors::BRIGHT_BLUE +
               "Note: The older one is at line " +
               std::to_string(PrevLoc.Line) + ", column " +
               std::to_string(PrevLoc.Col) + Colors::RESET;
    RelatedLocations_.push_back({PrevLoc, "originally declared here"});
  }
};

class UnreachableCodeError : public TurfError {
public:
  UnreachableCodeError(SourceLocation Loc, const std::string &What,
                       const std::string &Terminator = "return")
      : TurfError(Loc, Colors::BRIGHT_RED + "This " + What +
                           " will never happen because you used '" +
                           Terminator + "' before it!" + Colors::RESET,
                  "Move this code before the '" + Terminator +
                  "' statement if you want it to run.") {}
};

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
            "and 'else') has a 'return' statement.") {}
};

class UnreachableBlockWarning : public TurfWarning {
public:
  UnreachableBlockWarning(SourceLocation Loc, const std::string &FunctionName,
                          const std::string &BlockName)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "The code in '" + BlockName +
                        "' inside '" + FunctionName +
                        "' can never be reached!" + Colors::RESET,
                    "Check your 'if' conditions. Are they impossible to be "
                    "true, or did you 'return' before this block?") {}
};

class DeadBranchWarning : public TurfWarning {
public:
  DeadBranchWarning(SourceLocation Loc, const std::string &FunctionName)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "There's a part of '" +
                        FunctionName + "' that will never be executed!" +
                        Colors::RESET,
                    "Check if you have code after a 'return', 'break', or "
                    "'continue' that can't be reached.") {}
};

class StatementAfterTerminatorError : public TurfError {
public:
  StatementAfterTerminatorError(SourceLocation Loc,
                                const std::string &Terminator)
      : TurfError(Loc, Colors::BRIGHT_RED + "This code comes after a " +
                           Terminator + ", so it's impossible to reach it!" +
                           Colors::RESET,
                  "Delete this code or move it before the '" +
                  Terminator + "' statement.") {}
};

// Lint Warnings - Reviewer-style heuristic pattern warnings
// Redundant comparison: x == true, x != false, etc.
class RedundantComparisonWarning : public TurfWarning {
public:
  RedundantComparisonWarning(SourceLocation Loc, const std::string &Expr,
                             const std::string &Simplified)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "This comparison is redundant!" + Colors::RESET + "\n  " +
                        Colors::BRIGHT_CYAN + Expr + Colors::RESET +
                        " is the same as just writing " + Colors::BRIGHT_CYAN +
                        Simplified + Colors::RESET + ".",
                    "You can simplify this by removing the comparison "
                    "entirely.") {}
};

// Self-assignment: x = x
class SelfAssignmentWarning : public TurfWarning {
public:
  SelfAssignmentWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "You're assigning '" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "' to itself — this doesn't do anything!" +
                        Colors::RESET,
                    "Did you mean to assign a different value, or is this "
                    "left over from an edit?") {}
};

// Infinite loop: while(true) with no break/continue/return in the body
class SuspiciousInfiniteLoopWarning : public TurfWarning {
public:
  SuspiciousInfiniteLoopWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "This looks like an infinite loop!" + Colors::RESET +
                        " The condition is always true and the body has no " +
                        Colors::CYAN + "'break'" + Colors::BRIGHT_YELLOW + ", " +
                        Colors::CYAN + "'continue'" + Colors::BRIGHT_YELLOW + ", or " +
                        Colors::CYAN + "'return'" + Colors::BRIGHT_YELLOW + ".",
                    "If this is intentional, consider adding a comment. "
                    "Otherwise, add a 'break' or a proper exit condition.") {}
};

// Empty block in if/else branch
class EmptyBranchWarning : public TurfWarning {
public:
  EmptyBranchWarning(SourceLocation Loc, const std::string &BranchKind)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "The '" + Colors::CYAN + BranchKind +
                        Colors::BRIGHT_YELLOW + "' branch is empty — it doesn't do anything!" +
                        Colors::RESET,
                    "If you haven't written the code yet, add a comment "
                    "like '// TODO'. Otherwise, you may be able to simplify this "
                    "'if' statement.") {}
};

// Self-comparison: x == x, x != x, x < x, etc.
class SelfComparisonWarning : public TurfWarning {
public:
  SelfComparisonWarning(SourceLocation Loc, const std::string &Name,
                        const std::string &Result)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "You're comparing '" + Colors::CYAN + Name +
                        Colors::BRIGHT_YELLOW + "' with itself!" + Colors::RESET +
                        " This is always " + Colors::BRIGHT_CYAN + Result +
                        Colors::RESET + ".",
                    "Did you mean to compare with a different variable?") {}
};

// Division / modulo by zero literal
class DivisionByZeroLiteralWarning : public TurfWarning {
public:
  DivisionByZeroLiteralWarning(SourceLocation Loc, const std::string &Op)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "You're using '" + Colors::CYAN + Op + Colors::BRIGHT_YELLOW +
                        "' with zero — this will crash at runtime!" + Colors::RESET,
                    "Check the right-hand side. Division or modulo by zero "
                    "is undefined behavior.") {}
};

// Identical if/else branches
class IdenticalBranchesWarning : public TurfWarning {
public:
  IdenticalBranchesWarning(SourceLocation Loc)
      : TurfWarning(Loc,
                    Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET +
                        Colors::BRIGHT_YELLOW + "Both branches of this 'if' do the same thing!" +
                        Colors::RESET,
                    "The 'if'/'else' can be removed — just keep the body. "
                    "Or did you forget to change one of the branches?") {}
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
                "(value), or pass a different value.") {}
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
                  "(value), or change the function's return type.") {}
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
            "expression or assignment.") {}
};

// Array index out of bounds (compile-time constant check)
class ArrayBoundsError : public TurfError {
public:
  ArrayBoundsError(SourceLocation Loc, const std::string &ArrayName,
                   long long Index, int ArraySize)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Array index out of bounds!\n" +
                      Colors::RESET + "  You tried to access '" + Colors::CYAN +
                      ArrayName + "[" + std::to_string(Index) + "]" +
                      Colors::RESET + "', but the array only has " +
                      Colors::CYAN + std::to_string(ArraySize) + Colors::RESET +
                      " elements (indices 0 to " +
                      std::to_string(ArraySize - 1) + ").",
                  "Array indices start at 0 and go up to size - 1.") {}
};

// Array initializer size mismatch
class ArraySizeMismatchError : public TurfError {
public:
  ArraySizeMismatchError(SourceLocation Loc, const std::string &ArrayName,
                         int DeclaredSize, int InitSize)
      : TurfError(Loc, Colors::BRIGHT_RED + "Array size mismatch!\n" +
                           Colors::RESET + "  '" + Colors::CYAN + ArrayName +
                           Colors::RESET + "' was declared with size " +
                           Colors::CYAN + std::to_string(DeclaredSize) +
                           Colors::RESET + ", but you gave " + Colors::CYAN +
                           std::to_string(InitSize) + Colors::RESET +
                           " initial values.",
                  "The number of values in [...] must match the "
                  "declared array size.") {}
};

// Non-integer array index
class ArrayNonIntegerIndexError : public TurfError {
public:
  ArrayNonIntegerIndexError(SourceLocation Loc, const std::string &GotType)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Array index must be an integer!\n" + Colors::RESET +
                           "  You used a value of type '" + Colors::CYAN +
                           GotType + Colors::RESET +
                           "' as an array index.",
                  "Use an 'int' value to index into an array, "
                  "like arr[0] or arr[i] where i is an int.") {}
};

// Array element type mismatch
class ArrayTypeMismatchError : public TurfError {
public:
  ArrayTypeMismatchError(SourceLocation Loc, const std::string &ArrayName,
                         const std::string &ExpectedType,
                         const std::string &GotType)
      : TurfError(
            Loc, Colors::BRIGHT_RED + "Array element type mismatch!\n" +
                     Colors::RESET + "  Array '" + Colors::CYAN + ArrayName +
                     Colors::RESET + "' holds '" + Colors::CYAN + ExpectedType +
                     Colors::RESET + "' values, but you tried to store a '" +
                     Colors::CYAN + GotType + Colors::RESET + "'.",
            "All elements in the array must match the declared "
            "element type.") {}
};

// Use-after-free error
class UseAfterFreeError : public TurfError {
public:
  UseAfterFreeError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You're using memory from '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "' after it was freed." + Colors::RESET,
                  "Don't use memory after calling free(). Consider setting the pointer to null after freeing.") {}
};

// Double free error
class DoubleFreeError : public TurfError {
public:
  DoubleFreeError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Double free detected for '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "'." + Colors::RESET,
                  "Avoid calling free() on the same pointer more than once.") {}
};

// Use-before-assignment error
class DefiniteAssignmentError : public TurfError {
public:
  DefiniteAssignmentError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You're using '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "' before it has a value." + Colors::RESET,
                  "Make sure every path assigns to '" + Name + "' before you use it.") {}
};

// List Errors

// Missing generic type parameter: list bad  (instead of list<int> bad)
class ListGenericMissingError : public TurfError {
public:
  ListGenericMissingError(SourceLocation Loc)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "List declaration is missing a type parameter!\n" +
                           Colors::RESET +
                           "  You must specify the element type inside angle "
                           "brackets.",
                  "Use list<int>, list<double>, list<bool>, or "
                  "list<string>.") {}
};

// Wrong element type pushed into list: push(3.14) into list<int>
class ListTypeMismatchError : public TurfError {
public:
  ListTypeMismatchError(SourceLocation Loc, const std::string &ListName,
                        const std::string &ExpectedElem,
                        const std::string &GotType)
      : TurfError(
            Loc, Colors::BRIGHT_RED + "Type mismatch on list '" + Colors::CYAN +
                     ListName + Colors::BRIGHT_RED + "'!\n" + Colors::RESET +
                     "  This is a list<" + Colors::CYAN + ExpectedElem +
                     Colors::RESET + ">, but you tried to push a '" +
                     Colors::CYAN + GotType + Colors::RESET + "' value.",
            "Push only '" + ExpectedElem +
            "' values. No implicit coercion is allowed for list "
            "operations.") {}
};

// Method called on non-list variable
class ListNonListOperationError : public TurfError {
public:
  ListNonListOperationError(SourceLocation Loc, const std::string &VarName,
                            const std::string &Method)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "'" + Colors::CYAN + VarName +
                Colors::BRIGHT_RED + "' is not a list!\n" + Colors::RESET +
                "  You called '" + Colors::CYAN + Method + Colors::RESET +
                "' on it, but that method only works on list<T> variables.",
            "Declare it with list<int>, list<string>, etc. to use " +
            Method + ".") {}
};

// pop() on an empty list (runtime guard emitted as IR)
class ListEmptyPopError : public TurfError {
public:
  ListEmptyPopError(SourceLocation Loc, const std::string &ListName)
      : TurfError(Loc, Colors::BRIGHT_RED + "Calling pop() on an empty list '" +
                           Colors::CYAN + ListName + Colors::BRIGHT_RED +
                           "' will crash at runtime!\n" + Colors::RESET,
                  "Check list.length > 0 before calling pop().") {}
};

// Phase 3: Module search error
class ModuleNotFoundError : public TurfError {
public:
  ModuleNotFoundError(SourceLocation Loc, const std::string &ModuleName)
      : TurfError(Loc, Colors::BRIGHT_RED + "Could not find module '" +
                           Colors::CYAN + ModuleName + Colors::BRIGHT_RED +
                           "'.\n" + Colors::RESET,
                  "Make sure the file '" + ModuleName + ".tr' exists in the standard library or your current directory.") {}
};


// Phase 2: Module-scoped method lookup error
// Raised when a user types e.g. io.pritn() — wrong function name in a known
// module. Suggestions are sourced from that module's function registry.
class ModuleMethodNotFoundError : public TurfError {
public:
  ModuleMethodNotFoundError(SourceLocation Loc,
                            const std::string &ModuleName,
                            const std::string &MethodName,
                            const RankedSuggestions &Suggestions)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! Module '" + Colors::CYAN +
                      ModuleName + Colors::BRIGHT_RED +
                      "' has no function named '" + Colors::CYAN + MethodName +
                      Colors::BRIGHT_RED + "'." + Colors::RESET) {
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check your spelling, or maybe you need to define it?",
      "Did you mean:\n");
  }
};

// Phase 2: Module-scoped argument type mismatch error
// Raised when the function name is found in a module but no overload matches
// the given argument types (e.g. math.abs(true) — no abs(bool) overload).
class ModuleArityOrTypeError : public TurfError {
public:
  ModuleArityOrTypeError(SourceLocation Loc,
                         const std::string &ModuleName,
                         const std::string &MethodName,
                         const std::string &Detail)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "No matching overload of '" +
                      Colors::CYAN + ModuleName + "." + MethodName +
                      Colors::BRIGHT_RED + "' for the arguments you gave.\n" +
                      Colors::RESET + "  " + Detail,
                  "Hint: Check the argument types and count. Use the "
                  "correct overload, e.g. 'math.abs(int)' or "
                  "'math.abs(double)'.") {}
};

class ImmutableAssignmentError : public TurfError {
public:
  ImmutableAssignmentError(SourceLocation Loc, const std::string &VarName)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You can't change the value of '" +
                      Colors::CYAN + VarName + Colors::BRIGHT_RED +
                      "' because it's immutable.\n" + Colors::RESET,
                  "If you need to change its value later, declare it "
                  "with 'mutable' instead of 'immutable'.") {}
};

#endif
