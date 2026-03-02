#include "Builtins.h"
#include "Codegen.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "SymbolTable.h"
#include "Types.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace llvm;

// CLI helpers
struct CompilerOptions {
  std::string InputFile;
  std::string OutputName = "program"; // default output binary name
  bool EmitLLVM = false;              // --emit-llvm: write .ll instead
};

static CompilerOptions parseArgs(int argc, char **argv) {
  CompilerOptions opts;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--emit-llvm") {
      opts.EmitLLVM = true;
    } else if (arg == "-o" && i + 1 < argc) {
      opts.OutputName = argv[++i];
    } else if (arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      std::exit(1);
    } else {
      opts.InputFile = arg;
    }
  }

  if (opts.InputFile.empty()) {
    std::cerr << "Usage: turf <filename.tr> [-o output] [--emit-llvm]\n";
    std::exit(1);
  }

  return opts;
}

// Emit native object file via LLVM TargetMachine
static int emitObjectFile(Module &M, const std::string &ObjFilename) {
  // Initialise all targets so we can produce code for the host machine.
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllAsmPrinters();

  Triple TargetTriple(sys::getDefaultTargetTriple());

  // LLVM 20+ changed setTargetTriple to accept Triple; older versions take
  // StringRef. arch is so much better :((
#if LLVM_VERSION_MAJOR >= 20
  M.setTargetTriple(TargetTriple);
#else
  M.setTargetTriple(TargetTriple.str());
#endif

  std::string Error;
  auto Target = TargetRegistry::lookupTarget(TargetTriple.str(), Error);
  if (!Target) {
    std::cerr << "Error: " << Error << "\n";
    return 1;
  }

  auto CPU = "generic";
  auto Features = "";
  TargetOptions opt;

  // LLVM 20+ also changed createTargetMachine to accept Triple.
#if LLVM_VERSION_MAJOR >= 20
  auto TM = Target->createTargetMachine(TargetTriple, CPU, Features, opt,
                                        Reloc::PIC_);
#else
  auto TM = Target->createTargetMachine(TargetTriple.str(), CPU, Features, opt,
                                        Reloc::PIC_);
#endif

  M.setDataLayout(TM->createDataLayout());

  std::error_code EC;
  raw_fd_ostream dest(ObjFilename, EC, sys::fs::OF_None);
  if (EC) {
    std::cerr << "Could not open file: " << EC.message() << "\n";
    return 1;
  }

  legacy::PassManager pass;
  auto FileType = CodeGenFileType::ObjectFile;
  if (TM->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
    std::cerr << "Target machine can't emit a file of this type\n";
    return 1;
  }

  pass.run(M);
  dest.flush();
  return 0;
}

// Invoke system linker
static int linkObject(const std::string &ObjFile,
                      const std::string &OutputName) {
  std::string Cmd;

#ifdef _WIN32
  // On Windows, try gcc (MinGW) first
  Cmd = "gcc \"" + ObjFile + "\" -o \"" + OutputName + ".exe\" -lm";
#else
  // On Unix, use cc (always available on dev machines).
  Cmd = "cc \"" + ObjFile + "\" -o \"" + OutputName + "\" -lm";
#endif

  int Ret = std::system(Cmd.c_str());
  if (Ret != 0) {
    std::cerr << "Linking failed (exit code " << Ret << ")\n";
    std::cerr << "Command was: " << Cmd << "\n";
    return 1;
  }
  return 0;
}

// main
int main(int argc, char **argv) {
  auto opts = parseArgs(argc, argv);

  SourceFile.open(opts.InputFile);
  if (!SourceFile.is_open()) {
    std::cerr << "Error: Could not open file " << opts.InputFile << "\n";
    return 1;
  }

  // Read file into memory for error printing
  std::ifstream File(opts.InputFile);
  std::string Line;
  while (std::getline(File, Line)) {
    SourceLines.push_back(Line);
  }

  RegisterBuiltins(); // Populate builtin registry + inject keywords into Lexer
  InitializeModule(); // Initialize LLVM Context, Module, Builder
  InitializeSymbolTable(); // Initialize semantic symbol table
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

  // Pre-pass: hoist 'fn' function prototypes so that forward calls work.
  // We perform a second full parse of the file, i.e. only processing
  // FuncDefExprAST nodes, which registers the LLVM prototype on first
  // codegen call (before the body exists).
  {
    std::ifstream PrePassFile(opts.InputFile);
    if (PrePassFile.is_open()) {
      // Swap the global SourceFile for the pre-pass stream
      SourceFile.close();
      SourceFile.open(opts.InputFile);
      CurLoc = {1, 0};

      // Reset the lexer token stream
      CurTok = 0;
      getNextToken();

      while (CurTok != TOK_EOF) {
        if (CurTok == ';') {
          getNextToken();
          continue;
        }
        if (CurTok == TOK_FN) {
          // Parse and codegen prototype-only (body is parsed but we only
          // want to register the LLVM Function* in the module)
          auto AST = ParseExpression();
          if (AST) {
            LintExpression(AST.get()); // Lint function definitions
            AST->codegen(); // creates prototype + body the first time
          }
        } else {
          getNextToken(); // skip non-fn tokens
        }
      }

      // Re-open for the main pass
      SourceFile.close();
      SourceFile.open(opts.InputFile);
      CurLoc = {1, 0};
      CurTok = 0;
    }
  }

  // Main pass: re-enter the entry block and codegen everything.
  Builder->SetInsertPoint(Entry);

  // pre-pass 1 ends
  // Reset the lexer's internal state so that it starts from the beginning of
  // the file
  resetLexer();

  // Bootstrap the lexer for the main pass
  getNextToken();

  // Main Loop
  while (true) {
    if (CurTok == TOK_EOF)
      break; // Stop if end of file

    if (CurTok == ';') {
      getNextToken(); // Skip semicolons
      continue;
    }

    // fn definitions were already fully processed in the pre-pass.
    // Parse and discard them here to keep the token stream in sync.
    if (CurTok == TOK_FN) {
      ParseExpression(); // parse and drop; body was already codegen'd
      continue;
    }

    // Parse the next expression
    auto AST = ParseExpression();

    if (AST) {
      LintExpression(AST.get()); // Lint top-level statements before codegen
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

  // Output stage: either emit LLVM IR text or compile to native binary
  if (opts.EmitLLVM) {
    // Legacy mode: write human-readable .ll file
    std::string LLFile = opts.OutputName + ".ll";
    std::error_code EC;
    raw_fd_ostream OutFile(LLFile, EC);
    if (!EC) {
      TheModule->print(OutFile, nullptr);
      std::cout << "Successfully compiled to " << LLFile << "\n";
    } else {
      std::cerr << "Error writing file: " << EC.message() << "\n";
      return 1;
    }
  } else {
    // New default: emit native object code and link
    std::string ObjFile = opts.OutputName + ".o";

    if (emitObjectFile(*TheModule, ObjFile) != 0)
      return 1;

    std::cout << "Compiled to " << ObjFile << "\n";

    if (linkObject(ObjFile, opts.OutputName) != 0)
      return 1;

    // Clean up intermediate object file
    std::remove(ObjFile.c_str());

#ifdef _WIN32
    std::cout << "Successfully compiled to " << opts.OutputName << ".exe\n";
#else
    std::cout << "Successfully compiled to " << opts.OutputName << "\n";
#endif
  }

  return 0;
}
