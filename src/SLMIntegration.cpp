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

  // 2. Classify the error deterministically (keyword-aware)
  ErrorCategory category = classifyError(cleanError, contextKeyword);
  if (category == ErrorCategory::GENERAL) {
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
  //    KEY FIX: The JSON template uses a concrete example value (0.85) rather
  //    than a placeholder like <float>.  Small models (1.5B) latch onto the
  //    first concrete number they see; an abstract placeholder caused the
  //    model to always output 0.0.  The confidence guidance also lists high
  //    values first to bias the model upward for clear errors.
  std::string prompt =
      "<|im_start|>system\n"
      "You are a conservative compiler assistant for Turf-Lang.\n"
      "Error category: " +
      categoryName + "\n\n" + docContent +
      "\n"
      "RULES:\n"
      "1. Follow Turf-Lang type and ownership rules exactly.\n"
      "2. If code_patch is provided, it must contain NO comments.\n"
      "3. Set confidence as a float from 0.0 to 1.0:\n"
      "   - 1.0 = absolutely certain the fix is correct.\n"
      "   - 0.8 = high confidence, straightforward fix.\n"
      "   - 0.5 = moderate confidence, some ambiguity.\n"
      "   - 0.2 = low confidence, mostly guessing.\n"
      "   For clear, obvious errors give at least 0.8.\n"
      "4. Respond ONLY in valid JSON without markdown code blocks.\n"
      "5. Keep explanation, suggested_comment, and warning EXTREMELY brief (1-2 sentences max).\n"
      "<|im_end|>\n"
      "<|im_start|>user\n"
      "Error: " +
      cleanError +
      "\n"
      "Code:\n" +
      codeContext +
      "\n"
      "Respond with ONLY this JSON (no markdown, no extra text):\n"
      "{\"confidence\": 0.85, \"explanation\": \"...\", \"fix_mode\": "
      "\"MODIFY\", "
      "\"suggested_comment\": \"...\", \"code_patch\": \"...\", \"warning\": "
      "\"\"}\n"
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

    suggestion.confidence = extractJsonFieldNumber(jsonStr, "confidence");
    suggestion.explanation = extractJsonFieldString(jsonStr, "explanation");
    suggestion.fix_mode = extractJsonFieldString(jsonStr, "fix_mode");
    suggestion.suggested_comment =
        extractJsonFieldString(jsonStr, "suggested_comment");
    suggestion.code_patch = extractJsonFieldString(jsonStr, "code_patch");
    suggestion.warning = extractJsonFieldString(jsonStr, "warning");

    std::cerr << "Parsed Confidence: " << suggestion.confidence << "\n";
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
