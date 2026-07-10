#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "Lexer.h"
#include "Types.h"
#include "llvm/IR/Instructions.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
  bool IsIntrinsic = false;
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
   * Duplicate mangled names are silently ignored (prototype re-entry).
   */
  void registerFunction(const FunctionSymbol &sym) {
    auto &overloads = Functions[sym.BaseName];
    for (const auto &existing : overloads)
      if (existing.MangledName == sym.MangledName)
        return; // already registered
    overloads.push_back(sym);
  }

  /**
   * @brief Find the best matching overload for the given argument types.
   *
   * Matching rules (in priority order):
   *  1. Arity match required.
   *  2. Exact type match wins.
   *  3. Numeric implicit coercion (int<->double<->bool) allowed as fallback.
   *
   * @return Pointer to the matching FunctionSymbol, or nullptr if no match.
   */
  const FunctionSymbol *lookupFunction(
      const std::string &baseName,
      const std::vector<TurfTypePtr> &argTypes) const;

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
  Symbol *LookupSymbol(const std::string &Name);
  Symbol *LookupSymbolAnyScope(const std::string &Name);
  Symbol *LookupSymbolInCurrentScope(const std::string &Name);
  Symbol *GetSymbolByID(SymbolID ID);

  // Type registry
  void RegisterType(const std::string &Name, TurfTypePtr Type);
  TurfTypePtr LookupType(const std::string &Name) const;
  TurfTypePtr LookupTypeInCurrentScope(const std::string &Name) const;

  // Diagnostics helpers
  bool IsSymbolInCurrentScope(const std::string &Name) const;
  Symbol *FindShadowedSymbol(const std::string &Name) const;
  std::vector<std::string> GetAllVisibleNames() const;
  std::vector<SymbolID> GetSymbolIDs() const;

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

  /**
   * @brief Resolve a module-qualified call (e.g. io.printline(42)).
   *
   * Only the named module is searched — cross-namespace lookup is explicitly
   * forbidden.  Returns nullptr when no overload matches.
   */
  const FunctionSymbol *resolveModuleFunction(
      const std::string &moduleName,
      const std::string &funcName,
      const std::vector<TurfTypePtr> &argTypes) const;

  /**
   * @brief Return true if a module with the given name has been registered.
   * Used by MethodCallExprAST::codegen() to distinguish module calls from
   * struct method calls without scanning FuncParamTypes.
   */
  bool HasModule(const std::string &moduleName) const;

  /**
   * @brief Return all base function names registered under a module.
   * Used to build Levenshtein-distance suggestions for typos like io.pritn().
   */
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
