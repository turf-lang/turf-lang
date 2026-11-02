#ifndef ERRORS_SCOPE_H
#define ERRORS_SCOPE_H

#include "diagnostics/Errors.h"

/// LEC-005: the hint for a TYPE name (@p TypeKind: "struct", "enum",
/// "error type", anything else = a plain "type") written where a value
/// (@p Called false: `x = Node;`) or a function (@p Called true: `Node(5)`)
/// belongs. E040/E041's fallback hint -- "create a variable like `mut Node: int
/// = 5;`" -- is advice for a misspelled variable and is actively wrong for a
/// name that IS declared, as a type; what the user wants is a value of that
/// type, and Turf has exactly one construction syntax (`T::new(...)` /
/// `T::default()`, structs_and_enums.md).
inline std::string typeUsedAsValueHint(const std::string &Name,
                                       const std::string &TypeKind,
                                       bool Called) {
  const std::string Is = "`" + Name + "` is " +
                         (TypeKind == "enum" ? "an " : "a ") + TypeKind;
  if (TypeKind == "struct")
    return "Build one with `" + Name + "::default()`, or `" + Name +
           "::new(...)` if it declares a `.new` method, for example `mut v: " +
           Name + " = " + Name + "::default();`. " +
           (Called ? "Turf has no `" + Name + "(...)` constructor-call syntax."
                   : Is + ", not a value.");
  if (TypeKind == "enum" || TypeKind == "error type")
    return "Name one of its variants, as `" + Name + "::Variant`. " + Is +
           (Called ? ", not a function." : ", not a value.");
  return std::string(Called ? "Call a function here. " : "Write a value here. ") +
         Is + (Called ? ", not a function." : ", not a value.");
}

// (LookupError, E037, was deleted in issue #263: nothing ever constructed it --
// an unknown function is UnknownFunctionError / E041 below.)
// Arithmetic Error : Math issues

class UnknownFunctionError : public TurfError {
public:
  /// LEC-005: @p TypeKind is non-empty when @p Name is a visible TYPE
  /// ("struct", "enum", "error type", "type") called like a function --
  /// `Node(5)`. The suggestions would be a typo fix for a name that is not a
  /// typo, so the hint says what the language actually offers instead.
  UnknownFunctionError(SourceLocation Loc, const std::string &Name,
                       const RankedSuggestions &Suggestions,
                       const std::string &TypeKind = "")
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot find a function named `" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "`." +
                           Colors::RESET) {
    // UNDECLARED_VAR, not BUILTIN_FUNCTION: this is about an unresolved *name*
    // (could be a typo'd user function), not a builtin's own argument-type rules.
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    if (!TypeKind.empty()) {
      Hint_ = typeUsedAsValueHint(Name, TypeKind, /*Called=*/true);
      return;
    }
    Hint_ = suggestionsOrFallback(Suggestions,
      "Check the spelling, or import the module that defines it.");
    trySetDeterministicRename(Name, Suggestions);
  }
};
/// Emitted by the Resolver's Pass 2 when a struct field's NamedType reference
/// does not match any struct registered in Pass 1.
class UnknownTypeError : public TurfError {
public:
  UnknownTypeError(SourceLocation Loc, const std::string &TypeName,
                   const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot find a type named `" +
                           Colors::CYAN + TypeName + Colors::BRIGHT_RED + "`." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = suggestionsOrFallback(Suggestions,
      "Declare `" + TypeName + "` before it is used, or check the spelling.");
    trySetDeterministicRename(TypeName, Suggestions);
  }
};
/// Emitted when the function name is known but the argument count is wrong.
/// Shows every valid overload signature so the user knows what they can write.
class WrongArityError : public TurfError {
public:
  WrongArityError(SourceLocation Loc, const std::string &Name,
                  int Got, const std::vector<std::string> &KnownSignatures)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I found no overload of `" + Colors::CYAN +
                      Name + Colors::BRIGHT_RED + "` that takes " +
                      std::to_string(Got) + " argument" +
                      (Got == 1 ? "" : "s") + "." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
    if (!KnownSignatures.empty()) {
      Message += "\n  " + Colors::BRIGHT_GREEN + "The overloads are:" +
                 Colors::RESET;
      for (const auto &Sig : KnownSignatures)
        Message += "\n    " + Colors::CYAN + Sig + Colors::RESET;
    }
    Hint_ = KnownSignatures.empty()
                ? "Call `" + Name + "` with a different number of arguments "
                  "than " + std::to_string(Got) + "."
                : "Call `" + Name + "` the way one of the overloads above is "
                  "declared, for example `" + KnownSignatures.front() + "`.";
  }
};
class NestedFunctionError : public TurfError {
public:
  NestedFunctionError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "I cannot declare `" + Colors::CYAN + Name +
                           Colors::BRIGHT_RED + "` inside another function.",
                  "Move `" + Name + "` to the top level of the file or "
                  "module.") {
    Category_ = SLM::ErrorCategory::FUNCTION_SIGNATURE;
  }
};
// Reference Error : Variable lookup issues
class ReferenceError : public TurfError {
public:
  /// LEC-005: @p TypeKind is non-empty when @p Name is a visible TYPE used as
  /// a value (`mut a: int = Node;`) -- see typeUsedAsValueHint() for why the
  /// create-a-variable fallback and a rename suggestion are both wrong there.
  ReferenceError(SourceLocation Loc, const std::string &Name,
                 const RankedSuggestions &Suggestions,
                 const std::string &TypeKind = "")
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot find anything named `" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "`.") {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    if (!TypeKind.empty()) {
      Hint_ = typeUsedAsValueHint(Name, TypeKind, /*Called=*/false);
      return;
    }
    Hint_ = suggestionsOrFallback(Suggestions,
      "Declare it before you use it, for example `mut " + Name +
      ": int = 5;`.");
    trySetDeterministicRename(Name, Suggestions);
  }
};
// Semantic Error : Symbol binding issues
//
// Deliberately NOT given a deterministic-fix rename (SLM-001 item 1), unlike
// every other RankedSuggestions-based class in this file: `Name` here is NOT
// a typo -- it is a real, correctly-spelled identifier declared LATER in the
// same scope. A rename to some OTHER visible candidate would silently
// reference the wrong variable instead of fixing the actual problem (move
// the declaration earlier), and could easily still pass sandbox validation
// (same category, source line still compiles) while being semantically
// wrong -- exactly the "compiles but changes behaviour" risk §16.4's
// semantic-correctness metric exists to catch. Left to the model/user.
class UseBeforeDeclarationError : public TurfError {
public:
  UseBeforeDeclarationError(SourceLocation UseLoc, const std::string &Name,
                            const RankedSuggestions &Suggestions)
      : TurfError(UseLoc, Colors::BRIGHT_RED + "I cannot use `" + Colors::CYAN +
                           Name + Colors::BRIGHT_RED +
                           "` before its declaration.") {
    Category_ = SLM::ErrorCategory::UNDECLARED_VAR;
    // NOT "did you forget to create this variable?" -- this variable IS
    // created, further down; the mistake is the order. (Suggestions are not
    // offered either: a rename would point at a different variable, see this
    // class's own comment above.)
    (void)Suggestions;
    Hint_ = "Move the declaration of `" + Name + "` above this line: a "
            "variable exists only from its declaration onwards.";
  }
};
class DuplicateDeclarationError : public TurfError {
public:
  DuplicateDeclarationError(SourceLocation NewLoc, const std::string &Name,
                            SourceLocation PrevLoc)
      : TurfError(NewLoc,
                  Colors::BRIGHT_RED + "I cannot declare `" + Colors::CYAN +
                      Name + Colors::BRIGHT_RED +
                      "` again, because it is already declared.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = "Write `" + Name + " = new_value;` without the type to change its "
            "value, or give the new variable a different name.";
    // Renders as "Note: first declared here (line X, col Y)" -- printDiag()
    // (Lexer.cpp) now shows every RelatedLocation, so this used to also be
    // hand-embedded directly into Message with its own wording ("Note: You
    // first created it at line X, column Y") before that rendering existed;
    // removed here to avoid printing the same location twice.
    RelatedLocations_.push_back({PrevLoc, "first declared here"});
  }
};
/**
 * @brief A struct/enum/machine/error name declared twice in the SAME file or
 *        module.
 *
 * FDN-003 narrowed this: a type is identified by (module, name), so two
 * DIFFERENT modules each declaring `struct Point` is legal (two types,
 * `a::Point` and `b::Point`); only a same-module redeclaration shares an
 * identity and reaches here. History: when type names were one program-wide
 * namespace this also fired across modules, and before THAT the collision
 * went undetected until Codegen, where LLVM renamed the second struct
 * "Point.1" and code emitted against "Point" used the wrong layout
 * ("Internal compiler error: generated LLVM IR is invalid").
 */
class DuplicateTypeNameError : public TurfError {
public:
  // "enum"/"error" need "An", "struct" needs "A" -- every Kind this is
  // called with (ResolverCore.cpp) starts with either a vowel or 's', so a
  // plain vowel check is sufficient (no need for a full English-article
  // table).
  static bool startsWithVowel(const std::string &S) {
    return !S.empty() && std::string("aeiouAEIOU").find(S[0]) != std::string::npos;
  }

  DuplicateTypeNameError(SourceLocation NewLoc, const std::string &Name,
                         const std::string &Kind, SourceLocation PrevLoc)
      : TurfError(NewLoc,
                  Colors::BRIGHT_RED + "I found a second " + Kind +
                      " named `" + Colors::CYAN + Name +
                      Colors::BRIGHT_RED + "`.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    // FDN-003: only a SAME-module duplicate reaches here -- two modules
    // may each declare a type of the same name (type identity is
    // (module, name)), so the old "shared across every module you import"
    // note no longer describes the rule.
    Message += "\n  " + Colors::BRIGHT_BLUE +
               "A type name can be declared only once per file; another file "
               "or module may reuse it." +
               Colors::RESET;
    Hint_ = "Rename one of the two `" + TypeIdentity::bareName(Name) +
            "` declarations.";
    RelatedLocations_.push_back({PrevLoc, "first declared here"});
  }
};
class ShadowingWarning : public TurfWarning {
public:
  ShadowingWarning(SourceLocation NewLoc, const std::string &Name,
                   SourceLocation PrevLoc)
      : TurfWarning(NewLoc,
                    TurfWarning::WarningLabel() + "This new `" +
                        Colors::CYAN + Name + Colors::BRIGHT_YELLOW +
                        "` hides an older one." + Colors::RESET,
                    "Give the new variable a different name, so the older one "
                    "stays reachable. To silence this on purpose, pass "
                    "`--allow E152`.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
    // Renders as "Note: originally declared here (line X, col Y)" -- see
    // DuplicateDeclarationError's matching comment for why this is no
    // longer also hand-embedded into Message directly.
    RelatedLocations_.push_back({PrevLoc, "originally declared here"});
  }
};
// `EnumName::NoSuchVariant` / `ErrorTypeName::NoSuchVariant` -- the
// enum/error type itself is real, but the named variant isn't one of its
// declared variants. Covers both kinds (Resolver::visitScopeAccess's
// EnumVariant and ErrorVariantRef branches) via a shared Kind string
// ("enum"/"error type") rather than two near-identical classes -- both
// used to be raised via a raw LogErrorAt() call with the suggestion list
// hand-formatted directly into the message body: no color at all (every
// other diagnostic in this file is wrapped in Colors::BRIGHT_RED, so this
// one visibly stood out as plain white text) and no Hint_, just an
// inline "\nDid you mean:\n    X\n" appended to the message instead of the
// suggestionsOrFallback()-based Hint every comparable "unknown name" error
// in this file already uses.
class NoSuchVariantError : public TurfError {
public:
  NoSuchVariantError(SourceLocation Loc, const std::string &Kind,
                     const std::string &TypeName, const std::string &VariantName,
                     const RankedSuggestions &Suggestions)
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot find a variant `" +
                           Colors::CYAN + VariantName + Colors::BRIGHT_RED +
                           (Kind == "enum" ? "` in the enum `" : "` in the error type `") +
                           Colors::CYAN + TypeName + Colors::BRIGHT_RED + "`." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    Hint_ = suggestionsOrFallback(Suggestions,
      "Check the spelling against the variants declared in `" + TypeName + "`.");
    trySetDeterministicRename(VariantName, Suggestions);
  }
};

// `.transition(X)` where X isn't a state of the SAME machine as the
// receiver -- either X belongs to a completely different `machine` type, or
// isn't a state/variant access at all (e.g. `conn.transition(5)`).
// Deliberately NOT reusing NoSuchVariantError: that class covers "the type
// is right but the variant NAME doesn't exist", which for a `machine`
// receiver is already caught earlier and more precisely by
// Resolver::visitScopeAccess()'s own NoSuchVariantError (e.g.
// `conn.transition(ConnectionState::Bogus)` never reaches TypeChecker's
// machine-transition check at all -- it fails to resolve the scope access
// itself first). This class covers the type-level mismatch that check can't
// see: a real state, just not one of THIS machine's.
class InvalidTransitionTargetError : public TurfError {
public:
  InvalidTransitionTargetError(SourceLocation Loc, const std::string &MachineName,
                               const std::string &GotTypeName)
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot `" + Colors::CYAN + "transition" +
                           Colors::BRIGHT_RED + "` to `" + Colors::CYAN +
                           GotTypeName + Colors::BRIGHT_RED +
                           "`, which is not a state of `" + Colors::CYAN +
                           MachineName + Colors::BRIGHT_RED + "`." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
    Hint_ = "Pass a state of `" + MachineName + "`, such as `" + MachineName +
           "::SomeState`, to `transition()`.";
  }

  /// The target has the receiver's machine type but is a value (a variable,
  /// a call) rather than a written `Machine::State`, so its edge is unknown.
  struct NotWrittenAsState {};
  InvalidTransitionTargetError(SourceLocation Loc, const std::string &MachineName,
                               NotWrittenAsState)
      : TurfError(Loc, Colors::BRIGHT_RED + "I cannot tell which state this holds, and `" +
                           Colors::CYAN + "transition" + Colors::BRIGHT_RED +
                           "` needs a state written as `" + Colors::CYAN +
                           MachineName + "::SomeState" + Colors::BRIGHT_RED +
                           "`." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
    Hint_ = "Write the state to move to. To move to a state chosen at run "
            "time, `compare` the value that chooses it and write one "
            "transition per arm.";
  }
};

// `.transition(X)` called inside a `compare`/`matches` arm that has narrowed
// the receiver to one specific state (TypeChecker::MachineNarrowStack_), where
// the declared machine has no edge from that narrowed state to X. Distinct
// from the runtime panic message Codegen builds for the UNNARROWED case
// (CodegenStruct.cpp) -- this is a real compile-time error with a caret,
// since the narrowed state is statically known, not just possible.
class InvalidTransitionError : public TurfError {
public:
  InvalidTransitionError(SourceLocation Loc, const std::string &MachineName,
                         const std::string &FromState, const std::string &ToState)
      : TurfError(Loc, Colors::BRIGHT_RED + "`" + Colors::CYAN + MachineName +
                           Colors::BRIGHT_RED + "` has no transition from `" +
                           Colors::CYAN + FromState + Colors::BRIGHT_RED +
                           "` to `" + Colors::CYAN + ToState + Colors::BRIGHT_RED +
                           "`." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::TYPE_MISMATCH;
    Hint_ = "Add `" + FromState + " -> " + ToState +
           ",` to the declaration of `" + MachineName +
           "`, or transition to a state that `" + FromState +
           "` has an edge to.";
  }
};

// A declared machine state with zero INCOMING edges -- nothing in the
// machine's own declaration ever transitions INTO it, so (short of an
// external `.transition()` call constructing it out of thin air via some
// other machine's state, which can't happen -- InvalidTransitionTargetError
// already rejects cross-machine targets) it can only ever be reached by
// being the variable's OWN initial value, never arrived at mid-flow.
// Deliberately a warning, not an error, and its hint says so explicitly:
// this is the expected, correct shape for a machine's legitimate start
// state (e.g. `Idle` in a connection-state machine) -- v1 has no separate
// syntax to mark a state as "the" entry point, so this warning has a known,
// intentional false-positive on every well-designed machine's start state.
class UnreachableStateWarning : public TurfWarning {
public:
  UnreachableStateWarning(SourceLocation Loc, const std::string &MachineName,
                          const std::string &StateName)
      : TurfWarning(Loc,
                    TurfWarning::WarningLabel() + "Machine `" +
                        Colors::CYAN + MachineName + Colors::BRIGHT_YELLOW +
                        "` declares state `" + Colors::CYAN + StateName +
                        Colors::BRIGHT_YELLOW +
                        "` with no incoming transitions." + Colors::RESET,
                    "Add a `-> " + StateName + "` edge if the machine can "
                    "reach `" + StateName + "` after it starts. If `" +
                    StateName + "` is the starting state of `" + MachineName +
                    "`, pass `--allow E153` to silence this.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

// AN-7 (TDP-2-007, issue #334): dead code (analysis/UnusedDecls.cpp).
class UnusedPrivateFunctionWarning : public TurfWarning {
public:
  UnusedPrivateFunctionWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc, "Nothing calls or names the function `" + Name + "`.",
                    "Remove `" + Name + "`, or call it where it was meant to be "
                    "used. Mark it `pub` if another module is meant to call it.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

class UnusedImportWarning : public TurfWarning {
public:
  UnusedImportWarning(SourceLocation Loc, const std::string &Module)
      : TurfWarning(Loc, "Nothing in this file uses the import of `" + Module + "`.",
                    "Remove the `import` line.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

class UnusedPubInBinaryWarning : public TurfWarning {
public:
  UnusedPubInBinaryWarning(SourceLocation Loc, const std::string &Name)
      : TurfWarning(Loc, "Nothing calls the `pub` function `" + Name +
                             "`, and no other module can: this file is the program's root.",
                    "Remove `" + Name + "`, or call it from `main`. `pub` has no "
                    "effect in a program's root file.") {
    Category_ = SLM::ErrorCategory::DECLARATION;
  }
};

#endif
