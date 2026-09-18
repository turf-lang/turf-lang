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
  // Which line replacement_line should be applied to. 0 (the default) means
  // "the line the diagnostic was raised on" -- the common case. A model can
  // set this to a different line (e.g. a declaration site shown in the
  // prompt's pinned context) when the correct fix belongs there instead of
  // at the line the error fired on -- see target_line's doc comment in
  // grammars/slm_suggestion.gbnf for the motivating case.
  int target_line = 0;
  // Last line (inclusive, same 1-based numbering as target_line) of the
  // range replacement_line replaces. 0 (the default) means "just
  // target_line" -- a single-line edit, the common case. Set > target_line
  // when the fix genuinely requires replacing more than one existing line
  // (e.g. wrapping a statement in a new loop, adding a multi-line function
  // signature) -- replacement_line then holds the full new block, with
  // individual lines separated by the JSON "\n" escape. See
  // grammars/slm_suggestion.gbnf for the matching grammar rule.
  int end_line = 0;
  std::string warning;
  std::string self_reported_category; // Model's own assessment of error category
  bool valid = false;
  bool sandbox_validated = false; // Whether sandbox validation passed
};

// Extracts +/- `contextSize` lines around the given error line.
std::string getCodeContext(int errorLine, int contextSize = 10);

// Fetches a suggestion from the SLM based on the error message, source
// context, and optional hint. Returns an empty AISuggestion if the SLM fails or times out.
// `category`: ground-truth category, either from the raising TurfError
// subclass's own C++ type (see Errors.h's Category_ field) or from the
// explicit argument a raw LogErrorAt() call site passed (Lexer.h) -- never
// re-derived from message text (DIAG-001 deleted that fallback, formerly
// ErrorCategory.h's classifyError()). GENERAL is itself a real, audited
// answer for a site, not "no category available."
// `previousAttemptFailed`: the real compiler diagnostic text from the
// sandbox's rejection of a prior attempt's replacement_line (see
// Sandbox::getValidationDiagnostics()), fed back into the prompt so a retry
// (validateAndApplySuggestion()'s retry loop) can course-correct instead of
// re-guessing blind. Empty on a first attempt.
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine,
                             const std::string &hint = "",
                             const std::vector<RelatedLocation> &related = {},
                             ErrorCategory category = ErrorCategory::GENERAL,
                             const std::string &previousAttemptFailed = "");

// ── End-to-End Helper ────────────────────────────────────────────────────────
// 1. Calls fetchSuggestion() to query the SLM.
// 2. Extracts the resulting patch, comment, explanation, and fix strategy.
// 3. Performs basic validation (e.g. ensuring syntax isn't obviously broken).
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine,
                                        const std::string &hint = "",
                                        const std::vector<RelatedLocation> &related = {},
                                        ErrorCategory category = ErrorCategory::GENERAL);

// Applies `suggestion.replacement_line` in a fresh Sandbox and validates it
// (parse + semantic check + confirms the original error is actually gone).
// If validation fails and `outDiagnostics` is non-null, it's filled with the
// sandbox's own compiler diagnostics for the failed attempt (joined into one
// string) -- validateAndApplySuggestion()'s retry loop feeds this back into
// the next fetchSuggestion() call as `previousAttemptFailed` so a retry has
// the real reason the last patch didn't compile, not just "try again".
bool verifySuggestion(const AISuggestion &suggestion, int errorLine,
                      std::string *outDiagnostics = nullptr);

} // namespace SLM

#endif // SLM_INTEGRATION_H
