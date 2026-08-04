/**
 * @file CodegenVar.cpp
 * @brief Variable codegen: declarations, references, assignment, compound assignment, postfix.
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
  // history and git history for the now-removed
  // docs/planning/CODE_REVIEW_GAPS_2026-07.md's original writeup) silently
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
    UnreachableCodeError(Loc, "declaration").raise();
    return nullptr;
  }

  // Check for duplicate declaration in current scope
  if (GlobalSymbolTable && GlobalSymbolTable->IsSymbolInCurrentScope(Name)) {
    Symbol *Prev = GlobalSymbolTable->LookupSymbolInCurrentScope(Name);
    if (Prev) {
      DuplicateDeclarationError(Loc, Name, Prev->DeclLoc).raise();
    } else {
      SyntaxError(Loc, "Variable already declared").raise();
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
    Init = InitVal->codegen();
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
  // Generate code for the RHS first
  Value *Val = RHS->codegen();
  if (!Val)
    return nullptr;

    // Void value check is handled by TypeChecker.

  // Determine the LHS type without evaluating it (avoid double-evaluation side effects)
  const TurfType *LHSTy = getExprType(LHS.get());
  if (!LHSTy)
    return nullptr;

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
  case '+':
    Result = isDouble ? Builder->CreateFAdd(LHSVal, CastRHS, "addtmp")
                      : Builder->CreateAdd(LHSVal, CastRHS, "addtmp");
    break;
  case '-':
    Result = isDouble ? Builder->CreateFSub(LHSVal, CastRHS, "subtmp")
                      : Builder->CreateSub(LHSVal, CastRHS, "subtmp");
    break;
  case '*':
    Result = isDouble ? Builder->CreateFMul(LHSVal, CastRHS, "multmp")
                      : Builder->CreateMul(LHSVal, CastRHS, "multmp");
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
    Result = Builder->CreateShl(LHSVal, CastRHS, "lshiftassigntmp");
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

  bool isDouble = OpTy->isNamed("double");
  Value *One = isDouble ? static_cast<Value *>(ConstantFP::get(*TheContext, APFloat(1.0)))
                        : static_cast<Value *>(ConstantInt::get(*TheContext, APInt(64, 1, true)));

  Value *NewVal = nullptr;
  if (Opcode == '+') {
    NewVal = isDouble ? Builder->CreateFAdd(OldVal, One, "inctmp")
                      : Builder->CreateAdd(OldVal, One, "inctmp");
  } else {
    NewVal = isDouble ? Builder->CreateFSub(OldVal, One, "dectmp")
                      : Builder->CreateSub(OldVal, One, "dectmp");
  }

  Builder->CreateStore(NewVal, Ptr);
  return OldVal; // Postfix returns the old value
}
