/**
 * @file ResolverSymbolTable.cpp
 * @brief Implementation of the LLVM-free Resolver symbol table (Issue #2).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverSymbolTable.h"

#include <algorithm>
#include <map>
#include <ostream>
#include <stdexcept>
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

// NOTE: ResolverModule::lookupFunction(BaseName, ArgTypes) -- an arity +
// exact-type-match + numeric-coercion overload resolver for a single module
// -- used to live here. Removed after confirming via repo-wide grep it had
// zero callers (its only caller, ResolverSymbolTable::resolveModuleFunction,
// was itself dead and removed alongside it). See ResolverSymbolTable.h's
// comment at the registerFunction() declaration for what the Resolver
// actually uses instead (findInModules() + inline arity/type narrowing in
// Resolver::visitFuncCall/visitMethodCall).

std::vector<std::string> ResolverModule::getFunctionNames() const {
  std::vector<std::string> Names;
  Names.reserve(Functions.size());
  for (const auto &Pair : Functions)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── ResolverSymbolTable ──────────────────────────────────────────────────────

ResolverSymbolTable::ResolverSymbolTable() {
  // Start with global scope (depth 0)
  ScopeStack_.push_back(std::make_unique<ResolverScope>(0, nullptr));
  // NOTE: No implicit "main" module. Current-file functions use the
  // separate CurrentFileFunctions_ registry, not Modules_.
}

void ResolverSymbolTable::enterScope() {
  ++Depth_;
  ResolverScope *Parent = ScopeStack_.empty() ? nullptr : ScopeStack_.back().get();
  ScopeStack_.push_back(std::make_unique<ResolverScope>(Depth_, Parent));
}

void ResolverSymbolTable::exitScope() {
  if (ScopeStack_.size() <= 1)
    return; // Never pop the global scope
  ScopeStack_.pop_back();
  --Depth_;
}

void ResolverSymbolTable::resetToDepth(size_t TargetDepth) {
  while (Depth_ > TargetDepth && ScopeStack_.size() > 1) {
    exitScope();
  }
}

std::shared_ptr<VariableSymbol> ResolverSymbolTable::declareVariable(
    const std::string &Name, TurfTypePtr Type, ResolverSourceLocation Loc, bool IsImmutable) {

  if (ScopeStack_.empty())
    enterScope();

  ResolverScope &Current = *ScopeStack_.back();

  if (Current.has(Name)) {
    throw std::runtime_error("Variable '" + Name +
                             "' is already declared in the current scope");
  }

  auto Sym = std::make_shared<VariableSymbol>(Name, std::move(Type), Loc, Depth_, IsImmutable);
  Current.Symbols[Name] = Sym;
  return Sym;
}

std::shared_ptr<VariableSymbol> ResolverSymbolTable::lookupVariable(
    const std::string &Name) const {

  // Walk from innermost to outermost scope
  for (auto It = ScopeStack_.rbegin(); It != ScopeStack_.rend(); ++It) {
    auto Sym = (*It)->lookupLocal(Name);
    if (Sym)
      return Sym;
  }
  return nullptr;
}

bool ResolverSymbolTable::isDeclaredInCurrentScope(
    const std::string &Name) const {
  if (ScopeStack_.empty())
    return false;
  return ScopeStack_.back()->has(Name);
}

std::vector<std::string> ResolverSymbolTable::getAllVisibleNames() const {
  std::vector<std::string> Names;
  std::unordered_set<std::string> Seen;
  // Walk from innermost (back) to outermost (front) — innermost shadows outer.
  for (auto It = ScopeStack_.rbegin(); It != ScopeStack_.rend(); ++It) {
    for (const auto &Pair : (*It)->Symbols) {
      if (Seen.insert(Pair.first).second)
        Names.push_back(Pair.first);
    }
  }
  return Names;
}

void ResolverSymbolTable::registerModule(const std::string &ModuleName) {
  if (!Modules_.count(ModuleName))
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
}

void ResolverSymbolTable::registerModuleFunction(
    const std::string &ModuleName, const ResolverFunctionSymbol &Sym) {

  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end()) {
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
    It = Modules_.find(ModuleName);
  }
  It->second->registerFunction(Sym);
}

void ResolverSymbolTable::registerModuleConstant(
    const std::string &ModuleName, const ResolverConstantSymbol &Sym) {

  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end()) {
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
    It = Modules_.find(ModuleName);
  }
  It->second->Variables[Sym.Name] = Sym;
}

const ResolverConstantSymbol *ResolverSymbolTable::findModuleConstant(
    const std::string &ModuleName, const std::string &Name) const {
  auto ModIt = Modules_.find(ModuleName);
  if (ModIt == Modules_.end())
    return nullptr;
  auto VarIt = ModIt->second->Variables.find(Name);
  if (VarIt == ModIt->second->Variables.end())
    return nullptr;
  return &VarIt->second;
}

bool ResolverSymbolTable::hasModule(const std::string &ModuleName) const {
  return Modules_.count(ModuleName) > 0;
}

std::vector<std::string> ResolverSymbolTable::getModuleFunctionNames(
    const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end())
    return {};
  return It->second->getFunctionNames();
}

std::vector<std::string> ResolverSymbolTable::getModuleConstantNames(
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

void ResolverSymbolTable::registerCurrentFileFunction(
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
ResolverSymbolTable::lookupCurrentFileFunctions(
    const std::string &Name) const {
  std::vector<const ResolverFunctionSymbol *> Result;
  auto It = CurrentFileFunctions_.find(Name);
  if (It == CurrentFileFunctions_.end())
    return Result;
  for (const auto &Sym : It->second)
    Result.push_back(&Sym);
  return Result;
}

void ResolverSymbolTable::registerCurrentFileConstant(
    const ResolverConstantSymbol &Sym) {
  CurrentFileConstants_[Sym.Name] = Sym;
}

const ResolverConstantSymbol *ResolverSymbolTable::lookupCurrentFileConstant(
    const std::string &Name) const {
  auto It = CurrentFileConstants_.find(Name);
  if (It == CurrentFileConstants_.end())
    return nullptr;
  return &It->second;
}

std::vector<std::string>
ResolverSymbolTable::getAllCurrentFileFunctionNames() const {
  std::vector<std::string> Names;
  Names.reserve(CurrentFileFunctions_.size());
  for (const auto &Pair : CurrentFileFunctions_)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── On-demand (flat) import registry ────────────────────────────────────────

std::optional<ResolverSymbolTable::FlatImportedSymbol>
ResolverSymbolTable::registerFlatFunctionImport(const std::string &ContextLabel,
                                                const std::string &Name,
                                                const FlatImportedSymbol &Sym) {
  auto &ContextMap = FlatImportedFunctions_[ContextLabel];
  auto It = ContextMap.find(Name);
  if (It != ContextMap.end())
    return It->second; // collision -- caller raises an error
  ContextMap.emplace(Name, Sym);
  return std::nullopt;
}

const ResolverSymbolTable::FlatImportedSymbol *
ResolverSymbolTable::lookupFlatFunctionImport(const std::string &ContextLabel,
                                              const std::string &Name) const {
  auto ContextIt = FlatImportedFunctions_.find(ContextLabel);
  if (ContextIt == FlatImportedFunctions_.end())
    return nullptr;
  auto It = ContextIt->second.find(Name);
  if (It == ContextIt->second.end())
    return nullptr;
  return &It->second;
}

std::optional<ResolverSymbolTable::FlatImportedSymbol>
ResolverSymbolTable::registerFlatConstantImport(const std::string &ContextLabel,
                                                const std::string &Name,
                                                const FlatImportedSymbol &Sym) {
  auto &ContextMap = FlatImportedConstants_[ContextLabel];
  auto It = ContextMap.find(Name);
  if (It != ContextMap.end())
    return It->second; // collision -- caller raises an error
  ContextMap.emplace(Name, Sym);
  return std::nullopt;
}

const ResolverSymbolTable::FlatImportedSymbol *
ResolverSymbolTable::lookupFlatConstantImport(const std::string &ContextLabel,
                                              const std::string &Name) const {
  auto ContextIt = FlatImportedConstants_.find(ContextLabel);
  if (ContextIt == FlatImportedConstants_.end())
    return nullptr;
  auto It = ContextIt->second.find(Name);
  if (It == ContextIt->second.end())
    return nullptr;
  return &It->second;
}

std::optional<ResolverSymbolTable::FlatImportedSymbol>
ResolverSymbolTable::registerFlatTypeImport(const std::string &ContextLabel,
                                            const std::string &Name,
                                            const FlatImportedSymbol &Sym) {
  auto &ContextMap = FlatImportedTypes_[ContextLabel];
  auto It = ContextMap.find(Name);
  if (It != ContextMap.end())
    return It->second; // collision -- caller raises an error
  ContextMap.emplace(Name, Sym);
  return std::nullopt;
}

bool ResolverSymbolTable::isTypeVisibleUnqualified(
    const std::string &ContextLabel, const std::string &Name) const {
  if (getTypeOwningModule(Name) == ContextLabel)
    return true;
  auto ContextIt = FlatImportedTypes_.find(ContextLabel);
  if (ContextIt == FlatImportedTypes_.end())
    return false;
  return ContextIt->second.count(Name) > 0;
}

std::vector<std::string>
ResolverSymbolTable::getModuleTypeNames(const std::string &ModuleName) const {
  std::vector<std::string> Names;
  for (const auto &Pair : TypeOwningModule_)
    if (Pair.second == ModuleName)
      Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── Two-tier diagnostic helpers ─────────────────────────────────────────────

std::vector<ResolverSymbolTable::ModuleFunctionMatch>
ResolverSymbolTable::findInModules(const std::string &Name) const {
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
ResolverSymbolTable::getAllModuleFunctionNames() const {
  std::unordered_set<std::string> Seen;
  std::vector<std::string> Names;
  for (const auto &ModPair : Modules_)
    for (const auto &FuncPair : ModPair.second->Functions)
      if (Seen.insert(FuncPair.first).second)
        Names.push_back(FuncPair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ─── Struct name registry ────────────────────────────────────────────────────────────

std::optional<ResolverSourceLocation>
ResolverSymbolTable::registerStruct(const std::string &Name,
                                    ResolverSourceLocation Loc,
                                    const std::string &ModuleName) {
  auto It = RegisteredStructs_.find(Name);
  if (It != RegisteredStructs_.end())
    return It->second; // duplicate -- caller raises DuplicateTypeNameError
  RegisteredStructs_.emplace(Name, Loc);
  TypeOwningModule_.emplace(Name, ModuleName);
  return std::nullopt;
}

bool ResolverSymbolTable::isKnownStruct(const std::string &Name) const {
  return RegisteredStructs_.count(Name) > 0;
}

std::vector<std::string> ResolverSymbolTable::getAllStructNames() const {
  std::vector<std::string> Names;
  Names.reserve(RegisteredStructs_.size());
  for (const auto &Pair : RegisteredStructs_)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

std::string ResolverSymbolTable::getTypeOwningModule(const std::string &Name) const {
  auto It = TypeOwningModule_.find(Name);
  return It == TypeOwningModule_.end() ? "" : It->second;
}


// ─── Enum registry ──────────────────────────────────────────────────────────

std::optional<ResolverSourceLocation>
ResolverSymbolTable::registerEnumType(const std::string &Name,
                                      ResolverSourceLocation Loc,
                                      std::shared_ptr<TurfEnumType> ET,
                                      const std::string &ModuleName) {
  auto LocIt = EnumDeclLocs_.find(Name);
  if (LocIt != EnumDeclLocs_.end())
    return LocIt->second; // duplicate -- caller raises DuplicateTypeNameError
  EnumDeclLocs_.emplace(Name, Loc);
  RegisteredEnums_[Name] = std::move(ET);
  TypeOwningModule_.emplace(Name, ModuleName);
  return std::nullopt;
}

std::shared_ptr<TurfEnumType> ResolverSymbolTable::lookupEnumType(const std::string& Name) const {
  auto It = RegisteredEnums_.find(Name);
  if (It != RegisteredEnums_.end()) {
    return It->second;
  }
  return nullptr;
}

bool ResolverSymbolTable::isEnumName(const std::string& Name) const {
  return RegisteredEnums_.count(Name) > 0;
}

std::vector<std::string> ResolverSymbolTable::getAllEnumNames() const {
  std::vector<std::string> Names;
  Names.reserve(RegisteredEnums_.size());
  for (const auto& Pair : RegisteredEnums_) {
    Names.push_back(Pair.first);
  }
  std::sort(Names.begin(), Names.end());
  return Names;
}

void ResolverSymbolTable::dump(std::ostream &OS) const {
  OS << "\n=== Resolver Symbol Table Dump ===\n";

  // Registered struct names (hoisted in Pass 1)
  OS << "--- Registered Struct Names (Pass 1 Hoist) ---\n";
  if (RegisteredStructs_.empty()) {
    OS << "  (none)\n";
  } else {
    auto Names = getAllStructNames(); // sorted
    for (const auto &N : Names)
      OS << "  struct " << N << "\n";
  }

  // Scopes
  OS << "--- Variable Scopes (depth: " << Depth_ << ") ---\n";
  for (int i = static_cast<int>(ScopeStack_.size()) - 1; i >= 0; --i) {
    const auto &Scope = ScopeStack_[i];
    OS << "Scope Level " << Scope->Depth << ":\n";
    if (Scope->Symbols.empty()) {
      OS << "  (empty)\n";
    } else {
      for (const auto &Pair : Scope->Symbols) {
        const auto &Sym = Pair.second;
        OS << "  Var: " << Sym->Name << " : "
           << (Sym->Type ? Sym->Type->getName() : "unknown")
           << " [Loc: " << Sym->DeclLoc.Line << ":" << Sym->DeclLoc.Col << "]\n";
      }
    }
  }

  // Current-file functions (Tier 1)
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

  // Imported module functions (Tier 2)
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
  OS << "==================================\n\n";
}

void ResolverSymbolTable::dumpModules(std::ostream &OS) const {
  OS << "\n=== Module System Dump ===\n";

  // ── Every imported module and its full function registry ──────────────────
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

  // ── Struct/enum ownership, grouped by declaring module ─────────────────────
  OS << "\n--- Struct/Enum Type Ownership ---\n";
  if (TypeOwningModule_.empty()) {
    OS << "  (no struct/enum types registered)\n";
  } else {
    std::map<std::string, std::vector<std::string>> ByModule; // "" = main file
    for (const auto &Pair : TypeOwningModule_)
      ByModule[Pair.second].push_back(Pair.first);
    for (auto &Pair : ByModule)
      std::sort(Pair.second.begin(), Pair.second.end());
    for (const auto &Pair : ByModule) {
      OS << (Pair.first.empty() ? "(main file)" : "Module '" + Pair.first + "'")
         << " declares:\n";
      for (const auto &TypeName : Pair.second) {
        const char *Kind = RegisteredEnums_.count(TypeName) ? "enum" : "struct";
        OS << "    " << Kind << " " << TypeName << "\n";
      }
    }
  }

  // ── On-demand flat imports, grouped by requesting file/module context ─────
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

  // ── Main file's own top-level functions, for contrast ──────────────────────
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

  OS << "===========================\n\n";
}
