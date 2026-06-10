/**
 * @file ast_stubs.cpp
 * @brief Minimal stub that defines globals required by the standalone
 *        test_type_checker binary.
 *
 * In the full turf build:
 *   ExprAST::NextNodeID / ResetNodeIDCounter → defined in Sandbox.cpp
 *   CompilerExecutablePath                   → defined in main.cpp
 *   CollectChangedNodeIDs / AreNodesStructurallyEqual → Codegen.cpp / Sandbox.cpp
 *   SLM::validateAndApplySuggestion          → SLMIntegration.cpp
 *
 * The test target cannot link those files because they drag in the full
 * LLVM+Codegen+Parser chain.  This tiny stub provides the minimum definitions
 * that are ODR-required somewhere in the link unit.
 *
 * Do NOT include this file in the main turf executable.
 */

#include "AST.h"
#include "SLMIntegration.h"

#include <set>

// ── ExprAST static members (normally in Sandbox.cpp) ────────────────────────
uint64_t ExprAST::NextNodeID = 0;

void ExprAST::ResetNodeIDCounter() { NextNodeID = 0; }

// ── CompilerExecutablePath (normally in main.cpp) ────────────────────────────
std::string CompilerExecutablePath;

// ── SLM stubs (normally in SLMIntegration.cpp) ──────────────────────────────
namespace SLM {
std::string getCodeContext(int, int) { return {}; }
AISuggestion fetchSuggestion(const std::string &, int) { return {}; }
AISuggestion validateAndApplySuggestion(const std::string &, int) { return {}; }
bool verifySuggestion(const AISuggestion &, const std::string &, int) { return false; }
} // namespace SLM
