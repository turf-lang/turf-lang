#include "TypeWalk.h"

#include <algorithm>
#include <unordered_set>

namespace TypeWalk {

namespace {
// Exactly `isOrWrapsGenericParam()`'s own historical set (TypeCheckerDecl.cpp)
// -- preserved verbatim so `ParamScope::ThroughWrappers`/`ThroughGenericArgs`
// classify a `NamedType` identically to the function they replace.
const std::unordered_set<std::string> &knownPrimitiveNames() {
  static const std::unordered_set<std::string> Names = {
      "int", "int32", "double", "bool", "char",
      "string", "void", "null", "ptr", "void*"};
  return Names;
}
} // namespace

bool containsOpenParam(const TurfType *Ty, ParamScope Scope,
                       const NamedTypeIsKnownFn &IsKnownNamedType) {
  if (!Ty)
    return false;

  if (dynamic_cast<const TurfGenericParamType *>(Ty))
    return true;

  if (Scope == ParamScope::Bare)
    return false;

  // A bare NamedType whose name is neither a known primitive nor found by
  // the caller's own registry lookup can only be an open type parameter,
  // spelled the pre-substitution way -- see this function's own doc comment
  // (TypeWalk.h) and OVH-006's change record for the full mechanism. No
  // fallback offered (IsKnownNamedType unset) means this caller has nothing
  // to resolve a NamedType against, so it is left unclassified (not open) --
  // matches every ThroughWrappers/ThroughGenericArgs call site today, which
  // always supplies one.
  if (const auto *NT = dynamic_cast<const NamedType *>(Ty)) {
    if (knownPrimitiveNames().count(NT->getName()))
      return false;
    if (!IsKnownNamedType)
      return false;
    return !IsKnownNamedType(NT->getName());
  }

  // Catches TurfReferenceType (&T) and TurfNullableReferenceType (&?T) too
  // -- both ARE-A TurfPointerType (Types.h) -- exactly like the functions
  // this replaces: neither ever needed to reconstruct the pointer kind, only
  // to look through it, so one catch-all check is correct here (unlike
  // `substitute()` below, which must tell the three kinds apart to rebuild
  // the right one).
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty))
    return containsOpenParam(PT->getPointee().get(), Scope, IsKnownNamedType);

  if (const auto *AT = dynamic_cast<const ArrayType *>(Ty))
    return containsOpenParam(AT->getElementType(), Scope, IsKnownNamedType);

  if (Scope == ParamScope::ThroughGenericArgs) {
    if (const auto *GT = dynamic_cast<const GenericType *>(Ty)) {
      for (const auto &Arg : GT->getTypeArgs())
        if (containsOpenParam(Arg.get(), Scope, IsKnownNamedType))
          return true;
    }
  }

  return false;
}

TurfTypePtr substitute(const TurfTypePtr &Ty, const SubstMap &Subst) {
  if (!Ty || Subst.empty())
    return Ty;

  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(Ty.get())) {
    auto It = Subst.find(GP->getName());
    return It != Subst.end() ? It->second : Ty;
  }
  // A generic struct's own field types (and a not-yet-substituted
  // declaration's parameter/return types) are matched as NamedType too --
  // see this function's own doc comment (TypeWalk.h) for why both
  // representations must be handled in one pass.
  if (const auto *NT = dynamic_cast<const NamedType *>(Ty.get())) {
    auto It = Subst.find(NT->getName());
    return It != Subst.end() ? It->second : Ty;
  }
  // Checked BEFORE the plain TurfPointerType branch below: TurfReferenceType
  // (&T) IS-A TurfPointerType, so the more general check would match it too
  // -- without this earlier, more specific check, substituting a generic
  // `&T` parameter/return/field silently reconstructed a plain
  // TurfPointerType, turning every `&T` into `T*` (confirmed live pre-
  // unification, both original functions' own doc comments).
  if (const auto *RT = dynamic_cast<const TurfReferenceType *>(Ty.get())) {
    TurfTypePtr NewPointee = substitute(RT->getPointee(), Subst);
    return NewPointee == RT->getPointee()
              ? Ty
              : std::make_shared<TurfReferenceType>(NewPointee);
  }
  // Same bug class, same fix, for `&?T` -- TurfNullableReferenceType is a
  // SIBLING of TurfReferenceType (both direct TurfPointerType subclasses,
  // Types.h), so the check above does not catch it either.
  if (const auto *NRT = dynamic_cast<const TurfNullableReferenceType *>(Ty.get())) {
    TurfTypePtr NewPointee = substitute(NRT->getPointee(), Subst);
    return NewPointee == NRT->getPointee()
              ? Ty
              : std::make_shared<TurfNullableReferenceType>(NewPointee);
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Ty.get())) {
    TurfTypePtr NewPointee = substitute(PT->getPointee(), Subst);
    return NewPointee == PT->getPointee()
              ? Ty
              : std::make_shared<TurfPointerType>(NewPointee);
  }
  if (const auto *AT = dynamic_cast<const ArrayType *>(Ty.get())) {
    TurfTypePtr NewElem = substitute(AT->getElementTypePtr(), Subst);
    return NewElem == AT->getElementTypePtr()
              ? Ty
              : makeArrayType(NewElem, AT->getSize());
  }
  if (const auto *GT = dynamic_cast<const GenericType *>(Ty.get())) {
    std::vector<TurfTypePtr> NewArgs;
    bool Changed = false;
    for (const auto &Arg : GT->getTypeArgs()) {
      TurfTypePtr NewArg = substitute(Arg, Subst);
      Changed |= (NewArg != Arg);
      NewArgs.push_back(std::move(NewArg));
    }
    return Changed ? std::make_shared<GenericType>(GT->getBaseName(),
                                                    std::move(NewArgs))
                   : Ty;
  }
  // Not one of the seven original functions' shared cases: this compiler's
  // `Monomorphize::substituteConcreteTypes()` already had it,
  // `Resolver::substituteTypeParams()` never did -- a genuine, silent
  // incompleteness of exactly the kind SMP-002 exists to close (see the
  // change record's *Removed assumptions*). A generic declaration whose
  // return or parameter type is `!T` now gets its `T` correctly marked as
  // an open parameter at declaration time too, not just at each
  // instantiation site.
  if (const auto *EU = dynamic_cast<const TurfErrorUnionType *>(Ty.get())) {
    TurfTypePtr NewSuccess = substitute(EU->getSuccessTypePtr(), Subst);
    return NewSuccess == EU->getSuccessTypePtr()
              ? Ty
              : std::make_shared<TurfErrorUnionType>(NewSuccess);
  }
  // A callback parameter's own signature (`cmp: fn(T, T): int`) -- each
  // parameter position and the return type recurse the same way every
  // other wrapper case above does.
  if (const auto *FT = dynamic_cast<const FuncType *>(Ty.get())) {
    std::vector<TurfTypePtr> NewParams;
    bool Changed = false;
    for (const auto &P : FT->getParamTypes()) {
      TurfTypePtr NewP = substitute(P, Subst);
      Changed |= (NewP != P);
      NewParams.push_back(std::move(NewP));
    }
    TurfTypePtr NewRet = substitute(FT->getReturnType(), Subst);
    Changed |= (NewRet != FT->getReturnType());
    return Changed ? std::make_shared<FuncType>(std::move(NewRet),
                                                std::move(NewParams))
                   : Ty;
  }
  return Ty;
}

void unify(const TurfTypePtr &Pattern, const TurfTypePtr &Concrete,
          SubstMap &Subst) {
  if (!Pattern || !Concrete)
    return;

  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(Pattern.get())) {
    Subst.emplace(GP->getName(), Concrete);
    return;
  }
  if (const auto *PPtr = dynamic_cast<const TurfPointerType *>(Pattern.get())) {
    if (const auto *APtr = dynamic_cast<const TurfPointerType *>(Concrete.get()))
      unify(PPtr->getPointee(), APtr->getPointee(), Subst);
    return;
  }
  if (isArrayType(Pattern.get()) && isArrayType(Concrete.get())) {
    const auto *PArr = dynamic_cast<const ArrayType *>(Pattern.get());
    const auto *AArr = dynamic_cast<const ArrayType *>(Concrete.get());
    if (PArr && AArr)
      unify(PArr->getElementTypePtr(), AArr->getElementTypePtr(), Subst);
    return;
  }
  if (const auto *PGen = dynamic_cast<const GenericType *>(Pattern.get())) {
    if (const auto *AGen = dynamic_cast<const GenericType *>(Concrete.get())) {
      const auto &PArgs = PGen->getTypeArgs();
      const auto &AArgs = AGen->getTypeArgs();
      for (size_t i = 0; i < PArgs.size() && i < AArgs.size(); ++i)
        unify(PArgs[i], AArgs[i], Subst);
    }
    return;
  }
  // A callback parameter's own signature (`cmp: fn(T, T): int`) unified
  // against the caller's concrete function-typed argument.
  if (const auto *PFunc = dynamic_cast<const FuncType *>(Pattern.get())) {
    if (const auto *AFunc = dynamic_cast<const FuncType *>(Concrete.get())) {
      const auto &PParams = PFunc->getParamTypes();
      const auto &AParams = AFunc->getParamTypes();
      for (size_t i = 0; i < PParams.size() && i < AParams.size(); ++i)
        unify(PParams[i], AParams[i], Subst);
      unify(PFunc->getReturnType(), AFunc->getReturnType(), Subst);
    }
    return;
  }
}

void collectParamOrder(const TurfTypePtr &Pattern,
                       std::vector<std::string> &Order) {
  if (!Pattern)
    return;
  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(Pattern.get())) {
    if (std::find(Order.begin(), Order.end(), GP->getName()) == Order.end())
      Order.push_back(GP->getName());
    return;
  }
  if (const auto *PPtr = dynamic_cast<const TurfPointerType *>(Pattern.get())) {
    collectParamOrder(PPtr->getPointee(), Order);
    return;
  }
  if (isArrayType(Pattern.get())) {
    if (const auto *Arr = dynamic_cast<const ArrayType *>(Pattern.get()))
      collectParamOrder(Arr->getElementTypePtr(), Order);
    return;
  }
  if (const auto *PGen = dynamic_cast<const GenericType *>(Pattern.get())) {
    for (const auto &A : PGen->getTypeArgs())
      collectParamOrder(A, Order);
    return;
  }
  if (const auto *PFunc = dynamic_cast<const FuncType *>(Pattern.get())) {
    for (const auto &P : PFunc->getParamTypes())
      collectParamOrder(P, Order);
    collectParamOrder(PFunc->getReturnType(), Order);
    return;
  }
}

void collectOrderedFieldSubstitution(const TurfTypePtr &Pattern,
                                     const std::vector<TurfTypePtr> &TypeArgs,
                                     SubstMap &Subst,
                                     const NamedTypeIsKnownFn &IsKnownNamedType) {
  if (!Pattern || Subst.size() >= TypeArgs.size())
    return;
  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(Pattern.get())) {
    if (!Subst.count(GP->getName()))
      Subst.emplace(GP->getName(), TypeArgs[Subst.size()]);
    return;
  }
  if (const auto *NT = dynamic_cast<const NamedType *>(Pattern.get())) {
    if (knownPrimitiveNames().count(NT->getName()))
      return;
    bool KnownElsewhere = IsKnownNamedType && IsKnownNamedType(NT->getName());
    if (!KnownElsewhere && !Subst.count(NT->getName()))
      Subst.emplace(NT->getName(), TypeArgs[Subst.size()]);
    return;
  }
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(Pattern.get())) {
    collectOrderedFieldSubstitution(PT->getPointee(), TypeArgs, Subst, IsKnownNamedType);
    return;
  }
  if (isArrayType(Pattern.get())) {
    if (const auto *AT = dynamic_cast<const ArrayType *>(Pattern.get()))
      collectOrderedFieldSubstitution(AT->getElementTypePtr(), TypeArgs, Subst, IsKnownNamedType);
    return;
  }
  if (const auto *GT = dynamic_cast<const GenericType *>(Pattern.get())) {
    // TYPE-002 (S15/D4): a field declared as ANOTHER generic instantiation
    // at the same type parameter (`struct Outer<T> { inner: Inner<T> }`) --
    // recurse into its own type-argument list the same way a pointer/array
    // wrapper already does above.
    for (const auto &Arg : GT->getTypeArgs())
      collectOrderedFieldSubstitution(Arg, TypeArgs, Subst, IsKnownNamedType);
    return;
  }
  if (const auto *FT = dynamic_cast<const FuncType *>(Pattern.get())) {
    for (const auto &P : FT->getParamTypes())
      collectOrderedFieldSubstitution(P, TypeArgs, Subst, IsKnownNamedType);
    collectOrderedFieldSubstitution(FT->getReturnType(), TypeArgs, Subst, IsKnownNamedType);
    return;
  }
}

} // namespace TypeWalk
