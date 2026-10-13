#ifndef ERRORS_H
#define ERRORS_H

#include "analysis/Algorithms.h"
#include "sema/Builtins.h"
#include "diagnostics/Colors.h"
#include "diagnostics/ErrorCategory.h"
#include "diagnostics/ErrorCodes.h"
#include "frontend/Lexer.h"
#include "diagnostics/SuggestionIndex.h"
#include "slm/SLMIntegration.h"
#include "sema/Types.h" // for RankedSuggestions, used by several diagnostics below
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
  // See withCode(): a per-instance code that wins over the class-implied one.
  const char *CodeOverride_ = nullptr;
  // SLM-001 §16.3 items 1/3: structured facts for the SLM pipeline (a
  // deterministic rename candidate, known-but-not-visible-in-source types).
  // Empty by default -- see DiagnosticHints's own doc comment
  // (SLMIntegration.h) for what each field means and who populates it.
  SLM::DiagnosticHints Hints_;

  static bool isIdentChar(char C) {
    return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
  }
  // True when `Other` is in the same file as this diagnostic (an empty
  // Filename means "the file being compiled", as everywhere in SourceLocation).
  // A fix-it's columns are only meaningful against the text of the file the
  // diagnostic is in -- see withTokenFixIt().
  bool sameFileAsError(const SourceLocation &Other) const {
    return Other.Filename.empty() || Loc.Filename.empty() ||
           Other.Filename == Loc.Filename;
  }

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
    if (CodeOverride_)
      return CodeOverride_;
    const ErrorCodeEntry *Entry =
        ErrorCodes::lookupByClassName(demangleTypeName(typeid(*this).name()));
    return Entry ? Entry->Code : "";
  }

  // A code that is NOT the one the C++ class implies. A code is looked up by
  // class (above), which is exactly right for a class that means one mistake
  // -- and wrong for one that is a vehicle for many: SyntaxError carries
  // every "the shape of this code is wrong" message, and a beginner needs
  // the code for WHICH shape (a missing ')' is not a missing type). Such a
  // class takes its ErrCode:: (ErrorCodes.h) in its constructor and records it
  // here. Must name a row of AllErrorCodes. (issue #263)
  TurfError &withCode(const char *Code) {
    CodeOverride_ = Code;
    return *this;
  }

  // The category follows the code when the code is the caller's (withCode()):
  // a TypeError that is really a wrong argument count is a FUNCTION_SIGNATURE
  // problem to the SLM layer, whatever the class's constructor defaulted to.
  TurfError &setCategory(SLM::ErrorCategory C) {
    Category_ = C;
    return *this;
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

  // SLM_IMPROVEMENT_PLAN C4: the declaration of the struct this diagnostic is
  // about (DiagnosticHints::ReceiverDecl). Chainable.
  // The built-in construct the diagnostic is about (DiagnosticHints::
  // Construct): which documentation page the --smart model is given.
  TurfError &withConstruct(const std::string &Construct) {
    Hints_.Construct = Construct;
    return *this;
  }

  TurfError &withReceiverDecl(const std::string &Decl) {
    Hints_.ReceiverDecl = Decl;
    return *this;
  }

  // ── Fix-its (SLM-001 item 1, DiagnosticHints::FixIts) ──────────────────────
  //
  // A fix-it is an exact edit the raise site already knows, applied by --smart
  // without asking the model (SLM::tryDeterministicFix()). Attach one ONLY
  // where this site holds the whole edit -- the hint says what to write AND the
  // site knows where. Never one the writer might not have meant: a hint that
  // offers two fixes ("remove `pub`, or move the declaration"), or one that
  // only gives an example ("write the instantiation you mean, e.g. `Box<int>`"),
  // is advice, and --smart's model answers it. The sandbox re-checks every
  // fix-it, so a wrong one cannot be applied, but a wrong one that happens to
  // compile would still be shown to a beginner as "the compiler's own fix".
  //
  // Every builder below checks the source line before it adds anything, and
  // adds nothing if the text is not what this site expects, so a stale or
  // mis-pointed Loc degrades to "no fix-it", never to a bad edit.

  // The raw edit: replace [StartCol, EndCol) of `Line` (see FixIt).
  TurfError &withFixIt(int Line, int StartCol, int EndCol,
                       const std::string &Replacement) {
    Hints_.FixIts.push_back({Line, StartCol, EndCol, Replacement});
    return *this;
  }

  // Why the fix is right (DiagnosticHints::FixReason), in a sentence.
  TurfError &withFixReason(const std::string &Reason) {
    Hints_.FixReason = Reason;
    return *this;
  }

  // Which neighbouring blanks a deleted token takes with it, so the edit leaves
  // `fn f()` and not `  fn f()` / `f ()`.
  enum class EatBlanks { None, Before, After };

  // Replace (or, with an empty Replacement, delete) the token `Expected` that
  // starts exactly at `At`. Does nothing unless the line really reads
  // `Expected` there, in the same file as this diagnostic.
  TurfError &withTokenFixIt(const SourceLocation &At,
                            const std::string &Expected,
                            const std::string &Replacement,
                            EatBlanks Eat = EatBlanks::None) {
    const std::string *Line = FixItSourceLine(At);
    if (!Line || At.Col < 1 || !sameFileAsError(At) ||
        Line->compare(At.Col - 1, Expected.size(), Expected) != 0)
      return *this;
    size_t Start = At.Col - 1, End = Start + Expected.size();
    if (Eat == EatBlanks::After)
      while (End < Line->size() && ((*Line)[End] == ' ' || (*Line)[End] == '\t'))
        ++End;
    if (Eat == EatBlanks::Before)
      while (Start > 0 && ((*Line)[Start - 1] == ' ' || (*Line)[Start - 1] == '\t'))
        --Start;
    return withFixIt(At.Line, static_cast<int>(Start) + 1,
                     static_cast<int>(End) + 1, Replacement);
  }

  // Insert `Text` right after the token `Token` that starts exactly at `At`
  // (`fn` -> `fn<T>`). Same check as withTokenFixIt().
  TurfError &withInsertAfterTokenFixIt(const SourceLocation &At,
                                       const std::string &Token,
                                       const std::string &Text) {
    const std::string *Line = FixItSourceLine(At);
    if (!Line || At.Col < 1 || !sameFileAsError(At) ||
        Line->compare(At.Col - 1, Token.size(), Token) != 0)
      return *this;
    const int Col = At.Col + static_cast<int>(Token.size());
    return withFixIt(At.Line, Col, Col, Text);
  }

  // Delete the whole word `Word` (with the blanks before it) from the
  // diagnostic's own line -- only when it appears there EXACTLY ONCE. For a
  // keyword the raise site knows is on the line but never recorded the column
  // of (`static` in `fn S.new() static: void`): a second occurrence (a comment
  // mentioning it) means this site cannot tell which one it meant, and adds
  // nothing.
  TurfError &withWordDeletionFixIt(const std::string &Word,
                                   const std::string &Reason) {
    const std::string *Line = FixItSourceLine(Loc);
    if (!Line || Word.empty())
      return *this;
    size_t Found = std::string::npos;
    int Count = 0;
    for (size_t Pos = Line->find(Word); Pos != std::string::npos;
         Pos = Line->find(Word, Pos + Word.size())) {
      const size_t After = Pos + Word.size();
      if ((Pos == 0 || !isIdentChar((*Line)[Pos - 1])) &&
          (After >= Line->size() || !isIdentChar((*Line)[After]))) {
        Found = Pos;
        ++Count;
      }
    }
    if (Count != 1)
      return *this;
    withTokenFixIt({Loc.Line, static_cast<int>(Found) + 1, Loc.Filename}, Word,
                   "", EatBlanks::Before);
    return withFixReason(Reason);
  }

  // Rename: replace EVERY whole-word `OldName` on the diagnostic's own line with
  // `NewName` (a name that fails to resolve usually fails at each use on the
  // line, and fixing one use would leave the same error on the next). A match
  // must not touch an identifier character on either side, so `cnt` never
  // rewrites the inside of `recnt`. Adds nothing if `OldName` is not on the line
  // as a whole word. `Reason` (optional) explains a fix that is not a typo;
  // empty means "a typo", which is the sentence used.
  TurfError &withDeterministicFix(const std::string &OldName,
                                  const std::string &NewName,
                                  const std::string &Reason = "") {
    const std::string *Line = FixItSourceLine(Loc);
    if (!Line || OldName.empty())
      return *this;
    bool Any = false;
    for (size_t Pos = Line->find(OldName); Pos != std::string::npos;
         Pos = Line->find(OldName, Pos + OldName.size())) {
      const size_t After = Pos + OldName.size();
      const bool LeftOk = Pos == 0 || !isIdentChar((*Line)[Pos - 1]);
      const bool RightOk = After >= Line->size() || !isIdentChar((*Line)[After]);
      if (!LeftOk || !RightOk)
        continue;
      withFixIt(Loc.Line, static_cast<int>(Pos) + 1, static_cast<int>(After) + 1,
                NewName);
      Any = true;
    }
    if (Any)
      Hints_.FixReason =
          Reason.empty()
              ? "'" + OldName + "' isn't declared anywhere visible, but '" +
                    NewName + "' is an exact, unambiguous match nearby, so "
                    "this is almost certainly a typo."
              : Reason;
    return *this;
  }

  // Convenience for the common "unknown name" shape: every RankedSuggestions-
  // based error class (UnknownFunctionError, ReferenceError, UnknownStructFieldError,
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
        !OldName.empty() && Suggestions.Local[0] != OldName)
      withDeterministicFix(OldName, Suggestions.Local[0]);
    return *this;
  }

  void raise() const {
    DiagnosticEngine::add(Loc, Message, Hint_, /*IsWarning=*/false,
                          RelatedLocations_, Category_, getCode(), Hints_);

    if (g_recoverActive) {
      // Unwinds to the innermost recovery point (RecoverUnwind, Lexer.h),
      // destroying this error object, the temporaries that built it and
      // every abandoned frame's locals on the way.
      throw RecoverUnwind{};
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
// `#include "diagnostics/Errors.h"` still pulls in every diagnostic (existing call
// sites are unaffected), while a file that only needs one category can
// include that header directly instead.
#include "diagnostics/ErrorsLex.h"
#include "diagnostics/ErrorsType.h"
#include "diagnostics/ErrorsScope.h"
#include "diagnostics/ErrorsStruct.h"
#include "diagnostics/ErrorsModule.h"
#include "diagnostics/ErrorsFlow.h"
#include "diagnostics/ErrorsArray.h"
#include "diagnostics/ErrorsMemory.h"
#include "diagnostics/ErrorsPattern.h"
#include "diagnostics/ErrorsPayload.h"

#endif
