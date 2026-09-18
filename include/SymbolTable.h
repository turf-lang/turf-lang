#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "Lexer.h"
#include "Types.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration only -- llvm::AllocaInst is used exclusively as a
// pointer throughout this header and SymbolTable.cpp (stored, passed,
// compared; never dereferenced). Pulling in the real
// llvm/IR/Instructions.h here dragged the whole IR type hierarchy into
// every TU that includes SymbolTable.h transitively -- measured:
// src/ParserDecl.cpp alone preprocessed to 182,910 lines, of which this one
// header contributed 119,047 (~65%), costing 3.07s of syntax-check time at
// -O2 (HYG-001, docs/planning/ARCHITECTURE_OVERHAUL.md §3.4). The real
// include lives in SymbolTable.cpp instead, the one TU that actually
// constructs/consumes real AllocaInst values from Codegen.
namespace llvm { class AllocaInst; }

// Unique identifier for each symbol declaration
using SymbolID = size_t;

// Represents a single symbol (variable) in the program
struct Symbol {
  SymbolID ID;      // Unique stable identifier
  std::string Name; // Variable name
  TurfTypePtr Type; // Variable type (dynamic — works for list<T>, int[5], etc.)
  SourceLocation DeclLoc;   // Where it was declared
  llvm::AllocaInst *Alloca; // LLVM memory location
  size_t ScopeLevel;        // Nesting depth (0 = global, 1 = first block, etc.)
  // Array size is encoded inside Type (ArrayType::getSize()) — no separate
  // field needed.

  Symbol(SymbolID ID, std::string Name, TurfTypePtr Type,
         SourceLocation DeclLoc, llvm::AllocaInst *Alloca, size_t ScopeLevel)
      : ID(ID), Name(std::move(Name)), Type(std::move(Type)), DeclLoc(DeclLoc),
        Alloca(Alloca), ScopeLevel(ScopeLevel) {}
};

// Represents a lexical scope (block, function body, etc.)
struct Scope {
  size_t Level; // Nesting depth
  std::map<std::string, SymbolID>
      Symbols; // Name -> SymbolID mapping for this scope
  std::map<std::string, TurfTypePtr>
      Types;                 // Name -> Type mapping for this scope
  bool HasEarlyExit = false; // True if scope contains return/break

  explicit Scope(size_t Level) : Level(Level) {}
};

// ---------------------------------------------------------------------------
// Phase 2: Module-aware symbol resolution
// ---------------------------------------------------------------------------

/**
 * @brief Represents a single overloadable function registered in a module.
 *
 * Stores both the user-visible base name (e.g. "printline") and the fully
 * mangled LLVM function name (e.g. "turf_io_printline_int").  The ParamTypes
 * vector has one entry per formal parameter and is used for overload
 * resolution at call sites.
 */
struct FunctionSymbol {
  std::string BaseName;    ///< e.g. "printline"
  std::string MangledName; ///< e.g. "turf_io_printline_int"
  std::vector<TurfTypePtr> ParamTypes;
  TurfTypePtr ReturnType;
  bool IsVarArg    = false; ///< true for variadic extern fn (e.g. printf)
  int Arity = 0; ///< Number of formal parameters (cached for quick checks)
};

/**
 * @brief Represents one imported module (e.g. "io", "math", or the user file).
 *
 * Each module has a map from base function name to the vector of overloads
 * registered under that name.  Lookup is namespace-isolated: looking up
 * "abs" in the "math" module will never accidentally find "io.printline".
 */
class TurfModule {
public:
  std::string Name; ///< e.g. "io", "math", "main"

  /// base name -> list of overloads
  std::unordered_map<std::string, std::vector<FunctionSymbol>> Functions;

  explicit TurfModule(const std::string &name) : Name(name) {}

  /**
   * @brief Add a function overload to this module.
   *
   * Dedups by arity + per-parameter type name (mirrors
   * ResolverModule::registerFunction in ResolverSymbolTable.h, the
   * Resolver-side twin of this table) rather than MangledName string
   * equality. Behaviorally these are equivalent for every current caller
   * -- a mangled name is deterministically derived from module + base name
   * + parameter types, so two registrations with the same MangledName
   * always have the same signature anyway -- but comparing the actual
   * TurfType data directly doesn't depend on the mangling scheme staying
   * in lockstep with signature identity, and matches the Resolver-side
   * table's own logic instead of re-deriving the same fact a second way.
   * A true signature COLLISION between two different declarations (as
   * opposed to the same declaration seen twice, e.g. a prototype pass then
   * the real pass) is caught upstream, at the point each symbol is
   * actually created -- see FuncDefExprAST::codegen()'s "Duplicate
   * function definition" check and ExternFuncDeclExprAST::codegen()'s
   * signature-comparison check -- so a second registration reaching this
   * function with a matching signature is always the legitimate re-entry
   * case, safe to silently ignore.
   */
  void registerFunction(const FunctionSymbol &sym) {
    auto &overloads = Functions[sym.BaseName];
    for (const auto &existing : overloads) {
      if (existing.Arity != sym.Arity)
        continue;
      bool SameParams = true;
      for (size_t i = 0; i < existing.ParamTypes.size(); ++i) {
        const std::string ExistingName =
            existing.ParamTypes[i] ? existing.ParamTypes[i]->getName() : "";
        const std::string NewName =
            sym.ParamTypes[i] ? sym.ParamTypes[i]->getName() : "";
        if (ExistingName != NewName) {
          SameParams = false;
          break;
        }
      }
      if (SameParams)
        return; // already registered (same signature)
    }
    overloads.push_back(sym);
  }

  // NOTE: this class used to also have a lookupFunction(baseName, argTypes)
  // overload-resolution method (arity + exact-type-match + numeric-coercion
  // fallback). Removed after confirming via repo-wide grep it had zero
  // callers -- its only caller, SymbolTable::resolveModuleFunction, was
  // itself dead and removed alongside it. Codegen no longer resolves
  // overloads itself: it reads ResolvedCallTarget, set by the
  // Resolver/TypeChecker passes, directly (see the many "Codegen trusts the
  // Resolver" comments throughout Codegen.cpp). This mirrors the identical
  // cleanup already done to ResolverModule::lookupFunction in
  // ResolverSymbolTable.h/.cpp.

  /// Return all base function names registered in this module (for suggestions)
  std::vector<std::string> getFunctionNames() const;
};

// ---------------------------------------------------------------------------

// Stack-based symbol table with lexical scope tracking
class SymbolTable {
private:
  std::vector<Scope> ScopeStack;         // Stack of active scopes
  std::map<SymbolID, Symbol> AllSymbols; // All symbols by ID
  SymbolID NextID = 1;                   // Next available symbol ID
  std::string CurrentModule;             // Current module namespace
  std::vector<std::string> ModuleStack;  // Stack of active modules

  // Phase 2: per-module function registry
  std::unordered_map<std::string, std::shared_ptr<TurfModule>> Modules;

public:
  SymbolTable() {
    // Start with global scope
    EnterScope();
    // Register the implicit "main" module for user-defined functions
    Modules["main"] = std::make_shared<TurfModule>("main");
  }

  SymbolTable(const SymbolTable &Other);
  SymbolTable &operator=(const SymbolTable &Other);

  // Scope management
  void EnterScope();
  void ExitScope();
  size_t GetCurrentLevel() const { return ScopeStack.size() - 1; }

  // Module management
  void EnterModule(const std::string &ModuleName);
  void ExitModule();
  std::string GetCurrentModule() const { return CurrentModule; }
  bool IsInModule() const { return !CurrentModule.empty(); }

  // Symbol declaration
  SymbolID DeclareSymbol(const std::string &Name, TurfTypePtr Type,
                         SourceLocation DeclLoc, llvm::AllocaInst *Alloca);

  // Symbol lookup
  //
  // NOTE: LookupSymbolAnyScope(Name), GetSymbolByID(ID), and
  // LookupTypeInCurrentScope(Name) used to also live here. Removed after
  // confirming via repo-wide grep that all three had zero callers anywhere.
  Symbol *LookupSymbol(const std::string &Name);
  Symbol *LookupSymbolInCurrentScope(const std::string &Name);

  // Type registry
  void RegisterType(const std::string &Name, TurfTypePtr Type);
  TurfTypePtr LookupType(const std::string &Name) const;

  // Diagnostics helpers
  //
  // NOTE: GetSymbolIDs() used to also live here (returned every currently-
  // live SymbolID across all active scopes). Removed after confirming via
  // repo-wide grep it had zero callers.
  bool IsSymbolInCurrentScope(const std::string &Name) const;
  Symbol *FindShadowedSymbol(const std::string &Name) const;
  std::vector<std::string> GetAllVisibleNames() const;

  // Control flow tracking
  void MarkEarlyExit();
  bool CurrentScopeHasEarlyExit() const;
  // Recovery helpers: called by the pre-pass setjmp handler to undo orphaned
  // scope state left behind when a function's codegen aborts via longjmp.
  void ResetEarlyExitForCurrentScope();
  void ResetToDepth(size_t TargetDepth);
  size_t GetDepth() const { return ScopeStack.size(); }

  // -------------------------------------------------------------------------
  // Phase 2: Module-aware function registry and resolver
  // -------------------------------------------------------------------------

  /**
   * @brief Register a function symbol under the given module name.
   *
   * If the module doesn't exist yet it is created automatically.  Called from
   * FuncDefExprAST::codegen() for every function definition compiled.
   */
  void RegisterModuleFunction(const std::string &moduleName,
                               const FunctionSymbol &sym);

  // NOTE: this class used to also have a resolveModuleFunction(moduleName,
  // funcName, argTypes) method (a single-module overload resolver). Removed
  // after confirming via repo-wide grep it had zero callers -- Codegen no
  // longer resolves module calls itself, it reads ResolvedCallTarget (set by
  // the Resolver/TypeChecker passes) directly.
  //
  // HasModule() used to be called from MethodCallExprAST::codegen() to
  // distinguish a module-qualified call from a struct method call -- but
  // that check derived "is this a module call" from the raw base-variable
  // NAME, which doesn't know about local variables at all: a variable
  // sharing a name with an imported module (concretely `io`, auto-imported
  // into every program via prelude.tr) was misread as the module, producing
  // an out-of-bounds std::vector crash once the Resolver started correctly
  // treating such a variable as a variable. The fix (see
  // MethodCallExprAST::codegen()) was to check Var->ResolvedModuleName
  // instead -- the Resolver's own verdict, which already knows about
  // variable shadowing -- which removed HasModule()'s only remaining caller;
  // it was removed too, once confirmed via grep.

  /**
   * @brief Return all base function names registered under a module.
   * Used to build Levenshtein-distance suggestions for typos like io.pritn().
   */
  std::vector<std::string> GetModuleFunctionNames(
      const std::string &moduleName) const;

  /**
   * @brief Get ranked suggestions across current scope and imported modules.
   */
  RankedSuggestions GetRankedSuggestions(const std::string &Name) const;
};

// Global symbol table instance
extern std::unique_ptr<SymbolTable> GlobalSymbolTable;

// Initialize the global symbol table
void InitializeSymbolTable();

#endif
