#include "Codegen.h"
#include "AST.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "CFG.h"
#include "Errors.h"
#include "Lexer.h"
#include "SuggestionIndex.h"
#include "Lint.h"
#include "SymbolTable.h"
#include "Types.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <iostream>

using namespace llvm;

std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<IRBuilder<>> Builder;
std::unique_ptr<Module> TheModule;

std::map<std::string, VarInfo> NamedValues;

// Current enclosing function context (nullptr at top level)
TurfTypePtr CurrentFuncReturnType = TurfVoid();
llvm::Function *CurrentFunction = nullptr;

std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> LoopBlocks;

// Tracks full param types for user-defined functions.
static std::map<std::string, std::vector<TurfTypePtr>> FuncParamTypes;
static std::map<std::string, TurfTypePtr> FuncReturnTypes;

// Type cache for Monomorphization: maps type name -> LLVM type
static std::map<std::string, llvm::Type *> LLVMTypeCache;

// Forward declarations for list helpers (defined after Array Codegen)
static llvm::StructType *GetListStructType();
static llvm::Function *GetOrDeclareMalloc();
static llvm::Function *GetOrDeclareRealloc();
static void EmitListBoundsCheck(llvm::Value *, llvm::Value *, const std::string &, SourceLocation);

void InitializeModule() {
  // Holds types and constants
  TheContext = std::make_unique<LLVMContext>();

  // Holds functions
  TheModule = std::make_unique<Module>("Turf Compiler", *TheContext);

  // Builder to insert instructions
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

// Monomorphizing getLLVMType: maps a TurfType* to the corresponding llvm::Type*.
// For generics (list<T>), creates a unique LLVM struct per instantiation and caches it.
llvm::Type *getLLVMType(const TurfType *T) {
  if (!T) return llvm::Type::getDoubleTy(*TheContext);
  std::string Key = T->getName();

  auto It = LLVMTypeCache.find(Key);
  if (It != LLVMTypeCache.end())
    return It->second;

  // Primitive named types
  if (T->isNamed("int"))    return LLVMTypeCache[Key] = llvm::Type::getInt64Ty(*TheContext);
  if (T->isNamed("double")) return LLVMTypeCache[Key] = llvm::Type::getDoubleTy(*TheContext);
  if (T->isNamed("bool"))   return LLVMTypeCache[Key] = llvm::Type::getInt1Ty(*TheContext);
  if (T->isNamed("string")) return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);
  if (T->isNamed("void"))   return LLVMTypeCache[Key] = llvm::Type::getVoidTy(*TheContext);

  // list<T> — heap-allocated struct, stored as opaque pointer in NamedValues.
  // The actual struct layout is created once per element type in GetListStructType.
  if (isListType(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

  // Array types — the alloca is created per-site; return opaque pointer here.
  if (isArrayType(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

  // PointerType — lowers to ptr
  if (dynamic_cast<const TurfPointerType*>(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

  // User-defined Structs - they should be in the cache if declared, but if we call this 
  // before codegen of StructDecl, it might be missing. We shouldn't generate it here 
  // because we need the fields. So if it's a TurfStructType and not in cache, we're in trouble.
  if (auto *ST = dynamic_cast<const TurfStructType*>(T)) {
    // If we reach here, StructDecl codegen hasn't happened or it failed.
    // We'll return an opaque struct to avoid crashing.
    return LLVMTypeCache[Key] = llvm::StructType::create(*TheContext, Key);
  }

  // Unknown / future types
  return LLVMTypeCache[Key] = llvm::Type::getDoubleTy(*TheContext);
}

// Recover a TurfTypePtr from an LLVM Value type.
// Pointers are ambiguous (could be string or list), so callers that need
// precise list-vs-string info should use NamedValues directly.
static TurfTypePtr getTurfTypeFromLLVM(llvm::Type *Ty) {
  if (Ty->isDoubleTy())     return TurfDouble();
  if (Ty->isIntegerTy(64))  return TurfInt();
  if (Ty->isIntegerTy(32))  return TurfInt();
  if (Ty->isIntegerTy(1))   return TurfBool();
  if (Ty->isPointerTy())    return TurfString(); // conservative; callers may refine
  if (Ty->isVoidTy())       return TurfVoid();
  return TurfDouble();
}

static int getTypeRank(const TurfType *T) {
  if (!T) return 0;
  if (T->isNamed("double")) return 3;
  if (T->isNamed("int"))    return 2;
  if (T->isNamed("bool"))   return 1;
  if (T->isNamed("string")) return 4;
  return 0;
}

static TurfTypePtr getCommonType(TurfTypePtr A, TurfTypePtr B) {
  return getTypeRank(A.get()) >= getTypeRank(B.get()) ? A : B;
}

static TurfTypePtr RefineLLVMType(TurfTypePtr Deduced, TurfTypePtr Expected, llvm::Type *LLVMTy) {
  if (LLVMTy->isPointerTy() && Expected && (isListType(Expected.get()) || isArrayType(Expected.get()) || dynamic_cast<const TurfPointerType*>(Expected.get()) || Expected->isNamed("void*"))) {
    return Expected;
  }
  return Deduced;
}

// Check whether two types are implicitly compatible.
// Numerics (int, double, bool) are mutually compatible; string and void are not.
static bool isTypeCompatible(const TurfType *From, const TurfType *To) {
  if (!From || !To) return false;
  if (From->getName() == To->getName()) return true;

  // Auto-coerce void* to/from any pointer
  if (From->getName() == "void*" && dynamic_cast<const TurfPointerType*>(To)) return true;
  if (To->getName() == "void*" && dynamic_cast<const TurfPointerType*>(From)) return true;

  if (From->isVoid() || To->isVoid()) return false;
  if (From->isNamed("string") || To->isNamed("string")) return false;
  // Remaining numerics: int, double, bool — all mutually convertible
  return true;
}

static const TurfType *getExprType(const ExprAST *E) {
  if (!E) return nullptr;
  if (auto *Num = dynamic_cast<const NumberExprAST*>(E)) {
    if (!Num->isInteger()) return TurfDouble().get();
    return TurfInt().get();
  }
  if (auto *BoolE = dynamic_cast<const BoolExprAST*>(E)) {
    return TurfBool().get();
  }
  if (auto *Str = dynamic_cast<const StringExprAST*>(E)) {
    return TurfString().get();
  }
  if (auto *Bin = dynamic_cast<const BinaryExprAST*>(E)) {
    char Op = Bin->getOp();
    if (Op == '<' || Op == '>' || Op == '!' || Op == '=') return TurfBool().get();
    const TurfType *LType = getExprType(Bin->getLHS());
    const TurfType *RType = getExprType(Bin->getRHS());
    if (!LType) return RType;
    if (!RType) return LType;
    if (LType->isNamed("double") || RType->isNamed("double")) return TurfDouble().get();
    return LType;
  }
  if (auto *Unary = dynamic_cast<const UnaryExprAST*>(E)) {
    return getExprType(Unary->getOperand());
  }
  if (auto *FuncCall = dynamic_cast<const FuncCallExprAST*>(E)) {
    if (auto *VarCallee = dynamic_cast<const VariableExprAST*>(FuncCall->getCallee())) {
      std::string CalleeName = VarCallee->getName();
      
      if (CalleeName == "typeof") return TurfString().get();
      if (CalleeName == "lengthof") return TurfInt().get();
      if (CalleeName == "malloc" || CalleeName == "realloc") return std::make_shared<TurfPointerType>(TurfVoid()).get();
      if (CalleeName == "free") return TurfVoid().get();
      
      std::string MangledName = (CalleeName == "main") ? CalleeName : "turf_" + CalleeName;
      std::vector<const TurfType*> ArgTypes;
      for (const auto &Arg : FuncCall->getArgs()) {
        const TurfType *T = getExprType(Arg.get());
        if (!T) return nullptr;
        ArgTypes.push_back(T);
        if (CalleeName != "main") MangledName += "_" + T->getMangledName();
      }
      auto It = FuncReturnTypes.find(MangledName);
      if (It != FuncReturnTypes.end()) return It->second.get();
      
      for (const auto &[Name, ParamTypes] : FuncParamTypes) {
        std::string Prefix = "turf_" + CalleeName;
        if (Name == Prefix || (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
          if (ParamTypes.size() == ArgTypes.size()) {
            bool Compatible = true;
            for (size_t i = 0; i < ArgTypes.size(); ++i) {
              if (!isTypeCompatible(ArgTypes[i], ParamTypes[i].get())) {
                Compatible = false;
                break;
              }
            }
            if (Compatible) return FuncReturnTypes[Name].get();
          }
        }
      }
    }
    return nullptr;
  }
  if (auto *Cast = dynamic_cast<const CastExprAST*>(E)) {
    return Cast->getDestType();
  }
  if (auto *Var = dynamic_cast<const VariableExprAST*>(E)) {
    if (GlobalSymbolTable) {
      Symbol *Sym = GlobalSymbolTable->LookupSymbol(Var->getName());
      if (Sym) return Sym->Type.get();
    }
    auto It = NamedValues.find(Var->getName());
    if (It != NamedValues.end()) return It->second.Type.get();
  }
  if (auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST*>(E)) {
    const TurfType *BaseTy = getExprType(ArrAcc->getBase());
    if (BaseTy && isArrayType(BaseTy)) return getArrayElementType(BaseTy);
    if (BaseTy && isListType(BaseTy)) return getListElementType(BaseTy);
  }
  if (auto *MethodCall = dynamic_cast<const MethodCallExprAST*>(E)) {
    const TurfType *BaseTy = getExprType(MethodCall->getBase());
    if (auto *PtrTy = dynamic_cast<const TurfPointerType*>(BaseTy)) BaseTy = PtrTy->getPointee().get();

    if (MethodCall->getMethod() == "pop") {
      if (BaseTy && isListType(BaseTy)) return getListElementType(BaseTy);
    }
    if (MethodCall->getMethod() == "push") {
      return BaseTy;
    }
    if (BaseTy && dynamic_cast<const TurfStructType*>(BaseTy)) {
      std::string MethodName = BaseTy->getName() + "." + MethodCall->getMethod();
      std::string MangledName = "turf_" + MethodName + "_" + BaseTy->getName() + "_ptr";
      std::vector<const TurfType*> ArgTypes;
      for (const auto &Arg : MethodCall->getArgs()) {
        const TurfType *T = getExprType(Arg.get());
        if (!T) return nullptr;
        ArgTypes.push_back(T);
        MangledName += "_" + T->getMangledName();
      }
      auto It = FuncReturnTypes.find(MangledName);
      if (It != FuncReturnTypes.end()) return It->second.get();
      
      for (const auto &[Name, ParamTypes] : FuncParamTypes) {
        std::string Prefix = "turf_" + MethodName;
        if (Name == Prefix || (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
          if (ParamTypes.size() == ArgTypes.size() + 1) {
            bool Compatible = true;
            for (size_t i = 0; i < ArgTypes.size(); ++i) {
              if (!isTypeCompatible(ArgTypes[i], ParamTypes[i + 1].get())) {
                Compatible = false;
                break;
              }
            }
            if (Compatible) return FuncReturnTypes[Name].get();
          }
        }
      }
    }
  }
  if (auto *StructInit = dynamic_cast<const StructInitExprAST*>(E)) {
    if (GlobalSymbolTable) {
      if (auto T = GlobalSymbolTable->LookupType(StructInit->getName())) {
        return T.get();
      }
    }
  }
  if (auto *FieldAccess = dynamic_cast<const FieldAccessExprAST*>(E)) {
    const TurfType *BaseTy = getExprType(FieldAccess->getBase());
    if (auto *PtrTy = dynamic_cast<const TurfPointerType*>(BaseTy)) BaseTy = PtrTy->getPointee().get();

    if (auto *STy = dynamic_cast<const TurfStructType*>(BaseTy)) {
      // Return a raw pointer of the underlying TurfTypePtr to match return signature.
      // But we can't easily get the raw pointer from TurfTypePtr without storing it or retrieving it.
      // Actually we can:
      return STy->getFieldType(FieldAccess->getFieldName()).get();
    }
  }
  return nullptr;
}

static Value *CastToType(Value *Val, TurfTypePtr DestType, const std::string &Name,
                         SourceLocation Loc = {0, 0}) {
  Type *ValTy = Val->getType();

  // Normalize foreign integer widths (e.g. i32 from printf) to i64
  if (ValTy->isIntegerTy(32)) {
    Val = Builder->CreateSExt(Val, Type::getInt64Ty(*TheContext), "i32toi64");
    ValTy = Val->getType();
  }

  TurfTypePtr SrcType = getTurfTypeFromLLVM(ValTy);
  SrcType = RefineLLVMType(SrcType, DestType, ValTy);
  if (SrcType->getName() == DestType->getName())
    return Val;

  // Auto-coerce void* to/from any pointer
  if (SrcType->getName() == "void*" && dynamic_cast<const TurfPointerType*>(DestType.get()))
    return Val;
  if (DestType->getName() == "void*" && dynamic_cast<const TurfPointerType*>(SrcType.get()))
    return Val;

  // Strings cannot be cast to/from other types implicitly
  if (SrcType->isNamed("string") || DestType->isNamed("string")) {
    TypeError(CurLoc, "Cannot cast between string and non-string types").raise();
    return Val;
  }

  if (DestType->isNamed("double")) {
    if (SrcType->isNamed("int"))
      return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), Name);
    if (SrcType->isNamed("bool"))
      return Builder->CreateUIToFP(Val, Type::getDoubleTy(*TheContext), Name);
  }
  if (DestType->isNamed("int")) {
    if (SrcType->isNamed("double"))
      return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), Name);
    if (SrcType->isNamed("bool"))
      return Builder->CreateZExt(Val, Type::getInt64Ty(*TheContext), Name);
  }
  if (DestType->isNamed("bool")) {
    if (SrcType->isNamed("double"))
      return Builder->CreateFCmpONE(Val, ConstantFP::get(*TheContext, APFloat(0.0)), Name);
    if (SrcType->isNamed("int"))
      return Builder->CreateICmpNE(Val, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), Name);
  }

  return Val;
}

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

Value *StringExprAST::codegen() {
  // Create a global string constant
  return Builder->CreateGlobalString(Val, "str");
}

// Turns any expression into IR operation
Value *BinaryExprAST::codegen() {
  // Generate codes for the Left side and Right side
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if (!L || !R)
    return nullptr;

  // Detect void operands (e.g. using a void function call in an expression)
  if (L->getType()->isVoidTy()) {
    VoidValueError(Loc,
                   "The left side of this expression is a void function call.")
        .raise();
    return nullptr;
  }
  if (R->getType()->isVoidTy()) {
    VoidValueError(Loc,
                   "The right side of this expression is a void function call.")
        .raise();
    return nullptr;
  }

  TurfTypePtr LTy = getTurfTypeFromLLVM(L->getType());
  TurfTypePtr RTy = getTurfTypeFromLLVM(R->getType());

  auto CastBoth = [&](TurfTypePtr Target) {
    L = CastToType(L, Target, "lhscast");
    R = CastToType(R, Target, "rhscast");
  };

  TurfTypePtr CommonType = getCommonType(LTy, RTy);
  TurfTypePtr NumericType = CommonType->isNamed("bool") ? TurfInt() : CommonType;

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
    BasicBlock *DivZeroBB = BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB = BasicBlock::Create(*TheContext, "divcont", TheFunction);
    
    Value *IsZero;
    if (NumericType->isNamed("double")) {
      IsZero = Builder->CreateFCmpOEQ(R, ConstantFP::get(*TheContext, APFloat(0.0)), "iszero");
    } else {
      IsZero = Builder->CreateICmpEQ(R, ConstantInt::get(*TheContext, APInt(32, 0)), "iszero");
    }
    
    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);
    
    Builder->SetInsertPoint(DivZeroBB);
    std::vector<std::unique_ptr<ExprAST>> Args;
    Args.push_back(std::make_unique<StringExprAST>("Division by zero"));
    auto ExInit = std::make_unique<StructInitExprAST>(Loc, "ArithmeticException", std::move(Args));
    auto RaiseEx = std::make_unique<RaiseExprAST>(Loc, std::move(ExInit));
    RaiseEx->codegen();
    if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateUnreachable();
    
    Builder->SetInsertPoint(DivContBB);
    
    return NumericType->isNamed("double") ? Builder->CreateFDiv(L, R, "divtmp")
                                          : Builder->CreateSDiv(L, R, "divtmp");
  }

  case '%': {
    CastBoth(NumericType);
    
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB = BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB = BasicBlock::Create(*TheContext, "divcont", TheFunction);
    
    Value *IsZero;
    if (NumericType->isNamed("double")) {
      IsZero = Builder->CreateFCmpOEQ(R, ConstantFP::get(*TheContext, APFloat(0.0)), "iszero");
    } else {
      IsZero = Builder->CreateICmpEQ(R, ConstantInt::get(*TheContext, APInt(32, 0)), "iszero");
    }
    
    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);
    
    Builder->SetInsertPoint(DivZeroBB);
    std::vector<std::unique_ptr<ExprAST>> Args;
    Args.push_back(std::make_unique<StringExprAST>("Modulo by zero"));
    auto ExInit = std::make_unique<StructInitExprAST>(Loc, "ArithmeticException", std::move(Args));
    auto RaiseEx = std::make_unique<RaiseExprAST>(Loc, std::move(ExInit));
    RaiseEx->codegen();
    if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateUnreachable();
    
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
    return CmpType->isNamed("double") ? Builder->CreateFCmpONE(L, R, "cmptmp")
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

  // Power
  case '^': {
    CastBoth(TurfDouble());
    Function *PowFunc = llvm::Intrinsic::getDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
    return Builder->CreateCall(PowFunc, {L, R}, "powtmp");
  }
  default:
    SemanticError(CurLoc, "Error: invalid binary operator");
    return nullptr;
  }
}

// This creates an alloca instruction in the entry block of a function
AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                   const std::string &VarName,
                                   const TurfType *Type) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(getLLVMType(Type), 0, VarName.c_str());
}

Value *VarDeclExprAST::codegen() {
  if (Type->isVoid()) {
    TypeError(Loc, "Variables of type 'void' are not allowed").raise();
    return nullptr;
  }

  if (Keywords.find(Name) != Keywords.end()) {
    SemanticError(Loc, "Cannot use keyword '" + Name +
                           "' as a variable name. Keyword is reserved.")
        .raise();
    return nullptr;
  }

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

  // ── List<T> fast-path ─────────────────────────────────────────────────────
  // list<T> variables need heap allocation, not a scalar store.
  // The parser sets InitVal to a dummy 0LL — ignore it and do the real init.
  if (isListType(Type.get())) {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    AllocaInst *Alloca = TmpB.CreateAlloca(
        llvm::PointerType::get(*TheContext, 0), nullptr, Name);

    bool IsDummy = false;
    if (auto *Num = dynamic_cast<NumberExprAST *>(InitVal.get())) {
      if (Num->isInteger() && Num->getIntVal() == 0) {
        IsDummy = true;
      }
    }

    Value *RetVal = nullptr;

    if (IsDummy) {
      // malloc(24) for TurfListStruct {ptr, i64, i64}
      Function *MallocF = GetOrDeclareMalloc();
      Value *StructSize = ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 24);
      Value *RawPtr = Builder->CreateCall(MallocF, {StructSize}, "listalloc");

      StructType *LST = GetListStructType();
      Value *NullData = ConstantPointerNull::get(llvm::PointerType::get(*TheContext, 0));
      Value *Zero64   = ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 0);
      Builder->CreateStore(NullData, Builder->CreateStructGEP(LST, RawPtr, 0, "initdata"));
      Builder->CreateStore(Zero64,   Builder->CreateStructGEP(LST, RawPtr, 1, "initlen"));
      Builder->CreateStore(Zero64,   Builder->CreateStructGEP(LST, RawPtr, 2, "initcap"));
      Builder->CreateStore(RawPtr, Alloca);
      RetVal = Constant::getNullValue(llvm::Type::getInt64Ty(*TheContext));
    } else {
      Value *Init = InitVal->codegen();
      if (!Init) return nullptr;

      TurfTypePtr InitType = getTurfTypeFromLLVM(Init->getType());
      InitType = RefineLLVMType(InitType, Type, Init->getType());
      if (!isTypeCompatible(InitType.get(), Type.get())) {
        TypeError(Loc, std::string("Cannot initialize '") + Type->getName() +
                           "' variable '" + Name + "' with a value of type '" +
                           InitType->getName() + "'")
            .raise();
        return nullptr;
      }

      Init = CastToType(Init, Type, "initcast", Loc);
      Builder->CreateStore(Init, Alloca);
      RetVal = Init;
    }

    if (GlobalSymbolTable)
      GlobalSymbolTable->DeclareSymbol(Name, Type, Loc, Alloca);
    NamedValues[Name] = {Alloca, Type};
    return RetVal;
  }
  // ── End list fast-path ────────────────────────────────────────────────────

  Value *Init = InitVal->codegen();
  if (!Init)
    return nullptr;

  // Detect void value (e.g. int x = voidFunc())
  if (Init->getType()->isVoidTy()) {
    VoidValueError(Loc,
                   "You're trying to store the result of a void function in '" +
                       Name + "'.")
        .raise();
    return nullptr;
  }

  // Check type compatibility before implicit cast
  TurfTypePtr InitType = getTurfTypeFromLLVM(Init->getType());
  InitType = RefineLLVMType(InitType, Type, Init->getType());
  if (!isTypeCompatible(InitType.get(), Type.get())) {
    TypeError(Loc, std::string("Cannot initialize '") + Type->getName() +
                       "' variable '" + Name + "' with a value of type '" +
                       InitType->getName() + "'")
        .raise();
    return nullptr;
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
  // Look up the variable in the symbol table
  Symbol *Sym = nullptr;
  if (GlobalSymbolTable) {
    Sym = GlobalSymbolTable->LookupSymbol(Name);
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
  std::vector<std::string> VisibleNames;
  if (GlobalSymbolTable) {
    VisibleNames = GlobalSymbolTable->GetAllVisibleNames();
  } else {
    for (const auto &pair : NamedValues) {
      VisibleNames.push_back(pair.first);
    }
  }
  UseBeforeDeclarationError(Loc, Name, VisibleNames).raise();
  return nullptr;
}

Value *VariableExprAST::codegenLValue() {
  Symbol *Sym = nullptr;
  if (GlobalSymbolTable) Sym = GlobalSymbolTable->LookupSymbol(Name);
  if (Sym) return Sym->Alloca;

  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) return Iter->second.Alloca;

  std::vector<std::string> VisibleNames;
  if (GlobalSymbolTable) VisibleNames = GlobalSymbolTable->GetAllVisibleNames();
  else for (const auto &pair : NamedValues) VisibleNames.push_back(pair.first);
  UseBeforeDeclarationError(Loc, Name, VisibleNames).raise();
  return nullptr;
}

Value *AssignmentExprAST::codegen() {
  // Generate code for the RHS first
  Value *Val = RHS->codegen();
  if (!Val)
    return nullptr;

  // Detect void value (e.g. x = voidFunc())
  if (Val->getType()->isVoidTy()) {
    VoidValueError(
        Loc, "You're trying to assign the result of a void function.")
        .raise();
    return nullptr;
  }

  // We check LHS type by performing a dummy load through codegen
  Value *LHSVal = LHS->codegen();
  if (!LHSVal) return nullptr;
  TurfTypePtr VarType = getTurfTypeFromLLVM(LHSVal->getType());
  
  TurfTypePtr RHSType = getTurfTypeFromLLVM(Val->getType());
  RHSType = RefineLLVMType(RHSType, VarType, Val->getType());

  Value *CastVal = CastToType(Val, VarType, "assigncast", Loc);

  // Generate the Store instruction using LValue
  Value *Ptr = LHS->codegenLValue();
  if (!Ptr) return nullptr;
  Builder->CreateStore(CastVal, Ptr);

  // Assignment expressions usually return the value assigned (allows x = y = 5)
  return CastVal;
}

Value *IfExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  bool IsBlockForm = dynamic_cast<BlockExprAST *>(Branches.front().Body.get()) != nullptr;
  bool HasElse = (ElseBody != nullptr);

  // Block-form if/elseif/else: statement, no PHI node needed.
  if (IsBlockForm) {
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    for (size_t i = 0; i < Branches.size(); ++i) {
      Value *CondV = Branches[i].Cond->codegen();
      if (!CondV) return nullptr;
      CondV = CastToType(CondV, TurfBool(), "ifcond");

      BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
      BasicBlock *NextBB = BasicBlock::Create(*TheContext, "else");

      Builder->CreateCondBr(CondV, ThenBB, NextBB);

      // Emit then body
      Builder->SetInsertPoint(ThenBB);
      Branches[i].Body->codegen();
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);

      // NextBB becomes the insert point for the next condition or the else/merge
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
  if (!CondV) return nullptr;
  CondV = CastToType(CondV, TurfBool(), "ifcond");

  BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

  Builder->CreateCondBr(CondV, ThenBB, ElseBB);
  Builder->SetInsertPoint(ThenBB);
  
  Value *ThenV = Branches[0].Body->codegen();
  if (!ThenV) return nullptr;
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = ElseBody->codegen();
  if (!ElseV) return nullptr;
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
    TypeError(CurLoc, "Unknown unary operand type").raise();
    return nullptr;
  default:
    SemanticError(CurLoc, "Unknown unary operator");
    return nullptr;
  }
}

Value *BlockExprAST::codegen() {
  // Enter new scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  Value *LastVal = nullptr;
  for (auto &Expr : Expressions) {
    LastVal = Expr->codegen();
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

Value *SizeOfExprAST::codegen() {
  llvm::Type *LLVMTy = getLLVMType(TypeArg.get());
  if (!LLVMTy) return nullptr;
  uint64_t AllocSize = TheModule->getDataLayout().getTypeAllocSize(LLVMTy);
  return ConstantInt::get(Type::getInt64Ty(*TheContext), AllocSize);
}

Value *CastExprAST::codegen() {
  Value *Val = Operand->codegen();
  if (!Val)
    return nullptr;

  TurfTypePtr SrcType = getTurfTypeFromLLVM(Val->getType());
  const std::string SrcName = SrcType->getName();
  const std::string DstName = DestType->getName();

  // Identity - no-op
  if (SrcName == DstName)
    return Val;

  // numeric → numeric
  if (SrcName == "int" && DstName == "double")
    return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), "itod");

  if (SrcName == "double" && DstName == "int")
    return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), "dtoi");

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

    Function *PutsF = TheModule->getFunction("puts");
    if (!PutsF) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext), {I8Ptr}, false);
      PutsF = Function::Create(FT, Function::ExternalLinkage, "puts",
                               TheModule.get());
    }
    Value *ErrMsg = Builder->CreateGlobalString(
        "Runtime Error: Invalid string to int conversion.", "errmsg");
    Builder->CreateCall(PutsF, {ErrMsg});

    Function *ExitF = TheModule->getFunction("exit");
    if (!ExitF) {
      FunctionType *FT = FunctionType::get(
          Type::getVoidTy(*TheContext), {Type::getInt32Ty(*TheContext)}, false);
      ExitF = Function::Create(FT, Function::ExternalLinkage, "exit",
                               TheModule.get());
    }
    Builder->CreateCall(ExitF,
                        {ConstantInt::get(Type::getInt32Ty(*TheContext), 1)});
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
      FunctionType *FT = FunctionType::get(Type::getDoubleTy(*TheContext), {I8Ptr, I8PtrPtr}, /*isVarArg=*/false);
      StrtodF = Function::Create(FT, Function::ExternalLinkage, "strtod", TheModule.get());
    }
    Value *NullPtr = ConstantPointerNull::get(PointerType::get(PointerType::get(*TheContext, 0)->getContext(), 0));
    return Builder->CreateCall(StrtodF, {Val, NullPtr}, "strtod_res");
  }

  // int → string : snprintf(buf, 32, "%lld", val)
  if (SrcName == "int" && DstName == "string") {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(Type::getInt32Ty(*TheContext),
                              {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr}, /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf", TheModule.get());
    }
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize, "int_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%lld", "intfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_int");
    return Buf;
  }

  // double → string : snprintf(buf, 32, "%g", val)
  if (SrcName == "double" && DstName == "string") {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(Type::getInt32Ty(*TheContext),
                              {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr}, /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf", TheModule.get());
    }
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize, "dbl_to_str_buf");
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

  // Unsupported → string
  if (DstName == "string") {
    StringConversionError(Loc, SrcName.c_str()).raise();
    return nullptr;
  }

  // Everything else is unsupported
  TypeError(Loc, std::string("Cannot explicitly cast '") + SrcName + "' to '" + DstName + "'").raise();
  return nullptr;
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

// ForExprAST::codegen : desugars to init + while(cond) { body; step }
Value *ForExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  //  1. Init: declare loop variable with start value
  Value *StartV = Start->codegen();
  if (!StartV)
    return nullptr;

  // Determine the loop variable type from the start value
  TurfTypePtr VarType = StartV->getType()->isDoubleTy() ? TurfDouble() : TurfInt();

  StartV = CastToType(StartV, VarType, "forstart");

  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, VarType.get());
  Builder->CreateStore(StartV, Alloca);

  // Register loop variable in symbol table and NamedValues
  // Save old binding so we can restore it after the loop
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
  if (!EndV)
    return nullptr;
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
  Value *CurVar = Builder->CreateLoad(getLLVMType(VarType.get()), Alloca, VarName);
  Value *EndVal =
      Builder->CreateLoad(getLLVMType(VarType.get()), EndAlloca, VarName + ".end");
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

  if (!Step->codegen())
    return nullptr;

  // Branch back to condition
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // 8. After loop
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // Restore old binding
  if (HadOldBinding) {
    NamedValues[VarName] = SavedBinding;
  } else {
    NamedValues.erase(VarName);
  }

  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
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
    SyntaxError(Loc, "'continue' used outside of a loop").raise();
    return nullptr;
  }

  Builder->CreateBr(LoopBlocks.back().first);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_continue", TheFunction);
  Builder->SetInsertPoint(DeadBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

// FuncDefExprAST::codegen
// Two-pass strategy:
//   Pass 1 (prototype): if no LLVM Function yet, create it (no body).
//   Pass 2 (body):      if function has no body yet, fill it in.
// Calling codegen() twice is what makes forward calls work: main.cpp's
// pre-pass creates prototypes, then the normal loop fills bodies.
Value *FuncDefExprAST::codegen() {
  std::string MangledName = Name;
  std::string Prefix = "";
  if (GlobalSymbolTable && GlobalSymbolTable->IsInModule()) {
    Prefix = GlobalSymbolTable->GetCurrentModule() + "_";
    MangledName = Prefix + Name;
  }
  
  if (Name != "main") {
    MangledName = "turf_" + Prefix + Name;
    for (const auto &P : Params) {
      MangledName += "_" + P.Type->getMangledName();
    }
  }

  // Build/look up the LLVM prototype
  Function *TheFunc = TheModule->getFunction(MangledName);

  if (!TheFunc) {
    // Save param types and return type
    std::vector<TurfTypePtr> PTypes;
    for (const auto &P : Params) PTypes.push_back(P.Type);
    FuncParamTypes[MangledName] = PTypes;
    FuncReturnTypes[MangledName] = ReturnType;

    // Build the LLVM FunctionType
    std::vector<Type *> ParamTypes;
    for (const auto &P : Params)
      ParamTypes.push_back(getLLVMType(P.Type.get()));

    FunctionType *FT = FunctionType::get(getLLVMType(ReturnType.get()), ParamTypes,
                                         /*isVarArg=*/false);
    auto Linkage = (Name == "main") ? Function::ExternalLinkage : Function::InternalLinkage;
    TheFunc =
        Function::Create(FT, Linkage, MangledName, TheModule.get());

    // Name the arguments
    unsigned Idx = 0;
    for (auto &Arg : TheFunc->args())
      Arg.setName(Params[Idx++].Name);
  } else {
    // If the function already has a body, this is a duplicate definition
    if (!TheFunc->empty()) {
      SemanticError(Loc, "Duplicate function definition: '" + Name + "'")
          .raise();
      return nullptr;
    }
  }

  // If this is a prototype-only call (pre-pass), stop here
  // The Body will be nullptr only when called from the proto-hoisting pre-pass.
  // Since we always parse the body in ParseFuncDef, Body is never null here.
  // (Reserve this hook for a future separate pre-pass.)

  // Store full param types for call-site checking
  FuncParamTypes[MangledName] = {};
  for (const auto &P : Params)
    FuncParamTypes[MangledName].push_back(P.Type);

  // Fill in the function body
  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunc);

  // Save caller context
  auto SavedNamedValues = NamedValues;
  auto SavedReturnType = CurrentFuncReturnType;
  auto SavedFunction = CurrentFunction;
  auto *SavedInsertBlock = Builder->GetInsertBlock();
  auto SavedInsertPoint = Builder->GetInsertPoint();

  // Switch to the new function
  Builder->SetInsertPoint(BB);
  NamedValues.clear();
  CurrentFuncReturnType = ReturnType;
  CurrentFunction = TheFunc;

  if (!IntrinsicName.empty()) {
    std::vector<Value *> ArgVals;
    for (auto &Arg : TheFunc->args()) {
      ArgVals.push_back(&Arg);
    }
    Value *IntrinsicRet = CodegenIntrinsic(IntrinsicName, ArgVals, Loc);
    if (!IntrinsicRet) {
      // CodegenIntrinsic will raise its own error
      TheFunc->eraseFromParent();
      return nullptr;
    }
    if (ReturnType->isVoid()) {
      Builder->CreateRetVoid();
    } else {
      Builder->CreateRet(IntrinsicRet);
    }

    // Restore caller context
    NamedValues = SavedNamedValues;
    CurrentFuncReturnType = SavedReturnType;
    CurrentFunction = SavedFunction;
    if (SavedInsertBlock) {
      Builder->SetInsertPoint(SavedInsertBlock, SavedInsertPoint);
    } else {
      Builder->ClearInsertionPoint();
    }
    return TheFunc;
  }

  // Enter a new scope for the function body
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // Alloca each parameter and add to NamedValues and GlobalSymbolTable
  for (auto &Arg : TheFunc->args()) {
    TurfTypePtr ParamTurfType = TurfInt(); // fallback
    for (const auto &P : Params)
      if (P.Name == Arg.getName())
        ParamTurfType = P.Type;

    AllocaInst *Alloca = CreateEntryBlockAlloca(
        TheFunc, std::string(Arg.getName()), ParamTurfType.get());
    Builder->CreateStore(&Arg, Alloca);

    // For array parameters, normalise the registered type to size=-1 so that
    // ArrayAccessExprAST uses the pointer-load GEP path (not the two-index
    // alloca-GEP path that assumes the alloca IS the array).
    TurfTypePtr RegisteredType = ParamTurfType;
    if (isArrayType(ParamTurfType.get())) {
      const TurfType *ElemT = getArrayElementType(ParamTurfType.get());
      // Rebuild the type with the element type shared_ptr
      RegisteredType = makeArrayType(
          dynamic_cast<const ::ArrayType *>(ParamTurfType.get())->getElementTypePtr(),
          -1); // -1 = pointer parameter
    }

    NamedValues[std::string(Arg.getName())] = {Alloca, RegisteredType};

    // Add parameter to symbol table
    if (GlobalSymbolTable) {
      SourceLocation ParamLoc = Loc;
      GlobalSymbolTable->DeclareSymbol(std::string(Arg.getName()),
                                       RegisteredType, ParamLoc, Alloca);
    }
  }

  // Lint the function body before codegen
  LintExpression(Body.get());

  // Codegen the body
  Body->codegen();

  // Build and analyze CFG for flow diagnostics
  if (Body) {
    CFGBuilder CFGBuilder;
    auto FuncCFG = CFGBuilder.buildCFG(Name, ReturnType, Body.get());

    // Run flow analysis and report diagnostics (will raise error for missing
    // returns in non-void)
    FuncCFG->reportFlowDiagnostics();

    // Store CFG for later use (optional)
    GlobalCFGs.push_back(std::move(FuncCFG));
  }

  // Emit a return if the block has no terminator yet.
  // IMPORTANT: ReturnExprAST::codegen() leaves the builder pointing at a
  // fresh dead "after_ret" block (no predecessors, no terminator). We must
  // erase those dead blocks instead of treating them as missing-return paths.
  BasicBlock *CurBB = Builder->GetInsertBlock();
  if (!CurBB->getTerminator()) {
    bool IsDead = (CurBB != BB) && llvm::pred_empty(CurBB);
    if (IsDead) {
      // Dead block left behind by a return statement — erase it cleanly.
      CurBB->eraseFromParent();
    } else if (ReturnType->isVoid()) {
      Builder->CreateRetVoid();
    } else {
      // CFG already reported the error, but we still need to generate valid IR
      Builder->CreateRet(Constant::getNullValue(getLLVMType(ReturnType.get())));
    }
  }

  verifyFunction(*TheFunc);

  // Exit function scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // Restore caller context
  NamedValues = SavedNamedValues;
  CurrentFuncReturnType = SavedReturnType;
  CurrentFunction = SavedFunction;
  if (SavedInsertBlock)
    Builder->SetInsertPoint(SavedInsertBlock, SavedInsertPoint);

  // Function definitions are statements; they do not produce a value.
  return nullptr;
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

  if (!Val) {
    // empty return;
    if (!CurrentFuncReturnType->isVoid()) {
      TypeError(Loc, "'return;' is only valid inside a void function").raise();
      return nullptr;
    }
    Builder->CreateRetVoid();
  } else {
    // return <expr>;
    if (CurrentFuncReturnType->isVoid()) {
      TypeError(Loc, "void function cannot return a value").raise();
      return nullptr;
    }
    Value *RetVal = Val->codegen();
    if (!RetVal)
      return nullptr;

    // Check for void return value (e.g. return voidFunc())
    if (RetVal->getType()->isVoidTy()) {
      VoidValueError(Loc, "You're trying to return the result of a void function.").raise();
      return nullptr;
    }

    // Check type compatibility before implicit cast
    TurfTypePtr ActualType = getTurfTypeFromLLVM(RetVal->getType());
    ActualType = RefineLLVMType(ActualType, CurrentFuncReturnType, RetVal->getType());
    
    if (!isTypeCompatible(ActualType.get(), CurrentFuncReturnType.get())) {
      ReturnTypeMismatchError(Loc, CurrentFunction->getName().str(),
                              turfTypeName(CurrentFuncReturnType.get()),
                              turfTypeName(ActualType.get()))
          .raise();
      return nullptr;
    }

    RetVal = CastToType(RetVal, CurrentFuncReturnType, "retcast", Loc);
    Builder->CreateRet(RetVal);
  }

  // After a terminator, create a fresh unreachable block so the IR builder
  // has a valid insert point for any subsequent statements in the same block.
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_ret", CurrentFunction);
  Builder->SetInsertPoint(DeadBB);

  // Return a dummy null so the AST node has a non-null value
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *FuncCallExprAST::codegen() {
  auto *Var = dynamic_cast<VariableExprAST *>(Callee.get());
  if (!Var) {
    SemanticError(Loc, "First-class functions are not supported yet.").raise();
    return nullptr;
  }
  std::string CalleeName = Var->getName();

  if (CalleeName == "typeof" || CalleeName == "lengthof" || 
      CalleeName == "malloc" || CalleeName == "realloc" || CalleeName == "free") {
    std::vector<Value *> ArgVals;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V) return nullptr;
      ArgVals.push_back(V);
    }
    return CodegenIntrinsic(CalleeName, ArgVals, Loc);
  }

  std::vector<Value *> EvaluatedArgs;
  std::vector<TurfTypePtr> ArgTypes;
  for (auto &Arg : Args) {
    Value *V = Arg->codegen();
    if (!V) return nullptr;
    EvaluatedArgs.push_back(V);

    TurfTypePtr ActTy;
    if (auto *VE = dynamic_cast<VariableExprAST *>(Arg.get())) {
      auto It = NamedValues.find(VE->getName());
      if (It != NamedValues.end()) ActTy = It->second.Type;
    }
    if (!ActTy) ActTy = getTurfTypeFromLLVM(V->getType());
    ArgTypes.push_back(ActTy);
  }

  std::string MangledName = CalleeName;
  if (CalleeName != "main") {
    MangledName = "turf_" + CalleeName;
    for (const auto &T : ArgTypes) {
      MangledName += "_" + T->getMangledName();
    }
  }

  Function *CalleeF = TheModule->getFunction(MangledName);
  if (!CalleeF && CalleeName != "main") {
    // Fallback: search for a compatible overload using implicit casting
    for (const auto &[Name, ParamTypes] : FuncParamTypes) {
      std::string Prefix = "turf_" + CalleeName;
      if (Name == Prefix || (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
        if (ParamTypes.size() == ArgTypes.size()) {
          bool Compatible = true;
          for (size_t i = 0; i < ArgTypes.size(); ++i) {
            if (!isTypeCompatible(ArgTypes[i].get(), ParamTypes[i].get())) {
              Compatible = false;
              break;
            }
          }
          if (Compatible) {
            CalleeF = TheModule->getFunction(Name);
            MangledName = Name;
            break;
          }
        }
      }
    }
  }

  if (!CalleeF) {
    LookupError(Loc, CalleeName, SuggestionIndex::suggestBuiltins(CalleeName)).raise();
    return nullptr;
  }

  if (CalleeF->arg_size() != Args.size()) {
    SemanticError(Loc, "Wrong number of arguments to '" + CalleeName +
                           "': expected " +
                           std::to_string(CalleeF->arg_size()) + ", got " +
                           std::to_string(Args.size()))
        .raise();
    return nullptr;
  }

  std::vector<Value *> ArgVals;
  unsigned Idx = 0;

  // Look up full param type info (needed for list<T> params)
  auto ParamTypeIt = FuncParamTypes.find(MangledName);

  for (auto &Arg : CalleeF->args()) {
    Value *V = EvaluatedArgs[Idx];

    // Determine declared TurfTypePtr for this param
    TurfTypePtr DeclaredType;
    if (ParamTypeIt != FuncParamTypes.end() && Idx < ParamTypeIt->second.size())
      DeclaredType = ParamTypeIt->second[Idx];
    else
      DeclaredType = getTurfTypeFromLLVM(Arg.getType());

    // Check for void argument
    if (V->getType()->isVoidTy()) {
      VoidValueError(Loc, "Argument " + std::to_string(Idx + 1) + " to '" +
                              CalleeName + "' is a void function call.")
          .raise();
      return nullptr;
    }

    // For list params: verify the caller is passing a list of the right element type
    if (isListType(DeclaredType.get())) {
      TurfTypePtr ActualType;
      if (auto *VE = dynamic_cast<VariableExprAST *>(Args[Idx].get())) {
        auto It = NamedValues.find(VE->getName());
        if (It != NamedValues.end())
          ActualType = It->second.Type;
      }
      if (ActualType && ActualType->getName() != DeclaredType->getName()) {
        ArgumentTypeError(Loc, CalleeName, std::string(Arg.getName()), Idx + 1,
                          turfTypeName(DeclaredType.get()),
                          turfTypeName(ActualType.get()))
            .raise();
        return nullptr;
      }
    } else if (isArrayType(DeclaredType.get())) {
      // Array params: the function expects a pointer to the array data.
      // We must pass the raw AllocaInst of the caller's array, not a loaded
      // value. Also verify the caller is actually passing an array variable
      // with a compatible element type.
      if (auto *VE = dynamic_cast<VariableExprAST *>(Args[Idx].get())) {
        auto It = NamedValues.find(VE->getName());
        if (It == NamedValues.end() || !isArrayType(It->second.Type.get())) {
          SemanticError(Loc, "Argument " + std::to_string(Idx + 1) + " to '" +
                                 CalleeName + "' must be an array variable").raise();
          return nullptr;
        }
        // Element-type check
        const TurfType *ExpElem = getArrayElementType(DeclaredType.get());
        const TurfType *ActElem = getArrayElementType(It->second.Type.get());
        if (ExpElem && ActElem &&
            ExpElem->getName() != ActElem->getName()) {
          ArgumentTypeError(Loc, CalleeName, std::string(Arg.getName()), Idx + 1,
                            turfTypeName(DeclaredType.get()),
                            turfTypeName(It->second.Type.get()))
              .raise();
          return nullptr;
        }
        // Pass the alloca pointer directly (array passed by reference)
        V = It->second.Alloca;
      } else {
        SemanticError(Loc, "Argument " + std::to_string(Idx + 1) + " to '" +
                               CalleeName + "': arrays must be passed as a variable name")
            .raise();
        return nullptr;
      }
    } else {
      // Standard scalar type checking
      TurfTypePtr ExpectedType = getTurfTypeFromLLVM(Arg.getType());
      TurfTypePtr ActualType   = getTurfTypeFromLLVM(V->getType());
      if (!isTypeCompatible(ActualType.get(), ExpectedType.get())) {
        ArgumentTypeError(Loc, CalleeName, std::string(Arg.getName()), Idx + 1,
                          turfTypeName(ExpectedType.get()),
                          turfTypeName(ActualType.get()))
            .raise();
        return nullptr;
      }
      V = CastToType(V, ExpectedType, "argcast", Loc);
    }

    ArgVals.push_back(V);
    Idx++;
  }

  // void functions must not be given a name (LLVM requirement)
  if (CalleeF->getReturnType()->isVoidTy())
    return Builder->CreateCall(CalleeF, ArgVals);

  return Builder->CreateCall(CalleeF, ArgVals, "calltmp");
}

// Returns (or creates) the named LLVM struct { ptr, i64, i64 } used by all lists.
// Fields: data (ptr to element buffer), len (i64), cap (i64).
static llvm::StructType *GetListStructType() {
  llvm::StructType *ST =
      llvm::StructType::getTypeByName(*TheContext, "TurfListStruct");
  if (!ST) {
    ST = llvm::StructType::create(*TheContext, "TurfListStruct");
    ST->setBody({llvm::PointerType::get(*TheContext, 0),   // data
                 llvm::Type::getInt64Ty(*TheContext),       // len
                 llvm::Type::getInt64Ty(*TheContext)});     // cap
  }
  return ST;
}

// Ensure malloc / realloc / free are declared in the module.
static llvm::Function *GetOrDeclareMalloc() {
  if (auto *F = TheModule->getFunction("malloc"))
    return F;
  auto *FT = llvm::FunctionType::get(
      llvm::PointerType::get(*TheContext, 0),
      {llvm::Type::getInt64Ty(*TheContext)}, false);
  return llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "malloc",
                                TheModule.get());
}
static llvm::Function *GetOrDeclareRealloc() {
  if (auto *F = TheModule->getFunction("realloc"))
    return F;
  auto *FT = llvm::FunctionType::get(
      llvm::PointerType::get(*TheContext, 0),
      {llvm::PointerType::get(*TheContext, 0),
       llvm::Type::getInt64Ty(*TheContext)},
      false);
  return llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "realloc",
                                TheModule.get());
}

// Helper: emit a runtime bounds check for list access.
static void EmitListBoundsCheck(Value *IndexVal, Value *LenVal,
                                const std::string &ListName,
                                SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
  Value *TooSmall = Builder->CreateICmpSLT(IndexVal, Zero, "idx_neg");
  Value *TooBig   = Builder->CreateICmpSGE(IndexVal, LenVal, "idx_big");
  Value *OOB      = Builder->CreateOr(TooSmall, TooBig, "oob");

  BasicBlock *OobBB  = BasicBlock::Create(*TheContext, "list_oob", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "list_ok");
  Builder->CreateCondBr(OOB, OobBB, ContBB);

  Builder->SetInsertPoint(OobBB);
  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<StringExprAST>("List index out of bounds for '" + ListName + "'"));
  auto ExInit = std::make_unique<StructInitExprAST>(Loc, "OutOfBoundsException", std::move(Args));
  auto RaiseEx = std::make_unique<RaiseExprAST>(Loc, std::move(ExInit));
  RaiseEx->codegen();

  if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateUnreachable();

  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
}

// Array Codegen
Value *ArrayDeclExprAST::codegen() {
  if (ElementType->isVoid()) {
    TypeError(Loc, "Arrays of type 'void' are not allowed").raise();
    return nullptr;
  }

  if (Keywords.find(Name) != Keywords.end()) {
    SemanticError(Loc, "Cannot use keyword '" + Name +
                           "' as a variable name. Keyword is reserved.")
        .raise();
    return nullptr;
  }

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

  // Validate initializer list size if present
  if (!InitList.empty() && static_cast<int>(InitList.size()) != Size) {
    ArraySizeMismatchError(Loc, Name, Size, static_cast<int>(InitList.size()))
        .raise();
    return nullptr;
  }

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
      Alloca,
      ConstantInt::get(Type::getInt8Ty(*TheContext), 0),
      ConstantInt::get(Type::getInt64Ty(*TheContext), TotalBytes),
      Alloca->getAlign());

  // If we have an initializer list, store each element
  if (!InitList.empty()) {
    for (int i = 0; i < static_cast<int>(InitList.size()); ++i) {
      Value *ElemVal = InitList[i]->codegen();
      if (!ElemVal)
        return nullptr;

      // Detect void value
      if (ElemVal->getType()->isVoidTy()) {
        VoidValueError(Loc,
                       "Element " + std::to_string(i) + " of array '" +
                           Name + "' is a void function call.")
            .raise();
        return nullptr;
      }

      // Type check: element must be compatible with declared element type
      TurfTypePtr ElemTurfType = getTurfTypeFromLLVM(ElemVal->getType());
      if (!isTypeCompatible(ElemTurfType.get(), ElementType.get())) {
        ArrayTypeMismatchError(Loc, Name, turfTypeName(ElementType.get()),
                               turfTypeName(ElemTurfType.get()))
            .raise();
        return nullptr;
      }

      ElemVal = CastToType(ElemVal, ElementType, "arrayinit", Loc);

      // GEP into array[i] and store
      Value *Indices[] = {
          ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
          ConstantInt::get(Type::getInt64Ty(*TheContext), i)};
      Value *ElemPtr = Builder->CreateInBoundsGEP(ArrTy, Alloca, Indices,
                                                   "arrelem");
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

// Helper function to emit runtime bounds check
static void EmitRuntimeBoundsCheck(Value *IndexVal, int ArraySize,
                                   const std::string &ArrayName,
                                   SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Check: index < 0 || index >= size
  Value *SizeVal =
      ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize);
  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);

  Value *TooSmall = Builder->CreateICmpSLT(IndexVal, Zero, "idx_neg");
  Value *TooBig = Builder->CreateICmpSGE(IndexVal, SizeVal, "idx_toobig");
  Value *OutOfBounds = Builder->CreateOr(TooSmall, TooBig, "oob");

  BasicBlock *OobBB =
      BasicBlock::Create(*TheContext, "arr_oob", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "arr_ok");

  Builder->CreateCondBr(OutOfBounds, OobBB, ContBB);

  // Out-of-bounds block: raise exception
  Builder->SetInsertPoint(OobBB);

  std::vector<std::unique_ptr<ExprAST>> Args;
  Args.push_back(std::make_unique<StringExprAST>("Array index out of bounds for '" + ArrayName + "'"));
  auto ExInit = std::make_unique<StructInitExprAST>(Loc, "OutOfBoundsException", std::move(Args));
  auto RaiseEx = std::make_unique<RaiseExprAST>(Loc, std::move(ExInit));
  RaiseEx->codegen();

  if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateUnreachable();

  // Continue block
  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
}

Value *ArrayAccessExprAST::codegenLValue() {
  const TurfType *BaseTy = getExprType(Base.get());
  if (!BaseTy) {
    SemanticError(Loc, "Could not determine type of array/list base").raise();
    return nullptr;
  }
  
  if (isListType(BaseTy)) {
    Value *ListPtr = Base->codegen();
    if (!ListPtr) return nullptr;
    StructType *LST = GetListStructType();
    Type *PtrTy = PointerType::get(*TheContext, 0);
    Type *ElemTy = getLLVMType(getListElementType(BaseTy));
    
    Value *LenPtr = Builder->CreateStructGEP(LST, ListPtr, 1, "lenptr");
    Value *Len = Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "len");
    
    Value *IndexVal = Index->codegen();
    if (!IndexVal) return nullptr;
    TurfTypePtr IdxType = getTurfTypeFromLLVM(IndexVal->getType());
    if (IdxType->isNamed("string")) {
      ArrayNonIntegerIndexError(Loc, turfTypeName(IdxType.get())).raise();
      return nullptr;
    }
    IndexVal = CastToType(IndexVal, TurfInt(), "listidx", Loc);
    EmitListBoundsCheck(IndexVal, Len, "list", Loc);
    
    Value *DataPtr = Builder->CreateStructGEP(LST, ListPtr, 0, "dataptr");
    Value *DataBuf = Builder->CreateLoad(PtrTy, DataPtr, "databuf");
    return Builder->CreateGEP(ElemTy, DataBuf, IndexVal, "listelemptr");
  }
  
  if (auto *PtrTyTurf = dynamic_cast<const TurfPointerType*>(BaseTy)) {
    Value *PtrVal = Base->codegen();
    if (!PtrVal) return nullptr;
    
    Value *IndexVal = Index->codegen();
    if (!IndexVal) return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "ptridx", Loc);
    
    Type *ElemTy = getLLVMType(PtrTyTurf->getPointee().get());
    return Builder->CreateGEP(ElemTy, PtrVal, IndexVal, "ptrelemptr");
  }
  
  if (!isArrayType(BaseTy)) {
    SemanticError(Loc, "Base is not an array, list, or pointer").raise();
    return nullptr;
  }
  
  Value *BaseLVal = Base->codegenLValue();
  if (!BaseLVal) return nullptr;
  
  int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
  const TurfType *ElemType = getArrayElementType(BaseTy);
  
  Value *IndexVal = Index->codegen();
  if (!IndexVal) return nullptr;
  TurfTypePtr IdxType = getTurfTypeFromLLVM(IndexVal->getType());
  if (IdxType->isNamed("string")) {
    ArrayNonIntegerIndexError(Loc, turfTypeName(IdxType.get())).raise();
    return nullptr;
  }
  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);
  
  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError(Loc, "array", Idx, ArraySize).raise();
        return nullptr;
      }
    } else {
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, "array", Loc);
    }
  }
  
  Type *ElemTy = getLLVMType(ElemType);
  if (ArraySize > 0) {
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
    Value *Indices[] = { ConstantInt::get(Type::getInt64Ty(*TheContext), 0), IndexVal };
    return Builder->CreateInBoundsGEP(ArrTy, BaseLVal, Indices, "arrelemptr");
  } else {
    Type *PtrTy = PointerType::get(*TheContext, 0);
    Value *ArrPtr = Builder->CreateLoad(PtrTy, BaseLVal, "arrptr");
    return Builder->CreateGEP(ElemTy, ArrPtr, IndexVal, "arrelemptr");
  }
}

Value *ArrayAccessExprAST::codegen() {
  Value *Ptr = codegenLValue();
  if (!Ptr) return nullptr;
  const TurfType *BaseTy = getExprType(Base.get());
  const TurfType *ElemTyT = nullptr;
  if (isListType(BaseTy)) ElemTyT = getListElementType(BaseTy);
  else if (auto *PtrTy = dynamic_cast<const TurfPointerType*>(BaseTy)) ElemTyT = PtrTy->getPointee().get();
  else ElemTyT = getArrayElementType(BaseTy);
  return Builder->CreateLoad(getLLVMType(ElemTyT), Ptr, "arrelem");
}

Value *ArrayAssignExprAST::codegen() {
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end())
    VI = &Iter->second;

  // Delegate list[i] = val to list-specific path
  if (VI && isListType(VI->Type.get())) {
    StructType *LST      = GetListStructType();
    Type *PtrTy          = PointerType::get(*TheContext, 0);
    const TurfType *ElemT = getListElementType(VI->Type.get());
    Type *ElemTy         = getLLVMType(ElemT);

    Value *ListPtr = Builder->CreateLoad(PtrTy, VI->Alloca, "listptr");
    Value *LenPtr  = Builder->CreateStructGEP(LST, ListPtr, 1, "lenptr");
    Value *Len     = Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "len");

    Value *IndexVal = Index->codegen();
    if (!IndexVal) return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "listidx", Loc);
    EmitListBoundsCheck(IndexVal, Len, Name, Loc);

    Value *RHSVal = RHS->codegen();
    if (!RHSVal) return nullptr;
    const TurfType *ExprT = getExprType(RHS.get());
    std::string GotTypeName = ExprT ? ExprT->getName() : getTurfTypeFromLLVM(RHSVal->getType())->getName();
    if (GotTypeName != ElemT->getName()) {
      ListTypeMismatchError(Loc, Name, turfTypeName(ElemT), GotTypeName).raise();
      return nullptr;
    }
    Value *DataPtr = Builder->CreateStructGEP(LST, ListPtr, 0, "dataptr");
    Value *DataBuf = Builder->CreateLoad(PtrTy, DataPtr, "databuf");
    Value *ElemPtr = Builder->CreateGEP(ElemTy, DataBuf, IndexVal, "listelem");
    Builder->CreateStore(RHSVal, ElemPtr);
    return RHSVal;
  }

  if (!VI || !isArrayType(VI->Type.get())) {
    SemanticError(Loc, "'" + Name + "' is not an array").raise();
    return nullptr;
  }

  int ArraySize = dynamic_cast<const ::ArrayType *>(VI->Type.get())->getSize();
  const TurfType *ElemType = getArrayElementType(VI->Type.get());

  // Evaluate the index
  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;

  // Index must be integer
  TurfTypePtr IdxType = getTurfTypeFromLLVM(IndexVal->getType());
  if (IdxType->isNamed("string")) {
    ArrayNonIntegerIndexError(Loc, turfTypeName(IdxType.get())).raise();
    return nullptr;
  }
  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  // Compile-time bounds check for constant indices (local arrays only)
  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError(Loc, Name, Idx, ArraySize).raise();
        return nullptr;
      }
    } else {
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, Name, Loc);
    }
  }

  // Evaluate the RHS value
  Value *RHSVal = RHS->codegen();
  if (!RHSVal)
    return nullptr;

  // Detect void RHS
  if (RHSVal->getType()->isVoidTy()) {
    VoidValueError(Loc, "You're trying to store a void value in array '" +
                            Name + "'.").
        raise();
    return nullptr;
  }

  // Type check
  const TurfType *ExprT = getExprType(RHS.get());
  std::string GotTypeName = ExprT ? ExprT->getName() : getTurfTypeFromLLVM(RHSVal->getType())->getName();
  
  // Temporary TurfTypePtr for isTypeCompatible check if we had to fallback to LLVM type
  TurfTypePtr FallbackType;
  const TurfType *CheckType = ExprT;
  if (!CheckType) {
    FallbackType = getTurfTypeFromLLVM(RHSVal->getType());
    CheckType = FallbackType.get();
  }

  if (!isTypeCompatible(CheckType, ElemType)) {
    ArrayTypeMismatchError(Loc, Name, turfTypeName(ElemType), GotTypeName).raise();
    return nullptr;
  }

  // Cast RHS to the element type before storing
  TurfTypePtr ElemTypePtr = getTurfTypeFromLLVM(getLLVMType(ElemType));
  RHSVal = CastToType(RHSVal, ElemTypePtr, "arrstore", Loc);

  // GEP + store
  // Two cases (same as ArrayAccessExprAST):
  //  A) Local array (ArraySize > 0): two-index GEP into [N x T] alloca.
  //  B) Pointer parameter (ArraySize == -1): load pointer, single-index GEP.
  Type *ElemTy = getLLVMType(ElemType);
  Value *ElemPtr;
  if (ArraySize > 0) {
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
    Value *Indices[] = {
        ConstantInt::get(Type::getInt64Ty(*TheContext), 0), IndexVal};
    ElemPtr = Builder->CreateInBoundsGEP(ArrTy, VI->Alloca, Indices, "arrelem");
  } else {
    Type *PtrTy = PointerType::get(*TheContext, 0);
    Value *ArrPtr = Builder->CreateLoad(PtrTy, VI->Alloca, "arrptr");
    ElemPtr = Builder->CreateGEP(ElemTy, ArrPtr, IndexVal, "arrelem");
  }

  Builder->CreateStore(RHSVal, ElemPtr);
  return RHSVal;
}


Value *ArrayLengthExprAST::codegen() {
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end())
    VI = &Iter->second;

  // list.length: load len field from heap struct
  if (VI && isListType(VI->Type.get())) {
    StructType *LST = GetListStructType();
    Type *PtrTy     = PointerType::get(*TheContext, 0);
    Value *ListPtr  = Builder->CreateLoad(PtrTy, VI->Alloca, "listptr");
    Value *LenPtr   = Builder->CreateStructGEP(LST, ListPtr, 1, "lenptr");
    return Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "listlen");
  }

  if (!VI || !isArrayType(VI->Type.get())) {
    SemanticError(Loc, "'" + Name +
                           "' is not an array. '.length' can only be used on arrays.")
        .raise();
    return nullptr;
  }

  return ConstantInt::get(Type::getInt64Ty(*TheContext),
                          dynamic_cast<const ::ArrayType *>(VI->Type.get())->getSize());
}



// NOTE: ListDeclExprAST has been removed. List declarations are now
// represented as VarDeclExprAST with a GenericType("list", [elemType]).
// The VarDeclExprAST::codegen handles list initialization via the type system.

Value *MethodCallExprAST::codegen() {
  if (auto *Var = dynamic_cast<VariableExprAST*>(Base.get())) {
    std::string ModuleName = Var->getName();
    std::string PotentialMangledPrefix = "turf_" + ModuleName + "_" + Method;
    bool IsModule = false;
    for (const auto &Pair : FuncParamTypes) {
      if (Pair.first == PotentialMangledPrefix || (Pair.first.size() > PotentialMangledPrefix.size() && Pair.first[PotentialMangledPrefix.size()] == '_')) {
        IsModule = true;
        break;
      }
    }
    if (IsModule) {
      // It's a module call! E.g. math.sqrt() -> math_sqrt()
      auto ModCall = std::make_unique<FuncCallExprAST>(Loc, std::make_unique<VariableExprAST>(Loc, ModuleName + "_" + Method), std::move(Args));
      return ModCall->codegen();
    }
  }

  const TurfType *BaseTy = getExprType(Base.get());
  if (auto *PtrTy = dynamic_cast<const TurfPointerType*>(BaseTy)) BaseTy = PtrTy->getPointee().get();

  if (!BaseTy) {
    SemanticError(Loc, "Could not determine type of method call base").raise();
    return nullptr;
  }
  
  if (Method == "length") {
    if (isArrayType(BaseTy)) {
      int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
      return ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize);
    }
    if (isListType(BaseTy)) {
      Value *ListPtr = Base->codegen();
      if (!ListPtr) return nullptr;
      StructType *LST = GetListStructType();
      Value *LenPtr = Builder->CreateStructGEP(LST, ListPtr, 1, "lenptr");
      return Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "listlen");
    }
    SemanticError(Loc, "'.length' can only be used on arrays or lists").raise();
    return nullptr;
  }

  if (auto *StructTy = dynamic_cast<const TurfStructType*>(BaseTy)) {
    std::vector<Value *> EvaluatedArgs;
    std::vector<TurfTypePtr> ArgTypes;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V) return nullptr;
      EvaluatedArgs.push_back(V);
      
      TurfTypePtr ActTy;
      if (auto *VE = dynamic_cast<VariableExprAST *>(Arg.get())) {
        auto It = NamedValues.find(VE->getName());
        if (It != NamedValues.end()) ActTy = It->second.Type;
      }
      if (!ActTy) ActTy = getTurfTypeFromLLVM(V->getType());
      ArgTypes.push_back(ActTy);
    }

    std::string MethodName = StructTy->getName() + "." + Method;
    std::string MangledName = "turf_" + MethodName + "_" + StructTy->getName() + "_ptr";
    for (const auto &T : ArgTypes) {
      MangledName += "_" + T->getMangledName();
    }

    Function *CalleeF = TheModule->getFunction(MangledName);
    if (!CalleeF) {
      // Fallback overload resolution
      for (const auto &[Name, ParamTypes] : FuncParamTypes) {
        std::string Prefix = "turf_" + MethodName;
        if (Name == Prefix || (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
          if (ParamTypes.size() == ArgTypes.size() + 1) { // +1 for self
            bool Compatible = true;
            for (size_t i = 0; i < ArgTypes.size(); ++i) {
              if (!isTypeCompatible(ArgTypes[i].get(), ParamTypes[i + 1].get())) {
                Compatible = false;
                break;
              }
            }
            if (Compatible) {
              CalleeF = TheModule->getFunction(Name);
              break;
            }
          }
        }
      }
    }

    if (!CalleeF) {
      SemanticError(Loc, "Method '" + Method + "' not found on struct '" + StructTy->getName() + "'").raise();
      return nullptr;
    }
    if (CalleeF->arg_size() != Args.size() + 1) {
      SemanticError(Loc, "Wrong number of arguments to method '" + Method +
                             "': expected " + std::to_string(CalleeF->arg_size() - 1) + ", got " +
                             std::to_string(Args.size())).raise();
      return nullptr;
    }
    
    std::vector<Value *> ArgVals;
    Value *SelfVal = Base->codegenLValue();
    if (!SelfVal) {
      // If Base is an rvalue (like User(10)), we need a temporary alloca to pass by reference
      Value *Val = Base->codegen();
      if (!Val) return nullptr;
      SelfVal = Builder->CreateAlloca(Val->getType(), nullptr, "tmp.self");
      Builder->CreateStore(Val, SelfVal);
    } else if (dynamic_cast<const TurfPointerType*>(getExprType(Base.get()))) {
      SelfVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0), SelfVal, "deref");
    }
    if (!SelfVal) return nullptr;
    ArgVals.push_back(SelfVal);
    
    unsigned Idx = 0;
    for (auto ArgIt = std::next(CalleeF->arg_begin()); ArgIt != CalleeF->arg_end(); ++ArgIt, ++Idx) {
      Value *V = EvaluatedArgs[Idx];
      ArgVals.push_back(V);
    }
    
    return Builder->CreateCall(CalleeF, ArgVals, "methodcalltmp");
  }

  if (!isListType(BaseTy)) {
    ListNonListOperationError(Loc, "expression", Method).raise();
    return nullptr;
  }

  const TurfType *ElemType = getListElementType(BaseTy);
  Type *ElemTy  = getLLVMType(ElemType);
  Type *PtrTy   = PointerType::get(*TheContext, 0);
  StructType *LST = GetListStructType();

  Value *ListPtr = Base->codegen();
  if (!ListPtr) return nullptr;

  Value *LenPtr  = Builder->CreateStructGEP(LST, ListPtr, 1, "lenptr");
  Value *CapPtr  = Builder->CreateStructGEP(LST, ListPtr, 2, "capptr");
  Value *DataPtr = Builder->CreateStructGEP(LST, ListPtr, 0, "dataptr");

  if (Method == "length") {
    return Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "listlen");
  }

  if (Method == "push") {
    if (Args.empty() || !Args[0]) {
      LogErrorAt(Loc, "push() requires one argument");
      return nullptr;
    }
    Value *PushVal = Args[0]->codegen();
    if (!PushVal) return nullptr;
    const TurfType *GotType = getExprType(Args[0].get());
    std::string GotTypeName = GotType ? GotType->getName() : getTurfTypeFromLLVM(PushVal->getType())->getName();
    if (GotTypeName != ElemType->getName()) {
      ListTypeMismatchError(Loc, "list", turfTypeName(ElemType), GotTypeName).raise();
      return nullptr;
    }
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    Value *Len = Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "len");
    Value *Cap = Builder->CreateLoad(Type::getInt64Ty(*TheContext), CapPtr, "cap");

    Value *NeedsGrow = Builder->CreateICmpEQ(Len, Cap, "needsgrow");
    BasicBlock *GrowBB  = BasicBlock::Create(*TheContext, "list_grow", TheFunction);
    BasicBlock *StoreBB = BasicBlock::Create(*TheContext, "list_store");
    Builder->CreateCondBr(NeedsGrow, GrowBB, StoreBB);

    Builder->SetInsertPoint(GrowBB);
    Value *Cap2 = Builder->CreateMul(Cap, ConstantInt::get(Type::getInt64Ty(*TheContext), 2), "cap2");
    Value *IsZero = Builder->CreateICmpEQ(Cap, ConstantInt::get(Type::getInt64Ty(*TheContext), 0));
    Value *NewCap = Builder->CreateSelect(IsZero,
                        ConstantInt::get(Type::getInt64Ty(*TheContext), 8), Cap2, "newcap");
    uint64_t ElemSize = TheModule->getDataLayout().getTypeAllocSize(ElemTy);
    Value *NewBytes = Builder->CreateMul(NewCap,
                          ConstantInt::get(Type::getInt64Ty(*TheContext), ElemSize), "newbytes");
    Value *OldData = Builder->CreateLoad(PtrTy, DataPtr, "olddata");
    Value *NewData = Builder->CreateCall(GetOrDeclareRealloc(), {OldData, NewBytes}, "newdata");
    Builder->CreateStore(NewData, DataPtr);
    Builder->CreateStore(NewCap, CapPtr);
    Builder->CreateBr(StoreBB);

    TheFunction->insert(TheFunction->end(), StoreBB);
    Builder->SetInsertPoint(StoreBB);
    Value *Len2  = Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "len2");
    Value *Data2 = Builder->CreateLoad(PtrTy, DataPtr, "data2");
    Value *Slot  = Builder->CreateGEP(ElemTy, Data2, Len2, "pushslot");
    Builder->CreateStore(PushVal, Slot);
    Value *Len2Inc = Builder->CreateAdd(Len2,
                         ConstantInt::get(Type::getInt64Ty(*TheContext), 1), "leninc");
    Builder->CreateStore(Len2Inc, LenPtr);
    return ListPtr;
  }

  if (Method == "pop") {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    Value *Len = Builder->CreateLoad(Type::getInt64Ty(*TheContext), LenPtr, "len");
    Value *Zero64 = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
    Value *IsEmpty = Builder->CreateICmpEQ(Len, Zero64, "isempty");

    BasicBlock *EmptyBB = BasicBlock::Create(*TheContext, "pop_empty", TheFunction);
    BasicBlock *PopBB   = BasicBlock::Create(*TheContext, "pop_ok");
    Builder->CreateCondBr(IsEmpty, EmptyBB, PopBB);

    Builder->SetInsertPoint(EmptyBB);
    Type *I8P = PointerType::get(*TheContext, 0);
    Function *PutsF = TheModule->getFunction("puts");
    if (!PutsF) {
      auto *FT = FunctionType::get(Type::getInt32Ty(*TheContext), {I8P}, false);
      PutsF = Function::Create(FT, Function::ExternalLinkage, "puts", TheModule.get());
    }
    Value *Msg = Builder->CreateGlobalString(
        "Runtime Error: pop() on empty list.", "pop_err");
    Builder->CreateCall(PutsF, {Msg});
    Function *ExitF = TheModule->getFunction("exit");
    if (!ExitF) {
      auto *FT = FunctionType::get(Type::getVoidTy(*TheContext),
                                   {Type::getInt32Ty(*TheContext)}, false);
      ExitF = Function::Create(FT, Function::ExternalLinkage, "exit", TheModule.get());
    }
    Builder->CreateCall(ExitF, {ConstantInt::get(Type::getInt32Ty(*TheContext), 1)});
    Builder->CreateUnreachable();

    TheFunction->insert(TheFunction->end(), PopBB);
    Builder->SetInsertPoint(PopBB);
    Value *NewLen = Builder->CreateSub(Len,
                        ConstantInt::get(Type::getInt64Ty(*TheContext), 1), "poplen");
    Builder->CreateStore(NewLen, LenPtr);
    Value *Data   = Builder->CreateLoad(PtrTy, DataPtr, "popdata");
    Value *Slot   = Builder->CreateGEP(ElemTy, Data, NewLen, "popslot");
    return Builder->CreateLoad(ElemTy, Slot, "popval");
  }

  SemanticError(Loc, "Unknown list method '" + Method + "'").raise();
  return nullptr;
}

Value *StructDeclExprAST::codegen() {
  std::vector<Type *> LLVMFieldTypes;
  for (const auto &Field : Fields) {
    Type *LLVMType = getLLVMType(Field.Type.get());
    if (!LLVMType) {
      LogErrorAt(Loc, "Unknown type for field '" + Field.Name + "'");
      return nullptr;
    }
    LLVMFieldTypes.push_back(LLVMType);
  }

  StructType *ST = StructType::create(*TheContext, LLVMFieldTypes, Name);
  if (!ST) {
    LogErrorAt(Loc, "Failed to create LLVM StructType for '" + Name + "'");
    return nullptr;
  }
  
  LLVMTypeCache[Name] = ST;
  
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *StructInitExprAST::codegen() {
  TurfTypePtr T;
  if (GlobalSymbolTable) T = GlobalSymbolTable->LookupType(Name);
  auto *SType = dynamic_cast<TurfStructType*>(T.get());
  if (!SType) {
    LogErrorAt(Loc, "Unknown struct type '" + Name + "'");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(T.get());
  StructType *LLVMStructType = dyn_cast<StructType>(LLVMTy);
  if (!LLVMStructType) {
    LogErrorAt(Loc, "LLVM StructType '" + Name + "' not found");
    return nullptr;
  }

  std::vector<Value *> EvaluatedArgs;
  std::vector<TurfTypePtr> ArgTypes;
  for (auto &Arg : Args) {
    Value *V = Arg->codegen();
    if (!V) return nullptr;
    EvaluatedArgs.push_back(V);
    
    TurfTypePtr ActTy;
    if (auto *VE = dynamic_cast<VariableExprAST *>(Arg.get())) {
      auto It = NamedValues.find(VE->getName());
      if (It != NamedValues.end()) ActTy = It->second.Type;
    }
    if (!ActTy) ActTy = getTurfTypeFromLLVM(V->getType());
    ArgTypes.push_back(ActTy);
  }

  std::string InitName = Name + ".init";
  std::string MangledName = "turf_" + InitName + "_" + Name + "_ptr";
  for (const auto &T : ArgTypes) {
    MangledName += "_" + T->getMangledName();
  }

  Function *InitF = TheModule->getFunction(MangledName);
  if (!InitF) {
    for (const auto &[FName, ParamTypes] : FuncParamTypes) {
      std::string Prefix = "turf_" + InitName;
      if (FName == Prefix || (FName.size() > Prefix.size() && FName[Prefix.size()] == '_')) {
        if (ParamTypes.size() == ArgTypes.size() + 1) {
          bool Compatible = true;
          for (size_t i = 0; i < ArgTypes.size(); ++i) {
            if (!isTypeCompatible(ArgTypes[i].get(), ParamTypes[i + 1].get())) {
              Compatible = false;
              break;
            }
          }
          if (Compatible) {
            InitF = TheModule->getFunction(FName);
            break;
          }
        }
      }
    }
  }

  if (InitF) {
    if (InitF->arg_size() != Args.size() + 1) {
      LogErrorAt(Loc, "Constructor for '" + Name + "' expects " + 
                      std::to_string(InitF->arg_size() - 1) + 
                      " arguments, but got " + std::to_string(Args.size()));
      return nullptr;
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
    AllocaInst *Alloca = TmpB.CreateAlloca(LLVMStructType, nullptr, Name + "_init");

    std::vector<Value *> ArgVals;
    ArgVals.push_back(Alloca); // Implicit self pointer

    unsigned Idx = 0;
    for (auto ArgIt = std::next(InitF->arg_begin()); ArgIt != InitF->arg_end(); ++ArgIt, ++Idx) {
      Value *V = EvaluatedArgs[Idx];
      ArgVals.push_back(V);
    }

    Builder->CreateCall(InitF, ArgVals);
    return Builder->CreateLoad(LLVMStructType, Alloca, "structval");
  }

  // Fallback to default struct initialization
  if (Args.size() != SType->getFields().size()) {
    LogErrorAt(Loc, "Struct '" + Name + "' expects " + std::to_string(SType->getFields().size()) + " arguments, but got " + std::to_string(Args.size()));
    return nullptr;
  }

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
  AllocaInst *Alloca = TmpB.CreateAlloca(LLVMStructType, nullptr, Name + "_init");

  for (size_t i = 0; i < Args.size(); ++i) {
    Value *ArgVal = EvaluatedArgs[i];

    TurfTypePtr FieldType = SType->getFields()[i].Type;
    TurfTypePtr ArgTurfType = getTurfTypeFromLLVM(ArgVal->getType());
    ArgTurfType = RefineLLVMType(ArgTurfType, FieldType, ArgVal->getType());

    if (!isTypeCompatible(ArgTurfType.get(), FieldType.get())) {
      TypeError(Loc, "Type mismatch in struct initialization for field '" + SType->getFields()[i].Name + "'").raise();
      return nullptr;
    }
    
    ArgVal = CastToType(ArgVal, FieldType, "fieldcast", Loc);

    Value *FieldPtr = Builder->CreateStructGEP(LLVMStructType, Alloca, i, "fieldptr");
    Builder->CreateStore(ArgVal, FieldPtr);
  }

  return Builder->CreateLoad(LLVMStructType, Alloca, "structval");
}

Value *FieldAccessExprAST::codegenLValue() {
  Value *BaseLVal = Base->codegenLValue();
  if (!BaseLVal) return nullptr;

  const TurfType *BaseType = getExprType(Base.get());
  
  if (auto *PtrTy = dynamic_cast<const TurfPointerType*>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
    BaseLVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0), BaseLVal, "deref");
  }

  auto *SType = dynamic_cast<const TurfStructType*>(BaseType);
  if (!SType) {
    LogErrorAt(Loc, "Cannot access field on non-struct type");
    return nullptr;
  }

  int FieldIdx = SType->getFieldIndex(FieldName);
  if (FieldIdx < 0) {
    LogErrorAt(Loc, "Struct '" + SType->getName() + "' has no field named '" + FieldName + "'");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(BaseType);
  StructType *LLVMStructType = dyn_cast<StructType>(LLVMTy);
  
  return Builder->CreateStructGEP(LLVMStructType, BaseLVal, FieldIdx, "fieldptr");
}

Value *FieldAccessExprAST::codegen() {
  Value *Ptr = codegenLValue();
  if (!Ptr) return nullptr;

  const TurfType *BaseType = getExprType(Base.get());
  if (auto *PtrTy = dynamic_cast<const TurfPointerType*>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
  }

  auto *SType = dynamic_cast<const TurfStructType*>(BaseType);
  TurfTypePtr FieldType = SType->getFieldType(FieldName);

  return Builder->CreateLoad(getLLVMType(FieldType.get()), Ptr, FieldName.c_str());
}
Value *TryExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  
  // Global vars for exception state
  auto *StackGV = TheModule->getNamedGlobal("turf_exception_stack");
  if (!StackGV) {
    auto *ArrayTy = llvm::ArrayType::get(llvm::ArrayType::get(Type::getInt64Ty(*TheContext), 32), 32);
    StackGV = new GlobalVariable(*TheModule, ArrayTy, false, GlobalValue::InternalLinkage, Constant::getNullValue(ArrayTy), "turf_exception_stack");
  }
  auto *TopGV = TheModule->getNamedGlobal("turf_exception_top");
  if (!TopGV) {
    TopGV = new GlobalVariable(*TheModule, Type::getInt32Ty(*TheContext), false, GlobalValue::InternalLinkage, ConstantInt::get(Type::getInt32Ty(*TheContext), 0), "turf_exception_top");
  }
  auto *PayloadGV = TheModule->getNamedGlobal("turf_exception_payload");
  if (!PayloadGV) {
    PayloadGV = new GlobalVariable(*TheModule, PointerType::getUnqual(*TheContext), false, GlobalValue::InternalLinkage, ConstantPointerNull::get(PointerType::getUnqual(*TheContext)), "turf_exception_payload");
  }

  Function *SetJmpF = TheModule->getFunction("setjmp");
  if (!SetJmpF) {
    SetJmpF = Function::Create(FunctionType::get(Type::getInt32Ty(*TheContext), {PointerType::getUnqual(*TheContext)}, false), Function::ExternalLinkage, "setjmp", TheModule.get());
  }

  BasicBlock *TryBB = BasicBlock::Create(*TheContext, "try", TheFunction);
  BasicBlock *CatchBB = BasicBlock::Create(*TheContext, "catch");
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "trycont");
  
  Value *Top = Builder->CreateLoad(Type::getInt32Ty(*TheContext), TopGV);
  Value *NextTop = Builder->CreateAdd(Top, ConstantInt::get(Type::getInt32Ty(*TheContext), 1));
  Builder->CreateStore(NextTop, TopGV);
  
  Value *FramePtr = Builder->CreateGEP(StackGV->getValueType(), StackGV, {ConstantInt::get(Type::getInt32Ty(*TheContext), 0), Top});
  Value *JmpRes = Builder->CreateCall(SetJmpF, {FramePtr});
  Value *Cmp = Builder->CreateICmpEQ(JmpRes, ConstantInt::get(Type::getInt32Ty(*TheContext), 0));
  
  Builder->CreateCondBr(Cmp, TryBB, CatchBB);
  
  // TRY BLOCK
  Builder->SetInsertPoint(TryBB);
  if (!TryBlock->codegen()) return nullptr;
  Builder->CreateStore(Top, TopGV); 
  if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateBr(ContBB);
  
  // CATCH BLOCK
  TheFunction->insert(TheFunction->end(), CatchBB);
  Builder->SetInsertPoint(CatchBB);
  Builder->CreateStore(Top, TopGV);
  
  Value *PayloadPtr = Builder->CreateLoad(PointerType::getUnqual(*TheContext), PayloadGV);
  
  TurfTypePtr ExType;
  if (GlobalSymbolTable) {
    ExType = GlobalSymbolTable->LookupType("Exception");
  }
  if (!ExType) {
    ExType = TurfString(); // Fallback to string if Exception is not defined
  }
  
  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, PayloadName, ExType.get());
  
  Type *LLVMExTy = getLLVMType(ExType.get());
  Value *LoadedStruct = Builder->CreateLoad(LLVMExTy, PayloadPtr);
  Builder->CreateStore(LoadedStruct, Alloca);
  
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
    GlobalSymbolTable->DeclareSymbol(PayloadName, ExType, Loc, Alloca);
  }
  
  auto SavedNamedValues = NamedValues;
  NamedValues[PayloadName] = {Alloca, ExType};
  
  if (!CatchBlock->codegen()) return nullptr;
  
  if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
  NamedValues = SavedNamedValues;
  
  if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateBr(ContBB);
  
  // CONTINUE
  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
  
  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

Value *RaiseExprAST::codegen() {
  Value *PayloadVal = Payload->codegen();
  if (!PayloadVal) return nullptr;
  
  if (!PayloadVal->getType()->isPointerTy()) {
    // Allocate memory on the heap so it survives longjmp
    Function *MallocF = TheModule->getFunction("malloc");
    if (!MallocF) {
      auto *FT = FunctionType::get(PointerType::getUnqual(*TheContext), {Type::getInt64Ty(*TheContext)}, false);
      MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc", TheModule.get());
    }
    const DataLayout &DL = TheModule->getDataLayout();
    uint64_t StructSize = DL.getTypeAllocSize(PayloadVal->getType());
    Value *SizeVal = ConstantInt::get(Type::getInt64Ty(*TheContext), StructSize);
    Value *MallocCall = Builder->CreateCall(MallocF, {SizeVal});
    
    // Store the struct value into the allocated memory
    Builder->CreateStore(PayloadVal, MallocCall);
    PayloadVal = MallocCall;
  }


  auto *PayloadGV = TheModule->getNamedGlobal("turf_exception_payload");
  if (!PayloadGV) {
    PayloadGV = new GlobalVariable(*TheModule, PointerType::getUnqual(*TheContext), false, GlobalValue::InternalLinkage, ConstantPointerNull::get(PointerType::getUnqual(*TheContext)), "turf_exception_payload");
  }
  Builder->CreateStore(PayloadVal, PayloadGV);
  
  auto *TopGV = TheModule->getNamedGlobal("turf_exception_top");
  if (!TopGV) {
    TopGV = new GlobalVariable(*TheModule, Type::getInt32Ty(*TheContext), false, GlobalValue::InternalLinkage, ConstantInt::get(Type::getInt32Ty(*TheContext), 0), "turf_exception_top");
  }
  
  Function *LongJmpF = TheModule->getFunction("longjmp");
  if (!LongJmpF) {
    LongJmpF = Function::Create(FunctionType::get(Type::getVoidTy(*TheContext), {PointerType::getUnqual(*TheContext), Type::getInt32Ty(*TheContext)}, false), Function::ExternalLinkage, "longjmp", TheModule.get());
  }
  
  Value *Top = Builder->CreateLoad(Type::getInt32Ty(*TheContext), TopGV);
  Value *IsEmpty = Builder->CreateICmpEQ(Top, ConstantInt::get(Type::getInt32Ty(*TheContext), 0));
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *CrashBB = BasicBlock::Create(*TheContext, "crash", TheFunction);
  BasicBlock *JmpBB = BasicBlock::Create(*TheContext, "jmp");
  
  Builder->CreateCondBr(IsEmpty, CrashBB, JmpBB);
  
  Builder->SetInsertPoint(CrashBB);
  Function *PutsF = TheModule->getFunction("puts");
  if (!PutsF) {
    PutsF = Function::Create(FunctionType::get(Type::getInt32Ty(*TheContext), {PointerType::getUnqual(*TheContext)}, false), Function::ExternalLinkage, "puts", TheModule.get());
  }
  Value *UncaughtMsgPrefix = Builder->CreateGlobalString("Runtime Error: Uncaught exception:", "uncaught_msg_prefix");
  Builder->CreateCall(PutsF, {UncaughtMsgPrefix});
  Value *MessagePtrPtr = Builder->CreateBitCast(PayloadVal, PointerType::getUnqual(*TheContext));
  Value *MessagePtr = Builder->CreateLoad(PointerType::getUnqual(*TheContext), MessagePtrPtr, "msg_ptr");
  Builder->CreateCall(PutsF, {MessagePtr});
  Function *ExitF = TheModule->getFunction("exit");
  if (!ExitF) {
    ExitF = Function::Create(FunctionType::get(Type::getVoidTy(*TheContext), {Type::getInt32Ty(*TheContext)}, false), Function::ExternalLinkage, "exit", TheModule.get());
  }
  Builder->CreateCall(ExitF, {ConstantInt::get(Type::getInt32Ty(*TheContext), 1)});
  Builder->CreateUnreachable();
  
  TheFunction->insert(TheFunction->end(), JmpBB);
  Builder->SetInsertPoint(JmpBB);
  Value *FrameIdx = Builder->CreateSub(Top, ConstantInt::get(Type::getInt32Ty(*TheContext), 1));
  auto *StackGV = TheModule->getNamedGlobal("turf_exception_stack");
  if (!StackGV) {
    auto *ArrayTy = llvm::ArrayType::get(llvm::ArrayType::get(Type::getInt64Ty(*TheContext), 32), 32);
    StackGV = new GlobalVariable(*TheModule, ArrayTy, false, GlobalValue::InternalLinkage, Constant::getNullValue(ArrayTy), "turf_exception_stack");
  }
  Value *FramePtr = Builder->CreateGEP(StackGV->getValueType(), StackGV, {ConstantInt::get(Type::getInt32Ty(*TheContext), 0), FrameIdx});
  Builder->CreateCall(LongJmpF, {FramePtr, ConstantInt::get(Type::getInt32Ty(*TheContext), 1)});
  Builder->CreateUnreachable();
  
  BasicBlock *DeadBB = BasicBlock::Create(*TheContext, "after_raise", TheFunction);
  Builder->SetInsertPoint(DeadBB);
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
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
