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
  if (From->getName() == To->getName())
    return true;
  // Generic type parameters match anything (type-erasure)
  if (dynamic_cast<const TurfGenericParamType *>(From) ||
      dynamic_cast<const TurfGenericParamType *>(To))
    return true;
  // Issue #15 fix: `ptr` (OpaquePointerType) coercible to/from any typed pointer.
  auto isOpaquePtr = [](const TurfType *T) {
    return T->isNamed("ptr") || T->isNamed("void*");
  };
  if (isOpaquePtr(From) && To->isPointer())
    return true;
  if (isOpaquePtr(To) && From->isPointer())
    return true;
  // void and string are never coercible
  if (From->isVoid() || To->isVoid())
    return false;
  if (From->isNamed("string") || To->isNamed("string"))
    return false;
  // Issue #10 fix: bool is a distinct type -- must NOT silently coerce to int/double.
  if (From->isNamed("bool") || To->isNamed("bool"))
    return false;
  // Remaining numerics (int <-> double) are mutually coercible
  return true;
}
