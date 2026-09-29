#ifndef RESOLVER_MODULE_TABLE_H
#define RESOLVER_MODULE_TABLE_H

#include "ResolverSymbolBase.h" // ResolverFunctionSymbol, ResolverConstantSymbol, ResolverSourceLocation

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

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

  /// base name → exported constant. Unlike Functions, never a vector --
  /// variables don't overload.
  std::unordered_map<std::string, ResolverConstantSymbol> Variables;

  explicit ResolverModule(std::string name) : Name(std::move(name)) {}

  /**
   * @brief Register a function overload.
   * Duplicate registrations (same BaseName + arity + param types) are
   * silently ignored to handle prototype re-entry during the pre-pass.
   */
  void registerFunction(const ResolverFunctionSymbol &Sym);

  /// Return all registered base names (for suggestion / diagnostics).
  std::vector<std::string> getFunctionNames() const;
  /// LEC-002: names with at least one `pub` overload, sorted -- what another
  /// module may actually call, and so what a private-access hint offers.
  std::vector<std::string> getPublicFunctionNames() const;
};

// ─── ResolverModuleTable ─────────────────────────────────────────────────────

/**
 * @brief Module/import half of the Resolver's symbol table (split out of the
 * original monolithic ResolverSymbolTable -- see
 * docs/architecture/compiler_pipeline.md). Owns every imported module's
 * function/constant registry, the current file's own top-level function/
 * constant registry, and the on-demand ("flat") import bookkeeping. Owns no
 * struct/enum/error-type or lexical-scope state at all (see
 * ResolverTypeRegistry / ResolverScopeTable for those).
 */
class ResolverModuleTable {
public:
  /**
   * @brief Ensures @p ModuleName is registered as a known, imported module,
   * even if it turns out to declare zero functions (e.g. a module that only
   * declares structs/enums, like `enum Shade {...}` with no functions at
   * all). Called once per hoisted `ModuleImportExprAST`, unconditionally,
   * BEFORE any function/struct/enum inside it is processed -- registerModuleFunction()
   * ALSO lazily creates an entry, but only when a function actually exists,
   * which used to mean a function-less module was completely invisible to
   * hasModule(). Idempotent.
   */
  void registerModule(const std::string &ModuleName);

  /**
   * @brief Register a function symbol under the given imported module namespace.
   * The module is created automatically if it doesn't exist yet.
   * Use this for `import io` declarations — not for functions defined in the
   * current file (use registerCurrentFileFunction for those).
   */
  void registerModuleFunction(const std::string &ModuleName,
                               const ResolverFunctionSymbol &Sym);

  /**
   * @brief Register an `export immut` constant under the given module
   * namespace. Mirrors registerModuleFunction() exactly (auto-creates the
   * module entry if needed), except a duplicate name is a real conflict
   * (variables don't overload) -- the caller is expected to have already
   * checked existence before calling this.
   */
  void registerModuleConstant(const std::string &ModuleName,
                              const ResolverConstantSymbol &Sym);

  /**
   * @brief Look up an exported constant by name, scoped to exactly
   * @p ModuleName. Returns nullptr if that module has no such constant
   * (or isn't a known module at all).
   */
  const ResolverConstantSymbol *
  findModuleConstant(const std::string &ModuleName,
                     const std::string &Name) const;

  /**
   * @brief True if @p ModuleName names a module that was actually imported
   * somewhere in this program -- regardless of whether it happens to
   * declare any functions.
   */
  bool hasModule(const std::string &ModuleName) const;

  /**
   * @brief Return the name of every module imported somewhere in this
   * program (the keys of Modules_) -- used as suggestion candidates for a
   * `Left::Right` scope access whose `Left` isn't a known enum/struct/
   * error-type either, so is most plausibly a typo'd module name (e.g.
   * `oi::printline` meaning `io::printline`).
   */
  std::vector<std::string> getAllModuleNames() const;

  /**
   * @brief Return all base function names registered under @p ModuleName.
   */
  std::vector<std::string> getModuleFunctionNames(
      const std::string &ModuleName) const;
  /// LEC-002: the `pub` FREE functions of getModuleFunctionNames() (a method
  /// is registered as "Type.name" and is listed by getPublicMethodNames()
  /// instead, so a hint about a private function never offers `List.push`).
  std::vector<std::string> getModulePublicFunctionNames(
      const std::string &ModuleName) const;
  /// LEC-002: bare names of the `pub` methods of type @p OwnerIdentity,
  /// wherever they are declared (the receiver's module, an importer's
  /// extension methods, or the main file), sorted. The "use one of these
  /// instead" list in a private-field or private-method hint.
  std::vector<std::string> getPublicMethodNames(
      const std::string &OwnerIdentity) const;

  /**
   * @brief Return all `export immut` constant names registered under
   * @p ModuleName. Mirrors getModuleFunctionNames().
   */
  std::vector<std::string> getModuleConstantNames(
      const std::string &ModuleName) const;

  // ── Current-file function registry ──────────────────────────────────────────
  // These track `fn` definitions in the file being compiled, separately from
  // imported module functions.  This separation powers the two-tier diagnostic:
  //
  //   Error: Unknown function 'pritn'
  //     Current file:   print(int x)      — line 12
  //     From io module: io.printline(str)  — try 'io.printline(x)'

  /**
   * @brief Register a top-level fn definition from the current source file.
   * Duplicate registrations (same signature) are silently ignored.
   * Emits no error on overloads — that is the TypeChecker's job.
   */
  void registerCurrentFileFunction(const ResolverFunctionSymbol &Sym);

  /**
   * @brief Register an `export immut` constant declared at the top level of
   * the current (main) source file.
   */
  void registerCurrentFileConstant(const ResolverConstantSymbol &Sym);

  /**
   * @brief Look up an `export immut` constant declared in the current file
   * by name. Returns nullptr if no such constant exists there.
   */
  const ResolverConstantSymbol *
  lookupCurrentFileConstant(const std::string &Name) const;

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

  // ── On-demand (flat) import registry ─────────────────────────────────────────
  // Backs `import module::symbol;` / `import module::[a, b];` /
  // `import module::[*];` (ImportMode::Symbol/SymbolList/Wildcard) -- each
  // pulls specific function names into a "context" (the main file, if
  // ContextLabel == "", or a module name, if some module itself does an
  // on-demand import of another module) so a BARE call can resolve to them,
  // same as it already can for that context's own self-declared functions.

  struct FlatImportedSymbol {
    std::string SourceModule;
    ResolverSourceLocation ImportLoc;
  };

  /**
   * @brief Register that @p Name is on-demand-imported from @p Sym.SourceModule
   * into @p ContextLabel's flat namespace.
   * Returns the PREVIOUS entry if @p Name already has a flat import in this
   * context, so the caller can raise a collision error using it. Returns
   * std::nullopt on a fresh registration.
   */
  std::optional<FlatImportedSymbol>
  registerFlatFunctionImport(const std::string &ContextLabel,
                             const std::string &Name,
                             const FlatImportedSymbol &Sym);

  /**
   * @brief Look up whether @p Name is flat-imported into @p ContextLabel.
   * Returns nullptr if not.
   */
  const FlatImportedSymbol *
  lookupFlatFunctionImport(const std::string &ContextLabel,
                           const std::string &Name) const;

  /**
   * @brief Register that struct/enum @p Name is on-demand-imported from
   * @p Sym.SourceModule into @p ContextLabel's flat namespace. Kept as a
   * SEPARATE registry (not folded into the function-import one) because a
   * struct/enum name and a function name can legally coexist under the same
   * bare spelling without colliding -- they occupy different syntactic
   * positions (type position vs. call position).
   * Returns the PREVIOUS entry on a same-name collision from a different
   * module, std::nullopt on a fresh registration.
   */
  std::optional<FlatImportedSymbol>
  registerFlatTypeImport(const std::string &ContextLabel,
                         const std::string &Name,
                         const FlatImportedSymbol &Sym);

  /**
   * @brief True if struct/enum @p Name has a flat (on-demand) import into
   * @p ContextLabel. Used by ResolverSymbolTable::isTypeVisibleUnqualified()
   * alongside ResolverTypeRegistry::getTypeOwningModule() -- that check
   * spans both registries, so it lives on the composite ResolverSymbolTable
   * rather than on either sub-table alone.
   */
  bool hasFlatTypeImport(const std::string &ContextLabel,
                        const std::string &Name) const;

  /// FDN-003: the flat type import of bare @p Name into @p ContextLabel, or
  /// nullptr -- its SourceModule is what turns the bare spelling into a
  /// TypeIdentity (TypeIdentity::make(SourceModule, Name)).
  const FlatImportedSymbol *lookupFlatTypeImport(const std::string &ContextLabel,
                                                 const std::string &Name) const;

  /**
   * @brief Register that `export immut` constant @p Name is on-demand-
   * imported from @p Sym.SourceModule into @p ContextLabel's flat namespace.
   * Kept as its own registry for the same reason the type registry is
   * separate from the function one.
   * Returns the PREVIOUS entry on a same-name collision from a different
   * module, std::nullopt on a fresh registration.
   */
  std::optional<FlatImportedSymbol>
  registerFlatConstantImport(const std::string &ContextLabel,
                             const std::string &Name,
                             const FlatImportedSymbol &Sym);

  /**
   * @brief Look up whether constant @p Name is flat-imported into
   * @p ContextLabel. Returns nullptr if not.
   */
  const FlatImportedSymbol *
  lookupFlatConstantImport(const std::string &ContextLabel,
                           const std::string &Name) const;

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

  /// Emits the "Current-File Functions (Tier 1)" section of
  /// ResolverSymbolTable::dump().
  void dumpCurrentFileFunctionsBrief(std::ostream &OS) const;

  /// Emits the "Imported Module Functions (Tier 2)" section of
  /// ResolverSymbolTable::dump().
  void dumpModuleFunctionsBrief(std::ostream &OS) const;

  /// Emits the "Imported Modules" section of
  /// ResolverSymbolTable::dumpModules() (full detail: extern/vararg/loc).
  void dumpModulesDetailed(std::ostream &OS) const;

  /// Emits the "On-Demand Flat Imports" section of
  /// ResolverSymbolTable::dumpModules().
  void dumpFlatImports(std::ostream &OS) const;

  /// Emits the "Main-File Top-Level Functions" section of
  /// ResolverSymbolTable::dumpModules().
  void dumpCurrentFileFunctionsDetailed(std::ostream &OS) const;

private:
  /// Imported module registries (one per 'import X' statement).
  std::unordered_map<std::string, std::unique_ptr<ResolverModule>> Modules_;

  /// Top-level fn definitions from the current source file.
  /// Keyed by bare function name; value is the list of overloads.
  /// Separate from Modules_ so the two-tier error system can distinguish
  /// "this file" from "imported modules" without string-matching module names.
  std::unordered_map<std::string, std::vector<ResolverFunctionSymbol>>
      CurrentFileFunctions_;

  /// On-demand flat function imports, keyed by context ("" = main file,
  /// else a module name), then by the bare imported name.
  std::unordered_map<std::string, std::unordered_map<std::string, FlatImportedSymbol>>
      FlatImportedFunctions_;

  /// On-demand flat struct/enum TYPE imports -- same shape and keying as
  /// FlatImportedFunctions_ above, kept as a separate map.
  std::unordered_map<std::string, std::unordered_map<std::string, FlatImportedSymbol>>
      FlatImportedTypes_;

  /// `export immut` constants declared at the top level of the current
  /// (main) source file. Mirrors CurrentFileFunctions_ but never a vector --
  /// variables don't overload.
  std::unordered_map<std::string, ResolverConstantSymbol> CurrentFileConstants_;

  /// On-demand flat CONSTANT imports -- same shape and keying as
  /// FlatImportedFunctions_/FlatImportedTypes_ above, kept as a third
  /// separate map since a constant, a function, and a type can all legally
  /// share one bare spelling without colliding.
  std::unordered_map<std::string, std::unordered_map<std::string, FlatImportedSymbol>>
      FlatImportedConstants_;
};

#endif // RESOLVER_MODULE_TABLE_H
