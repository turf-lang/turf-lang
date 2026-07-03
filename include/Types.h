/**
 * @file Types.h
 * @brief Turflang abstract frontend type system — Issue #3 implementation.
 *
 * This header defines a fully polymorphic, LLVM-free type hierarchy for
 * the Turflang compiler frontend.  Every type in a Turf program is
 * represented as a subclass of TurfType so that passes (Resolver,
 * TypeChecker) can reason about types without touching llvm::Type*.
 *
 * Type hierarchy overview
 * ───────────────────────
 *  TurfType                   — abstract base
 *   ├─ IntType                — the `int` primitive
 *   ├─ DoubleType             — the `double` primitive
 *   ├─ BoolType               — the `bool` primitive
 *   ├─ StringType             — the `string` primitive
 *   ├─ VoidType               — the `void` return type
 *   ├─ TurfNullType           — the `null` literal type
 *   ├─ OpaquePointerType      — the `ptr` FFI opaque pointer (i8* in LLVM IR)
 *   ├─ NamedType              — user-defined struct names and legacy fallback
 *   ├─ ArrayType              — T[]  /  T[N]
 *   ├─ TurfPointerType        — T*   (pass-by-reference / self)
 *   ├─ TurfGenericParamType   — open generic parameter (e.g. T in fn<T>)
 *   ├─ GenericType            — closed generic instantiation (e.g. map<K,V>)
 *   └─ TurfStructType         — user-defined aggregate type
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

#ifndef TYPES_H
#define TYPES_H

#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
class TurfType;
using TurfTypePtr = std::shared_ptr<TurfType>;

// ─── Base class ───────────────────────────────────────────────────────────────

/**
 * @brief Abstract base for every type in the Turf type system.
 *
 * All passes work exclusively with TurfType* / TurfTypePtr; no LLVM header is
 * required by this file.  Virtual dispatch enables the TypeChecker to branch
 * on kind (e.g. isInt(), isList()) without raw string comparisons.
 */
class TurfType {
public:
  virtual ~TurfType() = default;

  /// Human-readable name as written in source code (e.g. "int", "list<int>").
  virtual std::string getName() const = 0;

  /// Mangled name used for LLVM symbol construction (e.g. "list_int").
  virtual std::string getMangledName() const { return getName(); }

  // ── Kind predicates ──────────────────────────────────────────────────────
  // Each primitive type overrides exactly one of these to return `true`.
  // Callers should prefer these over string comparisons.

  virtual bool isInt()    const { return false; }
  virtual bool isDouble() const { return false; }
  virtual bool isBool()   const { return false; }
  virtual bool isString() const { return false; }
  virtual bool isVoid()   const { return false; }
  virtual bool isNull()   const { return false; }

  /// True for any numeric type (int, double, bool).
  virtual bool isNumeric() const { return isInt() || isDouble() || isBool(); }

  // ── Structural predicates ────────────────────────────────────────────────

  /// True if this type is a `GenericType` (closed instantiation like list<int>).
  virtual bool isGeneric() const { return false; }

  /// True if this type is an `ArrayType` (fixed/dynamic array T[]).
  virtual bool isArray() const { return false; }

  /// True if this type is a `TurfPointerType`.
  virtual bool isPointer() const { return false; }

  /// True if this type is a `TurfStructType`.
  virtual bool isStruct() const { return false; }

  /// True if this type is a `TurfGenericParamType` (open type parameter).
  virtual bool isGenericParam() const { return false; }

  // ── Legacy predicate (backward-compatible) ───────────────────────────────

  /**
   * @brief True when getName() == N.
   *
   * Kept for backward compatibility with existing Codegen code.  New code
   * should prefer the typed predicates (isInt(), isString(), …) instead.
   */
  virtual bool isNamed(const std::string &N) const { return getName() == N; }

  // ── Structural equivalence ───────────────────────────────────────────────

  /// Two types are equivalent when they represent the same source-level type.
  virtual bool isEquivalent(const TurfType *Other) const {
    return Other && getName() == Other->getName();
  }
};

// ─── Concrete primitive types ─────────────────────────────────────────────────

/**
 * @brief The `int` primitive type.
 *
 * Maps to llvm::Type::getInt64Ty in Codegen.  Represented as a dedicated class
 * so that `dynamic_cast<const IntType*>` works without string comparison.
 */
class IntType : public TurfType {
public:
  std::string getName() const override { return "int"; }
  bool isInt()     const override { return true; }
  bool isNumeric() const override { return true; }
};

/**
 * @brief The `double` primitive type.
 *
 * Maps to llvm::Type::getDoubleTy in Codegen.
 */
class DoubleType : public TurfType {
public:
  std::string getName() const override { return "double"; }
  bool isDouble()  const override { return true; }
  bool isNumeric() const override { return true; }
};

/**
 * @brief The `bool` primitive type.
 *
 * Represented as i1 in LLVM IR.  Treated as numeric for coercion purposes.
 */
class BoolType : public TurfType {
public:
  std::string getName() const override { return "bool"; }
  bool isBool()    const override { return true; }
  bool isNumeric() const override { return true; }
};

/**
 * @brief The `string` primitive type.
 *
 * Stored as i8* in LLVM IR.  Standard library functions handle string
 * operations.
 */
class StringType : public TurfType {
public:
  std::string getName() const override { return "string"; }
  bool isString() const override { return true; }
};

/**
 * @brief The `void` return type.
 *
 * Used as the return type of functions that produce no value.
 */
class VoidType : public TurfType {
public:
  std::string getName() const override { return "void"; }
  bool isVoid() const override { return true; }
};

// ─── Null type ────────────────────────────────────────────────────────────────

/// The type of the `null` literal.
class TurfNullType : public TurfType {
public:
  std::string getName() const override { return "null"; }
  bool isNull() const override { return true; }
  bool isNamed(const std::string &N) const override { return N == "null"; }
};

// ─── OpaquePointerType (ptr) ────────────────────────────────────────────────────────

/**
 * @brief Opaque (untyped) pointer — Phase 3 FFI `ptr` keyword.
 *
 * Used to represent the return/parameter type of C functions that work with
 * untyped memory (e.g. malloc, free, dlopen).  Maps to i8* in LLVM IR
 * (represented as an opaque pointer in LLVM’s pointer-typed IR).
 *
 * Users cast a `ptr` value to a concrete pointer type using the existing `to`
 * operator.  Phase 4 will introduce `unsafe_ptr<T>` and remove this type.
 */
class OpaquePointerType : public TurfType {
public:
  std::string getName() const override { return "ptr"; }
  bool isPointer() const override { return true; }
  bool isNamed(const std::string &N) const override { return N == "ptr"; }
};

// ─── NamedType (legacy + user-defined struct names) ──────────────────────────

/**
 * @brief A type identified only by its string name.
 *
 * Used for:
 *  - User-defined struct types (until they are registered as TurfStructType).
 *  - The legacy `void*` pointer spelling used in some Codegen paths.
 *  - Any type the parser encounters before it is resolved.
 *
 * New code should prefer the explicit subclasses (IntType, etc.) over
 * constructing a NamedType.  The factory functions (TurfInt(), etc.) already
 * return the correct subclass.
 */
class NamedType : public TurfType {
  std::string Name;

public:
  explicit NamedType(std::string N) : Name(std::move(N)) {}
  std::string getName() const override { return Name; }
  bool isNamed(const std::string &N) const override { return Name == N; }
  bool isVoid() const override { return Name == "void"; }
};

// ─── ArrayType ────────────────────────────────────────────────────────────────

/**
 * @brief Fixed or dynamic array type: `T[]` / `T[N]`.
 *
 * The element type is a full TurfTypePtr so `int[]`, `string[]`, or even
 * `list<Player>[]` are representable without LLVM.
 */
class ArrayType : public TurfType {
  TurfTypePtr ElementType;
  int Size; // -1 = dynamic / unknown, >0 = fixed compile-time size

public:
  ArrayType(TurfTypePtr Elem, int Sz = -1)
      : ElementType(std::move(Elem)), Size(Sz) {}

  std::string getName() const override {
    return ElementType->getName() + "[]";
  }
  std::string getMangledName() const override {
    return ElementType->getMangledName() + "_arr_" + std::to_string(Size);
  }

  bool isArray() const override { return true; }

  const TurfType *getElementType() const { return ElementType.get(); }
  TurfTypePtr    getElementTypePtr() const { return ElementType; }
  int            getSize() const { return Size; }
};

// ─── TurfPointerType ─────────────────────────────────────────────────────────

/**
 * @brief Pointer to another type: `T*`.
 *
 * Used for pass-by-reference semantics and the implicit `self` parameter in
 * struct methods.
 */
class TurfPointerType : public TurfType {
  TurfTypePtr Pointee;

public:
  explicit TurfPointerType(TurfTypePtr Pointee) : Pointee(std::move(Pointee)) {}

  std::string getName() const override {
    return Pointee->getName() + "*";
  }
  std::string getMangledName() const override {
    return Pointee->getMangledName() + "_ptr";
  }

  bool isPointer() const override { return true; }

  TurfTypePtr getPointee() const { return Pointee; }
};

// ─── TurfGenericParamType ────────────────────────────────────────────────────

/**
 * @brief An open (unresolved) generic type parameter, e.g. `T` in `fn<T>`.
 *
 * These are introduced during parsing of generic function / struct headers
 * and replaced with concrete types during monomorphization.
 */
class TurfGenericParamType : public TurfType {
  std::string Name;

public:
  explicit TurfGenericParamType(std::string N) : Name(std::move(N)) {}
  std::string getName()        const override { return Name; }
  bool isGenericParam()        const override { return true; }
};

// ─── GenericType ─────────────────────────────────────────────────────────────

/**
 * @brief A closed generic type instantiation, e.g. `map<string, int>`.
 *
 * This is the core of monomorphization: the type arguments are full
 * TurfTypePtr values, so `list<Player>` works as soon as `Player` is a
 * registered type.
 */
class GenericType : public TurfType {
protected:
  std::string            BaseName;   ///< "list", "map", etc.
  std::vector<TurfTypePtr> TypeArgs; ///< [int], [string, int], etc.

public:
  GenericType(std::string Base, std::vector<TurfTypePtr> Args)
      : BaseName(std::move(Base)), TypeArgs(std::move(Args)) {}

  std::string getName() const override {
    std::string S = BaseName + "<";
    for (size_t i = 0; i < TypeArgs.size(); ++i) {
      if (i) S += ", ";
      S += TypeArgs[i]->getName();
    }
    return S + ">";
  }

  std::string getMangledName() const override {
    std::string S = BaseName;
    for (const auto &Arg : TypeArgs)
      S += "_" + Arg->getMangledName();
    return S;
  }

  bool isGeneric() const override { return true; }

  const std::string        &getBaseName()  const { return BaseName; }
  const std::vector<TurfTypePtr> &getTypeArgs()  const { return TypeArgs; }
  const TurfType           *getTypeArg(size_t I)    const { return TypeArgs[I].get(); }
  TurfTypePtr               getTypeArgPtr(size_t I) const { return TypeArgs[I]; }
};

// ─── TurfStructType ───────────────────────────────────────────────────────────

/// A single field in a struct declaration.
struct StructField {
  std::string Name;
  TurfTypePtr Type;
};

/**
 * @brief A user-defined aggregate (struct) type.
 *
 * Stores field names and their types so the TypeChecker can validate field
 * accesses without asking LLVM for a StructType layout.
 */
class TurfStructType : public TurfType {
  std::string              Name;
  std::vector<StructField> Fields;

public:
  TurfStructType(std::string Name, std::vector<StructField> Fields)
      : Name(std::move(Name)), Fields(std::move(Fields)) {}

  std::string getName() const override { return Name; }
  bool isStruct() const override { return true; }

  const std::vector<StructField> &getFields() const { return Fields; }

  int getFieldIndex(const std::string &FieldName) const {
    for (size_t i = 0; i < Fields.size(); ++i)
      if (Fields[i].Name == FieldName)
        return static_cast<int>(i);
    return -1;
  }

  TurfTypePtr getFieldType(const std::string &FieldName) const {
    int idx = getFieldIndex(FieldName);
    return idx >= 0 ? Fields[idx].Type : nullptr;
  }
};

// ─── RankedSuggestions (used by SymbolTable diagnostics) ─────────────────────

struct RankedSuggestions {
  std::vector<std::string>                          Local;
  std::map<std::string, std::vector<std::string>>  Module;

  bool empty() const { return Local.empty() && Module.empty(); }
};

// ─── Singleton primitive-type accessors ───────────────────────────────────────

/**
 * @defgroup PrimitiveTypeFactories Primitive type factory functions
 *
 * Each function returns a stable shared_ptr to the same object for the
 * lifetime of the process (local-static singleton, thread-safe in C++11+).
 * Callers may compare pointers for identity checks.
 *
 * The returned objects are instances of the dedicated subclasses
 * (IntType, DoubleType, …) so that both dynamic_cast and the kind
 * predicates (isInt(), isDouble(), …) work correctly.
 * @{
 */
TurfTypePtr TurfInt();
TurfTypePtr TurfDouble();
TurfTypePtr TurfBool();
TurfTypePtr TurfString();
TurfTypePtr TurfVoid();
TurfTypePtr TurfNull();
/// Opaque pointer singleton — the `ptr` FFI type (i8* in LLVM IR).
TurfTypePtr TurfPtr();
/** @} */

// ─── Array helper functions ───────────────────────────────────────────────────

bool        isArrayType(const TurfType *T);
const TurfType *getArrayElementType(const TurfType *T);
TurfTypePtr makeArrayType(TurfTypePtr ElemType, int Size = -1);

// ─── Diagnostic helper ────────────────────────────────────────────────────────

/// Human-readable diagnostic name for a type (handles null gracefully).
inline std::string turfTypeName(const TurfType *T) {
  return T ? T->getName() : "unknown";
}

#endif // TYPES_H
