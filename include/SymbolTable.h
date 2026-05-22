#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "Lexer.h"
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
  TurfTypePtr Type;               // Variable type (dynamic — works for list<T>, int[5], etc.)
  SourceLocation DeclLoc;         // Where it was declared
  llvm::AllocaInst *Alloca;       // LLVM memory location
  size_t ScopeLevel;              // Nesting depth (0 = global, 1 = first block, etc.)
  // Array size is encoded inside Type (ArrayType::getSize()) — no separate field needed.
  
  Symbol(SymbolID ID, std::string Name, TurfTypePtr Type, SourceLocation DeclLoc,
         llvm::AllocaInst *Alloca, size_t ScopeLevel)
      : ID(ID), Name(std::move(Name)), Type(std::move(Type)), DeclLoc(DeclLoc),
        Alloca(Alloca), ScopeLevel(ScopeLevel) {}
};

// Represents a lexical scope (block, function body, etc.)
struct Scope {
  size_t Level;                              // Nesting depth
  std::map<std::string, SymbolID> Symbols;   // Name -> SymbolID mapping for this scope
  std::map<std::string, TurfTypePtr> Types;  // Name -> Type mapping for this scope
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

  SymbolTable(const SymbolTable &Other);
  SymbolTable &operator=(const SymbolTable &Other);
  
  // Scope management
  void EnterScope();
  void ExitScope();
  size_t GetCurrentLevel() const { return ScopeStack.size() - 1; }
  
  // Symbol declaration
  SymbolID DeclareSymbol(const std::string &Name, TurfTypePtr Type,
                         SourceLocation DeclLoc, llvm::AllocaInst *Alloca);
  
  // Symbol lookup
  Symbol *LookupSymbol(const std::string &Name);
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
  
  // Control flow tracking
  void MarkEarlyExit();
  bool CurrentScopeHasEarlyExit() const;
  // Recovery helpers: called by the pre-pass setjmp handler to undo orphaned
  // scope state left behind when a function's codegen aborts via longjmp.
  void ResetEarlyExitForCurrentScope();
  void ResetToDepth(size_t TargetDepth);
  size_t GetDepth() const { return ScopeStack.size(); }
};

// Global symbol table instance
extern std::unique_ptr<SymbolTable> GlobalSymbolTable;

// Initialize the global symbol table
void InitializeSymbolTable();

#endif
