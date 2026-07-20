/**
 * @file Types.cpp
 * @brief TurfLang frontend type system — Issue #3 implementation.
 *
 * @details
 * Implements the factory singletons and helper functions declared in Types.h.
 *
 * Key changes vs. the previous version
 * ─────────────────────────────────────
 *  - TurfInt(), TurfDouble(), TurfBool(), TurfString(), TurfVoid() now return
 *    instances of the dedicated subclasses (IntType, DoubleType, …) instead of
 *    generic NamedType instances.  This enables dynamic_cast and the typed
 *    predicates (isInt(), isDouble(), …) to work correctly throughout the
 *    compiler without any string comparisons.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 2.0.0
 * @date 2026-05-29
 */

#include "Types.h"

// ─── Primitive singleton accessors ────────────────────────────────────────────
//
// Each function returns a stable shared_ptr whose lifetime is the process.
// Local-static initialisation is thread-safe in C++11+.
//
// IMPORTANT: The objects are now instances of the dedicated subclasses so that
//   dynamic_cast<const IntType*>(TurfInt().get()) != nullptr   // true
//   TurfInt()->isInt()                                         // true

TurfTypePtr TurfInt() {
  static TurfTypePtr T = std::make_shared<IntType>();
  return T;
}

TurfTypePtr TurfDouble() {
  static TurfTypePtr T = std::make_shared<DoubleType>();
  return T;
}

TurfTypePtr TurfBool() {
  static TurfTypePtr T = std::make_shared<BoolType>();
  return T;
}

TurfTypePtr TurfString() {
  static TurfTypePtr T = std::make_shared<StringType>();
  return T;
}

TurfTypePtr TurfVoid() {
  static TurfTypePtr T = std::make_shared<VoidType>();
  return T;
}

TurfTypePtr TurfNull() {
  static TurfTypePtr T = std::make_shared<TurfNullType>();
  return T;
}

TurfTypePtr TurfPtr() {
  static TurfTypePtr T = std::make_shared<OpaquePointerType>();
  return T;
}

// ─── Array helpers ────────────────────────────────────────────────────────────

bool isArrayType(const TurfType *T) {
  return dynamic_cast<const ArrayType *>(T) != nullptr;
}

const TurfType *getArrayElementType(const TurfType *T) {
  auto *A = dynamic_cast<const ArrayType *>(T);
  return A ? A->getElementType() : nullptr;
}

/**
 * @brief Constructs a frontend representation of an ArrayType.
 *
 * @param ElemType The type of the elements contained in the array.
 * @param Size     The fixed size of the array (-1 if unspecified/dynamic).
 * @return TurfTypePtr A shared pointer to the newly created ArrayType.
 */
TurfTypePtr makeArrayType(TurfTypePtr ElemType, int Size) {
  return std::make_shared<ArrayType>(std::move(ElemType), Size);
}

// ─── Type coercion helper ───────────────────────────────────────────────────

bool typesCompatible(const TurfType *From, const TurfType *To) {
  if (!From || !To)
    return false;

  // Exact match
  if (From->getName() == To->getName())
    return true;

  // Type Erasure: Generic Type Parameters are compatible with anything
  if (dynamic_cast<const TurfGenericParamType *>(From) ||
      dynamic_cast<const TurfGenericParamType *>(To))
    return true;

  // Rule 1: null is only compatible with pointer-like types (ptr,
  // void*, T*). It must NEVER be compatible with plain numerics, bool, or string.
  auto isPointerLikeStrict = [](const TurfType *T) {
    return T->isPointer() ||
           T->isNamed("ptr") || T->isNamed("void*");
  };
  if (From->isNull())
    return isPointerLikeStrict(To);
  if (To->isNull())
    return isPointerLikeStrict(From);

  // Rule 2: ptr / void* mutually coercible with typed pointers (T*) only.
  // Deliberately NOT extended to `string` — string and T* remain distinct.
  auto isOpaquePtr = [](const TurfType *T) {
    return T->isNamed("ptr") || T->isNamed("void*");
  };
  if (isOpaquePtr(From) && To->isPointer())
    return true;
  if (isOpaquePtr(To) && From->isPointer())
    return true;

  // void and string are never compatible with anything else past this point
  if (From->isVoid() || To->isVoid())
    return false;
  if (From->isString() || To->isString())
    return false;

  // Enums are strictly typed and do not coerce implicitly to ints or other enums
  if (From->isEnum() || To->isEnum())
    return false;

  // Rule 4: bool is strictly isolated — never coerces to/from numerics.
  if (From->isBool() || To->isBool())
    return false;

  // Rule 3: numeric widening/narrowing.
  // int -> double (widening): allowed.
  // double -> int (narrowing): rejected, requires explicit cast.
  if (From->isNumeric() && To->isNumeric()) {
    if (From->isInt() && To->isDouble())
      return true;
    if (From->isDouble() && To->isInt())
      return false;
    // Same-kind numerics not already caught by the exact-name check above
    // (shouldn't normally be reached, but keep as a safe explicit case).
    return false;
  }

  // Anything else (e.g. struct-to-struct name mismatch, pointer-to-struct,
  // etc.) is not implicitly compatible.
  return false;
}

// ─── TurfEnumType ────────────────────────────────────────────────────────────

TurfEnumType::TurfEnumType(std::string Name,
                           std::vector<std::string> Variants,
                           std::shared_ptr<TurfType> UnderlyingType)
    : Name_(std::move(Name)), Variants_(std::move(Variants)),
      UnderlyingType_(UnderlyingType ? std::move(UnderlyingType) : TurfInt()) {
    for (size_t i = 0; i < Variants_.size(); ++i) {
        VariantValues_[Variants_[i]] = static_cast<int>(i);
    }
}

int TurfEnumType::getVariantValue(const std::string& V) const {
    auto it = VariantValues_.find(V);
    if (it != VariantValues_.end()) {
        return it->second;
    }
    return -1;
}

bool TurfEnumType::hasVariant(const std::string& V) const {
    return VariantValues_.find(V) != VariantValues_.end();
}
