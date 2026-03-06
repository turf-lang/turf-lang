#ifndef CODEGEN_H
#define CODEGEN_H

#include "Types.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>
#include <vector>

// Common LLVM tools to be used everywhere.
extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::unique_ptr<llvm::Module> TheModule;

struct VarInfo {
  llvm::AllocaInst *Alloca;
  TurfType Type;
  std::vector<int> ArrayDims; // empty = scalar, {5} = 1D, {3,4} = 2D, etc.
};

// Symbol Table: Maps variables to their memory locations (AllocaInst)
extern std::map<std::string, VarInfo> NamedValues;

// Helper function to initialize the tools
void InitializeModule();
// Helper function to create an alloca instruction
llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                         const std::string &VarName,
                                         TurfType Type);
// Map a TurfType to the corresponding LLVM Type
llvm::Type *getLLVMType(TurfType Type);

// Current function context (nullptr at top level)
extern TurfType CurrentFuncReturnType;
extern llvm::Function *CurrentFunction;

#endif
