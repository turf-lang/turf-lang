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
#include <optional>
#include <set>
#include <unordered_map>

using namespace llvm;


// Issue #260: the `{ data, length }` slice a `T[]` parameter receives.
//
// The data pointer is the argument's OWN storage whenever it has one (a
// variable, a field, an element of an outer array), not a copy: writing
// `arr[i] = v` in the callee changes the caller's array, which is what `T[]`
// has always meant (and what makes `fill(arr, 0)`-style helpers possible).
// The TypeChecker refuses that write when the caller's array is `immut`
// (E141, TypeChecker::checkSliceWrites()). An argument with no storage of its
// own -- a literal `[1, 2, 3]`, a call returning `int[3]` -- is spilled to a
// slot in the caller's frame first; it used to be an internal compiler error
// ("array argument must be a variable"). Either way the storage outlives the
// call, and nothing can keep the slice past it: E140 keeps `T[]` out of every
// local, field and return type.
//
// An argument that is already a `T[]` (a parameter handed on to another
// function) is passed as it is. It used to pass the address of the slot that
// HELD the pointer, so the second function read garbage.
// > 0 while building a view that a VARIABLE will keep (CodegenStoredView()).
// Then a temporary is spilled to a fresh GC heap block rather than to the
// fixed entry-block slot: that slot is ONE per source location, so with
// `w = v; v = mk(i);` in a loop, w and v would both view it and w would
// silently change when v was refilled. An argument doesn't need this -- the
// slot is only read during the call it was made for.
static int StoredViewDepth = 0;

Value *CodegenStoredView(ExprAST *Source) {
  ++StoredViewDepth;
  Value *V = CodegenSliceArgument(Source);
  --StoredViewDepth;
  return V;
}

Value *CodegenSliceArgument(ExprAST *Arg) {
  const TurfType *ArgTy = getExprType(Arg);
  const auto *ArrTy = dynamic_cast<const ::ArrayType *>(ArgTy);
  if (!ArrTy) {
    llvm::report_fatal_error(
        "Internal error: a 'T[]' parameter was given a non-array argument. "
        "This should have been caught by the TypeChecker.");
    return nullptr;
  }
  if (ArrTy->getSize() < 0)
    return Arg->codegen();

  Value *Data = Arg->codegenLValue();
  if (!Data) {
    Value *Agg = Arg->codegen();
    if (!Agg)
      return nullptr;
    if (StoredViewDepth > 0) {
      Data = HeapAllocateValue(Agg); // traced per element, see its comment
    } else {
      Function *TheFunction = Builder->GetInsertBlock()->getParent();
      AllocaInst *Tmp = CreateEntryBlockAlloca(TheFunction, "slice.tmp", ArgTy);
      Builder->CreateStore(Agg, Tmp);
      Data = Tmp;
    }
  }
  Value *Slice = PoisonValue::get(getSliceLLVMType());
  Slice = Builder->CreateInsertValue(Slice, Data, 0, "slice.data");
  return Builder->CreateInsertValue(
      Slice, ConstantInt::get(Type::getInt64Ty(*TheContext), ArrTy->getSize()),
      1, "slice");
}


// `arr[start..end]`: a view of elements start..end-1 of the base array.
//
// The base's own `{ data, length }` comes from CodegenSliceArgument() (so a
// `T[N]` is viewed in place and a `T[]` passes through). Each bound present
// is evaluated once, left to right, and a negative one has the length added
// to it, as a negative index does. The range must then satisfy
// 0 <= start <= end <= length, or the program panics -- the TypeChecker has
// already refused the ranges it can prove wrong (E143). The result is
// `{ data + start, end - start }`: no element is copied.
Value *SliceExprAST::codegen() {
  Value *BaseSlice = CodegenSliceArgument(Base.get());
  if (!BaseSlice)
    return nullptr;
  Type *I64 = Type::getInt64Ty(*TheContext);
  Value *Data = Builder->CreateExtractValue(BaseSlice, 0, "slice.base");
  Value *Len = Builder->CreateExtractValue(BaseSlice, 1, "slice.baselen");
  Value *Zero = ConstantInt::get(I64, 0);

  auto Bound = [&](ExprAST *E, Value *Default, const char *Name) -> Value * {
    if (!E)
      return Default;
    Value *V = E->codegen();
    if (!V)
      return nullptr;
    V = CastToType(V, TurfInt(), Name, Loc);
    Value *IsNeg = Builder->CreateICmpSLT(V, Zero, "slice.isneg");
    return Builder->CreateSelect(IsNeg, Builder->CreateAdd(V, Len), V, Name);
  };
  Value *StartV = Bound(Start.get(), Zero, "slice.start");
  if (!StartV)
    return nullptr;
  Value *EndV = Bound(End.get(), Len, "slice.end");
  if (!EndV)
    return nullptr;

  // 0 <= start <= end <= length (a still-negative bound fails the first).
  Value *Bad = Builder->CreateOr(
      Builder->CreateOr(Builder->CreateICmpSLT(StartV, Zero),
                        Builder->CreateICmpSGT(StartV, EndV)),
      Builder->CreateICmpSGT(EndV, Len), "slice.oob");
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *OobBB = BasicBlock::Create(*TheContext, "slice_oob", TheFunction);
  BasicBlock *OkBB = BasicBlock::Create(*TheContext, "slice_ok");
  Builder->CreateCondBr(Bad, OobBB, OkBB);
  Builder->SetInsertPoint(OobBB);
  const auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base.get());
  CodegenPanic(Builder->CreateGlobalString(
                   "Slice range out of bounds for '" +
                   (BaseVar ? BaseVar->getName() : std::string("array")) + "'"),
               Loc);
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();
  TheFunction->insert(TheFunction->end(), OkBB);
  Builder->SetInsertPoint(OkBB);

  const auto *ArrTy = dynamic_cast<const ::ArrayType *>(getExprType(Base.get()));
  Type *ElemTy = getLLVMType(ArrTy->getElementType());
  Value *NewData = Builder->CreateInBoundsGEP(ElemTy, Data, StartV, "slice.data");
  Value *Result = Builder->CreateInsertValue(PoisonValue::get(getSliceLLVMType()),
                                             NewData, 0);
  return Builder->CreateInsertValue(Result, Builder->CreateSub(EndV, StartV), 1,
                                    "slice");
}


// Ensure malloc / realloc / free are declared in the module.


// Whether a value of type T has a `string` in it: itself, or a field or
// element at any depth (not through a pointer). Its zero value is then not
// its default, since a zero string is null and a default one is "".
static bool typeHoldsString(const TurfType *T, int Depth = 0) {
  if (!T || Depth > 16)
    return false;
  if (T->isNamed("string"))
    return true;
  if (const auto *AT = dynamic_cast<const ::ArrayType *>(T))
    return AT->getSize() > 0 && typeHoldsString(AT->getElementType(), Depth + 1);
  const auto *ST = dynamic_cast<const TurfStructType *>(T);
  if (!ST && GlobalSymbolTable && dynamic_cast<const NamedType *>(T))
    ST = dynamic_cast<const TurfStructType *>(GlobalSymbolTable->LookupType(T->getName()).get());
  if (!ST)
    return false;
  for (const auto &F : ST->getFields())
    if (typeHoldsString(F.Type.get(), Depth + 1))
      return true;
  return false;
}

// Array Codegen
Value *ArrayDeclExprAST::codegen() {
  // Validation (void type, keywords, duplicates, bounds, etc.) is handled by the
  // Resolver and TypeChecker passes before Codegen.


  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  Type *ElemTy = getLLVMType(ElementType.get());
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());

  // A local `T[]` (issue #260's range slicing) is a view of an existing
  // array (`= a`, `= a[1..]`, a `T[]` parameter, `= a.copy()`), built in place
  // exactly as an argument to a `T[]` parameter is. (`int[] = [1, 2, 3]` never
  // gets here: the Parser gives it the literal's size, so it is an int[3].)
  if (Size < 0) {
    AllocaInst *SliceSlot = TmpB.CreateAlloca(getSliceLLVMType(), nullptr, Name);
    Value *SliceVal = InitExpr ? CodegenStoredView(InitExpr.get()) : nullptr;
    if (!SliceVal)
      return nullptr; // the TypeChecker requires an initializer (E128)
    Builder->CreateStore(SliceVal, SliceSlot);

    TurfTypePtr SliceType = makeArrayType(ElementType, Size);
    if (GlobalSymbolTable)
      GlobalSymbolTable->DeclareSymbol(Name, SliceType, Loc, SliceSlot);
    NamedValues[Name] = {SliceSlot, SliceType};
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, Size);
  AllocaInst *Alloca = TmpB.CreateAlloca(ArrTy, nullptr, Name);

  // Zero-initialize the entire array (memset to 0)
  // Get total size in bytes
  auto &DL = TheModule->getDataLayout();
  uint64_t TotalBytes = DL.getTypeAllocSize(ArrTy);
  Builder->CreateMemSet(
      Alloca, ConstantInt::get(Type::getInt8Ty(*TheContext), 0),
      ConstantInt::get(Type::getInt64Ty(*TheContext), TotalBytes),
      Alloca->getAlign());

  // A zero `string` is a null pointer, not "": an element nothing wrote read
  // as null and segfaulted on use (owner decision 2026-10-12, found by
  // turf-fuzz). When any element is left unwritten and the element type
  // holds a string, every element gets its default value ("" for each
  // string, zero otherwise) in a loop, so a big array costs no code size.
  // A non-null `&U` inside the element is E128 without an initializer.
  if (!InitExpr && Size > 0 && static_cast<int>(InitList.size()) < Size &&
      typeHoldsString(ElementType.get())) {
    Value *Def = EmitDefaultValue(ElementType.get(), Loc);
    Type *I64 = Type::getInt64Ty(*TheContext);
    BasicBlock *PreBB = Builder->GetInsertBlock();
    BasicBlock *LoopBB = BasicBlock::Create(*TheContext, "arrfill", TheFunction);
    BasicBlock *DoneBB = BasicBlock::Create(*TheContext, "arrfill.done", TheFunction);
    Builder->CreateBr(LoopBB);
    Builder->SetInsertPoint(LoopBB);
    PHINode *I = Builder->CreatePHI(I64, 2, "arrfill.i");
    I->addIncoming(ConstantInt::get(I64, 0), PreBB);
    Value *Indices[] = {ConstantInt::get(I64, 0), I};
    Builder->CreateStore(Def, Builder->CreateInBoundsGEP(ArrTy, Alloca, Indices, "arrfill.elem"));
    Value *Next = Builder->CreateAdd(I, ConstantInt::get(I64, 1), "arrfill.next");
    I->addIncoming(Next, LoopBB);
    Builder->CreateCondBr(Builder->CreateICmpULT(Next, ConstantInt::get(I64, Size)),
                          LoopBB, DoneBB);
    Builder->SetInsertPoint(DoneBB);
  }

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

  // The element type TypeChecker::visitArrayLiteral() settled on (element
  // 0's). Codegen's own getExprType() cannot type every expression -- a
  // `give`/`otherwise` or, before e2560c1, `a.length()` as element 0 came
  // back null and crashed here (found by turf-fuzz) -- so it is only the
  // fallback for a literal the TypeChecker never visited.
  const TurfType *ElemType = nullptr;
  if (const auto *Cached = dynamic_cast<const ::ArrayType *>(CachedType.get()))
    ElemType = Cached->getElementType();
  if (!ElemType)
    ElemType = getExprType(Elements[0].get());
  if (!ElemType)
    llvm::report_fatal_error("Internal error: array literal element has no type");
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


// #256: a fixed-size array accepts Python-style negative indices -- `a[-1]` is
// the last element, `a[-N]` the first -- so the valid range is `-N <= i < N`,
// not `0 <= i < N`. This is the one place the wrap is decided, for both the
// l-value and the r-value access paths: a negative index has `N` added to it,
// and the result must then be a real position, so `a[-N-1]` and `a[N]` both
// still panic rather than wrapping a second time (no modular arithmetic: an
// off-by-one that happens to land back in range would hide the bug).
// `i + N` cannot overflow for `i < 0 && N > 0`. Constant indices never reach
// here -- the callers fold them at compile time and raise ArrayBoundsError.
//
// Returns the normalized, non-negative index to GEP with.
// @p SizeVal is the element count as an i64: a constant for a `T[N]`, the
// slice's own length field for a `T[]` parameter (issue #260).
// The name a bounds diagnostic or panic gives the indexed array: the
// variable's own name, or "array" for any other base (a field, a call).
static std::string ArrayDisplayName(const ExprAST *Base) {
  auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base);
  return BaseVar ? BaseVar->getName() : std::string("array");
}

static Value *EmitRuntimeBoundsCheck(Value *IndexVal, Value *SizeVal,
                                     const std::string &ArrayName,
                                     SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);

  Value *IsNeg = Builder->CreateICmpSLT(IndexVal, Zero, "idx_isneg");
  Value *Wrapped = Builder->CreateAdd(IndexVal, SizeVal, "idx_wrapped");
  IndexVal = Builder->CreateSelect(IsNeg, Wrapped, IndexVal, "idx_norm");

  // Check the normalized index: index < 0 || index >= size
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
  return IndexVal;
}

// Compile-time half of the same rule, for a constant index: validates
// `-N <= Idx < N` (raising ArrayBoundsError otherwise) and returns the
// non-negative position. `nullopt` means the error was raised.
static std::optional<long long>
FoldConstantArrayIndex(long long Idx, int ArraySize, ExprAST *Base,
                       SourceLocation Loc) {
  if (Idx >= -static_cast<long long>(ArraySize) && Idx < ArraySize)
    return Idx < 0 ? Idx + ArraySize : Idx;

  auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base);
  ArrayBoundsError Err(Loc, BaseVar ? BaseVar->getName() : "array", Idx,
                       ArraySize);
  if (BaseVar && GlobalSymbolTable) {
    if (Symbol *S = GlobalSymbolTable->LookupSymbol(BaseVar->getName()))
      Err.withRelated(S->DeclLoc, "array declared here");
  }
  Err.raise();
  return std::nullopt;
}


// FDN-004: bounds check for `s[i]` on a `string`. Unlike a Turf array, a
// `string` carries no compile-time (or even cached runtime) length -- it is
// a bare NUL-terminated `i8*` (F-STD-3, frozen) -- so the upper bound is a
// fresh `strlen(s)` call at every access, not a constant. This is the
// design doc's option (a) (ROADMAP_v2.0.0.md FDN-004 "Design notes"):
// correct and O(n) per access; a loop that indexes every byte is therefore
// O(n^2) unless the caller reaches for `strings::byte_at_unchecked()`
// instead.
//
// `CheckUpperBound` is false on the write path (codegenLValue() below) and
// true on the read path (codegen()): a write target is very often a
// `gc_alloc`'d buffer being filled one byte at a time BEFORE its NUL
// terminator is written (every strings.tr function that builds a string
// this way -- substring/replace/to_upper/to_lower/pad_left/pad_right --
// does exactly this), so `strlen(buf)` there reads however many
// (zero-initialized, from gc_alloc) leading zero bytes happen to precede
// the write cursor -- typically 0, which made EVERY first write into a
// fresh buffer panic immediately (confirmed live: this is not a
// hypothetical, the whole stdlib test suite failed this way the first time
// this check applied to writes). There is no way to recover a buffer's
// real allocated capacity from a bare `string` value to check against
// instead -- F-STD-3 is frozen. The negative-index half of the check is
// still meaningful and free of that problem, so it stays on both paths;
// only the `strlen`-based upper bound is read-only. This is a documented,
// intentional residual gap (see FDN-004's change record's *Known
// pitfalls*), not an oversight: `s[huge_positive_i] = c;` on an
// already-complete string is still unchecked, exactly as it always was.
//
// #256: a negative index counts from the end, Python-style (`s[-1]` is the
// last byte, `s[-strlen(s)]` the first), so the check is `-len <= i < len`.
// Returns the normalized, non-negative index to GEP with. On the read path
// the `strlen` is already there, so the wrap is a branch-free select. On the
// write path `strlen` must NOT run for an ordinary non-negative store (the
// buffer-fill case above), so a negative index alone takes a branch to
// compute it -- `s[-1] = c;` is the only kind of write that pays for it.
static Value *EmitStringBoundsCheck(Value *StrVal, Value *IndexVal,
                                    SourceLocation Loc, bool CheckUpperBound) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
  FunctionType *StrlenTy = FunctionType::get(
      Type::getInt64Ty(*TheContext), {PointerType::get(*TheContext, 0)},
      /*isVarArg=*/false);
  FunctionCallee StrlenFn = TheModule->getOrInsertFunction("strlen", StrlenTy);

  Value *IsNeg = Builder->CreateICmpSLT(IndexVal, Zero, "sidx_isneg");
  Value *LenVal = nullptr;
  if (CheckUpperBound) {
    LenVal = Builder->CreateCall(StrlenFn, {StrVal}, "strlen_bc");
    Value *Wrapped = Builder->CreateAdd(IndexVal, LenVal, "sidx_wrapped");
    IndexVal = Builder->CreateSelect(IsNeg, Wrapped, IndexVal, "sidx_norm");
  } else {
    BasicBlock *EntryBB = Builder->GetInsertBlock();
    BasicBlock *NegBB = BasicBlock::Create(*TheContext, "sidx_wrap", TheFunction);
    BasicBlock *JoinBB = BasicBlock::Create(*TheContext, "sidx_join");
    Builder->CreateCondBr(IsNeg, NegBB, JoinBB);

    Builder->SetInsertPoint(NegBB);
    Value *NegLen = Builder->CreateCall(StrlenFn, {StrVal}, "strlen_wr");
    Value *Wrapped = Builder->CreateAdd(IndexVal, NegLen, "sidx_wrapped");
    Builder->CreateBr(JoinBB);

    TheFunction->insert(TheFunction->end(), JoinBB);
    Builder->SetInsertPoint(JoinBB);
    PHINode *Norm = Builder->CreatePHI(Type::getInt64Ty(*TheContext), 2, "sidx_norm");
    Norm->addIncoming(Wrapped, NegBB);
    Norm->addIncoming(IndexVal, EntryBB);
    IndexVal = Norm;
  }

  // Wrapped-negative that is still below zero (`s[-len-1]`) lands here too.
  Value *OutOfBounds = Builder->CreateICmpSLT(IndexVal, Zero, "sidx_neg");
  if (CheckUpperBound) {
    Value *TooBig = Builder->CreateICmpSGE(IndexVal, LenVal, "sidx_toobig");
    OutOfBounds = Builder->CreateOr(OutOfBounds, TooBig, "soob");
  }

  BasicBlock *OobBB = BasicBlock::Create(*TheContext, "str_oob", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "str_ok");

  Builder->CreateCondBr(OutOfBounds, OobBB, ContBB);

  Builder->SetInsertPoint(OobBB);
  CodegenPanic(Builder->CreateGlobalString(
      "String index out of bounds"), Loc);

  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
  return IndexVal;
}


Value *ArrayAccessExprAST::codegenLValue() {
  // LEC-006: an `operator[]` read is a function call returning the element BY
  // VALUE, so there is no address to hand back. Returning null is the
  // existing "not addressable" answer -- FieldAccessExprAST::codegenLValue()
  // and MethodCallExprAST::codegen() both react to it by evaluating the
  // expression as an r-value and spilling it, which is exactly what
  // `list[i].field` / `list[i].method()` need. Before this, a struct-valued
  // element fell through to the "Base is not an array or pointer" fatal
  // error below: `bigList[1].c` was an ICE for any `List<T>` whose element
  // is a struct, however it was declared.
  //
  // The spill is only sound for a READ. A write into the spilled copy would
  // be silently lost, which is why the TypeChecker rejects every such write
  // (E098, TypeChecker::rejectOperatorTemporaryWrite()) before Codegen runs.
  if (ResolvedIndexGetTarget)
    return nullptr;

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

    // FDN-004: write-side -- no `strlen()` upper bound. See
    // EmitStringBoundsCheck()'s own doc comment for why it can't apply to a
    // write, and for how a negative (from-the-end) index is still handled.
    IndexVal =
        EmitStringBoundsCheck(StrVal, IndexVal, Loc, /*CheckUpperBound=*/false);

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

  int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
  const TurfType *ElemType = getArrayElementType(BaseTy);

  // A `T[N]` is indexed in place (its address); a `T[]` by its slice VALUE,
  // which also covers a base with no address of its own -- `nums[1..][0]`.
  Value *BaseLVal = nullptr;
  Value *Slice = nullptr;
  if (ArraySize >= 0) {
    BaseLVal = Base->codegenLValue();
    if (!BaseLVal)
      return nullptr;
  } else {
    Slice = Base->codegen();
    if (!Slice)
      return nullptr;
  }

  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;
  // Index type check is handled by TypeChecker.
  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      auto Folded =
          FoldConstantArrayIndex(CI->getSExtValue(), ArraySize, Base.get(), Loc);
      if (!Folded)
        return nullptr;
      IndexVal = ConstantInt::get(Type::getInt64Ty(*TheContext), *Folded);
    } else if (!ProvenInBounds) {
      // OPT-002: ProvenInBounds is set by ValueRangePass::markElidableChecks()
      // (ValueRange.cpp), which runs on a throwaway CFG BEFORE Body->codegen()
      // -- see that call site's own comment (CodegenFunc.cpp). F-OPT-2
      // (ARCHITECTURE_OVERHAUL.md §17.3): removal by proof only, independent
      // of `-O` level -- this is not gated on any optimization flag.
      IndexVal = EmitRuntimeBoundsCheck(
          IndexVal, ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize),
          ArrayDisplayName(Base.get()), Loc);
    }
  }

  Type *ElemTy = getLLVMType(ElemType);
  if (ArraySize > 0) {
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
    Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                        IndexVal};
    return Builder->CreateInBoundsGEP(ArrTy, BaseLVal, Indices, "arrelemptr");
  }

  // A `T[]` parameter (issue #260): the slice carries its length, so the
  // same `-len <= i < len` rule a `T[N]` gets applies here too, checked at
  // run time -- even a constant index, since no length is known before the
  // call. Before slices this was raw, unchecked pointer arithmetic.
  Value *DataPtr = Builder->CreateExtractValue(Slice, 0, "slice.data");
  Value *LenVal = Builder->CreateExtractValue(Slice, 1, "slice.len");
  IndexVal = EmitRuntimeBoundsCheck(IndexVal, LenVal,
                                    ArrayDisplayName(Base.get()), Loc);
  return Builder->CreateInBoundsGEP(ElemTy, DataPtr, IndexVal, "arrelemptr");
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
    if (SelfByRef && !BaseVal) {
      // The base has no address (`outer[a][b]`, where `outer[a]` is itself an
      // operator[] result). A read through a copy is a correct read, so spill
      // the value and pass that slot's address.
      if (Value *Tmp = Base->codegen()) {
        BaseVal = Builder->CreateAlloca(Tmp->getType(), nullptr, "tmp.idxbase");
        Builder->CreateStore(Tmp, BaseVal);
      }
    }
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
    return CallOperator(CalleeF, *ResolvedIndexGetTarget, {BaseVal, IndexVal}, Loc, "idxoptmp");
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

    // FDN-004: read-side bounds check -- see EmitStringBoundsCheck()'s own
    // doc comment for why this can't be a fixed-size check like an array's.
    IndexVal =
        EmitStringBoundsCheck(StrVal, IndexVal, Loc, /*CheckUpperBound=*/true);

    Value *CharPtr = Builder->CreateGEP(Type::getInt8Ty(*TheContext), StrVal,
                                        IndexVal, "charptr");
    // FDN-004 (D17): `s[i]` is `char`, not `int` -- no ZExt to i64 here
    // anymore. `char` and its i8 storage are the exact same width, so the
    // load result IS the value; widening it was only ever needed to match
    // the old (wrong) `int` result type.
    return Builder->CreateLoad(Type::getInt8Ty(*TheContext), CharPtr,
                               "charval");
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
      auto Folded =
          FoldConstantArrayIndex(CI->getSExtValue(), ArraySize, Base.get(), Loc);
      if (!Folded)
        return nullptr;
      return Builder->CreateExtractValue(BaseVal, static_cast<unsigned>(*Folded),
                                         "arrextract");
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
        IndexVal = EmitRuntimeBoundsCheck(
            IndexVal, ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize),
            ArrayDisplayName(Base.get()), Loc);

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
