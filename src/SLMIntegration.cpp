#include "SLMIntegration.h"
#include "Lexer.h" // Required to access the global SourceLines vector
#include <SLMEngine.h>
#include <algorithm>

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

  // 1. Lazy Load the model (Only happens if an error actually occurred)
  // TODO: Later, pass the path via a compiler flag or env var (e.g.,
  // TURF_SLM_PATH)
  const std::string modelPath = "models/Phi-3-mini-4k-instruct-Q4_K_M.gguf";
  if (!SLMEngine::getInstance().initialize(modelPath)) {
    return ""; // Fallback gracefully if model missing
  }

  // 2. Format specifically for Phi-3-Mini Instruct
  std::string prompt =
      "<|user|>\n"
      "You are an expert compiler assistant for a C-like programming "
      "language.\n"
      "Analyze this compilation error and provide a ONE sentence fix.\n\n"
      "Error: " +
      errorMessage +
      "\n\n"
      "Code Context:\n" +
      codeContext +
      "\n<|end|>\n"
      "<|assistant|>\n";

  // 3. Run Inference
  return SLMEngine::getInstance().generateSuggestion(prompt);
}

} // namespace SLM
