/**
 * @file ResolverVisibility.cpp
 * @brief The Resolver's `pub` checks (LEC-002, FREEZE.md §6.15): the shared
 *        raise/filter helpers every cross-module lookup goes through.
 *
 * @details Kept in one place so the private-access diagnostics are worded and
 *          hinted identically however the private name was reached, and so
 *          "which lookups are gated" is answerable by reading one file plus
 *          its callers -- see docs/development/changes/LEC-002.md.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Resolver.h"

#include "Errors.h"

void Resolver::raisePrivateFunction(SourceLocation Loc, const std::string &Module,
                                    const std::string &Name) const {
  SourceLocation DeclLoc = Loc; // never worse than pointing at the use itself
  std::string Keyword = "fn";
  for (const auto &Match : SymTable_.findInModules(Name)) {
    if (Match.ModuleName != Module || Match.Symbol->IsPublic)
      continue;
    DeclLoc = toSourceLocation(Match.Symbol->DeclLoc);
    if (Match.Symbol->IsExtern)
      Keyword = "extern fn";
    break;
  }
  PrivateSymbolAccessError(Loc, PrivateSymbolAccessError::Kind::Function, Name,
                           Module, DeclLoc, Keyword,
                           SymTable_.getModulePublicFunctionNames(Module))
      .raise();
}

void Resolver::raisePrivateType(SourceLocation Loc, const std::string &Identity) const {
  const std::string Module = TypeIdentity::owningModule(Identity);
  SourceLocation DeclLoc = Loc;
  std::string Keyword = "struct";
  if (const auto *Info = SymTable_.typeVisibility(Identity)) {
    DeclLoc = toSourceLocation(Info->DeclLoc);
    Keyword = Info->Keyword;
  }
  PrivateSymbolAccessError(Loc, PrivateSymbolAccessError::Kind::Type,
                           TypeIdentity::bareName(Identity), Module, DeclLoc,
                           Keyword, SymTable_.getModulePublicTypeNames(Module))
      .raise();
}

void Resolver::raisePrivateMethod(SourceLocation Loc, const std::string &Module,
                                  const std::string &Key) const {
  // A method is registered as "<owner identity>.<method>"; an identity holds
  // `::` but never `.`, so the first `.` splits the two.
  const size_t Dot = Key.find('.');
  const std::string OwnerIdentity = Dot == std::string::npos ? Key : Key.substr(0, Dot);
  const std::string Method = Dot == std::string::npos ? Key : Key.substr(Dot + 1);
  SourceLocation DeclLoc = Loc;
  for (const auto &Match : SymTable_.findInModules(Key)) {
    if (Match.ModuleName != Module || Match.Symbol->IsPublic)
      continue;
    DeclLoc = toSourceLocation(Match.Symbol->DeclLoc);
    break;
  }
  PrivateSymbolAccessError(Loc, PrivateSymbolAccessError::Kind::Method, Method,
                           Module, DeclLoc, "fn",
                           SymTable_.getPublicMethodNames(OwnerIdentity),
                           TypeIdentity::bareName(OwnerIdentity))
      .raise();
}

bool Resolver::filterToVisibleOverloads(
    std::vector<ResolverSymbolTable::ModuleFunctionMatch> &Matches,
    SourceLocation Loc, const std::string &Name) const {
  const std::string From = currentModuleName();
  std::vector<ResolverSymbolTable::ModuleFunctionMatch> Kept;
  const ResolverSymbolTable::ModuleFunctionMatch *FirstDropped = nullptr;
  for (const auto &Match : Matches) {
    if (Match.ModuleName == From || Match.Symbol->IsPublic)
      Kept.push_back(Match);
    else if (!FirstDropped)
      FirstDropped = &Match;
  }
  if (Kept.empty() && FirstDropped) {
    // "Owner.method" keys are methods (static calls and `Owner::new`).
    if (Name.find('.') != std::string::npos)
      raisePrivateMethod(Loc, FirstDropped->ModuleName, Name);
    else
      raisePrivateFunction(Loc, FirstDropped->ModuleName, Name);
    return false;
  }
  Matches = std::move(Kept);
  return true;
}
