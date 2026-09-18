#ifndef CODEGEN_H
#define CODEGEN_H

#include "Types.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>

// Common LLVM tools to be used everywhere.
extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::unique_ptr<llvm::Module> TheModule;

// TOOL-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): `-g` DWARF debug
// info. `g_EmitDebugInfo` is set once from the CLI, before InitializeModule()
// runs (main.cpp) -- everything else here is null/no-op when it's false, so
// a non-`-g` build never touches any of this. `DBuilder`/`DebugCU` are
// (re)built by InitializeModule() alongside TheContext/TheModule; every
// `-g`-gated call site below is written to tolerate `DBuilder == nullptr`
// (i.e. `-g` not passed) without a null check at the call site, by routing
// through SetDebugLoc()/getOrCreateDebugFile()/getDebugType() -- see
// CodegenInternal.h's own doc comments for why those are the ONLY three
// entry points the rest of Codegen needs.
extern bool g_EmitDebugInfo;
extern std::unique_ptr<llvm::DIBuilder> DBuilder;
extern llvm::DICompileUnit *DebugCU;
// The innermost enclosing function's DISubprogram -- null at top level and
// whenever `-g` is off. FuncDefExprAST::codegen() saves/restores this the
// same way it already saves/restores CurrentFunction/CurrentFuncReturnType,
// so nested codegen (control-flow/var-decl helpers in the other 6 Codegen
// split files) can always find "the current debug scope" without threading
// it through every call.
extern llvm::DISubprogram *CurrentDebugScope;

struct VarInfo {
  llvm::AllocaInst *Alloca;
  TurfTypePtr Type; // Dynamic type — works for int, list<Player>, int[5], etc.
  // Array size is encoded inside Type (ArrayType::getSize()) — no separate
  // field needed.
};

// Symbol Table: Maps variables to their memory locations (AllocaInst)
extern std::map<std::string, VarInfo> NamedValues;

// Helper function to initialize the tools
void InitializeModule();
// Helper function to create an alloca instruction
llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                         const std::string &VarName,
                                         const TurfType *Type);
// Map a TurfType to the corresponding LLVM Type (Monomorphizing)
llvm::Type *getLLVMType(const TurfType *Type);

// Current function context (nullptr at top level)
extern TurfTypePtr CurrentFuncReturnType;
extern llvm::Function *CurrentFunction;

#endif
