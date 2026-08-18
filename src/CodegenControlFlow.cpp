/**
 * @file CodegenControlFlow.cpp
 * @brief Control-flow codegen: if/block/while/do-while/for/for-each/break/continue/compare.
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

// Stack of (condition-block, after-block) pairs for the innermost enclosing
// loop(s) -- consulted by BreakExprAST/ContinueExprAST to know which block
// to jump to. Only ever pushed/popped by the loop-codegen methods below
// (While/DoWhile/For/ForEach), all in this same file.
std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> LoopBlocks;


Value *IfExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  bool IsBlockForm =
      dynamic_cast<BlockExprAST *>(Branches.front().Body.get()) != nullptr;
  bool HasElse = (ElseBody != nullptr);

  // Block-form if/elseif/else: statement, no PHI node needed.
  if (IsBlockForm) {
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    for (size_t i = 0; i < Branches.size(); ++i) {
      Value *CondV = Branches[i].Cond->codegen();
      if (!CondV)
        return nullptr;
      CondV = CastToType(CondV, TurfBool(), "ifcond");

      BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
      BasicBlock *NextBB = BasicBlock::Create(*TheContext, "else");

      Builder->CreateCondBr(CondV, ThenBB, NextBB);

      // Emit then body
      Builder->SetInsertPoint(ThenBB);
      Branches[i].Body->codegen();
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);

      // NextBB becomes the insert point for the next condition or the
      // else/merge
      TheFunction->insert(TheFunction->end(), NextBB);
      Builder->SetInsertPoint(NextBB);
    }

    // We're now in the final "else" block
    if (HasElse) {
      ElseBody->codegen();
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);
    } else {
      // No else: just branch to merge
      Builder->CreateBr(MergeBB);
    }

    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  // Ternary form: if cond then expr else expr
  Value *CondV = Branches[0].Cond->codegen();
  if (!CondV)
    return nullptr;
  CondV = CastToType(CondV, TurfBool(), "ifcond");

  BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

  Builder->CreateCondBr(CondV, ThenBB, ElseBB);
  Builder->SetInsertPoint(ThenBB);

  Value *ThenV = Branches[0].Body->codegen();
  if (!ThenV)
    return nullptr;
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = ElseBody->codegen();
  if (!ElseV)
    return nullptr;
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);

  TurfTypePtr ThenType = getTurfTypeFromLLVM(ThenV->getType());
  TurfTypePtr ElseType = getTurfTypeFromLLVM(ElseV->getType());
  TurfTypePtr MergeType = getCommonType(ThenType, ElseType);

  ThenV = CastToType(ThenV, MergeType, "thencast");
  ElseV = CastToType(ElseV, MergeType, "elsecast");

  PHINode *PN = Builder->CreatePHI(ThenV->getType(), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}


Value *BlockExprAST::codegen() {
  // Enter new scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // A statement whose codegen() returns nullptr without having raised its
  // own diagnostic first is a genuine, silent compiler failure -- stop
  // processing the rest of this block instead of quietly dropping the
  // failing statement from the compiled program. This closes the
  // "BlockExprAST::codegen() never checks for a null statement result" gap
  // tracked in the now-removed docs/planning/CODE_REVIEW_GAPS_2026-07.md
  // (see git history for the original writeup). This relies on every "processed
  // StructDeclExprAST, EnumDeclExprAST, ErrorDeclExprAST, an `export
  // immut` VarDeclExprAST) returning a non-null sentinel instead of a
  // literal nullptr -- see each of their own codegen()'s doc comments.
  // A block is exactly the context that would otherwise mask this: it's
  // the only place multiple top-level-style statements (a module's own
  // functions/structs/etc, reached via ModuleImportExprAST::codegen()'s
  // Body->codegen()) get blindly iterated and their individual return
  // values ignored except for the last one.
  Value *LastVal = nullptr;
  for (auto &Expr : Expressions) {
    LastVal = Expr->codegen();
    if (!LastVal) {
      if (GlobalSymbolTable) {
        GlobalSymbolTable->ExitScope();
      }
      return nullptr;
    }
  }

  // Exit scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // An empty block is valid - it just produces no meaningful value.
  // Return a dummy zero so callers (e.g. IfExprAST::codegen) don't
  // mistake it for a codegen failure and bail out mid-IR-construction.
  if (!LastVal)
    LastVal = Constant::getNullValue(Type::getInt64Ty(*TheContext));

  return LastVal;
}


Value *UnsafeBlockExprAST::codegen() {
  // Pure pass-through -- by Codegen time the TypeChecker has already
  // rejected anything the `unsafe` gate disallows (issue #238), so there's
  // no unsafe-tracking to do here, only the same-representation Body to
  // codegen. Body is always a BlockExprAST (see ParseUnsafeExpr()), which
  // already opens/closes its own scope, so this doesn't need to.
  return Body->codegen();
}


Value *WhileExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  BasicBlock *LoopCondBB =
      BasicBlock::Create(*TheContext, "loopcond", TheFunction);
  BasicBlock *LoopBodyBB = BasicBlock::Create(*TheContext, "loopbody");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop");

  Builder->CreateBr(LoopCondBB);
  Builder->SetInsertPoint(LoopCondBB);

  Value *CondV = Cond->codegen();
  if (!CondV)
    return nullptr;

  CondV = CastToType(CondV, TurfBool(), "loopcond");

  // Conditional Branch: if true -> Body, else -> After
  Builder->CreateCondBr(CondV, LoopBodyBB, AfterBB);

  // Loop Body Block
  TheFunction->insert(TheFunction->end(), LoopBodyBB);
  Builder->SetInsertPoint(LoopBodyBB);

  LoopBlocks.push_back({LoopCondBB, AfterBB});

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    return nullptr;
  }

  LoopBlocks.pop_back();

  // Jump back to the condition to loop again
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // After Loop Block
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // While loops always return 0.0
  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}


Value *DoWhileExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  BasicBlock *LoopBodyBB =
      BasicBlock::Create(*TheContext, "loopbody", TheFunction);
  BasicBlock *LoopCondBB = BasicBlock::Create(*TheContext, "loopcond");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop");

  // Entry jumps straight into the body — no condition check on first entry.
  Builder->CreateBr(LoopBodyBB);
  Builder->SetInsertPoint(LoopBodyBB);

  // continue -> re-check the condition; break -> after the loop.
  LoopBlocks.push_back({LoopCondBB, AfterBB});

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    return nullptr;
  }

  LoopBlocks.pop_back();

  // Fall through to the condition check if the body didn't already terminate.
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // Condition Block
  TheFunction->insert(TheFunction->end(), LoopCondBB);
  Builder->SetInsertPoint(LoopCondBB);

  Value *CondV = Cond->codegen();
  if (!CondV)
    return nullptr;

  CondV = CastToType(CondV, TurfBool(), "loopcond");

  // Conditional Branch: if true -> loop back to Body, else -> After
  Builder->CreateCondBr(CondV, LoopBodyBB, AfterBB);

  // After Loop Block
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // do-while loops always return 0.0, matching while's convention.
  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}


// ForExprAST::codegen : desugars to init + while(cond) { body; step }
Value *ForExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  //  1. Init: declare loop variable with start value.
  //     Evaluate start/end in the outer scope first — they cannot reference
  //     the loop variable (it doesn't exist yet).
  Value *StartV = Start->codegen();
  if (!StartV)
    return nullptr;

  // Determine the loop variable type from the start value
  TurfTypePtr VarType =
      StartV->getType()->isDoubleTy() ? TurfDouble() : TurfInt();

  StartV = CastToType(StartV, VarType, "forstart");

  AllocaInst *Alloca =
      CreateEntryBlockAlloca(TheFunction, VarName, VarType.get());
  Builder->CreateStore(StartV, Alloca);

  // Push a new scope in GlobalSymbolTable so the loop variable doesn't leak
  // out into the surrounding scope after the loop ends.
  // Previously, DeclareSymbol was called without EnterScope, causing the loop
  // variable to persist in the caller's scope — now fixed.
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // Register loop variable in symbol table and NamedValues.
  // Save old NamedValues binding for backward-compatible restore after loop.
  auto OldBinding = NamedValues.find(VarName);
  VarInfo SavedBinding;
  bool HadOldBinding = false;
  if (OldBinding != NamedValues.end()) {
    SavedBinding = OldBinding->second;
    HadOldBinding = true;
  }
  NamedValues[VarName] = {Alloca, VarType};

  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(VarName, VarType, Loc, Alloca);
  }

  //  2. Evaluate end value
  Value *EndV = End->codegen();
  if (!EndV) {
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }
  EndV = CastToType(EndV, VarType, "forend");

  // Store end value so it doesn't get re-evaluated each iteration
  AllocaInst *EndAlloca =
      CreateEntryBlockAlloca(TheFunction, VarName + ".end", VarType.get());
  Builder->CreateStore(EndV, EndAlloca);

  //  3. Determine loop direction: start < end → ascending, else descending
  Value *Ascending;
  if (VarType->isNamed("int")) {
    Ascending = Builder->CreateICmpSLT(StartV, EndV, "ascending");
  } else {
    Ascending = Builder->CreateFCmpOLT(StartV, EndV, "ascending");
  }

  // Store direction flag
  AllocaInst *AscAlloca =
      CreateEntryBlockAlloca(TheFunction, VarName + ".asc", TurfBool().get());
  Builder->CreateStore(Ascending, AscAlloca);

  //  4. Create basic blocks
  BasicBlock *LoopCondBB =
      BasicBlock::Create(*TheContext, "for.cond", TheFunction);
  BasicBlock *LoopBodyBB = BasicBlock::Create(*TheContext, "for.body");
  BasicBlock *LoopStepBB = BasicBlock::Create(*TheContext, "for.step");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "for.end");

  Builder->CreateBr(LoopCondBB);

  //  5. Loop condition
  Builder->SetInsertPoint(LoopCondBB);
  Value *CurVar =
      Builder->CreateLoad(getLLVMType(VarType.get()), Alloca, VarName);
  Value *EndVal = Builder->CreateLoad(getLLVMType(VarType.get()), EndAlloca,
                                      VarName + ".end");
  Value *AscFlag =
      Builder->CreateLoad(Type::getInt1Ty(*TheContext), AscAlloca, "asc");

  // If ascending: cond = (i < end), else: cond = (i > end)
  Value *CondAsc, *CondDesc;
  if (VarType->isNamed("int")) {
    CondAsc = Builder->CreateICmpSLT(CurVar, EndVal, "lt");
    CondDesc = Builder->CreateICmpSGT(CurVar, EndVal, "gt");
  } else {
    CondAsc = Builder->CreateFCmpOLT(CurVar, EndVal, "lt");
    CondDesc = Builder->CreateFCmpOGT(CurVar, EndVal, "gt");
  }

  Value *CondV = Builder->CreateSelect(AscFlag, CondAsc, CondDesc, "forcond");
  Builder->CreateCondBr(CondV, LoopBodyBB, AfterBB);

  //  6. Loop body
  TheFunction->insert(TheFunction->end(), LoopBodyBB);
  Builder->SetInsertPoint(LoopBodyBB);

  // Push {StepBB, AfterBB} so 'continue' jumps to step, 'break' jumps to after
  LoopBlocks.push_back({LoopStepBB, AfterBB});

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  LoopBlocks.pop_back();

  // Fall through to step block
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopStepBB);
  }

  // 7. Loop step
  TheFunction->insert(TheFunction->end(), LoopStepBB);
  Builder->SetInsertPoint(LoopStepBB);

  if (!Step->codegen()) {
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  // Branch back to condition
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // 8. After loop
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // Exit the for-loop scope — loop variable is no longer visible.
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // Restore old NamedValues binding for backward-compatibility.
  if (HadOldBinding) {
    NamedValues[VarName] = SavedBinding;
  } else {
    NamedValues.erase(VarName);
  }

  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}


// ForEachExprAST::codegen : desugars to a compiler-generated ascending
// 0..Size-1 counted loop over the array's compile-time-known size (the
// TypeChecker already guarantees the iterable is an array with Size > 0 --
// see TypeChecker::visitForEach()). Unlike ForExprAST, direction is always
// ascending and step is always 1, so there's no need for the
// ascending/descending dance ForExprAST::codegen() does for user-supplied
// ranges.
Value *ForEachExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  const TurfType *IterableTy = getExprType(Iterable.get());
  if (!IterableTy || !isArrayType(IterableTy)) {
    llvm::report_fatal_error(
        "Internal error: for-each iterable is not an array. "
        "This should have been caught by the TypeChecker.");
    return nullptr;
  }

  const auto *ArrTy = dynamic_cast<const ::ArrayType *>(IterableTy);
  int Size = ArrTy->getSize();
  if (Size <= 0) {
    llvm::report_fatal_error(
        "Internal error: for-each over an unsized array reached Codegen. "
        "This should have been caught by the TypeChecker.");
    return nullptr;
  }

  TurfTypePtr ElemTy = ArrTy->getElementTypePtr();
  Type *ElemLLVMTy = getLLVMType(ElemTy.get());
  llvm::ArrayType *ArrLLVMTy = llvm::ArrayType::get(ElemLLVMTy, Size);

  // 1. Get an address for the iterable to GEP into. Same dual-path pattern
  //    ArrayAccessExprAST::codegenLValue()/codegen() use: prefer the
  //    l-value form (a variable, field, or nested index), falling back to
  //    dumping an r-value (e.g. a function call returning an array) into a
  //    fresh temp alloca.
  Value *ArrPtr = Iterable->codegenLValue();
  if (!ArrPtr) {
    Value *AggVal = Iterable->codegen();
    if (!AggVal)
      return nullptr;
    AllocaInst *TmpAlloca =
        CreateEntryBlockAlloca(TheFunction, "foreach.tmp", IterableTy);
    Builder->CreateStore(AggVal, TmpAlloca);
    ArrPtr = TmpAlloca;
  }

  // 2. Alloca the value var. The index var (paired form) gets its own
  //    alloca that doubles as the loop counter; the single-var form uses
  //    one hidden counter alloca instead.
  AllocaInst *ValueAlloca =
      CreateEntryBlockAlloca(TheFunction, ValueVarName, ElemTy.get());

  AllocaInst *CounterAlloca =
      hasIndexVar()
          ? CreateEntryBlockAlloca(TheFunction, IndexVarName, TurfInt().get())
          : CreateEntryBlockAlloca(TheFunction, "foreach.idx", TurfInt().get());
  Builder->CreateStore(ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                       CounterAlloca);

  // 3. Push a new scope so the loop variable(s) don't leak out after the
  //    loop ends, saving/restoring any pre-existing NamedValues bindings --
  //    exact mirror of ForExprAST::codegen()'s scope handling.
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  auto OldValueBinding = NamedValues.find(ValueVarName);
  VarInfo SavedValueBinding;
  bool HadOldValueBinding = false;
  if (OldValueBinding != NamedValues.end()) {
    SavedValueBinding = OldValueBinding->second;
    HadOldValueBinding = true;
  }
  NamedValues[ValueVarName] = {ValueAlloca, ElemTy};
  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(ValueVarName, ElemTy, Loc, ValueAlloca);
  }

  bool HadOldIndexBinding = false;
  VarInfo SavedIndexBinding;
  if (hasIndexVar()) {
    auto OldIndexBinding = NamedValues.find(IndexVarName);
    if (OldIndexBinding != NamedValues.end()) {
      SavedIndexBinding = OldIndexBinding->second;
      HadOldIndexBinding = true;
    }
    NamedValues[IndexVarName] = {CounterAlloca, TurfInt()};
    if (GlobalSymbolTable) {
      GlobalSymbolTable->DeclareSymbol(IndexVarName, TurfInt(), Loc, CounterAlloca);
    }
  }

  // 4. Basic blocks.
  BasicBlock *CondBB =
      BasicBlock::Create(*TheContext, "foreach.cond", TheFunction);
  BasicBlock *BodyBB = BasicBlock::Create(*TheContext, "foreach.body");
  BasicBlock *StepBB = BasicBlock::Create(*TheContext, "foreach.step");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "foreach.end");

  Builder->CreateBr(CondBB);

  // 5. Condition: counter < Size
  Builder->SetInsertPoint(CondBB);
  Value *CounterVal = Builder->CreateLoad(Type::getInt64Ty(*TheContext),
                                          CounterAlloca, "foreach.idxval");
  Value *SizeConst = ConstantInt::get(Type::getInt64Ty(*TheContext), Size);
  Value *CondV = Builder->CreateICmpSLT(CounterVal, SizeConst, "foreach.cond");
  Builder->CreateCondBr(CondV, BodyBB, AfterBB);

  // 6. Body: load array[counter] into the value var's alloca.
  TheFunction->insert(TheFunction->end(), BodyBB);
  Builder->SetInsertPoint(BodyBB);

  Value *IdxForGEP = Builder->CreateLoad(Type::getInt64Ty(*TheContext),
                                         CounterAlloca, "foreach.idxval");
  Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                      IdxForGEP};
  // The index is compiler-generated and provably in [0, Size) -- no runtime
  // bounds check needed here, unlike ordinary user-supplied array indexing
  // (ArrayAccessExprAST::codegen()).
  Value *ElemPtr =
      Builder->CreateInBoundsGEP(ArrLLVMTy, ArrPtr, Indices, "foreach.elemptr");
  Value *ElemVal = Builder->CreateLoad(ElemLLVMTy, ElemPtr, "foreach.elem");
  Builder->CreateStore(ElemVal, ValueAlloca);

  // Push {StepBB, AfterBB} so 'continue' jumps to step, 'break' jumps to after.
  LoopBlocks.push_back({StepBB, AfterBB});

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  LoopBlocks.pop_back();

  // Fall through to step block.
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(StepBB);
  }

  // 7. Step: counter += 1.
  TheFunction->insert(TheFunction->end(), StepBB);
  Builder->SetInsertPoint(StepBB);
  Value *CurCounter = Builder->CreateLoad(Type::getInt64Ty(*TheContext),
                                          CounterAlloca, "foreach.idxval");
  Value *NextCounter = Builder->CreateAdd(
      CurCounter, ConstantInt::get(Type::getInt64Ty(*TheContext), 1),
      "foreach.next");
  Builder->CreateStore(NextCounter, CounterAlloca);

  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(CondBB);
  }

  // 8. After loop.
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  if (HadOldValueBinding) {
    NamedValues[ValueVarName] = SavedValueBinding;
  } else {
    NamedValues.erase(ValueVarName);
  }
  if (hasIndexVar()) {
    if (HadOldIndexBinding) {
      NamedValues[IndexVarName] = SavedIndexBinding;
    } else {
      NamedValues.erase(IndexVarName);
    }
  }

  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}


// ─── CompareExprAST::codegen ─────────────────────────────────────────────────
//
// Two code-generation paths:
//
//  Integer / enum subjects  (SubjectIsString == false)
//  ─────────────────────────
//  Uses LLVM SwitchInst.  Each arm adds one or more case constants; the
//  backend promotes this to a jump table when the value range is dense enough.
//  The default target is either the 'otherwise' body block or the merge block.
//
//  String subjects  (SubjectIsString == true)
//  ────────────────────────────────────────────
//  Falls back to a strcmp chain (SwitchInst only works for integers).
//  For each arm we call strcmp(subject, pattern) == 0.  Multiple patterns in
//  one stacked arm are OR-ed: if any pattern matches we jump to the body.
//
//  Both paths guarantee no fall-through: every body always branches to MergeBB.
//
Value *CompareExprAST::codegen() {
  // Codegen the subject exactly once
  Value *SubjectV = Subject->codegen();
  if (!SubjectV)
    return nullptr;

  if (Arms.empty() && !OtherBody) {
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "compare.end");

  // ─── String subject: strcmp chain ────────────────────────────────────────
  if (SubjectIsString) {
    // Look up (or declare) strcmp
    FunctionType *StrcmpTy = FunctionType::get(
        Type::getInt32Ty(*TheContext),
        {PointerType::get(*TheContext, 0), PointerType::get(*TheContext, 0)},
        /*isVarArg=*/false);
    FunctionCallee StrcmpFn =
        TheModule->getOrInsertFunction("strcmp", StrcmpTy);

    // Build: otherwise body block (or merge if absent)
    BasicBlock *DefaultBB = OtherBody
                                ? BasicBlock::Create(*TheContext, "compare.otherwise")
                                : MergeBB;

    for (size_t i = 0; i < Arms.size(); ++i) {
      const MatchesArm &Arm = Arms[i];
      BasicBlock *ArmBodyBB =
          BasicBlock::Create(*TheContext, "compare.arm." + std::to_string(i), TheFunction);
      BasicBlock *NoMatchBB =
          BasicBlock::Create(*TheContext, "compare.nomatch." + std::to_string(i));

      // For each pattern in this arm: strcmp(subj, pat) == 0 → jump to body
      BasicBlock *CheckBB = Builder->GetInsertBlock();
      (void)CheckBB;
      Value *AnyMatch = ConstantInt::getFalse(*TheContext);

      for (size_t j = 0; j < Arm.Values.size(); ++j) {
        Value *PatV = Arm.Values[j]->codegen();
        if (!PatV)
          return nullptr;
        Value *Cmp = Builder->CreateCall(
            StrcmpFn, {SubjectV, PatV}, "strcmp.res." + std::to_string(j));
        Value *IsZero = Builder->CreateICmpEQ(
            Cmp, ConstantInt::get(Type::getInt32Ty(*TheContext), 0),
            "strcmp.eq." + std::to_string(j));

        if (j == 0) {
          AnyMatch = IsZero;
        } else {
          // Emit short-circuit OR:
          //   if AnyMatch already true → don't evaluate more strcmp
          //   For simplicity we use a phi-free OR here (non-short-circuit)
          //   This is correct because patterns are literals with no side effects.
          AnyMatch = Builder->CreateOr(AnyMatch, IsZero, "or.pat");
        }
      }

      Builder->CreateCondBr(AnyMatch, ArmBodyBB, NoMatchBB);

      // Arm body
      Builder->SetInsertPoint(ArmBodyBB);
      if (!Arm.Body->codegen())
        return nullptr;
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);

      // Next arm or default
      TheFunction->insert(TheFunction->end(), NoMatchBB);
      Builder->SetInsertPoint(NoMatchBB);
    }

    // Fall through to otherwise or merge
    if (OtherBody) {
      // The last NoMatchBB needs to branch to DefaultBB
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(DefaultBB);

      TheFunction->insert(TheFunction->end(), DefaultBB);
      Builder->SetInsertPoint(DefaultBB);
      if (!OtherBody->codegen())
        return nullptr;
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);
    } else {
      // No otherwise: current block (NoMatchBB or original) branches to merge
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);
    }

    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  // ─── Integer / enum subject: SwitchInst ──────────────────────────────────
  //
  // Cast subject to i64 (the natural integer type in TurfLang) so SwitchInst
  // cases are always the same width regardless of whether the subject was
  // declared as bool or a named enum.
  Value *IntSubjectV = CastToType(SubjectV, TurfInt(), "switch.subj");

  // Default block: either the 'otherwise' body or the merge block.
  BasicBlock *DefaultBB = OtherBody
                              ? BasicBlock::Create(*TheContext, "compare.otherwise")
                              : MergeBB;

  SwitchInst *SI = Builder->CreateSwitch(IntSubjectV, DefaultBB,
                                         static_cast<unsigned>(Arms.size()));

  // One basic block per arm (shared by all stacked patterns in that arm).
  for (size_t i = 0; i < Arms.size(); ++i) {
    const MatchesArm &Arm = Arms[i];
    BasicBlock *ArmBodyBB =
        BasicBlock::Create(*TheContext, "compare.arm." + std::to_string(i),
                           TheFunction);

    // Add all pattern constants as cases targeting this block.
    for (const auto &PatExpr : Arm.Values) {
      // Constant-fold the pattern.  Patterns must be integer literals or
      // constant-foldable expressions (TypeChecker already enforced int type).
      Value *PatV = PatExpr->codegen();
      if (!PatV)
        return nullptr;

      auto *PatConst = dyn_cast<ConstantInt>(PatV);
      if (!PatConst) {
        // Cast to i64 so the switch width matches
        PatV = CastToType(PatV, TurfInt(), "patcast");
        PatConst = dyn_cast<ConstantInt>(PatV);
      }
      if (!PatConst) {
        SemanticError(Arm.Loc,
                      "matches pattern must be a compile-time integer constant.")
            .raise();
        return nullptr;
      }
      // Normalise to i64 width
      APInt CaseVal = PatConst->getValue().sextOrTrunc(64);
      SI->addCase(
          llvm::cast<ConstantInt>(
              ConstantInt::get(Type::getInt64Ty(*TheContext), CaseVal)),
          ArmBodyBB);
    }

    // Emit arm body
    Builder->SetInsertPoint(ArmBodyBB);
    if (!Arm.Body->codegen())
      return nullptr;
    // Guarantee no fall-through: always branch to merge
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
  }

  // Emit otherwise body (the switch's default target)
  if (OtherBody) {
    TheFunction->insert(TheFunction->end(), DefaultBB);
    Builder->SetInsertPoint(DefaultBB);
    if (!OtherBody->codegen())
      return nullptr;
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
  }

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}


Value *BreakExprAST::codegen() {
  if (LoopBlocks.empty()) {
    ControlFlowError(Loc, "'break' used outside of a loop").raise();
    return nullptr;
  }

  Builder->CreateBr(LoopBlocks.back().second);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_break", TheFunction);
  Builder->SetInsertPoint(DeadBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}


Value *ContinueExprAST::codegen() {
  if (LoopBlocks.empty()) {
    ControlFlowError(Loc, "'continue' used outside of a loop").raise();
    return nullptr;
  }

  Builder->CreateBr(LoopBlocks.back().first);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_continue", TheFunction);
  Builder->SetInsertPoint(DeadBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}



// ReturnExprAST::codegen
Value *ReturnExprAST::codegen() {
  if (CurrentFunction == nullptr) {
    ControlFlowError(Loc, "'return' used outside of a function").raise();
    return nullptr;
  }

  // Mark scope as having early exit for unreachable code detection
  if (GlobalSymbolTable) {
    GlobalSymbolTable->MarkEarlyExit();
  }

  // `!T` (docs/planning/Errors.md): the function's ACTUAL LLVM return type
  // is always the FallibleValue struct (see getLLVMType()), never a bare
  // T/void, so every `return` inside a `!T` function -- including a bare
  // `return;` for a `!void` function's success case -- must wrap its value
  // instead of returning it (or void) directly.
  bool IsFallibleReturn = CurrentFuncReturnType && CurrentFuncReturnType->isErrorUnion();

  if (!Val) {
    // empty return; TypeChecker has verified this is a void (or `!void`)
    // function.
    if (IsFallibleReturn)
      Builder->CreateRet(WrapFallibleValue(nullptr, /*IsError=*/false));
    else
      Builder->CreateRetVoid();
  } else {
    // return <expr>; TypeChecker has verified the expression type.
    Value *RetVal = Val->codegen();
    if (!RetVal)
      return nullptr;

    // Type validation and compatibility are handled by the TypeChecker.

    if (IsFallibleReturn) {
      bool IsError = IsErrorConstructionExpr(Val.get());
      Builder->CreateRet(WrapFallibleValue(RetVal, IsError));
    } else {
      RetVal = CastToType(RetVal, CurrentFuncReturnType, "retcast", Loc);
      Builder->CreateRet(RetVal);
    }
  }

  // After a terminator, create a fresh unreachable block so the IR builder
  // has a valid insert point for any subsequent statements in the same block.
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_ret", CurrentFunction);
  Builder->SetInsertPoint(DeadBB);

  // Return a dummy null so the AST node has a non-null value
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}
