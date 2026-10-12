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
#include "Types.h"
#include "CodegenInternal.h"
#include "Monomorphize.h" // Monomorphize::instantiatedDeclName() -- FieldAccessExprAST's per-instantiation lookup, TYPE-002 S13b
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/ModuleUtils.h" // appendToCompilerUsed -- RT-001 TurfTypeDesc globals
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <unordered_map>
#include <vector>

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
      // Codegen all arguments (a `T[]` parameter takes a slice --
      // CodegenSliceArgument()).
      std::vector<Value *> ArgVals;
      for (size_t I = 0; I < Args.size(); ++I) {
        const bool ToSlice =
            ResolvedCallTarget && I < ResolvedCallTarget->Params.size() &&
            isUnsizedArrayType(ResolvedCallTarget->Params[I].Type.get());
        Value *V = ToSlice ? CodegenSliceArgument(Args[I].get()) : Args[I]->codegen();
        if (!V) return nullptr;
        ArgVals.push_back(V);
      }

      // ResolvedCallTarget is set by the TypeChecker during overload resolution.
      // If it's null the TypeChecker already emitted a type-mismatch error; just
      // propagate nullptr so Codegen can bail out cleanly.
      if (!ResolvedCallTarget) {
        LogErrorAt(Loc, ErrCode::ModuleArityOrType,
                   "No matching overload found for '" + ModuleName + "." +
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
            V = EmitCheckedFPToSI(V, ExpTy, Loc, "argcast");
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
    // A `T[]` receiver or argument (`fn sum(arr: int[]): int` called as
    // `arr.sum()`) takes a slice, exactly as in an ordinary call --
    // CodegenSliceArgument() also covers a receiver with no address of its
    // own (`makeArr().sum()`), which used to be an internal error here.
    const auto &Params = ResolvedCallTarget->Params;
    auto EvalArg = [&](ExprAST *E, size_t ParamIdx) -> Value * {
      if (ParamIdx < Params.size() &&
          isUnsizedArrayType(Params[ParamIdx].Type.get()))
        return CodegenSliceArgument(E);
      return E->codegen();
    };
    Value *ReceiverVal = EvalArg(Base.get(), 0);
    if (!ReceiverVal)
      return nullptr;
    // `self.f()` for a `fn f(t: T)`: the receiver is a `T*`, the callee
    // takes the struct by value (TypeChecker::tryUFCSOnReceiver()).
    if (UFCSLoadsReceiver && ReceiverVal->getType()->isPointerTy() &&
        !Params.empty())
      ReceiverVal = Builder->CreateLoad(getLLVMType(Params[0].Type.get()),
                                        ReceiverVal, "ufcsrecv");
    std::vector<Value *> ArgVals;
    ArgVals.push_back(ReceiverVal);
    for (size_t I = 0; I < Args.size(); ++I) {
      Value *V = EvalArg(Args[I].get(), I + 1);
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
          V = EmitCheckedFPToSI(V, ExpTy, Loc, "argcast");
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

  // `arr.copy()`: a new `T[]` with storage of its own (TypeChecker::
  // visitMethodCall()). The storage is a GC heap block, because the length
  // may only be known at run time (`a[i..j].copy()`) and a frame slot can't
  // be sized then. It is allocated with the per-ELEMENT type descriptor a
  // List's buffer uses, so the collector traces each element precisely: a
  // `string[]` copy keeps its strings alive. The block's only reference is
  // the slice's data pointer, held on the stack (the conservative scan sees
  // it) -- once the slice is gone, the copy is garbage.
  if (Method == "copy" && Args.empty() && isArrayType(BaseTy)) {
    Value *Src = CodegenSliceArgument(Base.get());
    if (!Src)
      return nullptr;
    Type *I64 = Type::getInt64Ty(*TheContext);
    Type *ElemTy = getLLVMType(dynamic_cast<const ::ArrayType *>(BaseTy)->getElementType());
    uint64_t ElemSize = TheModule->getDataLayout().getTypeAllocSize(ElemTy);
    Value *SrcData = Builder->CreateExtractValue(Src, 0, "copy.src");
    Value *Len = Builder->CreateExtractValue(Src, 1, "copy.len");
    Value *Bytes = Builder->CreateMul(Len, ConstantInt::get(I64, ElemSize), "copy.bytes");
    // Never ask for 0 bytes: an empty copy still gets a real (unused) block.
    Value *AllocBytes = Builder->CreateSelect(
        Builder->CreateICmpEQ(Bytes, ConstantInt::get(I64, 0)),
        ConstantInt::get(I64, ElemSize ? ElemSize : 1), Bytes, "copy.allocbytes");
    Value *Dst = Builder->CreateCall(
        getOrCreateTurfRtAllocFn(),
        {AllocBytes, getOrCreateElementTurfTypeDesc(ElemTy)}, "copy.data");
    Builder->CreateMemCpy(Dst, MaybeAlign(8), SrcData, MaybeAlign(8), Bytes);
    Value *Result = Builder->CreateInsertValue(PoisonValue::get(getSliceLLVMType()),
                                               Dst, 0);
    return Builder->CreateInsertValue(Result, Len, 1, "copy");
  }

  if (Method == "length") {
    if (isArrayType(BaseTy)) {
      int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
      // A `T[]` parameter's length is the slice's second field (issue #260).
      // Before slices it was the static Size, so this answered -1.
      if (ArraySize < 0) {
        Value *Slice = Base->codegen();
        if (!Slice)
          return nullptr;
        return Builder->CreateExtractValue(Slice, 1, "slice.len");
      }
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
    // A `T[]` parameter takes a slice (CodegenSliceArgument()). An instance
    // method's Params[0] is `self`, which has no entry in Args.
    const auto *BaseVarForSlice = dynamic_cast<const VariableExprAST *>(Base.get());
    const size_t SliceParamOffset =
        (BaseVarForSlice && BaseVarForSlice->IsStructName) ? 0 : 1;
    std::vector<Value *> EvaluatedArgs;
    for (size_t I = 0; I < Args.size(); ++I) {
      const size_t P = I + SliceParamOffset;
      const bool ToSlice = ResolvedCallTarget && P < ResolvedCallTarget->Params.size() &&
                           isUnsizedArrayType(ResolvedCallTarget->Params[P].Type.get());
      Value *V = ToSlice ? CodegenSliceArgument(Args[I].get()) : Args[I]->codegen();
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
      // TYPE-002 (S15/D1): a non-empty MangledName means the TypeChecker
      // already resolved this call to a real declaration (ResolvedCallTarget
      // is set) -- if the LLVM module has no function under that name, the
      // monomorphizer (or ordinary codegen) failed to emit it, not the user.
      // This used to return nullptr here with NO diagnostic at all: a
      // missed monomorphization rewrite (D1) silently dropped the call
      // entirely, and if this call sat on the RHS of a `||`/`&&`
      // short-circuit, BinaryExprAST::codegen()'s own `if (!R) return
      // nullptr;` early-return skipped emitting the merge-block branch,
      // producing a confusing, badly-misattributed "does not have
      // terminator! label %or.merge" LLVM-verifier error nowhere near the
      // real cause -- see TYPE-002.md's own *Removed assumptions* for the
      // %or.merge story this mirrors. Matches the wording
      // FuncCallExprAST's own equivalent miss already uses
      // (CodegenFunc.cpp).
      if (!CalleeF) {
        SemanticError(Loc, "Internal error: LLVM function '" + MangledName +
                               "' not found after successful name resolution. "
                               "This is a compiler bug.")
            .raise();
        return nullptr;
      }
    }

    // ResolvedCallTarget itself null (MangledName empty): overload
    // resolution already failed and the TypeChecker emitted a real
    // diagnostic elsewhere -- propagate nullptr quietly, as before.
    if (!CalleeF) {
      return nullptr;
    }

    bool IsStaticMethod = false;
    if (auto *Var = dynamic_cast<const VariableExprAST*>(Base.get())) {
      IsStaticMethod = Var->IsStructName;
    }

    // Arity is guaranteed by the TypeChecker — if it's wrong here, it's a bug.
    // OVH-003: a `@caller_location` callee's REAL LLVM arg count is one MORE
    // than its Turf-level signature (self + Args) suggests -- its hidden
    // trailing parameter (caller_location.md §3.5) is never written by the
    // caller, so it doesn't appear in `Args` at all. Missing this in the
    // arity check let a mismatched callee slip past an `assert()` that's
    // compiled out in a release build, straight into the arg-binding loop
    // below indexing `EvaluatedArgs[Idx]` one past its end -- a real,
    // previously-undiscovered segfault (confirmed live: any `@caller_location`
    // struct method, generic or not, crashed the compiler; this call path
    // had simply never been exercised with one before -- FuncCallExprAST::
    // codegen()'s sibling free-function path already handles this correctly,
    // see its own matching comment, CodegenFunc.cpp).
    int ExpectedArity = IsStaticMethod ? Args.size() : (Args.size() + 1);
    if (ResolvedCallTarget && ResolvedCallTarget->HasCallerLocation)
      ExpectedArity += 1;
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
      // OVH-003: stop before the hidden trailing `caller_location` LLVM
      // parameter (see the arity-check comment above) -- there is nothing
      // in `EvaluatedArgs` for it to read; it's supplied separately, below.
      if (Idx >= EvaluatedArgs.size())
        break;
      Value *V = EvaluatedArgs[Idx];
      Type *ExpectedTy = ArgIt->getType();
      if (V->getType() != ExpectedTy) {
        // TYPE-002 (S15/D8): this whole mismatch block used to erase/
        // un-erase an argument INTO/OUT OF a generic parameter's opaque
        // Type-Erasure slot (IntToPtr/PtrToInt/bitcast) -- dead ever since
        // monomorphization gives every call site a REAL, concrete callee
        // signature (S12a-S14a; INV-GEN-1 holds). Verified live, this
        // session: all four branches (plus the double<->pointer pair)
        // instrumented with an `fprintf` probe, zero hits compiling every
        // file under tests/, lib/std/, and showcase/ -- the enclosing
        // `if` itself never fires any more. Kept as a hybrid "erasure path
        // for compatibility" was exactly the ambiguity this whole group's
        // Must-NOT-reinterpret list forbids, and a silent bitcast here
        // would paper over a future monomorphization miss instead of
        // failing loudly (the theme of D1's identical fix, above) --
        // replaced with a diagnostic rather than deleted outright, so a
        // regression here is a clear compiler-bug report, not a
        // resurrected `bitcast double to ptr`-style illegal-IR crash.
        SemanticError(Loc,
                      "Internal error: struct method call argument type "
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

    // OVH-003: auto-supply the hidden trailing argument a `@caller_location`
    // callee's REAL signature has but its Turf-level `Args` never wrote --
    // mirrors FuncCallExprAST::codegen()'s identical, already-correct fix
    // (CodegenFunc.cpp) for the free-function call path.
    if (ResolvedCallTarget && ResolvedCallTarget->HasCallerLocation)
      ArgVals.push_back(BuildCallerLocationValue(Loc));

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
      LogErrorAt(Loc, ErrCode::InternalCompilerError, "Internal error: exported constant not resolved in codegen", SLM::ErrorCategory::GENERAL);
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
    LogErrorAt(Loc, ErrCode::InternalCompilerError, "Internal error: enum type not found in codegen", SLM::ErrorCategory::GENERAL);
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

// ─── RT-001: TurfTypeDesc emission ─────────────────────────────────────────
// See runtime/turfrt.h's TurfTypeDesc doc comment for the full contract.
// The LLVM layout built here ({i64, ptr, i64}) must match that C struct
// field-for-field -- nothing enforces this automatically across the
// C/LLVM-IR boundary, so a change to one requires the matching change here.

// Registers (or reuses) the identified LLVM struct type mirroring C's
// `TurfTypeDesc` -- one shared type definition for every descriptor global,
// not synthesized fresh per struct. OVH-001: widened from
// `{i64, ptr, i64}` (a single inline `uint64_t` bitmap, capped at 64
// words/512 bytes -- MEM-002's mark loop silently stopped tracing past
// that point, a live use-after-free) to `{i64, ptr, i64, ptr}` --
// `word_count` plus a pointer to an out-of-line `[N x i64]` bitmap array,
// covering a type's entire layout at any size. This is a compiler<->
// runtime ABI change (F-MEM-4: lock-step, no compatibility shim) -- see
// runtime/turfrt.h's matching `TurfTypeDesc` definition.
static llvm::StructType *getOrCreateTurfTypeDescType() {
  if (auto *Existing = llvm::StructType::getTypeByName(*TheContext, "TurfTypeDesc"))
    return Existing;
  llvm::Type *I64Ty = llvm::Type::getInt64Ty(*TheContext);
  llvm::Type *PtrTy = llvm::PointerType::getUnqual(*TheContext);
  return llvm::StructType::create(*TheContext, {I64Ty, PtrTy, I64Ty, PtrTy},
                                  "TurfTypeDesc");
}

// Recursively ORs pointer-word bits for `T`'s layout into `Bitmap`
// (pre-sized to cover every word of `T`'s own alloc size -- see
// buildTurfTypeDescInit() below), starting at `ByteOffset`. Conservative,
// not GC-vs-manual precise (nothing before MEM-001/MEM-002 tags a pointer
// field with which heap it points into) -- see runtime/turfrt.h's doc
// comment on TurfTypeDesc for why over-marking here is sound rather than
// merely convenient. By the time a struct field's LLVM type reaches here
// it is always fully built (StructDeclExprAST::codegen()'s own comment on
// embedded, non-recursive struct fields -- a self-referential/recursive
// field is always a raw POINTER, never embedded by value, so the
// isOpaque() branch below is unreachable in practice and exists only as a
// defensive bound, not a silently-accepted case).
static void collectPointerWordBits(llvm::Type *T, const llvm::DataLayout &DL,
                                   uint64_t ByteOffset,
                                   std::vector<uint64_t> &Bitmap) {
  if (T->isPointerTy()) {
    uint64_t Word = ByteOffset / 8;
    uint64_t Idx = Word / 64;
    assert(Idx < Bitmap.size() &&
           "OVH-001: pointer word falls outside the sized bitmap -- "
           "buildTurfTypeDescInit()'s word-count computation is wrong");
    if (Idx < Bitmap.size())
      Bitmap[Idx] |= (1ULL << (Word % 64));
    return;
  }
  if (auto *NestedST = llvm::dyn_cast<llvm::StructType>(T)) {
    if (NestedST->isOpaque())
      return;
    const llvm::StructLayout *SL = DL.getStructLayout(NestedST);
    for (unsigned i = 0; i < NestedST->getNumElements(); ++i) {
      collectPointerWordBits(NestedST->getElementType(i), DL,
                             ByteOffset + SL->getElementOffset(i), Bitmap);
    }
    return;
  }
  if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(T)) {
    llvm::Type *ElemTy = AT->getElementType();
    uint64_t ElemSize = DL.getTypeAllocSize(ElemTy).getFixedValue();
    for (uint64_t i = 0; i < AT->getNumElements(); ++i)
      collectPointerWordBits(ElemTy, DL, ByteOffset + i * ElemSize, Bitmap);
    return;
  }
  // Scalar (int/int32/double/bool/char): contributes no pointer bits.
}

// Shared by emitTurfTypeDesc() and getOrCreateElementTurfTypeDesc() (OVH-001
// -- the old per-copy inline-bitmap logic was duplicated across both, which
// is exactly how the 64-word cap ended up needing two independent fixes
// instead of one): builds the `{size, name, word_count, pointer_words}`
// constant for one LLVM type `T`. `NameStr` is the human-readable name
// stored in the descriptor; `NameGVName`/`BitmapGVName` name the two
// out-of-line globals the descriptor's `name`/`pointer_words` fields point
// into (callers keep their own pre-existing naming for the first so
// scripts/runtime_smoke.sh's --emit-llvm greps keep matching).
static llvm::Constant *buildTurfTypeDescInit(llvm::Type *T,
                                              const std::string &NameStr,
                                              const std::string &NameGVName,
                                              const std::string &BitmapGVName) {
  const llvm::DataLayout &DL = TheModule->getDataLayout();
  llvm::Type *I64Ty = llvm::Type::getInt64Ty(*TheContext);
  uint64_t Size = DL.getTypeAllocSize(T).getFixedValue();

  // Every 8-byte word of the type's own alloc size, including a trailing
  // partial one -- OVH-001's replacement for the old fixed 64-word cap.
  uint64_t WordCount = (Size + 7) / 8;
  uint64_t NumBitmapWords = std::max<uint64_t>(1, (WordCount + 63) / 64);

  std::vector<uint64_t> Bits(NumBitmapWords, 0);
  collectPointerWordBits(T, DL, 0, Bits);

  // The emitter's own length assertion (OVERHAUL_REMAINING_WORK.md OVH-001,
  // strategy point 5) -- a future truncation of either quantity is loud in
  // a debug build, not a silent missed root.
  assert(WordCount * 8 >= Size &&
         "OVH-001: word_count must cover every byte of the type's alloc size");
  assert(Bits.size() == NumBitmapWords &&
         "OVH-001: bitmap array length must match the computed word count");

  // Same Builder-independent global-string-constant idiom this file's own
  // ScopeAccessExprAST "NAMES" case above uses -- this runs at the very end
  // of codegen (see EmitAllTurfTypeDescs() below), well past any point
  // Builder's insertion point is meaningful for a fresh global.
  llvm::Constant *NameStrConst =
      llvm::ConstantDataArray::getString(*TheContext, NameStr, true);
  auto *NameGV = new llvm::GlobalVariable(
      *TheModule, NameStrConst->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, NameStrConst, NameGVName);
  llvm::Constant *ZeroIdx =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext), 0);
  llvm::Constant *Indices[] = {ZeroIdx, ZeroIdx};
  llvm::Constant *NamePtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      NameStrConst->getType(), NameGV, Indices);

  llvm::ArrayType *BitmapArrTy = llvm::ArrayType::get(I64Ty, NumBitmapWords);
  std::vector<llvm::Constant *> BitmapWordConsts;
  BitmapWordConsts.reserve(NumBitmapWords);
  for (uint64_t W : Bits)
    BitmapWordConsts.push_back(llvm::ConstantInt::get(I64Ty, W));
  llvm::Constant *BitmapArrConst =
      llvm::ConstantArray::get(BitmapArrTy, BitmapWordConsts);
  auto *BitmapGV = new llvm::GlobalVariable(
      *TheModule, BitmapArrTy, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, BitmapArrConst, BitmapGVName);
  llvm::Constant *BitmapPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      BitmapArrTy, BitmapGV, Indices);

  llvm::StructType *DescTy = getOrCreateTurfTypeDescType();
  return llvm::ConstantStruct::get(
      DescTy, {llvm::ConstantInt::get(I64Ty, Size), NamePtr,
               llvm::ConstantInt::get(I64Ty, WordCount), BitmapPtr});
}

// OVH-002 (docs/development/changes/OVH-002.md): a function body may
// need to reference a NAMED struct type's own descriptor (e.g. to pass it
// to `turf_rt_alloc` when heap-allocating an instance of that struct) at a
// point in codegen that runs BEFORE `EmitAllTurfTypeDescs()` -- which is
// deliberately the LAST thing to run (INV-RT-3). `getOrInsertGlobal()` is
// the standard LLVM idiom for exactly this: if nothing has referenced this
// name yet, it creates a plain external declaration (no initializer) that
// `emitTurfTypeDesc()` (below) later fills in when it reaches this same
// struct type in its own sweep; if `emitTurfTypeDesc()` already ran first,
// this simply returns the real definition. Either order produces the same
// single global -- never two, never a forward reference left dangling.
llvm::GlobalVariable *getOrForwardDeclareTurfTypeDescGlobal(
    llvm::StructType *ST) {
  std::string Name = ST->getName().str();
  llvm::StructType *DescTy = getOrCreateTurfTypeDescType();
  return llvm::cast<llvm::GlobalVariable>(
      TheModule->getOrInsertGlobal("__turf_typedesc_" + Name, DescTy));
}

// Emits a constant TurfTypeDesc global for one already-fully-built (non-
// opaque) `ST`, and keeps it alive across OPT-001's Internalize/GlobalDCE
// via @llvm.compiler.used -- the forward hazard OPT-001's own text already
// records (ARCHITECTURE_OVERHAUL.md §8 RT-001: "Internalize/GlobalDCE must
// not delete a descriptor referenced only from metadata"). Routed through
// `getOrForwardDeclareTurfTypeDescGlobal()` (OVH-002) rather than an
// unconditional `new GlobalVariable(...)` so that a call site elsewhere in
// Codegen (`CodegenCore.cpp`'s `turf_rt_alloc` sites) that already forward-
// referenced this exact struct's descriptor gets its definition filled in
// on the SAME global, instead of silently receiving a renamed ".1"
// duplicate while the real definition sits under the original name,
// referenced by nothing.
static void emitTurfTypeDesc(llvm::StructType *ST) {
  std::string Name = ST->getName().str();
  llvm::Constant *DescInit =
      buildTurfTypeDescInit(ST, Name, "__turf_typedesc_name." + Name,
                             "__turf_typedesc_bitmap." + Name);

  llvm::GlobalVariable *DescGV = getOrForwardDeclareTurfTypeDescGlobal(ST);
  DescGV->setInitializer(DescInit);
  DescGV->setConstant(true);
  DescGV->setLinkage(llvm::GlobalValue::InternalLinkage);
  llvm::appendToCompilerUsed(*TheModule, {DescGV});
}

// ─── MEM-002: per-ELEMENT TurfTypeDesc for a collection's backing buffer ──
// See runtime/turfrt.h's TurfTypeDesc doc comment for the repeating-stride
// semantics this reuses (deliberately no new C struct field): a collection
// growth call site (list/stack/queue/set/map.tr, each writing
// `gc_alloc(byteSize) to T*`) allocates N elements of T, not one instance of
// a struct -- this builds the descriptor for ONE element of T (whatever
// concrete LLVM type T monomorphized to at this call site), the same way
// emitTurfTypeDesc() above builds one for a whole struct instance, and lets
// the collector's mark phase (turfrt_gc.c) repeat it across the block's
// real length. Called from CastExprAST::codegen()'s MEM-002 special case
// (CodegenExpr.cpp) -- see that call site for how `ElemTy` is determined
// (the cast's own destination type, `T*`, at a `gc_alloc(...) to T*` site).
//
// Memoized by the raw llvm::Type* pointer -- every LLVM type this compiler
// ever hands here (a primitive, an opaque pointer, or a specific
// llvm::StructType*) is already uniqued/interned within *TheContext, so
// pointer identity is a correct, and much simpler, cache key than deriving
// a string name to dedupe on.
static std::map<llvm::Type *, llvm::GlobalVariable *> ElementTypeDescCache;

llvm::GlobalVariable *getOrCreateElementTurfTypeDesc(llvm::Type *ElemTy) {
  auto It = ElementTypeDescCache.find(ElemTy);
  if (It != ElementTypeDescCache.end())
    return It->second;

  // A struct element must already be fully built (setBody() run) before its
  // layout can be queried -- exactly INV-RT-3's own condition, restated
  // here because this call site is NOT EmitAllTurfTypeDescs()'s deliberately
  // late single pass, it runs interleaved with ordinary function-body
  // codegen. Safe in practice: every struct sub-pass (2c/3a/3b) finishes
  // before "the main pass" codegens any function body, and this helper is
  // only ever reached from inside a function body (a gc_alloc/gc_realloc
  // call site) -- an opaque struct reaching here would mean that ordering
  // guarantee broke, which is a compiler bug worth crashing loudly on in a
  // debug build (an under-reported bitmap is a MISSED ROOT, §14's own
  // "catastrophic" risk category) rather than silently emitting an unsound
  // descriptor in release.
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(ElemTy)) {
    assert(!ST->isOpaque() &&
           "MEM-002: getOrCreateElementTurfTypeDesc() called on an opaque "
           "struct element type -- see this function's own comment");
  }

  std::string Name;
  llvm::raw_string_ostream OS(Name);
  ElemTy->print(OS);

  llvm::Constant *DescInit = buildTurfTypeDescInit(
      ElemTy, Name, "__turf_elemdesc_name", "__turf_elemdesc_bitmap");

  llvm::StructType *DescTy = getOrCreateTurfTypeDescType();
  auto *DescGV = new llvm::GlobalVariable(
      *TheModule, DescTy, /*isConstant=*/true,
      llvm::GlobalValue::InternalLinkage, DescInit, "__turf_elemdesc");
  llvm::appendToCompilerUsed(*TheModule, {DescGV});

  ElementTypeDescCache[ElemTy] = DescGV;
  return DescGV;
}

// RT-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8, point 3): emits a
// TurfTypeDesc for every real (fully-built) aggregate type in the module.
// Declared in include/Codegen.h; called exactly once from main.cpp,
// deliberately LATE -- immediately before verifyModule(), well after both
// the struct pre-pass (main.cpp sub-pass 2c) AND every import-triggered
// struct build (sub-passes 3a/3b) have finished, i.e. the same point
// main.cpp's own pre-existing "scan for a lingering opaque monomorphized
// stub" check already treats as "every struct that will ever be built now
// is."
//
// This does NOT run per-struct, inline in StructDeclExprAST::codegen(),
// despite that being the more obvious place -- found live, via a genuine
// crash, why that doesn't work: `struct Holder { items: List<int> }`
// codegens BEFORE `List<int>`'s own monomorphized clone (monomorphized
// clones are APPENDED to PrePassASTs, so they codegen LATER than an
// original top-level struct declared earlier in the source). Computing
// Holder's own DataLayout::getTypeAllocSize() at THAT point forces LLVM to
// also compute (and PERMANENTLY CACHE, keyed by StructType* -- LLVM's
// DataLayout has no cache-invalidation hook) a StructLayout for the
// still-opaque `List<int>` field, treating it as zero-sized. When
// `List<int>`'s own clone is codegenned moments later and setBody() gives
// it its real fields, DataLayout::getStructLayout() for that exact
// StructType* returns the SAME stale, zero-element cached layout --
// `StructLayout::getElementOffset(0)` then asserts
// ("Idx < NumElements failed"), a real, reproducible compiler crash
// (confirmed live against tests/38_monomorphization/04_method_on_generic_struct.tr
// and four sibling tests in that folder, all embedding a generic
// instantiation by value). Waiting until every struct in the module is
// unconditionally fully built removes the ordering dependency entirely:
// `DL.getStructLayout()` is never called on any StructType before its own
// `setBody()` has run.
void EmitAllTurfTypeDescs() {
  for (llvm::StructType *ST : TheModule->getIdentifiedStructTypes()) {
    if (ST->isOpaque())
      continue; // A genuinely unfilled stub -- main.cpp's own scan (run
                // just before this) already diagnosed it as a compiler bug;
                // nothing more for this pass to do with it.
    emitTurfTypeDesc(ST);
  }
}

Value *StructDeclExprAST::codegen() {
  // TYPE-002 (S14a): a StructDeclExprAST with non-empty TypeParams is an
  // uninstantiated generic TEMPLATE -- drainStructRequests() (S13a) always
  // appends a fully-concrete CLONE elsewhere in PrePassASTs for every
  // instantiation the program actually needs, so the template's own shared,
  // erasure-era layout (S13a/S13b deliberately left it codegenning
  // unconditionally, coexisting with per-instantiation clones, while
  // erasure still existed to make an open TurfGenericParamType field safe
  // to lower) is now genuinely dead: nothing resolves a GenericType to it
  // any more (getLLVMType()'s GenericType branch and FieldAccessExprAST's
  // two collapse points were both redirected to the per-instantiation
  // mangled name in S13b). Found live: even `Box<T> { val: T }`'s bare
  // TEMPLATE field reaching getLLVMType(TurfGenericParamType) trips
  // INV-GEN-1 the moment erasure's tolerant fallback is gone. Checked here,
  // not just in main.cpp's sub-pass 2c loop, for the identical
  // single-real-entry-point reason FuncDefExprAST::codegen() checks its own
  // TypeParams internally (CodegenFunc.cpp) -- a struct declared inside an
  // imported module is ALSO reached via ModuleImportExprAST::codegen()'s
  // own recursive body walk, which has no equivalent guard of its own.
  if (!TypeParams.empty())
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));

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
    // Brand-new opaque struct — no body yet. Named `M.Name` for a module-
    // owned type (FDN-003, TypeIdentity::llvmName()); LLVMTypeCache below
    // stays keyed by the identity itself. The GC descriptor's symbol and
    // runtime-visible name are both derived from this LLVM name.
    ST = StructType::create(*TheContext, TypeIdentity::llvmName(Name));
    if (!ST) {
      LogErrorAt(Loc, ErrCode::InternalCompilerError, "Failed to create LLVM StructType for '" + Name + "'", SLM::ErrorCategory::GENERAL);
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
      LogErrorAt(Loc, ErrCode::InternalCompilerError, "Unknown type for field '" + Field.Name + "'", SLM::ErrorCategory::GENERAL);
      return nullptr;
    }
    LLVMFieldTypes.push_back(LLVMType);
  }

  // Step 3: Fill in the body now that all field types are resolved.
  ST->setBody(LLVMFieldTypes);

  // RT-001: TurfTypeDesc emission does NOT happen here -- see
  // EmitAllTurfTypeDescs()'s own doc comment (below) for why emitting it
  // eagerly, per struct, at THIS point crashed on a real, live case (a
  // struct embedding a monomorphized generic instantiation by value before
  // that instantiation's own clone has been codegenned).

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
    LogErrorAt(Loc, ErrCode::InternalCompilerError, "Cannot access field on non-struct type", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  int FieldIdx = SType->getFieldIndex(FieldName);
  if (FieldIdx < 0) {
    LogErrorAt(Loc, ErrCode::UnknownStructField,
               "Struct '" + SType->getName() + "' has no field named '" +
                        FieldName + "'", SLM::ErrorCategory::UNDECLARED_VAR);
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(BaseType);
  StructType *LLVMStructType = dyn_cast<StructType>(LLVMTy);

  return Builder->CreateStructGEP(LLVMStructType, BaseLVal, FieldIdx,
                                  "fieldptr");
}


Value *FieldAccessExprAST::codegen() {
  // STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(c)): `err.message` is not an
  // ordinary struct field -- `err`'s VALUE (not its address) is already a
  // pointer to the {tag, message} TurfErrorValue struct (BuildErrorTag()'s
  // own doc comment, CodegenCore.cpp), so this reads through the base's
  // ORDINARY codegen(), never codegenLValue() below (there is no struct to
  // take the ADDRESS OF a field within here -- `err` itself already IS the
  // pointer). Checked BEFORE calling codegenLValue() at all, which would
  // otherwise try (and fail) to treat `err` as an addressable struct base.
  if (FieldName == "message") {
    const TurfType *BaseType = getExprType(Base.get());
    if (BaseType && BaseType->isAnyError()) {
      Value *ErrVal = Base->codegen();
      if (!ErrVal)
        return nullptr;
      return LoadErrorValueField(ErrVal, /*FieldIdx=*/1, "message");
    }
  }

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
    LogErrorAt(Loc, ErrCode::InternalCompilerError, "Cannot load field on non-struct type", SLM::ErrorCategory::DECLARATION);
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
