#ifndef CODEGEN_H
#define CODEGEN_H

#include "sema/Types.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <memory>

class ExprAST;

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
// The LLVM shape of an unsized `T[]` parameter: `{ ptr data, i64 length }`
// (issue #260). Field 0 is the first element, field 1 the element count.
llvm::StructType *getSliceLLVMType();
// Builds the slice a `T[]` parameter receives from the argument expression
// @p Arg (a `T[N]` array or another `T[]`). See its definition
// (CodegenArray.cpp) for why the argument's own storage is shared, not copied.
llvm::Value *CodegenSliceArgument(ExprAST *Arg);
// The same, for a view a VARIABLE keeps: a temporary source gets its own GC
// block instead of a per-location stack slot (see its definition).
llvm::Value *CodegenStoredView(ExprAST *Source);
// True when @p T is the unsized `T[]` spelling.
bool isUnsizedArrayType(const TurfType *T);

// Current function context (nullptr at top level)
extern TurfTypePtr CurrentFuncReturnType;
extern llvm::Function *CurrentFunction;

// The compiler/runtime ABI stamp's symbol (FDN-006 stage 3). One constant so
// the call WrapMainWithRuntimeBootstrap() emits and the "runtime" line of
// `turfc --version` (REL-001) cannot disagree; runtime/turfrt_abi.c defines
// the same name and scripts/smoke/cli_smoke.sh checks that libturfrt.a exports what
// this says, since C code cannot read a C++ constant. Renaming it is an ABI
// break: rename all three together.
inline constexpr const char *RuntimeAbiSymbol = "turf_rt_abi_2_0";

// RT-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8, F-MEM-4): wraps the
// already-fully-codegenned LLVM function literally named "main" (built by
// FuncDefExprAST::codegen()'s existing `Name == "main"` special-casing,
// untouched by this) with a process bootstrap: renames it out of the way,
// keeps it internal, and emits a NEW function named "main" (the real,
// external C-ABI entry point) that calls turf_rt_abi_2_0() (FDN-006 stage 3
// -- the ABI stamp, runtime/turfrt_abi.c), then turf_rt_init(), then the
// renamed user body (forwarding argc/argv unchanged), then
// turf_rt_shutdown(), then returns the user body's own result.
//
// Defined in CodegenFunc.cpp (the file that already owns every other
// `Name == "main"` special case) but declared here, in the PUBLIC header,
// specifically so main.cpp's driver can call it -- src/codegen/CodegenInternal.h
// (where a helper like this would ordinarily go) is private to the 7
// Codegen*.cpp split files and not visible from main.cpp.
//
// Must be called AFTER every function (pre-pass and main-pass alike) has
// finished codegen -- main.cpp calls it immediately after confirming
// `TheModule->getFunction("main")` exists and has a body, and BEFORE
// runOptimizationPipeline() -- so the module still has exactly one
// external symbol named "main" (OPT-001's Internalize must-preserve set is
// exactly {"main"}, ARCHITECTURE_OVERHAUL.md §8 OPT-001) by the time
// Internalize/GlobalDCE run: the renamed user body keeps its own liveness
// through the wrapper's ordinary in-module call to it, needing no separate
// preserve-set entry.
void WrapMainWithRuntimeBootstrap();

// RT-001: emits a TurfTypeDesc constant global (runtime/turfrt.h) for every
// real, fully-built aggregate type in the module. Defined in
// CodegenStruct.cpp; declared here (not CodegenInternal.h) for the same
// main.cpp-visibility reason as WrapMainWithRuntimeBootstrap() above. Call
// exactly once, from main.cpp, immediately before verifyModule() -- see
// this function's own doc comment at its definition for why emitting a
// struct's descriptor any earlier (inline in StructDeclExprAST::codegen())
// is unsafe in general, not just inconvenient.
void EmitAllTurfTypeDescs();

#endif
