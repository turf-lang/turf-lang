#include "Builtins.h"
#include "Codegen.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "SLMEngine.h"
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
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"
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
  bool AIEnabled = false;             // --smart: enable SLM AI suggestions
};

static CompilerOptions parseArgs(int argc, char **argv) {
  CompilerOptions opts;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--emit-llvm") {
      opts.EmitLLVM = true;
    } else if (arg == "--smart") {
      opts.AIEnabled = true;
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
    std::cerr
        << "Usage: turf <filename.tr> [-o output] [--emit-llvm] [--smart]\n";
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
  struct SLMCleanup {
    ~SLMCleanup() { SLM::SLMEngine::getInstance().release(); }
  } cleanup;

  auto opts = parseArgs(argc, argv);

  // Set the global AI flag so DiagnosticEngine knows whether to invoke the SLM.
  g_AIEnabled = opts.AIEnabled;

  SourceFile.open(opts.InputFile);
  if (!SourceFile.is_open()) {
    std::cerr << "Error: Could not open file " << opts.InputFile << "\n";
    return 1;
  }
  SourceFilename = opts.InputFile;

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
  ClearParsedTopLevelASTs();

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
          // Save scope depth so we can restore it if codegen aborts via longjmp.
          // Without this, a function that references an unknown variable causes
          // EnterScope() to be called but ExitScope() to be skipped (longjmp
          // bypasses all destructors and normal returns), leaving the scope
          // stack dirty and the global scope's HasEarlyExit flag stale.
          size_t SavedDepth = GlobalSymbolTable ? GlobalSymbolTable->GetDepth() : 0;

          // Parse and codegen prototype-only. Guard with setjmp so a bad
          // fn body doesn't kill the whole pre-pass.
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            auto AST = ParseExpression();
            if (AST) {
              RecordParsedTopLevelAST(AST.get());
              LintExpression(AST.get());
              AST->codegen();
            }
          } else {
            // longjmp landed here: diagnostic already recorded.
            // Unwind any scopes that were opened during the partial compile,
            // then clear any stale early-exit flag so subsequent top-level
            // declarations are not falsely flagged as unreachable.
            if (GlobalSymbolTable) {
              GlobalSymbolTable->ResetToDepth(SavedDepth);
              GlobalSymbolTable->ResetEarlyExitForCurrentScope();
            }
            // Skip to next fn or EOF
            while (CurTok != TOK_FN && CurTok != TOK_EOF)
              getNextToken();
          }
          g_recoverActive = false;
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

  // ── Phase Separation: Parse All ──────────────────────────────────────────
  // Fully parse every top-level statement into an in-memory AST list BEFORE
  // calling codegen on any of them. This prevents orphaned LLVM BasicBlocks
  // and corrupted module state that previously occurred when a semantic error
  // (e.g. type mismatch) triggered a longjmp mid-codegen.
  //
  // fn definitions are kept in a SEPARATE list (FnASTs) because they were
  // already fully codegenned in the pre-pass. We parse them here purely to
  // sync the token stream and run the lint pass — not to codegen them again.
  std::vector<std::unique_ptr<ExprAST>> TopLevelASTs; // non-fn: lint + codegen
  std::vector<std::unique_ptr<ExprAST>> FnASTs;       // fn:     lint only

  while (true) {
    if (CurTok == TOK_EOF)
      break;

    if (CurTok == ';') {
      getNextToken();
      continue;
    }

    // fn definitions were already fully codegenned in the pre-pass.
    // Parse them here to keep the token stream in sync and to lint them,
    // but do NOT push them into TopLevelASTs (that would re-codegen them).
    if (CurTok == TOK_FN) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        auto AST = ParseExpression();
        if (AST) {
          RecordParsedTopLevelAST(AST.get());
          FnASTs.push_back(std::move(AST)); // lint only — not re-codegenned
        }
      } else {
        while (CurTok != TOK_FN && CurTok != ';' && CurTok != TOK_EOF)
          getNextToken();
      }
      g_recoverActive = false;
      continue;
    }

    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      auto AST = ParseExpression();
      if (AST) {
        RecordParsedTopLevelAST(AST.get());
        TopLevelASTs.push_back(std::move(AST));
      } else {
        getNextToken();
      }
    } else {
      // longjmp landed here: skip to next statement boundary.
      while (CurTok != ';' && CurTok != TOK_EOF && CurTok != TOK_FN &&
             CurTok != TOK_TYPE_INT && CurTok != TOK_TYPE_DOUBLE &&
             CurTok != TOK_TYPE_BOOL && CurTok != TOK_TYPE_STRING &&
             CurTok != TOK_IF && CurTok != TOK_WHILE && CurTok != TOK_FOR &&
             CurTok != TOK_RETURN) {
        getNextToken();
      }
    }
    g_recoverActive = false;
  }

  // ── Phase Separation: Lint All ────────────────────────────────────────────
  // Lint fn definitions first (body-only checks), then top-level statements.
  for (auto &AST : FnASTs)
    LintExpression(AST.get());
  for (auto &AST : TopLevelASTs)
    LintExpression(AST.get());

  // ── Phase Separation: Codegen All ────────────────────────────────────────
  // Only non-fn top-level statements need codegen here. fn bodies were already
  // compiled in the pre-pass; codegenning them again would duplicate them.
  for (auto &AST : TopLevelASTs) {
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      AST->codegen();
    } else {
      if (GlobalSymbolTable) {
        GlobalSymbolTable->ResetToDepth(1);
        GlobalSymbolTable->ResetEarlyExitForCurrentScope();
      }
    }
    g_recoverActive = false;
  }

  // Flush all collected diagnostics (sorted by line).  If any errors were
  // recorded, stop here: do not attempt further IR generation or linking.
  DiagnosticEngine::flushAll();
  if (DiagnosticEngine::hasErrors())
    return 1;

  // Only after all codegens are done, add the return statement to main.
  Builder->CreateRet(ConstantInt::get(*TheContext, APInt(32, 0)));
  // Verify the generated IR
  verifyFunction(*TheFunction);

  // ── Phase 5: LLVM Optimization Passes ─────────────────────────────────────
  // Run LLVM's standard optimization pipeline over every function in the
  // module. This turns our Alloca/Load/Store pattern into register-based SSA
  // (mem2reg), folds constants, eliminates dead code, and simplifies the CFG,
  // reaching near-C-level performance without any extra compiler work.
  {
    legacy::FunctionPassManager FPM(TheModule.get());
    // Promote memory to registers (eliminates alloca/load/store overhead)
    FPM.add(createPromoteMemoryToRegisterPass());
    // Fold redundant instructions (x + 0 => x, etc.)
    FPM.add(createInstructionCombiningPass());
    // Reassociate expressions for better constant folding
    FPM.add(createReassociatePass());
    // Eliminate common sub-expressions
    FPM.add(createGVNPass());
    // Simplify the control-flow graph (remove unreachable blocks, etc.)
    FPM.add(createCFGSimplificationPass());
    FPM.doInitialization();
    for (auto &F : *TheModule)
      FPM.run(F);
  }

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
