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

#include "ResolverSymbolTable.h"

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
