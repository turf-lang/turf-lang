/**
 * @file SLMEngineDisabled.cpp
 * @brief The SLMEngine a turfc built with -DTURF_SMART=OFF links instead of
 *        SLMEngine.cpp: same interface, no llama.cpp.
 *
 * @details `--smart`'s model runtime is optional (CMakeLists.txt, TURF_SMART):
 *          a source build that only wants to compile TurfLang should not have
 *          to clone and build an ML runtime first. This engine never loads a
 *          model. initialize() returns false, which is exactly the path a
 *          missing model file already takes (SLMIntegration.cpp returns the
 *          compiler's own diagnostic unchanged), so nothing downstream needs to
 *          know which engine it has (SLMIntegration.cpp asks builtIn() first,
 *          so it does not look for a model either). The one difference is a single note, so a
 *          user who asked for `--smart` learns why they got no suggestion
 *          instead of assuming the model had nothing to say.
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#include "slm/SLMEngine.h"
#include <iostream>

namespace SLM {

SLMEngine::SLMEngine() {}
SLMEngine::~SLMEngine() {}

SLMEngine &SLMEngine::getInstance() {
  static SLMEngine instance;
  return instance;
}

bool SLMEngine::builtIn() { return false; }

bool SLMEngine::initialize(const std::string &) {
  // Once per compile: initialize() is reached once per diagnostic --smart
  // tries to improve, and one explanation is enough.
  static bool Noted = false;
  if (!Noted) {
    Noted = true;
    std::cerr << "note: --smart is not part of this TurfLang build. Install "
                 "the smart build (install.sh --smart, or install.ps1 -Smart), "
                 "or build from source without --no-smart; showing the "
                 "compiler's own diagnostics.\n";
  }
  return false;
}

std::string SLMEngine::generateSuggestion(const std::string &,
                                          const std::string &,
                                          const std::string &) {
  return "";
}

void SLMEngine::release() {}

} // namespace SLM
