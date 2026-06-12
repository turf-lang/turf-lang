/**
 * @file test_type_checker.cpp
 * @brief LLVM-free unit tests for the TypeChecker pass (Issue #7 / #135).
 *
 * These tests verify:
 *  1.  Literal type inference (int, double, bool, string, null)
 *  2.  Homogeneous binary arithmetic (int+int, double+double, string+string)
 *  3.  Numeric promotion (int+double → double, double+int → double)
 *  4.  Type mismatch detection (string+int, int+string → error)
 *  5.  Comparison operators always produce bool
 *  6.  Unary negation type propagation
 *  7.  For-loop variable typed as int (integer range) or double (float range)
 *  8.  VarDecl type propagation
 *  9.  Cast expression produces the destination type
 * 10.  Nested binary expressions (bottom-up inference)
 * 11.  Block expression type is last child's type
 * 12.  String + string is valid (concatenation)
 * 13.  String + int produces a TypeError
 * 14.  Int + string produces a TypeError
 * 15.  Double arithmetic promotes correctly
 * 16.  Unary '!' always produces bool
 * 17.  Variable lookup from ResolverSymbolTable
 * 18.  Return type checking inside functions
 * 19.  ForExprAST loop variable visible inside body (visitVariable resolves it)
 * 20.  Assignment expression propagates RHS type
 *
 * Build (standalone — no LLVM required):
 *   g++ -std=c++20 -I../../include \
 *       -DTYPECHECKER_NO_ERRORS \
 *       test_type_checker.cpp \
 *       ../../src/TypeChecker.cpp \
 *       ../../src/ResolverSymbolTable.cpp \
 *       ../../src/Types.cpp \
 *       -o test_tc && ./test_tc
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

// ─── IMPORTANT: define this before any project headers ───────────────────────
// Prevents TypeChecker.cpp from including Errors.h (which pulls in LLVM).
#define TYPECHECKER_NO_ERRORS

#include "TypeChecker.h"
#include "ResolverSymbolTable.h"
#include "Types.h"

// AST.h must come after the define above because TypeChecker.cpp is compiled
// separately; here we only need it for constructing test nodes.
#include "AST.h"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

// ─── Minimal test framework ───────────────────────────────────────────────────

static int TestsPassed = 0;
static int TestsFailed = 0;

#define TEST(name) void name()
#define RUN(name)                                                              \
  do {                                                                         \
    std::cout << "  running " #name " ... ";                                   \
    try {                                                                      \
      name();                                                                  \
      std::cout << "PASS\n";                                                   \
      ++TestsPassed;                                                           \
    } catch (const std::exception &E) {                                        \
      std::cout << "FAIL: " << E.what() << "\n";                              \
      ++TestsFailed;                                                           \
    } catch (...) {                                                            \
      std::cout << "FAIL (unknown exception)\n";                              \
      ++TestsFailed;                                                           \
    }                                                                          \
  } while (false)

#define EXPECT_TRUE(expr)                                                      \
  do {                                                                         \
    if (!(expr))                                                               \
      throw std::runtime_error("EXPECT_TRUE failed: " #expr);                 \
  } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b))                                                            \
      throw std::runtime_error("EXPECT_EQ failed: " #a " != " #b);           \
  } while (false)

#define EXPECT_THROWS(expr)                                                    \
  do {                                                                         \
    bool _threw = false;                                                       \
    try {                                                                      \
      expr;                                                                    \
    } catch (...) {                                                            \
      _threw = true;                                                           \
    }                                                                          \
    if (!_threw)                                                               \
      throw std::runtime_error(                                                \
          "EXPECT_THROWS: no exception thrown for " #expr);                   \
  } while (false)

// ─── Shared error-throwing handler (LLVM-free) ───────────────────────────────

static TypeChecker::ErrorHandler ThrowingHandler =
    [](SourceLocation, const std::string &Msg) {
      throw std::runtime_error("TypeError: " + Msg);
    };

/// Build a TypeChecker with ThrowingHandler and an empty ResolverSymbolTable.
/// Caller is responsible for keeping RST alive while tc exists.
static ResolverSymbolTable makeEmptyRST() { return ResolverSymbolTable{}; }

// ─── AST helper: build SourceLocation ────────────────────────────────────────
static SourceLocation loc(int line = 1, int col = 1) { return {line, col}; }

// ─── Test 1: Integer literal → int ───────────────────────────────────────────

TEST(test_integer_literal_infers_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Node = std::make_unique<NumberExprAST>(static_cast<long long>(42));
  Node->accept(TC);
  EXPECT_TRUE(TC.hasType(Node.get()));
  EXPECT_TRUE(TC.getType(Node.get())->isInt());
}

// ─── Test 2: Double literal → double ─────────────────────────────────────────

TEST(test_double_literal_infers_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Node = std::make_unique<NumberExprAST>(3.14);
  Node->accept(TC);
  EXPECT_TRUE(TC.getType(Node.get())->isDouble());
}

// ─── Test 3: Bool literal → bool ─────────────────────────────────────────────

TEST(test_bool_literal_infers_bool) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Node = std::make_unique<BoolExprAST>(true);
  Node->accept(TC);
  EXPECT_TRUE(TC.getType(Node.get())->isBool());
}

// ─── Test 4: String literal → string ─────────────────────────────────────────

TEST(test_string_literal_infers_string) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Node = std::make_unique<StringExprAST>("hello");
  Node->accept(TC);
  EXPECT_TRUE(TC.getType(Node.get())->isString());
}

// ─── Test 5: Null literal → null ─────────────────────────────────────────────

TEST(test_null_literal_infers_null) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Node = std::make_unique<NullExprAST>(loc());
  Node->accept(TC);
  EXPECT_TRUE(TC.getType(Node.get())->isNull());
}

// ─── Test 6: int + int → int ─────────────────────────────────────────────────

TEST(test_int_plus_int_is_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(static_cast<long long>(1));
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(2));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isInt());
}

// ─── Test 7: double + double → double ────────────────────────────────────────

TEST(test_double_plus_double_is_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(1.5);
  auto RHS = std::make_unique<NumberExprAST>(2.5);
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isDouble());
}

// ─── Test 8: int + double → double (promotion) ───────────────────────────────

TEST(test_int_plus_double_promotes_to_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(static_cast<long long>(1));
  auto RHS = std::make_unique<NumberExprAST>(2.0);
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isDouble());
}

// ─── Test 9: double + int → double (promotion) ───────────────────────────────

TEST(test_double_plus_int_promotes_to_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(2.0);
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(1));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isDouble());
}

// ─── Test 10: string + string → string ───────────────────────────────────────

TEST(test_string_concat_is_string) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<StringExprAST>("hello");
  auto RHS = std::make_unique<StringExprAST>(" world");
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isString());
}

// ─── Test 11: string + int → TypeError ───────────────────────────────────────

TEST(test_string_plus_int_throws_type_error) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<StringExprAST>("hello");
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(5));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  EXPECT_THROWS(Bin->accept(TC));
}

// ─── Test 12: int + string → TypeError ───────────────────────────────────────

TEST(test_int_plus_string_throws_type_error) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(static_cast<long long>(5));
  auto RHS = std::make_unique<StringExprAST>("hello");
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '+', std::move(LHS),
                                              std::move(RHS));
  EXPECT_THROWS(Bin->accept(TC));
}

// ─── Test 13: string - string → TypeError (only '+' is valid for strings) ────

TEST(test_string_minus_string_throws_type_error) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<StringExprAST>("hello");
  auto RHS = std::make_unique<StringExprAST>("world");
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '-', std::move(LHS),
                                              std::move(RHS));
  EXPECT_THROWS(Bin->accept(TC));
}

// ─── Test 14: int < int → bool ───────────────────────────────────────────────

TEST(test_int_lt_int_is_bool) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(static_cast<long long>(3));
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(7));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '<', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isBool());
}

// ─── Test 15: double > double → bool ─────────────────────────────────────────

TEST(test_double_gt_double_is_bool) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(9.9);
  auto RHS = std::make_unique<NumberExprAST>(1.1);
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '>', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isBool());
}

// ─── Test 16: Unary '-' on int → int ─────────────────────────────────────────

TEST(test_unary_negate_int_is_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Operand = std::make_unique<NumberExprAST>(static_cast<long long>(42));
  auto Unary   = std::make_unique<UnaryExprAST>('-', std::move(Operand));
  Unary->accept(TC);
  EXPECT_TRUE(TC.getType(Unary.get())->isInt());
}

// ─── Test 17: Unary '-' on double → double ───────────────────────────────────

TEST(test_unary_negate_double_is_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Operand = std::make_unique<NumberExprAST>(3.14);
  auto Unary   = std::make_unique<UnaryExprAST>('-', std::move(Operand));
  Unary->accept(TC);
  EXPECT_TRUE(TC.getType(Unary.get())->isDouble());
}

// ─── Test 18: Unary '!' on any type → bool ───────────────────────────────────

TEST(test_unary_not_is_bool) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Operand = std::make_unique<BoolExprAST>(true);
  auto Unary   = std::make_unique<UnaryExprAST>('!', std::move(Operand));
  Unary->accept(TC);
  EXPECT_TRUE(TC.getType(Unary.get())->isBool());
}

// ─── Test 19: Unary '-' on string → TypeError ────────────────────────────────

TEST(test_unary_negate_string_throws) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Operand = std::make_unique<StringExprAST>("oops");
  auto Unary   = std::make_unique<UnaryExprAST>('-', std::move(Operand));
  EXPECT_THROWS(Unary->accept(TC));
}

// ─── Test 20: Nested binary (int + int) * int → int ──────────────────────────

TEST(test_nested_binary_int_arithmetic) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto L1  = std::make_unique<NumberExprAST>(static_cast<long long>(1));
  auto L2  = std::make_unique<NumberExprAST>(static_cast<long long>(2));
  auto Sum = std::make_unique<BinaryExprAST>(loc(), '+', std::move(L1),
                                              std::move(L2));
  auto L3  = std::make_unique<NumberExprAST>(static_cast<long long>(3));
  auto Mul = std::make_unique<BinaryExprAST>(loc(), '*', std::move(Sum),
                                              std::move(L3));
  Mul->accept(TC);
  EXPECT_TRUE(TC.getType(Mul.get())->isInt());
}

// ─── Test 21: CastExpr double(42) → double ───────────────────────────────────

TEST(test_cast_int_to_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Operand = std::make_unique<NumberExprAST>(static_cast<long long>(42));
  auto Cast    = std::make_unique<CastExprAST>(loc(), TurfDouble(),
                                                std::move(Operand));
  Cast->accept(TC);
  EXPECT_TRUE(TC.getType(Cast.get())->isDouble());
}

// ─── Test 22: CastExpr int("hello") → int (TypeChecker trusts explicit cast) ─

TEST(test_cast_string_to_int_is_trusted) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Operand = std::make_unique<StringExprAST>("42");
  auto Cast    = std::make_unique<CastExprAST>(loc(), TurfInt(),
                                                std::move(Operand));
  // No error — explicit cast is always trusted by the TypeChecker
  Cast->accept(TC);
  EXPECT_TRUE(TC.getType(Cast.get())->isInt());
}

// ─── Test 23: BlockExpr — last child type propagates ─────────────────────────

TEST(test_block_type_is_last_expression) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Stmts;
  Stmts.push_back(std::make_unique<BoolExprAST>(true));
  Stmts.push_back(std::make_unique<NumberExprAST>(3.14)); // last → double
  auto Block = std::make_unique<BlockExprAST>(std::move(Stmts));
  Block->accept(TC);
  EXPECT_TRUE(TC.getType(Block.get())->isDouble());
}

// ─── Test 24: Empty block → void ─────────────────────────────────────────────

TEST(test_empty_block_is_void) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Block = std::make_unique<BlockExprAST>(
      std::vector<std::unique_ptr<ExprAST>>{});
  Block->accept(TC);
  EXPECT_TRUE(TC.getType(Block.get())->isVoid());
}

// ─── Test 25: For loop variable typed as int (integer range) ─────────────────
//
// This is the prerequisite check mentioned in Issue #135:
//   "Ensure ForExprAST loop variables are properly typed in the Resolver
//    (e.g., inferring int from the range) before this pass runs."
//
// The TypeChecker handles this internally: visitFor() inserts the loop var
// into LoopVarTypes_ so that visitVariable() inside the body resolves it.

TEST(test_for_loop_var_typed_as_int_integer_range) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  // Body: a VariableExprAST that references the loop variable "i"
  auto BodyVar = std::make_unique<VariableExprAST>(loc(2, 5), "i");
  auto BodyPtr = BodyVar.get(); // save raw ptr before moving

  // for i in 0..10 { i }
  auto Start = std::make_unique<NumberExprAST>(static_cast<long long>(0));
  auto End   = std::make_unique<NumberExprAST>(static_cast<long long>(10));

  // Wrap body in a block so ForExprAST has a proper body node
  std::vector<std::unique_ptr<ExprAST>> Stmts;
  Stmts.push_back(std::move(BodyVar));
  auto Body = std::make_unique<BlockExprAST>(std::move(Stmts));

  auto For = std::make_unique<ForExprAST>(loc(1, 1), "i", std::move(Start),
                                           std::move(End),
                                           nullptr /*step*/, std::move(Body));
  For->accept(TC);

  // The loop variable reference "i" inside the body should be typed as int
  EXPECT_TRUE(TC.hasType(BodyPtr));
  EXPECT_TRUE(TC.getType(BodyPtr)->isInt());
}

// ─── Test 26: For loop variable typed as double (double range) ───────────────

TEST(test_for_loop_var_typed_as_double_double_range) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto BodyVar = std::make_unique<VariableExprAST>(loc(2, 5), "f");
  auto BodyPtr = BodyVar.get();

  auto Start = std::make_unique<NumberExprAST>(0.0);   // double
  auto End   = std::make_unique<NumberExprAST>(10.0);  // double

  std::vector<std::unique_ptr<ExprAST>> Stmts;
  Stmts.push_back(std::move(BodyVar));
  auto Body = std::make_unique<BlockExprAST>(std::move(Stmts));

  auto For = std::make_unique<ForExprAST>(loc(1, 1), "f", std::move(Start),
                                           std::move(End),
                                           nullptr, std::move(Body));
  For->accept(TC);

  EXPECT_TRUE(TC.hasType(BodyPtr));
  EXPECT_TRUE(TC.getType(BodyPtr)->isDouble());
}

// ─── Test 27: Variable lookup from ResolverSymbolTable ───────────────────────

TEST(test_variable_lookup_from_rst) {
  ResolverSymbolTable RST = makeEmptyRST();
  RST.declareVariable("myVar", TurfString(), {1, 1});

  TypeChecker TC(RST, ThrowingHandler);

  auto VarNode = std::make_unique<VariableExprAST>(loc(2, 1), "myVar");
  VarNode->accept(TC);
  EXPECT_TRUE(TC.hasType(VarNode.get()));
  EXPECT_TRUE(TC.getType(VarNode.get())->isString());
}

// ─── Test 28: VarDecl type propagates ────────────────────────────────────────

TEST(test_var_decl_type_propagates) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Init = std::make_unique<NumberExprAST>(static_cast<long long>(99));
  auto Decl = std::make_unique<VarDeclExprAST>(loc(), "x", TurfInt(),
                                                std::move(Init), false);
  Decl->accept(TC);
  EXPECT_TRUE(TC.hasType(Decl.get()));
  EXPECT_TRUE(TC.getType(Decl.get())->isInt());
}

// ─── Test 29: VarDecl type mismatch (string init into int var) → TypeError ───

TEST(test_var_decl_type_mismatch_throws) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Init = std::make_unique<StringExprAST>("oops");
  auto Decl = std::make_unique<VarDeclExprAST>(loc(), "x", TurfInt(),
                                                std::move(Init), false);
  EXPECT_THROWS(Decl->accept(TC));
}

// ─── Test 30: Assignment propagates RHS type ─────────────────────────────────

TEST(test_assignment_propagates_rhs_type) {
  ResolverSymbolTable RST = makeEmptyRST();
  RST.declareVariable("x", TurfInt(), {1, 1});
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS   = std::make_unique<VariableExprAST>(loc(2, 1), "x");
  auto RHS   = std::make_unique<NumberExprAST>(static_cast<long long>(5));
  auto Assign = std::make_unique<AssignmentExprAST>(loc(), std::move(LHS),
                                                     std::move(RHS));
  Assign->accept(TC);
  EXPECT_TRUE(TC.hasType(Assign.get()));
  EXPECT_TRUE(TC.getType(Assign.get())->isInt());
}

// ─── Test 31: sizeof() always produces int ────────────────────────────────────

TEST(test_sizeof_produces_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto SizeOf = std::make_unique<SizeOfExprAST>(loc(), TurfInt());
  SizeOf->accept(TC);
  EXPECT_TRUE(TC.getType(SizeOf.get())->isInt());
}

// ─── Test 32: ArrayLength always produces int ─────────────────────────────────

TEST(test_array_length_produces_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto Len = std::make_unique<ArrayLengthExprAST>(loc(), "myArr");
  Len->accept(TC);
  EXPECT_TRUE(TC.getType(Len.get())->isInt());
}

// ─── Test 33: Return inside a function matches declared type ──────────────────

TEST(test_return_type_matches_func_return_type) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  // Build: fn int foo() { return 42; }
  auto RetVal  = std::make_unique<NumberExprAST>(static_cast<long long>(42));
  auto RetStmt = std::make_unique<ReturnExprAST>(loc(2, 5), std::move(RetVal));

  std::vector<std::unique_ptr<ExprAST>> Stmts;
  Stmts.push_back(std::move(RetStmt));
  auto Body = std::make_unique<BlockExprAST>(std::move(Stmts));

  auto FuncDef = std::make_unique<FuncDefExprAST>(
      loc(1, 1), "foo", TurfInt(),
      std::vector<ParamDecl>{}, std::move(Body));

  // Should not throw
  FuncDef->accept(TC);
  EXPECT_TRUE(TC.hasType(FuncDef.get()));
}

// ─── Test 34: Return type mismatch inside a function → TypeError ──────────────

TEST(test_return_type_mismatch_throws) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  // Build: fn int bar() { return "oops"; }
  auto RetVal  = std::make_unique<StringExprAST>("oops");
  auto RetStmt = std::make_unique<ReturnExprAST>(loc(2, 5), std::move(RetVal));

  std::vector<std::unique_ptr<ExprAST>> Stmts;
  Stmts.push_back(std::move(RetStmt));
  auto Body = std::make_unique<BlockExprAST>(std::move(Stmts));

  auto FuncDef = std::make_unique<FuncDefExprAST>(
      loc(1, 1), "bar", TurfInt(),
      std::vector<ParamDecl>{}, std::move(Body));

  EXPECT_THROWS(FuncDef->accept(TC));
}

// ─── Test 35: Comparison of compatible numeric types → bool ──────────────────

TEST(test_eq_comparison_same_type_is_bool) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  // 5 == 5  (using TOK_EQ = -9 as operator)
  auto LHS = std::make_unique<NumberExprAST>(static_cast<long long>(5));
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(5));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), -9 /*TOK_EQ*/,
                                              std::move(LHS), std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isBool());
}

// ─── Test 36: int - int → int ────────────────────────────────────────────────

TEST(test_int_minus_int_is_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(static_cast<long long>(10));
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(3));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '-', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isInt());
}

// ─── Test 37: double - int → double ──────────────────────────────────────────

TEST(test_double_minus_int_is_double) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  auto LHS = std::make_unique<NumberExprAST>(5.0);
  auto RHS = std::make_unique<NumberExprAST>(static_cast<long long>(2));
  auto Bin = std::make_unique<BinaryExprAST>(loc(), '-', std::move(LHS),
                                              std::move(RHS));
  Bin->accept(TC);
  EXPECT_TRUE(TC.getType(Bin.get())->isDouble());
}
// ─── Issue #136 Tests: Intrinsic Arity & Overload Resolution ─────────────────
// Helper: build a FuncCallExprAST with a VariableExprAST callee.
// The Args list is a move-only vector, so we pass it by value.
static std::unique_ptr<FuncCallExprAST>
makeCall(const std::string &Name,
         std::vector<std::unique_ptr<ExprAST>> Args,
         int line = 1) {
  auto Callee = std::make_unique<VariableExprAST>(loc(line, 1), Name);
  return std::make_unique<FuncCallExprAST>(loc(line, 1), std::move(Callee),
                                           std::move(Args));
}

// ─── Test 38: typeof() — 0 args → TypeChecker arity error ────────────────────

TEST(test_typeof_zero_args_throws_arity_error) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  // typeof() with 0 arguments
  auto Call = makeCall("typeof", {});
  EXPECT_THROWS(Call->accept(TC));
}

// ─── Test 39: typeof(a, b) — 2 args → TypeChecker arity error ────────────────

TEST(test_typeof_two_args_throws_arity_error) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(1)));
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(2)));
  auto Call = makeCall("typeof", std::move(Args));
  EXPECT_THROWS(Call->accept(TC));
}

// ─── Test 40: typeof(x) — 1 arg → inferred type is string ───────────────────

TEST(test_typeof_one_arg_infers_string) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(42)));
  auto Call = makeCall("typeof", std::move(Args));
  Call->accept(TC);
  EXPECT_TRUE(TC.hasType(Call.get()));
  EXPECT_TRUE(TC.getType(Call.get())->isString());
}

// ─── Test 41: lengthof(x) — 1 string arg → inferred type is int ──────────────

TEST(test_lengthof_string_arg_infers_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<StringExprAST>("hello"));
  auto Call = makeCall("lengthof", std::move(Args));
  Call->accept(TC);
  EXPECT_TRUE(TC.hasType(Call.get()));
  EXPECT_TRUE(TC.getType(Call.get())->isInt());
}

// ─── Test 42: power(1) — wrong arity → TypeChecker arity error ───────────────

TEST(test_power_wrong_arity_throws) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(1)));
  auto Call = makeCall("power", std::move(Args));
  EXPECT_THROWS(Call->accept(TC));
}

// ─── Test 43: abs("hello") — type mismatch → TypeChecker error ────────────────

TEST(test_abs_string_arg_throws_type_error) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<StringExprAST>("hello"));
  auto Call = makeCall("abs", std::move(Args));
  EXPECT_THROWS(Call->accept(TC));
}

// ─── Test 44: abs(42) — int arg → inferred type is int ───────────────────────

TEST(test_abs_int_arg_infers_int) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(-5)));
  auto Call = makeCall("abs", std::move(Args));
  Call->accept(TC);
  EXPECT_TRUE(TC.hasType(Call.get()));
  EXPECT_TRUE(TC.getType(Call.get())->isInt());
}

// ─── Test 45: User-defined overload — arity mismatch → TypeChecker error ──────
//
// Simulates calling `foo(1, 2)` when foo is only declared as `foo(int x)`.

TEST(test_user_defined_arity_mismatch_throws) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  // Build a FuncCallExprAST with 2 args but a candidate of arity 1.
  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(1)));
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(2)));
  auto Call = makeCall("foo", std::move(Args));

  // Populate ResolvedCandidates with a single arity-1 symbol.
  auto Sym = std::make_shared<ResolverFunctionSymbol>();
  Sym->BaseName = "foo";
  Sym->ReturnType = TurfInt();
  Sym->Params.push_back({"x", TurfInt()});
  Call->ResolvedCandidates.push_back(Sym);

  EXPECT_THROWS(Call->accept(TC));
}

// ─── Test 46: User-defined overload — type mismatch → TypeChecker error ───────
//
// Simulates calling `bar("hello")` when bar is declared as `bar(int x)`.

TEST(test_user_defined_type_mismatch_throws) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<StringExprAST>("hello"));
  auto Call = makeCall("bar", std::move(Args));

  // Candidate: bar(int x) -> void
  auto Sym = std::make_shared<ResolverFunctionSymbol>();
  Sym->BaseName = "bar";
  Sym->ReturnType = TurfVoid();
  Sym->Params.push_back({"x", TurfInt()});
  Call->ResolvedCandidates.push_back(Sym);

  EXPECT_THROWS(Call->accept(TC));
}

// ─── Test 47: User-defined overload — exact match → ResolvedCallTarget set ────

TEST(test_user_defined_exact_match_resolves_target) {
  ResolverSymbolTable RST = makeEmptyRST();
  TypeChecker TC(RST, ThrowingHandler);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<NumberExprAST>(static_cast<long long>(5)));
  auto Call = makeCall("compute", std::move(Args));
  auto *CallPtr = Call.get();

  // Candidate: compute(int x) -> double
  auto Sym = std::make_shared<ResolverFunctionSymbol>();
  Sym->BaseName = "compute";
  Sym->ReturnType = TurfDouble();
  Sym->Params.push_back({"x", TurfInt()});
  Call->ResolvedCandidates.push_back(Sym);

  Call->accept(TC);

  // ResolvedCallTarget must be set
  EXPECT_TRUE(CallPtr->ResolvedCallTarget != nullptr);
  // Inferred return type must be double
  EXPECT_TRUE(TC.hasType(CallPtr));
  EXPECT_TRUE(TC.getType(CallPtr)->isDouble());
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "=== TurfLang TypeChecker Unit Tests (Issue #135 / #136) ===\n\n";

  std::cout << "[Literal type inference]\n";
  RUN(test_integer_literal_infers_int);
  RUN(test_double_literal_infers_double);
  RUN(test_bool_literal_infers_bool);
  RUN(test_string_literal_infers_string);
  RUN(test_null_literal_infers_null);

  std::cout << "\n[Homogeneous binary arithmetic]\n";
  RUN(test_int_plus_int_is_int);
  RUN(test_double_plus_double_is_double);
  RUN(test_int_minus_int_is_int);
  RUN(test_double_minus_int_is_double);

  std::cout << "\n[Numeric promotion]\n";
  RUN(test_int_plus_double_promotes_to_double);
  RUN(test_double_plus_int_promotes_to_double);

  std::cout << "\n[String operations]\n";
  RUN(test_string_concat_is_string);
  RUN(test_string_plus_int_throws_type_error);
  RUN(test_int_plus_string_throws_type_error);
  RUN(test_string_minus_string_throws_type_error);

  std::cout << "\n[Comparison operators -> bool]\n";
  RUN(test_int_lt_int_is_bool);
  RUN(test_double_gt_double_is_bool);
  RUN(test_eq_comparison_same_type_is_bool);

  std::cout << "\n[Unary operators]\n";
  RUN(test_unary_negate_int_is_int);
  RUN(test_unary_negate_double_is_double);
  RUN(test_unary_not_is_bool);
  RUN(test_unary_negate_string_throws);

  std::cout << "\n[Nested expressions]\n";
  RUN(test_nested_binary_int_arithmetic);

  std::cout << "\n[Cast expression]\n";
  RUN(test_cast_int_to_double);
  RUN(test_cast_string_to_int_is_trusted);

  std::cout << "\n[Block expression]\n";
  RUN(test_block_type_is_last_expression);
  RUN(test_empty_block_is_void);

  std::cout << "\n[For loop variable inference -- Issue #135 prerequisite]\n";
  RUN(test_for_loop_var_typed_as_int_integer_range);
  RUN(test_for_loop_var_typed_as_double_double_range);

  std::cout << "\n[Variable lookup]\n";
  RUN(test_variable_lookup_from_rst);

  std::cout << "\n[VarDecl]\n";
  RUN(test_var_decl_type_propagates);
  RUN(test_var_decl_type_mismatch_throws);

  std::cout << "\n[Assignment]\n";
  RUN(test_assignment_propagates_rhs_type);

  std::cout << "\n[Misc intrinsics]\n";
  RUN(test_sizeof_produces_int);
  RUN(test_array_length_produces_int);

  std::cout << "\n[Function return type checking]\n";
  RUN(test_return_type_matches_func_return_type);
  RUN(test_return_type_mismatch_throws);

  std::cout << "\n[Issue #136: Intrinsic arity & overload resolution]\n";
  RUN(test_typeof_zero_args_throws_arity_error);
  RUN(test_typeof_two_args_throws_arity_error);
  RUN(test_typeof_one_arg_infers_string);
  RUN(test_lengthof_string_arg_infers_int);
  RUN(test_power_wrong_arity_throws);
  RUN(test_abs_string_arg_throws_type_error);
  RUN(test_abs_int_arg_infers_int);
  RUN(test_user_defined_arity_mismatch_throws);
  RUN(test_user_defined_type_mismatch_throws);
  RUN(test_user_defined_exact_match_resolves_target);

  std::cout << "\n=== Results: " << TestsPassed << " passed, " << TestsFailed
            << " failed ===\n";
  return TestsFailed == 0 ? 0 : 1;
}
