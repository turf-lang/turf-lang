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

// NOTE (Issue #10): The IntrinsicArity table has been removed.
// Arity checking for all intrinsics is now performed by the TypeChecker
// (Issue #8) before Codegen runs. Codegen trusts the TypeChecker and emits
// LLVM IR blindly.

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
  // Register the canonical builtin names used by the compiler. Keep the
  // Arity information when known; -1 means variadic/unknown.
  RegisterBuiltin("print", {1, "Print a value"});
  RegisterBuiltin("printline", {1, "Print a value with newline"});
  RegisterBuiltin("input", {0, "Read a line from stdin"});
  RegisterBuiltin("malloc", {1, "Allocate memory"});
  RegisterBuiltin("realloc", {2, "Reallocate memory"});
  RegisterBuiltin("free", {1, "Free memory"});
  RegisterBuiltin("power", {2, "Power operation"});
  RegisterBuiltin("abs", {1, "Absolute value"});
  RegisterBuiltin("lengthof", {1, "Length of string"});
  RegisterBuiltin("typeof", {1, "Type of value"});
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
 * @note (Issue #10) Arity and type validation is no longer performed here.
 * The TypeChecker (Issue #8) guarantees correct arity and argument types
 * before this function is called. Codegen emits LLVM IR blindly.
 */
llvm::Value *CodegenIntrinsic(const std::string &IntrinsicName,
                              std::vector<llvm::Value *> &Args,
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
    // TypeChecker guarantees the argument is a known printable type.
    // Reaching here is an internal compiler bug.
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
    // TypeChecker guarantees the argument is a known printable type.
    // Reaching here is an internal compiler bug.
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
    // TypeChecker (Issue #8) guarantees both arguments are numeric (int or
    // double) before Codegen runs. Emit IR directly, trusting the types.
    Value *A = Args[0];
    Value *B = Args[1];
    bool AIsDouble = A->getType()->isDoubleTy();
    bool BIsDouble = B->getType()->isDoubleTy();
    bool AIsInt    = A->getType()->isIntegerTy(64);
    bool BIsInt    = B->getType()->isIntegerTy(64);

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
    // TypeChecker (Issue #8) guarantees the argument is int or double.
    Value *Val = Args[0];
    if (Val->getType()->isDoubleTy()) {
      Function *FAbsFunc = llvm::Intrinsic::getOrInsertDeclaration(
        TheModule.get(), llvm::Intrinsic::fabs,
        {Type::getDoubleTy(*TheContext)});
      return Builder->CreateCall(FAbsFunc, {Val}, "abs");
    }
    // int (i64) path — TypeChecker ensures we only reach here for numeric types.
    Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
    Value *Neg  = Builder->CreateNeg(Val, "absneg");
    Value *Cmp  = Builder->CreateICmpSLT(Val, Zero, "abscmp");
    return Builder->CreateSelect(Cmp, Neg, Val, "abs");
  }

  if (IntrinsicName == "lengthof") {
    // TypeChecker (Issue #8) guarantees the argument is a string (pointer).
    Value *Val = Args[0];
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
    // TypeChecker (Issue #8) guarantees arity == 1 before we reach here.
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

  SemanticError(Loc, "Unknown intrinsic: " + IntrinsicName).raise();
  return nullptr;
}


