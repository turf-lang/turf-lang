/**
 * @file TypeCheckerVisibility.cpp
 * @brief The TypeChecker's half of `pub` enforcement (LEC-002, FREEZE.md
 *        §6.15): struct fields, instance methods and UFCS candidates.
 *
 * @details The Resolver gates everything it can name without knowing a
 *          receiver's type (functions, types, static methods, `T::new`).
 *          A field access `p.x`, an instance call `p.len()` and a UFCS call
 *          `p.f()` all depend on the TYPE of `p`, which only this pass
 *          knows -- so their visibility check lives here, against
 *          CurrentModule_ (TypeChecker.h).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "sema/TypeChecker.h"

#include "diagnostics/Errors.h"

bool TypeChecker::memberVisible(const VisibilityMark &Vis,
                                const std::string &OwnerIdentity) const {
  return Vis.IsPublic || TypeIdentity::owningModule(OwnerIdentity) == CurrentModule_;
}

void TypeChecker::raisePrivateField(SourceLocation Loc, const TurfStructType &STy,
                                    const StructField &Field) const {
  const std::string &Identity = STy.getName();
  PrivateSymbolAccessError(Loc, PrivateSymbolAccessError::Kind::Field, Field.Name,
                           TypeIdentity::owningModule(Identity), Field.NameLoc, "",
                           RST_.getPublicMethodNames(Identity),
                           TypeIdentity::bareName(Identity))
      .raise();
}

bool TypeChecker::filterVisibleCandidates(
    std::vector<const ResolverFunctionSymbol *> &Candidates, SourceLocation Loc,
    const std::string &Name, const std::string &OwnerIdentity) {
  std::vector<const ResolverFunctionSymbol *> Kept;
  const ResolverFunctionSymbol *FirstDropped = nullptr;
  for (const auto *C : Candidates) {
    if (C->IsPublic || C->SourceModule == CurrentModule_)
      Kept.push_back(C);
    else if (!FirstDropped)
      FirstDropped = C;
  }
  if (Kept.empty() && FirstDropped) {
    if (OwnerIdentity.empty()) {
      PrivateSymbolAccessError(Loc, PrivateSymbolAccessError::Kind::Function, Name,
                               FirstDropped->SourceModule,
                               toSourceLocation(FirstDropped->DeclLoc), "fn",
                               RST_.getModulePublicFunctionNames(FirstDropped->SourceModule))
          .raise();
    } else {
      PrivateSymbolAccessError(Loc, PrivateSymbolAccessError::Kind::Method, Name,
                               FirstDropped->SourceModule,
                               toSourceLocation(FirstDropped->DeclLoc), "fn",
                               RST_.getPublicMethodNames(OwnerIdentity),
                               TypeIdentity::bareName(OwnerIdentity))
          .raise();
    }
    return false;
  }
  Candidates = std::move(Kept);
  return true;
}
