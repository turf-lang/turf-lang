/**
 * @file SLMEngine.cpp
 * @brief Llama.cpp backend interface for the Small Language Model.
 * @details Manages the lifecycle, loading, and inference of the quantized
 *          GGML model used for smart diagnostics. Provides a singleton engine
 *          instance for low-overhead tokenization and text generation.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "SLMEngine.h"
#include "llama.h"
#include <cstdio>
#include <iostream>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace SLM {
SLMEngine::SLMEngine() {}
SLMEngine::~SLMEngine() {
  if (ctx)
    llama_free(ctx);
  if (model)
    llama_model_free(model);
  if (backendInitialized) {
    llama_backend_free();
    backendInitialized = false;
  }
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
  if (saved_stderr != -1) {
    freopen("/dev/null", "w", stderr);
  }

  if (!backendInitialized) {
    llama_backend_init();
    backendInitialized = true;
  }
  
  llama_model_params model_params = llama_model_default_params();

  model_params.use_mmap = true;
  model = llama_model_load_from_file(modelPath.c_str(), model_params);

  if (!model) {
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
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

  if (!ctx) {
    llama_model_free(model);
    model = nullptr;
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
    std::cerr << "Compiler Warning: Failed to create SLM context for model at " << modelPath
              << "\n";
    return false;
  }

  // Restore stderr
  if (saved_stderr != -1) {
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
  }

  return true;
}

/**
 * @brief Generates an AI suggestion based on the provided prompt.
 *
 * Tokenizes the input, runs inference using greedy sampling, and stops
 * generation upon encountering ChatML stop tokens. Suppresses underlying
 * backend logs to maintain compiler output cleanliness.
 *
 * @param prompt The formatted ChatML prompt containing the error context.
 * @return std::string The raw generated response from the model.
 */
std::string SLMEngine::generateSuggestion(const std::string &systemPrompt,
                                          const std::string &userPrompt,
                                          const std::string &grammarStr) {
  if (!ctx)
    return "";

  // Suppress llama.cpp output during inference too
  int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr != -1) {
    freopen("/dev/null", "w", stderr);
  }

  int n_ctx = llama_n_ctx(ctx);
  const llama_vocab *vocab_tok = llama_model_get_vocab(llama_get_model(ctx));

  // ── 1. Evaluate System Prompt (Cached) ──
  if (systemPrompt != CurrentCachedSystemPrompt || CachedSystemTokens.empty()) {
    llama_memory_clear(llama_get_memory(ctx), false);
    
    std::vector<llama_token> sysTokens(n_ctx);
    int n_sys = llama_tokenize(vocab_tok, systemPrompt.c_str(), systemPrompt.size(),
                               sysTokens.data(), n_ctx,
                               /*add_special=*/true, /*parse_special=*/true);
    
    if (n_sys > 0) {
      sysTokens.resize(n_sys);
      llama_batch sysBatch = llama_batch_get_one(sysTokens.data(), n_sys);
      if (llama_decode(ctx, sysBatch) == 0) {
        CachedSystemTokens.clear();
        for (int tok : sysTokens) CachedSystemTokens.push_back(tok);
        CurrentCachedSystemPrompt = systemPrompt;
      } else {
        CachedSystemTokens.clear();
        CurrentCachedSystemPrompt.clear();
      }
    }
  }

  // ── 2. Evaluate User Prompt ──
  std::vector<llama_token> userTokens(n_ctx);
  int n_user = llama_tokenize(vocab_tok, userPrompt.c_str(), userPrompt.size(),
                              userTokens.data(), n_ctx,
                              /*add_special=*/false, /*parse_special=*/true);

  if (n_user <= 0) {
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
    return "";
  }
  userTokens.resize(n_user);

  int total_tokens = CachedSystemTokens.size() + n_user;

  // Token-budget guard
  static constexpr int GEN_BUDGET = 512;
  if (total_tokens > n_ctx - GEN_BUDGET) {
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
    std::cerr << "Compiler Warning: SLM prompt too large (" << total_tokens
              << " tokens, ctx=" << n_ctx
              << "). Skipping suggestion to avoid truncation.\n";
    return "";
  }

  // Manually construct the user batch with the correct starting position offset.
  // llama_batch_get_one() returns pos=nullptr — writing through a nullptr is UB
  // (and the crash the bug report describes). We allocate a positions buffer and
  // point the batch's pos field at it before passing it to llama_decode.
  llama_batch userBatch = llama_batch_get_one(userTokens.data(), n_user);
  std::vector<llama_pos> userPositions(n_user);
  for (int i = 0; i < n_user; i++) {
    userPositions[i] = static_cast<llama_pos>(CachedSystemTokens.size()) + i;
  }
  userBatch.pos = userPositions.data();

  if (llama_decode(ctx, userBatch) != 0) {
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
    return "";
  }

  llama_sampler *sampler =
      llama_sampler_chain_init(llama_sampler_chain_default_params());

  // Grammar-constrained sampling: when a GBNF grammar string is provided,
  // add it first in the chain so it masks out any token that would violate
  // the grammar — making malformed JSON structurally unproduceable.
  // The grammar sampler is skipped for free-text calls (e.g. classifier).
  if (!grammarStr.empty()) {
    const llama_vocab *vocab_g = llama_model_get_vocab(llama_get_model(ctx));
    llama_sampler *grammar =
        llama_sampler_init_grammar(vocab_g, grammarStr.c_str(), "root");
    if (grammar) {
      llama_sampler_chain_add(sampler, grammar);
    } else {
      // nullptr means the grammar string failed to parse (not just missing file).
      // The file-missing case produces an empty grammarStr and never reaches here.
      std::cerr << "Compiler Warning: GBNF grammar failed to parse; "
                   "falling back to unconstrained output.\n";
    }
  }

  // Repetition penalty: prevents the 1.5B model from looping/repeating tokens
  // under greedy decoding — a known failure mode for small models.
  // last_n=64 tokens window, repeat_penalty=1.1, freq/presence penalties off.
  llama_sampler_chain_add(
      sampler, llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
  // Greedy decoding: pick the highest-probability token deterministically.
  // Note: a temperature sampler is intentionally NOT chained before greedy —
  // temperature only reshapes the distribution and has zero effect on argmax.
  llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

  std::string result;
  // Longest stop token is "<|im_start|>" (13 chars). Keep a trailing window
  // of that length so we can detect stop sequences without re-scanning the
  // entire result string from position 0 on every token (O(n) → O(1)).
  static constexpr size_t STOP_WINDOW = 20; // slightly more than 13 for safety
  const std::string STOP_IM_END = "<|im_end|>";
  const std::string STOP_IM_START = "<|im_start|>";

  const llama_vocab *vocab = llama_model_get_vocab(llama_get_model(ctx));
  for (int i = 0; i < 512; i++) {
    llama_token token = llama_sampler_sample(sampler, ctx, -1);
    if (llama_vocab_is_eog(vocab, token))
      break;

    char buf[256];
    int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
    if (n > 0) {
      result += std::string(buf, n);

      // Only search the trailing window — avoids re-scanning the whole string.
      size_t windowStart =
          result.size() > STOP_WINDOW ? result.size() - STOP_WINDOW : 0;
      std::string_view tail(result.data() + windowStart,
                            result.size() - windowStart);

      auto findInTail = [&](const std::string &stop) -> size_t {
        size_t p = tail.find(stop);
        return p == std::string_view::npos ? std::string::npos
                                          : windowStart + p;
      };

      size_t pos_im_end = findInTail(STOP_IM_END);
      size_t pos_im_start = findInTail(STOP_IM_START);

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

    // For generating subsequent tokens, the position must continue incrementing.
    // llama_batch_get_one() returns pos=nullptr, so we must point it at a valid
    // stack variable before calling llama_decode.
    llama_pos nextPos =
        static_cast<llama_pos>(CachedSystemTokens.size()) + n_user + i;
    llama_batch next = llama_batch_get_one(&token, 1);
    next.pos = &nextPos;
    if (llama_decode(ctx, next) != 0)
      break;
  }

  llama_sampler_free(sampler);
  
  // Clear only the user prompt and generated tokens from the KV cache,
  // retaining the cached system prompt prefix.
  llama_memory_seq_rm(llama_get_memory(ctx), -1, CachedSystemTokens.size(), -1);

  if (saved_stderr != -1) {
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
  }

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
  if (backendInitialized) {
    llama_backend_free();
    backendInitialized = false;
  }
}

} // namespace SLM
