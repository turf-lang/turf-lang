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

#include "resolve/Resolver.h"

#include "diagnostics/Errors.h"
#include "frontend/Parser.h"

bool Resolver::mayQualifyWith(const std::string &Module, const std::string &Context) const {
  // The prelude is "always there" (docs/slm/modules.txt): ParsePreludeImports()
  // injects it into the main file only, so a module that uses `strings::` would
  // otherwise newly fail where it always worked.
  return IsPreludeModuleName(Module) || SymTable_.isModuleImportedBy(Module, Context);
}

bool Resolver::requireModuleImported(const std::string &Module, const std::string &Context,
                                     SourceLocation Loc, const std::string &Spelled) const {
  if (mayQualifyWith(Module, Context))
    return true;
  // A clone of a generic is resolved in its template's module (Context), never
  // the instantiating file, so a `pub fn<T>` that uses `io::` is judged by its
  // own imports.
  ModuleNotImportedError(Loc, Module, Context, SymTable_.hasSymbolImport(Module, Context),
                         Module + "::" + Spelled)
      .raise();
  return false;
}

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

namespace {

/// Every named user-type identity @p T mentions, at any depth. Generic type
/// parameters and primitives are not registered identities, so the caller's
/// isKnownTypeIdentity() test drops them.
void collectMentionedTypes(const TurfType *T, std::vector<std::string> &Out) {
  if (!T)
    return;
  if (const auto *NT = dynamic_cast<const NamedType *>(T)) {
    Out.push_back(NT->getName());
  } else if (const auto *GT = dynamic_cast<const GenericType *>(T)) {
    Out.push_back(GT->getBaseName());
    for (const auto &Arg : GT->getTypeArgs())
      collectMentionedTypes(Arg.get(), Out);
  } else if (const auto *PT = dynamic_cast<const TurfPointerType *>(T)) {
    // Also `&T` and `&?T`, which are TurfPointerType subclasses.
    collectMentionedTypes(PT->getPointee().get(), Out);
  } else if (const auto *AT = dynamic_cast<const ArrayType *>(T)) {
    collectMentionedTypes(AT->getElementTypePtr().get(), Out);
  } else if (const auto *EU = dynamic_cast<const TurfErrorUnionType *>(T)) {
    collectMentionedTypes(EU->getSuccessTypePtr().get(), Out);
  } else if (const auto *FT = dynamic_cast<const FuncType *>(T)) {
    for (const auto &P : FT->getParamTypes())
      collectMentionedTypes(P.get(), Out);
    collectMentionedTypes(FT->getReturnType().get(), Out);
  }
}

} // namespace

void Resolver::checkNoPrivateInPublic(
    SourceLocation Loc, const std::string &ItemKind, const std::string &ItemName,
    const std::vector<const TurfType *> &Types) const {
  const std::string Module = currentModuleName();
  if (Module.empty() || FunctionNestingDepth_ > 0 || SuppressUnqualifiedVisibilityCheck_)
    return;
  std::vector<std::string> Mentioned;
  for (const auto *T : Types)
    collectMentionedTypes(T, Mentioned);
  for (const auto &Id : Mentioned) {
    // Only this module's own private types can leak through its own
    // signatures: a type of another module that is not `pub` could not have
    // been written here at all (E088).
    if (!SymTable_.isKnownTypeIdentity(Id) || TypeIdentity::owningModule(Id) != Module ||
        SymTable_.isTypePublic(Id))
      continue;
    SourceLocation DeclLoc = Loc;
    std::string Keyword = "struct";
    if (const auto *Info = SymTable_.typeVisibility(Id)) {
      DeclLoc = toSourceLocation(Info->DeclLoc);
      Keyword = Info->Keyword;
    }
    PrivateInPublicError(Loc, ItemKind, ItemName, TypeIdentity::bareName(Id), Keyword,
                         Module, DeclLoc)
        .raise();
    return; // one diagnostic per item is enough
  }
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
