/**
 * @file builtins_stubs.cpp
 * @brief Minimal stub for the Builtins API used by the standalone
 *        test_type_checker binary.
 *
 * In the full turf build, Builtins.cpp provides:
 *   - RegisterBuiltin / LookupBuiltin / GetBuiltinNames   (registry)
 *   - CodegenIntrinsic                                    (LLVM IR gen)
 *
 * The test binary cannot link Builtins.cpp because CodegenIntrinsic calls
 * TheModule / Builder / TheContext (defined in Codegen.cpp → needs the full
 * LLVM back-end chain).
 *
 * This stub provides ONLY the registry functions with the same built-in table
 * that Builtins.cpp registers, plus a no-op CodegenIntrinsic.  The TypeChecker
 * only calls LookupBuiltin(), so CodegenIntrinsic is never invoked.
 *
 * Do NOT include this file in the main turf executable.
 */

#include "Builtins.h"
#include "Lexer.h"   // SourceLocation
#include "llvm/IR/Value.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Private registry (mirrors the one in Builtins.cpp) ──────────────────────

static std::unordered_map<std::string, BuiltinInfo> g_builtins;
static std::once_flag g_builtins_init_flag;

static void ensure_builtins_registered_stub() {
  g_builtins["print"]     = {1, "Print a value"};
  g_builtins["printline"] = {1, "Print a value with newline"};
  g_builtins["input"]     = {0, "Read a line from stdin"};
  g_builtins["malloc"]    = {1, "Allocate memory"};
  g_builtins["realloc"]   = {2, "Reallocate memory"};
  g_builtins["free"]      = {1, "Free memory"};
  g_builtins["power"]     = {2, "Power operation"};
  g_builtins["pow"]       = {2, "Power operation (alias)"};
  g_builtins["abs"]       = {1, "Absolute value"};
  g_builtins["lengthof"]  = {1, "Length of string"};
  g_builtins["typeof"]    = {1, "Type of value"};
}

void RegisterBuiltin(const std::string &Name, const BuiltinInfo &Info) {
  g_builtins[Name] = Info;
}

const BuiltinInfo *LookupBuiltin(const std::string &Name) {
  std::call_once(g_builtins_init_flag, ensure_builtins_registered_stub);
  auto It = g_builtins.find(Name);
  return It == g_builtins.end() ? nullptr : &It->second;
}

std::vector<std::string> GetBuiltinNames() {
  std::call_once(g_builtins_init_flag, ensure_builtins_registered_stub);
  std::vector<std::string> Names;
  Names.reserve(g_builtins.size());
  for (const auto &P : g_builtins)
    Names.push_back(P.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── CodegenIntrinsic stub (never called in unit tests) ──────────────────────

llvm::Value *CodegenIntrinsic(const std::string & /*IntrinsicName*/,
                               std::vector<llvm::Value *> & /*Args*/,
                               SourceLocation /*Loc*/) {
  // This function is never called from the TypeChecker; it exists only so the
  // linker is satisfied (Builtins.h declares it).
  return nullptr;
}
