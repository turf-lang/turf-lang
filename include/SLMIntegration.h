#ifndef SLM_INTEGRATION_H
#define SLM_INTEGRATION_H

#include <string>

namespace SLM {

struct AISuggestion {
    float confidence = 0.0f;
    std::string explanation;
    std::string fix_mode = "NONE";
    std::string suggested_comment;
    std::string code_patch;
    std::string warning;
    bool valid = false;
    bool sandbox_validated = false;      // Whether sandbox validation passed
};

// Extracts +/- `contextSize` lines around the given error line.
std::string getCodeContext(int errorLine, int contextSize = 10);

// Fetches a suggestion from the SLM based on the error message and source
// context. Returns an empty AISuggestion if the SLM fails or times out.
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine);

// Validate and apply an SLM suggestion in a sandbox environment.
// This function:
// 1. Gets an SLM suggestion
// 2. Creates a sandbox environment
// 3. Applies the fix in the sandbox
// 4. Re-validates compilation in the sandbox
// 5. Commits if valid (confidence >= 0.9), rejects if invalid
// Returns the suggestion with sandbox_validated set appropriately.
AISuggestion validateAndApplySuggestion(const std::string &errorMessage,
                                        int errorLine);

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
bool verifySuggestion(const AISuggestion &suggestion,
                      const std::string &originalSource,
                      int errorLine);

} // namespace SLM

#endif // SLM_INTEGRATION_H
