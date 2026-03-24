#include "SLMIntegration.h"
#include "Lexer.h" // Required to access the global SourceLines vector
#include <SLMEngine.h>
#include <algorithm>
#include <regex>

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

std::string fetchSuggestion(const std::string &errorMessage, int errorLine) {
  std::string codeContext = getCodeContext(errorLine, 10);

  // Strip ANSI color codes so the SLM gets clean, readable text
  std::string cleanError = stripAnsiCodes(errorMessage);

  // 1. Lazy Load the model (Only happens if an error actually occurred)
  // TODO: Later, pass the path via a compiler flag or env var (e.g.,
  // TURF_SLM_PATH)
  const std::string modelPath = "models/Phi-3-mini-4k-instruct-Q4_K_M.gguf";
  if (!SLMEngine::getInstance().initialize(modelPath)) {
    return ""; // Fallback gracefully if model missing
  }

  // 2. Format specifically for Phi-3-Mini Instruct
  std::string prompt =
      "<|system|>\n"
      "You are a Turf compiler assistant. Turf is a statically-typed language "
      "with this syntax:\n"
      "- Types: int, double, bool, string\n"
      "- Declarations: int x = 5 | double pi = 3.14 | bool flag = true | "
      "string s = \"hi\"\n"
      "- Casting: int(\"42\") | double(10) | string(x)\n"
      "- Operators: + - * / % ^ | += -= *= /= %= | ++ -- | && || | < > == != "
      "<= >=\n"
      "- Control flow: if/else | while { } | break | continue\n"
      "- If expression: int max = if a > b then a else b\n"
      "- Builtins: print() | printline() | input() | typeof() | lengthof() | "
      "string() | int() | double()\n"

      "- Functions: fn name(type param) -> type { ... } | return val\n"
      "- Arrays: int arr[5] = {1,2,3,4,5}\n"
      "- Comments: // ...\n"
      "<|end|>\n"
      "<|user|>\n"
      "Compiler error: " +
      cleanError +
      "\n"
      "Code:\n" +
      codeContext +
      "\n"
      "Give ONE sentence fix.\n"
      "<|end|>\n"
      "<|assistant|>\n";

  // 3. Run Inference
  return SLMEngine::getInstance().generateSuggestion(prompt);
}

} // namespace SLM
