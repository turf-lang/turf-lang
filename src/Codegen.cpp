#include "Codegen.h"
#include "AST.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "CFG.h"
#include "Errors.h"
#include "Lexer.h"
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
TurfType CurrentFuncReturnType = TURF_VOID;
llvm::Function *CurrentFunction = nullptr;

std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> LoopBlocks;

void InitializeModule() {
  // Holds types and constants
  TheContext = std::make_unique<LLVMContext>();

  // Holds functions
  TheModule = std::make_unique<Module>("Turf Compiler", *TheContext);

  // Builder to insert instructions
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

Type *getLLVMType(TurfType Type) {
  switch (Type) {
  case TURF_INT:
    return llvm::Type::getInt64Ty(*TheContext);
  case TURF_DOUBLE:
    return llvm::Type::getDoubleTy(*TheContext);
  case TURF_BOOL:
    return llvm::Type::getInt1Ty(*TheContext);
  case TURF_STRING:
    return llvm::PointerType::get(*TheContext, 0);
  case TURF_VOID:
    return llvm::Type::getVoidTy(*TheContext);
  default:
    return llvm::Type::getDoubleTy(*TheContext);
  }
}

static TurfType getTurfTypeFromLLVM(Type *Ty) {
  if (Ty->isDoubleTy())
    return TURF_DOUBLE;
  if (Ty->isIntegerTy(64))
    return TURF_INT;
  if (Ty->isIntegerTy(32))
    return TURF_INT;
  if (Ty->isIntegerTy(32))
    return TURF_INT;
  if (Ty->isIntegerTy(1))
    return TURF_BOOL;
  if (Ty->isPointerTy())
    return TURF_STRING;
  if (Ty->isVoidTy())
    return TURF_VOID;
  return TURF_DOUBLE;
}

static int getTypeRank(TurfType T) {
  switch (T) {
  case TURF_DOUBLE:
    return 3;
  case TURF_INT:
    return 2;
  case TURF_BOOL:
    return 1;
  case TURF_STRING:
    return 4; // Highest rank, but not castable
  default:
    return 0;
  }
}

static TurfType getCommonType(TurfType A, TurfType B) {
  return getTypeRank(A) >= getTypeRank(B) ? A : B;
}

static Value *CastToType(Value *Val, TurfType DestType, const std::string &Name,
                         SourceLocation Loc = {0, 0}) {
  Type *ValTy = Val->getType();

  // Normalize foreign integer widths (e.g. i32 from printf) to i64
  // before doing any TurfType-level comparison.
  if (ValTy->isIntegerTy(32)) {
    Val = Builder->CreateSExt(Val, Type::getInt64Ty(*TheContext), "i32toi64");
    ValTy = Val->getType();
  }

  TurfType SrcType = getTurfTypeFromLLVM(ValTy);
  if (SrcType == DestType)
    return Val;

  // Strings cannot be cast to/from other types
  if (SrcType == TURF_STRING || DestType == TURF_STRING) {
    TypeError(CurLoc, "Cannot cast between string and non-string types")
        .raise();
    return Val;
  }

  switch (DestType) {
  case TURF_DOUBLE:
    if (SrcType == TURF_INT)
      return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), Name);
    if (SrcType == TURF_BOOL)
      return Builder->CreateUIToFP(Val, Type::getDoubleTy(*TheContext), Name);
    break;
  case TURF_INT:
    if (SrcType == TURF_DOUBLE)
      return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), Name);
    if (SrcType == TURF_BOOL)
      return Builder->CreateZExt(Val, Type::getInt64Ty(*TheContext), Name);
    break;
  case TURF_BOOL:
    if (SrcType == TURF_DOUBLE)
      return Builder->CreateFCmpONE(
          Val, ConstantFP::get(*TheContext, APFloat(0.0)), Name);
    if (SrcType == TURF_INT)
      return Builder->CreateICmpNE(
          Val, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), Name);
    break;
  default:
    break;
  }

  return Val;
}

// Check whether two TurfTypes are implicitly compatible.
// Numeric types (int, double, bool) are mutually compatible.
// String is only compatible with string. Void is never compatible.
static bool isTypeCompatible(TurfType From, TurfType To) {

  if (From == To) return true;
  if (From == TURF_VOID || To == TURF_VOID) return false;
  if (From == TURF_STRING || To == TURF_STRING) return false;
  // Remaining: int, double, bool — all mutually convertible
  return true;
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

  TurfType LTy = getTurfTypeFromLLVM(L->getType());
  TurfType RTy = getTurfTypeFromLLVM(R->getType());

  auto CastBoth = [&](TurfType Target) {
    L = CastToType(L, Target, "lhscast");
    R = CastToType(R, Target, "rhscast");
  };

  TurfType CommonType = getCommonType(LTy, RTy);
  TurfType NumericType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;

  // Create the instruction based on the operator
  switch (Op) {

    // Arithmetic
  case '+':
    CastBoth(NumericType);
    return NumericType == TURF_DOUBLE ? Builder->CreateFAdd(L, R, "addtmp")
                                      : Builder->CreateAdd(L, R, "addtmp");

  case '-':
    CastBoth(NumericType);
    return NumericType == TURF_DOUBLE ? Builder->CreateFSub(L, R, "subtmp")
                                      : Builder->CreateSub(L, R, "subtmp");

  case '*':
    CastBoth(NumericType);
    return NumericType == TURF_DOUBLE ? Builder->CreateFMul(L, R, "multmp")
                                      : Builder->CreateMul(L, R, "multmp");

  case '/':
    CastBoth(NumericType);
    return NumericType == TURF_DOUBLE ? Builder->CreateFDiv(L, R, "divtmp")
                                      : Builder->CreateSDiv(L, R, "divtmp");

  case '%':
    CastBoth(NumericType);
    return NumericType == TURF_DOUBLE ? Builder->CreateFRem(L, R, "modtmp")
                                      : Builder->CreateSRem(L, R, "modtmp");

  // Comparison
  case '<': {
    TurfType CmpType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == TURF_DOUBLE ? Builder->CreateFCmpOLT(L, R, "cmptmp")
                                  : Builder->CreateICmpSLT(L, R, "cmptmp");
  }

  case '>': {
    TurfType CmpType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == TURF_DOUBLE ? Builder->CreateFCmpOGT(L, R, "cmptmp")
                                  : Builder->CreateICmpSGT(L, R, "cmptmp");
  }

  case TOK_EQ: {
    TurfType CmpType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == TURF_DOUBLE ? Builder->CreateFCmpOEQ(L, R, "cmptmp")
                                  : Builder->CreateICmpEQ(L, R, "cmptmp");
  }

  case TOK_NEQ: {
    TurfType CmpType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == TURF_DOUBLE ? Builder->CreateFCmpONE(L, R, "cmptmp")
                                  : Builder->CreateICmpNE(L, R, "cmptmp");
  }

  case TOK_GEQ: {
    TurfType CmpType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == TURF_DOUBLE ? Builder->CreateFCmpOGE(L, R, "cmptmp")
                                  : Builder->CreateICmpSGE(L, R, "cmptmp");
  }

  case TOK_LEQ: {
    TurfType CmpType = (CommonType == TURF_BOOL) ? TURF_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == TURF_DOUBLE ? Builder->CreateFCmpOLE(L, R, "cmptmp")
                                  : Builder->CreateICmpSLE(L, R, "cmptmp");
  }

  // Logical operators
  case TOK_AND: {
    // Cast both operands to bool
    L = CastToType(L, TURF_BOOL, "lhstobool");
    R = CastToType(R, TURF_BOOL, "rhstobool");
    return Builder->CreateAnd(L, R, "andtmp");
  }

  case TOK_OR: {
    // Cast both operands to bool
    L = CastToType(L, TURF_BOOL, "lhstobool");
    R = CastToType(R, TURF_BOOL, "rhstobool");
    return Builder->CreateOr(L, R, "ortmp");
  }

  // Power
  case '^': {
    // Ensure both operands are double
    CastBoth(TURF_DOUBLE);

    Function *PowFunc = Intrinsic::getDeclaration(
        TheModule.get(), Intrinsic::pow, Type::getDoubleTy(*TheContext));

    return Builder->CreateCall(PowFunc, {L, R}, "powtmp");
  }
  default:
    SemanticError(CurLoc, "Error: invalid binary operator");
    return nullptr;
  }
}

// This creates an alloca instruction in the entry block of a function
AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                   const std::string &VarName, TurfType Type) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(getLLVMType(Type), 0, VarName.c_str());
}

Value *VarDeclExprAST::codegen() {
  if (Type == TURF_VOID) {
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
      // Emit warning but continue compilation
      ShadowingWarning(Loc, Name, Shadowed->DeclLoc).warn();
    }
  }

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
  TurfType InitType = getTurfTypeFromLLVM(Init->getType());
  if (!isTypeCompatible(InitType, Type)) {
    TypeError(Loc, std::string("Cannot initialize '") + turfTypeName(Type) +
                       "' variable '" + Name + "' with a value of type '" +
                       turfTypeName(InitType) + "'")
        .raise();
    return nullptr;
  }

  Init = CastToType(Init, Type, "initcast", Loc);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Name, Type);

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

Value *AssignmentExprAST::codegen() {
  // Generate code for the RHS first
  Value *Val = RHS->codegen();
  if (!Val)
    return nullptr;

  // Detect void value (e.g. x = voidFunc())
  if (Val->getType()->isVoidTy()) {
    VoidValueError(
        Loc, "You're trying to assign the result of a void function to '" +
                 Name + "'.")
        .raise();
    return nullptr;
  }

  if (Keywords.find(Name) != Keywords.end()) {
    SemanticError(Loc, "Cannot assign to keyword '" + Name +
                           "'. Keyword is reserved.")
        .raise();
    return nullptr;
  }

  // Look up the variable
  auto Iter = NamedValues.find(Name);
  if (Iter == NamedValues.end()) {
    SemanticError(Loc, "Variable must be declared with a type before use")
        .raise();
    return nullptr;
  }

  AllocaInst *Alloca = Iter->second.Alloca;
  TurfType VarType = Iter->second.Type;

  Value *CastVal = CastToType(Val, VarType, "assigncast", Loc);

  // Generate the Store instruction
  Builder->CreateStore(CastVal, Alloca);

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
      CondV = CastToType(CondV, TURF_BOOL, "ifcond");

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
  CondV = CastToType(CondV, TURF_BOOL, "ifcond");

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

  TurfType ThenType = getTurfTypeFromLLVM(ThenV->getType());
  TurfType ElseType = getTurfTypeFromLLVM(ElseV->getType());
  TurfType MergeType = getCommonType(ThenType, ElseType);

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

  TurfType OperandType = getTurfTypeFromLLVM(OperandV->getType());
  if (OperandType == TURF_BOOL)
    OperandV = CastToType(OperandV, TURF_INT, "boolneg");

  OperandType = getTurfTypeFromLLVM(OperandV->getType());

  switch (Opcode) {
  case '-':
    if (OperandType == TURF_DOUBLE)
      return Builder->CreateFNeg(OperandV);
    if (OperandType == TURF_INT)
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

Value *CastExprAST::codegen() {
  Value *Val = Operand->codegen();
  if (!Val)
    return nullptr;

  TurfType SrcType = getTurfTypeFromLLVM(Val->getType());

  // Identity - no-op
  if (SrcType == DestType)
    return Val;

  // numeric → numeric
  if (SrcType == TURF_INT && DestType == TURF_DOUBLE)
    return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), "itod");

  if (SrcType == TURF_DOUBLE && DestType == TURF_INT)
    return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), "dtoi");

  // string → int  : call strtoll(str, &endptr, 10)
  if (SrcType == TURF_STRING && DestType == TURF_INT) {
    // Declare strtoll if not already in the module
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
  if (SrcType == TURF_STRING && DestType == TURF_DOUBLE) {
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

  // int → string : snprintf(buf, 32, "%lld", val)
  if (SrcType == TURF_INT && DestType == TURF_STRING) {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext),
                            {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr},
                            /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf",
                                   TheModule.get());
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize,
                                        "int_to_str_buf");

    Value *Fmt = Builder->CreateGlobalString("%lld", "intfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_int");
    return Buf;
  }

  // double → string : snprintf(buf, 32, "%g", val)
  if (SrcType == TURF_DOUBLE && DestType == TURF_STRING) {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext),
                            {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr},
                            /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf",
                                   TheModule.get());
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize,
                                        "dbl_to_str_buf");

    Value *Fmt = Builder->CreateGlobalString("%g", "dblfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_dbl");
    return Buf;
  }

  // bool → string : select(val, "true", "false")
  if (SrcType == TURF_BOOL && DestType == TURF_STRING) {
    Value *TrueStr = Builder->CreateGlobalString("true", "str_true");
    Value *FalseStr = Builder->CreateGlobalString("false", "str_false");
    return Builder->CreateSelect(Val, TrueStr, FalseStr, "bool_to_str");
  }

  // Unsupported → string
  if (DestType == TURF_STRING) {
    const char *SrcName = (SrcType == TURF_INT)      ? "int"
                          : (SrcType == TURF_DOUBLE) ? "double"
                          : (SrcType == TURF_BOOL)   ? "bool"
                          : (SrcType == TURF_STRING) ? "string"
                                                     : "void";
    StringConversionError(Loc, SrcName).raise();
    return nullptr;
  }

  // Everything else is unsupported
  const char *SrcName = (SrcType == TURF_INT)      ? "int"
                        : (SrcType == TURF_DOUBLE) ? "double"
                        : (SrcType == TURF_BOOL)   ? "bool"
                        : (SrcType == TURF_STRING) ? "string"
                                                   : "unknown";
  const char *DstName = (DestType == TURF_INT)      ? "int"
                        : (DestType == TURF_DOUBLE) ? "double"
                        : (DestType == TURF_BOOL)   ? "bool"
                        : (DestType == TURF_STRING) ? "string"
                        : (DestType == TURF_VOID)   ? "void"
                                                    : "unknown";
  TypeError(Loc, std::string("Cannot explicitly cast '") + SrcName + "' to '" +
                     DstName + "'")
      .raise();
  return nullptr;
}

Value *BuiltinCallExprAST::codegen() {
  const BuiltinDef *Def = FindBuiltin(Name);
  if (!Def) {
    SemanticError(CurLoc, "Unknown builtin function: '" + Name + "'").raise();
    return nullptr;
  }

  // Evaluate every argument expression first
  std::vector<Value *> ArgVals;
  for (auto &Arg : Args) {
    Value *V = Arg->codegen();
    if (!V)
      return nullptr;
    ArgVals.push_back(V);
  }

  // Dispatch to the lambda defined in Builtins.cpp
  return Def->Codegen(ArgVals, CurLoc);
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

  CondV = CastToType(CondV, TURF_BOOL, "loopcond");

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
  TurfType VarType = TURF_INT;
  if (StartV->getType()->isDoubleTy())
    VarType = TURF_DOUBLE;

  StartV = CastToType(StartV, VarType, "forstart");

  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, VarType);
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
      CreateEntryBlockAlloca(TheFunction, VarName + ".end", VarType);
  Builder->CreateStore(EndV, EndAlloca);

  //  3. Determine loop direction: start < end → ascending, else descending
  // Compare start vs end to pick the right comparison operator.
  Value *Ascending;
  if (VarType == TURF_INT) {
    Ascending = Builder->CreateICmpSLT(StartV, EndV, "ascending");
  } else {
    Ascending = Builder->CreateFCmpOLT(StartV, EndV, "ascending");
  }

  // Store direction flag
  AllocaInst *AscAlloca =
      CreateEntryBlockAlloca(TheFunction, VarName + ".asc", TURF_BOOL);
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
  Value *CurVar = Builder->CreateLoad(getLLVMType(VarType), Alloca, VarName);
  Value *EndVal =
      Builder->CreateLoad(getLLVMType(VarType), EndAlloca, VarName + ".end");
  Value *AscFlag =
      Builder->CreateLoad(Type::getInt1Ty(*TheContext), AscAlloca, "asc");

  // If ascending: cond = (i < end), else: cond = (i > end)
  Value *CondAsc, *CondDesc;
  if (VarType == TURF_INT) {
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
  // Build/look up the LLVM prototype
  Function *TheFunc = TheModule->getFunction(Name);

  if (!TheFunc) {
    // Build the LLVM FunctionType
    std::vector<Type *> ParamTypes;
    for (const auto &P : Params)
      ParamTypes.push_back(getLLVMType(P.Type));

    FunctionType *FT = FunctionType::get(getLLVMType(ReturnType), ParamTypes,
                                         /*isVarArg=*/false);
    TheFunc =
        Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

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

  // Enter a new scope for the function body
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // Alloca each parameter and add to NamedValues and GlobalSymbolTable
  for (auto &Arg : TheFunc->args()) {
    TurfType ParamTurfType = TURF_INT;
    for (const auto &P : Params)
      if (P.Name == Arg.getName())
        ParamTurfType = P.Type;

    AllocaInst *Alloca = CreateEntryBlockAlloca(
        TheFunc, std::string(Arg.getName()), ParamTurfType);
    Builder->CreateStore(&Arg, Alloca);
    NamedValues[std::string(Arg.getName())] = {Alloca, ParamTurfType};

    // Add parameter to symbol table
    if (GlobalSymbolTable) {
      SourceLocation ParamLoc = Loc; // Use function declaration location
      GlobalSymbolTable->DeclareSymbol(std::string(Arg.getName()),
                                       ParamTurfType, ParamLoc, Alloca);
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
    } else if (ReturnType == TURF_VOID) {
      Builder->CreateRetVoid();
    } else {
      // CFG already reported the error, but we still need to generate valid IR
      Builder->CreateRet(Constant::getNullValue(getLLVMType(ReturnType)));
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
    if (CurrentFuncReturnType != TURF_VOID) {
      TypeError(Loc, "'return;' is only valid inside a void function").raise();
      return nullptr;
    }
    Builder->CreateRetVoid();
  } else {
    // return <expr>;
    if (CurrentFuncReturnType == TURF_VOID) {
      TypeError(Loc, "void function cannot return a value").raise();
      return nullptr;
    }
    Value *RetVal = Val->codegen();
    if (!RetVal)
      return nullptr;

    // Check for void return value (e.g. return voidFunc())
    if (RetVal->getType()->isVoidTy()) {
      VoidValueError(Loc,
                     "You're trying to return the result of a void function.")
          .raise();
      return nullptr;
    }

    // Check type compatibility before implicit cast
    TurfType ActualType = getTurfTypeFromLLVM(RetVal->getType());
    if (!isTypeCompatible(ActualType, CurrentFuncReturnType)) {
      ReturnTypeMismatchError(Loc, CurrentFunction->getName().str(),
                              turfTypeName(CurrentFuncReturnType),
                              turfTypeName(ActualType))
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

// FuncCallExprAST::codegen
Value *FuncCallExprAST::codegen() {
  Function *CalleeF = TheModule->getFunction(Name);
  if (!CalleeF) {
    SemanticError(Loc, "Unknown function: '" + Name + "'").raise();
    return nullptr;
  }

  if (CalleeF->arg_size() != Args.size()) {
    SemanticError(Loc, "Wrong number of arguments to '" + Name +
                           "': expected " +
                           std::to_string(CalleeF->arg_size()) + ", got " +
                           std::to_string(Args.size()))
        .raise();
    return nullptr;
  }

  std::vector<Value *> ArgVals;
  unsigned Idx = 0;
  for (auto &Arg : CalleeF->args()) {
    Value *V = Args[Idx]->codegen();
    if (!V)
      return nullptr;

    TurfType ExpectedType = getTurfTypeFromLLVM(Arg.getType());

    // Check for void argument (e.g. passing a void function result)
    if (V->getType()->isVoidTy()) {
      VoidValueError(Loc, "Argument " + std::to_string(Idx + 1) + " to '" +
                              Name + "' is a void function call.")
          .raise();
      return nullptr;
    }

    TurfType ActualType = getTurfTypeFromLLVM(V->getType());
    if (!isTypeCompatible(ActualType, ExpectedType)) {
      ArgumentTypeError(Loc, Name, std::string(Arg.getName()), Idx + 1,
                        turfTypeName(ExpectedType), turfTypeName(ActualType))
          .raise();
      return nullptr;
    }

    V = CastToType(V, ExpectedType, "argcast", Loc);
    ArgVals.push_back(V);
    Idx++;
  }

  // void functions must not be given a name (LLVM requirement)
  if (CalleeF->getReturnType()->isVoidTy())
    return Builder->CreateCall(CalleeF, ArgVals);

  return Builder->CreateCall(CalleeF, ArgVals, "calltmp");
}

// Array Codegen
Value *ArrayDeclExprAST::codegen() {
  if (ElementType == TURF_VOID) {
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
  int TotalSize = getTotalSize();
  if (!InitList.empty() && static_cast<int>(InitList.size()) != TotalSize) {
    ArraySizeMismatchError(Loc, Name, TotalSize, static_cast<int>(InitList.size()))
        .raise();
    return nullptr;
  }

  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  Type *ElemTy = getLLVMType(ElementType);
  Type *ArrTy = ElemTy;
  for (auto it = Dimensions.rbegin(); it != Dimensions.rend(); ++it) {
    ArrTy = llvm::ArrayType::get(ArrTy, *it);
  }

  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  AllocaInst *Alloca = TmpB.CreateAlloca(ArrTy, nullptr, Name);

  // Zero-initialize the entire array (memset to 0)
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
      TurfType ElemTurfType = getTurfTypeFromLLVM(ElemVal->getType());
      if (!isTypeCompatible(ElemTurfType, ElementType)) {
        ArrayTypeMismatchError(Loc, Name, turfTypeName(ElementType),
                               turfTypeName(ElemTurfType))
            .raise();
        return nullptr;
      }

      ElemVal = CastToType(ElemVal, ElementType, "arrayinit", Loc);

      // Map flat index `i` back to {dim0, dim1, ...} for GEP
      std::vector<Value *> GEPIndices(Dimensions.size() + 1);
      GEPIndices[0] = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
      int temp = i;
      for (int d = Dimensions.size() - 1; d >= 0; --d) {
        int idx = temp % Dimensions[d];
        temp /= Dimensions[d];
        GEPIndices[d + 1] = ConstantInt::get(Type::getInt64Ty(*TheContext), idx);
      }

      Value *ElemPtr = Builder->CreateInBoundsGEP(ArrTy, Alloca, GEPIndices, "arrelem");
      Builder->CreateStore(ElemVal, ElemPtr);
    }
  }

  // Register in symbol table
  TurfType ArrType = getArrayType(ElementType);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(Name, ArrType, Loc, Alloca, Dimensions);
  }

  NamedValues[Name] = {Alloca, ArrType, Dimensions};

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

  // Out-of-bounds block: print error and exit
  Builder->SetInsertPoint(OobBB);

  // Declare puts if not already
  Type *I8Ptr = PointerType::get(*TheContext, 0);
  Function *PutsF = TheModule->getFunction("puts");
  if (!PutsF) {
    FunctionType *FT =
        FunctionType::get(Type::getInt32Ty(*TheContext), {I8Ptr}, false);
    PutsF = Function::Create(FT, Function::ExternalLinkage, "puts",
                             TheModule.get());
  }
  Value *ErrMsg = Builder->CreateGlobalString(
      "Runtime Error: Array index out of bounds for '" + ArrayName + "'.",
      "arr_oob_msg");
  Builder->CreateCall(PutsF, {ErrMsg});

  // Declare exit if not already
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

  // Continue block
  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
}

Value *ArrayAccessExprAST::codegen() {
  // Look up the array variable
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) {
    VI = &Iter->second;
  }

  if (!VI || !isArrayType(VI->Type)) {
    SemanticError(Loc, "'" + Name + "' is not an array").raise();
    return nullptr;
  }

  TurfType ElemType = getArrayElementType(VI->Type);

  if (Indices.size() != VI->ArrayDims.size()) {
    SemanticError(Loc, "'" + Name + "' is a " + std::to_string(VI->ArrayDims.size()) + 
                       "D array, but " + std::to_string(Indices.size()) + " indices were provided.").raise();
    return nullptr;
  }

  std::vector<Value *> GEPIndices;
  GEPIndices.push_back(ConstantInt::get(Type::getInt64Ty(*TheContext), 0));

  for (size_t d = 0; d < Indices.size(); ++d) {
    Value *IndexVal = Indices[d]->codegen();
    if (!IndexVal) return nullptr;

    // Index must be integer
    TurfType IdxType = getTurfTypeFromLLVM(IndexVal->getType());
    if (IdxType == TURF_STRING) {
      ArrayNonIntegerIndexError(Loc, turfTypeName(IdxType)).raise();
      return nullptr;
    }
    IndexVal = CastToType(IndexVal, TURF_INT, "arridx", Loc);

    int ArraySize = VI->ArrayDims[d];

    // Compile-time bounds check for constant indices
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError(Loc, Name, Idx, ArraySize).raise();
        return nullptr;
      }
    } else {
      // Runtime bounds check
      std::string DimName = Name + " (dim " + std::to_string(d) + ")";
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, DimName, Loc);
    }
    
    GEPIndices.push_back(IndexVal);
  }

  // Build the LLVM nested array type for the GEP
  Type *ElemTy = getLLVMType(ElemType);
  Type *ArrTy = ElemTy;
  for (auto it = VI->ArrayDims.rbegin(); it != VI->ArrayDims.rend(); ++it) {
    ArrTy = llvm::ArrayType::get(ArrTy, *it);
  }

  Value *ElemPtr = Builder->CreateInBoundsGEP(ArrTy, VI->Alloca, GEPIndices, "arrelem");
  return Builder->CreateLoad(ElemTy, ElemPtr, Name + "_elem");
}

Value *ArrayAssignExprAST::codegen() {
  // Look up the array variable
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) {
    VI = &Iter->second;
  }

  if (!VI || !isArrayType(VI->Type)) {
    SemanticError(Loc, "'" + Name + "' is not an array").raise();
    return nullptr;
  }

  TurfType ElemType = getArrayElementType(VI->Type);

  if (Indices.size() != VI->ArrayDims.size()) {
    SemanticError(Loc, "'" + Name + "' is a " + std::to_string(VI->ArrayDims.size()) + 
                       "D array, but " + std::to_string(Indices.size()) + " indices were provided.").raise();
    return nullptr;
  }

  std::vector<Value *> GEPIndices;
  GEPIndices.push_back(ConstantInt::get(Type::getInt64Ty(*TheContext), 0));

  for (size_t d = 0; d < Indices.size(); ++d) {
    Value *IndexVal = Indices[d]->codegen();
    if (!IndexVal) return nullptr;

    // Index must be integer
    TurfType IdxType = getTurfTypeFromLLVM(IndexVal->getType());
    if (IdxType == TURF_STRING) {
      ArrayNonIntegerIndexError(Loc, turfTypeName(IdxType)).raise();
      return nullptr;
    }
    IndexVal = CastToType(IndexVal, TURF_INT, "arridx", Loc);

    int ArraySize = VI->ArrayDims[d];

    // Compile-time bounds check for constant indices
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError(Loc, Name, Idx, ArraySize).raise();
        return nullptr;
      }
    } else {
      // Runtime bounds check
      std::string DimName = Name + " (dim " + std::to_string(d) + ")";
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, DimName, Loc);
    }
    
    GEPIndices.push_back(IndexVal);
  }

  // Evaluate the RHS value
  Value *RHSVal = RHS->codegen();
  if (!RHSVal)
    return nullptr;

  // Detect void RHS
  if (RHSVal->getType()->isVoidTy()) {
    VoidValueError(Loc, "You're trying to store a void value in array '" +
                            Name + "'.")
        .raise();
    return nullptr;
  }

  // Type check
  TurfType RHSType = getTurfTypeFromLLVM(RHSVal->getType());
  if (!isTypeCompatible(RHSType, ElemType)) {
    ArrayTypeMismatchError(Loc, Name, turfTypeName(ElemType),
                           turfTypeName(RHSType))
        .raise();
    return nullptr;
  }

  RHSVal = CastToType(RHSVal, ElemType, "arrstore", Loc);

  // Build the LLVM nested array type for the GEP
  Type *ElemTy = getLLVMType(ElemType);
  Type *ArrTy = ElemTy;
  for (auto it = VI->ArrayDims.rbegin(); it != VI->ArrayDims.rend(); ++it) {
    ArrTy = llvm::ArrayType::get(ArrTy, *it);
  }

  Value *ElemPtr = Builder->CreateInBoundsGEP(ArrTy, VI->Alloca, GEPIndices, "arrelem");
  Builder->CreateStore(RHSVal, ElemPtr);
  
  return RHSVal;
}

Value *ArrayLengthExprAST::codegen() {
  // Look up the array variable
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) {
    VI = &Iter->second;
  }

  if (!VI || !isArrayType(VI->Type)) {
    SemanticError(Loc, "'" + Name +
                           "' is not an array. '.length' can only be used on arrays.")
        .raise();
    return nullptr;
  }

  int ResultSize = 1;
  if (VI->ArrayDims.empty()) {
    ResultSize = 0;
  } else if (DimIndex == -1) {
    for (int dim : VI->ArrayDims) ResultSize *= dim;
  } else if (DimIndex >= 0 && DimIndex < static_cast<int>(VI->ArrayDims.size())) {
    ResultSize = VI->ArrayDims[DimIndex];
  } else {
    ResultSize = 0;
  }

  // Return the compile-time constant size
  return ConstantInt::get(Type::getInt64Ty(*TheContext), ResultSize);
}

