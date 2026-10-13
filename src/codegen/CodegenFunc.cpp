/**
 * @file CodegenFunc.cpp
 * @brief Function codegen: definitions, extern declarations, calls, recover, error declarations.
 * @details Part of LLVM IR generation for TurfLang, split from the original
 *          monolithic Codegen.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
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

#include "codegen/Codegen.h"
#include "frontend/AST.h"
#include "analysis/Algorithms.h"
#include "sema/Builtins.h"
#include "analysis/CFG.h"
#include "diagnostics/Errors.h"
#include "analysis/Analysis.h"
#include "analysis/AliasAnalysis.h"
#include "frontend/FfiAnnotation.h"
#include "frontend/Lexer.h"
#include "analysis/ValueRange.h"
#include "analysis/Lint.h"
#include "resolve/SymbolTable.h"
#include "sema/Types.h"
#include "codegen/CodegenInternal.h"
#include "sema/Monomorphize.h" // Monomorphize::instantiatedDeclName() -- ConstructedType lookup, TYPE-002 S13b
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;

// Tracks where each `extern fn` name was FIRST declared, so a later
// conflicting re-declaration (see ExternFuncDeclExprAST::codegen()) can
// point at it via a RelatedLocation instead of just naming the conflict
// with no context. LLVM's own `Function` object carries no Turf-level
// SourceLocation, hence this small side table.
static std::map<std::string, SourceLocation> ExternDeclLocations;

// Set of library names collected from 'extern from "libname" fn' declarations.
// Populated during codegen; consumed by main.cpp::linkObject() (which has its
// own separate `extern std::set<std::string> g_RequiredLibs;` declaration).
std::set<std::string> g_RequiredLibs;

// The platform key `@ffi` is read for (src/frontend/FfiAnnotation.h): the host's,
// unless main.cpp sets the `--target` platform before code generation (it has
// its own separate `extern std::string g_FfiPlatform;` declaration).
std::string g_FfiPlatform = FfiAnnotation::hostPlatformKey();

// ─── argv/argc plumbing for main ───────────────────────────────────────────
// Turf's `main` is compiled directly AS the real C-ABI `main` (no wrapper --
// see the `Name == "main"` branches below and main.cpp:980's lookup of the
// LLVM function literally named "main"), but Turf source never declares
// argc/argv as params on `fn main(): int` -- there is no language syntax
// for it. To let lib/std/os/os.tr's `extern fn TurfRuntimeGetArgc()`/
// `TurfRuntimeGetArgv(idx)` bindings reach the real values, main's LLVM
// FunctionType is forced to `(i32, ptr) -> <ReturnType>` below regardless of
// its (always-empty) Turf Params list, the two incoming args are stashed
// into these two compiler-private globals at the top of main's body, and
// two small C-ABI accessor functions are synthesized directly in IR here
// (mirrors ExternFuncDeclExprAST's platform-forwarding-wrapper pattern
// further down this file) so os.tr can reach them via ordinary `extern fn`
// declarations, exactly like any other libc binding.
//
// An explicit in-Turf call to `main()` would mismatch this synthesized
// 2-arg LLVM signature against a 0-arg call site (CalleeName == "main"
// passthrough further below never applies argc/argv). Resolver::visitFuncCall
// rejects `main()` calls outright (ResolverCall.cpp), and
// Resolver::visitFuncDef rejects a declared `fn main(...)` with params
// (ResolverDecl.cpp), so this mismatch can never actually reach Codegen --
// the passthrough below is defense-in-depth, not a supported call path.
static GlobalVariable *ArgcStorageGV = nullptr;
static GlobalVariable *ArgvStorageGV = nullptr;

// See CodegenInternal.h's doc comment -- called from InitializeModule() so
// these don't outlive the TheModule they were allocated in.
void ResetArgvHelpers() {
  ArgcStorageGV = nullptr;
  ArgvStorageGV = nullptr;
}

// Looks up an already-declared `extern fn Name` (os.tr's TurfRuntimeGetArgc/
// TurfRuntimeGetArgv, created bodyless during the pre-pass) and reuses it,
// or creates a fresh prototype if none exists yet -- same lookup-or-create
// shape as getOrCreateTurfRtAllocFn()/getOrCreateStrcmpFn() (CodegenCore.cpp),
// specialized here because these two also need the conflicting-signature
// guard below (mirrors ExternFuncDeclExprAST::codegen()'s own check, just
// against Codegen's own synthesized definition instead of another extern).
static Function *getOrCreateArgvHelperPrototype(const std::string &Name,
                                                FunctionType *FT) {
  Function *F = TheModule->getFunction(Name);
  if (!F)
    return Function::Create(FT, Function::ExternalLinkage, Name,
                            TheModule.get());
  if (F->getFunctionType() != FT) {
    // A conflicting `extern fn` declaration of this name exists (wrong
    // arity/types) -- reusing it as-is would attach a body to a Function
    // whose declared type disagrees, an invalid-module verifier failure
    // with no clue as to why.
    auto It = ExternDeclLocations.find(Name);
    auto Err = ConflictingExternDeclarationError(
        It != ExternDeclLocations.end() ? It->second : SourceLocation{0, 0},
        Name);
    Err.raise();
  }
  return F;
}

static void EnsureArgvHelpers() {
  if (ArgcStorageGV)
    return; // already synthesized for this module

  // Turf's `int` is LLVM i64 (getLLVMType() in CodegenCore.cpp), NOT i32 --
  // so every Turf-facing surface here (storage, TurfRuntimeGetArgc's return,
  // TurfRuntimeGetArgv's `index` param) must be i64 to match the i64
  // FunctionType os.tr's `extern fn ...: int;` declarations actually build.
  // Only the real C-ABI `main(int argc, ...)` parameter itself stays i32
  // (see the call site below, and FuncDefExprAST::codegen()'s FunctionType
  // for "main") -- that one genuinely is a 32-bit `int` at the platform ABI
  // level, sign-extended into the i64 storage slot on entry.
  Type *I32Ty = Type::getInt32Ty(*TheContext);
  Type *I64Ty = Type::getInt64Ty(*TheContext);
  Type *PtrTy = PointerType::get(*TheContext, 0);

  ArgcStorageGV = new GlobalVariable(
      *TheModule, I64Ty, /*isConstant=*/false, GlobalValue::InternalLinkage,
      ConstantInt::get(I64Ty, 0), "turf_runtime_argc_storage");
  ArgvStorageGV = new GlobalVariable(
      *TheModule, PtrTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
      ConstantPointerNull::get(cast<PointerType>(PtrTy)),
      "turf_runtime_argv_storage");

  // int TurfRuntimeGetArgc() -- os.tr's `extern fn TurfRuntimeGetArgc()`
  // declaration (if present in the program being compiled) already created
  // this Function as a body-less prototype during the pre-pass; reuse it
  // rather than calling Function::Create() again, which would silently
  // rename the new one to "TurfRuntimeGetArgc.1" on a name collision and
  // leave the real declared symbol permanently undefined at link time.
  FunctionType *ArgcFT = FunctionType::get(I64Ty, {}, /*isVarArg=*/false);
  Function *ArgcF =
      getOrCreateArgvHelperPrototype("TurfRuntimeGetArgc", ArgcFT);
  BasicBlock *ArgcBB = BasicBlock::Create(*TheContext, "entry", ArgcF);
  IRBuilder<> ArgcBuilder(ArgcBB);
  ArgcBuilder.CreateRet(ArgcBuilder.CreateLoad(I64Ty, ArgcStorageGV));

  // ptr TurfRuntimeGetArgv(int index) -- bounds-checked; returns "" (not a
  // crash) for an out-of-range index, since a Turf `string` and a raw `ptr`
  // are the same opaque LLVM pointer type (see getLLVMType in
  // CodegenCore.cpp), so the empty-string constant is a valid return value
  // with no conversion needed. Same reuse-if-declared reasoning as above.
  FunctionType *ArgvFT = FunctionType::get(PtrTy, {I64Ty}, /*isVarArg=*/false);
  Function *ArgvF =
      getOrCreateArgvHelperPrototype("TurfRuntimeGetArgv", ArgvFT);
  Argument *IdxArg = ArgvF->getArg(0);
  IdxArg->setName("index");

  BasicBlock *EntryBB = BasicBlock::Create(*TheContext, "entry", ArgvF);
  BasicBlock *InRangeBB = BasicBlock::Create(*TheContext, "in_range", ArgvF);
  BasicBlock *OutRangeBB =
      BasicBlock::Create(*TheContext, "out_of_range", ArgvF);

  IRBuilder<> ArgvBuilder(EntryBB);
  Value *ArgcVal = ArgvBuilder.CreateLoad(I64Ty, ArgcStorageGV);
  Value *TooLow = ArgvBuilder.CreateICmpSLT(IdxArg, ConstantInt::get(I64Ty, 0));
  Value *TooHigh = ArgvBuilder.CreateICmpSGE(IdxArg, ArgcVal);
  Value *OutOfRange = ArgvBuilder.CreateOr(TooLow, TooHigh);
  ArgvBuilder.CreateCondBr(OutOfRange, OutRangeBB, InRangeBB);

  ArgvBuilder.SetInsertPoint(InRangeBB);
  Value *ArgvBase = ArgvBuilder.CreateLoad(PtrTy, ArgvStorageGV);
  // GEP index must match the real argv array's element width regardless of
  // Turf `int` being 64-bit -- truncate back down to i32 (an argv array
  // longer than INT32_MAX entries doesn't happen in practice, matching the
  // real C-ABI argc's own i32 width used to bound this array in the first
  // place).
  Value *Idx32 = ArgvBuilder.CreateTrunc(IdxArg, I32Ty, "index_i32");
  Value *ElemPtr =
      ArgvBuilder.CreateGEP(PtrTy, ArgvBase, Idx32, "argv_elem");
  Value *Elem = ArgvBuilder.CreateLoad(PtrTy, ElemPtr);
  ArgvBuilder.CreateRet(Elem);

  ArgvBuilder.SetInsertPoint(OutRangeBB);
  ArgvBuilder.CreateRet(ArgvBuilder.CreateGlobalStringPtr(""));
}

// RT-001 (docs/archive/ARCHITECTURE_OVERHAUL.md §8, F-MEM-4): see
// src/codegen/Codegen.h's doc comment on WrapMainWithRuntimeBootstrap() for the
// full contract and call-ordering requirement (must run after every
// function is codegenned, before runOptimizationPipeline()). Implemented
// here, not main.cpp, because it operates purely on the already-built LLVM
// Function object for "main" -- it does not need, and deliberately does not
// touch, any of the argc/argv-stashing IR EnsureArgvHelpers() above already
// built inside that function's body (this is exactly why a pure
// post-processing rename+wrapper approach was chosen over threading
// turf_rt_init/turf_rt_shutdown calls into FuncDefExprAST::codegen()'s own
// `Name == "main"` branches: that delicate, already-tested argc/argv
// machinery needs no changes at all this way). FDN-006 stage 3 added the
// `turf_rt_abi_2_0()` call alongside `turf_rt_init()` -- see that symbol's
// own doc comment (runtime/turfrt_abi.c) for why.
void WrapMainWithRuntimeBootstrap() {
  Function *UserMain = TheModule->getFunction("main");
  if (!UserMain || UserMain->empty())
    return; // main.cpp's own caller already checked this; defensive only.

  FunctionType *FT = UserMain->getFunctionType();

  // Rename the user's compiled main out of the way and demote it to
  // internal linkage -- it keeps its liveness through the wrapper's own
  // in-module call below, needing no entry in OPT-001's Internalize
  // must-preserve set (which stays exactly {"main"}, main.cpp's own
  // comment on MustPreserveGV).
  UserMain->setName("__turf_user_main");
  UserMain->setLinkage(GlobalValue::InternalLinkage);

  Function *InitFn = TheModule->getFunction("turf_rt_init");
  if (!InitFn) {
    FunctionType *InitFT =
        FunctionType::get(Type::getVoidTy(*TheContext), false);
    InitFn = Function::Create(InitFT, Function::ExternalLinkage,
                              "turf_rt_init", TheModule.get());
  }
  Function *ShutdownFn = TheModule->getFunction("turf_rt_shutdown");
  if (!ShutdownFn) {
    FunctionType *ShutdownFT =
        FunctionType::get(Type::getVoidTy(*TheContext), false);
    ShutdownFn = Function::Create(ShutdownFT, Function::ExternalLinkage,
                                  "turf_rt_shutdown", TheModule.get());
  }

  // FDN-006 stage 3: the compiler/runtime ABI stamp
  // (docs/planning/ROADMAP_v2.0.0.md, runtime/turfrt_abi.c). Every compiled
  // binary's wrapper references this symbol so that linking against a
  // `libturfrt.a` that predates it (or a hypothetical future ABI-breaking
  // runtime that renamed it) fails at LINK TIME ("undefined reference to
  // `turf_rt_abi_2_0`"), never silently at run time against a runtime this
  // compiler no longer agrees with.
  Function *AbiStampFn = TheModule->getFunction(RuntimeAbiSymbol);
  if (!AbiStampFn) {
    FunctionType *AbiStampFT =
        FunctionType::get(Type::getVoidTy(*TheContext), false);
    AbiStampFn = Function::Create(AbiStampFT, Function::ExternalLinkage,
                                  RuntimeAbiSymbol, TheModule.get());
  }

  // `fn main(): void` is accepted (only parameters are checked, Resolver),
  // and the process still needs a status, so its wrapper is `i32 main()`
  // returning 0. Before, the wrapper copied the void type and named the void
  // call's result, which the verifier rejected as an internal error.
  const bool VoidMain = FT->getReturnType()->isVoidTy();
  FunctionType *WrapperFT =
      VoidMain ? FunctionType::get(Type::getInt32Ty(*TheContext), FT->params(),
                                   FT->isVarArg())
               : FT;
  Function *Wrapper = Function::Create(WrapperFT, Function::ExternalLinkage,
                                       "main", TheModule.get());
  BasicBlock *EntryBB = BasicBlock::Create(*TheContext, "entry", Wrapper);
  IRBuilder<> WB(EntryBB);

  WB.CreateCall(AbiStampFn, {});
  WB.CreateCall(InitFn, {});

  std::vector<Value *> ForwardedArgs;
  for (Argument &A : Wrapper->args())
    ForwardedArgs.push_back(&A);
  Value *Result =
      WB.CreateCall(UserMain, ForwardedArgs, VoidMain ? "" : "user_main_result");

  WB.CreateCall(ShutdownFn, {});
  if (VoidMain)
    WB.CreateRet(ConstantInt::get(Type::getInt32Ty(*TheContext), 0));
  else
    WB.CreateRet(Result);
}

// FuncDefExprAST::codegen
// Two-pass strategy:
//   Pass 1 (prototype): if no LLVM Function yet, create it (no body).
//   Pass 2 (body):      if function has no body yet, fill it in.
// Calling codegen() twice is what makes forward calls work: main.cpp's
// pre-pass creates prototypes, then the normal loop fills bodies.
Value *FuncDefExprAST::codegen() {
  // TYPE-002 (S14a): a FuncDefExprAST with non-empty TypeParams is an
  // uninstantiated generic TEMPLATE -- the monomorphizer (Monomorphize.cpp)
  // always appends a fully-concrete CLONE elsewhere in PrePassASTs and
  // rewrites every call site to it, so the template itself is dead code by
  // the time Codegen runs and must never be codegenned (F-GEN-2, INV-GEN-1).
  // main.cpp's sub-pass 3a/3b loops already skip a template found directly
  // in PrePassASTs -- but a template declared INSIDE an imported module is
  // ALSO reached a SECOND way, via ModuleImportExprAST::codegen()'s own
  // recursive walk of that module's body (CodegenStruct.cpp), which has no
  // equivalent check of its own. Found live: even a completely trivial
  // `fn main(): int { return 0; }` hit this, because the auto-imported
  // prelude itself is walked this way. Checked HERE, at the single real
  // entry point every caller eventually goes through, instead of
  // duplicating the same guard at every call site (main.cpp's two loops
  // plus this one, and any future caller). Same "processed, nothing to
  // return" sentinel convention VarDeclExprAST's IsExported early-return
  // already uses.
  if (!TypeParams.empty())
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));

  std::string MangledName = Name;
  std::string Prefix = "";
  if (GlobalSymbolTable && GlobalSymbolTable->IsInModule()) {
    Prefix = GlobalSymbolTable->GetCurrentModule() + "_";
    MangledName = Prefix + Name;
  }

  if (Name != "main") {
    // sanitizeSymbolName() is a no-op for every ordinary function name
    // (map-miss fallback) -- only synthesized operator names like
    // "operator+" get remapped here, to match ResolverFunctionSymbol::
    // mangledName()'s identical use of it (both must agree, or Codegen's
    // ResolvedOperatorTarget dispatch in BinaryExprAST::codegen() would
    // look up a symbol that was never actually registered under that name).
    MangledName = "turf_" + Prefix + sanitizeSymbolName(Name);
    for (const auto &P : Params) {
      MangledName += "_" + P.Type->getMangledName();
    }
  }

  // Build/look up the LLVM prototype
  Function *TheFunc = TheModule->getFunction(MangledName);

  if (!TheFunc) {
    // Save param types and return type
    std::vector<TurfTypePtr> PTypes;
    for (const auto &P : Params)
      PTypes.push_back(P.Type);
    FuncParamTypes[MangledName] = PTypes;
    FuncReturnTypes[MangledName] = ReturnType;

    // `main` is special: it's always the real C-ABI entry point
    // (argc: i32, argv: ptr) -> ReturnType, ExternalLinkage, with its two
    // args named "argc"/"argv" -- regardless of Turf's Params list (always
    // empty -- see EnsureArgvHelpers() above). Built as one combined
    // ParamTypes/Linkage/ArgNames branch rather than three separate
    // `Name == "main"` checks (FunctionType, Linkage, argument naming used
    // to be decided independently a few lines apart) so a future change to
    // main's ABI (e.g. adding envp) has one place to touch, not three.
    std::vector<Type *> ParamTypes;
    std::vector<std::string> ArgNames;
    Function::LinkageTypes Linkage;
    if (Name == "main") {
      ParamTypes = {Type::getInt32Ty(*TheContext),
                    PointerType::get(*TheContext, 0)};
      ArgNames = {"argc", "argv"};
      Linkage = Function::ExternalLinkage;
    } else {
      for (const auto &P : Params) {
        ParamTypes.push_back(getLLVMType(P.Type.get()));
        ArgNames.push_back(P.Name);
      }
      // STD-002: `@caller_location` appends ONE hidden extra parameter at
      // the very end of the REAL LLVM signature -- deliberately never added
      // to `Params` itself (see FuncDefExprAST's own doc comment, ASTDecl.h,
      // for why keeping it out of the Turf-level, arity-checked signature
      // is the whole point). The alloca-binding loop below iterates
      // `TheFunc->args()` (not `Params`) precisely so this extra argument
      // gets allocated/stored/registered into NamedValues via the exact
      // same generic code every ordinary parameter already goes through --
      // it only needs one special case there, for its TYPE (TurfString(),
      // since it has no matching `Params` entry to read a type from).
      if (hasCallerLocation()) {
        ParamTypes.push_back(getLLVMType(TurfString().get()));
        ArgNames.push_back("caller_location");
      }
      Linkage = Function::InternalLinkage;
    }

    FunctionType *FT =
        FunctionType::get(getLLVMType(ReturnType.get()), ParamTypes,
                          /*isVarArg=*/false);
    TheFunc = Function::Create(FT, Linkage, MangledName, TheModule.get());

    unsigned ArgIdx = 0;
    for (auto &Arg : TheFunc->args())
      Arg.setName(ArgNames[ArgIdx++]);

    // TOOL-001: the DISubprogram is built once, right alongside the LLVM
    // prototype -- this `if (!TheFunc)` block runs exactly once per
    // function (the pre-pass's own proto-then-body two-call structure,
    // §0's "double-parse wrinkle" doc comment above, finds the SAME
    // llvm::Function* on its second call and skips this whole branch), so
    // there's no risk of building a second, duplicate DISubprogram for the
    // same function the way there would be if this lived down in the
    // body-codegen section below. `TheFunc->getSubprogram()` (below, where
    // body codegen starts) reads it back out regardless of which of the two
    // calls actually created it.
    if (DBuilder) {
      DIFile *File = getOrCreateDebugFile(Loc.Filename);
      std::vector<Metadata *> ParamDebugTypes;
      ParamDebugTypes.push_back(getDebugType(ReturnType.get()));
      for (const auto &P : Params)
        ParamDebugTypes.push_back(getDebugType(P.Type.get()));
      // STD-002: keep this list's length matching the REAL LLVM signature
      // (ParamTypes above), which already got the hidden extra parameter --
      // a DISubroutineType describing fewer parameters than the function
      // actually has would be internally inconsistent DWARF (INV-DEBUG-2).
      if (hasCallerLocation())
        ParamDebugTypes.push_back(getDebugType(TurfString().get()));
      DISubroutineType *SubTy = DBuilder->createSubroutineType(
          DBuilder->getOrCreateTypeArray(ParamDebugTypes));

      DISubprogram::DISPFlags SPFlags = DISubprogram::SPFlagDefinition;
      if (Name == "main")
        SPFlags |= DISubprogram::SPFlagMainSubprogram;

      // No distinct DW_AT_linkage_name: every low_pc/high_pc range LLVM's
      // own DWARF emission attaches to this DISubprogram already comes
      // straight from `TheFunc` itself (via TheFunc->setSubprogram(SP)
      // below), so a separate linkage name buys nothing for breakpoint/
      // backtrace correctness -- and empirically, HURTS it: gdb 17.2
      // (confirmed live) prefers `DW_AT_linkage_name` over `DW_AT_name`
      // for frame/breakpoint display whenever both are present on a
      // `DW_LANG_C99` subprogram, which would show the raw mangled
      // "turf_add_int_int" instead of "add" at every breakpoint hit and
      // backtrace line -- exactly backwards from TOOL-001's own "show a
      // backtrace with TurfLang function names" completion criterion.
      DISubprogram *SP = DBuilder->createFunction(
          File, Name, /*LinkageName=*/"", File, Loc.Line, SubTy, Loc.Line,
          DINode::FlagPrototyped, SPFlags);
      TheFunc->setSubprogram(SP);
    }
  } else {
    // If the function already has a body, this is a duplicate definition
    if (!TheFunc->empty()) {
      SemanticError(Loc, "Duplicate function definition: '" + Name + "'")
          .raise();
      return nullptr;
    }
  }

  // If this is a prototype-only pass (pre-pass sub-pass 3a), stop here.
  // The body will be compiled in sub-pass 3b once all prototypes exist.
  if (g_protoOnlyPass) {
    return TheFunc;
  }

  // Store full param types for call-site checking
  FuncParamTypes[MangledName] = {};
  for (const auto &P : Params)
    FuncParamTypes[MangledName].push_back(P.Type);

  // Phase 2: also register this function in the SymbolTable's module registry.
  // This populates the hashmap used by MethodCallExprAST to do namespace-aware
  // overload resolution without string-prefix scanning.
  if (GlobalSymbolTable && Name != "main") {
    FunctionSymbol FSym;
    FSym.BaseName    = Name; // user-visible name, e.g. "printline"
    FSym.MangledName = MangledName; // e.g. "turf_io_printline_int"
    FSym.Arity       = static_cast<int>(Params.size());
    FSym.ReturnType  = ReturnType;
    for (const auto &P : Params)
      FSym.ParamTypes.push_back(P.Type);

    // Determine which module this function belongs to.
    // IsInModule() is true when we're currently inside a ModuleImportExprAST
    // codegen block (i.e. compiling a library like io.tr or math.tr).
    std::string ModuleName =
        (GlobalSymbolTable->IsInModule())
            ? GlobalSymbolTable->GetCurrentModule()
            : "main";
    GlobalSymbolTable->RegisterModuleFunction(ModuleName, FSym);
  }

  // Fill in the function body
  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunc);

  // Save caller context
  auto SavedNamedValues = NamedValues;
  auto SavedReturnType = CurrentFuncReturnType;
  auto SavedFunction = CurrentFunction;
  auto *SavedInsertBlock = Builder->GetInsertBlock();
  auto SavedInsertPoint = Builder->GetInsertPoint();
  DISubprogram *SavedDebugScope = CurrentDebugScope;
  DebugLoc SavedDebugLoc = Builder->getCurrentDebugLocation();

  // Switch to the new function
  Builder->SetInsertPoint(BB);
  NamedValues.clear();
  CurrentFuncReturnType = ReturnType;
  CurrentFunction = TheFunc;
  // TOOL-001: `getSubprogram()` reads back whichever of the two codegen()
  // calls actually built it (see the `if (!TheFunc)` block above) -- null
  // when `-g` is off, which is also what makes SetDebugLoc()/getDebugType()/
  // every dbg-var insertion below into no-ops for the rest of this function.
  CurrentDebugScope = TheFunc->getSubprogram();
  SetDebugLoc(Loc);

  // Enter a new scope for the function body
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // Alloca each parameter and add to NamedValues and GlobalSymbolTable.
  // `main`'s two LLVM-level args (argc/argv) are not real Turf parameters --
  // they're not in Params and have no Turf type to alloca -- so they're
  // stashed into the argv-plumbing globals instead (see EnsureArgvHelpers()
  // above) and never registered as visible symbols.
  std::vector<SymbolID> ParamIDs;
  if (Name == "main") {
    EnsureArgvHelpers();
    auto ArgIt = TheFunc->args().begin();
    Argument &ArgcArg = *ArgIt;
    Argument &ArgvArg = *(++ArgIt);
    Value *Argc64 = Builder->CreateSExt(&ArgcArg, Type::getInt64Ty(*TheContext),
                                        "argc_i64");
    Builder->CreateStore(Argc64, ArgcStorageGV);
    Builder->CreateStore(&ArgvArg, ArgvStorageGV);
  } else {
    unsigned ArgNo = 1; // DIBuilder::createParameterVariable's ArgNo is 1-based.
    for (auto &Arg : TheFunc->args()) {
      TurfTypePtr ParamTurfType = TurfInt(); // fallback
      SourceLocation ParamDeclLoc = Loc; // fallback: the 'fn' keyword itself
      // STD-002: the hidden `caller_location` argument (appended above,
      // never a real `Params` entry) would otherwise silently fall through
      // to the `TurfInt()` fallback above -- checked BEFORE the `Params`
      // scan since no entry there could ever match it by name anyway.
      if (Arg.getName() == "caller_location" && hasCallerLocation()) {
        ParamTurfType = TurfString();
      } else {
        for (const auto &P : Params)
          if (P.Name == Arg.getName()) {
            ParamTurfType = P.Type;
            ParamDeclLoc = P.Loc;
          }
      }

      AllocaInst *Alloca = CreateEntryBlockAlloca(
          TheFunc, std::string(Arg.getName()), ParamTurfType.get());
      Builder->CreateStore(&Arg, Alloca);

      TurfTypePtr RegisteredType = ParamTurfType;

      NamedValues[std::string(Arg.getName())] = {Alloca, RegisteredType};

      // Add parameter to symbol table
      if (GlobalSymbolTable) {
        SourceLocation ParamLoc = Loc;
        SymbolID id = GlobalSymbolTable->DeclareSymbol(
            std::string(Arg.getName()), RegisteredType, ParamLoc, Alloca);
        ParamIDs.push_back(id);
      }

      // TOOL-001: `-g` parameter debug info -- a synthetic 'self' parameter
      // has ParamDeclLoc == {0,0} (ParamDecl::Loc's own doc comment,
      // ASTDecl.h), which SetDebugLoc()-style Line<=0 handling would treat
      // as "keep the current location" if used for the dbg.declare's OWN
      // location -- but createParameterVariable()'s LineNo just wants
      // "where's a reasonable place to show this parameter", so a fallback
      // to the function's own declaration line (ParamDeclLoc's default,
      // already assigned above) is exactly right, not a bug to special-case.
      if (CurrentDebugScope) {
        DIFile *File = getOrCreateDebugFile(ParamDeclLoc.Filename.empty()
                                                ? Loc.Filename
                                                : ParamDeclLoc.Filename);
        DILocalVariable *ParamVar = DBuilder->createParameterVariable(
            CurrentDebugScope, std::string(Arg.getName()), ArgNo, File,
            ParamDeclLoc.Line > 0 ? ParamDeclLoc.Line : Loc.Line,
            getDebugType(ParamTurfType.get()), /*AlwaysPreserve=*/true);
        DBuilder->insertDeclare(Alloca, ParamVar, DBuilder->createExpression(),
                                DILocation::get(*TheContext, ParamVar->getLine(),
                                                0, CurrentDebugScope),
                                Builder->GetInsertBlock());
      }
      ++ArgNo;
    }
  }

  // Lint the function body before codegen
  LintExpression(Body.get());

  // OPT-002 (ARCHITECTURE_OVERHAUL.md §8, §17.3): a THROWAWAY, EARLY CFG,
  // built and analyzed here purely so Codegen (about to run, right below)
  // can consult its results while emitting THIS function's bounds/overflow
  // checks. The "real" CFG build + full analysis suite a few dozen lines
  // below (ValueRangePass::runOnCFG() included) necessarily runs AFTER
  // Body->codegen() -- it needs the LLVM IR nowhere near ready yet -- which
  // means its own constant-propagation results always arrive one compile
  // too late to suppress anything: by the time it runs, every runtime
  // check for this function is already baked into the IR. Building a
  // second CFG from the same (not-yet-codegenned) Body AST costs a cheap
  // AST walk, is thrown away immediately after, and is the only way this
  // group's proofs can actually reach Codegen in time. ValueRangePass::
  // markElidableChecks() never raises a diagnostic (see its own doc
  // comment, ValueRange.cpp) -- the LATER runOnCFG() call remains the only
  // place ArrayBoundsError fires, completely unaffected by this.
  if (Body) {
    auto ElisionCFG = CFGBuilder().buildCFG(Name, ReturnType, Body.get(), Loc, MangledName);
    ValueRangePass().markElidableChecks(*ElisionCFG, ParamIDs);
  }

  // Codegen the body
  Body->codegen();

  // Build and analyze CFG for flow diagnostics
  if (Body) {
    CFGBuilder CFGBuilder;
    auto FuncCFG = CFGBuilder.buildCFG(Name, ReturnType, Body.get(), Loc, MangledName);

    // Store CFG for later use (push before analyses so it survives raises)
    GlobalCFGs.push_back(std::move(FuncCFG));

    // Every flow check (missing return, definite assignment, E017/E018, ...),
    // shared with the query facade (Analysis.h).
    runFunctionFlowPasses(*GlobalCFGs.back(), Name, Loc, ParamIDs);
  }

  // Emit a return if the block has no terminator yet.
  // IMPORTANT: ReturnExprAST::codegen() leaves the builder pointing at a
  // fresh dead "after_ret" block (no predecessors, no terminator). We must
  // erase those dead blocks instead of treating them as missing-return paths.
  BasicBlock *CurBB = Builder->GetInsertBlock();
  if (!CurBB->getTerminator()) {
    bool IsDead = (CurBB != BB) && llvm::pred_empty(CurBB);
    if (IsDead) {
      // Dead block left behind by a return statement — erase it cleanly.
      CurBB->eraseFromParent();
      // The builder still points at the block just freed. A nested function
      // restores its caller's insert point below, but a TOP-LEVEL one (a
      // module's own function) has none to restore, so the freed block stayed
      // "current" -- and the module body's BlockExprAST::codegen() then asked
      // it for its terminator (CodegenControlFlow.cpp, which already says
      // GetInsertBlock() may legitimately be null there). Usually a harmless
      // read of freed memory; segfaulted (exit 139) once the prelude grew to
      // nine modules and the allocator happened to reuse it. Found with ASan:
      // `turfc --check --module` on a file under a temp directory.
      Builder->ClearInsertionPoint();
    } else if (ReturnType->isVoid()) {
      Builder->CreateRetVoid();
    } else {
      // CFG already reported the error, but we still need to generate valid IR
      Builder->CreateRet(Constant::getNullValue(getLLVMType(ReturnType.get())));
    }
  }

  verifyFunction(*TheFunc);

  // TOOL-001: this function's own DISubprogram is fully built now (every
  // local/parameter dbg.declare it will ever get has already been inserted)
  // -- finalizing it here, per-function, rather than waiting for the single
  // whole-module DBuilder->finalize() call (main.cpp, right before
  // verifyModule()) is what DIBuilder's own docs recommend for a subprogram
  // whose body is fully emitted before the next one starts, and is needed
  // for a RECURSIVE function: this function may call itself, but that
  // self-call is reached through a call site's OWN scope resolution, not
  // through this DISubprogram being temporary/unresolved, so there's no
  // cycle finalizeSubprogram() here could break -- it only ever resolves
  // outstanding forward references belonging to THIS subprogram (e.g. a
  // local variable's type that was still a temporary composite type when
  // its dbg.declare was inserted, see getDebugType()'s struct branch).
  if (DBuilder && CurrentDebugScope)
    DBuilder->finalizeSubprogram(CurrentDebugScope);

  // Exit function scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // Restore caller context
  NamedValues = SavedNamedValues;
  CurrentFuncReturnType = SavedReturnType;
  CurrentFunction = SavedFunction;
  CurrentDebugScope = SavedDebugScope;
  Builder->SetCurrentDebugLocation(SavedDebugLoc);
  if (SavedInsertBlock)
    Builder->SetInsertPoint(SavedInsertBlock, SavedInsertPoint);

  // Function definitions are statements; they do not produce a value.
  // Return a non-null sentinel, NOT nullptr -- same convention
  // StructDeclExprAST/EnumDeclExprAST/ErrorDeclExprAST/VarDeclExprAST's
  // exported-constant path already use for "processed successfully,
  // nothing to return." nullptr specifically means "codegen failed" to
  // every blind-iteration caller (BlockExprAST::codegen(),
  // ModuleImportExprAST::codegen()'s Body->codegen()) -- this is by far
  // the most common statement kind inside a module body, so returning a
  // literal nullptr here silently truncated EVERY imported module after
  // its first function once BlockExprAST::codegen() was hardened to stop
  // at the first null statement (see git history for the retired 2026-07
  // code-review-gaps audit's original writeup).
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}


// Some libc symbols an `extern fn` declaration binds to aren't named
// consistently across platforms -- e.g. lib/std/mem/mem.tr's
// malloc_usable_size() is a glibc/Linux extension; Darwin's equivalent is
// malloc_size() (identical void* -> size_t signature), and MSVC's CRT
// equivalent is _msize(). Turf itself has no conditional-compilation
// feature a stdlib .tr file could use to route around this per platform
// (see mem.tr's own doc comment on malloc_usable_size()), so a bare
// `extern fn malloc_usable_size` compiles fine everywhere but only LINKS
// successfully on glibc/Linux -- confirmed via a real link failure
// ("Undefined symbols ... _malloc_usable_size") on macOS/arm64.
//
// The declaration itself says so: `@ffi(macos: malloc_size)` on the extern
// (src/frontend/FfiAnnotation.h). ExternFuncDeclExprAST::codegen() asks the
// declaration for a forwarding target on the platform being compiled for
// (g_FfiPlatform) and, if there is one, emits a one-line DEFINED wrapper under the
// Turf-visible name that forwards to the real platform symbol -- instead of a
// bare external declaration. There used to be a table of libc names in this
// file that did the same; no compiler table keyed on a function's name
// remains (CONSTITUTION.md, Article V).
//
// Every other extern-fn code path (call resolution, the symbol table,
// FuncParamTypes/FuncReturnTypes) keeps treating this exactly like an
// ordinary external declaration; only this one function's LLVM IR shape
// (defined body vs. no body) differs.
//
// Returns the real symbol to forward to, or empty if the declaration names
// none for this platform.
static std::string PlatformExternForward(const ExternFuncDeclExprAST &Decl) {
  return FfiAnnotation::forwardTarget(Decl, g_FfiPlatform);
}


// ExternFuncDeclExprAST::codegen
// Emits an LLVM ExternalLinkage prototype (no body block) -- unless
// PlatformExternForward() says this name needs a same-signature forwarding
// wrapper on the host platform, in which case a defined one-line wrapper is
// emitted instead (see that function's doc comment).
// Mirrors what `declare i64 @strlen(ptr)` looks like in LLVM IR.
Value *ExternFuncDeclExprAST::codegen() {
  // Build the LLVM parameter type list.
  std::vector<Type *> ParamLLVMTypes;
  std::vector<TurfTypePtr> ParamTurfTypes;
  for (const auto &[PName, PType] : Params_) {
    ParamLLVMTypes.push_back(getLLVMType(PType.get()));
    ParamTurfTypes.push_back(PType);
  }

  // Build the LLVM function type. Built BEFORE the existing-declaration
  // check below (moved up from where it used to live, after that check)
  // so a re-declaration's signature can be compared against any earlier
  // one -- LLVM interns structurally-identical FunctionTypes to the same
  // pointer within a context, so this comparison is a cheap, exact one.
  Type *RetLLVMType = getLLVMType(ReturnType_.get());
  FunctionType *FT =
      FunctionType::get(RetLLVMType, ParamLLVMTypes, /*isVarArg=*/IsVarArg_);

  // If a declaration with this name already exists in the module, this is a
  // re-declaration (e.g. the same libc function externed from two
  // different stdlib modules, like strlen in both strings.tr and net.tr) --
  // legitimate ONLY if the signature actually matches. Extern mangled names
  // are just the bare C name (not parameter-type-qualified, unlike ordinary
  // Turf functions), so unlike a genuine duplicate function BODY (caught by
  // FuncDefExprAST::codegen()'s "Duplicate function definition" check via
  // LLVM's own function table), nothing else in the compiler previously
  // verified two externs of the same name actually agreed on what they
  // were declaring -- confirmed real: the old code reused whichever
  // declaration came first UNCONDITIONALLY, with no signature check at
  // all, silently binding any call resolved against a conflicting second
  // declaration to the first one's real (different) LLVM type.
  // This platform doesn't export `Name_` under that literal name -- declare
  // the real symbol (same FT, since it's a documented same-signature
  // equivalent) and give `F` a one-line body that forwards every argument to
  // it. See PlatformExternForward()'s doc comment for why this happens here
  // instead of at the call site.
  std::string ForwardTo = PlatformExternForward(*this);
  if (ForwardTo == Name_)
    ForwardTo.clear(); // `@ffi(linux: f)` on `f` itself: a wrapper would call itself
  auto defineForwarder = [&](Function *F) {
    Function *RealF = TheModule->getFunction(ForwardTo);
    if (!RealF)
      RealF = Function::Create(FT, Function::ExternalLinkage, ForwardTo,
                               TheModule.get());
    BasicBlock *WrapperBB = BasicBlock::Create(*TheContext, "entry", F);
    IRBuilder<> WrapperBuilder(WrapperBB);
    std::vector<Value *> ForwardArgs;
    for (auto &Arg : F->args())
      ForwardArgs.push_back(&Arg);
    Value *ForwardResult = WrapperBuilder.CreateCall(RealF, ForwardArgs);
    if (RetLLVMType->isVoidTy())
      WrapperBuilder.CreateRetVoid();
    else
      WrapperBuilder.CreateRet(ForwardResult);
  };

  if (Function *Existing = TheModule->getFunction(Name_)) {
    if (Existing->getFunctionType() != FT) {
      auto It = ExternDeclLocations.find(Name_);
      auto Err = ConflictingExternDeclarationError(Loc_, Name_);
      if (It != ExternDeclLocations.end())
        Err.withRelated(It->second, "first declared here");
      Err.raise();
    }
    // The `@ffi` forwarding is a fact about the C symbol, so it holds whichever
    // declaration of it carries the annotation: an earlier, unannotated one
    // (still a bare declaration) gets the forwarding body now.
    if (!ForwardTo.empty() && Existing->isDeclaration())
      defineForwarder(Existing);
    return Existing;
  }
  ExternDeclLocations[Name_] = Loc_;

  // Create with ExternalLinkage — no body block unless it forwards.
  Function *F =
      Function::Create(FT, Function::ExternalLinkage, Name_, TheModule.get());
  if (!ForwardTo.empty())
    defineForwarder(F);

  // Name the parameters for IR readability.
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Params_[Idx++].first);

  // Register in the type caches so call sites can look up param/return types.
  FuncParamTypes[Name_] = ParamTurfTypes;
  FuncReturnTypes[Name_] = ReturnType_;

  // Register in the GlobalSymbolTable so FuncCallExprAST can find this
  // function by its user-visible name (without mangling).
  if (GlobalSymbolTable) {
    FunctionSymbol FSym;
    FSym.BaseName    = Name_;
    FSym.MangledName = Name_; // extern fns use their C name directly
    FSym.Arity       = static_cast<int>(Params_.size());
    FSym.ReturnType  = ReturnType_;
    FSym.IsVarArg    = IsVarArg_;  // propagate variadic flag to the symbol
    for (const auto &[PName, PType] : Params_)
      FSym.ParamTypes.push_back(PType);
    GlobalSymbolTable->RegisterModuleFunction("main", FSym);
  }

  // If this extern fn specifies a library (extern from "ssl" fn ...),
  // record it so the linker can append -lssl automatically.
  if (!LibName_.empty())
    g_RequiredLibs.insert(LibName_);

  return F;
}


Value *FuncCallExprAST::codegen() {
  auto *Var = dynamic_cast<VariableExprAST *>(Callee.get());
  auto *ScopeCallee = dynamic_cast<ScopeAccessExprAST *>(Callee.get());
  if (!Var && !ScopeCallee) {
    llvm::report_fatal_error("Internal error: First-class functions are not supported yet.");
    return nullptr;
  }
  // `Module::func(...)`: the Resolver already validated this and set
  // ResolvedCallTarget exactly as it would for a bare call (see
  // Resolver::visitFuncCall's ScopeCallee branch) -- CalleeName here only
  // matters for the malloc/realloc/free/typeof/... bare-intrinsic checks
  // below (which a qualified call can never match, since those are always
  // spelled bare) and as ResolvedCallTarget's fallback if it's ever null.
  std::string CalleeName = Var ? Var->getName() : ScopeCallee->getRightName();

  // Indirect call through a function-typed local variable/parameter, e.g.
  // `fn apply(f: fn(int): int, x: int): int { return f(x); }` -- Resolver
  // deliberately leaves ResolvedCallTarget/ResolvedCandidates empty for this
  // shape (see Resolver::visitFuncCall's function-value branch: it attaches
  // an ordinary ResolvedSymbol to the callee instead, exactly like any other
  // variable read), so it's detected here purely from the callee's own
  // resolved symbol type, before the mangled-name lookup path below (which
  // has nothing to look up -- there is no single LLVM function this call
  // resolves to statically).
  if (Var && Var->getResolvedSymbol() && Var->getResolvedSymbol()->Type &&
      Var->getResolvedSymbol()->Type->isFunc()) {
    const auto *FT =
        static_cast<const FuncType *>(Var->getResolvedSymbol()->Type.get());
    Value *CalleeVal = Var->codegen();
    if (!CalleeVal)
      return nullptr;

    std::vector<llvm::Type *> ParamLLVMTypes;
    for (const auto &PT : FT->getParamTypes())
      ParamLLVMTypes.push_back(getLLVMType(PT.get()));
    llvm::Type *RetLLVMType = getLLVMType(FT->getReturnType().get());
    auto *FnLLVMType =
        llvm::FunctionType::get(RetLLVMType, ParamLLVMTypes, false);

    const auto &ParamTypes = FT->getParamTypes();
    std::vector<Value *> ArgVals;
    for (size_t i = 0; i < Args.size(); ++i) {
      Value *V = Args[i]->codegen();
      if (!V)
        return nullptr;
      if (i < ParamTypes.size())
        V = CastToType(V, ParamTypes[i], "argcast", Loc);
      ArgVals.push_back(V);
    }

    if (FT->getReturnType()->isVoid())
      return Builder->CreateCall(FnLLVMType, CalleeVal, ArgVals);
    return Builder->CreateCall(FnLLVMType, CalleeVal, ArgVals, "calltmp");
  }

  // `ErrorType::Variant(fields..., [message])` -- the construction of an error
  // value: STD-002's message (ARCHITECTURE_OVERHAUL.md §8/§15.6(c)) and, since
  // LEC-004 (FREEZE.md §6.18), the variant's payload fields. Every call with
  // a count the variant does not accept never reaches Codegen at all --
  // Resolver::visitFuncCall rejects it first. The arguments are evaluated in
  // source order, fields (each cast to its declared type, as a struct field
  // initialiser would be) and then the trailing message if one was written.
  if (ScopeCallee &&
      ScopeCallee->ResolvedKind == ScopeAccessKind::ErrorVariantConstruct) {
    const std::vector<TurfTypePtr> FieldTypes =
        ErrorVariantFieldTypes(ScopeCallee->getLeftName(), ScopeCallee->getRightName());
    std::vector<Value *> FieldVals;
    Value *MsgVal = nullptr;
    for (size_t I = 0; I < Args.size(); ++I) {
      Value *V = Args[I]->codegen();
      if (!V)
        return nullptr;
      if (I < FieldTypes.size())
        FieldVals.push_back(CastToType(V, FieldTypes[I], "payloadcast", Loc));
      else
        MsgVal = V;
    }
    return CodegenErrorVariantConstruct(ScopeCallee->getLeftName(),
                                        ScopeCallee->getRightName(), FieldVals,
                                        MsgVal);
  }

  // `StructName::default()` / `StructName::new(...)` -- construction.
  // Neither reaches the ordinary ResolvedCallTarget->mangledName() lookup
  // below the same way: default() has no backing function at all (fully
  // compiler-synthesized), and new()'s ResolvedCallTarget (set by
  // Resolver::visitFuncCall) deliberately has `self` STRIPPED from its
  // Params (so TypeChecker's ordinary overload-narrowing works unmodified,
  // see that function's comment) -- its mangledName() would therefore be
  // missing the `_StructName_ptr` component the REAL LLVM function
  // (declared with self as its first parameter) actually has, so it's
  // reconstructed manually here instead of trusting mangledName() wholesale.
  if (ScopeCallee &&
      (ScopeCallee->ResolvedKind == ScopeAccessKind::ConstructorCall ||
       ScopeCallee->ResolvedKind == ScopeAccessKind::DefaultConstruct)) {
    const std::string &StructName = ScopeCallee->getLeftName();
    // TYPE-002 (S13b): if TypeChecker::visitVarDecl() determined this
    // construction call is building a SPECIFIC generic instantiation (e.g.
    // `mut b: Box<Big> = Box::default();`), look up that instantiation's
    // OWN per-instantiation TurfStructType (drainStructRequests(), S13a)
    // instead of always the shared, erasure-era base struct -- see
    // ConstructedType's own doc comment (ASTAccess.h) for why the call
    // itself has no other way to know which instantiation it's building.
    TurfTypePtr T;
    if (ConstructedType) {
      if (auto *CGT = dynamic_cast<const GenericType *>(ConstructedType.get())) {
        std::string MangledName =
            Monomorphize::instantiatedDeclName(CGT->getBaseName(), CGT->getTypeArgs());
        T = GlobalSymbolTable ? GlobalSymbolTable->LookupType(MangledName) : nullptr;
      }
    }
    if (!T)
      T = GlobalSymbolTable ? GlobalSymbolTable->LookupType(StructName) : nullptr;
    auto *SType = dynamic_cast<TurfStructType *>(T.get());
    if (!SType) {
      SemanticError(Loc, "Internal error: unknown struct type '" + StructName +
                             "' in construction -- this is a compiler bug.")
          .raise();
      return nullptr;
    }
    Type *LLVMTy = getLLVMType(T.get());
    auto *LLVMStructType = dyn_cast<StructType>(LLVMTy);
    if (!LLVMStructType) {
      SemanticError(Loc, "Internal error: LLVM StructType '" + StructName +
                             "' not found -- this is a compiler bug.")
          .raise();
      return nullptr;
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());

    if (ScopeCallee->ResolvedKind == ScopeAccessKind::DefaultConstruct) {
      AllocaInst *Alloca = TmpB.CreateAlloca(LLVMStructType, nullptr, StructName + "_default");
      llvm::Value *Default = EmitDefaultValue(T.get(), Loc);
      if (!Default)
        return nullptr;
      Builder->CreateStore(Default, Alloca);
      return Builder->CreateLoad(LLVMStructType, Alloca, "structval");
    }

    // ConstructorCall: `StructName::new(args...)`.
    const ResolverFunctionSymbol *Target = getResolvedCallTarget();
    if (!Target) {
      SemanticError(Loc, "Internal error: '" + StructName +
                             "::new' has no resolved constructor target -- "
                             "this is a compiler bug.")
          .raise();
      return nullptr;
    }

    // TYPE-002 (S14a): a GENERIC constructor (`fn<T> Box.new(v: T): void`)
    // is now monomorphized (Monomorphize.cpp's drainConstructorRequests())
    // -- its clone's OWN ResolvedDef has self back in Params[0] (never
    // stripped; the clone is an ordinary, ResolvedDef-driven declaration,
    // not a temporary overload-resolution candidate), so its
    // mangledName() is ALREADY correct and complete the ordinary way,
    // exactly like any other resolved call target. Params[0].Name=="self"
    // is what distinguishes this case from the STILL-self-stripped
    // candidate every NON-generic constructor's Target remains (stripSelf()
    // in ResolverCall.cpp runs unconditionally for every `::new(...)` call,
    // generic or not) -- that path still needs the manual reconstruction
    // below, since its own Params never included self to begin with.
    std::string CtorMangledName;
    if (!Target->Params.empty() && Target->Params[0].Name == "self") {
      CtorMangledName = Target->mangledName();
    } else {
      CtorMangledName = "turf_";
      if (!Target->SourceModule.empty())
        CtorMangledName += Target->SourceModule + "_";
      CtorMangledName += Target->BaseName;      // "StructName.new"
      CtorMangledName += "_" + StructName + "_ptr"; // self, reinserted
      for (const auto &P : Target->Params)
        if (P.Type)
          CtorMangledName += "_" + P.Type->getMangledName();
    }

    Function *CtorF = TheModule->getFunction(CtorMangledName);
    if (!CtorF) {
      SemanticError(Loc, "Internal error: LLVM function '" + CtorMangledName +
                             "' not found after successful name resolution. "
                             "This is a compiler bug.")
          .raise();
      return nullptr;
    }

    std::vector<Value *> EvaluatedArgs;
    // Params[0] is `self` only for a monomorphized generic constructor (see
    // just above); otherwise the list is self-stripped and lines up with Args.
    const size_t CtorParamOffset =
        (!Target->Params.empty() && Target->Params[0].Name == "self") ? 1 : 0;
    for (size_t I = 0; I < Args.size(); ++I) {
      const size_t P = I + CtorParamOffset;
      const bool ToSlice = P < Target->Params.size() &&
                           isUnsizedArrayType(Target->Params[P].Type.get());
      Value *V = ToSlice ? CodegenSliceArgument(Args[I].get()) : Args[I]->codegen();
      if (!V)
        return nullptr;
      EvaluatedArgs.push_back(V);
    }

    AllocaInst *SelfAlloca = TmpB.CreateAlloca(LLVMStructType, nullptr, StructName + "_new");
    std::vector<Value *> ArgVals;
    ArgVals.push_back(SelfAlloca); // implicit self pointer

    unsigned Idx = 0;
    for (auto ArgIt = std::next(CtorF->arg_begin()); ArgIt != CtorF->arg_end();
         ++ArgIt, ++Idx) {
      Value *V = EvaluatedArgs[Idx];
      Type *ExpectedTy = ArgIt->getType();
      if (V->getType() != ExpectedTy) {
        // TYPE-002 (S15/D8): see CodegenStruct.cpp's identical fix on
        // MethodCallExprAST::codegen() for the full story -- this block
        // used to erase/un-erase a constructor argument INTO/OUT OF a
        // generic parameter's opaque Type-Erasure slot, dead ever since
        // monomorphization gives every constructor call site a real,
        // concrete callee signature. Verified live, this session:
        // instrumented with an `fprintf` probe, zero hits compiling every
        // file under tests/, lib/std/, and showcase/.
        SemanticError(Loc,
                      "Internal error: constructor call argument type "
                      "mismatch (expected '" +
                          std::string(ExpectedTy->isPointerTy() ? "pointer" :
                                      ExpectedTy->isDoubleTy() ? "double" :
                                      ExpectedTy->isIntegerTy() ? "integer" : "?") +
                          "'-shaped LLVM type, got a different shape). "
                          "This is a compiler bug -- every call site should "
                          "reach a fully-monomorphized, concretely-typed "
                          "callee.")
            .raise();
        return nullptr;
      }
      ArgVals.push_back(V);
    }

    Builder->CreateCall(CtorF, ArgVals);
    return Builder->CreateLoad(LLVMStructType, SelfAlloca, "structval");
  }

  // malloc/realloc/free/assert/fail used to be routed through this dispatch
  // (gated on "no user extern fn with that name exists yet" for the first
  // three), but CodegenIntrinsic itself has had no case for ANY of these
  // five names for a while now -- malloc/realloc/free became ordinary
  // lib/std/mem.tr `extern fn` bindings, assert/fail became ordinary
  // lib/std/test.tr Turf functions. Every program that actually uses any of
  // them imports the owning module, so the "no user declaration" condition
  // this dispatch was gating on is never true in practice; keeping these
  // names listed here was pure dead weight that could only ever reach
  // CodegenIntrinsic's final "Unknown intrinsic: ..." fallback -- a worse,
  // more confusing diagnostic than the normal unresolved-function path
  // below already gives (e.g. "unknown function 'malloc', did you mean
  // mem::alloc?"). `typeof` is the only real intrinsic left (`lengthof`
  // was the other one -- STRING_UFCS_OPERATOR_CHAR_RFC.md §7 step 7 deleted
  // it, `strings::length(s)` is the real replacement).
  if (isIntrinsic(BareIntrinsic::HashOf)) {
    Value *V = Args[0]->codegen();
    if (!V)
      return nullptr;
    return EmitStructuralHash(V, getExprType(Args[0].get()));
  }
  if (isIntrinsic(BareIntrinsic::TypeOf)) {

    std::vector<Value *> ArgVals;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V)
        return nullptr;
      ArgVals.push_back(V);
    }

    // Phase 4 Note 14: typeof should return the compile-time type of the expression,
    // avoiding the loss of generic type information in LLVM IR.
    std::string TypeNameStr = "unknown";
    if (const TurfType *ExprTy = getExprType(Args[0].get())) {
      TypeNameStr = turfTypeName(ExprTy);
    } else {
      // Fallback to LLVM type if AST type resolution failed
      Type *Ty = ArgVals[0]->getType();
      if (Ty->isDoubleTy()) {
        TypeNameStr = "double";
      } else if (Ty->isIntegerTy(64)) {
        TypeNameStr = "int";
      } else if (Ty->isIntegerTy(1)) {
        TypeNameStr = "bool";
      } else if (Ty->isPointerTy()) {
        TypeNameStr = "string";
      } else if (Ty->isVoidTy()) {
        TypeNameStr = "void";
      }
    }
    return Builder->CreateGlobalString(TypeNameStr, "typename");
  }

  // ── gc_alloc(n) / gc_alloc_zeroed(n) / gc_realloc(p, n) ──────────────────
  // MEM-001/MEM-002 (ARCHITECTURE_OVERHAUL.md §8): the Turf-visible surface
  // for libturfrt's typed-allocation entrypoints (runtime/turfrt.h). Handled
  // here, alongside typeof, for the same reason -- these are bare
  // intrinsics with no ResolvedCallTarget/MangledName, so they must be
  // dispatched before the "Issue #9: Codegen trusts the Resolver" path
  // below, which assumes both exist. As of MEM-002, `desc` is
  // PendingGCAllocDescOverride when CastExprAST::codegen() set one (a real
  // per-element TurfTypeDesc for a `gc_alloc(...) to T*`-shaped site,
  // CodegenExpr.cpp), consumed here and reset to nullptr immediately so it
  // never leaks into an unrelated later call; otherwise still a literal
  // null pointer -- MEM-002 defines that as "opaque, no pointer words
  // traced", correct (not a fallback) for a raw byte buffer like a string
  // (see turf_rt_alloc's own doc comment in runtime/turfrt.h).
  if (isIntrinsic(BareIntrinsic::GcAlloc) ||
      isIntrinsic(BareIntrinsic::GcAllocZeroed) ||
      isIntrinsic(BareIntrinsic::GcRealloc)) {
    PointerType *PtrTy = PointerType::getUnqual(*TheContext);
    Type *I64Ty = Type::getInt64Ty(*TheContext);
    Value *DescArg = PendingGCAllocDescOverride
                          ? PendingGCAllocDescOverride
                          : (Value *)ConstantPointerNull::get(PtrTy);
    PendingGCAllocDescOverride = nullptr;

    if (isIntrinsic(BareIntrinsic::GcRealloc)) {
      Value *PVal = Args[0]->codegen();
      Value *NVal = Args[1]->codegen();
      if (!PVal || !NVal)
        return nullptr;
      Function *ReallocFunc = TheModule->getFunction("turf_rt_realloc");
      if (!ReallocFunc) {
        FunctionType *FT =
            FunctionType::get(PtrTy, {PtrTy, I64Ty, PtrTy}, /*isVarArg=*/false);
        ReallocFunc = Function::Create(FT, Function::ExternalLinkage,
                                        "turf_rt_realloc", TheModule.get());
      }
      return Builder->CreateCall(ReallocFunc, {PVal, NVal, DescArg});
    }

    Value *NVal = Args[0]->codegen();
    if (!NVal)
      return nullptr;
    const char *RtFuncName =
        isIntrinsic(BareIntrinsic::GcAlloc) ? "turf_rt_alloc" : "turf_rt_alloc_zeroed";
    Function *AllocFunc = TheModule->getFunction(RtFuncName);
    if (!AllocFunc) {
      FunctionType *FT =
          FunctionType::get(PtrTy, {I64Ty, PtrTy}, /*isVarArg=*/false);
      AllocFunc = Function::Create(FT, Function::ExternalLinkage, RtFuncName,
                                    TheModule.get());
    }
    return Builder->CreateCall(AllocFunc, {NVal, DescArg});
  }

  // ── gc_collect() / gc_is_managed(p) ───────────────────────────────────────
  // MEM-002. Same dispatch shape as gc_alloc above -- bare intrinsics, no
  // ResolvedCallTarget/MangledName.
  if (isIntrinsic(BareIntrinsic::GcCollect)) {
    Function *CollectFunc = TheModule->getFunction("turf_rt_collect");
    if (!CollectFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getVoidTy(*TheContext), {}, /*isVarArg=*/false);
      CollectFunc = Function::Create(FT, Function::ExternalLinkage,
                                      "turf_rt_collect", TheModule.get());
    }
    return Builder->CreateCall(CollectFunc, {});
  }
  if (isIntrinsic(BareIntrinsic::GcIsManaged)) {
    PointerType *PtrTy = PointerType::getUnqual(*TheContext);
    Value *PVal = Args[0]->codegen();
    if (!PVal)
      return nullptr;
    Function *IsManagedFunc = TheModule->getFunction("turf_rt_is_gc_pointer");
    if (!IsManagedFunc) {
      FunctionType *FT = FunctionType::get(Type::getInt32Ty(*TheContext),
                                            {PtrTy}, /*isVarArg=*/false);
      IsManagedFunc = Function::Create(FT, Function::ExternalLinkage,
                                        "turf_rt_is_gc_pointer",
                                        TheModule.get());
    }
    Value *IntResult = Builder->CreateCall(IsManagedFunc, {PVal});
    return Builder->CreateICmpNE(
        IntResult, ConstantInt::get(Type::getInt32Ty(*TheContext), 0),
        "gc_is_managed");
  }
  if (isIntrinsic(BareIntrinsic::GcCheckedFree)) {
    PointerType *PtrTy = PointerType::getUnqual(*TheContext);
    Value *PVal = Args[0]->codegen();
    if (!PVal)
      return nullptr;
    Function *CheckedFreeFunc = TheModule->getFunction("turf_rt_checked_free");
    if (!CheckedFreeFunc) {
      FunctionType *FT = FunctionType::get(Type::getVoidTy(*TheContext),
                                            {PtrTy}, /*isVarArg=*/false);
      CheckedFreeFunc = Function::Create(FT, Function::ExternalLinkage,
                                          "turf_rt_checked_free",
                                          TheModule.get());
    }
    return Builder->CreateCall(CheckedFreeFunc, {PVal});
  }
  if (isIntrinsic(BareIntrinsic::GcPin) || isIntrinsic(BareIntrinsic::GcUnpin)) {
    PointerType *PtrTy = PointerType::getUnqual(*TheContext);
    Value *PVal = Args[0]->codegen();
    if (!PVal)
      return nullptr;
    const char *RtFuncName =
        isIntrinsic(BareIntrinsic::GcPin) ? "turf_rt_pin" : "turf_rt_unpin";
    Function *PinFunc = TheModule->getFunction(RtFuncName);
    if (!PinFunc) {
      FunctionType *FT = FunctionType::get(Type::getVoidTy(*TheContext),
                                            {PtrTy}, /*isVarArg=*/false);
      PinFunc = Function::Create(FT, Function::ExternalLinkage, RtFuncName,
                                  TheModule.get());
    }
    return Builder->CreateCall(PinFunc, {PVal});
  }

  // ── heap(expr) → &T ───────────────────────────────────────────────────────
  // FDN-001 (DEC-5): the safe-heap-allocation intrinsic. Same dispatch shape
  // as every other bare intrinsic above -- no ResolvedCallTarget/MangledName
  // -- but the actual allocation logic (size + real TurfTypeDesc selection +
  // turf_rt_alloc + store) is shared with BoxToI64Slot()'s identical
  // aggregate-boxing need, via HeapAllocateValue() (CodegenCore.cpp).
  if (isIntrinsic(BareIntrinsic::Heap)) {
    Value *ArgVal = Args[0]->codegen();
    if (!ArgVal)
      return nullptr;
    return HeapAllocateValue(ArgVal);
  }

  std::vector<Value *> EvaluatedArgs;
  for (size_t I = 0; I < Args.size(); ++I) {
    // A `T[]` parameter takes a slice, which needs the argument's address,
    // not its loaded value -- see CodegenSliceArgument().
    const bool ToSlice = ResolvedCallTarget &&
                         I < ResolvedCallTarget->Params.size() &&
                         isUnsizedArrayType(ResolvedCallTarget->Params[I].Type.get());
    Value *V = ToSlice ? CodegenSliceArgument(Args[I].get()) : Args[I]->codegen();
    if (!V)
      return nullptr;
    EvaluatedArgs.push_back(V);
  }

  // ── Issue #9: Codegen trusts the Resolver ────────────────────────────────
  // ResolvedCallTarget was set by Resolver::visitFuncCall (mandatory pass).
  // Codegen reads the pre-computed mangled name blindly — no scanning, no
  // FuncParamTypes lookups, no arity checks.  Those are the Resolver's job.
  //
  // typeof is handled above (the one remaining language-level bare
  // intrinsic that never passes through the Resolver's function registry).
  std::string MangledName;
  if (CalleeName == "main") {
    MangledName = "main";
  } else if (ResolvedCallTarget) {
    MangledName = ResolvedCallTarget->mangledName();
  } else {
    // ResolvedCallTarget is null: this can happen for extern fns whose
    // raw C name is used directly.  Fall back to the user-visible name.
    MangledName = CalleeName;
  }

  Function *CalleeF = TheModule->getFunction(MangledName);
  if (!CalleeF) {
    // Fallback: extern fn declarations register under the raw C name, not the
    // mangled name (e.g. `strlen`, not `turf_strlen_string`).  Try the user-
    // visible callee name before declaring an internal compiler error.
    CalleeF = TheModule->getFunction(CalleeName);
  }
  if (!CalleeF) {
    // This path should be unreachable if the Resolver ran correctly.
    // If hit, it's an internal compiler bug — not a user error.
    SemanticError(Loc, "Internal error: LLVM function '" + MangledName +
                           "' not found after successful name resolution. "
                           "This is a compiler bug.")
        .raise();
    return nullptr;
  }

  std::vector<Value *> ArgVals;
  unsigned Idx = 0;

  // Look up full param type info (needed for generic struct params).
  // Try mangled name first; fall back to raw callee name for extern fns.
  auto ParamTypeIt = FuncParamTypes.find(MangledName);
  if (ParamTypeIt == FuncParamTypes.end())
    ParamTypeIt = FuncParamTypes.find(CalleeName);

  for (auto &Arg : CalleeF->args()) {
    // STD-002: a `@caller_location` callee's REAL LLVM arg count is one
    // more than `EvaluatedArgs.size()` (the ordinary, Turf-level, written
    // argument list) -- its hidden trailing argument is appended
    // separately, below, never read from EvaluatedArgs (there is nothing
    // there to read: the user never writes it). Bounding this loop here,
    // rather than letting it run over the extra LLVM argument too, is what
    // keeps `EvaluatedArgs[Idx]` from indexing out of bounds.
    if (Idx >= EvaluatedArgs.size())
      break;
    Value *V = EvaluatedArgs[Idx];

    // Determine declared TurfTypePtr for this param
    TurfTypePtr DeclaredType;
    if (ParamTypeIt != FuncParamTypes.end() && Idx < ParamTypeIt->second.size())
      DeclaredType = ParamTypeIt->second[Idx];
    else
      DeclaredType = getTurfTypeFromLLVM(Arg.getType());

    // Arrays pass through unchanged: a `T[]` argument is already the slice
    // the evaluation loop above built (CodegenSliceArgument()), and a
    // fixed-size `T[N]` is passed by value as the loaded array.
    if (isArrayType(DeclaredType.get())) {
      if (isUnsizedArrayType(DeclaredType.get()) &&
          V->getType() != getSliceLLVMType()) {
        // The callee wants a slice, but ResolvedCallTarget did not say so
        // when the arguments were evaluated -- a compiler bug, not a user's.
        llvm::report_fatal_error(
            "Internal error: a 'T[]' argument reached the call without being "
            "made into a slice.");
        return nullptr;
      }
    } else {
      // Standard scalar type casting.
      //
      // Always prefer the already-computed, accurate DeclaredType over
      // re-deriving a type from the callee's raw LLVM parameter type.
      //
      // Getting this wrong for the concrete (non-generic) case is not
      // hypothetical: a struct-typed parameter's LLVM type is an LLVM
      // struct (not a primitive getTurfTypeFromLLVM() recognizes), so it
      // silently fell through to that function's `return TurfDouble();`
      // catch-all. The resulting CastToType() call "happened" to still pass
      // the struct value through unchanged (both sides of its exact-match
      // fast path degraded to "double" symmetrically) -- but only by
      // accident, and it skipped right past CastToType()'s own explicit,
      // purpose-built struct-to-struct passthrough check ("if DestType is a
      // struct ... pass through without casting") instead of actually
      // exercising it. Using DeclaredType directly routes through that
      // intended path.
      //
      // TYPE-002 (S14a): the redundant PRE-unerase this block used to do
      // before even calling CastToType() (manually PtrToInt/bitcasting a
      // pointer-shaped value into int/bool/double whenever DeclaredType was
      // numeric) is deleted -- CastToType()'s OWN matching branch is gone
      // too (INV-GEN-1: DeclaredType can no longer legitimately be a
      // TurfGenericParamType here at all, so there is no "erased" value
      // left to un-erase). See the change record's *Removed assumptions*.
      TurfTypePtr ExpectedType = DeclaredType;
      V = CastToType(V, ExpectedType, "argcast", Loc);
    }

    ArgVals.push_back(V);
    Idx++;
  }

  // STD-002: auto-supply the hidden trailing argument a `@caller_location`
  // callee's REAL signature has but its Turf-level `Args` never wrote --
  // see BuildCallerLocationValue()'s own doc comment (CodegenInternal.h)
  // for the forward-vs-stamp decision this makes.
  if (ResolvedCallTarget && ResolvedCallTarget->HasCallerLocation)
    ArgVals.push_back(BuildCallerLocationValue(Loc));

  // For variadic functions, append any extra arguments beyond the declared
  // parameter count. These are passed raw per the C vararg ABI — no casting.
  if (CalleeF->isVarArg()) {
    for (size_t i = Idx; i < EvaluatedArgs.size(); ++i)
      ArgVals.push_back(EvaluatedArgs[i]);
  }

  // void functions must not be given a name (LLVM requirement)
  if (CalleeF->getReturnType()->isVoidTy())
    return Builder->CreateCall(CalleeF, ArgVals);

  return Builder->CreateCall(CalleeF, ArgVals, "calltmp");
}


// A declaration codegens to nothing meaningful, mirroring EnumDeclExprAST
// above -- the real type info was already registered by the Parser/Resolver.
// Runtime representation for `!T`/error variants lands in Stage 3
// (docs/architecture/errors_and_recover.md).
Value *ErrorDeclExprAST::codegen() {
  return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
}


// `<expr> recover ...` (docs/architecture/errors_and_recover.md) -- branches on
// the wrapped FallibleValue's isOk bit, unboxes/produces/runs accordingly, and
// merges back to a single value of the success type T (or, if T is void, no
// value at all -- see the IsVoidSuccess early-return below).
//
// A block that diverges (every `recover run`/`recover as err run` except the
// void-success case -- enforced by TypeChecker's AllPathsDiverge check)
// leaves Builder's insert point on the "after_ret" dead block
// ReturnExprAST::codegen() creates after every `ret` -- unconditionally
// branching that dead block to MergeBB below (same as the live/void-success
// fall-through case) is deliberately NOT special-cased away: that branch is
// provably unreachable at runtime (nothing can reach a block positioned
// right after an unconditional `ret`), so it becomes a harmless extra PHI
// predecessor rather than a correctness problem -- LLVM permits this, and it
// avoids needing a parallel, error-prone re-implementation of the
// TypeChecker's own divergence analysis here.
Value *RecoverExprAST::codegen() {
  Value *FV = Wrapped ? Wrapped->codegen() : nullptr;
  if (!FV)
    return nullptr;

  TurfTypePtr SuccessTy = ResolvedSuccessType ? ResolvedSuccessType : TurfVoid();
  bool IsVoidSuccess = SuccessTy->isVoid();

  Value *IsOk = Builder->CreateExtractValue(FV, 0, "fv.isok");

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *SuccessBB = BasicBlock::Create(*TheContext, "recover.ok", TheFunction);
  BasicBlock *FailBB = BasicBlock::Create(*TheContext, "recover.fail", TheFunction);
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "recover.merge");

  Builder->CreateCondBr(IsOk, SuccessBB, FailBB);

  // ── Success path ──────────────────────────────────────────────────────
  Builder->SetInsertPoint(SuccessBB);
  Value *SuccessVal = nullptr;
  if (!IsVoidSuccess) {
    Value *SuccessBits = Builder->CreateExtractValue(FV, 1, "fv.bits");
    SuccessVal = UnboxFromI64Slot(SuccessBits, SuccessTy.get());
  }
  BasicBlock *SuccessEndBB = Builder->GetInsertBlock();
  Builder->CreateBr(MergeBB);

  // ── Failure path ─────────────────────────────────────────────────────
  Builder->SetInsertPoint(FailBB);
  Value *ErrPayload = Builder->CreateExtractValue(FV, 2, "fv.errptr");

  Value *FailVal = IsVoidSuccess ? nullptr : llvm::UndefValue::get(getLLVMType(SuccessTy.get()));
  BasicBlock *FailEndBB = nullptr;

  if (Kind == RecoverKind::Produce) {
    Value *P = ProduceExpr ? ProduceExpr->codegen() : nullptr;
    if (!P)
      return nullptr;
    FailVal = IsVoidSuccess ? nullptr : CastToType(P, SuccessTy, "producecast");
    FailEndBB = Builder->GetInsertBlock();
    Builder->CreateBr(MergeBB);
  } else {
    // Run / AsRun: AsRun binds `err` to the error payload for the duration
    // of the block, same save/restore-NamedValues + GlobalSymbolTable
    // scope-push/pop idiom ForExprAST::codegen() already uses for its loop
    // variable.
    bool HadOldBinding = false;
    VarInfo SavedBinding;
    if (Kind == RecoverKind::AsRun) {
      AllocaInst *ErrAlloca = CreateEntryBlockAlloca(TheFunction, BoundName, TurfAnyError().get());
      Builder->CreateStore(ErrPayload, ErrAlloca);

      auto OldBinding = NamedValues.find(BoundName);
      if (OldBinding != NamedValues.end()) {
        SavedBinding = OldBinding->second;
        HadOldBinding = true;
      }
      NamedValues[BoundName] = {ErrAlloca, TurfAnyError()};

      if (GlobalSymbolTable) {
        GlobalSymbolTable->EnterScope();
        GlobalSymbolTable->DeclareSymbol(BoundName, TurfAnyError(),
                                        {Loc.Line, Loc.Col}, ErrAlloca);
      }
    }

    Value *BlockResult = Block ? Block->codegen() : nullptr;

    if (Kind == RecoverKind::AsRun) {
      if (GlobalSymbolTable)
        GlobalSymbolTable->ExitScope();
      if (HadOldBinding)
        NamedValues[BoundName] = SavedBinding;
      else
        NamedValues.erase(BoundName);
    }

    if (!BlockResult)
      return nullptr;

    // See this function's doc comment for why branching unconditionally
    // here (rather than only for the "legitimately falls through" case) is
    // correct even when the block actually diverged.
    if (!Builder->GetInsertBlock()->getTerminator()) {
      FailEndBB = Builder->GetInsertBlock();
      Builder->CreateBr(MergeBB);
    }
  }

  // ── Merge ────────────────────────────────────────────────────────────
  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);

  if (IsVoidSuccess)
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));

  llvm::PHINode *Phi =
      Builder->CreatePHI(getLLVMType(SuccessTy.get()), 2, "recover.result");
  Phi->addIncoming(SuccessVal, SuccessEndBB);
  if (FailEndBB)
    Phi->addIncoming(FailVal, FailEndBB);
  return Phi;
}
