/**
 * @file ResolverScopeTable.cpp
 * @brief Lexical-scope half of the Resolver's symbol table (Issue #2), split
 *        from the original monolithic ResolverSymbolTable.cpp -- see
 *        docs/planning/COMPILER_MODULE_BREAKDOWN.md.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverScopeTable.h"

#include <stdexcept>
#include <unordered_set>

ResolverScopeTable::ResolverScopeTable() {
  // Start with global scope (depth 0)
  ScopeStack_.push_back(std::make_unique<ResolverScope>(0, nullptr));
}

void ResolverScopeTable::enterScope() {
  ++Depth_;
  ResolverScope *Parent = ScopeStack_.empty() ? nullptr : ScopeStack_.back().get();
  ScopeStack_.push_back(std::make_unique<ResolverScope>(Depth_, Parent));
}

void ResolverScopeTable::exitScope() {
  if (ScopeStack_.size() <= 1)
    return; // Never pop the global scope
  ScopeStack_.pop_back();
  --Depth_;
}

void ResolverScopeTable::resetToDepth(size_t TargetDepth) {
  while (Depth_ > TargetDepth && ScopeStack_.size() > 1) {
    exitScope();
  }
}

std::shared_ptr<VariableSymbol> ResolverScopeTable::declareVariable(
    const std::string &Name, TurfTypePtr Type, ResolverSourceLocation Loc,
    bool IsImmutable, bool IsParameter) {

  if (ScopeStack_.empty())
    enterScope();

  ResolverScope &Current = *ScopeStack_.back();

  if (Current.has(Name)) {
    throw std::runtime_error("Variable '" + Name +
                             "' is already declared in the current scope");
  }

  auto Sym = std::make_shared<VariableSymbol>(Name, std::move(Type), Loc, Depth_, IsImmutable, IsParameter);
  Current.Symbols[Name] = Sym;
  return Sym;
}

std::shared_ptr<VariableSymbol> ResolverScopeTable::lookupVariable(
    const std::string &Name) const {

  // Walk from innermost to outermost scope
  for (auto It = ScopeStack_.rbegin(); It != ScopeStack_.rend(); ++It) {
    auto Sym = (*It)->lookupLocal(Name);
    if (Sym)
      return Sym;
  }
  return nullptr;
}

bool ResolverScopeTable::isDeclaredInCurrentScope(
    const std::string &Name) const {
  if (ScopeStack_.empty())
    return false;
  return ScopeStack_.back()->has(Name);
}

std::vector<std::string> ResolverScopeTable::getAllVisibleNames() const {
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

void ResolverScopeTable::dumpScopes(std::ostream &OS) const {
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
}
