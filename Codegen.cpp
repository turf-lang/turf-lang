#include "Codegen.h"
#include "AST.h"
#include "Algorithms.h"
#include "Errors.h"
#include "Lexer.h"
#include "Types.h"
#include "llvm/IR/Verifier.h"
#include <iostream>

using namespace llvm;

std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<IRBuilder<>> Builder;
std::unique_ptr<Module> TheModule;

std::map<std::string, VarInfo> NamedValues;

void InitializeModule() {
  // Holds types and constants
  TheContext = std::make_unique<LLVMContext>();

  // Holds functions
  TheModule = std::make_unique<Module>("Kirk Compiler", *TheContext);

  // Builder to insert instructions
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

Type *getLLVMType(KirkType Type) {
  switch (Type) {
  case KIRK_INT:
    return llvm::Type::getInt64Ty(*TheContext);
  case KIRK_DOUBLE:
    return llvm::Type::getDoubleTy(*TheContext);
  case KIRK_BOOL:
    return llvm::Type::getInt1Ty(*TheContext);
  case KIRK_STRING:
    return llvm::PointerType::get(llvm::Type::getInt8Ty(*TheContext), 0);
  case KIRK_VOID:
    return llvm::Type::getVoidTy(*TheContext);
  default:
    return llvm::Type::getDoubleTy(*TheContext);
  }
}

static KirkType getKirkTypeFromLLVM(Type *Ty) {
  if (Ty->isDoubleTy())
    return KIRK_DOUBLE;
  if (Ty->isIntegerTy(64))
    return KIRK_INT;
  if (Ty->isIntegerTy(1))
    return KIRK_BOOL;
  if (Ty->isPointerTy())
    return KIRK_STRING;
  if (Ty->isVoidTy())
    return KIRK_VOID;
  return KIRK_DOUBLE;
}

static int getTypeRank(KirkType T) {
  switch (T) {
  case KIRK_DOUBLE:
    return 3;
  case KIRK_INT:
    return 2;
  case KIRK_BOOL:
    return 1;
  case KIRK_STRING:
    return 4; // Highest rank, but not castable
  default:
    return 0;
  }
}

static KirkType getCommonType(KirkType A, KirkType B) {
  return getTypeRank(A) >= getTypeRank(B) ? A : B;
}

static Value *CastToType(Value *Val, KirkType DestType,
                         const std::string &Name) {
  KirkType SrcType = getKirkTypeFromLLVM(Val->getType());
  if (SrcType == DestType)
    return Val;

  // Strings cannot be cast to/from other types
  if (SrcType == KIRK_STRING || DestType == KIRK_STRING) {
    SyntaxError(CurLoc, "Cannot cast between string and non-string types")
        .raise();
    return Val;
  }

  switch (DestType) {
  case KIRK_DOUBLE:
    if (SrcType == KIRK_INT)
      return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), Name);
    if (SrcType == KIRK_BOOL)
      return Builder->CreateUIToFP(Val, Type::getDoubleTy(*TheContext), Name);
    break;
  case KIRK_INT:
    if (SrcType == KIRK_DOUBLE)
      return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), Name);
    if (SrcType == KIRK_BOOL)
      return Builder->CreateZExt(Val, Type::getInt64Ty(*TheContext), Name);
    break;
  case KIRK_BOOL:
    if (SrcType == KIRK_DOUBLE)
      return Builder->CreateFCmpONE(
          Val, ConstantFP::get(*TheContext, APFloat(0.0)), Name);
    if (SrcType == KIRK_INT)
      return Builder->CreateICmpNE(
          Val, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), Name);
    break;
  default:
    break;
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
  return Builder->CreateGlobalStringPtr(Val, "str");
}

// Turns any expression into IR operation
Value *BinaryExprAST::codegen() {
  // Generate codes for the Left side and Right side
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if (!L || !R)
    return nullptr;

  KirkType LTy = getKirkTypeFromLLVM(L->getType());
  KirkType RTy = getKirkTypeFromLLVM(R->getType());

  auto CastBoth = [&](KirkType Target) {
    L = CastToType(L, Target, "lhscast");
    R = CastToType(R, Target, "rhscast");
  };

  KirkType CommonType = getCommonType(LTy, RTy);
  KirkType NumericType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;

  // Create the instruction based on the operator
  switch (Op) {

    // Arithmetic
  case '+':
    CastBoth(NumericType);
    return NumericType == KIRK_DOUBLE ? Builder->CreateFAdd(L, R, "addtmp")
                                      : Builder->CreateAdd(L, R, "addtmp");

  case '-':
    CastBoth(NumericType);
    return NumericType == KIRK_DOUBLE ? Builder->CreateFSub(L, R, "subtmp")
                                      : Builder->CreateSub(L, R, "subtmp");

  case '*':
    CastBoth(NumericType);
    return NumericType == KIRK_DOUBLE ? Builder->CreateFMul(L, R, "multmp")
                                      : Builder->CreateMul(L, R, "multmp");

  case '/':
    CastBoth(NumericType);
    return NumericType == KIRK_DOUBLE ? Builder->CreateFDiv(L, R, "divtmp")
                                      : Builder->CreateSDiv(L, R, "divtmp");

  case '%':
    CastBoth(NumericType);
    return NumericType == KIRK_DOUBLE ? Builder->CreateFRem(L, R, "modtmp")
                                      : Builder->CreateSRem(L, R, "modtmp");

  // Comparison
  case '<': {
    KirkType CmpType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == KIRK_DOUBLE ? Builder->CreateFCmpOLT(L, R, "cmptmp")
                                  : Builder->CreateICmpSLT(L, R, "cmptmp");
  }

  case '>': {
    KirkType CmpType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == KIRK_DOUBLE ? Builder->CreateFCmpOGT(L, R, "cmptmp")
                                  : Builder->CreateICmpSGT(L, R, "cmptmp");
  }

  case TOK_EQ: {
    KirkType CmpType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == KIRK_DOUBLE ? Builder->CreateFCmpOEQ(L, R, "cmptmp")
                                  : Builder->CreateICmpEQ(L, R, "cmptmp");
  }

  case TOK_NEQ: {
    KirkType CmpType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == KIRK_DOUBLE ? Builder->CreateFCmpONE(L, R, "cmptmp")
                                  : Builder->CreateICmpNE(L, R, "cmptmp");
  }

  case TOK_GEQ: {
    KirkType CmpType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == KIRK_DOUBLE ? Builder->CreateFCmpOGE(L, R, "cmptmp")
                                  : Builder->CreateICmpSGE(L, R, "cmptmp");
  }

  case TOK_LEQ: {
    KirkType CmpType = (CommonType == KIRK_BOOL) ? KIRK_INT : CommonType;
    CastBoth(CmpType);
    return CmpType == KIRK_DOUBLE ? Builder->CreateFCmpOLE(L, R, "cmptmp")
                                  : Builder->CreateICmpSLE(L, R, "cmptmp");
  }

  // Logical operators
  case TOK_AND: {
    // Cast both operands to bool
    L = CastToType(L, KIRK_BOOL, "lhstobool");
    R = CastToType(R, KIRK_BOOL, "rhstobool");
    return Builder->CreateAnd(L, R, "andtmp");
  }

  case TOK_OR: {
    // Cast both operands to bool
    L = CastToType(L, KIRK_BOOL, "lhstobool");
    R = CastToType(R, KIRK_BOOL, "rhstobool");
    return Builder->CreateOr(L, R, "ortmp");
  }

  // Power
  case '^': {
    // Ensure both operands are double
    CastBoth(KIRK_DOUBLE);

    Function *PowFunc = Intrinsic::getDeclaration(
        TheModule.get(), Intrinsic::pow, Type::getDoubleTy(*TheContext));

    return Builder->CreateCall(PowFunc, {L, R}, "powtmp");
  }
  default:
    SyntaxError(CurLoc, "Error: invalid binary operator");
    return nullptr;
  }
}

// This creates an alloca instruction in the entry block of a function
AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                   const std::string &VarName, KirkType Type) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(getLLVMType(Type), 0, VarName.c_str());
}

Value *VarDeclExprAST::codegen() {
  if (NamedValues.count(Name)) {
    SyntaxError(Loc, "Variable already declared").raise();
    return nullptr;
  }

  Value *Init = InitVal->codegen();
  if (!Init)
    return nullptr;

  Init = CastToType(Init, Type, "initcast");

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Name, Type);

  Builder->CreateStore(Init, Alloca);

  // Store both Alloc and Type in symbol table
  NamedValues[Name] = {Alloca, Type};
  return Init;
}

Value *VariableExprAST::codegen() {
  // Look up the variable in the symbol table
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) {
    AllocaInst *A = Iter->second.Alloca;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
  }

  ReferenceError(Loc, Name, NamedValues).raise();
  return nullptr;
}

Value *AssignmentExprAST::codegen() {
  // Generate code for the RHS first
  Value *Val = RHS->codegen();
  if (!Val)
    return nullptr;

  // Look up the variable
  auto Iter = NamedValues.find(Name);
  if (Iter == NamedValues.end()) {
    SyntaxError(Loc, "Variable must be declared with a type before use")
        .raise();
    return nullptr;
  }

  AllocaInst *Alloca = Iter->second.Alloca;
  KirkType VarType = Iter->second.Type;

  Value *CastVal = CastToType(Val, VarType, "assigncast");

  // Generate the Store instruction
  Builder->CreateStore(CastVal, Alloca);

  // Assignment expressions usually return the value assigned (allows x = y = 5)
  return CastVal;
}

Value *IfExprAST::codegen() {
  Value *CondV = Cond->codegen();
  if (!CondV)
    return nullptr;

  // Convert condition to a boolean
  CondV = CastToType(CondV, KIRK_BOOL, "ifcond");

  // Get the current function so we can insert blocks into it
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Create blocks for 'then', 'else', and 'merge'
  BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

  // Create the Conditional Branch
  // "If CondV is true, go to ThenBB, otherwise go to ElseBB"
  Builder->CreateCondBr(CondV, ThenBB, ElseBB);

  Builder->SetInsertPoint(ThenBB);

  Value *ThenV = Then->codegen();
  if (!ThenV)
    return nullptr;

  Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);

  Value *ElseV = Else->codegen();
  if (!ElseV)
    return nullptr;

  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);

  KirkType ThenType = getKirkTypeFromLLVM(ThenV->getType());
  KirkType ElseType = getKirkTypeFromLLVM(ElseV->getType());
  KirkType MergeType = getCommonType(ThenType, ElseType);

  ThenV = CastToType(ThenV, MergeType, "thencast");
  ElseV = CastToType(ElseV, MergeType, "elsecast");

  // The PHI Node
  PHINode *PN = Builder->CreatePHI(ThenV->getType(), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);

  return PN;
}

Value *UnaryExprAST::codegen() {
  Value *OperandV = Operand->codegen();
  if (!OperandV)
    return nullptr;

  KirkType OperandType = getKirkTypeFromLLVM(OperandV->getType());
  if (OperandType == KIRK_BOOL)
    OperandV = CastToType(OperandV, KIRK_INT, "boolneg");

  OperandType = getKirkTypeFromLLVM(OperandV->getType());

  switch (Opcode) {
  case '-':
    if (OperandType == KIRK_DOUBLE)
      return Builder->CreateFNeg(OperandV);
    if (OperandType == KIRK_INT)
      return Builder->CreateNeg(OperandV);
    SyntaxError(CurLoc, "Unknown unary operand type").raise();
    return nullptr;
  default:
    SyntaxError(CurLoc, "Unknown unary operator");
    return nullptr;
  }
}

Value *BlockExprAST::codegen() {
  Value *LastVal = nullptr;
  for (auto &Expr : Expressions) {
    LastVal = Expr->codegen();
  }
  return LastVal;
}

Value *PrintExprAST::codegen() {
  Value *Val = Expr->codegen();
  if (!Val)
    return nullptr;

  Function *PrintfFunc = TheModule->getFunction("printf");
  Type *Ty = Val->getType();

  if (Ty->isDoubleTy()) {
    Value *FormatStr =
        Builder->CreateGlobalStringPtr("%.2f\n", "printstrdbl");
    return Builder->CreateCall(PrintfFunc, {FormatStr, Val}, "printcall");
  }

  if (Ty->isIntegerTy(64)) {
    Value *FormatStr =
        Builder->CreateGlobalStringPtr("%lld\n", "printstrint");
    return Builder->CreateCall(PrintfFunc, {FormatStr, Val}, "printcall");
  }

  if (Ty->isIntegerTy(1)) {
    Value *Zext = Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext),
                                      "booltoint");
    Value *FormatStr =
        Builder->CreateGlobalStringPtr("%d\n", "printstrbool");
    return Builder->CreateCall(PrintfFunc, {FormatStr, Zext}, "printcall");
  }

  if (Ty->isPointerTy()) {
    Value *FormatStr =
        Builder->CreateGlobalStringPtr("%s\n", "printstrstr");
    return Builder->CreateCall(PrintfFunc, {FormatStr, Val}, "printcall");
  }

  SyntaxError(CurLoc, "Unsupported type for print").raise();
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

  CondV = CastToType(CondV, KIRK_BOOL, "loopcond");

  // Conditional Branch: if true -> Body, else -> After
  Builder->CreateCondBr(CondV, LoopBodyBB, AfterBB);

  // Loop Body Block
  TheFunction->insert(TheFunction->end(), LoopBodyBB);
  Builder->SetInsertPoint(LoopBodyBB);

  if (!Body->codegen())
    return nullptr;

  // Jump back to the condition to loop again
  Builder->CreateBr(LoopCondBB);

  // After Loop Block
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // While loops always return 0.0
  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}
