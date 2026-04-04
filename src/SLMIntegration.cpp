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
  static const std::regex ansiPattern("\033\\[[0-9;]*m");
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

AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine) {
  AISuggestion suggestion;
  std::string codeContext = getCodeContext(errorLine, 10);
  std::string cleanError = stripAnsiCodes(errorMessage);

  const std::string modelPath =
      "models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf";
  if (!SLMEngine::getInstance().initialize(modelPath)) {
    return suggestion;
  }

  // 1. Classify the error deterministically
  ErrorCategory category = classifyError(cleanError);
  std::string categoryName = getCategoryName(category);
  std::string docPath = getCategoryDocPath(category);

  std::cerr << "[SLM] Error category: " << categoryName
            << " | Doc: " << docPath << "\n";

  // 2. Load the targeted documentation for this error category
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

  // 3. Build a focused prompt with only the relevant documentation
  std::string prompt =
      "<|im_start|>system\n"
      "You are a conservative compiler assistant for Turf-Lang.\n"
      "Error category: " +
      categoryName +
      "\n\n" + docContent +
      "\n"
      "RULES:\n"
      "1. Follow Turf-Lang type and ownership rules exactly.\n"
      "2. If code_patch is provided, it must contain NO comments.\n"
      "3. Respond ONLY in valid JSON without markdown code blocks:\n"
      "{\n"
      "  \"confidence\": 0.9,\n"
      "  \"explanation\": \"One sentence root cause and fix\",\n"
      "  \"fix_mode\": \"MODIFY\",\n"
      "  \"suggested_comment\": \"Teaching text\",\n"
      "  \"code_patch\": \"Exact code replacement\",\n"
      "  \"warning\": \"Any caveats\"\n"
      "}\n"
      "<|im_end|>\n"
      "<|im_start|>user\n"
      "Error: " +
      cleanError +
      "\n"
      "Code:\n" +
      codeContext +
      "\n"
      "Return ONLY valid JSON.\n"
      "<|im_end|>\n"
      "<|im_start|>assistant\n";

  std::string rawResponse =
      SLMEngine::getInstance().generateSuggestion(prompt);

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

} // namespace SLM
