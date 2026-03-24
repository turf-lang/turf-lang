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
};

// Extracts +/- `contextSize` lines around the given error line.
std::string getCodeContext(int errorLine, int contextSize = 10);

// Fetches a suggestion from the SLM based on the error message and source
// context. Returns an empty AISuggestion if the SLM fails or times out.
AISuggestion fetchSuggestion(const std::string &errorMessage, int errorLine);

} // namespace SLM

#endif // SLM_INTEGRATION_H
