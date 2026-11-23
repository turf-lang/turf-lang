/**
 * @file SLMIntegration.cpp
 * @brief Integration logic for the Small Language Model.
 * @details Bridges the compiler diagnostics system with the SLM backend.
 *          Handles prompt construction, error classification, dynamic
 *          context gathering, and JSON response parsing for AI suggestions.
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

#include "slm/SLMIntegration.h"
#include "diagnostics/Colors.h"
#include "diagnostics/ErrorCategory.h"
#include "diagnostics/ErrorCodes.h" // InstalledDocPath()
#include "frontend/Lexer.h"
#include "slm/Sandbox.h"
#include "slm/SLMEngine.h"
#include "frontend/AST.h"
#include "llvm/ADT/SmallString.h"
#include <map>
#include <set>
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/ADT/StringExtras.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static std::string stripAnsiCodes(const std::string &input) {
  static const std::regex ansiPattern("\\033\\[[0-9;]*m");
  return std::regex_replace(input, ansiPattern, "");
}

// Reads an external documentation file into a string.
// Returns empty string on failure (file missing, unreadable, etc.).
// A relative path (`docs/slm/structs.txt`) is resolved against the
// installation root, not the current directory: --smart run from any
// directory but the repository root used to prompt the model without its
// category doc (InstalledDocPath(), ErrorCodes.h).
static std::string loadDocFile(const std::string &path) {
  std::ifstream file(InstalledDocPath(path));
  if (!file.is_open())
    return "";

  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

namespace SLM {

// The source around the error, exactly as the file has it: the lines
// [errorLine - contextSize, errorLine + contextSize] joined with '\n', with no
// numbers, no header and no marker on any line.
//
// Until the model stopped reporting line numbers (SLM_IMPROVEMENT_PLAN
// workstream B) every line carried a `>> 21 | ` / `   20 | ` gutter so it could
// say where the fix went. Afterwards the gutter was pure noise the model had to
// be told NOT to copy (rule 2), and the cause of a whole failure mode
// (stripLineNumberPrefix() exists for it): a model that echoes `12 |` into
// original_code makes its copy match nothing. With the text verbatim, a copy of
// any line IS a valid original_code, so there is nothing to strip and nothing
// to forget. Which line the error is on travels in its own field
// ("error_line", see buildUserPrompt()), so the line's text is never altered.
std::string getCodeContext(int errorLine, int contextSize) {
  if (SourceLines.empty()) {
    return "";
  }

  int startLine = std::max(1, errorLine - contextSize);
  int endLine =
      std::min(static_cast<int>(SourceLines.size()), errorLine + contextSize);

  std::string context;
  context.reserve(512);
  for (int i = startLine; i <= endLine; ++i) {
    if (i > startLine)
      context += "\n";
    context += SourceLines[i - 1];
  }
  return context;
}

// Returns the context size (± lines) appropriate for the given error category.
// Tighter windows keep the model focused on the broken statement;
// broader windows let it trace type declarations and scope origins.
static int contextSizeForCategory(ErrorCategory cat) {
  switch (cat) {
  // Tight (±3): error is self-contained on a single statement
  case ErrorCategory::SYNTAX_ERROR:
  case ErrorCategory::BUILTIN_FUNCTION:
  case ErrorCategory::ARITHMETIC:
  case ErrorCategory::STRING_CONVERSION:
  case ErrorCategory::CONTROL_FLOW:
    return 3;

  // Broad (±8): need to trace type origin or scope declarations
  case ErrorCategory::TYPE_MISMATCH:
  case ErrorCategory::ARRAY_ERROR:
  case ErrorCategory::GENERAL:
    return 8;

  // Medium (±5): call site + nearby declaration usually sufficient ─
  case ErrorCategory::FUNCTION_SIGNATURE:
  case ErrorCategory::DECLARATION:
  case ErrorCategory::UNDECLARED_VAR:
  case ErrorCategory::MEMORY_ERROR:
    return 5;
  }
  return 5; // unreachable, but satisfies -Wreturn-type
}

// ── What the model is told besides the code window (C4 / C5) ────────────────
//
// Each fact is a field of its own in the user prompt, never folded into the
// "code" string: the code must stay a verbatim copy of the file so that a copy
// of any line of it is a valid original_code, and a model that has to tell
// code from commentary inside one string reads the commentary as code.

// One line the diagnostic points at -- the declaration an error refers back to,
// a candidate overload -- with the reason the raise site gave for it. `code` is
// the line exactly as it is in its file; it is empty when that file's text is
// not available, in which case `note` alone still carries the information
// (a candidate's note IS its signature).
struct PinnedLine {
  std::string code;
  std::string note;
};

// How many related lines reach the model. A diagnostic can name a dozen
// overloads; past a few the rest cost tokens and tell it nothing new.
static constexpr size_t kMaxPinnedLines = 6;

// A location's source text from ITS file. DiagnosticEngine reports locations
// of imported modules (the callee of a call, a type declared elsewhere) with
// that module's line numbers, and the old pinned-lines code indexed the MAIN
// file's lines with them: it showed an unrelated line of the program as "the
// candidate". AllSourceLines is the persistent per-file cache, so the right
// line is still there even though the module finished parsing long ago.
static std::string relatedLineText(const RelatedLocation &rel) {
  const std::vector<std::string> *lines = &SourceLines;
  if (!rel.Loc.Filename.empty() && rel.Loc.Filename != SourceFilename) {
    auto it = AllSourceLines.find(rel.Loc.Filename);
    if (it == AllSourceLines.end())
      return "";
    lines = &it->second;
  }
  if (rel.Loc.Line < 1 || rel.Loc.Line > static_cast<int>(lines->size()))
    return "";
  return (*lines)[rel.Loc.Line - 1];
}

// True when `rel` points into the file being fixed. Only those can be an
// anchor for locating the model's copy, since the copy is searched for in
// SourceLines.
static bool relatedIsInMainFile(const RelatedLocation &rel) {
  return rel.Loc.Filename.empty() || rel.Loc.Filename == SourceFilename;
}

// The related locations as the model sees them. Takes the full
// RelatedLocation list, not just line numbers: each Description ("declared
// here", "candidate: fn f(int)", "first freed here", ...) is a label every
// .withRelated() call site already computes. A line that is also inside the
// code window is kept, because the label is the point (which of these lines is
// "the declaration"?); two entries with the same text and label are one.
static std::vector<PinnedLine>
collectPinnedLines(const std::vector<RelatedLocation> &related) {
  std::vector<PinnedLine> out;
  for (const auto &rel : related) {
    PinnedLine p{relatedLineText(rel), rel.Description};
    if (p.code.empty() && p.note.empty())
      continue;
    bool dup = false;
    for (const auto &q : out)
      dup = dup || (q.code == p.code && q.note == p.note);
    if (!dup && out.size() < kMaxPinnedLines)
      out.push_back(std::move(p));
  }
  return out;
}

// True for a declaration header line that starts at column 0. Turf's top-level
// declarations are never indented (`turf --format` keeps them so), which is
// what makes "the nearest header above the error" a reliable way to find the
// declaration an error sits in without parsing anything.
static bool isDeclHeader(const std::string &line, std::string &kind) {
  static const char *kinds[] = {"fn", "struct", "enum", "error", "machine",
                                "operator"};
  size_t at = 0;
  if (line.compare(0, 4, "pub ") == 0)
    at = 4;
  else if (line.compare(0, 7, "public ") == 0)
    at = 7;
  for (const char *k : kinds) {
    const size_t n = std::strlen(k);
    if (line.compare(at, n, k) == 0 &&
        (line.size() == at + n || !(std::isalnum(static_cast<unsigned char>(
                                        line[at + n])) ||
                                    line[at + n] == '_'))) {
      kind = k;
      return true;
    }
  }
  return false;
}

// The declaration the error line sits in -- `fn Box.count(): string`,
// `struct Point` -- as its header up to (not including) the opening brace, and
// the line it starts on. Empty when the error is between declarations. A fix
// to a `return` or a call is only as good as the signature it must agree with,
// and in a function longer than the code window that signature is not shown.
//
// Walks up from the error line to the nearest column-0 header. A column-0 `}`
// above the error line means that line closed an earlier declaration, so the
// error is not inside one. (An error ON a column-0 `}` -- "missing return" is
// reported there -- is inside the declaration that brace closes, so that one
// line is skipped.) A signature that continues onto the next lines is joined,
// up to four lines.
static std::string enclosingDeclaration(int errorLine, int &headerLine) {
  headerLine = 0;
  const int n = static_cast<int>(SourceLines.size());
  if (errorLine < 1 || errorLine > n)
    return "";
  int i = errorLine;
  if (!SourceLines[i - 1].empty() && SourceLines[i - 1][0] == '}')
    --i;
  for (; i >= 1; --i) {
    const std::string &line = SourceLines[i - 1];
    if (line.empty() || line[0] == ' ' || line[0] == '\t')
      continue;
    std::string kind;
    if (isDeclHeader(line, kind)) {
      std::string sig;
      for (int j = i; j <= std::min(n, i + 3); ++j) {
        const std::string &part = SourceLines[j - 1];
        const size_t brace = part.find('{');
        std::string piece = part.substr(0, brace);
        const size_t first = piece.find_first_not_of(" \t");
        if (first != std::string::npos)
          piece = piece.substr(first);
        const size_t last = piece.find_last_not_of(" \t");
        piece = last == std::string::npos ? "" : piece.substr(0, last + 1);
        if (!sig.empty() && !piece.empty())
          sig += " ";
        sig += piece;
        if (brace != std::string::npos || (!part.empty() && part.back() == ';'))
          break;
      }
      headerLine = i;
      return sig;
    }
    if (line[0] == '}')
      return "";
  }
  return "";
}


static std::string escapeJsonString(const std::string &src) {
  std::string result;
  for (char c : src) {
    if (c == '\\') {
      result += "\\\\";
    } else if (c == '"') {
      result += "\\\"";
    } else if (c == '\n') {
      result += "\\n";
    } else if (c == '\r') {
      result += "\\r";
    } else if (c == '\t') {
      result += "\\t";
    } else {
      result += c;
    }
  }
  return result;
}

// Defensive cleanup, kept as a FALLBACK now that the model is no longer shown
// line numbers (getCodeContext() prints the code verbatim, C5). It was written
// for a failure mode of the old display: the model echoed the "N |  code"
// gutter that prefixed every context line back inside original_code or
// new_code, e.g. "6 |     mut b: int = 5;" instead of just
// "    mut b: int = 5;". A model can still produce that shape from habit (it has
// seen `12 | code` listings in training), and that prefix is never valid
// Turf-Lang syntax at the start of a line, so stripping it here is safe and
// turns an otherwise guaranteed sandbox-parse failure (new_code), or a copy
// that matches nothing in the source (original_code), into a usable patch.
// The replay case method_block.echoed_gutter keeps it honest.
//
// A multi-line value echoes the prefix on EVERY line ("11 | fn ...",
// "12 |     return ...", "13 | }"); stripping only the first (issue #267)
// left "12 |" on the rest, a guaranteed parse failure the model was then
// told was its own mistake. Every line is stripped, but only when every
// non-blank line carries a prefix: a lone "1 | flags" in the middle of a real
// block is code, not an echo, and a single-line value keeps the old rule.
static std::string stripLineNumberPrefix(const std::string &text) {
  static const std::regex prefixPattern(
      "^[ \\t]*(?:>>|\\*)?[ \\t]*[0-9]+[ \\t]*\\|[ \\t]?");
  std::vector<std::string> lines;
  size_t pos = 0;
  while (true) {
    size_t nl = text.find('\n', pos);
    lines.push_back(text.substr(pos, nl == std::string::npos ? nl : nl - pos));
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  bool allPrefixed = true;
  for (const auto &l : lines)
    if (l.find_first_not_of(" \t\r") != std::string::npos &&
        !std::regex_search(l, prefixPattern))
      allPrefixed = false;
  if (!allPrefixed)
    return std::regex_replace(text, prefixPattern, "",
                              std::regex_constants::format_first_only);
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i)
      out += "\n";
    out += std::regex_replace(lines[i], prefixPattern, "",
                              std::regex_constants::format_first_only);
  }
  return out;
}

// Defensive cleanup for another known small-model failure mode: wrapping
// new_code (or original_code) in a markdown code fence (```turf ... ``` or
// ``` ... ```) despite the rules saying "no markdown code blocks". A leading
// or trailing fence line is never valid Turf-Lang syntax, so stripping it
// here is safe and turns an otherwise guaranteed sandbox-parse failure (the
// parser chokes on a literal "```" token) into a usable patch. Observed
// especially often on multi-line values.
static std::string stripMarkdownFences(const std::string &text) {
  static const std::regex leadingFence("^\\s*```[A-Za-z]*[ \\t]*\\n?");
  static const std::regex trailingFence("\\n?[ \\t]*```\\s*$");
  std::string result =
      std::regex_replace(text, leadingFence, "", std::regex_constants::format_first_only);
  result =
      std::regex_replace(result, trailingFence, "", std::regex_constants::format_first_only);
  return result;
}

static std::string extractJsonFieldString(const std::string &json,
                                          const std::string &field) {
  std::string key = "\"" + field + "\":";
  size_t pos = json.find(key);
  if (pos == std::string::npos)
    return "";
  pos += key.length();

  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
    pos++;
  }
  if (pos >= json.length() || json[pos] != '"')
    return "";

  pos++;
  size_t endPos = pos;
  bool escaped = false;
  while (endPos < json.length()) {
    if (json[endPos] == '\\' && !escaped) {
      escaped = true;
    } else if (json[endPos] == '"' && !escaped) {
      break;
    } else {
      escaped = false;
    }
    endPos++;
  }

  std::string result = json.substr(pos, endPos - pos);
  size_t escapePos = 0;

  while ((escapePos = result.find('\\', escapePos)) != std::string::npos) {
    if (escapePos + 1 < result.length()) {
      char nextChar = result[escapePos + 1];
      if (nextChar == 'n') {
        result.replace(escapePos, 2, "\n"); escapePos += 1;
      } else if (nextChar == 't') {
        result.replace(escapePos, 2, "\t"); escapePos += 1;
      } else if (nextChar == '"') {
        result.replace(escapePos, 2, "\""); escapePos += 1;
      } else if (nextChar == '\\') {
        result.replace(escapePos, 2, "\\"); escapePos += 1;
      } else {
        escapePos++;
      }
    } else {
      break;
    }
  }
  return result;
}

// Fills the model-produced fields of `Out` from one suggestion JSON object
// (a model response, or a cache file -- which spells the category key
// `categoryKey` differently, hence the parameter).
//
// A real JSON parser first (issue #267). The hand-rolled extractors above
// decode only \n \t \" \\, but grammars/slm_suggestion.gbnf also lets the
// model emit \r, \/, \b, \f and \uXXXX -- and \u003c/\u003e for `<`/`>`
// is a common JSON habit that lands squarely on generic syntax (`fn<T>`),
// so a correct fix reached the sandbox with a literal "\u003c" in it. They
// also find a key by raw substring, so a string value that happens to
// contain `"new_code":` (an explanation quoting the schema) shadows the real
// field. The extractors stay as the fallback for a response that is not one
// well-formed object (no grammar loaded, or generation cut short), which is
// exactly what they were written for.
// `Cached`: Json is a cache entry, whose `summary` and `explanation` are the
// ones written after the sandbox accepted the fix (#364). From the model,
// `reasoning` is its private scratch (an older response's `explanation` is
// read as that too), and anything else it says about the fix is ignored.
static void parseSuggestionJson(const std::string &Json,
                                const std::string &categoryKey,
                                AISuggestion &Out, bool Cached = false) {
  std::function<std::string(const char *)> Str;
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  const llvm::json::Object *Obj = Parsed ? Parsed->getAsObject() : nullptr;
  if (!Parsed)
    llvm::consumeError(Parsed.takeError());
  if (Obj)
    Str = [&](const char *Key) -> std::string {
      if (auto S = Obj->getString(Key))
        return S->str();
      return "";
    };
  else
    Str = [&](const char *Key) { return extractJsonFieldString(Json, Key); };
  Out.fix_mode = Str("fix_mode");
  Out.original_code = Str("original_code");
  Out.new_code = Str("new_code");
  Out.self_reported_category = Str(categoryKey.c_str());
  if (Cached) {
    Out.summary = Str("summary");
    Out.explanation = Str("explanation");
  } else {
    Out.reasoning = Str("reasoning");
    if (Out.reasoning.empty())
      Out.reasoning = Str("explanation");
  }
}

// ── Finding `original_code` in the source ───────────────────────────────────
//
// The model reports no line numbers (SLM_IMPROVEMENT_PLAN workstream B): it
// copies the lines it wants to change and the harness looks them up. These
// helpers are that lookup, kept free of the sandbox so they can be reasoned
// about -- and tested through the replay harness -- on their own.

// Splits `text` on '\n' (a trailing '\r' on a line is dropped).
static std::vector<std::string> splitTextLines(const std::string &text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (true) {
    size_t nl = text.find('\n', pos);
    std::string line =
        text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  return lines;
}

static bool isBlankLine(const std::string &s) {
  return s.find_first_not_of(" \t\r") == std::string::npos;
}

// A line as the locator compares it: surrounding whitespace dropped and every
// inner run of whitespace reduced to one space, so a copy that differs only in
// indentation, tabs versus spaces, or `a  =  b` spacing is still an exact
// copy. Turf does not care about any of those, and a model cannot be expected
// to reproduce them: it sees the source through a tokenizer.
static std::string normalizeLine(const std::string &s) {
  std::string out;
  bool pendingSpace = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace)
      out += ' ';
    pendingSpace = false;
    out += c;
  }
  return out;
}

// Drops wholly blank lines from both ends of `text` (not indentation: a
// leading-space first line is real code). A model often pads its string with a
// newline, and a blank edge line would otherwise be spliced into the program
// or make a copy miss its source.
static std::string trimBlankEdgeLines(const std::string &text) {
  std::vector<std::string> lines = splitTextLines(text);
  size_t lo = 0, hi = lines.size();
  while (lo < hi && isBlankLine(lines[lo]))
    ++lo;
  while (hi > lo && isBlankLine(lines[hi - 1]))
    --hi;
  std::string out;
  for (size_t i = lo; i < hi; ++i) {
    if (i > lo)
      out += "\n";
    out += lines[i];
  }
  return out;
}

// How alike two source lines are, ignoring whitespace: 1.0 for identical,
// else 1 - (edit distance / longer length).
static double lineSimilarity(const std::string &a, const std::string &b) {
  std::string x = normalizeLine(a), y = normalizeLine(b);
  if (x == y)
    return 1.0;
  if (x.empty() || y.empty())
    return 0.0;
  std::vector<size_t> prev(y.size() + 1), cur(y.size() + 1);
  for (size_t j = 0; j <= y.size(); ++j)
    prev[j] = j;
  for (size_t i = 1; i <= x.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= y.size(); ++j)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                         prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1)});
    std::swap(prev, cur);
  }
  return 1.0 - static_cast<double>(prev[y.size()]) /
                   static_cast<double>(std::max(x.size(), y.size()));
}

// One place `original_code` was found, best first in locateOriginal()'s
// answer. [Start, End] is the 1-based inclusive source range the model's copy
// stands for; the harness replaces exactly that range with `new_code`.
struct OriginalMatch {
  enum Kind {
    Exact,    // every line equal after normalizeLine()
    Fuzzy,    // aligned line by line, each at least 0.6 alike (a typo'd copy)
    Insertion // original_code was empty: after the error line
  };
  Kind How;
  int Start, End;
};

// How far a range is from the diagnostic: 0 when it contains the line, else
// the gap in lines.
static int rangeDistance(int start, int end, int line) {
  if (line >= start && line <= end)
    return 0;
  return line < start ? start - line : line - end;
}

// Exact copies of `orig` (its blank lines ignored) in the source. A source
// line that is blank may sit between two matched lines -- the model leaves out
// or invents blank lines freely -- but the range starts and ends on a real
// line. Every start position is reported, so the caller can pick the nearest.
static std::vector<std::pair<int, int>>
findExactCopies(const std::vector<std::string> &orig) {
  std::vector<std::pair<int, int>> found;
  const int n = static_cast<int>(SourceLines.size());
  if (orig.empty())
    return found;
  for (int s = 1; s <= n; ++s) {
    if (normalizeLine(SourceLines[s - 1]) != orig[0])
      continue;
    int line = s;
    size_t matched = 1;
    while (matched < orig.size() && line < n) {
      ++line;
      if (isBlankLine(SourceLines[line - 1]))
        continue;
      if (normalizeLine(SourceLines[line - 1]) != orig[matched])
        break;
      ++matched;
    }
    if (matched == orig.size())
      found.push_back({s, line});
  }
  return found;
}

// The fuzzy half of the locator, the old alignReplacement() turned around to
// align the model's COPY instead of its replacement (issue #267 wrote it for
// replacements, but a copy is what a model can be held to). Finds the source
// window whose lines best line up with `orig`'s lines, among windows near
// `anchor` (within kFuzzyReach lines of it, at most two lines longer than the
// copy). A one-typo copy matches; a paraphrase does not.
//
// Scored as a sequence alignment of the copy's lines against each candidate
// window [s, e]: an aligned pair earns its similarity (pairs under 0.6 don't
// align; a line of two characters or fewer, like `}`, earns 0.3 of it, since a
// lone brace matches every block's end); a window line left unaligned costs
// 0.5 (0.25 if blank), because the splice deletes it; a copy line left
// unaligned is a line the model invented and costs nothing. The best window
// must score at least 0.6 -- one convincing match -- or there is no answer.
// Ties go to the smaller window, then the one nearest `anchor`. `outScore`
// receives the winner's score, which orders the answers for different anchors.
//
// A wrong answer here is as safe as a wrong model line number: the result is
// only a candidate verifySuggestion() hands to the full sandbox re-check.
static bool alignOriginal(const std::vector<std::string> &repl, int anchor,
                          int &outStart, int &outEnd, double &outScore) {
  constexpr int kFuzzyReach = 10;
  const int n = static_cast<int>(SourceLines.size());
  const int k = static_cast<int>(repl.size());
  if (anchor < 1 || anchor > n || k == 0)
    return false;

  // Every window lies inside [lo, hi]; similarity of each copy line against
  // each source line there is computed once (an edit distance per cell).
  const int lo = std::max(1, anchor - kFuzzyReach - k - 1);
  const int hi = std::min(n, anchor + kFuzzyReach + k + 1);
  std::vector<std::vector<double>> pairScore(k,
                                              std::vector<double>(hi - lo + 1));
  for (int i = 0; i < k; ++i) {
    const size_t len = normalizeLine(repl[i]).size();
    for (int line = lo; line <= hi; ++line) {
      double sim = lineSimilarity(repl[i], SourceLines[line - 1]);
      pairScore[i][line - lo] =
          sim < 0.6 ? -1.0 : (len <= 2 ? sim * 0.3 : sim); // -1: no alignment
    }
  }

  double bestScore = 0.0;
  int bestStart = 0, bestEnd = 0;
  for (int s = lo; s <= hi; ++s) {
    for (int e = s; e <= std::min(hi, s + k + 1); ++e) {
      if (rangeDistance(s, e, anchor) > kFuzzyReach)
        continue;
      const int m = e - s + 1;
      // dp[i][j]: best score aligning repl[0..i) with window lines [0..j).
      std::vector<std::vector<double>> dp(k + 1, std::vector<double>(m + 1));
      for (int j = 1; j <= m; ++j)
        dp[0][j] = dp[0][j - 1] -
                   (isBlankLine(SourceLines[s + j - 2]) ? 0.25 : 0.5);
      for (int i = 1; i <= k; ++i) {
        dp[i][0] = dp[i - 1][0];
        for (int j = 1; j <= m; ++j) {
          const int line = s + j - 1;
          double best =
              std::max(dp[i - 1][j],
                       dp[i][j - 1] -
                           (isBlankLine(SourceLines[line - 1]) ? 0.25 : 0.5));
          double ps = pairScore[i - 1][line - lo];
          if (ps >= 0)
            best = std::max(best, dp[i - 1][j - 1] + ps);
          dp[i][j] = best;
        }
      }
      double score = dp[k][m];
      auto dist = [&](int a, int b) { return rangeDistance(a, b, anchor); };
      bool better = score > bestScore + 1e-9 ||
                    (bestStart != 0 && std::abs(score - bestScore) <= 1e-9 &&
                     (m < bestEnd - bestStart + 1 ||
                      (m == bestEnd - bestStart + 1 &&
                       dist(s, e) < dist(bestStart, bestEnd))));
      if (better && score >= 0.6) {
        bestScore = score;
        bestStart = s;
        bestEnd = e;
      }
    }
  }
  if (bestStart == 0)
    return false;
  // A blank edge line never earns score (only costs), so dropping it can only
  // improve the alignment -- and the splice should not eat a blank line the
  // model never copied.
  while (bestStart < bestEnd && isBlankLine(SourceLines[bestStart - 1]))
    ++bestStart;
  while (bestEnd > bestStart && isBlankLine(SourceLines[bestEnd - 1]))
    --bestEnd;
  outStart = bestStart;
  outEnd = bestEnd;
  outScore = bestScore;
  return true;
}

// Where in the program `original_code` is: the model's own answer to "which
// lines do you want to change", as text instead of numbers. Returns the
// places it could mean, best first, or nothing when it appears nowhere.
//
//  1. Exact: every non-blank line equal to a source line after normalizeLine()
//     (indentation, tab/space and spacing differences do not matter). Several
//     exact copies -- the same `return x;` in two functions -- are ordered by
//     distance from the error line, or from a pinned related line (the
//     declaration an error points back to), whichever is nearer; the nearest
//     wins and the rest follow, so a wrong guess costs one more sandbox run.
//  2. Otherwise fuzzy: alignOriginal() around the error line and around each
//     related line; answers are ordered by alignment score.
//  3. Empty `original_code` is an INSERTION: new_code goes on the line(s) right
//     after the error line, nowhere else. Anything the model wants placed
//     elsewhere it must anchor by copying the line it sits next to (and
//     repeating that line in new_code). Deleting lines is not expressible: an
//     empty new_code is rejected (verifySuggestion()) -- a forgotten field
//     would otherwise "fix" an error by deleting the line it is on, and pass
//     validation.
//  4. Not found: the list is empty. verifySuggestion() then splices nothing --
//     a copy that matches nowhere is a copying failure, and says so in its
//     feedback, instead of guessing a line for new_code.
static std::vector<OriginalMatch>
locateOriginal(const std::string &originalCode, int errorLine,
               const std::vector<RelatedLocation> &related) {
  std::vector<OriginalMatch> out;
  const int n = static_cast<int>(SourceLines.size());
  if (errorLine < 1 || errorLine > n)
    return out;

  std::vector<std::string> orig;
  for (const std::string &line : splitTextLines(originalCode))
    if (!isBlankLine(line))
      orig.push_back(normalizeLine(line));
  if (orig.empty()) {
    out.push_back({OriginalMatch::Insertion, errorLine, errorLine});
    return out;
  }

  std::vector<int> anchors{errorLine};
  for (const auto &rel : related)
    if (relatedIsInMainFile(rel) && rel.Loc.Line >= 1 && rel.Loc.Line <= n &&
        std::find(anchors.begin(), anchors.end(), rel.Loc.Line) == anchors.end())
      anchors.push_back(rel.Loc.Line);
  auto nearest = [&](int s, int e) {
    int best = rangeDistance(s, e, anchors[0]);
    for (int a : anchors)
      best = std::min(best, rangeDistance(s, e, a));
    return best;
  };

  std::vector<std::pair<int, int>> exact = findExactCopies(orig);
  if (!exact.empty()) {
    std::stable_sort(exact.begin(), exact.end(),
                     [&](const auto &a, const auto &b) {
                       int da = nearest(a.first, a.second);
                       int db = nearest(b.first, b.second);
                       if (da != db)
                         return da < db;
                       // Equally near: the one that actually holds the error
                       // line's neighbourhood, not a pinned declaration.
                       return rangeDistance(a.first, a.second, errorLine) <
                              rangeDistance(b.first, b.second, errorLine);
                     });
    for (const auto &r : exact)
      out.push_back({OriginalMatch::Exact, r.first, r.second});
    return out;
  }

  struct Scored {
    double score;
    int start, end;
  };
  std::vector<Scored> fuzzy;
  for (int anchor : anchors) {
    int s = 0, e = 0;
    double score = 0;
    if (!alignOriginal(orig, anchor, s, e, score))
      continue;
    bool dup = false;
    for (const auto &f : fuzzy)
      dup = dup || (f.start == s && f.end == e);
    if (!dup)
      fuzzy.push_back({score, s, e});
  }
  std::stable_sort(fuzzy.begin(), fuzzy.end(),
                   [](const Scored &a, const Scored &b) {
                     return a.score > b.score;
                   });
  for (const auto &f : fuzzy)
    out.push_back({OriginalMatch::Fuzzy, f.start, f.end});
  return out;
}

// `newCode` moved to the indentation the source actually has. A model that
// copies `    return self.n;` as `return self.n;` writes new_code unindented too,
// and splicing that as it is would leave the fix flush left in the middle of a
// method (it compiles, but it is what the user is shown). When every
// non-blank new_code line starts with the whitespace the model's own copy
// started with, that prefix is swapped for the source's; anything else is left
// exactly as written, since the shift would be a guess.
static std::string reindentNewCode(const std::string &newCode,
                                   const std::string &originalCode,
                                   const std::string &sourceFirstLine) {
  auto indentOf = [](const std::string &s) {
    return s.substr(0, s.find_first_not_of(" \t"));
  };
  std::string modelIndent;
  for (const std::string &line : splitTextLines(originalCode))
    if (!isBlankLine(line)) {
      modelIndent = indentOf(line);
      break;
    }
  const std::string sourceIndent = indentOf(sourceFirstLine);
  if (modelIndent == sourceIndent)
    return newCode;
  std::vector<std::string> lines = splitTextLines(newCode);
  for (const std::string &line : lines)
    if (!isBlankLine(line) && line.compare(0, modelIndent.size(), modelIndent) != 0)
      return newCode;
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i)
      out += "\n";
    if (!isBlankLine(lines[i]))
      out += sourceIndent + lines[i].substr(modelIndent.size());
  }
  return out;
}

// True when `a` and `b` are the same code once indentation, spacing and blank
// lines are ignored: the model "changed" nothing that matters to a compiler.
static bool sameCode(const std::string &a, const std::string &b) {
  auto norm = [](const std::string &s) {
    std::vector<std::string> out;
    for (const std::string &line : splitTextLines(s))
      if (!isBlankLine(line))
        out.push_back(normalizeLine(line));
    return out;
  };
  return norm(a) == norm(b);
}

// Computes a structural prior for confidence from shallow, pre-sandbox signals.
// finalizeConfidence() then folds in the sandbox's pass or fail. The result
// decides nothing: what is shown is decided by the Fix Score's tier (SL-1,
// diagnostics/FixScore.h). It is printed under TURF_SLM_DEBUG, and the
// --smart benchmark reads it for the old score's Brier score.
//
//   +0.4  Error category resolved by hardcoded rules (not SLM fallback)
//   +0.2  fix_mode == MODIFY
//   +0.2  new_code is non-empty (actual code, not an empty field)
//   +0.1  original_code is an exact copy of source lines -- the model read
//         the real program, which is the strongest cheap sign it is fixing
//         THIS program and not one from its training data. A fuzzy copy or an
//         insertion earns nothing: not a penalty, just no evidence.
//   +0.1  the edit is local: new_code is within one line of original_code's
//         length and not a bare brace. A regenerated declaration is not
//         wrong (the sandbox decides), but a small edit to what was copied is
//         the shape of a real fix. Replaces "a single clean line": a block
//         edit is now expected, a block rewrite is what is suspicious.
//   +0.1  new_code contains the error's keyword on a word boundary
//
// Hard 0.0, whatever else holds: new_code is original_code again once
// whitespace is ignored. A small-model failure mode that passes every other
// check while fixing nothing.
//
// Maximum score: 1.0 (the bonuses sum to 1.1 and are clamped, as before)
// Minimum: 0.0
static float computeStructuralPrior(const AISuggestion &s, ErrorCategory category,
                                    bool deterministic,
                                    const std::string &contextKeyword,
                                    int errorLine,
                                    const std::vector<RelatedLocation> &related) {
  float score = 0.0f;

  // +0.4 if the category was resolved by deterministic rules (not SLM fallback)
  if (deterministic && category != ErrorCategory::GENERAL)
    score += 0.4f;

  // +0.2 if the model decided a concrete patch is applicable
  if (s.fix_mode == "MODIFY")
    score += 0.2f;

  const std::string &newCode = s.new_code;
  if (newCode.empty())
    return std::min(score, 1.0f); // verifySuggestion() rejects it regardless

  // Hard-reject: an identical edit. Insertions (empty original_code) cannot be
  // identical to anything.
  if (!isBlankLine(s.original_code) && sameCode(s.original_code, newCode))
    return 0.0f; // can never clear the gate

  // +0.2 for a non-empty new_code
  score += 0.2f;

  // +0.1 if the copy is exact
  std::vector<OriginalMatch> where =
      locateOriginal(s.original_code, errorLine, related);
  if (!where.empty() && where.front().How == OriginalMatch::Exact)
    score += 0.1f;

  // +0.1 if the edit is local
  const int origLines = static_cast<int>(splitTextLines(s.original_code).size());
  const int newLines = static_cast<int>(splitTextLines(newCode).size());
  const bool isBareBrace = (newCode == "{" || newCode == "}");
  if (!isBlankLine(s.original_code) && !isBareBrace &&
      std::abs(newLines - origLines) <= 1)
    score += 0.1f;

  // +0.1 if new_code contains the keyword at a word boundary.
  // Raw substring matching is avoided because short identifiers (e.g. i, n)
  // appear inside unrelated tokens, making the signal nearly unconditional.
  if (!contextKeyword.empty()) {
    size_t kwPos = 0;
    while ((kwPos = newCode.find(contextKeyword, kwPos)) != std::string::npos) {
      // Check that the characters immediately surrounding the match are not
      // alphanumeric or underscore (i.e. it is a word boundary).
      bool leftOk = (kwPos == 0) ||
                    (!std::isalnum(static_cast<unsigned char>(newCode[kwPos - 1])) &&
                     newCode[kwPos - 1] != '_');
      bool rightOk =
          (kwPos + contextKeyword.size() >= newCode.size()) ||
          (!std::isalnum(static_cast<unsigned char>(
               newCode[kwPos + contextKeyword.size()])) &&
           newCode[kwPos + contextKeyword.size()] != '_');
      if (leftOk && rightOk) {
        score += 0.1f;
        break; // only count once
      }
      kwPos += contextKeyword.size();
    }
  }

  // Clamp to [0, 1]
  if (score > 1.0f)
    score = 1.0f;
  return score;
}

// Incorporates the strongest available signal — sandbox empirical validation —
// into the structural prior to produce the final confidence score.
//
// If sandbox validation passed, the fix has been proven correct by the actual
// compiler. The structural prior can only nudge the score for logging/ranking;
// it can no longer veto a proven-correct fix.
//
// If sandbox did not validate (NONE mode, or sandbox rejected the fix), the
// structural prior is all we have, capped at 0.5.
static float finalizeConfidence(float structuralPrior, bool sandboxValidated) {
  if (sandboxValidated) {
    // Proven-correct: clamp to at least 0.9, no longer blockable by the prior.
    return std::max(0.9f, structuralPrior);
  }
  // Not validated.
  return std::min(structuralPrior, 0.5f);
}

// ── Item 4: on-disk suggestion cache (~/.cache/turf/slm/) ───────────────────
//
// Keyed on (error code, normalized message, hash of the user-prompt body and
// of the documentation) -- §16.3 item 4's tuple, widened since -- so an
// IDENTICAL diagnostic (same code, same message text, same surrounding source,
// same facts and docs shown to the model) on a later compile skips
// the model call entirely. "Normalized message" here means exactly
// cleanError (already ANSI-code-stripped by stripAnsiCodes() before this
// ever runs): no further whitespace/number normalization is attempted, since
// a TurfError's message is already deterministic text for a given
// (construct, names, types) tuple with no built-in noise (timestamps,
// addresses, ...) that would need stripping first.
//
// Invalidation: item 4 says "on compiler version", but this project tracks
// no literal version string -- confirmed via grep, CMakeLists.txt has no
// CMAKE_PROJECT_VERSION or embedded git hash. The compiler BINARY's own
// size+mtime is used as the version fingerprint instead: a real rebuild
// always changes at least one of the two (a no-op `cmake --build` that
// touches nothing doesn't relink, so this doesn't over-invalidate either),
// and it costs nothing extra to maintain in lockstep. An ordinary
// engineering judgment call (SESSION_PROTOCOL.md §6.0 rule 2), not a
// FREEZE.md decision -- recorded here and in the SLM-001 change record.
static std::string fnv1aHex(const std::string &Data) {
  uint64_t Hash = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
  for (unsigned char C : Data) {
    Hash ^= C;
    Hash *= 1099511628211ULL; // FNV-1a 64-bit prime
  }
  std::ostringstream Out;
  Out << std::hex << std::setfill('0') << std::setw(16) << Hash;
  return Out.str();
}

// `PromptBody` is the whole diagnostic-dependent part of the user prompt
// (buildUserPrompt()): the code window and every structured fact beside it
// (enclosing declaration, receiver struct, known types, related lines, the
// hint). It used to be just the numbered code context, so a fact the model
// was shown but the key did not cover could change without the key changing,
// and a changed prompt would be answered from the cache with what the model
// said to the old one. `Docs` is the same for the system prompt: the pages
// under docs/slm/ are files read at run time, not part of the binary the
// fingerprint covers, so editing one used to change the prompt and not the key.
static std::string slmCacheKey(const std::string &ErrorCode,
                               const std::string &NormalizedMessage,
                               const std::string &PromptBody,
                               const std::string &Docs) {
  // '\x1f' (ASCII unit separator) as the join delimiter -- never appears in
  // any of the inputs (error codes are "E0NN", messages/context are
  // printable source text), so no input can be crafted to collide two
  // otherwise-different keys by shifting a delimiter.
  return fnv1aHex(ErrorCode + "\x1f" + NormalizedMessage + "\x1f" + PromptBody +
                  "\x1f" + Docs);
}

// The model --smart loads: the ONE .gguf file in <root>/models, where <root>
// is the directory above the one turfc is in (the checkout for build/turfc,
// the install for bin/turfc). The file's name does not matter, so a user can
// drop in any GGUF model llama.cpp can run; the recommended, tested one is
// RecommendedSmartModel. Exactly one, deliberately: with none there is
// nothing to run, and with two there is no telling which the user meant, so
// neither is guessed at. In both cases, and in a build without the model
// runtime, the compile carries on with the compiler's own diagnostics and one
// note says why. Resolved once per compile: this is reached once per
// diagnostic --smart tries to improve, and one explanation is enough.
static const char *const RecommendedSmartModel =
    "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf";

static bool resolveSmartModel(std::string &ModelPath) {
  static bool Resolved = false, Found = false;
  static std::string Cached;
  if (Resolved) {
    ModelPath = Cached;
    return Found;
  }
  Resolved = true;
  auto Note = [](const std::string &Why) {
    std::cerr << "note: " << Why
              << "; showing the compiler's own diagnostics.\n";
  };

  // A build without the runtime says so itself (SLMEngineDisabled.cpp), and
  // must not send the user after a model it could not run.
  if (!SLMEngine::builtIn()) {
    Found = true; // initialize() then prints its note and returns false
    return true;
  }

  // SL-4: TURF_SLM_MODEL=<file.gguf> names the model outright, for
  // scripts/slm/qualify_model.sh and the benchmark, which measure one model
  // without moving it into the install.
  if (const char *Forced = std::getenv("TURF_SLM_MODEL"); Forced && *Forced) {
    if (!llvm::sys::fs::is_regular_file(Forced)) {
      Note(std::string("TURF_SLM_MODEL names ") + Forced + ", which is not a file");
      return false;
    }
    Cached = Forced;
    ModelPath = Cached;
    Found = true;
    return true;
  }

  llvm::SmallString<128> Dir(CompilerExecutablePath);
  llvm::sys::fs::make_absolute(Dir);
  llvm::sys::path::remove_filename(Dir);
  llvm::sys::path::append(Dir, "..", "models");
  llvm::sys::path::remove_dots(Dir, /*remove_dot_dot=*/true);

  std::vector<std::string> Models;
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; !EC && It != End;
       It.increment(EC)) {
    llvm::StringRef Name = llvm::sys::path::filename(It->path());
    if (Name.starts_with(".") || !Name.ends_with_insensitive(".gguf"))
      continue;
    // status(), not the entry's own type: a model symlinked in from wherever
    // the user keeps their models is a model.
    llvm::sys::fs::file_status St;
    if (llvm::sys::fs::status(It->path(), St) ||
        !llvm::sys::fs::is_regular_file(St))
      continue;
    Models.push_back(Name.str());
  }
  std::sort(Models.begin(), Models.end());

  if (Models.empty()) {
    Note("--smart has no model to run. Put one .gguf file in " +
         std::string(Dir.str()) + " (recommended: " + RecommendedSmartModel +
         ")");
    return false;
  }
  if (Models.size() > 1) {
    std::string List;
    for (const std::string &M : Models)
      List += (List.empty() ? "" : ", ") + M;
    Note("--smart uses exactly one model and found " +
         std::to_string(Models.size()) + " in " + std::string(Dir.str()) +
         " (" + List + "). Leave one .gguf file there");
    return false;
  }
  llvm::sys::path::append(Dir, Models.front());
  Cached = std::string(Dir.str());
  ModelPath = Cached;
  Found = true;
  return true;
}

static std::string slmCacheDir() {
  const char *Home = std::getenv("HOME");
  if (!Home || !*Home)
    return ""; // no HOME -- caching is a pure optimization, disable cleanly
  return std::string(Home) + "/.cache/turf/slm";
}

// ── SL-4: which model, and may it run here ─────────────────────────────────

// The SHA-256 of the model file, hex. Hashing a 1 GB file takes seconds, so
// the result is kept beside the suggestion cache, keyed by the file's path,
// size and modification time. Empty when the file cannot be read.
static std::string modelSha256(const std::string &Path) {
  llvm::sys::fs::file_status St;
  if (llvm::sys::fs::status(Path, St))
    return "";
  const std::string Stamp =
      std::to_string(St.getSize()) + " " +
      std::to_string(St.getLastModificationTime().time_since_epoch().count());
  const std::string Dir = slmCacheDir();
  const std::string Memo = Dir.empty() ? "" : Dir + "/model-" + fnv1aHex(Path) + ".sha256";
  if (!Memo.empty()) {
    std::ifstream In(Memo);
    std::string Line;
    if (std::getline(In, Line) && Line.rfind(Stamp + " ", 0) == 0)
      return Line.substr(Stamp.size() + 1);
  }
  std::ifstream File(Path, std::ios::binary);
  if (!File)
    return "";
  llvm::SHA256 Hasher;
  std::vector<char> Buf(1 << 20);
  while (File) {
    File.read(Buf.data(), static_cast<std::streamsize>(Buf.size()));
    const std::streamsize Got = File.gcount();
    if (Got > 0)
      Hasher.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Buf.data()),
                                            static_cast<size_t>(Got)));
  }
  const std::string Hex = llvm::toHex(Hasher.final(), /*LowerCase=*/true);
  if (!Memo.empty() && !llvm::sys::fs::create_directories(Dir)) {
    std::ofstream Out(Memo, std::ios::trunc);
    Out << Stamp << " " << Hex << "\n";
  }
  return Hex;
}

// What answers depend on besides the prompt: the llama.cpp revision the
// runtime was built from and the model file's SHA-256. Part of the cache
// fingerprint, so an answer one model gave is never served for another.
// "replay" under TURF_SLM_REPLAY (no model runs).
static const std::string &smartModelIdentity() {
  static const std::string Id = [] {
    if (std::getenv("TURF_SLM_REPLAY"))
      return std::string("replay");
    std::string Path;
    const std::string Sha = resolveSmartModel(Path) ? modelSha256(Path) : "";
    return std::string("llama=") + TURF_LLAMA_REVISION + ";model=" + Sha;
  }();
  return Id;
}

// Qualified models: docs/slm/qualified_models.tsv, one line per model that
// met the published thresholds under scripts/slm/qualify_model.sh (its
// SHA-256 first, then its file name, the date and the record). Any other
// model still runs; one note says it is not qualified.
static void noteIfUnqualified(const std::string &Path) {
  static bool Noted = false;
  if (Noted)
    return;
  Noted = true;
  const std::string Sha = modelSha256(Path);
  std::ifstream List(InstalledDocPath("docs/slm/qualified_models.tsv"));
  std::string Line;
  while (std::getline(List, Line))
    if (!Sha.empty() && Line.rfind(Sha + "\t", 0) == 0)
      return;
  std::cerr << "note: --smart is running " << llvm::sys::path::filename(Path).str()
            << ", which is not a qualified model (scripts/slm/qualify_model.sh); "
               "its suggestions are checked the same way.\n";
}

// Bytes of memory the system can still give, or 0 when it cannot be told.
// TURF_SLM_ASSUME_FREE_MB overrides it (tests).
static uint64_t availableMemoryBytes() {
  if (const char *Forced = std::getenv("TURF_SLM_ASSUME_FREE_MB"))
    return static_cast<uint64_t>(std::strtoull(Forced, nullptr, 10)) << 20;
#if defined(__linux__)
  std::ifstream Info("/proc/meminfo");
  std::string Key;
  uint64_t Kb = 0;
  std::string Unit;
  while (Info >> Key >> Kb >> Unit)
    if (Key == "MemAvailable:")
      return Kb << 10;
  return 0;
#elif defined(__APPLE__)
  vm_statistics64_data_t Stats;
  mach_msg_type_number_t Count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&Stats), &Count) != KERN_SUCCESS)
    return 0;
  const uint64_t Page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  return (static_cast<uint64_t>(Stats.free_count) + Stats.inactive_count +
          Stats.purgeable_count) * Page;
#elif defined(_WIN32)
  MEMORYSTATUSEX M;
  M.dwLength = sizeof(M);
  return GlobalMemoryStatusEx(&M) ? static_cast<uint64_t>(M.ullAvailPhys) : 0;
#else
  return 0;
#endif
}

// SL-4 resource bound: the model's file plus room for its context (the KV
// cache at n_ctx 10240 and llama.cpp's buffers, kModelHeadroomMB) must fit
// in the memory still free, or the model is not loaded. A system that does
// not say how much is free is not refused.
static constexpr uint64_t kModelHeadroomMB = 512;
static bool enoughMemoryFor(const std::string &Path) {
  uint64_t Size = 0;
  if (llvm::sys::fs::file_size(Path, Size))
    return true;
  const uint64_t Free = availableMemoryBytes();
  const uint64_t Need = Size + (kModelHeadroomMB << 20);
  if (Free == 0 && !std::getenv("TURF_SLM_ASSUME_FREE_MB"))
    return true;
  if (Free >= Need)
    return true;
  std::cerr << "note: --smart needs about " << (Need >> 20) << " MB free to load "
            << llvm::sys::path::filename(Path).str() << " and " << (Free >> 20)
            << " MB is; showing the compiler's own diagnostics.\n";
  return false;
}

// See this section's own top comment for why the compiler binary's
// size+mtime (rather than a version string this project doesn't have) is
// the invalidation fingerprint. Computed fresh per call rather than once
// per process: fetchSuggestion() is never hot enough for one extra stat()
// to matter, and this keeps the cache free of its own global init-order
// state.
//
// The suggestion schema's version is part of the fingerprint line too. The
// binary's own size+mtime already changes with every rebuild, so on a dev
// machine it would invalidate a stale entry by itself -- but that is an
// accident of how it is computed, not a promise: a fingerprint that stops
// depending on the binary (a release build stamping a version, say) would
// otherwise let an entry in the previous field layout parse "successfully" as
// an empty edit. Bump kSlmSchemaVersion whenever the fields the model is asked
// for, or the cache file's own, change -- and when what it is SHOWN changes in
// a way the key (slmCacheKey()) does not already hash.
//   1: replacement_line / target_line / end_line / code_patch
//   2: original_code / new_code (workstream B)
//   3: the code is shown verbatim, with no line-number gutter, and the
//      diagnostic's facts are structured fields (workstream C, C4/C5)
//   4: reasoning / fix_mode / original_code / new_code / category; the
//      summary and explanation are written after the sandbox (#364)
static constexpr int kSlmSchemaVersion = 4;

static std::string slmCompilerFingerprint() {
  llvm::sys::fs::file_status Status;
  if (llvm::sys::fs::status(CompilerExecutablePath, Status))
    return ""; // stat() failed -- never matches a real cache entry below
  auto Mtime = Status.getLastModificationTime().time_since_epoch().count();
  return "schema" + std::to_string(kSlmSchemaVersion) + ":" +
         std::to_string(Status.getSize()) + ":" + std::to_string(Mtime) + ":" +
         smartModelIdentity();
}

static bool readSlmCache(const std::string &Key, AISuggestion &Out) {
  std::string Dir = slmCacheDir();
  if (Dir.empty())
    return false;
  std::ifstream File(Dir + "/" + Key + ".json");
  if (!File.is_open())
    return false;
  std::string FingerprintLine;
  if (!std::getline(File, FingerprintLine))
    return false;
  if (FingerprintLine != slmCompilerFingerprint())
    return false; // stale -- the compiler binary changed since this was written
  std::ostringstream Rest;
  Rest << File.rdbuf();
  std::string Json = Rest.str();
  if (Json.empty())
    return false;
  parseSuggestionJson(Json, "self_reported_category", Out, /*Cached=*/true);
  if (Out.fix_mode.empty())
    Out.fix_mode = "NONE";
  Out.valid = true;
  // Deliberately NOT cached: confidence, sandbox_validated. "Confidence
  // cannot be faked" (§16.1) -- finalizeConfidence() always recomputes both
  // fresh from THIS run's structural prior and THIS run's sandbox result,
  // exactly as if the model had just been called. A cache hit skips only the
  // expensive, purely-a-function-of-the-prompt model inference step, never
  // the cheap, must-stay-live trust computation downstream.
  return true;
}

static void writeSlmCache(const std::string &Key, const AISuggestion &S) {
  std::string Dir = slmCacheDir();
  if (Dir.empty())
    return;
  if (llvm::sys::fs::create_directories(Dir))
    return; // couldn't create the cache dir -- skip caching this run
  std::ofstream File(Dir + "/" + Key + ".json", std::ios::trunc);
  if (!File.is_open())
    return;
  File << slmCompilerFingerprint() << "\n"
       << "{"
       << "\"summary\":\"" << escapeJsonString(S.summary) << "\","
       << "\"explanation\":\"" << escapeJsonString(S.explanation) << "\","
       << "\"fix_mode\":\"" << escapeJsonString(S.fix_mode) << "\","
       << "\"original_code\":\"" << escapeJsonString(S.original_code) << "\","
       << "\"new_code\":\"" << escapeJsonString(S.new_code) << "\","
       << "\"self_reported_category\":\"" << escapeJsonString(S.self_reported_category)
       << "\"}\n";
}

static void evictSlmCache(const std::string &Key) {
  std::string Dir = slmCacheDir();
  if (!Dir.empty())
    (void)llvm::sys::fs::remove(Dir + "/" + Key + ".json");
}

// ── Negative cache: "the model could not fix this" (SLM plan, D4) ───────────
//
// Only a VALIDATED suggestion is cached above, so an error the model answers
// `NONE` to, or whose three attempts all fail the sandbox, paid the whole model
// cost (load + up to three generations) on every later `--smart` run. This
// remembers that outcome, separately, so a repeat run skips the model and shows
// the plain diagnostic -- exactly what the first run ended with.
//
// It is a different file from a positive entry (`<key>.neg`, next to
// `<key>.json`) and holds no suggestion at all, only the fingerprint line and
// the word `none` (the model answered NONE) or `failed` (it ran out of
// attempts). readSlmCache() never looks at it, so a negative can never be
// served as a fix: the worst a stale or wrong one can do is skip the model.
//
// It is the same key as the positive entry (the first-attempt diagnostic), and
// is consulted only on a first attempt, so a retry -- which exists because the
// previous suggestion failed -- is never blocked by one.
//
// Lifetime: kSlmNegativeTtl, measured from the file's mtime. A negative is a
// statement about a 1.5B model on one prompt, so it is allowed to go stale: 24
// hours is long enough that fixing a file by hand and re-running does not
// re-pay for the errors that are still there, and short enough that a model,
// prompt or docs change is picked up the next day. A compiler rebuild, a
// docs/slm edit or any change to the diagnostic's facts invalidates it at once
// (fingerprint line, and the key's own hash). Rewriting an entry resets its age.
// There is no bypass flag: delete ~/.cache/turf/slm to force a fresh run.
static constexpr std::chrono::hours kSlmNegativeTtl{24};

static std::string slmNegativePath(const std::string &Dir,
                                   const std::string &Key) {
  return Dir + "/" + Key + ".neg";
}

// True (and `Reason` set to "none" or "failed") when a live negative entry
// exists for `Key`. An expired or stale-fingerprint one is deleted on the way.
static bool readSlmNegativeCache(const std::string &Key, std::string &Reason) {
  std::string Dir = slmCacheDir();
  if (Dir.empty())
    return false;
  const std::string Path = slmNegativePath(Dir, Key);
  llvm::sys::fs::file_status Status;
  if (llvm::sys::fs::status(Path, Status))
    return false; // no such entry
  auto Age = std::chrono::system_clock::now() - Status.getLastModificationTime();
  std::ifstream File(Path);
  std::string FingerprintLine, ReasonLine;
  const bool wellFormed = Age < kSlmNegativeTtl && File.is_open() &&
                          std::getline(File, FingerprintLine) &&
                          FingerprintLine == slmCompilerFingerprint() &&
                          std::getline(File, ReasonLine) &&
                          (ReasonLine == "none" || ReasonLine == "failed");
  if (!wellFormed) {
    (void)llvm::sys::fs::remove(Path);
    return false;
  }
  Reason = ReasonLine;
  return true;
}

static void writeSlmNegativeCache(const std::string &Key,
                                  const std::string &Reason) {
  std::string Dir = slmCacheDir();
  if (Dir.empty() || llvm::sys::fs::create_directories(Dir))
    return;
  std::ofstream File(slmNegativePath(Dir, Key), std::ios::trunc);
  if (File.is_open())
    File << slmCompilerFingerprint() << "\n" << Reason << "\n";
}

static void evictSlmNegativeCache(const std::string &Key) {
  std::string Dir = slmCacheDir();
  if (!Dir.empty())
    (void)llvm::sys::fs::remove(slmNegativePath(Dir, Key));
}

// TURF_SLM_REPLAY runs bypass the whole cache (positive and negative), in both
// directions, so a test never depends on or pollutes ~/.cache/turf/slm.
// TURF_SLM_REPLAY_CACHE=1 turns that bypass off, for the one smoke test that
// has to exercise the cache itself (scripts/slm/slm_cache_smoke.sh, which points
// HOME at a temp directory first).
int ExtraSamples = 0;
// Which answer fetchSuggestion() is getting: 0 the greedy one, 1.. a
// --smart=thorough extra one (sampled, and never read from or written to the
// cache).
static int SampleIndex = 0;

// TURF_SLM_NO_CACHE: neither read nor write the suggestion cache, so every
// answer comes from the model (SL-4's determinism check: a cache would hide a
// model that answers differently twice).
static bool slmCacheBypassed() {
  if (std::getenv("TURF_SLM_NO_CACHE"))
    return true;
  return std::getenv("TURF_SLM_REPLAY") != nullptr &&
         std::getenv("TURF_SLM_REPLAY_CACHE") == nullptr;
}

// ── SL-4: the run's budget and its output conformance ──────────────────────

// One --smart run (validateAndApplySuggestion(): every attempt, the
// explanation, --smart=thorough's samples) gets kSmartBudgetSeconds of wall
// clock, TURF_SLM_BUDGET_SECONDS to change it. Past it no model call starts,
// a generation in progress stops (SLMEngine::setDeadline()), and a run with
// no validated fix yet ends with the compiler's own diagnostics. A fix that
// already passed is still shown (without an explanation if that did not fit).
static constexpr int kSmartBudgetSeconds = 180;
static std::chrono::steady_clock::time_point RunDeadline =
    std::chrono::steady_clock::time_point::max();

static int smartBudgetSeconds() {
  if (const char *E = std::getenv("TURF_SLM_BUDGET_SECONDS")) {
    const long V = std::strtol(E, nullptr, 10);
    if (V > 0)
      return static_cast<int>(V);
  }
  return kSmartBudgetSeconds;
}

static bool overBudget() { return std::chrono::steady_clock::now() >= RunDeadline; }

// Counts of what the model's answers looked like, per run: a rise is a harness
// regression (the issue-#267 class), not a model one. TURF_SLM_DEBUG prints
// them at the end of the run as `[SLM] conformance: ...`, which
// scripts/slm/slm_benchmark/run_benchmark.py sums per benchmark run.
//   empty            the model returned nothing
//   parse_failures   no JSON object, or one that does not parse
//   grammar_escapes  JSON that parses but breaks the grammar's shape (a field
//                    missing, fix_mode or category outside its values)
//   splice_*         an answer the harness could not place: original_code
//                    found nowhere, an empty new_code, new_code == original_code
struct Conformance {
  int Answers = 0, Empty = 0, ParseFailures = 0, GrammarEscapes = 0;
  int SpliceNotFound = 0, SpliceEmptyNewCode = 0, SpliceUnchanged = 0;
};
static Conformance Conf;

// Counts one answer against the keys its grammar requires; `Allowed` maps a
// key to the values it may take (empty: any string).
static void countAnswer(const std::string &Raw, const std::vector<std::string> &Keys,
                        const std::map<std::string, std::set<std::string>> &Allowed = {}) {
  ++Conf.Answers;
  if (Raw.find_first_not_of(" \t\r\n") == std::string::npos) {
    ++Conf.Empty;
    return;
  }
  const size_t From = Raw.find('{'), To = Raw.rfind('}');
  if (From == std::string::npos || To == std::string::npos || To < From) {
    ++Conf.ParseFailures;
    return;
  }
  auto Parsed = llvm::json::parse(Raw.substr(From, To - From + 1));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    ++Conf.ParseFailures;
    return;
  }
  const llvm::json::Object *Obj = Parsed->getAsObject();
  bool Escaped = !Obj;
  for (const auto &K : Keys) {
    if (Escaped)
      break;
    auto V = Obj->getString(K);
    auto A = Allowed.find(K);
    Escaped = !V || (A != Allowed.end() && !A->second.count(V->str()));
  }
  if (Escaped)
    ++Conf.GrammarEscapes;
}

static std::string conformanceLine() {
  return "[SLM] conformance: answers=" + std::to_string(Conf.Answers) +
         " empty=" + std::to_string(Conf.Empty) +
         " parse_failures=" + std::to_string(Conf.ParseFailures) +
         " grammar_escapes=" + std::to_string(Conf.GrammarEscapes) +
         " splice_not_found=" + std::to_string(Conf.SpliceNotFound) +
         " splice_empty_new_code=" + std::to_string(Conf.SpliceEmptyNewCode) +
         " splice_unchanged=" + std::to_string(Conf.SpliceUnchanged);
}

// TURF_SLM_REPLAY=<file>: answer each model call with the next recorded
// response from <file> (responses separated by a line that is exactly
// `%%`) instead of running the model. Everything after the model --
// parsing, splice-site resolution, the sandbox, the retry loop and its
// feedback -- runs unchanged, which is the point: until this existed, the
// only way to exercise that path was a live ~1GB model whose output is not
// reproducible, so a harness bug that discarded correct fixes (issue #267)
// had no regression test at all. Works in a -DTURF_SMART=OFF build too, and
// bypasses the suggestion cache in both directions so a test never depends
// on (or pollutes) ~/.cache/turf/slm. A call past the last response gets ""
// -- "the model produced nothing" -- which ends the retry loop.
static std::string nextReplayResponse() {
  static std::vector<std::string> Responses;
  static size_t Next = 0;
  static bool Loaded = false;
  if (!Loaded) {
    Loaded = true;
    std::ifstream File(std::getenv("TURF_SLM_REPLAY"));
    std::string Line, Current;
    while (std::getline(File, Line)) {
      if (Line == "%%") {
        Responses.push_back(Current);
        Current.clear();
      } else {
        Current += Line + "\n";
      }
    }
    if (Current.find_first_not_of(" \t\r\n") != std::string::npos)
      Responses.push_back(Current);
  }
  // TURF_SLM_REPLAY_DELAY_MS: each answer takes this long, as a slow model's
  // would, and one that would end past the run's budget is cut off empty, as
  // SLMEngine's deadline cuts a generation (SL-4's budget test).
  if (const char *Delay = std::getenv("TURF_SLM_REPLAY_DELAY_MS")) {
    const auto Until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(std::strtol(Delay, nullptr, 10));
    std::this_thread::sleep_until(std::min(Until, RunDeadline));
    if (Until > RunDeadline) {
      ++Next;
      return "";
    }
  }
  return Next < Responses.size() ? Responses[Next++] : "";
}

// ── The user prompt's JSON header ──────────────────────────────────────────

// Everything the model is told about one diagnostic. `code` is the verbatim
// window (getCodeContext()); every other member is a structured fact that is
// emitted only when it exists, so a diagnostic that has no enclosing
// declaration or no receiver struct simply has no such field.
struct PromptFacts {
  std::string error, hint;
  // The text of the line the diagnostic is on, unaltered: how the model learns
  // WHICH line of "code" the error is on now that the code carries no numbers
  // or markers (a marker inside the line would change the text it copies).
  std::string errorLine;
  std::string code;
  // `fn Box.count(): string` -- the declaration the error line sits in, only
  // when it is not already in the window (enclosingDeclaration()).
  std::string enclosing;
  // `struct Box { v: int, n: int }` -- the struct a field or method error is
  // about, from the raise site's own type information
  // (DiagnosticHints::ReceiverDecl).
  std::string receiver;
  // DiagnosticHints::KnownTypes: `x: int`, `n was declared with 'guess' ...`.
  std::vector<std::string> knownTypes;
  // SL-6: the structs a mismatch is about (`struct Point { x: int, y: int }`),
  // the called function's signature (`fn area(w: int, h: int): int`) and the
  // parameters of the function the error is in (`n: int`).
  std::vector<std::string> structs;
  std::string callee;
  std::vector<std::string> params;
  // The raise site's related locations (candidates, "declared here", ...).
  std::vector<PinnedLine> related;
};

static std::string jsonStringArray(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i)
    out += (i ? ", \"" : "\"") + escapeJsonString(items[i]) + "\"";
  return out + "]";
}

// The JSON object the user turn opens with. The code is a "\n"-escaped JSON
// string, not a raw block after the object: measured live on 8 programs
// (2026-10-05), the model found its copy in 7 of 8 either way, so the simpler
// form stayed. Field order matters to a
// small model: the question (error, hint), then what is known about it, and
// "code" last, right before the retry feedback and the schema, so the text it
// must copy from is the text nearest its answer.
//
// The prompt cache key is derived from this string (previousAttemptFailed
// empty), so a change to ANY fact -- a different enclosing declaration, a new
// receiver -- is a different key, instead of serving an answer the model gave
// for a prompt that no longer exists.
static std::string buildUserPrompt(const PromptFacts &f,
                                   const std::string &previousAttemptFailed) {
  std::string out = "{\n";
  out += "  \"error\": \"" + escapeJsonString(f.error) + "\",\n";
  out += "  \"hint\": \"" + escapeJsonString(f.hint) + "\",\n";
  out += "  \"error_line\": \"" + escapeJsonString(f.errorLine) + "\"";
  if (!f.enclosing.empty())
    out += ",\n  \"inside\": \"" + escapeJsonString(f.enclosing) + "\"";
  if (!f.receiver.empty())
    out += ",\n  \"receiver\": \"" + escapeJsonString(f.receiver) + "\"";
  if (!f.knownTypes.empty())
    out += ",\n  \"known_types\": " + jsonStringArray(f.knownTypes);
  if (!f.params.empty())
    out += ",\n  \"params\": " + jsonStringArray(f.params);
  if (!f.callee.empty())
    out += ",\n  \"callee\": \"" + escapeJsonString(f.callee) + "\"";
  if (!f.structs.empty())
    out += ",\n  \"structs\": " + jsonStringArray(f.structs);
  if (!f.related.empty()) {
    out += ",\n  \"related\": [";
    for (size_t i = 0; i < f.related.size(); ++i) {
      out += i ? ", {" : "{";
      bool first = true;
      if (!f.related[i].code.empty()) {
        out += "\"code\": \"" + escapeJsonString(f.related[i].code) + "\"";
        first = false;
      }
      if (!f.related[i].note.empty())
        out += std::string(first ? "" : ", ") + "\"note\": \"" +
               escapeJsonString(f.related[i].note) + "\"";
      out += "}";
    }
    out += "]";
  }
  out += ",\n  \"code\": \"" + escapeJsonString(f.code) + "\"";
  if (!previousAttemptFailed.empty())
    out += ",\n  \"previous_attempt_failed\": \"" +
           escapeJsonString(previousAttemptFailed) + "\"";
  out += "\n}\n";
  return out;
}

/**
 * @brief Fetches and parses an AI suggestion for a given compilation error.
 *
 * Extracts relevant code context, categorizes the error, queries the SLM,
 * and parses the JSON response into a structured AISuggestion.
 *
 * @param errorMessage The raw compiler error message.
 * @param errorLine The 1-based source line where the error occurred.
 * @return AISuggestion A structured AI response containing an explanation and
 * patch.
 */
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine,
                             const std::string &hint,
                             const std::vector<RelatedLocation> &related,
                             ErrorCategory category,
                             const std::string &previousAttemptFailed,
                             const DiagnosticHints &hints,
                             const std::string &errorCode) {
  AISuggestion suggestion;
  std::string cleanError = stripAnsiCodes(errorMessage);

  // Extract a keyword hint for doc-path routing (getCategoryDocPath's
  // constructDocs map) and the structural-prior patch-keyword bonus
  // (computeStructuralPrior above) -- independent of category classification.
  std::string contextKeyword =
      hints.Construct.empty() ? extractContextKeyword(cleanError) : hints.Construct;

  // `category` (the parameter) is the GROUND-TRUTH category: either the
  // raising TurfError subclass's own Category_ (Errors.h), or an explicit
  // argument the raw LogErrorAt() call site passed (Lexer.h) -- every call
  // site names one deliberately now (DIAG-001), so there is no text-matching
  // fallback left to run here. GENERAL is a real, audited answer for a site
  // (a genuine catch-all, or a judgment call documented at that site), not
  // "classification failed" -- see ErrorCategory.h's removed classifyError()
  // for why re-deriving a category from message text was deleted, not kept
  // as a fallback: it silently drifted out of sync with reworded messages
  // and had no way to recover once a real category was already known.
  bool categoryWasDeterministic = (category != ErrorCategory::GENERAL);

  // Context window + pinned lines, computed BEFORE the model/grammar load
  // below (moved up from "Step 3" further down -- see that step's own
  // comment for what each call does) so a cache lookup (item 4) can use the
  // same codeContext string as its key WITHOUT first paying the cost of
  // loading the GGUF model into RAM, which is the single most expensive
  // part of this function on a cold cache miss and pure waste on a hit.
  int contextSize = contextSizeForCategory(category);
  PromptFacts facts;
  facts.error = cleanError;
  facts.hint = hint;
  facts.code = getCodeContext(errorLine, contextSize);
  if (errorLine >= 1 && errorLine <= static_cast<int>(SourceLines.size()))
    facts.errorLine = SourceLines[errorLine - 1];
  {
    int headerLine = 0;
    const std::string sig = enclosingDeclaration(errorLine, headerLine);
    // Not repeated when the window already shows the header: the model has it
    // verbatim, and a second copy is just tokens.
    if (!sig.empty() && headerLine < errorLine - contextSize)
      facts.enclosing = sig;
  }
  facts.receiver = hints.ReceiverDecl;
  facts.knownTypes = hints.KnownTypes;
  facts.structs = hints.Structs;
  facts.callee = hints.Callee;
  facts.params = hints.Params;
  facts.related = collectPinnedLines(related);
  std::string categoryName = getCategoryName(category);
  // Everything in the prompt that depends on the diagnostic -- hashed into the
  // cache key below.
  const std::string promptBody = buildUserPrompt(facts, "");

  // The documentation the model is given, assembled BEFORE the cache lookup
  // (it is a few small file reads, nothing next to loading the model) because
  // the cache key covers it: an edit to a page under docs/slm/ changes what
  // the model would be told, and must not be answered from the cache with
  // what it said to the old page.
  //
  // 3. The most specific documentation available for the category.
  const std::string docPath = getCategoryDocPath(category, contextKeyword);

  // 4. Load the targeted documentation for this error category
  std::string docContent = loadDocFile(docPath);
  if (docContent.empty()) {
    // Fallback: try the general doc if the specific one is missing
    docContent = loadDocFile("docs/slm/general.txt");
  }
  if (docContent.empty()) {
    // Hard fallback: inline minimal reference
    docContent =
        "Turf is a statically-typed language with types: int, double, bool, "
        "string.\n"
        "Declarations: type name = value. Explicit casts: int(), double(), "
        "string().\n"
        "Control flow: if/else, while, break, continue, return.\n"
        "Functions: fn name(type param) -> type { return val }\n";
  }

  // The page for this exact error code (docs/slm/codes/Exxx.txt), ahead of
  // the category doc. All 144 pages were written for the model, with the
  // construct's correct syntax and the fix, but nothing ever loaded them
  // (getSlmCodeDocPath() had no caller): the model only saw the coarse
  // category doc -- for E138, declaration.txt, which never shows
  // `fn<T> Box.m()` -- and answered with `-> int` (issue #267). First,
  // because an over-long system prompt loses the end of its docs
  // (SLMEngine::generateSuggestion()), and the code's own page is the more
  // specific of the two.
  std::string codeDocPath;
  if (!errorCode.empty()) {
    codeDocPath = ErrorCodes::getSlmCodeDocPath(errorCode);
    std::string codeDoc = codeDocPath.empty() ? "" : loadDocFile(codeDocPath);
    if (codeDoc.empty())
      codeDocPath.clear();
    else
      docContent = "THIS ERROR (" + errorCode + "):\n" + codeDoc +
                   "\nGENERAL REFERENCE:\n" + docContent;
  }

  // The syntax card (docs/slm/syntax_card.txt, C2): the Turf every fix has to
  // be written in, always, ahead of both pages. The model was told to take its
  // syntax only from the documentation, and a code page shows the one construct
  // it is about -- E064's page says `expr to string` in a sentence, and the
  // model wrote `self.n.to_string()` three times in a row (issue #267 live
  // run). Ahead of everything because the engine drops tokens from the MIDDLE
  // of an over-long system prompt, never the head.
  // scripts/slm/slm_doc_examples_check.sh compiles every example in it.
  const std::string syntaxCard = loadDocFile("docs/slm/syntax_card.txt");
  if (!syntaxCard.empty())
    docContent = syntaxCard + "\n" + docContent;

  // Item 4 (caching): key on (error code, normalized message, hash of the
  // user-prompt body and of the documentation) -> the model's raw JSON
  // fields, persisted under
  // ~/.cache/turf/slm/, invalidated whenever the compiler binary itself
  // changes. Only consulted on a FIRST attempt (previousAttemptFailed
  // empty) -- a retry exists specifically because the last suggestion (quite
  // possibly this exact cached one) already failed sandbox validation, so
  // serving it again would just reproduce the same failure forever instead
  // of letting the retry reach the model with real feedback. See
  // slmCacheKey()/readSlmCache()/writeSlmCache() below for the on-disk
  // format and the version-fingerprint invalidation rule.
  //
  // Only ever written for a suggestion that validated (see
  // AISuggestion::cacheKey), so a hit is a known-good answer, not just the
  // model's last guess.
  const std::string cacheKey =
      slmCacheKey(errorCode, cleanError, promptBody, docContent);
  suggestion.cacheKey = cacheKey;
  const bool replaying = std::getenv("TURF_SLM_REPLAY") != nullptr;
  if (previousAttemptFailed.empty() && !slmCacheBypassed() && SampleIndex == 0) {
    AISuggestion cached;
    if (readSlmCache(cacheKey, cached)) {
      if (std::getenv("TURF_SLM_DEBUG")) {
        std::cerr << "[SLM] Cache hit -- skipping model load entirely.\n";
      }
      cached.cacheKey = cacheKey;
      cached.fromCache = true;
      return cached;
    }
    // A live negative entry (see readSlmNegativeCache()): the model already
    // failed on this exact diagnostic recently. Not valid and NONE, which is
    // what the run that wrote it ended with, so the caller shows the plain
    // diagnostic. Checked only AFTER the positive entry, and only on a first
    // attempt: a retry is never short-circuited.
    std::string negativeReason;
    if (readSlmNegativeCache(cacheKey, negativeReason)) {
      if (std::getenv("TURF_SLM_DEBUG")) {
        std::cerr << "[SLM] Negative cache hit (" << negativeReason
                  << " within the last 24h) -- skipping the model.\n";
      }
      suggestion.fromCache = true;
      return suggestion; // valid = false, fix_mode = "NONE"
    }
  }

  if (!replaying) {
    std::string ModelPath;
    if (!resolveSmartModel(ModelPath))
      return suggestion;
    if (SLMEngine::builtIn()) {
      if (std::getenv("TURF_SLM_DEBUG"))
        std::cerr << "[SLM] model: " << ModelPath << " (" << smartModelIdentity() << ")\n";
      static const bool Fits = enoughMemoryFor(ModelPath);
      if (!Fits)
        return suggestion;
      noteIfUnqualified(ModelPath);
    }
    if (!SLMEngine::getInstance().initialize(ModelPath))
      return suggestion;
  }

  // Resolve the GBNF grammar file (same binary-relative pattern as model path).
  // If the file is missing or the grammar fails to parse inside llama.cpp,
  // generateSuggestion degrades gracefully to unconstrained output.
  llvm::SmallString<128> GrammarPath(CompilerExecutablePath);
  llvm::sys::path::remove_filename(GrammarPath);
  llvm::sys::path::append(GrammarPath, "..", "grammars",
                          "slm_suggestion.gbnf");
  std::string grammarStr = loadDocFile(GrammarPath.str().str());
  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "[SLM] Grammar: "
              << (grammarStr.empty() ? "not loaded (unconstrained output)"
                                     : "loaded (constrained JSON output)")
              << "\n";
  }

  // Step 2: REMOVED — classification is now merged into the main response.
  // When the deterministic classifier yields GENERAL, we proceed with GENERAL
  // for context-sizing, pinning, and doc-loading (all have sensible GENERAL
  // defaults). The main call's self-reported "category" field then corrects
  // the bookkeeping (confidence scoring) for that slice — but cannot
  // retroactively change what the model was already shown in the prompt.
  // This eliminates the second model call in exchange for less
  // category-tailored context on GENERAL-class errors.

  // Step 3: the context window, the diagnostic's facts and the documentation
  // are all computed further up now (see the comment above the cache-lookup
  // block), before the model/grammar load, so a cache hit can skip that load
  // entirely. Debug echo kept here, at the point they're actually used.
  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "[SLM] Context window: ±" << contextSize
              << " lines (category: " << categoryName << ")\n";
    std::cerr << "[SLM] Error category: " << categoryName << " | Keyword: "
              << (contextKeyword.empty() ? "(none)" : contextKeyword)
              << " | Context: ±" << contextSize << " | Doc: " << docPath << "\n";
    std::cerr << "[SLM] Code doc: "
              << (codeDocPath.empty() ? "(none)" : codeDocPath) << "\n";
    std::cerr << "[SLM] Syntax card: "
              << (syntaxCard.empty() ? "(none)" : "docs/slm/syntax_card.txt")
              << "\n";
  }

  // 5. Build a focused prompt with only the relevant documentation.
  //    IMPORTANT: Do NOT pre-fill confidence or fix_mode in the JSON template.
  //    Small models latch onto literal values in the template and copy them
  //    verbatim (template anchoring), making the confidence gate useless.
  //    Instead, describe the schema and let the model reason about the values.
  //    Likewise, new_code must be constrained to Turf-Lang syntax only to
  //    prevent the model from falling back to its C/Java training data.
  // NOTE: confidence is intentionally NOT part of the JSON the model is asked
  // to produce. Small models "anchor" to example values in the prompt and copy
  // them verbatim, making any LLM-emitted probability meaningless. Instead we
  // compute confidence deterministically after parsing (see computeConfidence).
  // VOICE + GROUNDING preamble: added alongside the docs/slm/*.txt accuracy
  // pass (see those files' own history for the "docs described a completely
  // different, out-of-date version of Turf-Lang" bug this all follows from).
  // Two distinct problems, addressed separately:
  //
  //  1. Tone: the deterministic (non-AI) diagnostics elsewhere in this
  //     compiler all use a warm, direct, second-person voice ("Oops! You
  //     can't...", "Wait a minute! You already..."), but the SLM's own
  //     system prompt never said anything about VOICE at all -- only
  //     mechanical output-format rules. The model's prose defaulted to
  //     generic, textbook-flat phrasing that reads inconsistently next to
  //     every other message a user sees from this compiler.
  //
  //  2. Grounding: now that docContent is verified-accurate (previously it
  //     wasn't -- see the doc files), it's finally trustworthy enough to
  //     tell the model to defer to it COMPLETELY over its own training
  //     data, which is mostly C/Python/Java/etc. and actively misleading
  //     for a language with genuinely unusual syntax choices (postfix `to`
  //     casts, `mut`/`immut`, `give...otherwise` ternaries). Explicitly
  //     permitting the model to say "I'm not sure" instead of guessing
  //     matters because a wrong `new_code` only gets caught by
  //     sandbox validation -- but a wrong, confidently-stated claim in
  //     `explanation` is never validated at all; it just gets shown to the
  //     user as fact.
  // Plain text, no chat markers: SLMEngine puts it in the loaded model's own chat
  // format (for ChatML models the result is byte for byte what was written here
  // before: <|im_start|>system, this text, <|im_end|>).
  std::string systemPrompt =
      "You are a friendly compiler assistant for Turf-Lang, writing in the "
      "SAME voice as this compiler's own built-in error messages: warm, "
      "direct, second-person ('you'), never robotic or condescending. For "
      "tone only (do not copy these verbatim, they're for a different "
      "error) -- \"Oops! You can't change the value of 'x' because it's "
      "immutable.\" / \"Wait a minute! You already created a variable "
      "named 'count'.\" Match that warmth while staying technically "
      "precise.\n\n"
      "GROUNDING -- read this before anything else: everything you know "
      "about Turf-Lang syntax comes ONLY from the documentation and code "
      "context below, NOT from C, Python, Java, Rust, or any other "
      "language. Turf-Lang has several genuinely unusual choices (postfix "
      "casts like `x to int`, `mut`/`immut` instead of a bare type "
      "keyword, `give ... otherwise ...` for a ternary) that do not exist "
      "in mainstream languages -- do not silently 'correct' these into "
      "something more familiar. If a construct isn't shown in the "
      "documentation below, don't invent it. If you are not sure whether "
      "something is valid Turf-Lang, say so in reasoning, or answer "
      "NONE, rather than guessing confidently. reasoning is for you: the "
      "user never sees it. The compiler checks new_code before anything "
      "is shown.\n\n"
      "Error category: " +
      categoryName + "\n\n" + docContent +
      "\n"
      "RULES:\n"
      "1. Follow Turf-Lang type and ownership rules exactly, as shown in "
      "the documentation above -- not general programming intuition.\n"
      "2. original_code: COPY, character for character, the existing "
      "line(s) of \"code\" that your fix changes. \"code\" is the program "
      "exactly as it is in the file and \"error_line\" is the line the error "
      "is on. You never say where the code is; it is found by matching your "
      "copy, so copy exactly, spaces at the start of the line included, and "
      "do not correct anything while copying -- the correction belongs in "
      "new_code. Copy only what the fix touches: usually one line, more "
      "only when the fix really changes more than one existing line (e.g. "
      "a whole function header and body), with the lines separated by \\n. "
      "The line to change may be another line of \"code\", or one in "
      "\"related\" (e.g. a declaration), not only the line the error is on. "
      "If the fix only ADDS a line, copy the line it goes next to and "
      "repeat that line in new_code. Leave original_code empty only to "
      "insert new lines directly after the line the error is on. "
      "\"inside\", \"receiver\", \"known_types\", \"params\" (the "
      "parameters of the function the error is in), \"callee\" (the "
      "function a call is to) and \"structs\" (the structs involved) are "
      "facts the compiler knows: rely on them, they are not lines to copy.\n"
      "3. new_code: the Turf-Lang code that replaces original_code -- the "
      "same lines with your fix applied, lines separated by \\n. It must "
      "differ from original_code, and it is just code: no explanation, no "
      "prose.\n"
      "   Do NOT use C, Java, Python, or any other language syntax.\n"
      "   Do NOT include multiple statements, semicolons, or braces unless\n"
      "   they are required by Turf-Lang syntax for that construct. NEVER "
      "wrap original_code or new_code in a markdown code fence (```) -- "
      "they are fed directly to the compiler, not rendered as markdown, so "
      "a fence is a guaranteed syntax error.\n"
      "4. If you cannot produce a valid Turf-Lang patch, set fix_mode to NONE\n"
      "   and leave original_code and new_code empty. This is not a "
      "failure -- 'NONE' is\n"
      "   the right answer whenever you're not sure, and is far better than\n"
      "   a wrong patch.\n"
      "5. Set fix_mode to one of: MODIFY, NONE.\n"
      "   Use MODIFY when you have a concrete patch (usually a single "
      "line, sometimes a small block). Use NONE when you are not "
      "confident enough\n"
      "   to suggest a specific change.\n"
      "6. Respond ONLY in valid JSON without markdown code blocks.\n"
      "7. Keep reasoning to a few sentences: what is wrong, and what the "
      "edit must do.\n";

  // The user turn: buildUserPrompt() renders the JSON header -- the error, the
  // verbatim code, and every structured fact the raise site holds (item 3's
  // known types, C4's receiver struct and enclosing declaration, the related
  // lines), each its own field and never folded into the free-text "code"
  // and "error" strings, so the model doesn't have to parse a fact back out
  // of prose and a copy of a line of "code" is a copy of the file.
  std::string userPrompt = buildUserPrompt(facts, previousAttemptFailed);
  if (!previousAttemptFailed.empty()) {
    userPrompt +=
        "Your previous suggested fix was rejected -- "
        "\"previous_attempt_failed\" above says why: the compiler's own "
        "error for that attempt, or a problem with how you copied "
        "original_code. Address that specific feedback; do not send the "
        "same fix again.\n";
  }
  userPrompt +=
      "Respond with ONLY this JSON object — fill every field yourself:\n"
      "{\"reasoning\": <string>, \"fix_mode\": <MODIFY|NONE>, "
      "\"original_code\": <the existing line(s) you change, copied exactly "
      "from \"code\" or \"related\", separated by \\n if more than one — "
      "see rule 2>, "
      "\"new_code\": <THE EXACT Turf-Lang code that replaces "
      "original_code — no explanation, no prose, just the raw code, lines "
      "separated by \\n if more than one>, "
      "\"category\": <one of: TYPE_MISMATCH, UNDECLARED_VARIABLE, "
      "CONTROL_FLOW, SYNTAX_ERROR, ARITHMETIC, STRING_CONVERSION, "
      "ARRAY_ERROR, DECLARATION, FUNCTION_SIGNATURE, BUILTIN_FUNCTION, "
      "MEMORY_ERROR, GENERAL>}\n";

  if (std::getenv("TURF_SLM_DEBUG")) {
    // Full user prompt, every structured field included -- verified live
    // during SLM-001 that this is exactly what a guess-inferred operand's
    // resolved type looks like once it reaches the model. The replay smoke test
    // (scripts/slm/slm_replay_smoke.sh) greps this block to prove what the model is
    // and is not shown.
    std::cerr << "--- USER PROMPT ---\n" << userPrompt << "\n-------------------\n";
  }
  // The system prompt too, but only on request: it is ~4000 tokens of
  // documentation that would bury every debug log. scripts/slm/slm_prompt_budget.sh
  // asks for it to measure the real prompt instead of re-deriving it.
  if (std::getenv("TURF_SLM_DUMP_PROMPT")) {
    std::cerr << "--- SYSTEM PROMPT ---\n" << systemPrompt
              << "--- END SYSTEM PROMPT ---\n";
  }
  std::string rawResponse =
      replaying ? nextReplayResponse()
                : SLMEngine::getInstance().generateSuggestion(
                      systemPrompt, userPrompt, grammarStr,
                      SampleIndex > 0 ? ThoroughTemperature : 0.0f,
                      static_cast<unsigned>(SampleIndex));

  countAnswer(rawResponse, {"reasoning", "fix_mode", "original_code", "new_code", "category"},
              {{"fix_mode", {"MODIFY", "NONE"}},
               {"category",
                {"TYPE_MISMATCH", "UNDECLARED_VARIABLE", "CONTROL_FLOW", "SYNTAX_ERROR",
                 "ARITHMETIC", "STRING_CONVERSION", "ARRAY_ERROR", "DECLARATION",
                 "FUNCTION_SIGNATURE", "BUILTIN_FUNCTION", "MEMORY_ERROR", "GENERAL"}}});
  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "Clean Error: " << cleanError << "\n";
    std::cerr << "\n--- SLM RAW RESPONSE DEBUG ---\n"
              << rawResponse << "\n------------------------------\n";
  }

  if (rawResponse.empty()) {
    return suggestion;
  }

  size_t startJson = rawResponse.find('{');
  size_t endJson = rawResponse.rfind('}');
  if (startJson != std::string::npos && endJson != std::string::npos &&
      endJson > startJson) {
    std::string jsonStr =
        rawResponse.substr(startJson, endJson - startJson + 1);

    parseSuggestionJson(jsonStr, "category", suggestion);
    // Both fields get the same cleanup: a model that echoes the gutter or a
    // fence in one does it in the other (a gutter left on original_code
    // makes the copy match nothing).
    suggestion.original_code = trimBlankEdgeLines(
        stripLineNumberPrefix(stripMarkdownFences(suggestion.original_code)));
    suggestion.new_code = trimBlankEdgeLines(
        stripLineNumberPrefix(stripMarkdownFences(suggestion.new_code)));

    // B3-2: If the deterministic classifier returned GENERAL (i.e. it couldn't
    // identify the category), try using the model's self-reported category
    // from the main response before falling back to a second model call.
    // The GBNF grammar constrains "category" to the exact enum strings, so
    // categoryFromString will map it correctly.
    if (!categoryWasDeterministic && !suggestion.self_reported_category.empty()) {
      ErrorCategory selfCat = categoryFromString(suggestion.self_reported_category);
      if (selfCat != ErrorCategory::GENERAL) {
        // Model gave us a specific category — accept it, no second call needed.
        category = selfCat;
        categoryWasDeterministic = true; // treat as deterministic for confidence
        if (std::getenv("TURF_SLM_DEBUG")) {
          std::cerr << "[SLM] Used self-reported category: "
                    << suggestion.self_reported_category
                    << " (eliminated second model call)\n";
        }
      }
    }

    // Compute the structural prior from shallow, pre-sandbox signals.
    // This is NOT finalized here — sandbox validation hasn't run yet.
    // finalizeConfidence() in validateAndApplySuggestion() incorporates the
    // empirical sandbox result to produce the true, gate-relevant score.
    suggestion.confidence = computeStructuralPrior(
        suggestion, category, categoryWasDeterministic, contextKeyword,
        errorLine, related);

    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "Structural Prior (pre-sandbox): "
                << suggestion.confidence << "\n";
      std::cerr << "  category_deterministic=" << categoryWasDeterministic
                << " fix_mode=" << suggestion.fix_mode
                << " original_code_empty=" << suggestion.original_code.empty()
                << " new_code_empty=" << suggestion.new_code.empty() << "\n";
      std::cerr << "Parsed Fix Mode: " << suggestion.fix_mode << "\n";
    }

    if (suggestion.fix_mode.empty()) {
      suggestion.fix_mode = "NONE";
    }
    suggestion.valid = true;
    // Not cached here: validateAndApplySuggestion() caches it once (and
    // only if) the sandbox validates it -- see AISuggestion::cacheKey.
  }

  return suggestion;
}

// Sandbox-Based Suggestion Validation

// SL-1: the facts of a suggestion the sandbox validated, replacing lines
// [start, end] (FixScore.h): the file before and after it and the same-depth
// analysis of each (SL-5). The model's own ambiguity is not measured
// (Candidates = 0).
static void scoreSuggestion(AISuggestion &s, const Sandbox &sandbox, int start,
                            int end, int errorLine) {
  FixScore::Facts &F = s.score;
  F = FixScore::Facts();
  F.From = s.fromCache ? FixScore::Provenance::Cache : FixScore::Provenance::Model;
  F.Valid = true;
  F.Candidates = 0;
  FixScore::Program P;
  P.Before = sandbox.getOriginalSource();
  P.After = sandbox.getModifiedSource();
  P.Start = start;
  P.End = end;
  P.ErrorLine = errorLine;
  auto collect = [](const std::vector<turf::Diagnostic> &From,
                    std::vector<FixScore::ProgramDiag> &To) {
    for (const auto &d : From)
      if (d.Where.Path.empty() || d.Where.Path == SourceFilename)
        To.push_back({static_cast<int>(d.Where.Line), static_cast<int>(d.Where.Col), d.Code,
                      d.Message, d.Level == turf::Severity::Error});
  };
  collect(sandbox.getOriginalAnalysisDiagnostics(), P.BeforeDiags);
  collect(sandbox.getValidationDiagnostics(), P.AfterDiags);
  FixScore::describeProgram(F, P);
}

bool verifySuggestion(AISuggestion &suggestion, int errorLine,
                      std::string *outDiagnostics,
                      const std::vector<RelatedLocation> &related) {
  const int n = static_cast<int>(SourceLines.size());
  const bool debug = std::getenv("TURF_SLM_DEBUG") != nullptr;
  // A rejection that never reaches the sandbox still owes the retry loop a
  // reason: without one the next attempt is told "failed to compile", which
  // for a copying mistake is false and teaches the model nothing.
  auto reject = [&](const std::string &why) {
    if (outDiagnostics)
      *outDiagnostics = why;
    return false;
  };
  auto lineText = [&](int line) {
    return line >= 1 && line <= n ? SourceLines[line - 1] : std::string();
  };

  // new_code is what gets compiled. An empty one is rejected rather than
  // spliced as a deletion: a model that leaves the field blank by mistake
  // would otherwise "fix" most errors by deleting the line they are on, and
  // the sandbox -- which only asks "does it compile and is the error gone?" --
  // would approve it.
  if (suggestion.new_code.empty()) {
    ++Conf.SpliceEmptyNewCode;
    return reject("new_code was empty. Put the corrected Turf code in "
                  "new_code; the harness cannot delete lines for you.");
  }
  if (!isBlankLine(suggestion.original_code) &&
      sameCode(suggestion.original_code, suggestion.new_code)) {
    ++Conf.SpliceUnchanged;
    return reject("new_code is the same code as original_code. Change "
                  "something: the error is still there.");
  }

  // Where the model's copy is. See locateOriginal() for the whole contract;
  // the one thing to know here is that a copy found nowhere stops the attempt
  // instead of falling back to some line, and says so (issue #267 was a model
  // being told its CODE was wrong when only its line number was).
  const std::vector<OriginalMatch> located =
      locateOriginal(suggestion.original_code, errorLine, related);
  if (located.empty()) {
    ++Conf.SpliceNotFound;
    // The error line is quoted, not numbered: the model is not shown line
    // numbers any more (getCodeContext()), and a number it cannot see in its
    // input would only invite it to count lines again.
    return reject(
        "original_code does not appear in the program; copy the lines "
        "exactly as they are in \"code\", character for character. The line "
        "the error is on reads: " + normalizeLine(lineText(errorLine)));
  }

  // Candidate edits, tried in order until one validates: each located range
  // (nearest the error first; for an exact copy that is the only one that
  // matters unless the same lines occur more than once), then the error line
  // itself as a fallback for a copy that matched the wrong lines but whose
  // new_code was written for the error line. Each candidate gets a fresh
  // sandbox, so a failed one leaves nothing behind; all of them must clear
  // the same full re-check, so trying more places can only find more fixes
  // that compile and clear the error, never apply one that doesn't.
  struct Candidate {
    int start, end;
    std::string text; // what replaces [start, end]
    const char *why;
    OriginalMatch::Kind how;
  };
  std::vector<Candidate> candidates;
  auto add = [&](int s, int e, const std::string &text, const char *why,
                 OriginalMatch::Kind how) {
    if (s < 1 || e < s || e > n)
      return;
    for (const auto &c : candidates)
      if (c.start == s && c.end == e)
        return;
    candidates.push_back({s, e, text, why, how});
  };
  for (const OriginalMatch &m : located) {
    // The first line of the copied range is what the model's indentation is
    // measured against.
    const std::string text = reindentNewCode(
        suggestion.new_code, suggestion.original_code, lineText(m.Start));
    switch (m.How) {
    case OriginalMatch::Exact:
      add(m.Start, m.End, text, "exact copy", m.How);
      break;
    case OriginalMatch::Fuzzy:
      add(m.Start, m.End, text, "fuzzy copy", m.How);
      break;
    case OriginalMatch::Insertion:
      // New line(s) after the error line: replace that line with itself plus
      // the new code, which the sandbox's existing range edit can express.
      add(m.Start, m.End, lineText(m.Start) + "\n" + text,
          "insertion after the error line", m.How);
      break;
    }
  }
  if (located.front().How != OriginalMatch::Insertion)
    add(errorLine, errorLine,
        reindentNewCode(suggestion.new_code, suggestion.original_code,
                        lineText(errorLine)),
        "error line", located.front().How);

  // The candidate whose diagnostics are fed back to the model if none
  // validates: the best placement of its copy, so the feedback describes its
  // code, not a misplacement.
  const int feedbackStart = candidates.front().start;
  const int feedbackEnd = candidates.front().end;

  std::string feedback;
  for (const auto &c : candidates) {
    auto sandbox = CreateSandbox();
    if (!sandbox) {
      std::cerr << Colors::BRIGHT_BLACK << "[Verify] Failed to create sandbox"
                << Colors::RESET << "\n";
      return false;
    }
    const std::string &fixLine = c.text;
    const auto began = std::chrono::steady_clock::now();
    bool ok = ApplyAndValidateFix(*sandbox, errorLine, c.start, c.end, fixLine,
                                  suggestion, /*reportFailure=*/debug);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    suggestion.sandboxDepth = sandboxDepthName(sandboxDepth());
    if (debug)
      std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] candidate [" << c.start
                << ", " << c.end << "] (" << c.why << "): "
                << (ok ? "validated" : "rejected") << " (depth "
                << suggestion.sandboxDepth << ", " << std::fixed
                << std::setprecision(1) << ms << " ms)" << std::defaultfloat
                << Colors::RESET << "\n";

    // SLM-001 follow-up: on success, capture the FULL patched source -- not
    // just the one line that changed -- so a caller can write out (or
    // recompile+run) the whole corrected program, not just read a
    // description of the fix. `sandbox` (and its Snapshot.SourceCode) is
    // about to go out of scope, so this is the only point this data is
    // available at all.
    if (ok) {
      std::string joined;
      for (const auto &line : sandbox->getModifiedSource()) {
        joined += line;
        joined += "\n";
      }
      suggestion.appliedSource = std::move(joined);
      scoreSuggestion(suggestion, *sandbox, c.start, c.end, errorLine);
      // Where it went, for renderPatchDiff() -- the lines that were really
      // replaced, which for a fuzzy copy are not the model's words.
      suggestion.patchStart = c.start;
      suggestion.patchEnd = c.end;
      suggestion.patchText = fixLine;
      return true;
    }

    // Hand the sandbox's own compiler diagnostics back to the caller
    // (validateAndApplySuggestion()'s retry loop) so the next
    // fetchSuggestion() call can tell the model exactly why the last patch
    // didn't compile, instead of blindly re-guessing -- from the feedback
    // candidate only (see feedbackStart), each distinct message once, and
    // with what the patched lines actually became, so the model can tell
    // "my code is wrong" from "it was placed somewhere I didn't mean".
    if (c.start == feedbackStart && c.end == feedbackEnd) {
      std::vector<std::string> seen;
      for (const auto &diag : sandbox->getValidationDiagnostics()) {
        if (std::find(seen.begin(), seen.end(), diag.Message) != seen.end())
          continue;
        seen.push_back(diag.Message);
        if (!feedback.empty())
          feedback += " | ";
        feedback += diag.Message;
      }
      const auto &patched = sandbox->getModifiedSource();
      int newLines = 1 + static_cast<int>(
                             std::count(fixLine.begin(), fixLine.end(), '\n'));
      int lo = std::max(1, c.start - 1);
      int hi = std::min(static_cast<int>(patched.size()), c.start + newLines);
      const std::string where =
          c.start == c.end ? " " + std::to_string(c.start)
                           : "s " + std::to_string(c.start) + "-" +
                                 std::to_string(c.end);
      // A copy that was only approximately right was still located, but the
      // model should know what it was taken to mean, and that it was off.
      if (c.how == OriginalMatch::Fuzzy) {
        feedback += "\nYour original_code was not an exact copy of the "
                    "program; it was matched to line" + where +
                    ", which read:\n";
        for (int i = c.start; i <= c.end; ++i)
          feedback += "  " + SourceLines[i - 1] + "\n";
        feedback += "Copy the lines exactly next time.";
      }
      feedback += "\nYour new_code replaced line" + where +
                  "; afterwards the code read:\n";
      for (int i = lo; i <= hi; ++i)
        feedback += "  " + patched[i - 1] + "\n";
    }
  }

  if (outDiagnostics)
    *outDiagnostics = feedback;
  return false;
}

// Shown under "Suggested fix:" for every validated model suggestion. Built
// from the spliced range and text rather than the model's
// original_code/new_code so that the old lines are the ones really replaced
// (a fuzzy copy shows what it matched, not what the model typed). Lines equal at both ends of the range are left out, which
// is what makes an inserted `;` or a renamed word one `-` and one `+` line, and
// an insertion after the error line just `+` lines. The first `+` line carries
// the line number, the rest `..`, because new lines have no number of their own
// in the file being compiled.
std::string renderPatchDiff(const AISuggestion &s, bool color) {
  const int n = static_cast<int>(SourceLines.size());
  if (s.patchStart < 1 || s.patchEnd < s.patchStart || s.patchEnd > n)
    return "";
  std::vector<std::string> oldLines(SourceLines.begin() + (s.patchStart - 1),
                                    SourceLines.begin() + s.patchEnd);
  std::vector<std::string> newLines = splitTextLines(s.patchText);

  size_t head = 0;
  while (head < oldLines.size() && head < newLines.size() &&
         oldLines[head] == newLines[head])
    ++head;
  size_t tail = 0;
  while (tail < oldLines.size() - head && tail < newLines.size() - head &&
         oldLines[oldLines.size() - 1 - tail] ==
             newLines[newLines.size() - 1 - tail])
    ++tail;

  std::string out;
  auto emit = [&](const std::string &line) {
    if (!out.empty())
      out += "\n    ";
    out += line;
  };
  for (size_t i = head; i < oldLines.size() - tail; ++i)
    emit((color ? Colors::BRIGHT_RED : std::string()) + "- " + std::to_string(s.patchStart + i) + " | " +
         oldLines[i] + (color ? Colors::RESET : std::string()));
  for (size_t i = head; i < newLines.size() - tail; ++i)
    emit((color ? Colors::BRIGHT_GREEN : std::string()) + "+ " +
         (i == head ? std::to_string(s.patchStart + head) : std::string("..")) +
         " | " + newLines[i] + (color ? Colors::RESET : std::string()));
  return out;
}

// SL-1, --smart=thorough: asks the model for ExtraSamples more answers to the
// same prompt, sampled, checks each in the sandbox, and sets the validated
// suggestion's Candidates to the number of different programs the validated
// answers make. One means every validated answer agreed (or none other
// validated); more makes the suggestion plausible.
static void measureAgreement(AISuggestion &suggestion, const std::string &errorMessage,
                             int errorLine, const std::string &hint,
                             const std::vector<RelatedLocation> &related,
                             ErrorCategory category,
                             const std::string &previousAttemptFailed,
                             const DiagnosticHints &hints, const std::string &errorCode) {
  std::set<std::string> programs{suggestion.appliedSource};
  int validated = 0;
  bool cut = false;
  for (int k = 1; k <= ExtraSamples; ++k) {
    if (overBudget()) {
      cut = true;
      break;
    }
    SampleIndex = k;
    AISuggestion extra = fetchSuggestion(errorMessage, errorLine, hint, related, category,
                                         previousAttemptFailed, hints, errorCode);
    SampleIndex = 0;
    if (!extra.valid || extra.fix_mode != "MODIFY")
      continue;
    if (verifySuggestion(extra, errorLine, nullptr, related)) {
      ++validated;
      programs.insert(extra.appliedSource);
    }
  }
  // Cut short by the budget with no disagreement seen: not measured.
  suggestion.score.Candidates = cut && programs.size() == 1 ? 0 : static_cast<int>(programs.size());
  if (std::getenv("TURF_SLM_DEBUG"))
    std::cerr << "[SLM] thorough: " << (cut ? "cut short by the budget; " : "") << validated << " of " << ExtraSamples
              << " extra answers validated; " << programs.size()
              << " different program(s)\n";
}

// High-Level Auto-Fix API

// Retry cap for the loop below -- matches the roadmap wording exactly
// ("Retry up to 3x on TypeChecker failure, feeding the error back into the
// prompt"): docs/archive/ROADMAP_v1.0.0_COMPLETE.md §7's `--smart` Retry
// Loop item, which absorbed the retired final-sprint checklist's Track C.
static constexpr int kMaxSlmAttempts = 3;

// Wrapper that gets an SLM suggestion, validates it in a sandbox, and
// applies it if valid. Returns an enhanced AISuggestion with validation
// results.
//
// Single-shot until this loop was added: one fetchSuggestion() +
// verifySuggestion() pair, downgrading straight to fix_mode "NONE" on the
// first sandbox rejection. That wastes the sandbox's most useful output --
// verifySuggestion() (via Sandbox::getValidationDiagnostics()) already
// knows exactly why the patch didn't compile, but nothing was feeding that
// back to the model. Now: on a sandbox failure, that diagnostic text is
// passed to the next fetchSuggestion() call as `previousAttemptFailed`, and
// a whole new attempt (new model call, new sandbox) runs, up to
// kMaxSlmAttempts total. computeStructuralPrior()/finalizeConfidence()
// themselves are unchanged -- they just run once per attempt instead of
// once total, exactly like every other step in the loop body.
// #364: what the user reads about a fix is written once the sandbox has
// accepted it, by one short request that sees the error, the whole edit from
// the original program to the fixed one, and the errors earlier attempts met.
// It cannot doubt that the fix compiles (it is told it does), and after a
// retry it describes the whole edit, not the last step. On no answer the fix
// is shown without an explanation. A fix from the cache keeps the
// explanation cached with it, and is never sent back to the model for one.
static void explainFix(AISuggestion &s, const std::string &errorMessage,
                       int errorLine, const std::vector<std::string> &errorsMet) {
  if (s.fromCache)
    return;
  if (overBudget()) {
    if (std::getenv("TURF_SLM_DEBUG"))
      std::cerr << "[SLM] explain: skipped, out of the run's budget\n";
    return;
  }
  const bool replaying = std::getenv("TURF_SLM_REPLAY") != nullptr;
  const int n = static_cast<int>(SourceLines.size());
  std::string user = "{\n  \"error\": \"" + escapeJsonString(stripAnsiCodes(errorMessage)) + "\",\n";
  user += "  \"error_line\": \"" +
          escapeJsonString(errorLine >= 1 && errorLine <= n ? SourceLines[errorLine - 1] : "") +
          "\",\n";
  // The edit as the lines it replaced and the lines that replace them, the
  // ones equal at both ends left out (as renderPatchDiff() does), with no
  // gutter: a small model reads `- 12 |` as part of the code.
  {
    std::vector<std::string> was, now = splitTextLines(s.patchText);
    for (int i = s.patchStart; i >= 1 && i <= s.patchEnd && i <= n; ++i)
      was.push_back(SourceLines[i - 1]);
    size_t head = 0;
    while (head < was.size() && head < now.size() && was[head] == now[head])
      ++head;
    size_t tail = 0;
    while (tail < was.size() - head && tail < now.size() - head &&
           was[was.size() - 1 - tail] == now[now.size() - 1 - tail])
      ++tail;
    auto join = [&](const std::vector<std::string> &v) {
      std::string out;
      for (size_t i = head; i < v.size() - tail; ++i)
        out += (out.empty() ? "" : "\n") + normalizeLine(v[i]);
      return out;
    };
    user += "  \"before\": \"" + escapeJsonString(join(was)) + "\",\n";
    user += "  \"after\": \"" + escapeJsonString(join(now)) + "\"";
  }
  if (!errorsMet.empty())
    user += ",\n  \"errors_on_the_way\": " + jsonStringArray(errorsMet);
  user += "\n}\nRespond with ONLY this JSON object: {\"summary\": <one line: what the "
          "fix does>, \"explanation\": <1-3 sentences: why the error happened and "
          "how this edit removes it>}\n";
  const std::string system =
      "You explain a fix to a Turf-Lang program to the person who wrote it, in "
      "the compiler's own voice: warm, direct, second-person ('you'), never "
      "condescending. The fix is already checked: the compiler compiled the "
      "fixed program and the error is gone, so never say you are unsure "
      "whether it works. \"before\" is the code the fix replaced and "
      "\"after\" what replaced it: the whole change. Say what changed from "
      "before to after, and describe nothing else. \"errors_on_the_way\", when present, are errors earlier "
      "drafts of the fix ran into: do not describe the drafts, but you may "
      "use them to say why the edit is the way it is. Do not suggest other "
      "changes. When you name code, copy it from the edit exactly, in "
      "backticks (`self.n to string`): Turf-Lang is not C, Python or Rust, "
      "so never rename it into another language's spelling, such as a method "
      "the edit does not call. Respond ONLY in valid JSON without markdown "
      "code blocks.\n";
  if (std::getenv("TURF_SLM_DUMP_PROMPT"))
    std::cerr << "--- EXPLAIN SYSTEM PROMPT ---\n" << system << "--- END EXPLAIN SYSTEM PROMPT ---\n"
              << "--- EXPLAIN USER PROMPT ---\n" << user << "--- END EXPLAIN USER PROMPT ---\n";
  llvm::SmallString<128> GrammarPath(CompilerExecutablePath);
  llvm::sys::path::remove_filename(GrammarPath);
  llvm::sys::path::append(GrammarPath, "..", "grammars", "slm_explanation.gbnf");
  const std::string grammar = loadDocFile(GrammarPath.str().str());
  const auto began = std::chrono::steady_clock::now();
  const std::string raw = replaying ? nextReplayResponse()
                                    : SLMEngine::getInstance().generateSuggestion(system, user, grammar);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
  countAnswer(raw, {"summary", "explanation"});
  const size_t from = raw.find('{'), to = raw.rfind('}');
  if (from != std::string::npos && to != std::string::npos && to > from) {
    const std::string json = raw.substr(from, to - from + 1);
    s.summary = extractJsonFieldString(json, "summary");
    s.explanation = extractJsonFieldString(json, "explanation");
    if (auto Parsed = llvm::json::parse(json)) {
      if (const auto *Obj = Parsed->getAsObject()) {
        if (auto V = Obj->getString("summary"))
          s.summary = V->str();
        if (auto V = Obj->getString("explanation"))
          s.explanation = V->str();
      }
    } else {
      llvm::consumeError(Parsed.takeError());
    }
  }
  if (std::getenv("TURF_SLM_DEBUG"))
    std::cerr << "[SLM] explain: " << (s.explanation.empty() ? "no answer" : "written") << " in "
              << std::fixed << std::setprecision(1) << ms << std::defaultfloat << " ms\n";
}

AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine, const std::string &hint,
                                        const std::vector<RelatedLocation> &related,
                                        ErrorCategory category,
                                        const DiagnosticHints &hints,
                                        const std::string &errorCode) {
  // A diagnostic the compiler can fix itself never gets here: its fix is
  // checked and printed first (TDP-2-001, DiagnosticEngine::add()), and the
  // model is asked only when there is none.
  AISuggestion suggestion;
  std::string previousAttemptFailed; // empty on attempt 1
  std::vector<std::string> errorsMet; // each failed attempt's first error

  // SL-4: this run's budget, and its conformance counts at the end.
  RunDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(smartBudgetSeconds());
  SLMEngine::getInstance().setDeadline(RunDeadline);
  Conf = Conformance();
  struct ReportConformance {
    ~ReportConformance() {
      if (std::getenv("TURF_SLM_DEBUG"))
        std::cerr << conformanceLine() << "\n";
    }
  } Report;

  for (int attempt = 1; attempt <= kMaxSlmAttempts; ++attempt) {
    if (overBudget()) {
      // Fail closed: no fix has passed, so the compiler's own diagnostic.
      std::cerr << "note: --smart stopped at its " << smartBudgetSeconds()
                << "-second budget; showing the compiler's own diagnostics.\n";
      suggestion.valid = false;
      suggestion.fix_mode = "NONE";
      return suggestion;
    }
    suggestion = fetchSuggestion(errorMessage, errorLine, hint, related,
                                 category, previousAttemptFailed, hints,
                                 errorCode);

    if (!suggestion.valid) {
      // The SLM itself failed/timed out/produced unparsable JSON -- not a
      // sandbox rejection, so retrying with the same unreachable model
      // wouldn't help. Bail immediately, same as the old single-shot path.
      if (overBudget())
        std::cerr << "note: --smart stopped at its " << smartBudgetSeconds()
                  << "-second budget; showing the compiler's own diagnostics.\n";
      return suggestion;
    }

    // Skip validation for NONE fixes (no patch to apply)
    if (suggestion.fix_mode == "NONE") {
      std::cerr << Colors::BRIGHT_BLACK
                << "[Sandbox] Fix mode is NONE, skipping validation"
                << Colors::RESET << "\n";
      // The model's answer is "no fix": remember it for a day (D4) so the
      // next run on this diagnostic does not ask again. Never for a
      // suggestion that came out of the cache.
      if (!slmCacheBypassed() && !suggestion.cacheKey.empty() &&
          !suggestion.fromCache)
        writeSlmNegativeCache(suggestion.cacheKey, "none");
      return suggestion;
    }

    // Verify the suggestion in a sandbox
    std::string diagnosticText;
    bool isValid =
        verifySuggestion(suggestion, errorLine, &diagnosticText, related);

    // The old confidence score, kept for TURF_SLM_DEBUG and the benchmark;
    // display is decided by the Fix Score's tier (SL-1).
    suggestion.confidence = finalizeConfidence(suggestion.confidence, isValid);

    // Explained once it is known to work, and before it is cached, so the
    // cache holds the explanation that goes with it (#364).
    if (isValid)
      explainFix(suggestion, errorMessage, errorLine, errorsMet);
    else if (!diagnosticText.empty())
      errorsMet.push_back(
          diagnosticText.substr(0, std::min(diagnosticText.find('\n'), diagnosticText.find(" | "))));

    if (!slmCacheBypassed() && !suggestion.cacheKey.empty()) {
      if (isValid && !suggestion.fromCache) {
        writeSlmCache(suggestion.cacheKey, suggestion);
        // A fix now exists for this diagnostic; an older "no fix" must not
        // outlive it (the positive entry is read first, but don't leave it).
        evictSlmNegativeCache(suggestion.cacheKey);
      } else if (!isValid && suggestion.fromCache)
        evictSlmCache(suggestion.cacheKey);
      // Out of attempts below: every one of the three failed the sandbox, so
      // the next run would pay for three generations again (D4).
      if (!isValid && attempt == kMaxSlmAttempts)
        writeSlmNegativeCache(suggestion.cacheKey, "failed");
    }

    if (isValid) {
      suggestion.valid = true;
      if (ExtraSamples > 0)
        measureAgreement(suggestion, errorMessage, errorLine, hint, related,
                         category, previousAttemptFailed, hints, errorCode);
      std::cerr << Colors::BRIGHT_GREEN << "[Sandbox] ✓ Suggestion validated"
                << (attempt > 1 ? " (attempt " + std::to_string(attempt) + "/" +
                                      std::to_string(kMaxSlmAttempts) + ")"
                                : "")
                << Colors::RESET << "\n";
      if (std::getenv("TURF_SLM_DEBUG")) {
        std::cerr << "Final Confidence (post-sandbox): " << suggestion.confidence << "\n";
        std::cerr << "Sandbox depth: " << suggestion.sandboxDepth << "\n";
        std::cerr << "Fix tier: " << FixScore::tierName(FixScore::tier(suggestion.score));
        for (const auto &f : FixScore::flags(suggestion.score))
          std::cerr << " | " << f;
        std::cerr << "\n";
      }
      return suggestion;
    }

    if (attempt == kMaxSlmAttempts) {
      suggestion.valid = false;
      suggestion.fix_mode = "NONE"; // Downgrade to no fix -- out of attempts
      std::cerr << Colors::BRIGHT_YELLOW
                << "[Sandbox] ✗ Suggestion validation failed after "
                << kMaxSlmAttempts << " attempts" << Colors::RESET << "\n";
      if (std::getenv("TURF_SLM_DEBUG")) {
        std::cerr << "Final Confidence (post-sandbox, capped): " << suggestion.confidence << "\n";
      }
      return suggestion;
    }

    // Attempts remain: feed this attempt's real compiler diagnostic back
    // into the next fetchSuggestion() call and try again.
    previousAttemptFailed = diagnosticText.empty()
        ? "The previous suggested new_code failed to compile."
        : diagnosticText;
    std::cerr << Colors::BRIGHT_YELLOW << "[Sandbox] Attempt " << attempt
              << "/" << kMaxSlmAttempts
              << " failed validation, retrying with compiler feedback..."
              << Colors::RESET << "\n";
  }

  return suggestion; // unreachable -- loop always returns on its last iteration
}

} // namespace SLM
