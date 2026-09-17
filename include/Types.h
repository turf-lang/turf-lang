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
 *   ├─ CharType               — the `char` primitive (1 byte, no arithmetic)
 *   ├─ StringType             — the `string` primitive
 *   ├─ VoidType               — the `void` return type
 *   ├─ TurfNullType           — the `null` literal type
 *   ├─ OpaquePointerType      — the `ptr` FFI opaque pointer (i8* in LLVM IR)
 *   ├─ NamedType              — user-defined struct names and legacy fallback
 *   ├─ ArrayType              — T[]  /  T[N]
 *   ├─ TurfPointerType        — T*   (pass-by-reference / self)
 *   ├─ TurfGenericParamType   — open generic parameter (e.g. T in fn<T>)
 *   ├─ GenericType            — closed generic instantiation (e.g. map<K,V>)
 *   ├─ TurfStructType         — user-defined aggregate type
 *   ├─ TurfEnumType           — user-defined enum type
 *   └─ FuncType               — first-class function value type (fn(T): R)
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

#include "Lexer.h" // SourceLocation -- for StructField's Name/Type spans; no circular dep (Lexer.h doesn't include Types.h)

#include <cassert>
#include <map>
#include <memory>
#include <set>
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
  virtual bool isInt32()  const { return false; }
  virtual bool isDouble() const { return false; }
  virtual bool isBool()   const { return false; }
  virtual bool isChar()   const { return false; }
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

  /// True if this type is a `TurfReferenceType` (`&T`). A reference IS-A
  /// pointer (isPointer() is also true for it, via inheritance -- see
  /// TurfReferenceType's own doc comment) but carries a stricter,
  /// compiler-enforced non-null contract; this predicate is how callers
  /// that need to tell the two apart (typesCompatible(), the `unsafe`
  /// gate) do so without dynamic_cast.
  virtual bool isReference() const { return false; }

  /// True if this type is a `TurfStructType`.
  virtual bool isStruct() const { return false; }

  /// True if this type is a `TurfGenericParamType` (open type parameter).
  virtual bool isGenericParam() const { return false; }

  /// True if this type is an `TurfEnumType`.
  virtual bool isEnum() const { return false; }
  virtual bool isMachine() const { return false; }

  /// True if this type is a `FuncType` (a function VALUE's type, e.g. the
  /// `fn(int): int` in `mutable f: fn(int): int = double_it;` -- distinct
  /// from a function DECLARATION, which has no TurfType of its own at all).
  virtual bool isFunc() const { return false; }

  /// True if this type is a `TurfUserErrorType` (a user-declared `error Name
  /// { ... }` type, e.g. `IOError`/`MathError`). NOT related to `TurfError`
  /// (include/Errors.h), the compiler's own internal diagnostic base class.
  virtual bool isUserError() const { return false; }

  /// True if this type is a `TurfErrorUnionType` (a `!T` fallible return
  /// type -- T on success, or any declared error-type value on failure).
  virtual bool isErrorUnion() const { return false; }

  /// True if this type is `AnyErrorType` -- the type of `err` inside
  /// `recover as err run { ... }`. Since `!T` doesn't restrict WHICH declared
  /// error type a fallible call can fail with, `err`'s real concrete type
  /// isn't knowable at compile time; AnyErrorType is a type-erased stand-in
  /// that `==`/`!=` specifically allows comparing against any real
  /// TurfUserErrorType value (see TypeChecker::inferRelationalType).
  virtual bool isAnyError() const { return false; }

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
 * @brief The `int32` primitive type.
 *
 * Maps to llvm::Type::getInt32Ty in Codegen. Exists specifically so
 * `extern fn` declarations can honestly describe a C function whose
 * parameter/return is a genuine 32-bit `int` (e.g. libc's strcmp, toupper) --
 * declaring those as Turf's 64-bit `int` builds the wrong LLVM function
 * signature and silently reads garbage upper bits from the return register
 * (confirmed live: strcmp() reads back as an unsigned 4294967295 instead of
 * signed -1). isNumeric() IS overridden true (unlike CharType) -- int32 is a
 * genuine arithmetic integer, just narrower; arithmetic/comparisons with it
 * fall through the ordinary numeric paths for free and promote straight to
 * `int` (mirrors how `bool` arithmetic already promotes to `int` today).
 * Deliberately NOT a parallel 32-bit arithmetic type system -- see
 * typesCompatible()'s int32<->int Rule 3 entries and CastToType()'s existing
 * "normalize foreign i32 to i64" step, which together mean a genuinely
 * i32-typed value widens to i64 automatically the moment it's used in any
 * operation, with no new Codegen dispatch needed.
 */
class Int32Type : public TurfType {
public:
  std::string getName() const override { return "int32"; }
  bool isInt32()   const override { return true; }
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
 * @brief The `char` primitive type.
 *
 * A single byte, not a Unicode codepoint (RFC: STRING_UFCS_OPERATOR_CHAR_RFC
 * §3). Maps to llvm::Type::getInt8Ty in Codegen. Deliberately does NOT
 * override isNumeric() -- this is the load-bearing decision that keeps char
 * out of every existing numeric-coercion path (typesCompatible(), implicit
 * widening, etc.) without needing any new exclusion logic anywhere else.
 * char supports built-in comparisons but not arithmetic; arithmetic requires
 * an explicit `to int` cast first.
 */
class CharType : public TurfType {
public:
  std::string getName() const override { return "char"; }
  bool isChar() const override { return true; }
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
/**
 * @brief The type of `err` inside `recover as err run { ... }`
 * (docs/architecture/errors_and_recover.md). See `TurfType::isAnyError()`'s doc
 * comment for why this exists as its own type rather than any specific declared
 * error type.
 */
class AnyErrorType : public TurfType {
public:
  std::string getName() const override { return "<error>"; }
  bool isAnyError() const override { return true; }
};

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
  // The `Module` in `Module::Name` (empty if unqualified). Deliberately NOT
  // part of getName()/isNamed()'s identity -- struct/enum names are still
  // one flat, module-blind namespace (a name is globally unique once
  // declared), so two NamedTypes for the same bare Name must still compare
  // equal for ordinary type-compatibility purposes regardless of whether
  // one spelling happened to be qualified. This field exists ONLY so the
  // Resolver can validate the qualifier (real module? does IT own this
  // type?) once, at the point the type reference is first resolved -- see
  // Resolver::validateModuleQualifier().
  std::string Module;

public:
  explicit NamedType(std::string N, std::string M = "")
      : Name(std::move(N)), Module(std::move(M)) {}
  std::string getName() const override { return Name; }
  const std::string &getModule() const { return Module; }
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

  /**
   * @brief Human-readable name: "T[N]" for a concrete size, "T[]" for the
   *        unsized/"any length" spelling (Size == -1, used for array
   *        function parameters).
   *
   * Deliberately includes the size for concrete arrays -- it used to always
   * render as "T[]" regardless of Size, which made int[5] and int[10]
   * display, and (before typesCompatible() was fixed to special-case arrays)
   * COMPARE, as "the same type". Audited every getName()-equality call site
   * in the codebase before making this change; every one either doesn't
   * involve arrays, or only relies on same-size arrays continuing to compare
   * equal (which they still do), so this is a strict improvement with no
   * behavioral loss.
   */
  std::string getName() const override {
    if (Size < 0)
      return ElementType->getName() + "[]";
    return ElementType->getName() + "[" + std::to_string(Size) + "]";
  }
  std::string getMangledName() const override {
    return ElementType->getMangledName() + "_arr_" + std::to_string(Size);
  }

  bool isArray() const override { return true; }

  bool isEquivalent(const TurfType *Other) const override {
    auto *OtherArr = dynamic_cast<const ArrayType *>(Other);
    if (!OtherArr) return false;
    return Size == OtherArr->Size &&
           ElementType->isEquivalent(OtherArr->ElementType.get());
  }

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

// ─── TurfReferenceType ───────────────────────────────────────────────────────

/**
 * @brief Compiler-guaranteed non-null safe reference: `&T` (issue #238).
 *
 * Deliberately a SUBCLASS of TurfPointerType, not a sibling. At the LLVM
 * level a `&T` and a `T*` are identical -- both an opaque pointer value --
 * so every existing `dynamic_cast<const TurfPointerType *>` call site
 * throughout Codegen (auto-deref field access, argument passing,
 * address-of caching, pointer-cast codegen) keeps working for `&T` for
 * free via ordinary derived-to-base dynamic_cast, with zero Codegen
 * changes. `isPointer()` is inherited (stays true); the only new surface
 * is `isReference()`, consulted exclusively by typesCompatible()
 * (src/Types.cpp) and the TypeChecker's `unsafe` gate to tell a safe
 * reference apart from a raw, gated pointer.
 */
class TurfReferenceType : public TurfPointerType {
public:
  explicit TurfReferenceType(TurfTypePtr Pointee)
      : TurfPointerType(std::move(Pointee)) {}

  std::string getName() const override {
    return "&" + getPointee()->getName();
  }
  std::string getMangledName() const override {
    return getPointee()->getMangledName() + "_ref";
  }

  bool isReference() const override { return true; }
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
  // The `Module` in `Module::BaseName<...>` (empty if unqualified) -- same
  // purpose and same "not part of type identity" rule as NamedType::Module,
  // see its doc comment.
  std::string Module;

public:
  GenericType(std::string Base, std::vector<TurfTypePtr> Args, std::string Mod = "")
      : BaseName(std::move(Base)), TypeArgs(std::move(Args)), Module(std::move(Mod)) {}

  const std::string &getModule() const { return Module; }

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
  const TurfType           *getTypeArg(size_t I)    const {
    assert(I < TypeArgs.size() && "GenericType type argument out of bounds");
    return TypeArgs[I].get();
  }
  TurfTypePtr               getTypeArgPtr(size_t I) const {
    assert(I < TypeArgs.size() && "GenericType type argument out of bounds");
    return TypeArgs[I];
  }
};

// ─── TurfStructType ───────────────────────────────────────────────────────────

/// A single field in a struct declaration.
struct StructField {
  std::string Name;
  TurfTypePtr Type;
  // Source spans for the field's NAME and declared TYPE tokens respectively
  // (`x` and `int` in `x: int`). Added for tooling (LSP semantic tokens /
  // hover on a struct field) -- TurfType itself has no source location of
  // its own (a type is a semantic value object, possibly shared across
  // unrelated occurrences), so the span has to live on the syntactic site
  // that names it instead. Default {0,0} for the handful of StructFields
  // built without a real parse (e.g. by code paths other than
  // Parser::ParseStructDecl(), if any exist).
  SourceLocation NameLoc;
  SourceLocation TypeLoc;
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

// ─── TurfEnumType ────────────────────────────────────────────────────────────

class TurfEnumType : public TurfType {
    std::string                   Name_;
    std::vector<std::string>      Variants_;
    std::map<std::string, int>    VariantValues_;
    std::shared_ptr<TurfType>     UnderlyingType_;

public:
    TurfEnumType(std::string Name,
                 std::vector<std::string> Variants,
                 std::shared_ptr<TurfType> UnderlyingType);

    std::string getName() const override { return Name_; }
    bool isEnum() const override { return true; }

    const TurfType*  getUnderlyingType()    const { return UnderlyingType_.get(); }
    TurfTypePtr      getUnderlyingTypePtr() const { return UnderlyingType_; }
    int  getVariantValue(const std::string& V) const;
    bool hasVariant(const std::string& V)      const;
    const std::vector<std::string>& getVariants() const { return Variants_; }
    int numVariants() const { return static_cast<int>(Variants_.size()); }

    std::string getMangledName() const override {
        return Name_ + "_enum_" + UnderlyingType_->getMangledName();
    }
};

// ─── TurfMachineType ─────────────────────────────────────────────────────────

// A `machine` declaration's type: an enum (states = variants, same positional
// ordinals) plus a compiler-checked transition graph. Subclasses TurfEnumType
// rather than duplicating its variant/ordinal machinery -- this is the design
// spine of the whole `machine` feature: because a TurfMachineType IS-A
// TurfEnumType, `MachineName::State` access, `compare`/`matches`
// exhaustiveness checking, and Codegen's "inline the variant's ordinal as a
// constant at every use site" all work completely unchanged, with zero new
// code anywhere in those paths (see ResolverCall.cpp's visitScopeAccess(),
// TypeCheckerExpr.cpp's visitCompare(), CodegenStruct.cpp's
// ScopeAccessExprAST::codegen() -- all three only ever check
// isEnum()/dynamic_cast<TurfEnumType*>, both of which a TurfMachineType
// satisfies for free). Only the transition-edge data below is new.
class TurfMachineType : public TurfEnumType {
    // Adjacency keyed by STATE NAME (not ordinal) -- checked directly against
    // a ScopeAccessExprAST's variant name (a string) at every `.transition()`
    // call site with no extra ordinal lookup, and machine state counts are
    // always small enough that this isn't a performance concern.
    std::map<std::string, std::set<std::string>> Edges_; // From -> {To, To, ...}

public:
    TurfMachineType(std::string Name, std::vector<std::string> States,
                    std::map<std::string, std::set<std::string>> Edges);

    bool isMachine() const override { return true; }
    bool hasEdge(const std::string &From, const std::string &To) const;
    const std::map<std::string, std::set<std::string>> &getEdges() const {
        return Edges_;
    }

    std::string getMangledName() const override {
        return getName() + "_machine";
    }
};

// ─── TurfUserErrorType ────────────────────────────────────────────────────────

/**
 * @brief A user-declared `error Name { Variant, ... }` type
 * (docs/architecture/errors_and_recover.md). Deliberately NOT named
 * `TurfErrorType` -- one character away from `TurfError` (include/Errors.h),
 * the compiler's own internal diagnostic base class, which is an entirely
 * unrelated concept.
 *
 * Variants are bare names only, exactly like `TurfEnumType` -- no payload
 * fields. This is a deliberate scope decision (see errors_and_recover.md
 * §5.1): a variant with fields, and a real destructuring syntax to read them
 * back safely, is deferred until there's an actual need for it.
 */
class TurfUserErrorType : public TurfType {
    std::string               Name_;
    std::vector<std::string> Variants_;
    bool                      IsBareForm_; // true for `error Name;` (implicit single DEFAULT variant)

public:
    TurfUserErrorType(std::string Name, std::vector<std::string> Variants, bool IsBareForm)
        : Name_(std::move(Name)), Variants_(std::move(Variants)), IsBareForm_(IsBareForm) {}

    std::string getName() const override { return Name_; }
    bool isUserError() const override { return true; }
    bool isBareForm() const { return IsBareForm_; }

    const std::vector<std::string>& getVariants() const { return Variants_; }

    bool hasVariant(const std::string& V) const {
        for (const auto& Variant : Variants_)
            if (Variant == V) return true;
        return false;
    }

    /// Alias for getVariants() -- for Damerau-Levenshtein suggestion
    /// diagnostics (mirrors TurfEnumType::getVariants(), already bare names).
    std::vector<std::string> getVariantNames() const {
        std::vector<std::string> Names;
        Names.reserve(Variants_.size());
        for (const auto& Variant : Variants_)
            Names.push_back(Variant);
        return Names;
    }

    std::string getMangledName() const override { return Name_ + "_error"; }
};

// ─── TurfErrorUnionType ───────────────────────────────────────────────────────

/**
 * @brief The `!T` fallible-return type
 * (docs/architecture/errors_and_recover.md): T on success, or a value of ANY
 * declared `error` type on failure. Distinct from `TurfUserErrorType` (the
 * declared error TYPE itself) -- a `!T` function doesn't name one specific
 * error type, it can fail with any of the program's declared error types. Also
 * unrelated to `TurfError` (include/Errors.h), the compiler's internal
 * diagnostic base class.
 */
class TurfErrorUnionType : public TurfType {
    TurfTypePtr SuccessType_;

public:
    explicit TurfErrorUnionType(TurfTypePtr SuccessType) : SuccessType_(std::move(SuccessType)) {}

    std::string getName() const override { return "!" + SuccessType_->getName(); }
    bool isErrorUnion() const override { return true; }

    const TurfType* getSuccessType()    const { return SuccessType_.get(); }
    TurfTypePtr     getSuccessTypePtr() const { return SuccessType_; }

    std::string getMangledName() const override { return "errunion_" + SuccessType_->getMangledName(); }
};

// ─── FuncType ─────────────────────────────────────────────────────────────────

/**
 * @brief The type of a first-class function VALUE: `fn(T1, T2, ...): R`.
 *
 * Not to be confused with a function DECLARATION (`FuncDefExprAST`/
 * `ResolverFunctionSymbol`), which is a named, callable entity registered in
 * the symbol table, not a value with a TurfType. FuncType exists so a
 * function can also be treated as an ordinary VALUE: passed as an argument,
 * stored in a variable, or used as a parameter/return type annotation --
 * `fn(int): int` is parsed by ParseType() exactly like any other type
 * expression (see Parser.cpp's TOK_FN branch).
 *
 * Two FuncTypes are compatible (typesCompatible()) purely via the "exact
 * getName() match" fallback every other TurfType already gets -- no bespoke
 * structural-equality code was needed since getName() is itself already a
 * deterministic, structural rendering of the signature.
 */
class FuncType : public TurfType {
  TurfTypePtr ReturnType_;
  std::vector<TurfTypePtr> ParamTypes_;

public:
  FuncType(TurfTypePtr ReturnType, std::vector<TurfTypePtr> ParamTypes)
      : ReturnType_(std::move(ReturnType)), ParamTypes_(std::move(ParamTypes)) {}

  std::string getName() const override {
    std::string S = "fn(";
    for (size_t i = 0; i < ParamTypes_.size(); ++i) {
      if (i) S += ", ";
      S += ParamTypes_[i] ? ParamTypes_[i]->getName() : "?";
    }
    S += "): ";
    S += ReturnType_ ? ReturnType_->getName() : "void";
    return S;
  }

  std::string getMangledName() const override {
    std::string S = "fnty";
    for (const auto &P : ParamTypes_)
      S += "_" + (P ? P->getMangledName() : "unk");
    S += "_ret_" + (ReturnType_ ? ReturnType_->getMangledName() : "void");
    return S;
  }

  bool isFunc() const override { return true; }

  TurfTypePtr getReturnType() const { return ReturnType_; }
  const std::vector<TurfTypePtr> &getParamTypes() const { return ParamTypes_; }

  bool isEquivalent(const TurfType *Other) const override {
    const auto *OtherFT = dynamic_cast<const FuncType *>(Other);
    if (!OtherFT || ParamTypes_.size() != OtherFT->ParamTypes_.size())
      return false;
    for (size_t i = 0; i < ParamTypes_.size(); ++i)
      if (!ParamTypes_[i] || !ParamTypes_[i]->isEquivalent(OtherFT->ParamTypes_[i].get()))
        return false;
    return ReturnType_ && ReturnType_->isEquivalent(OtherFT->ReturnType_.get());
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
TurfTypePtr TurfInt32();
TurfTypePtr TurfDouble();
TurfTypePtr TurfBool();
TurfTypePtr TurfChar();
TurfTypePtr TurfString();
TurfTypePtr TurfVoid();
TurfTypePtr TurfNull();
/// Opaque pointer singleton — the `ptr` FFI type (i8* in LLVM IR).
TurfTypePtr TurfPtr();
/// The type of `err` inside `recover as err run { ... }` -- see
/// `AnyErrorType`'s doc comment.
TurfTypePtr TurfAnyError();
/** @} */

// ─── Array helper functions ───────────────────────────────────────────────────

bool        isArrayType(const TurfType *T);
const TurfType *getArrayElementType(const TurfType *T);
TurfTypePtr makeArrayType(TurfTypePtr ElemType, int Size = -1);

// ─── Type coercion helper ───────────────────────────────────────────────────

/**
 * @brief Helper: check if two Turf type names are implicitly compatible.
 *
 * Numerics (int, double, bool) are mutually coercible; string and void are
 * not compatible with anything else. Opaque pointers (ptr) can coerce to/from
 * any typed pointer.
 */
bool typesCompatible(const TurfType *From, const TurfType *To);

// ─── Diagnostic helper ────────────────────────────────────────────────────────

/// Human-readable diagnostic name for a type (handles null gracefully).
inline std::string turfTypeName(const TurfType *T) {
  return T ? T->getName() : "unknown";
}

#endif // TYPES_H
