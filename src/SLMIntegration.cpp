#include "SLMIntegration.h"
#include "Lexer.h" // Required to access the global SourceLines vector
#include <SLMEngine.h>
#include <algorithm>
#include <regex>
#include <cstdlib>

// Strip ANSI escape codes (e.g. \033[91m) so the SLM sees clean text
static std::string stripAnsiCodes(const std::string &input) {
  // Matches ESC[ followed by any number of params and a final letter
  static const std::regex ansiPattern("\033\\[[0-9;]*m");
  return std::regex_replace(input, ansiPattern, "");
}

namespace SLM {

std::string getCodeContext(int errorLine, int contextSize) {
  // Guard against uninitialized state or empty files
  if (SourceLines.empty()) {
    return "";
  }

  // errorLine is 1-indexed, SourceLines is 0-indexed.
  // Ensure we don't read out of bounds.
  int startLine = std::max(1, errorLine - contextSize);
  int endLine =
      std::min(static_cast<int>(SourceLines.size()), errorLine + contextSize);

  std::string context;
  context.reserve(512); // Pre-allocate to avoid multiple reallocations

  context += "Code Context\n";
  for (int i = startLine; i <= endLine; ++i) {

    // Distinctly mark the exact error line for the SLM's attention
    std::string prefix = (i == errorLine) ? ">> " : "   ";
    context += prefix + std::to_string(i) + " | " + SourceLines[i - 1] + "\n";
  }
  context += "\n";

  return context;
}

// Helper to extract string field from simple flat JSON
static std::string extractJsonFieldString(const std::string& json, const std::string& field) {
    std::string key = "\"" + field + "\":";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += key.length();
    
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
    }
    if (pos >= json.length() || json[pos] != '"') return "";
    
    pos++; // Skip opening quote
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
            if (nextChar == 'n') { result.replace(escapePos, 2, "\n"); }
            else if (nextChar == 't') { result.replace(escapePos, 2, "\t"); }
            else if (nextChar == '"') { result.replace(escapePos, 2, "\""); }
            else if (nextChar == '\\') { result.replace(escapePos, 2, "\\"); }
            else { escapePos++; }
        } else { break; }
    }
    return result;
}

// Helper to extract float field from simple flat JSON
static float extractJsonFieldNumber(const std::string& json, const std::string& field) {
    std::string key = "\"" + field + "\":";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return 0.0f;
    pos += key.length();
    
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
    }
    if (pos >= json.length()) return 0.0f;
    
    size_t endPos = pos;
    while (endPos < json.length() && (std::isdigit(json[endPos]) || json[endPos] == '.' || json[endPos] == '-')) {
        endPos++;
    }
    
    std::string numStr = json.substr(pos, endPos - pos);
    char* endptr;
    float val = std::strtof(numStr.c_str(), &endptr);
    if (endptr == numStr.c_str()) {
        return 0.0f;
    }
    return val;
}

AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine) {
  AISuggestion suggestion;
  std::string codeContext = getCodeContext(errorLine, 10);

  // Strip ANSI color codes so the SLM gets clean, readable text
  std::string cleanError = stripAnsiCodes(errorMessage);

  // 1. Lazy Load the model (Only happens if an error actually occurred)
  // TODO: Later, pass the path via a compiler flag or env var (e.g.,
  // TURF_SLM_PATH)
  const std::string modelPath = "models/Phi-3-mini-4k-instruct-Q4_K_M.gguf";
  if (!SLMEngine::getInstance().initialize(modelPath)) {
    return suggestion; // Fallback gracefully if model missing
  }

  // 2. Format specifically for Phi-3-Mini Instruct
  std::string prompt =
    "<|system|>\n"
    "You are a VERY conservative expert compiler assistant for Turf, a statically-typed C-like language.\n"
    "\n"
    "TURF LANGUAGE REFERENCE:\n"
    "- Types: int, double, bool, string\n"
    "- Declarations: int x = 5 | double pi = 3.14 | bool flag = true | string s = \"hi\"\n"
    "- Type casting: int(\"42\") | double(10) | string(x)\n"
    "- Type rules: Implicit promotion bool->int->double in expressions only. No implicit cast in assignments.\n"
    "- Operators: + - * / % ^ | += -= *= /= %= | ++ -- | && || | < > == != <= >=\n"
    "- Control flow: if/else | while { } | break | continue\n"
    "- If expression: int max = if a > b then a else b\n"
    "- Functions: fn name(type param) -> type { return val }\n"
    "- Arrays: int arr[5] = {1,2,3,4,5}\n"
    "- Builtins: print(), printline(), input(), typeof(), lengthof(), string(), int(), double()\n"
    "- Strings: support \\n \\t \\\\ \\\" escape sequences\n"
    "\n"
    "STRICT ADHERENCE RULES:\n"
    "1. Follow Turf-Lang ownership/borrowing and type rules exactly.\n"
    "2. If code_patch is provided, it must contain NO comments.\n"
    "3. If unsure, set confidence < 0.6 and say \"I'm not confident — check the docs.\"\n"
    "4. Respond ONLY in the following JSON format:\n"
    "\n"
    "{\n"
    "  \"confidence\": 0.0 to 1.0,   // <0.6 means uncertain\n"
    "  \"explanation\": \"One sentence root cause and fix\",\n"
    "  \"fix_mode\": \"LEARNING\" | \"MODIFY\" | \"NONE\",\n"
    "  \"suggested_comment\": \"Teaching text (will be displayed separately from code)\",\n"
    "  \"code_patch\": \"Only if MODIFY and confidence >= 0.85: exact code replacement without comments\",\n"
    "  \"warning\": \"Any caveats or 'this might not be perfect'\"\n"
    "}\n"
    "\n"
    "COMMON MISTAKES TO IDENTIFY:\n"
    "- Assigning double to int without explicit cast: use int(x)\n"
    "- Using undeclared variables\n"
    "- Function return type mismatches\n"
    "- Mixing string concatenation with non-string types without string()\n"
    "- Using C-style for loops (Turf only has while)\n"
    "<|end|>\n"
    "<|user|>\n"
    "Analyze this compilation error and provide a fix.\n\n"
    "Error: " + cleanError + "\n"
    "Code:\n" + codeContext + "\n\n"
    "What is the fix?\n"
    "<|end|>\n"
    "<|assistant|>\n";

  // 3. Run Inference
  std::string rawResponse = SLMEngine::getInstance().generateSuggestion(prompt);
  
  if (rawResponse.empty()) {
      return suggestion;
  }

  // Extract JSON content if bounded by { }
  size_t startJson = rawResponse.find('{');
  size_t endJson = rawResponse.rfind('}');
  if (startJson != std::string::npos && endJson != std::string::npos && endJson > startJson) {
      std::string jsonStr = rawResponse.substr(startJson, endJson - startJson + 1);
      suggestion.confidence = extractJsonFieldNumber(jsonStr, "confidence");
      suggestion.explanation = extractJsonFieldString(jsonStr, "explanation");
      suggestion.fix_mode = extractJsonFieldString(jsonStr, "fix_mode");
      suggestion.suggested_comment = extractJsonFieldString(jsonStr, "suggested_comment");
      suggestion.code_patch = extractJsonFieldString(jsonStr, "code_patch");
      suggestion.warning = extractJsonFieldString(jsonStr, "warning");
      
      if (suggestion.fix_mode.empty()) {
          suggestion.fix_mode = "NONE";
      }
      suggestion.valid = true;
  }

  return suggestion;
}

} // namespace SLM
