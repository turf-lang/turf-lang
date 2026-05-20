#include "Types.h"

// ── Singleton primitive types ──────────────────────────────────────────────
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

// ── Helper predicates ──────────────────────────────────────────────────────

bool isListType(const TurfType *T) {
  auto *G = dynamic_cast<const GenericType *>(T);
  return G && G->getBaseName() == "list";
}

bool isArrayType(const TurfType *T) {
  return dynamic_cast<const ArrayType *>(T) != nullptr;
}

const TurfType *getListElementType(const TurfType *T) {
  auto *G = dynamic_cast<const GenericType *>(T);
  return G ? G->getTypeArg(0) : nullptr;
}

const TurfType *getArrayElementType(const TurfType *T) {
  auto *A = dynamic_cast<const ArrayType *>(T);
  return A ? A->getElementType() : nullptr;
}

// ── Factory helpers ────────────────────────────────────────────────────────

TurfTypePtr makeListType(TurfTypePtr ElemType) {
  std::vector<TurfTypePtr> Args = {std::move(ElemType)};
  return std::make_shared<GenericType>("list", std::move(Args));
}

TurfTypePtr makeArrayType(TurfTypePtr ElemType, int Size) {
  return std::make_shared<ArrayType>(std::move(ElemType), Size);
}
