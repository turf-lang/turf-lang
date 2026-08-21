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

TurfTypePtr TurfInt32() {
  static TurfTypePtr T = std::make_shared<Int32Type>();
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

TurfTypePtr TurfChar() {
  static TurfTypePtr T = std::make_shared<CharType>();
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

TurfTypePtr TurfAnyError() {
  static TurfTypePtr T = std::make_shared<AnyErrorType>();
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

  // Type Erasure: Generic Type Parameters are compatible with anything.
  //
  // Checked FIRST -- before both the array rule and the general exact-match
  // check below -- so an unresolved generic parameter keeps matching against
  // literally anything, exactly as it always has. This has to come before
  // the array rule specifically: a generic function like
  // `fn<T> firstElem(arr: T[]): T` has a parameter type of
  // ArrayType(TurfGenericParamType("T")), and the array rule below requires
  // BOTH sides to be arrays -- a bare, unwrapped `T` on either side needs to
  // be caught here first, or it would incorrectly fall into "one side is an
  // array, the other isn't -> false".
  if (dynamic_cast<const TurfGenericParamType *>(From) ||
      dynamic_cast<const TurfGenericParamType *>(To))
    return true;

  // Array compatibility -- handled separately from the generic "exact match"
  // check below, because ArrayType::getName() intentionally omits the
  // compile-time size ("int[]" for both int[5] and int[10]; the size isn't
  // meaningful in a human-readable type name). A bare getName() comparison
  // therefore cannot tell int[5] and int[10] apart and would call them "the
  // same type" -- which used to let a size-mismatched assignment like
  // `mut a: int[5] = ...; mut b: int[10] = ...; a = b;` compile with no
  // diagnostic at all. That is not just a missed type error: Codegen's
  // CastToType() has the identical getName()-equality fast path, so the
  // mismatch survived all the way to a raw LLVM `store` of an 80-byte array
  // value into a's 40-byte stack allocation -- a genuine stack buffer
  // overflow, reproduced and confirmed before this fix.
  //
  // Two arrays are compatible when their element types are equivalent AND
  // either their sizes match exactly OR at least one side is "unsized"
  // (Size == -1, the `int[]` spelling used for array function parameters
  // that must accept an argument of any concrete length, e.g.
  // `fn sum(arr: int[]): int` called with an int[5] or int[10] argument --
  // a real, working feature that must keep working). Two concretely-sized
  // arrays of DIFFERENT sizes are correctly NOT compatible with each other.
  if (From->isArray() || To->isArray()) {
    if (!From->isArray() || !To->isArray())
      return false; // one side is an array, the other isn't
    const auto *FromArr = dynamic_cast<const ArrayType *>(From);
    const auto *ToArr = dynamic_cast<const ArrayType *>(To);
    bool SizeOk = FromArr->getSize() == ToArr->getSize() ||
                  FromArr->getSize() == -1 || ToArr->getSize() == -1;
    if (!SizeOk)
      return false;

    // Element-type check: same type-erasure leniency as the top-level check
    // above, one level deeper (an array of an unresolved generic parameter,
    // e.g. the T in `T[]`, must match an array of any concrete element
    // type). Anything else requires isEquivalent() -- NOT a recursive call
    // back into typesCompatible() -- which is the exact same strictness the
    // old getName()-based check already had for element types (differing
    // concrete element types, e.g. int[] vs double[], always produced
    // different getName() strings and were therefore already incompatible).
    // This fix only corrects the SIZE handling; it does not newly permit
    // numeric widening (int[5] -> double[5]) through arrays that wasn't
    // allowed before. isEquivalent()'s default implementation is itself
    // just a getName() comparison, so this stays transparently compatible
    // with the NamedType-placeholder-vs-resolved-TurfStructType leniency
    // struct element types already depend on elsewhere (see ParseFuncDef's
    // lazy `self` type).
    const TurfType *FromElem = FromArr->getElementType();
    const TurfType *ToElem = ToArr->getElementType();
    if (dynamic_cast<const TurfGenericParamType *>(FromElem) ||
        dynamic_cast<const TurfGenericParamType *>(ToElem))
      return true;
    return FromElem->isEquivalent(ToElem);
  }

  // Pointer-to-generic-parameter compatibility -- same type-erasure leniency
  // as the bare-T and array-of-T rules above: `T*` (a generic function's own
  // type parameter behind a pointer, e.g. `fn<T> identityPtr(x: T*): T*`)
  // must match a concrete pointer of ANY pointee type (int*, double*, a
  // struct pointer, ...), not just a pointer whose pointee's getName()
  // happens to equal "T". Checked narrowly -- only when BOTH sides are
  // genuinely TurfPointerType -- so it can't interfere with the
  // ptr/void*-coercion rules (Rule 2) or the null-compatibility rule (Rule 1)
  // below, which apply to a broader notion of "pointer-like" than this one
  // check cares about.
  if (const auto *FromPtr = dynamic_cast<const TurfPointerType *>(From)) {
    if (const auto *ToPtr = dynamic_cast<const TurfPointerType *>(To)) {
      if (dynamic_cast<const TurfGenericParamType *>(FromPtr->getPointee().get()) ||
          dynamic_cast<const TurfGenericParamType *>(ToPtr->getPointee().get()))
        return true;
    }
  }

  // Struct-literal / generic-struct compatibility: a plain struct-init like
  // `Box { val: null }` always produces the bare NamedType("Box") -- this
  // language has no syntax for `Box<int> { val: null }` (an explicit
  // type-argument list on a brace literal), so TypeChecker::visitStructInit()
  // can never produce a GenericType itself. Under this compiler's
  // type-erasure model, a generic struct's LLVM layout does not actually
  // vary per type argument -- every field typed with the struct's own type
  // parameter is itself always a pointer, which erases to an opaque pointer
  // regardless of what it points to (see getLLVMType()'s TurfGenericParamType/
  // GenericType handling) -- so there is no real information lost by
  // treating bare "Box" as compatible with "Box<AnyTypeArg>": the concrete
  // type argument is carried entirely by the DECLARED variable/parameter
  // type, and Codegen never needs to pick between different underlying
  // layouts for different instantiations. Without this,
  // `mut b: Box<int> = Box { val: null };` was flatly rejected ("Cannot
  // initialise 'Box<int>' variable 'b' with value of type 'Box'"), making
  // generic structs impossible to construct under ANY existing syntax.
  {
    const auto *FromGen = dynamic_cast<const GenericType *>(From);
    const auto *ToGen = dynamic_cast<const GenericType *>(To);
    if (FromGen && !ToGen && FromGen->getBaseName() == To->getName())
      return true;
    if (ToGen && !FromGen && ToGen->getBaseName() == From->getName())
      return true;

    // Two GenericTypes of the SAME base ("Map<string, V>" vs
    // "Map<string, string>", say) -- per-type-argument compatibility,
    // recursing one level so a bare unresolved generic parameter in ONE
    // argument slot (e.g. a function generic only over V, taking a
    // `Map<string, V>` parameter) still matches a fully concrete argument
    // (`Map<string, string>`). Same type-erasure leniency as the "T[]" /
    // "T*" rules above, just one level deeper for a GenericType's own
    // argument list -- that recursion was already relied on by
    // collectGenericSubstitution() (TypeCheckerCall.cpp) to fill in the
    // substitution map itself, but nothing upstream of it recursed the
    // same way for the plain "is this candidate even a match" check,
    // so a call site like this was rejected before substitution ever
    // ran ("No matching overload" against the ONE candidate whose
    // signature is, in fact, the correct match). Each argument pair uses
    // isEquivalent(), not a recursive typesCompatible() call -- deliberately
    // stricter than full compatibility (no numeric widening between type
    // ARGUMENTS; `Map<int, int>` must not satisfy a `Map<double, double>`
    // parameter just because int widens to double for ordinary values).
    if (FromGen && ToGen && FromGen->getBaseName() == ToGen->getBaseName()) {
      const auto &FA = FromGen->getTypeArgs();
      const auto &TA = ToGen->getTypeArgs();
      if (FA.size() == TA.size()) {
        bool AllArgsCompatible = true;
        for (size_t i = 0; i < FA.size(); ++i) {
          if (dynamic_cast<const TurfGenericParamType *>(FA[i].get()) ||
              dynamic_cast<const TurfGenericParamType *>(TA[i].get()))
            continue;
          if (!FA[i]->isEquivalent(TA[i].get())) {
            AllArgsCompatible = false;
            break;
          }
        }
        if (AllArgsCompatible)
          return true;
      }
    }
  }

  // Exact match
  if (From->getName() == To->getName())
    return true;

  // Rule 1: null is only compatible with pointer-like types (ptr,
  // void*, T*). It must NEVER be compatible with plain numerics, bool, or
  // string. A `&T` safe reference (issue #238) is deliberately EXCLUDED
  // here even though isPointer() is true for it (inherited from
  // TurfPointerType) -- `&T`'s whole point is a compiler-guaranteed
  // non-null contract, so `null` must never be assignable to it.
  auto isPointerLikeStrict = [](const TurfType *T) {
    return (T->isPointer() && !T->isReference()) ||
           T->isNamed("ptr") || T->isNamed("void*");
  };
  if (From->isNull())
    return isPointerLikeStrict(To);
  if (To->isNull())
    return isPointerLikeStrict(From);

  // Rule 2: ptr / void* mutually coercible with typed pointers (T*) only.
  // Deliberately NOT extended to `string` — string and T* remain distinct.
  // The raw-ptr/void* -> To direction excludes `&T` targets (`!To->isReference()`)
  // for the same non-null-contract reason as Rule 1 -- a raw/opaque pointer
  // might be null, so it must go through an explicit `to`-cast inside
  // `unsafe { }` to become a `&T`, never an implicit coercion. The reverse
  // direction (`&T` decaying into `ptr`/`void*`) is unaffected: it's
  // always sound, since a non-null reference is always a valid raw pointer.
  auto isOpaquePtr = [](const TurfType *T) {
    return T->isNamed("ptr") || T->isNamed("void*");
  };
  if (isOpaquePtr(From) && To->isPointer() && !To->isReference())
    return true;
  if (isOpaquePtr(To) && From->isPointer())
    return true;

  // Rule 2.5 (issue #238): a safe reference (&T) always widens implicitly
  // into a concrete raw pointer of an equivalent pointee type -- this
  // direction is unconditionally sound (a non-null &T is always a valid
  // T*), so no `unsafe` gate applies to the widening itself (only to the
  // raw pointer TYPE's use at the target location, which the TypeChecker
  // gates separately). The reverse (raw T*/ptr -> &T) is deliberately NOT
  // handled here -- with no matching rule anywhere above, it correctly
  // falls through to `false` below, forcing an explicit `to`-cast inside
  // `unsafe { }`. Pointee comparison uses isEquivalent(), not a recursive
  // typesCompatible() call, so this doesn't accidentally also permit
  // unsound pointee widening (e.g. &int -> double* through int->double's
  // numeric-widening rule below) -- only a genuinely equivalent pointee.
  if (const auto *FromRef = dynamic_cast<const TurfReferenceType *>(From)) {
    if (!To->isReference()) {
      if (const auto *ToPtr = dynamic_cast<const TurfPointerType *>(To)) {
        if (FromRef->getPointee()->isEquivalent(ToPtr->getPointee().get()))
          return true;
      }
    }
  }

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
  // int32 -> int / int32 -> double (widening, no precision loss): allowed --
  // int32 exists purely as an honest extern-fn ABI declaration type (see
  // Int32Type's doc comment in Types.h), so it should widen into normal
  // `int` usage exactly as freely as `int` widens into `double`.
  // int -> int32 (narrowing): rejected, requires explicit cast, same
  // reasoning as double -> int.
  if (From->isNumeric() && To->isNumeric()) {
    if (From->isInt() && To->isDouble())
      return true;
    if (From->isDouble() && To->isInt())
      return false;
    if (From->isInt32() && (To->isInt() || To->isDouble()))
      return true;
    if (To->isInt32() && !From->isInt32())
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

// ─── TurfMachineType ─────────────────────────────────────────────────────────

TurfMachineType::TurfMachineType(std::string Name,
                                 std::vector<std::string> States,
                                 std::map<std::string, std::set<std::string>> Edges)
    : TurfEnumType(std::move(Name), std::move(States), nullptr),
      Edges_(std::move(Edges)) {}

bool TurfMachineType::hasEdge(const std::string &From, const std::string &To) const {
    auto It = Edges_.find(From);
    if (It == Edges_.end())
        return false;
    return It->second.count(To) > 0;
}
