/**
 * @file CodegenExpr.cpp
 * @brief Literal and operator-expression codegen: numbers, binary/unary/address-of/cast/sizeof.
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
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;


// Turns a number to LLVM Number constant
Value *NumberExprAST::codegen() {
  if (isInteger())
    return ConstantInt::get(*TheContext, APInt(64, getIntVal(), true));

  // APFloat is how LLVM represents floating point numbers internally
  return ConstantFP::get(*TheContext, APFloat(getDoubleVal()));
}


Value *BoolExprAST::codegen() {
  return ConstantInt::get(Type::getInt1Ty(*TheContext), Val ? 1 : 0);
}


Value *CharExprAST::codegen() {
  return ConstantInt::get(Type::getInt8Ty(*TheContext),
                           static_cast<uint8_t>(Val));
}


Value *StringExprAST::codegen() {
  // Create a global string constant
  return Builder->CreateGlobalString(Val, "str");
}


Value *NullExprAST::codegen() {
  // A null pointer of type i8*
  return ConstantPointerNull::get(PointerType::getUnqual(*TheContext));
}


Value *DefaultInitExprAST::codegen() {
  return GetDefaultValue(Type.get());
}


// EmitStringConcat/EmitStrcmp/EmitStrcmpRelational used to live here,
// hand-rolling String's `+`/`==`/`!=`/`< > <= >=` directly in Codegen.
// Deleted: this behavior now lives in lib/std/strings.tr as ordinary
// `operator` overloads (operator +/==/!=/</>/<=/>= (string, string)),
// resolved the same way any other operator overload is (see
// TypeChecker::visitBinary()'s operator-resolution gate and this
// function's ResolvedOperatorTarget dispatch below). See
// STRING_UFCS_OPERATOR_CHAR_RFC.md §1/§7 step 2. Note: two OTHER,
// independent strcmp-based call sites still exist (string->enum
// conversion, match/switch-over-string) -- both inline their own
// `getOrInsertFunction("strcmp", ...)` and never referenced these helpers,
// so they are correctly unaffected by this deletion (documented follow-on
// cleanup, RFC §1, not done here).

Value *BinaryExprAST::codegen() {
  // `err == MathError::DivisionByZero`
  // (docs/architecture/errors_and_recover.md) -- compares error-tag identity
  // via strcmp, since every error-type value (constructed or type-erased as
  // AnyErrorType) lowers to an opaque i8* payload pointer that's meaningless to
  // compare by raw pointer identity (two logically-equal error references never
  // share one malloc'd payload). Checked before operand codegen (not just
  // before the generic fallback further down) specifically so a bare
  // `ErrorType::Variant` operand can skip its ordinary
  // ScopeAccessExprAST::codegen() path entirely -- see GetErrorTagValue()'s doc
  // comment.
  if ((Op == TOK_EQ || Op == TOK_NEQ) &&
      (IsErrorTagExpr(LHS.get()) || IsErrorTagExpr(RHS.get()))) {
    Value *LTag = GetErrorTagValue(LHS.get());
    Value *RTag = GetErrorTagValue(RHS.get());
    if (!LTag || !RTag)
      return nullptr;
    Value *Cmp = Builder->CreateCall(getOrCreateStrcmpFn(), {LTag, RTag}, "errtagcmp");
    Value *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    return Op == TOK_EQ ? Builder->CreateICmpEQ(Cmp, Zero, "erreq")
                        : Builder->CreateICmpNE(Cmp, Zero, "errneq");
  }

  // ── Short-circuit && / || ───────────────────────────────────────────────
  // These operators must NOT evaluate both sides eagerly.
  // We emit a branch: LHS first, then conditionally evaluate RHS.
  if (Op == TOK_AND) {
    Function *TheFunc = Builder->GetInsertBlock()->getParent();
    BasicBlock *RHSBlock  = BasicBlock::Create(*TheContext, "and.rhs",  TheFunc);
    BasicBlock *MergeBlock = BasicBlock::Create(*TheContext, "and.merge", TheFunc);

    Value *L = LHS->codegen();
    if (!L) return nullptr;
    L = CastToType(L, TurfBool(), "lhstobool");

    BasicBlock *LHSEnd = Builder->GetInsertBlock();
    Builder->CreateCondBr(L, RHSBlock, MergeBlock);

    // RHS block — only reached when LHS is true
    Builder->SetInsertPoint(RHSBlock);
    Value *R = RHS->codegen();
    if (!R) return nullptr;
    R = CastToType(R, TurfBool(), "rhstobool");
    BasicBlock *RHSEnd = Builder->GetInsertBlock();
    Builder->CreateBr(MergeBlock);

    // Merge — phi to pick the right bool value
    Builder->SetInsertPoint(MergeBlock);
    PHINode *Phi = Builder->CreatePHI(Type::getInt1Ty(*TheContext), 2, "andtmp");
    Phi->addIncoming(ConstantInt::getFalse(*TheContext), LHSEnd);
    Phi->addIncoming(R, RHSEnd);
    return Phi;
  }

  if (Op == TOK_OR) {
    Function *TheFunc = Builder->GetInsertBlock()->getParent();
    BasicBlock *RHSBlock  = BasicBlock::Create(*TheContext, "or.rhs",  TheFunc);
    BasicBlock *MergeBlock = BasicBlock::Create(*TheContext, "or.merge", TheFunc);

    Value *L = LHS->codegen();
    if (!L) return nullptr;
    L = CastToType(L, TurfBool(), "lhstobool");

    BasicBlock *LHSEnd = Builder->GetInsertBlock();
    Builder->CreateCondBr(L, MergeBlock, RHSBlock);

    // RHS block — only reached when LHS is false
    Builder->SetInsertPoint(RHSBlock);
    Value *R = RHS->codegen();
    if (!R) return nullptr;
    R = CastToType(R, TurfBool(), "rhstobool");
    BasicBlock *RHSEnd = Builder->GetInsertBlock();
    Builder->CreateBr(MergeBlock);

    // Merge — phi to pick the right bool value
    Builder->SetInsertPoint(MergeBlock);
    PHINode *Phi = Builder->CreatePHI(Type::getInt1Ty(*TheContext), 2, "ortmp");
    Phi->addIncoming(ConstantInt::getTrue(*TheContext), LHSEnd);
    Phi->addIncoming(R, RHSEnd);
    return Phi;
  }
  // ───────────────────────────────────────────────────────────────────────

  // Generate codes for the Left side and Right side
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if (!L || !R)
    return nullptr;

  // ── Operator overload dispatch ──────────────────────────────────────────
  // TypeChecker::visitBinary() already resolved ResolvedOperatorTarget (if
  // any) via exact operand-type match against ResolvedOperatorCandidates --
  // a non-null target here means a user/stdlib `operator` overload applies
  // and the builtin arithmetic/comparison switch below must NOT run at all.
  // Mirrors MethodCallExprAST::codegen()'s module-call dispatch (mangled-
  // name lookup, direct call). No argument casting needed: exact-match
  // resolution guarantees L/R already have the exact types the overload
  // declares.
  if (ResolvedOperatorTarget) {
    std::string MangledName = ResolvedOperatorTarget->mangledName();
    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: operator overload '" +
                          MangledName + "' was resolved but never codegenned."));
      return nullptr;
    }
    return Builder->CreateCall(CalleeF, {L, R}, "optmp");
  }

  // Type validation (void checks, compatibility, etc.) is handled by the TypeChecker.
  TurfTypePtr LTy = getTurfTypeFromLLVM(L->getType());
  TurfTypePtr RTy = getTurfTypeFromLLVM(R->getType());

  if (isa<ConstantPointerNull>(L) || isa<ConstantPointerNull>(R)) {
    if (Op == TOK_EQ || Op == TOK_NEQ) {
      if (isa<ConstantPointerNull>(L)) L = Builder->CreateBitCast(L, R->getType(), "lhscast");
      if (isa<ConstantPointerNull>(R)) R = Builder->CreateBitCast(R, L->getType(), "rhscast");
      return Op == TOK_EQ ? Builder->CreateICmpEQ(L, R, "cmptmp") : Builder->CreateICmpNE(L, R, "cmptmp");
    } else {
      llvm::report_fatal_error("Internal error: Invalid operation with null (caught by TypeChecker)");
      return nullptr;
    }
  }

  auto CastBoth = [&](TurfTypePtr Target) {
    L = CastToType(L, Target, "lhscast");
    R = CastToType(R, Target, "rhscast");
  };

  TurfTypePtr CommonType = getCommonType(LTy, RTy);
  TurfTypePtr NumericType =
      CommonType->isNamed("bool") ? TurfInt() : CommonType;

  // TypeChecker has already verified operator compatibility.

  // Create the instruction based on the operator
  switch (Op) {

    // Arithmetic
  case '+':
    CastBoth(NumericType);
    return NumericType->isNamed("double") ? Builder->CreateFAdd(L, R, "addtmp")
                                          : Builder->CreateAdd(L, R, "addtmp");

  case '-':
    CastBoth(NumericType);
    return NumericType->isNamed("double") ? Builder->CreateFSub(L, R, "subtmp")
                                          : Builder->CreateSub(L, R, "subtmp");

  case '*':
    CastBoth(NumericType);
    return NumericType->isNamed("double") ? Builder->CreateFMul(L, R, "multmp")
                                          : Builder->CreateMul(L, R, "multmp");

  case '/': {
    CastBoth(NumericType);

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB =
        BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB =
        BasicBlock::Create(*TheContext, "divcont", TheFunction);

    Value *IsZero;
    if (NumericType->isNamed("double")) {
      IsZero = Builder->CreateFCmpOEQ(
          R, ConstantFP::get(*TheContext, APFloat(0.0)), "iszero");
    } else {
      IsZero = Builder->CreateICmpEQ(
          R, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "iszero");
    }

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenPanic(Builder->CreateGlobalString("Division by zero"), Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);

    return NumericType->isNamed("double") ? Builder->CreateFDiv(L, R, "divtmp")
                                          : Builder->CreateSDiv(L, R, "divtmp");
  }

  case '%': {
    CastBoth(NumericType);

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB =
        BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB =
        BasicBlock::Create(*TheContext, "divcont", TheFunction);

    Value *IsZero;
    if (NumericType->isNamed("double")) {
      IsZero = Builder->CreateFCmpOEQ(
          R, ConstantFP::get(*TheContext, APFloat(0.0)), "iszero");
    } else {
      IsZero = Builder->CreateICmpEQ(
          R, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "iszero");
    }

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenPanic(Builder->CreateGlobalString("Modulo by zero"), Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);

    return NumericType->isNamed("double") ? Builder->CreateFRem(L, R, "modtmp")
                                          : Builder->CreateSRem(L, R, "modtmp");
  }

  // Comparison
  case '<': {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOLT(L, R, "cmptmp")
                                      : Builder->CreateICmpSLT(L, R, "cmptmp");
  }

  case '>': {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGT(L, R, "cmptmp")
                                      : Builder->CreateICmpSGT(L, R, "cmptmp");
  }

  case TOK_EQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOEQ(L, R, "cmptmp")
                                      : Builder->CreateICmpEQ(L, R, "cmptmp");
  }

  case TOK_NEQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    // UNE ("unordered or not equal"), not ONE ("ordered and not equal"):
    // '!=' must be the logical negation of '==' for every input, including
    // NaN. '==' correctly uses OEQ below (ordered-equal is false for NaN,
    // matching NaN == NaN -> false). ONE requires BOTH operands to be
    // non-NaN AND unequal, so ONE(NaN, NaN) is *also* false -- meaning
    // `x != x` silently evaluated to false instead of true whenever x held
    // NaN, breaking the standard `x != x` isnan(x) idiom (found while
    // writing a Lint.cpp test that relied on it -- see
    // tests/02_resolver/warn_self_comparison_nan.tr).
    return CmpType->isNamed("double") ? Builder->CreateFCmpUNE(L, R, "cmptmp")
                                      : Builder->CreateICmpNE(L, R, "cmptmp");
  }

  case TOK_GEQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGE(L, R, "cmptmp")
                                      : Builder->CreateICmpSGE(L, R, "cmptmp");
  }

  case TOK_LEQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOLE(L, R, "cmptmp")
                                      : Builder->CreateICmpSLE(L, R, "cmptmp");
  }

  // Logical operators
  case TOK_AND: {
    L = CastToType(L, TurfBool(), "lhstobool");
    R = CastToType(R, TurfBool(), "rhstobool");
    return Builder->CreateAnd(L, R, "andtmp");
  }

  case TOK_OR: {
    L = CastToType(L, TurfBool(), "lhstobool");
    R = CastToType(R, TurfBool(), "rhstobool");
    return Builder->CreateOr(L, R, "ortmp");
  }

  // Bitwise XOR (^ is now XOR, not power)
  case '^':
    // Bitwise XOR — both operands must be int (TypeChecker verified)
    return Builder->CreateXor(L, R, "xortmp");

  // Bitwise AND
  case '&':
    return Builder->CreateAnd(L, R, "bitandtmp");

  // Bitwise OR
  case '|':
    return Builder->CreateOr(L, R, "bitormp");

  // Bitwise left shift
  case TOK_LSHIFT:
    return Builder->CreateShl(L, R, "lshifttmp");

  // Bitwise right shift (arithmetic, preserves sign)
  case TOK_RSHIFT:
    return Builder->CreateAShr(L, R, "rshifttmp");

  // Exponentiation: ** (replaces the old ^ operator)
  case TOK_POWER: {
    CastBoth(TurfDouble());
#if LLVM_VERSION_MAJOR >= 19
    Function *PowFunc = llvm::Intrinsic::getOrInsertDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
#else
    Function *PowFunc = llvm::Intrinsic::getDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
#endif
    return Builder->CreateCall(PowFunc, {L, R}, "powtmp");
  }
  default:
    SemanticError(CurLoc, "Error: invalid binary operator");
    return nullptr;
  }
}


Value *UnaryExprAST::codegen() {
  Value *OperandV = Operand->codegen();
  if (!OperandV)
    return nullptr;

  TurfTypePtr OperandType = getTurfTypeFromLLVM(OperandV->getType());
  if (OperandType->isNamed("bool"))
    OperandV = CastToType(OperandV, TurfInt(), "boolneg");

  OperandType = getTurfTypeFromLLVM(OperandV->getType());

  switch (Opcode) {
  case '-':
    if (OperandType->isNamed("double"))
      return Builder->CreateFNeg(OperandV);
    if (OperandType->isNamed("int"))
      return Builder->CreateNeg(OperandV);
    llvm::report_fatal_error("Internal error: Unknown unary operand type");
    return nullptr;
  case '+':
    // No-op: +x == x for every numeric type. TypeChecker::visitUnary()
    // already rejected a non-numeric operand, so there's nothing to lower.
    return OperandV;
  case '~':
    // Bitwise NOT: ~x == x XOR -1 (all bits set)
    if (!OperandType->isNamed("int")) {
      return ConstantInt::get(*TheContext, APInt(64, 0, true));
    }
    return Builder->CreateNot(OperandV, "bitnottmp");
  default:
    llvm::report_fatal_error("Internal error: Unknown unary operator");
    return nullptr;
  }
}


// Address-of: &expr — returns the lvalue address (alloca ptr) of the operand.
// The operand must be addressable (variable, field, or array element).
Value *AddressOfExprAST::codegen() {
  Value *LVal = Operand->codegenLValue();
  if (!LVal) {
    LogErrorAt(Loc, "Cannot take the address of a non-addressable expression."
                    "\n  Hint: Only variables, struct fields, and array elements"
                    " are addressable.");
    return nullptr;
  }
  return LVal;
}


Value *SizeOfExprAST::codegen() {
  llvm::Type *LLVMTy = getLLVMType(TypeArg.get());
  if (!LLVMTy)
    return nullptr;
  uint64_t AllocSize = TheModule->getDataLayout().getTypeAllocSize(LLVMTy);
  return ConstantInt::get(Type::getInt64Ty(*TheContext), AllocSize);
}


Value *CastExprAST::codegen() {
  const TurfType *ExprTy = getExprType(Operand.get());
  
  Value *Val = Operand->codegen();
  if (!Val)
    return nullptr;
    
  TurfTypePtr SrcType;
  if (ExprTy) {
    // We only need the type info, so we can clone it or cast it if needed
    // But since CastExprAST only uses dynamic_cast and getName, we can just use a trick
    // to keep it compatible with TurfTypePtr.
    if (auto *Ptr = dynamic_cast<const TurfPointerType*>(ExprTy))
      SrcType = std::make_shared<TurfPointerType>(Ptr->getPointee());
    else if (ExprTy->isNamed("void*"))
      SrcType = std::make_shared<TurfPointerType>(TurfVoid());
    else if (ExprTy->isEnum()) {
      // Preserve the enum type pointer directly so isEnum() checks work below.
      SrcType = GlobalSymbolTable->LookupType(ExprTy->getName());
      if (!SrcType) SrcType = getTurfTypeFromLLVM(Val->getType());
    }
    else
      SrcType = getTurfTypeFromLLVM(Val->getType());
  } else {
    SrcType = getTurfTypeFromLLVM(Val->getType());
  }
  const std::string SrcName = ExprTy ? ExprTy->getName() : SrcType->getName();

  // Resolve DestType: NamedType may point to a registered enum/struct — look it
  // up at codegen time so isEnum() and isStruct() return the correct value.
  TurfTypePtr ResolvedDest = DestType;
  if (dynamic_cast<const NamedType *>(DestType.get()) && GlobalSymbolTable) {
    if (auto Registered = GlobalSymbolTable->LookupType(DestType->getName()))
      ResolvedDest = Registered;
  }
  const std::string DstName = ResolvedDest->getName();

  // Identity - no-op
  if (SrcName == DstName)
    return Val;

  // numeric → numeric
  if (SrcName == "int" && DstName == "double")
    return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), "itod");

  if (SrcName == "double" && DstName == "int")
    return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), "dtoi");

  // int32 -> int : sign-extend. Widening, no precision loss -- int32 exists
  // purely as an honest extern-fn ABI declaration type (see Int32Type's doc
  // comment in Types.h), so this mirrors int->double's implicit-widening
  // philosophy rather than needing any special narrowing guard.
  if (SrcName == "int32" && DstName == "int")
    return Builder->CreateSExt(Val, Type::getInt64Ty(*TheContext), "i32toi64");

  // int -> int32 : truncate. Explicit narrowing cast only, mirroring the
  // double->int narrowing-requires-explicit-cast philosophy above.
  if (SrcName == "int" && DstName == "int32")
    return Builder->CreateTrunc(Val, Type::getInt32Ty(*TheContext), "i64toi32");

  // int32 -> double / double -> int32 : same widen/narrow split as int<->double.
  if (SrcName == "int32" && DstName == "double")
    return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), "i32tod");
  if (SrcName == "double" && DstName == "int32")
    return Builder->CreateFPToSI(Val, Type::getInt32Ty(*TheContext), "dtoi32");

  // char -> int : zero-extend. char is treated as an unsigned byte (0-255),
  // not a signed C-style char, per STRING_UFCS_OPERATOR_CHAR_RFC.md §3's
  // "byte, not codepoint" framing -- predictable, no negative surprises.
  if (SrcName == "char" && DstName == "int")
    return Builder->CreateZExt(Val, Type::getInt64Ty(*TheContext), "chartoint");

  // int -> char : truncate. Explicit narrowing cast only, mirroring the
  // double->int narrowing-requires-explicit-cast philosophy above.
  if (SrcName == "int" && DstName == "char")
    return Builder->CreateTrunc(Val, Type::getInt8Ty(*TheContext), "inttochar");

  // Resolve both SrcType and DestType through GlobalSymbolTable so that
  // enum types (stored as NamedType at parse time) are correctly identified.
  // All primitive type names ("int", "double", etc.) return null from
  // LookupType and are handled by the existing branches.
  TurfTypePtr SrcEnum, DstEnum;
  if (GlobalSymbolTable) {
    if (auto R = GlobalSymbolTable->LookupType(SrcName))
      if (dynamic_cast<const TurfEnumType *>(R.get())) SrcEnum = R;
    if (auto R = GlobalSymbolTable->LookupType(DstName))
      if (dynamic_cast<const TurfEnumType *>(R.get())) DstEnum = R;
  }

  // enum → int : enums are stored as i64 — just zero-extend
  if (SrcEnum && DstName == "int")
    return Builder->CreateIntCast(Val, Type::getInt64Ty(*TheContext), false, "enum_to_int");

  // int → enum : reinterpret the integer as the enum's underlying type
  if (SrcName == "int" && DstEnum) {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(DstEnum.get());
    if (ET) {
      llvm::Type *LLVMTy = getLLVMType(ET->getUnderlyingType());
      return Builder->CreateIntCast(Val, LLVMTy, false, "int_to_enum");
    }
  }

  // string → int  : call strtoll(str, &endptr, 10)
  if (SrcName == "string" && DstName == "int") {
    Function *StrtollF = TheModule->getFunction("strtoll");
    Type *I8Ptr = PointerType::get(*TheContext, 0);
    if (!StrtollF) {
      Type *I8PtrPtr = PointerType::get(I8Ptr->getContext(), 0);
      FunctionType *FT =
          FunctionType::get(Type::getInt64Ty(*TheContext),
                            {I8Ptr, I8PtrPtr, Type::getInt32Ty(*TheContext)},
                            /*isVarArg=*/false);
      StrtollF = Function::Create(FT, Function::ExternalLinkage, "strtoll",
                                  TheModule.get());
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    AllocaInst *EndPtrAlloca = TmpB.CreateAlloca(I8Ptr, 0, "endptr");

    Value *Base = ConstantInt::get(Type::getInt32Ty(*TheContext), 10);
    Value *Res =
        Builder->CreateCall(StrtollF, {Val, EndPtrAlloca, Base}, "strtoll_res");

    Value *EndPtrVal = Builder->CreateLoad(I8Ptr, EndPtrAlloca, "endptr_val");
    Value *IsSame = Builder->CreateICmpEQ(EndPtrVal, Val, "is_same_ptr");

    BasicBlock *ErrorBB =
        BasicBlock::Create(*TheContext, "cast_error", TheFunction);
    BasicBlock *ContBB = BasicBlock::Create(*TheContext, "cast_cont");

    Builder->CreateCondBr(IsSame, ErrorBB, ContBB);

    Builder->SetInsertPoint(ErrorBB);

    CodegenPanic(Builder->CreateGlobalString(
        "Runtime Error: Invalid string to int conversion."), Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    TheFunction->insert(TheFunction->end(), ContBB);
    Builder->SetInsertPoint(ContBB);

    return Res;
  }

  // string → double : call strtod(str, nullptr)
  if (SrcName == "string" && DstName == "double") {
    Function *StrtodF = TheModule->getFunction("strtod");
    if (!StrtodF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      Type *I8PtrPtr = PointerType::get(I8Ptr->getContext(), 0);
      FunctionType *FT =
          FunctionType::get(Type::getDoubleTy(*TheContext), {I8Ptr, I8PtrPtr},
                            /*isVarArg=*/false);
      StrtodF = Function::Create(FT, Function::ExternalLinkage, "strtod",
                                 TheModule.get());
    }
    Value *NullPtr = ConstantPointerNull::get(
        PointerType::get(PointerType::get(*TheContext, 0)->getContext(), 0));
    return Builder->CreateCall(StrtodF, {Val, NullPtr}, "strtod_res");
  }

  // int → string : snprintf into a heap-allocated buffer.
  //
  // BUG FIX (was: TmpB.CreateAlloca):
  //   The previous implementation used alloca which places the buffer on the
  //   current function's stack frame. When this value is returned from a
  //   function (e.g. `fn make_label(n: int): string { return n to string; }`)
  //   the caller receives a pointer into a dead stack frame — silent UB.
  //   We now use malloc(32) so the buffer lives on the heap and survives the
  //   function return. The caller is responsible for freeing (no GC yet).
  if (SrcName == "int" && DstName == "string") {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(
          Type::getInt32Ty(*TheContext),
          {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr}, /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf",
                                   TheModule.get());
    }
    Function *MallocF = TheModule->getFunction("malloc");
    if (!MallocF) {
      FunctionType *FT =
          FunctionType::get(PointerType::get(*TheContext, 0),
                            {Type::getInt64Ty(*TheContext)}, false);
      MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc",
                                 TheModule.get());
    }
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    Value *Buf = Builder->CreateCall(MallocF, {BufSize}, "int_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%lld", "intfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_int");
    return Buf;
  }

  // double → string : snprintf into a heap-allocated buffer.
  //
  // BUG FIX (was: TmpB.CreateAlloca):
  //   Same root cause as int → string above. Stack alloca returns dangling
  //   pointer when the result escapes the function. Fixed with malloc(32).
  if (SrcName == "double" && DstName == "string") {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(
          Type::getInt32Ty(*TheContext),
          {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr}, /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf",
                                   TheModule.get());
    }
    Function *MallocF = TheModule->getFunction("malloc");
    if (!MallocF) {
      FunctionType *FT =
          FunctionType::get(PointerType::get(*TheContext, 0),
                            {Type::getInt64Ty(*TheContext)}, false);
      MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc",
                                 TheModule.get());
    }
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    Value *Buf = Builder->CreateCall(MallocF, {BufSize}, "dbl_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%g", "dblfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_dbl");
    return Buf;
  }

  // bool → string : select(val, "true", "false")
  if (SrcName == "bool" && DstName == "string") {
    Value *TrueStr = Builder->CreateGlobalString("true", "str_true");
    Value *FalseStr = Builder->CreateGlobalString("false", "str_false");
    return Builder->CreateSelect(Val, TrueStr, FalseStr, "bool_to_str");
  }

  // enum -> string : switch on the integer value, each case returns the name
  // IR layout:
  //   [current block]: switch val, default=defaultBB, [case i -> caseBB_i]
  //   defaultBB:  store "<unknown enum>", br mergeBB
  //   caseBB_0:   store "North",   br mergeBB
  //   caseBB_1:   store "East",    br mergeBB
  //   ...
  //   mergeBB:    phi [str from each case]
  if (SrcEnum && DstName == "string") {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(SrcEnum.get());
    if (!ET) return nullptr;

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    llvm::Type *UnderlyingLLVMTy = getLLVMType(ET->getUnderlyingType());

    // Destination blocks
    BasicBlock *DefaultBB = BasicBlock::Create(*TheContext, "enum_str_def", TheFunction);
    BasicBlock *MergeBB   = BasicBlock::Create(*TheContext, "enum_str_merge", TheFunction);

    // Emit switch in current block
    SwitchInst *SI = Builder->CreateSwitch(Val, DefaultBB, ET->numVariants());

    // Default block: unknown enum value
    Builder->SetInsertPoint(DefaultBB);
    Value *UnknownStr = Builder->CreateGlobalString("<unknown enum>", "str_unk");
    Builder->CreateBr(MergeBB);

    // PHI in merge block — add default incoming first
    Builder->SetInsertPoint(MergeBB);
    PHINode *PN = Builder->CreatePHI(PointerType::getUnqual(*TheContext),
                                     ET->numVariants() + 1, "enum_str_phi");
    PN->addIncoming(UnknownStr, DefaultBB);

    // Per-variant case blocks
    for (const auto& V : ET->getVariants()) {
      BasicBlock *CaseBB = BasicBlock::Create(*TheContext, "enum_case_" + V, TheFunction);
      ConstantInt *CaseVal = cast<ConstantInt>(
          ConstantInt::get(UnderlyingLLVMTy, ET->getVariantValue(V)));
      SI->addCase(CaseVal, CaseBB);

      Builder->SetInsertPoint(CaseBB);
      Value *VariantStr = Builder->CreateGlobalString(V, "str_" + V);
      Builder->CreateBr(MergeBB);
      PN->addIncoming(VariantStr, CaseBB);
    }

    Builder->SetInsertPoint(MergeBB);
    return PN;
  }

  // string -> enum : strcmp chain starting from the current block
  // IR layout:
  //   [current block]:  cmp "North", condBr matchBB_0, nextBB_0
  //   matchBB_0:        br mergeBB  (value = 0)
  //   nextBB_0:         cmp "East",  condBr matchBB_1, nextBB_1
  //   ...
  //   nextBB_last:      br failBB
  //   failBB:           call fail()
  //   mergeBB:          phi [values from each match]
  if (SrcName == "string" && DstEnum) {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(DstEnum.get());
    if (!ET) return nullptr;

    Function *StrcmpF = TheModule->getFunction("strcmp");
    if (!StrcmpF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(Type::getInt32Ty(*TheContext),
                                           {I8Ptr, I8Ptr}, false);
      StrcmpF = Function::Create(FT, Function::ExternalLinkage, "strcmp",
                                 TheModule.get());
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    llvm::Type *UnderlyingLLVMTy = getLLVMType(ET->getUnderlyingType());

    // KEY: save the entry block BEFORE moving the insert point anywhere.
    BasicBlock *EntryBB = Builder->GetInsertBlock();

    // Create terminal blocks (appended to function, filled below)
    BasicBlock *FailBB  = BasicBlock::Create(*TheContext, "enum_parse_fail",  TheFunction);
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "enum_parse_merge", TheFunction);

    // Build PHI in MergeBB
    Builder->SetInsertPoint(MergeBB);
    PHINode *PN = Builder->CreatePHI(UnderlyingLLVMTy, ET->numVariants(), "enum_parse_phi");

    // Build fail block
    Builder->SetInsertPoint(FailBB);
    CodegenPanic(Builder->CreateGlobalString(
        "Runtime Error: Invalid string to enum conversion."), Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    // Build the strcmp chain directly from EntryBB (saved before any SetInsertPoint calls)
    BasicBlock *CurrentBB = EntryBB;
    for (const auto& V : ET->getVariants()) {
      BasicBlock *MatchBB = BasicBlock::Create(*TheContext, "enum_parse_match_" + V, TheFunction, FailBB);
      BasicBlock *NextBB  = BasicBlock::Create(*TheContext, "enum_parse_next_"  + V, TheFunction, FailBB);

      Builder->SetInsertPoint(CurrentBB);
      Value *VariantStr = Builder->CreateGlobalString(V, "cmpstr_" + V);
      Value *CmpRes  = Builder->CreateCall(StrcmpF, {Val, VariantStr}, "strcmp_" + V);
      Value *IsMatch = Builder->CreateICmpEQ(
          CmpRes, ConstantInt::get(Type::getInt32Ty(*TheContext), 0), "match_" + V);
      Builder->CreateCondBr(IsMatch, MatchBB, NextBB);

      Builder->SetInsertPoint(MatchBB);
      ConstantInt *CaseVal = cast<ConstantInt>(
          ConstantInt::get(UnderlyingLLVMTy, ET->getVariantValue(V)));
      Builder->CreateBr(MergeBB);
      PN->addIncoming(CaseVal, MatchBB);

      CurrentBB = NextBB;
    }

    // Last "next" block falls to fail
    Builder->SetInsertPoint(CurrentBB);
    Builder->CreateBr(FailBB);

    Builder->SetInsertPoint(MergeBB);
    return PN;
  }

    // (moved unsupported string cast down)

  // int -> pointer : inttoptr
  if (SrcName == "int" && dynamic_cast<const TurfPointerType*>(DestType.get())) {
    return Builder->CreateIntToPtr(Val, getLLVMType(DestType.get()), "inttoptr");
  }

  // pointer -> pointer : bitcast
  bool isSrcPtr = (ExprTy && dynamic_cast<const TurfPointerType*>(ExprTy)) || SrcName == "ptr" || SrcName == "string";
  bool isDstPtr = dynamic_cast<const TurfPointerType*>(DestType.get()) || DstName == "ptr" || DstName == "string";
  
  if (isSrcPtr && isDstPtr) {
    return Builder->CreateBitCast(Val, getLLVMType(DestType.get()), "ptrcast");
  }

  // Unsupported → string
  if (DstName == "string") {
    StringConversionError(Loc, SrcName.c_str()).raise();
    return nullptr;
  }

  // Everything else is unsupported
  TypeError(Loc, std::string("Cannot explicitly cast '") + SrcName + "' to '" +
                     DstName + "'")
      .raise();
  return nullptr;
}
