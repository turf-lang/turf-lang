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

#include "codegen/Codegen.h"
#include "frontend/AST.h"
#include "analysis/Algorithms.h"
#include "sema/Builtins.h"
#include "analysis/CFG.h"
#include "diagnostics/Errors.h"
#include "analysis/Analysis.h"
#include "analysis/AliasAnalysis.h"
#include "frontend/Lexer.h"
#include "analysis/ValueRange.h"
#include "analysis/Lint.h"
#include "resolve/SymbolTable.h"
#include "sema/Types.h"
#include "codegen/CodegenInternal.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;


// Turns a number to LLVM Number constant. SEM-001
// (docs/planning/ARCHITECTURE_OVERHAUL.md §8): builds the LLVM constant
// directly from the node's own exact APSInt/APFloat rather than round-
// tripping through the narrow getIntVal()/getDoubleVal() convenience
// accessors -- see NumberExprAST's own doc comment (ASTExpr.h).
//
// `int` is a fixed 64-bit LLVM type regardless of how many bits IntVal_
// actually needed to hold the literal exactly (a wider-than-64-bit literal,
// e.g. the 48-digit example in §10.2, is representationally impossible to
// avoid narrowing here) -- extOrTrunc(64) makes that narrowing explicit and
// well-defined (never UB) rather than relying on getIntVal()'s own
// already-safe-but-indirect truncation. Out-of-range representability is
// deliberately NOT checked here or anywhere else in SEM-001 (SEM-002's
// job, per §8's completion criteria: an out-of-range literal must still
// compile in this group, carrying its exact value up to this exact point).
Value *NumberExprAST::codegen() {
  if (isInteger())
    return ConstantInt::get(*TheContext, getIntValExact().extOrTrunc(64));

  // APFloat is how LLVM represents floating point numbers internally --
  // getFloatValExact() already IS one (IEEEdouble semantics), so this is a
  // direct pass-through, not a re-derivation from a plain C++ double.
  return ConstantFP::get(*TheContext, getFloatValExact());
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

  // ── Unary `operator!` dispatch (LANG-001) ────────────────────────────────
  // TypeChecker::visitBinary() already picked ResolvedNotOperatorTarget over
  // the ordinary operator!=/builtin path when this node is a user-written
  // `!x` whose operand type declares a unary `operator!` -- see that
  // field's doc comment (ASTExpr.h). Only L (the real operand) is passed;
  // R is the synthetic `true` literal, already evaluated above for no
  // reason other than every other branch of this function unconditionally
  // evaluating both sides first (harmless -- a BoolExprAST::codegen() is a
  // single constant, not a side effect).
  if (ResolvedNotOperatorTarget) {
    std::string MangledName = ResolvedNotOperatorTarget->mangledName();
    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: operator overload '" +
                          MangledName + "' was resolved but never codegenned."));
      return nullptr;
    }
    return CallOperator(CalleeF, *ResolvedNotOperatorTarget, {L}, Loc, "nottmp");
  }

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
    return CallOperator(CalleeF, *ResolvedOperatorTarget, {L, R}, Loc, "optmp");
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

    // Arithmetic. int (never int32/double) gets runtime overflow trapping
    // -- FREEZE.md §6.2 row 7 ("Runtime, signed integer: trap"). int32 is
    // deliberately excluded: it exists solely for extern-fn ABI honesty
    // (§6.2), and arithmetic on it isn't named anywhere in SEM-002's own
    // scope -- see this group's change record for the explicit scoping
    // note. A `TurfGenericParamType`-typed operand never reaches this
    // branch at all: before TYPE-002, its erased runtime representation
    // resolves to something other than "int" via getTurfTypeFromLLVM()
    // above, so NumericType->isNamed("int") is naturally false for it --
    // no separate exclusion needed (see §8 SEM-002's own "Generic
    // interactions" note on why this group must not touch that case).
  // OPT-002: ProvenNoOverflow is set by ValueRangePass::markElidableChecks()
  // (ValueRange.cpp) BEFORE Body->codegen() runs, when both operands proved
  // to be the same known constants on every path reaching this node, AND
  // the exact checked result is representable -- so `nsw` is sound here
  // specifically BECAUSE the trap was already proved unreachable (F-OPT-2,
  // §17.3), never attached speculatively.
  case '+':
    CastBoth(NumericType);
    if (NumericType->isNamed("double"))
      return Builder->CreateFAdd(L, R, "addtmp");
    if (NumericType->isNamed("int"))
      return ProvenNoOverflow ? Builder->CreateNSWAdd(L, R, "addtmp")
                              : EmitCheckedIntArith('+', L, R, Loc);
    return Builder->CreateAdd(L, R, "addtmp");

  case '-':
    CastBoth(NumericType);
    if (NumericType->isNamed("double"))
      return Builder->CreateFSub(L, R, "subtmp");
    if (NumericType->isNamed("int"))
      return ProvenNoOverflow ? Builder->CreateNSWSub(L, R, "subtmp")
                              : EmitCheckedIntArith('-', L, R, Loc);
    return Builder->CreateSub(L, R, "subtmp");

  case '*':
    CastBoth(NumericType);
    if (NumericType->isNamed("double"))
      return Builder->CreateFMul(L, R, "multmp");
    if (NumericType->isNamed("int"))
      return ProvenNoOverflow ? Builder->CreateNSWMul(L, R, "multmp")
                              : EmitCheckedIntArith('*', L, R, Loc);
    return Builder->CreateMul(L, R, "multmp");

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

    // The one signed-division overflow case (INT64_MIN / -1) -- see
    // EmitSignedDivOverflowGuard()'s own doc comment. int32/double are
    // scoped out for the same reason noted above the '+' case.
    if (NumericType->isNamed("int"))
      EmitSignedDivOverflowGuard(L, R, Loc);

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

    // INT_MIN % -1 is 0, never poison -- see EmitSafeSRem().
    return NumericType->isNamed("double") ? Builder->CreateFRem(L, R, "modtmp")
                                          : EmitSafeSRem(L, R, "modtmp");
  }

  // Comparison. A `char` is an unsigned byte (0-255, variables_and_types.md),
  // so its ordering predicates are unsigned: with signed ones `(200 to char)
  // > 'a'` was false, though `char -> int` zero-extends (found by turf-fuzz).
  case '<': {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    if (CmpType->isNamed("char"))
      return Builder->CreateICmpULT(L, R, "cmptmp");
    return CmpType->isNamed("double") ? Builder->CreateFCmpOLT(L, R, "cmptmp")
                                      : Builder->CreateICmpSLT(L, R, "cmptmp");
  }

  case '>': {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    if (CmpType->isNamed("char"))
      return Builder->CreateICmpUGT(L, R, "cmptmp");
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
    if (CmpType->isNamed("char"))
      return Builder->CreateICmpUGE(L, R, "cmptmp");
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGE(L, R, "cmptmp")
                                      : Builder->CreateICmpSGE(L, R, "cmptmp");
  }

  case TOK_LEQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    if (CmpType->isNamed("char"))
      return Builder->CreateICmpULE(L, R, "cmptmp");
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

  // Bitwise left shift -- checked (FREEZE.md §6.2 row 7 names '<<'
  // explicitly alongside +/-/*). Both operands are already 'int' by the
  // time TypeChecker::visitBinary() accepts a shift, so no NumericType
  // gate is needed here the way +/-/* need one (those also accept
  // double).
  case TOK_LSHIFT:
    // OPT-002: see the '+'/'-'/'*' cases above for ProvenNoOverflow's
    // contract. Shl has no `with.overflow` intrinsic to build a plain
    // variant from -- the checked-safe result IS a plain CreateShl.
    return ProvenNoOverflow ? Builder->CreateShl(L, R, "lshifttmp")
                            : EmitCheckedShl(L, R, Loc);

  // Bitwise right shift (arithmetic, preserves sign) -- amount checked,
  // see EmitCheckedShr().
  case TOK_RSHIFT:
    return EmitCheckedShr(L, R, Loc);

  // Exponentiation: ** (replaces the old ^ operator). Exact for int on both
  // sides; llvm.pow on doubles once either side is double (visitBinary()).
  case TOK_POWER: {
    if (!NumericType->isNamed("double")) {
      CastBoth(TurfInt());
      return EmitCheckedIntPow(L, R, Loc);
    }
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

  // ── Operator overload dispatch (LANG-001) ────────────────────────────────
  // TypeChecker::visitUnary() already resolved ResolvedOperatorTarget (if
  // any) via exact operand-type match -- mirrors BinaryExprAST::codegen()'s
  // identical dispatch. Checked BEFORE getTurfTypeFromLLVM() below: that
  // function is the erasure-era LLVM-type-guessing fallback (§9.7/D18) and
  // has no reliable answer for a struct operand anyway -- irrelevant once a
  // real operator overload applies.
  if (ResolvedOperatorTarget) {
    std::string MangledName = ResolvedOperatorTarget->mangledName();
    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: operator overload '" +
                          MangledName + "' was resolved but never codegenned."));
      return nullptr;
    }
    return CallOperator(CalleeF, *ResolvedOperatorTarget, {OperandV}, Loc, "unaryoptmp");
  }

  TurfTypePtr OperandType = getTurfTypeFromLLVM(OperandV->getType());
  if (OperandType->isNamed("bool"))
    OperandV = CastToType(OperandV, TurfInt(), "boolneg");

  OperandType = getTurfTypeFromLLVM(OperandV->getType());

  switch (Opcode) {
  case '-':
    if (OperandType->isNamed("double"))
      return Builder->CreateFNeg(OperandV);
    if (OperandType->isNamed("int")) {
      // Unary '-' applied DIRECTLY to an untyped integer literal folds
      // into the literal itself, before any runtime check -- FREEZE.md
      // §6.2's `-9223372036854775808` example (ARCHITECTURE_OVERHAUL.md
      // §10.2), explicitly deferred here by SEM-001 (see ParserLiteral.cpp's
      // own doc comment on ParseNumberExpr). Without this special case,
      // negating the ALREADY-64-BIT-TRUNCATED magnitude via the ordinary
      // checked-negate path below would incorrectly TRAP for exactly this
      // one legitimate input: NumberExprAST::codegen() truncates the
      // literal `9223372036854775808` (2^63) to a 64-bit value whose bit
      // pattern is indistinguishable from a genuine INT64_MIN, so
      // EmitCheckedIntNegate() would (wrongly) read it as "negating
      // INT64_MIN" and panic. TypeChecker::checkLiteralRepresentable()
      // (SEM-002) is what actually enforces representability for this
      // shape; this is purely correct bit-level construction of an
      // already-validated (or, for a stray unchecked occurrence, honestly
      // truncated -- mirroring NumberExprAST::codegen()'s own contract)
      // literal value, never a runtime-checked operation.
      if (const auto *NE = dynamic_cast<const NumberExprAST *>(Operand.get())) {
        if (NE->isInteger() && NE->isUntyped()) {
          const llvm::APSInt &Exact = NE->getIntValExact();
          llvm::APInt Widened = llvm::APInt(Exact).sext(Exact.getBitWidth() + 1);
          llvm::APInt Negated = -Widened;
          return ConstantInt::get(*TheContext, Negated.sextOrTrunc(64));
        }
      }
      // Checked (FREEZE.md §6.2 row 7 names negation alongside +/-/*):
      // negating a genuine runtime INT64_MIN value is the one input with
      // no representable int64 result -- see EmitCheckedIntNegate()'s own
      // doc comment. OPT-002: ProvenNoOverflow (see BinaryExprAST's cases,
      // CodegenExpr.cpp) means the operand is already known != INT64_MIN.
      if (ProvenNoOverflow)
        return Builder->CreateNSWNeg(OperandV, "negtmp");
      return EmitCheckedIntNegate(OperandV, Loc);
    }
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
    LogErrorAt(Loc, ErrCode::NonAddressableExpression,
               "Cannot take the address of a non-addressable expression."
                    "\n  Hint: Only variables, struct fields, and array elements"
                    " are addressable.", SLM::ErrorCategory::GENERAL);
    return nullptr;
  }
  return LVal;
}


Value *SizeOfExprAST::codegen() {
  llvm::Type *LLVMTy = getLLVMType(TypeArg.get());
  if (!LLVMTy)
    return nullptr;
  // TypeChecker::visitSizeOf() rejects `sizeof(void)`; getTypeAllocSize() on
  // an unsized type loops forever inside LLVM rather than failing, so never
  // hand it one even if a future type slips past the checker.
  if (!LLVMTy->isSized()) {
    LogErrorAt(Loc, ErrCode::InternalCompilerError,
               "Internal error: sizeof of an unsized type reached codegen",
               SLM::ErrorCategory::GENERAL);
    return nullptr;
  }
  uint64_t AllocSize = TheModule->getDataLayout().getTypeAllocSize(LLVMTy);
  return ConstantInt::get(Type::getInt64Ty(*TheContext), AllocSize);
}


// STD-002: TypeChecker::visitCallerLocation() already guaranteed this node
// is lexically inside a `@caller_location` function's body, so
// BuildCallerLocationValue() always takes its "forward" branch here --
// never its "stamp a literal" branch, which exists for the call-site
// injection case instead (FuncCallExprAST::codegen(), CodegenFunc.cpp).
Value *CallerLocationExprAST::codegen() {
  return BuildCallerLocationValue(Loc);
}


Value *CastExprAST::codegen() {
  const TurfType *ExprTy = getExprType(Operand.get());

  // ─── MEM-002 ────────────────────────────────────────────────────────────
  // `gc_alloc(n)`/`gc_alloc_zeroed(n)`/`gc_realloc(p, n)` cast directly
  // `to T*` -- every collection-growth call site in list/stack/queue/set/
  // map.tr is written exactly this way, inside a monomorphized clone where
  // T is now a concrete type (TYPE-002). Inject a real per-element
  // TurfTypeDesc here (instead of MEM-001's literal NULL) so the collector
  // can trace T-shaped pointer words inside the allocated block -- see
  // docs/development/changes/MEM-001.md's *Removed assumptions* for why
  // this couldn't be done before this group existed (no array-shaped
  // descriptor was designed yet). Must run BEFORE `Operand->codegen()`
  // below: that call is what actually dispatches into
  // FuncCallExprAST::codegen()'s gc_alloc/gc_alloc_zeroed/gc_realloc
  // branch (CodegenFunc.cpp), which reads and consumes
  // PendingGCAllocDescOverride while building the call -- there is no
  // other hook point between "we know the destination element type" and
  // "the call is built" for a bare intrinsic with no ResolvedCallTarget to
  // attach this to (CodegenInternal.h's own doc comment on the override).
  // A `gc_alloc(n) to string` site (strings.tr/io.tr/net.tr) is NOT this
  // shape (`string` is its own StringType, never a TurfPointerType, so the
  // dynamic_cast below simply fails to match) and correctly falls through
  // to the original NULL-desc path -- MEM-002 defines that as "opaque, no
  // pointer words", which is precise, not a fallback, for a raw byte
  // buffer.
  if (auto *FC = dynamic_cast<const FuncCallExprAST *>(Operand.get())) {
    if (FC->isIntrinsic(BareIntrinsic::GcAlloc) ||
        FC->isIntrinsic(BareIntrinsic::GcAllocZeroed) ||
        FC->isIntrinsic(BareIntrinsic::GcRealloc)) {
      if (auto *DestPtr = dynamic_cast<const TurfPointerType *>(DestType.get())) {
        llvm::Type *ElemTy = getLLVMType(DestPtr->getPointee().get());
        if (ElemTy && !ElemTy->isVoidTy()) {
          PendingGCAllocDescOverride = getOrCreateElementTurfTypeDesc(ElemTy);
        }
      }
    }
  }

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
    return EmitCheckedFPToSI(Val, Type::getInt64Ty(*TheContext), Loc, "dtoi");

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
    return EmitCheckedFPToSI(Val, Type::getInt32Ty(*TheContext), Loc, "dtoi32");

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

  // int → enum : checked against the variant range [0, numVariants), then
  // reinterpreted as the enum's underlying type. Unchecked, `7 to En` was a
  // value with no variant that slipped past an exhaustive `compare` (owner
  // decision 2026-10-12, found by turf-fuzz); a constant literal out of
  // range is already E127 in visitCast(). One unsigned
  // compare also catches a negative value.
  if (SrcName == "int" && DstEnum) {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(DstEnum.get());
    if (ET) {
      Function *TheFunction = Builder->GetInsertBlock()->getParent();
      Value *Count = ConstantInt::get(Val->getType(), ET->numVariants());
      BasicBlock *BadBB = BasicBlock::Create(*TheContext, "int_to_enum_bad", TheFunction);
      BasicBlock *OkBB = BasicBlock::Create(*TheContext, "int_to_enum_ok", TheFunction);
      Builder->CreateCondBr(Builder->CreateICmpUGE(Val, Count, "int_to_enum_oob"),
                            BadBB, OkBB);
      Builder->SetInsertPoint(BadBB);
      CodegenPanic(Builder->CreateGlobalString(
          "Runtime Error: Invalid int to enum conversion."), Loc);
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateUnreachable();
      Builder->SetInsertPoint(OkBB);
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

  // int → string : snprintf into a GC-heap-allocated buffer.
  //
  // BUG FIX (was: TmpB.CreateAlloca):
  //   The previous implementation used alloca which places the buffer on the
  //   current function's stack frame. When this value is returned from a
  //   function (e.g. `fn make_label(n: int): string { return n to string; }`)
  //   the caller receives a pointer into a dead stack frame — silent UB.
  //   We now use turf_rt_alloc(32, NULL) so the buffer lives on the GC heap
  //   and survives the function return.
  //
  // OVH-002 follow-up (docs/development/changes/OVH-002.md): this used
  // to be a bare `malloc(32)` -- never registered with the collector, so a
  // `string` this cast produced was permanently invisible to gc_collect(),
  // contradicting FREEZE.md §6.5 ("string values produced by any operation"
  // belong on the GC heap) and leaking forever rather than being reclaimed
  // once unreachable. `desc == NULL` is correct here, not a placeholder --
  // this buffer holds raw formatted digits, never an outgoing pointer,
  // exactly like strings.tr's/io.tr's own raw byte buffers.
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
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    Value *NullDesc = ConstantPointerNull::get(PointerType::get(*TheContext, 0));
    Value *Buf = Builder->CreateCall(getOrCreateTurfRtAllocFn(), {BufSize, NullDesc}, "int_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%lld", "intfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_int");
    return Buf;
  }

  // double → string : snprintf into a GC-heap-allocated buffer.
  //
  // BUG FIX (was: TmpB.CreateAlloca):
  //   Same root cause as int → string above. Stack alloca returns dangling
  //   pointer when the result escapes the function. Fixed with
  //   turf_rt_alloc(32, NULL) -- see the int → string branch's own comment
  //   (OVH-002) for why a bare `malloc` here was a real bug, not just a
  //   style inconsistency.
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
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    Value *NullDesc = ConstantPointerNull::get(PointerType::get(*TheContext, 0));
    Value *Buf = Builder->CreateCall(getOrCreateTurfRtAllocFn(), {BufSize, NullDesc}, "dbl_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%g", "dblfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_dbl");
    // A NaN is "nan" whatever its sign bit (owner decision 2026-10-12). %g
    // prints the sign, and that bit differs between run-time arithmetic
    // ("-nan" from x86's default NaN) and constant folding ("nan"), so the
    // text changed with -O (found by turf-fuzz). The choice is made on the
    // string, never by rewriting the value, which LLVM may fold away.
    Value *IsNaN = Builder->CreateFCmpUNO(Val, Val, "dbl_isnan");
    return Builder->CreateSelect(IsNaN, Builder->CreateGlobalString("nan", "str_nan"),
                                 Buf, "dbl_to_str");
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
  TypeError(Loc, ErrCode::InvalidCast, std::string("Cannot explicitly cast '") + SrcName + "' to '" +
                     DstName + "'")
      .raise();
  return nullptr;
}
