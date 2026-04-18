#include "SLMEngine.h"
#include "llama.h"
#include <cstdio>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace SLM {
SLMEngine::SLMEngine() {}
SLMEngine::~SLMEngine() {
  if (ctx)
    llama_free(ctx);
  if (model)
    llama_model_free(model);
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
  model = llama_model_load_from_file(modelPath.c_str(), model_params);

  if (!model) {
    // Restore stderr before printing our own error
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    std::cerr << "Compiler Warning: Failed to load SLM model at " << modelPath
              << "\n";
    return false;
  }

  llama_context_params ctx_params = llama_context_default_params();
  // ctx_params.n_ctx = 4096;
  // ctx_params.n_ctx = 8192;
  ctx_params.n_ctx = 10240;
  ctx_params.n_batch = 4096;
  ctx_params.type_k = GGML_TYPE_Q8_0;
  ctx_params.type_v = GGML_TYPE_Q8_0;

  ctx = llama_init_from_model(model, ctx_params);

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
  const llama_vocab *vocab_tok = llama_model_get_vocab(llama_get_model(ctx));
  int n_tokens = llama_tokenize(vocab_tok, prompt.c_str(), prompt.size(),
                                tokens.data(), n_ctx,
                                /*add_special=*/true, /*parse_special=*/true);

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
  for (int i = 0; i < 512; i++) {
    llama_token token = llama_sampler_sample(sampler, ctx, -1);
    if (llama_vocab_is_eog(vocab, token))
      break;

    char buf[256];
    int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
    if (n > 0) {
      result += std::string(buf, n);

      // Check for Qwen2.5 ChatML stop sequences
      const std::string STOP_IM_END = "<|im_end|>";
      const std::string STOP_IM_START = "<|im_start|>";

      size_t pos_im_end = result.find(STOP_IM_END);
      size_t pos_im_start = result.find(STOP_IM_START);

      size_t stop_pos = std::string::npos;
      if (pos_im_end != std::string::npos)
        stop_pos = std::min(stop_pos, pos_im_end);
      if (pos_im_start != std::string::npos)
        stop_pos = std::min(stop_pos, pos_im_start);

      if (stop_pos != std::string::npos) {
        result = result.substr(0, stop_pos);
        break;
      }
    }

    llama_batch next = llama_batch_get_one(&token, 1);
    if (llama_decode(ctx, next) != 0)
      break;
  }

  llama_sampler_free(sampler);
  llama_memory_clear(llama_get_memory(ctx), false);

  dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);

  size_t start = result.find_first_not_of(" \t\n\r");
  size_t end = result.find_last_not_of(" \t\n\r");
  if (start == std::string::npos)
    return "";
  return result.substr(start, end - start + 1);
}

void SLMEngine::release() {
  if (ctx) {
    llama_free(ctx);
    ctx = nullptr;
  }
  if (model) {
    llama_model_free(model);
    model = nullptr;
  }
  llama_backend_free();
}

} // namespace SLM
