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

#include "SLMIntegration.h"
#include "Colors.h"
#include "ErrorCategory.h"
#include "ErrorCodes.h" // InstalledDocPath()
#include "Lexer.h"
#include "Sandbox.h"
#include <SLMEngine.h>
#include "AST.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

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

std::string getCodeContext(int errorLine, int contextSize) {
  if (SourceLines.empty()) {
    return "";
  }

  int startLine = std::max(1, errorLine - contextSize);
  int endLine =
      std::min(static_cast<int>(SourceLines.size()), errorLine + contextSize);

  std::string context;
  context.reserve(512);

  context += "Code Context\n";
  for (int i = startLine; i <= endLine; ++i) {
    std::string prefix = (i == errorLine) ? ">> " : "   ";
    context += prefix + std::to_string(i) + " | " + SourceLines[i - 1] + "\n";
  }

  context += "\n";

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

// Builds the standard context window then appends one or more "pinned" lines
// that are outside the window but directly relevant to the error (e.g. the
// site where a function was declared). Pinned lines are shown with a '*'
// prefix so the model knows they are a deliberate anchor, not part of the
// surrounding range.
// Lines that already fall within the [errorLine ± contextSize] window are
// silently skipped to avoid duplication.
//
// Takes the full RelatedLocation list (not just line numbers): each one's
// Description ("declared here", "candidate", "first freed here", ...) is a
// real, meaningful label every .withRelated() call site already computes --
// previously only rel.Loc.Line survived into this function, so the model
// saw an unlabeled source line with no indication of WHY it was pinned or
// how it relates to the error (especially bad for a multi-candidate arity
// error, or telling a double-free's "declared here" apart from its "first
// freed here").
static std::string
getCodeContextWithPinnedLines(int errorLine, int contextSize,
                              const std::vector<RelatedLocation> &related) {
  std::string context = getCodeContext(errorLine, contextSize);

  if (related.empty() || SourceLines.empty())
    return context;

  // Compute the window boundaries to avoid pinning lines already visible.
  int windowStart = std::max(1, errorLine - contextSize);
  int windowEnd =
      std::min(static_cast<int>(SourceLines.size()), errorLine + contextSize);

  bool headerAdded = false;
  for (const auto &rel : related) {
    int lineNo = rel.Loc.Line;
    if (lineNo < 1 || lineNo > static_cast<int>(SourceLines.size()))
      continue;
    // Skip lines already inside the normal context window.
    if (lineNo >= windowStart && lineNo <= windowEnd)
      continue;
    if (!headerAdded) {
      context += "Relevant definition(s)\n";
      headerAdded = true;
    }
    context += "*  " + std::to_string(lineNo) + " | " +
              SourceLines[lineNo - 1];
    if (!rel.Description.empty())
      context += "   // " + rel.Description;
    context += "\n";
  }
  if (headerAdded)
    context += "\n";
  return context;
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

// Defensive cleanup for a known small-model failure mode: despite explicit
// prompt instructions not to, the model occasionally echoes the "N |  code"
// line-number-prefix formatting used to display context lines (see
// getCodeContext()/getCodeContextWithPinnedLines()) back inside
// original_code or new_code itself, e.g. "6 |     mut b: int = 5;" instead of
// just "    mut b: int = 5;". That prefix is never valid Turf-Lang syntax at the
// start of a line, so stripping it here is safe and turns an otherwise
// guaranteed sandbox-parse failure (new_code), or a copy that matches nothing
// in the source (original_code), into a usable patch.
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
static void parseSuggestionJson(const std::string &Json,
                                const std::string &categoryKey,
                                AISuggestion &Out) {
  if (auto Parsed = llvm::json::parse(Json)) {
    if (const llvm::json::Object *Obj = Parsed->getAsObject()) {
      auto Str = [&](llvm::StringRef Key) -> std::string {
        if (auto S = Obj->getString(Key))
          return S->str();
        return "";
      };
      Out.explanation = Str("explanation");
      Out.fix_mode = Str("fix_mode");
      Out.original_code = Str("original_code");
      Out.new_code = Str("new_code");
      Out.suggested_comment = Str("suggested_comment");
      Out.warning = Str("warning");
      Out.self_reported_category = Str(categoryKey);
      return;
    }
  } else {
    llvm::consumeError(Parsed.takeError());
  }
  Out.explanation = extractJsonFieldString(Json, "explanation");
  Out.fix_mode = extractJsonFieldString(Json, "fix_mode");
  Out.original_code = extractJsonFieldString(Json, "original_code");
  Out.new_code = extractJsonFieldString(Json, "new_code");
  Out.suggested_comment = extractJsonFieldString(Json, "suggested_comment");
  Out.warning = extractJsonFieldString(Json, "warning");
  Out.self_reported_category = extractJsonFieldString(Json, categoryKey);
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
    if (rel.Loc.Line >= 1 && rel.Loc.Line <= n &&
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
// This is NOT the final confidence — sandbox validation hasn't run yet.
// The score is meant to estimate plausibility; finalizeConfidence() then
// incorporates the strongest signal (empirical sandbox pass/fail) to produce
// the true confidence used by the display gate. (In practice that makes the
// prior matter only for logging and for ranking an unvalidated answer below
// the display gate; keep it cheap and honest rather than clever.)
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
// structural prior is all we have. It is capped below the display gate (0.8)
// so that an unvalidated fix never auto-applies.
static float finalizeConfidence(float structuralPrior, bool sandboxValidated) {
  if (sandboxValidated) {
    // Proven-correct: clamp to at least 0.9, no longer blockable by the prior.
    return std::max(0.9f, structuralPrior);
  }
  // Not validated: cap well below the gate so it can never auto-apply.
  return std::min(structuralPrior, 0.5f);
}

// ── Item 4: on-disk suggestion cache (~/.cache/turf/slm/) ───────────────────
//
// Keyed on (error code, normalized message, hash of the pinned context) --
// exactly the tuple §16.3 item 4 names -- so an IDENTICAL diagnostic (same
// code, same message text, same surrounding source) on a later compile skips
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

static std::string slmCacheKey(const std::string &ErrorCode,
                               const std::string &NormalizedMessage,
                               const std::string &CodeContext) {
  // '\x1f' (ASCII unit separator) as the join delimiter -- never appears in
  // any of the three inputs (error codes are "E0NN", messages/context are
  // printable source text), so no input can be crafted to collide two
  // otherwise-different keys by shifting a delimiter.
  return fnv1aHex(ErrorCode + "\x1f" + NormalizedMessage + "\x1f" + CodeContext);
}

static std::string slmCacheDir() {
  const char *Home = std::getenv("HOME");
  if (!Home || !*Home)
    return ""; // no HOME -- caching is a pure optimization, disable cleanly
  return std::string(Home) + "/.cache/turf/slm";
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
// for, or the cache file's own, change.
//   1: replacement_line / target_line / end_line / code_patch
//   2: original_code / new_code (workstream B)
static constexpr int kSlmSchemaVersion = 2;

static std::string slmCompilerFingerprint() {
  llvm::sys::fs::file_status Status;
  if (llvm::sys::fs::status(CompilerExecutablePath, Status))
    return ""; // stat() failed -- never matches a real cache entry below
  auto Mtime = Status.getLastModificationTime().time_since_epoch().count();
  return "schema" + std::to_string(kSlmSchemaVersion) + ":" +
         std::to_string(Status.getSize()) + ":" + std::to_string(Mtime);
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
  parseSuggestionJson(Json, "self_reported_category", Out);
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
       << "\"explanation\":\"" << escapeJsonString(S.explanation) << "\","
       << "\"fix_mode\":\"" << escapeJsonString(S.fix_mode) << "\","
       << "\"original_code\":\"" << escapeJsonString(S.original_code) << "\","
       << "\"new_code\":\"" << escapeJsonString(S.new_code) << "\","
       << "\"suggested_comment\":\"" << escapeJsonString(S.suggested_comment) << "\","
       << "\"warning\":\"" << escapeJsonString(S.warning) << "\","
       << "\"self_reported_category\":\"" << escapeJsonString(S.self_reported_category)
       << "\"}\n";
}

static void evictSlmCache(const std::string &Key) {
  std::string Dir = slmCacheDir();
  if (!Dir.empty())
    (void)llvm::sys::fs::remove(Dir + "/" + Key + ".json");
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
  return Next < Responses.size() ? Responses[Next++] : "";
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
  std::string contextKeyword = extractContextKeyword(cleanError);

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
  std::string codeContext = related.empty()
      ? getCodeContext(errorLine, contextSize)
      : getCodeContextWithPinnedLines(errorLine, contextSize, related);
  std::string categoryName = getCategoryName(category);

  // Item 4 (caching): key on (error code, normalized message, hash of the
  // pinned context) -> the model's raw JSON fields, persisted under
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
  const std::string cacheKey = slmCacheKey(errorCode, cleanError, codeContext);
  suggestion.cacheKey = cacheKey;
  const bool replaying = std::getenv("TURF_SLM_REPLAY") != nullptr;
  if (previousAttemptFailed.empty() && !replaying) {
    AISuggestion cached;
    if (readSlmCache(cacheKey, cached)) {
      if (std::getenv("TURF_SLM_DEBUG")) {
        std::cerr << "[SLM] Cache hit -- skipping model load entirely.\n";
      }
      cached.cacheKey = cacheKey;
      cached.fromCache = true;
      return cached;
    }
  }

  if (!replaying) {
    llvm::SmallString<128> ModelPath(CompilerExecutablePath);
    llvm::sys::path::remove_filename(ModelPath);
    llvm::sys::path::append(ModelPath, "..", "models",
                            "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf");
    if (!SLMEngine::getInstance().initialize(ModelPath.str().str())) {
      return suggestion;
    }
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

  // Step 3: dynamic context window -- contextSize/codeContext/categoryName
  // are computed further up now (see the comment above the cache-lookup
  // block), before the model/grammar load, so a cache hit can skip that
  // load entirely. Debug echo kept here, at the point they're actually used.
  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "[SLM] Context window: ±" << contextSize
              << " lines (category: " << categoryName << ")\n";
    std::cerr << "--- PINNED CONTEXT ---\n" << codeContext << "\n----------------------\n";
  }

  // 3. Get the most specific documentation available
  std::string docPath = getCategoryDocPath(category, contextKeyword);

  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "[SLM] Error category: " << categoryName << " | Keyword: "
              << (contextKeyword.empty() ? "(none)" : contextKeyword)
              << " | Context: ±" << contextSize << " | Doc: " << docPath << "\n";
  }

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
  if (!errorCode.empty()) {
    std::string codeDocPath = ErrorCodes::getSlmCodeDocPath(errorCode);
    std::string codeDoc = codeDocPath.empty() ? "" : loadDocFile(codeDocPath);
    if (!codeDoc.empty())
      docContent = "THIS ERROR (" + errorCode + "):\n" + codeDoc +
                   "\nGENERAL REFERENCE:\n" + docContent;
    if (std::getenv("TURF_SLM_DEBUG"))
      std::cerr << "[SLM] Code doc: "
                << (codeDoc.empty() ? "(none)" : codeDocPath) << "\n";
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
  std::string systemPrompt =
      "<|im_start|>system\n"
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
      "something is valid Turf-Lang, SAY SO in explanation rather than "
      "guessing confidently -- an honest 'I'm not certain this is right' "
      "is more useful than a fluent wrong answer, since only "
      "new_code gets sandbox-validated before a user sees it; "
      "explanation and suggested_comment do not.\n\n"
      "Error category: " +
      categoryName + "\n\n" + docContent +
      "\n"
      "RULES:\n"
      "1. Follow Turf-Lang type and ownership rules exactly, as shown in "
      "the documentation above -- not general programming intuition.\n"
      "2. original_code: COPY, character for character, the existing "
      "line(s) of the code above that your fix changes, WITHOUT the line "
      "numbers or the >> and * markers. You never say where the code is; "
      "it is found by matching your copy, so copy exactly and do not "
      "correct anything while copying -- the correction belongs in "
      "new_code. Copy only what the fix touches: usually one line, more "
      "only when the fix really changes more than one existing line (e.g. "
      "a whole function header and body), with the lines separated by \\n. "
      "The line to change may be one of the other lines shown above (e.g. "
      "a declaration), not only the line the error is on. If the fix only "
      "ADDS a line, copy the line it goes next to and repeat that line in "
      "new_code. Leave original_code empty only to insert new lines "
      "directly after the line the error is on.\n"
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
      "7. Keep explanation, suggested_comment, and warning to 1-3 sentences "
      "-- warm and human-sized, not a one-word reply, but never rambling.\n"
      "<|im_end|>\n";

  std::string userPrompt =
      "<|im_start|>user\n"
      "{\n"
      "  \"error\": \"" + escapeJsonString(cleanError) + "\",\n"
      "  \"hint\": \"" + escapeJsonString(hint) + "\",\n"
      "  \"code\": \"" + escapeJsonString(codeContext) + "\"";
  // Item 3: known concrete types the type checker already computed that are
  // NOT visible just by reading the code above (see DiagnosticHints's doc
  // comment, ErrorCategory.h, for exactly which facts qualify and why --
  // currently a `guess`-inferred local's resolved type, the one piece of
  // type information a Turf-Lang source line can never spell out itself).
  // A structured field, not folded into the free-text "code"/"error"
  // strings above, so the model doesn't have to parse it back out of prose.
  if (!hints.KnownTypes.empty()) {
    std::string knownTypes;
    for (size_t i = 0; i < hints.KnownTypes.size(); ++i) {
      if (i)
        knownTypes += " ";
      knownTypes += hints.KnownTypes[i];
    }
    userPrompt += ",\n  \"known_types\": \"" + escapeJsonString(knownTypes) + "\"";
  }
  if (!previousAttemptFailed.empty()) {
    userPrompt += ",\n  \"previous_attempt_failed\": \"" +
                  escapeJsonString(previousAttemptFailed) + "\"";
  }
  userPrompt += "\n}\n";
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
      "{\"explanation\": <string>, \"fix_mode\": <MODIFY|NONE>, "
      "\"original_code\": <the existing line(s) you change, copied exactly "
      "from the code above without line numbers, separated by \\n if more "
      "than one — see rule 2>, "
      "\"new_code\": <THE EXACT Turf-Lang code that replaces "
      "original_code — no explanation, no prose, just the raw code, lines "
      "separated by \\n if more than one>, "
      "\"suggested_comment\": <string>, "
      "\"warning\": <string>, "
      "\"category\": <one of: TYPE_MISMATCH, UNDECLARED_VARIABLE, "
      "CONTROL_FLOW, SYNTAX_ERROR, ARITHMETIC, STRING_CONVERSION, "
      "ARRAY_ERROR, DECLARATION, FUNCTION_SIGNATURE, BUILTIN_FUNCTION, "
      "MEMORY_ERROR, GENERAL>}\n"
      "<|im_end|>\n"
      "<|im_start|>assistant\n";

  if (std::getenv("TURF_SLM_DEBUG")) {
    // Full user prompt, including the "known_types" field (item 3) when
    // present -- verified live during SLM-001 that this is exactly what a
    // guess-inferred operand's resolved type looks like once it reaches the
    // model, distinct from "Clean Error"/"PINNED CONTEXT" above (which show
    // the error text and source window, not this structured JSON payload).
    std::cerr << "--- USER PROMPT ---\n" << userPrompt << "\n-------------------\n";
  }
  std::string rawResponse =
      replaying ? nextReplayResponse()
                : SLMEngine::getInstance().generateSuggestion(
                      systemPrompt, userPrompt, grammarStr);

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
  if (suggestion.new_code.empty())
    return reject("new_code was empty. Put the corrected Turf code in "
                  "new_code; the harness cannot delete lines for you.");
  if (!isBlankLine(suggestion.original_code) &&
      sameCode(suggestion.original_code, suggestion.new_code))
    return reject("new_code is the same code as original_code. Change "
                  "something: the error is still there.");

  // Where the model's copy is. See locateOriginal() for the whole contract;
  // the one thing to know here is that a copy found nowhere stops the attempt
  // instead of falling back to some line, and says so (issue #267 was a model
  // being told its CODE was wrong when only its line number was).
  const std::vector<OriginalMatch> located =
      locateOriginal(suggestion.original_code, errorLine, related);
  if (located.empty())
    return reject(
        "original_code does not appear in the program; copy the lines "
        "exactly as shown, without the line numbers. The error is on line " +
        std::to_string(errorLine) + ": " + normalizeLine(lineText(errorLine)));

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
    bool ok = ApplyAndValidateFix(*sandbox, errorLine, c.start, c.end, fixLine,
                                  suggestion, /*reportFailure=*/debug);
    if (debug)
      std::cerr << Colors::BRIGHT_BLACK << "[Sandbox] candidate [" << c.start
                << ", " << c.end << "] (" << c.why << "): "
                << (ok ? "validated" : "rejected") << Colors::RESET << "\n";

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

// Shown under "Suggested fix:" for every validated suggestion, whoever made
// it. Built from the spliced range and text rather than the model's
// original_code/new_code so that the old lines are the ones really replaced
// (a fuzzy copy shows what it matched, not what the model typed) and so the
// compiler's fix-its need no display field of their own: they fill the same
// three patch fields. Lines equal at both ends of the range are left out, which
// is what makes an inserted `;` or a renamed word one `-` and one `+` line, and
// an insertion after the error line just `+` lines. The first `+` line carries
// the line number, the rest `..`, because new lines have no number of their own
// in the file being compiled.
std::string renderPatchDiff(const AISuggestion &s) {
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
    emit(Colors::BRIGHT_RED + "- " + std::to_string(s.patchStart + i) + " | " +
         oldLines[i] + Colors::RESET);
  for (size_t i = head; i < newLines.size() - tail; ++i)
    emit(Colors::BRIGHT_GREEN + "+ " +
         (i == head ? std::to_string(s.patchStart + head) : std::string("..")) +
         " | " + newLines[i] + Colors::RESET);
  return out;
}

// ── Item 1: deterministic-fix layer ─────────────────────────────────────────
//
// A diagnostic whose raising site already holds the exact edit (the rename an
// unambiguous "did you mean" names, a generic method's missing `<T>`, the `;`
// a statement lacks -- DiagnosticHints::FixIts, ErrorCategory.h, and the
// withFixIt() family in Errors.h for which sites earn one) is fixed here,
// BEFORE the model is ever asked: the fix-its are applied to a fresh sandbox
// and validated by the same ApplyAndValidateFix() every model suggestion
// goes through. A wrong fix-it is therefore exactly as safe as a wrong model
// guess: it can only fail validation, never apply bad code. It deliberately
// does not go through verifySuggestion(): that function searches for WHERE a
// model's copied original_code is (locateOriginal()), and a fix-it already says
// where, to the column. Returns an invalid (default-constructed) AISuggestion when there is
// nothing to try, the edits do not fit the source, or the sandbox rejects
// them -- the caller (validateAndApplySuggestion()) then falls through to the
// ordinary model path, unaffected.

// Applies `FixIts` to SourceLines without touching them. On success fills the
// line range [firstLine, lastLine] the edits span, the patched text that
// replaces exactly that range ('\n'-joined, so it can be spliced like a model's
// new_code), and a short "'old' -> 'new'" label per edit for the status line.
// The before/after display is not built here: renderPatchDiff() makes it from
// the range and text, the same way for a model suggestion.
// Fails (returns false) for an edit outside the source, two edits that overlap
// or tie on one position (their order would be a guess), or edits that change
// nothing.
static bool applyFixIts(const std::vector<FixIt> &FixIts, int errorLine,
                        int &firstLine, int &lastLine, std::string &patched,
                        std::vector<std::string> &labels) {
  const int n = static_cast<int>(SourceLines.size());
  if (FixIts.empty())
    return false;

  struct Edit {
    int line, start, end; // 0-based start/end offsets into the line
    const std::string *replacement;
  };
  std::vector<Edit> edits;
  for (const FixIt &F : FixIts) {
    const int line = F.Line == 0 ? errorLine : F.Line;
    if (line < 1 || line > n)
      return false;
    const int len = static_cast<int>(SourceLines[line - 1].size());
    if (F.StartCol < 1 || F.EndCol < F.StartCol || F.EndCol > len + 1)
      return false;
    edits.push_back({line, F.StartCol - 1, F.EndCol - 1, &F.Replacement});
  }
  std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) {
    return a.line != b.line ? a.line < b.line : a.start < b.start;
  });
  for (size_t i = 1; i < edits.size(); ++i)
    if (edits[i].line == edits[i - 1].line &&
        (edits[i].start < edits[i - 1].end ||
         (edits[i].start == edits[i - 1].start)))
      return false;

  firstLine = edits.front().line;
  lastLine = edits.back().line;

  auto isIdent = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

  patched.clear();
  bool changed = false;
  size_t next = 0;
  for (int line = firstLine; line <= lastLine; ++line) {
    const std::string &orig = SourceLines[line - 1];
    std::string out;
    size_t at = 0;
    while (next < edits.size() && edits[next].line == line) {
      const Edit &e = edits[next++];
      out += orig.substr(at, e.start - at);
      out += *e.replacement;
      at = e.end;
      if (orig.compare(e.start, e.end - e.start, *e.replacement) != 0)
        changed = true;

      // A short "'old' -> 'new'" for the status line: the edit widened to the
      // whole word it touches, so `fn` + `<T>` reads `'fn' -> 'fn<T>'` and a
      // ';' after `5` reads `'5' -> '5;'`, not a bare `'' -> '<T>'`.
      size_t lo = e.start, hi = e.end;
      const std::string removed = orig.substr(e.start, e.end - e.start);
      if (removed.empty()) {
        // An insertion: show the unbroken run of text it lands against
        // (`fn`, `5`, `io::printline("hi")`), capped so a long call stays one
        // readable status line.
        while (lo > 0 && !std::isspace(static_cast<unsigned char>(orig[lo - 1])) &&
               e.start - lo < 24)
          --lo;
        // Nothing before it on the line (`pub` moved to sit before `fn`):
        // show the run it lands in front of instead.
        if (lo == e.start)
          while (hi < orig.size() &&
                 !std::isspace(static_cast<unsigned char>(orig[hi])) &&
                 hi - e.end < 24)
            ++hi;
      } else {
        while (isIdent(removed.front()) && lo > 0 && isIdent(orig[lo - 1]))
          --lo;
      }
      while (!removed.empty() && isIdent(removed.back()) && hi < orig.size() &&
             isIdent(orig[hi]))
        ++hi;
      const std::string before = orig.substr(lo, hi - lo);
      const std::string after = orig.substr(lo, e.start - lo) + *e.replacement +
                                orig.substr(e.end, hi - e.end);
      auto trim = [](std::string s) {
        const char *ws = " \t";
        s.erase(0, s.find_first_not_of(ws) == std::string::npos
                       ? s.size()
                       : s.find_first_not_of(ws));
        s.erase(s.find_last_not_of(ws) == std::string::npos
                    ? 0
                    : s.find_last_not_of(ws) + 1);
        return s;
      };
      const std::string label = "'" + trim(before) + "' -> '" + trim(after) + "'";
      if (std::find(labels.begin(), labels.end(), label) == labels.end())
        labels.push_back(label);
    }
    out += orig.substr(at);
    if (line > firstLine)
      patched += "\n";
    patched += out;

  }
  return changed;
}

// `summaryOut` receives the "'old' -> 'new'" status text on success.
static AISuggestion tryDeterministicFix(int errorLine,
                                        const DiagnosticHints &Hints,
                                        ErrorCategory category,
                                        std::string &summaryOut) {
  AISuggestion result;
  int first = 0, last = 0;
  std::string patched;
  std::vector<std::string> labels;
  if (!applyFixIts(Hints.FixIts, errorLine, first, last, patched, labels))
    return result;

  std::string summary;
  for (const auto &label : labels)
    summary += (summary.empty() ? "" : ", ") + label;

  result.fix_mode = "MODIFY";
  // The same fields a model suggestion fills, so everything downstream treats
  // the two alike: the lines the edits span, and what they became.
  for (int line = first; line <= last; ++line)
    result.original_code += (line > first ? "\n" : "") + SourceLines[line - 1];
  result.new_code = patched;
  result.patchStart = first;
  result.patchEnd = last;
  result.patchText = patched;
  result.suggested_comment = "Compiler fix: " + summary + ".";
  result.explanation = Hints.FixReason;
  result.self_reported_category = getCategoryName(category);

  // The model path's sandbox gate, minus the search for where the code goes:
  // the fix-its already name the range [first, last].
  auto sandbox = CreateSandbox();
  if (!sandbox)
    return AISuggestion{};
  if (!ApplyAndValidateFix(*sandbox, errorLine, first, last, patched, result,
                           /*reportFailure=*/std::getenv("TURF_SLM_DEBUG") !=
                               nullptr))
    return AISuggestion{}; // didn't validate -- not this fix, try the model
  std::string joined;
  for (const auto &line : sandbox->getModifiedSource()) {
    joined += line;
    joined += "\n";
  }
  result.appliedSource = std::move(joined);
  summaryOut = summary;
  // Sandbox-proven, same as a validated model suggestion: finalizeConfidence()
  // clamps to >= 0.9 regardless of the structural-prior argument once
  // sandboxValidated is true (see its own doc comment above), so the exact
  // prior value here doesn't matter.
  result.confidence = finalizeConfidence(1.0f, /*sandboxValidated=*/true);
  result.sandbox_validated = true;
  result.valid = true;
  return result;
}

// High-Level Auto-Fix API

// Retry cap for the loop below -- matches the roadmap wording exactly
// ("Retry up to 3x on TypeChecker failure, feeding the error back into the
// prompt"): docs/planning/ROADMAP_v1.0.0_COMPLETE.md §7's `--smart` Retry
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
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine, const std::string &hint,
                                        const std::vector<RelatedLocation> &related,
                                        ErrorCategory category,
                                        const DiagnosticHints &hints,
                                        const std::string &errorCode) {
  // Item 1: try the deterministic-fix layer FIRST -- fetchSuggestion() (and
  // therefore the model) is never called at all if this validates. See
  // tryDeterministicFix()'s own doc comment for the safety argument.
  if (!hints.FixIts.empty()) {
    std::string summary;
    AISuggestion detFix = tryDeterministicFix(errorLine, hints, category, summary);
    if (detFix.valid) {
      // scripts/slm_deterministic_fix_smoke.sh greps this line, including the
      // `'old' -> 'new'` summary and "the model was never called".
      std::cerr << Colors::BRIGHT_GREEN << "[Deterministic Fix] ✓ " << summary
                << " validated -- the model was never called."
                << Colors::RESET << "\n";
      return detFix;
    }
  }

  AISuggestion suggestion;
  std::string previousAttemptFailed; // empty on attempt 1

  for (int attempt = 1; attempt <= kMaxSlmAttempts; ++attempt) {
    suggestion = fetchSuggestion(errorMessage, errorLine, hint, related,
                                 category, previousAttemptFailed, hints,
                                 errorCode);

    if (!suggestion.valid) {
      // The SLM itself failed/timed out/produced unparsable JSON -- not a
      // sandbox rejection, so retrying with the same unreachable model
      // wouldn't help. Bail immediately, same as the old single-shot path.
      return suggestion;
    }

    // Skip validation for NONE fixes (no patch to apply)
    if (suggestion.fix_mode == "NONE") {
      std::cerr << Colors::BRIGHT_BLACK
                << "[Sandbox] Fix mode is NONE, skipping validation"
                << Colors::RESET << "\n";
      return suggestion;
    }

    // Verify the suggestion in a sandbox
    std::string diagnosticText;
    bool isValid =
        verifySuggestion(suggestion, errorLine, &diagnosticText, related);

    // Finalize confidence NOW — after sandbox validation.
    // The structural prior (computed before sandbox ran) is only a prior;
    // the sandbox result is the strongest possible signal (empirical proof).
    // A proven-correct fix is clamped to >= 0.9; an unvalidated fix is capped
    // at <= 0.5, making it impossible for a pre-validation guess to veto a
    // validated fix, or to promote an unvalidated one past the display gate.
    suggestion.confidence = finalizeConfidence(suggestion.confidence, isValid);

    const bool replaying = std::getenv("TURF_SLM_REPLAY") != nullptr;
    if (!replaying && !suggestion.cacheKey.empty()) {
      if (isValid && !suggestion.fromCache)
        writeSlmCache(suggestion.cacheKey, suggestion);
      else if (!isValid && suggestion.fromCache)
        evictSlmCache(suggestion.cacheKey);
    }

    if (isValid) {
      suggestion.valid = true;
      std::cerr << Colors::BRIGHT_GREEN << "[Sandbox] ✓ Suggestion validated"
                << (attempt > 1 ? " (attempt " + std::to_string(attempt) + "/" +
                                      std::to_string(kMaxSlmAttempts) + ")"
                                : "")
                << Colors::RESET << "\n";
      if (std::getenv("TURF_SLM_DEBUG")) {
        std::cerr << "Final Confidence (post-sandbox): " << suggestion.confidence << "\n";
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
