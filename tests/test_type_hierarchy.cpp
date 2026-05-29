/**
 * @file test_type_hierarchy.cpp
 * @brief LLVM-free unit tests for the TurfLang Type Hierarchy (Issue #3).
 *
 * These tests verify that:
 *
 *  1. Primitive singletons return the correct concrete subclass.
 *  2. Kind predicates (isInt, isDouble, isBool, isString, isVoid, isNull)
 *     return true for their own type and false for all others.
 *  3. isNumeric() covers int/double/bool and excludes string/void/null.
 *  4. getName() returns the canonical source-level spelling.
 *  5. getMangledName() returns a valid linker-safe mangled name.
 *  6. isNamed() backward-compat predicate still works.
 *  7. GenericType represents map<string,int> correctly.
 *  8. ListType represents list<int> with isList() == true and no LLVM.
 *  9. makeListType(TurfInt()) satisfies the Issue #3 acceptance criterion:
 *       frontend can represent List<int> without asking LLVM anything.
 * 10. ArrayType stores element type and optional fixed size.
 * 11. TurfPointerType wraps its pointee.
 * 12. TurfStructType field lookup works.
 * 13. isListType() helper accepts both ListType and legacy GenericType("list").
 * 14. getListElementType() extracts T from list<T>.
 * 15. TurfGenericParamType (open type parameter) round-trips correctly.
 * 16. isEquivalent() structural equality check.
 * 17. turfTypeName() handles null gracefully.
 *
 * Build (standalone — no LLVM required):
 *   g++ -std=c++20 -I../../include \
 *       test_type_hierarchy.cpp \
 *       ../../src/Types.cpp \
 *       -o test_types && ./test_types
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Types.h"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

// ─── Minimal test framework ───────────────────────────────────────────────────

static int TestsPassed = 0;
static int TestsFailed = 0;

#define TEST(name) void name()
#define RUN(name)                                                               \
  do {                                                                          \
    std::cout << "  running " #name " ... ";                                    \
    try {                                                                       \
      name();                                                                   \
      std::cout << "PASS\n";                                                    \
      ++TestsPassed;                                                             \
    } catch (const std::exception &E) {                                         \
      std::cout << "FAIL: " << E.what() << "\n";                               \
      ++TestsFailed;                                                             \
    } catch (...) {                                                              \
      std::cout << "FAIL (unknown exception)\n";                                \
      ++TestsFailed;                                                             \
    }                                                                           \
  } while (false)

#define EXPECT_TRUE(expr)                                                       \
  do {                                                                          \
    if (!(expr))                                                                \
      throw std::runtime_error("EXPECT_TRUE failed: " #expr);                  \
  } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b)                                                         \
  do {                                                                          \
    if ((a) != (b))                                                             \
      throw std::runtime_error("EXPECT_EQ failed: " #a " != " #b);            \
  } while (false)

// ─── Test: Primitive singleton identity ─────────────────────────────────────

TEST(test_primitives_are_singletons) {
  // Each call should return the same pointer
  EXPECT_TRUE(TurfInt()    == TurfInt());
  EXPECT_TRUE(TurfDouble() == TurfDouble());
  EXPECT_TRUE(TurfBool()   == TurfBool());
  EXPECT_TRUE(TurfString() == TurfString());
  EXPECT_TRUE(TurfVoid()   == TurfVoid());
  EXPECT_TRUE(TurfNull()   == TurfNull());
}

// ─── Test: Concrete subclasses ───────────────────────────────────────────────

TEST(test_int_type_concrete_class) {
  auto T = TurfInt();
  EXPECT_TRUE(dynamic_cast<const IntType*>(T.get()) != nullptr);
}

TEST(test_double_type_concrete_class) {
  auto T = TurfDouble();
  EXPECT_TRUE(dynamic_cast<const DoubleType*>(T.get()) != nullptr);
}

TEST(test_bool_type_concrete_class) {
  auto T = TurfBool();
  EXPECT_TRUE(dynamic_cast<const BoolType*>(T.get()) != nullptr);
}

TEST(test_string_type_concrete_class) {
  auto T = TurfString();
  EXPECT_TRUE(dynamic_cast<const StringType*>(T.get()) != nullptr);
}

TEST(test_void_type_concrete_class) {
  auto T = TurfVoid();
  EXPECT_TRUE(dynamic_cast<const VoidType*>(T.get()) != nullptr);
}

TEST(test_null_type_concrete_class) {
  auto T = TurfNull();
  EXPECT_TRUE(dynamic_cast<const TurfNullType*>(T.get()) != nullptr);
}

// ─── Test: Kind predicates ───────────────────────────────────────────────────

TEST(test_isInt_is_exclusive) {
  EXPECT_TRUE (TurfInt()->isInt());
  EXPECT_FALSE(TurfInt()->isDouble());
  EXPECT_FALSE(TurfInt()->isBool());
  EXPECT_FALSE(TurfInt()->isString());
  EXPECT_FALSE(TurfInt()->isVoid());
  EXPECT_FALSE(TurfInt()->isNull());
}

TEST(test_isDouble_is_exclusive) {
  EXPECT_FALSE(TurfDouble()->isInt());
  EXPECT_TRUE (TurfDouble()->isDouble());
  EXPECT_FALSE(TurfDouble()->isBool());
  EXPECT_FALSE(TurfDouble()->isString());
  EXPECT_FALSE(TurfDouble()->isVoid());
  EXPECT_FALSE(TurfDouble()->isNull());
}

TEST(test_isBool_is_exclusive) {
  EXPECT_FALSE(TurfBool()->isInt());
  EXPECT_FALSE(TurfBool()->isDouble());
  EXPECT_TRUE (TurfBool()->isBool());
  EXPECT_FALSE(TurfBool()->isString());
  EXPECT_FALSE(TurfBool()->isVoid());
  EXPECT_FALSE(TurfBool()->isNull());
}

TEST(test_isString_is_exclusive) {
  EXPECT_FALSE(TurfString()->isInt());
  EXPECT_FALSE(TurfString()->isDouble());
  EXPECT_FALSE(TurfString()->isBool());
  EXPECT_TRUE (TurfString()->isString());
  EXPECT_FALSE(TurfString()->isVoid());
  EXPECT_FALSE(TurfString()->isNull());
}

TEST(test_isVoid_is_exclusive) {
  EXPECT_FALSE(TurfVoid()->isInt());
  EXPECT_FALSE(TurfVoid()->isDouble());
  EXPECT_FALSE(TurfVoid()->isBool());
  EXPECT_FALSE(TurfVoid()->isString());
  EXPECT_TRUE (TurfVoid()->isVoid());
  EXPECT_FALSE(TurfVoid()->isNull());
}

TEST(test_isNull_is_exclusive) {
  EXPECT_FALSE(TurfNull()->isInt());
  EXPECT_FALSE(TurfNull()->isDouble());
  EXPECT_FALSE(TurfNull()->isBool());
  EXPECT_FALSE(TurfNull()->isString());
  EXPECT_FALSE(TurfNull()->isVoid());
  EXPECT_TRUE (TurfNull()->isNull());
}

// ─── Test: isNumeric ─────────────────────────────────────────────────────────

TEST(test_isNumeric_covers_int_double_bool) {
  EXPECT_TRUE(TurfInt()->isNumeric());
  EXPECT_TRUE(TurfDouble()->isNumeric());
  EXPECT_TRUE(TurfBool()->isNumeric());
}

TEST(test_isNumeric_excludes_non_numerics) {
  EXPECT_FALSE(TurfString()->isNumeric());
  EXPECT_FALSE(TurfVoid()->isNumeric());
  EXPECT_FALSE(TurfNull()->isNumeric());
}

// ─── Test: getName ───────────────────────────────────────────────────────────

TEST(test_getname_canonical_spellings) {
  EXPECT_EQ(TurfInt()->getName(),    std::string("int"));
  EXPECT_EQ(TurfDouble()->getName(), std::string("double"));
  EXPECT_EQ(TurfBool()->getName(),   std::string("bool"));
  EXPECT_EQ(TurfString()->getName(), std::string("string"));
  EXPECT_EQ(TurfVoid()->getName(),   std::string("void"));
  EXPECT_EQ(TurfNull()->getName(),   std::string("null"));
}

// ─── Test: getMangledName ────────────────────────────────────────────────────

TEST(test_getmangledname_primitives) {
  // For primitives, mangled == display name
  EXPECT_EQ(TurfInt()->getMangledName(),    std::string("int"));
  EXPECT_EQ(TurfDouble()->getMangledName(), std::string("double"));
  EXPECT_EQ(TurfBool()->getMangledName(),   std::string("bool"));
  EXPECT_EQ(TurfString()->getMangledName(), std::string("string"));
}

// ─── Test: isNamed backward compat ───────────────────────────────────────────

TEST(test_isnamed_backward_compat) {
  EXPECT_TRUE (TurfInt()->isNamed("int"));
  EXPECT_FALSE(TurfInt()->isNamed("double"));
  EXPECT_TRUE (TurfString()->isNamed("string"));
  EXPECT_TRUE (TurfNull()->isNamed("null"));
  EXPECT_FALSE(TurfNull()->isNamed("int"));
}

// ─── Test: NamedType (legacy / user-defined struct names) ────────────────────

TEST(test_named_type_user_defined) {
  auto T = std::make_shared<NamedType>("Player");
  EXPECT_EQ(T->getName(), std::string("Player"));
  EXPECT_TRUE(T->isNamed("Player"));
  EXPECT_FALSE(T->isInt());
  EXPECT_FALSE(T->isString());
}

// ─── Test: ArrayType ─────────────────────────────────────────────────────────

TEST(test_array_type_int) {
  auto ArrTy = makeArrayType(TurfInt());
  EXPECT_TRUE(ArrTy->isArray());
  EXPECT_FALSE(ArrTy->isInt());
  EXPECT_EQ(ArrTy->getName(), std::string("int[]"));
  EXPECT_EQ(ArrTy->getMangledName(), std::string("int_arr"));
}

TEST(test_array_type_element_accessor) {
  auto ArrTy = makeArrayType(TurfString(), 5);
  EXPECT_TRUE(isArrayType(ArrTy.get()));
  const TurfType *Elem = getArrayElementType(ArrTy.get());
  EXPECT_TRUE(Elem != nullptr);
  EXPECT_TRUE(Elem->isString());
  auto *A = dynamic_cast<const ArrayType*>(ArrTy.get());
  EXPECT_EQ(A->getSize(), 5);
}

TEST(test_array_type_not_list) {
  auto ArrTy = makeArrayType(TurfInt());
  EXPECT_FALSE(isListType(ArrTy.get()));
}

// ─── Test: TurfPointerType ───────────────────────────────────────────────────

TEST(test_pointer_type) {
  auto PtrTy = std::make_shared<TurfPointerType>(TurfInt());
  EXPECT_TRUE(PtrTy->isPointer());
  EXPECT_FALSE(PtrTy->isInt());
  EXPECT_EQ(PtrTy->getName(), std::string("int*"));
  EXPECT_EQ(PtrTy->getMangledName(), std::string("int_ptr"));
  EXPECT_TRUE(PtrTy->getPointee()->isInt());
}

// ─── Test: TurfGenericParamType ──────────────────────────────────────────────

TEST(test_generic_param_type) {
  auto T = std::make_shared<TurfGenericParamType>("T");
  EXPECT_TRUE(T->isGenericParam());
  EXPECT_FALSE(T->isGeneric());
  EXPECT_EQ(T->getName(), std::string("T"));
}

// ─── Test: GenericType (map<K,V>) ────────────────────────────────────────────

TEST(test_generic_type_map) {
  auto MapTy = std::make_shared<GenericType>(
      "map", std::vector<TurfTypePtr>{TurfString(), TurfInt()});

  EXPECT_TRUE(MapTy->isGeneric());
  EXPECT_FALSE(MapTy->isList());
  EXPECT_EQ(MapTy->getName(), std::string("map<string, int>"));
  EXPECT_EQ(MapTy->getMangledName(), std::string("map_string_int"));
  EXPECT_EQ(MapTy->getBaseName(), std::string("map"));
  EXPECT_EQ(MapTy->getTypeArgs().size(), 2u);
  EXPECT_TRUE(MapTy->getTypeArg(0)->isString());
  EXPECT_TRUE(MapTy->getTypeArg(1)->isInt());
}

// ─── Test: ListType — Issue #3 acceptance criterion ─────────────────────────

/**
 * Issue #3 acceptance criterion:
 *   "The compiler frontend can represent a List<int> natively without asking
 *    LLVM what a pointer is."
 *
 * This test constructs a List<int> type using only frontend APIs and verifies
 * every relevant property.  No LLVM initialisation is performed.
 */
TEST(test_list_type_int_acceptance_criterion) {
  TurfTypePtr ListOfInt = makeListType(TurfInt());

  // ── Kind predicates ──────────────────────────────────────────────────────
  EXPECT_TRUE (ListOfInt->isList());
  EXPECT_TRUE (ListOfInt->isGeneric()); // ListType derives from GenericType
  EXPECT_FALSE(ListOfInt->isArray());
  EXPECT_FALSE(ListOfInt->isInt());
  EXPECT_FALSE(ListOfInt->isString());

  // ── Name / mangling ──────────────────────────────────────────────────────
  EXPECT_EQ(ListOfInt->getName(),        std::string("list<int>"));
  EXPECT_EQ(ListOfInt->getMangledName(), std::string("list_int"));

  // ── Element type access ──────────────────────────────────────────────────
  auto *L = dynamic_cast<const ListType*>(ListOfInt.get());
  EXPECT_TRUE(L != nullptr);
  EXPECT_TRUE(L->getElementType()->isInt());
  EXPECT_TRUE(L->getElementTypePtr()->isInt());

  // ── Helper functions ─────────────────────────────────────────────────────
  EXPECT_TRUE(isListType(ListOfInt.get()));
  const TurfType *Elem = getListElementType(ListOfInt.get());
  EXPECT_TRUE(Elem != nullptr && Elem->isInt());
}

TEST(test_list_type_string) {
  TurfTypePtr ListOfStr = makeListType(TurfString());
  EXPECT_TRUE(ListOfStr->isList());
  EXPECT_EQ(ListOfStr->getName(),        std::string("list<string>"));
  EXPECT_EQ(ListOfStr->getMangledName(), std::string("list_string"));
  EXPECT_TRUE(getListElementType(ListOfStr.get())->isString());
}

TEST(test_list_type_nested) {
  // list<list<int>>
  TurfTypePtr Inner = makeListType(TurfInt());
  TurfTypePtr Outer = makeListType(Inner);

  EXPECT_TRUE(Outer->isList());
  EXPECT_EQ(Outer->getName(), std::string("list<list<int>>"));
  EXPECT_EQ(Outer->getMangledName(), std::string("list_list_int"));

  const TurfType *InnerElem = getListElementType(Outer.get());
  EXPECT_TRUE(InnerElem != nullptr);
  EXPECT_TRUE(InnerElem->isList());
  EXPECT_TRUE(getListElementType(InnerElem)->isInt());
}

TEST(test_list_type_struct_element) {
  // list<Player>
  auto PlayerType = std::make_shared<TurfStructType>(
      "Player", std::vector<StructField>{{"name", TurfString()}, {"score", TurfInt()}});
  TurfTypePtr ListOfPlayers = makeListType(PlayerType);

  EXPECT_TRUE(ListOfPlayers->isList());
  EXPECT_EQ(ListOfPlayers->getName(), std::string("list<Player>"));
  EXPECT_EQ(getListElementType(ListOfPlayers.get())->getName(), std::string("Player"));
}

// ─── Test: isListType with legacy GenericType ────────────────────────────────

TEST(test_is_list_type_legacy_generic) {
  // Pre-migration code might have created GenericType("list", …) directly.
  // isListType() must still recognise these.
  auto Legacy = std::make_shared<GenericType>(
      "list", std::vector<TurfTypePtr>{TurfDouble()});

  EXPECT_TRUE(isListType(Legacy.get()));
  const TurfType *Elem = getListElementType(Legacy.get());
  EXPECT_TRUE(Elem != nullptr && Elem->isDouble());
}

TEST(test_is_list_type_false_for_non_list) {
  EXPECT_FALSE(isListType(TurfInt().get()));
  EXPECT_FALSE(isListType(makeArrayType(TurfInt()).get()));
  EXPECT_FALSE(isListType(nullptr));
}

// ─── Test: TurfStructType ────────────────────────────────────────────────────

TEST(test_struct_type_fields) {
  auto Struct = std::make_shared<TurfStructType>(
      "Point",
      std::vector<StructField>{{"x", TurfDouble()}, {"y", TurfDouble()}});

  EXPECT_TRUE(Struct->isStruct());
  EXPECT_FALSE(Struct->isInt());
  EXPECT_EQ(Struct->getName(), std::string("Point"));

  EXPECT_EQ(Struct->getFieldIndex("x"), 0);
  EXPECT_EQ(Struct->getFieldIndex("y"), 1);
  EXPECT_EQ(Struct->getFieldIndex("z"), -1);

  TurfTypePtr XType = Struct->getFieldType("x");
  EXPECT_TRUE(XType != nullptr && XType->isDouble());
  EXPECT_TRUE(Struct->getFieldType("missing") == nullptr);
}

// ─── Test: isEquivalent ──────────────────────────────────────────────────────

TEST(test_is_equivalent_same_type) {
  EXPECT_TRUE(TurfInt()->isEquivalent(TurfInt().get()));
  EXPECT_TRUE(TurfString()->isEquivalent(TurfString().get()));
}

TEST(test_is_equivalent_different_types) {
  EXPECT_FALSE(TurfInt()->isEquivalent(TurfDouble().get()));
  EXPECT_FALSE(TurfString()->isEquivalent(TurfBool().get()));
}

TEST(test_is_equivalent_null) {
  EXPECT_FALSE(TurfInt()->isEquivalent(nullptr));
}

TEST(test_is_equivalent_list_types) {
  auto L1 = makeListType(TurfInt());
  auto L2 = makeListType(TurfInt());
  auto L3 = makeListType(TurfString());

  EXPECT_TRUE(L1->isEquivalent(L2.get()));  // same name "list<int>"
  EXPECT_FALSE(L1->isEquivalent(L3.get())); // "list<int>" vs "list<string>"
}

// ─── Test: turfTypeName ──────────────────────────────────────────────────────

TEST(test_turf_type_name_null) {
  EXPECT_EQ(turfTypeName(nullptr), std::string("unknown"));
}

TEST(test_turf_type_name_int) {
  EXPECT_EQ(turfTypeName(TurfInt().get()), std::string("int"));
}

TEST(test_turf_type_name_list) {
  auto L = makeListType(TurfInt());
  EXPECT_EQ(turfTypeName(L.get()), std::string("list<int>"));
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "=== TurfLang Type Hierarchy Unit Tests (Issue #3) ===\n\n";
  std::cout << "[Primitive singletons]\n";
  RUN(test_primitives_are_singletons);
  RUN(test_int_type_concrete_class);
  RUN(test_double_type_concrete_class);
  RUN(test_bool_type_concrete_class);
  RUN(test_string_type_concrete_class);
  RUN(test_void_type_concrete_class);
  RUN(test_null_type_concrete_class);

  std::cout << "\n[Kind predicates]\n";
  RUN(test_isInt_is_exclusive);
  RUN(test_isDouble_is_exclusive);
  RUN(test_isBool_is_exclusive);
  RUN(test_isString_is_exclusive);
  RUN(test_isVoid_is_exclusive);
  RUN(test_isNull_is_exclusive);

  std::cout << "\n[isNumeric]\n";
  RUN(test_isNumeric_covers_int_double_bool);
  RUN(test_isNumeric_excludes_non_numerics);

  std::cout << "\n[getName / getMangledName]\n";
  RUN(test_getname_canonical_spellings);
  RUN(test_getmangledname_primitives);

  std::cout << "\n[isNamed backward compat]\n";
  RUN(test_isnamed_backward_compat);

  std::cout << "\n[NamedType]\n";
  RUN(test_named_type_user_defined);

  std::cout << "\n[ArrayType]\n";
  RUN(test_array_type_int);
  RUN(test_array_type_element_accessor);
  RUN(test_array_type_not_list);

  std::cout << "\n[TurfPointerType]\n";
  RUN(test_pointer_type);

  std::cout << "\n[TurfGenericParamType]\n";
  RUN(test_generic_param_type);

  std::cout << "\n[GenericType]\n";
  RUN(test_generic_type_map);

  std::cout << "\n[ListType — Issue #3 acceptance criterion]\n";
  RUN(test_list_type_int_acceptance_criterion);
  RUN(test_list_type_string);
  RUN(test_list_type_nested);
  RUN(test_list_type_struct_element);

  std::cout << "\n[isListType helpers]\n";
  RUN(test_is_list_type_legacy_generic);
  RUN(test_is_list_type_false_for_non_list);

  std::cout << "\n[TurfStructType]\n";
  RUN(test_struct_type_fields);

  std::cout << "\n[isEquivalent]\n";
  RUN(test_is_equivalent_same_type);
  RUN(test_is_equivalent_different_types);
  RUN(test_is_equivalent_null);
  RUN(test_is_equivalent_list_types);

  std::cout << "\n[turfTypeName]\n";
  RUN(test_turf_type_name_null);
  RUN(test_turf_type_name_int);
  RUN(test_turf_type_name_list);

  std::cout << "\n=== Results: " << TestsPassed << " passed, "
            << TestsFailed << " failed ===\n";
  return TestsFailed == 0 ? 0 : 1;
}
