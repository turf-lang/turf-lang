/**
 * @file CodegenVar.cpp
 * @brief Variable codegen: declarations, references, assignment, compound assignment, postfix.
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


void VarDeclExprAST::emitLocalDebugVariable(AllocaInst *Alloca) const {
  // TOOL-001: `-g` local-variable debug info. NameLoc (the variable's own
  // NAME token, distinct from Loc -- the 'mutable'/'immutable' keyword,
  // see this class's own field doc comments in ASTDecl.h) is what a
  // debugger should point at for "where is this local declared", matching
  // what DuplicateDeclarationError/ShadowingWarning already use NameLoc
  // for. Flat function-level scoping (CurrentDebugScope is always the
  // enclosing DISubprogram, never a nested DILexicalBlock per `{ }`) is a
  // deliberate simplification -- see docs/development/changes/TOOL-001.md's
  // Known pitfalls: correct for TOOL-001's own completion criteria (a
  // breakpoint, a backtrace, printing a local by name), imprecise only for
  // a debugger's *scope-based* view of two same-named locals in sibling
  // blocks shadowing each other. Shared by both of codegen()'s alloca
  // paths so neither can drift from the other again.
  if (!DBuilder || !CurrentDebugScope)
    return;
  DIFile *File = getOrCreateDebugFile(NameLoc.Filename.empty()
                                          ? Loc.Filename
                                          : NameLoc.Filename);
  unsigned DeclLine = NameLoc.Line > 0 ? NameLoc.Line : Loc.Line;
  DILocalVariable *LV = DBuilder->createAutoVariable(
      CurrentDebugScope, Name, File, DeclLine, getDebugType(Type.get()),
      /*AlwaysPreserve=*/true);
  DBuilder->insertDeclare(
      Alloca, LV, DBuilder->createExpression(),
      DILocation::get(*TheContext, DeclLine, 0, CurrentDebugScope),
      Builder->GetInsertBlock());
}


Value *VarDeclExprAST::codegen() {
  // `export immut` constants have no real LLVM storage at all -- Resolver
  // Pass 1 (Resolver::hoistExportedConstant) already extracted their literal
  // value into the symbol table, and every reference site inlines it
  // directly (see VariableExprAST/ScopeAccessExprAST's ResolvedConstant
  // field + CodegenConstantValue()). This is a genuine no-op, not just an
  // optimization: an exported constant is reached here from MULTIPLE blind-
  // iteration call sites with no valid insertion point at all -- top-level
  // main-file codegen (main.cpp's pre-pass, which also explicitly skips
  // these) AND, for a module-declared export, ModuleImportExprAST::codegen()
  // unconditionally codegenning every expression in the module's body.
  // Running the ordinary alloca-based path below in either case corrupts
  // whatever LLVM function happens to be getting built adjacent to it in
  // iteration order (confirmed live: "Basic Block ... does not have
  // terminator" in a totally unrelated function).
  //
  // Return a non-null sentinel, NOT nullptr -- nullptr means "codegen
  // failed" to every blind-iteration caller (BlockExprAST::codegen(),
  // ModuleImportExprAST::codegen()'s Body->codegen()), and this is a
  // success, just one with no real value to produce. Same convention
  // StructDeclExprAST/EnumDeclExprAST/ErrorDeclExprAST already use for
  // their own "processed, nothing to return" codegen paths. Confirmed
  // live: returning nullptr here made BlockExprAST::codegen() (once
  // hardened to stop at the first null statement -- see this file's
  // history, and git history for the retired 2026-07 code-review-gaps
  // audit's original writeup) silently
  // abandon the rest of a module's body the moment it reached an
  // `export immut` constant -- e.g. sys.tr's `export immut STDERR_FD`
  // sits right before `fn panic(...)`, so `panic` never got a prototype
  // or body, and every caller (mem.tr, test.tr) hit "LLVM function
  // 'turf_sys_panic_string' not found after successful name resolution."
  if (IsExported)
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));

  // Validation (void type, keywords, duplicates, etc.) is handled by the
  // Resolver and TypeChecker passes before Codegen.


  // Check for unreachable declaration
  if (GlobalSymbolTable && GlobalSymbolTable->CurrentScopeHasEarlyExit()) {
    // E012, not the retired E013: the same mistake, and the code the CFG pass
    // reports for it before Codegen ever runs (this is only the backstop).
    StatementAfterTerminatorError(Loc, "return, break or continue").raise();
    return nullptr;
  }

  // Check for duplicate declaration in current scope
  if (GlobalSymbolTable && GlobalSymbolTable->IsSymbolInCurrentScope(Name)) {
    Symbol *Prev = GlobalSymbolTable->LookupSymbolInCurrentScope(Name);
    if (Prev) {
      DuplicateDeclarationError(Loc, Name, Prev->DeclLoc).raise();
    } else {
      SyntaxError(Loc, ErrCode::DuplicateDeclaration, "Variable already declared").raise();
    }
    return nullptr;
  }

  // Check for shadowing
  if (GlobalSymbolTable) {
    Symbol *Shadowed = GlobalSymbolTable->FindShadowedSymbol(Name);
    if (Shadowed) {
      ShadowingWarning(Loc, Name, Shadowed->DeclLoc).warn();
    }
  }

  Value *Init = nullptr;
  if (InitVal) {
    // An inferred view (`mut v = mk()[1..];`) keeps its source, so a
    // temporary needs storage of its own -- CodegenStoredView().
    Init = isUnsizedArrayType(Type.get()) ? CodegenStoredView(InitVal.get())
                                          : InitVal->codegen();
    if (!Init)
      return nullptr;

    // ── Struct-construction shortcut ────────────────────────────────────────
    // `StructName::new(...)`/`StructName::default()` (FuncCallExprAST::codegen()'s
    // ConstructorCall/DefaultConstruct branches, below) already allocated a
    // fresh alloca, filled it, and returned a load of that alloca. If we
    // create ANOTHER alloca and store the loaded struct value into it,
    // LLVM's SROA pass promotes each alloca independently and loses the
    // alias between `u`'s alloca and the pointer passed to `self` inside a
    // method. Fix: detect the load-from-construction-alloca and reuse it
    // directly. (Previously gated on `dynamic_cast<StructInitExprAST*>` for
    // the old brace-literal form, which allocated the same way.)
    auto *InitScopeCallee = [&]() -> const ScopeAccessExprAST * {
      auto *FC = dynamic_cast<const FuncCallExprAST *>(InitVal.get());
      return FC ? dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee())
               : nullptr;
    }();
    bool IsConstructionInit =
        InitScopeCallee &&
        (InitScopeCallee->ResolvedKind == ScopeAccessKind::ConstructorCall ||
         InitScopeCallee->ResolvedKind == ScopeAccessKind::DefaultConstruct);
    if (IsConstructionInit) {
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(Init)) {
        if (auto *SrcAlloca = llvm::dyn_cast<AllocaInst>(LI->getPointerOperand())) {
          SrcAlloca->setName(Name);
          // This early return used to skip the debug-variable emission
          // below entirely: under `-g`, every local initialized by
          // `T::new(...)`/`T::default()` -- the ordinary way to make a
          // struct -- had no DW_TAG_variable, invisible to `print`/`info
          // locals`, while the same local initialized by any other call was
          // fine.
          emitLocalDebugVariable(SrcAlloca);
          if (GlobalSymbolTable)
            GlobalSymbolTable->DeclareSymbol(Name, Type, Loc, SrcAlloca);
          NamedValues[Name] = {SrcAlloca, Type};
          return Init;
        }
      }
    }
    // ──────────────────────────────────────────────────────────────────────
  } else {
    Init = GetDefaultValue(Type.get());
  }

  Init = CastToType(Init, Type, "initcast", Loc);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Name, Type.get());

  Builder->CreateStore(Init, Alloca);

  emitLocalDebugVariable(Alloca);

  // Register in symbol table
  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(Name, Type, Loc, Alloca);
  }

  // Also keep in NamedValues for backward compatibility
  NamedValues[Name] = {Alloca, Type};
  return Init;
}


Value *VariableExprAST::codegen() {
  if (ResolvedConstant)
    return CodegenConstantValue(*ResolvedConstant);

  // First-class function reference (bare name of a real `fn`, e.g.
  // `double_it` in `mutable f: fn(int): int = double_it;` or
  // `apply(double_it, 5)`) -- functions are already first-class values under
  // LLVM's opaque-pointer model, so this just looks up the existing
  // llvm::Function* by its mangled name and returns it directly; no alloca/
  // load involved (see ResolvedFunctionRef's doc comment in AST.h).
  if (ResolvedFunctionRef) {
    std::string MangledName = ResolvedFunctionRef->mangledName();
    Function *F = TheModule->getFunction(MangledName);
    if (!F)
      F = TheModule->getFunction(ResolvedFunctionRef->BaseName);
    if (!F) {
      SemanticError(Loc, "Internal error: LLVM function '" + MangledName +
                             "' not found after successful name resolution. "
                             "This is a compiler bug.")
          .raise();
      return nullptr;
    }
    return F;
  }

  // Use the mangled name resolved by Resolver/TypeChecker (Issue #5)
  Symbol *Sym = nullptr;
  if (GlobalSymbolTable && ResolvedSymbol) {
    Sym = GlobalSymbolTable->LookupSymbol(ResolvedSymbol->Name);
  }

  if (Sym) {
    AllocaInst *A = Sym->Alloca;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
  }

  // Fall back to old table for built-in compatibility
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) {
    AllocaInst *A = Iter->second.Alloca;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
  }

  // Use-before-declaration error
  RankedSuggestions Suggestions;
  if (GlobalSymbolTable) {
    Suggestions = GlobalSymbolTable->GetRankedSuggestions(Name);
  } else {
    std::vector<std::string> VisibleNames;
    for (const auto &pair : NamedValues) VisibleNames.push_back(pair.first);
    Suggestions.Local = SuggestionIndex::suggestVisible(Name, VisibleNames);
  }
  UseBeforeDeclarationError(Loc, Name, Suggestions).raise();
  return nullptr;
}


Value *VariableExprAST::codegenLValue() {
  if (ResolvedConstant) {
    // No real storage exists (see CodegenConstantValue's doc comment) --
    // return nullptr so callers get the SAME "not addressable" handling as
    // any other non-lvalue expression (AddressOfExprAST::codegen() already
    // turns a null LValue into a clean diagnostic; ordinary assignment to a
    // constant is caught earlier, at TypeChecker time, by
    // ImmutableAssignmentError).
    return nullptr;
  }
  Symbol *Sym = nullptr;
  if (GlobalSymbolTable && ResolvedSymbol)
    Sym = GlobalSymbolTable->LookupSymbol(ResolvedSymbol->Name);
  if (Sym)
    return Sym->Alloca;

  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end())
    return Iter->second.Alloca;

  RankedSuggestions Suggestions;
  if (GlobalSymbolTable) {
    Suggestions = GlobalSymbolTable->GetRankedSuggestions(Name);
  } else {
    std::vector<std::string> VisibleNames;
    for (const auto &pair : NamedValues) VisibleNames.push_back(pair.first);
    Suggestions.Local = SuggestionIndex::suggestVisible(Name, VisibleNames);
  }
  UseBeforeDeclarationError(Loc, Name, Suggestions).raise();
  return nullptr;
}


Value *AssignmentExprAST::codegen() {
  // ── operator[]= dispatch (LANG-001) ──────────────────────────────────────
  // TypeChecker::visitAssignment() already resolved ArrayAccessExprAST::
  // ResolvedIndexSetTarget (if any) via exact (BaseType, IndexType,
  // ValueType) match, for a struct LHS base with no physical address
  // codegenLValue() could ever GEP into. Checked first and returns early --
  // an operator[]= write is an ordinary 3-argument function call, not a
  // store, so none of the getExprType()/CastToType()/codegenLValue()
  // machinery below applies to it at all.
  if (const auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST *>(LHS.get())) {
    if (ArrAcc->ResolvedIndexSetTarget) {
      // Self is passed BY ADDRESS when the overload declared it `&T`
      // (operatorSelfOperandMatches()'s deliberate relaxation,
      // TypeCheckerCall.cpp) -- BY VALUE otherwise, exactly matching
      // whichever form the exact-match check actually accepted.
      const bool SelfByRef = !ArrAcc->ResolvedIndexSetTarget->Params.empty() &&
          ArrAcc->ResolvedIndexSetTarget->Params[0].Type &&
          ArrAcc->ResolvedIndexSetTarget->Params[0].Type->isReference();
      Value *BaseVal = SelfByRef
          ? const_cast<ExprAST *>(ArrAcc->getBase())->codegenLValue()
          : const_cast<ExprAST *>(ArrAcc->getBase())->codegen();
      if (SelfByRef && !BaseVal && dynamic_cast<const ArrayAccessExprAST *>(ArrAcc->getBase())) {
        // `outer[a][b] = v` where `outer[a]` is an operator[] result: the
        // container being written to is a temporary copy, so the write would
        // be lost. The TypeChecker rejects this first (E098); this is the
        // backstop so a path that skips that check cannot silently drop the
        // write (a null here reads to every caller as "an error was already
        // reported").
        LogErrorAt(Loc, ErrCode::OperatorTemporaryWrite,
                   "Cannot assign through 'operator[]=' on a temporary: the "
                   "container being indexed is the result of another "
                   "'operator[]' call, so the write would only change a copy."
                   "\n  Hint: read the inner container into a variable first, "
                   "write to it there, and store it back.",
                   SLM::ErrorCategory::TYPE_MISMATCH);
        return nullptr;
      }
      if (!BaseVal)
        return nullptr;
      Value *IndexVal = const_cast<ExprAST *>(ArrAcc->getIndex())->codegen();
      if (!IndexVal)
        return nullptr;
      Value *ValueVal = RHS->codegen();
      if (!ValueVal)
        return nullptr;
      std::string MangledName = ArrAcc->ResolvedIndexSetTarget->mangledName();
      Function *CalleeF = TheModule->getFunction(MangledName);
      if (!CalleeF) {
        llvm::report_fatal_error(
            llvm::StringRef("Internal error: operator overload '" +
                            MangledName + "' was resolved but never codegenned."));
        return nullptr;
      }
      Builder->CreateCall(CalleeF, {BaseVal, IndexVal, ValueVal});
      // Assignment expressions usually return the value assigned (allows
      // x = arr[i] = 5) -- same convention as the ordinary store path below.
      return ValueVal;
    }
  }

  // Determine the LHS type without evaluating it (avoid double-evaluation side effects)
  const TurfType *LHSTy = getExprType(LHS.get());
  if (!LHSTy)
    return nullptr;

  // Rebinding a `T[]` view (`view = other[1..];`, `view = someArray;`): the
  // RHS becomes a slice the same way a `T[]` argument does, viewing an
  // array's storage instead of loading a copy of it.
  if (isUnsizedArrayType(LHSTy)) {
    Value *SliceVal = CodegenStoredView(RHS.get());
    if (!SliceVal)
      return nullptr;
    Value *Ptr = LHS->codegenLValue();
    if (!Ptr)
      return nullptr;
    Builder->CreateStore(SliceVal, Ptr);
    return SliceVal;
  }

  // Generate code for the RHS first
  Value *Val = RHS->codegen();
  if (!Val)
    return nullptr;

    // Void value check is handled by TypeChecker.

  // Cast directly against LHSTy (the raw-pointer CastToType overload) rather
  // than round-tripping it through getLLVMType()/getTurfTypeFromLLVM() to
  // rebuild a TurfTypePtr. That roundtrip used to be here and is lossy for
  // anything getTurfTypeFromLLVM() doesn't specifically recognize (double,
  // int64/32, i1, pointer, void) -- notably STRUCTS, which aren't mentioned
  // in that function's own doc comment at all and silently fell through to
  // its catch-all `return TurfDouble();`. For a same-struct-type assignment
  // this happened to stay harmless only by coincidence: both the source and
  // destination sides degraded to "double" symmetrically, so CastToType's
  // getName()-equality fast path still passed the value through unchanged.
  // But it was fragile (relying on typesCompatible() upstream to guarantee
  // the two sides could never degrade *asymmetrically*) and actively
  // misleading to read (a struct assignment's "VarType" claiming to be
  // "double"). Using LHSTy directly is both a correctness hardening and a
  // simplification -- no roundtrip needed at all.
  Value *CastVal = CastToType(Val, LHSTy, "assigncast", Loc);

  // Generate the Store instruction using LValue
  Value *Ptr = LHS->codegenLValue();
  if (!Ptr)
    return nullptr;

  // FDN-004 (D17): `s[i] = c;` on a `string` base used to need a manual
  // i64->i8 truncation here, because the TypeChecker gave `s[i]` the
  // LOGICAL type `int` while ArrayAccessExprAST::codegenLValue() (correctly)
  // handed back a GEP into the underlying 1-BYTE `i8` storage -- storing a
  // full i64 there silently wrote 8 bytes at every index, corrupting
  // whatever followed in the buffer (confirmed live: `strings::substring()`
  // reliably corrupted glibc's heap metadata this way). Now that `s[i]`'s
  // real type IS `char` (i8), CastToType(Val, LHSTy, ...) above already
  // produces an i8 value -- its SrcType==DestType identity fast path fires
  // for any RHS that is already `char`, and the TypeChecker rejects
  // anything else (no implicit `char` <-> numeric conversion, FREEZE.md
  // §6.2 row 4) -- so no separate truncation step is needed or correct here
  // any more.
  Builder->CreateStore(CastVal, Ptr);

  // Assignment expressions usually return the value assigned (allows x = y = 5)
  return CastVal;
}


Value *CompoundAssignmentExprAST::codegen() {
  Value *Val = RHS->codegen();
  if (!Val) return nullptr;

  Value *Ptr = LHS->codegenLValue();
  if (!Ptr) return nullptr;

  const TurfType *LHSTy = getExprType(LHS.get());
  if (!LHSTy) {
    llvm::report_fatal_error("Internal error: Unknown LHS type for CompoundAssignment");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(LHSTy);
  Value *LHSVal = Builder->CreateLoad(LLVMTy, Ptr, "compassign.load");

  // ── Operator overload dispatch (F-OP-2's desugaring) ─────────────────────
  // TypeChecker::visitCompoundAssignment() already resolved
  // ResolvedOperatorTarget (if any) via exact (LHSType, RHSType) match.
  // LHSVal/Ptr above are already the CORRECT struct-typed load/address (no
  // getTurfTypeFromLLVM() round-trip needed for this path, unlike the
  // builtin numeric branches below) -- call the overload directly and
  // store its result back through the SAME Ptr, so the LHS location is
  // still only ever evaluated once (§13.5's explicit requirement).
  if (ResolvedOperatorTarget) {
    std::string MangledName = ResolvedOperatorTarget->mangledName();
    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: operator overload '" +
                          MangledName + "' was resolved but never codegenned."));
      return nullptr;
    }
    Value *Result = Builder->CreateCall(CalleeF, {LHSVal, Val}, "compassign.optmp");
    Builder->CreateStore(Result, Ptr);
    return Result;
  }

  TurfTypePtr VarType = getTurfTypeFromLLVM(LLVMTy);

  // `**=` handled separately, BEFORE the generic `CastRHS = CastToType(Val,
  // VarType, ...)` below: llvm.pow only has a double overload, so both
  // operands must go in as doubles regardless of LhsTy. Using the generic
  // CastRHS here would be wrong for an int LHS -- it truncates the exponent
  // to int BEFORE the call (`x **= 2.5;` on an int x would silently compute
  // `x ** 2`, not `x ** 2.5` rounded afterward). Mirrors visitBinary()'s
  // TOK_POWER codegen (both operands cast to double, llvm.pow, one shared
  // path) but additionally casts the double result back down to LHSTy
  // before the store, since a compound assignment must leave the variable's
  // own declared type unchanged.
  if (Opcode == TOK_POWER) {
    Value *BaseD = CastToType(LHSVal, TurfDouble(), "compassign.powbase", Loc);
    Value *ExpD = CastToType(Val, TurfDouble(), "compassign.powexp", Loc);
#if LLVM_VERSION_MAJOR >= 19
    Function *PowFunc = llvm::Intrinsic::getOrInsertDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
#else
    Function *PowFunc = llvm::Intrinsic::getDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
#endif
    Value *PowResultD = Builder->CreateCall(PowFunc, {BaseD, ExpD}, "powtmp");
    Value *Result = CastToType(PowResultD, VarType, "compassign.powcast", Loc);
    Builder->CreateStore(Result, Ptr);
    return Result;
  }

  Value *CastRHS = CastToType(Val, VarType, "compassign.cast", Loc);

  Value *Result = nullptr;
  bool isDouble = LHSTy->isNamed("double");

  switch (Opcode) {
  // Checked for int (never double) -- SEM-002 (FREEZE.md §6.2 row 7). This
  // duplicates BinaryExprAST::codegen()'s own +/-/* handling rather than
  // sharing it because CompoundAssignmentExprAST has never desugared to a
  // literal re-walk of BinaryExprAST at the Codegen layer (each already had
  // its own div-by-zero check, above/below, before this group) -- see
  // EmitCheckedIntArith()'s own doc comment (CodegenCore.cpp). OPT-002:
  // ProvenNoOverflow -- see BinaryExprAST::codegen()'s identical cases
  // (CodegenExpr.cpp) for the contract.
  case '+':
    Result = isDouble ? Builder->CreateFAdd(LHSVal, CastRHS, "addtmp")
             : ProvenNoOverflow ? Builder->CreateNSWAdd(LHSVal, CastRHS, "addtmp")
                                : EmitCheckedIntArith('+', LHSVal, CastRHS, Loc);
    break;
  case '-':
    Result = isDouble ? Builder->CreateFSub(LHSVal, CastRHS, "subtmp")
             : ProvenNoOverflow ? Builder->CreateNSWSub(LHSVal, CastRHS, "subtmp")
                                : EmitCheckedIntArith('-', LHSVal, CastRHS, Loc);
    break;
  case '*':
    Result = isDouble ? Builder->CreateFMul(LHSVal, CastRHS, "multmp")
             : ProvenNoOverflow ? Builder->CreateNSWMul(LHSVal, CastRHS, "multmp")
                                : EmitCheckedIntArith('*', LHSVal, CastRHS, Loc);
    break;
  case '/': {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB = BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB = BasicBlock::Create(*TheContext, "divcont", TheFunction);

    Value *IsZero = isDouble
                        ? Builder->CreateFCmpOEQ(CastRHS, ConstantFP::get(*TheContext, APFloat(0.0)))
                        : Builder->CreateICmpEQ(CastRHS, ConstantInt::get(Type::getInt64Ty(*TheContext), 0));

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenPanic(Builder->CreateGlobalString("Division by zero in compound assignment"), Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);
    // The one signed-division overflow case (INT64_MIN / -1) -- see
    // EmitSignedDivOverflowGuard()'s own doc comment.
    if (!isDouble)
      EmitSignedDivOverflowGuard(LHSVal, CastRHS, Loc);
    Result = isDouble ? Builder->CreateFDiv(LHSVal, CastRHS, "divtmp")
                      : Builder->CreateSDiv(LHSVal, CastRHS, "divtmp");
    break;
  }
  case '%': {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB = BasicBlock::Create(*TheContext, "modzero", TheFunction);
    BasicBlock *DivContBB = BasicBlock::Create(*TheContext, "modcont", TheFunction);

    Value *IsZero = isDouble
                        ? Builder->CreateFCmpOEQ(CastRHS, ConstantFP::get(*TheContext, APFloat(0.0)))
                        : Builder->CreateICmpEQ(CastRHS, ConstantInt::get(Type::getInt64Ty(*TheContext), 0));

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenPanic(Builder->CreateGlobalString("Modulo by zero in compound assignment"), Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);
    Result = isDouble ? Builder->CreateFRem(LHSVal, CastRHS, "modtmp")
                      : Builder->CreateSRem(LHSVal, CastRHS, "modtmp");
    break;
  }
  // Bitwise compound-assign: TypeChecker::visitCompoundAssignment() already
  // rejected anything but int operands for these, so LHSVal/CastRHS are
  // guaranteed int here -- no isDouble branching needed, unlike +/-/*/%.
  case '^':
    Result = Builder->CreateXor(LHSVal, CastRHS, "xorassigntmp");
    break;
  case '&':
    Result = Builder->CreateAnd(LHSVal, CastRHS, "andassigntmp");
    break;
  case '|':
    Result = Builder->CreateOr(LHSVal, CastRHS, "orassigntmp");
    break;
  case TOK_LSHIFT:
    // Checked -- see EmitCheckedShl()'s own doc comment (CodegenCore.cpp).
    // OPT-002: ProvenNoOverflow, same contract as the '+'/'-'/'*' cases above.
    Result = ProvenNoOverflow ? Builder->CreateShl(LHSVal, CastRHS, "lshiftassigntmp")
                              : EmitCheckedShl(LHSVal, CastRHS, Loc);
    break;
  case TOK_RSHIFT:
    Result = Builder->CreateAShr(LHSVal, CastRHS, "rshiftassigntmp");
    break;
  default:
    llvm::report_fatal_error("Unknown compound assignment operator");
  }

  Builder->CreateStore(Result, Ptr);
  return Result; // Compound assignment returns the new value
}


Value *PostfixExprAST::codegen() {
  Value *Ptr = Operand->codegenLValue();
  if (!Ptr) return nullptr;

  const TurfType *OpTy = getExprType(Operand.get());
  if (!OpTy) {
    llvm::report_fatal_error("Internal error: Unknown operand type for PostfixExpr");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(OpTy);
  Value *OldVal = Builder->CreateLoad(LLVMTy, Ptr, "postfix.load");

  // ── Operator overload dispatch (D21) ─────────────────────────────────────
  // TypeChecker::visitPostfixExpr() already resolved ResolvedOperatorTarget
  // (if any) via exact match against (OperandType, int). Postfix returns
  // the OLD value, prefix the NEW one -- everything about computing and
  // storing the new value is otherwise identical between the two.
  if (ResolvedOperatorTarget) {
    std::string MangledName = ResolvedOperatorTarget->mangledName();
    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      llvm::report_fatal_error(
          llvm::StringRef("Internal error: operator overload '" +
                          MangledName + "' was resolved but never codegenned."));
      return nullptr;
    }
    Value *DeltaOne = ConstantInt::get(*TheContext, APInt(64, 1, true));
    Value *NewVal = Builder->CreateCall(CalleeF, {OldVal, DeltaOne}, "postfix.optmp");
    Builder->CreateStore(NewVal, Ptr);
    return IsPrefix ? NewVal : OldVal;
  }

  bool isDouble = OpTy->isNamed("double");
  // OVH-005: the non-double path below unconditionally builds an i64 '1'
  // and (through EmitCheckedIntArith()) an i64-typed overflow-check
  // intrinsic call -- sound only when OldVal is genuinely i64. A
  // non-double, non-'int' numeric type (concretely: 'int32') reaching here
  // would load an i32 OldVal and emit a call with mismatched operand
  // widths -- invalid LLVM IR, a real internal-compiler-error, not a
  // diagnosable one. TypeChecker::visitPostfixExpr() now rejects exactly
  // that case before Codegen is ever reached (a dedicated error, not this
  // assertion) -- this exists so a FUTURE gap of the same shape (e.g. a new
  // numeric type added without updating that gate) fails loudly here
  // instead of silently emitting bad IR again.
  assert((isDouble || OpTy->isNamed("int")) &&
        "PostfixExprAST::codegen(): non-double operand must be 'int' -- "
        "TypeChecker::visitPostfixExpr() must reject any other numeric "
        "type (e.g. 'int32') before Codegen is reached.");
  Value *One = isDouble ? static_cast<Value *>(ConstantFP::get(*TheContext, APFloat(1.0)))
                        : static_cast<Value *>(ConstantInt::get(*TheContext, APInt(64, 1, true)));

  Value *NewVal = nullptr;
  if (Opcode == '+') {
    // Checked for int (never double) -- `x++`/`x--` is arithmetically
    // `x = x +/- 1`, so it gets the same SEM-002 overflow trap as any other
    // '+'/'-' -- see EmitCheckedIntArith()'s own doc comment (CodegenCore.cpp).
    // OPT-002: ProvenNoOverflow, same contract as BinaryExprAST's cases
    // (CodegenExpr.cpp).
    NewVal = isDouble ? Builder->CreateFAdd(OldVal, One, "inctmp")
             : ProvenNoOverflow ? Builder->CreateNSWAdd(OldVal, One, "inctmp")
                                : EmitCheckedIntArith('+', OldVal, One, Loc);
  } else {
    NewVal = isDouble ? Builder->CreateFSub(OldVal, One, "dectmp")
             : ProvenNoOverflow ? Builder->CreateNSWSub(OldVal, One, "dectmp")
                                : EmitCheckedIntArith('-', OldVal, One, Loc);
  }

  Builder->CreateStore(NewVal, Ptr);
  return IsPrefix ? NewVal : OldVal;
}
