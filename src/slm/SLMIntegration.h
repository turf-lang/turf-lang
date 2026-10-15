#ifndef SLM_INTEGRATION_H
#define SLM_INTEGRATION_H

#include "diagnostics/ErrorCategory.h" // for ErrorCategory
#include "frontend/Lexer.h"            // for RelatedLocation
#include <string>
#include <vector>

namespace SLM {

// DiagnosticHints is defined in ErrorCategory.h (included above) -- see its
// doc comment there for why (Lexer.h needs the complete type without being
// able to include this header).

struct AISuggestion {
  float confidence = 0.0f;
  std::string explanation;
  std::string fix_mode = "NONE";
  std::string suggested_comment;
  // The edit, as the model states it (schema version kSlmSchemaVersion, see
  // SLMIntegration.cpp): the lines it wants to change, copied from the code
  // it was shown, and what they should become. The model never says WHERE --
  // a 1.5B model copies text far more reliably than it counts lines, and a
  // wrong line number was how correct fixes were lost (issue #267).
  // verifySuggestion() finds original_code in the source (locateOriginal()).
  // An empty original_code means "insert new_code on the line(s) after the
  // error line".
  std::string original_code;
  std::string new_code;
  // Where the validated edit went, set by verifySuggestion() once the sandbox
  // accepts it, and zero otherwise: the 1-based inclusive source range that was replaced, and the
  // exact text it was replaced with ('\n'-joined). These are what the
  // suggestion's before/after is rendered from (renderPatchDiff()), for the
  // so the display never depends on what the model claimed to be editing -- a fuzzy copy shows the lines it
  // really matched.
  int patchStart = 0;
  int patchEnd = 0;
  std::string patchText;
  std::string warning;
  std::string self_reported_category; // Model's own assessment of error category
  bool valid = false;
  bool sandbox_validated = false; // Whether sandbox validation passed
  // SLM-001 follow-up: the FULL patched source (every line, '\n'-joined),
  // populated by verifySuggestion() only when sandbox_validated is true --
  // empty otherwise, including for a suggestion that was never sandbox-
  // checked at all (fix_mode == NONE). This is what --smart-write-fix=<path>
  // (Lexer.h's g_SmartFixOutPath) writes to disk, and what lets a tool
  // outside the compiler (scripts/slm/slm_benchmark/run_benchmark.py) actually
  // recompile and RUN the corrected program instead of only ever reading a
  // description of the fix.
  std::string appliedSource;
  // The first-attempt cache key (item 4) of the diagnostic this suggestion
  // answers, set by fetchSuggestion() on every attempt, and whether this
  // suggestion was served from that cache. validateAndApplySuggestion()
  // writes the cache only once a suggestion has VALIDATED, and evicts a
  // cached one that no longer does (issue #267): caching every raw first
  // answer meant a wrong one was replayed as attempt 1 on every later run.
  std::string cacheKey;
  bool fromCache = false;
};

// The source lines [errorLine - contextSize, errorLine + contextSize], joined
// with '\n' and exactly as the file has them: no line numbers, no header, no
// marker. Verbatim on purpose, so any line copied out of it is a valid
// `original_code`; which line the error is on travels in the prompt's own
// "error_line" field. Empty when no source has been loaded.
std::string getCodeContext(int errorLine, int contextSize = 10);

// The coloured before/after of a validated suggestion, ready to print under
// "Suggested fix:" (src/frontend/Lexer.cpp): the replaced source lines `- N | ...`,
// then the new ones `+ N | ...`, with the lines that are the same at either
// end of the range left out so an inserted `;` shows one line, not a whole
// block. Built from patchStart/patchEnd/patchText and the ORIGINAL source
// (SourceLines is never committed to), not from original_code/new_code, so a
// fuzzy-matched copy shows what was really replaced. Empty when the
// suggestion carries no validated patch.
std::string renderPatchDiff(const AISuggestion &suggestion);

// Fetches a suggestion from the SLM based on the error message, source
// context, and optional hint. Returns an empty AISuggestion if the SLM fails or times out.
// `category`: ground-truth category, either from the raising TurfError
// subclass's own C++ type (see Errors.h's Category_ field) or from the
// explicit argument a raw LogErrorAt() call site passed (Lexer.h) -- never
// re-derived from message text (DIAG-001 deleted that fallback, formerly
// ErrorCategory.h's classifyError()). GENERAL is itself a real, audited
// answer for a site, not "no category available."
// `previousAttemptFailed`: why a prior attempt was rejected -- the real
// compiler diagnostic text from the sandbox (see
// Sandbox::getValidationDiagnostics()), or verifySuggestion()'s own message
// when `original_code` was not found in the program -- fed back into the
// prompt so a retry (validateAndApplySuggestion()'s retry loop) can
// course-correct instead of re-guessing blind. Empty on a first attempt.
// `hints`: see DiagnosticHints -- the known-type facts and the receiver
// struct's declaration become their own fields in the prompt's JSON header
// (buildUserPrompt()); the fix-its are not consulted here (a diagnostic
// whose fix-its check out never reaches the model: CompilerFix.h).
// `errorCode`: the diagnostic's stable "E0NN" code (TurfError::getCode()),
// or "" for the raw LogErrorAt() path/warnings. Part of the on-disk cache
// key alongside the normalized message, the whole user-prompt body (code
// window and every fact beside it) and the documentation text -- see
// slmCacheKey() (SLMIntegration.cpp) for exactly what is hashed and why. Only
// a suggestion that passed sandbox validation is ever cached as a fix. The
// other outcome, "the model could not fix this" (it answered NONE, or all
// three attempts failed), is remembered separately for 24 hours as a negative
// entry that carries no suggestion: a first-attempt fetchSuggestion() that hits
// one returns an invalid NONE suggestion with fromCache set, without loading
// the model. A retry (non-empty previousAttemptFailed) never consults it.
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine,
                             const std::string &hint = "",
                             const std::vector<RelatedLocation> &related = {},
                             ErrorCategory category = ErrorCategory::GENERAL,
                             const std::string &previousAttemptFailed = "",
                             const DiagnosticHints &hints = {},
                             const std::string &errorCode = "");

// ── End-to-End Helper ────────────────────────────────────────────────────────
// Asked only for a diagnostic the compiler has no checked fix for of its own
// (TDP-2-001: DiagnosticEngine::add() tries CompilerFix first).
// 1. Calls fetchSuggestion() to query the SLM.
// 2. Extracts the resulting patch, comment, explanation, and fix strategy.
// 3. Performs basic validation (e.g. ensuring syntax isn't obviously broken).
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine,
                                        const std::string &hint = "",
                                        const std::vector<RelatedLocation> &related = {},
                                        ErrorCategory category = ErrorCategory::GENERAL,
                                        const DiagnosticHints &hints = {},
                                        const std::string &errorCode = "");

// Finds `suggestion.original_code` in the source, replaces it with
// `suggestion.new_code` in a fresh Sandbox and validates the result (parse +
// semantic check + confirms the original error is actually gone). Where the
// code is found, and what happens when it is not, is locateOriginal()'s
// contract (SLMIntegration.cpp).
// `suggestion` is a non-const reference (not just read from) because a
// SUCCESSFUL validation writes the full patched source into
// `suggestion.appliedSource` (SLM-001 follow-up) and the range and text that
// were spliced into patchStart/patchEnd/patchText -- every other field is
// read-only here.
// `related` is the diagnostic's pinned locations: a copy of one of those
// lines (the declaration an error points back to) is as legitimate a place
// to look as the error line itself.
// If validation fails and `outDiagnostics` is non-null, it's filled with why
// -- the sandbox's own compiler diagnostics for the failed attempt (joined
// into one string), or the message for a copy that matched nothing --
// validateAndApplySuggestion()'s retry loop feeds this back into the next
// fetchSuggestion() call as `previousAttemptFailed` so a retry has the real
// reason the last patch was rejected, not just "try again".
bool verifySuggestion(AISuggestion &suggestion, int errorLine,
                      std::string *outDiagnostics = nullptr,
                      const std::vector<RelatedLocation> &related = {});

} // namespace SLM

#endif // SLM_INTEGRATION_H
