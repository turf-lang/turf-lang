#ifndef ERRORS_H
#define ERRORS_H

#include "Algorithms.h"
#include "Builtins.h"
#include "Colors.h"
#include "ErrorCategory.h"
#include "ErrorCodes.h"
#include "Lexer.h"
#include "SuggestionIndex.h"
#include "SLMIntegration.h"
#include "Types.h" // for RankedSuggestions, used by several diagnostics below
#include <cstdlib>
#include <iostream>
#include <map>

#include <string>
#include <typeinfo>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif

struct SourceLocation;

// Demangles a typeid(...).name() string into a plain C++ class name (e.g.
// "18ArrayBoundsError" -> "ArrayBoundsError"). GCC/Clang only -- the only
// toolchains this project builds with (see AGENTS.md) -- via the Itanium
// ABI's __cxa_demangle. Falls back to the raw mangled name on any other
// toolchain or if demangling fails, which just means ErrorCodes::
// lookupByClassName() below won't find a match (getCode() degrades to "").
inline std::string demangleTypeName(const char *MangledName) {
#if defined(__GNUC__) || defined(__clang__)
  int Status = 0;
  char *Demangled = abi::__cxa_demangle(MangledName, nullptr, nullptr, &Status);
  if (Status == 0 && Demangled) {
    std::string Result(Demangled);
    free(Demangled);
    return Result;
  }
#endif
  return MangledName;
}

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
  // the final rendered message text (formerly `SLM::classifyError()`, DELETED
  // in DIAG-001), which silently drifted out of sync every time a message
  // was reworded and left several real, commonly-hit error classes
  // (ImmutableAssignmentError, NullAssignmentError, DefiniteAssignmentError,
  // UseAfterFreeError, DoubleFreeError) permanently misclassified as GENERAL
  // -- confirmed live, several of classifyError()'s own LogErrorAt-targeted
  // substring patterns no longer matched ANY current message at all (the
  // exact drift this comment used to warn about, caught only once it was
  // actually verified: see docs/development/changes/DIAG-001.md). Every
  // subclass here is individually audited (a class left at the GENERAL
  // default is a deliberate judgment call, documented at that class -- see
  // e.g. SemanticError, ModuleMethodNotFoundError); the raw, untyped
  // LogErrorAt() path (Lexer.h) takes an explicit SLM::ErrorCategory
  // argument at every call site instead of falling back to text matching.
  SLM::ErrorCategory Category_ = SLM::ErrorCategory::GENERAL;
  // SLM-001 §16.3 items 1/3: structured facts for the SLM pipeline (a
  // deterministic rename candidate, known-but-not-visible-in-source types).
  // Empty by default -- see DiagnosticHints's own doc comment
  // (SLMIntegration.h) for what each field means and who populates it.
  SLM::DiagnosticHints Hints_;

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

  // Stable "E0NN" diagnostic code for this error, looked up from
  // ErrorCodes.h's registry by the ACTUAL derived class -- typeid(*this)
  // resolves polymorphically (TurfError has a virtual destructor) as long as
  // this runs on a fully-constructed object, which raise()/warn() always do
  // (never called from inside a constructor). Returns "" for any subclass
  // not yet in the registry, e.g. one added after ErrorCodes.h was last
  // regenerated -- callers must treat that as "no code", not an error.
  std::string getCode() const {
    const ErrorCodeEntry *Entry =
        ErrorCodes::lookupByClassName(demangleTypeName(typeid(*this).name()));
    return Entry ? Entry->Code : "";
  }

  // Chainable: TurfError(loc, msg).withRelated(declLoc, "declared here").raise();
  TurfError &withRelated(SourceLocation RelLoc, const std::string &Desc) {
    RelatedLocations_.push_back({RelLoc, Desc});
    return *this;
  }

  // SLM-001 item 3: attach a known-type fact not visible in the pinned
  // source lines (see DiagnosticHints::KnownTypes). Chainable, same shape as
  // withRelated().
  TurfError &withKnownType(const std::string &Fact) {
    Hints_.KnownTypes.push_back(Fact);
    return *this;
  }

  // SLM-001 item 1: record an unambiguous rename candidate directly, for a
  // call site that already knows both the wrong and right spelling without
  // going through a RankedSuggestions lookup at all.
  TurfError &withDeterministicFix(const std::string &OldName,
                                  const std::string &NewName) {
    Hints_.DeterministicOldName = OldName;
    Hints_.DeterministicNewName = NewName;
    return *this;
  }

  // Convenience for the common "unknown name" shape: every RankedSuggestions-
  // based error class (LookupError, ReferenceError, UnknownStructFieldError,
  // ...) calls this right after computing its Hint_, passing the exact name
  // that failed to resolve. Sets a deterministic-fix candidate only when the
  // lookup was genuinely UNAMBIGUOUS -- exactly one local candidate and zero
  // module candidates -- so a genuinely ambiguous typo (multiple equally
  // plausible corrections) is deliberately left to the model/user instead of
  // guessing. Never re-derives anything from message text: Suggestions is
  // the same structured RankedSuggestions object the constructor's own Hint_
  // was built from moments earlier.
  TurfError &trySetDeterministicRename(const std::string &OldName,
                                       const RankedSuggestions &Suggestions) {
    if (Suggestions.Local.size() == 1 && Suggestions.Module.empty() &&
        !OldName.empty() && Suggestions.Local[0] != OldName) {
      Hints_.DeterministicOldName = OldName;
      Hints_.DeterministicNewName = Suggestions.Local[0];
    }
    return *this;
  }

  void raise() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/false,
                          RelatedLocations_, Category_, getCode(), Hints_);

    if (g_recoverActive) {
      // longjmp() skips C++ destructors between here and the setjmp() point
      // -- including every live ParseRecursionGuard on the abandoned call
      // stack, which would otherwise leave g_parseRecursionDepth
      // permanently inflated by however deep parsing had gotten when this
      // error fired. Reset explicitly here (still on the deep side of the
      // jump, so this runs regardless of which of the many setjmp() call
      // sites ends up catching it) -- mirrors g_braceDepth's own defensive
      // per-pass reset in resetLexer(), just at the point that actually
      // needs it for a counter that longjmp can desync. See ParseExpression/
      // ParseBlock's recursion-guard doc comment in Lexer.h.
      g_parseRecursionDepth = 0;
      longjmp(g_recoverJmp, 1);
    } else {
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

  // No longer takes a Prefix param (e.g. "Did you mean:\n") to prepend --
  // formatRankedSuggestions() already produces a complete, self-contained
  // sentence on its own ("It may be from the current file: ..." / "Or,
  // could be one of these from the imported files: ..."), so a prefix
  // stacked in front of it either double-headed the local-suggestion case
  // ("Did you mean:\nIt may be from the current file: ...") or, when only
  // module suggestions existed with no local match at all, left the prefix
  // dangling above an unrelated "Or, could be..." sentence with nothing
  // for the "Or" to follow. Confirmed live on both call sites that used to
  // pass one (UnknownScopeQualifierError, UnknownModuleSymbolError).
  static std::string suggestionsOrFallback(const RankedSuggestions &Suggestions, const std::string &Fallback) {
    std::string formatted = formatRankedSuggestions(Suggestions);
    if (!formatted.empty()) {
      if (formatted.rfind("\n  ", 0) == 0)
        return formatted.substr(3);
      return formatted;
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

  // Mirrors TurfError::getCode() exactly (see its own doc comment) -- added
  // for HYG-001, the first warning classes (UnreachableBlockWarning,
  // DeadBranchWarning) to register a real "E0NN" code. Every other warning
  // class predates this and has no registry entry, so this still returns ""
  // for them, same as before this method existed (printDiag() only prints
  // a "[E0NN]" tag when Code is non-empty).
  std::string getCode() const {
    const ErrorCodeEntry *Entry =
        ErrorCodes::lookupByClassName(demangleTypeName(typeid(*this).name()));
    return Entry ? Entry->Code : "";
  }

  // NEVER interrupts compilation flow — always returns normally.
  void emit() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/true,
                          RelatedLocations_, Category_, getCode());
  }

  void warn() const { emit(); }

  // Shared "Warning: " label every subclass message below starts with.
  // Extracted because ~15 subclasses across ErrorsFlow.h/ErrorsScope.h
  // repeated this exact literal color-code sequence verbatim.
  static std::string WarningLabel() {
    return Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " +
           Colors::RESET + Colors::BRIGHT_YELLOW;
  }
};

// Umbrella: every diagnostic class used to live directly in this file,
// grouped by which compiler pass raises it. Split out so
// `#include "Errors.h"` still pulls in every diagnostic (existing call
// sites are unaffected), while a file that only needs one category can
// include that header directly instead.
#include "ErrorsLex.h"
#include "ErrorsType.h"
#include "ErrorsScope.h"
#include "ErrorsStruct.h"
#include "ErrorsModule.h"
#include "ErrorsFlow.h"
#include "ErrorsArray.h"
#include "ErrorsMemory.h"
#include "ErrorsPattern.h"
#include "ErrorsPayload.h"

#endif
