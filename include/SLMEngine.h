#ifndef SLM_ENGINE_H
#define SLM_ENGINE_H

#include <string>

// Forward declarations to keep llama.cpp headers out of our compiler headers
struct llama_model;
struct llama_context;

namespace SLM {

class SLMEngine {
private:
  llama_model *model = nullptr;
  llama_context *ctx = nullptr;
  bool backendInitialized = false;

  // Private constructor for Singleton / Lazy Load
  SLMEngine();

public:
  ~SLMEngine();

  // Disable copy/move
  SLMEngine(const SLMEngine &) = delete;
  SLMEngine &operator=(const SLMEngine &) = delete;

  // Get the global instance
  static SLMEngine &getInstance();

  // Loads the model into RAM. Safe to call multiple times (no-op if loaded).
  bool initialize(const std::string &modelPath);

  // Generates the suggestion based on the prompt.
  std::string generateSuggestion(const std::string &prompt);
  void release();
};

} // namespace SLM

#endif // SLM_ENGINE_H
