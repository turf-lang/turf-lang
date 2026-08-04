#ifndef ERRORS_H
#define ERRORS_H

#include "Algorithms.h"
#include "Builtins.h"
#include "Colors.h"
#include "ErrorCategory.h"
#include "Lexer.h"
#include "SuggestionIndex.h"
#include "SLMIntegration.h"
#include "Types.h" // for RankedSuggestions, used by several diagnostics below
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

  // NEVER interrupts compilation flow — always returns normally.
  void emit() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/true,
                          RelatedLocations_, Category_);
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

#endif
