#include "SymbolTable.h"
#include <algorithm>
#include <unordered_set>

std::unique_ptr<SymbolTable> GlobalSymbolTable;

SymbolTable::SymbolTable(const SymbolTable &Other)
    : ScopeStack(Other.ScopeStack), AllSymbols(Other.AllSymbols),
      NextID(Other.NextID) {}

SymbolTable &SymbolTable::operator=(const SymbolTable &Other) {
  if (this == &Other)
    return *this;

  ScopeStack = Other.ScopeStack;
  AllSymbols = Other.AllSymbols;
  NextID = Other.NextID;
  return *this;
}

void InitializeSymbolTable() {
  GlobalSymbolTable = std::make_unique<SymbolTable>();
}

void SymbolTable::EnterScope() {
  ScopeStack.emplace_back(ScopeStack.size());
}

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
  AllSymbols.emplace(ID, Symbol(ID, Name, std::move(Type), DeclLoc, Alloca, Level));
  
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

Symbol *SymbolTable::GetSymbolByID(SymbolID ID) {
  auto It = AllSymbols.find(ID);
  if (It != AllSymbols.end()) {
    return &It->second;
  }
  return nullptr;
}

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

TurfTypePtr SymbolTable::LookupTypeInCurrentScope(const std::string &Name) const {
  if (ScopeStack.empty())
    return nullptr;
  const Scope &CurrentScope = ScopeStack.back();
  auto It = CurrentScope.Types.find(Name);
  if (It != CurrentScope.Types.end()) {
    return It->second;
  }
  return nullptr;
}

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
