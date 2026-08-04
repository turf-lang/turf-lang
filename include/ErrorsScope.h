#ifndef ERRORS_SCOPE_H
#define ERRORS_SCOPE_H

#include "Errors.h"

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
      "functions like 'printline' or 'typeof'.");
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
    Hint_ = "If you want to change its value, just do `" + Name +
            " = new_value;` without the type (like 'int' or 'string'). If "
            "you want a new variable, give it a different name!";
    // Renders as "Note: first declared here (line X, col Y)" -- printDiag()
    // (Lexer.cpp) now shows every RelatedLocation, so this used to also be
    // hand-embedded directly into Message with its own wording ("Note: You
    // first created it at line X, column Y") before that rendering existed;
    // removed here to avoid printing the same location twice.
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
                    TurfWarning::WarningLabel() + "Just so you know, you're creating a new '" +
                        Colors::CYAN + Name + Colors::BRIGHT_YELLOW +
                        "' that hides an older one!" + Colors::RESET,
                    "It's better to give this new variable a different name "
                    "to avoid confusion later.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    // Renders as "Note: originally declared here (line X, col Y)" -- see
    // DuplicateDeclarationError's matching comment for why this is no
    // longer also hand-embedded into Message directly.
    RelatedLocations_.push_back({PrevLoc, "originally declared here"});
  }
};

#endif
