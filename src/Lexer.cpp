/**
 * @file Lexer.cpp
 * @brief Tokenization and lexical analysis for TurfLang.
 * @details Reads the source file character by character and groups them into
 *          meaningful tokens (identifiers, literals, operators, etc.). Also
 *          provides the DiagnosticEngine for error reporting and SLM
 * integration.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Lexer.h"
#include "Colors.h"
#include "ErrorCodes.h"
#include "ErrorsLex.h" // MalformedRadixPrefixError and friends (SEM-001) -- see
                       // ErrorsLex.h's own top comment for why Lexer.cpp,
                       // alone among the lexer/parser split, includes a
                       // TurfError header directly
#include "Parser.h"    // OVH-012: IsLibStdWarningLocation() -- flushAll()'s
                       // system-module-warning gate
#include "SLMIntegration.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

std::vector<std::string> SourceLines;
std::map<std::string, std::vector<std::string>> AllSourceLines;
std::string SourceFilename;
// The ORIGINAL top-level file passed to the compiler, set once in main.cpp
// and never touched again (unlike SourceFilename, which is repointed at
// each imported module while it's being parsed, then restored). Used only
// by flushAll() to print the main file's batch of diagnostics before the
// imported modules' --
// comparing against SourceFilename wouldn't work reliably since by the time
// flushAll() runs (after every pass has completed) it's already back to
// this same value for every diagnostic, imported-module ones included.
std::string MainSourceFilename;
SourceLocation CurLoc = {1, 0};
int CurLine = 1;
int CurCol = 0;
int PrevTokEndLine = -1;
int PrevTokEndCol = -1;
static int LastChar = ' ';

const std::string *FixItSourceLine(const SourceLocation &Loc) {
  if (Loc.Line < 1)
    return nullptr;
  const std::string &File = Loc.Filename.empty() ? SourceFilename : Loc.Filename;
  // AllSourceLines is keyed by the file's path, which is how a diagnostic raised
  // in a later pass still finds the right file's text; the transient
  // SourceLines is the same content for the file being parsed right now.
  auto It = AllSourceLines.find(File);
  const std::vector<std::string> &Lines =
      It != AllSourceLines.end() ? It->second : SourceLines;
  if (It == AllSourceLines.end() && File != SourceFilename)
    return nullptr;
  if (Loc.Line > static_cast<int>(Lines.size()))
    return nullptr;
  return &Lines[Loc.Line - 1];
}

// DiagnosticEngine : static storage
std::vector<DiagnosticEngine::Diag> DiagnosticEngine::Diagnostics;
std::set<int> DiagnosticEngine::ErrorLines;
bool DiagnosticEngine::CollectAllDiagnostics = false;

// setjmp/longjmp recovery buffers
jmp_buf g_recoverJmp;
bool g_recoverActive = false;
bool g_protoOnlyPass = false; // set by pre-pass 3a to stop after prototype

// Running count of unclosed '{'/'('/'[' minus '}'/')'/']', tracked globally
// by getNextToken() (see Parser.cpp) as it consumes each token -- i.e. the
// current nesting depth relative to the top level (0), regardless of how
// deeply nested inside if/while/compare/etc. the parser currently is.
// Used by main.cpp's SkipToNextTopLevelBoundary() to recover correctly from
// a parse failure at ANY nesting depth, not just one level deep -- see that
// function's doc comment for why a naive "assume depth 1" recovery isn't
// enough. Reset to 0 by resetLexer() so a fresh parse pass (pre-pass vs.
// main pass, or a Sandbox.cpp re-parse) never inherits a stale value.
int g_braceDepth = 0;
int g_curlyDepth = 0; // see Lexer.h

// See Lexer.h for the full doc comment. Reset to 0 on longjmp (TurfError::
// raise(), Errors.h) since a RAII ParseRecursionGuard's destructor never
// runs for a stack frame longjmp skips past.
int g_parseRecursionDepth = 0;

// AI/SLM flag: only true when --smart is passed on the CLI.
bool g_AIEnabled = false;
std::string g_SmartFixOutPath;
bool g_AllowUnsafeModules = false;
bool g_ShowStdlibWarnings = false;

// Tracks if the SLM has generated a diagnostic this pass to suppress
// analysis of subsequent cascading errors.
static bool g_slmFiredThisPass = false;

// The real on-disk backing store for the common case (OpenSourceFile()
// below). SourceFile itself is a std::istream* -- see Lexer.h's doc comment
// -- so it can ALSO point at a caller-owned in-memory std::istringstream via
// SetSourceBuffer() (issue #232) without gettok() (which only ever calls
// ->get()/->peek(), both plain std::istream members) needing to know or
// care which kind of stream is currently backing it.
std::ifstream GlobalSourceFile;
std::istream *SourceFile = &GlobalSourceFile;

bool OpenSourceFile(const std::string &Path) {
  GlobalSourceFile.close();
  GlobalSourceFile.clear(); // clear any eof/fail bits left over from a previous file
  GlobalSourceFile.open(Path);
  SourceFile = &GlobalSourceFile;
  return GlobalSourceFile.is_open();
}

void CloseSourceFile() {
  // A no-op when SourceFile currently points at a caller-owned buffer
  // (SetSourceBuffer()) rather than GlobalSourceFile -- there is nothing of
  // ours to close in that case; the caller owns that stream's lifetime.
  if (SourceFile == &GlobalSourceFile)
    GlobalSourceFile.close();
}

void SetSourceBuffer(std::istream &Buffer) { SourceFile = &Buffer; }
std::string NumLitSpelling;
std::string NumLitClean;
int NumLitRadix = 10;
bool BoolVal;
char CharVal;
std::string IdentifierStr;
std::string StringVal;
int GetLastChar() { return LastChar; }
void SetLastChar(int c) { LastChar = c; }
std::map<std::string, int> Keywords = {{"if", TOK_IF},
                                       {"else", TOK_ELSE},
                                       {"while", TOK_WHILE},
                                       {"do", TOK_DO},
                                       {"int", TOK_TYPE_INT},
                                       {"float", TOK_TYPE_DOUBLE},
                                       {"double", TOK_TYPE_DOUBLE},
                                       {"bool", TOK_TYPE_BOOL},
                                       {"char", TOK_TYPE_CHAR},
                                       {"true", TOK_BOOL_LITERAL},
                                       {"false", TOK_BOOL_LITERAL},
                                       {"string", TOK_TYPE_STRING},
                                       {"void", TOK_TYPE_VOID},
                                       {"return", TOK_RETURN},
                                       {"fn", TOK_FN},
                                       {"break", TOK_BREAK},
                                       {"continue", TOK_CONTINUE},
                                       {"for", TOK_FOR},
                                       {"in", TOK_IN},
                                       {"step", TOK_STEP},
                                       {"elseif", TOK_ELSEIF},
                                       {"struct", TOK_STRUCT},
                                       {"self", TOK_SELF},
                                       {"sizeof", TOK_SIZEOF},
                                       {"import", TOK_IMPORT},
                                       {"enum", TOK_ENUM},
                                       {"machine", TOK_MACHINE},

                                       {"null", TOK_NULL},
                                       {"mutable", TOK_MUTABLE},
                                       {"mut", TOK_MUTABLE},
                                       {"immutable", TOK_IMMUTABLE},
                                       {"immut", TOK_IMMUTABLE},
                                       {"to", TOK_TO},
                                       {"give", TOK_GIVE},
                                       {"otherwise", TOK_OTHERWISE},
                                       {"extern", TOK_EXTERN},
                                       {"ptr", TOK_PTR},
                                       {"compare", TOK_COMPARE},
                                       {"matches", TOK_MATCHES},
                                       {"done", TOK_DONE},
                                       {"export", TOK_EXPORT},
                                       {"static", TOK_STATIC},
                                       {"operator", TOK_OPERATOR},
                                       {"int32", TOK_TYPE_INT32},
                                       {"error", TOK_ERROR},
                                       {"recover", TOK_RECOVER},
                                       {"try", TOK_TRY},
                                       {"produce", TOK_PRODUCE},
                                       {"run", TOK_RUN},
                                       {"as", TOK_AS},
                                       {"unsafe", TOK_UNSAFE},
                                       {"guess", TOK_GUESS},
                                       {"defer", TOK_DEFER},
                                       {"pub", TOK_PUB},
                                       {"public", TOK_PUB},
                                       {"caller_location", TOK_CALLER_LOCATION}};

/**
 * @brief True if @p Msg looks like a warning rather than a hard error.
 *
 * Used as a fallback classifier for diagnostics that were logged through the
 * untyped LogErrorAt() path (as opposed to a proper TurfError/TurfWarning
 * subclass, which already knows its own severity). Matches on the literal
 * "Warning:"/"warning:" substring that warning-producing call sites embed in
 * their message text.
 */
static bool isMessageWarning(const std::string &Msg) {
  return Msg.find("Warning:") != std::string::npos ||
         Msg.find("warning:") != std::string::npos;
}

/**
 * @brief The path a diagnostic names a file by: relative to where the compiler
 *        was run, never an absolute path that differs per machine.
 *
 * Standard-library files are shown from `lib/std/` on (an install path says
 * nothing about which module it is). A path outside the working directory
 * keeps the spelling it was given with -- `../../x.tr` computed from an
 * absolute path is longer and no clearer than what the user typed. Never
 * throws: a diagnostic must still print when the file no longer exists or
 * the working directory is unreadable.
 */
static std::string displayPath(const std::string &Path) {
  if (const size_t Std = Path.rfind("lib/std/"); Std != std::string::npos)
    return Path.substr(Std);

  namespace fs = std::filesystem;
  std::error_code EC;
  const fs::path Abs = fs::absolute(fs::path(Path), EC).lexically_normal();
  const fs::path Cwd = fs::current_path(EC);
  if (EC || Abs.empty() || Cwd.empty())
    return Path;
  const std::string Rel = Abs.lexically_relative(Cwd).generic_string();
  if (Rel.empty() || Rel == "." || Rel.rfind("..", 0) == 0)
    return Path;
  return Rel;
}

// Internal helper: physically print one diagnostic to stderr.
static void printDiag(const DiagnosticEngine::Diag &D) {
  bool isWarning = D.IsWarning || isMessageWarning(D.Msg);

  const std::string &AccentColor =
      isWarning ? Colors::BRIGHT_YELLOW : Colors::BRIGHT_RED;
  const char *TypeLabel = isWarning ? "warning" : "error";

  // error/warning at line X, col Y [E0NN] [in file: ...]
  //
  // [E0NN] (when present) is folded into the same BRIGHT_BLACK segment as
  // "at line X, col Y" rather than getting its own AccentColor->BRIGHT_BLACK
  // ->AccentColor round-trip mid-line -- one color switch (AccentColor for
  // the "error"/"warning" label, then a single RESET+BRIGHT_BLACK for
  // everything after it), same as before E0NN codes existed.
  //
  // A diagnostic about a whole file rather than a place in it -- "no `main`
  // function" is the one today -- is raised at Line 0 and prints without a
  // position ("\u2500\u2500 error [E119]") instead of claiming "line 0, col 0".
  std::cerr << Colors::BOLD << AccentColor << "\u2500\u2500 " << TypeLabel
            << " " << Colors::RESET << Colors::BRIGHT_BLACK;
  if (D.Loc.Line > 0)
    std::cerr << "at line " << D.Loc.Line << ", col " << D.Loc.Col;
  if (!D.Code.empty())
    std::cerr << (D.Loc.Line > 0 ? " " : "") << "[" << D.Code << "]";
  // No ", in file X" here any more: flushAll() prints a file header (#266)
  // before each file's batch, so every diagnostic's file is already named
  // above it -- the main file's included, not only an imported module's.
  std::cerr << Colors::RESET << "\n\n";

  // Message body — embedded ANSI codes from error classes render intact.
  std::cerr << "  " << D.Msg << "\n";

  // Structural "declared here" / "candidate: ..." context -- e.g. where an
  // earlier conflicting declaration or a rejected overload actually lives.
  // ~30 call sites across the compiler already attach these via
  // TurfError::withRelated()/RelatedLocations_, but until now they were
  // ONLY ever fed to the SLM prompt as pinned context (see RelatedLocation's
  // own doc comment) -- an ordinary compile with no --smart/--online never
  // rendered them at all, so e.g. an extern-conflict or duplicate-
  // 'otherwise'-arm error pointed at the new occurrence with no way to see
  // where the original one was. Rendered as one blue note per location,
  // mirroring the hand-rolled "Note: you first created it at line X..."
  // wording a couple of error classes used to build manually before this
  // existed (now redundant there and removed, see DuplicateDeclarationError/
  // ShadowingWarning).
  for (const auto &Rel : D.RelatedLocations) {
    // LEC-005: a note in a DIFFERENT file than the diagnostic (an
    // instantiation chain walks from the user's file into `lib/std`) names
    // it -- "line 125, col 29" alone would read as a position in the user's
    // own file. Same-file notes are unchanged.
    const bool OtherFile = !Rel.Loc.Filename.empty() &&
                           Rel.Loc.Filename != D.Filename;
    const std::string RelFile = displayPath(Rel.Loc.Filename);
    std::cerr << "  " << Colors::BRIGHT_BLUE << "Note: " << Rel.Description
              << " (" << (OtherFile ? RelFile + ", " : std::string())
              << "line " << Rel.Loc.Line << ", col " << Rel.Loc.Col << ")"
              << Colors::RESET << "\n";
  }

  if (!D.Hint.empty()) {
    std::cerr << "\n  " << Colors::BRIGHT_GREEN << "Hint: " << D.Hint << Colors::RESET << "\n";
  }

  // Code snippet with │ gutter. Looked up from AllSourceLines (a persistent,
  // cross-pass, per-file cache) via D.Filename, NOT the transient global
  // SourceLines -- see AllSourceLines' doc comment in Lexer.h for why: by
  // the time flushAll() runs, SourceLines reflects whatever file was parsed
  // LAST (normally back to the main file), which is the wrong content for
  // any diagnostic whose Loc originated inside an imported module.
  auto FileLinesIt = AllSourceLines.find(D.Filename);
  if (FileLinesIt != AllSourceLines.end() && D.Loc.Line > 0 &&
      D.Loc.Line <= static_cast<int>(FileLinesIt->second.size())) {
    const std::string &LineContent = FileLinesIt->second[D.Loc.Line - 1];
    std::string LineNumStr = std::to_string(D.Loc.Line);
    int CaretOffset = (D.Loc.Col > 0) ? D.Loc.Col - 1 : 0;

    // Underline the token's full width when an end position was recorded
    // (EndCol != -1, same line as the start -- see SourceLocation's own doc
    // comment in Lexer.h for which token kinds populate this) instead of
    // just a single '^'. Falls back to a bare '^' for locations with no end
    // position yet (multi-token spans -- a whole sub-expression -- remain
    // out of scope, see issue #241 Track D) or a degenerate zero-width span.
    int SpanWidth = 1;
    if (D.Loc.EndCol != -1 && D.Loc.EndLine == D.Loc.Line &&
        D.Loc.EndCol > D.Loc.Col)
      SpanWidth = D.Loc.EndCol - D.Loc.Col;
    std::string Underline = "^" + std::string(SpanWidth - 1, '~');

    std::cerr << "\n"
              << Colors::BRIGHT_BLACK << "  " << LineNumStr << " \u2502 "
              << Colors::RESET << LineContent << "\n"
              << Colors::BRIGHT_BLACK << "  "
              << std::string(LineNumStr.length(), ' ') << " \u2502 "
              << Colors::RESET << std::string(CaretOffset, ' ') << Colors::BOLD
              << AccentColor << Underline << " Here!" << Colors::RESET << "\n";
  }

  // Point at `turf --explain E0NN` for the longer, beginner-facing
  // writeup -- same Code the header already showed, so no separate
  // lookup on the reader's part. Only for diagnostics that actually have
  // a code (every error does, issue #263; a warning without a registry row
  // does not -- see DiagnosticEngine::Diag::Code's own doc comment in
  // Lexer.h), so this never dangles into "for more details on ''".
  //
  // A small curated set of codes (ErrorCodeEntry::Important -- raw-
  // pointer lifetime rules, unbounded recursion, state-machine
  // transitions) get a more insistent, differently-colored version of
  // this line instead of the routine one: these are exactly the errors
  // whose one-line message is least likely to be self-explanatory on
  // its own, so it's worth nudging harder than for a plain typo.
  if (!D.Code.empty()) {
    const ErrorCodeEntry *Entry = ErrorCodes::lookupByCode(D.Code);
    if (Entry && Entry->Important) {
      std::cerr << "\n  " << Colors::BOLD << Colors::BRIGHT_CYAN
                << "Worth reading in full -- run `turf --explain " << D.Code
                << "` before you patch this one." << Colors::RESET << "\n";
    } else {
      std::cerr << "\n  " << Colors::BRIGHT_BLACK << "Run `turf --explain "
                << D.Code << "` for more details." << Colors::RESET << "\n";
    }
  }

  std::cerr << "\n";
}

/**
 * @brief Record one diagnostic (error or warning) and, for errors only, run
 *        the --smart/--online AI-suggestion pipeline against it.
 *
 * This is the single choke point every diagnostic in the compiler passes
 * through -- TurfError::raise(), TurfWarning::emit(), and the raw
 * LogErrorAt() helper all funnel here.
 *
 * Two independent things happen, in order:
 *
 *  1. Cascade suppression: if a *primary error* (IsWarning == false) has
 *     already been recorded on this exact source line, everything else on
 *     that line -- further errors AND warnings -- is dropped silently. This
 *     keeps one bad statement from spamming five near-duplicate diagnostics.
 *
 *  2. AI augmentation (only reached for actual errors -- see below): when
 *     --smart/--online is enabled (g_AIEnabled), the first error of the
 *     current compilation pass is sent to SLM::validateAndApplySuggestion(),
 *     and a high-confidence, sandbox-validated fix replaces the compiler's
 *     own message. Every error *after* that first one in the same pass just
 *     gets a short "this may be a cascading effect" note appended instead of
 *     a second model call -- see g_slmFiredThisPass.
 *
 * IMPORTANT: step 2 is gated on `!IsWarning`. Warnings (shadowing, redundant
 * comparisons, etc.) must never trigger a model call or carry the cascade
 * note -- the documented --smart/--online contract is "on error only, zero
 * cost on a clean compile," and a warning is not evidence of a preceding
 * error worth explaining away. A prior version of this function omitted that
 * check and ran full SLM inference (and attached a nonsensical "cascading
 * effect of the error above" note) on ordinary warnings.
 *
 * @param Loc      Source location the diagnostic is anchored to.
 * @param Msg      Fully-formatted (ANSI-colored) message body.
 * @param Hint     Optional one-line remediation hint shown under the message.
 * @param IsWarning  false = hard error (blocks compilation); true = warning.
 * @param Related  Structural "declared here" / "candidate: ..." locations,
 *                 threaded through to the SLM prompt as pinned context lines.
 */
void DiagnosticEngine::add(SourceLocation Loc, const std::string &Msg,
                           const std::string &Hint, bool IsWarning,
                           const std::vector<RelatedLocation> &Related,
                           SLM::ErrorCategory Category,
                           const std::string &Code,
                           const SLM::DiagnosticHints &Hints) {
  // If there is already an error on this line, suppress everything else
  // (both additional errors and warnings) to avoid cascading noise -- unless
  // an embedder opted into CollectAllDiagnostics (see its doc comment),
  // which wants every real diagnostic, not just the first per line.
  if (!CollectAllDiagnostics && ErrorLines.count(Loc.Line))
    return;

  std::string finalMessage = Msg;

  // Conditionally invoke SLM analysis based on CLI configuration.
  // Errors only -- see the "IMPORTANT" note in the function doc comment
  // above for why warnings must never reach this block.
  if (g_AIEnabled && !IsWarning) {
    if (g_slmFiredThisPass) {
      // Append advisory AI note for cascading errors.
      finalMessage +=
          "\n\n  " + Colors::BRIGHT_BLACK +
          "[Smart Assist]  This error may be a cascading effect of the one "
          "reported above. Fix the first issue and run --smart again for a "
          "fresh analysis." +
          Colors::RESET;
    } else {
      // A fix-it's columns describe ITS file's text, but the sandbox patches
      // SourceLines, the file being compiled right now: for a diagnostic
      // raised inside an imported module, applying them there would edit an
      // unrelated line of the importer. Such a fix goes to the model (which has
      // the same placement limit, but proposes whole lines and is validated).
      SLM::DiagnosticHints UsableHints = Hints;
      if (!Loc.Filename.empty() && Loc.Filename != SourceFilename) {
        UsableHints.FixIts.clear();
        UsableHints.FixReason.clear();
      }
      SLM::AISuggestion ai = SLM::validateAndApplySuggestion(Msg, Loc.Line, Hint, Related, Category, UsableHints, Code);

      if (ai.valid && ai.confidence >= 0.8f && ai.fix_mode != "NONE") {
        // Replace compiler message with high-confidence AI suggestion.
        g_slmFiredThisPass = true;
        finalMessage = Colors::BOLD + Colors::BRIGHT_CYAN + "[AI Suggestion]" +
                       Colors::RESET + "  " + ai.suggested_comment;

        if (!ai.explanation.empty()) {
          finalMessage += "\n  " + Colors::CYAN +
                          "Explanation: " + Colors::RESET + ai.explanation;
        }

        if (ai.fix_mode == "MODIFY" && !ai.code_patch.empty()) {
          finalMessage += "\n  " + Colors::BRIGHT_GREEN +
                          "Suggested fix:" + Colors::RESET + "\n    " +
                          Colors::BRIGHT_GREEN + ai.code_patch + Colors::RESET;
        }

        if (!ai.warning.empty()) {
          finalMessage +=
              "\n  " + Colors::YELLOW + "Note: " + Colors::RESET + ai.warning;
        }

        // SLM-001 follow-up: write the full patched source out, if asked.
        // Deliberately gated on the SAME confidence/fix_mode check as the
        // display above -- this must never write a fix a user wouldn't
        // also have been shown as validated.
        if (!g_SmartFixOutPath.empty() && !ai.appliedSource.empty()) {
          std::ofstream fixOut(g_SmartFixOutPath, std::ios::trunc);
          if (fixOut.is_open())
            fixOut << ai.appliedSource;
        }
      }
    }
  }

  // Which file this diagnostic belongs to: Loc's own Filename if it has one
  // (the normal case -- captured from CurLoc at parse time, see gettok()),
  // else fall back to whatever file is currently active. AllSourceLines is a
  // PERSISTENT cache (never overwritten/restored like SourceLines), so this
  // resolves correctly even for diagnostics raised by a pass that runs after
  // every file has already finished parsing (Resolver/TypeChecker/Codegen).
  std::string Filename = Loc.Filename.empty() ? SourceFilename : Loc.Filename;

  Diagnostics.push_back({Loc, finalMessage, Hint, IsWarning, Related, Category,
                         Filename, Code, Hints});

  if (!IsWarning)
    ErrorLines.insert(Loc.Line);
}

void DiagnosticEngine::add(SourceLocation Loc, const std::string &Msg,
                           bool IsWarning,
                           const std::vector<RelatedLocation> &Related) {
  add(Loc, Msg, "", IsWarning, Related);
}

bool DiagnosticEngine::hasErrors() { return !ErrorLines.empty(); }

bool DiagnosticEngine::hasErrorAt(int Line) {
  return ErrorLines.count(Line) != 0;
}

void DiagnosticEngine::flushAll() {
  // #266: one batch per file, each opened by a line naming the file. The
  // main file comes first (it is the one the user asked about), then the
  // modules it imported, by path so the order never depends on which pass
  // happened to reach a file first. Within a file: line, then column.
  //
  // Grouping by file also fixes a latent interleave: the old sort compared
  // line numbers alone, so an error at line 5 of an imported module and one
  // at line 5 of the main file were ordered arbitrarily relative to each
  // other, and neither said which file it was in unless it was the imported
  // one. stable_sort keeps emission order for diagnostics at the same
  // position, which std::sort did not promise.
  std::stable_sort(Diagnostics.begin(), Diagnostics.end(),
                   [](const Diag &A, const Diag &B) {
                     const bool AMain = A.Filename == MainSourceFilename;
                     const bool BMain = B.Filename == MainSourceFilename;
                     if (AMain != BMain)
                       return AMain;
                     if (A.Filename != B.Filename)
                       return A.Filename < B.Filename;
                     return A.Loc.Line < B.Loc.Line ||
                            (A.Loc.Line == B.Loc.Line && A.Loc.Col < B.Loc.Col);
                   });

  // OVH-012: a warning anchored inside the standard library itself (e.g.
  // `NeverReturnsErrorWarning`/`InfiniteLoopWarning` on stdlib code a
  // user merely `import`ed, never wrote) is a "system-module warning" --
  // suppressed at PRINT time only (Diagnostics itself, getDiagnostics(),
  // and getDiagnosticCount() are untouched, so nothing that inspects
  // diagnostics directly -- ParserModule.cpp's speculative-parse retry,
  // the query facade (AnalysisFacade.cpp) -- changes behavior). An error is
  // never filtered here: only IsWarning diagnostics reach this check. The
  // filter runs BEFORE grouping so a file whose only diagnostics were
  // suppressed gets no header and no "0 errors" line.
  std::vector<const Diag *> Shown;
  for (const auto &D : Diagnostics) {
    if (D.IsWarning && !g_ShowStdlibWarnings && IsLibStdWarningLocation(D.Filename))
      continue;
    Shown.push_back(&D);
  }

  for (size_t Begin = 0; Begin < Shown.size();) {
    size_t End = Begin;
    size_t Errors = 0, Warnings = 0;
    while (End < Shown.size() && Shown[End]->Filename == Shown[Begin]->Filename) {
      // Same rule as printDiag()'s own label, so the counts agree with what is
      // printed under them (a raw LogErrorAt() "warning:" message counts too).
      if (Shown[End]->IsWarning || isMessageWarning(Shown[End]->Msg))
        ++Warnings;
      else
        ++Errors;
      ++End;
    }

    if (!Shown[Begin]->Filename.empty()) {
      auto Plural = [](size_t N, const char *Word) {
        return std::to_string(N) + " " + Word + (N == 1 ? "" : "s");
      };
      std::string Counts;
      if (Errors)
        Counts += Plural(Errors, "error");
      if (Warnings)
        Counts += (Counts.empty() ? "" : ", ") + Plural(Warnings, "warning");
      std::cerr << Colors::BOLD << Colors::BRIGHT_CYAN
                << displayPath(Shown[Begin]->Filename) << Colors::RESET << " "
                << Colors::BRIGHT_BLACK << "(" << Counts << ")" << Colors::RESET
                << "\n\n";
    }

    for (size_t I = Begin; I < End; ++I)
      printDiag(*Shown[I]);
    Begin = End;
  }

  Diagnostics.clear();
}

void DiagnosticEngine::reset() {
  Diagnostics.clear();
  ErrorLines.clear();
  g_slmFiredThisPass =
      false; // Reset cascade guard for the next compilation pass
}

DiagnosticEngine::State DiagnosticEngine::saveState() {
  return {Diagnostics, ErrorLines, CollectAllDiagnostics, g_slmFiredThisPass};
}

void DiagnosticEngine::restoreState(State S) {
  Diagnostics = std::move(S.Diagnostics);
  ErrorLines = std::move(S.ErrorLines);
  CollectAllDiagnostics = S.CollectAllDiagnostics;
  g_slmFiredThisPass = S.SLMFiredThisPass;
}

void DiagnosticEngine::restoreDiagnostics(const std::vector<Diag> &Saved) {
  Diagnostics = Saved;
  ErrorLines.clear();
  for (const auto &D : Diagnostics) {
    if (!D.IsWarning) {
      ErrorLines.insert(D.Loc.Line);
    }
  }
}

// Emits and collects a generic diagnostic message at the given source
// location. `Category` (see Lexer.h's doc comment on the declaration) is
// forwarded to DiagnosticEngine::add() unconditionally -- inert for a
// warning message (the SLM path is gated on !IsWarning, same as
// TurfWarning::Category_), same as every other diagnostic sink in the
// compiler.
void LogErrorAt(SourceLocation Loc, const char *Code, const std::string &Msg,
                SLM::ErrorCategory Category, const SLM::DiagnosticHints &Hints) {
  // An unregistered code would print a "[E0NN]" that `--explain` rejects as
  // unknown -- exactly the dead end this parameter exists to remove -- so a
  // typo'd or never-registered code must be caught the first time a
  // developer's build hits that site, not shipped. (tests/api checks every
  // ErrCode:: constant too; this catches a raw string literal.)
  assert(ErrorCodes::lookupByCode(Code) &&
         "LogErrorAt: code is not a row of AllErrorCodes (ErrorCodes.h)");
  DiagnosticEngine::add(Loc, Msg, "", isMessageWarning(Msg), {}, Category,
                        Code, Hints);
}

void resetLexer() {
  LastChar = ' ';
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 0};
  PrevTokEndLine = -1;
  PrevTokEndCol = -1;
  g_braceDepth = 0;
  g_curlyDepth = 0;
}

/**
 * @brief Scans and returns the next token, without stamping CurLoc's end
 *        position -- see gettok() below, the public entry point, which
 *        wraps this and does that uniformly for every token kind.
 *
 * Scans characters sequentially, skipping whitespace, identifying keywords,
 * operators, numeric literals, and strings. Updates CurLoc for diagnostics.
 *
 * @return int The token type identifier (e.g., TOK_IDENTIFIER, TOK_NUMBER, or
 * ASCII value).
 */
static int gettokScan() {
  while (isspace(LastChar)) {
    // Handle newlines to track line numbers
    if (LastChar == '\n') {
      CurLine++;
      CurCol = 0;
    }

    LastChar = SourceFile->get();

    // Increment column for every character read
    if (LastChar != EOF) {
      CurCol++;
    }
  }

  // Snapshot the location before parsing the token body. Stamping Filename
  // here (rather than leaving SourceLocation's Filename field to default) is
  // what lets every AST node's Loc -- captured from CurLoc at parse time --
  // correctly remember which file it came from, even after later compiler
  // passes (Resolver/TypeChecker/Codegen) have moved on to a different file
  // and SourceFilename itself has changed. See AllSourceLines' doc comment.
  CurLoc = {CurLine, CurCol, SourceFilename};

  // Identifiers: id = [a-zA-Z_][a-zA-Z0-9_]*
  //
  // A leading '_' used to be unlexable, although the unused-variable hint
  // tells the user to prefix a name with one (audit F-22). LEC-003 (FREEZE.md
  // §6.17) makes `_name` an ordinary identifier -- which, by convention, also
  // silences the unused-variable warning -- and reserves `_` ALONE as the
  // wildcard pattern. The lone-`_` test below is on the WHOLE spelling, so
  // `__` and `_1` stay identifiers.
  if (isalpha(LastChar) || LastChar == '_') {
    IdentifierStr = LastChar;
    LastChar = SourceFile->get();
    CurCol++;

    while (isalnum(LastChar) || LastChar == '_') {
      IdentifierStr += LastChar;
      LastChar = SourceFile->get();
      CurCol++;
    }

    if (IdentifierStr == "_")
      return TOK_WILDCARD;

    // Check for keywords
    auto It = Keywords.find(IdentifierStr);
    if (It != Keywords.end()) {
      if (It->second == TOK_BOOL_LITERAL)
        BoolVal = (IdentifierStr == "true");
      return It->second;
    }

    return TOK_IDENTIFIER;
  }

  // String literals: "..."
  //
  // A Turf string literal must open and close on the same physical source
  // line -- a raw newline byte inside the quotes is always a mistake (a
  // forgotten closing '"'), never a way to write a multi-line string; that
  // requires the \n escape instead. The scan loop below stops at '"', EOF,
  // OR a raw '\n' for exactly that reason: without the '\n' stop condition,
  // an unclosed string would silently swallow the rest of the file --
  // including real code on later lines -- as string contents, and because
  // CurLine is only ever incremented by the whitespace-skipping logic at the
  // very top of gettok() (which this inner loop bypasses), every diagnostic
  // for the remainder of the file would then report the wrong line number.
  if (LastChar == '"') {
    StringVal = "";
    LastChar = SourceFile->get();
    CurCol++;

    while (LastChar != '"' && LastChar != EOF && LastChar != '\n') {
      if (LastChar == '\\') {
        // Handle escape sequences. Snapshot the backslash's own location
        // before consuming the escape character, so an "unknown escape"
        // diagnostic points at the actual '\' rather than wherever CurLoc
        // (the opening-quote location, unchanged since) happens to be.
        SourceLocation EscapeLoc{CurLine, CurCol, ""};
        LastChar = SourceFile->get();
        CurCol++;
        if (LastChar == 'n') {
          StringVal += '\n';
        } else if (LastChar == 't') {
          StringVal += '\t';
        } else if (LastChar == 'r') {
          StringVal += '\r';
        } else if (LastChar == '0') {
          StringVal += '\0';
        } else if (LastChar == 'a') {
          StringVal += '\a';
        } else if (LastChar == 'b') {
          StringVal += '\b';
        } else if (LastChar == 'f') {
          StringVal += '\f';
        } else if (LastChar == 'v') {
          StringVal += '\v';
        } else if (LastChar == '\\') {
          StringVal += '\\';
        } else if (LastChar == '"') {
          StringVal += '"';
        } else if (LastChar == '\'') {
          StringVal += '\'';
        } else {
          // Anything else after a '\' used to be silently accepted as if
          // the backslash weren't there at all -- e.g. "a\rb" used to
          // produce the 3-character string a/r/b (a literal 'r', not a
          // carriage return), because \r wasn't in the recognized set.
          // That's exactly the kind of mistake that should be caught at
          // compile time, not silently miscompiled: an unrecognized
          // escape is now a clean error instead of a quiet pass-through.
          LogErrorAt(EscapeLoc, ErrCode::InvalidEscapeSequence,
                     std::string("Unknown escape sequence '\\") +
                         static_cast<char>(LastChar) +
                         "' in string literal.\n  Hint: Recognized escapes "
                         "are \\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\\" \\'. "
                         "Did you mean to write a literal backslash? Use "
                         "'\\\\' instead.", SLM::ErrorCategory::SYNTAX_ERROR);
          StringVal += LastChar;
        }
      } else {
        StringVal += LastChar;
      }
      LastChar = SourceFile->get();
      CurCol++;
    }

    // Both the EOF and raw-newline paths below report the same underlying
    // mistake ("you never closed this string") with the same remediation, so
    // the hint text -- every diagnostic in Turf carries one; it's the main
    // thing that makes an error actually actionable rather than just a
    // location -- is written once here instead of duplicated per call site.
    // Uses the "\n  Hint: ..." convention that every other raw LogErrorAt()
    // call in the parser/lexer follows (TurfError subclasses in Errors.h use
    // a structured Hint_ field instead; LogErrorAt has no such field, so the
    // hint is appended directly into the message text).
    static const char *const UnterminatedStringHint =
        "\n  Hint: Add a closing '\"' before the end of the line -- Turf "
        "string literals can't span multiple lines. Need an actual line "
        "break inside the string? Use the \\n escape instead.";

    if (LastChar == EOF) {
      // Ran off the end of the file with no closing '"' anywhere -- there is
      // truly nothing left to lex, so end the token stream here exactly as
      // the pre-existing behavior did.
      LogErrorAt(CurLoc, ErrCode::UnterminatedLiteral,
                 std::string("Unterminated string literal") +
                     UnterminatedStringHint, SLM::ErrorCategory::SYNTAX_ERROR);
      return TOK_EOF;
    }

    if (LastChar == '\n') {
      // Hit end-of-line before a closing '"'. Report it at the opening-quote
      // location (CurLoc was snapshotted there at the top of gettok() and is
      // untouched since) and return the partial token WITHOUT consuming the
      // newline. Leaving '\n' in LastChar means the *next* gettok() call
      // walks into it via the ordinary isspace()/whitespace-skip path at the
      // top of this function, which correctly bumps CurLine and resets
      // CurCol -- so lexing (and line tracking) of the rest of the file
      // recovers cleanly instead of desyncing for every line that follows.
      LogErrorAt(CurLoc, ErrCode::UnterminatedLiteral,
                 std::string("Unterminated string literal") +
                     UnterminatedStringHint, SLM::ErrorCategory::SYNTAX_ERROR);
      return TOK_STRING_LITERAL;
    }

    // Consume closing "
    LastChar = SourceFile->get();
    CurCol++;

    return TOK_STRING_LITERAL;
  }

  // Char literals: 'x'
  //
  // A char is a single byte (RFC STRING_UFCS_OPERATOR_CHAR_RFC §3), not a
  // Unicode codepoint -- exactly one character, or one escape sequence, may
  // appear between the quotes. Reuses the identical escape-sequence set
  // recognized inside double-quoted strings above (\n \t \r \0 \a \b \f \v
  // \\ \" \'), so the same source text means the same byte in both string
  // and char literals.
  if (LastChar == '\'') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '\'') {
      // Empty literal '' -- consume the closing quote so the lexer doesn't
      // desync, report, and recover with a placeholder value.
      LogErrorAt(CurLoc, ErrCode::InvalidCharLiteral,
                 "Empty char literal.\n  Hint: A char literal must contain "
                 "exactly one character, e.g. 'a'.", SLM::ErrorCategory::SYNTAX_ERROR);
      LastChar = SourceFile->get();
      CurCol++;
      CharVal = '\0';
      return TOK_CHAR_LITERAL;
    }

    if (LastChar == EOF || LastChar == '\n') {
      // Same rationale as the unterminated-string case above: don't consume
      // the newline, so the next gettok() call's whitespace-skip recovers
      // line tracking cleanly instead of desyncing for the rest of the file.
      LogErrorAt(CurLoc, ErrCode::UnterminatedLiteral,
                 std::string("Unterminated char literal") +
                     "\n  Hint: Add a closing ''' after the character.", SLM::ErrorCategory::SYNTAX_ERROR);
      return LastChar == EOF ? TOK_EOF : TOK_CHAR_LITERAL;
    }

    if (LastChar == '\\') {
      SourceLocation EscapeLoc{CurLine, CurCol, ""};
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == 'n') {
        CharVal = '\n';
      } else if (LastChar == 't') {
        CharVal = '\t';
      } else if (LastChar == 'r') {
        CharVal = '\r';
      } else if (LastChar == '0') {
        CharVal = '\0';
      } else if (LastChar == 'a') {
        CharVal = '\a';
      } else if (LastChar == 'b') {
        CharVal = '\b';
      } else if (LastChar == 'f') {
        CharVal = '\f';
      } else if (LastChar == 'v') {
        CharVal = '\v';
      } else if (LastChar == '\\') {
        CharVal = '\\';
      } else if (LastChar == '"') {
        CharVal = '"';
      } else if (LastChar == '\'') {
        CharVal = '\'';
      } else {
        LogErrorAt(EscapeLoc, ErrCode::InvalidEscapeSequence,
                   std::string("Unknown escape sequence '\\") +
                       static_cast<char>(LastChar) +
                       "' in char literal.\n  Hint: Recognized escapes are "
                       "\\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\\" \\'.", SLM::ErrorCategory::SYNTAX_ERROR);
        CharVal = static_cast<char>(LastChar);
      }
    } else {
      CharVal = static_cast<char>(LastChar);
    }

    // Consume the character (or the last byte of the escape) just recorded.
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar != '\'') {
      // More than one character before the closing quote, e.g. 'ab'. Skip
      // forward to the next ''' (or EOF/newline) so the rest of the file
      // keeps lexing from a sane point instead of every later token being
      // offset by the stray characters.
      LogErrorAt(CurLoc, ErrCode::InvalidCharLiteral,
                 "Char literal must contain exactly one character.\n  Hint: "
                 "Use a string literal (\"ab\") for multiple characters, or "
                 "check for a missing closing '.", SLM::ErrorCategory::SYNTAX_ERROR);
      while (LastChar != '\'' && LastChar != EOF && LastChar != '\n') {
        LastChar = SourceFile->get();
        CurCol++;
      }
      if (LastChar == '\'') {
        LastChar = SourceFile->get();
        CurCol++;
      }
      return TOK_CHAR_LITERAL;
    }

    // Consume closing '
    LastChar = SourceFile->get();
    CurCol++;

    return TOK_CHAR_LITERAL;
  }

  // Ellipsis '...' (variadic marker in extern fn) must be checked before '..'
  // (range operator) so three dots aren't consumed as range + stray dot.
  if (LastChar == '.' && SourceFile->peek() == '.') {
    // Peek at the character after the second dot
    SourceFile->get(); CurCol++;
    int third  = SourceFile->peek();
    if (third == '.') {
      SourceFile->get(); CurCol++; // consume third dot
      LastChar = SourceFile->get(); CurCol++;
      return TOK_ELLIPSIS;
    }
    // Only two dots — range operator
    LastChar = SourceFile->get(); CurCol++;
    return TOK_RANGE;
  }

  // Numbers (SEM-001, docs/planning/ARCHITECTURE_OVERHAUL.md §8;
  // FREEZE.md §6.2 row 1). Four integer forms -- plain decimal (`[0-9_]+`),
  // hex (`0x…`/`0X…`), binary (`0b…`/`0B…`), octal (`0o…`/`0O…`) -- and two
  // float forms -- dotted (an optional integer part, a mandatory `.`, a
  // mandatory fractional digit, an optional exponent) or dotless-exponent
  // (digits then a MANDATORY exponent, no dot at all, e.g. `1e6`; FREEZE's
  // grammar originally only listed the dotted form -- corrected 2026-09-18
  // once SEM-001's own testing scope showed the omission, see FREEZE.md's
  // row 1 for the full note). `_` is a separator anywhere BETWEEN two
  // digits, in any digit run below, never leading or trailing one.
  //
  // This whole branch reports its own lexical errors directly, as real
  // TurfError subclasses (ErrorsLex.h) -- unlike every other diagnostic in
  // this file, which goes through the raw LogErrorAt() (and names its code) (see
  // ErrorsLex.h's own top comment for why that's safe to do from here).
  // NumLitSpelling/NumLitClean/NumLitRadix (Lexer.h) carry the exact result
  // back to ParseNumberExpr (ParserLiteral.cpp), which builds the real
  // llvm::APSInt/llvm::APFloat -- kept out of this file so Lexer.h/
  // Lexer.cpp stay LLVM-free, as they always have been.
  if (isdigit(LastChar) || (LastChar == '.' && isdigit(SourceFile->peek()))) {
    SourceLocation NumLoc = CurLoc; // stamped at the top of gettokScan()
    std::string Spelling; // exact original text, prefix + separators intact
    std::string Clean;    // same text with the prefix and every '_' removed

    // Consumes a run of IsDigit(c)-or-'_' characters, appending each to
    // Spelling (always) and Clean (real digits only). Raises
    // MisplacedSeparatorError if the FIRST or LAST character THIS CALL
    // actually consumes is '_' -- a run this call consumes nothing from
    // (Spelling unchanged) is never flagged, which is what lets the
    // leading-dot ('.5') and dotless-integer ('5') entry points share this
    // same helper safely. Returns how many real digits (excluding any '_')
    // were consumed.
    auto ScanDigitRun = [&](auto IsDigit) -> size_t {
      size_t RunStart = Spelling.size();
      size_t CleanStart = Clean.size();
      while (IsDigit(LastChar) || LastChar == '_') {
        if (LastChar != '_')
          Clean += static_cast<char>(LastChar);
        Spelling += static_cast<char>(LastChar);
        LastChar = SourceFile->get();
        CurCol++;
      }
      if (Spelling.size() > RunStart) {
        if (Spelling[RunStart] == '_')
          MisplacedSeparatorError(NumLoc, Spelling, /*Leading=*/true).raise();
        if (Spelling.back() == '_')
          MisplacedSeparatorError(NumLoc, Spelling, /*Leading=*/false).raise();
      }
      return Clean.size() - CleanStart;
    };
    auto IsDecDigit = [](int c) { return isdigit(c) != 0; };

    // A '.' sitting right where this literal ended is never silently left for
    // the parser: `3.14.15` used to lex as `3.14` then `.15` ("Expected ';'")
    // and `5.` as `5` then `.` ("Expected a method name"), each telling a
    // beginner about the wrong mistake. Only `..`/`...` (the range and
    // ellipsis operators: `1..5`) and `.name` (member access on a literal) are
    // legitimate continuations; everything else is E139. LastChar is the '.',
    // peek() the character after it.
    auto RejectStrayPoint = [&](bool Radix, bool AlreadyFloat) {
      if (LastChar != '.')
        return;
      int Next = SourceFile->peek();
      if (Next == '.' || isalpha(Next) || Next == '_')
        return;
      using K = MalformedNumberError::Kind;
      if (Radix) {
        if (isdigit(Next)) // `0xFF.` alone is left to the parser, as before
          MalformedNumberError(NumLoc, Spelling, K::RadixPoint).raise();
        return;
      }
      MalformedNumberError(NumLoc, Spelling,
                           AlreadyFloat ? K::SecondPoint : K::TrailingPoint)
          .raise();
    };

    // Radix-prefixed integer. Only recognized when the token starts with
    // exactly '0' followed immediately by one of these six letters -- '0'
    // alone, or '0' followed by an ORDINARY digit (the legacy-octal-
    // looking `0755`), falls through to the plain-decimal path below
    // instead, per FREEZE's deliberate "no bare-leading-zero octal" rule:
    // `0755` stays decimal `755`.
    if (LastChar == '0' &&
        (SourceFile->peek() == 'x' || SourceFile->peek() == 'X' ||
         SourceFile->peek() == 'b' || SourceFile->peek() == 'B' ||
         SourceFile->peek() == 'o' || SourceFile->peek() == 'O')) {
      Spelling += '0';
      LastChar = SourceFile->get(); CurCol++; // consume '0'
      char RadixChar = static_cast<char>(LastChar);
      Spelling += RadixChar;
      LastChar = SourceFile->get(); CurCol++; // consume x/X/b/B/o/O
      bool IsHex = (RadixChar == 'x' || RadixChar == 'X');
      bool IsBin = (RadixChar == 'b' || RadixChar == 'B');
      NumLitRadix = IsHex ? 16 : IsBin ? 2 : 8;

      size_t DigitsConsumed =
          IsHex ? ScanDigitRun([](int c) { return isxdigit(c) != 0; })
          : IsBin ? ScanDigitRun([](int c) { return c == '0' || c == '1'; })
                 : ScanDigitRun([](int c) { return c >= '0' && c <= '7'; });

      // A character that looks like it was meant to continue this literal
      // (alphanumeric) takes priority over "bare prefix" -- e.g. `0b012`'s
      // '2' and `0xG5`'s 'G' are both a wrong-for-this-radix DIGIT, a more
      // specific and more useful diagnosis than "no digit at all". Only a
      // truly empty continuation (whitespace/operator/EOF right after the
      // prefix, or right after the last valid digit) is the bare-prefix
      // case.
      if (isalnum(LastChar))
        InvalidRadixDigitError(NumLoc, static_cast<char>(LastChar),
                               NumLitRadix, Spelling)
            .raise();
      else if (DigitsConsumed == 0)
        MalformedRadixPrefixError(NumLoc, Spelling).raise();

      RejectStrayPoint(/*Radix=*/true, /*AlreadyFloat=*/false);
      NumLitSpelling = Spelling;
      NumLitClean = Clean;
      return TOK_INT_LITERAL;
    }

    // Plain decimal: an integer (`[0-9_]+`), or one of the two float forms.
    NumLitRadix = 10;
    ScanDigitRun(IsDecDigit); // integer part -- legitimately empty only via
                              // the leading-dot entry ('.5'), where LastChar
                              // is '.' and this call safely consumes nothing
    bool IsFloat = false;

    // Dotted form: only commit to '.' as this literal's decimal point when
    // a digit immediately follows it (mirrors this whole branch's own
    // leading-dot entry check above) -- a bare trailing dot like `5.` with
    // no digit after it is therefore NOT part of the number at all; `5` is
    // returned as a plain integer and `.` is left for the next token
    // (FREEZE's dotted-float grammar requires at least one fractional
    // digit, so `5.` was never a valid float form to begin with). The
    // `peek() != '.'` guard additionally keeps this from eating into the
    // START of the `..` range / `...` ellipsis operators (`1..5` must
    // still lex as NUMBER, RANGE, NUMBER).
    if (LastChar == '.' && SourceFile->peek() != '.' &&
        isdigit(SourceFile->peek())) {
      IsFloat = true;
      Spelling += '.';
      Clean += '.';
      LastChar = SourceFile->get(); CurCol++;
      ScanDigitRun(IsDecDigit); // at least one digit guaranteed by the peek() above
    }

    // Exponent: optional after the dotted form, but it's what makes the
    // DOTLESS form a float at all (if we reach EOF/an operator here with
    // IsFloat still false, this is just a plain decimal integer). Once
    // 'e'/'E' appears immediately here, it is ALWAYS committed to as an
    // exponent marker -- never silently reinterpreted as a separate
    // following token -- see MalformedExponentError's own doc comment.
    if (LastChar == 'e' || LastChar == 'E') {
      IsFloat = true;
      Spelling += static_cast<char>(LastChar);
      Clean += static_cast<char>(LastChar);
      LastChar = SourceFile->get(); CurCol++;
      if (LastChar == '+' || LastChar == '-') {
        Spelling += static_cast<char>(LastChar);
        Clean += static_cast<char>(LastChar);
        LastChar = SourceFile->get(); CurCol++;
      }
      if (ScanDigitRun(IsDecDigit) == 0)
        MalformedExponentError(NumLoc, Spelling).raise();
    }

    RejectStrayPoint(/*Radix=*/false, IsFloat);
    NumLitSpelling = Spelling;
    NumLitClean = Clean;
    return IsFloat ? TOK_NUMBER : TOK_INT_LITERAL;
  }

  if (LastChar == '=') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_EQ;
    }

    return TOK_ASSIGN;
  }

  if (LastChar == ':') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == ':') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_SCOPE;
    }

    return TOK_COLON;
  }

  if (LastChar == '!') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_NEQ;
    }

    return '!';
  }

  if (LastChar == '>') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_GEQ;
    }

    if (LastChar == '>') {
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == '=') {
        LastChar = SourceFile->get();
        CurCol++;
        return TOK_RSHIFT_ASSIGN;
      }
      return TOK_RSHIFT;
    }

    return '>';
  }

  if (LastChar == '<') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_LEQ;
    }

    if (LastChar == '<') {
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == '=') {
        LastChar = SourceFile->get();
        CurCol++;
        return TOK_LSHIFT_ASSIGN;
      }
      return TOK_LSHIFT;
    }

    return '<';
  }

  if (LastChar == '&') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '&') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_AND;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_AND_ASSIGN;
    }

    return '&';
  }

  if (LastChar == '|') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '|') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_OR;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_OR_ASSIGN;
    }

    return '|';
  }

  if (LastChar == '^') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_XOR_ASSIGN;
    }

    return '^';
  }

  if (LastChar == '+') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '+') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_PLUS_PLUS;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_PLUS_ASSIGN;
    }

    return '+';
  }

  if (LastChar == '-') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '-') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MINUS_MINUS;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MINUS_ASSIGN;
    }

    if (LastChar == '>') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_ARROW;
    }

    return '-';
  }

  if (LastChar == '*') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MUL_ASSIGN;
    }

    if (LastChar == '*') {
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == '=') {
        LastChar = SourceFile->get();
        CurCol++;
        return TOK_POWER_ASSIGN;
      }
      return TOK_POWER;
    }

    return '*';
  }

  if (LastChar == '%') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MOD_ASSIGN;
    }

    return '%';
  }

  if (LastChar == '/') {
    if (SourceFile->peek() == '/') {
      do {
        LastChar = SourceFile->get();
        CurCol++;
      } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

      if (LastChar != EOF)
        return gettokScan();
    } else if (SourceFile->peek() == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_DIV_ASSIGN;
    }
  }

  if (LastChar == '@') {
    LastChar = SourceFile->get();
    CurCol++;
    return TOK_AT;
  }

  if (LastChar == EOF)
    return TOK_EOF;

  // Handle ASCII characters
  int ThisChar = LastChar;
  LastChar = SourceFile->get();
  CurCol++;

  return ThisChar;
}

/**
 * @brief Public token-scanning entry point. Wraps gettokScan() and stamps
 *        CurLoc's end position (EndLine/EndCol) uniformly for every token
 *        kind it returns -- see SourceLocation's own doc comment (Lexer.h)
 *        for the field's semantics.
 *
 * By the time gettokScan() returns, CurLine/CurCol already sit exactly one
 * past the token's last consumed character for every branch that fully
 * scans its token (this was true even before this wrapper existed --
 * confirmed by tracing the '='/'==' branch: CurCol is incremented once per
 * character actually read via SourceFile->get(), and every branch reads
 * through the last character of its token before returning) -- i.e.
 * exactly the exclusive end column an LSP Range expects, and previously
 * only identifiers stamped explicitly (issue #243). The few branches that
 * intentionally stop short of a full token (unterminated string/char
 * literal recovery) never consume a mid-token newline, so CurLine is
 * always still the token's start line there too -- a same-line span is a
 * safe invariant to rely on unconditionally, not just for identifiers.
 */
int gettok() {
  // CurLoc still describes the token about to become the previous one, with
  // its end stamped below by the call that scanned it.
  PrevTokEndLine = CurLoc.EndLine;
  PrevTokEndCol = CurLoc.EndCol;
  int Tok = gettokScan();
  CurLoc.EndLine = CurLine;
  CurLoc.EndCol = CurCol;
  return Tok;
}

