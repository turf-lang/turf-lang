/**
 * @file TypeCheckerPattern.cpp
 * @brief Typing of destructuring patterns in `compare` (LEC-003,
 *        FREEZE.md §6.17 / F-PAT-1).
 *
 * @details TypeChecker::visitCompare() (TypeCheckerExpr.cpp) owns the compare
 *          as a whole -- arm order, guards, unreachable arms, exhaustiveness.
 *          This file owns one pattern at a time: given the type of the value a
 *          pattern is matched against, it checks the pattern makes sense for
 *          that type, writes each binding's type into its symbol and each
 *          struct pattern's field types into the AST (Codegen reads them
 *          there rather than re-deriving them), and answers "is this pattern
 *          irrefutable", which is what makes an arm a catch-all.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "TypeChecker.h"

#include "AST.h"
#include "Errors.h"
#include "Monomorphize.h" // a pattern over a generic struct is an instantiation site
#include "SuggestionIndex.h"
#include "SymbolTable.h"
#include "TypeCheckerInternal.h"

#include <algorithm>

namespace {

// `5` or `-5`: the only shapes an integer literal pattern takes. Unary minus
// is checked structurally because a negative literal is a unary node until
// something folds it, and a pattern must not depend on whether that happened.
bool isIntegerLiteralPattern(const ExprAST *E) {
  if (const auto *N = dynamic_cast<const NumberExprAST *>(E))
    return N->isInteger();
  if (const auto *U = dynamic_cast<const UnaryExprAST *>(E))
    return U->getOpcode() == '-' && isIntegerLiteralPattern(U->getOperand());
  return false;
}

// The value sub-patterns a field can be compared against: a literal whose
// comparison is exact equality, or an enum/machine variant. Everything else
// is refused with a pointer to the guard that does the job.
bool isValuePatternShape(const ExprAST *E) {
  if (isIntegerLiteralPattern(E))
    return true;
  if (dynamic_cast<const CharExprAST *>(E) || dynamic_cast<const StringExprAST *>(E) ||
      dynamic_cast<const BoolExprAST *>(E))
    return true;
  if (const auto *S = dynamic_cast<const ScopeAccessExprAST *>(E))
    return S->ResolvedKind == ScopeAccessKind::EnumVariant;
  return false;
}

// The field types a literal/variant sub-pattern can test. `double` is out on
// purpose (FREEZE.md §6.17: exact float equality is almost never what the
// writer means); a struct, array or pointer has no literal to write.
bool isComparableFieldType(const TurfType *T) {
  if (!T)
    return false;
  if (T->isEnum())
    return true; // enums and machines
  return T->isNamed("int") || T->isNamed("int32") || T->isNamed("char") ||
         T->isNamed("bool") || T->isNamed("string");
}

TurfTypePtr resolveNamed(const TurfTypePtr &T) {
  if (!T)
    return T;
  if (const auto *NT = dynamic_cast<const NamedType *>(T.get()))
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        return R;
  return T;
}

} // namespace

bool TypeChecker::checkPattern(
    ExprAST *Pat, const TurfTypePtr &Ty, SourceLocation Where,
    std::unordered_map<std::string, TurfTypePtr> &ArmBindingTypes,
    bool SelfSubject) {
  if (!Pat)
    return true;

  if (dynamic_cast<WildcardPatternExprAST *>(Pat))
    return true;

  // ── A binding: the matched value, copied, under a name ────────────────────
  if (auto *B = dynamic_cast<BindingPatternExprAST *>(Pat)) {
    TurfTypePtr BoundTy = resolveNamed(Ty);
    auto Prev = ArmBindingTypes.find(B->getName());
    if (Prev != ArmBindingTypes.end() && BoundTy && Prev->second &&
        !sameTypeExact(Prev->second.get(), BoundTy.get())) {
      // Two stacked headers feed one body; the body reads ONE symbol, so the
      // name must mean the same type on every path that reaches it.
      InvalidPatternError(
          B->getLoc(),
          "'" + B->getName() + "' is bound as '" + BoundTy->getName() +
              "' here but as '" + Prev->second->getName() +
              "' in another header of the same arm.",
          "Stacked `matches` headers share one body, so a name has to have "
          "one type. Rename one of the bindings, or split the headers into "
          "separate arms.")
          .raise();
      return true;
    }
    if (B->ResolvedVar)
      B->ResolvedVar->Type = BoundTy;
    ArmBindingTypes[B->getName()] = BoundTy;
    return true;
  }

  // ── A struct pattern ──────────────────────────────────────────────────────
  if (auto *S = dynamic_cast<StructPatternExprAST *>(Pat)) {
    TurfTypePtr ValueTy = Ty;

    // Look through a reference. `&T` is never null and its payload is exactly
    // what `p.field` would read, so it is allowed at any depth. `&?T` may be
    // null and a raw `T*` is unchecked: neither is destructured (the first
    // needs a null test, the second an `unsafe` the pattern cannot express).
    if (const auto *PT = dynamic_cast<const TurfPointerType *>(ValueTy.get())) {
      if (PT->isNullableReference()) {
        InvalidPatternError(
            S->getLoc(),
            "can't destructure '" + ValueTy->getName() +
                "': the reference may be null.",
            "Test it first (`if r != null { ... }`) and match the narrowed "
            "reference, or read the fields through `r.field` inside that "
            "branch.")
            .raise();
        return false;
      }
      if (!PT->isReference() && !SelfSubject) {
        InvalidPatternError(
            S->getLoc(),
            "can't destructure the raw pointer '" + ValueTy->getName() + "'.",
            "A pattern only looks through a safe reference ('&T'). Take a "
            "'&T' to the value, or read its fields with '.field'.")
            .raise();
        return false;
      }
      ValueTy = PT->getPointee();
    }

    // Resolve to the registered struct, remembering a generic instance's
    // arguments: they are what turns the field's declared `T` into `int`.
    std::vector<TurfTypePtr> GenericArgs;
    TurfTypePtr Resolved = ValueTy;
    if (const auto *GT = dynamic_cast<const GenericType *>(ValueTy.get())) {
      GenericArgs = GT->getTypeArgs();
      if (GlobalSymbolTable)
        Resolved = GlobalSymbolTable->LookupType(GT->getBaseName());
    } else {
      Resolved = resolveNamed(ValueTy);
    }
    const auto *STy = dynamic_cast<const TurfStructType *>(Resolved.get());
    if (!STy) {
      InvalidPatternError(
          S->getLoc(),
          "this is a struct pattern, but the value it is matched against has "
          "type '" + (Ty ? Ty->getName() : std::string("<unknown>")) +
              "', which is not a struct.",
          "A struct pattern (`Point { x, y }`) destructures a struct value. "
          "Match an int, string, char, bool or enum against a value "
          "(`matches 3:`, `matches Color::Red:`) instead.")
          .raise();
      return false;
    }

    // The pattern must name the struct it is matched against. Both are
    // identities after the Resolver (FDN-003), so a same-named struct from
    // another module is a different type, as it is everywhere else.
    if (S->getTypeSpelling() && S->getTypeSpelling()->getName() != STy->getName()) {
      InvalidPatternError(
          S->getLoc(),
          "this pattern destructures '" + S->getTypeSpelling()->getName() +
              "', but the value being matched is a '" + STy->getName() + "'.",
          "Name the value's own struct: `" + TypeIdentity::bareName(STy->getName()) +
              " { ... }`.")
          .raise();
      return false;
    }

    // A pattern over a generic instance asks for that instance to exist, just
    // like a field access on it would (visitFieldAccess, TYPE-002 S13b):
    // `matches Holder { items: List<int> { data, .. } }` may be the only place
    // a `List<int>` is ever mentioned.
    if (MonoCollector_) {
      if (const auto *GT = dynamic_cast<const GenericType *>(ValueTy.get())) {
        bool AllConcrete = true;
        for (const auto &A : GT->getTypeArgs())
          if (!isFullyConcreteType(A.get())) {
            AllConcrete = false;
            break;
          }
        if (AllConcrete)
          MonoCollector_->record({Monomorphize::InstantiationKind::Struct,
                                  GT->getBaseName(),
                                  TypeIdentity::owningModule(GT->getBaseName()),
                                  GT->getTypeArgs(), S->getLoc()},
                                 S);
      }
    }

    S->MatchedType = Ty;

    bool Irrefutable = true;
    for (auto &F : S->getFieldsMut()) {
      const StructField *Field = nullptr;
      for (const auto &SF : STy->getFields())
        if (SF.Name == F.Name) {
          Field = &SF;
          break;
        }
      if (!Field) {
        std::vector<std::string> FieldNames;
        for (const auto &SF : STy->getFields())
          FieldNames.push_back(SF.Name);
        RankedSuggestions Suggestions;
        Suggestions.Local = SuggestionIndex::suggestVisible(F.Name, FieldNames);
        UnknownStructFieldError(F.Loc, STy->getName(), F.Name, Suggestions).raise();
        return false;
      }
      // A private field is as unreadable through a pattern as through `.`.
      if (!memberVisible(Field->Vis, STy->getName())) {
        raisePrivateField(F.Loc, *STy, *Field);
        return false;
      }

      F.FieldType = instantiatedFieldType(*STy, GenericArgs, Field->Type);
      if (!checkPattern(F.Sub.get(), F.FieldType, F.Loc, ArmBindingTypes))
        Irrefutable = false;
    }

    // Every field is either listed or waved off with `..`. A pattern that
    // silently ignores a field it never names would keep compiling, meaning
    // something different, after the struct gains one.
    if (!S->hasRest()) {
      std::vector<std::string> Missing;
      for (const auto &SF : STy->getFields()) {
        bool Listed = false;
        for (const auto &F : S->getFields())
          if (F.Name == SF.Name) {
            Listed = true;
            break;
          }
        if (!Listed)
          Missing.push_back(SF.Name);
      }
      if (!Missing.empty()) {
        std::string List;
        for (size_t I = 0; I < Missing.size(); ++I)
          List += (I ? ", '" : "'") + Missing[I] + "'";
        const std::string Example =
            TypeIdentity::bareName(STy->getName()) + " { " +
            (S->getFields().empty() ? std::string()
                                    : S->getFields().front().Name + ", ") +
            ".. }";
        InvalidPatternError(
            S->getLoc(),
            "this pattern doesn't mention the field" +
                std::string(Missing.size() > 1 ? "s " : " ") + List + " of '" +
                STy->getName() + "'.",
            "List every field, or end the pattern with `..` to ignore the "
            "ones you don't need: `" + Example + "`.")
            .raise();
        return false;
      }
    }
    return Irrefutable;
  }

  // ── An error-variant pattern: `E::V(sub, ..)` (LEC-004) ───────────────────
  if (auto *EV = dynamic_cast<ErrorVariantPatternExprAST *>(Pat)) {
    TurfTypePtr Subject = resolveNamed(Ty);
    if (!Subject || !(Subject->isAnyError() || Subject->isUserError())) {
      InvalidPatternError(
          EV->getLoc(),
          "this is an error-variant pattern, but the value it is matched "
          "against has type '" + (Ty ? Ty->getName() : std::string("<unknown>")) +
              "', which is not an error.",
          "An error-variant pattern (`IOError::NotFound(path)`) matches an "
          "error value -- the `err` bound by `recover as err run`. Match an "
          "int, string, char, bool or enum against a value instead "
          "(`matches 3:`, `matches Color::Red:`).")
          .raise();
      return false;
    }

    const auto *Head = dynamic_cast<const ScopeAccessExprAST *>(EV->getHead());
    const TurfUserErrorType *ET = nullptr;
    if (Head && GlobalSymbolTable)
      if (auto T = GlobalSymbolTable->LookupType(Head->getLeftName()))
        ET = dynamic_cast<const TurfUserErrorType *>(T.get());
    const ErrorVariantInfo *Info = ET ? ET->getVariantInfo(Head->getRightName()) : nullptr;
    if (!Info) // The Resolver already reported an unknown type or variant.
      return false;

    const std::string Full =
        TypeIdentity::bareName(ET->getName()) + "::" + Head->getRightName();

    // A subject of a declared error type can only ever hold that type's
    // variants; a pattern over another type's is a pattern that can never
    // match. (An open `AnyErrorType` subject can hold anything.)
    if (Subject->isUserError() && Subject->getName() != ET->getName()) {
      InvalidPatternError(
          EV->getLoc(),
          "this pattern is over '" + Full + "', but the value being matched is "
          "a '" + Subject->getName() + "'.",
          "A value of the error type '" + Subject->getName() +
              "' can only be one of its own variants -- name one of those.")
          .raise();
      return false;
    }

    const size_t N = Info->Fields.size();
    const size_t Listed = EV->getSubs().size();

    if (!EV->hasParens()) {
      // Bare `E::V:` matches a variant that has nothing to destructure. One
      // that has fields must say what happens to them, so the pattern cannot
      // keep compiling, meaning something different, after a field is added.
      if (N > 0) {
        InvalidPatternError(
            EV->getLoc(),
            "'" + Full + "' has " + std::to_string(N) + " field" +
                (N == 1 ? "" : "s") + ", so a pattern for it must say what to "
                "do with " + (N == 1 ? "it" : "them") + ".",
            "Write `" + Full + "(..)` to ignore the payload, or list the fields: `" +
                Full + "(" + Info->Fields[0].Name + (N > 1 ? ", ..)" : ")") + "`.")
            .raise();
        return false;
      }
    } else {
      if (Listed == 0 && !EV->hasRest()) {
        InvalidPatternError(
            EV->getLoc(), "'" + Full + "' has empty parentheses.",
            N == 0 ? "'" + Full + "' has no fields: write it without parentheses, `" +
                         Full + ":`."
                   : "List the fields (`" + Full + "(" + Info->Fields[0].Name +
                         (N > 1 ? ", ..)" : ")") + "`) or write `" + Full +
                         "(..)` to ignore them.")
            .raise();
        return false;
      }
      if (Listed > N) {
        InvalidPatternError(
            EV->getLoc(),
            "this pattern lists " + std::to_string(Listed) + " field" +
                (Listed == 1 ? "" : "s") + ", but '" + Full + "' declares " +
                std::to_string(N) + ".",
            N == 0 ? "'" + Full + "' has no fields; match it as `" + Full + ":`."
                   : "Match one sub-pattern per declared field, in order.")
            .raise();
        return false;
      }
      if (Listed < N && !EV->hasRest()) {
        std::string Missing;
        for (size_t I = Listed; I < N; ++I)
          Missing += (I > Listed ? ", '" : "'") + Info->Fields[I].Name + "'";
        InvalidPatternError(
            EV->getLoc(),
            "this pattern doesn't mention the field" +
                std::string(N - Listed > 1 ? "s " : " ") + Missing + " of '" + Full +
                "'.",
            "List every field, or end the pattern with `..` to ignore the ones "
            "you don't need: `" + Full + "(" + (Listed ? "... , " : "") + "..)`.")
            .raise();
        return false;
      }
    }

    EV->FieldTypes.clear();
    bool AllIrrefutable = true;
    for (size_t I = 0; I < Listed; ++I) {
      EV->FieldTypes.push_back(Info->Fields[I].Type);
      if (!checkPattern(EV->getSubsMut()[I].get(), Info->Fields[I].Type, EV->getLoc(),
                        ArmBindingTypes))
        AllIrrefutable = false;
    }
    // Fields a trailing `..` ignores are irrefutable by construction.
    EV->FieldsIrrefutable = AllIrrefutable;

    // Never a catch-all: it covers one variant, and an error value is an open
    // union. Coverage of a declared error type's variants is visitCompare()'s.
    return false;
  }

  // ── A value sub-pattern: a literal or an enum variant ─────────────────────
  //
  // (A value pattern at the top of a header never reaches here; it is the
  // original equality pattern and visitCompare() checks it itself.)
  if (!isValuePatternShape(Pat)) {
    const bool IsDouble = [&] {
      const auto *N = dynamic_cast<const NumberExprAST *>(Pat);
      return N && !N->isInteger();
    }();
    InvalidPatternError(
        Where,
        IsDouble ? "a 'double' literal can't be a pattern."
                 : "this field's pattern must be a literal, an enum variant, "
                   "'_', a name to bind, or a nested struct pattern.",
        IsDouble
            ? "Exact equality on floating point is almost never what you "
              "mean. Bind the field and test it in a guard: "
              "`matches P { x } if x > 0.5:`."
            : "To compare a field with a variable or any other expression, "
              "bind it and use a guard: `matches P { x } if x == limit:`.")
        .raise();
    return false;
  }

  check(Pat);
  TurfTypePtr PatTy = getType(Pat);
  TurfTypePtr FieldTy = resolveNamed(Ty);

  if (!isComparableFieldType(FieldTy.get())) {
    InvalidPatternError(
        Where,
        "a value can't be matched against a field of type '" +
            (FieldTy ? FieldTy->getName() : std::string("<unknown>")) + "'.",
        "Literal and enum-variant patterns test an int, int32, char, bool, "
        "string or enum field. To test anything else, bind the field and use "
        "a guard.")
        .raise();
    return false;
  }
  if (PatTy && FieldTy && !typesCompatible(FieldTy.get(), PatTy.get())) {
    InvalidPatternError(
        Where,
        "the field has type '" + FieldTy->getName() +
            "' but this pattern has type '" + PatTy->getName() + "'.",
        "A literal or variant pattern must have the field's own type.")
        .raise();
    return false;
  }
  return false; // a value pattern is always refutable
}
