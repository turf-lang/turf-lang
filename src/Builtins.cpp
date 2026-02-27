// The single file to edit when adding new builtin functions.
// Each call to RegisterBuiltins() pushes a BuiltinDef into the Builtins
// vector. The Lexer, Parser, and Codegen layers all read from this registry
// automatically, so no other file needs to change.

#include "Builtins.h"
#include "Codegen.h"  // TheModule, Builder, TheContext
#include "Errors.h"   // SyntaxError
#include "Lexer.h"    // Keywords, SourceLocation

#include "llvm/IR/Function.h"
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
  // Prints one value followed by a newline.
  // Dispatches to the C runtime's printf based on the LLVM type of its arg.
  Builtins.push_back(
      {"print", TOK_BUILTIN_PRINT, /*ArgCount=*/1,
       [](std::vector<Value *> &Args, SourceLocation Loc) -> Value * {
         Value *Val = Args[0];
         Function *PrintfFunc = TheModule->getFunction("printf");
         Type *Ty = Val->getType();

         if (Ty->isDoubleTy()) {
           Value *Fmt = Builder->CreateGlobalStringPtr("%.2f\n", "printstrdbl");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         if (Ty->isIntegerTy(64)) {
           Value *Fmt = Builder->CreateGlobalStringPtr("%lld\n", "printstrint");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         if (Ty->isIntegerTy(1)) {
           Value *Zext =
               Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext), "booltoint");
           Value *Fmt = Builder->CreateGlobalStringPtr("%d\n", "printstrbool");
           return Builder->CreateCall(PrintfFunc, {Fmt, Zext}, "printcall");
         }

         if (Ty->isPointerTy()) {
           Value *Fmt = Builder->CreateGlobalStringPtr("%s\n", "printstrstr");
           return Builder->CreateCall(PrintfFunc, {Fmt, Val}, "printcall");
         }

         SyntaxError(Loc, "Unsupported type for print").raise();
         return nullptr;
       }});

  // Register each builtin's name into the Lexer keyword table
  // This is what makes the Lexer emit the right token when it sees "print",
  // "println", etc. No change to Lexer.cpp is ever needed for new builtins.
  for (const auto &B : Builtins)
    Keywords[B.Name] = B.Token;
}
