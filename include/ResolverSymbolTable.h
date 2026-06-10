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
 *   FunctionParam    — a single parameter (name + type) for diagnostic formatting
 *   FunctionSymbol   — base name, arity, parameter names+types, return type,
 *                      source module, and declaration location.
 *                      NOTE: No mangled name — Codegen derives it on demand from
 *                      SourceModule + BaseName + ParamTypes.  This keeps the
 *                      Resolver's error messages in the user's namespace, never
 *                      the LLVM namespace.
 *   ResolverModule   — namespace-isolated map of function overloads (imported)
 *   ResolverScope    — flat name→VariableSymbol map + parent pointer
 *   ResolverSymbolTable — hierarchical scope manager + loaded modules +
 *                         current-file function registry (two-tier diagnostics)
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

// ─── FunctionParam ───────────────────────────────────────────────────────────

/**
 * @brief A single function parameter, carrying both name and type.
 *
 * Storing the name enables rich diagnostic messages:
 *   print(int x)   — rather than just   print(int)
 *
 * The TypeChecker and Codegen use ParamTypes extracted from this for overload
 * resolution and LLVM type mapping respectively.
 */
struct FunctionParam {
  std::string Name; ///< e.g. "x"
  TurfTypePtr Type; ///< e.g. TurfInt()
};

// ─── ResolverFunctionSymbol ──────────────────────────────────────────────────

/**
 * @brief Represents one overload of a function visible to the Resolver.
 *
 * Design decisions driven by the two-tier diagnostic system:
 *
 *  - NO MangledName field.  The LLVM symbol name is derived by Codegen on
 *    demand via a deterministic mangling function:
 *        mangle("io", "printline", [int])  → "turf_io_printline_int"
 *        mangle("",   "foo",       [int])  → "foo"
 *    Removing it from the Resolver keeps all error messages in the user's
 *    namespace, never in LLVM's.
 *
 *  - SourceModule: empty = defined in the current file;
 *                  non-empty = name of the imported module ("io", "math", …).
 *    This drives the two-tier error format:
 *
 *        Error: Unknown function 'pritn'
 *          Current file:   print(int x)     — line 12
 *          From io module: io.printline(str) — try 'io.printline(x)'
 *
 *  - DeclLoc: source location of the fn keyword, used for "defined at …" notes.
 *
 *  - IsIntrinsic: marks compiler-synthesised builtins that never have a Turf
 *    source location.
 */
struct ResolverFunctionSymbol {
  std::string              BaseName;     ///< User-visible name, e.g. "printline"
  std::string              SourceModule; ///< "" = current file, "io" = imported
  std::vector<FunctionParam> Params;     ///< Ordered parameters (name + type)
  TurfTypePtr              ReturnType;
  ResolverSourceLocation   DeclLoc;      ///< Location of the fn definition
  bool                     IsIntrinsic = false;

  /// Arity of this overload.
  int arity() const { return static_cast<int>(Params.size()); }

  /// Format a human-readable signature for diagnostics:
  ///   "print(int x, string y) -> void"
  std::string signature() const {
    std::string S = BaseName + "(";
    for (size_t i = 0; i < Params.size(); ++i) {
      if (i) S += ", ";
      S += (Params[i].Type ? Params[i].Type->getName() : "?");
      if (!Params[i].Name.empty()) { S += " "; S += Params[i].Name; }
    }
    S += ") -> ";
    S += (ReturnType ? ReturnType->getName() : "void");
    return S;
  }

  /// Extract just the parameter types — used by overload resolution logic
  /// and by ResolverModule::lookupFunction.
  std::vector<TurfTypePtr> paramTypes() const {
    std::vector<TurfTypePtr> T;
    T.reserve(Params.size());
    for (const auto &P : Params) T.push_back(P.Type);
    return T;
  }

  /// Derive the LLVM-level mangled name for this overload.
  /// Mirrors the mangling performed by FuncDefExprAST::codegen():
  ///   module="io",  base="printline", params=[int] → "turf_io_printline_int"
  ///   module="",    base="add",       params=[int,int] → "turf_add_int_int"
  ///   module="",    base="main",      params=[] → "main"
  std::string mangledName() const {
    if (BaseName == "main") return "main";
    std::string S = "turf_";
    if (!SourceModule.empty()) S += SourceModule + "_";
    S += BaseName;
    for (const auto &P : Params)
      if (P.Type) S += "_" + P.Type->getMangledName();
    return S;
  }
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
  std::string Name; ///< e.g. "io", "math"

  /// base name → list of overloads
  std::unordered_map<std::string, std::vector<ResolverFunctionSymbol>>
      Functions;

  explicit ResolverModule(std::string name) : Name(std::move(name)) {}

  /**
   * @brief Register a function overload.
   * Duplicate registrations (same BaseName + arity + param types) are
   * silently ignored to handle prototype re-entry during the pre-pass.
   */
  void registerFunction(const ResolverFunctionSymbol &Sym);

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

  /**
   * @brief Collect every variable name visible from the current scope.
   *
   * Walks the scope stack from innermost to outermost and deduplicates names
   * so that a shadowed outer variable is only listed once.  The result is
   * suitable as the candidate list passed to SuggestionIndex::suggestVisible
   * when emitting an "Unknown Variable" diagnostic.
   */
  std::vector<std::string> getAllVisibleNames() const;

  // ── Module / imported function registry ─────────────────────────────────────

  /**
   * @brief Register a function symbol under the given imported module namespace.
   * The module is created automatically if it doesn't exist yet.
   * Use this for `import io` declarations — not for functions defined in the
   * current file (use registerCurrentFileFunction for those).
   */
  void registerModuleFunction(const std::string &ModuleName,
                               const ResolverFunctionSymbol &Sym);

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

  // ── Current-file function registry ──────────────────────────────────────────
  // These track `fn` definitions in the file being compiled, separately from
  // imported module functions.  This separation powers the two-tier diagnostic:
  //
  //   Error: Unknown function 'pritn'
  //     Current file:   print(int x)      — line 12
  //     From io module: io.printline(str)  — try 'io.printline(x)'
  //
  // Issue #6 wires visitFuncDef to call registerCurrentFileFunction.

  /**
   * @brief Register a top-level fn definition from the current source file.
   * Duplicate registrations (same signature) are silently ignored.
   * Emits no error on overloads — that is the TypeChecker's job.
   */
  void registerCurrentFileFunction(const ResolverFunctionSymbol &Sym);

  /**
   * @brief Return all overloads of @p Name defined in the current file.
   * Returns an empty vector if the name is not defined here.
   */
  std::vector<const ResolverFunctionSymbol *>
  lookupCurrentFileFunctions(const std::string &Name) const;

  /**
   * @brief Return all function names defined in the current file.
   * Used as the Tier-1 candidate list for Damerau-Levenshtein suggestions
   * when an unknown function name is encountered.
   */
  std::vector<std::string> getAllCurrentFileFunctionNames() const;

  // ── Two-tier diagnostic helpers ──────────────────────────────────────────────

  /**
   * @brief Search every loaded module for overloads matching @p Name.
   *
   * Returns a list of (module_name, symbol_ptr) pairs so the diagnostic
   * can format:  "From io: io.printline(str)  — try 'io.printline(x)'"
   *
   * This is Tier-2 in the error hierarchy — checked after the current-file
   * registry fails.
   */
  struct ModuleFunctionMatch {
    std::string ModuleName;
    const ResolverFunctionSymbol *Symbol;
  };
  std::vector<ModuleFunctionMatch>
  findInModules(const std::string &Name) const;

  /**
   * @brief Collect every function name visible across ALL modules.
   * Used to compute Damerau-Levenshtein suggestions spanning both tiers:
   * the Resolver can rank and present them grouped by source.
   */
  std::vector<std::string> getAllModuleFunctionNames() const;

  /**
   * @brief Dumps the contents of the symbol table (all scopes and modules) to the output stream.
   */
  void dump(std::ostream &OS) const;

private:
  // Stack of scope objects.  We own them via unique_ptr; the parent pointer
  // inside ResolverScope is a raw (non-owning) back-reference.
  std::vector<std::unique_ptr<ResolverScope>> ScopeStack_;
  size_t Depth_ = 0;

  /// Imported module registries (one per 'import X' statement).
  std::unordered_map<std::string, std::unique_ptr<ResolverModule>> Modules_;

  /// Top-level fn definitions from the current source file.
  /// Keyed by bare function name; value is the list of overloads.
  /// Separate from Modules_ so the two-tier error system can distinguish
  /// "this file" from "imported modules" without string-matching module names.
  std::unordered_map<std::string, std::vector<ResolverFunctionSymbol>>
      CurrentFileFunctions_;
};

#endif // RESOLVER_SYMBOL_TABLE_H
