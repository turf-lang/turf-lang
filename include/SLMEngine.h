#ifndef SLM_ENGINE_H
#define SLM_ENGINE_H

#include <string>
#include <vector>

// Forward declarations to keep llama.cpp headers out of our compiler headers
struct llama_model;
struct llama_context;

namespace SLM {

class SLMEngine {
private:
  llama_model *model = nullptr;
  llama_context *ctx = nullptr;
  bool backendInitialized = false;

  std::string CurrentCachedSystemPrompt;
  std::vector<int> CachedSystemTokens; // int to represent llama_token

  // Private constructor for Singleton / Lazy Load
  SLMEngine();

public:
  ~SLMEngine();

  // Disable copy/move
  SLMEngine(const SLMEngine &) = delete;
  SLMEngine &operator=(const SLMEngine &) = delete;

  // Get the global instance
  static SLMEngine &getInstance();

  // True in a turfc built with the model runtime (TURF_SMART=ON), false in one
  // built without it (SLMEngineDisabled.cpp). Asked before looking for a model,
  // so a build that cannot run one never tells the user to go and fetch one.
  static bool builtIn();

  // Loads the model into RAM. Safe to call multiple times (no-op if loaded).
  bool initialize(const std::string &modelPath);

  // Generates the suggestion based on the prompt.
  // The systemPrompt is evaluated and cached. If the systemPrompt matches the
  // previously cached prompt, it reuses the KV cache for those tokens.
  // If grammarStr is non-empty, grammar-constrained sampling is applied.
  std::string generateSuggestion(const std::string &systemPrompt,
                                 const std::string &userPrompt,
                                 const std::string &grammarStr = "");
  void release();
};

} // namespace SLM

#endif // SLM_ENGINE_H
