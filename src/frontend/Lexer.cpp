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

#include "frontend/Lexer.h"
#include "diagnostics/Colors.h"
#include "diagnostics/ErrorCodes.h"
#include "diagnostics/LintLevels.h"
#include "diagnostics/PrimaryLabels.h" // DX-5: a code's own primary label
#include "diagnostics/DiagnosticsJson.h" // DX-2: --diagnostics=json
#include "diagnostics/ErrorsLex.h" // MalformedRadixPrefixError and friends (SEM-001) -- see
                       // ErrorsLex.h's own top comment for why Lexer.cpp,
                       // alone among the lexer/parser split, includes a
                       // TurfError header directly
#include "frontend/TestRegistry.h"
#include "frontend/Parser.h"    // OVH-012: IsLibStdWarningLocation() -- flushAll()'s
                       // system-module-warning gate
#include "slm/SLMIntegration.h"

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

// Error recovery: see RecoverUnwind (Lexer.h)
bool g_recoverActive = false;
bool g_protoOnlyPass = false; // set by pre-pass 3a to stop after prototype

// AI/SLM flag: only true when --smart is passed on the CLI.
bool g_AIEnabled = false;
std::string g_SmartFixOutPath;
bool g_AllowUnsafeModules = false;
bool g_ShowStdlibWarnings = false;

// Tracks if the SLM has generated a diagnostic this pass to suppress
// analysis of subsequent cascading errors.
static bool g_slmFiredThisPass = false;

LexerState::LexerState(std::istream &Buffer, std::string Filename)
    : Source(&Buffer), Filename(std::move(Filename)) {}

bool LexerState::open(const std::string &Path) {
  File.close();
  File.clear(); // eof/fail bits left over from the last read of the file
  File.open(Path);
  Source = &File;
  Filename = Path;
  reset();
  return File.is_open();
}

void LexerState::reset() {
  Lookahead.clear();
  LastChar = ' ';
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 0};
  PrevTokEndLine = -1;
  PrevTokEndCol = -1;
}

const std::map<std::string, int> Keywords = {{"if", TOK_IF},
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

// TDP-2-001: builds and checks D's fix, at most once per diagnostic and for at
// most CompilerFixes::MaxChecksPerCompile diagnostics in one compile. Any
// error or warning whose raise site attached fix-its, in a file the user wrote
// (never lib/std), gets one when the re-check passes.
// `D` must not be an element of DiagnosticEngine's own list (see flushAll()).
static void checkCompilerFix(DiagnosticEngine::Diag &D) {
  static int Checks = 0;
  // TURF_FIX_DEBUG=1 names every diagnostic this looks at and says why a
  // fix-it was not shown (scripts/slm/slm_deterministic_fix_smoke.sh checks
  // that a clean compile never gets here).
  const bool Debug = std::getenv("TURF_FIX_DEBUG") != nullptr;
  if (Debug)
    std::cerr << "[fix] " << D.Code << " line " << D.Loc.Line << ": looking\n";
  if (D.FixChecked || !D.Hints.hasFix() ||
      D.Code.empty() || IsLibStdWarningLocation(D.Filename))
    return;
  D.FixChecked = true;
  auto Lines = AllSourceLines.find(D.Filename);
  if (Lines == AllSourceLines.end() ||
      Checks >= CompilerFixes::MaxChecksPerCompile)
    return;
  // SL-7: alternatives, of which exactly one may pass. Every one is checked
  // (one that is not cannot be ruled out), so they need the budget for all.
  if (D.Hints.FixIts.empty()) {
    const auto &Alts = D.Hints.FixAlternatives;
    if (static_cast<int>(Alts.size()) > CompilerFixes::MaxChecksPerCompile - Checks) {
      if (Debug)
        std::cerr << "[fix] " << D.Code << " line " << D.Loc.Line << ": "
                  << Alts.size() << " alternatives exceed the check budget\n";
      return;
    }
    CompilerFix Passed;
    int Passing = 0;
    for (const auto &Alt : Alts) {
      SLM::DiagnosticHints H;
      H.FixIts = Alt.FixIts;
      H.FixReason = Alt.FixReason;
      H.FixDescription = Alt.FixDescription;
      CompilerFix Fix;
      if (!CompilerFixes::build(H, D.Loc.Line, Lines->second, Fix))
        continue;
      ++Checks;
      if (!CompilerFixes::check(Fix, D.Filename, D.Code, D.Loc.Line, D.IsWarning)) {
        if (Debug)
          std::cerr << "[fix] " << D.Code << " line " << D.Loc.Line << ": '"
                    << Fix.Description << "' failed the re-check\n";
        continue;
      }
      if (++Passing == 1)
        Passed = std::move(Fix);
    }
    if (Passing == 1)
      D.Fix = std::move(Passed);
    else if (Debug && Passing > 1)
      std::cerr << "[fix] " << D.Code << " line " << D.Loc.Line << ": "
                << Passing << " alternatives pass, so none is shown\n";
    return;
  }
  CompilerFix Fix;
  if (!CompilerFixes::build(D.Hints, D.Loc.Line, Lines->second, Fix)) {
    if (Debug)
      std::cerr << "[fix] " << D.Code << " line " << D.Loc.Line
                << ": the fix-its do not apply to the source\n";
    return;
  }
  ++Checks;
  if (!CompilerFixes::check(Fix, D.Filename, D.Code, D.Loc.Line, D.IsWarning)) {
    if (Debug)
      std::cerr << "[fix] " << D.Code << " line " << D.Loc.Line << ": '"
                << Fix.Description << "' failed the re-check\n";
    return;
  }
  D.Fix = std::move(Fix);
}

// Set once --smart has written a model's fix to --smart-write-fix, which the
// compiler's own fixes then leave alone.
static bool g_ModelFixWritten = false;

// --smart-write-fix=<path>: the file being compiled with every fix shown for it
// applied (two fixes that touch the same line: the first). Each fix was
// checked on its own; `turfc --test` checks them together.
static void writeCompilerFixes(const std::vector<DiagnosticEngine::Diag> &Diags) {
  if (g_SmartFixOutPath.empty() || g_ModelFixWritten)
    return;
  auto Main = AllSourceLines.find(MainSourceFilename);
  if (Main == AllSourceLines.end())
    return;
  std::vector<std::string> Lines = Main->second;
  std::set<int> Taken;
  bool Any = false;
  for (const auto &D : Diags) {
    if (D.Fix.empty() || D.Filename != MainSourceFilename)
      continue;
    bool Free = true;
    for (int L = D.Fix.FirstLine; L <= D.Fix.LastLine; ++L)
      Free = Free && !Taken.count(L);
    if (!Free)
      continue;
    for (int L = D.Fix.FirstLine; L <= D.Fix.LastLine; ++L) {
      Lines[L - 1] = D.Fix.NewLines[L - D.Fix.FirstLine];
      Taken.insert(L);
    }
    Any = true;
  }
  if (!Any)
    return;
  std::ofstream Out(g_SmartFixOutPath, std::ios::trunc);
  for (const auto &L : Lines)
    Out << L << '\n';
}

std::string diagLabel(const DiagnosticEngine::Diag &D) {
  if (!D.Hints.Label.empty())
    return D.Hints.Label;
  if (const char *L = ErrorCodes::defaultLabel(D.Code.c_str()))
    return L;
  return "here";
}

namespace {

// At most this many related locations are drawn as secondary spans; the rest
// are notes. A list of every overload is a note, not a picture.
constexpr size_t MaxSecondarySpans = 3;

const std::vector<std::string> *diagFileLines(const DiagnosticEngine::Diag &D) {
  auto It = AllSourceLines.find(D.Filename);
  if (It == AllSourceLines.end() || D.Loc.Line < 1 ||
      D.Loc.Line > static_cast<int>(It->second.size()))
    return nullptr;
  return &It->second;
}

// One underlined stretch of a snippet line: [Col, Col + Width), 1-based.
struct SnippetSpan {
  int Line = 0, Col = 1, Width = 1;
  bool Primary = false;
  std::string Label;
};

int spanWidth(const SourceLocation &L, const std::string &Text) {
  int W = 1;
  if (L.EndCol != -1 && L.EndLine == L.Line && L.EndCol > L.Col)
    W = L.EndCol - L.Col;
  const int Room = static_cast<int>(Text.size()) - (L.Col - 1);
  return std::max(1, std::min(W, std::max(Room, 1)));
}

} // namespace

// DX-5: which of D's related locations renderSnippet() draws: in D's own
// file, on D's line or an earlier one, not at D's own position, and only the
// first MaxSecondarySpans of those.
static std::vector<bool> drawnRelated(const DiagnosticEngine::Diag &D) {
  std::vector<bool> Drawn(D.RelatedLocations.size(), false);
  const auto *Lines = diagFileLines(D);
  if (!Lines)
    return Drawn;
  size_t Count = 0;
  for (size_t I = 0; I < D.RelatedLocations.size() && Count < MaxSecondarySpans; ++I) {
    const SourceLocation &L = D.RelatedLocations[I].Loc;
    const bool SameFile = L.Filename.empty() || L.Filename == D.Filename;
    if (!SameFile || L.Line < 1 || L.Line > D.Loc.Line || L.Col < 1 ||
        L.Col > static_cast<int>((*Lines)[L.Line - 1].size()) ||
        (L.Line == D.Loc.Line && L.Col == D.Loc.Col))
      continue;
    Drawn[I] = true;
    ++Count;
  }
  return Drawn;
}

// DX-5: the snippet under a diagnostic. Every line that carries a span is
// shown, in order, with a gap mark between lines that are not adjacent (one
// line between is shown instead). On a line, the spans are underlined --
// `^~~` for the primary, `---` for a secondary -- on one row when they do not
// overlap, on rows below when they do. The rightmost span of a row has its
// label beside it; the others hang below, joined to their span by `│`.
static std::string renderSnippet(const DiagnosticEngine::Diag &D,
                                 const std::vector<bool> &Drawn,
                                 const std::string &AccentColor) {
  const auto *Lines = diagFileLines(D);
  if (!Lines)
    return "";
  std::vector<SnippetSpan> Spans;
  {
    const std::string &Text = (*Lines)[D.Loc.Line - 1];
    SnippetSpan P;
    P.Line = D.Loc.Line;
    P.Col = std::max(D.Loc.Col, 1);
    P.Width = spanWidth(SourceLocation{D.Loc.Line, P.Col, "", D.Loc.EndLine, D.Loc.EndCol}, Text);
    P.Primary = true;
    P.Label = diagLabel(D);
    Spans.push_back(P);
  }
  for (size_t I = 0; I < D.RelatedLocations.size(); ++I) {
    if (!Drawn[I])
      continue;
    const auto &R = D.RelatedLocations[I];
    SnippetSpan S;
    S.Line = R.Loc.Line;
    S.Col = R.Loc.Col;
    S.Width = spanWidth(R.Loc, (*Lines)[R.Loc.Line - 1]);
    S.Label = R.Description;
    Spans.push_back(S);
  }

  std::vector<int> Shown;
  for (const auto &S : Spans)
    Shown.push_back(S.Line);
  std::sort(Shown.begin(), Shown.end());
  Shown.erase(std::unique(Shown.begin(), Shown.end()), Shown.end());
  // One line between two shown ones is shown itself; more are a gap.
  for (size_t I = 1; I < Shown.size(); ++I)
    if (Shown[I] - Shown[I - 1] == 2) {
      Shown.insert(Shown.begin() + I, Shown[I - 1] + 1);
      ++I;
    }

  const size_t NumWidth = std::to_string(Shown.back()).size();
  const std::string Blank(NumWidth, ' ');
  auto gutter = [&](const std::string &Num) {
    return Colors::BRIGHT_BLACK + "  " + std::string(NumWidth - Num.size(), ' ') +
           Num + " │ " + Colors::RESET;
  };
  auto colorOf = [&](const SnippetSpan &S) {
    return S.Primary ? Colors::BOLD + AccentColor : Colors::BRIGHT_BLUE;
  };

  std::string Out = "\n";
  for (size_t LI = 0; LI < Shown.size(); ++LI) {
    const int Line = Shown[LI];
    if (LI > 0 && Line - Shown[LI - 1] > 1)
      Out += Colors::BRIGHT_BLACK + "  " + Blank + " ┊" + Colors::RESET + "\n";
    Out += gutter(std::to_string(Line)) + (*Lines)[Line - 1] + "\n";

    // This line's spans, in rows that do not overlap: the primary first.
    std::vector<const SnippetSpan *> Here;
    for (const auto &S : Spans)
      if (S.Line == Line)
        Here.push_back(&S);
    std::stable_sort(Here.begin(), Here.end(), [](const SnippetSpan *A, const SnippetSpan *B) {
      return A->Primary > B->Primary;
    });
    std::vector<std::vector<const SnippetSpan *>> Rows;
    for (const SnippetSpan *S : Here) {
      bool Placed = false;
      for (auto &Row : Rows) {
        bool Clear = true;
        for (const SnippetSpan *O : Row)
          Clear = Clear && (S->Col + S->Width < O->Col || O->Col + O->Width < S->Col);
        if (Clear) {
          Row.push_back(S);
          Placed = true;
          break;
        }
      }
      if (!Placed)
        Rows.push_back({S});
    }

    for (auto &Row : Rows) {
      std::sort(Row.begin(), Row.end(), [](const SnippetSpan *A, const SnippetSpan *B) {
        return A->Col < B->Col;
      });
      // The underlines, and the rightmost span's label beside its own.
      std::string Marks;
      int At = 1;
      for (const SnippetSpan *S : Row) {
        Marks += std::string(S->Col - At, ' ') + colorOf(*S) +
                 (S->Primary ? "^" + std::string(S->Width - 1, '~')
                             : std::string(S->Width, '-')) +
                 Colors::RESET;
        At = S->Col + S->Width;
      }
      const SnippetSpan *Last = Row.back();
      Out += gutter("") + Marks + colorOf(*Last) + " " + Last->Label + Colors::RESET + "\n";
      // The others' labels hang below, right to left.
      for (size_t K = Row.size() - 1; K-- > 0;) {
        auto bars = [&](size_t UpTo) {
          std::string B;
          int Col = 1;
          for (size_t J = 0; J < UpTo; ++J) {
            B += std::string(Row[J]->Col - Col, ' ') + colorOf(*Row[J]) + "│" + Colors::RESET;
            Col = Row[J]->Col + 1;
          }
          return std::make_pair(B, Col);
        };
        auto [Above, ColA] = bars(K + 1);
        Out += gutter("") + Above + "\n";
        auto [Left, ColL] = bars(K);
        Out += gutter("") + Left + std::string(Row[K]->Col - ColL, ' ') + colorOf(*Row[K]) +
               Row[K]->Label + Colors::RESET + "\n";
      }
    }
  }
  return Out;
}

// Internal helper: physically print one diagnostic to stderr.
static void printDiag(const DiagnosticEngine::Diag &D) {
  // A denied warning is an error even though its text still says "Warning:".
  bool isWarning = !D.Denied && (D.IsWarning || isMessageWarning(D.Msg));

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
  if (!D.Code.empty()) {
    // "[E068 · unknown struct field]": the heading names the code's
    // registered name (ErrorCodes.h, DX-9), never the --smart category, so a
    // warning with a code shows its name too. Anything that matches a code
    // in the output matches on the "[E068" prefix, never on what follows.
    std::string CategoryText;
    if (const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(D.Code))
      CategoryText = ErrorCodes::headingName(*Row);
    std::cerr << (D.Loc.Line > 0 ? " " : "") << "[" << D.Code
              << (CategoryText.empty() ? "" : " · " + CategoryText) << "]";
  }
  // No ", in file X" here any more: flushAll() prints a file header (#266)
  // before each file's batch, so every diagnostic's file is already named
  // above it -- the main file's included, not only an imported module's.
  std::cerr << Colors::RESET << "\n\n";

  // Message body — embedded ANSI codes from error classes render intact.
  std::cerr << "  " << D.Msg << "\n";

  // The related locations drawn in the snippet below (DX-5): the secondary
  // spans. The rest -- another file, a later line, beyond the first few --
  // are notes here, one per location.
  const std::vector<bool> Drawn = drawnRelated(D);
  for (size_t I = 0; I < D.RelatedLocations.size(); ++I) {
    if (Drawn[I])
      continue;
    const auto &Rel = D.RelatedLocations[I];
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

  // V-1: a warning the build turned into an error says so, or the reader would
  // look for the mistake in a check that is normally only advice.
  if (D.Denied)
    std::cerr << "\n  " << Colors::BRIGHT_BLACK << D.Code
              << " is a warning by default; its lint level here is deny."
              << Colors::RESET << "\n";

  // Code snippet with │ gutter, the primary span labelled and the drawn
  // related locations as secondary spans (DX-5, renderSnippet()). Looked up
  // from AllSourceLines (a persistent, cross-pass, per-file cache) via
  // D.Filename, NOT the transient global SourceLines: by the time flushAll()
  // runs, SourceLines reflects whatever file was parsed LAST.
  std::cerr << renderSnippet(D, Drawn, AccentColor);

  // TDP-2-001: the checked fix, as the corrected line(s).
  std::cerr << CompilerFixes::render(D.Fix);

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
  // Which file this diagnostic belongs to: Loc's own Filename if it has one
  // (the normal case -- captured from CurLoc at parse time, see gettok()),
  // else fall back to whatever file is currently active. AllSourceLines is a
  // PERSISTENT cache (never overwritten/restored like SourceLines), so this
  // resolves correctly even for diagnostics raised by a pass that runs after
  // every file has already finished parsing (Resolver/TypeChecker/Codegen).
  std::string Filename = Loc.Filename.empty() ? SourceFilename : Loc.Filename;

  // TDP-2-006: an error inside a compile_error test's body is the test's
  // (TestRegistry.h), and so is a warning there; a warning test's expected
  // warning is its own and takes no lint level. Before the cascade rule
  // below, so the registry hears about every error a pass raises.
  bool OwnedByTest = false;
  {
    Diag Probe{Loc, Msg, Hint, IsWarning, Related, Category, Filename, Code, Hints,
               CompilerFix{}, false, false};
    if (IsWarning ? TestRegistry::swallowWarning(Probe) : TestRegistry::captureError(Probe))
      return;
    OwnedByTest = IsWarning && TestRegistry::ownsWarning(Probe);
  }

  // If there is already an error on this line, suppress everything else
  // (both additional errors and warnings) to avoid cascading noise -- unless
  // an embedder opted into CollectAllDiagnostics (see its doc comment),
  // which wants every real diagnostic, not just the first per line.
  if (!CollectAllDiagnostics && ErrorLines.count(Loc.Line))
    return;

  std::string finalMessage = Msg;

  // V-1 (LintLevels.h): the level the build chose for this warning's code.
  // `allow` records nothing; `deny` records an error, and from here on this IS
  // an error (it blocks the build, takes the error's cascade rule, reaches the
  // facade as an error). A denied warning inside lib/std stays the hidden
  // warning it is: the user did not write that code and cannot fix it, so
  // `--deny` must not fail a build over it (--stdlib-warnings opts back in).
  // Asked even with no level set: a warning whose row is AllowByDefault is
  // `allow` until something names it.
  bool Denied = false;
  if (IsWarning && !OwnedByTest && !Code.empty()) {
    switch (LintLevels::levelOf(Code)) {
    case LintLevels::Level::Allow:
      return;
    case LintLevels::Level::Deny:
      if (g_ShowStdlibWarnings || !IsLibStdWarningLocation(Filename)) {
        IsWarning = false;
        Denied = true;
      }
      break;
    case LintLevels::Level::Warn:
      break;
    }
  }

  Diag New{Loc, Msg, Hint, IsWarning, Related, Category, Filename, Code, Hints,
           CompilerFix{}, false, Denied};

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
      // The compiler's own fix first (TDP-2-001): when it has one, it is
      // printed as the diagnostic's `Fix:` and the model is never asked.
      checkCompilerFix(New);
      if (!New.Fix.empty()) {
        g_slmFiredThisPass = true;
      } else {
        SLM::AISuggestion ai = SLM::validateAndApplySuggestion(
            Msg, Loc.Line, Hint, Related, Category, Hints, Code);

        if (ai.valid && ai.confidence >= 0.8f && ai.fix_mode != "NONE") {
          // Replace compiler message with high-confidence model suggestion.
          g_slmFiredThisPass = true;
          finalMessage = Colors::BOLD + Colors::BRIGHT_CYAN +
                         "[Model Suggestion]" + Colors::RESET + "  " +
                         ai.suggested_comment;

          if (!ai.explanation.empty()) {
            finalMessage += "\n  " + Colors::CYAN +
                            "Explanation: " + Colors::RESET + ai.explanation;
          }

          // The before/after is rendered from the validated patch (the lines
          // actually replaced and their replacement), not from anything the
          // model wrote about it (SLM::renderPatchDiff()).
          const std::string patchDiff = SLM::renderPatchDiff(ai);
          if (ai.fix_mode == "MODIFY" && !patchDiff.empty()) {
            finalMessage += "\n  " + Colors::BRIGHT_GREEN +
                            "Suggested fix:" + Colors::RESET + "\n    " +
                            patchDiff;
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
            if (fixOut.is_open()) {
              fixOut << ai.appliedSource;
              g_ModelFixWritten = true;
            }
          }
        }
      }
    }
  }
  New.Msg = finalMessage;

  // The same warning at the same place, twice, says nothing new. A warning
  // raised while parsing comes twice for the main file, which is parsed
  // twice (an unreachable machine state, an empty `compare`).
  if (IsWarning || Denied)
    for (const Diag &D : Diagnostics)
      if ((D.IsWarning || D.Denied) && D.Loc.Line == Loc.Line &&
          D.Loc.Col == Loc.Col &&
          D.Filename == Filename && D.Msg == finalMessage)
        return;

  if (IsWarning)
    TestRegistry::noteWarning(New);
  Diagnostics.push_back(std::move(New));

  if (!IsWarning)
    ErrorLines.insert(Loc.Line);
}

void DiagnosticEngine::checkFix(Diag &D) { checkCompilerFix(D); }

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

  // TDP-2-001: every error is known now, so a fix is checked against the
  // program the reader will edit. In the order printed, so the cap keeps the
  // first errors' fixes.
  // The check analyses the program again, which saves and restores this
  // list: it works on a copy, never on an element of the list itself.
  if (CompilerFixes::Enabled)
    for (size_t I = 0; I < Diagnostics.size(); ++I) {
      if (Diagnostics[I].FixChecked || !Diagnostics[I].Hints.hasFix())
        continue;
      Diag D = Diagnostics[I];
      checkCompilerFix(D);
      Diagnostics[I].FixChecked = true;
      Diagnostics[I].Fix = std::move(D.Fix);
    }
  writeCompilerFixes(Diagnostics);

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

  // DX-2: one line of JSON per diagnostic, in the same order, instead of the
  // text (no file headers: every object names its file).
  if (DiagnosticsJson::Enabled) {
    for (const Diag *D : Shown)
      std::cerr << DiagnosticsJson::render(
                       *D, !D->Denied && (D->IsWarning || isMessageWarning(D->Msg)))
                << "\n";
    Diagnostics.clear();
    return;
  }

  for (size_t Begin = 0; Begin < Shown.size();) {
    size_t End = Begin;
    size_t Errors = 0, Warnings = 0;
    while (End < Shown.size() && Shown[End]->Filename == Shown[Begin]->Filename) {
      // Same rule as printDiag()'s own label, so the counts agree with what is
      // printed under them (a raw LogErrorAt() "warning:" message counts too).
      if (!Shown[End]->Denied &&
          (Shown[End]->IsWarning || isMessageWarning(Shown[End]->Msg)))
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

DiagnosticEngine::Mark DiagnosticEngine::mark() {
  return {Diagnostics.size(), ErrorLines, g_slmFiredThisPass};
}

void DiagnosticEngine::rollbackTo(Mark M) {
  if (Diagnostics.size() > M.Count)
    Diagnostics.resize(M.Count);
  ErrorLines = std::move(M.ErrorLines);
  g_slmFiredThisPass = M.SLMFiredThisPass;
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
  // DX-3 (issue #291): every error carries a concrete hint. A raw site that
  // wrote its own "\n  Hint: ..." keeps it; one that did not gets the line
  // below for its code, so a diagnostic never prints without one.
  std::string Full = Msg;
  if (!isMessageWarning(Msg) && Msg.find("Hint:") == std::string::npos) {
    const std::string C = Code;
    const char *H = nullptr;
    if (C == ErrCode::MissingSemicolon)
      H = "End the statement with ';' right after its last token, e.g. "
          "`x = x + 1;` or `return 0;`.";
    else if (C == ErrCode::UnexpectedToken && Msg.rfind("Unexpected ';'", 0) == 0)
      H = "Remove the extra ';' -- a statement ends with exactly one.";
    else if (C == ErrCode::UnexpectedToken)
      H = "Remove the token the arrow points at, or write what belongs there: "
          "a value, a name, or a complete statement.";
    else if (C == ErrCode::MissingComma)
      H = "Put a ',' between the items of the list, e.g. `a, b, c`, and close "
          "it with the matching bracket.";
    else if (C == ErrCode::MissingColon)
      H = "Put a ':' there: a name is followed by its type (`x: int`) and a "
          "function's parameter list by its return type (`): int`).";
    else if (C == ErrCode::MissingOpeningDelimiter)
      H = "Open the block with '{' (or the list with '(') at the spot the "
          "arrow points to.";
    else if (C == ErrCode::ExpectedName)
      H = "Write a name there: a letter or '_' followed by letters, digits "
          "or '_', e.g. `total`.";
    else if (C == ErrCode::ExpectedKeyword)
      H = "Write the keyword the message names at the spot the arrow points "
          "to.";
    else if (C == ErrCode::InvalidOperatorDeclaration)
      H = "Declare it with the parameter count its operator allows, e.g. "
          "`operator +(a: T, b: T): T { ... }`.";
    else if (C == ErrCode::ExternFunctionBody)
      H = "End an `extern fn` declaration with ';' and no body, or drop "
          "`extern` to write the body in Turf.";
    else if (C == ErrCode::StatementOutsideFunction)
      H = "Move the statement into a function body, e.g. `fn main(): int { "
          "... return 0; }`.";
    else if (C == ErrCode::InvalidArraySize)
      H = "Give the array a whole-number size of at least 1, e.g. `int[3]`.";
    else if (C == ErrCode::MissingInitializer)
      H = "Give the variable its value on the spot: `mut x: int = 5;`.";
    else if (C == ErrCode::SizeParameter)
      H = "Declare `N: size` only on a free function, e.g. `fn<T, N: size> "
          "first(a: T[N]): T`; not on an operator, a method or a struct.";
    if (H)
      Full += std::string("\n  Hint: ") + H;
  }
  DiagnosticEngine::add(Loc, Full, "", isMessageWarning(Msg), {}, Category,
                        Code, Hints);
}

static LexedToken captureToken(const LexerState &L, int Tok) {
  return {Tok,           L.IdentifierStr, L.StringVal,      L.NumLitSpelling,
          L.NumLitClean, L.NumLitRadix,   L.BoolVal,        L.CharVal,
          L.CurLoc,      L.PrevTokEndLine, L.PrevTokEndCol};
}

static void installToken(LexerState &L, const LexedToken &T) {
  L.IdentifierStr = T.IdentifierStr;
  L.StringVal = T.StringVal;
  L.NumLitSpelling = T.NumLitSpelling;
  L.NumLitClean = T.NumLitClean;
  L.NumLitRadix = T.NumLitRadix;
  L.BoolVal = T.BoolVal;
  L.CharVal = T.CharVal;
  L.CurLoc = T.Loc;
  L.PrevTokEndLine = T.PrevTokEndLine;
  L.PrevTokEndCol = T.PrevTokEndCol;
}

int peekToken(size_t N) {
  LexerState &L = Lex();
  if (N < L.Lookahead.size())
    return L.Lookahead[N].Tok;
  // gettok() writes the fields that describe CurTok; put them back after.
  // Each new token is lexed with the previous lookahead token installed, so
  // its PrevTokEnd* are right.
  const LexedToken Current = captureToken(L, 0);
  if (!L.Lookahead.empty())
    installToken(L, L.Lookahead.back());
  while (L.Lookahead.size() <= N) {
    const int Tok = gettok(L);
    L.Lookahead.push_back(captureToken(L, Tok));
  }
  installToken(L, Current);
  return L.Lookahead[N].Tok;
}

bool takeLookaheadToken(int &Tok) {
  LexerState &L = Lex();
  if (L.Lookahead.empty())
    return false;
  installToken(L, L.Lookahead.front());
  Tok = L.Lookahead.front().Tok;
  L.Lookahead.erase(L.Lookahead.begin());
  return true;
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
static int gettokScan(LexerState &L) {
  // No stream (Lex()'s idle lexer, or a LexerState never opened): EOF, at
  // wherever the cursor is.
  if (!L.Source) {
    L.CurLoc = {L.CurLine, L.CurCol, L.Filename};
    return TOK_EOF;
  }
  while (isspace(L.LastChar)) {
    // Handle newlines to track line numbers
    if (L.LastChar == '\n') {
      L.CurLine++;
      L.CurCol = 0;
    }

    L.LastChar = L.Source->get();

    // Increment column for every character read
    if (L.LastChar != EOF) {
      L.CurCol++;
    }
  }

  // Snapshot the location before parsing the token body. Stamping Filename
  // here (rather than leaving SourceLocation's Filename field to default) is
  // what lets every AST node's Loc -- captured from CurLoc at parse time --
  // correctly remember which file it came from, even after later compiler
  // passes (Resolver/TypeChecker/Codegen) have moved on to a different file
  // and SourceFilename itself has changed. See AllSourceLines' doc comment.
  L.CurLoc = {L.CurLine, L.CurCol, L.Filename};

  // Identifiers: id = [a-zA-Z_][a-zA-Z0-9_]*
  //
  // A leading '_' used to be unlexable, although the unused-variable hint
  // tells the user to prefix a name with one (audit F-22). LEC-003 (FREEZE.md
  // §6.17) makes `_name` an ordinary identifier -- which, by convention, also
  // silences the unused-variable warning -- and reserves `_` ALONE as the
  // wildcard pattern. The lone-`_` test below is on the WHOLE spelling, so
  // `__` and `_1` stay identifiers.
  if (isalpha(L.LastChar) || L.LastChar == '_') {
    L.IdentifierStr = L.LastChar;
    L.LastChar = L.Source->get();
    L.CurCol++;

    while (isalnum(L.LastChar) || L.LastChar == '_') {
      L.IdentifierStr += L.LastChar;
      L.LastChar = L.Source->get();
      L.CurCol++;
    }

    if (L.IdentifierStr == "_")
      return TOK_WILDCARD;

    // Check for keywords
    auto It = Keywords.find(L.IdentifierStr);
    if (It != Keywords.end()) {
      if (It->second == TOK_BOOL_LITERAL)
        L.BoolVal = (L.IdentifierStr == "true");
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
  if (L.LastChar == '"') {
    L.StringVal = "";
    L.LastChar = L.Source->get();
    L.CurCol++;

    while (L.LastChar != '"' && L.LastChar != EOF && L.LastChar != '\n') {
      if (L.LastChar == '\\') {
        // Handle escape sequences. Snapshot the backslash's own location
        // before consuming the escape character, so an "unknown escape"
        // diagnostic points at the actual '\' rather than wherever CurLoc
        // (the opening-quote location, unchanged since) happens to be.
        SourceLocation EscapeLoc{L.CurLine, L.CurCol, ""};
        L.LastChar = L.Source->get();
        L.CurCol++;
        if (L.LastChar == 'n') {
          L.StringVal += '\n';
        } else if (L.LastChar == 't') {
          L.StringVal += '\t';
        } else if (L.LastChar == 'r') {
          L.StringVal += '\r';
        } else if (L.LastChar == '0') {
          L.StringVal += '\0';
        } else if (L.LastChar == 'a') {
          L.StringVal += '\a';
        } else if (L.LastChar == 'b') {
          L.StringVal += '\b';
        } else if (L.LastChar == 'f') {
          L.StringVal += '\f';
        } else if (L.LastChar == 'v') {
          L.StringVal += '\v';
        } else if (L.LastChar == '\\') {
          L.StringVal += '\\';
        } else if (L.LastChar == '"') {
          L.StringVal += '"';
        } else if (L.LastChar == '\'') {
          L.StringVal += '\'';
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
                         static_cast<char>(L.LastChar) +
                         "' in string literal.\n  Hint: Recognized escapes "
                         "are \\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\\" \\'. "
                         "Did you mean to write a literal backslash? Use "
                         "'\\\\' instead.", SLM::ErrorCategory::SYNTAX_ERROR);
          L.StringVal += L.LastChar;
        }
      } else {
        L.StringVal += L.LastChar;
      }
      L.LastChar = L.Source->get();
      L.CurCol++;
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

    if (L.LastChar == EOF) {
      // Ran off the end of the file with no closing '"' anywhere -- there is
      // truly nothing left to lex, so end the token stream here exactly as
      // the pre-existing behavior did.
      LogErrorAt(L.CurLoc, ErrCode::UnterminatedLiteral,
                 std::string("Unterminated string literal") +
                     UnterminatedStringHint, SLM::ErrorCategory::SYNTAX_ERROR);
      return TOK_EOF;
    }

    if (L.LastChar == '\n') {
      // Hit end-of-line before a closing '"'. Report it at the opening-quote
      // location (CurLoc was snapshotted there at the top of gettok() and is
      // untouched since) and return the partial token WITHOUT consuming the
      // newline. Leaving '\n' in LastChar means the *next* gettok() call
      // walks into it via the ordinary isspace()/whitespace-skip path at the
      // top of this function, which correctly bumps CurLine and resets
      // CurCol -- so lexing (and line tracking) of the rest of the file
      // recovers cleanly instead of desyncing for every line that follows.
      LogErrorAt(L.CurLoc, ErrCode::UnterminatedLiteral,
                 std::string("Unterminated string literal") +
                     UnterminatedStringHint, SLM::ErrorCategory::SYNTAX_ERROR);
      return TOK_STRING_LITERAL;
    }

    // Consume closing "
    L.LastChar = L.Source->get();
    L.CurCol++;

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
  if (L.LastChar == '\'') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '\'') {
      // Empty literal '' -- consume the closing quote so the lexer doesn't
      // desync, report, and recover with a placeholder value.
      LogErrorAt(L.CurLoc, ErrCode::InvalidCharLiteral,
                 "Empty char literal.\n  Hint: A char literal must contain "
                 "exactly one character, e.g. 'a'.", SLM::ErrorCategory::SYNTAX_ERROR);
      L.LastChar = L.Source->get();
      L.CurCol++;
      L.CharVal = '\0';
      return TOK_CHAR_LITERAL;
    }

    if (L.LastChar == EOF || L.LastChar == '\n') {
      // Same rationale as the unterminated-string case above: don't consume
      // the newline, so the next gettok() call's whitespace-skip recovers
      // line tracking cleanly instead of desyncing for the rest of the file.
      LogErrorAt(L.CurLoc, ErrCode::UnterminatedLiteral,
                 std::string("Unterminated char literal") +
                     "\n  Hint: Add a closing ''' after the character.", SLM::ErrorCategory::SYNTAX_ERROR);
      return L.LastChar == EOF ? TOK_EOF : TOK_CHAR_LITERAL;
    }

    if (L.LastChar == '\\') {
      SourceLocation EscapeLoc{L.CurLine, L.CurCol, ""};
      L.LastChar = L.Source->get();
      L.CurCol++;
      if (L.LastChar == 'n') {
        L.CharVal = '\n';
      } else if (L.LastChar == 't') {
        L.CharVal = '\t';
      } else if (L.LastChar == 'r') {
        L.CharVal = '\r';
      } else if (L.LastChar == '0') {
        L.CharVal = '\0';
      } else if (L.LastChar == 'a') {
        L.CharVal = '\a';
      } else if (L.LastChar == 'b') {
        L.CharVal = '\b';
      } else if (L.LastChar == 'f') {
        L.CharVal = '\f';
      } else if (L.LastChar == 'v') {
        L.CharVal = '\v';
      } else if (L.LastChar == '\\') {
        L.CharVal = '\\';
      } else if (L.LastChar == '"') {
        L.CharVal = '"';
      } else if (L.LastChar == '\'') {
        L.CharVal = '\'';
      } else {
        LogErrorAt(EscapeLoc, ErrCode::InvalidEscapeSequence,
                   std::string("Unknown escape sequence '\\") +
                       static_cast<char>(L.LastChar) +
                       "' in char literal.\n  Hint: Recognized escapes are "
                       "\\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\\" \\'.", SLM::ErrorCategory::SYNTAX_ERROR);
        L.CharVal = static_cast<char>(L.LastChar);
      }
    } else {
      L.CharVal = static_cast<char>(L.LastChar);
    }

    // Consume the character (or the last byte of the escape) just recorded.
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar != '\'') {
      // More than one character before the closing quote, e.g. 'ab'. Skip
      // forward to the next ''' (or EOF/newline) so the rest of the file
      // keeps lexing from a sane point instead of every later token being
      // offset by the stray characters.
      LogErrorAt(L.CurLoc, ErrCode::InvalidCharLiteral,
                 "Char literal must contain exactly one character.\n  Hint: "
                 "Use a string literal (\"ab\") for multiple characters, or "
                 "check for a missing closing '.", SLM::ErrorCategory::SYNTAX_ERROR);
      while (L.LastChar != '\'' && L.LastChar != EOF && L.LastChar != '\n') {
        L.LastChar = L.Source->get();
        L.CurCol++;
      }
      if (L.LastChar == '\'') {
        L.LastChar = L.Source->get();
        L.CurCol++;
      }
      return TOK_CHAR_LITERAL;
    }

    // Consume closing '
    L.LastChar = L.Source->get();
    L.CurCol++;

    return TOK_CHAR_LITERAL;
  }

  // Ellipsis '...' (variadic marker in extern fn) must be checked before '..'
  // (range operator) so three dots aren't consumed as range + stray dot.
  if (L.LastChar == '.' && L.Source->peek() == '.') {
    // Peek at the character after the second dot
    L.Source->get(); L.CurCol++;
    int third  = L.Source->peek();
    if (third == '.') {
      L.Source->get(); L.CurCol++; // consume third dot
      L.LastChar = L.Source->get(); L.CurCol++;
      return TOK_ELLIPSIS;
    }
    // Only two dots — range operator
    L.LastChar = L.Source->get(); L.CurCol++;
    return TOK_RANGE;
  }

  // Numbers (SEM-001, docs/archive/ARCHITECTURE_OVERHAUL.md §8;
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
  if (isdigit(L.LastChar) || (L.LastChar == '.' && isdigit(L.Source->peek()))) {
    SourceLocation NumLoc = L.CurLoc; // stamped at the top of gettokScan()
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
      while (IsDigit(L.LastChar) || L.LastChar == '_') {
        if (L.LastChar != '_')
          Clean += static_cast<char>(L.LastChar);
        Spelling += static_cast<char>(L.LastChar);
        L.LastChar = L.Source->get();
        L.CurCol++;
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
      if (L.LastChar != '.')
        return;
      int Next = L.Source->peek();
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
    if (L.LastChar == '0' &&
        (L.Source->peek() == 'x' || L.Source->peek() == 'X' ||
         L.Source->peek() == 'b' || L.Source->peek() == 'B' ||
         L.Source->peek() == 'o' || L.Source->peek() == 'O')) {
      Spelling += '0';
      L.LastChar = L.Source->get(); L.CurCol++; // consume '0'
      char RadixChar = static_cast<char>(L.LastChar);
      Spelling += RadixChar;
      L.LastChar = L.Source->get(); L.CurCol++; // consume x/X/b/B/o/O
      bool IsHex = (RadixChar == 'x' || RadixChar == 'X');
      bool IsBin = (RadixChar == 'b' || RadixChar == 'B');
      L.NumLitRadix = IsHex ? 16 : IsBin ? 2 : 8;

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
      if (isalnum(L.LastChar))
        InvalidRadixDigitError(NumLoc, static_cast<char>(L.LastChar),
                               L.NumLitRadix, Spelling)
            .raise();
      else if (DigitsConsumed == 0)
        MalformedRadixPrefixError(NumLoc, Spelling).raise();

      RejectStrayPoint(/*Radix=*/true, /*AlreadyFloat=*/false);
      L.NumLitSpelling = Spelling;
      L.NumLitClean = Clean;
      return TOK_INT_LITERAL;
    }

    // Plain decimal: an integer (`[0-9_]+`), or one of the two float forms.
    L.NumLitRadix = 10;
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
    if (L.LastChar == '.' && L.Source->peek() != '.' &&
        isdigit(L.Source->peek())) {
      IsFloat = true;
      Spelling += '.';
      Clean += '.';
      L.LastChar = L.Source->get(); L.CurCol++;
      ScanDigitRun(IsDecDigit); // at least one digit guaranteed by the peek() above
    }

    // Exponent: optional after the dotted form, but it's what makes the
    // DOTLESS form a float at all (if we reach EOF/an operator here with
    // IsFloat still false, this is just a plain decimal integer). Once
    // 'e'/'E' appears immediately here, it is ALWAYS committed to as an
    // exponent marker -- never silently reinterpreted as a separate
    // following token -- see MalformedExponentError's own doc comment.
    if (L.LastChar == 'e' || L.LastChar == 'E') {
      IsFloat = true;
      Spelling += static_cast<char>(L.LastChar);
      Clean += static_cast<char>(L.LastChar);
      L.LastChar = L.Source->get(); L.CurCol++;
      if (L.LastChar == '+' || L.LastChar == '-') {
        Spelling += static_cast<char>(L.LastChar);
        Clean += static_cast<char>(L.LastChar);
        L.LastChar = L.Source->get(); L.CurCol++;
      }
      if (ScanDigitRun(IsDecDigit) == 0)
        MalformedExponentError(NumLoc, Spelling).raise();
    }

    RejectStrayPoint(/*Radix=*/false, IsFloat);
    L.NumLitSpelling = Spelling;
    L.NumLitClean = Clean;
    return IsFloat ? TOK_NUMBER : TOK_INT_LITERAL;
  }

  if (L.LastChar == '=') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_EQ;
    }

    return TOK_ASSIGN;
  }

  if (L.LastChar == ':') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == ':') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_SCOPE;
    }

    return TOK_COLON;
  }

  if (L.LastChar == '!') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_NEQ;
    }

    return '!';
  }

  if (L.LastChar == '>') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_GEQ;
    }

    if (L.LastChar == '>') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      if (L.LastChar == '=') {
        L.LastChar = L.Source->get();
        L.CurCol++;
        return TOK_RSHIFT_ASSIGN;
      }
      return TOK_RSHIFT;
    }

    return '>';
  }

  if (L.LastChar == '<') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_LEQ;
    }

    if (L.LastChar == '<') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      if (L.LastChar == '=') {
        L.LastChar = L.Source->get();
        L.CurCol++;
        return TOK_LSHIFT_ASSIGN;
      }
      return TOK_LSHIFT;
    }

    return '<';
  }

  if (L.LastChar == '&') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '&') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_AND;
    }

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_AND_ASSIGN;
    }

    return '&';
  }

  if (L.LastChar == '|') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '|') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_OR;
    }

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_OR_ASSIGN;
    }

    return '|';
  }

  if (L.LastChar == '^') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_XOR_ASSIGN;
    }

    return '^';
  }

  if (L.LastChar == '+') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '+') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_PLUS_PLUS;
    }

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_PLUS_ASSIGN;
    }

    return '+';
  }

  if (L.LastChar == '-') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '-') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_MINUS_MINUS;
    }

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_MINUS_ASSIGN;
    }

    if (L.LastChar == '>') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_ARROW;
    }

    return '-';
  }

  if (L.LastChar == '*') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_MUL_ASSIGN;
    }

    if (L.LastChar == '*') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      if (L.LastChar == '=') {
        L.LastChar = L.Source->get();
        L.CurCol++;
        return TOK_POWER_ASSIGN;
      }
      return TOK_POWER;
    }

    return '*';
  }

  if (L.LastChar == '%') {
    L.LastChar = L.Source->get();
    L.CurCol++;

    if (L.LastChar == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_MOD_ASSIGN;
    }

    return '%';
  }

  if (L.LastChar == '/') {
    if (L.Source->peek() == '/') {
      do {
        L.LastChar = L.Source->get();
        L.CurCol++;
      } while (L.LastChar != EOF && L.LastChar != '\n' && L.LastChar != '\r');

      if (L.LastChar != EOF)
        return gettokScan(L);
    } else if (L.Source->peek() == '=') {
      L.LastChar = L.Source->get();
      L.CurCol++;
      L.LastChar = L.Source->get();
      L.CurCol++;
      return TOK_DIV_ASSIGN;
    }
  }

  if (L.LastChar == '@') {
    L.LastChar = L.Source->get();
    L.CurCol++;
    return TOK_AT;
  }

  if (L.LastChar == EOF)
    return TOK_EOF;

  // Handle ASCII characters
  int ThisChar = L.LastChar;
  L.LastChar = L.Source->get();
  L.CurCol++;

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
 * character actually read via L.Source->get(), and every branch reads
 * through the last character of its token before returning) -- i.e.
 * exactly the exclusive end column an LSP Range expects, and previously
 * only identifiers stamped explicitly (issue #243). The few branches that
 * intentionally stop short of a full token (unterminated string/char
 * literal recovery) never consume a mid-token newline, so CurLine is
 * always still the token's start line there too -- a same-line span is a
 * safe invariant to rely on unconditionally, not just for identifiers.
 */
int gettok(LexerState &L) {
  // CurLoc still describes the token about to become the previous one, with
  // its end stamped below by the call that scanned it.
  L.PrevTokEndLine = L.CurLoc.EndLine;
  L.PrevTokEndCol = L.CurLoc.EndCol;
  int Tok = gettokScan(L);
  L.CurLoc.EndLine = L.CurLine;
  L.CurLoc.EndCol = L.CurCol;
  return Tok;
}

