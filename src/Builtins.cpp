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

  // ── Removed: "malloc", "realloc", "free" ─────────────────────────────────
  // These were previously @intrinsic stubs, but are now declared as extern fn
  // in lib/std/mem.tr and resolved by the linker against libc. The compiler
  // no longer needs to know about them here.
  // Codegen.cpp still uses TheModule->getFunction("malloc") internally for
  // EmitStringConcat and CastExprAST (int/double → string) — those are
  // compiler-emitted IR, not user-facing stdlib calls, and correctly remain.

  // ── Removed: "llvm.*" ────────────────────────────────────────────────────
  // LLVM intrinsic math functions (llvm.sqrt.f64, llvm.pow.f64, etc.) were
  // previously forwarded here. They are now replaced by extern fn declarations
  // in lib/std/math.tr that link against libm (e.g. sqrt, pow from <math.h>).
  // ── Removed: "power", "abs" ───────────────────────────────────────────────
  // Both are now implemented in Turf in lib/std/math.tr.
  // math.power() delegates to extern fn pow (libm).
  // math.abs()   delegates to extern fn fabs for doubles; Turf conditional for int.
  // No compiler C++ needed.

  // Defensive Args.size() checks below (lengthof/fail/assert): these three,
  // together with typeof above, are the only names reachable through the
  // "bare intrinsic" path -- called with no module qualifier, arity/type
  // validated by TypeChecker's ad-hoc BuiltinInfo registry rather than by
  // real overload resolution against a fixed-arity FunctionSymbol (compare
  // print/printline/input, which are only ever reached via a real,
  // fully-typed `@intrinsic`-decorated Turf function in lib/std/io.tr, so
  // Args here is already guaranteed correct by the normal call machinery).
  // That ad-hoc registry isn't a reliable enough guarantee to skip
  // bounds-checking on: main.cpp's pre-pass (sub-pass 2b) runs its own
  // TypeChecker instance but deliberately discards its errors
  // ("informational only... main-pass TC will re-check"), then sub-pass 3b
  // unconditionally codegens the same AST anyway. A wrong-arity call like a
  // bare `assert();` or `fail();` used to hit an unchecked Args[0] and
  // crash the compiler (SIGABRT) via the pre-pass, before the user ever saw
  // a diagnostic -- see TypeChecker.cpp's visitFuncCall doc comment on the
  // assert/fail branches for the full story and the TypeChecker-side half
  // of this fix (assert's arity was also never validated at all: it's
  // registered with BuiltinInfo::Arity == -1 since it takes 1 or 2 args,
  // which disabled the generic single-arity check entirely).
  if (IntrinsicName == "lengthof") {
    if (Args.empty()) {
      SemanticError(Loc, "'lengthof' requires exactly 1 argument").raise();
      return nullptr;
    }
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
    if (Args.empty()) {
      SemanticError(Loc, "'fail' requires exactly 1 argument").raise();
      return nullptr;
    }
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
    if (Args.empty()) {
      SemanticError(Loc, "'assert' requires at least 1 argument (a condition)").raise();
      return nullptr;
    }
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

