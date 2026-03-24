#ifndef SLM_INTEGRATION_H
#define SLM_INTEGRATION_H

#include <string>

namespace SLM {

// Extracts +/- `contextSize` lines around the given error line.
std::string getCodeContext(int errorLine, int contextSize = 10);

// Fetches a suggestion from the SLM based on the error message and source
// context. Returns an empty string if the SLM fails or times out.
std::string fetchSuggestion(const std::string &errorMessage, int errorLine);

} // namespace SLM

#endif // SLM_INTEGRATION_H
