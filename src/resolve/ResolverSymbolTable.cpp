/**
 * @file ResolverSymbolTable.cpp
 * @brief Implementation of the composite ResolverSymbolTable (Issue #2).
 *
 * All the actual data-owning logic now lives in ResolverScopeTable.cpp /
 * ResolverModuleTable.cpp / ResolverTypeRegistry.cpp -- this file only
 * orchestrates dump()/dumpModules(), the two methods that genuinely need to
 * combine output from all 3 sub-tables in a specific interleaved order.
 * Every other ResolverSymbolTable method is a trivial one-line delegation,
 * inlined directly in ResolverSymbolTable.h.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "resolve/ResolverSymbolTable.h"

#include "frontend/AST.h" // FuncDefExprAST -- methodKey() reads a declaration's name/arity

// The identity two `self.m()` calls and a declaration share: module, the
// "Struct.method" name FuncDefExprAST stores, and arity counting `self`.
// Overloads that differ only by parameter TYPE share a key on purpose -- the
// Resolver cannot tell them apart (no types yet), so the closure treats the
// key as a group (see mutatesSelfTransitively()).
std::string ResolverSymbolTable::methodKey(const std::string &Module,
                                           const FuncDefExprAST *Decl) {
  return Module + "::" + Decl->getName() + "/" + std::to_string(Decl->getParams().size());
}

void ResolverSymbolTable::computeMutatingClosure() const {
  Mutating_ = DirectMutators_;
  // Fixed point: a method joins once some call in its body names a group whose
  // every member is already in. Each pass adds at least one method or stops,
  // so it ends in at most (#methods) passes -- and a recursive pair that never
  // mutates directly stays out, as it should.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const auto &[Caller, Callees] : SelfCalls_) {
      if (Mutating_.count(Caller))
        continue;
      for (const std::string &Key : Callees) {
        auto It = MethodsByKey_.find(Key);
        if (It == MethodsByKey_.end() || It->second.empty())
          continue;
        bool AllMutate = true;
        for (const FuncDefExprAST *M : It->second)
          if (!Mutating_.count(M)) { AllMutate = false; break; }
        if (AllMutate) {
          Mutating_.insert(Caller);
          Changed = true;
          break;
        }
      }
    }
  }
  ClosureDirty_ = false;
}

void ResolverSymbolTable::dump(std::ostream &OS) const {
  OS << "\n=== Resolver Symbol Table Dump ===\n";
  TypeRegistry_.dumpStructNames(OS);
  ScopeTable_.dumpScopes(OS);
  ModuleTable_.dumpCurrentFileFunctionsBrief(OS);
  ModuleTable_.dumpModuleFunctionsBrief(OS);
  OS << "==================================\n\n";
}

void ResolverSymbolTable::dumpModules(std::ostream &OS) const {
  OS << "\n=== Module System Dump ===\n";
  ModuleTable_.dumpModulesDetailed(OS);
  TypeRegistry_.dumpTypeOwnership(OS);
  ModuleTable_.dumpFlatImports(OS);
  ModuleTable_.dumpCurrentFileFunctionsDetailed(OS);
  OS << "===========================\n\n";
}
