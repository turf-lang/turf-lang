/**
 * @file main.cpp
 * @brief TurfLang Compiler entry point.
 * @details Parses command-line arguments, orchestrates the lexing, parsing,
 *          and code generation passes, and writes the compiled binary or IR
 * output.
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

#include "Builtins.h"
#include "Codegen.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "CFG.h"
#include "Resolver.h"
#include "SLMEngine.h"
#include "SymbolTable.h"
#include "Types.h"
#include "ASTPrinter.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LegacyPassManager.h"
#include "CallGraph.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"

using namespace llvm;

std::string CompilerExecutablePath;

// CLI helpers
struct CompilerOptions {
  std::string InputFile;
  std::string OutputName = "program"; // default output binary name
  bool EmitLLVM = false;              // --emit-llvm: write .ll instead
  bool AIEnabled = false;             // --smart: enable SLM AI suggestions
  bool DumpAST = false;               // --dump-ast: print AST via ASTPrinter
  bool RunResolver = false;           // --resolve: run Resolver pass (shadow mode, Issue #4)
  bool DumpResolverSymbols = false;   // --dump-resolver-symbols: print symbol table after resolution
};

static CompilerOptions parseArgs(int argc, char **argv) {
  CompilerOptions opts;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--emit-llvm") {
      opts.EmitLLVM = true;
    } else if (arg == "--smart") {
      opts.AIEnabled = true;
    } else if (arg == "--dump-ast") {
      opts.DumpAST = true;
    } else if (arg == "--resolve") {
      opts.RunResolver = true;
    } else if (arg == "--dump-resolver-symbols") {
      opts.RunResolver = true;
      opts.DumpResolverSymbols = true;
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
        << "Usage: turf <filename.tr> [-o output] [--emit-llvm] [--smart] [--dump-ast] [--resolve] [--dump-resolver-symbols]\n";
    std::exit(1);
  }

  if (opts.InputFile.size() < 3 || opts.InputFile.substr(opts.InputFile.size() - 3) != ".tr") {
    std::cerr << "Error: Input file must have a '.tr' extension (got: " << opts.InputFile << ").\n";
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
/**
 * @brief The compiler entry point.
 *
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line argument strings.
 * @return int Exit code (0 on success, non-zero on compilation failure).
 */
int main(int argc, char **argv) {
  CompilerExecutablePath = llvm::sys::fs::getMainExecutable(argv[0], (void *)(intptr_t)main);

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

  // Builtin registry removed
  InitializeModule(); // Initialize LLVM Context, Module, Builder
  // Clear any previous call graph state
  ClearCallGraph();
  InitializeSymbolTable(); // Initialize semantic symbol table
  InitializePrecedence();

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

      // Auto-import prelude using relative path from compiler
      llvm::SmallString<128> PreludePath(CompilerExecutablePath);
      llvm::sys::path::remove_filename(PreludePath);
      llvm::sys::path::append(PreludePath, "..", "lib", "std", "prelude.tr");

      if (auto PreludeAST = ParseFile(PreludePath.str().str())) {
        RecordParsedTopLevelAST(PreludeAST.get());
        PreludeAST->codegen();
      }

      while (CurTok != TOK_EOF) {
        if (CurTok == ';') {
          getNextToken();
          continue;
        }
        if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_IMPORT ||
            CurTok == TOK_AT) {
          // Save scope depth so we can restore it if codegen aborts via
          // longjmp. Without this, a function that references an unknown
          // variable causes EnterScope() to be called but ExitScope() to be
          // skipped (longjmp bypasses all destructors and normal returns),
          // leaving the scope stack dirty and the global scope's HasEarlyExit
          // flag stale.
          size_t SavedDepth =
              GlobalSymbolTable ? GlobalSymbolTable->GetDepth() : 0;

          // Parse and codegen prototype-only. Guard with setjmp so a bad
          // fn body doesn't kill the whole pre-pass.
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            auto AST = ParseExpression();
            if (AST) {
              if (RequiresSemicolon(AST.get())) {
                if (CurTok != ';') {
                  LogErrorAt(CurLoc, "Expected ';' after statement");
                } else {
                  getNextToken(); // eat ';'
                }
              }
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
            // Skip to next fn, struct or EOF
            while (CurTok != TOK_FN && CurTok != TOK_STRUCT &&
                   CurTok != TOK_IMPORT && CurTok != TOK_AT &&
                   CurTok != TOK_EOF)
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
  // Builder->SetInsertPoint(Entry);

  // pre-pass 1 ends
  // Reset the lexer's internal state so that it starts from the beginning of
  // the file
  resetLexer();

  // Bootstrap the lexer for the main pass
  getNextToken();

  // Phase Separation: Parse All
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
    if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_AT) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        auto AST = ParseExpression();
        if (AST) {
          RecordParsedTopLevelAST(AST.get());
          FnASTs.push_back(std::move(AST)); // lint only — not re-codegenned
        }
      } else {
        while (CurTok != TOK_FN && CurTok != TOK_STRUCT && CurTok != TOK_AT &&
               CurTok != ';' && CurTok != TOK_EOF)
          getNextToken();
      }
      g_recoverActive = false;
      continue;
    }

    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      auto AST = ParseExpression();
      if (AST) {
        if (RequiresSemicolon(AST.get())) {
          if (CurTok != ';') {
            LogErrorAt(CurLoc, "Expected ';' after statement");
          } else {
            getNextToken(); // eat ';'
          }
        }
        if (!dynamic_cast<FuncDefExprAST *>(AST.get()) &&
            !dynamic_cast<StructDeclExprAST *>(AST.get()) &&
            !dynamic_cast<ModuleImportExprAST *>(AST.get())) {
          std::cerr
              << "Error: Statements are not allowed outside of a function. "
                 "Please define 'fn int main()' and move this code inside.\n";
          DiagnosticEngine::flushAll();
          exit(1);
        }
        RecordParsedTopLevelAST(AST.get());
        TopLevelASTs.push_back(std::move(AST));
      } else {
        getNextToken();
      }
    } else {
      // longjmp landed here: skip to next statement boundary.
      while (CurTok != ';' && CurTok != TOK_EOF && CurTok != TOK_FN &&
             CurTok != TOK_STRUCT && CurTok != TOK_TYPE_INT &&
             CurTok != TOK_TYPE_DOUBLE && CurTok != TOK_TYPE_BOOL &&
             CurTok != TOK_TYPE_STRING && CurTok != TOK_IF &&
             CurTok != TOK_WHILE && CurTok != TOK_FOR && CurTok != TOK_RETURN) {
        getNextToken();
      }
    }
    g_recoverActive = false;
  }

  // Phase Separation: Lint All
  // Lint fn definitions first (body-only checks), then top-level statements.
  for (auto &AST : FnASTs)
    LintExpression(AST.get());
  for (auto &AST : TopLevelASTs)
    LintExpression(AST.get());

  // --dump-ast: run the ASTPrinter visitor over every top-level node.
  // This is the Issue #1 acceptance-criteria demo: a concrete ASTVisitor
  // subclass successfully traverses and prints the entire parsed AST tree.
  if (opts.DumpAST) {
    ASTPrinter Printer;
    std::cout << "\n=== AST Dump (via ASTPrinter visitor) ===\n\n";
    std::cout << "--- Functions & Structs ---\n";
    for (auto &AST : FnASTs)
      Printer.print(AST.get());
    std::cout << "--- Top-Level Statements ---\n";
    for (auto &AST : TopLevelASTs)
      Printer.print(AST.get());
    std::cout << "\n=== End AST Dump ===\n\n";
  }

  // --resolve: Run the Resolver pass in shadow mode (Issue #4).
  // The Resolver performs name resolution over the fully-parsed AST and emits
  // rich "Unknown Variable" / "Duplicate Declaration" errors if it finds
  // problems.  This runs BEFORE Codegen so that resolver errors are reported
  // with accurate source locations and Damerau-Levenshtein suggestions.
  //
  // Shadow mode: if no resolver error is found, Codegen still runs normally
  // (the resolver output is not yet fed to Codegen — that is Issue #5+).
  if (opts.RunResolver) {
    std::cerr << "\n[Resolver] Running name-resolution pass (Issue #4 — shadow mode)...\n";
    Resolver R;
    bool ResolverClean = true;

    // Use the same recovery mechanism as the rest of the compiler so that a
    // resolver error doesn't crash the process — it is reported and we can
    // decide whether to continue.
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      R.resolveAll(FnASTs);
      R.resolveAll(TopLevelASTs);
    } else {
      // A diagnostic was raised and longjmp'd here.  The DiagnosticEngine
      // already has the error buffered; we just note that resolution failed.
      ResolverClean = false;
    }
    g_recoverActive = false;

    if (ResolverClean) {
      std::cerr << "[Resolver] ✓  All names resolved successfully. "
                   "Scope depth on exit: "
                << R.symbolTable().currentDepth() << "\n";
                
      if (opts.DumpResolverSymbols) {
        R.symbolTable().dump(std::cerr);
      }
    } else {
      std::cerr << "[Resolver] ✗  Name resolution failed (see error above).\n";
      // Flush diagnostics and abort — a resolver error means Codegen would
      // crash or produce wrong IR anyway.
      DiagnosticEngine::flushAll();
      return 1;
    }
  }

  // Phase Separation: Codegen All
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

  // Build cross-function call graph and print summary (run before flushing diagnostics)
  std::cerr << "[Main] GlobalCFGs size before CallGraph: " << GlobalCFGs.size() << "\n";
  CallGraphPass CGP;
  CGP.runGlobal();

  // Flush all collected diagnostics (sorted by line).  If any errors were
  // recorded, stop here: do not attempt further IR generation or linking.
  DiagnosticEngine::flushAll();
  if (DiagnosticEngine::hasErrors())
    return 1;

  // Verify that an explicit 'main' function was defined in the primary module
  Function *MainFunc = TheModule->getFunction("main");
  if (!MainFunc || MainFunc->empty()) {
    std::cerr << "Error: No entry point found. Please define 'fn int main()' "
                 "in your file.\n";
    return 1;
  }

  // Verify the generated IR
  verifyFunction(*MainFunc);

  // Phase 5: LLVM Optimization Passes
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
