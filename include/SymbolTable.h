#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "Lexer.h"
#include "Trie.h"
#include "Types.h"
#include "llvm/IR/Instructions.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

// Unique identifier for each symbol declaration
using SymbolID = size_t;

// Represents a single symbol (variable) in the program
struct Symbol {
  SymbolID ID;                    // Unique stable identifier
  std::string Name;               // Variable name
  TurfType Type;                  // Variable type
  SourceLocation DeclLoc;         // Where it was declared
  llvm::AllocaInst *Alloca;       // LLVM memory location
  size_t ScopeLevel;              // Nesting depth (0 = global, 1 = first block, etc.)
  int ArraySize = 0;              // >0 for array variables
  
  Symbol(SymbolID ID, std::string Name, TurfType Type, SourceLocation DeclLoc,
         llvm::AllocaInst *Alloca, size_t ScopeLevel, int ArraySize = 0)
      : ID(ID), Name(std::move(Name)), Type(Type), DeclLoc(DeclLoc),
        Alloca(Alloca), ScopeLevel(ScopeLevel), ArraySize(ArraySize) {}
};

// Represents a lexical scope (block, function body, etc.)
struct Scope {
  size_t Level;                              // Nesting depth
  std::map<std::string, SymbolID> Symbols;   // Name -> SymbolID mapping for this scope
  bool HasEarlyExit = false;                 // True if scope contains return/break
  
  explicit Scope(size_t Level) : Level(Level) {}
};

// Stack-based symbol table with lexical scope tracking
class SymbolTable {
private:
  std::vector<Scope> ScopeStack;             // Stack of active scopes
  std::map<SymbolID, Symbol> AllSymbols;     // All symbols by ID
  SymbolID NextID = 1;                       // Next available symbol ID
  
public:
  SymbolTable() {
    // Start with global scope
    EnterScope();
  }
  
  // Scope management
  void EnterScope();
  void ExitScope();
  size_t GetCurrentLevel() const { return ScopeStack.size() - 1; }
  
  // Symbol declaration
  SymbolID DeclareSymbol(const std::string &Name, TurfType Type,
                         SourceLocation DeclLoc, llvm::AllocaInst *Alloca);
  SymbolID DeclareSymbol(const std::string &Name, TurfType Type,
                         SourceLocation DeclLoc, llvm::AllocaInst *Alloca,
                         int ArraySize);
  
  // Symbol lookup
  Symbol *LookupSymbol(const std::string &Name);
  Symbol *LookupSymbolInCurrentScope(const std::string &Name);
  Symbol *GetSymbolByID(SymbolID ID);
  
  // Diagnostics helpers
  bool IsSymbolInCurrentScope(const std::string &Name) const;
  Symbol *FindShadowedSymbol(const std::string &Name) const;
  std::vector<std::string> GetAllVisibleNames() const;
  
  // Control flow tracking
  void MarkEarlyExit();
  bool CurrentScopeHasEarlyExit() const;
};

// Global symbol table instance
extern std::unique_ptr<SymbolTable> GlobalSymbolTable;

// Initialize the global symbol table
void InitializeSymbolTable();

#endif
