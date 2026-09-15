/**
 * @file Formatter.cpp
 * @brief Indentation-only source formatter (doc 09/doc 02 §D of turf-lsp's
 *        implementation plan). See Formatter.h for the design rationale.
 */

#include "Formatter.h"
#include "Lexer.h"
#include "Parser.h"

#include <cstring>
#include <sstream>
#include <unordered_map>

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

  // Save every piece of lexer/parser global state this touches, mirroring
  // turf-lsp's CompilerService::analyze() -- format() must be safe to call
  // from the SAME worker thread the LSP already serializes all other
  // compiler access onto, without corrupting whatever analysis is
  // (sequentially) running around it.
  std::istream *SavedSourceFile = SourceFile;
  std::string SavedSourceFilename = SourceFilename;
  SourceLocation SavedCurLoc = CurLoc;
  int SavedCurTok = CurTok;

  std::istringstream Buffer{std::string(Source)};
  SetSourceBuffer(Buffer);
  SourceFilename = "<format>";
  resetLexer();
  CurTok = 0;
  getNextToken();

  int LastSeenLine = -1;
  while (CurTok != TOK_EOF) {
    int Line = CurLoc.Line;
    if (Line != LastSeenLine) {
      int Raw = g_braceDepth;
      int Display = (isCloseBracket(CurTok) && Raw > 0) ? Raw - 1 : Raw;
      LineIndent[Line] = {Display, Raw};
      LastSeenLine = Line;
    }
    getNextToken();
  }

  SourceFile = SavedSourceFile;
  SourceFilename = SavedSourceFilename;
  CurLoc = SavedCurLoc;
  CurTok = SavedCurTok;

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

} // namespace

FormatResult format(std::string_view Source, const FormatOptions &Options) {
  if (Source.empty()) {
    return {Options.InsertFinalNewline ? "" : "", true};
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

  return {std::move(Out), true};
}

} // namespace turf::format
