/**
 * @file CodegenArray.cpp
 * @brief Array codegen: declarations, literals, and bounds-checked element access.
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
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;




// Ensure malloc / realloc / free are declared in the module.


// Array Codegen
Value *ArrayDeclExprAST::codegen() {
  // Validation (void type, keywords, duplicates, bounds, etc.) is handled by the
  // Resolver and TypeChecker passes before Codegen.


  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  Type *ElemTy = getLLVMType(ElementType.get());
  llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, Size);

  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  AllocaInst *Alloca = TmpB.CreateAlloca(ArrTy, nullptr, Name);

  // Zero-initialize the entire array (memset to 0)
  // Get total size in bytes
  auto &DL = TheModule->getDataLayout();
  uint64_t TotalBytes = DL.getTypeAllocSize(ArrTy);
  Builder->CreateMemSet(
      Alloca, ConstantInt::get(Type::getInt8Ty(*TheContext), 0),
      ConstantInt::get(Type::getInt64Ty(*TheContext), TotalBytes),
      Alloca->getAlign());

  // If we have an initializer list, store each element
  if (InitExpr) {
    Value *RHSVal = InitExpr->codegen();
    if (!RHSVal)
      return nullptr;
    // Cast to the array type to ensure it's exact (though the TypeChecker
    // should have already validated this).
    TurfTypePtr ArrType = makeArrayType(ElementType, Size);
    RHSVal = CastToType(RHSVal, ArrType, "arrayinit", Loc);
    Builder->CreateStore(RHSVal, Alloca);
  } else if (!InitList.empty()) {
    for (int i = 0; i < static_cast<int>(InitList.size()); ++i) {
      Value *ElemVal = InitList[i]->codegen();
      if (!ElemVal)
        return nullptr;

      // Element type compatibility and void checks are handled by the TypeChecker.

      ElemVal = CastToType(ElemVal, ElementType, "arrayinit", Loc);

      // GEP into array[i] and store
      Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                          ConstantInt::get(Type::getInt64Ty(*TheContext), i)};
      Value *ElemPtr =
          Builder->CreateInBoundsGEP(ArrTy, Alloca, Indices, "arrelem");
      Builder->CreateStore(ElemVal, ElemPtr);
    }
  }

  // Register in symbol table — array type already carries the size
  TurfTypePtr ArrType = makeArrayType(ElementType, Size);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(Name, ArrType, Loc, Alloca);
  }

  NamedValues[Name] = {Alloca, ArrType};

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}


Value *ArrayLiteralExprAST::codegen() {
  if (Elements.empty()) {
    llvm::report_fatal_error("Internal error: empty array literal codegen");
    return nullptr;
  }

  const TurfType *ElemType = getExprType(Elements[0].get());
  Type *LLVMElemTy = getLLVMType(ElemType);
  llvm::ArrayType *ArrTy = llvm::ArrayType::get(LLVMElemTy, Elements.size());

  Value *ArrVal = llvm::UndefValue::get(ArrTy);

  for (size_t i = 0; i < Elements.size(); ++i) {
    Value *ElemVal = Elements[i]->codegen();
    if (!ElemVal)
      return nullptr;
    
    ElemVal = CastToType(ElemVal, ElemType, "arrlitelem", Loc);
    ArrVal = Builder->CreateInsertValue(ArrVal, ElemVal, i, "arrlitinsert");
  }

  return ArrVal;
}


// Helper function to emit runtime bounds check
static void EmitRuntimeBoundsCheck(Value *IndexVal, int ArraySize,
                                   const std::string &ArrayName,
                                   SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Check: index < 0 || index >= size
  Value *SizeVal = ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize);
  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);

  Value *TooSmall = Builder->CreateICmpSLT(IndexVal, Zero, "idx_neg");
  Value *TooBig = Builder->CreateICmpSGE(IndexVal, SizeVal, "idx_toobig");
  Value *OutOfBounds = Builder->CreateOr(TooSmall, TooBig, "oob");

  BasicBlock *OobBB = BasicBlock::Create(*TheContext, "arr_oob", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "arr_ok");

  Builder->CreateCondBr(OutOfBounds, OobBB, ContBB);

  // Out-of-bounds block: call fail()
  Builder->SetInsertPoint(OobBB);
  CodegenPanic(Builder->CreateGlobalString(
      "Array index out of bounds for '" + ArrayName + "'"), Loc);

  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  // Continue block
  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
}


Value *ArrayAccessExprAST::codegenLValue() {
  const TurfType *BaseTy = getExprType(Base.get());
  if (!BaseTy) {
    llvm::report_fatal_error("Internal error: Could not determine type of array/list base");
    return nullptr;
  }

  if (BaseTy->isString()) {
    Value *StrVal = Base->codegen();
    if (!StrVal)
      return nullptr;

    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "stridx", Loc);

    return Builder->CreateGEP(Type::getInt8Ty(*TheContext), StrVal, IndexVal,
                              "charptr");
  }

  if (auto *PtrTyTurf = dynamic_cast<const TurfPointerType *>(BaseTy)) {
    Value *PtrVal = Base->codegen();
    if (!PtrVal)
      return nullptr;

    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "ptridx", Loc);

    Type *ElemTy = getLLVMType(PtrTyTurf->getPointee().get());
    return Builder->CreateGEP(ElemTy, PtrVal, IndexVal, "ptrelemptr");
  }

  if (!isArrayType(BaseTy)) {
    llvm::report_fatal_error("Internal error: Base is not an array or pointer");
    return nullptr;
  }

  Value *BaseLVal = Base->codegenLValue();
  if (!BaseLVal)
    return nullptr;

  int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
  const TurfType *ElemType = getArrayElementType(BaseTy);

  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;
  // Index type check is handled by TypeChecker.
  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base.get());
        ArrayBoundsError Err(Loc, BaseVar ? BaseVar->getName() : "array", Idx,
                             ArraySize);
        if (BaseVar && GlobalSymbolTable) {
          if (Symbol *S = GlobalSymbolTable->LookupSymbol(BaseVar->getName()))
            Err.withRelated(S->DeclLoc, "array declared here");
        }
        Err.raise();
        return nullptr;
      }
    } else if (!ProvenInBounds) {
      // OPT-002: ProvenInBounds is set by ValueRangePass::markElidableChecks()
      // (ValueRange.cpp), which runs on a throwaway CFG BEFORE Body->codegen()
      // -- see that call site's own comment (CodegenFunc.cpp). F-OPT-2
      // (ARCHITECTURE_OVERHAUL.md §17.3): removal by proof only, independent
      // of `-O` level -- this is not gated on any optimization flag.
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, "array", Loc);
    }
  }

  Type *ElemTy = getLLVMType(ElemType);
  if (ArraySize > 0) {
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
    Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                        IndexVal};
    return Builder->CreateInBoundsGEP(ArrTy, BaseLVal, Indices, "arrelemptr");
  } else {
    Type *PtrTy = PointerType::get(*TheContext, 0);
    Value *ArrPtr = Builder->CreateLoad(PtrTy, BaseLVal, "arrptr");
    return Builder->CreateGEP(ElemTy, ArrPtr, IndexVal, "arrelemptr");
  }
}


Value *ArrayAccessExprAST::codegen() {
  // ── operator[] dispatch (LANG-001) ───────────────────────────────────────
  // TypeChecker::visitArrayAccess() already resolved ResolvedIndexGetTarget
  // (if any) via exact (BaseType, IndexType) match. A struct base has no
  // physical memory layout `codegenLValue()` could GEP into for this at
  // all -- this is an ordinary function call returning T BY VALUE, so it is
  // checked first and never touches codegenLValue()/the array/pointer/
  // string paths below (mirrors BinaryExprAST::codegen()'s ResolvedOperator
  // Target dispatch).
  if (ResolvedIndexGetTarget) {
    // Self is passed BY ADDRESS when the overload declared it `&T`
    // (operatorSelfOperandMatches()'s deliberate relaxation,
    // TypeCheckerCall.cpp) -- BY VALUE otherwise.
    const bool SelfByRef = !ResolvedIndexGetTarget->Params.empty() &&
        ResolvedIndexGetTarget->Params[0].Type &&
        ResolvedIndexGetTarget->Params[0].Type->isReference();
    Value *BaseVal = SelfByRef ? Base->codegenLValue() : Base->codegen();
    if (!BaseVal)
      return nullptr;
    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    std::string MangledName = ResolvedIndexGetTarget->mangledName();
    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: operator overload '" +
                          MangledName + "' was resolved but never codegenned."));
      return nullptr;
    }
    return Builder->CreateCall(CalleeF, {BaseVal, IndexVal}, "idxoptmp");
  }

  const TurfType *BaseTy = getExprType(Base.get());
  if (BaseTy && BaseTy->isString()) {
    Value *StrVal = Base->codegen();
    if (!StrVal)
      return nullptr;

    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "stridx", Loc);

    Value *CharPtr = Builder->CreateGEP(Type::getInt8Ty(*TheContext), StrVal,
                                        IndexVal, "charptr");
    Value *CharVal = Builder->CreateLoad(Type::getInt8Ty(*TheContext), CharPtr,
                                         "charval");
    return Builder->CreateZExt(CharVal, Type::getInt64Ty(*TheContext),
                               "charint");
  }

  // First try to codegen the base as an L-value (e.g., a variable or another pointer-like access).
  Value *Ptr = codegenLValue();
  if (Ptr) {
    BaseTy = getExprType(Base.get());
    const TurfType *ElemTyT = nullptr;
    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
      ElemTyT = PtrTy->getPointee().get();
    else
      ElemTyT = getArrayElementType(BaseTy);
    return Builder->CreateLoad(getLLVMType(ElemTyT), Ptr, "arrelem");
  }

  // If the base cannot be treated as an L-value (e.g., it is an R-value array returned from a function call),
  // we codegen it as an R-value.
  Value *BaseVal = Base->codegen();
  if (!BaseVal)
    return nullptr;

  BaseTy = getExprType(Base.get());
  if (!isArrayType(BaseTy)) {
    llvm::report_fatal_error("Internal error: Base is not an array");
    return nullptr;
  }

  int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
  const TurfType *ElemType = getArrayElementType(BaseTy);

  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;

  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base.get());
        ArrayBoundsError Err(Loc, BaseVar ? BaseVar->getName() : "array", Idx,
                             ArraySize);
        if (BaseVar && GlobalSymbolTable) {
          if (Symbol *S = GlobalSymbolTable->LookupSymbol(BaseVar->getName()))
            Err.withRelated(S->DeclLoc, "array declared here");
        }
        Err.raise();
        return nullptr;
      }
      return Builder->CreateExtractValue(BaseVal, CI->getZExtValue(), "arrextract");
    } else {
      // Dynamic index on an R-value array. We must dump it into an alloca to GEP into it.
      Function *TheFunction = Builder->GetInsertBlock()->getParent();
      AllocaInst *TempAlloca = CreateEntryBlockAlloca(TheFunction, "tmparr", BaseTy);
      Builder->CreateStore(BaseVal, TempAlloca);

      // OPT-002: see codegenLValue()'s identical guard's comment. In
      // practice ProvenInBounds is never true on THIS path -- markNode()
      // (ValueRange.cpp) only sets it when the base is a plain array
      // variable, and a plain array variable's base always succeeds via
      // codegenLValue() above, which never reaches this R-value fallback --
      // but the guard is kept here too rather than assumed, for the same
      // reason the compile-time-constant check just above is duplicated
      // rather than shared between the two branches.
      if (!ProvenInBounds)
        EmitRuntimeBoundsCheck(IndexVal, ArraySize, "array", Loc);

      Type *ElemTy = getLLVMType(ElemType);
      llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
      Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0), IndexVal};
      Value *ElemPtr = Builder->CreateInBoundsGEP(ArrTy, TempAlloca, Indices, "arrelemptr");
      return Builder->CreateLoad(ElemTy, ElemPtr, "arrelem");
    }
  }

  llvm::report_fatal_error("Internal error: Could not codegen ArrayAccessExprAST");
  return nullptr;
}
