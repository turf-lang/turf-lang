/**
 * @file ResolverSymbolTable.h
 * @brief The composite Resolver symbol table (Issue #2) -- see
 *        ResolverSymbolBase.h for the shared data types, and
 *        ResolverScopeTable.h/ResolverModuleTable.h/ResolverTypeRegistry.h
 *        for the 3 sub-tables this composes.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#ifndef RESOLVER_SYMBOL_TABLE_H
#define RESOLVER_SYMBOL_TABLE_H

#include "ResolverSymbolBase.h"


// Umbrella: the scope/module/type-registry halves of what used to be one
// single ResolverSymbolTable class each live in their own header now (split
// out for the same reason Parser/Resolver/TypeChecker/Codegen were --  see
// docs/architecture/compiler_pipeline.md). Included here, after the
// shared data types above (VariableSymbol, ResolverFunctionSymbol, ...) that
// all 3 depend on, so ResolverSymbolTable below can compose them.
#include "ResolverScopeTable.h"
#include "ResolverModuleTable.h"
#include "ResolverTypeRegistry.h"

#include <ostream>

// ─── ResolverSymbolTable ─────────────────────────────────────────────────────

/**
 * @brief Hierarchical symbol table for the Resolver and TypeChecker passes.
 *
 * A thin composite over 3 independent sub-tables:
 *  - ResolverScopeTable    — lexical variable scoping.
 *  - ResolverModuleTable   — imported-module/current-file function+constant
 *                            registries, and on-demand ("flat") imports.
 *  - ResolverTypeRegistry  — struct/enum/error type names and their owning
 *                            module.
 *
 * Every public method below just forwards to the sub-table that actually
 * owns the relevant state, EXCEPT isTypeVisibleUnqualified(), which
 * genuinely needs data from both ResolverTypeRegistry (getTypeOwningModule)
 * and ResolverModuleTable (the flat type-import registry) and so is
 * implemented here, at the one layer that has both.
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
  // Preserved as nested-type aliases so existing call sites that spell
  // `ResolverSymbolTable::FlatImportedSymbol` / `::ModuleFunctionMatch`
  // (e.g. Resolver::registerOnDemandImports, Resolver::visitFuncCall) don't
  // need to change -- the canonical definitions now live on
  // ResolverModuleTable, which actually owns the underlying registries.
  using FlatImportedSymbol = ResolverModuleTable::FlatImportedSymbol;
  using ModuleFunctionMatch = ResolverModuleTable::ModuleFunctionMatch;

  ResolverSymbolTable() = default;

  // ── Scope management ────────────────────────────────────────────────────────

  void enterScope() { ScopeTable_.enterScope(); }
  void exitScope() { ScopeTable_.exitScope(); }
  void resetToDepth(size_t TargetDepth) { ScopeTable_.resetToDepth(TargetDepth); }
  size_t currentDepth() const { return ScopeTable_.currentDepth(); }

  // ── Variable declarations ────────────────────────────────────────────────────

  std::shared_ptr<VariableSymbol> declareVariable(
      const std::string &Name, TurfTypePtr Type,
      ResolverSourceLocation Loc = {}, bool IsImmutable = false,
      bool IsParameter = false) {
    return ScopeTable_.declareVariable(Name, std::move(Type), Loc, IsImmutable, IsParameter);
  }

  std::shared_ptr<VariableSymbol> lookupVariable(const std::string &Name) const {
    return ScopeTable_.lookupVariable(Name);
  }

  bool isDeclaredInCurrentScope(const std::string &Name) const {
    return ScopeTable_.isDeclaredInCurrentScope(Name);
  }

  std::vector<std::string> getAllVisibleNames() const {
    return ScopeTable_.getAllVisibleNames();
  }

  // ── Module / imported function registry ─────────────────────────────────────

  void registerModuleFunction(const std::string &ModuleName,
                               const ResolverFunctionSymbol &Sym) {
    ModuleTable_.registerModuleFunction(ModuleName, Sym);
  }

  void registerModuleConstant(const std::string &ModuleName,
                              const ResolverConstantSymbol &Sym) {
    ModuleTable_.registerModuleConstant(ModuleName, Sym);
  }

  const ResolverConstantSymbol *
  findModuleConstant(const std::string &ModuleName,
                     const std::string &Name) const {
    return ModuleTable_.findModuleConstant(ModuleName, Name);
  }

  void registerModule(const std::string &ModuleName) {
    ModuleTable_.registerModule(ModuleName);
  }

  bool hasModule(const std::string &ModuleName) const {
    return ModuleTable_.hasModule(ModuleName);
  }

  std::vector<std::string> getAllModuleNames() const {
    return ModuleTable_.getAllModuleNames();
  }

  std::vector<std::string> getModuleFunctionNames(
      const std::string &ModuleName) const {
    return ModuleTable_.getModuleFunctionNames(ModuleName);
  }

  /// LEC-002: the `pub` subset of getModuleFunctionNames().
  std::vector<std::string> getModulePublicFunctionNames(
      const std::string &ModuleName) const {
    return ModuleTable_.getModulePublicFunctionNames(ModuleName);
  }

  std::vector<std::string> getModuleConstantNames(
      const std::string &ModuleName) const {
    return ModuleTable_.getModuleConstantNames(ModuleName);
  }

  // ── Current-file function registry ──────────────────────────────────────────

  void registerCurrentFileFunction(const ResolverFunctionSymbol &Sym) {
    ModuleTable_.registerCurrentFileFunction(Sym);
  }

  void registerCurrentFileConstant(const ResolverConstantSymbol &Sym) {
    ModuleTable_.registerCurrentFileConstant(Sym);
  }

  const ResolverConstantSymbol *
  lookupCurrentFileConstant(const std::string &Name) const {
    return ModuleTable_.lookupCurrentFileConstant(Name);
  }

  std::vector<const ResolverFunctionSymbol *>
  lookupCurrentFileFunctions(const std::string &Name) const {
    return ModuleTable_.lookupCurrentFileFunctions(Name);
  }

  std::vector<std::string> getAllCurrentFileFunctionNames() const {
    return ModuleTable_.getAllCurrentFileFunctionNames();
  }

  // ── On-demand (flat) import registry ─────────────────────────────────────────

  std::optional<FlatImportedSymbol>
  registerFlatFunctionImport(const std::string &ContextLabel,
                             const std::string &Name,
                             const FlatImportedSymbol &Sym) {
    return ModuleTable_.registerFlatFunctionImport(ContextLabel, Name, Sym);
  }

  const FlatImportedSymbol *
  lookupFlatFunctionImport(const std::string &ContextLabel,
                           const std::string &Name) const {
    return ModuleTable_.lookupFlatFunctionImport(ContextLabel, Name);
  }

  std::optional<FlatImportedSymbol>
  registerFlatTypeImport(const std::string &ContextLabel,
                         const std::string &Name,
                         const FlatImportedSymbol &Sym) {
    return ModuleTable_.registerFlatTypeImport(ContextLabel, Name, Sym);
  }

  std::optional<FlatImportedSymbol>
  registerFlatConstantImport(const std::string &ContextLabel,
                             const std::string &Name,
                             const FlatImportedSymbol &Sym) {
    return ModuleTable_.registerFlatConstantImport(ContextLabel, Name, Sym);
  }

  const FlatImportedSymbol *
  lookupFlatConstantImport(const std::string &ContextLabel,
                           const std::string &Name) const {
    return ModuleTable_.lookupFlatConstantImport(ContextLabel, Name);
  }

  /**
   * @brief True if struct/enum @p Name is unqualified-visible from
   * @p ContextLabel: either @p ContextLabel is the type's OWN declaring
   * module (per ResolverTypeRegistry::getTypeOwningModule()), or it was
   * brought in via an on-demand symbol/list/wildcard import
   * (ResolverModuleTable::registerFlatTypeImport()).
   *
   * Qualified access (`list::List`) is NOT gated by this at all -- it goes
   * through Resolver::validateModuleQualifier() instead, which only cares
   * whether the qualifier is correct, not whether it was on-demand-imported.
   */
  bool isTypeVisibleUnqualified(const std::string &ContextLabel,
                                const std::string &Identity) const {
    // FDN-003: @p Identity is a TypeIdentity. Visible bare iff it is
    // ContextLabel's own type, or ContextLabel flat-imported ITS bare name
    // from ITS owning module -- a flat import of some other module's
    // same-spelled type does not make this one visible.
    const std::string Owner = TypeIdentity::owningModule(Identity);
    if (Owner == ContextLabel)
      return TypeRegistry_.isKnownTypeIdentity(Identity);
    const auto *Flat = ModuleTable_.lookupFlatTypeImport(
        ContextLabel, TypeIdentity::bareName(Identity));
    return Flat && Flat->SourceModule == Owner;
  }

  /**
   * @brief FDN-003: every type a BARE spelling @p Name can mean from
   * @p ContextLabel -- ContextLabel's own `Name`, and whatever ContextLabel
   * flat-imported under that name. More than one entry is an ambiguous
   * reference (AmbiguousTypeReferenceError); none means the spelling is not
   * visible here (it may still exist qualified elsewhere --
   * identitiesWithBareName()).
   */
  std::vector<std::string> unqualifiedTypeCandidates(const std::string &ContextLabel,
                                                     const std::string &Name) const {
    std::vector<std::string> Out;
    std::string Own = TypeIdentity::make(ContextLabel, Name);
    if (TypeRegistry_.isKnownTypeIdentity(Own))
      Out.push_back(Own);
    if (const auto *Flat = ModuleTable_.lookupFlatTypeImport(ContextLabel, Name)) {
      std::string Imported = TypeIdentity::make(Flat->SourceModule, Name);
      if (Imported != Own && TypeRegistry_.isKnownTypeIdentity(Imported))
        Out.push_back(Imported);
    }
    return Out;
  }

  bool isKnownTypeIdentity(const std::string &Identity) const {
    return TypeRegistry_.isKnownTypeIdentity(Identity);
  }

  std::vector<std::string> identitiesWithBareName(const std::string &BareName) const {
    return TypeRegistry_.identitiesWithBareName(BareName);
  }

  std::vector<std::string> getModuleTypeNames(const std::string &ModuleName) const {
    return TypeRegistry_.getModuleTypeNames(ModuleName);
  }

  // ── Visibility (LEC-002, FREEZE.md §6.15) ───────────────────────────────────

  void markTypeVisibility(const std::string &Identity,
                          ResolverTypeRegistry::TypeVisibilityInfo Info) {
    TypeRegistry_.markTypeVisibility(Identity, std::move(Info));
  }
  const ResolverTypeRegistry::TypeVisibilityInfo *
  typeVisibility(const std::string &Identity) const {
    return TypeRegistry_.typeVisibility(Identity);
  }
  std::vector<std::string> getModulePublicTypeNames(const std::string &ModuleName) const {
    return TypeRegistry_.getModulePublicTypeNames(ModuleName);
  }

  /**
   * @brief Whether code in module @p FromModule ("" = the main file) may name
   *        type @p Identity: it owns the type, or the type is `pub`.
   *
   * The single rule every cross-module type reference is checked against. A
   * type never marked (a local type, a monomorphized clone) is private, which
   * is right: neither can be spelled by another module.
   */
  bool isTypeVisibleFrom(const std::string &Identity,
                         const std::string &FromModule) const {
    return TypeIdentity::owningModule(Identity) == FromModule ||
           TypeRegistry_.isTypePublic(Identity);
  }

  // ── Two-tier diagnostic helpers ──────────────────────────────────────────────

  std::vector<ModuleFunctionMatch>
  findInModules(const std::string &Name) const {
    return ModuleTable_.findInModules(Name);
  }

  std::vector<std::string> getAllModuleFunctionNames() const {
    return ModuleTable_.getAllModuleFunctionNames();
  }

  // ── Struct name registry ─────────────────────────────────────────────────

  std::optional<ResolverSourceLocation>
  registerStruct(const std::string &Name, ResolverSourceLocation Loc,
                 const std::string &ModuleName = "") {
    return TypeRegistry_.registerStruct(Name, Loc, ModuleName);
  }

  bool isKnownStruct(const std::string &Name) const {
    return TypeRegistry_.isKnownStruct(Name);
  }

  std::vector<std::string> getAllStructNames() const {
    return TypeRegistry_.getAllStructNames();
  }

  // ── Enum registry ─────────────────────────────────────────────────────────

  std::optional<ResolverSourceLocation>
  registerEnumType(const std::string& Name, ResolverSourceLocation Loc,
                   std::shared_ptr<TurfEnumType> ET,
                   const std::string &ModuleName = "") {
    return TypeRegistry_.registerEnumType(Name, Loc, std::move(ET), ModuleName);
  }
  std::shared_ptr<TurfEnumType> lookupEnumType(const std::string& Name) const {
    return TypeRegistry_.lookupEnumType(Name);
  }
  bool isEnumName(const std::string& Name) const {
    return TypeRegistry_.isEnumName(Name);
  }
  std::vector<std::string> getAllEnumNames() const {
    return TypeRegistry_.getAllEnumNames();
  }

  // ── User-declared `error` type registry ───────────────────────────────────
  // (docs/architecture/errors_and_recover.md)

  std::optional<ResolverSourceLocation>
  registerErrorType(const std::string& Name, ResolverSourceLocation Loc,
                    std::shared_ptr<TurfUserErrorType> ET,
                    const std::string &ModuleName = "") {
    return TypeRegistry_.registerErrorType(Name, Loc, std::move(ET), ModuleName);
  }
  std::shared_ptr<TurfUserErrorType> lookupErrorType(const std::string& Name) const {
    return TypeRegistry_.lookupErrorType(Name);
  }
  bool isErrorTypeName(const std::string& Name) const {
    return TypeRegistry_.isErrorTypeName(Name);
  }
  std::vector<std::string> getAllErrorTypeNames() const {
    return TypeRegistry_.getAllErrorTypeNames();
  }

  std::string getTypeOwningModule(const std::string &Name) const {
    return TypeRegistry_.getTypeOwningModule(Name);
  }

  /**
   * @brief Dumps the contents of the symbol table (all scopes and modules) to the output stream.
   */
  void dump(std::ostream &OS) const;

  /**
   * @brief Dumps EVERYTHING this Resolver instance knows about modules,
   * specifically -- unlike dump() (which mixes in variable scopes and is
   * organized around the two-tier call-resolution model), this is a
   * dedicated module-system introspection report: every imported module's
   * full function list, which module owns each struct/enum type, and every
   * on-demand (Symbol/SymbolList/Wildcard) flat import currently in effect,
   * grouped by which file/module requested it. Backs `--dump-modules`.
   */
  void dumpModules(std::ostream &OS) const;

private:
  ResolverScopeTable ScopeTable_;
  ResolverModuleTable ModuleTable_;
  ResolverTypeRegistry TypeRegistry_;
};

#endif // RESOLVER_SYMBOL_TABLE_H
