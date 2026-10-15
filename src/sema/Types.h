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
 *   │   ├─ TurfReferenceType         — &T   (compiler-guaranteed non-null)
 *   │   └─ TurfNullableReferenceType — &?T  (nullable, NullabilityPass-checked; LANG-002)
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

#include "frontend/Lexer.h" // SourceLocation -- for StructField's Name/Type spans; no circular dep (Lexer.h doesn't include Types.h)

#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declarations
class TurfType;
using TurfTypePtr = std::shared_ptr<TurfType>;

// ─── Type identity (FDN-003) ─────────────────────────────────────────────────

/**
 * @brief The one encoding of a struct/enum/machine/error type's identity,
 *        (owning module, name), as the string every name-keyed table in the
 *        compiler already uses.
 *
 * A type declared in an imported module `M` is `M::Name`; a type declared in
 * the main program file is its bare `Name` (its module is `main` -- DEC-13 --
 * but that qualifier is never spelled, so every main-file program's type
 * names, `typeof()` output and LLVM names are what they were before FDN-003).
 * `::` can never appear in an identifier, so `M::Name` cannot collide with a
 * main-file type, and two modules' `Node`s cannot collide with each other.
 *
 * INV-TYPE-3: identity is only ever produced by make() from a (module, name)
 * pair the Resolver or Parser actually knows owns the declaration -- never by
 * pasting a spelling together, and never by stripping a qualifier off for
 * comparison. bareName() exists for display-adjacent uses only (a
 * "did you mean" candidate, an import-list lookup); comparing two bareName()s
 * to decide whether two types are the same is exactly the pre-FDN-003 bug.
 */
namespace TypeIdentity {
inline std::string make(const std::string &Module, const std::string &Name) {
  return Module.empty() ? Name : Module + "::" + Name;
}
/// The module part of @p Id ("" for a main-file type). Splits at the FIRST
/// `::` -- a generic clone's identity (`a::Box_gen1_b::Item`) carries its
/// type arguments' identities after the base, and belongs to the base's module.
inline std::string owningModule(const std::string &Id) {
  auto P = Id.find("::");
  return P == std::string::npos ? "" : Id.substr(0, P);
}
inline std::string bareName(const std::string &Id) {
  auto P = Id.find("::");
  return P == std::string::npos ? Id : Id.substr(P + 2);
}
/// True for a string that can only be an identity, never a source spelling
/// (a spelling's qualifier lives in NamedType/GenericType::getModule(), never
/// inside the name itself).
inline bool isModuleQualified(const std::string &Id) {
  return Id.find("::") != std::string::npos;
}
/// The spelling used for LLVM struct names, GC descriptor symbols and DWARF
/// type names: `M.Name` (owner-approved FDN-003 stage 1). Never used as a key.
inline std::string llvmName(const std::string &Id) {
  std::string S;
  S.reserve(Id.size());
  for (size_t i = 0; i < Id.size(); ++i) {
    if (Id[i] == ':' && i + 1 < Id.size() && Id[i + 1] == ':') {
      S += '.';
      ++i;
    } else {
      S += Id[i];
    }
  }
  return S;
}
} // namespace TypeIdentity

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

  /// True for the `guess` placeholder (SEM-003) -- the type ParseType()
  /// produces for `: guess` or an omitted annotation, before the
  /// TypeChecker replaces it with the initializer's real type
  /// (VarDeclExprAST::setTypePtr()). A `guess`-typed node reaching Codegen
  /// would mean the TypeChecker skipped inference (a compiler bug); unlike
  /// F-GEN-2's TurfGenericParamType, this has no dedicated Codegen assert
  /// because a `guess` that failed to resolve always already raised a hard
  /// TypeChecker error first, and main.cpp aborts before Codegen runs on
  /// any errored program (see main.cpp's own comment above its TypeChecker
  /// pass, "Codegen must never run on" an errored compile).
  virtual bool isGuess()  const { return false; }

  /// True for any numeric type (int, int32, double).
  virtual bool isNumeric() const { return isInt() || isDouble(); }

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

  /// True if this type is a `TurfNullableReferenceType` (`&?T`, LANG-002).
  /// Deliberately a SIBLING of TurfReferenceType (both direct
  /// TurfPointerType subclasses), not a further subclass of it -- `&?T`
  /// does NOT carry `&T`'s non-null contract, so isReference() stays FALSE
  /// for it (see TurfNullableReferenceType's own doc comment for why this
  /// makes the existing `&T`-widening typesCompatible() rules, and the
  /// `unsafe` gate's raw-pointer exemption, apply to `&?T` for free without
  /// implying a false non-null guarantee). Consulted by typesCompatible()
  /// (null-compatibility, the `&?T -> T*` widening rule) and by
  /// TypeChecker::isRawPointer() (both safe reference kinds are exempt from
  /// the `unsafe` gate -- `&?T`'s own safety is enforced by a SEPARATE
  /// mechanism, NullabilityPass, not the raw-pointer gate).
  virtual bool isNullableReference() const { return false; }

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
  /// (src/diagnostics/Errors.h), the compiler's own internal diagnostic base class.
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
 * Represented as i1 in LLVM IR. Not numeric: no arithmetic, ordering or
 * implicit conversion to a number; `b to int` converts explicitly.
 */
class BoolType : public TurfType {
public:
  std::string getName() const override { return "bool"; }
  bool isBool()    const override { return true; }
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

// ─── GuessType (SEM-003) ────────────────────────────────────────────────────

/**
 * @brief Placeholder type for `: guess` / an omitted variable-declaration
 * annotation (ARCHITECTURE_OVERHAUL.md §11).
 *
 * ParseType() and ParseLetStatement() produce this singleton for both
 * spellings (`mut x: guess = 5;` and `mut x = 5;`) so they parse to the
 * identical AST shape (§11.2). It is a pure placeholder, never a real type
 * for anything downstream of the TypeChecker: `TypeChecker::visitVarDecl`
 * infers the real type from the (already-checked) initializer and
 * overwrites both `VarDeclExprAST::Type` (via `setTypePtr()`) and the
 * declaration's own `VariableSymbol::Type` in place -- the same
 * "placeholder now, refine later" contract `ForEachExprAST`'s loop
 * variable and a plain-variable-declaration's `NamedType` already use
 * (see `Resolver::visitForEach`'s and `TypeChecker::visitVarDecl`'s own
 * comments). `Resolver::visitVarDecl` declares the variable with this
 * placeholder type -- never a null type -- so no other Resolver code path
 * needs to special-case an unset type.
 */
class GuessType : public TurfType {
public:
  std::string getName() const override { return "guess"; }
  bool isGuess() const override { return true; }
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
  // The `Module` in `Module::Name` as SPELLED (empty if unqualified). Not
  // part of identity by itself: once the Resolver has resolved this
  // reference (resolveTo()), Name holds the full TypeIdentity and this field
  // is kept only as a record of what the source wrote.
  std::string Module;
  // FDN-003: set once the Resolver has turned the spelling into a
  // TypeIdentity. Needed because a main-file type's identity is its bare
  // name, indistinguishable by text from an unresolved spelling -- and a
  // monomorphized clone is re-resolved in its TEMPLATE's module, where a
  // substituted main-file `Node` must not be re-read as that module's own
  // `Node` (resolveMonomorphizedClone(), Resolver.h).
  bool Resolved = false;

public:
  explicit NamedType(std::string N, std::string M = "")
      : Name(std::move(N)), Module(std::move(M)) {}
  std::string getName() const override { return Name; }
  const std::string &getModule() const { return Module; }
  bool isNamed(const std::string &N) const override { return Name == N; }
  bool isVoid() const override { return Name == "void"; }

  bool isResolvedIdentity() const {
    return Resolved || TypeIdentity::isModuleQualified(Name);
  }
  /// Mutates in place: a parsed NamedType is owned by exactly one source
  /// occurrence (ParseType() never shares one), and every holder of this
  /// pointer -- the AST node, the parse-time TurfStructType copy of a field
  /// list, a ResolverFunctionSymbol's captured signature -- must see the
  /// identity, not just the one the Resolver happened to reach it through.
  void resolveTo(std::string Identity) {
    Name = std::move(Identity);
    Resolved = true;
  }
};

/// A NamedType that already holds a TypeIdentity -- for types synthesized
/// after resolution (a `Type::new(...)` call's result type, a monomorphized
/// `self`), which must never be re-resolved against a different module.
inline std::shared_ptr<NamedType> makeResolvedNamedType(std::string Identity) {
  auto NT = std::make_shared<NamedType>(Identity);
  NT->resolveTo(std::move(Identity));
  return NT;
}

// ─── ArrayType ────────────────────────────────────────────────────────────────

/**
 * @brief Fixed or dynamic array type: `T[]` / `T[N]`.
 *
 * The element type is a full TurfTypePtr so `int[]`, `string[]`, or even
 * `list<Player>[]` are representable without LLVM.
 */
class ArrayType : public TurfType {
  TurfTypePtr ElementType;
  int Size; // -1 = dynamic / unknown, >0 = fixed compile-time size,
            // kSymbolicSize = a size parameter's (SizeParam)
  // `T[N]` in a declaration `fn<T, N: size>`: the size is the parameter N,
  // fixed per instantiation (TypeWalk::substitute()). Never unsized.
  std::string SizeParam;

public:
  static constexpr int kSymbolicSize = -2;

  ArrayType(TurfTypePtr Elem, int Sz = -1)
      : ElementType(std::move(Elem)), Size(Sz) {}
  ArrayType(TurfTypePtr Elem, std::string SizeParamName)
      : ElementType(std::move(Elem)), Size(kSymbolicSize),
        SizeParam(std::move(SizeParamName)) {}

  bool hasSymbolicSize() const { return !SizeParam.empty(); }
  const std::string &getSizeParam() const { return SizeParam; }

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
  ///
  /// Dimensions print in source order, outermost first: `int[2][3]` (two rows
  /// of three) is ArrayType(ArrayType(int, 3), 2), and appending this level's
  /// "[2]" after the element's own name used to print it "int[3][2]" -- which
  /// read as the opposite type once `T[]` made the order matter (`int[][3]`,
  /// any number of rows of three, vs. the invalid `int[3][]`).
  std::string getName() const override {
    std::string Dims;
    const TurfType *Base = this;
    while (const auto *AT = dynamic_cast<const ArrayType *>(Base)) {
      Dims += AT->hasSymbolicSize() ? "[" + AT->SizeParam + "]"
              : AT->Size < 0        ? "[]"
                                    : "[" + std::to_string(AT->Size) + "]";
      Base = AT->ElementType.get();
    }
    // `(&N)[2]`, an array of references, is spelled with parentheses (a bare
    // `&N[2]` is a reference to an array), and prints the way it is written.
    // So is `(fn(int): int)[2]`: in `fn(int): int[2]` the `[2]` belongs to
    // the return type.
    if (Base->isReference() || Base->isNullableReference() || Base->isFunc())
      return "(" + Base->getName() + ")" + Dims;
    return Base->getName() + Dims;
  }
  std::string getMangledName() const override {
    return ElementType->getMangledName() + "_arr_" +
           (hasSymbolicSize() ? SizeParam : std::to_string(Size));
  }

  bool isArray() const override { return true; }

  bool isEquivalent(const TurfType *Other) const override {
    auto *OtherArr = dynamic_cast<const ArrayType *>(Other);
    if (!OtherArr) return false;
    return Size == OtherArr->Size && SizeParam == OtherArr->SizeParam &&
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
 * (src/sema/Types.cpp) and the TypeChecker's `unsafe` gate to tell a safe
 * reference apart from a raw, gated pointer.
 */
class TurfReferenceType : public TurfPointerType {
public:
  explicit TurfReferenceType(TurfTypePtr Pointee)
      : TurfPointerType(std::move(Pointee)) {}

  // A reference to an array prints as `&(N[2])`, so it never reads like the
  // array of references `(&N)[2]` (turf-fuzz: both used to print `&N[2]`).
  std::string getName() const override {
    if (getPointee()->isArray())
      return "&(" + getPointee()->getName() + ")";
    return "&" + getPointee()->getName();
  }
  std::string getMangledName() const override {
    return getPointee()->getMangledName() + "_ref";
  }

  bool isReference() const override { return true; }
};

// ─── TurfNullableReferenceType ───────────────────────────────────────────────

/**
 * @brief Compiler-checked NULLABLE reference: `&?T` (LANG-002).
 *
 * Closes the gap `&T` deliberately leaves open: a `&T` can never be null,
 * so it cannot represent "no next node" and user code had no safe way to
 * write a linked list (only `T*`, gated behind `unsafe { }` and banned
 * outright from ordinary function signatures). `&?T` is the answer --
 * `null` is assignable to it, and a `NullabilityPass` (Analysis.h) requires
 * a direct-comparison null check (`p != null` / `p == null`) before a
 * `&?T`-typed variable may be field-accessed, tracked per-variable over the
 * function's CFG.
 *
 * A SIBLING of TurfReferenceType, not a subclass of it -- both are direct
 * TurfPointerType subclasses. This is deliberate: `&?T` does NOT carry
 * `&T`'s "always non-null" contract, so `isReference()` stays FALSE for it
 * (only `isNullableReference()` is true). That single choice makes the
 * EXISTING pointer-widening rules apply to `&?T` correctly with no further
 * changes:
 *   - typesCompatible()'s null-compatibility rule (`isPointer() &&
 *     !isReference()`) already accepts `&?T` as a null target, unchanged.
 *   - typesCompatible()'s Rule 2 (`ptr`/`void*` <-> `T*` coercion) already
 *     treats `&?T` like a plain raw pointer for widening TO opaque, unchanged.
 *   - typesCompatible()'s Rule 2.5 (`&T` widens to a concrete `T2*`) already
 *     matches `&?T` as a valid TARGET (its `!To->isReference()` guard is
 *     true for `&?T`), so `&T -> &?T` widens implicitly for free.
 * Only ONE new typesCompatible() rule is needed: `&?T -> T2*` (widening a
 * nullable reference to a concrete raw pointer, always sound -- the LLVM
 * value doesn't change), mirroring Rule 2.5 but keyed on
 * TurfNullableReferenceType instead of TurfReferenceType, since `dynamic_
 * cast<TurfReferenceType*>` does NOT match a sibling class.
 *
 * The reverse, narrowing direction (`&?T -> &T`) is deliberately NEVER
 * granted by typesCompatible() -- LANG-002's own "Must NOT reinterpret"
 * list: "`&T` stays non-null -- do not merge the two types." The ONLY way
 * to bridge that direction is an explicit `p to &T` cast (already
 * syntactically legal via the existing pointer-like cast bucket in
 * TypeCheckerExpr.cpp's visitCast(), no `unsafe` required since neither
 * side is a raw pointer), whose SOUNDNESS -- was `p` actually narrowed
 * non-null at this exact cast site -- is verified by NullabilityPass at
 * Codegen time, not by the TypeChecker (which has no flow information).
 *
 * At the LLVM level `&?T` is representationally IDENTICAL to `&T`/`T*`
 * (an opaque pointer) -- `isPointer()` is inherited (true), so every
 * existing Codegen `dynamic_cast<TurfPointerType*>` call site (auto-deref
 * field access, argument passing, GEP) handles it for free, zero Codegen
 * changes, same as `&T` before it.
 */
class TurfNullableReferenceType : public TurfPointerType {
public:
  explicit TurfNullableReferenceType(TurfTypePtr Pointee)
      : TurfPointerType(std::move(Pointee)) {}

  std::string getName() const override {
    if (getPointee()->isArray())
      return "&?(" + getPointee()->getName() + ")";
    return "&?" + getPointee()->getName();
  }
  std::string getMangledName() const override {
    return getPointee()->getMangledName() + "_nref";
  }

  bool isNullableReference() const override { return true; }
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

/**
 * @brief The value a size parameter (`N` in `fn<T, N: size>`) is bound to:
 *        a positive array length.
 *
 * It travels where a type argument does (a substitution map, an
 * instantiation's TypeArgs), so it is a TurfType, but it is never the type of
 * a value: it only ever replaces the size of an ArrayType (TypeWalk::
 * substitute()) and the `N` a body reads (Monomorphize).
 */
class TurfSizeValueType : public TurfType {
  int Value;

public:
  explicit TurfSizeValueType(int V) : Value(V) {}
  int getValue() const { return Value; }
  std::string getName() const override { return std::to_string(Value); }
  std::string getMangledName() const override { return "n" + std::to_string(Value); }
  bool isEquivalent(const TurfType *Other) const override {
    const auto *O = dynamic_cast<const TurfSizeValueType *>(Other);
    return O && O->Value == Value;
  }
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
  // The `Module` in `Module::BaseName<...>` as spelled (empty if
  // unqualified) -- same role as NamedType::Module, see its doc comment.
  std::string Module;
  // FDN-003: BaseName holds a TypeIdentity -- same role as
  // NamedType::Resolved, see its doc comment.
  bool BaseResolved = false;

public:
  GenericType(std::string Base, std::vector<TurfTypePtr> Args, std::string Mod = "")
      : BaseName(std::move(Base)), TypeArgs(std::move(Args)), Module(std::move(Mod)) {}

  const std::string &getModule() const { return Module; }

  bool isBaseResolved() const {
    return BaseResolved || TypeIdentity::isModuleQualified(BaseName);
  }
  /// In place, for the same reason as NamedType::resolveTo().
  void resolveBaseTo(std::string Identity) {
    BaseName = std::move(Identity);
    BaseResolved = true;
  }
  /// The same instantiation with different type arguments. Every rebuild of
  /// a GenericType from an existing one goes through here so the resolved
  /// base identity is never dropped on the floor (a rebuilt base that lost
  /// it would be re-resolved -- possibly to another module's same-named
  /// type -- the next time a clone is resolved).
  std::shared_ptr<GenericType> withTypeArgs(std::vector<TurfTypePtr> NewArgs) const {
    auto GT = std::make_shared<GenericType>(BaseName, std::move(NewArgs), Module);
    GT->BaseResolved = BaseResolved;
    return GT;
  }

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
/**
 * @brief Whether a declaration was written with the `pub`/`public` prefix
 *        (LEC-002, FREEZE.md §6.15), and where.
 *
 * Kept as a mark on each declaration rather than a bare bool so a later
 * diagnostic can point at the exact `pub` token (or, for a private item, name
 * where one WOULD go -- the declaration's own keyword) and so the two
 * spellings, `pub` and `public`, stay indistinguishable downstream: only the
 * token's location survives the parse, never which word was typed.
 * Default-constructed means "private" -- the frozen default.
 */
struct VisibilityMark {
  bool IsPublic = false;
  SourceLocation Loc; ///< of the `pub`/`public` token; {0,0} when not written
};

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
  /// LEC-002: private unless written `pub` -- even inside a `pub struct`
  /// (FREEZE.md §6.15). Trailing member with a default, so every existing
  /// aggregate-initialised StructField still compiles as private.
  VisibilityMark Vis;
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

/// One declared field of an error variant's payload: `path: string` in
/// `NotFound(path: string)` (LEC-004, FREEZE.md §6.18). The type is shared
/// (not copied) between the declaration node and every registered
/// TurfUserErrorType, so the Resolver's in-place spelling -> identity rewrite
/// (NamedType::resolveTo()) is seen by all of them -- the same arrangement a
/// struct's field types have.
struct ErrorPayloadField {
    std::string    Name;
    TurfTypePtr    Type;
    SourceLocation Loc;
};

/// One variant of an error type, with its (possibly empty) payload fields.
struct ErrorVariantInfo {
    std::string                   Name;
    std::vector<ErrorPayloadField> Fields;
    SourceLocation                Loc;
};

/**
 * @brief A user-declared `error Name { Variant, Variant(field: T, ...) }` type
 * (docs/architecture/errors_and_recover.md). Deliberately NOT named
 * `TurfErrorType` -- one character away from `TurfError` (src/diagnostics/Errors.h),
 * the compiler's own internal diagnostic base class, which is an entirely
 * unrelated concept.
 *
 * A variant is a bare name or carries typed payload fields (LEC-004,
 * FREEZE.md §6.18 / F-ERR-1), read back only by destructuring patterns -- which
 * fields exist depends on which variant occurred, so there is no `err.field`.
 */
class TurfUserErrorType : public TurfType {
    std::string                   Name_;
    std::vector<ErrorVariantInfo> VariantInfos_;
    std::vector<std::string>      Variants_;   // names only, in declaration order
    bool                          IsBareForm_; // true for `error Name;` (implicit single DEFAULT variant)

public:
    TurfUserErrorType(std::string Name, std::vector<ErrorVariantInfo> Variants, bool IsBareForm)
        : Name_(std::move(Name)), VariantInfos_(std::move(Variants)), IsBareForm_(IsBareForm) {
        for (const auto &V : VariantInfos_)
            Variants_.push_back(V.Name);
    }

    std::string getName() const override { return Name_; }
    bool isUserError() const override { return true; }
    bool isBareForm() const { return IsBareForm_; }

    const std::vector<std::string>& getVariants() const { return Variants_; }
    const std::vector<ErrorVariantInfo>& getVariantInfos() const { return VariantInfos_; }

    bool hasVariant(const std::string& V) const {
        for (const auto& Variant : Variants_)
            if (Variant == V) return true;
        return false;
    }

    /// The declaration of variant @p V, or nullptr if there is no such variant.
    const ErrorVariantInfo *getVariantInfo(const std::string &V) const {
        for (const auto &Info : VariantInfos_)
            if (Info.Name == V) return &Info;
        return nullptr;
    }

    /// Alias for getVariants() -- for Damerau-Levenshtein suggestion
    /// diagnostics (mirrors TurfEnumType::getVariants(), already bare names).
    std::vector<std::string> getVariantNames() const { return Variants_; }

    std::string getMangledName() const override { return Name_ + "_error"; }
};

// ─── TurfErrorUnionType ───────────────────────────────────────────────────────

/**
 * @brief The `!T` fallible-return type
 * (docs/architecture/errors_and_recover.md): T on success, or a value of ANY
 * declared `error` type on failure. Distinct from `TurfUserErrorType` (the
 * declared error TYPE itself) -- a `!T` function doesn't name one specific
 * error type, it can fail with any of the program's declared error types. Also
 * unrelated to `TurfError` (src/diagnostics/Errors.h), the compiler's internal
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
/// The `guess` placeholder singleton (SEM-003) -- see `GuessType`'s doc comment.
TurfTypePtr TurfGuess();
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
