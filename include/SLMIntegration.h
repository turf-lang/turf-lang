#ifndef SLM_INTEGRATION_H
#define SLM_INTEGRATION_H

#include "ErrorCategory.h" // for ErrorCategory
#include "Lexer.h"         // for RelatedLocation
#include <string>
#include <vector>

namespace SLM {

struct AISuggestion {
  float confidence = 0.0f;
  std::string explanation;
  std::string fix_mode = "NONE";
  std::string suggested_comment;
  std::string code_patch;       // Human-readable description of the patch
  std::string replacement_line; // Exact Turf-Lang source line to substitute
  std::string warning;
  std::string self_reported_category; // Model's own assessment of error category
  bool valid = false;
  bool sandbox_validated = false; // Whether sandbox validation passed
};

// Extracts +/- `contextSize` lines around the given error line.
std::string getCodeContext(int errorLine, int contextSize = 10);

// Fetches a suggestion from the SLM based on the error message, source
// context, and optional hint. Returns an empty AISuggestion if the SLM fails or times out.
// `category`: ground-truth category from the raising TurfError subclass's own
// C++ type (see Errors.h's Category_ field). When non-GENERAL, this is used
// directly instead of re-deriving a category via classifyError()'s text
// matching -- GENERAL (the default) preserves the old classifyError()
// fallback behavior for callers that don't have a category (the raw
// LogErrorAt() path has no TurfError instance to read one from).
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine,
                             const std::string &hint = "",
                             const std::vector<RelatedLocation> &related = {},
                             ErrorCategory category = ErrorCategory::GENERAL);

// ── End-to-End Helper ────────────────────────────────────────────────────────
// 1. Calls fetchSuggestion() to query the SLM.
// 2. Extracts the resulting patch, comment, explanation, and fix strategy.
// 3. Performs basic validation (e.g. ensuring syntax isn't obviously broken).
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine,
                                        const std::string &hint = "",
                                        const std::vector<RelatedLocation> &related = {},
                                        ErrorCategory category = ErrorCategory::GENERAL);

// TODO(future): Self-running verification of AI-suggested code patches.
// Before presenting a suggested fix to the user, this function should:
// 1. Take the original source code and the suggested patch.
// 2. Substitute the patched line(s) into the source.
// 3. Run the modified source through a lightweight compilation pass.
// 4. Return true only if the patched code compiles without errors.
// If verification fails, the AI suggestion should be silently discarded
// and the default compiler error message shown instead.
//
// Current status: stub — always returns true. The infrastructure for
// invoking a child compile pass needs to be designed first.
bool verifySuggestion(const AISuggestion &suggestion, int errorLine);

} // namespace SLM

#endif // SLM_INTEGRATION_H
