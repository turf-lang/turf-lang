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
 *  - Three new list-type helpers are provided:
 *      isListType()        — true for ListType or GenericType("list",…)
 *      getListElementType()— extract T from list<T>
 *      makeListType()      — construct a list<T> without LLVM
 *
 * Acceptance criterion (Issue #3):
 *   makeListType(TurfInt())->getName() == "list<int>"   ✓
 *   makeListType(TurfInt())->isList()  == true           ✓
 *   No LLVM header is included in this translation unit. ✓
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

// ─── List helpers ─────────────────────────────────────────────────────────────

/**
 * @brief True if @p T is a `ListType` or a legacy `GenericType` whose base
 * name is "list".
 *
 * The dual check allows code that was written before the ListType subclass
 * existed (and used GenericType("list", …) directly) to keep working.
 */
bool isListType(const TurfType *T) {
  if (!T) return false;
  // Fast path: dedicated subclass
  if (dynamic_cast<const ListType *>(T)) return true;
  // Legacy path: GenericType with base "list"
  if (auto *G = dynamic_cast<const GenericType *>(T))
    return G->getBaseName() == "list";
  return false;
}

/**
 * @brief Return the element type of a list, or nullptr if @p T is not a list.
 */
const TurfType *getListElementType(const TurfType *T) {
  if (!T) return nullptr;
  // Dedicated ListType — use its own accessor
  if (auto *L = dynamic_cast<const ListType *>(T))
    return L->getElementType();
  // Legacy GenericType("list", …)
  if (auto *G = dynamic_cast<const GenericType *>(T))
    if (G->getBaseName() == "list" && !G->getTypeArgs().empty())
      return G->getTypeArg(0);
  return nullptr;
}

/**
 * @brief Construct a `list<ElemType>` frontend type.
 *
 * Returns a `ListType` which:
 *   - Reports isList()  == true
 *   - Reports isGeneric() == true (via base class)
 *   - getName()  == "list<T>"
 *   - getMangledName() == "list_T"
 *
 * No LLVM headers are required.
 *
 * @param ElemType The element type (must not be null).
 * @return A shared_ptr<ListType>.
 */
TurfTypePtr makeListType(TurfTypePtr ElemType) {
  return std::make_shared<ListType>(std::move(ElemType));
}
