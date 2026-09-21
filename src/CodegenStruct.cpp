/**
 * @file CodegenStruct.cpp
 * @brief Struct/enum/module codegen: struct decl, field access, method calls, scope access.
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
#include "Monomorphize.h" // Monomorphize::instantiatedDeclName() -- FieldAccessExprAST's per-instantiation lookup, TYPE-002 S13b
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;


// NOTE: ListDeclExprAST has been removed. List declarations are now
// represented as VarDeclExprAST with a GenericType("list", [elemType]).
// The VarDeclExprAST::codegen handles list initialization via the type system.

Value *MethodCallExprAST::codegen() {
  // ── Machine transition (conn.transition(ConnectionState::X)) ────────────
  // TypeChecker::visitMethodCall() already confirmed IsMachineTransition
  // only when the receiver is TurfMachineType-typed, Method == "transition",
  // and Args[0] resolves to a state of the SAME machine -- Codegen doesn't
  // re-verify any of that, only picks a lowering strategy.
  if (IsMachineTransition) {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    Value *TargetVal = Args[0]->codegen(); // inlined ordinal constant
    if (!TargetVal)
      return nullptr;

    Value *Ptr = Base->codegenLValue();
    if (!Ptr)
      return nullptr;

    if (TransitionCompileTimeProven) {
      // TypeChecker's compare-arm narrowing already proved this specific
      // edge exists -- a plain store, zero runtime cost, no switch needed.
      // Returns a non-null sentinel, NOT a literal nullptr -- this is a
      // successful void-statement codegen, and a literal nullptr here reads
      // as "codegen failed" to every blind-iteration caller (most
      // importantly BlockExprAST::codegen(), which stops processing the
      // rest of the block the moment any statement returns nullptr -- see
      // its own doc comment and CodegenVar.cpp's/CodegenFunc.cpp's matching
      // ones for the exact same convention on other void statements).
      Builder->CreateStore(TargetVal, Ptr);
      return Constant::getNullValue(Type::getInt64Ty(*TheContext));
    }

    // Unproven: runtime-guard via a switch over the CURRENT value, one case
    // per declared source state that has an edge to the target, default =
    // panic. Mirrors CompareExprAST::codegen()'s int/enum switch and
    // EmitRuntimeBoundsCheck()'s panic-on-default pattern (CodegenArray.cpp).
    const TurfType *BaseTy = getExprType(Base.get());
    if (auto *NT = dynamic_cast<const NamedType *>(BaseTy)) {
      if (GlobalSymbolTable)
        BaseTy = GlobalSymbolTable->LookupType(NT->getName()).get();
    }
    const auto *MT = dynamic_cast<const TurfMachineType *>(BaseTy);
    if (!MT) {
      llvm::report_fatal_error(
          "Internal error: machine transition receiver lost its machine "
          "type in codegen. This is a compiler bug.");
      return nullptr;
    }

    const auto *TargetAccess =
        dynamic_cast<const ScopeAccessExprAST *>(Args[0].get());
    std::string TargetState = TargetAccess ? TargetAccess->getRightName() : "";

    Value *CurVal = Base->codegen();
    if (!CurVal)
      return nullptr;

    unsigned NumCases = 0;
    for (const auto &[From, Tos] : MT->getEdges())
      if (Tos.count(TargetState))
        ++NumCases;

    BasicBlock *OkBB = BasicBlock::Create(*TheContext, "transition.ok", TheFunction);
    BasicBlock *InvalidBB =
        BasicBlock::Create(*TheContext, "transition.invalid", TheFunction);
    // Created without a parent (like EmitRuntimeBoundsCheck's ContBB) and
    // inserted explicitly right before use, so it lands after the panic
    // block in the function's layout instead of interleaved.
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "transition.merge");

    SwitchInst *SI = Builder->CreateSwitch(CurVal, InvalidBB, NumCases);
    for (const auto &[From, Tos] : MT->getEdges()) {
      if (Tos.count(TargetState)) {
        SI->addCase(
            ConstantInt::get(cast<IntegerType>(CurVal->getType()),
                             MT->getVariantValue(From)),
            OkBB);
      }
    }

    Builder->SetInsertPoint(OkBB);
    Builder->CreateStore(TargetVal, Ptr);
    Builder->CreateBr(MergeBB);

    Builder->SetInsertPoint(InvalidBB);
    Value *MsgVal = Builder->CreateGlobalString(
        MT->getName() + "::transition(" + TargetState +
            ") -- no such transition from the current state.",
        "panicmsg");
    CodegenPanic(MsgVal, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    // Same non-null-sentinel convention as the proven branch above -- a
    // literal nullptr here would look like a codegen failure to
    // BlockExprAST::codegen() and silently truncate every statement after
    // this one.
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  if (auto *Var = dynamic_cast<VariableExprAST *>(Base.get())) {
    // Trust the Resolver's ResolvedModuleName verdict -- set only when the
    // base name is a genuine module qualifier AND is not shadowed by a
    // locally-declared variable of the same name (see
    // Resolver::visitMethodCall) -- rather than independently re-deriving
    // "is this a module call" from the raw variable name here.
    //
    // The raw-name check (GlobalSymbolTable->HasModule(Var->getName())) used
    // to live here instead, and it doesn't know about local variables at
    // all: a variable that happens to share a name with an imported module
    // (concretely, `io` -- auto-imported into EVERY Turf program via
    // lib/std/prelude.tr) would take this branch even though TypeChecker had
    // already set ResolvedCallTarget to the STRUCT method instead. That
    // mismatch was a real, reproduced compiler crash, not just a wrong
    // diagnostic: a few lines below, `ArgVals[Idx++]` walked past the end of
    // an empty ArgVals (the module path assumed zero explicit args) against
    // CalleeF's real argument count (the struct method's self parameter),
    // an out-of-bounds std::vector access that aborted the process.
    const std::string &ModuleName = Var->ResolvedModuleName;
    bool IsModule = !ModuleName.empty();
    if (IsModule) {
      // Codegen all arguments.
      std::vector<Value *> ArgVals;
      for (auto &Arg : Args) {
        Value *V = Arg->codegen();
        if (!V) return nullptr;
        ArgVals.push_back(V);
      }

      // ResolvedCallTarget is set by the TypeChecker during overload resolution.
      // If it's null the TypeChecker already emitted a type-mismatch error; just
      // propagate nullptr so Codegen can bail out cleanly.
      if (!ResolvedCallTarget) {
        LogErrorAt(Loc, "No matching overload found for '" + ModuleName + "." +
                            Method + "' (see type-mismatch error above)", SLM::ErrorCategory::FUNCTION_SIGNATURE);
        return nullptr;
      }
      std::string MangledName = ResolvedCallTarget->mangledName();

      llvm::Function *CalleeF = TheModule->getFunction(MangledName);
      // If the function isn't in the LLVM module, that's a compiler bug —
      // the prototype pre-pass should have emitted it.
      if (!CalleeF) {
        llvm::report_fatal_error(
            llvm::StringRef("Internal error: LLVM function '" + MangledName +
            "' not found after successful type-checking. This is a compiler bug."));
        return nullptr;
      }

      // Cast arguments to match callee signature (numeric promotions only).
      std::vector<Value *> CastVals;
      unsigned Idx = 0;
      for (auto &FArg : CalleeF->args()) {
        Value *V = ArgVals[Idx++];
        llvm::Type *ExpTy = FArg.getType();
        if (V->getType() != ExpTy) {
          if (ExpTy->isDoubleTy() && V->getType()->isIntegerTy(64))
            V = Builder->CreateSIToFP(V, ExpTy, "argcast");
          else if (ExpTy->isDoubleTy() && V->getType()->isIntegerTy(1))
            V = Builder->CreateUIToFP(V, ExpTy, "argcast");
          else if (ExpTy->isIntegerTy(64) && V->getType()->isDoubleTy())
            V = Builder->CreateFPToSI(V, ExpTy, "argcast");
          else if (ExpTy->isIntegerTy(64) && V->getType()->isIntegerTy(1))
            V = Builder->CreateZExt(V, ExpTy, "argcast");
          else if (ExpTy->isIntegerTy(1) && V->getType()->isIntegerTy(64))
            V = Builder->CreateICmpNE(
                    V, ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                    "argcast");
          // pointer <-> integer / pointer passthrough handled by LLVM
        }
        CastVals.push_back(V);
      }

      // void functions must not be given a name (LLVM requirement) -- an
      // instruction that produces no value can't have a name bound to it.
      // Verified: this was previously unconditional, producing technically
      // invalid IR for every void-returning module call (e.g.
      // io.printline(...)) that `verifyModule()` (see main.cpp) now catches
      // -- LLVM's optimizer/backend evidently tolerated it silently before,
      // but it was never valid.
      if (CalleeF->getReturnType()->isVoidTy())
        return Builder->CreateCall(CalleeF, CastVals);
      return Builder->CreateCall(CalleeF, CastVals, "modcalltmp");
    }
  }

  // ── UFCS call ─────────────────────────────────────────────────────────────
  // TypeChecker::tryUFCSFallback() resolved this dot-call to an ordinary
  // free function taking the receiver as its first argument (ResolvedViaUFCS
  // -- STRING_UFCS_OPERATOR_CHAR_RFC.md §5.5: no AST desugaring, Codegen
  // just needs to know which calling convention to use). This is a real
  // free function, not a bound method, so it uses the SAME by-value
  // argument-passing shape as the module-call branch above -- NOT the
  // struct-method branch's self-pointer convention below, which only
  // applies to genuine `fn StructName.method(...)` declarations.
  if (ResolvedViaUFCS) {
    if (!ResolvedCallTarget) {
      llvm::report_fatal_error(
          "Internal error: ResolvedViaUFCS set with no ResolvedCallTarget. "
          "This is a compiler bug.");
      return nullptr;
    }
    // Unsized-array receiver (`fn sum(arr: int[]): int` called as
    // `arr.sum()`): mirrors FuncCallExprAST::codegen()'s identical special
    // case for ordinary calls -- the callee's real LLVM parameter type is an
    // opaque pointer (getLLVMType()'s unsized-array branch), not the
    // aggregate array value Base->codegen() would load. Passing the loaded
    // aggregate here would build an ill-typed llvm::CallInst (wrong argument
    // type for the callee's declared parameter). TypeChecker's
    // ufcsReceiverTypeMatches() only accepts a receiver whose base is
    // array-typed to begin with, and UFCS call syntax's Base is always
    // itself an addressable expression (a variable, field, or index), so
    // codegenLValue() here has the exact same safety guarantee ordinary
    // calls already rely on.
    Value *ReceiverVal;
    const TurfType *ReceiverParamTy =
        !ResolvedCallTarget->Params.empty()
            ? ResolvedCallTarget->Params[0].Type.get()
            : nullptr;
    const auto *ReceiverArrTy =
        ReceiverParamTy ? dynamic_cast<const ::ArrayType *>(ReceiverParamTy)
                         : nullptr;
    if (ReceiverArrTy && ReceiverArrTy->getSize() == -1) {
      ReceiverVal = Base->codegenLValue();
      if (!ReceiverVal) {
        llvm::report_fatal_error(
            "Internal error: UFCS array receiver must be addressable. "
            "This should have been caught by the TypeChecker.");
        return nullptr;
      }
    } else {
      ReceiverVal = Base->codegen();
      if (!ReceiverVal)
        return nullptr;
    }
    std::vector<Value *> ArgVals;
    ArgVals.push_back(ReceiverVal);
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V)
        return nullptr;
      ArgVals.push_back(V);
    }

    std::string MangledName = ResolvedCallTarget->mangledName();
    llvm::Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: LLVM function '" + MangledName +
          "' not found after successful UFCS resolution. This is a compiler bug."));
      return nullptr;
    }

    // Cast arguments to match callee signature (numeric promotions only) --
    // identical shape to the module-call branch's own cast chain above,
    // since a UFCS callee is an ordinary free function exactly like a
    // module function is.
    std::vector<Value *> CastVals;
    unsigned Idx = 0;
    for (auto &FArg : CalleeF->args()) {
      Value *V = ArgVals[Idx++];
      llvm::Type *ExpTy = FArg.getType();
      if (V->getType() != ExpTy) {
        if (ExpTy->isDoubleTy() && V->getType()->isIntegerTy(64))
          V = Builder->CreateSIToFP(V, ExpTy, "argcast");
        else if (ExpTy->isDoubleTy() && V->getType()->isIntegerTy(1))
          V = Builder->CreateUIToFP(V, ExpTy, "argcast");
        else if (ExpTy->isIntegerTy(64) && V->getType()->isDoubleTy())
          V = Builder->CreateFPToSI(V, ExpTy, "argcast");
        else if (ExpTy->isIntegerTy(64) && V->getType()->isIntegerTy(1))
          V = Builder->CreateZExt(V, ExpTy, "argcast");
        else if (ExpTy->isIntegerTy(1) && V->getType()->isIntegerTy(64))
          V = Builder->CreateICmpNE(
                  V, ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                  "argcast");
        // pointer <-> integer / pointer passthrough handled by LLVM
      }
      CastVals.push_back(V);
    }

    if (CalleeF->getReturnType()->isVoidTy())
      return Builder->CreateCall(CalleeF, CastVals);
    return Builder->CreateCall(CalleeF, CastVals, "ufcscalltmp");
  }

  // ── Struct / pointer method call ─────────────────────────────────────────
  // Base is a struct instance or pointer-to-struct.  ResolvedCallTarget is set
  // by the TypeChecker.  We just emit the call.
  const TurfType *BaseTy = getExprType(Base.get());
  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
    BaseTy = PtrTy->getPointee().get();

  if (!BaseTy) {
    // TypeChecker should have caught this — reaching here is a compiler bug.
    llvm::report_fatal_error(
        "Internal error: could not determine type of method call base. "
        "This is a compiler bug.");
    return nullptr;
  }

  if (Method == "length") {
    if (isArrayType(BaseTy)) {
      int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
      return ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize);
    }
    // For non-array types (e.g. list<T>), fall through to struct method dispatch.
  }

  std::string StructNameStr;
  // Resolve NamedType → TurfStructType, same as FieldAccessExprAST::codegen().
  // Without this, `expr.method()` where expr has type NamedType("T") will not
  // find the struct name and falls through to the fatal error below.
  if (auto *NT = dynamic_cast<const NamedType *>(BaseTy)) {
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        BaseTy = R.get();
  }
  if (auto *StructTy = dynamic_cast<const TurfStructType *>(BaseTy)) {
    StructNameStr = StructTy->getName();
  } else if (auto *GenTy = dynamic_cast<const GenericType *>(BaseTy)) {
    StructNameStr = GenTy->getBaseName();
  }

  if (!StructNameStr.empty()) {
    std::vector<Value *> EvaluatedArgs;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V)
        return nullptr;
      EvaluatedArgs.push_back(V);
    }

    std::string MangledName;
    if (ResolvedCallTarget) {
      MangledName = ResolvedCallTarget->mangledName();
    }

    Function *CalleeF = nullptr;
    if (!MangledName.empty()) {
      CalleeF = TheModule->getFunction(MangledName);
    }

    if (!CalleeF) {
      return nullptr;
    }

    bool IsStaticMethod = false;
    if (auto *Var = dynamic_cast<const VariableExprAST*>(Base.get())) {
      IsStaticMethod = Var->IsStructName;
    }

    // Arity is guaranteed by the TypeChecker — if it's wrong here, it's a bug.
    int ExpectedArity = IsStaticMethod ? Args.size() : (Args.size() + 1);
    assert(CalleeF->arg_size() == ExpectedArity &&
           "Struct method arity mismatch after TypeChecker — compiler bug");

    std::vector<Value *> ArgVals;
    if (!IsStaticMethod) {
      Value *SelfVal = Base->codegenLValue();
      if (!SelfVal) {
        // If Base is an rvalue (like User(10)), we need a temporary alloca to
        // pass by reference -- UNLESS Base's r-value IS ALREADY a pointer
        // (e.g. a chained call to a method that itself returns `self` as
        // Counter*, like `c.increment().increment()`). In that case the
        // pointer value itself is already what `self` should receive:
        // wrapping it in another alloca and passing THAT alloca's address
        // introduces a spurious extra level of indirection (self would
        // become Counter** instead of Counter*), so the callee's
        // `self.val = ...` writes end up mutating the temporary alloca
        // (which merely holds a copy of the real pointer) instead of the
        // real struct. Confirmed via a chained `.increment()` test: without
        // this check, only the first call in a chain actually mutated the
        // original struct -- every subsequent chained call silently wrote
        // to a throwaway temporary instead.
        Value *Val = Base->codegen();
        if (!Val)
          return nullptr;
        if (dynamic_cast<const TurfPointerType *>(getExprType(Base.get()))) {
          SelfVal = Val;
        } else {
          SelfVal = Builder->CreateAlloca(Val->getType(), nullptr, "tmp.self");
          Builder->CreateStore(Val, SelfVal);
        }
      } else if (dynamic_cast<const TurfPointerType *>(getExprType(Base.get()))) {
        SelfVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0),
                                      SelfVal, "deref");
      }
      if (!SelfVal)
        return nullptr;
      ArgVals.push_back(SelfVal);
    }

    unsigned Idx = 0;
    auto ArgIt = IsStaticMethod ? CalleeF->arg_begin() : std::next(CalleeF->arg_begin());
    for (; ArgIt != CalleeF->arg_end(); ++ArgIt, ++Idx) {
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
        }
        // The reverse direction (erasing a `double` argument INTO an opaque
        // generic-parameter slot, e.g. calling Map<K,V>.put(key, 1.5) where
        // V is type-erased to `ptr`) was missing entirely -- confirmed via a
        // real crash: `bitcast double to ptr` is illegal LLVM IR (bitcast
        // never converts between float and pointer types, only same-size
        // int-like/pointer-like ones), which the catch-all `else` branch
        // below produced unconditionally for this combination. Fixed by
        // mirroring CastToType()'s own generic-erasure branch exactly:
        // double -> i64 (bitcast, same width) -> ptr (inttoptr).
        else if (ExpectedTy->isPointerTy() && V->getType()->isDoubleTy()) {
          V = Builder->CreateBitCast(V, Type::getInt64Ty(*TheContext), "argcast_dbl_to_i64");
          V = Builder->CreateIntToPtr(V, ExpectedTy, "argcast_erase_dbl");
        }
        else
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_bit");
      }
      ArgVals.push_back(V);
    }

    // void functions must not be given a name (LLVM requirement) -- see the
    // matching fix/comment on the module-call path above for the full story.
    if (CalleeF->getReturnType()->isVoidTy())
      return Builder->CreateCall(CalleeF, ArgVals);
    return Builder->CreateCall(CalleeF, ArgVals, "methodcalltmp");
  }

  // TypeChecker should have rejected a method call on an unsupported type.
  std::cerr << "BaseTy: " << (BaseTy ? BaseTy->getName() : "null") << "\n";
  llvm::report_fatal_error(
      "Internal error: method call on unsupported type. This is a compiler bug.");
  return nullptr;
}


Value *EnumDeclExprAST::codegen() {
  return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
}


// A `machine` declaration generates no LLVM type/global of its own, exactly
// like EnumDeclExprAST just above -- every use site (MachineName::State)
// inlines the state's ordinal as a plain constant via
// ScopeAccessExprAST::codegen() below, which works unchanged on a
// TurfMachineType since it IS-A TurfEnumType (see include/Types.h).
Value *MachineDeclExprAST::codegen() {
  return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
}


// Only ever reached for the EnumVariant meaning of a ScopeAccessExprAST
// (Color::Red used as a value) -- the Resolver (visitScopeAccess) already
// rejects a module-function-ref used outside call position with a clean
// TypeError before Codegen ever runs, and a module-function-ref IN call
// position is handled entirely by FuncCallExprAST::codegen() instead
// (which never calls Callee->codegen() at all -- a bare/qualified function
// name has no "value" to compute, only a lookup).
Value *ScopeAccessExprAST::codegen() {
  // module::CONST -- an `export immut` constant, e.g. `math::PI`. Checked
  // first since every other branch below assumes LeftName names an enum.
  if (ResolvedKind == ScopeAccessKind::ModuleConstantRef) {
    if (!ResolvedConstant) {
      LogErrorAt(Loc, "Internal error: exported constant not resolved in codegen", SLM::ErrorCategory::GENERAL);
      return nullptr;
    }
    return CodegenConstantValue(*ResolvedConstant);
  }

  // `ErrorType::Variant` (docs/architecture/errors_and_recover.md) -- always a
  // bare, non-callable reference (variants have no payload, see
  // BuildErrorTag()'s doc comment). Returns the (error type, variant)'s tag
  // string.
  if (ResolvedKind == ScopeAccessKind::ErrorVariantRef) {
    return BuildErrorTag(LeftName, RightName);
  }

  auto EnumTy = GlobalSymbolTable->LookupType(LeftName);
  auto *ET = dynamic_cast<const TurfEnumType *>(EnumTy.get());
  if (!ET) {
    LogErrorAt(Loc, "Internal error: enum type not found in codegen", SLM::ErrorCategory::GENERAL);
    return nullptr;
  }

  if (RightName == "COUNT") {
    return ConstantInt::get(Type::getInt64Ty(*TheContext), ET->numVariants());
  }

  if (RightName == "VALUES") {
    llvm::Type *UnderlyingLLVMTy = getLLVMType(ET->getUnderlyingType());
    std::vector<Constant*> ArrayVals;
    for (const auto& V : ET->getVariants()) {
      ArrayVals.push_back(ConstantInt::get(UnderlyingLLVMTy, ET->getVariantValue(V)));
    }
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(UnderlyingLLVMTy, ArrayVals.size());
    Constant *ArrConst = ConstantArray::get(ArrTy, ArrayVals);
    GlobalVariable *GV = new GlobalVariable(*TheModule, ArrTy, true,
                                            GlobalValue::PrivateLinkage, ArrConst, LeftName + ".values");
    Constant *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    Constant *Indices[] = {Zero, Zero};
    return ConstantExpr::getInBoundsGetElementPtr(ArrTy, GV, Indices);
  }

  if (RightName == "NAMES") {
    std::vector<Constant*> ArrayVals;
    llvm::Type *StringTy = llvm::PointerType::getUnqual(*TheContext);
    for (const auto& V : ET->getVariants()) {
      Constant *StrConstant = llvm::ConstantDataArray::getString(*TheContext, V, true);
      GlobalVariable *StrGV = new GlobalVariable(*TheModule, StrConstant->getType(), true,
                                                 GlobalValue::PrivateLinkage, StrConstant, ".str");
      Constant *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
      Constant *Indices[] = {Zero, Zero};
      ArrayVals.push_back(ConstantExpr::getInBoundsGetElementPtr(StrConstant->getType(), StrGV, Indices));
    }
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(StringTy, ArrayVals.size());
    Constant *ArrConst = ConstantArray::get(ArrTy, ArrayVals);
    GlobalVariable *GV = new GlobalVariable(*TheModule, ArrTy, true,
                                            GlobalValue::PrivateLinkage, ArrConst, LeftName + ".names");
    Constant *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    Constant *Indices[] = {Zero, Zero};
    return ConstantExpr::getInBoundsGetElementPtr(ArrTy, GV, Indices);
  }

  llvm::Type *LLVMTy = getLLVMType(ET->getUnderlyingType());
  return ConstantInt::get(LLVMTy, ET->getVariantValue(RightName));
}


Value *StructDeclExprAST::codegen() {
  // ── Fix for issue #176: Two-step struct creation ──────────────────────────
  // Step 1: Create (or reuse) an opaque StructType and register it in
  // LLVMTypeCache *before* evaluating any field types.  This ensures that a
  // self-referential field like `next: Node*` can call getLLVMType("Node")
  // and find the opaque stub in the cache rather than triggering infinite
  // recursion or creating a duplicate struct (which LLVM would rename to
  // "Node.0", breaking all previously-built call/alloca sites).
  //
  // This function CAN be called more than once for the same struct -- see
  // the early-return guard right below for why that's a real, reachable
  // case (not hypothetical) and how it's made safe. main.cpp's pre-pass
  // sub-pass 2c still tries to codegen every TOP-LEVEL struct exactly once
  // up front, before any function prototype or body, for forward-reference
  // reasons (see its own doc comment) -- but a struct declared inside an
  // IMPORTED MODULE is invisible to that top-level-only scan, and instead
  // gets (re-)built as a side effect of ModuleImportExprAST::codegen()
  // walking the whole module body, which main.cpp's pre-pass invokes twice
  // (sub-pass 3a's prototype walk, then sub-pass 3b's body walk).
  StructType *ST = nullptr;
  auto CacheIt = LLVMTypeCache.find(Name);
  if (CacheIt != LLVMTypeCache.end()) {
    if (auto *Existing = dyn_cast<StructType>(CacheIt->second)) {
      if (!Existing->isOpaque()) {
        // Already fully built (setBody() ran on a PREVIOUS call to this
        // function for the same struct) -- return immediately instead of
        // falling through to the `!ST` branch below, which would create a
        // SECOND, distinct llvm::StructType under the same name (LLVM
        // silently renames it "Name.0") and corrupt LLVMTypeCache for
        // every already-built reference to the original. This guard is
        // NOT just defensive: a struct declared inside an IMPORTED
        // MODULE genuinely gets its ModuleImportExprAST::codegen() call
        // -- which walks the whole module body, including this node --
        // invoked twice by main.cpp's pre-pass (once in sub-pass 3a's
        // prototype walk, again in sub-pass 3b's body walk), so this is
        // a real, reachable path, not a hypothetical one. Confirmed live:
        // a module declaring a struct AND a free function that returns it
        // by value crashed the whole compiler ("ret %Point.0 %structval"
        // against a function signature still declaring %Point) before
        // this guard existed, because the corrupted SECOND StructType
        // landed in the cache in between the function's prototype being
        // declared (sub-pass 3a, using the original %Point) and its body
        // being codegenned (sub-pass 3b, using the corrupted %Point.0
        // read back out of the cache for the return-value construction).
        return Constant::getNullValue(Type::getInt64Ty(*TheContext));
      }
      // An opaque forward-declaration was already inserted (e.g., because a
      // NamedType("Node") appeared in another struct or function signature
      // before this decl ran).  Reuse it so the single LLVM type identity is
      // preserved everywhere it was referenced.
      ST = Existing;
    }
  }
  if (!ST) {
    // Brand-new opaque struct — no body yet.
    ST = StructType::create(*TheContext, Name);
    if (!ST) {
      LogErrorAt(Loc, "Failed to create LLVM StructType for '" + Name + "'", SLM::ErrorCategory::GENERAL);
      return nullptr;
    }
  }

  // Register immediately so that any recursive getLLVMType call (e.g., for a
  // Node* field) finds the opaque stub instead of recursing back here.
  LLVMTypeCache[Name] = ST;

  // Step 2: Evaluate all field LLVM types now that the stub is in the cache.
  // Self-referential pointer fields (TurfPointerType) lower to opaque ptr
  // unconditionally, so they never need to re-enter this codegen path.
  // Direct struct-valued fields (unusual but possible for non-recursive types)
  // will find the opaque stub above and use it.
  std::vector<Type *> LLVMFieldTypes;
  for (const auto &Field : Fields) {
    Type *LLVMType = getLLVMType(Field.Type.get());
    if (!LLVMType) {
      LogErrorAt(Loc, "Unknown type for field '" + Field.Name + "'", SLM::ErrorCategory::GENERAL);
      return nullptr;
    }
    LLVMFieldTypes.push_back(LLVMType);
  }

  // Step 3: Fill in the body now that all field types are resolved.
  ST->setBody(LLVMFieldTypes);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}


Value *FieldAccessExprAST::codegenLValue() {
  Value *BaseLVal = Base->codegenLValue();
  if (!BaseLVal) {
    // Base is not itself addressable -- e.g. a function-call result
    // (`makeBox().val`), a method-call result, or any other r-value-only
    // expression. Without this fallback, this used to silently return
    // nullptr with NO diagnostic anywhere: every caller up the chain
    // (VarDeclExprAST::codegen(), MethodCallExprAST arg evaluation, etc.)
    // treats a null Value* as "an error was already reported, propagate
    // silently" -- so `io.printline(makeBox().val)` used to compile clean
    // and simply never execute, and `mut p: int = makeBox().val;` used to
    // leave `p` completely unregistered, surfacing only as a confusing
    // "undeclared variable" error at the NEXT use of `p`, nowhere near the
    // real cause.
    //
    // Mirrors the identical fallback ArrayAccessExprAST::codegen() and
    // MethodCallExprAST::codegen() already use for the same r-value-base
    // scenario: evaluate the base as an r-value, spill it into a fresh
    // stack slot, and GEP into that instead. This is correct uniformly for
    // both a direct struct-valued base (spill the struct value; the
    // TurfStructType branch below GEPs into it directly) and a pointer-
    // typed base (spill the pointer value; the TurfPointerType branch below
    // already expects BaseLVal to be "address of the pointer" and loads
    // through it once, exactly as it does for an ordinary addressable
    // pointer variable).
    Value *BaseVal = Base->codegen();
    if (!BaseVal)
      return nullptr;
    BaseLVal = Builder->CreateAlloca(BaseVal->getType(), nullptr, "tmp.fieldbase");
    Builder->CreateStore(BaseVal, BaseLVal);
  }

  const TurfType *BaseType = getExprType(Base.get());

  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
    BaseLVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0),
                                   BaseLVal, "deref");
  }
  // Resolve NamedType base to TurfStructType for chained field access.
  if (auto *NT = dynamic_cast<const NamedType *>(BaseType)) {
    if (GlobalSymbolTable) {
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        BaseType = R.get();
    }
  }
  // Resolve a GenericType base (e.g. a `Box<int>`-typed variable) to its
  // OWN per-instantiation TurfStructType, via its mangled instantiation
  // name -- TYPE-002 (S13b): this REPLACES the erasure-era base-name
  // collapse (every instantiation used to share ONE registered
  // TurfStructType, correct only because Type Erasure made every T/K/V a
  // uniform 8-byte slot). drainStructRequests() (S13a) registers a real,
  // distinct TurfStructType per instantiation in GlobalSymbolTable under
  // exactly this mangled name -- see the change record's *Removed
  // assumptions* for the two real bugs found live wiring this up (a
  // FieldAccessExprAST that never went through a VarDecl or method call
  // needed its OWN instantiation-request recording site, TypeCheckerCall.cpp's
  // visitFieldAccess).
  if (auto *GT = dynamic_cast<const GenericType *>(BaseType)) {
    if (GlobalSymbolTable) {
      std::string MangledName =
          Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs());
      if (auto R = GlobalSymbolTable->LookupType(MangledName))
        BaseType = R.get();
    }
  }
  auto *SType = dynamic_cast<const TurfStructType *>(BaseType);
  if (!SType) {
    LogErrorAt(Loc, "Cannot access field on non-struct type", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  int FieldIdx = SType->getFieldIndex(FieldName);
  if (FieldIdx < 0) {
    LogErrorAt(Loc, "Struct '" + SType->getName() + "' has no field named '" +
                        FieldName + "'", SLM::ErrorCategory::UNDECLARED_VAR);
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(BaseType);
  StructType *LLVMStructType = dyn_cast<StructType>(LLVMTy);

  return Builder->CreateStructGEP(LLVMStructType, BaseLVal, FieldIdx,
                                  "fieldptr");
}


Value *FieldAccessExprAST::codegen() {
  Value *Ptr = codegenLValue();
  if (!Ptr)
    return nullptr;

  const TurfType *BaseType = getExprType(Base.get());
  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
  }

  // Resolve NamedType base to TurfStructType, mirroring what codegenLValue()
  // already does. Without this, `p: Node*` has pointee NamedType("Node") —
  // not TurfStructType — and the cast below would fail.
  if (auto *NT = dynamic_cast<const NamedType *>(BaseType)) {
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        BaseType = R.get();
  }
  // Resolve a GenericType base (e.g. `Box<int>`) the same way -- see the
  // matching comment in codegenLValue() just above.
  if (auto *GT = dynamic_cast<const GenericType *>(BaseType)) {
    if (GlobalSymbolTable) {
      std::string MangledName =
          Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs());
      if (auto R = GlobalSymbolTable->LookupType(MangledName))
        BaseType = R.get();
    }
  }

  auto *SType = dynamic_cast<const TurfStructType *>(BaseType);
  if (!SType) {
    LogErrorAt(Loc, "Cannot load field on non-struct type", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  TurfTypePtr FieldType = SType->getFieldType(FieldName);

  return Builder->CreateLoad(getLLVMType(FieldType.get()), Ptr,
                             FieldName.c_str());
}


Value *ModuleImportExprAST::codegen() {
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterModule(ModuleName);
  }
  Value *Res = Body->codegen();
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitModule();
  }
  return Res;
}
