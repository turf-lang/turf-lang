#include "SLMIntegration.h"
#include "Lexer.h"
#include <SLMEngine.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <regex>

static std::string stripAnsiCodes(const std::string &input) {
  static const std::regex ansiPattern("\033\\[[0-9;]*m");
  return std::regex_replace(input, ansiPattern, "");
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

  std::string prompt =
      "<|im_start|>system\n"

      "You are a VERY conservative expert compiler assistant for Turf, a "
      "statically-typed C-like language.\n"
      "\n"
      "TURF LANGUAGE REFERENCE:\n"
      "- Types: int, double, bool, string\n"

      "- Declarations: int x = 5 | double pi = 3.14 | bool flag = true | "
      "string s = \"hi\"\n"
      "- Type casting: int(\"42\") | double(10) | string(x)\n"
      "- Type rules: Implicit promotion bool->int->double in expressions only. "
      "No implicit cast in assignments.\n"
      "- Operators: + - * / % ^ | += -= *= /= %= | ++ -- | && || | < > == != "
      "<= >=\n"
      "- Control flow: if/else | while { } | break | continue\n"
      "- If expression: int max = if a > b then a else b\n"
      "- Functions: fn name(type param) -> type { return val }\n"
      "- Arrays: int arr[5] = {1,2,3,4,5}\n"

      "- Builtins: print(), printline(), input(), typeof(), lengthof(), "
      "string(), int(), double()\n"
      "- Strings: support \\n \\t \\\\ \\\" escape sequences\n"
      "\n"
      "STRICT ADHERENCE RULES:\n"
      "1. Follow Turf-Lang ownership/borrowing and type rules exactly.\n"
      "2. If code_patch is provided, it must contain NO comments.\n"
      "3. Respond ONLY in the following valid JSON format without markdown "
      "code blocks:\n"
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
      "Analyze this compilation error and provide a fix.\n\n"
      "Error: " +
      cleanError +
      "\n"
      "Code:\n" +
      codeContext +
      "\n\n"
      "What is the fix? Return ONLY valid JSON.\n"
      "<|im_end|>\n"

      "<|im_start|>assistant\n";

  std::string rawResponse = SLMEngine::getInstance().generateSuggestion(prompt);

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
