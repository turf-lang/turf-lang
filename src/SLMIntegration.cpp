#include "SLMIntegration.h"
#include "ErrorCategory.h"
#include "Lexer.h"
#include <SLMEngine.h>
#include <algorithm>
#include <cstdlib>
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
        result.replace(escapePos, 2, "\n");
      } else if (nextChar == 't') {
        result.replace(escapePos, 2, "\t");
      } else if (nextChar == '"') {
        result.replace(escapePos, 2, "\"");
      } else if (nextChar == '\\') {
        result.replace(escapePos, 2, "\\");
      } else {
        escapePos++;
      }
    } else {
      break;
    }
  }
  return result;
}

static float extractJsonFieldNumber(const std::string &json,
                                    const std::string &field) {
  std::string key = "\"" + field + "\":";
  size_t pos = json.find(key);
  if (pos == std::string::npos)
    return 0.0f;

  pos += key.length();

  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
    pos++;
  }
  if (pos >= json.length())
    return 0.0f;

  size_t endPos = pos;

  while (endPos < json.length() &&
         (std::isdigit(json[endPos]) || json[endPos] == '.' ||
          json[endPos] == '-')) {
    endPos++;
  }

  std::string numStr = json.substr(pos, endPos - pos);
  char *endptr;

  float val = std::strtof(numStr.c_str(), &endptr);
  if (endptr == numStr.c_str()) {
    return 0.0f;
  }
  return val;
}

static ErrorCategory classifyErrorWithSLM(const std::string &errorMessage, const std::string &codeContext) {
  // Load the category reference file so the SLM knows what each category means.
  std::string categoryDoc = loadDocFile("docs/slm/error_categories.txt");

  std::string categoryList =
      "TYPE_MISMATCH, UNDECLARED_VARIABLE, CONTROL_FLOW, SYNTAX_ERROR, ARITHMETIC, "
      "STRING_CONVERSION, ARRAY_ERROR, DECLARATION, FUNCTION_SIGNATURE, "
      "BUILTIN_FUNCTION, GENERAL";

  std::string systemContent =
      "You are an expert compiler error classifier for Turf-Lang.\n"
      "Categorize the following error into EXACTLY ONE of these categories:\n" +
      categoryList + "\n\n";

  if (!categoryDoc.empty()) {
    systemContent += "Category descriptions:\n" + categoryDoc + "\n\n";
  }

  systemContent += "Respond with ONLY the exact category name. No explanation, no quotes, no markdown.\n";

  std::string prompt =
      "<|im_start|>system\n" + systemContent +
      "<|im_end|>\n"
      "<|im_start|>user\n"
      "Error: " + errorMessage +
      "\nCode Context:\n" + codeContext +
      "\n<|im_end|>\n"
      "<|im_start|>assistant\n";

  std::string response = SLMEngine::getInstance().generateSuggestion(prompt);

  size_t start = response.find_first_not_of(" \t\r\n\"'");
  size_t end = response.find_last_not_of(" \t\r\n\"'");
  if (start != std::string::npos && end != std::string::npos) {
    std::string categoryStr = response.substr(start, end - start + 1);
    std::cerr << "[SLM Classifier] AI classification: " << categoryStr << "\n";
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
static float computeConfidence(const AISuggestion &s,
                               ErrorCategory category,
                               bool deterministic,
                               const std::string &contextKeyword) {
  float score = 0.0f;

  // +0.4 if the category was resolved by deterministic rules (not SLM fallback)
  if (deterministic && category != ErrorCategory::GENERAL)
    score += 0.4f;

  // +0.2 if the model decided a concrete patch is applicable
  if (s.fix_mode == "MODIFY")
    score += 0.2f;

  // +0.2 if a non-empty patch was actually produced
  if (!s.code_patch.empty())
    score += 0.2f;

  // +0.1 if the patch is a single, clean line (no embedded newlines or lone braces)
  if (!s.code_patch.empty()) {
    bool hasNewline    = s.code_patch.find('\n') != std::string::npos;
    bool isBareBrace   = (s.code_patch == "{" || s.code_patch == "}");
    if (!hasNewline && !isBareBrace)
      score += 0.1f;
  }

  // +0.1 if the patch mentions the relevant keyword (patch is on-topic)
  if (!contextKeyword.empty() && !s.code_patch.empty() &&
      s.code_patch.find(contextKeyword) != std::string::npos)
    score += 0.1f;

  // Clamp to [0, 1]
  if (score > 1.0f) score = 1.0f;
  return score;
}

AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine) {
  AISuggestion suggestion;
  std::string codeContext = getCodeContext(errorLine, 10);
  std::string cleanError = stripAnsiCodes(errorMessage);

  const std::string modelPath =
      "models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf";
  if (!SLMEngine::getInstance().initialize(modelPath)) {
    return suggestion;
  }

  // 1. Extract a keyword/construct hint from the error message for
  //    precise classification and targeted doc loading.
  std::string contextKeyword = extractContextKeyword(cleanError, codeContext);

  // 2. Classify the error deterministically (keyword-aware).
  //    Track whether the category was resolved without SLM — used later in
  //    the deterministic confidence formula.
  ErrorCategory category = classifyError(cleanError, contextKeyword);
  bool categoryWasDeterministic = (category != ErrorCategory::GENERAL);
  if (!categoryWasDeterministic) {
    std::cerr << "[SLM Classifier] Hardcoded rules yielded GENERAL. Consulting SLM...\n";
    category = classifyErrorWithSLM(cleanError, codeContext);
  }
  std::string categoryName = getCategoryName(category);

  // 3. Get the most specific documentation available
  std::string docPath = getCategoryDocPath(category, contextKeyword);

  std::cerr << "[SLM] Error category: " << categoryName << " | Keyword: "
            << (contextKeyword.empty() ? "(none)" : contextKeyword)
            << " | Doc: " << docPath << "\n";

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
      "   Use NONE when the issue requires multi-line restructuring or you are\n"
      "   not confident enough to suggest a specific change.\n"
      "5. Respond ONLY in valid JSON without markdown code blocks.\n"
      "6. Keep explanation, suggested_comment, and warning to 1-2 sentences max.\n"
      "<|im_end|>\n"
      "<|im_start|>user\n"
      "Error: " +
      cleanError +
      "\n"
      "Code:\n" +
      codeContext +
      "\n"
      "Respond with ONLY this JSON object — fill every field yourself:\n"
      "{\"explanation\": <string>, \"fix_mode\": <MODIFY|NONE>, "
      "\"suggested_comment\": <string>, \"code_patch\": <string>, \"warning\": <string>}\n"
      "<|im_end|>\n"
      "<|im_start|>assistant\n";

  std::string rawResponse = SLMEngine::getInstance().generateSuggestion(prompt);

  std::cerr << "Clean Error: " << cleanError << "\n";

  std::cerr << "\n--- SLM RAW RESPONSE DEBUG ---\n"
            << rawResponse << "\n------------------------------\n";

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
    suggestion.warning = extractJsonFieldString(jsonStr, "warning");

    // Compute confidence deterministically from measurable output signals.
    // The LLM is no longer asked to self-report a probability.
    suggestion.confidence = computeConfidence(
        suggestion, category, categoryWasDeterministic, contextKeyword);

    std::cerr << "Computed Confidence (deterministic): " << suggestion.confidence << "\n";
    std::cerr << "  category_deterministic=" << categoryWasDeterministic
              << " fix_mode=" << suggestion.fix_mode
              << " patch_empty=" << suggestion.code_patch.empty() << "\n";
    std::cerr << "Parsed Fix Mode: " << suggestion.fix_mode << "\n";

    if (suggestion.fix_mode.empty()) {
      suggestion.fix_mode = "NONE";
    }
    suggestion.valid = true;
  }

  return suggestion;
}

// ────────────────────────────────────────────────────────────────────────────
// TODO(future): Self-running verification of AI-suggested code patches.
//
// The idea is to verify that an AI-suggested fix actually compiles by
// running the patched source through a lightweight compilation pass before
// presenting it to the user. This eliminates false-positive suggestions.
//
// Implementation steps (not yet done):
// 1. Take the original SourceLines and the AI's code_patch.
// 2. Create a modified copy of the source with the patch applied at
//    the error line.
// 3. Invoke a "verify-only" compilation of the modified source (parse +
//    type-check, no codegen/linking needed).
// 4. If the verification pass produces zero errors, the suggestion is
//    valid and can be shown to the user.
// 5. If the verification fails, discard the AI suggestion silently and
//    fall back to the default compiler error message.
//
// Challenges:
// - The compiler currently reads from a file; need a way to feed a
//   string-based source (or a temporary file) to the pipeline.
// - Must avoid infinite recursion: the verification pass should NOT
//   itself invoke the SLM.
// - Performance: verification should be fast (skip codegen/linking).
// ────────────────────────────────────────────────────────────────────────────
bool verifySuggestion(const AISuggestion &suggestion,
                      const std::string &originalSource, int errorLine) {
  // Stub: always returns true.
  // When implemented, this will run the patched code through a verify-only
  // compilation pass and return false if any errors are produced.
  (void)suggestion;
  (void)originalSource;
  (void)errorLine;
  return true;
}

} // namespace SLM
