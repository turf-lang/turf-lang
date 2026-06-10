/**
 * @file ResolverSymbolTable.cpp
 * @brief Implementation of the LLVM-free Resolver symbol table (Issue #2).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverSymbolTable.h"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <unordered_set>

// ─── Local helpers ────────────────────────────────────────────────────────────

/// Check if two Turf type names are implicitly compatible for overload
/// resolution.  Mirrors the logic in SymbolTable.cpp without depending on it.
static bool typesCompatible(const TurfType *From, const TurfType *To) {
  if (!From || !To)
    return false;
  if (From->getName() == To->getName())
    return true;
  // Generic type parameters match anything (type erasure)
  if (dynamic_cast<const TurfGenericParamType *>(From) ||
      dynamic_cast<const TurfGenericParamType *>(To))
    return true;
  // void and string are not numeric-coercible
  if (From->isVoid() || To->isVoid())
    return false;
  if (From->isNamed("string") || To->isNamed("string"))
    return false;
  // Remaining numerics (int, double, bool) are mutually coercible
  return true;
}

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

const ResolverFunctionSymbol *ResolverModule::lookupFunction(
    const std::string &BaseName,
    const std::vector<TurfTypePtr> &ArgTypes) const {

  auto It = Functions.find(BaseName);
  if (It == Functions.end())
    return nullptr;

  const auto &Overloads = It->second;
  const int Arity = static_cast<int>(ArgTypes.size());

  // Pass 1: exact arity + exact type name match
  for (const auto &Sym : Overloads) {
    if (Sym.arity() != Arity)
      continue;
    bool Exact = true;
    auto PT = Sym.paramTypes();
    for (int i = 0; i < Arity; ++i) {
      if (!ArgTypes[i] || !PT[i])
        continue;
      if (ArgTypes[i]->getName() != PT[i]->getName()) {
        Exact = false;
        break;
      }
    }
    if (Exact)
      return &Sym;
  }

  // Pass 2: arity match + implicit numeric coercion
  for (const auto &Sym : Overloads) {
    if (Sym.arity() != Arity)
      continue;
    bool Compatible = true;
    auto PT = Sym.paramTypes();
    for (int i = 0; i < Arity; ++i) {
      if (!typesCompatible(ArgTypes[i].get(), PT[i].get())) {
        Compatible = false;
        break;
      }
    }
    if (Compatible)
      return &Sym;
  }

  return nullptr;
}

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

std::shared_ptr<VariableSymbol> ResolverSymbolTable::declareVariable(
    const std::string &Name, TurfTypePtr Type, ResolverSourceLocation Loc) {

  if (ScopeStack_.empty())
    enterScope();

  ResolverScope &Current = *ScopeStack_.back();

  if (Current.has(Name)) {
    throw std::runtime_error("Variable '" + Name +
                             "' is already declared in the current scope");
  }

  auto Sym = std::make_shared<VariableSymbol>(Name, std::move(Type), Loc, Depth_);
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

void ResolverSymbolTable::registerModuleFunction(
    const std::string &ModuleName, const ResolverFunctionSymbol &Sym) {

  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end()) {
    Modules_[ModuleName] = std::make_unique<ResolverModule>(ModuleName);
    It = Modules_.find(ModuleName);
  }
  It->second->registerFunction(Sym);
}

const ResolverFunctionSymbol *ResolverSymbolTable::resolveModuleFunction(
    const std::string &ModuleName,
    const std::string &FuncName,
    const std::vector<TurfTypePtr> &ArgTypes) const {

  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end())
    return nullptr;
  return It->second->lookupFunction(FuncName, ArgTypes);
}

bool ResolverSymbolTable::hasModule(const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end())
    return false;
  return !It->second->Functions.empty();
}

std::vector<std::string> ResolverSymbolTable::getModuleFunctionNames(
    const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  if (It == Modules_.end())
    return {};
  return It->second->getFunctionNames();
}

const ResolverModule *
ResolverSymbolTable::getModule(const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  return It != Modules_.end() ? It->second.get() : nullptr;
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

std::vector<std::string>
ResolverSymbolTable::getAllCurrentFileFunctionNames() const {
  std::vector<std::string> Names;
  Names.reserve(CurrentFileFunctions_.size());
  for (const auto &Pair : CurrentFileFunctions_)
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


void ResolverSymbolTable::dump(std::ostream &OS) const {
  OS << "\n=== Resolver Symbol Table Dump ===\n";

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
    OS << "  (none registered yet — Issue #6 will populate this)\n";
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
