/**
 * @file Builtins.cpp
 * @brief Implementation of TurfLang intrinsic and built-in functions.
 * @details Handles built-in operations like memory management (malloc, free),
 *          input/output (print, input), mathematical operations, and direct
 * LLVM IR mapping.
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

#include "Builtins.h"
#include "Codegen.h" // TheModule, Builder, TheContext
#include "Errors.h"  // SemanticError
#include "Lexer.h"   // SourceLocation

#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"

using namespace llvm;

// Runtime registry for builtins
#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

static std::unordered_map<std::string, BuiltinInfo> g_builtins;
static std::once_flag g_builtins_init_flag;



static void ensure_builtins_registered();

void RegisterBuiltin(const std::string &Name, const BuiltinInfo &Info) {
  g_builtins[Name] = Info;
}

const BuiltinInfo *LookupBuiltin(const std::string &Name) {
  std::call_once(g_builtins_init_flag, ensure_builtins_registered);
  auto It = g_builtins.find(Name);
  if (It == g_builtins.end()) return nullptr;
  return &It->second;
}

std::vector<std::string> GetBuiltinNames() {
  std::call_once(g_builtins_init_flag, ensure_builtins_registered);
  std::vector<std::string> Names;
  Names.reserve(g_builtins.size());
  for (const auto &P : g_builtins) Names.push_back(P.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

static void ensure_builtins_registered() {
  // Only register the two true bare language-level intrinsics that are
  // callable without a module qualifier.  These are the only names that
  // appear in g_builtins for suggestion quality (suggestBuiltins()) and
  // for the IntrinsicArity guard in CodegenIntrinsic().
  //
  // stdlib functions (print, printline, input, malloc, abs, power, sqrt, …)
  // live in lib/std modules and are resolved via the Resolver's module
  // registry — they must NOT be listed here.
  RegisterBuiltin("typeof",   {1, "Compile-time type name of an expression"});
  RegisterBuiltin("lengthof", {1, "Byte-length of a string"});
  RegisterBuiltin("assert",   {-1, "Asserts a condition at runtime"});
  RegisterBuiltin("fail",     {1, "Aborts execution with a message"});
}

/**
 * @brief Generates LLVM IR for a built-in intrinsic function.
 *
 * Takes the name of an intrinsic (e.g., "print", "malloc") and its arguments,
 * then constructs the appropriate LLVM instructions or C standard library
 * function calls.
 *
 * @param IntrinsicName The string name of the intrinsic function to generate.
 * @param Args A vector of LLVM Values representing the arguments to the
 * intrinsic.
 * @param Loc The source location where the intrinsic is called, used for error
 * reporting.
 * @return llvm::Value* The resulting LLVM Value after evaluating the intrinsic,
 * or nullptr on failure.
 * @note Arity and type validation are the TypeChecker's responsibility.
 *       By the time this is called, all arguments are guaranteed correct.
 */
llvm::Value *CodegenIntrinsic(const std::string &IntrinsicName,
                              const std::vector<llvm::Value *> &Args,
                              SourceLocation Loc) {

  if (IntrinsicName == "print") {
    Value *Val = Args[0];
    Function *PrintfFunc = TheModule->getFunction("printf");
    if (!PrintfFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext),
                            {PointerType::getUnqual(*TheContext)}, true);
      PrintfFunc = Function::Create(FT, Function::ExternalLinkage, "printf",
                                    TheModule.get());
    }

    Type *Ty = Val->getType();
    if (Ty->isDoubleTy()) {
      Value *Fmt = Builder->CreateGlobalString("%g", "printstrdbl");
      return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
    } else if (Ty->isIntegerTy(64)) {
      Value *Fmt = Builder->CreateGlobalString("%lld", "printstrint");
      return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
    } else if (Ty->isIntegerTy(1)) {
      Value *Zext =
          Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext), "booltoint");
      Value *Fmt = Builder->CreateGlobalString("%d", "printstrbool");
      return Builder->CreateCall(PrintfFunc, {Fmt, Zext}, "printcall");
    } else if (Ty->isPointerTy()) {
      Value *Fmt = Builder->CreateGlobalString("%s", "printstrstr");
      return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
    }
    SemanticError(Loc, "Unsupported type for print").raise();
    return nullptr;
  }

  if (IntrinsicName == "printline") {
    Value *Val = Args[0];
    Function *PrintfFunc = TheModule->getFunction("printf");
    if (!PrintfFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext),
                            {PointerType::getUnqual(*TheContext)}, true);
      PrintfFunc = Function::Create(FT, Function::ExternalLinkage, "printf",
                                    TheModule.get());
    }

    Type *Ty = Val->getType();
    if (Ty->isDoubleTy()) {
      Value *Fmt = Builder->CreateGlobalString("%g\n", "printstrdbl");
      return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
    } else if (Ty->isIntegerTy(64)) {
      Value *Fmt = Builder->CreateGlobalString("%lld\n", "printstrint");
      return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
    } else if (Ty->isIntegerTy(1)) {
      Value *Zext =
          Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext), "booltoint");
      Value *Fmt = Builder->CreateGlobalString("%d\n", "printstrbool");
      return Builder->CreateCall(PrintfFunc, {Fmt, Zext}, "printcall");
    } else if (Ty->isPointerTy()) {
      Value *Fmt = Builder->CreateGlobalString("%s\n", "printstrstr");
      return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
    }
    SemanticError(Loc, "Unsupported type for printline").raise();
    return nullptr;
  }

  if (IntrinsicName == "input") {
    Function *CallocFunc = TheModule->getFunction("calloc");
    if (!CallocFunc) {
      FunctionType *FT = FunctionType::get(
          PointerType::getUnqual(*TheContext),
          {Type::getInt64Ty(*TheContext), Type::getInt64Ty(*TheContext)},
          false);
      CallocFunc = Function::Create(FT, Function::ExternalLinkage, "calloc",
                                    TheModule.get());
    }

    Function *ScanfFunc = TheModule->getFunction("scanf");
    if (!ScanfFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext),
                            {PointerType::getUnqual(*TheContext)}, true);
      ScanfFunc = Function::Create(FT, Function::ExternalLinkage, "scanf",
                                   TheModule.get());
    }

    Function *GetcharFunc = TheModule->getFunction("getchar");
    if (!GetcharFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext), false);
      GetcharFunc = Function::Create(FT, Function::ExternalLinkage, "getchar",
                                     TheModule.get());
    }

    Value *NumItems = ConstantInt::get(Type::getInt64Ty(*TheContext), 4096);
    Value *Size = ConstantInt::get(Type::getInt64Ty(*TheContext), 1);
    Value *Buf = Builder->CreateCall(CallocFunc, {NumItems, Size}, "buf");
    Value *Fmt = Builder->CreateGlobalString("%4095[^\n]", "scanfmt");
    Builder->CreateCall(ScanfFunc, {Fmt, Buf});
    Builder->CreateCall(GetcharFunc, {});
    return Buf;
  }

  if (IntrinsicName == "malloc") {
    Function *MallocFunc = TheModule->getFunction("malloc");
    if (!MallocFunc) {
      FunctionType *FT =
          FunctionType::get(PointerType::getUnqual(*TheContext),
                            {Type::getInt64Ty(*TheContext)}, false);
      MallocFunc = Function::Create(FT, Function::ExternalLinkage, "malloc",
                                    TheModule.get());
    }
    return Builder->CreateCall(MallocFunc, {Args[0]}, "malloc_call");
  }

  if (IntrinsicName == "realloc") {
    Function *ReallocFunc = TheModule->getFunction("realloc");
    if (!ReallocFunc) {
      FunctionType *FT = FunctionType::get(
          PointerType::getUnqual(*TheContext),
          {PointerType::getUnqual(*TheContext), Type::getInt64Ty(*TheContext)},
          false);
      ReallocFunc = Function::Create(FT, Function::ExternalLinkage, "realloc",
                                     TheModule.get());
    }
    return Builder->CreateCall(ReallocFunc, {Args[0], Args[1]}, "realloc_call");
  }

  if (IntrinsicName == "free") {
    Function *FreeFunc = TheModule->getFunction("free");
    if (!FreeFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getVoidTy(*TheContext),
                            {PointerType::getUnqual(*TheContext)}, false);
      FreeFunc = Function::Create(FT, Function::ExternalLinkage, "free",
                                  TheModule.get());
    }
    return Builder->CreateCall(FreeFunc, {Args[0]}, "");
  }

  if (IntrinsicName.find("llvm.") == 0) {
    Function *LLVMFunc = TheModule->getFunction(IntrinsicName);
    if (!LLVMFunc) {
      std::vector<Type *> ParamTypes;
      for (Value *Arg : Args) {
        ParamTypes.push_back(Arg->getType());
      }
      FunctionType *FT =
          FunctionType::get(Type::getDoubleTy(*TheContext), ParamTypes, false);
      LLVMFunc = Function::Create(FT, Function::ExternalLinkage, IntrinsicName,
                                  TheModule.get());
    }
    return Builder->CreateCall(LLVMFunc, Args, "llvm_call");
  }
  if (IntrinsicName == "power") {
    // Arity is already verified above. Type-check each argument.
    Value *A = Args[0];
    Value *B = Args[1];
    bool AIsDouble = A->getType()->isDoubleTy();
    bool BIsDouble = B->getType()->isDoubleTy();
    bool AIsInt    = A->getType()->isIntegerTy(64);
    bool BIsInt    = B->getType()->isIntegerTy(64);

    // Phase 1 fix: reject non-numeric types (bool, string/ptr, etc.)
    if (!AIsDouble && !AIsInt) {
      SemanticError(Loc,
                    "'power' first argument must be int or double, not '" +
                        std::string(A->getType()->isIntegerTy(1) ? "bool" : "non-numeric") +
                        "'")
          .raise();
      return nullptr;
    }
    if (!BIsDouble && !BIsInt) {
      SemanticError(Loc,
                    "'power' second argument must be int or double, not '" +
                        std::string(B->getType()->isIntegerTy(1) ? "bool" : "non-numeric") +
                        "'")
          .raise();
      return nullptr;
    }

    if (!AIsDouble)
      A = Builder->CreateSIToFP(A, Type::getDoubleTy(*TheContext), "powcast1");
    if (!BIsDouble)
      B = Builder->CreateSIToFP(B, Type::getDoubleTy(*TheContext), "powcast2");

    Function *PowFunc = llvm::Intrinsic::getOrInsertDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
    Value *Result = Builder->CreateCall(PowFunc, {A, B}, "powtmp");

    if (AIsInt && BIsInt)
      return Builder->CreateFPToSI(Result, Type::getInt64Ty(*TheContext),
                                   "powint");
    return Result;
  }

  if (IntrinsicName == "abs") {
    // Arity is already verified above.
    Value *Val = Args[0];
    if (Val->getType()->isDoubleTy()) {
        Function *FAbsFunc = llvm::Intrinsic::getOrInsertDeclaration(
          TheModule.get(), llvm::Intrinsic::fabs,
          {Type::getDoubleTy(*TheContext)});
      return Builder->CreateCall(FAbsFunc, {Val}, "abs");
    } else if (Val->getType()->isIntegerTy(64)) {
      // Phase 1 fix: explicit int-only path — no catch-all else.
      Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
      Value *Neg  = Builder->CreateNeg(Val, "absneg");
      Value *Cmp  = Builder->CreateICmpSLT(Val, Zero, "abscmp");
      return Builder->CreateSelect(Cmp, Neg, Val, "abs");
    } else {
      // Phase 1 fix: reject bool, string (ptr), or any other type.
      std::string TypeDesc = Val->getType()->isIntegerTy(1)
                                 ? "bool"
                                 : (Val->getType()->isPointerTy() ? "string/pointer"
                                                                  : "non-numeric");
      SemanticError(Loc,
                    "'abs' requires an int or double argument, not '" +
                        TypeDesc + "'")
          .raise();
      return nullptr;
    }
  }

  if (IntrinsicName == "lengthof") {
    Value *Val = Args[0];
    Type *Ty = Val->getType();
    if (!Ty->isPointerTy()) {
      SemanticError(Loc, "lengthof requires a string (pointer) type").raise();
      return nullptr;
    }
    Function *StrlenF = TheModule->getFunction("strlen");
    if (!StrlenF) {
      FunctionType *FT =
          FunctionType::get(Type::getInt64Ty(*TheContext),
                            {PointerType::getUnqual(*TheContext)}, false);
      StrlenF = Function::Create(FT, Function::ExternalLinkage, "strlen",
                                 TheModule.get());
    }
    return Builder->CreateCall(StrlenF, {Val}, "lengthof");
  }

  if (IntrinsicName == "typeof") {
    // Arity is already verified above (exactly 1 arg).
    // Defensive check retained as belt-and-suspenders.
    if (Args.empty()) {
      SemanticError(Loc, "'typeof' requires exactly 1 argument").raise();
      return nullptr;
    }
    Value *Val = Args[0];
    Type *Ty = Val->getType();
    const char *TypeName = nullptr;
    if (Ty->isDoubleTy()) {
      TypeName = "double";
    } else if (Ty->isIntegerTy(64)) {
      TypeName = "int";
    } else if (Ty->isIntegerTy(1)) {
      TypeName = "bool";
    } else if (Ty->isPointerTy()) {
      TypeName = "string";
    } else if (Ty->isVoidTy()) {
      TypeName = "void";
    } else {
      TypeName = "unknown";
    }
    return Builder->CreateGlobalString(TypeName, "typename");
  }

  if (IntrinsicName == "fail") {
    // Arg 0: message string
    Value *MsgVal = Args[0];

    // Get or declare fprintf
    Function *FPrintFFunc = TheModule->getFunction("fprintf");
    if (!FPrintFFunc) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext),
                            {PointerType::getUnqual(*TheContext), PointerType::getUnqual(*TheContext)}, true);
      FPrintFFunc = Function::Create(FT, Function::ExternalLinkage, "fprintf",
                                     TheModule.get());
    }

    // Get or declare abort
    Function *AbortFunc = TheModule->getFunction("abort");
    if (!AbortFunc) {
      FunctionType *FT = FunctionType::get(Type::getVoidTy(*TheContext), false);
      AbortFunc = Function::Create(FT, Function::ExternalLinkage, "abort", TheModule.get());
    }

    // Get stderr
    std::string StderrSymbol = "stderr";
#ifdef __APPLE__
    StderrSymbol = "__stderrp";
#endif
    GlobalVariable *StderrGV = TheModule->getNamedGlobal(StderrSymbol);
    if (!StderrGV) {
      StderrGV = new GlobalVariable(*TheModule, PointerType::getUnqual(*TheContext), false,
                                    GlobalValue::ExternalLinkage, nullptr, StderrSymbol);
    }
    Value *StderrVal = Builder->CreateLoad(PointerType::getUnqual(*TheContext), StderrGV, "stderr_val");

    // Print message to stderr and abort
    Value *Fmt = Builder->CreateGlobalString("%s\n", "failfmt");
    Builder->CreateCall(FPrintFFunc, {StderrVal, Fmt, MsgVal});
    Builder->CreateCall(AbortFunc, {});
    Builder->CreateUnreachable();

    // fail doesn't return, but we need to return something to satisfy the type signature
    // Any value will do since it's unreachable, we'll return a dummy integer
    return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
  }

  if (IntrinsicName == "assert") {
    // Arg 0: condition (bool)
    // Arg 1 (optional): message string
    Value *Cond = Args[0];
    
    // We check if it's already a boolean (i1), though TypeChecker enforces this
    if (!Cond->getType()->isIntegerTy(1)) {
        SemanticError(Loc, "'assert' condition must be a boolean").raise();
        return nullptr;
    }

    Value *MsgVal;
    if (Args.size() > 1) {
      MsgVal = Args[1];
    } else {
      MsgVal = Builder->CreateGlobalString("Oops! An assertion failed here.\nHint: Assertions are checks that make sure your code is running exactly as you expect. This one didn't pass.", "default_assert_msg");
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *FailBB = BasicBlock::Create(*TheContext, "assert.fail", TheFunction);
    BasicBlock *PassBB = BasicBlock::Create(*TheContext, "assert.pass", TheFunction);

    Builder->CreateCondBr(Cond, PassBB, FailBB);

    // Fail block
    Builder->SetInsertPoint(FailBB);
    std::vector<llvm::Value *> FailArgs = {MsgVal};
    CodegenIntrinsic("fail", FailArgs, Loc);
    // CodegenIntrinsic for fail already creates unreachable

    // Pass block
    Builder->SetInsertPoint(PassBB);

    return ConstantInt::get(Type::getInt64Ty(*TheContext), 1);
  }

  SemanticError(Loc, "Unknown intrinsic: " + IntrinsicName).raise();
  return nullptr;
}

