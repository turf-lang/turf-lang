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
#include "TypeChecker.h"
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
  bool DumpResolved = false;          // --dump-resolved: run Resolver then print annotated AST (Issue #5)
  bool RunTypeChecker = false;        // --typecheck: run TypeChecker pass (shadow mode, Issue #7)
  bool DumpTypechecked = false;       // --dump-typechecked: run TypeChecker then print type-annotated AST
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
    } else if (arg == "--dump-resolved") {
      // Implies --resolve: run the Resolver, then print the annotated AST
      // showing which nodes have a green ResolvedSymbol badge (Issue #5 demo).
      opts.RunResolver = true;
      opts.DumpResolved = true;
    } else if (arg == "--typecheck") {
      // Implies --resolve: TypeChecker requires a resolved symbol table.
      opts.RunResolver = true;
      opts.RunTypeChecker = true;
    } else if (arg == "--dump-typechecked") {
      // Implies --resolve + --typecheck: run both passes, then print the
      // type-annotated AST showing each node's inferred TurfType (Issue #7).
      opts.RunResolver = true;
      opts.RunTypeChecker = true;
      opts.DumpTypechecked = true;
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
        << "Usage: turf <filename.tr> [-o output] [--emit-llvm] [--smart] "
           "[--dump-ast] [--resolve] [--dump-resolver-symbols] "
           "[--dump-resolved] [--typecheck] [--dump-typechecked]\n";
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
  std::cout << "DEBUG CompilerExecutablePath: " << CompilerExecutablePath << "\n";

  struct SLMCleanup {
    ~SLMCleanup() { SLM::SLMEngine::getInstance().release(); }
  } cleanup;

  auto opts = parseArgs(argc, argv);

  // Set the global AI flag so DiagnosticEngine knows whether to invoke the SLM.
  g_AIEnabled = opts.AIEnabled;

  SourceFile->open(opts.InputFile);
  if (!SourceFile->is_open()) {
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
  // PreludeAST is kept alive so the Resolver can walk it later and register
  // the `io` module (and any other prelude imports) in its module registry
  // before it processes user code.
  std::unique_ptr<ExprAST> PreludeAST;

  {
    if (true) {
      // Swap the global SourceFile for the pre-pass stream
      SourceFile->close();
      SourceFile->open(opts.InputFile);
      CurLoc = {1, 0};

      // Reset the lexer token stream
      CurTok = 0;
      getNextToken();

      // Auto-import prelude using relative path from compiler
      llvm::SmallString<128> PreludePath(CompilerExecutablePath);
      llvm::sys::path::remove_filename(PreludePath);
      llvm::sys::path::append(PreludePath, "..", "lib", "std", "prelude.tr");

      if (auto Parsed = ParseFile(PreludePath.str().str())) {
        RecordParsedTopLevelAST(Parsed.get());
        Parsed->codegen();
        PreludeAST = std::move(Parsed); // keep alive for Resolver
      }

      // ── Pre-pass sub-pass 1: Parse ALL top-level declarations ─────────────
      // We collect every fn/struct/import node BEFORE codegenning any body.
      // This is required so the Resolver can pre-declare all functions before
      // we attempt to codegen any call sites (Issue #9: Codegen reads
      // ResolvedCallTarget blindly, so Resolver must run first).
      std::vector<std::unique_ptr<ExprAST>> PrePassASTs;

      while (CurTok != TOK_EOF) {
        if (CurTok == ';') { getNextToken(); continue; }
        if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_AT ||
            CurTok == TOK_EXTERN) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            auto AST = ParseExpression();
            if (AST) {
              if (CurTok == ';') {
                LogErrorAt(CurLoc, "Unexpected ';'");
              }
              RecordParsedTopLevelAST(AST.get());
              LintExpression(AST.get());
              PrePassASTs.push_back(std::move(AST));
            }
          } else {
            while (CurTok != TOK_FN && CurTok != TOK_STRUCT &&
                   CurTok != TOK_IMPORT && CurTok != TOK_AT &&
                   CurTok != TOK_EXTERN && CurTok != TOK_EOF)
              getNextToken();
          }
          g_recoverActive = false;
        } else if (CurTok == TOK_IMPORT) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            auto AST = ParseExpression();
            if (AST) {
              if (CurTok != ';') {
                LogErrorAt(CurLoc, "Expected ';' after statement");
              } else {
                getNextToken(); // eat ';'
              }
              RecordParsedTopLevelAST(AST.get());
              LintExpression(AST.get());
              PrePassASTs.push_back(std::move(AST));
            }
          } else {
            while (CurTok != TOK_FN && CurTok != TOK_STRUCT &&
                   CurTok != TOK_IMPORT && CurTok != TOK_AT &&
                   CurTok != TOK_EOF)
              getNextToken();
          }
          g_recoverActive = false;
        } else {
          getNextToken();
        }
      }

      // ── Pre-pass sub-pass 2a: Resolve ─────────────────────────────────────
      // Run the Resolver over everything parsed so far (prelude + user fns).
      // This populates ResolvedCandidates on every FuncCallExprAST and sets
      // ResolvedCallTarget for unambiguous single-overload calls.
      Resolver PreR;
      {
        bool PreResolverClean = true;
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
          if (PreludeAST)
            PreR.resolve(PreludeAST.get());
          PreR.resolveAll(PrePassASTs);
        } else {
          PreResolverClean = false;
        }
        g_recoverActive = false;
        if (!PreResolverClean) {
          DiagnosticEngine::flushAll();
          return 1;
        }
      }

      // ── Pre-pass sub-pass 2b: TypeCheck ───────────────────────────────────
      // Run the TypeChecker over the pre-pass ASTs to narrow overloaded calls
      // (e.g. io.printline has 4 overloads; TypeChecker picks the right one
      // and sets ResolvedCallTarget so sub-pass 3b codegen reads it blindly).
      {
        TypeChecker PreTC(PreR.symbolTable());
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
          if (PreludeAST)
            PreTC.check(PreludeAST.get());
          PreTC.checkAll(PrePassASTs);
        }
        // Errors in the pre-pass TC are informational only here — the main-pass
        // TC will re-check and report them with full diagnostics.
        g_recoverActive = false;
      }

      // ── Pre-pass sub-pass 3a: Emit all LLVM function prototypes ────────────
      // Before compiling any body, declare every function in LLVM so that
      // forward calls (main calling helper() defined later) work correctly.
      // FuncDefExprAST::codegen() creates the prototype on the first call when
      // TheModule->getFunction(mangledName) returns null.
      // We set a global flag to stop after prototype creation.
      g_protoOnlyPass = true;
      for (auto &AST : PrePassASTs) {
        if (dynamic_cast<FuncDefExprAST *>(AST.get()) ||
            dynamic_cast<ExternFuncDeclExprAST *>(AST.get()) ||
            dynamic_cast<ModuleImportExprAST *>(AST.get())) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            AST->codegen();
          }
          g_recoverActive = false;
        }
      }
      g_protoOnlyPass = false;

      // ── Pre-pass sub-pass 3b: Full codegen (bodies) ───────────────────────
      // All prototypes exist now — codegen each function body safely.
      for (auto &AST : PrePassASTs) {
        size_t SavedDepth = GlobalSymbolTable ? GlobalSymbolTable->GetDepth() : 0;
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
          AST->codegen();
        } else {
          if (GlobalSymbolTable) {
            GlobalSymbolTable->ResetToDepth(SavedDepth);
            GlobalSymbolTable->ResetEarlyExitForCurrentScope();
          }
        }
        g_recoverActive = false;
      }

      // Re-open for the main pass
      SourceFile->close();
      SourceFile->open(opts.InputFile);
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

    // the pre-pass.  Parse them here to keep the token stream in sync and to
    // lint/resolve them, but do NOT push them into TopLevelASTs (that would
    // re-codegen them).  TOK_IMPORT is included here because `import` nodes
    // are top-level declarations handled by the pre-pass, exactly like `fn`.
    // TOK_EXTERN is included because extern fn declarations are also emitted
    // in the pre-pass.
    if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_AT ||
        CurTok == TOK_EXTERN) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        auto AST = ParseExpression();
        if (AST) {
          if (CurTok == ';') {
            LogErrorAt(CurLoc, "Unexpected ';'");
          }
          RecordParsedTopLevelAST(AST.get());
          FnASTs.push_back(std::move(AST)); // lint only — not re-codegenned
        }
      } else {
        while (CurTok != TOK_FN && CurTok != TOK_STRUCT && CurTok != TOK_AT &&
               CurTok != TOK_EXTERN && CurTok != ';' && CurTok != TOK_EOF)
          getNextToken();
      }
      g_recoverActive = false;
      continue;
    }

    if (CurTok == TOK_IMPORT) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        auto AST = ParseExpression();
        if (AST) {
          if (CurTok != ';') {
            LogErrorAt(CurLoc, "Expected ';' after statement");
          } else {
            getNextToken(); // eat ';'
          }
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
        if (CurTok != ';') {
          LogErrorAt(CurLoc, "Expected ';' after statement");
        } else {
          getNextToken(); // eat ';'
        }
        if (!dynamic_cast<FuncDefExprAST *>(AST.get()) &&
            !dynamic_cast<StructDeclExprAST *>(AST.get()) &&
            !dynamic_cast<ModuleImportExprAST *>(AST.get()) &&
            !dynamic_cast<ExternFuncDeclExprAST *>(AST.get())) {
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
             CurTok != TOK_STRUCT && CurTok != TOK_MUTABLE &&
             CurTok != TOK_IMMUTABLE && CurTok != TOK_IF &&
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

  // ── Resolver pass (mandatory, Issue #9) ─────────────────────────────────
  // The Resolver runs unconditionally on every compilation.  It annotates the
  // AST with resolved symbols, candidate overloads, and arity-checked call
  // targets so that Codegen can read them blindly without re-doing any name
  // lookup or arity checking.
  //
  // If resolution fails the process aborts here — Codegen must never run on
  // an unresolved AST.
  Resolver R;
  {
    bool ResolverClean = true;
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      // Resolve the prelude first so that module imports (e.g. `import io`)
      // are registered in the Resolver's module registry before any user
      // code is walked.
      if (PreludeAST)
        R.resolve(PreludeAST.get());
      R.resolveAll(FnASTs);
      R.resolveAll(TopLevelASTs);
    } else {
      ResolverClean = false;
    }
    g_recoverActive = false;

    if (!ResolverClean) {
      DiagnosticEngine::flushAll();
      return 1;
    }
  }

  // --resolve: verbose output for presentation / debugging.
  // The Resolver already ran above; this block just prints its results.
  if (opts.RunResolver) {
    std::cerr << "\n[Resolver] Running name-resolution pass (Issue #4)...\n";
    std::cerr << "[Resolver] \u2713  All names resolved successfully. "
                 "Scope depth on exit: "
              << R.symbolTable().currentDepth() << "\n";

    if (opts.DumpResolverSymbols)
      R.symbolTable().dump(std::cerr);

    if (opts.DumpResolved) {
      ASTPrinter Printer;
      std::cout << "\n";
      std::cout << "\033[1m=== Resolved AST Dump (Issue #5) ===\033[0m\n";
      std::cout << "  \033[32m[resolved \u2713]\033[0m = Resolver attached a VariableSymbol pointer\n";
      std::cout << "  \033[31m[NOT resolved \u2717]\033[0m = pointer is null (structural node or leaf)\n";
      std::cout << "\n";
      std::cout << "\033[1m--- Functions ---\033[0m\n";
      for (auto &AST : FnASTs)
        Printer.print(AST.get());
      std::cout << "\n\033[1m--- Top-Level Statements ---\033[0m\n";
      for (auto &AST : TopLevelASTs)
        Printer.print(AST.get());
      std::cout << "\n\033[1m=== End Resolved AST Dump ===\033[0m\n\n";
    }
  }



  // ── TypeChecker pass (mandatory, Issue #11) ──────────────────────────────
  // The TypeChecker runs unconditionally on every compilation, immediately
  // after the Resolver.  It infers and validates types on every AST node,
  // populating type annotations that Codegen reads blindly.
  //
  // If the TypeChecker finds any errors, compilation aborts here — Codegen
  // must never run on a type-incorrect AST.
  //
  // Pipeline: Parser → Resolver → TypeChecker → Codegen
  //
  // --typecheck and --dump-typechecked still control the debug banner and the
  // annotated-AST dump, but the TC pass itself is not optional.
  TypeChecker TC(R.symbolTable());
  {
    bool TCClean = true;
    g_recoverActive = true;
    if (setjmp(g_recoverJmp) == 0) {
      if (PreludeAST)
        TC.check(PreludeAST.get());
      TC.checkAll(FnASTs);
      TC.checkAll(TopLevelASTs);
    } else {
      TCClean = false;
    }
    g_recoverActive = false;

    if (opts.RunTypeChecker) {
      if (TCClean)
        std::cerr << "\n[TypeChecker] Running type-inference pass (Issue #7)...\n"
                  << "[TypeChecker] \u2713  Type inference completed without errors.\n";
      else
        std::cerr << "\n[TypeChecker] Running type-inference pass (Issue #7)...\n"
                  << "[TypeChecker] \u2717  Type checking failed (see error above).\n";
    }

    if (opts.DumpTypechecked && TCClean) {
      ASTPrinter Printer;
      Printer.setTypeChecker(&TC);
      std::cout << "\n";
      std::cout << "\033[1m=== Type-Checked AST Dump (Issue #7) ===\033[0m\n";
      std::cout << "  \033[32m[resolved \u2713]\033[0m = symbol resolved\n";
      std::cout << "  \033[36m[module: X \u2713]\033[0m = module qualifier\n";
      std::cout << "  \033[35m[: type]\033[0m = TypeChecker inferred type\n";
      std::cout << "\n";
      std::cout << "\033[1m--- Functions ---\033[0m\n";
      for (auto &AST : FnASTs)
        Printer.print(AST.get());
      std::cout << "\n\033[1m--- Top-Level Statements ---\033[0m\n";
      for (auto &AST : TopLevelASTs)
        Printer.print(AST.get());
      std::cout << "\n\033[1m=== End Type-Checked AST Dump ===\033[0m\n\n";
    }

    if (!TCClean) {
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
