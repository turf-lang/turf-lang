/**
 * @file Formatter.cpp
 * @brief Indentation-only source formatter (doc 09/doc 02 §D of turf-lsp's
 *        implementation plan). See Formatter.h for the design rationale.
 */

#include "tooling/Formatter.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "turf/FrontEndIsolation.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace turf::format {

namespace {

bool isCloseBracket(int Tok) { return Tok == '}' || Tok == ')' || Tok == ']'; }

// Per code line (a line with at least one real token): the depth that
// line's OWN leading whitespace should use (`Display`, pre-adjusted for a
// leading closer -- see below), and the raw, unadjusted depth (`Raw`) --
// needed separately because a comment-only or blank GAP line between two
// code lines must inherit the depth that was in effect AT THAT POINT in
// the token stream, which is the raw depth of whichever code line comes
// NEXT, not that next line's own (possibly closer-adjusted) display
// depth. Getting this wrong was a real bug caught by testing: a comment
// on its own line directly after an opening `{` was landing at the
// OUTER depth instead of the new block's inner one.
struct LineDepth { int Display; int Raw; };

// Everything below that drives the lexer does it inside
// turf::detail::runIsolated(): a caller in the middle of its own lexing (the
// language server's worker, the facade's host compile) must find the front
// end's globals as it left them, and the one place that knows which globals
// those are is the query facade's GlobalStateScope (INV-API-1). A hand-written
// save/restore here would go stale the day the lexer gains a global.

// Runs the real lexer/token-advance machinery (getNextToken(), the exact
// same entry point the actual parser uses) over `Source` and records, for
// every line that has at least one real token, its LineDepth.
//
// getNextToken() (ParserCore.cpp) updates g_braceDepth based on the token
// being LEFT, one call in arrears of CurTok -- so at the moment CurTok
// first becomes some token on a new line, g_braceDepth already reflects
// every earlier line's net effect, but NOT yet the current token's own
// (if the current token is itself an open/close bracket, its effect
// applies on the NEXT call). For a line whose first token is a closing
// bracket, that means g_braceDepth is still one too DEEP for THAT LINE's
// own indent (it hasn't "left" the closer yet) -- Display subtracts one
// (floored at 0) to align the closer with its opener's own indent, the
// standard "hanging brace" convention every C-like formatter uses. Raw
// keeps the un-adjusted value, since it's what was genuinely in effect
// through the end of the PREVIOUS line -- correct for gap-filling
// purposes regardless of whether THIS line happens to be a closer.
std::unordered_map<int, LineDepth> computeLineIndents(std::string_view Source) {
  std::unordered_map<int, LineDepth> LineIndent;

  turf::detail::runIsolated([&] {
    std::istringstream Buffer{std::string(Source)};
    LexerState Lexer(Buffer, "<format>");
    LexerScope Scope(Lexer);
    SourceFilename = "<format>";
    resetLexer();
    CurTok = 0;
    getNextToken();

    int LastSeenLine = -1;
    while (CurTok != TOK_EOF) {
      int Line = Lex().CurLoc.Line;
      if (Line != LastSeenLine) {
        int Raw = g_braceDepth;
        int Display = (isCloseBracket(CurTok) && Raw > 0) ? Raw - 1 : Raw;
        LineIndent[Line] = {Display, Raw};
        LastSeenLine = Line;
      }
      getNextToken();
    }
  });

  return LineIndent;
}

// Splits `Source` into physical lines, remembering whether the file
// predominantly used CRLF so the output can preserve it -- checked only
// once, on the first line ending found, rather than per-line (a file
// doesn't usually mix conventions, and this isn't the place to silently
// normalize one that does).
std::vector<std::string> splitLines(std::string_view Source, bool &UsesCRLF) {
  UsesCRLF = false;
  std::vector<std::string> Lines;
  size_t Start = 0;
  bool DetectedEol = false;
  while (Start <= Source.size()) {
    size_t Nl = Source.find('\n', Start);
    std::string_view LineView = Source.substr(
        Start, Nl == std::string_view::npos ? std::string_view::npos : Nl - Start);
    if (!LineView.empty() && LineView.back() == '\r') {
      if (!DetectedEol) { UsesCRLF = true; DetectedEol = true; }
      LineView.remove_suffix(1);
    } else if (!DetectedEol && Nl != std::string_view::npos) {
      DetectedEol = true; // first real EOL found was a bare \n
    }
    Lines.emplace_back(LineView);
    if (Nl == std::string_view::npos) break;
    Start = Nl + 1;
  }
  // A trailing '\n' produces one extra empty "line" after it via the loop
  // above (the substr from just-past-the-last-'\n' to end-of-string) --
  // drop it here rather than complicating the loop, so line COUNT matches
  // physical line count exactly (needed for LineIndent's 1-indexed keys
  // to line up).
  if (!Lines.empty() && Lines.back().empty() && !Source.empty() &&
      (Source.back() == '\n')) {
    Lines.pop_back();
  }
  return Lines;
}

std::string trimTrailing(const std::string &Line) {
  size_t End = Line.find_last_not_of(" \t");
  return End == std::string::npos ? std::string() : Line.substr(0, End + 1);
}

bool isBlank(const std::string &Line) {
  return Line.find_first_not_of(" \t") == std::string::npos;
}

// Defined with the verification code below; format() asks it first whether
// the source lexes at all.
bool tokenKeys(std::string_view Source, std::vector<std::string> &Keys, std::string &Why);

} // namespace

FormatResult format(std::string_view Source, const FormatOptions &Options) {
  if (Source.empty()) {
    return {Options.InsertFinalNewline ? "" : "", true, ""};
  }

  // A source the lexer rejects is not formatted: its bracket depths are not
  // trustworthy, and the lexer would otherwise end the process on the first
  // such error (see tokenKeys()). This is the "couldn't be tokenized at all"
  // case the header always said Ok = false was for.
  {
    std::vector<std::string> Ignored;
    std::string Why;
    if (!tokenKeys(Source, Ignored, Why))
      return {"", false, "it has a lexical error (" + Why + ")", /*Unlexable=*/true};
  }

  std::unordered_map<int, LineDepth> LineIndent = computeLineIndents(Source);

  bool UsesCRLF = false;
  std::vector<std::string> Lines = splitLines(Source, UsesCRLF);

  // Gap lines (blank or comment-only -- the lexer never emits a token for
  // either, so they have no LineIndent entry of their own) inherit the
  // RAW depth of the NEXT code line, not the previous one: that raw value
  // already reflects everything through the end of whatever preceded the
  // gap (see computeLineIndents()'s doc comment on why Raw vs Display
  // matters here). Computed as a single backward pass so a gap right
  // before a closing brace correctly gets the INNER depth, not the
  // closer's own outer one.
  std::vector<int> FillDepth(Lines.size() + 1, 0);
  {
    int NextRaw = 0;
    for (int Line = static_cast<int>(Lines.size()); Line >= 1; --Line) {
      auto It = LineIndent.find(Line);
      if (It != LineIndent.end()) NextRaw = It->second.Raw;
      FillDepth[Line] = NextRaw;
    }
  }

  std::string IndentUnit = Options.InsertSpaces
                                ? std::string(static_cast<size_t>(Options.IndentWidth), ' ')
                                : "\t";

  std::string Out;
  Out.reserve(Source.size());
  int BlankRun = 0;
  bool WroteAnyLine = false;

  for (size_t i = 0; i < Lines.size(); ++i) {
    std::string Line = Options.TrimTrailingWhitespace ? trimTrailing(Lines[i]) : Lines[i];
    int LineNo = static_cast<int>(i) + 1;

    if (isBlank(Line)) {
      // Collapse runs of 2+ blank lines to 1 (doc 09's rule); a blank
      // line is emitted as a truly empty line, never indented whitespace.
      ++BlankRun;
      if (BlankRun > 1 || !WroteAnyLine) continue; // also drops leading blank lines
      Out += UsesCRLF ? "\r\n" : "\n";
      continue;
    }
    BlankRun = 0;

    auto It = LineIndent.find(LineNo);
    int Depth = (It != LineIndent.end()) ? It->second.Display : FillDepth[LineNo];

    for (int d = 0; d < Depth; ++d) Out += IndentUnit;
    size_t FirstNonWs = Line.find_first_not_of(" \t");
    Out += Line.substr(FirstNonWs);
    Out += UsesCRLF ? "\r\n" : "\n";
    WroteAnyLine = true;
  }

  // Trailing blank lines were already skipped by the "collapse" logic
  // above only down to 1; strip that last one entirely plus normalize to
  // exactly one final newline (or none), per InsertFinalNewline.
  while (!Out.empty() && Out.size() >= 2 &&
         ((UsesCRLF && Out.compare(Out.size() - 4, 4, "\r\n\r\n") == 0) ||
          (!UsesCRLF && Out.size() >= 2 && Out[Out.size() - 1] == '\n' && Out[Out.size() - 2] == '\n'))) {
    Out.pop_back();
    if (UsesCRLF && !Out.empty() && Out.back() == '\r') Out.pop_back();
  }
  if (!Options.InsertFinalNewline) {
    if (UsesCRLF && Out.size() >= 2 && Out.compare(Out.size() - 2, 2, "\r\n") == 0) {
      Out.resize(Out.size() - 2);
    } else if (!Out.empty() && Out.back() == '\n') {
      Out.pop_back();
    }
  }

  return {std::move(Out), true, ""};
}

namespace {

// The token stream the REAL lexer reads from `Source`, one key per token:
// the kind, plus the value for the kinds that carry one. Never a position, so
// two texts that differ only in whitespace give equal streams -- and one that
// differs in a name, a literal or an operator does not. Isolated from the
// caller's globals the same way computeLineIndents() is.
//
// Returns false, with the first error in `Why`, if the source does not lex
// cleanly. That is the one place the formatter learns a file is not valid
// TurfLang, and it must not take the process with it: a TurfError raised
// outside a recovery guard prints and calls exit(1), which for `turf fmt` over
// a project would end the run at the first broken file and for the language
// server would kill the server. So the lexing runs inside the same setjmp
// guard the parser uses, against the isolated scope's own diagnostic list --
// nothing the lexer reports reaches the user as a compiler error, since the
// formatter's own message says what it could not do.
struct LexJob {
  std::vector<std::string> *Keys;
};

// Runs inside the guard; an error raised by getNextToken() skips the rest of it.
void lexJob(LexJob &Job) {
  CurTok = 0;
  getNextToken();
  while (CurTok != TOK_EOF) {
    std::string Key = CurTok >= 0 ? std::string(1, static_cast<char>(CurTok))
                                  : "token" + std::to_string(CurTok);
    switch (CurTok) {
    case TOK_IDENTIFIER: Key += ":" + Lex().IdentifierStr; break;
    case TOK_STRING_LITERAL: Key += ":" + Lex().StringVal; break;
    case TOK_INT_LITERAL:
    case TOK_NUMBER: Key += ":" + Lex().NumLitSpelling; break;
    case TOK_CHAR_LITERAL: Key += ":" + std::string(1, Lex().CharVal); break;
    case TOK_BOOL_LITERAL: Key += Lex().BoolVal ? ":true" : ":false"; break;
    default: break;
    }
    Job.Keys->push_back(std::move(Key));
    getNextToken();
  }
}

bool tokenKeys(std::string_view Source, std::vector<std::string> &Keys, std::string &Why) {
  bool Clean = true;

  turf::detail::runIsolated([&] {
    std::istringstream Buffer{std::string(Source)};
    LexerState Lexer(Buffer, "<format>");
    LexerScope Scope(Lexer);
    SourceFilename = "<format>";
    resetLexer();

    LexJob Job{&Keys};
    g_recoverActive = true;
    try {
      lexJob(Job);
    } catch (const RecoverUnwind &) {
    }
    g_recoverActive = false;

    // A lexical error that raised stopped the loop above; one that was only
    // recorded (an unterminated string) let it finish. Either way the file is
    // not valid, and formatting it would indent by brackets the lexer could
    // not honestly count.
    for (const auto &D : DiagnosticEngine::getDiagnostics()) {
      if (D.IsWarning)
        continue;
      Clean = false;
      std::string Msg = D.Msg.substr(0, D.Msg.find('\n'));
      Why = "line " + std::to_string(D.Loc.Line) + ": " + Msg;
      break;
    }
  });

  return Clean;
}

std::string withoutWhitespace(std::string_view Text) {
  std::string Out;
  Out.reserve(Text.size());
  for (char C : Text)
    if (C != ' ' && C != '\t' && C != '\r' && C != '\n')
      Out += C;
  return Out;
}

int firstDifferingLine(std::string_view A, std::string_view B) {
  int Line = 1;
  size_t N = std::min(A.size(), B.size());
  for (size_t I = 0; I < N; ++I) {
    if (A[I] != B[I])
      return Line;
    if (A[I] == '\n')
      ++Line;
  }
  return Line;
}

} // namespace

std::string verifyFormatting(std::string_view Original, std::string_view Formatted,
                             const FormatOptions &Options) {
  // 1. Content. Whitespace is the only thing a formatter may touch.
  if (withoutWhitespace(Original) != withoutWhitespace(Formatted))
    return "the formatter changed something other than whitespace";

  // 2. The program. Equal non-whitespace bytes still allow a changed token
  // stream when whitespace moved INSIDE a token, which is the case this
  // exists for.
  std::vector<std::string> Before, After;
  std::string Why;
  if (!tokenKeys(Original, Before, Why))
    return "the original does not lex cleanly (" + Why + ")";
  if (!tokenKeys(Formatted, After, Why))
    return "the formatted text does not lex cleanly (" + Why + ")";
  if (Before != After) {
    size_t I = 0;
    while (I < Before.size() && I < After.size() && Before[I] == After[I])
      ++I;
    return "the formatter changed the token stream (token " + std::to_string(I + 1) + ": '" +
           (I < Before.size() ? Before[I] : "<end>") + "' became '" +
           (I < After.size() ? After[I] : "<end>") + "')";
  }

  // 3. Stability. A second pass must find nothing left to do, or running
  // `turf fmt --check` right after `turf fmt` would report the file again.
  FormatResult Again = format(Formatted, Options);
  if (!Again.Ok || Again.Text != Formatted)
    return "formatting is not idempotent (a second pass changes line " +
           std::to_string(firstDifferingLine(Formatted, Again.Text)) + ")";
  return "";
}

FormatResult formatChecked(std::string_view Source, const FormatOptions &Options) {
  FormatResult R = format(Source, Options);
  if (!R.Ok)
    return R;
  if (std::string Problem = verifyFormatting(Source, R.Text, Options); !Problem.empty())
    return {"", false, "formatter bug: " + Problem};
  return R;
}

} // namespace turf::format
