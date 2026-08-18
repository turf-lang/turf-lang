/**
 * @file CodegenFunc.cpp
 * @brief Function codegen: definitions, extern declarations, calls, recover, error declarations.
 * @details Part of LLVM IR generation for TurfLang, split from the original
 *          monolithic Codegen.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/planning/COMPILER_MODULE_BREAKDOWN.md).
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

#include "Codegen.h"
#include "AST.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "CFG.h"
#include "Errors.h"
#include "Analysis.h"
#include "AliasAnalysis.h"
#include "Lexer.h"
#include "ValueRange.h"
#include "Lint.h"
#include "SymbolTable.h"
#include "CallGraph.h"
#include "Types.h"
#include "CodegenInternal.h"
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
// Known limitation: an explicit in-Turf call to `main()` (CodegenFunc.cpp's
// own CalleeName == "main" passthrough further below suggests this was once
// intended to be callable) would now mismatch this synthesized 2-arg LLVM
// signature against the 0-arg call site and fail LLVM's module verifier.
// No test or showcase file does this -- recursive/explicit `main()` calls
// aren't part of the language's supported surface.
static GlobalVariable *ArgcStorageGV = nullptr;
static GlobalVariable *ArgvStorageGV = nullptr;

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
  Function *ArgcF = TheModule->getFunction("TurfRuntimeGetArgc");
  if (!ArgcF)
    ArgcF = Function::Create(ArgcFT, Function::ExternalLinkage,
                             "TurfRuntimeGetArgc", TheModule.get());
  BasicBlock *ArgcBB = BasicBlock::Create(*TheContext, "entry", ArgcF);
  IRBuilder<> ArgcBuilder(ArgcBB);
  ArgcBuilder.CreateRet(ArgcBuilder.CreateLoad(I64Ty, ArgcStorageGV));

  // ptr TurfRuntimeGetArgv(int index) -- bounds-checked; returns "" (not a
  // crash) for an out-of-range index, since a Turf `string` and a raw `ptr`
  // are the same opaque LLVM pointer type (see getLLVMType in
  // CodegenCore.cpp), so the empty-string constant is a valid return value
  // with no conversion needed. Same reuse-if-declared reasoning as above.
  FunctionType *ArgvFT = FunctionType::get(PtrTy, {I64Ty}, /*isVarArg=*/false);
  Function *ArgvF = TheModule->getFunction("TurfRuntimeGetArgv");
  if (!ArgvF)
    ArgvF = Function::Create(ArgvFT, Function::ExternalLinkage,
                             "TurfRuntimeGetArgv", TheModule.get());
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

// FuncDefExprAST::codegen
// Two-pass strategy:
//   Pass 1 (prototype): if no LLVM Function yet, create it (no body).
//   Pass 2 (body):      if function has no body yet, fill it in.
// Calling codegen() twice is what makes forward calls work: main.cpp's
// pre-pass creates prototypes, then the normal loop fills bodies.
Value *FuncDefExprAST::codegen() {
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

    // Build the LLVM FunctionType. `main` is special: it's always the real
    // C-ABI entry point (argc: i32, argv: ptr) -> ReturnType, regardless of
    // Turf's Params list (always empty -- see EnsureArgvHelpers() above).
    std::vector<Type *> ParamTypes;
    if (Name == "main") {
      ParamTypes = {Type::getInt32Ty(*TheContext),
                    PointerType::get(*TheContext, 0)};
    } else {
      for (const auto &P : Params)
        ParamTypes.push_back(getLLVMType(P.Type.get()));
    }

    FunctionType *FT =
        FunctionType::get(getLLVMType(ReturnType.get()), ParamTypes,
                          /*isVarArg=*/false);
    auto Linkage = (Name == "main") ? Function::ExternalLinkage
                                    : Function::InternalLinkage;
    TheFunc = Function::Create(FT, Linkage, MangledName, TheModule.get());

    // Name the arguments
    if (Name == "main") {
      auto ArgIt = TheFunc->args().begin();
      ArgIt->setName("argc");
      (++ArgIt)->setName("argv");
    } else {
      unsigned Idx = 0;
      for (auto &Arg : TheFunc->args())
        Arg.setName(Params[Idx++].Name);
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

  // Switch to the new function
  Builder->SetInsertPoint(BB);
  NamedValues.clear();
  CurrentFuncReturnType = ReturnType;
  CurrentFunction = TheFunc;

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
    for (auto &Arg : TheFunc->args()) {
      TurfTypePtr ParamTurfType = TurfInt(); // fallback
      for (const auto &P : Params)
        if (P.Name == Arg.getName())
          ParamTurfType = P.Type;

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
    }
  }

  // Lint the function body before codegen
  LintExpression(Body.get());

  // Codegen the body
  Body->codegen();

  // Collect call edges from the AST for this function (safer than scanning CFGs)
  if (Body) {
    std::function<void(const ExprAST*)> scanCalls = [&](const ExprAST *N) {
      if (!N) return;
      if (auto FC = dynamic_cast<const FuncCallExprAST*>(N)) {
        if (auto CalleeVar = dynamic_cast<const VariableExprAST*>(FC->getCallee())) {
          std::string callee = CalleeVar->getName();
          AddCallEdge(Name, callee);
        }
      }
      if (auto B = dynamic_cast<const BinaryExprAST*>(N)) { scanCalls(B->getLHS()); scanCalls(B->getRHS()); }
      else if (auto U = dynamic_cast<const UnaryExprAST*>(N)) scanCalls(U->getOperand());
      else if (auto As = dynamic_cast<const AssignmentExprAST*>(N)) { scanCalls(As->getLHS()); scanCalls(As->getRHS()); }
      else if (auto Vd = dynamic_cast<const VarDeclExprAST*>(N)) scanCalls(Vd->getInitializer());
      else if (auto Bl = dynamic_cast<const BlockExprAST*>(N)) for (const auto &e : Bl->getExpressions()) scanCalls(e.get());
      else if (auto If = dynamic_cast<const IfExprAST*>(N)) { for (const auto &Br : If->getBranches()) { scanCalls(Br.Cond.get()); scanCalls(Br.Body.get()); } scanCalls(If->getElse()); }
    };
    scanCalls(Body.get());
  }

  // Build and analyze CFG for flow diagnostics
  if (Body) {
    CFGBuilder CFGBuilder;
    auto FuncCFG = CFGBuilder.buildCFG(Name, ReturnType, Body.get(), Loc, MangledName);

    // Store CFG for later use (push before analyses so it survives raises)
    GlobalCFGs.push_back(std::move(FuncCFG));

    // Run flow analysis and report diagnostics (will raise error for missing
    // returns in non-void)
    CFG *CfgPtr = GlobalCFGs.back().get();
    CfgPtr->reportFlowDiagnostics();

    // CFG Phase 1 (Issue #185): Cyclomatic complexity check.
    // M = E - N + 2P; threshold 15. Warning only — does not stop compilation.
    int M = CfgPtr->calculateCyclomaticComplexity();
    if (M > 15)
      CyclomaticComplexityWarning(Loc, Name, M).emit();

    // CFG Phase 1 (Issue #185): Infinite loop detection.
    // A back-edge cycle with no Break/Return terminator AND no outside-cycle
    // successor means the loop can never exit. Warning only.
    if (CfgPtr->hasUnbreakableLoop())
      InfiniteLoopWarning(Loc, Name).emit();

    // Run definite-assignment analysis (parameters are considered assigned)
    DefiniteAssignmentPass DAP;
    DAP.runOnCFG(*CfgPtr, ParamIDs);

    // Run value-range (constant) propagation
    ValueRangePass VRP;
    VRP.runOnCFG(*CfgPtr, ParamIDs);

    // Run alias/lifetime analysis (detect use-after-free, double-free)
    AliasAnalysisPass AAP;
    AAP.runOnCFG(*CfgPtr, ParamIDs);

    // CFG Phase 2 (Issue #186): Liveness analysis (dead stores & unused vars).
    // Collect parameter VariableSymbol* pointers to exclude from warnings.
    {
      std::unordered_set<const VariableSymbol *> ParamSyms;
      // Parameters are registered in the body scope; their ResolvedSymbols
      // are attached to the parameter VarDeclExprAST nodes created by Codegen.
      // We collect them via the ParamIDs workaround already used by DAP:
      // iterate GlobalSymbolTable for the current function's param entries.
      // (Implementation detail: we use the same ParamIDs vector already
      //  available at this point in FuncDefExprAST::codegen().)
      // NOTE: The simplest correct approach: build ParamSyms by walking the
      // function params from the FuncDefExprAST node directly (Params_ vector),
      // using the ResolvedSymbol attached to each param's VarDeclExprAST.
      // This mirrors exactly how DefiniteAssignmentPass receives InitialAssigned.
      // For now, pass an empty set — parameters are excluded by AllBodyDeclared
      // (they don't appear in VarDeclExprAST nodes in the body).
      LivenessAnalysisPass LAP;
      LAP.runOnCFG(*CfgPtr, ParamSyms);
    }

    // CFG Phase 2 (Issue #186): Infinite recursion detection.
    // Check if every path from entry to exit passes through a self-call.
    // This is a fatal error — infinite recursion guarantees a stack overflow.
    if (CfgPtr->allPathsRecurse()) {
      RecursionWithoutBaseError(Loc, Name).raise();
    }

    // Constructor field-assignment analysis: `fn StructName.new(...): void`
    // must assign every field of StructName on every path (see
    // ConstructorFieldAssignmentPass, Analysis.h/Analysis.cpp). The
    // internal name suffix ".new" unambiguously identifies a real
    // constructor here -- Resolver::visitFuncDef already guarantees 'new'
    // can never be declared 'static' and 'default' can never be declared
    // by hand at all, so nothing else can produce this exact suffix on a
    // struct-bound method.
    {
      size_t Dot = Name.rfind('.');
      if (Dot != std::string::npos && Name.substr(Dot + 1) == "new") {
        std::string CtorStructName = Name.substr(0, Dot);
        TurfTypePtr CtorStructT =
            GlobalSymbolTable ? GlobalSymbolTable->LookupType(CtorStructName) : nullptr;
        if (auto *CtorSType = dynamic_cast<TurfStructType *>(CtorStructT.get())) {
          std::vector<std::string> FieldNames;
          for (const auto &F : CtorSType->getFields())
            FieldNames.push_back(F.Name);
          ConstructorFieldAssignmentPass CFAP;
          CFAP.runOnCFG(*CfgPtr, CtorStructName, FieldNames, Loc);
        }
      }
    }
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
    } else if (ReturnType->isVoid()) {
      Builder->CreateRetVoid();
    } else {
      // CFG already reported the error, but we still need to generate valid IR
      Builder->CreateRet(Constant::getNullValue(getLLVMType(ReturnType.get())));
    }
  }

  verifyFunction(*TheFunc);

  // Exit function scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // Restore caller context
  NamedValues = SavedNamedValues;
  CurrentFuncReturnType = SavedReturnType;
  CurrentFunction = SavedFunction;
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
  // at the first null statement (see git history for the now-removed
  // docs/planning/CODE_REVIEW_GAPS_2026-07.md's original writeup).
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
// Rather than push platform knowledge into every extern declaration site
// (there's only this one case today), ExternFuncDeclExprAST::codegen()
// consults this small, explicit table for the specific extern name being
// declared and, if the HOST platform this compiler itself was built for
// has a same-signature equivalent under a different name, emits a
// one-line DEFINED wrapper under the Turf-visible name that forwards to
// the real platform symbol -- instead of a bare external declaration.
// Every other extern-fn code path (call resolution, the symbol table,
// FuncParamTypes/FuncReturnTypes) keeps treating this exactly like an
// ordinary external declaration; only this one function's LLVM IR shape
// (defined body vs. no body) differs.
//
// Returns the real symbol to forward `Name` to, or empty if `Name` needs
// no remapping on this platform (either because the literal libc name
// already resolves here, or because `Name` isn't one of the small set of
// known-portability-hazard symbols this table covers).
static std::string PlatformExternForward(const std::string &Name) {
#if defined(__APPLE__)
  if (Name == "malloc_usable_size")
    return "malloc_size";
#elif defined(_WIN32)
  // Same void* -> size_t shape as malloc_usable_size(); MSVC CRT's name
  // for it. Untested on Windows in this environment -- guarded behind
  // _WIN32 so it can only ever affect a Windows build, never Linux/macOS.
  if (Name == "malloc_usable_size")
    return "_msize";
#endif
  (void)Name;
  return "";
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
  if (Function *Existing = TheModule->getFunction(Name_)) {
    if (Existing->getFunctionType() != FT) {
      auto It = ExternDeclLocations.find(Name_);
      auto Err = ConflictingExternDeclarationError(Loc_, Name_);
      if (It != ExternDeclLocations.end())
        Err.withRelated(It->second, "first declared here");
      Err.raise();
    }
    return Existing;
  }
  ExternDeclLocations[Name_] = Loc_;

  Function *F;
  std::string ForwardTo = PlatformExternForward(Name_);
  if (!ForwardTo.empty()) {
    // This platform doesn't export `Name_` under that literal name --
    // declare the real symbol (same FT, since it's a documented
    // same-signature equivalent) and define Name_ as a one-line wrapper
    // that forwards every argument to it. See PlatformExternForward()'s
    // doc comment for why this happens here instead of at the call site.
    Function *RealF = TheModule->getFunction(ForwardTo);
    if (!RealF)
      RealF = Function::Create(FT, Function::ExternalLinkage, ForwardTo,
                               TheModule.get());

    F = Function::Create(FT, Function::ExternalLinkage, Name_, TheModule.get());
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
  } else {
    // Create with ExternalLinkage — no body block is created.
    F = Function::Create(FT, Function::ExternalLinkage, Name_, TheModule.get());
  }

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

  // NOTE: `ErrorType::Variant(...)` (call syntax) never reaches Codegen at
  // all -- Resolver::visitFuncCall rejects it with a TypeError ("is an
  // error variant, not a function") before this point, exactly like a
  // plain `enum` variant. No branch is needed here.

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
    TurfTypePtr T = GlobalSymbolTable ? GlobalSymbolTable->LookupType(StructName) : nullptr;
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
      Builder->CreateStore(GetDefaultValue(T.get()), Alloca);
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

    std::string CtorMangledName = "turf_";
    if (!Target->SourceModule.empty())
      CtorMangledName += Target->SourceModule + "_";
    CtorMangledName += Target->BaseName;      // "StructName.new"
    CtorMangledName += "_" + StructName + "_ptr"; // self, reinserted
    for (const auto &P : Target->Params)
      if (P.Type)
        CtorMangledName += "_" + P.Type->getMangledName();

    Function *CtorF = TheModule->getFunction(CtorMangledName);
    if (!CtorF) {
      SemanticError(Loc, "Internal error: LLVM function '" + CtorMangledName +
                             "' not found after successful name resolution. "
                             "This is a compiler bug.")
          .raise();
      return nullptr;
    }

    std::vector<Value *> EvaluatedArgs;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
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
        if (ExpectedTy->isPointerTy() && V->getType()->isIntegerTy())
          V = Builder->CreateIntToPtr(V, ExpectedTy, "argcast_ptr");
        else if (ExpectedTy->isIntegerTy() && V->getType()->isPointerTy())
          V = Builder->CreatePtrToInt(V, ExpectedTy, "argcast_int");
        else if (ExpectedTy->isDoubleTy() && V->getType()->isPointerTy()) {
          V = Builder->CreatePtrToInt(V, Type::getInt64Ty(*TheContext));
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_dbl");
        } else if (ExpectedTy->isPointerTy() && V->getType()->isDoubleTy()) {
          V = Builder->CreateBitCast(V, Type::getInt64Ty(*TheContext), "argcast_dbl_to_i64");
          V = Builder->CreateIntToPtr(V, ExpectedTy, "argcast_erase_dbl");
        } else
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_bit");
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
  if (CalleeName == "typeof") {

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

  std::vector<Value *> EvaluatedArgs;
  for (auto &Arg : Args) {
    Value *V = Arg->codegen();
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
    Value *V = EvaluatedArgs[Idx];

    // Determine declared TurfTypePtr for this param
    TurfTypePtr DeclaredType;
    if (ParamTypeIt != FuncParamTypes.end() && Idx < ParamTypeIt->second.size())
      DeclaredType = ParamTypeIt->second[Idx];
    else
      DeclaredType = getTurfTypeFromLLVM(Arg.getType());

    // Unsized array params: the function expects a pointer to the array data.
    // We must pass the raw AllocaInst of the caller's array, not a loaded
    // value. TypeChecker has already verified the caller is passing an array
    // variable with a compatible element type.
    // Note: Fixed-size arrays (Size > 0) are passed by value, so V remains the
    // evaluated argument (the loaded array value).
    if (isArrayType(DeclaredType.get())) {
      const auto *ArrTy = dynamic_cast<const ::ArrayType *>(DeclaredType.get());
      if (ArrTy && ArrTy->getSize() == -1) {
        if (auto *VE = dynamic_cast<VariableExprAST *>(Args[Idx].get())) {
          V = VE->codegenLValue();
          assert(V && "Array argument LValue generation failed");
        } else {
          llvm::report_fatal_error(
              "Internal error: array argument must be a variable. "
              "This should have been caught by the TypeChecker.");
          return nullptr;
        }
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
      // This previously special-cased away from DeclaredType whenever it
      // WAS a TurfGenericParamType (the T in `fn<T> foo(x: T)`), substituting
      // getTurfTypeFromLLVM(Arg.getType()) (== TurfPtr()) instead, on the
      // theory that a type-erased parameter has no single "real" type to
      // use. That reasoning no longer applies now that Resolver::
      // substituteTypeParams() (see Resolver.cpp) mutates the shared AST
      // node so DeclaredType really is a genuine, meaningful
      // TurfGenericParamType instance here -- and CastToType() has a
      // purpose-built erasure branch keyed on exactly that dynamic_cast
      // (`if (dynamic_cast<const TurfGenericParamType *>(DestType))`,
      // further down in this file). Substituting TurfPtr() instead skipped
      // right past that branch the same way the struct case above did:
      // TurfPtr() doesn't match any of CastToType()'s numeric-conversion
      // branches either, so the scalar argument value (e.g. a bare `i1` for
      // `identity(true)`) was returned completely unconverted and stored
      // into an opaque-pointer-typed slot with no cast at all -- confirmed
      // to segfault for a bool argument and silently corrupt a double
      // argument. Using DeclaredType directly here fixes both.
      TurfTypePtr ExpectedType = DeclaredType;

      // Type-erasure un-erase: if the value is a pointer (erased generic T)
      // and the expected type is numeric, convert via PtrToInt/bitcast.
      if (V->getType()->isPointerTy() &&
          (ExpectedType->isNamed("int") || ExpectedType->isNamed("bool") ||
           ExpectedType->isNamed("double"))) {
        Value *IntVal = Builder->CreatePtrToInt(V, Type::getInt64Ty(*TheContext), "unerase_arg");
        if (ExpectedType->isNamed("bool"))
          V = Builder->CreateICmpNE(IntVal, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "unerase_bool");
        else if (ExpectedType->isNamed("double"))
          V = Builder->CreateBitCast(IntVal, Type::getDoubleTy(*TheContext), "unerase_dbl");
        else
          V = IntVal;
      }

      V = CastToType(V, ExpectedType, "argcast", Loc);
    }

    ArgVals.push_back(V);
    Idx++;
  }

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
// (docs/planning/Errors.md).
Value *ErrorDeclExprAST::codegen() {
  return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
}


// `<expr> recover ...` (docs/planning/Errors.md) -- branches on the wrapped
// FallibleValue's isOk bit, unboxes/produces/runs accordingly, and merges
// back to a single value of the success type T (or, if T is void, no value
// at all -- see the IsVoidSuccess early-return below).
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
