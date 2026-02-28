#include "SymbolTable.h"
#include <algorithm>

std::unique_ptr<SymbolTable> GlobalSymbolTable;

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
}

SymbolID SymbolTable::DeclareSymbol(const std::string &Name, TurfType Type,
                                    SourceLocation DeclLoc,
                                    llvm::AllocaInst *Alloca) {
  if (ScopeStack.empty())
    EnterScope();
  
  SymbolID ID = NextID++;
  size_t Level = GetCurrentLevel();
  
  // Add to current scope
  ScopeStack.back().Symbols[Name] = ID;
  
  // Add to all symbols map
  AllSymbols.emplace(ID, Symbol(ID, Name, Type, DeclLoc, Alloca, Level));
  
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
  
  // Collect all visible names (from all scopes)
  for (const auto &Scope : ScopeStack) {
    for (const auto &pair : Scope.Symbols) {
      Names.push_back(pair.first);
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
