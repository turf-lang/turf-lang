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

// Extracts an unquoted integer JSON field (e.g. "target_line": 42). Returns
// 0 (the "unset" sentinel -- see AISuggestion::target_line) if the field is
// missing or not a plain integer.
static int extractJsonFieldInt(const std::string &json,
                               const std::string &field) {
  std::string key = "\"" + field + "\":";
  size_t pos = json.find(key);
  if (pos == std::string::npos)
    return 0;
  pos += key.length();

  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
    pos++;
  }

  bool negative = false;
  if (pos < json.length() && json[pos] == '-') {
    negative = true;
    pos++;
  }

  size_t start = pos;
  while (pos < json.length() && std::isdigit(static_cast<unsigned char>(json[pos])))
    pos++;

  if (pos == start)
    return 0;

  int value = std::atoi(json.substr(start, pos - start).c_str());
  return negative ? -value : value;
}

// Defensive cleanup for a known small-model failure mode: despite explicit
// prompt instructions not to, the model occasionally echoes the "N |  code"
// line-number-prefix formatting used to display context lines (see
// getCodeContext()/getCodeContextWithPinnedLines()) back inside
// replacement_line itself, e.g. "6 |     mut b: int = 5;" instead of just
// "    mut b: int = 5;". That prefix is never valid Turf-Lang syntax at the
// start of a line, so stripping it here is safe and turns an otherwise
// guaranteed sandbox-parse failure into a usable patch.
//
// A multi-line replacement echoes the prefix on EVERY line ("11 | fn ...",
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
// replacement_line in a markdown code fence (```turf ... ``` or ``` ... ```)
// despite rule 6 explicitly saying "no markdown code blocks". A leading or
// trailing fence line is never valid Turf-Lang syntax, so stripping it here
// is safe and turns an otherwise guaranteed sandbox-parse failure (the
// parser chokes on a literal "```" token) into a usable patch. Observed
// especially often on multi-line replacement_line values.
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
// model emit \r, \/, \b, \f and \uXXXX -- and `<`/`>` for `<`/`>`
// is a common JSON habit that lands squarely on generic syntax (`fn<T>`),
// so a correct fix reached the sandbox with a literal "<" in it. They
// also find a key by raw substring, so a string value that happens to
// contain `"end_line":` (an explanation quoting the schema) shadows the real
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
      // A grammar-constrained response always has a JSON integer here; a
      // cache file or an unconstrained one may quote it ("12"), which the
      // old extractor silently read as 0 -- "the error's own line".
      auto Int = [&](llvm::StringRef Key) -> int {
        if (auto I = Obj->getInteger(Key))
          return static_cast<int>(*I);
        if (auto S = Obj->getString(Key)) {
          int V = 0;
          if (!S->trim().getAsInteger(10, V))
            return V;
        }
        return 0;
      };
      Out.explanation = Str("explanation");
      Out.fix_mode = Str("fix_mode");
      Out.suggested_comment = Str("suggested_comment");
      Out.code_patch = Str("code_patch");
      Out.replacement_line = Str("replacement_line");
      Out.target_line = Int("target_line");
      Out.end_line = Int("end_line");
      Out.warning = Str("warning");
      Out.self_reported_category = Str(categoryKey);
      return;
    }
  } else {
    llvm::consumeError(Parsed.takeError());
  }
  Out.explanation = extractJsonFieldString(Json, "explanation");
  Out.fix_mode = extractJsonFieldString(Json, "fix_mode");
  Out.suggested_comment = extractJsonFieldString(Json, "suggested_comment");
  Out.code_patch = extractJsonFieldString(Json, "code_patch");
  Out.replacement_line = extractJsonFieldString(Json, "replacement_line");
  Out.target_line = extractJsonFieldInt(Json, "target_line");
  Out.end_line = extractJsonFieldInt(Json, "end_line");
  Out.warning = extractJsonFieldString(Json, "warning");
  Out.self_reported_category = extractJsonFieldString(Json, categoryKey);
}

// Computes a structural prior for confidence from shallow, pre-sandbox signals.
// This is NOT the final confidence — sandbox validation hasn't run yet.
// The score is meant to estimate plausibility; finalizeConfidence() then
// incorporates the strongest signal (empirical sandbox pass/fail) to produce
// the true confidence used by the display gate.
//
//   +0.4  Error category resolved by hardcoded rules (not SLM fallback)
//   +0.2  fix_mode == MODIFY
//   +0.2  replacement_line is non-empty (actual code, not prose)
//   +0.1  replacement_line is a single clean line (no newlines or bare braces)
//   +0.1  replacement_line contains the error keyword on a word boundary
//
// Note: If replacement_line is empty, patchCode is NOT used as a fallback —
// code_patch is prose, and scoring prose as code produces misleading results
// since verifySuggestion() refuses to validate it anyway.
//
// Maximum score: 1.0  |  Minimum: 0.0
static float computeStructuralPrior(const AISuggestion &s, ErrorCategory category,
                                    bool deterministic,
                                    const std::string &contextKeyword,
                                    int errorLine) {
  float score = 0.0f;

  // +0.4 if the category was resolved by deterministic rules (not SLM fallback)
  if (deterministic && category != ErrorCategory::GENERAL)
    score += 0.4f;

  // +0.2 if the model decided a concrete patch is applicable
  if (s.fix_mode == "MODIFY")
    score += 0.2f;

  // Only score replacement_line — code_patch is human-readable prose, not
  // parseable code. If replacement_line is empty, all code-quality checks
  // are skipped entirely (verifySuggestion would reject it regardless).
  const std::string &patchCode = s.replacement_line;

  if (!patchCode.empty()) {
    // Hard-reject: the model echoed back the original text unchanged.
    // This is a known small-model failure mode. An identical replacement is
    // actively harmful — it passes every other check while fixing nothing.
    // Compared against the actual text being patched: s.target_line (if set
    // and in range) through s.end_line when a multi-line range was given,
    // otherwise just the single line (s.target_line if set, else errorLine)
    // -- comparing against errorLine when target_line points elsewhere, or
    // against only the first line of a multi-line range, would check the
    // wrong original text entirely.
    int compareStart = (s.target_line > 0 &&
                        s.target_line <= static_cast<int>(SourceLines.size()))
                           ? s.target_line
                           : errorLine;
    int compareEnd = (s.end_line >= compareStart &&
                      s.end_line <= static_cast<int>(SourceLines.size()))
                          ? s.end_line
                          : compareStart;
    std::string originalText;
    if (compareStart >= 1 &&
        compareEnd <= static_cast<int>(SourceLines.size())) {
      for (int ln = compareStart; ln <= compareEnd; ++ln) {
        if (ln > compareStart)
          originalText += "\n";
        originalText += SourceLines[ln - 1];
      }
    }
    // Trim leading/trailing whitespace for comparison
    auto trim = [](const std::string &s) {
      size_t l = s.find_first_not_of(" \t\r\n");
      size_t r = s.find_last_not_of(" \t\r\n");
      return (l == std::string::npos) ? std::string{} : s.substr(l, r - l + 1);
    };
    if (!originalText.empty() && trim(patchCode) == trim(originalText)) {
      // Hard override — this score can never clear the gate.
      return 0.0f;
    }

    // +0.2 if a non-empty replacement_line was produced
    score += 0.2f;

    // +0.1 if it is a single, clean line (no embedded newlines or lone
    // braces). A genuine multi-line range fix (end_line > compareStart) is
    // expected to contain newlines, so it simply doesn't earn this bonus
    // rather than being penalized -- the sandbox's empirical pass/fail is
    // what ultimately decides those cases (see finalizeConfidence()).
    bool hasNewline = patchCode.find('\n') != std::string::npos;
    bool isBareBrace = (patchCode == "{" || patchCode == "}");
    if (!hasNewline && !isBareBrace)
      score += 0.1f;

    // +0.1 if the replacement contains the keyword at a word boundary.
    // Raw substring matching is avoided because short identifiers (e.g. i, n)
    // appear inside unrelated tokens, making the signal nearly unconditional.
    if (!contextKeyword.empty()) {
      size_t kwPos = 0;
      while ((kwPos = patchCode.find(contextKeyword, kwPos)) != std::string::npos) {
        // Check that the characters immediately surrounding the match are not
        // alphanumeric or underscore (i.e. it is a word boundary).
        bool leftOk = (kwPos == 0) ||
                      (!std::isalnum(static_cast<unsigned char>(patchCode[kwPos - 1])) &&
                       patchCode[kwPos - 1] != '_');
        bool rightOk =
            (kwPos + contextKeyword.size() >= patchCode.size()) ||
            (!std::isalnum(static_cast<unsigned char>(
                 patchCode[kwPos + contextKeyword.size()])) &&
             patchCode[kwPos + contextKeyword.size()] != '_');
        if (leftOk && rightOk) {
          score += 0.1f;
          break; // only count once
        }
        kwPos += contextKeyword.size();
      }
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
static std::string slmCompilerFingerprint() {
  llvm::sys::fs::file_status Status;
  if (llvm::sys::fs::status(CompilerExecutablePath, Status))
    return ""; // stat() failed -- never matches a real cache entry below
  auto Mtime = Status.getLastModificationTime().time_since_epoch().count();
  return std::to_string(Status.getSize()) + ":" + std::to_string(Mtime);
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
       << "\"suggested_comment\":\"" << escapeJsonString(S.suggested_comment) << "\","
       << "\"code_patch\":\"" << escapeJsonString(S.code_patch) << "\","
       << "\"replacement_line\":\"" << escapeJsonString(S.replacement_line) << "\","
       << "\"target_line\":" << S.target_line << ","
       << "\"end_line\":" << S.end_line << ","
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
  //    Likewise, code_patch must be constrained to Turf-Lang syntax only to
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
  //     matters because a wrong `replacement_line` only gets caught by
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
      "replacement_line gets sandbox-validated before a user sees it; "
      "explanation and suggested_comment do not.\n\n"
      "Error category: " +
      categoryName + "\n\n" + docContent +
      "\n"
      "RULES:\n"
      "1. Follow Turf-Lang type and ownership rules exactly, as shown in "
      "the documentation above -- not general programming intuition.\n"
      "2. Prefer a SINGLE LINE of valid Turf-Lang syntax in replacement_line.\n"
      "   Do NOT use C, Java, Python, or any other language syntax.\n"
      "   Do NOT include multiple statements, semicolons, or braces unless\n"
      "   they are required by Turf-Lang syntax for that construct -- unless\n"
      "   the fix genuinely needs more than one line (see rule 5), in which\n"
      "   case replacement_line may span multiple lines. NEVER wrap "
      "replacement_line\n"
      "   in a markdown code fence (```) -- it is fed directly to the "
      "compiler,\n"
      "   not rendered as markdown, so a fence is a guaranteed syntax "
      "error.\n"
      "3. If you cannot produce a valid Turf-Lang patch, set fix_mode to NONE\n"
      "   and leave code_patch empty. This is not a failure -- 'NONE' is\n"
      "   the right answer whenever you're not sure, and is far better than\n"
      "   a wrong patch.\n"
      "4. Set fix_mode to one of: MODIFY, NONE.\n"
      "   Use MODIFY when you have a concrete patch (single-line or, per "
      "rule 5,\n"
      "   a small multi-line block). Use NONE when you are not confident "
      "enough\n"
      "   to suggest a specific change.\n"
      "5. target_line/end_line: usually target_line=0, end_line=0 (fix is a\n"
      "   single line, the error's own line). If the real fix site is a\n"
      "   different line shown above (e.g. a declaration), set target_line "
      "to\n"
      "   that line number. If the fix genuinely requires replacing MORE "
      "THAN\n"
      "   ONE existing line (e.g. wrapping a statement in a new loop, "
      "adding a\n"
      "   multi-line function signature), set target_line to the first "
      "line of\n"
      "   that range and end_line to the last line (both from the code "
      "shown\n"
      "   above), and put the ENTIRE replacement block in replacement_line, "
      "with\n"
      "   its lines separated by \\n. Keep the range as small as it can "
      "possibly\n"
      "   be -- never replace more existing lines than the fix strictly "
      "needs.\n"
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
        "Your previous suggested fix did not compile -- "
        "\"previous_attempt_failed\" above is the compiler's own error for "
        "that attempt. Produce a DIFFERENT replacement_line that actually "
        "addresses that specific feedback, not the same patch again.\n";
  }
  userPrompt +=
      "Respond with ONLY this JSON object — fill every field yourself:\n"
      "{\"explanation\": <string>, \"fix_mode\": <MODIFY|NONE>, "
      "\"suggested_comment\": <string>, \"code_patch\": <human-readable "
      "description of what changed>, "
      "\"replacement_line\": <THE EXACT Turf-Lang code that replaces "
      "target_line, or the whole [target_line, end_line] range if end_line "
      "is set — no explanation, no prose, just the raw code, lines "
      "separated by \\n if more than one>, "
      "\"target_line\": <0, or a different line number — see rule 5>, "
      "\"end_line\": <0, or the last line of a multi-line range — see rule "
      "5>, "
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
    suggestion.replacement_line = stripLineNumberPrefix(
        stripMarkdownFences(suggestion.replacement_line));

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
        errorLine);

    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "Structural Prior (pre-sandbox): "
                << suggestion.confidence << "\n";
      std::cerr << "  category_deterministic=" << categoryWasDeterministic
                << " fix_mode=" << suggestion.fix_mode
                << " replacement_line_empty=" << suggestion.replacement_line.empty()
                << " target_line=" << suggestion.target_line
                << " end_line=" << suggestion.end_line << "\n";
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

// Recover the intended [start, end] source range for a multi-line
// replacement_line whose end_line the model left unset (== 0 or an
// out-of-range/backwards value that didn't resolve to a real range).
//
// This is the fix for the single most impactful defect found while
// benchmarking the pipeline (benchmark writeup A.4 §7, kept outside this
// repository):
// when the model decides the right fix is to "regenerate an enclosing
// declaration" (a struct / error / machine / function block), it naturally
// emits the WHOLE corrected block in replacement_line but still reports
// target_line as just the error's own line and leaves end_line at 0. The
// old code then spliced that N-line block into that ONE line, leaving the
// other M-1 original block lines in place and producing a nested, duplicated
// construct that fails re-validation no matter how correct the model's fix
// idea was. This reproduced verbatim on state machines, structs, AND error
// types -- i.e. across entirely ordinary syntax, not just novel constructs --
// and is a large part of why 06_structs / 25_errors / 26_state_machines
// scored so low. A 1.5B model does not reliably set end_line no matter how
// the prompt is worded, so the robust fix belongs here, in the harness.
//
// Recovery strategy: content-anchoring. The first and last lines of a
// regenerated block are structural markers ("struct User {" ... "}",
// "machine ConnectionState {" ... "}") that appear verbatim in the source.
// Find the source line matching the replacement's first line, then the next
// source line matching its last line, and accept that range only if it is
// strictly multi-line (end > start) and brackets target_line -- so a spurious
// match can't run away. A wrong guess is still completely safe: the spliced
// result is re-parsed, re-resolved, and re-typechecked in full by the sandbox
// (same path as every other fix), so it can only ever fail validation, never
// silently apply incorrect code.
//
// NOTE this deliberately does NOT fire for the "wrap a single statement in a
// new loop" shape (fix A.4 §5): there the replacement is multi-line but is
// meant to replace exactly ONE original line, its first line ("while ... {")
// does not appear in the source, so anchoring fails and the caller correctly
// falls back to a single-line [target, target] splice -- which is exactly
// right for a wrap/insert.
static bool inferBlockRange(const std::string &replacement, int targetLine,
                            int &outStart, int &outEnd) {
  auto trim = [](const std::string &s) {
    size_t l = s.find_first_not_of(" \t\r\n");
    size_t r = s.find_last_not_of(" \t\r\n");
    return (l == std::string::npos) ? std::string{} : s.substr(l, r - l + 1);
  };

  // Split replacement into trimmed, non-empty lines.
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= replacement.size()) {
    size_t nl = replacement.find('\n', pos);
    std::string raw = (nl == std::string::npos)
                          ? replacement.substr(pos)
                          : replacement.substr(pos, nl - pos);
    std::string t = trim(raw);
    if (!t.empty())
      lines.push_back(t);
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  if (lines.size() < 2)
    return false;

  const std::string &firstRepl = lines.front();
  const std::string &lastRepl = lines.back();
  const int n = static_cast<int>(SourceLines.size());
  if (targetLine < 1 || targetLine > n)
    return false;

  // For each source line matching the replacement's first line, find the next
  // source line matching its last line; accept the tightest resulting range
  // that is strictly multi-line and brackets targetLine.
  int bestStart = 0, bestEnd = 0;
  for (int s = 1; s <= n; ++s) {
    if (trim(SourceLines[s - 1]) != firstRepl)
      continue;
    for (int e = s + 1; e <= n; ++e) {
      if (trim(SourceLines[e - 1]) != lastRepl)
        continue;
      if (s <= targetLine && targetLine <= e &&
          (bestStart == 0 || (e - s) < (bestEnd - bestStart))) {
        bestStart = s;
        bestEnd = e;
      }
      break; // only the first lastRepl match after this s
    }
  }
  if (bestStart == 0)
    return false;
  outStart = bestStart;
  outEnd = bestEnd;
  return true;
}

// How alike two source lines are, ignoring surrounding whitespace: 1.0 for
// identical, else 1 - (edit distance / longer length).
static double lineSimilarity(const std::string &a, const std::string &b) {
  auto trim = [](const std::string &s) {
    size_t l = s.find_first_not_of(" \t\r");
    size_t r = s.find_last_not_of(" \t\r");
    return (l == std::string::npos) ? std::string{} : s.substr(l, r - l + 1);
  };
  std::string x = trim(a), y = trim(b);
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

// Find the source range a replacement most plausibly rewrites, from its
// content alone (issue #267): the window around `anchor` whose lines best
// line up with the replacement's lines. inferBlockRange() needs the
// replacement's first and last lines VERBATIM in the source, but a fix
// usually changes exactly one of them (`fn Box.count()` -> `fn<T>
// Box.count()`), so it declined on the very block rewrites it was written
// for. Here lines match when they are merely similar.
//
// Scored as a sequence alignment of the replacement's lines against each
// candidate window [s, e] containing `anchor` (at most two lines longer than
// the replacement): an aligned pair earns its similarity (pairs under 0.6
// don't align; a line of two characters or fewer, like `}`, earns 0.3 of it,
// since a lone brace matches every block's end); a window line left
// unaligned costs 0.5 (0.25 if blank), because the splice deletes it; a
// replacement line left unaligned is a new line and costs nothing. The best
// window must score at least 0.6 -- one convincing match -- or there is no
// answer. Ties go to the smaller window, then the one nearest `anchor`.
//
// A wrong answer here is as safe as a wrong model line number: the result
// is only a candidate verifySuggestion() hands to the full sandbox re-check.
static bool alignReplacement(const std::string &replacement, int anchor,
                             int &outStart, int &outEnd) {
  const int n = static_cast<int>(SourceLines.size());
  if (anchor < 1 || anchor > n)
    return false;
  std::vector<std::string> repl;
  size_t pos = 0;
  while (true) {
    size_t nl = replacement.find('\n', pos);
    std::string line = replacement.substr(pos, nl == std::string::npos
                                                   ? std::string::npos
                                                   : nl - pos);
    if (line.find_first_not_of(" \t\r") != std::string::npos)
      repl.push_back(line);
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  const int k = static_cast<int>(repl.size());
  if (k == 0)
    return false;

  auto isBlank = [](const std::string &s) {
    return s.find_first_not_of(" \t\r") == std::string::npos;
  };
  auto pairScore = [&](const std::string &r, const std::string &src) {
    double sim = lineSimilarity(r, src);
    if (sim < 0.6)
      return -1.0; // does not align
    size_t len = r.find_first_not_of(" \t") == std::string::npos
                     ? 0
                     : r.substr(r.find_first_not_of(" \t")).size();
    return len <= 2 ? sim * 0.3 : sim;
  };

  double bestScore = 0.0;
  int bestStart = 0, bestEnd = 0;
  for (int s = std::max(1, anchor - k - 1); s <= anchor; ++s) {
    for (int e = anchor; e <= std::min(n, s + k + 1); ++e) {
      const int m = e - s + 1;
      // dp[i][j]: best score aligning repl[0..i) with window lines [0..j).
      std::vector<std::vector<double>> dp(k + 1, std::vector<double>(m + 1));
      for (int j = 1; j <= m; ++j)
        dp[0][j] = dp[0][j - 1] -
                   (isBlank(SourceLines[s + j - 2]) ? 0.25 : 0.5);
      for (int i = 1; i <= k; ++i) {
        dp[i][0] = dp[i - 1][0];
        for (int j = 1; j <= m; ++j) {
          const std::string &src = SourceLines[s + j - 2];
          double best = std::max(dp[i - 1][j],
                                 dp[i][j - 1] - (isBlank(src) ? 0.25 : 0.5));
          double ps = pairScore(repl[i - 1], src);
          if (ps >= 0)
            best = std::max(best, dp[i - 1][j - 1] + ps);
          dp[i][j] = best;
        }
      }
      double score = dp[k][m];
      auto dist = [&](int a, int b) {
        return std::abs((a + b) / 2 - anchor);
      };
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
  // The window had to contain `anchor`, which a model often points at the
  // blank line beside the real code; the splice should not eat that blank
  // line too. A blank edge line never earns score (only costs), so dropping
  // it can only improve the alignment.
  while (bestStart < bestEnd && isBlank(SourceLines[bestStart - 1]))
    ++bestStart;
  while (bestEnd > bestStart && isBlank(SourceLines[bestEnd - 1]))
    --bestEnd;
  outStart = bestStart;
  outEnd = bestEnd;
  return true;
}

bool verifySuggestion(AISuggestion &suggestion, int errorLine,
                      std::string *outDiagnostics) {
  // Use replacement_line (exact Turf-Lang code) for sandbox validation.
  // code_patch is a human-readable description and must NOT be fed to the
  // parser. If replacement_line is empty, there is no valid code to validate.
  if (suggestion.replacement_line.empty()) {
    return false; // no valid code to validate; code_patch is prose-only
  }
  const std::string &fixLine = suggestion.replacement_line;
  const int n = static_cast<int>(SourceLines.size());
  const bool debug = std::getenv("TURF_SLM_DEBUG") != nullptr;

  // target_line == 0 means "same line as the error" (the common case). A
  // positive target_line is only honored if it's within the actual source
  // range -- a model-hallucinated out-of-range value silently falls back to
  // errorLine rather than crashing applyFix().
  int applyLine = errorLine;
  if (suggestion.target_line > 0 &&
      suggestion.target_line <= static_cast<int>(SourceLines.size())) {
    applyLine = suggestion.target_line;
  }

  // end_line == 0 (or an out-of-range/backwards value) means "just
  // applyLine" -- a single-line edit, the common case. Only honored as a
  // genuine range when it's within source bounds and doesn't precede
  // applyLine -- a hallucinated value silently falls back to a single-line
  // edit rather than crashing applyFix() with an inverted range.
  int applyEndLine = applyLine;
  if (suggestion.end_line >= applyLine &&
      suggestion.end_line <= static_cast<int>(SourceLines.size())) {
    applyEndLine = suggestion.end_line;
  }

  // Recover the intended range when replacement_line is multi-line but
  // end_line didn't resolve to a real range above (applyEndLine collapsed
  // back to applyLine). Two distinct shapes land here, and only one needs
  // recovery:
  //   (a) block rewrite -- the model regenerated a whole enclosing
  //       declaration (struct/error/machine/fn) and left end_line at 0.
  //       inferBlockRange() content-anchors the replacement's first/last
  //       lines to the source to recover [start, end]. This is the A.4 §7
  //       fix -- without it these validate at ~0% despite a correct fix idea.
  //   (b) wrap/insert -- the model wrapped ONE original line in a new
  //       construct (e.g. a bare `break;` into a loop). The replacement is
  //       multi-line but replaces exactly one source line; its first line
  //       does not appear in the source, so inferBlockRange() declines and we
  //       correctly keep the [applyLine, applyLine] single-line splice.
  // Either way the sandbox below re-validates the full spliced result, so a
  // mis-inferred range can only fail validation, never apply bad code.
  //
  // Candidate ranges, tried in order until one validates (issue #267). The
  // model's line numbers are a hint, not ground truth: a 1.5B model reads
  // the `>> 11 |` gutter and still answers target_line=10 (the blank line
  // above), and splicing a correct `fn<T> Box.count(): int {...}` into that
  // blank line left the original `fn Box.count()` below it -- the same
  // error again, which the retry loop then reported to the model as "your
  // fix did not compile". The fix was right; only its placement was wrong,
  // and every retry was poisoned by that false feedback. So the harness
  // also tries where the replacement's own CONTENT says it belongs
  // (alignReplacement()), around the model's line and around the error's.
  // Order keeps every placement that validated before this change winning
  // exactly as before: the recovered block range (when the old code would
  // have used it), then the model's literal range, then the aligned ones,
  // then the error's own line. Each candidate gets a fresh sandbox, so a
  // failed one leaves nothing behind; all of them must clear the same full
  // re-check, so a wider search can only find more fixes that compile and
  // clear the error, never apply one that doesn't.
  struct Candidate {
    int start, end;
    const char *why;
  };
  std::vector<Candidate> candidates;
  auto add = [&](int s, int e, const char *why) {
    if (s < 1 || e < s || e > n)
      return;
    for (const auto &c : candidates)
      if (c.start == s && c.end == e)
        return;
    candidates.push_back({s, e, why});
  };
  const bool multiLine = fixLine.find('\n') != std::string::npos;
  if (multiLine && applyEndLine == applyLine) {
    int inferredStart = 0, inferredEnd = 0;
    if (inferBlockRange(fixLine, applyLine, inferredStart, inferredEnd) ||
        inferBlockRange(fixLine, errorLine, inferredStart, inferredEnd))
      add(inferredStart, inferredEnd, "block anchors");
  }
  add(applyLine, applyEndLine, "model's range");
  int alignStart = 0, alignEnd = 0;
  // The aligned range whose diagnostics are fed back to the model if no
  // candidate validates: the best evidence of where the model MEANT its
  // code to go, so the feedback describes its code, not a misplacement.
  int feedbackStart = 0, feedbackEnd = 0;
  if (alignReplacement(fixLine, applyLine, alignStart, alignEnd)) {
    add(alignStart, alignEnd, "content-aligned (model's line)");
    feedbackStart = alignStart;
    feedbackEnd = alignEnd;
  }
  if (alignReplacement(fixLine, errorLine, alignStart, alignEnd)) {
    add(alignStart, alignEnd, "content-aligned (error line)");
    if (feedbackStart == 0) {
      feedbackStart = alignStart;
      feedbackEnd = alignEnd;
    }
  }
  add(errorLine, errorLine, "error line");
  if (feedbackStart == 0 && !candidates.empty()) {
    feedbackStart = candidates.front().start;
    feedbackEnd = candidates.front().end;
  }

  std::string feedback;
  for (const auto &c : candidates) {
    auto sandbox = CreateSandbox();
    if (!sandbox) {
      std::cerr << Colors::BRIGHT_BLACK << "[Verify] Failed to create sandbox"
                << Colors::RESET << "\n";
      return false;
    }
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
      feedback += "\nYour replacement_line replaced line";
      feedback += c.start == c.end ? " " + std::to_string(c.start)
                                   : "s " + std::to_string(c.start) + "-" +
                                         std::to_string(c.end);
      feedback += "; afterwards the code read:\n";
      for (int i = lo; i <= hi; ++i)
        feedback += "  " + patched[i - 1] + "\n";
    }
  }

  if (outDiagnostics)
    *outDiagnostics = feedback;
  return false;
}

// ── Item 1: deterministic-fix layer ─────────────────────────────────────────
//
// Given an unambiguous rename candidate (DeterministicOldName/NewName -- see
// DiagnosticHints's own doc comment, ErrorCategory.h, for exactly which
// call sites earn one and why "unambiguous" is required), synthesize a
// word-boundary search/replace patch on the error's own line and
// sandbox-verify it through verifySuggestion() -- the EXACT SAME path every
// model-produced suggestion goes through. A wrong guess here is therefore
// exactly as safe as a wrong model guess: it can only fail sandbox
// validation, never apply bad code. Returns an invalid (default-constructed)
// AISuggestion when there's no hint to try or the sandboxed replace didn't
// validate -- either way the caller (validateAndApplySuggestion()) falls
// through to the ordinary model path, unaffected.
static AISuggestion tryDeterministicFix(int errorLine,
                                        const DiagnosticHints &Hints,
                                        ErrorCategory category) {
  AISuggestion result;
  if (Hints.DeterministicOldName.empty() || Hints.DeterministicNewName.empty())
    return result;
  if (errorLine < 1 || errorLine > static_cast<int>(SourceLines.size()))
    return result;

  const std::string &Line = SourceLines[errorLine - 1];
  const std::string &OldName = Hints.DeterministicOldName;
  std::string Patched;
  Patched.reserve(Line.size());
  size_t Pos = 0;
  bool ReplacedAny = false;
  while (Pos < Line.size()) {
    size_t Found = Line.find(OldName, Pos);
    if (Found == std::string::npos) {
      Patched += Line.substr(Pos);
      break;
    }
    Patched += Line.substr(Pos, Found - Pos);
    // Word-boundary check on both sides, same shape as
    // computeStructuralPrior()'s keyword-boundary check above -- a bare
    // substring match would wrongly rewrite e.g. "cnt" inside "recnt" for an
    // OldName of "cnt".
    bool LeftOk = (Found == 0) ||
        (!std::isalnum(static_cast<unsigned char>(Line[Found - 1])) &&
         Line[Found - 1] != '_');
    size_t After = Found + OldName.size();
    bool RightOk = (After >= Line.size()) ||
        (!std::isalnum(static_cast<unsigned char>(Line[After])) &&
         Line[After] != '_');
    if (LeftOk && RightOk) {
      Patched += Hints.DeterministicNewName;
      ReplacedAny = true;
    } else {
      Patched += OldName;
    }
    Pos = After;
  }
  if (!ReplacedAny)
    return result; // OldName never appeared as a real token on this line

  result.fix_mode = "MODIFY";
  result.replacement_line = Patched;
  // target_line/end_line left at 0 ("same line as the error, single line") --
  // verifySuggestion() already treats that as the common case.
  result.code_patch = "Replaced '" + OldName + "' with '" +
                      Hints.DeterministicNewName + "'.";
  if (Hints.DeterministicReason.empty()) {
    result.suggested_comment = "Looks like a typo -- I replaced '" + OldName +
                               "' with '" + Hints.DeterministicNewName + "'.";
    result.explanation = "'" + OldName + "' isn't declared anywhere visible, "
                         "but '" + Hints.DeterministicNewName + "' is an exact, "
                         "unambiguous match nearby, so this is almost "
                         "certainly a typo.";
  } else {
    result.suggested_comment = "I replaced '" + OldName + "' with '" +
                               Hints.DeterministicNewName + "'.";
    result.explanation = Hints.DeterministicReason;
  }
  result.self_reported_category = getCategoryName(category);

  if (!verifySuggestion(result, errorLine, nullptr)) {
    return AISuggestion{}; // didn't validate -- not this rename, try the model
  }
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
  if (!hints.DeterministicOldName.empty()) {
    AISuggestion detFix = tryDeterministicFix(errorLine, hints, category);
    if (detFix.valid) {
      std::cerr << Colors::BRIGHT_GREEN << "[Deterministic Fix] ✓ '"
                << hints.DeterministicOldName << "' -> '"
                << hints.DeterministicNewName
                << "' validated -- the model was never called."
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
    bool isValid = verifySuggestion(suggestion, errorLine, &diagnosticText);

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
        ? "The previous suggested replacement_line failed to compile."
        : diagnosticText;
    std::cerr << Colors::BRIGHT_YELLOW << "[Sandbox] Attempt " << attempt
              << "/" << kMaxSlmAttempts
              << " failed validation, retrying with compiler feedback..."
              << Colors::RESET << "\n";
  }

  return suggestion; // unreachable -- loop always returns on its last iteration
}

} // namespace SLM
