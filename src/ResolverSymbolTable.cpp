/**
 * @file ResolverSymbolTable.cpp
 * @brief Implementation of the LLVM-free Resolver symbol table (Issue #2).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverSymbolTable.h"

#include <algorithm>
#include <stdexcept>

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
    for (int i = 0; i < Arity; ++i) {
      if (!ArgTypes[i] || !Sym.ParamTypes[i])
        continue;
      if (ArgTypes[i]->getName() != Sym.ParamTypes[i]->getName()) {
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
    for (int i = 0; i < Arity; ++i) {
      if (!typesCompatible(ArgTypes[i].get(), Sym.ParamTypes[i].get())) {
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
  // Start with global scope
  ScopeStack_.push_back(std::make_unique<ResolverScope>(0, nullptr));
  // Register implicit "main" module for user-defined functions
  Modules_["main"] = std::make_unique<ResolverModule>("main");
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

const ResolverModule *ResolverSymbolTable::getModule(
    const std::string &ModuleName) const {
  auto It = Modules_.find(ModuleName);
  return It != Modules_.end() ? It->second.get() : nullptr;
}
