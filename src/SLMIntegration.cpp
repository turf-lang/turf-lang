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
#include "Lexer.h"
#include "Sandbox.h"
#include <SLMEngine.h>
#include "AST.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

static std::string stripAnsiCodes(const std::string &input) {
  static const std::regex ansiPattern("\\033\\[[0-9;]*m");
  return std::regex_replace(input, ansiPattern, "");
}

// Reads an external documentation file into a string.
// Returns empty string on failure (file missing, unreadable, etc.).
static std::string loadDocFile(const std::string &path) {
  std::ifstream file(path);
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
static std::string
getCodeContextWithPinnedLines(int errorLine, int contextSize,
                              const std::vector<int> &pinnedLines) {
  std::string context = getCodeContext(errorLine, contextSize);

  if (pinnedLines.empty() || SourceLines.empty())
    return context;

  // Compute the window boundaries to avoid pinning lines already visible.
  int windowStart = std::max(1, errorLine - contextSize);
  int windowEnd =
      std::min(static_cast<int>(SourceLines.size()), errorLine + contextSize);

  bool headerAdded = false;
  for (int lineNo : pinnedLines) {
    if (lineNo < 1 || lineNo > static_cast<int>(SourceLines.size()))
      continue;
    // Skip lines already inside the normal context window.
    if (lineNo >= windowStart && lineNo <= windowEnd)
      continue;
    if (!headerAdded) {
      context += "Relevant definition(s)\n";
      headerAdded = true;
    }
    context +=
        "*  " + std::to_string(lineNo) + " | " + SourceLines[lineNo - 1] + "\n";
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
    // Hard-reject: the model echoed back the original broken line unchanged.
    // This is a known small-model failure mode. An identical replacement is
    // actively harmful — it passes every other check while fixing nothing.
    std::string originalLine =
        (errorLine >= 1 && errorLine <= static_cast<int>(SourceLines.size()))
            ? SourceLines[errorLine - 1]
            : "";
    // Trim leading/trailing whitespace for comparison
    auto trim = [](const std::string &s) {
      size_t l = s.find_first_not_of(" \t\r\n");
      size_t r = s.find_last_not_of(" \t\r\n");
      return (l == std::string::npos) ? std::string{} : s.substr(l, r - l + 1);
    };
    if (!originalLine.empty() && trim(patchCode) == trim(originalLine)) {
      // Hard override — this score can never clear the gate.
      return 0.0f;
    }

    // +0.2 if a non-empty replacement_line was produced
    score += 0.2f;

    // +0.1 if it is a single, clean line (no embedded newlines or lone braces)
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
                             const std::vector<RelatedLocation> &related) {
  AISuggestion suggestion;
  std::string cleanError = stripAnsiCodes(errorMessage);

  // Step 0: extract keyword hint early so classification can use it
  // (classifyError only uses cleanError; codeContext not needed for this step)
  std::string contextKeyword = extractContextKeyword(cleanError);

  // Step 1: classify first (deterministic, no context window required) ─
  // We classify before building the context so we can size the window to fit
  // the category rather than always using the generic ±10 default.
  ErrorCategory category = classifyError(cleanError, contextKeyword);
  bool categoryWasDeterministic = (category != ErrorCategory::GENERAL);

  llvm::SmallString<128> ModelPath(CompilerExecutablePath);
  llvm::sys::path::remove_filename(ModelPath);
  llvm::sys::path::append(ModelPath, "..", "models",
                          "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf");
  if (!SLMEngine::getInstance().initialize(ModelPath.str().str())) {
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

  // Step 3: dynamic context window
  // Size the context window based on the deterministic category.
  // If deterministic classification failed, GENERAL gives ±8 lines (broadest
  // window), which is already the most generous fallback we have.
  int contextSize = contextSizeForCategory(category);
  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "[SLM] Context window: ±" << contextSize
              << " lines (category: " << getCategoryName(category) << ")\n";
  }

  std::string codeContext;

  // Smart context: pin relevant definitions outside the window
  //
  // Pinning is now data-driven: RelatedLocations are populated by the
  // compiler's own Resolver/TypeChecker at the exact point each error fires,
  // carrying structurally correct locations from real symbol-table entries.
  // Zero category branching, zero hardcoded syntax, zero scanner functions.
  // If `related` is empty (error classes that don't yet populate it), this
  // naturally falls back to plain sliding-window context — same as before.
  std::vector<int> pinnedLines;
  for (const auto &rel : related)
    pinnedLines.push_back(rel.Loc.Line);

  codeContext = pinnedLines.empty()
      ? getCodeContext(errorLine, contextSize)
      : getCodeContextWithPinnedLines(errorLine, contextSize, pinnedLines);

  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "--- PINNED CONTEXT ---\n" << codeContext << "\n----------------------\n";
  }

  std::string categoryName = getCategoryName(category);

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
  std::string systemPrompt =
      "<|im_start|>system\n"
      "You are a conservative compiler assistant for Turf-Lang.\n"
      "Error category: " +
      categoryName + "\n\n" + docContent +
      "\n"
      "RULES:\n"
      "1. Follow Turf-Lang type and ownership rules exactly.\n"
      "2. code_patch must be a SINGLE LINE of valid Turf-Lang syntax ONLY.\n"
      "   Do NOT use C, Java, Python, or any other language syntax.\n"
      "   Do NOT include multiple statements, semicolons, or braces unless\n"
      "   they are required by Turf-Lang syntax for that construct.\n"
      "3. If you cannot produce a valid Turf-Lang patch, set fix_mode to NONE\n"
      "   and leave code_patch empty.\n"
      "4. Set fix_mode to one of: MODIFY, NONE.\n"
      "   Use MODIFY when you have a concrete single-line patch.\n"
      "   Use NONE when the issue requires multi-line restructuring or you "
      "are\n"
      "   not confident enough to suggest a specific change.\n"
      "5. Respond ONLY in valid JSON without markdown code blocks.\n"
      "6. Keep explanation, suggested_comment, and warning to 1-2 sentences "
      "max.\n"
      "<|im_end|>\n";

  std::string userPrompt =
      "<|im_start|>user\n"
      "{\n"
      "  \"error\": \"" + escapeJsonString(cleanError) + "\",\n"
      "  \"hint\": \"" + escapeJsonString(hint) + "\",\n"
      "  \"code\": \"" + escapeJsonString(codeContext) + "\"\n"
      "}\n"
      "Respond with ONLY this JSON object — fill every field yourself:\n"
      "{\"explanation\": <string>, \"fix_mode\": <MODIFY|NONE>, "
      "\"suggested_comment\": <string>, \"code_patch\": <human-readable "
      "description of what changed>, "
      "\"replacement_line\": <THE EXACT SINGLE LINE of valid Turf-Lang code "
      "that replaces the erroneous line — no explanation, no prose, just the "
      "raw code>, "
      "\"warning\": <string>, "
      "\"category\": <one of: TYPE_MISMATCH, UNDECLARED_VARIABLE, "
      "CONTROL_FLOW, SYNTAX_ERROR, ARITHMETIC, STRING_CONVERSION, "
      "ARRAY_ERROR, DECLARATION, FUNCTION_SIGNATURE, BUILTIN_FUNCTION, "
      "GENERAL>}\n"
      "<|im_end|>\n"
      "<|im_start|>assistant\n";

  std::string rawResponse =
      SLMEngine::getInstance().generateSuggestion(systemPrompt, userPrompt, grammarStr);

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

    suggestion.explanation = extractJsonFieldString(jsonStr, "explanation");
    suggestion.fix_mode = extractJsonFieldString(jsonStr, "fix_mode");
    suggestion.suggested_comment =
        extractJsonFieldString(jsonStr, "suggested_comment");
    suggestion.code_patch = extractJsonFieldString(jsonStr, "code_patch");
    suggestion.replacement_line =
        extractJsonFieldString(jsonStr, "replacement_line");
    suggestion.warning = extractJsonFieldString(jsonStr, "warning");
    suggestion.self_reported_category =
        extractJsonFieldString(jsonStr, "category");

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
                << " replacement_line_empty=" << suggestion.replacement_line.empty() << "\n";
      std::cerr << "Parsed Fix Mode: " << suggestion.fix_mode << "\n";
    }

    if (suggestion.fix_mode.empty()) {
      suggestion.fix_mode = "NONE";
    }
    suggestion.valid = true;
  }

  return suggestion;
}

// Sandbox-Based Suggestion Validation

bool verifySuggestion(const AISuggestion &suggestion, int errorLine) {
  // Create a sandbox from the current compiler state
  auto sandbox = CreateSandbox();
  if (!sandbox) {
    std::cerr << Colors::BRIGHT_BLACK << "[Verify] Failed to create sandbox"
              << Colors::RESET << "\n";
    return false;
  }

  // Use replacement_line (exact Turf-Lang code) for sandbox validation.
  // code_patch is a human-readable description and must NOT be fed to the
  // parser. If replacement_line is empty, there is no valid code to validate.
  if (suggestion.replacement_line.empty()) {
    return false; // no valid code to validate; code_patch is prose-only
  }
  const std::string &fixLine = suggestion.replacement_line;
  return ApplyAndValidateFix(*sandbox, errorLine, fixLine, suggestion);
}

// High-Level Auto-Fix API

// Wrapper that gets SLM suggestion, validates it in a sandbox, and applies it
// if valid. Returns an enhanced AISuggestion with validation results.
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine, const std::string &hint,
                                        const std::vector<RelatedLocation> &related) {
  // First, get the SLM suggestion
  AISuggestion suggestion = fetchSuggestion(errorMessage, errorLine, hint, related);

  if (!suggestion.valid) {
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
  bool isValid = verifySuggestion(suggestion, errorLine);

  // Finalize confidence NOW — after sandbox validation.
  // The structural prior (computed before sandbox ran) is only a prior;
  // the sandbox result is the strongest possible signal (empirical proof).
  // A proven-correct fix is clamped to >= 0.9; an unvalidated fix is capped
  // at <= 0.5, making it impossible for a pre-validation guess to veto a
  // validated fix, or to promote an unvalidated one past the display gate.
  suggestion.confidence = finalizeConfidence(suggestion.confidence, isValid);

  if (isValid) {
    suggestion.valid = true;
    std::cerr << Colors::BRIGHT_GREEN << "[Sandbox] ✓ Suggestion validated"
              << Colors::RESET << "\n";
    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "Final Confidence (post-sandbox): " << suggestion.confidence << "\n";
    }
  } else {
    suggestion.valid = false;
    suggestion.fix_mode = "NONE"; // Downgrade to no fix on validation failure
    std::cerr << Colors::BRIGHT_YELLOW
              << "[Sandbox] ✗ Suggestion validation failed" << Colors::RESET
              << "\n";
    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "Final Confidence (post-sandbox, capped): " << suggestion.confidence << "\n";
    }
  }

  return suggestion;
}

} // namespace SLM
