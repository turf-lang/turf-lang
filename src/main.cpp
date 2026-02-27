#include "Builtins.h"
#include "Codegen.h"
#include "Lexer.h"
#include "Parser.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <iostream>

using namespace llvm;

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: turf <filename.tr>\n";
    return 1;
  }

  SourceFile.open(argv[1]);
  if (!SourceFile.is_open()) {
    std::cerr << "Error: Could not open file " << argv[1] << "\n";
    return 1;
  }

  // Read file into memory for error printing
  std::ifstream File(argv[1]);
  std::string Line;
  while (std::getline(File, Line)) {
    SourceLines.push_back(Line);
  }

  RegisterBuiltins();   // Populate builtin registry + inject keywords into Lexer
  InitializeModule();   // Initialize LLVM Context, Module, Builder
  InitializePrecedence();

  // Setup the main function wrapper to hold all the code
  FunctionType *PrintfType =
      FunctionType::get(Type::getInt32Ty(*TheContext),
                        {PointerType::getUnqual(*TheContext)}, true);
  Function *PrintfFunc = Function::Create(PrintfType, Function::ExternalLinkage,
                                          "printf", TheModule.get());

  FunctionType *FT = FunctionType::get(Type::getInt32Ty(*TheContext), false);
  Function *TheFunction =
      Function::Create(FT, Function::ExternalLinkage, "main", TheModule.get());

  BasicBlock *Entry = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(Entry);

  // Load the first token before entering the loop
  getNextToken();

  // Main Loop
  while (true) {
    if (CurTok == TOK_EOF)
      break; // Stop if end of file

    if (CurTok == ';') {
      getNextToken(); // Skip semicolons
      continue;
    }

    // Parse the next expression
    auto AST = ParseExpression();

    if (AST) {
      AST->codegen();
    } else {
      // Error Recovery: Skip token and try again
      getNextToken();
    }
  }

  // Only after the loop ends (EOF), we add the return statement.
  Builder->CreateRet(ConstantInt::get(*TheContext, APInt(32, 0)));
  // Verify and Print
  verifyFunction(*TheFunction);

  std::error_code EC;
  raw_fd_ostream OutFile("output.ll", EC);
  if (!EC) {
    TheModule->print(OutFile, nullptr);
    std::cout << "Successfully compiled to output.ll\n";
  } else {
    std::cerr << "Error writing file: " << EC.message() << "\n";
  }

  return 0;
}
