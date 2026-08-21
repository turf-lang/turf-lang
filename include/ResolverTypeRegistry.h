#ifndef RESOLVER_TYPE_REGISTRY_H
#define RESOLVER_TYPE_REGISTRY_H

#include "ResolverSymbolBase.h" // ResolverSourceLocation

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Struct/enum/user-error-type registry half of the Resolver's symbol
 * table (split out of the original monolithic ResolverSymbolTable -- see
 * docs/planning/COMPILER_MODULE_BREAKDOWN.md). Tracks every struct, enum,
 * and `error` type name declared in the current file or any transitively-
 * imported module, plus which module owns each one -- struct/enum/error
 * names live in ONE flat namespace shared by the current file and every
 * transitively-imported module (unlike functions, which are namespaced
 * per-module), so this is what powers `Module::TypeName` qualifier
 * validation without involving the TypeChecker or LLVM.
 */
class ResolverTypeRegistry {
public:
  // ── Struct name registry ─────────────────────────────────────────────────
  // Tracks struct names declared in the current file so the Resolver can
  // validate NamedType references in Pass 2 without asking the TypeChecker.
  // Populated during Pass 1 (hoistTopLevel); queried during visitStructDecl
  // and any future pass that needs to distinguish struct names from unknowns.

  /**
   * @brief Register a struct name hoisted from a top-level StructDeclExprAST.
   *
   * Returns the ORIGINAL declaration's location if @p Name was already
   * registered (so the caller can raise a clean DuplicateTypeNameError
   * instead of reaching Codegen at all), or std::nullopt if this is a fresh
   * registration.
   *
   * @p ModuleName records WHICH module actually declares this type ("" for
   * the main file) -- powers getTypeOwningModule(), used to validate an
   * explicit `Module::TypeName` qualifier actually names the RIGHT module,
   * not just A real one.
   */
  std::optional<ResolverSourceLocation>
  registerStruct(const std::string &Name, ResolverSourceLocation Loc,
                 const std::string &ModuleName = "");

  /**
   * @brief True if @p Name was registered as a struct in the current file.
   * Used by the Resolver to validate NamedType field references.
   */
  bool isKnownStruct(const std::string &Name) const;

  /**
   * @brief Return all registered struct names (sorted).
   * Used as the candidate list for Damerau-Levenshtein suggestions when an
   * unknown named type is encountered in a struct field declaration.
   */
  std::vector<std::string> getAllStructNames() const;

  // ── Enum registry ─────────────────────────────────────────────────────────

  // Same flat, module-blind namespace concern as registerStruct() above
  // applies to enums (they share the type namespace with structs at the
  // Codegen/LLVM layer too). Returns the original declaration's location on
  // a duplicate name, std::nullopt on a fresh registration.
  std::optional<ResolverSourceLocation>
  registerEnumType(const std::string& Name, ResolverSourceLocation Loc,
                   std::shared_ptr<TurfEnumType> ET,
                   const std::string &ModuleName = "");
  std::shared_ptr<TurfEnumType> lookupEnumType(const std::string& Name) const;
  bool isEnumName(const std::string& Name) const;
  std::vector<std::string> getAllEnumNames() const;

  // ── User-declared `error` type registry (docs/planning/Errors.md) ────────

  // Same shape as the enum registry above -- a flat, module-blind namespace,
  // own duplicate-name map (not cross-checked against structs/enums, same as
  // struct vs. enum today). Returns the original declaration's location on a
  // duplicate name, std::nullopt on a fresh registration.
  std::optional<ResolverSourceLocation>
  registerErrorType(const std::string& Name, ResolverSourceLocation Loc,
                    std::shared_ptr<TurfUserErrorType> ET,
                    const std::string &ModuleName = "");
  std::shared_ptr<TurfUserErrorType> lookupErrorType(const std::string& Name) const;
  bool isErrorTypeName(const std::string& Name) const;
  std::vector<std::string> getAllErrorTypeNames() const;

  /**
   * @brief Which module declares @p Name ("" for the main file, or if
   * @p Name isn't a known struct/enum at all). Struct/enum names are
   * globally unique once declared (see registerStruct()'s doc comment on
   * the flat-namespace collision rule), so there is always exactly one
   * owner to report.
   */
  std::string getTypeOwningModule(const std::string &Name) const;

  /**
   * @brief Every struct/enum name declared directly by @p ModuleName (not
   * transitively -- a nested import inside that module doesn't count).
   * Used by registerOnDemandImports()'s Wildcard branch to flat-import
   * every type a module owns, mirroring ResolverModuleTable::
   * getModuleFunctionNames(). Operates purely on TypeOwningModule_ below,
   * same as getTypeOwningModule() -- kept here rather than on
   * ResolverModuleTable for that reason, even though it's queried "by
   * module name" the way several ResolverModuleTable methods are.
   */
  std::vector<std::string> getModuleTypeNames(const std::string &ModuleName) const;

  /// Emits the "Registered Struct Names (Pass 1 Hoist)" section of
  /// ResolverSymbolTable::dump().
  void dumpStructNames(std::ostream &OS) const;

  /// Emits the "Struct/Enum Type Ownership" section of
  /// ResolverSymbolTable::dumpModules().
  void dumpTypeOwnership(std::ostream &OS) const;

private:
  /// Struct names declared in the current file or any transitively-imported
  /// module (hoisted in Pass 1), mapped to their declaration site so a
  /// same-named second declaration can be reported with a "first declared
  /// here" related location. Queried in Pass 2 to validate NamedType field
  /// references without involving the TypeChecker or LLVM.
  std::unordered_map<std::string, ResolverSourceLocation> RegisteredStructs_;

  /// Enums declared in the current file or any transitively-imported module.
  std::unordered_map<std::string, std::shared_ptr<TurfEnumType>> RegisteredEnums_;

  /// Parallel to RegisteredEnums_, since TurfEnumType itself carries no
  /// declaration location -- see registerEnumType()'s doc comment.
  std::unordered_map<std::string, ResolverSourceLocation> EnumDeclLocs_;

  /// User-declared `error` types (docs/planning/Errors.md), declared in the
  /// current file or any transitively-imported module. Parallel shape to
  /// RegisteredEnums_/EnumDeclLocs_ above.
  std::unordered_map<std::string, std::shared_ptr<TurfUserErrorType>> RegisteredErrorTypes_;
  std::unordered_map<std::string, ResolverSourceLocation> ErrorDeclLocs_;

  /// Which module owns each struct/enum/error name ("" = main file). See
  /// getTypeOwningModule()'s doc comment.
  std::unordered_map<std::string, std::string> TypeOwningModule_;
};

#endif // RESOLVER_TYPE_REGISTRY_H
