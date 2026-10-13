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

#include "slm/SLMEngine.h"
#include "llama.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

bool SLMEngine::builtIn() { return true; }

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
  // NOTE: keep well below n_ctx -- llama_decode() hard-aborts via
  // GGML_ASSERT(n_tokens_all <= n_batch) if a single call's token count
  // (e.g. system-prompt evaluation in generateSuggestion()) exceeds this,
  // and raising it materially slows every call (ggml-metal appears to fall
  // off a fast kernel path above ~4096 batch size -- measured ~4-5x slower
  // at 5120/8192/10240 than at 4096, not proportional to the increase, so
  // this isn't a "just raise it" fix). The real fix for a too-long prompt is
  // keeping the prompt under this cap (see SLMIntegration.cpp's system
  // prompt construction) -- this constant is a hard ceiling to stay under,
  // not a knob to tune around a too-long prompt.
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

namespace {
/// Shrinks `tokens[0..n)` to at most `n_batch` entries by dropping tokens from
/// the middle: the head is kept, plus a tail of min(1024, n_batch / 4) tokens
/// (where closing rules / the ChatML turn markers live). Returns the new count.
int clampMiddleDrop(std::vector<llama_token> &tokens, int n, int n_batch) {
  if (n <= n_batch)
    return n;
  const int tail = std::min(1024, n_batch / 4);
  const int head = n_batch - tail;
  std::copy(tokens.begin() + (n - tail), tokens.begin() + n,
            tokens.begin() + head);
  return n_batch;
}

/// A conversation (one system message, one user message, the assistant's turn
/// opened) rendered in a model's own chat format and cut in two: `Prefix` is
/// everything that does not depend on the user message (the part whose KV cache
/// is kept between calls), `Rest` is everything after it.
struct ChatPrompt {
  std::string Prefix, Rest;
  bool FromModelTemplate = false;
};

/// llama_chat_apply_template() into a std::string. False when the template is
/// not one llama.cpp knows (it has a list of formats, not a Jinja engine).
bool applyChatTemplate(const char *Tmpl,
                       const std::vector<llama_chat_message> &Msgs,
                       bool AddAssistant, std::string &Out) {
  size_t Chars = 256;
  for (const llama_chat_message &M : Msgs)
    Chars += 2 * std::string_view(M.content).size();
  std::vector<char> Buf(Chars);
  int32_t N = llama_chat_apply_template(Tmpl, Msgs.data(), Msgs.size(),
                                        AddAssistant, Buf.data(),
                                        static_cast<int32_t>(Buf.size()));
  if (N < 0)
    return false;
  if (static_cast<size_t>(N) > Buf.size()) {
    Buf.resize(N);
    N = llama_chat_apply_template(Tmpl, Msgs.data(), Msgs.size(), AddAssistant,
                                  Buf.data(), static_cast<int32_t>(Buf.size()));
    if (N < 0 || static_cast<size_t>(N) > Buf.size())
      return false;
  }
  Out.assign(Buf.data(), N);
  return true;
}

/// Renders System + User with template `Tmpl` and splits the result in two.
/// Three ways, each tried only if the one before cannot work for this template:
///  1. the system message rendered alone is the start of the whole rendering:
///     that is the cached prefix, exactly where the system turn ends;
///  2. otherwise render with a marker standing in for the user's text and cut
///     at the marker (the user's turn opener then lands in the prefix);
///  3. a template with no system role at all (Gemma's): the system text goes
///     in front of the user's, in the user message, and is cut the same way.
bool splitChatPrompt(const char *Tmpl, const std::string &System,
                     const std::string &User, ChatPrompt &Out) {
  static const std::string Marker = "@@TURF_USER_TEXT@@";
  std::string Full, SysOnly;
  std::vector<llama_chat_message> Both = {{"system", System.c_str()},
                                          {"user", User.c_str()}};
  if (applyChatTemplate(Tmpl, Both, true, Full)) {
    std::vector<llama_chat_message> OnlySys = {{"system", System.c_str()}};
    if (applyChatTemplate(Tmpl, OnlySys, false, SysOnly) && !SysOnly.empty() &&
        Full.compare(0, SysOnly.size(), SysOnly) == 0) {
      Out.Prefix = SysOnly;
      Out.Rest = Full.substr(SysOnly.size());
      return true;
    }
  }
  auto CutAtMarker = [&](const std::vector<llama_chat_message> &Msgs) {
    std::string S;
    if (!applyChatTemplate(Tmpl, Msgs, true, S))
      return false;
    size_t At = S.find(Marker);
    if (At == std::string::npos)
      return false;
    Out.Prefix = S.substr(0, At);
    Out.Rest = User + S.substr(At + Marker.size());
    return true;
  };
  std::vector<llama_chat_message> WithMarker = {{"system", System.c_str()},
                                                {"user", Marker.c_str()}};
  if (CutAtMarker(WithMarker))
    return true;
  std::string Merged = System + "\n\n" + Marker;
  std::vector<llama_chat_message> UserOnly = {{"user", Merged.c_str()}};
  return CutAtMarker(UserOnly);
}

/// The prompt in the loaded model's own chat format, read from its GGUF
/// (tokenizer.chat_template). A model with no template, or one llama.cpp does
/// not recognise, gets ChatML, the format the recommended Qwen model uses and
/// the one this was written for.
ChatPrompt formatChatPrompt(const llama_model *Model, const std::string &System,
                            const std::string &User) {
  ChatPrompt P;
  if (const char *T = llama_model_chat_template(Model, nullptr)) {
    if (*T && splitChatPrompt(T, System, User, P)) {
      P.FromModelTemplate = true;
      return P;
    }
  }
  splitChatPrompt("chatml", System, User, P);
  return P;
}
} // namespace

/**
 * @brief Generates an AI suggestion based on the provided prompt.
 *
 * Puts the two texts into the loaded model's own chat format (its GGUF's
 * chat template; ChatML when it has none llama.cpp knows), tokenizes them, runs
 * inference using greedy sampling, and stops on the model's end-of-generation
 * token (or a ChatML turn marker). Suppresses underlying backend logs to
 * maintain compiler output cleanliness.
 *
 * @param systemPrompt The system message, plain text with no chat markers.
 *        Its rendered prefix is what the KV cache keeps between calls.
 * @param userPrompt The user message, plain text with no chat markers.
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
  // llama_decode() hard-aborts via GGML_ASSERT if a single call's token
  // count exceeds n_batch -- not a catchable error, a native abort that
  // takes the whole process down. n_batch is deliberately kept well below
  // n_ctx (see initialize()'s own comment on why it isn't just raised to
  // match), so either the system prompt (a large per-category doc file, or
  // future prompt growth) or the user prompt (large code context) can
  // legitimately exceed it even though n_ctx has room. Every llama_decode()
  // call below clamps its token count to n_batch first -- a clipped prompt
  // degrades response quality, which is fine: the caller's own
  // confidence-gated, sandbox-validated pipeline is exactly the safety net
  // that makes a worse response from a clipped prompt safe to just fall
  // through as normal, since it still can't reach the user without passing
  // sandbox re-validation either way. A crashed compiler process has no
  // such safety net.
  int n_batch = static_cast<int>(llama_n_batch(ctx));
  const llama_vocab *vocab_tok = llama_model_get_vocab(llama_get_model(ctx));

  // The two texts are plain; the model's own chat format goes around them here.
  const ChatPrompt Chat =
      formatChatPrompt(llama_get_model(ctx), systemPrompt, userPrompt);
  if (std::getenv("TURF_SLM_DEBUG") && saved_stderr != -1) {
    auto Escaped = [](const std::string &S, bool Tail) {
      std::string E;
      for (char C : Tail ? S.substr(S.size() > 48 ? S.size() - 48 : 0)
                         : S.substr(0, 48))
        E += C == '\n' ? std::string("\\n") : std::string(1, C);
      return E;
    };
    std::string Msg = std::string("[SLM] chat format: ") +
                      (Chat.FromModelTemplate ? "the model's own template"
                                              : "ChatML (the model has no template llama.cpp knows)") +
                      "; cached part starts \"" + Escaped(Chat.Prefix, false) +
                      "\" and ends \"" + Escaped(Chat.Prefix, true) +
                      "\"; the rest ends \"" + Escaped(Chat.Rest, true) + "\"\n";
    write(saved_stderr, Msg.data(), Msg.size());
  }
  // A template that writes the BOS token itself must not get a second one.
  const char *BosText = llama_vocab_get_text(vocab_tok, llama_vocab_bos(vocab_tok));
  const bool TemplateHasBos =
      BosText && *BosText && Chat.Prefix.rfind(BosText, 0) == 0;

  // ── 1. Evaluate System Prompt (Cached) ──
  if (Chat.Prefix != CurrentCachedSystemPrompt || CachedSystemTokens.empty()) {
    llama_memory_clear(llama_get_memory(ctx), false);

    std::vector<llama_token> sysTokens(n_ctx);
    int n_sys = llama_tokenize(vocab_tok, Chat.Prefix.c_str(), Chat.Prefix.size(),
                               sysTokens.data(), n_ctx,
                               /*add_special=*/!TemplateHasBos,
                               /*parse_special=*/true);
    // Too long: drop tokens from the MIDDLE, not the end. The prompt is
    // preamble, docs, then RULES and the closing `<|im_end|>`, and the old
    // cut-the-tail clamp removed exactly those -- general.txt alone is ~3500
    // tokens, so every GENERAL/BUILTIN_FUNCTION/MEMORY_ERROR prompt lost its
    // output rules and never closed the system turn (measured with
    // llama-tokenize, issue #267). The kept tail covers the rules with room
    // to spare; what is dropped is the end of the category doc, which
    // fetchSuggestion() places after the more specific per-code page.
    n_sys = clampMiddleDrop(sysTokens, n_sys, n_batch);

    if (n_sys > 0) {
      sysTokens.resize(n_sys);
      llama_batch sysBatch = llama_batch_get_one(sysTokens.data(), n_sys);
      if (llama_decode(ctx, sysBatch) == 0) {
        CachedSystemTokens.clear();
        for (int tok : sysTokens) CachedSystemTokens.push_back(tok);
        CurrentCachedSystemPrompt = Chat.Prefix;
      } else {
        CachedSystemTokens.clear();
        CurrentCachedSystemPrompt.clear();
      }
    }
  }

  // ── 2. Evaluate User Prompt ──
  std::vector<llama_token> userTokens(n_ctx);
  int n_user = llama_tokenize(vocab_tok, Chat.Rest.c_str(), Chat.Rest.size(),
                              userTokens.data(), n_ctx,
                              /*add_special=*/false, /*parse_special=*/true);

  if (n_user <= 0) {
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
    return "";
  }
  // Same middle-drop as the system prompt: the END of the user turn holds
  // the turn-closing marker and the opening of the assistant's turn, which the
  // model needs to answer at all.
  n_user = clampMiddleDrop(userTokens, n_user, n_batch);
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
      //
      // IMPORTANT: write this via the saved (real) fd, not std::cerr/stderr --
      // stderr is redirected to /dev/null for the entire duration of this
      // function (see the freopen() above) to suppress llama.cpp's own
      // verbose logging. A plain std::cerr write here would be silently
      // swallowed along with it, which is exactly what happened to this
      // warning for an unknown period: the bundled grammars/slm_suggestion.gbnf
      // had a real GBNF syntax error (multi-line rule bodies need explicit
      // parenthesization in this parser -- bare line-continuation isn't
      // supported, unlike some other GBNF dialects) that made every grammar
      // init silently fail, degrading every --smart call to fully
      // unconstrained generation with zero visible indication -- the model
      // emitted unquoted enum-like JSON values (e.g. `"fix_mode": MODIFY`
      // instead of `"fix_mode": "MODIFY"`), which silently failed
      // extractJsonFieldString()'s parsing and defaulted to fix_mode=NONE,
      // which meant the sandbox-validation path (ApplyAndValidateFix) was
      // never even reached. Writing straight to the saved fd means this
      // class of regression is visible again even with logging suppressed.
      if (saved_stderr != -1) {
        // Plain write(), not dprintf() -- MinGW's <unistd.h> compatibility
        // shim (already relied on above for dup/dup2/close/STDERR_FILENO)
        // declares write() but not the GNU/BSD-extension dprintf(), so this
        // failed to compile at all on Windows ("'dprintf' was not declared
        // in this scope"). No actual printf-style formatting was happening
        // here anyway (no varargs beyond the literal message itself), so
        // write() is a direct, no-behavior-change portable equivalent.
        static const char Msg[] =
            "Compiler Warning: GBNF grammar failed to parse; "
            "falling back to unconstrained output.\n";
        write(saved_stderr, Msg, sizeof(Msg) - 1);
      }
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
