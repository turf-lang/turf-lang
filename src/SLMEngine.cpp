#include "SLMEngine.h"
#include "llama.h"
#include <cstdio>
#include <iostream>
#include <unistd.h>

namespace SLM {
SLMEngine::SLMEngine() {}
SLMEngine::~SLMEngine() {
  if (ctx)
    llama_free(ctx);
  if (model)
    llama_free_model(model);
  llama_backend_free();
}
SLMEngine &SLMEngine::getInstance() {
  static SLMEngine instance;
  return instance;
}

bool SLMEngine::initialize(const std::string &modelPath) {
  if (model)
    return true;

  // Suppress llama.cpp's verbose loading output
  int saved_stderr = dup(STDERR_FILENO);
  freopen("/dev/null", "w", stderr);

  llama_backend_init();
  llama_model_params model_params = llama_model_default_params();

  model_params.use_mmap = true;
  model = llama_load_model_from_file(modelPath.c_str(), model_params);

  if (!model) {
    // Restore stderr before printing our own error
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    std::cerr << "Compiler Warning: Failed to load SLM model at " << modelPath
              << "\n";
    return false;
  }

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = 2048;

  ctx = llama_new_context_with_model(model, ctx_params);

  // Restore stderr
  dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);

  return ctx != nullptr;
}

std::string SLMEngine::generateSuggestion(const std::string &prompt) {
  if (!ctx)
    return "";

  // Suppress llama.cpp output during inference too
  int saved_stderr = dup(STDERR_FILENO);
  freopen("/dev/null", "w", stderr);

  // Tokenize the prompt
  int n_ctx = llama_n_ctx(ctx);
  std::vector<llama_token> tokens(n_ctx);
  int n_tokens = llama_tokenize(llama_get_model(ctx), prompt.c_str(),
                                prompt.size(), tokens.data(), n_ctx,
                                /*add_special=*/true, /*parse_special=*/false);

  if (n_tokens < 0) {
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    return "";
  }
  tokens.resize(n_tokens);

  llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
  if (llama_decode(ctx, batch) != 0) {
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    return "";
  }

  llama_sampler *sampler =
      llama_sampler_chain_init(llama_sampler_chain_default_params());
  llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.2f));

  llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

  std::string result;
  const llama_vocab *vocab = llama_model_get_vocab(llama_get_model(ctx));
  for (int i = 0; i < 128; i++) {
    llama_token token = llama_sampler_sample(sampler, ctx, -1);
    if (llama_vocab_is_eog(vocab, token))
      break;

    char buf[256];
    int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
    if (n > 0)
      result += std::string(buf, n);

    llama_batch next = llama_batch_get_one(&token, 1);
    if (llama_decode(ctx, next) != 0)
      break;
  }

  llama_sampler_free(sampler);
  llama_kv_cache_clear(ctx);

  dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);

  size_t start = result.find_first_not_of(" \t\n\r");
  size_t end = result.find_last_not_of(" \t\n\r");
  if (start == std::string::npos)
    return "";
  return result.substr(start, end - start + 1);
} // namespace SLM
