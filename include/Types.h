#ifndef TYPES_H
#define TYPES_H

#include <memory>
#include <string>
#include <vector>

// ────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ────────────────────────────────────────────────────────────────────────────
class TurfType;
using TurfTypePtr = std::shared_ptr<TurfType>;

// ────────────────────────────────────────────────────────────────────────────
// Base class — all types are TurfType subclasses
// ────────────────────────────────────────────────────────────────────────────
class TurfType {
public:
  virtual ~TurfType() = default;
  virtual std::string getName() const = 0;

  // Convenience predicates — override in subclasses as needed
  virtual bool isNamed(const std::string &N) const { return false; }
  virtual bool isGeneric() const { return false; }
  virtual bool isArray() const { return false; }
  virtual bool isVoid() const { return false; }

  // Structural equivalence: two types are equivalent if they have the same name
  virtual bool isEquivalent(const TurfType *Other) const {
    return Other && getName() == Other->getName();
  }
};

// ────────────────────────────────────────────────────────────────────────────
// NamedType — int, double, bool, string, void, and future user-defined types
// ────────────────────────────────────────────────────────────────────────────
class NamedType : public TurfType {
  std::string Name;

public:
  explicit NamedType(std::string N) : Name(std::move(N)) {}
  std::string getName() const override { return Name; }
  bool isNamed(const std::string &N) const override { return Name == N; }
  bool isVoid() const override { return Name == "void"; }
};

// ────────────────────────────────────────────────────────────────────────────
// ArrayType — int[], double[], bool[], string[]
// ────────────────────────────────────────────────────────────────────────────
class ArrayType : public TurfType {
  TurfTypePtr ElementType;
  int Size; // -1 for unknown/dynamic, >0 for fixed compile-time size

public:
  ArrayType(TurfTypePtr Elem, int Sz = -1)
      : ElementType(std::move(Elem)), Size(Sz) {}
  std::string getName() const override { return ElementType->getName() + "[]"; }
  bool isArray() const override { return true; }
  const TurfType *getElementType() const { return ElementType.get(); }
  TurfTypePtr getElementTypePtr() const { return ElementType; }
  int getSize() const { return Size; }
};

// ────────────────────────────────────────────────────────────────────────────
// GenericType — list<int>, list<string>, future map<K,V>, etc.
// This is the core of Monomorphization: the element type is a full TurfType,
// so list<Player> works as soon as Player is a registered type.
// ────────────────────────────────────────────────────────────────────────────
class GenericType : public TurfType {
  std::string BaseName;          // "list", "map", etc.
  std::vector<TurfTypePtr> TypeArgs; // [int], [string, int], etc.

public:
  GenericType(std::string Base, std::vector<TurfTypePtr> Args)
      : BaseName(std::move(Base)), TypeArgs(std::move(Args)) {}

  std::string getName() const override {
    std::string S = BaseName + "<";
    for (size_t i = 0; i < TypeArgs.size(); ++i) {
      if (i)
        S += ", ";
      S += TypeArgs[i]->getName();
    }
    return S + ">";
  }

  bool isGeneric() const override { return true; }
  const std::string &getBaseName() const { return BaseName; }
  const std::vector<TurfTypePtr> &getTypeArgs() const { return TypeArgs; }
  const TurfType *getTypeArg(size_t I) const { return TypeArgs[I].get(); }
  TurfTypePtr getTypeArgPtr(size_t I) const { return TypeArgs[I]; }
};

// ────────────────────────────────────────────────────────────────────────────
// Singleton accessors for built-in primitive types
// Defined in src/Types.cpp so callers never construct NamedType directly.
// ────────────────────────────────────────────────────────────────────────────
TurfTypePtr TurfInt();
TurfTypePtr TurfDouble();
TurfTypePtr TurfBool();
TurfTypePtr TurfString();
TurfTypePtr TurfVoid();

// ────────────────────────────────────────────────────────────────────────────
// Helper predicates (defined in src/Types.cpp)
// ────────────────────────────────────────────────────────────────────────────
bool isListType(const TurfType *T);  // GenericType with BaseName "list"
bool isArrayType(const TurfType *T); // ArrayType

const TurfType *getListElementType(const TurfType *T);  // inner T of list<T>
const TurfType *getArrayElementType(const TurfType *T); // inner T of T[]

TurfTypePtr makeListType(TurfTypePtr ElemType);
TurfTypePtr makeArrayType(TurfTypePtr ElemType, int Size = -1);

// ────────────────────────────────────────────────────────────────────────────
// Human-readable diagnostic name
// ────────────────────────────────────────────────────────────────────────────
inline std::string turfTypeName(const TurfType *T) {
  return T ? T->getName() : "unknown";
}

#endif
