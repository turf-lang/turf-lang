/**
 * @file ResolverModuleTable.cpp
 * @brief Module/import half of the Resolver's symbol table (Issue #2), split
 *        from the original monolithic ResolverSymbolTable.cpp -- see
 *        docs/planning/COMPILER_MODULE_BREAKDOWN.md.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverModuleTable.h"

#include <algorithm>
#include <unordered_set>

// ─── ResolverModule ───────────────────────────────────────────────────────────

void ResolverModule::registerFunction(const ResolverFunctionSymbol &Sym) {
  auto &Overloads = Functions[Sym.BaseName];
  // Dedup by arity + param type names (MangledName no longer exists).
  auto ParamTypes = Sym.paramTypes();
  for (const auto &Existing : Overloads) {
    if (Existing.arity() != Sym.arity())
      continue;
    auto ExistingTypes = Existing.paramTypes();
    bool Same = true;
    for (int i = 0; i < Sym.arity(); ++i) {
      if (!ExistingTypes[i] || !ParamTypes[i] ||
          ExistingTypes[i]->getName() != ParamTypes[i]->getName()) {
        Same = false;
        break;
      }
    }
    if (Same)
      return; // already registered
  }
  Overloads.push_back(Sym);
}

std::vector<std::string> ResolverModule::getFunctionNames() const {
  std::vector<std::string> Names;
  Names.reserve(Functions.size());
  for (const auto &Pair : Functions)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── ResolverModuleTable ──────────────────────────────────────────────────────

void ResolverModuleTable::registerModule(const std::string &ModuleName) {
  if (!Modules_.count(ModuleName))
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
}

void ResolverModuleTable::registerModuleFunction(
    const std::string &ModuleName, const ResolverFunctionSymbol &Sym) {

  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end()) {
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
    It = Modules_.find(ModuleName);
  }
  It->second->registerFunction(Sym);
}

void ResolverModuleTable::registerModuleConstant(
    const std::string &ModuleName, const ResolverConstantSymbol &Sym) {

  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end()) {
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
    It = Modules_.find(ModuleName);
  }
  It->second->Variables[Sym.Name] = Sym;
}

const ResolverConstantSymbol *ResolverModuleTable::findModuleConstant(
    const std::string &ModuleName, const std::string &Name) const {
  auto ModIt = Modules_.find(ModuleName);
  if (ModIt == Modules_.end())
    return nullptr;
  auto VarIt = ModIt->second->Variables.find(Name);
  if (VarIt == ModIt->second->Variables.end())
    return nullptr;
  return &VarIt->second;
}

bool ResolverModuleTable::hasModule(const std::string &ModuleName) const {
  return Modules_.count(ModuleName) > 0;
}

std::vector<std::string> ResolverModuleTable::getAllModuleNames() const {
  std::vector<std::string> Names;
  Names.reserve(Modules_.size());
  for (const auto &Pair : Modules_)
    Names.push_back(Pair.first);
  return Names;
}

std::vector<std::string> ResolverModuleTable::getModuleFunctionNames(
    const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end())
    return {};
  return It->second->getFunctionNames();
}

std::vector<std::string> ResolverModuleTable::getModuleConstantNames(
    const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end())
    return {};
  std::vector<std::string> Names;
  Names.reserve(It->second->Variables.size());
  for (const auto &Pair : It->second->Variables)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── Current-file function registry ──────────────────────────────────────────

void ResolverModuleTable::registerCurrentFileFunction(
    const ResolverFunctionSymbol &Sym) {
  auto &Overloads = CurrentFileFunctions_[Sym.BaseName];
  // Dedup by arity + param type names.
  auto ParamTypes = Sym.paramTypes();
  for (const auto &Existing : Overloads) {
    if (Existing.arity() != Sym.arity())
      continue;
    auto ExistingTypes = Existing.paramTypes();
    bool Same = true;
    for (int i = 0; i < Sym.arity(); ++i) {
      if (!ExistingTypes[i] || !ParamTypes[i] ||
          ExistingTypes[i]->getName() != ParamTypes[i]->getName()) {
        Same = false;
        break;
      }
    }
    if (Same)
      return;
  }
  Overloads.push_back(Sym);
}

std::vector<const ResolverFunctionSymbol *>
ResolverModuleTable::lookupCurrentFileFunctions(
    const std::string &Name) const {
  std::vector<const ResolverFunctionSymbol *> Result;
  auto It = CurrentFileFunctions_.find(Name);
  if (It == CurrentFileFunctions_.end())
    return Result;
  for (const auto &Sym : It->second)
    Result.push_back(&Sym);
  return Result;
}

void ResolverModuleTable::registerCurrentFileConstant(
    const ResolverConstantSymbol &Sym) {
  CurrentFileConstants_[Sym.Name] = Sym;
}

const ResolverConstantSymbol *ResolverModuleTable::lookupCurrentFileConstant(
    const std::string &Name) const {
  auto It = CurrentFileConstants_.find(Name);
  if (It == CurrentFileConstants_.end())
    return nullptr;
  return &It->second;
}

std::vector<std::string>
ResolverModuleTable::getAllCurrentFileFunctionNames() const {
  std::vector<std::string> Names;
  Names.reserve(CurrentFileFunctions_.size());
  for (const auto &Pair : CurrentFileFunctions_)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── On-demand (flat) import registry ────────────────────────────────────────

std::optional<ResolverModuleTable::FlatImportedSymbol>
ResolverModuleTable::registerFlatFunctionImport(const std::string &ContextLabel,
                                                const std::string &Name,
                                                const FlatImportedSymbol &Sym) {
  auto &ContextMap = FlatImportedFunctions_[ContextLabel];
  auto It = ContextMap.find(Name);
  if (It != ContextMap.end())
    return It->second; // collision -- caller raises an error
  ContextMap.emplace(Name, Sym);
  return std::nullopt;
}

const ResolverModuleTable::FlatImportedSymbol *
ResolverModuleTable::lookupFlatFunctionImport(const std::string &ContextLabel,
                                              const std::string &Name) const {
  auto ContextIt = FlatImportedFunctions_.find(ContextLabel);
  if (ContextIt == FlatImportedFunctions_.end())
    return nullptr;
  auto It = ContextIt->second.find(Name);
  if (It == ContextIt->second.end())
    return nullptr;
  return &It->second;
}

std::optional<ResolverModuleTable::FlatImportedSymbol>
ResolverModuleTable::registerFlatConstantImport(const std::string &ContextLabel,
                                                const std::string &Name,
                                                const FlatImportedSymbol &Sym) {
  auto &ContextMap = FlatImportedConstants_[ContextLabel];
  auto It = ContextMap.find(Name);
  if (It != ContextMap.end())
    return It->second; // collision -- caller raises an error
  ContextMap.emplace(Name, Sym);
  return std::nullopt;
}

const ResolverModuleTable::FlatImportedSymbol *
ResolverModuleTable::lookupFlatConstantImport(const std::string &ContextLabel,
                                              const std::string &Name) const {
  auto ContextIt = FlatImportedConstants_.find(ContextLabel);
  if (ContextIt == FlatImportedConstants_.end())
    return nullptr;
  auto It = ContextIt->second.find(Name);
  if (It == ContextIt->second.end())
    return nullptr;
  return &It->second;
}

std::optional<ResolverModuleTable::FlatImportedSymbol>
ResolverModuleTable::registerFlatTypeImport(const std::string &ContextLabel,
                                            const std::string &Name,
                                            const FlatImportedSymbol &Sym) {
  auto &ContextMap = FlatImportedTypes_[ContextLabel];
  auto It = ContextMap.find(Name);
  if (It != ContextMap.end())
    return It->second; // collision -- caller raises an error
  ContextMap.emplace(Name, Sym);
  return std::nullopt;
}

bool ResolverModuleTable::hasFlatTypeImport(
    const std::string &ContextLabel, const std::string &Name) const {
  auto ContextIt = FlatImportedTypes_.find(ContextLabel);
  if (ContextIt == FlatImportedTypes_.end())
    return false;
  return ContextIt->second.count(Name) > 0;
}

// ─── Two-tier diagnostic helpers ─────────────────────────────────────────────

std::vector<ResolverModuleTable::ModuleFunctionMatch>
ResolverModuleTable::findInModules(const std::string &Name) const {
  std::vector<ModuleFunctionMatch> Matches;
  for (const auto &ModPair : Modules_) {
    auto It = ModPair.second->Functions.find(Name);
    if (It == ModPair.second->Functions.end())
      continue;
    for (const auto &Sym : It->second)
      Matches.push_back({ModPair.first, &Sym});
  }
  return Matches;
}

std::vector<std::string>
ResolverModuleTable::getAllModuleFunctionNames() const {
  std::unordered_set<std::string> Seen;
  std::vector<std::string> Names;
  for (const auto &ModPair : Modules_)
    for (const auto &FuncPair : ModPair.second->Functions)
      if (Seen.insert(FuncPair.first).second)
        Names.push_back(FuncPair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── dump()/dumpModules() section helpers ────────────────────────────────────

void ResolverModuleTable::dumpCurrentFileFunctionsBrief(std::ostream &OS) const {
  OS << "\n--- Current-File Functions (Tier 1) ---\n";
  if (CurrentFileFunctions_.empty()) {
    OS << "  (none registered yet)\n";
  } else {
    for (const auto &Pair : CurrentFileFunctions_) {
      for (const auto &Sym : Pair.second) {
        OS << "  fn " << Sym.signature()
           << " [Loc: " << Sym.DeclLoc.Line << ":" << Sym.DeclLoc.Col << "]\n";
      }
    }
  }
}

void ResolverModuleTable::dumpModuleFunctionsBrief(std::ostream &OS) const {
  OS << "\n--- Imported Module Functions (Tier 2) ---\n";
  if (Modules_.empty()) {
    OS << "  (no modules imported)\n";
  } else {
    for (const auto &ModPair : Modules_) {
      const auto &Mod = ModPair.second;
      OS << "Module '" << Mod->Name << "':\n";
      if (Mod->Functions.empty()) {
        OS << "  (empty)\n";
      } else {
        for (const auto &FuncPair : Mod->Functions) {
          for (const auto &Sym : FuncPair.second) {
            OS << "  fn " << Sym.signature() << "\n";
          }
        }
      }
    }
  }
}

void ResolverModuleTable::dumpModulesDetailed(std::ostream &OS) const {
  OS << "--- Imported Modules ---\n";
  if (Modules_.empty()) {
    OS << "  (no modules imported)\n";
  } else {
    std::vector<std::string> ModNames;
    ModNames.reserve(Modules_.size());
    for (const auto &Pair : Modules_)
      ModNames.push_back(Pair.first);
    std::sort(ModNames.begin(), ModNames.end());
    for (const auto &MName : ModNames) {
      const auto &Mod = Modules_.at(MName);
      OS << "Module '" << MName << "':\n";
      if (Mod->Functions.empty()) {
        OS << "    (no functions)\n";
        continue;
      }
      std::vector<std::string> FnNames;
      FnNames.reserve(Mod->Functions.size());
      for (const auto &FPair : Mod->Functions)
        FnNames.push_back(FPair.first);
      std::sort(FnNames.begin(), FnNames.end());
      for (const auto &FName : FnNames) {
        for (const auto &Sym : Mod->Functions.at(FName)) {
          OS << "    fn " << Sym.signature();
          if (Sym.IsExtern) OS << "  [extern]";
          if (Sym.IsVarArg) OS << "  [vararg]";
          OS << "  @ " << Sym.DeclLoc.Line << ":" << Sym.DeclLoc.Col << "\n";
        }
      }
    }
  }
}

void ResolverModuleTable::dumpFlatImports(std::ostream &OS) const {
  OS << "\n--- On-Demand Flat Imports (import module::sym / [list] / [*]) ---\n";
  if (FlatImportedFunctions_.empty()) {
    OS << "  (none -- every import in this program is bare, qualified-access-only)\n";
  } else {
    std::vector<std::string> Contexts;
    Contexts.reserve(FlatImportedFunctions_.size());
    for (const auto &Pair : FlatImportedFunctions_)
      Contexts.push_back(Pair.first);
    std::sort(Contexts.begin(), Contexts.end());
    for (const auto &Ctx : Contexts) {
      OS << (Ctx.empty() ? "(main file)" : "Module '" + Ctx + "'")
         << " flat-imports:\n";
      const auto &Imports = FlatImportedFunctions_.at(Ctx);
      if (Imports.empty()) {
        OS << "    (none)\n";
        continue;
      }
      std::vector<std::string> Names;
      Names.reserve(Imports.size());
      for (const auto &Pair : Imports)
        Names.push_back(Pair.first);
      std::sort(Names.begin(), Names.end());
      for (const auto &Name : Names) {
        const auto &Sym = Imports.at(Name);
        OS << "    " << Name << "  <- from module '" << Sym.SourceModule
           << "' (imported at " << Sym.ImportLoc.Line << ":" << Sym.ImportLoc.Col
           << ")\n";
      }
    }
  }
}

void ResolverModuleTable::dumpCurrentFileFunctionsDetailed(std::ostream &OS) const {
  OS << "\n--- Main-File Top-Level Functions ---\n";
  if (CurrentFileFunctions_.empty()) {
    OS << "  (none)\n";
  } else {
    std::vector<std::string> Names;
    Names.reserve(CurrentFileFunctions_.size());
    for (const auto &Pair : CurrentFileFunctions_)
      Names.push_back(Pair.first);
    std::sort(Names.begin(), Names.end());
    for (const auto &Name : Names)
      for (const auto &Sym : CurrentFileFunctions_.at(Name))
        OS << "  fn " << Sym.signature() << "  @ " << Sym.DeclLoc.Line << ":"
           << Sym.DeclLoc.Col << "\n";
  }
}
