// The single file to edit when adding new builtin functions.
// Each call to RegisterBuiltins() pushes a BuiltinDef into the Builtins
// vector. The Lexer, Parser, and Codegen layers all read from this registry
// automatically, so no other file needs to change.

#include "Builtins.h"
#include "Codegen.h" // TheModule, Builder, TheContext
#include "Errors.h"  // SyntaxError
#include "Lexer.h"   // Keywords, SourceLocation

#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"

using namespace llvm;

std::vector<BuiltinDef> Builtins;

// Lookup helpers
const BuiltinDef *FindBuiltin(const std::string &Name) {
  for (const auto &B : Builtins)
    if (B.Name == Name)
      return &B;
  return nullptr;
}

const BuiltinDef *FindBuiltinByToken(int Tok) {
  for (const auto &B : Builtins)
    if (B.Token == Tok)
      return &B;
  return nullptr;
}

// TO ADD A NEW BUILTIN:
//   1. Add a token constant to the BuiltinToken enum in Builtins.h
//      (e.g.  TOK_BUILTIN_PRINTLN = -101)
//   2. Push a new BuiltinDef into Builtins below.

void RegisterBuiltins() {
  // print(expr)
  // Prints one value.
  Builtins.push_back(
      {"print", TOK_BUILTIN_PRINT, /*ArgCount=*/1,
       [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
         Value *Val = Args[0];
         Function *PrintfFunc = TheModule->getFunction("printf");
         Type *Ty = Val->getType();

         if (Ty->isDoubleTy()) {
           Value *Fmt = Builder->CreateGlobalString("%g", "printstrdbl");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         if (Ty->isIntegerTy(64)) {
           Value *Fmt = Builder->CreateGlobalString("%lld", "printstrint");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         if (Ty->isIntegerTy(1)) {
           Value *Zext = Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext),
                                             "booltoint");
           Value *Fmt = Builder->CreateGlobalString("%d", "printstrbool");
           return Builder->CreateCall(PrintfFunc, {Fmt, Zext}, "printcall");
         }

         if (Ty->isPointerTy()) {
           Value *Fmt = Builder->CreateGlobalString("%s", "printstrstr");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         TypeError(Loc, "Unsupported type for print").raise();
         return nullptr;
       }});

  // printline(expr)
  // Prints one value followed by a new line.
  Builtins.push_back(
      {"printline", TOK_BUILTIN_PRINTLINE, /*ArgCount=*/1,
       [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
         Value *Val = Args[0];
         Function *PrintfFunc = TheModule->getFunction("printf");
         Type *Ty = Val->getType();

         if (Ty->isDoubleTy()) {
           Value *Fmt = Builder->CreateGlobalString("%g\n", "printstrdbl");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         if (Ty->isIntegerTy(64)) {
           Value *Fmt = Builder->CreateGlobalString("%lld\n", "printstrint");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         if (Ty->isIntegerTy(1)) {
           Value *Zext = Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext),
                                             "booltoint");
           Value *Fmt = Builder->CreateGlobalString("%d\n", "printstrbool");
           return Builder->CreateCall(PrintfFunc, {Fmt, Zext}, "printcall");
         }

         if (Ty->isPointerTy()) {
           Value *Fmt = Builder->CreateGlobalString("%s\n", "printstrstr");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         SyntaxError(Loc, "Unsupported type for print").raise();
         return nullptr;
       }});

  // input()
  // Reads string from stdin
  Builtins.push_back(
      {"input", TOK_BUILTIN_INPUT, /*ArgCount=*/0,
       [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
         // Create functions 'calloc', 'scanf', 'getchar' if they don't exist
         Function *CallocFunc = TheModule->getFunction("calloc");
         if (!CallocFunc) {
           FunctionType *FT = FunctionType::get(
               PointerType::getUnqual(*TheContext),
               {Type::getInt64Ty(*TheContext), Type::getInt64Ty(*TheContext)},
               false);
           CallocFunc = Function::Create(FT, Function::ExternalLinkage,
                                         "calloc", TheModule.get());
         }

         Function *ScanfFunc = TheModule->getFunction("scanf");
         if (!ScanfFunc) {
           FunctionType *FT =
               FunctionType::get(Type::getInt32Ty(*TheContext),
                                 {PointerType::getUnqual(*TheContext)}, true);
           ScanfFunc = Function::Create(FT, Function::ExternalLinkage, "scanf",
                                        TheModule.get());
         }

         Function *GetcharFunc = TheModule->getFunction("getchar");
         if (!GetcharFunc) {
           FunctionType *FT =
               FunctionType::get(Type::getInt32Ty(*TheContext), false);
           GetcharFunc = Function::Create(FT, Function::ExternalLinkage,
                                          "getchar", TheModule.get());
         }

         // calloc(4096, 1)
         Value *NumItems =
             ConstantInt::get(Type::getInt64Ty(*TheContext), 4096);
         Value *Size = ConstantInt::get(Type::getInt64Ty(*TheContext), 1);
         Value *Buf = Builder->CreateCall(CallocFunc, {NumItems, Size}, "buf");

         // scanf("%4095[^\n]", Buf)
         Value *Fmt = Builder->CreateGlobalString("%4095[^\n]", "scanfmt");
         Builder->CreateCall(ScanfFunc, {Fmt, Buf});

         // getchar()
         Builder->CreateCall(GetcharFunc, {});

         return Buf;
       }});

  // lengthof(string) -> int
  // Returns the number of characters (bytes) in a string.
  Builtins.push_back(
      {"lengthof", TOK_BUILTIN_LENGTHOF, /*ArgCount=*/1,
       [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
         Value *Val = Args[0];
         Type *Ty = Val->getType();

         // Enforce string (pointer) type
         if (!Ty->isPointerTy()) {
           const char *TypeName = Ty->isDoubleTy()      ? "double"
                                  : Ty->isIntegerTy(64) ? "int"
                                  : Ty->isIntegerTy(1)  ? "bool"
                                                        : "unknown";
           LengthofTypeError(Loc, TypeName).raise();
           return nullptr;
         }

         // Declare strlen if not already in the module:
         //   i64 strlen(i8*)
         Function *StrlenF = TheModule->getFunction("strlen");
         if (!StrlenF) {
           FunctionType *FT =
               FunctionType::get(Type::getInt64Ty(*TheContext),
                                 {PointerType::getUnqual(*TheContext)},
                                 /*isVarArg=*/false);
           StrlenF = Function::Create(FT, Function::ExternalLinkage, "strlen",
                                      TheModule.get());
         }

         return Builder->CreateCall(StrlenF, {Val}, "lengthof");
       }});

   // typeof(expression) -> string
   // Returns the static type name of the expression as a string.
   Builtins.push_back(
       {"typeof", TOK_BUILTIN_TYPEOF, /*ArgCount=*/1,
        [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
          Value *Val = Args[0];
          Type *Ty = Val->getType();
 
          // Determine the Turf type from LLVM type
          const char *TypeName = nullptr;
          if (Ty->isDoubleTy()) {
            TypeName = "double";
          } else if (Ty->isIntegerTy(64)) {
            TypeName = "int";
          } else if (Ty->isIntegerTy(1)) {
            TypeName = "bool";
          } else if (Ty->isPointerTy()) {
            TypeName = "string";
          } else if (Ty->isVoidTy()) {
            TypeName = "void";
          } else {
            TypeName = "unknown";
          }
 
          // Create a global constant string with the type name
          return Builder->CreateGlobalString(TypeName, "typename");
        }});

   // abs(expr)
   // Returns the absolute value. Only int and double supported.
   Builtins.push_back(
       {"abs", TOK_BUILTIN_ABS, /*ArgCount=*/1,
        [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
          Value *Val = Args[0];
          Type *Ty = Val->getType();

          if (Ty->isDoubleTy()) {
            Function *FAbsFunc = Intrinsic::getOrInsertDeclaration(
                TheModule.get(), Intrinsic::fabs,
                {Type::getDoubleTy(*TheContext)});
            return Builder->CreateCall(FAbsFunc, {Val}, "abs");
          }

          if (Ty->isIntegerTy(64)) {
            Value *Zero =
                ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
            Value *Neg = Builder->CreateNeg(Val, "absneg");
            Value *Cmp = Builder->CreateICmpSLT(Val, Zero, "abscmp");
            return Builder->CreateSelect(Cmp, Neg, Val, "abs");
          }

          const char *TypeName = Ty->isIntegerTy(1) ? "bool"
                                : Ty->isPointerTy()  ? "string"
                                                     : "unknown";
          AbsArgTypeError(Loc, TypeName).raise();
          return nullptr;
        }});

   // power(base, exponent)
   // Returns base raised to exponent.
   // Deterministic rule: if either operand is double, result is double.
   // Only int,int returns int.
   Builtins.push_back(
       {"power", TOK_BUILTIN_POWER, /*ArgCount=*/2,
        [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
          Value *A = Args[0];
          Value *B = Args[1];
          Type *ATy = A->getType();
          Type *BTy = B->getType();

          bool AIsDouble = ATy->isDoubleTy();
          bool BIsDouble = BTy->isDoubleTy();
          bool AIsInt = ATy->isIntegerTy(64);
          bool BIsInt = BTy->isIntegerTy(64);

          if (!(AIsDouble || AIsInt) || !(BIsDouble || BIsInt)) {
            const char *AName = AIsDouble   ? "double"
                                : AIsInt    ? "int"
                                : ATy->isIntegerTy(1) ? "bool"
                                : ATy->isPointerTy()  ? "string"
                                                      : "unknown";
            const char *BName = BIsDouble   ? "double"
                                : BIsInt    ? "int"
                                : BTy->isIntegerTy(1) ? "bool"
                                : BTy->isPointerTy()  ? "string"
                                                      : "unknown";
            PowerArgTypeError(Loc, AName, BName).raise();
            return nullptr;
          }

          if (!AIsDouble)
            A = Builder->CreateSIToFP(A, Type::getDoubleTy(*TheContext),
                                      "powcast1");
          if (!BIsDouble)
            B = Builder->CreateSIToFP(B, Type::getDoubleTy(*TheContext),
                                      "powcast2");

          Function *PowFunc = Intrinsic::getOrInsertDeclaration(
              TheModule.get(), Intrinsic::pow,
              {Type::getDoubleTy(*TheContext)});
          Value *Result =
              Builder->CreateCall(PowFunc, {A, B}, "powtmp");

          if (AIsInt && BIsInt)
            return Builder->CreateFPToSI(Result,
                                         Type::getInt64Ty(*TheContext),
                                         "powint");

          return Result;
        }});

   // Register each builtin's name into the Lexer keyword table
  // This is what makes the Lexer emit the right token when it sees "print",
  // "println", etc. No change to Lexer.cpp is ever needed for new builtins.
  for (const auto &B : Builtins)
    Keywords[B.Name] = B.Token;
}
