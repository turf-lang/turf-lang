/**
 * @file test_resolver_symbol_table.cpp
 * @brief Unit tests for the ResolverSymbolTable (Issue #2).
 *
 * These tests are LLVM-free — they compile and run without initialising any
 * LLVM context.  They verify:
 *
 *  1. Global scope starts empty.
 *  2. Variables are declared and looked up correctly.
 *  3. Shadowing across nested scopes works.
 *  4. Exiting a scope makes its variables invisible.
 *  5. Double-declaration in the same scope is an error.
 *  6. Module registration and function lookup works.
 *  7. Overload resolution (exact match, then coercion fallback).
 *  8. Cross-module isolation: functions from one module are not visible in another.
 *
 * Build (standalone, no LLVM needed):
 *   g++ -std=c++20 -I../../include \
 *       test_resolver_symbol_table.cpp \
 *       ../../src/ResolverSymbolTable.cpp \
 *       ../../src/Types.cpp \
 *       -o test_rst && ./test_rst
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverSymbolTable.h"
#include "Types.h"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

// ─── Minimal test framework ──────────────────────────────────────────────────

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
    } catch (const std::exception &E) {                                       \
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
    try { expr; } catch (...) { _threw = true; }                              \
    if (!_threw)                                                               \
      throw std::runtime_error("EXPECT_THROWS: no exception was thrown for " #expr); \
  } while (false)

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST(test_global_scope_starts_empty) {
  ResolverSymbolTable RST;
  EXPECT_EQ(RST.currentDepth(), 0u);
  EXPECT_TRUE(RST.lookupVariable("x") == nullptr);
}

TEST(test_declare_and_lookup_variable) {
  ResolverSymbolTable RST;
  auto Sym = RST.declareVariable("x", TurfInt(), {1, 1});
  EXPECT_TRUE(Sym != nullptr);
  EXPECT_EQ(Sym->Name, "x");
  EXPECT_EQ(Sym->Type->getName(), std::string("int"));
  EXPECT_EQ(Sym->ScopeDepth, 0u);

  auto Found = RST.lookupVariable("x");
  EXPECT_TRUE(Found != nullptr);
  EXPECT_EQ(Found->Name, "x");
}

TEST(test_lookup_undefined_variable_returns_null) {
  ResolverSymbolTable RST;
  EXPECT_TRUE(RST.lookupVariable("undefined") == nullptr);
}

TEST(test_nested_scope_shadowing) {
  ResolverSymbolTable RST;
  // Declare "x" as int in global scope
  RST.declareVariable("x", TurfInt(), {1, 1});

  RST.enterScope();
  EXPECT_EQ(RST.currentDepth(), 1u);
  // Redeclare "x" as string in inner scope (shadowing is allowed)
  RST.declareVariable("x", TurfString(), {2, 1});

  auto Inner = RST.lookupVariable("x");
  EXPECT_TRUE(Inner != nullptr);
  EXPECT_EQ(Inner->Type->getName(), std::string("string")); // inner wins
  EXPECT_EQ(Inner->ScopeDepth, 1u);

  RST.exitScope();
  EXPECT_EQ(RST.currentDepth(), 0u);

  auto Outer = RST.lookupVariable("x");
  EXPECT_TRUE(Outer != nullptr);
  EXPECT_EQ(Outer->Type->getName(), std::string("int")); // outer restored
}

TEST(test_exit_scope_hides_variables) {
  ResolverSymbolTable RST;
  RST.enterScope();
  RST.declareVariable("inner_var", TurfBool(), {3, 1});
  EXPECT_TRUE(RST.lookupVariable("inner_var") != nullptr);

  RST.exitScope();
  EXPECT_TRUE(RST.lookupVariable("inner_var") == nullptr);
}

TEST(test_double_declaration_in_same_scope_throws) {
  ResolverSymbolTable RST;
  RST.declareVariable("x", TurfInt(), {1, 1});
  EXPECT_THROWS(RST.declareVariable("x", TurfDouble(), {2, 1}));
}

TEST(test_is_declared_in_current_scope) {
  ResolverSymbolTable RST;
  RST.declareVariable("g", TurfInt(), {1, 1});
  EXPECT_TRUE(RST.isDeclaredInCurrentScope("g"));
  EXPECT_FALSE(RST.isDeclaredInCurrentScope("missing"));

  RST.enterScope();
  // "g" is in the outer scope — not in the current one
  EXPECT_FALSE(RST.isDeclaredInCurrentScope("g"));
  RST.declareVariable("local", TurfString(), {2, 1});
  EXPECT_TRUE(RST.isDeclaredInCurrentScope("local"));
  RST.exitScope();
}

TEST(test_multiple_nested_scopes) {
  ResolverSymbolTable RST;
  RST.declareVariable("a", TurfInt(), {1, 1});

  RST.enterScope();
  RST.declareVariable("b", TurfDouble(), {2, 1});

  RST.enterScope();
  RST.declareVariable("c", TurfBool(), {3, 1});
  EXPECT_EQ(RST.currentDepth(), 2u);

  // All three should be visible from the innermost scope
  EXPECT_TRUE(RST.lookupVariable("a") != nullptr);
  EXPECT_TRUE(RST.lookupVariable("b") != nullptr);
  EXPECT_TRUE(RST.lookupVariable("c") != nullptr);

  RST.exitScope();
  EXPECT_TRUE(RST.lookupVariable("a") != nullptr);
  EXPECT_TRUE(RST.lookupVariable("b") != nullptr);
  EXPECT_TRUE(RST.lookupVariable("c") == nullptr); // gone

  RST.exitScope();
  EXPECT_TRUE(RST.lookupVariable("a") != nullptr);
  EXPECT_TRUE(RST.lookupVariable("b") == nullptr); // gone
}

TEST(test_module_registration_and_lookup) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol PrintInt;
  PrintInt.BaseName    = "printline";
  PrintInt.SourceModule = "io";
  PrintInt.Params      = {{"x", TurfInt()}};
  PrintInt.ReturnType  = TurfVoid();

  RST.registerModuleFunction("io", PrintInt);
  EXPECT_TRUE(RST.hasModule("io"));

  auto *Resolved = RST.resolveModuleFunction("io", "printline", {TurfInt()});
  EXPECT_TRUE(Resolved != nullptr);
  EXPECT_EQ(Resolved->BaseName, std::string("printline"));
  EXPECT_EQ(Resolved->SourceModule, std::string("io"));
}

TEST(test_overload_resolution_exact_match) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol AbsInt;
  AbsInt.BaseName    = "abs";
  AbsInt.SourceModule = "math";
  AbsInt.Params      = {{"x", TurfInt()}};
  AbsInt.ReturnType  = TurfInt();

  ResolverFunctionSymbol AbsDouble;
  AbsDouble.BaseName    = "abs";
  AbsDouble.SourceModule = "math";
  AbsDouble.Params      = {{"x", TurfDouble()}};
  AbsDouble.ReturnType  = TurfDouble();

  RST.registerModuleFunction("math", AbsInt);
  RST.registerModuleFunction("math", AbsDouble);

  // Exact match: int arg → int overload
  auto *R1 = RST.resolveModuleFunction("math", "abs", {TurfInt()});
  EXPECT_TRUE(R1 != nullptr);
  EXPECT_EQ(R1->ReturnType->getName(), std::string("int"));

  // Exact match: double arg → double overload
  auto *R2 = RST.resolveModuleFunction("math", "abs", {TurfDouble()});
  EXPECT_TRUE(R2 != nullptr);
  EXPECT_EQ(R2->ReturnType->getName(), std::string("double"));
}

TEST(test_overload_resolution_no_match_returns_null) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol Fn;
  Fn.BaseName    = "concat";
  Fn.SourceModule = "strings";
  Fn.Params      = {{"a", TurfString()}, {"b", TurfString()}};
  Fn.ReturnType  = TurfString();

  RST.registerModuleFunction("strings", Fn);

  // Wrong arity
  auto *R1 = RST.resolveModuleFunction("strings", "concat", {TurfString()});
  EXPECT_TRUE(R1 == nullptr);

  // Type incompatible (string not coercible to int)
  auto *R2 = RST.resolveModuleFunction("strings", "concat",
                                        {TurfInt(), TurfInt()});
  EXPECT_TRUE(R2 == nullptr);
}

TEST(test_cross_module_isolation) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol IoFn;
  IoFn.BaseName    = "read";
  IoFn.SourceModule = "io";
  IoFn.Params      = {};
  IoFn.ReturnType  = TurfString();

  RST.registerModuleFunction("io", IoFn);

  // Looking up "read" in "math" must fail (cross-namespace lookup forbidden)
  auto *R = RST.resolveModuleFunction("math", "read", {});
  EXPECT_TRUE(R == nullptr);

  // Looking up in the correct module must succeed
  auto *R2 = RST.resolveModuleFunction("io", "read", {});
  EXPECT_TRUE(R2 != nullptr);
}

TEST(test_unknown_module_returns_null) {
  ResolverSymbolTable RST;
  EXPECT_FALSE(RST.hasModule("nonexistent"));
  EXPECT_TRUE(RST.resolveModuleFunction("nonexistent", "fn", {}) == nullptr);
}

TEST(test_get_module_function_names) {
  ResolverSymbolTable RST;

  for (const std::string &Name : {"sqrt", "abs", "pow"}) {
    ResolverFunctionSymbol Sym;
    Sym.BaseName    = Name;
    Sym.SourceModule = "math";
    Sym.Params      = {{"x", TurfDouble()}};
    Sym.ReturnType  = TurfDouble();
    RST.registerModuleFunction("math", Sym);
  }

  auto Names = RST.getModuleFunctionNames("math");
  // Names are sorted alphabetically by getFunctionNames()
  EXPECT_EQ(Names.size(), 3u);
  EXPECT_EQ(Names[0], std::string("abs"));
  EXPECT_EQ(Names[1], std::string("pow"));
  EXPECT_EQ(Names[2], std::string("sqrt"));
}

TEST(test_duplicate_signature_ignored) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol Sym;
  Sym.BaseName    = "print";
  Sym.SourceModule = "io";
  Sym.Params      = {{"x", TurfInt()}};
  Sym.ReturnType  = TurfVoid();

  RST.registerModuleFunction("io", Sym);
  RST.registerModuleFunction("io", Sym); // duplicate — should be ignored

  auto Names = RST.getModuleFunctionNames("io");
  EXPECT_EQ(Names.size(), 1u); // only one "print"
}

// ─── Two-tier registry tests ──────────────────────────────────────────────────

TEST(test_current_file_function_registration) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol MyFn;
  MyFn.BaseName    = "compute";
  MyFn.SourceModule = ""; // current file
  MyFn.Params      = {{"a", TurfInt()}, {"b", TurfInt()}};
  MyFn.ReturnType  = TurfInt();
  MyFn.DeclLoc     = {10, 1};

  RST.registerCurrentFileFunction(MyFn);

  auto Results = RST.lookupCurrentFileFunctions("compute");
  EXPECT_EQ(Results.size(), 1u);
  EXPECT_EQ(Results[0]->BaseName, std::string("compute"));
  EXPECT_EQ(Results[0]->SourceModule, std::string(""));
  EXPECT_EQ(Results[0]->DeclLoc.Line, 10);
}

TEST(test_current_file_functions_not_in_module_tier) {
  ResolverSymbolTable RST;

  ResolverFunctionSymbol MyFn;
  MyFn.BaseName    = "localFn";
  MyFn.SourceModule = "";
  MyFn.Params      = {};
  MyFn.ReturnType  = TurfVoid();
  RST.registerCurrentFileFunction(MyFn);

  // Must NOT appear when searching module tier
  auto ModuleMatches = RST.findInModules("localFn");
  EXPECT_EQ(ModuleMatches.size(), 0u);
}

TEST(test_two_tier_find_in_modules) {
  ResolverSymbolTable RST;

  // Register in two different modules under the same base name
  ResolverFunctionSymbol IoRead;
  IoRead.BaseName    = "read";
  IoRead.SourceModule = "io";
  IoRead.Params      = {};
  IoRead.ReturnType  = TurfString();
  RST.registerModuleFunction("io", IoRead);

  ResolverFunctionSymbol FsRead;
  FsRead.BaseName    = "read";
  FsRead.SourceModule = "fs";
  FsRead.Params      = {{"path", TurfString()}};
  FsRead.ReturnType  = TurfString();
  RST.registerModuleFunction("fs", FsRead);

  // findInModules should return entries from BOTH modules
  auto Matches = RST.findInModules("read");
  EXPECT_EQ(Matches.size(), 2u);
}

TEST(test_signature_formatting) {
  ResolverFunctionSymbol Sym;
  Sym.BaseName   = "print";
  Sym.Params     = {{"x", TurfInt()}, {"sep", TurfString()}};
  Sym.ReturnType = TurfVoid();

  // signature() should produce "print(int x, string sep) -> void"
  auto Sig = Sym.signature();
  EXPECT_EQ(Sig, std::string("print(int x, string sep) -> void"));
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "=== ResolverSymbolTable Unit Tests ===\n\n";

  RUN(test_global_scope_starts_empty);
  RUN(test_declare_and_lookup_variable);
  RUN(test_lookup_undefined_variable_returns_null);
  RUN(test_nested_scope_shadowing);
  RUN(test_exit_scope_hides_variables);
  RUN(test_double_declaration_in_same_scope_throws);
  RUN(test_is_declared_in_current_scope);
  RUN(test_multiple_nested_scopes);
  RUN(test_module_registration_and_lookup);
  RUN(test_overload_resolution_exact_match);
  RUN(test_overload_resolution_no_match_returns_null);
  RUN(test_cross_module_isolation);
  RUN(test_unknown_module_returns_null);
  RUN(test_get_module_function_names);
  RUN(test_duplicate_signature_ignored);
  // Two-tier tests
  RUN(test_current_file_function_registration);
  RUN(test_current_file_functions_not_in_module_tier);
  RUN(test_two_tier_find_in_modules);
  RUN(test_signature_formatting);

  std::cout << "\n=== Results: " << TestsPassed << " passed, "
            << TestsFailed << " failed ===\n";

  return TestsFailed == 0 ? 0 : 1;
}
