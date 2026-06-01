/**
 * @file ResolverSymbolTable.h
 * @brief Isolated, LLVM-free symbol table data structures for the Resolver pass.
 *
 * Issue #2 of the monolith → multi-pass migration.
 *
 * This file intentionally has ZERO dependency on LLVM headers.  It models the
 * same concepts as the codegen-facing SymbolTable (scopes, variables, functions,
 * modules) but stores only the information the Resolver and TypeChecker need:
 *
 *   VariableSymbol   — name + type + declaration source location + scope depth
 *   FunctionSymbol   — base name, arity, parameter types, return type
 *   ResolverModule   — namespace-isolated map of function overloads
 *   ResolverScope    — flat name→VariableSymbol map + parent pointer
 *   ResolverSymbolTable — hierarchical scope manager + loaded modules
 *
 * Design constraints:
 *  - No llvm::AllocaInst, no llvm::Type, no LLVM headers of any kind.
 *  - All symbols use std::shared_ptr so the Resolver can safely attach them
 *    to AST nodes (Issue #5 will add AST fields to hold these pointers).
 *  - The SymbolTable can be constructed and used entirely without running the
 *    LLVM back-end; unit tests therefore require no special LLVM initialisation.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef RESOLVER_SYMBOL_TABLE_H
#define RESOLVER_SYMBOL_TABLE_H

#include "Types.h" // TurfTypePtr, TurfType — pure C++ header, no LLVM

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Source location (same layout as Lexer.h's SourceLocation) ───────────────
// Duplicated as a plain POD here so this header has no dependency on Lexer.h.
struct ResolverSourceLocation {
  int Line = 0;
  int Col  = 0;
};

// ─── VariableSymbol ──────────────────────────────────────────────────────────

/**
 * @brief Represents a single declared variable visible to the Resolver.
 *
 * Fields deliberately mirror what a future TypeChecker pass will need:
 *  - Name and Type are the primary data.
 *  - DeclLoc enables rich "already declared here" error messages.
 *  - ScopeDepth allows the Resolver to implement correct shadowing rules.
 */
struct VariableSymbol {
  std::string            Name;
  TurfTypePtr            Type;        ///< Frontend type (no LLVM involvement)
  ResolverSourceLocation DeclLoc;
  size_t                 ScopeDepth; ///< 0 = global, 1 = first block, …

  VariableSymbol() = default;
  VariableSymbol(std::string Name, TurfTypePtr Type,
                 ResolverSourceLocation DeclLoc, size_t ScopeDepth)
      : Name(std::move(Name)), Type(std::move(Type)), DeclLoc(DeclLoc),
        ScopeDepth(ScopeDepth) {}
};

// ─── FunctionParam + FunctionSymbol ─────────────────────────────────────────

// A single function parameter with a name and a frontend type.  Used for
// diagnostics and for attaching to ResolverFunctionSymbol so the Resolver can
// present rich messages to the user.
struct FunctionParam {
  std::string Name;
  TurfTypePtr Type;
};

/**
 * @brief Represents an overloadable function registered in a module or scope.
 *
 * New design: the Resolver stores only frontend information (base name,
 * source module, typed parameters, declaration location). Mangled names are
 * intentionally not stored here — Codegen performs "late mangling" on the
 * narrowed symbol.
 */
struct ResolverFunctionSymbol {
  std::string               BaseName;    ///< e.g. "printline"
  std::string               SourceModule;///< "" = current file, or module name
  std::vector<FunctionParam> Params;     ///< parameter name+type for diagnostics
  TurfTypePtr               ReturnType;
  ResolverSourceLocation    DeclLoc;
  bool                      IsIntrinsic = false;

  // Backwards-compatible fields retained for tests / older codepaths
  std::string              MangledName; ///< optional cached mangled name
  std::vector<TurfTypePtr> ParamTypes;  ///< convenience for old lookup routines

  int arity() const { return static_cast<int>(Params.size()); }
};

// ─── ResolverModule ──────────────────────────────────────────────────────────

/**
 * @brief Represents one imported module's function namespace.
 *
 * Lookup is strictly namespace-isolated: searching "io" will never find
 * functions registered under "math".
 */
class ResolverModule {
public:
  std::string Name; ///< e.g. "io", "math", "main"

  /// base name → list of overloads
  std::unordered_map<std::string, std::vector<ResolverFunctionSymbol>>
      Functions;

  explicit ResolverModule(std::string name) : Name(std::move(name)) {}

  /**
   * @brief Register a function overload.
   * Duplicate mangled names are silently ignored (prototype re-entry).
   */
  void registerFunction(const ResolverFunctionSymbol &Sym) {
    auto &Overloads = Functions[Sym.BaseName];
    for (const auto &Existing : Overloads)
      if (Existing.MangledName == Sym.MangledName)
        return;
    Overloads.push_back(Sym);
  }

  /**
   * @brief Look up a function overload by base name and argument types.
   *
   * Resolution rules (priority order):
   *  1. Arity match is required.
   *  2. Exact type name match wins.
   *  3. Numeric implicit coercion (int↔double↔bool) allowed as fallback.
   *
   * @return Pointer to the matching symbol, or nullptr on no match.
   */
  const ResolverFunctionSymbol *lookupFunction(
      const std::string &BaseName,
      const std::vector<TurfTypePtr> &ArgTypes) const;

  /// Return all registered base names (for suggestion / diagnostics).
  std::vector<std::string> getFunctionNames() const;
};

// ─── ResolverScope ───────────────────────────────────────────────────────────

/**
 * @brief A single lexical scope in the Resolver's scope stack.
 *
 * Uses a pointer-to-parent chain so scopes can be queried independently of
 * the full stack (useful for the TypeChecker which may clone a scope chain).
 */
struct ResolverScope {
  size_t Depth;   ///< 0 = global, increments per nested block
  ResolverScope *Parent = nullptr; ///< Outer scope, or nullptr at global level

  /// Variable name → owned VariableSymbol
  std::unordered_map<std::string, std::shared_ptr<VariableSymbol>> Symbols;

  explicit ResolverScope(size_t Depth, ResolverScope *Parent = nullptr)
      : Depth(Depth), Parent(Parent) {}

  /// Look up a name in this scope only (no parent traversal).
  std::shared_ptr<VariableSymbol> lookupLocal(const std::string &Name) const {
    auto It = Symbols.find(Name);
    return It != Symbols.end() ? It->second : nullptr;
  }

  /// True if @p Name is declared directly in this scope.
  bool has(const std::string &Name) const {
    return Symbols.count(Name) > 0;
  }
};

// ─── ResolverSymbolTable ─────────────────────────────────────────────────────

/**
 * @brief Hierarchical symbol table for the Resolver and TypeChecker passes.
 *
 * This class manages:
 *  - A stack of ResolverScope objects for lexical scope handling.
 *  - A registry of ResolverModule objects for module-qualified lookups.
 *
 * It is entirely LLVM-free.  Unit tests can construct and exercise it without
 * initialising the LLVM back-end.
 *
 * Error handling: lookup failures return nullptr (not exceptions) so that the
 * Resolver can accumulate multiple errors per compilation run.  enterScope /
 * exitScope are infallible (no error on over-pop; production code tracks this
 * separately).
 */
class ResolverSymbolTable {
public:
  ResolverSymbolTable();

  // ── Scope management ────────────────────────────────────────────────────────

  /// Push a new nested scope.
  void enterScope();

  /// Pop the current scope.  Variables declared in it become invisible.
  void exitScope();

  /// Current nesting depth (0 = global).
  size_t currentDepth() const { return Depth_; }

  // ── Variable declarations ────────────────────────────────────────────────────

  /**
   * @brief Declare a new variable in the current scope.
   *
   * Returns a shared_ptr to the new VariableSymbol so the Resolver can attach
   * it to AST nodes (Issue #5).  Throws std::runtime_error if the name is
   * already declared in the *current* scope (shadowing outer scopes is allowed).
   */
  std::shared_ptr<VariableSymbol> declareVariable(
      const std::string &Name, TurfTypePtr Type,
      ResolverSourceLocation Loc = {});

  /**
   * @brief Look up a variable by name, searching from the innermost scope out.
   * @return Shared pointer to the symbol, or nullptr if not found.
   */
  std::shared_ptr<VariableSymbol> lookupVariable(const std::string &Name) const;

  /**
   * @brief True if @p Name is declared in the current (innermost) scope only.
   */
  bool isDeclaredInCurrentScope(const std::string &Name) const;

  // ── Module / function registry ───────────────────────────────────────────────

  /**
   * @brief Register a function symbol under the given module namespace.
   * The module is created automatically if it doesn't exist yet.
   */
  void registerModuleFunction(const std::string &ModuleName,
                               const ResolverFunctionSymbol &Sym);

  /**
   * @brief Register a function defined in the current file (tier-1 registry).
   * The Resolver uses this to record user-defined functions so they are
   * preferred over imported module functions during lookup.
   */
  void registerCurrentFileFunction(const ResolverFunctionSymbol &Sym);

  /**
   * @brief Lookup all overloads with the given base name defined in the
   * current file. Returns an empty vector if none found.
   */
  std::vector<std::shared_ptr<ResolverFunctionSymbol>>
  lookupCurrentFileFunctions(const std::string &BaseName) const;

  /**
   * @brief Return all base function names registered in the current file.
   */
  std::vector<std::string> getAllCurrentFileFunctionNames() const;

  /**
   * @brief Resolve a module-qualified call (e.g. io.printline(42)).
   * Only the named module is searched.
   * @return Pointer to the matching symbol, or nullptr.
   */
  const ResolverFunctionSymbol *resolveModuleFunction(
      const std::string &ModuleName,
      const std::string &FuncName,
      const std::vector<TurfTypePtr> &ArgTypes) const;

  /**
   * @brief True if a module with the given name has at least one function.
   */
  bool hasModule(const std::string &ModuleName) const;

  /**
   * @brief Return all base function names registered under @p ModuleName.
   */
  std::vector<std::string> getModuleFunctionNames(
      const std::string &ModuleName) const;

  /**
   * @brief Retrieve the ResolverModule object for @p ModuleName.
   * Returns nullptr if not registered.
   */
  const ResolverModule *getModule(const std::string &ModuleName) const;

  /**
   * @brief Search all imported modules (tier-2) for overloads with the given
   * base name. Returns a vector of shared_ptrs to matching symbols.
   */
  std::vector<std::shared_ptr<ResolverFunctionSymbol>>
  findInModules(const std::string &BaseName) const;

  /**
   * @brief Return all function base names across all registered modules.
   * Useful for suggestion generation.
   */
  std::vector<std::string> getAllModuleFunctionNames() const;

private:
  // Stack of scope objects.  We own them via unique_ptr; the parent pointer
  // inside ResolverScope is a raw (non-owning) back-reference.
  std::vector<std::unique_ptr<ResolverScope>> ScopeStack_;
  size_t Depth_ = 0;

  std::unordered_map<std::string, std::unique_ptr<ResolverModule>> Modules_;
  // Tier-1 (current file) function registry: base name → overload list
  std::unordered_map<std::string,
                     std::vector<std::shared_ptr<ResolverFunctionSymbol>>>
      CurrentFileFunctions_;
};

#endif // RESOLVER_SYMBOL_TABLE_H
