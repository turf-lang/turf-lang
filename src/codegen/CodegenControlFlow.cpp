/**
 * @file CodegenControlFlow.cpp
 * @brief Control-flow codegen: if/block/while/do-while/for/for-each/break/continue/compare.
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

// Stack of (condition-block, after-block) pairs for the innermost enclosing
// loop(s) -- consulted by BreakExprAST/ContinueExprAST to know which block
// to jump to. Only ever pushed/popped by the loop-codegen methods below
// (While/DoWhile/For/ForEach), all in this same file.
std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> LoopBlocks;

// LANG-003 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): `defer`'s entire
// lowering lives in this file, as three pieces of shared state plus one
// helper, rather than in the separate CFG.cpp static-analysis subsystem --
// CFGBuilder (CFG.cpp) never needs to know about `defer` at all, since a
// `defer <stmt>;` never itself branches: from the CFG's point of view it's
// an ordinary "regular statement" leaf (CFGBuilder::visitExpr()'s fallback
// case already handles it correctly with zero changes there), and its
// runtime effect is purely a matter of WHEN its wrapped statement's own
// codegen() actually runs -- a pure AST-recursive-Codegen lowering problem.
//
// One frame per currently-open `{ }` scope (pushed/popped by
// BlockExprAST::codegen() itself, below), holding the raw ExprAST* each
// `defer <stmt>;` registers into it (DeferExprAST::codegen() never codegens
// its own wrapped statement -- it just appends to the innermost open
// frame). Reusing the SAME raw ExprAST* pointer across every exit edge that
// flushes it is deliberate, not an aliasing bug: `defer` re-runs the
// statement AS WRITTEN against whatever variable state exists at each exit
// point (no argument-snapshotting the way Go's `defer f(x)` evaluates `x`
// immediately) -- codegen()-ing the same node once per reached exit edge is
// exactly what that requires, and every ExprAST::codegen() implementation
// in this compiler already tolerates being called more than once on the
// same node (e.g. the "after_ret" dead-block duplicate-flush case
// FlushPendingDefers()/BlockExprAST::codegen() below rely on).
static std::vector<std::vector<ExprAST *>> PendingDeferFrames;

// Parallel to LoopBlocks: PendingDeferFrames.size() at the moment each
// loop's OWN body starts (i.e. before that body's BlockExprAST::codegen()
// pushes its own frame) -- lets BreakExprAST/ContinueExprAST bound how many
// of the currently-open frames belong to blocks INSIDE this loop (which
// must run) versus frames belonging to blocks enclosing the loop itself
// (which break/continue must never touch -- only `return` exits those).
static std::vector<size_t> LoopDeferDepth;

// Runs every deferred statement in every currently-open frame from
// `FromDepth` (inclusive) to the top of the stack: innermost frame first,
// each frame's own statements in reverse (LIFO) registration order -- the
// exact ordering LANG-003 specifies for `defer` firing on any exit edge.
// Deliberately does NOT pop anything: the block(s) that own these frames
// are still lexically open (control is merely leaving them EARLY, via a
// return/break/continue reachable from somewhere inside); each block still
// pops its own frame exactly once, itself, when its own
// BlockExprAST::codegen() call returns (same "owner does its own cleanup"
// discipline GlobalSymbolTable::EnterScope()/ExitScope() already follows
// around this identical early-exit machinery). A block whose own natural
// end is never reached at runtime because this function already flushed
// and returned/broke/continued through it still calls FlushPendingDefers()
// a second time on itself when Codegen reaches that now-dead code position
// (BlockExprAST::codegen()'s own post-loop check can't statically tell "no
// terminator yet" apart from "definitely reachable") -- harmless: nothing
// branches into that position, so LLVM never executes the duplicate.
static void FlushPendingDefers(size_t FromDepth) {
  for (size_t i = PendingDeferFrames.size(); i-- > FromDepth;) {
    std::vector<ExprAST *> &Frame = PendingDeferFrames[i];
    for (auto It = Frame.rbegin(); It != Frame.rend(); ++It)
      (*It)->codegen();
  }
}


Value *IfExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  bool IsBlockForm =
      dynamic_cast<BlockExprAST *>(Branches.front().Body.get()) != nullptr;
  bool HasElse = (ElseBody != nullptr);

  // Block-form if/elseif/else: statement, no PHI node needed.
  if (IsBlockForm) {
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    for (size_t i = 0; i < Branches.size(); ++i) {
      SetDebugLoc(Branches[i].Loc);
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
  SetDebugLoc(Branches[0].Loc);
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

  // LANG-003: this block's own `defer` frame. Pushed/popped here regardless
  // of which OTHER construct this BlockExprAST happens to be (a function
  // body, an if/while/for body, a `recover run { }` block, ...) -- every
  // one of those already routes through this exact function for its body,
  // so `defer` support falls out uniformly with no per-construct plumbing.
  PendingDeferFrames.emplace_back();

  // A statement whose codegen() returns nullptr without having raised its
  // own diagnostic first is a genuine, silent compiler failure -- stop
  // processing the rest of this block instead of quietly dropping the
  // failing statement from the compiled program. This closes the
  // "BlockExprAST::codegen() never checks for a null statement result" gap
  // tracked in the 2026-07 code-review-gaps audit (its doc was retired; see
  // git history for the original writeup). This relies on every "processed
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
    // TOOL-001: this single loop is what gives every ORDINARY statement
    // inside a `{ }` body a correct DILocation with no per-node-kind
    // plumbing -- every statement-shaped AST node ends up as a direct
    // child of some BlockExprAST (a function body, an if/while/for body,
    // ...), so setting the debug location once here, right before each
    // child's own codegen(), covers every instruction that child's
    // codegen() goes on to build (IRBuilder attaches "the current debug
    // location" to every instruction it creates until this call changes it
    // again). A handful of control-flow HEADERS (an if/while/for's own
    // condition, evaluated directly rather than through a Block) set their
    // own location separately, right below in this same file.
    SetDebugLoc(GetNodeDebugLoc(Expr.get()));
    LastVal = Expr->codegen();
    if (!LastVal) {
      PendingDeferFrames.pop_back();
      if (GlobalSymbolTable) {
        GlobalSymbolTable->ExitScope();
      }
      return nullptr;
    }
  }

  // LANG-003: this block's own natural (non-early-exit) fallthrough to its
  // end -- run its pending defers now, LIFO. Skipped when the current
  // insert block already has a terminator: every path through this block
  // already exited early (return/break/continue), each of which already
  // ran these same defers itself via FlushPendingDefers() before its own
  // terminator (see that function's doc comment for why this position, if
  // reached at all in that case, is provably dead code and a harmless
  // no-op to re-flush into).
  //
  // GetInsertBlock() can genuinely be null here: a MODULE's own top-level
  // body is a BlockExprAST too (ModuleImportExprAST::codegen() calls
  // Body->codegen() directly, CodegenStruct.cpp), with no enclosing
  // function and therefore no current insert point -- a `defer` can't
  // legally appear there anyway (nothing but declarations do), but this
  // function still runs the push/pop bookkeeping unconditionally, so the
  // flush decision has to tolerate it instead of dereferencing a null
  // BasicBlock*.
  if (Builder->GetInsertBlock() && !Builder->GetInsertBlock()->getTerminator())
    FlushPendingDefers(PendingDeferFrames.size() - 1);
  PendingDeferFrames.pop_back();

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


// LANG-003: registers Stmt into the innermost currently-open block's defer
// frame -- does NOT codegen Stmt itself. That happens later, once per exit
// edge that actually reaches it (FlushPendingDefers(), called from this
// same file's BlockExprAST::codegen() for the natural-fallthrough case and
// from Break/Continue/ReturnExprAST::codegen() below for early exits).
Value *DeferExprAST::codegen() {
  if (PendingDeferFrames.empty()) {
    // Can't happen from a valid parse: `defer` only ever reaches Codegen as
    // a statement inside SOME `{ }` body, and a function/if/while/...  body
    // is always a BlockExprAST, which always pushes a frame before any of
    // its own children codegen (immediately above). Kept as a defensive
    // diagnostic rather than a crash, mirroring BreakExprAST/
    // ContinueExprAST's identical "used outside of a loop" shape below.
    ControlFlowError(Loc, "'defer' used outside of any block").raise();
    return nullptr;
  }
  PendingDeferFrames.back().push_back(Stmt.get());
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
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

  SetDebugLoc(getLoc());
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
  LoopDeferDepth.push_back(PendingDeferFrames.size());

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    LoopDeferDepth.pop_back();
    return nullptr;
  }

  LoopBlocks.pop_back();
  LoopDeferDepth.pop_back();

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
  LoopDeferDepth.push_back(PendingDeferFrames.size());

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    LoopDeferDepth.pop_back();
    return nullptr;
  }

  LoopBlocks.pop_back();
  LoopDeferDepth.pop_back();

  // Fall through to the condition check if the body didn't already terminate.
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // Condition Block
  TheFunction->insert(TheFunction->end(), LoopCondBB);
  Builder->SetInsertPoint(LoopCondBB);

  SetDebugLoc(getLoc());
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

  SetDebugLoc(getLoc());

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
  LoopDeferDepth.push_back(PendingDeferFrames.size());

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    LoopDeferDepth.pop_back();
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  LoopBlocks.pop_back();
  LoopDeferDepth.pop_back();

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
// 0..Size-1 counted loop over the array's size -- compile-time for a `T[N]`,
// the slice's run-time length for a `T[]` parameter (the TypeChecker already
// guarantees the iterable is an array -- see TypeChecker::visitForEach()).
// Unlike ForExprAST, direction is always
// ascending and step is always 1, so there's no need for the
// ascending/descending dance ForExprAST::codegen() does for user-supplied
// ranges.
Value *ForEachExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  SetDebugLoc(getLoc());

  const TurfType *IterableTy = getExprType(Iterable.get());
  if (!IterableTy || !isArrayType(IterableTy)) {
    llvm::report_fatal_error(
        "Internal error: for-each iterable is not an array. "
        "This should have been caught by the TypeChecker.");
    return nullptr;
  }

  const auto *ArrTy = dynamic_cast<const ::ArrayType *>(IterableTy);
  int Size = ArrTy->getSize();
  const bool IsSlice = Size < 0;

  TurfTypePtr ElemTy = ArrTy->getElementTypePtr();
  Type *ElemLLVMTy = getLLVMType(ElemTy.get());

  // 1. Get an address for the iterable to GEP into, and the bound. A `T[]`
  //    parameter (issue #260) is a slice that carries both: its data pointer
  //    and its run-time length. Otherwise, the same dual-path pattern
  //    ArrayAccessExprAST::codegenLValue()/codegen() use: prefer the
  //    l-value form (a variable, field, or nested index), falling back to
  //    dumping an r-value (e.g. a function call returning an array) into a
  //    fresh temp alloca.
  Value *ArrPtr = nullptr;
  Value *SizeVal = nullptr;
  if (IsSlice) {
    Value *Slice = Iterable->codegen();
    if (!Slice)
      return nullptr;
    ArrPtr = Builder->CreateExtractValue(Slice, 0, "foreach.data");
    SizeVal = Builder->CreateExtractValue(Slice, 1, "foreach.len");
  } else {
    SizeVal = ConstantInt::get(Type::getInt64Ty(*TheContext), Size);
    ArrPtr = Iterable->codegenLValue();
  }
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
  Value *CondV = Builder->CreateICmpSLT(CounterVal, SizeVal, "foreach.cond");
  Builder->CreateCondBr(CondV, BodyBB, AfterBB);

  // 6. Body: load array[counter] into the value var's alloca.
  TheFunction->insert(TheFunction->end(), BodyBB);
  Builder->SetInsertPoint(BodyBB);

  Value *IdxForGEP = Builder->CreateLoad(Type::getInt64Ty(*TheContext),
                                         CounterAlloca, "foreach.idxval");
  // The index is compiler-generated and provably in [0, Size) -- no runtime
  // bounds check needed here, unlike ordinary user-supplied array indexing
  // (ArrayAccessExprAST::codegen()). Both shapes address the element as
  // "the IdxForGEP-th T after ArrPtr": a `T[N]`'s storage and a slice's data
  // pointer each point at element 0.
  Value *ElemPtr = Builder->CreateInBoundsGEP(ElemLLVMTy, ArrPtr, IdxForGEP,
                                              "foreach.elemptr");
  Value *ElemVal = Builder->CreateLoad(ElemLLVMTy, ElemPtr, "foreach.elem");
  Builder->CreateStore(ElemVal, ValueAlloca);

  // Push {StepBB, AfterBB} so 'continue' jumps to step, 'break' jumps to after.
  LoopBlocks.push_back({StepBB, AfterBB});
  LoopDeferDepth.push_back(PendingDeferFrames.size());

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    LoopDeferDepth.pop_back();
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  LoopBlocks.pop_back();
  LoopDeferDepth.pop_back();

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
  // LEC-003: a struct pattern, `_` or a guard cannot be a switch/strcmp chain.
  // Decided BEFORE the subject is evaluated -- the pattern lowering evaluates
  // it itself, once, and this function must not have already done so.
  if (needsPatternCodegen())
    return codegenPatterns();

  // Codegen the subject exactly once
  Value *SubjectV = Subject->codegen();
  if (!SubjectV)
    return nullptr;

  if (Arms.empty() && !OtherBody) {
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "compare.end");

  // Only `otherwise`: nothing to compare, so the body runs whatever the
  // subject's type. A struct subject fell through to the integer switch
  // below, whose CastToType(struct, int) emitted an FCmp on a struct value
  // ("generated LLVM IR is invalid"; turf-fuzz, 2026-10-13).
  if (Arms.empty()) {
    BasicBlock *OtherBB = BasicBlock::Create(*TheContext, "compare.otherwise", TheFunction);
    Builder->CreateBr(OtherBB);
    Builder->SetInsertPoint(OtherBB);
    if (!OtherBody->codegen())
      return nullptr;
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

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
      // Normalise to i64 width. A bool pattern (`matches true:`) is an i1,
      // and sign-extending it gives -1 while the subject above was widened
      // with CastToType(), which zero-extends an i1 to 1 -- so `matches true:`
      // used to never match (found by LEC-003 once bool compares became
      // exhaustiveness-checked and so actually written). Zero-extend a 1-bit
      // constant to agree with the subject.
      APInt CaseVal = PatConst->getValue().getBitWidth() == 1
                          ? PatConst->getValue().zext(64)
                          : PatConst->getValue().sextOrTrunc(64);
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

  // LANG-003: run every pending defer belonging to a block INSIDE this
  // loop (down to, and including, the loop body's own frame) before
  // jumping out -- but none belonging to a block enclosing the loop, which
  // `break` never exits. LoopDeferDepth.back() is exactly that boundary
  // (see its own doc comment above).
  FlushPendingDefers(LoopDeferDepth.back());

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

  // LANG-003: same reasoning as BreakExprAST::codegen() just above -- a
  // `defer` inside a loop body runs per iteration, and `continue` ending
  // one iteration early is exactly one of the exit edges that must run it.
  FlushPendingDefers(LoopDeferDepth.back());

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

  // `!T` (docs/architecture/errors_and_recover.md): the function's ACTUAL LLVM
  // return type is always the FallibleValue struct (see getLLVMType()), never a
  // bare T/void, so every `return` inside a `!T` function -- including a bare
  // `return;` for a `!void` function's success case -- must wrap its value
  // instead of returning it (or void) directly.
  bool IsFallibleReturn = CurrentFuncReturnType && CurrentFuncReturnType->isErrorUnion();

  if (!Val) {
    // empty return; TypeChecker has verified this is a void (or `!void`)
    // function.
    //
    // LANG-003: run every pending defer, in every currently-open block
    // frame, before actually leaving -- `return` exits the WHOLE function,
    // so unlike break/continue (bounded to the innermost loop), this always
    // flushes down to depth 0. No return value exists on this path to
    // protect the ordering of, so it's safe to flush right here.
    FlushPendingDefers(0);
    if (IsFallibleReturn)
      Builder->CreateRet(WrapFallibleValue(nullptr, /*IsError=*/false));
    else
      Builder->CreateRetVoid();
  } else {
    // return <expr>; TypeChecker has verified the expression type.
    Value *RetVal = Val->codegen();
    if (!RetVal)
      return nullptr;

    // LANG-003: RetVal is deliberately computed BEFORE flushing (matching
    // ordinary defer semantics elsewhere: the return value is fixed before
    // cleanup runs, not re-derived after it) -- a `defer` that mutates a
    // variable the return expression already read must not retroactively
    // change what gets returned. Wrapping/casting RetVal below is a pure
    // value transformation with no further variable reads, so flushing
    // here is equivalent to flushing immediately before the eventual
    // CreateRet() call further down.
    FlushPendingDefers(0);

    // Type validation and compatibility are handled by the TypeChecker.

    if (IsFallibleReturn && PassesThroughFallible) {
      // LEC-004, `return f();` where `f` returns the `!T` this function
      // returns: RetVal already IS a FallibleValue, success or error, so it is
      // handed back as it is -- wrapping it again would box a whole
      // FallibleValue into the success slot of another. Set by the
      // TypeChecker (TypeChecker::visitReturn()): nothing in the operand's
      // shape says it is fallible.
      Builder->CreateRet(RetVal);
    } else if (IsFallibleReturn) {
      // NOT IsErrorConstructionExpr() alone -- that only recognizes the
      // bare `ErrorType::Variant` shape, not `return err;` propagating a
      // caught error straight out of an outer `!T` function (`inner()
      // recover as err run { return err; }; ... return x;` doesn't hit
      // this line at all -- it's the `return err;` INSIDE the recover
      // block that does, once TypeChecker's isDeclaredErrorTypeValue()
      // fix (TypeCheckerStmt.cpp) allows it to type-check in the first
      // place). IsErrorTagExpr() (CodegenCore.cpp) already recognizes
      // BOTH shapes -- a bare ErrorType::Variant reference AND a
      // VariableExprAST whose ResolvedSymbol type is AnyErrorType/
      // TurfUserErrorType (exactly `err`'s type, see
      // Resolver::visitRecoverExpr) -- and RetVal is already the correct
      // i8* tag pointer either way (a plain variable load for `err`), so
      // no other change is needed: WrapFallibleValue's IsError=true branch
      // just stores RetVal as-is into the errptr slot. Confirmed live:
      // before this fix, `return err;` type-checked fine but ran with
      // `err`'s tag pointer silently boxed into the SUCCESS slot instead
      // (IsError=false), producing a garbage "successful" int value
      // instead of the propagated error.
      bool IsError = IsErrorTagExpr(Val.get());
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


// `try <expr>` (LEC-004, FREEZE.md §6.18): evaluates the `!U` operand; on
// success the value is the unboxed `U`, on failure this RETURNS the error from
// the enclosing `!V` function -- the same `{false, 0, error}` FallibleValue a
// `return err;` builds, after the same pending `defer`s a `return` runs.
//
// The error value travels as the operand's own errorPayload pointer, never
// copied or re-wrapped, so a `try` passes on exactly what failed: the same tag,
// message and payload. `V` and `U` are unrelated (the error union is open), and
// the function's return type is always one fixed FallibleValue whatever `V` is,
// so nothing here depends on either.
//
// The failure block ends in `ret`, so it needs no "dead block after the
// return" the way ReturnExprAST does: control only continues on the success
// block, which is where the builder is left.
Value *TryExprAST::codegen() {
  Value *FV = Operand ? Operand->codegen() : nullptr;
  if (!FV)
    return nullptr;

  TurfTypePtr SuccessTy = ResolvedSuccessType ? ResolvedSuccessType : TurfVoid();

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  Value *IsOk = Builder->CreateExtractValue(FV, 0, "try.isok");
  BasicBlock *OkBB = BasicBlock::Create(*TheContext, "try.ok", TheFunction);
  BasicBlock *FailBB = BasicBlock::Create(*TheContext, "try.fail", TheFunction);
  Builder->CreateCondBr(IsOk, OkBB, FailBB);

  Builder->SetInsertPoint(FailBB);
  Value *ErrPayload = Builder->CreateExtractValue(FV, 2, "try.err");
  FlushPendingDefers(0);
  Builder->CreateRet(WrapFallibleValue(ErrPayload, /*IsError=*/true));

  Builder->SetInsertPoint(OkBB);
  if (SuccessTy->isVoid())
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  Value *Bits = Builder->CreateExtractValue(FV, 1, "try.bits");
  return UnboxFromI64Slot(Bits, SuccessTy.get());
}
