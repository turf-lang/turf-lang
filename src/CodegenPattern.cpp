/**
 * @file CodegenPattern.cpp
 * @brief Lowering of `compare` with destructuring patterns and guards
 *        (LEC-003, FREEZE.md §6.17 / F-PAT-1).
 *
 * @details A compare whose headers are all plain value patterns keeps the
 *          original `switch`/`strcmp` lowering in CodegenControlFlow.cpp
 *          untouched -- every pre-LEC-003 program generates exactly the code
 *          it always did. A compare with a struct pattern, `_`, or a guard
 *          comes here, because those cannot be a `switch`: a guard is an
 *          arbitrary expression and a struct pattern is a sequence of loads
 *          and tests.
 *
 *          The lowering is an ordered test chain. The subject is evaluated
 *          ONCE and copied to a stack slot; each header, in source order, is a
 *          run of field loads and comparisons that branches to the next header
 *          on the first mismatch, then its guard (if any), then the arm's
 *          body. "First header that matches wins" is therefore a property of
 *          the control-flow graph, not of any ordering LLVM might choose --
 *          which is what FREEZE.md §6.17 freezes.
 *
 *          Bindings are plain allocas, one per name per arm (stacked headers
 *          share the body, so they share the slot). They are registered in
 *          NamedValues and the codegen symbol table for the guard and body
 *          only, exactly as a `for`-each variable is.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "AST.h"
#include "Codegen.h"
#include "CodegenInternal.h"
#include "Errors.h"
#include "Monomorphize.h"
#include "SymbolTable.h"
#include "Types.h"

#include <optional>
#include <unordered_map>

using namespace llvm;

// A pattern is lowered by CompareExprAST::codegenPatterns() as part of an
// arm's test, never as an expression. Reaching one of these means a pass
// visited a header as if it were a value -- a compiler bug, not a user error.
Value *WildcardPatternExprAST::codegen() {
  report_fatal_error("Internal error: a wildcard pattern reached Codegen as an expression.");
}
Value *BindingPatternExprAST::codegen() {
  report_fatal_error("Internal error: a binding pattern reached Codegen as an expression.");
}
Value *StructPatternExprAST::codegen() {
  report_fatal_error("Internal error: a struct pattern reached Codegen as an expression.");
}
Value *ErrorVariantPatternExprAST::codegen() {
  report_fatal_error("Internal error: an error-variant pattern reached Codegen as an expression.");
}

namespace {

using AllocaMap = std::unordered_map<std::string, AllocaInst *>;

// The registered struct a matched type names: FieldAccessExprAST's own three
// steps (NamedType -> registry, GenericType -> the instantiation's mangled
// registry entry, else already the struct). A pattern must land on exactly the
// type `p.field` would, or its GEP indices would disagree with the layout.
const TurfStructType *structTypeOf(const TurfType *T) {
  if (!T)
    return nullptr;
  if (const auto *NT = dynamic_cast<const NamedType *>(T)) {
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        return dynamic_cast<const TurfStructType *>(R.get());
    return nullptr;
  }
  if (const auto *GT = dynamic_cast<const GenericType *>(T)) {
    if (GlobalSymbolTable) {
      std::string Mangled =
          Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs());
      if (auto R = GlobalSymbolTable->LookupType(Mangled))
        return dynamic_cast<const TurfStructType *>(R.get());
    }
    return nullptr;
  }
  return dynamic_cast<const TurfStructType *>(T);
}

// Emits the test of ONE pattern against the value stored at @p Addr (whose
// type is @p Ty, loaded as LLVM type @p LoadTy when the pattern needs a
// scalar). On success the builder is left inserting into the block where the
// next test goes; on a mismatch control branches to @p FailBB. Bindings store
// the matched value into their arm's slot as they go -- a partly-matched
// header may have stored some, which is harmless: the slots are only read once
// the whole header AND its guard passed.
bool emitPatternTest(ExprAST *Pat, Value *Addr, const TurfType *Ty, Type *LoadTy,
                     BasicBlock *FailBB, Function *TheFunction, AllocaMap &Slots) {
  if (dynamic_cast<WildcardPatternExprAST *>(Pat))
    return true;

  if (auto *B = dynamic_cast<BindingPatternExprAST *>(Pat)) {
    const TurfType *BTy = B->ResolvedVar && B->ResolvedVar->Type
                              ? B->ResolvedVar->Type.get()
                              : Ty;
    Value *V = Builder->CreateLoad(getLLVMType(BTy), Addr, B->getName());
    Builder->CreateStore(V, Slots.at(B->getName()));
    return true;
  }

  if (auto *S = dynamic_cast<StructPatternExprAST *>(Pat)) {
    const TurfType *ValTy = S->MatchedType.get();
    Value *StructAddr = Addr;
    if (const auto *PT = dynamic_cast<const TurfPointerType *>(ValTy)) {
      // Matched through a reference: the slot holds the pointer, the struct is
      // what it points at.
      StructAddr = Builder->CreateLoad(PointerType::get(*TheContext, 0), Addr,
                                       "pat.deref");
      ValTy = PT->getPointee().get();
    }
    const TurfStructType *ST = structTypeOf(ValTy);
    auto *LLVMST = ST ? dyn_cast<StructType>(getLLVMType(ST)) : nullptr;
    if (!LLVMST)
      report_fatal_error("Internal error: a struct pattern's type is not a "
                         "registered struct at Codegen. The TypeChecker should "
                         "have rejected it.");

    for (auto &F : S->getFieldsMut()) {
      // `x: _` and `{ .. }` ignored fields need no load at all.
      if (dynamic_cast<WildcardPatternExprAST *>(F.Sub.get()))
        continue;
      int Idx = ST->getFieldIndex(F.Name);
      Value *FieldAddr = Builder->CreateStructGEP(LLVMST, StructAddr, Idx, "pat.field");
      if (!emitPatternTest(F.Sub.get(), FieldAddr, F.FieldType.get(),
                           getLLVMType(F.FieldType.get()), FailBB, TheFunction, Slots))
        return false;
    }
    return true;
  }

  // An error-variant pattern (LEC-004): the slot holds a `TurfErrorValue*`.
  // The header matches when its tag is this variant's (strcmp on the tag
  // strings, exactly what `err == E::V` compares), and then each listed
  // payload field, read positionally out of the payload struct, matches its
  // sub-pattern. A variant with no fields has a null payload, which is never
  // read: the TypeChecker only lets a pattern with nothing to destructure name
  // no fields, so `NeedsPayload` below is false for it.
  if (auto *EV = dynamic_cast<ErrorVariantPatternExprAST *>(Pat)) {
    const auto *Head = static_cast<const ScopeAccessExprAST *>(EV->getHead());
    Value *ErrPtr = Builder->CreateLoad(PointerType::get(*TheContext, 0), Addr, "pat.err");

    Value *Tag = LoadErrorValueField(ErrPtr, /*FieldIdx=*/0, "tag");
    Value *Cmp = Builder->CreateCall(
        getOrCreateStrcmpFn(),
        {Tag, BuildErrorVariantTag(Head->getLeftName(), Head->getRightName())},
        "pat.tagcmp");
    Value *SameTag = Builder->CreateICmpEQ(
        Cmp, ConstantInt::get(Type::getInt32Ty(*TheContext), 0), "pat.tageq");
    BasicBlock *TagOkBB = BasicBlock::Create(*TheContext, "pat.tagok", TheFunction);
    Builder->CreateCondBr(SameTag, TagOkBB, FailBB);
    Builder->SetInsertPoint(TagOkBB);

    bool NeedsPayload = false;
    for (const auto &Sub : EV->getSubs())
      if (!dynamic_cast<WildcardPatternExprAST *>(Sub.get()))
        NeedsPayload = true;
    if (!NeedsPayload)
      return true;

    llvm::StructType *PTy =
        getErrorPayloadStructType(Head->getLeftName(), Head->getRightName());
    if (!PTy)
      report_fatal_error("Internal error: an error-variant pattern names fields "
                         "of a variant that has none. The TypeChecker should "
                         "have rejected it.");
    Value *Payload = LoadErrorValueField(ErrPtr, /*FieldIdx=*/2, "payload");
    for (size_t I = 0; I < EV->getSubs().size(); ++I) {
      ExprAST *Sub = EV->getSubsMut()[I].get();
      if (dynamic_cast<WildcardPatternExprAST *>(Sub))
        continue;
      Value *FieldAddr = Builder->CreateStructGEP(PTy, Payload, I, "pat.payload");
      if (!emitPatternTest(Sub, FieldAddr, EV->FieldTypes[I].get(),
                           getLLVMType(EV->FieldTypes[I].get()), FailBB, TheFunction,
                           Slots))
        return false;
    }
    return true;
  }

  // A value pattern: equality against the loaded value. A string compares by
  // content (strcmp, exactly as the original string compare does); every other
  // comparable type is an integer at this level -- int, int32, char, bool, and
  // an enum or machine's ordinal -- widened to i64 on BOTH sides by the same
  // conversion, so equal values can never disagree on an extension.
  Value *Loaded = Builder->CreateLoad(LoadTy, Addr, "pat.val");
  Value *PatV = Pat->codegen();
  if (!PatV)
    return false;

  Value *Eq;
  if (Ty && Ty->isNamed("string")) {
    FunctionType *StrcmpTy = FunctionType::get(
        Type::getInt32Ty(*TheContext),
        {PointerType::get(*TheContext, 0), PointerType::get(*TheContext, 0)},
        /*isVarArg=*/false);
    FunctionCallee StrcmpFn = TheModule->getOrInsertFunction("strcmp", StrcmpTy);
    Value *Cmp = Builder->CreateCall(StrcmpFn, {Loaded, PatV}, "pat.strcmp");
    Eq = Builder->CreateICmpEQ(Cmp, ConstantInt::get(Type::getInt32Ty(*TheContext), 0),
                               "pat.streq");
  } else {
    Value *L = CastToType(Loaded, TurfInt(), "pat.l");
    Value *R = CastToType(PatV, TurfInt(), "pat.r");
    Eq = Builder->CreateICmpEQ(L, R, "pat.eq");
  }

  BasicBlock *NextBB = BasicBlock::Create(*TheContext, "pat.next", TheFunction);
  Builder->CreateCondBr(Eq, NextBB, FailBB);
  Builder->SetInsertPoint(NextBB);
  return true;
}

} // namespace


Value *CompareExprAST::codegenPatterns() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  SetDebugLoc(Loc);

  // The subject is evaluated exactly once and snapshotted into a stack slot,
  // so every header reads the same value even if a guard calls something that
  // changes the variable the subject came from. The slot is typed from the
  // VALUE just produced (not from the AST type), so a load can never disagree
  // with the store about width. It lives in the entry block: a compare inside
  // a loop must not grow the stack once per iteration.
  Value *SubjectV = Subject->codegen();
  if (!SubjectV)
    return nullptr;
  const TurfType *SubjectTy = getExprType(Subject.get());

  IRBuilder<> EntryB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
  AllocaInst *SubjectSlot =
      EntryB.CreateAlloca(SubjectV->getType(), nullptr, "compare.subj");
  Builder->CreateStore(SubjectV, SubjectSlot);

  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "compare.end");
  BasicBlock *OtherBB =
      OtherBody ? BasicBlock::Create(*TheContext, "compare.otherwise") : nullptr;

  for (size_t I = 0; I < Arms.size(); ++I) {
    MatchesArm &Arm = Arms[I];
    SetDebugLoc(Arm.Loc);

    // The arm's bindings: one slot per name, visible to every header's guard
    // and to the body, and to nothing after the arm.
    AllocaMap Slots;
    std::vector<std::pair<std::string, std::optional<VarInfo>>> SavedNamed;
    if (GlobalSymbolTable)
      GlobalSymbolTable->EnterScope();
    for (const auto &Sym : Arm.Bindings) {
      AllocaInst *Slot = CreateEntryBlockAlloca(TheFunction, Sym->Name, Sym->Type.get());
      Slots[Sym->Name] = Slot;

      auto Old = NamedValues.find(Sym->Name);
      SavedNamed.push_back({Sym->Name, Old == NamedValues.end()
                                           ? std::nullopt
                                           : std::optional<VarInfo>(Old->second)});
      NamedValues[Sym->Name] = {Slot, Sym->Type};
      if (GlobalSymbolTable)
        GlobalSymbolTable->DeclareSymbol(Sym->Name, Sym->Type, Arm.Loc, Slot);
    }

    BasicBlock *BodyBB =
        BasicBlock::Create(*TheContext, "compare.arm." + std::to_string(I));

    // Each header is an independent alternative, tried in source order. The
    // fall-through of the LAST header is where the next arm's tests begin.
    for (size_t K = 0; K < Arm.Values.size(); ++K) {
      BasicBlock *FailBB = BasicBlock::Create(*TheContext, "compare.next");

      if (!emitPatternTest(Arm.Values[K].get(), SubjectSlot, SubjectTy,
                           SubjectV->getType(), FailBB, TheFunction, Slots))
        return nullptr;

      if (K < Arm.Guards.size() && Arm.Guards[K]) {
        Value *G = Arm.Guards[K]->codegen();
        if (!G)
          return nullptr;
        G = CastToType(G, TurfBool(), "guard");
        Builder->CreateCondBr(G, BodyBB, FailBB);
      } else {
        // The pattern matched. For an irrefutable header nothing above
        // branched to FailBB, so it has no predecessors: a dead block, which
        // is exactly right -- the next header or arm is unreachable behind a
        // catch-all, and LLVM removes it.
        Builder->CreateBr(BodyBB);
      }

      TheFunction->insert(TheFunction->end(), FailBB);
      Builder->SetInsertPoint(FailBB);
    }

    // The body. Emitted out of line so the test chain stays one straight run:
    // after it, the builder returns to the last header's failure block, which
    // is where the next arm starts.
    BasicBlock *Resume = Builder->GetInsertBlock();
    TheFunction->insert(TheFunction->end(), BodyBB);
    Builder->SetInsertPoint(BodyBB);
    if (!Arm.Body->codegen()) {
      if (GlobalSymbolTable)
        GlobalSymbolTable->ExitScope();
      return nullptr;
    }
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
    Builder->SetInsertPoint(Resume);

    if (GlobalSymbolTable)
      GlobalSymbolTable->ExitScope();
    for (auto &[Name, Saved] : SavedNamed) {
      if (Saved)
        NamedValues[Name] = *Saved;
      else
        NamedValues.erase(Name);
    }
  }

  // Nothing matched: `otherwise`, or fall out of the compare.
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateBr(OtherBB ? OtherBB : MergeBB);

  if (OtherBB) {
    TheFunction->insert(TheFunction->end(), OtherBB);
    Builder->SetInsertPoint(OtherBB);
    if (!OtherBody->codegen())
      return nullptr;
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
  }

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}
