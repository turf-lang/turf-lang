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
#include <set>
#include <vector>
#ifdef _WIN32
#include <process.h> // _spawnvp, _P_WAIT
#else
#include <sys/wait.h> // waitpid, WIFEXITED, WEXITSTATUS
#include <unistd.h>   // fork, execvp
#endif

using namespace llvm;

std::string CompilerExecutablePath;

// CLI helpers
struct CompilerOptions {
  std::string InputFile;
  std::string OutputName;             // default derived from input later
  bool EmitLLVM = false;              // --emit-llvm: write .ll instead
  bool AIEnabled = false;             // --smart: enable SLM AI suggestions
  bool DumpAST = false;               // --dump-ast: print AST via ASTPrinter
  bool RunResolver = false;           // --resolve: run Resolver pass (shadow mode, Issue #4)
  bool DumpResolverSymbols = false;   // --dump-resolver-symbols: print symbol table after resolution
  bool DumpResolved = false;          // --dump-resolved: run Resolver then print annotated AST (Issue #5)
  bool RunTypeChecker = false;        // --typecheck: run TypeChecker pass (shadow mode, Issue #7)
  bool DumpTypechecked = false;       // --dump-typechecked: run TypeChecker then print type-annotated AST
  bool DumpModules = false;           // --dump-modules: print full module-system introspection
  std::vector<std::string> LinkLibs;  // --link <name>: extra libraries to pass to the linker as -l<name>
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
    } else if (arg == "--dump-modules") {
      // Implies --resolve: the module system's state (Modules_,
      // TypeOwningModule_, FlatImportedFunctions_) only exists once Pass 1
      // hoisting has run, same as every other --dump-* flag.
      opts.RunResolver = true;
      opts.DumpModules = true;
    } else if (arg == "-o" && i + 1 < argc) {
      opts.OutputName = argv[++i];
    } else if (arg == "--link" && i + 1 < argc) {
      opts.LinkLibs.push_back(argv[++i]);
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
           "[--dump-resolved] [--typecheck] [--dump-typechecked] "
           "[--dump-modules] [--link <libname>]\n";
    std::exit(1);
  }

  if (opts.InputFile.size() < 3 || opts.InputFile.substr(opts.InputFile.size() - 3) != ".tr") {
    std::cerr << "Error: Input file must have a '.tr' extension (got: " << opts.InputFile << ").\n";
    std::exit(1);
  }

  if (opts.OutputName.empty()) {
    size_t lastSlash = opts.InputFile.find_last_of("/\\");
    std::string base = (lastSlash == std::string::npos) ? opts.InputFile : opts.InputFile.substr(lastSlash + 1);
    opts.OutputName = base.substr(0, base.size() - 3);
    if (opts.OutputName.empty()) {
      opts.OutputName = "program"; // fallback
    }
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

// g_RequiredLibs is populated by ExternFuncDeclExprAST::codegen() for every
// 'extern from "libname" fn' declaration encountered during code generation.
extern std::set<std::string> g_RequiredLibs;

// Invoke system linker
//
// Runs the linker directly via fork+exec (POSIX) / _spawnvp (Windows) with
// an explicit argument array, NOT std::system() on a concatenated shell
// command string. ObjFile and OutputName both ultimately derive from `-o`
// and the input filename on the command line -- arbitrary, unescaped user
// input. The previous std::system("cc \"" + ObjFile + "\" -o \"" +
// OutputName + "\" ...") only quoted against spaces; a filename containing
// a `"`, `` ` ``, `$(...)`, or `;` would break out of the quoting and run as
// shell syntax (e.g. `turf prog.tr -o 'x"; rm -rf ~; echo "'`). Passing
// argv entries directly to exec*/_spawnvp means the OS never re-parses them
// through a shell, so no argument can ever be interpreted as shell syntax
// regardless of its contents.
static int linkObject(const std::string &ObjFile,
                      const std::string &OutputName,
                      const std::vector<std::string> &ExtraLibs) {
  // Build -l flags from both sources:
  //   1. Libraries declared via 'extern from "libname" fn' in source code
  //   2. Libraries passed via --link on the command line
  std::set<std::string> AllLibs(g_RequiredLibs);
  for (const auto &L : ExtraLibs) AllLibs.insert(L);

#ifdef _WIN32
  std::string FinalOutput = OutputName + ".exe";
  std::vector<std::string> Args = {"gcc", ObjFile, "-o", FinalOutput, "-lm"};
#else
  std::string FinalOutput = OutputName;
  std::vector<std::string> Args = {"cc", ObjFile, "-o", FinalOutput, "-lm"};
#endif
  for (const auto &L : AllLibs) Args.push_back("-l" + L);

  // Human-readable form for error reporting only -- never passed to a shell.
  std::string CmdForDisplay;
  for (const auto &A : Args) CmdForDisplay += A + " ";

#ifdef _WIN32
  std::vector<const char *> Argv;
  for (const auto &A : Args) Argv.push_back(A.c_str());
  Argv.push_back(nullptr);
  intptr_t Ret = _spawnvp(_P_WAIT, Argv[0], Argv.data());
  if (Ret != 0) {
    std::cerr << "Linking failed (exit code " << Ret << ")\n";
    std::cerr << "Command was: " << CmdForDisplay << "\n";
    return 1;
  }
#else
  std::vector<char *> Argv;
  for (auto &A : Args) Argv.push_back(const_cast<char *>(A.c_str()));
  Argv.push_back(nullptr);

  pid_t Pid = fork();
  if (Pid < 0) {
    std::cerr << "Linking failed: fork() error\n";
    return 1;
  }
  if (Pid == 0) {
    execvp(Argv[0], Argv.data());
    // execvp only returns on failure.
    std::cerr << "Linking failed: could not exec '" << Argv[0] << "'\n";
    _exit(127);
  }
  int Status = 0;
  if (waitpid(Pid, &Status, 0) < 0) {
    std::cerr << "Linking failed: waitpid() error\n";
    return 1;
  }
  if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0) {
    std::cerr << "Linking failed (exit code "
              << (WIFEXITED(Status) ? WEXITSTATUS(Status) : -1) << ")\n";
    std::cerr << "Command was: " << CmdForDisplay << "\n";
    return 1;
  }
#endif
  return 0;
}

/**
 * @brief Skip forward past a malformed top-level construct (a `fn`/`struct`/
 *        `enum`/`extern`/`import` whose own parse just failed) until reaching
 *        the next safe top-level boundary -- i.e. until g_braceDepth (see
 *        Lexer.h/.cpp, maintained globally by getNextToken() as it consumes
 *        each token) returns to 0.
 *
 * All eight recovery loops in the parsing loops below (pre-pass and main
 * pass, `fn`/`struct`/... branch and `import` branch, longjmp path and
 * plain-nullptr-return path) used to do a flat, depth-blind scan: skip
 * tokens one at a time until hitting ANY of a fixed set of boundary tokens
 * (`fn`, `struct`, ..., `;`). That works fine when the failure happens
 * genuinely at the top level, but a failure partway through a function
 * BODY (e.g. a typo'd keyword like `iff` for `if`) leaves the token stream
 * mid-way through that body, with its own already-open, not-yet-closed `{`
 * that the naive scan has no way to know about -- it stops at the FIRST
 * `;` or `fn`/`struct`/... it happens to see, which is very often still
 * inside the malformed body, not a real top-level boundary. The leftover
 * garbage after that point (the rest of the body, its closing `}`, etc.)
 * then gets misread as new top-level content, producing a second,
 * completely unrelated, confusing error stacked on top of the real one.
 *
 * This function itself went through exactly that bug once already: the
 * first version assumed the failure was always exactly ONE brace level
 * deep (hardcoding "Depth = 1", i.e. only the failed construct's own `{`
 * is open) -- true for a failure directly inside a function body, but not
 * for one nested further, e.g. inside a `compare` block's `otherwise` arm
 * within that body (two levels: the function's `{` AND the compare's `{`).
 * A second stray `otherwise:` there produced a "duplicate/must be last"
 * error from a totally different, unrelated code path than expected --
 * the arm's own statement-parsing loop tried to parse the second
 * `otherwise:` as the START OF A NEW STATEMENT (it has no way to know
 * "otherwise" means "the compare block's next arm header, stop here"), hit
 * ParsePrimaryBase()'s generic "unexpected token" fallback, and returned a
 * soft (non-longjmp) nullptr failure -- which the OLD Depth=1 recovery then
 * mis-resynchronized one brace level too shallow (stopping at the
 * COMPARE block's closing `}`, not the function's), leaving `return 0; }`
 * dangling as apparent new top-level content and producing a second,
 * unrelated "Statements are not allowed outside of a function" error
 * stacked on top of the real one -- the exact class of bug this function
 * exists to prevent, just at a nesting depth the original fix didn't
 * anticipate. g_braceDepth's global, always-accurate tracking (rather than
 * a fixed assumption re-derived locally at each call site) fixes this for
 * ANY nesting depth, not just one level.
 */
static void SkipToNextTopLevelBoundary() {
  // Always make forward progress, even if g_braceDepth is already 0 (e.g. a
  // failure that happened before any '{' was ever opened, such as a
  // malformed function signature) -- otherwise a caller whose next parse
  // attempt immediately fails on the same token would loop forever.
  do {
    if (CurTok == TOK_EOF) return;
    getNextToken();
  } while (g_braceDepth > 0);
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

  SourceFile->open(opts.InputFile);
  if (!SourceFile->is_open()) {
    std::cerr << "Error: Could not open file " << opts.InputFile << "\n";
    return 1;
  }
  SourceFilename = opts.InputFile;
  MainSourceFilename = opts.InputFile;

  // Read file into memory for error printing
  std::ifstream File(opts.InputFile);
  std::string Line;
  while (std::getline(File, Line)) {
    SourceLines.push_back(Line);
  }
  // Also seed the persistent cross-pass cache -- see AllSourceLines' doc
  // comment in Lexer.h for why this can't just be SourceLines alone.
  AllSourceLines[SourceFilename] = SourceLines;

  // Builtin registry removed
  InitializeModule(); // Initialize LLVM Context, Module, Builder
  // Clear any previous call graph state
  ClearCallGraph();
  ClearGlobalCFGs();
  InitializeSymbolTable(); // Initialize semantic symbol table
  InitializePrecedence();

  // Pre-pass: hoist 'fn' function prototypes so that forward calls work.
  // We perform a second full parse of the file, i.e. only processing
  // FuncDefExprAST nodes, which registers the LLVM prototype on first
  // codegen call (before the body exists).
  //
  // There is deliberately no prelude auto-import here. lib/std/prelude.tr
  // used to always `import io;` for every compile, which meant bare
  // `printline(...)` (etc.) silently compiled with zero import statements
  // anywhere in user code -- io's functions leaked into every program's
  // module registry regardless of whether that program actually asked for
  // them. Every module a program uses must now be imported explicitly by
  // that program itself.

  // Swap the global SourceFile for the pre-pass stream
  SourceFile->close();
      SourceFile->open(opts.InputFile);
      CurLoc = {1, 0};

      // Reset the lexer token stream
      CurTok = 0;
      getNextToken();

      // ── Pre-pass sub-pass 1: Parse ALL top-level declarations ─────────────
      // We collect every fn/struct/import node BEFORE codegenning any body.
      // This is required so the Resolver can pre-declare all functions before
      // we attempt to codegen any call sites (Issue #9: Codegen reads
      // ResolvedCallTarget blindly, so Resolver must run first).
      std::vector<std::unique_ptr<ExprAST>> PrePassASTs;

      // Prelude auto-import (STRING_UFCS_OPERATOR_CHAR_RFC.md §7 step 6):
      // inject `import int; import double; ...` as if they were the literal
      // first lines of the TOP-LEVEL file only (not library modules
      // importing each other -- see ParsePreludeImports()'s doc comment).
      // These go through the exact same ParseModuleImportByName() path a
      // real import does -- not a separate always-on registry, which is
      // what caused the original prelude.tr's two bugs (see this file's
      // other prelude comment, a few dozen lines below, for that history).
      // Deliberately NOT passed to RecordParsedTopLevelAST() -- that tracks
      // the USER's own top-level declarations for sandbox/SLM diffing, and
      // these aren't part of the user's source.
      for (auto &PreludeAST : ParsePreludeImports(CurLoc))
        PrePassASTs.push_back(std::move(PreludeAST));

      while (CurTok != TOK_EOF) {
        if (CurTok == ';') { getNextToken(); continue; }
        if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM ||
            CurTok == TOK_AT || CurTok == TOK_EXTERN || CurTok == TOK_EXPORT ||
            CurTok == TOK_OPERATOR || CurTok == TOK_ERROR) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            auto AST = ParseExpression();
            if (AST) {
              if (CurTok == ';') {
                LogErrorAt(CurLoc, "Unexpected ';'");
              }
              RecordParsedTopLevelAST(AST.get());
              // NOTE: no LintExpression() call here (deliberately -- see the
              // "Phase Separation: Lint All" comment near FnASTs' declaration
              // for the full story). This AST is about to flow through
              // Resolver -> TypeChecker -> full codegen (sub-pass 3b) in this
              // same pre-pass, and FuncDefExprAST::codegen() already lints
              // its own body once that's done. Linting here too used to
              // triple-warn on every function: once here (before the AST was
              // even resolved), once via that embedded codegen-time lint,
              // and a third time later when the main pass re-parses the same
              // source into a fresh (but structurally identical) AST and
              // lints THAT via the FnASTs loop -- three near-identical
              // warnings for one line of source. Found while adding a
              // regression test for Lint.cpp's self-comparison wording fix.
              PrePassASTs.push_back(std::move(AST));
            } else {
              // ParseExpression() already reported a real error and
              // returned nullptr NORMALLY (no longjmp) -- see
              // SkipToNextTopLevelBoundary()'s doc comment for why a
              // depth-aware skip is needed here instead of a flat scan.
              SkipToNextTopLevelBoundary();
            }
          } else {
            SkipToNextTopLevelBoundary();
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
              // See the matching NOTE above (fn/struct/enum/@/extern branch):
              // no LintExpression() call here either, for the same reason.
              PrePassASTs.push_back(std::move(AST));
            } else {
              SkipToNextTopLevelBoundary();
            }
          } else {
            SkipToNextTopLevelBoundary();
          }
          g_recoverActive = false;
        } else {
          getNextToken();
        }
      }

      // ── Pre-pass sub-pass 2a: Resolve ─────────────────────────────────────
      // Run the Resolver over everything parsed so far (user fns only -- no
      // prelude to resolve first any more).
      // This populates ResolvedCandidates on every FuncCallExprAST and sets
      // ResolvedCallTarget for unambiguous single-overload calls.
      Resolver PreR;
      {
        bool PreResolverClean = true;
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
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
      bool PreTCClean = true;
      {
        TypeChecker PreTC(PreR.symbolTable());
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
          PreTC.checkAll(PrePassASTs);
        } else {
          PreTCClean = false;
        }
        // Errors in the pre-pass TC are informational only in the sense that
        // the main-pass TC (below, after the whole pre-pass finishes) is what
        // actually PRINTS them with full diagnostics and is authoritative for
        // whether compilation ultimately fails -- but PreTCClean still gates
        // sub-pass 3b's body codegen just below. Without that gate, a type
        // error the pre-pass TC correctly caught (e.g. `x.foo()` where
        // x: int -- "cannot call methods on a non-struct type") used to be
        // silently discarded here, and sub-pass 3b would codegen the same
        // broken body anyway, reaching a Codegen-side
        // `llvm::report_fatal_error()` ("this is a compiler bug") and
        // aborting the whole process -- BEFORE the main-pass TC ever got a
        // chance to run and print the real, correct diagnostic. The prior
        // codegen (structs, sub-pass 2c) and prototypes (sub-pass 3a) stay
        // ungated: they're structural/signature-level and don't execute
        // arbitrary expression codegen, so they carry effectively none of
        // this crash risk, and skipping them too would serve no purpose
        // since the main-pass TC exits before anything downstream of them
        // runs anyway once PreTCClean is false.
        g_recoverActive = false;
      }

      // ── Pre-pass sub-pass 2c: Fully codegen all structs FIRST ──────────────
      // Unlike functions, StructDeclExprAST::codegen() has no g_protoOnlyPass
      // split -- one call does the whole job: create-or-reuse an opaque
      // llvm::StructType stub, then immediately populate its body (field
      // types) in place. That "immediately" is the problem this pass exists
      // to fix. Structs used to only be codegen'd in sub-pass 3b below,
      // interleaved with everything else in TEXTUAL file order -- so a
      // struct field access GEP built while compiling a method that's
      // declared BEFORE its owning struct (`fn User.grow()` above `struct
      // User {...}`, a supported forward-reference -- see
      // tests/06_structs/19_method_before_struct.tr) got built against the
      // struct type while it was still opaque (zero fields). The opaque
      // llvm::StructType* object identity is correctly preserved and later
      // filled in via setBody() -- so by the time the module finishes,
      // GlobalSymbolTable and LLVMTypeCache both point at a fully-populated
      // struct -- but the GEP instruction itself was already permanently
      // constructed while that type had no known fields, and its embedded
      // "indexed type" information doesn't get retroactively fixed up by a
      // later setBody() call on the same object. The result was IR that
      // `verifyModule()` (see below) correctly rejects: "GEP is not of right
      // type for indices!". This was completely invisible before that
      // verification call was wired up correctly (see its own doc comment).
      //
      // Fix: give structs the same treatment functions already get for
      // exactly this reason (forward calls) -- process ALL of them, fully,
      // before ANY function prototype or body touches a struct type. Structs
      // are excluded from both loops below since they're entirely done here.
      for (auto &AST : PrePassASTs) {
        if (dynamic_cast<StructDeclExprAST *>(AST.get())) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            AST->codegen();
          }
          g_recoverActive = false;
        }
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
            dynamic_cast<EnumDeclExprAST *>(AST.get()) ||
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
      // Structs are skipped: sub-pass 2c above already fully codegen'd them,
      // and StructDeclExprAST::codegen() is NOT safe to call a second time
      // (its opaque-stub reuse check keys on `isOpaque()`, which is false
      // once setBody() has run -- a second call would create a second,
      // distinct llvm::StructType and silently corrupt LLVMTypeCache).
      //
      // Gated on PreTCClean: if the pre-pass TypeChecker (sub-pass 2b) found
      // ANY error anywhere in the file, don't run body codegen at all here --
      // see PreTCClean's doc comment above for why. The main-pass TC (after
      // this whole pre-pass block) will re-find the same error and exit
      // cleanly before its own codegen ever runs, so skipping this is always
      // safe when !PreTCClean, never a missed compile.
      if (PreTCClean) {
        for (auto &AST : PrePassASTs) {
          if (dynamic_cast<StructDeclExprAST *>(AST.get()))
            continue;
          // `export immut` constants have no real LLVM storage at all --
          // Resolver Pass 1 (hoistExportedConstant) already extracted their
          // literal value into the symbol table, and every reference site
          // emits that literal directly (see VariableExprAST/
          // ScopeAccessExprAST's ResolvedConstant field). Calling
          // VarDeclExprAST::codegen() here would run it with no enclosing
          // function/basic-block context (true top level), corrupting
          // whatever LLVM function is being codegenned adjacent to it in
          // iteration order -- confirmed live via a real crash ("Basic
          // Block ... does not have terminator") in an unrelated function.
          if (dynamic_cast<VarDeclExprAST *>(AST.get()))
            continue;
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
      }

  // Re-open for the main pass
  SourceFile->close();
  SourceFile->open(opts.InputFile);
  CurLoc = {1, 0};
  CurTok = 0;


  // Main pass: re-enter the entry block and codegen everything.
  // Builder->SetInsertPoint(Entry);

  // pre-pass 1 ends
  // Reset the lexer's internal state so that it starts from the beginning of
  // the file
  ClearParsedTopLevelASTs();
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
  // already fully codegenned -- and linted (see FuncDefExprAST::codegen()'s
  // own LintExpression(Body.get()) call) -- in the pre-pass. We parse them
  // here purely to sync the token stream, NOT to codegen or lint them again:
  // this is a fresh re-parse producing structurally-identical-but-distinct
  // AST nodes from the pre-pass's copy, so re-linting here would just repeat
  // every warning a second time for the same source line.
  std::vector<std::unique_ptr<ExprAST>> TopLevelASTs; // non-fn: lint + codegen
  std::vector<std::unique_ptr<ExprAST>> FnASTs;       // fn:     kept for Resolver/TypeChecker re-validation only

  // Prelude auto-import, main-pass side -- mirrors the pre-pass injection
  // above exactly (same ParsePreludeImports() call, same rationale). Pushed
  // into FnASTs, matching how a real `import X;` is categorized in this
  // pass (already codegenned in the pre-pass; re-parsed here purely for
  // Resolver/TypeChecker re-validation, not re-codegen).
  for (auto &PreludeAST : ParsePreludeImports(CurLoc))
    FnASTs.push_back(std::move(PreludeAST));

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
    // in the pre-pass. TOK_EXPORT (export immut NAME: TYPE = LITERAL;) is
    // included for the same reason -- it's a genuine top-level declaration,
    // not a statement, and needs the exact same hoist-then-lint treatment.
    if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM ||
        CurTok == TOK_AT || CurTok == TOK_EXTERN || CurTok == TOK_EXPORT ||
        CurTok == TOK_OPERATOR || CurTok == TOK_ERROR) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        auto AST = ParseExpression();
        if (AST) {
          if (CurTok == ';') {
            LogErrorAt(CurLoc, "Unexpected ';'");
          }
          RecordParsedTopLevelAST(AST.get());
          FnASTs.push_back(std::move(AST)); // lint only — not re-codegenned
        } else {
          // ParseExpression() already reported a real error (e.g. a typo'd
          // keyword deep inside the function body, like `iff` for `if`) and
          // returned nullptr NORMALLY -- see SkipToNextTopLevelBoundary()'s
          // doc comment for why a depth-aware skip is needed here instead
          // of a flat token scan.
          SkipToNextTopLevelBoundary();
        }
      } else {
        SkipToNextTopLevelBoundary();
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
        } else {
          SkipToNextTopLevelBoundary();
        }
      } else {
        SkipToNextTopLevelBoundary();
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
            !dynamic_cast<EnumDeclExprAST *>(AST.get()) &&
            !dynamic_cast<ModuleImportExprAST *>(AST.get()) &&
            !dynamic_cast<ExternFuncDeclExprAST *>(AST.get())) {
          std::cerr
              << "Error: Statements are not allowed outside of a function. "
                 "Please define 'fn main(): int' and move this code inside.\n";
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
  // Only TopLevelASTs (non-fn statements) are linted here. FnASTs are
  // deliberately skipped: they're a fresh re-parse of the same fn bodies
  // the pre-pass already codegenned (and, via FuncDefExprAST::codegen()'s
  // own embedded LintExpression(Body.get()) call, already linted) --
  // linting them again here would just repeat every warning for the exact
  // same source line a second time. See the FnASTs declaration's comment
  // above for the full story.
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

    if (opts.DumpModules)
      R.symbolTable().dumpModules(std::cerr);

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
  size_t SavedDepth = GlobalSymbolTable ? GlobalSymbolTable->GetDepth() : 0;
  for (auto &AST : TopLevelASTs) {
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

  // Build cross-function call graph and print summary (run before flushing diagnostics)
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
    std::cerr << "Error: No entry point found. Please define 'fn main(): int' "
                 "in your file.\n";
    return 1;
  }

  // Verify the generated IR for the WHOLE module (not just main -- a bug in
  // any function's codegen can produce broken IR that crashes or misbehaves
  // deep inside the optimizer/backend below with no indication of where the
  // real problem is).
  //
  // This used to be `verifyFunction(*MainFunc);` with the return value
  // discarded and no output stream passed -- verifyFunction() only prints a
  // diagnostic if given a non-null raw_ostream*, and only actually means
  // anything if the caller checks its bool return ("true" = broken). With
  // neither done, this call was a complete no-op: it could never abort
  // compilation and never printed anything, regardless of whether the
  // generated IR was valid. A broken-IR bug in Codegen.cpp -- for any
  // function, not just main -- would silently sail through this checkpoint
  // and only surface later as a confusing LLVM optimizer/backend crash with
  // no TurfLang-level context, rather than a clear internal-compiler-error
  // message right here, before any of that runs.
  {
    std::string VerifyErrors;
    raw_string_ostream VerifyOS(VerifyErrors);
    if (verifyModule(*TheModule, &VerifyOS)) {
      std::cerr << "Internal compiler error: generated LLVM IR is invalid.\n"
                   "This is a compiler bug, not a mistake in your Turf code "
                   "-- please report it.\n\n"
                << VerifyErrors;
      return 1;
    }
  }

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

    if (linkObject(ObjFile, opts.OutputName, opts.LinkLibs) != 0)
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
