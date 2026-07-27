#ifndef ERRORS_H
#define ERRORS_H

#include "Algorithms.h"
#include "Builtins.h"
#include "Colors.h"
#include "ErrorCategory.h"
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
  // Ground-truth error category, set by each subclass's own constructor body
  // (e.g. `Category_ = SLM::ErrorCategory::TYPE_MISMATCH;`). This is the
  // error's own C++ type telling the SLM layer what it is, directly --
  // replacing the old approach of re-deriving a category by pattern-matching
  // the final rendered message text (SLM::classifyError()), which silently
  // drifted out of sync every time a message was reworded and left several
  // real, commonly-hit error classes (ImmutableAssignmentError,
  // NullAssignmentError, DefiniteAssignmentError, UseAfterFreeError,
  // DoubleFreeError) permanently misclassified as GENERAL. classifyError()
  // is NOT removed -- it remains the correct fallback for the raw,
  // untyped LogErrorAt() path (parser-level errors with no TurfError
  // instance at all) and for any subclass left untagged (default GENERAL).
  SLM::ErrorCategory Category_ = SLM::ErrorCategory::GENERAL;

public:
  TurfError(SourceLocation Loc, const std::string &Msg,
            const std::string &Hint = "")
      : Loc(Loc), Message(Msg), Hint_(Hint) {}

  virtual ~TurfError() = default;

  const std::string &getHint() const { return Hint_; }
  const std::string &getMessage() const { return Message; }
  SLM::ErrorCategory getCategory() const { return Category_; }

  const std::vector<RelatedLocation> &getRelatedLocations() const {
    return RelatedLocations_;
  }

  // Chainable: TurfError(loc, msg).withRelated(declLoc, "declared here").raise();
  TurfError &withRelated(SourceLocation RelLoc, const std::string &Desc) {
    RelatedLocations_.push_back({RelLoc, Desc});
    return *this;
  }

  void raise() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/false,
                          RelatedLocations_, Category_);

    if (g_recoverActive)
      longjmp(g_recoverJmp, 1);
    else {
      DiagnosticEngine::flushAll();
      std::exit(1);
    }
  }

  void warn() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/true,
                          RelatedLocations_, Category_);
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
  // Mirrors TurfError::Category_ for class symmetry. Currently inert: the
  // SLM layer is only ever invoked for errors (DiagnosticEngine::add()'s
  // AI-augmentation block is gated on `!IsWarning`), so no warning's
  // category is ever read. Kept for consistency and in case that changes.
  SLM::ErrorCategory Category_ = SLM::ErrorCategory::GENERAL;

public:
  TurfWarning(SourceLocation Loc, const std::string &Msg,
              const std::string &Hint = "")
      : Loc(Loc), Message(Msg), Hint_(Hint) {}

  virtual ~TurfWarning() = default;

  const std::string &getHint() const { return Hint_; }
  const std::string &getMessage() const { return Message; }
  SLM::ErrorCategory getCategory() const { return Category_; }

  const std::vector<RelatedLocation> &getRelatedLocations() const {
    return RelatedLocations_;
  }

  TurfWarning &withRelated(SourceLocation RelLoc, const std::string &Desc) {
    RelatedLocations_.push_back({RelLoc, Desc});
    return *this;
  }

  // NEVER interrupts compilation flow — always returns normally.
  void emit() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/true,
                          RelatedLocations_, Category_);
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
                  "'}', and make sure everything is closed properly.") {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
  }
};

// Type Error : Errors generated due to incompatible types
class TypeError : public TurfError {
public:
  // CustomHint, when non-empty, replaces the generic default below instead
  // of stacking alongside it. TypeChecker::reportError() is the single
  // production path every TypeChecker diagnostic funnels through
  // (TypeChecker.h's productionErrorHandler always constructs a TypeError
  // regardless of what kind of semantic problem actually occurred -- a
  // duplicate `matches` pattern, a non-exhaustive enum, an arity mismatch,
  // and a genuine "int vs string" type clash all render as "Type
  // mismatch!" with this same hint), so several call sites used to embed
  // their own much more specific hint directly IN THE MESSAGE TEXT as the
  // only way to say anything more useful -- producing a redundant,
  // sometimes literally doubled ("Hint: ... \n\n Hint: ...") diagnostic.
  TypeError(SourceLocation Loc, const std::string &Msg,
           const std::string &CustomHint = "")
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Type mismatch!\n" + Colors::RESET +
                      "  " + Msg,
                  CustomHint.empty()
                      ? "Check that you're not mixing incompatible types "
                        "(like assigning a string to an int variable)."
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
                  "    Correct usage:  int n = lengthof(myStringVar)") {
    Category_ = SLM::ErrorCategory::BUILTIN_FUNCTION;
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

// Control Flow Misuse : break/continue/return in wrong context
class ControlFlowError : public TurfError {
public:
  ControlFlowError(SourceLocation Loc, const std::string &Msg)
      : TurfError(
            Loc,
            Colors::BRIGHT_RED + "Control flow error!\n" + Colors::RESET +
                "  " + Msg,
            "Make sure 'break' and 'continue' are inside a loop, "
            "and 'return' is inside a function.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

// Semantic Error : General semantic violations
class SemanticError : public TurfError {
public:
  SemanticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! Something doesn't add up:\n" + Colors::RESET +
                           "  " + Msg) {
    // Intentionally left as the default GENERAL: this class is a genuine
    // catch-all for miscellaneous semantic violations that don't fit any
    // other bucket, not an oversight.
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

// Keyword Error : Misspelled keywords
class KeywordError : public TurfError {
public:
  KeywordError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED + "Hmm, I don't know the word '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;

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
    // UNDECLARED_VAR, not BUILTIN_FUNCTION: this is about an unresolved
    // *name* (could be a typo'd user function), not a builtin's own
    // argument-type rules -- matches the enum's own stated definition
    // better than the old text-matcher's (accidental) BUILTIN_FUNCTION
    // routing for "Unknown function" text.
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
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
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR; // see LookupError's note above
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check the spelling or import the module that "
      "defines it.");
  }
};

/**
 * @brief A struct instance method call (`x.method(...)`) names a method that
 *        doesn't exist on that struct at all -- not "wrong argument count",
 *        genuinely no method by that name.
 *
 * Before this class existed, TypeChecker::visitMethodCall()'s struct-method
 * branch had a deliberate "be permissive" fallback for this exact case
 * (Node.ResolvedCallTarget simply stayed null, with a comment saying so).
 * That silence propagated all the way to Codegen: MethodCallExprAST::
 * codegen()'s struct-method path does `if (!CalleeF) return nullptr;` with
 * NO diagnostic of its own either. The net effect, confirmed via
 * `l.nonexistentMethod(5)` on a `List<int>`: the compiler reports success,
 * and the method call simply VANISHES from the compiled program -- no
 * error, no warning, no runtime crash, just silently miscompiled code that
 * looks like it worked. This is the struct-method counterpart of
 * UnknownFunctionError (free functions) / the Resolver's module-method
 * UnknownFunctionError usage (`module.method(...)`) -- both of those paths
 * already raised a real error for the equivalent mistake; only the struct
 * path was silently permissive.
 */
class StructMethodNotFoundError : public TurfError {
public:
  StructMethodNotFoundError(SourceLocation Loc, const std::string &StructName,
                            const std::string &MethodName,
                            const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + StructName +
                           Colors::BRIGHT_RED + "' has no method named '" +
                           Colors::CYAN + MethodName + Colors::BRIGHT_RED +
                           "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    Hint_ = suggestionsOrFallback(Suggestions,
      "Double-check the spelling, or make sure the method is declared as "
      "`fn " + StructName + "." + MethodName + "(...)` (instance method, " +
      "called as `x." + MethodName + "(...)`) or `fn " + StructName +
      "::" + MethodName + "(...)` (static method, called as `" + StructName +
      "::" + MethodName + "(...)`).");
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
    Category_ = SLM::ErrorCategory::DECLARATION;
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
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
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
                  "file or module.") {
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
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

// Reference Error : Variable lookup issues
class ReferenceError : public TurfError {
public:
  ReferenceError(SourceLocation Loc, const std::string &Name,
                 const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "I can't find anything named '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'.") {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    Hint_ = suggestionsOrFallback(Suggestions,
      "Did you forget to create this variable? You have to "
      "create it before you can use it, like this: `mut " +
      Name + ": int = 5;`");
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
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    Hint_ = suggestionsOrFallback(Suggestions,
      "Did you forget to create this variable? You have to "
      "create it before you can use it, like this: `mut " +
      Name + ": int = 5;`");
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
    Category_ = SLM::ErrorCategory::DECLARATION;
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

/**
 * @brief A struct or enum name collides with one already declared -- either
 *        in the same file, or in a DIFFERENT (possibly unrelated) module
 *        pulled in transitively via 'import'.
 *
 * Unlike functions (which are namespaced per-module: `io.printline()` vs.
 * `math.printline()` can coexist), struct and enum names share ONE flat
 * namespace across the current file and every transitively-imported module
 * -- this is also why `List<int>` works without a `list.` prefix in the
 * first place (see list.tr). The tradeoff is that two unrelated modules
 * declaring a same-named struct/enum silently collide: before this class
 * existed, the collision went undetected all the way to Codegen, where
 * LLVM's `StructType::create` refuses two distinct opaque types sharing one
 * name and renames the second to "Name.1" -- producing a crash reported as
 * "Internal compiler error: generated LLVM IR is invalid... this is a
 * compiler bug, please report it," which is both alarming and wrong: it's a
 * legitimate, avoidable naming collision, not a compiler bug. Confirmed via
 * two modules each declaring `struct Point`, imported together.
 */
class DuplicateTypeNameError : public TurfError {
public:
  DuplicateTypeNameError(SourceLocation NewLoc, const std::string &Name,
                         const std::string &Kind, SourceLocation PrevLoc)
      : TurfError(NewLoc,
                  Colors::BRIGHT_RED + "A " + Kind + " named '" +
                      Colors::CYAN + Name + Colors::BRIGHT_RED +
                      "' is already declared.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Message += "\n  " + Colors::BRIGHT_BLUE +
               "Note: struct/enum names are shared across your file and "
               "every module you import (unlike functions, which are "
               "namespaced per-module), so this name can only be used once." +
               Colors::RESET;
    Hint_ = "Rename one of the two '" + Name +
            "' declarations, or rename the import (not yet supported) to "
            "tell them apart.";
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
    Category_ = SLM::ErrorCategory::DECLARATION;
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
                  "' statement if you want it to run.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
            "and 'else') has a 'return' statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
                    "true, or did you 'return' before this block?") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
                    "'continue' that can't be reached.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
};

class StatementAfterTerminatorError : public TurfError {
public:
  StatementAfterTerminatorError(SourceLocation Loc,
                                const std::string &Terminator)
      : TurfError(Loc, Colors::BRIGHT_RED + "This code comes after a " +
                           Terminator + ", so it's impossible to reach it!" +
                           Colors::RESET,
                  "Delete this code or move it before the '" +
                  Terminator + "' statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
                    "entirely.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
                    "left over from an edit?") {
    Category_ = SLM::ErrorCategory::GENERAL;
  }
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
                    "Otherwise, add a 'break' or a proper exit condition.") {
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
            Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " +
                Colors::RESET + Colors::BRIGHT_YELLOW +
                "The loop variable '" + Colors::CYAN + VarName +
                Colors::BRIGHT_YELLOW +
                "' is never changed by this loop's 'step' or its body, so " +
                Outcome + Colors::RESET,
            "Make sure 'step' (or something in the body) actually updates '" +
                VarName +
                "'. If a DIFFERENT variable is meant to control when the "
                "loop ends, use 'while' instead of 'for' -- 'for' always "
                "iterates its own induction variable, it doesn't watch "
                "other variables for you.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
                    "'if' statement.") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
  }
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
                    "Did you mean to compare with a different variable?") {
    Category_ = SLM::ErrorCategory::GENERAL;
  }
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
                    "is undefined behavior.") {
    Category_ = SLM::ErrorCategory::ARITHMETIC;
  }
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
                    "Or did you forget to change one of the branches?") {
    Category_ = SLM::ErrorCategory::CONTROL_FLOW;
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
                  "Array indices start at 0 and go up to size - 1.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
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
                  "declared array size.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
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
                  "like arr[0] or arr[i] where i is an int.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
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
            "element type.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};

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

// Use-before-assignment error
class DefiniteAssignmentError : public TurfError {
public:
  DefiniteAssignmentError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Oops! You're using '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "' before it has a value." + Colors::RESET,
                  "Make sure every path assigns to '" + Name + "' before you use it.") {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
  }
};

// Phase 3: Module search error
class ModuleNotFoundError : public TurfError {
public:
  ModuleNotFoundError(SourceLocation Loc, const std::string &ModuleName)
      : TurfError(Loc, Colors::BRIGHT_RED + "Could not find module '" +
                           Colors::CYAN + ModuleName + Colors::BRIGHT_RED +
                           "'.\n" + Colors::RESET,
                  "Make sure the file '" + ModuleName + ".tr' exists in the standard library or your current directory.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

// A single hop in a detected import cycle: which module, and the location
// of the `import` statement that pulled it onto the in-progress stack.
struct ImportChainEntry {
  std::string ModuleName;
  SourceLocation Loc;
};

// Raised when ParseFile() re-enters a module that is still being parsed
// (present on the Parser's in-progress import stack) -- a genuine cycle
// (A imports B imports ... imports A), not the ordinary "already fully
// parsed, reuse it" case (which returns an empty body silently and is by
// design, see ParseFile()'s ImportedFiles set). Before this class existed, a
// cycle didn't error or hang -- it silently truncated the re-imported side
// to an empty module body, which is a confusing, hard-to-diagnose class of
// bug all on its own (missing symbols with no explanation), not just a
// missing diagnostic.
class CircularImportError : public TurfError {
public:
  CircularImportError(SourceLocation Loc, const std::string &ModuleName,
                      const std::vector<ImportChainEntry> &Chain)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Circular import detected -- these modules are "
                           "waiting on each other." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;

    // Render as a simple linear chain (A --imports--> B --imports--> ...
    // --imports--> A) rather than true box-drawing: a 2-3 module cycle is
    // the overwhelmingly common case and reads perfectly clearly this way,
    // and unlike a box diagram this degrades gracefully to any chain length
    // without needing terminal-width-aware layout logic.
    std::string Diagram = "\n\n    ";
    for (const auto &Entry : Chain) {
      Diagram += Colors::CYAN + Entry.ModuleName + Colors::RESET +
                 Colors::BRIGHT_BLACK + "  --imports-->  " + Colors::RESET;
    }
    Diagram += Colors::CYAN + ModuleName + Colors::RESET + Colors::BRIGHT_RED +
               "  <- back to the start!" + Colors::RESET;
    Message += Diagram;

    Hint_ = "Break the cycle by removing one of these imports, or moving "
            "the code both sides actually need into a third module that "
            "neither one has to import the other to reach.";

    for (const auto &Entry : Chain)
      RelatedLocations_.push_back({Entry.Loc, "imports '" + Entry.ModuleName + "' here"});
  }
};

// Raised by Resolver::visitScopeAccess/visitFuncCall when the left-hand
// name of a `Left::Right` expression isn't a known enum OR a known module
// -- the two things `::` can currently name. Mirrors ModuleNotFoundError's
// category choice (DECLARATION): this is fundamentally the same shape of
// problem (an unresolved name), just for the newer, more general `::`
// operator rather than specifically `import`.
class UnknownScopeQualifierError : public TurfError {
public:
  UnknownScopeQualifierError(SourceLocation Loc, const std::string &LeftName,
                             const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + LeftName +
                           Colors::BRIGHT_RED +
                           "' isn't a known enum, struct, or module." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = suggestionsOrFallback(
        Suggestions,
        "'::' only works after an enum name (Color::Red), an imported "
        "module name (io::printline), or a struct type name for a static "
        "method (StructName::method). Double-check the spelling, or make "
        "sure you've imported the module.",
        "Did you mean:\n");
  }
};

// Raised when '::' -- reserved for namespace-resolution (enum/module/
// struct-type qualifiers) -- is used with a base that names a real
// declared VARIABLE (an instance) instead. The natural counterpart to
// DotOperatorOnNonInstanceError ('.' misused on a non-instance): this is
// '::' misused ON an instance. Deliberately a separate, more specific
// error than the generic UnknownScopeQualifierError fallback -- "isn't a
// known enum, struct, or module" is technically true but unhelpful here,
// since the real, nameable mistake is "you have an instance; you meant
// '.', not '::'".
class ScopeOperatorOnInstanceError : public TurfError {
public:
  ScopeOperatorOnInstanceError(SourceLocation Loc, const std::string &Name,
                               const std::string &RightName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + Name +
                           Colors::BRIGHT_RED +
                           "' is a variable, not a type or module -- '::' "
                           "doesn't apply to it." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'::' is for namespace-qualified access (enum variants, module "
            "functions, static methods) -- '" + Name + "' holds an "
            "instance. Did you mean '" + Name + "." + RightName + "(...)'?";
  }
};

// Raised when `import module::symbol;` / `module::[a, b]` / `module::[*]`
// names a symbol that doesn't actually exist in that module (as either a
// function or a struct/enum type) -- the on-demand counterpart to
// ModuleNotFoundError (which is about the MODULE not existing; this is
// about the module existing but not having what was asked for).
class UnknownModuleSymbolError : public TurfError {
public:
  UnknownModuleSymbolError(SourceLocation Loc, const std::string &ModuleName,
                           const std::string &SymbolName,
                           const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "Module '" + Colors::CYAN +
                           ModuleName + Colors::BRIGHT_RED +
                           "' has nothing named '" + Colors::CYAN + SymbolName +
                           Colors::BRIGHT_RED + "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = suggestionsOrFallback(
        Suggestions, "Double-check the spelling, or check " + ModuleName +
                         ".tr for its actual exported names.",
        "Did you mean:\n");
  }
};

// Raised when two on-demand imports (import module::[...];) in the SAME
// file/context bring in two DIFFERENT symbols under the same bare name --
// exactly the ambiguity the on-demand import feature was designed to catch
// explicitly rather than silently letting one shadow the other. Distinct
// from DuplicateDeclarationError/DuplicateTypeNameError (which are about
// two actual DECLARATIONS colliding): here neither symbol is declared
// twice, they're both real, singly-declared symbols that just happen to
// share a bare name once flattened into the same importing file.
class ImportSymbolCollisionError : public TurfError {
public:
  ImportSymbolCollisionError(SourceLocation Loc, const std::string &SymbolName,
                             const std::string &FirstModule,
                             const std::string &SecondModule,
                             SourceLocation FirstImportLoc)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + SymbolName +
                           Colors::BRIGHT_RED +
                           "' is imported from two different modules -- '" +
                           Colors::CYAN + FirstModule + Colors::BRIGHT_RED +
                           "' and '" + Colors::CYAN + SecondModule +
                           Colors::BRIGHT_RED + "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "Either qualify one of them explicitly (`" + FirstModule +
            "::" + SymbolName + "` or `" + SecondModule + "::" + SymbolName +
            "`), or switch one of the wildcard/list imports to a more "
            "targeted one that leaves '" + SymbolName + "' out.";
    RelatedLocations_.push_back({FirstImportLoc, "'" + SymbolName + "' first imported from '" + FirstModule + "' here"});
  }
};

// Raised when a struct/enum type name is real (declared SOMEWHERE in the
// program) but not visible UNQUALIFIED from the current file/module: it
// isn't declared in this same file/module, and it was never brought in via
// an on-demand import (`import Module::Type;` / `import Module::[Type,
// ...];` / `import Module::[*];`). Bare visibility for types now follows
// the exact same rule bare function-call visibility already did -- a plain
// `import Module;` grants QUALIFIED access (Module::Type) but not bare
// access, matching import.tr's documented on-demand semantics.
class UnqualifiedTypeNotVisibleError : public TurfError {
public:
  UnqualifiedTypeNotVisibleError(SourceLocation Loc, const std::string &TypeName,
                                 const std::string &OwningModule)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + TypeName +
                           Colors::BRIGHT_RED +
                           "' isn't visible here without qualification." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'" + TypeName + "' is declared in module '" + OwningModule +
            "'. Either use it qualified (`" + OwningModule + "::" + TypeName +
            "`), or bring it into scope with `import " + OwningModule +
            "::" + TypeName + ";`.";
  }
};

// Raised when '.' -- reserved for instance-member access -- is used on a
// base that names a MODULE (`io.printline(...)`) or a STRUCT TYPE used
// bare, i.e. a static-style call with no instance (`StructName.method(...)`)
// -- instead of the required '::' (`io::printline(...)` /
// `StructName::method(...)`).
//
// Before this check existed, both cases had their own separate, fully
// functional legacy resolution paths (VariableExprAST::ResolvedModuleName
// for the module case, ::IsStructName for the struct case) that silently
// let '.' work as an undocumented ALIAS for '::' -- directly contradicting
// this language's stated, otherwise-consistent design ("'.' is instance-
// member-only, '::' is the general namespace-resolution operator", see
// docs/architecture/modules_and_scope_resolution.md section 1.1).
// Confirmed live: `io.printline(...)` and a `::`-declared static method
// called as `StructName.method(...)` both compiled and ran successfully
// before this fix, identically to their correct `::` counterparts.
class DotOperatorOnNonInstanceError : public TurfError {
public:
  DotOperatorOnNonInstanceError(SourceLocation Loc, const std::string &Name,
                                const std::string &Kind,
                                const std::string &MethodName)
      : TurfError(Loc, Colors::BRIGHT_RED + "'.' can't be used here -- '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED +
                           "' is a " + Kind + ", not an instance." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "'.' is for instance-member access only. Use '" + Name + "::" +
            MethodName + "(...)' instead.";
  }
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
    // Left as the default GENERAL (a judgment call): could arguably be
    // UNDECLARED_VAR, but the ranked suggestions already do the heavy
    // lifting here, and classifyError()'s text match gets a fair shot too.
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
                  "Check the argument types and count. Use the "
                  "correct overload, e.g. 'math.abs(int)' or "
                  "'math.abs(double)'.") {
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

#endif
