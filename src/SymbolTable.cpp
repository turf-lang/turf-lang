/**
 * @file SymbolTable.cpp
 * @brief Scope and symbol management for the compiler.
 * @details Implements a hierarchical symbol table to track variable scopes,
 *          type registrations, and early-exit control flow (e.g., returns)
 *          during AST generation and compilation.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "SymbolTable.h"
#include "Types.h"
#include "SuggestionIndex.h"
#include <algorithm>
#include <unordered_set>

std::unique_ptr<SymbolTable> GlobalSymbolTable;

SymbolTable::SymbolTable(const SymbolTable &Other)
    : ScopeStack(Other.ScopeStack), AllSymbols(Other.AllSymbols),
      NextID(Other.NextID), CurrentModule(Other.CurrentModule),
      ModuleStack(Other.ModuleStack) {
  for (const auto &pair : Other.Modules) {
    Modules[pair.first] = std::make_shared<TurfModule>(*pair.second);
  }
}

SymbolTable &SymbolTable::operator=(const SymbolTable &Other) {
  if (this == &Other)
    return *this;

  ScopeStack = Other.ScopeStack;
  AllSymbols = Other.AllSymbols;
  NextID = Other.NextID;
  CurrentModule = Other.CurrentModule;
  ModuleStack = Other.ModuleStack;
  Modules.clear();
  for (const auto &pair : Other.Modules) {
    Modules[pair.first] = std::make_shared<TurfModule>(*pair.second);
  }
  return *this;
}

void InitializeSymbolTable() {
  GlobalSymbolTable = std::make_unique<SymbolTable>();
}

void SymbolTable::EnterScope() { ScopeStack.emplace_back(ScopeStack.size()); }

void SymbolTable::ExitScope() {
  if (ScopeStack.empty())
    return;

  // Remove symbols from this scope
  const Scope &CurrentScope = ScopeStack.back();
  for (const auto &pair : CurrentScope.Symbols) {
    AllSymbols.erase(pair.second);
  }

  ScopeStack.pop_back();

  // Explicitly do NOT propagate HasEarlyExit into the parent scope.
  // A `return` inside a function body (or an `if`-branch inside a function)
  // marks the function's scope with HasEarlyExit=true.  When ExitScope()
  // pops that function scope, the outer (global/caller) scope must remain
  // unaffected.  Without this guarantee, any top-level declaration that
  // appears after a function definition containing a `return` statement would
  // be falsely flagged as unreachable code.
  //
  // The parent's HasEarlyExit is intentionally left as-is (not touched here)
  // because it may legitimately be true if the *parent* scope itself already
  // contains an early exit (e.g. a top-level `return` — which is illegal, but
  // that is caught elsewhere).
}

void SymbolTable::EnterModule(const std::string &ModuleName) {
  ModuleStack.push_back(CurrentModule);
  CurrentModule = ModuleName;
}

void SymbolTable::ExitModule() {
  if (ModuleStack.empty())
    return;
  CurrentModule = ModuleStack.back();
  ModuleStack.pop_back();
}

/**
 * @brief Declares a new symbol in the current scope.
 *
 * Allocates a unique SymbolID, stores the type and source location, and
 * optionally binds it to an LLVM memory allocation (AllocaInst) for codegen.
 *
 * @param Name The identifier name of the symbol.
 * @param Type The TurfType representation of the symbol.
 * @param DeclLoc The source location where the symbol is declared.
 * @param Alloca An optional pointer to the LLVM AllocaInst.
 * @return SymbolID The unique identifier assigned to the new symbol.
 */
SymbolID SymbolTable::DeclareSymbol(const std::string &Name, TurfTypePtr Type,
                                    SourceLocation DeclLoc,
                                    llvm::AllocaInst *Alloca) {
  if (ScopeStack.empty())
    EnterScope();

  SymbolID ID = NextID++;
  size_t Level = GetCurrentLevel();

  // Add to current scope
  ScopeStack.back().Symbols[Name] = ID;

  // Add to all symbols map
  AllSymbols.emplace(ID,
                     Symbol(ID, Name, std::move(Type), DeclLoc, Alloca, Level));

  return ID;
}

Symbol *SymbolTable::LookupSymbol(const std::string &Name) {
  // Search from innermost to outermost scope
  for (auto It = ScopeStack.rbegin(); It != ScopeStack.rend(); ++It) {
    auto SymIt = It->Symbols.find(Name);
    if (SymIt != It->Symbols.end()) {
      auto SymbolIt = AllSymbols.find(SymIt->second);
      if (SymbolIt != AllSymbols.end()) {
        return &SymbolIt->second;
      }
    }
  }
  return nullptr;
}

// NOTE: LookupSymbolAnyScope(Name) -- a scan of every currently-live symbol
// across ALL active scopes, preferring the deepest ScopeLevel on a name
// collision -- used to live here. Removed after confirming via repo-wide
// grep it had zero callers; LookupSymbol() (innermost-to-outermost scope
// walk, stopping at the first match) is what every real caller actually
// uses.

Symbol *SymbolTable::LookupSymbolInCurrentScope(const std::string &Name) {
  if (ScopeStack.empty())
    return nullptr;

  const Scope &CurrentScope = ScopeStack.back();
  auto It = CurrentScope.Symbols.find(Name);
  if (It != CurrentScope.Symbols.end()) {
    auto SymIt = AllSymbols.find(It->second);
    if (SymIt != AllSymbols.end()) {
      return &SymIt->second;
    }
  }
  return nullptr;
}

// NOTE: GetSymbolByID(ID) -- a direct SymbolID -> Symbol* lookup -- used to
// live here. Removed after confirming via repo-wide grep it had zero
// callers.

void SymbolTable::RegisterType(const std::string &Name, TurfTypePtr Type) {
  if (ScopeStack.empty())
    EnterScope();
  ScopeStack.back().Types[Name] = std::move(Type);
}

TurfTypePtr SymbolTable::LookupType(const std::string &Name) const {
  for (auto It = ScopeStack.rbegin(); It != ScopeStack.rend(); ++It) {
    auto TypeIt = It->Types.find(Name);
    if (TypeIt != It->Types.end()) {
      return TypeIt->second;
    }
  }
  return nullptr;
}

// NOTE: LookupTypeInCurrentScope(Name) -- a type lookup restricted to just
// the innermost scope -- used to live here. Removed after confirming via
// repo-wide grep it had zero callers; LookupType() (innermost-to-outermost
// walk) is what every real caller actually uses.

bool SymbolTable::IsSymbolInCurrentScope(const std::string &Name) const {
  if (ScopeStack.empty())
    return false;

  const Scope &CurrentScope = ScopeStack.back();
  return CurrentScope.Symbols.find(Name) != CurrentScope.Symbols.end();
}

Symbol *SymbolTable::FindShadowedSymbol(const std::string &Name) const {
  // Skip the current scope, search outer scopes
  if (ScopeStack.size() <= 1)
    return nullptr;

  for (auto It = ScopeStack.rbegin() + 1; It != ScopeStack.rend(); ++It) {
    auto SymIt = It->Symbols.find(Name);
    if (SymIt != It->Symbols.end()) {
      auto SymbolIt = AllSymbols.find(SymIt->second);
      if (SymbolIt != AllSymbols.end()) {
        return const_cast<Symbol *>(&SymbolIt->second);
      }
    }
  }
  return nullptr;
}

std::vector<std::string> SymbolTable::GetAllVisibleNames() const {
  std::vector<std::string> Names;

  std::unordered_set<std::string> Seen;

  // Collect visible names from innermost to outermost scope so shadowed
  // identifiers do not leak into typo suggestions.
  for (auto It = ScopeStack.rbegin(); It != ScopeStack.rend(); ++It) {
    for (const auto &pair : It->Symbols) {
      if (Seen.insert(pair.first).second) {
        Names.push_back(pair.first);
      }
    }
  }

  return Names;
}

void SymbolTable::MarkEarlyExit() {
  if (!ScopeStack.empty()) {
    ScopeStack.back().HasEarlyExit = true;
  }
}

bool SymbolTable::CurrentScopeHasEarlyExit() const {
  if (ScopeStack.empty())
    return false;
  return ScopeStack.back().HasEarlyExit;
}

void SymbolTable::ResetEarlyExitForCurrentScope() {
  // Clear the HasEarlyExit flag on the innermost scope.
  // Called by the pre-pass setjmp recovery handler to clean up any stale
  // early-exit state that was set by a `return` inside a function whose
  // compilation was aborted via longjmp (and therefore never reached the
  // matching ExitScope() call).  Without this, the global scope can
  // inherit HasEarlyExit=true and falsely flag subsequent top-level
  // declarations as unreachable.
  if (!ScopeStack.empty()) {
    ScopeStack.back().HasEarlyExit = false;
  }
}

void SymbolTable::ResetToDepth(size_t TargetDepth) {
  // Unwind the scope stack to TargetDepth by calling ExitScope() for each
  // extra scope that was pushed but never popped (e.g. because longjmp
  // fired mid-function and skipped the normal ExitScope() call at the end
  // of FuncDefExprAST::codegen).
  while (ScopeStack.size() > TargetDepth) {
    ExitScope();
  }
}

// ---------------------------------------------------------------------------
// Phase 2: TurfModule implementation
// ---------------------------------------------------------------------------

// NOTE: TurfModule::lookupFunction(baseName, argTypes) -- an arity +
// exact-type-match + numeric-coercion overload resolver for a single module
// -- used to live here. Removed after confirming via repo-wide grep it had
// zero callers (its only caller, SymbolTable::resolveModuleFunction, was
// itself dead and removed alongside it -- see the NOTE at
// RegisterModuleFunction's declaration in SymbolTable.h for what Codegen
// actually uses instead).

std::vector<std::string> TurfModule::getFunctionNames() const {
  std::vector<std::string> Names;
  Names.reserve(Functions.size());
  for (const auto &pair : Functions)
    Names.push_back(pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

// ---------------------------------------------------------------------------
// Phase 2: SymbolTable module-registry methods
// ---------------------------------------------------------------------------

void SymbolTable::RegisterModuleFunction(const std::string &moduleName,
                                          const FunctionSymbol &sym) {
  auto it = Modules.find(moduleName);
  if (it == Modules.end()) {
    Modules[moduleName] = std::make_shared<TurfModule>(moduleName);
    it = Modules.find(moduleName);
  }
  it->second->registerFunction(sym);
}

// NOTE: resolveModuleFunction(moduleName, funcName, argTypes) and
// HasModule(moduleName) used to live here too. Both were removed after
// confirming via repo-wide grep they had zero callers -- see the NOTE at
// RegisterModuleFunction's declaration in SymbolTable.h for the full
// explanation (HasModule() in particular was removed as a direct
// consequence of fixing a real bug in MethodCallExprAST::codegen()).

std::vector<std::string> SymbolTable::GetModuleFunctionNames(
    const std::string &moduleName) const {
  auto it = Modules.find(moduleName);
  if (it == Modules.end())
    return {};
  return it->second->getFunctionNames();
}

RankedSuggestions SymbolTable::GetRankedSuggestions(const std::string &Name) const {
  RankedSuggestions Result;

  // 1. Gather all local visible names (variables, current scope functions)
  std::vector<std::string> VisibleNames = GetAllVisibleNames();
  
  // Also include functions from the "main" module (current file's functions)
  std::vector<std::string> MainFuncs = GetModuleFunctionNames("main");
  for (const auto &MF : MainFuncs) {
    if (std::find(VisibleNames.begin(), VisibleNames.end(), MF) == VisibleNames.end()) {
      VisibleNames.push_back(MF);
    }
  }

  Result.Local = SuggestionIndex::suggestVisible(Name, VisibleNames);

  // 2. Gather from imported modules (skip "main")
  for (const auto &pair : Modules) {
    if (pair.first == "main") continue;
    // Only check if it's actually an imported module (has functions)
    if (pair.second->Functions.empty()) continue;

    std::vector<std::string> ModFuncs = pair.second->getFunctionNames();
    std::vector<std::string> ModMatches = SuggestionIndex::suggestVisible(Name, ModFuncs);
    if (!ModMatches.empty()) {
      Result.Module[pair.first] = ModMatches;
    }
  }

  return Result;
}

// NOTE: GetSymbolIDs() -- returned every currently-live SymbolID across all
// active scopes -- used to live here. Removed after confirming via
// repo-wide grep it had zero callers.
