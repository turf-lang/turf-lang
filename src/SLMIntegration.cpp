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

// Scans SourceLines for the definition of a function whose name appears in
// the error message or in the error line itself. Returns the 1-based line
// number of the matching `fn <name>(` header, or -1 if not found.
// This is a pure text scan — no AST required — so it is fully self-contained
// inside SLMIntegration.cpp with no symbol-table dependency.
static int findFunctionSignatureLine(const std::string &errorMsg,
                                     int errorLine) {
  if (SourceLines.empty())
    return -1;

  // Collect candidate function names from the error message.
  // Common patterns:
  //   "Unknown function: 'foo'"        -> extract "foo"
  //   "void value from 'foo'"          -> extract "foo"
  //   "Wrong argument count for 'foo'" -> extract "foo"
  std::vector<std::string> candidates;

  auto extractQuoted = [&](const std::string &src) {
    size_t p = src.find('\'');
    while (p != std::string::npos) {
      size_t q = src.find('\'', p + 1);
      if (q != std::string::npos) {
        candidates.push_back(src.substr(p + 1, q - p - 1));
        p = src.find('\'', q + 1);
      } else {
        break;
      }
    }
  };

  extractQuoted(errorMsg);

  // Also scan the error line itself for identifiers followed by '('
  if (errorLine >= 1 && errorLine <= static_cast<int>(SourceLines.size())) {
    const std::string &src = SourceLines[errorLine - 1];
    for (size_t i = 0; i < src.size();) {
      if (std::isalpha(static_cast<unsigned char>(src[i]))) {
        size_t j = i;
        while (
            j < src.size() &&
            (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_'))
          ++j;
        // Check whether the identifier is immediately followed by '('
        size_t k = j;
        while (k < src.size() && src[k] == ' ')
          ++k;
        if (k < src.size() && src[k] == '(')
          candidates.push_back(src.substr(i, j - i));
        i = j;
      } else {
        ++i;
      }
    }
  }

  if (candidates.empty())
    return -1;

  // Scan every source line for `fn <candidate>(`
  for (int ln = 1; ln <= static_cast<int>(SourceLines.size()); ++ln) {
    const std::string &line = SourceLines[ln - 1];
    size_t fnPos = line.find("fn ");
    if (fnPos == std::string::npos)
      continue;

    for (const auto &name : candidates) {
      if (name.empty())
        continue;
      size_t namePos = line.find(name, fnPos + 3);
      if (namePos == std::string::npos)
        continue;
      // Confirm the identifier is followed (after optional whitespace) by '('
      size_t after = namePos + name.size();
      while (after < line.size() && line[after] == ' ')
        ++after;
      if (after < line.size() && line[after] == '(') {
        if (std::getenv("TURF_SLM_DEBUG")) {
          std::cerr << "[SLM 3b] Found fn '" << name << "' at line " << ln
                    << " (pinning to context)\n";
        }
        return ln;
      }
    }
  }

  return -1;
}

// Scans SourceLines for a variable declaration matching the variable name
// extracted from an UNDECLARED_VAR error message. Returns the 1-based line
// number of the nearest prior declaration in the same function scope, or -1.
// Uses Turf-Lang syntax: `mut name : type` or `immut name : type`.
// Stops the backward scan at the nearest `fn ` line to avoid crossing into
// an unrelated function's scope.
static int findVariableDeclarationLine(const std::string &errorMsg,
                                       int errorLine) {
  if (SourceLines.empty())
    return -1;

  // Extract the variable name from single-quoted text in the error message.
  std::string varName;
  size_t q1 = errorMsg.find('\'');
  if (q1 != std::string::npos) {
    size_t q2 = errorMsg.find('\'', q1 + 1);
    if (q2 != std::string::npos && q2 > q1 + 1)
      varName = errorMsg.substr(q1 + 1, q2 - q1 - 1);
  }

  if (varName.empty())
    return -1;

  // Scan backwards from just before the error line.
  // Stop at the nearest enclosing `fn ` line to avoid crossing scope.
  int lastLine = std::min(errorLine - 1, static_cast<int>(SourceLines.size()));
  for (int ln = lastLine; ln >= 1; --ln) {
    const std::string &line = SourceLines[ln - 1];

    // Function boundary: stop scanning if we've crossed into another fn scope.
    if (line.find("fn ") != std::string::npos)
      break;

    // Turf-Lang declaration syntax: `mut <varName> :` or `immut <varName> :`
    for (const char *prefix : {"mut ", "immut "}) {
      size_t pfxLen = std::strlen(prefix);
      size_t pos = line.find(prefix);
      if (pos == std::string::npos)
        continue;
      size_t nameStart = pos + pfxLen;
      // Skip extra whitespace after keyword
      while (nameStart < line.size() && line[nameStart] == ' ')
        ++nameStart;
      // Check varName match
      if (nameStart + varName.size() <= line.size() &&
          line.compare(nameStart, varName.size(), varName) == 0) {
        size_t afterName = nameStart + varName.size();
        // Whole-word check: must be followed by whitespace or ':'
        if (afterName >= line.size() ||
            line[afterName] == ' ' || line[afterName] == ':') {
          if (std::getenv("TURF_SLM_DEBUG")) {
            std::cerr << "[SLM 3a] Found declaration of '" << varName
                      << "' at line " << ln << " (pinning to context)\n";
          }
          return ln;
        }
      }
    }
  }

  return -1;
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


static ErrorCategory classifyErrorWithSLM(const std::string &errorMessage,
                                          const std::string &codeContext) {
  // Load the category reference file so the SLM knows what each category means.
  std::string categoryDoc = loadDocFile("docs/slm/error_categories.txt");

  std::string categoryList =
      "TYPE_MISMATCH, UNDECLARED_VARIABLE, CONTROL_FLOW, SYNTAX_ERROR, "
      "ARITHMETIC, "
      "STRING_CONVERSION, ARRAY_ERROR, DECLARATION, FUNCTION_SIGNATURE, "
      "BUILTIN_FUNCTION, GENERAL";

  std::string systemContent =
      "You are an expert compiler error classifier for Turf-Lang.\n"
      "Categorize the following error into EXACTLY ONE of these categories:\n" +
      categoryList + "\n\n";

  if (!categoryDoc.empty()) {
    systemContent += "Category descriptions:\n" + categoryDoc + "\n\n";
  }

  systemContent += "Respond with ONLY the exact category name. No explanation, "
                   "no quotes, no markdown.\n";

  std::string prompt = "<|im_start|>system\n" + systemContent +
                       "<|im_end|>\n"
                       "<|im_start|>user\n"
                       "Error: " +
                       errorMessage + "\nCode Context:\n" + codeContext +
                       "\n<|im_end|>\n"
                       "<|im_start|>assistant\n";

  std::string response = SLMEngine::getInstance().generateSuggestion(prompt);

  size_t start = response.find_first_not_of(" \t\r\n\"'");
  size_t end = response.find_last_not_of(" \t\r\n\"'");
  if (start != std::string::npos && end != std::string::npos) {
    std::string categoryStr = response.substr(start, end - start + 1);
    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "[SLM Classifier] AI classification: " << categoryStr << "\n";
    }
    return categoryFromString(categoryStr);
  }

  return ErrorCategory::GENERAL;
}

// Deterministic confidence scoring.
// Rather than asking the LLM to guess a probability (which small models anchor
// to example values in the prompt), we compute a score from measurable signals:
//
//   +0.4  Error category resolved by hardcoded rules (no SLM fallback needed)
//   +0.2  fix_mode == MODIFY  (the model believes a patch is applicable)
//   +0.2  code_patch is non-empty
//   +0.1  code_patch is a single line (no embedded newlines or bare braces)
//   +0.1  code_patch contains the error keyword — patch is on-topic
//
// Maximum score: 1.0  |  Minimum: 0.0
static float computeConfidence(const AISuggestion &s, ErrorCategory category,
                               bool deterministic,
                               const std::string &contextKeyword) {
  float score = 0.0f;

  // +0.4 if the category was resolved by deterministic rules (not SLM fallback)
  if (deterministic && category != ErrorCategory::GENERAL)
    score += 0.4f;

  // +0.2 if the model decided a concrete patch is applicable
  if (s.fix_mode == "MODIFY")
    score += 0.2f;

  // +0.2 if a non-empty replacement_line was actually produced
  // (replacement_line is the exact code; code_patch is just a prose
  // description)
  const std::string &patchCode =
      s.replacement_line.empty() ? s.code_patch : s.replacement_line;
  if (!patchCode.empty())
    score += 0.2f;

  // +0.1 if the replacement is a single, clean line (no embedded newlines or
  // lone braces)
  if (!patchCode.empty()) {
    bool hasNewline = patchCode.find('\n') != std::string::npos;
    bool isBareBrace = (patchCode == "{" || patchCode == "}");
    if (!hasNewline && !isBareBrace)
      score += 0.1f;
  }

  // +0.1 if the replacement line mentions the relevant keyword (patch is
  // on-topic)
  if (!contextKeyword.empty() && !patchCode.empty() &&
      patchCode.find(contextKeyword) != std::string::npos)
    score += 0.1f;

  // Clamp to [0, 1]
  if (score > 1.0f)
    score = 1.0f;
  return score;
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
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine, const std::string &hint) {
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

  // Step 2: SLM fallback classification (if deterministic rules failed) ─
  // Only invoke the model when the pattern-matcher returned GENERAL.
  // We pass a small preliminary context (±3) just for the classifier;
  // the real context for the suggestion prompt is built after this.
  if (!categoryWasDeterministic) {
    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "[SLM Classifier] Hardcoded rules yielded GENERAL. Consulting "
                   "SLM...\n";
    }
    std::string classifierContext = getCodeContext(errorLine, 3);
    category = classifyErrorWithSLM(cleanError, classifierContext);
  }

  // Step 3: dynamic context window
  // Now that we know the category, size the context window appropriately.
  int contextSize = contextSizeForCategory(category);
  if (std::getenv("TURF_SLM_DEBUG")) {
    std::cerr << "[SLM] Context window: ±" << contextSize
              << " lines (category: " << getCategoryName(category) << ")\n";
  }

  std::string codeContext;

  // Smart context: pin relevant definitions outside the window
  //
  // 3b: For FUNCTION_SIGNATURE errors, locate the `fn <name>(` definition
  //     in SourceLines and pin it to the context.
  // 3a: For UNDECLARED_VAR errors, locate the nearest prior declaration
  //     of the undeclared variable (e.g. `int x = ...` in an outer scope
  //     that has since been exited) and pin it.
  //
  // getCodeContextWithPinnedLines automatically skips pinned lines that
  // already fall inside the ±contextSize window, so there is no duplication
  // risk.  If no relevant line is found, we fall through to plain context.
  if (category == ErrorCategory::FUNCTION_SIGNATURE) {
    int sigLine = findFunctionSignatureLine(cleanError, errorLine);
    if (sigLine > 0) {
      codeContext =
          getCodeContextWithPinnedLines(errorLine, contextSize, {sigLine});
    } else {
      codeContext = getCodeContext(errorLine, contextSize);
    }
  } else if (category == ErrorCategory::UNDECLARED_VAR) {
    int declLine = findVariableDeclarationLine(cleanError, errorLine);
    if (declLine > 0) {
      codeContext =
          getCodeContextWithPinnedLines(errorLine, contextSize, {declLine});
    } else {
      codeContext = getCodeContext(errorLine, contextSize);
    }
  } else if (category == ErrorCategory::TYPE_MISMATCH ||
             category == ErrorCategory::ARRAY_ERROR) {
    // For type mismatches and array errors, try to pin the declaration of the
    // variable involved — the model needs to see where the type was declared
    // to understand the conflict, which may be far outside the ±window.
    int declLine = findVariableDeclarationLine(cleanError, errorLine);
    if (declLine > 0) {
      codeContext =
          getCodeContextWithPinnedLines(errorLine, contextSize, {declLine});
    } else {
      codeContext = getCodeContext(errorLine, contextSize);
    }
  } else {
    codeContext = getCodeContext(errorLine, contextSize);
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
  std::string prompt =
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
      "<|im_end|>\n"
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
      "\"warning\": <string>}\n"
      "<|im_end|>\n"
      "<|im_start|>assistant\n";

  std::string rawResponse = SLMEngine::getInstance().generateSuggestion(prompt);

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

    // Compute confidence deterministically from measurable output signals.
    // The LLM is no longer asked to self-report a probability.
    suggestion.confidence = computeConfidence(
        suggestion, category, categoryWasDeterministic, contextKeyword);

    if (std::getenv("TURF_SLM_DEBUG")) {
      std::cerr << "Computed Confidence (deterministic): "
                << suggestion.confidence << "\n";
      std::cerr << "  category_deterministic=" << categoryWasDeterministic
                << " fix_mode=" << suggestion.fix_mode
                << " patch_empty=" << suggestion.code_patch.empty() << "\n";
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
                                        int errorLine, const std::string &hint) {
  // First, get the SLM suggestion
  AISuggestion suggestion = fetchSuggestion(errorMessage, errorLine, hint);

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

  if (isValid) {
    suggestion.valid = true;
    std::cerr << Colors::BRIGHT_GREEN << "[Sandbox] ✓ Suggestion validated"
              << Colors::RESET << "\n";
  } else {
    suggestion.valid = false;
    suggestion.fix_mode = "NONE"; // Downgrade to no fix on validation failure
    std::cerr << Colors::BRIGHT_YELLOW
              << "[Sandbox] ✗ Suggestion validation failed" << Colors::RESET
              << "\n";
  }

  return suggestion;
}

} // namespace SLM
