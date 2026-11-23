/**
 * @file NullablePlace.h
 * @brief AN-4: which `&?T` expressions the nullability analysis can narrow.
 *
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#pragma once

#include "frontend/AST.h"

#include <cstdint>
#include <optional>
#include <string>

namespace NullablePlace {

/// A variable, or a field read from one, any number of fields deep (`h`,
/// `h.next`, `self.head.next`): what a `!= null` test can narrow. A call,
/// an index or anything else is a fresh value each time it is evaluated, so
/// no earlier test says anything about it.
inline bool isPlace(const ExprAST *E) {
  if (dynamic_cast<const VariableExprAST *>(E))
    return true;
  if (const auto *F = dynamic_cast<const FieldAccessExprAST *>(E))
    return isPlace(F->getBase());
  return false;
}

/// The field path a `!= null` test narrows, keyed by its root variable's
/// symbol (or `self`): `<symbol>.next.val`. Empty for a bare variable, which
/// the pass tracks by symbol, and for anything that is not a place.
inline std::optional<std::string> fieldPathKey(const ExprAST *E) {
  const auto *F = dynamic_cast<const FieldAccessExprAST *>(E);
  if (!F)
    return std::nullopt;
  std::string Fields = "." + F->getFieldName();
  const ExprAST *Base = F->getBase();
  while (const auto *Inner = dynamic_cast<const FieldAccessExprAST *>(Base)) {
    Fields = "." + Inner->getFieldName() + Fields;
    Base = Inner->getBase();
  }
  const auto *Root = dynamic_cast<const VariableExprAST *>(Base);
  if (!Root)
    return std::nullopt;
  if (const auto *Sym = Root->getResolvedSymbol())
    return std::to_string(reinterpret_cast<std::uintptr_t>(Sym)) + Fields;
  if (Root->getName() == "self")
    return "self" + Fields;
  return std::nullopt;
}

/// The root variable's key, the prefix of every path through it.
inline std::optional<std::string> rootKey(const VariableExprAST *V) {
  if (const auto *Sym = V->getResolvedSymbol())
    return std::to_string(reinterpret_cast<std::uintptr_t>(Sym));
  if (V->getName() == "self")
    return std::string("self");
  return std::nullopt;
}

/// The source spelling of a place, for a message: `h.next.val`.
inline std::string spelling(const ExprAST *E) {
  if (const auto *V = dynamic_cast<const VariableExprAST *>(E))
    return V->getName();
  if (const auto *F = dynamic_cast<const FieldAccessExprAST *>(E))
    return spelling(F->getBase()) + "." + F->getFieldName();
  return "";
}

/// Where a place is, for a diagnostic.
inline SourceLocation location(const ExprAST *E) {
  if (const auto *V = dynamic_cast<const VariableExprAST *>(E))
    return V->getLoc();
  if (const auto *F = dynamic_cast<const FieldAccessExprAST *>(E))
    return F->getLoc();
  return E->getSpan();
}

} // namespace NullablePlace
