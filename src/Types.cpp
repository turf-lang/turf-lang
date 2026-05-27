/**
 * @file Types.cpp
 * @brief TurfLang frontend type system.
 * @details Implements the primitives, generics (lists), and array types used
 *          by the frontend AST. Provides factory functions and singleton access
 *          for builtin types to ensure pointer-equality where possible.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Types.h"

// Singleton primitive types
// Each returns a stable shared_ptr to the same object for the lifetime of
// the process. Using local static is thread-safe in C++11 and later.

TurfTypePtr TurfInt() {
  static TurfTypePtr T = std::make_shared<NamedType>("int");
  return T;
}
TurfTypePtr TurfDouble() {
  static TurfTypePtr T = std::make_shared<NamedType>("double");
  return T;
}
TurfTypePtr TurfBool() {
  static TurfTypePtr T = std::make_shared<NamedType>("bool");
  return T;
}
TurfTypePtr TurfString() {
  static TurfTypePtr T = std::make_shared<NamedType>("string");
  return T;
}
TurfTypePtr TurfVoid() {
  static TurfTypePtr T = std::make_shared<NamedType>("void");
  return T;
}
TurfTypePtr TurfNull() {
  static TurfTypePtr T = std::make_shared<TurfNullType>();
  return T;
}

// Helper predicates

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
 * @param Size The fixed size of the array (-1 if unspecified/dynamic).
 * @return TurfTypePtr A shared pointer to the newly created ArrayType.
 */
TurfTypePtr makeArrayType(TurfTypePtr ElemType, int Size) {
  return std::make_shared<ArrayType>(std::move(ElemType), Size);
}
