#include "analysis/AnalysisFacts.h"

// Not an instance: the compilation owns its facts, and this only names the
// innermost live scope's (AnalysisFacts.h).
static AnalysisFacts *CurrentFacts = nullptr;

AnalysisFacts *Facts() { return CurrentFacts; }

AnalysisFactsScope::AnalysisFactsScope(AnalysisFacts *F) : Saved(CurrentFacts) {
  CurrentFacts = F;
}

AnalysisFactsScope::~AnalysisFactsScope() { CurrentFacts = Saved; }
