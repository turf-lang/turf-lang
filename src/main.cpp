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
#include "Colors.h"
#include "ErrorCategory.h"
#include "ErrorCodes.h"
#include "Formatter.h"
#include "Lexer.h"
#include "Lint.h"
#include "Parser.h"
#include "CFG.h"
#include "Monomorphize.h" // TYPE-002 (S11b): --dump-instantiations
#include "Resolver.h"
#include "TypeChecker.h"
#include "SLMEngine.h"
#include "SymbolTable.h"
#include "Types.h"
#include "ASTPrinter.h"
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
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include <set>
#include <sstream>
#include <vector>
#ifdef _WIN32
#include <process.h> // _spawnvp, _P_WAIT
#else
#include <sys/wait.h> // waitpid, WIFEXITED, WEXITSTATUS
#include <unistd.h>   // fork, execvp
#endif

using namespace llvm;

std::string CompilerExecutablePath;

// OPT-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §17.2): the five levels
// the CLI's `-O` flag selects. Os is not "O1 through O3 plus one more" --
// it is `PassBuilder::buildPerModuleDefaultPipeline(OptimizationLevel::Os)`,
// a genuinely different pipeline tuned for size over throughput -- so this
// is its own enum, not an int a caller could accidentally compare/clamp as
// if the levels were totally ordered by "more optimization."
enum class OptLevelKind { O0, O1, O2, O3, Os };

// CLI helpers
struct CompilerOptions {
  std::string InputFile;
  std::string OutputName;             // default derived from input later
  bool EmitLLVM = false;              // --emit-llvm: write .ll instead
  bool AIEnabled = false;             // --smart: enable SLM AI suggestions
  std::string SmartFixOutPath;        // --smart-write-fix=<path>: write the first validated --smart fix's full patched source here (SLM-001 follow-up)
  bool AllowUnsafeModules = false;    // --allow-unsafe-modules: LANG-002, honor `unsafe module;` outside the install root
  bool DumpAST = false;               // --dump-ast: print AST via ASTPrinter
  bool RunResolver = false;           // --resolve: run Resolver pass (shadow mode, Issue #4)
  bool DumpResolverSymbols = false;   // --dump-resolver-symbols: print symbol table after resolution
  bool DumpResolved = false;          // --dump-resolved: run Resolver then print annotated AST (Issue #5)
  bool RunTypeChecker = false;        // --typecheck: run TypeChecker pass (shadow mode, Issue #7)
  bool DumpTypechecked = false;       // --dump-typechecked: run TypeChecker then print type-annotated AST
  bool DumpModules = false;           // --dump-modules: print full module-system introspection
  bool DumpInstantiations = false;    // --dump-instantiations: TYPE-002 (S11b) report-only census
  bool Format = false;                // --format: print the formatted source to stdout and exit
  bool FormatWrite = false;           // --write (with --format): rewrite the file in place instead
  std::vector<std::string> LinkLibs;  // --link <name>: extra libraries to pass to the linker as -l<name>
  // OPT-001: `-O2` is the FROZEN default (FREEZE.md §6.10/§17.2) so a user
  // who never passes `-O` at all sees the exact optimization behavior this
  // milestone shipped as its baseline, not the weakest level.
  OptLevelKind OptLevel = OptLevelKind::O2;      // -O0/-O1/-O2/-O3/-Os
  std::string TargetTriple;           // --target=<triple>; empty = host
  std::string MCPU;                   // -mcpu=<cpu>; empty = "generic" (unchanged default)
  bool DebugInfo = false;             // -g: emit DWARF debug info (TOOL-001)
};

// `turf --explain E0NN` is the HUMAN-facing side of error-code
// documentation -- read straight in a terminal, so it's rendered with
// actual colors/structure instead of a flat file dump (see
// printHumanExplainDoc() below). It is NOT what a future
// `--explain-to-slm` would use: that flag (not implemented yet) would read
// the parallel docs/slm/codes/ tree instead -- plain, uncolored grounding
// text meant for the model, not a person. Keeping the two trees (and this
// comment) separate is deliberate -- see ErrorCodes::getHumanCodeDocPath()/
// getSlmCodeDocPath()'s own doc comment in ErrorCodes.h for why.
//
// This mode takes no input .tr file, so it's handled before parseArgs()
// (which requires and validates one) rather than folded into
// CompilerOptions. Doc resolution is a three-tier fallback, most-specific
// first:
//   1. A per-code human doc, "docs/errors/codes/E0NN.txt" (ErrorCodes::
//      getHumanCodeDocPath()) -- opt-in, only written for codes where the
//      coarser tiers below conflate genuinely distinct mistakes (e.g. E068
//      "unknown struct field" vs E054 "unknown struct method").
//   2. A construct-specific doc derived from the code's own short
//      Description run through the SAME keyword classifier
//      (extractContextKeyword()) the live --smart pipeline already uses on
//      real error messages -- e.g. "Struct method not found" contains
//      "struct" and routes to docs/slm/structs.txt, matching what a live
//      compile of that same error class would show.
//   3. The plain per-category doc (SLM::getCategoryDocPath() with no
//      keyword) -- the original, coarsest fallback.
// Tiers 2 and 3 both read from docs/slm/ (there's no per-category human
// tree -- only per-code docs are split by audience so far) but are still
// rendered through the same human colorizer either way.
// See issue #241 Track D.

// Prints one line of body prose, highlighting `backtick`-quoted spans --
// the docs already write inline identifiers/snippets this way ("the
// `password` field") -- in bold green and stripping the backticks
// themselves, rather than printing them literally. Bold (vs. a code
// block's plain green) so a reader can tell "this is a snippet worth
// noticing inline" from "this is a full sample to read" at a glance.
// Deliberately minimal -- balanced backtick pairs only, no nesting, no
// other markdown -- so doc authors don't need to learn real markdown.
static void printBodyLine(const std::string &Line) {
  size_t Pos = 0;
  while (Pos < Line.size()) {
    size_t Open = Line.find('`', Pos);
    if (Open == std::string::npos) {
      std::cout << Line.substr(Pos);
      return;
    }
    size_t Close = Line.find('`', Open + 1);
    if (Close == std::string::npos) {
      // Unmatched backtick -- print the remainder literally rather than
      // silently eating it while searching for a partner that isn't there.
      std::cout << Line.substr(Pos);
      return;
    }
    std::cout << Line.substr(Pos, Open - Pos) << Colors::BOLD
              << Colors::BRIGHT_GREEN
              << Line.substr(Open + 1, Close - Open - 1) << Colors::RESET;
    Pos = Close + 1;
  }
}

// Renders a doc file (docs/errors/codes/*.txt, or a docs/slm/*.txt
// category/construct fallback -- both already follow the same loose shape)
// with real terminal color instead of a flat rdbuf() dump: the first
// non-blank line is treated as a title, any line indented 4+ spaces is
// treated as a code sample (the exact convention every docs/slm/*.txt file
// already uses for its own examples), and everything else is body prose
// (which additionally gets `backtick` spans highlighted -- see
// printBodyLine() above). Deliberately simple -- no markdown beyond that
// one convention, no nested spans -- so doc authors don't need to learn a
// new syntax; it works with the shape these files already have.
static void printHumanExplainDoc(std::istream &Doc) {
  std::string Line;
  bool PrintedTitle = false;

  while (std::getline(Doc, Line)) {
    if (!PrintedTitle) {
      if (Line.empty())
        continue; // skip leading blank lines before the title
      std::cout << Colors::BOLD << Colors::BRIGHT_CYAN << Line
                << Colors::RESET << "\n";
      PrintedTitle = true;
      continue;
    }

    size_t FirstNonSpace = Line.find_first_not_of(' ');
    bool IsCode = FirstNonSpace != std::string::npos && FirstNonSpace >= 4;
    if (IsCode) {
      std::cout << Colors::BRIGHT_GREEN << Line << Colors::RESET << "\n";
    } else {
      printBodyLine(Line);
      std::cout << "\n";
    }
  }
}

static void handleExplainFlagIfPresent(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) != "--explain")
      continue;

    if (i + 1 >= argc) {
      std::cerr << "Usage: turf --explain E0NN\n";
      std::exit(1);
    }

    std::string Code = argv[i + 1];
    const ErrorCodeEntry *Entry = ErrorCodes::lookupByCode(Code);
    if (!Entry) {
      std::cerr << "Unknown error code: " << Code
                << "\nRun `turf --explain E0NN` with a code shown in an "
                   "error's \"[E0NN]\" header, e.g. `turf --explain E001`.\n";
      std::exit(1);
    }

    std::cout << Colors::BOLD << Colors::BRIGHT_CYAN << Entry->Code
              << Colors::RESET << "  " << Entry->ClassName << " ("
              << SLM::getCategoryName(Entry->Category) << ")\n\n";

    std::string DocPath = ErrorCodes::getHumanCodeDocPath(Entry->Code);
    if (DocPath.empty()) {
      std::string Keyword = SLM::extractContextKeyword(Entry->Description);
      DocPath = SLM::getCategoryDocPath(Entry->Category, Keyword);
    }
    std::ifstream Doc(DocPath);
    if (Doc.is_open()) {
      printHumanExplainDoc(Doc);
    } else {
      // Doc file missing on disk (e.g. run from outside the repo root) --
      // fall back to the short auto-generated description rather than
      // printing nothing.
      std::cout << Entry->Description << ".\n";
    }

    std::exit(0);
  }
}

static CompilerOptions parseArgs(int argc, char **argv) {
  CompilerOptions opts;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--emit-llvm") {
      opts.EmitLLVM = true;
    } else if (arg == "--smart") {
      opts.AIEnabled = true;
    } else if (arg.rfind("--smart-write-fix=", 0) == 0) {
      opts.SmartFixOutPath = arg.substr(std::string("--smart-write-fix=").size());
    } else if (arg == "--allow-unsafe-modules") {
      opts.AllowUnsafeModules = true;
    } else if (arg == "--dump-ast") {
      opts.DumpAST = true;
    } else if (arg == "--dump-instantiations") {
      opts.DumpInstantiations = true;
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
    } else if (arg == "--format") {
      opts.Format = true;
    } else if (arg == "--write") {
      opts.FormatWrite = true;
    } else if (arg == "-o" && i + 1 < argc) {
      opts.OutputName = argv[++i];
    } else if (arg == "--link" && i + 1 < argc) {
      opts.LinkLibs.push_back(argv[++i]);
    } else if (arg.rfind("--target=", 0) == 0) {
      opts.TargetTriple = arg.substr(std::string("--target=").size());
    } else if (arg.rfind("-mcpu=", 0) == 0) {
      opts.MCPU = arg.substr(std::string("-mcpu=").size());
    } else if (arg == "-g") {
      opts.DebugInfo = true;
    } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'O') {
      // F-OPT semantics (FREEZE.md §6.10): an unrecognized level is a hard
      // error, never a silent fallback to some default -- a typo like
      // `-O4`/`-Ox` must not quietly compile at `-O2` and let a user believe
      // they got the level they asked for.
      std::string Level = arg.substr(2);
      if (Level == "0") opts.OptLevel = OptLevelKind::O0;
      else if (Level == "1") opts.OptLevel = OptLevelKind::O1;
      else if (Level == "2") opts.OptLevel = OptLevelKind::O2;
      else if (Level == "3") opts.OptLevel = OptLevelKind::O3;
      else if (Level == "s") opts.OptLevel = OptLevelKind::Os;
      else {
        std::cerr << "Error: unknown optimization level '" << arg
                  << "' (expected one of -O0, -O1, -O2, -O3, -Os).\n";
        std::exit(1);
      }
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
           "[--smart-write-fix=<path>] "
           "[--dump-ast] [--resolve] [--dump-resolver-symbols] "
           "[--dump-resolved] [--typecheck] [--dump-typechecked] "
           "[--dump-modules] [--dump-instantiations] [--link <libname>] "
           "[--allow-unsafe-modules] "
           "[-O0|-O1|-O2|-O3|-Os] [-g] [--target=<triple>] [-mcpu=<cpu>]\n"
           "       turf --format [--write] <filename.tr>\n"
           "       turf --explain E0NN\n";
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

// OPT-001: `-O0`/`-O1`/`-O2`/`-O3` map onto LLVM's own `CodeGenOptLevel`
// (backend instruction selection/scheduling), while `-Os` shares `-O2`'s
// backend level -- LLVM has no "smaller code" CodeGenOptLevel; the size
// tuning for `-Os` happens entirely in the IR-level pipeline
// (`OptimizationLevel::Os`, see runOptimizationPipeline() below), matching
// how every other `PassBuilder`-based frontend (e.g. clang) maps it.
static CodeGenOptLevel toCodeGenOptLevel(OptLevelKind Level) {
  switch (Level) {
  case OptLevelKind::O0: return CodeGenOptLevel::None;
  case OptLevelKind::O1: return CodeGenOptLevel::Less;
  case OptLevelKind::O2: return CodeGenOptLevel::Default;
  case OptLevelKind::O3: return CodeGenOptLevel::Aggressive;
  case OptLevelKind::Os: return CodeGenOptLevel::Default;
  }
  llvm_unreachable("unhandled OptLevelKind");
}

// Builds the ONE TargetMachine shared by both the optimization pipeline
// (runOptimizationPipeline(), which needs it to enable target-aware
// transforms) and native object emission (emitObjectFile()). Before
// OPT-001 these were two independent, unsynchronized constructions:
// Phase 5's FunctionPassManager ran with no TargetMachine at all (a
// PassBuilder-based pipeline needs one to run any target-aware pass), and
// emitObjectFile() built its own from a hardcoded host triple and
// CPU="generic", invisible to `--target`/`-mcpu` (§8 OPT-001's own CLI
// additions) if those had been threaded through separately. Constructing it
// once here is what makes `--target`/`-mcpu`/`-O` agree between the two
// phases by construction rather than by two call sites happening to be
// kept in sync by hand.
//
// Returned pointer is intentionally leaked (never `delete`d) -- this
// mirrors the pre-OPT-001 code's own lifetime (a `TargetMachine` built by
// `createTargetMachine` was never freed either), and the process is a
// short-lived CLI invocation that exits immediately after linking. Fixing
// that leak is out of scope for this change (§25's scope discipline).
static TargetMachine *createTurfTargetMachine(const CompilerOptions &opts,
                                              Module &M) {
  // Initialise all targets so we can produce code for the host machine (or,
  // with --target, a different one -- see D20 in §27: a `--target` binary
  // still links the HOST's libturfrt/libc, so cross-compilation is not a
  // supported end-to-end path yet. This flag exists so OPT-001's own IR
  // pipeline and object emission are target-correct; RT-001 is where
  // "produces a binary you can actually run elsewhere" gets decided.).
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllAsmPrinters();

  Triple TargetTriple(opts.TargetTriple.empty() ? sys::getDefaultTargetTriple()
                                                 : opts.TargetTriple);

  // LLVM 20+ changed setTargetTriple to accept Triple; older versions take
  // StringRef. arch is so much better :((
#if LLVM_VERSION_MAJOR >= 20
  M.setTargetTriple(TargetTriple);
#else
  M.setTargetTriple(TargetTriple.str());
#endif

  std::string Error;
  auto Target = TargetRegistry::lookupTarget(TargetTriple, Error);
  if (!Target) {
    std::cerr << "Error: " << Error << "\n";
    return nullptr;
  }

  std::string CPU = opts.MCPU.empty() ? "generic" : opts.MCPU;
  auto Features = "";
  TargetOptions TO;
  CodeGenOptLevel CGOptLevel = toCodeGenOptLevel(opts.OptLevel);

  // LLVM 20+ also changed createTargetMachine to accept Triple.
#if LLVM_VERSION_MAJOR >= 20
  auto TM = Target->createTargetMachine(TargetTriple, CPU, Features, TO,
                                        Reloc::PIC_, std::nullopt, CGOptLevel);
#else
  auto TM = Target->createTargetMachine(TargetTriple.str(), CPU, Features, TO,
                                        Reloc::PIC_, std::nullopt, CGOptLevel);
#endif
  if (TM)
    M.setDataLayout(TM->createDataLayout());
  return TM;
}

// OPT-001 (§17.1/§17.2): the module-level optimization pipeline, replacing
// the pre-existing `legacy::FunctionPassManager` running a fixed 5-pass
// sequence with no module-level view (could not see across functions, so
// nothing could ever be deleted -- an empty `main` shipped 84 unused
// `turf_*` functions, §1.9's measured baseline). `TM` may be null (target
// lookup failed) -- the pipeline still runs target-INDEPENDENT passes in
// that case rather than skipping optimization entirely; emitObjectFile()
// is what actually fails the compile when `TM` is null.
static void runOptimizationPipeline(Module &M, OptLevelKind Level,
                                    TargetMachine *TM) {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassBuilder PB(TM);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // F-OPT-1/F-OPT-2 (FREEZE.md §6.10): internalizing anything the linker
  // still needs breaks the link. TurfLang never separately compiles (every
  // import is re-parsed and re-codegenned into THIS module, §27 D7), so the
  // only symbol anything outside this translation unit can still reach by
  // name is `main` (the C runtime's own entry point, invoked by name, not
  // by an in-module reference GlobalDCE could see) -- every `extern fn` is a
  // DECLARATION with no body to internalize, and every genuinely
  // Turf-defined function this module calls is already a normal in-module
  // use that keeps it live without needing external linkage at all. This is
  // why the "must-preserve" set is exactly {main}, not something derived
  // from `g_RequiredLibs` (§8 OPT-001's own text names that set as the
  // starting point, but g_RequiredLibs is a list of *external C libraries
  // to link against*, e.g. "m", not a list of Turf symbol names -- there is
  // nothing in it to preserve BY). Verified live against
  // showcase/llvm_ffi_selfhost_demo.tr (the most FFI-dense program in the
  // tree, per §8's own risk note) at every level below.
  auto MustPreserveGV = [](const GlobalValue &GV) {
    return GV.getName() == "main";
  };

  switch (Level) {
  case OptLevelKind::O0: {
    // "verify only; AlwaysInliner; nothing else" (§17.2). LLVM's own
    // buildO0DefaultPipeline() IS that pipeline for a coroutine-free,
    // non-instrumented, non-LTO module (which every TurfLang module is) --
    // using the real API here instead of hand-assembling the equivalent
    // means it stays correct if a future LLVM upgrade changes what O0
    // requires, rather than silently drifting from what "O0" means upstream.
    ModulePassManager MPM = PB.buildO0DefaultPipeline(OptimizationLevel::O0);
    MPM.run(M, MAM);
    break;
  }
  case OptLevelKind::O1:
  case OptLevelKind::O2:
  case OptLevelKind::O3:
  case OptLevelKind::Os: {
    // `-O1` used to run a small hand-picked 6-pass subset (the pre-OPT-001
    // 5-pass sequence ported to the new PM, plus EarlyCSE) rather than
    // LLVM's own `default<O1>` pipeline -- a deliberate reading of §17.2's
    // table at the time, but a real gap once measured: LLVM's actual O1
    // pipeline runs ~109 pass invocations (confirmed live via `opt
    // -passes="default<O1>" -print-pipeline-passes`), not ~6. Folded into
    // this same branch on request so `-O1` gets LLVM's real, full O1
    // pipeline like every other non-O0 level, rather than a curated
    // subset nobody was asking for specifically.
    //
    // Internalize FIRST, in its own pass-manager run, strictly before
    // handing off to LLVM's own default pipeline: buildPerModuleDefaultPipeline()
    // runs its own GlobalDCE internally, but GlobalDCE only deletes a
    // definition that is BOTH unreferenced AND not externally visible --
    // without Internalize running first, every `turf_*` function still has
    // external linkage (nothing in Codegen ever marks one internal), so the
    // default pipeline's own DCE would see all 84 dead functions in the
    // empty-main baseline as "externally visible, therefore keep" and
    // delete none of them -- confirmed live while implementing this: the
    // -O2 binary was NOT smaller than -O0's until this ordering was fixed.
    ModulePassManager InternalizeMPM;
    InternalizeMPM.addPass(InternalizePass(MustPreserveGV));
    InternalizeMPM.run(M, MAM);

    OptimizationLevel LLVMLevel = Level == OptLevelKind::O1 ? OptimizationLevel::O1
                                 : Level == OptLevelKind::O2 ? OptimizationLevel::O2
                                 : Level == OptLevelKind::O3 ? OptimizationLevel::O3
                                                              : OptimizationLevel::Os;
    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(LLVMLevel);
    MPM.run(M, MAM);
    break;
  }
  }
}

// Emit native object file via the shared TargetMachine (createTurfTargetMachine()).
static int emitObjectFile(Module &M, TargetMachine &TM,
                          const std::string &ObjFilename) {
  std::error_code EC;
  raw_fd_ostream dest(ObjFilename, EC, sys::fs::OF_None);
  if (EC) {
    std::cerr << "Could not open file: " << EC.message() << "\n";
    return 1;
  }

  legacy::PassManager pass;
  auto FileType = CodeGenFileType::ObjectFile;
  if (TM.addPassesToEmitFile(pass, dest, nullptr, FileType)) {
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

  // RT-001: every compiled Turf binary links libturfrt (F-MEM-4). The
  // search path is anchored at TurfInstallRoot() -- the exact same root
  // `import` already resolves lib/std against (Parser.h/ParserModule.cpp)
  // -- so a plain dev build (`build/turf`, no TURF_HOME set) finds
  // `<repo-root>/lib/libturfrt.a` with zero setup, the same "no setup
  // needed" property lib/std already has. `-L` before `-l` on the linker
  // line is what makes cc/gcc search this exact directory FIRST, ahead of
  // any system default; the flag order below is deliberate.
  std::string TurfLibDir = TurfInstallRoot() + "/lib";

#ifdef _WIN32
  std::string FinalOutput = OutputName + ".exe";
  std::vector<std::string> Args = {"gcc",       ObjFile, "-o", FinalOutput,
                                    "-L" + TurfLibDir, "-lturfrt", "-lm"};
#else
  std::string FinalOutput = OutputName;
  std::vector<std::string> Args = {"cc",        ObjFile, "-o", FinalOutput,
                                    "-L" + TurfLibDir, "-lturfrt", "-lm"};
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
// IsTopLevelBoundaryToken()/SkipToNextTopLevelBoundary() used to live here as
// file-local `static` functions. Moved to ParserCore.cpp (declared in
// Parser.h) so Sandbox.cpp's own recovery-mode parse passes can resync on a
// per-statement basis using the exact same logic, instead of duplicating it
// -- see issue #228/#233. Behavior is unchanged; only the definitions' home
// and linkage moved.

// main
/**
 * @brief The compiler entry point.
 *
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line argument strings.
 * @return int Exit code (0 on success, non-zero on compilation failure).
 */
static int runCompilerMain(int argc, char **argv) {
  CompilerExecutablePath = llvm::sys::fs::getMainExecutable(argv[0], (void *)(intptr_t)runCompilerMain);

  handleExplainFlagIfPresent(argc, argv);

  struct SLMCleanup {
    ~SLMCleanup() { SLM::SLMEngine::getInstance().release(); }
  } cleanup;

  auto opts = parseArgs(argc, argv);

  // Set the global AI flag so DiagnosticEngine knows whether to invoke the SLM.
  g_AIEnabled = opts.AIEnabled;
  g_SmartFixOutPath = opts.SmartFixOutPath;
  // LANG-002: must be set before any parsing happens -- HasRawPointerCapability()
  // (ParserModule.cpp) consults it for every `unsafe module;` pragma outside
  // the install root.
  g_AllowUnsafeModules = opts.AllowUnsafeModules;

  // TOOL-001: must be set before InitializeModule() runs (below) -- that's
  // the one call site that decides whether DBuilder/DebugCU get built at
  // all for this compile.
  g_EmitDebugInfo = opts.DebugInfo;

  // --format needs none of the lex/parse/resolve/typecheck/codegen
  // pipeline below -- just the raw source text -- so it short-circuits
  // before any of that global state (GlobalSymbolTable, LLVM
  // module, ...) gets initialized at all.
  if (opts.Format) {
    std::ifstream FormatInput(opts.InputFile);
    if (!FormatInput) {
      std::cerr << "Error: Could not open file " << opts.InputFile << "\n";
      return 1;
    }
    std::stringstream FormatBuf;
    FormatBuf << FormatInput.rdbuf();
    auto Result = turf::format::format(FormatBuf.str());
    if (!Result.Ok) {
      std::cerr << "Error: Could not format " << opts.InputFile << "\n";
      return 1;
    }
    if (opts.FormatWrite) {
      std::ofstream FormatOutput(opts.InputFile, std::ios::trunc | std::ios::binary);
      FormatOutput << Result.Text;
    } else {
      std::cout << Result.Text;
    }
    return 0;
  }

  if (!OpenSourceFile(opts.InputFile)) {
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
  ClearGlobalCFGs();
  InitializeCompilerGlobals(); // GlobalSymbolTable + BinopPrecedence -- see Parser.h

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
  CloseSourceFile();
      OpenSourceFile(opts.InputFile);
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

      // LANG-002: the entry file's own leading `unsafe module;` capability
      // pragma, if any -- must be checked here, immediately after the token
      // stream reset above, the only point that sees this file's first real
      // token (see ParseLeadingUnsafeModuleDecl()'s own doc comment).
      {
        std::unique_ptr<ExprAST> LeadingUnsafeBlock;
        if (!ParseLeadingUnsafeModuleDecl(LeadingUnsafeBlock) && LeadingUnsafeBlock)
          AppendFlattenedTopLevelDecl(std::move(LeadingUnsafeBlock), PrePassASTs, true);
      }

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

      // OVH-011: if a transitive import inside the prelude just injected
      // above (e.g. `strings`'s own `import list::List;`) already resolved
      // to this exact entry file's own path -- true whenever the file being
      // compiled directly IS the lib/std module some prelude module
      // transitively imports -- skip re-parsing this file's own top-level
      // declarations entirely: they are already fully present, correctly
      // registered under whatever module name reached them. See
      // ClaimEntryFileImportSlot()'s own doc comment (ParserModule.cpp) for
      // the full rationale and why this call must sit exactly here (after
      // prelude injection, before this loop).
      if (!ClaimEntryFileImportSlot(SourceFilename)) {
      while (CurTok != TOK_EOF) {
        if (CurTok == ';') { getNextToken(); continue; }
        if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM ||
            CurTok == TOK_MACHINE ||
            CurTok == TOK_AT || CurTok == TOK_EXTERN || CurTok == TOK_EXPORT ||
            CurTok == TOK_OPERATOR || CurTok == TOK_ERROR || CurTok == TOK_UNSAFE) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            auto AST = ParseExpression();
            if (AST) {
              if (CurTok == ';') {
                LogErrorAt(CurLoc, "Unexpected ';'", SLM::ErrorCategory::SYNTAX_ERROR);
              }
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
              //
              // AppendFlattenedTopLevelDecl() also transparently splices a
              // top-level `unsafe { }` block's children in here instead of
              // the wrapper node itself -- see its doc comment (Parser.h).
              AppendFlattenedTopLevelDecl(std::move(AST), PrePassASTs, true);
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
                LogErrorAt(CurLoc, "Expected ';' after statement", SLM::ErrorCategory::SYNTAX_ERROR);
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
      } // OVH-011: end of "if (!ClaimEntryFileImportSlot(...))"

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
        // TYPE-002 (docs/development/changes/TYPE-002.md, S11b): this is
        // U-4's answer (U-4') in practice -- the real monomorphizer will
        // hook in here too, as sub-pass 2b.5, immediately after this
        // TypeChecker run and before sub-pass 2c's struct codegen just
        // below. For now, report-only: --dump-instantiations feeds a
        // collector during this SAME checkAll() call (TypeChecker's
        // visitFuncCall/visitMethodCall/visitVarDecl record into it when
        // set) and prints the census once it finishes. Zero behaviour
        // change whenever the flag is absent -- MonoCollector is never
        // attached, so every record() call site stays a no-op.
        Monomorphize::InstantiationCollector MonoCollector;
        // TYPE-002 (S12a): always attached now, not just under
        // --dump-instantiations -- the collector's record() calls are what
        // SEED sub-pass 2b.5's drain below; S11b's opt-in gate only made
        // sense while nothing consumed the worklist.
        PreTC.setMonomorphizationCollector(&MonoCollector);
        g_recoverActive = true;
        if (setjmp(g_recoverJmp) == 0) {
          PreTC.checkAll(PrePassASTs);
        } else {
          PreTCClean = false;
        }
        // ── Pre-pass sub-pass 2b.5: Monomorphize ──────────────────────────
        // U-4's answer (U-4', docs/development/changes/TYPE-002.md), acted
        // on here: drains every FreeFunction- and Method-kind instantiation
        // request sub-pass 2b's TypeChecker run seeded, to a fixed point
        // (clone/substitute/re-resolve/re-typecheck/register each one,
        // appending it to PrePassASTs so 2c/3a/3b below codegen it like any
        // other declaration -- see drainFreeFunctionRequests()'s and
        // drainMethodRequests()'s own doc comments, Monomorphize.h). Gated
        // on PreTCClean for the identical reason 2c/3a/3b already are just
        // below: a program the ordinary TypeChecker already rejected has no
        // business being monomorphized either. Struct-kind requests are
        // left recorded, undrained -- S13a.
        //
        // TYPE-002 (S12b): the two drain functions are called in an
        // ALTERNATING loop, not once each in sequence, because they are
        // NOT independent -- re-typechecking a method's clone can discover
        // a nested call to a generic FREE function (or vice versa: a free
        // function constructing/calling into a generic struct's method),
        // and that nested request needs the OTHER drain function to act on
        // it. Each function reports (via its bool return) whether it did
        // any work THIS call; looping while EITHER one still is guarantees
        // the true cross-kind fixed point, not just each function's own
        // internal one. Terminates because every successful drain either
        // marks an entry Drained (never revisited) or exits via
        // GenericInstantiationDepthError (E079) if a truly unbounded chain
        // keeps discovering brand-new work forever.
        if (PreTCClean) {
          g_recoverActive = true;
          if (setjmp(g_recoverJmp) == 0) {
            bool Progress = true;
            while (Progress) {
              Progress = false;
              Progress |= Monomorphize::drainFreeFunctionRequests(PrePassASTs, MonoCollector, PreR);
              Progress |= Monomorphize::drainMethodRequests(PrePassASTs, MonoCollector, PreR);
              // TYPE-002 (S14a): drains the struct-bound FreeFunction-kind
              // entries drainFreeFunctionRequests() deliberately skips
              // (`StructName::new(...)` constructors) -- see its own doc
              // comment (Monomorphize.h/.cpp) for why this needed S13a's
              // per-instantiation struct layout to exist first.
              Progress |= Monomorphize::drainConstructorRequests(PrePassASTs, MonoCollector, PreR);
              // TYPE-002 (S13a): a fourth, independent consumer of the SAME
              // non-destructive collector -- a struct instantiation request
              // can be discovered mid-sweep by any of the other three (a
              // method/constructor clone's own `self` type, or a nested
              // generic call's VarDeclExprAST), and this one, in turn, never
              // discovers a NEW FreeFunction/Method request itself (a struct
              // decl has no body/call sites of its own) -- but it still
              // participates in the SAME fixed-point loop rather than
              // running once after, since ordering between all four is not
              // guaranteed by discovery order alone.
              Progress |= Monomorphize::drainStructRequests(PrePassASTs, MonoCollector, PreR);
            }
          } else {
            PreTCClean = false;
          }
        }
        if (opts.DumpInstantiations)
          MonoCollector.report(std::cerr);
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
        // TYPE-002 (S12a-S14a): a FuncDefExprAST that STILL has a non-empty
        // TypeParams list is an uninstantiated GENERIC TEMPLATE -- the
        // monomorphizer (sub-pass 2b.5, above) only ever appends a fully-
        // concrete CLONE to PrePassASTs (TypeParams cleared) and rewrites
        // every call site to it; the template itself is never meant to
        // reach Codegen with an open TurfGenericParamType anywhere in it
        // (F-GEN-2, INV-GEN-1). Originally (S12a) this only covered NON-
        // struct-bound (free-function) templates -- a struct-bound generic
        // (method or constructor) was deliberately left un-skipped, because
        // S12a's drain left it un-monomorphized on purpose (no
        // per-instantiation struct layout existed yet to substitute
        // `self.<field>` against). S13a built that layout and S14a's own
        // drainConstructorRequests()/visitMethodCall fix now monomorphize
        // EVERY struct-bound generic too, so the exemption is gone: ANY
        // FuncDefExprAST with non-empty TypeParams is now always a
        // template with a real clone elsewhere in PrePassASTs, regardless
        // of self-binding -- skipping all of them uniformly is what makes
        // erasure's deletion (below, this session) safe: a template's body
        // can still literally contain a TurfGenericParamType occurrence
        // with no monomorphized equivalent operation to fall back on if it
        // ever reached Codegen. Getting this too NARROW (the old,
        // self-bound-exempting condition) reintroduces exactly the
        // "Invalid bitcast"/erasure-branch-missing crash class this session
        // deletes erasure specifically to get away from -- for a template
        // whose body Codegen would now hit with no erasure branch left to
        // catch it.
        if (auto *FD = dynamic_cast<FuncDefExprAST *>(AST.get()))
          if (!FD->getTypeParams().empty())
            continue;
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
          // TYPE-002 (S12a-S14a): same template-skip as sub-pass 3a above --
          // see that comment for the full history of why this now covers
          // EVERY generic template uniformly, struct-bound or not.
          if (auto *FD = dynamic_cast<FuncDefExprAST *>(AST.get()))
            if (!FD->getTypeParams().empty())
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
  CloseSourceFile();
  OpenSourceFile(opts.InputFile);
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

  // LANG-002: re-consume the entry file's own leading `unsafe module;`
  // pragma (if any) here too -- this is a FRESH token stream (resetLexer()
  // above), so it sees the file's first token again just like the pre-pass
  // did. Idempotent re-registration (see UnsafeModuleFiles' own doc
  // comment, ParserModule.cpp); a genuine top-level unsafe BLOCK, if that's
  // what this turns out to be instead, is pushed into FnASTs to mirror how
  // the ordinary per-statement loop below categorizes one for this pass
  // (already codegenned in the pre-pass; re-parsed here purely for
  // Resolver/TypeChecker re-validation).
  {
    std::unique_ptr<ExprAST> LeadingUnsafeBlock;
    if (!ParseLeadingUnsafeModuleDecl(LeadingUnsafeBlock) && LeadingUnsafeBlock)
      AppendFlattenedTopLevelDecl(std::move(LeadingUnsafeBlock), FnASTs, true);
  }

  // Prelude auto-import, main-pass side -- mirrors the pre-pass injection
  // above exactly (same ParsePreludeImports() call, same rationale). Pushed
  // into FnASTs, matching how a real `import X;` is categorized in this
  // pass (already codegenned in the pre-pass; re-parsed here purely for
  // Resolver/TypeChecker re-validation, not re-codegen).
  for (auto &PreludeAST : ParsePreludeImports(CurLoc))
    FnASTs.push_back(std::move(PreludeAST));

  // OVH-011: mandatory-pass side of the same skip the pre-pass applies
  // above -- ImportedFiles was cleared by ClearParsedTopLevelASTs() between
  // passes, so this pass's own prelude injection just above re-populates it
  // from scratch, and this call must re-check/re-claim against THIS pass's
  // fresh cache. See ClaimEntryFileImportSlot()'s own doc comment
  // (ParserModule.cpp).
  if (!ClaimEntryFileImportSlot(SourceFilename)) {
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
        CurTok == TOK_MACHINE ||
        CurTok == TOK_AT || CurTok == TOK_EXTERN || CurTok == TOK_EXPORT ||
        CurTok == TOK_OPERATOR || CurTok == TOK_ERROR || CurTok == TOK_UNSAFE) {
      g_recoverActive = true;
      if (setjmp(g_recoverJmp) == 0) {
        auto AST = ParseExpression();
        if (AST) {
          if (CurTok == ';') {
            LogErrorAt(CurLoc, "Unexpected ';'", SLM::ErrorCategory::SYNTAX_ERROR);
          }
          // lint only — not re-codegenned. Also transparently flattens a
          // top-level `unsafe { }` block -- see AppendFlattenedTopLevelDecl()'s
          // doc comment (Parser.h).
          AppendFlattenedTopLevelDecl(std::move(AST), FnASTs, true);
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
            LogErrorAt(CurLoc, "Expected ';' after statement", SLM::ErrorCategory::SYNTAX_ERROR);
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
          LogErrorAt(CurLoc, "Expected ';' after statement", SLM::ErrorCategory::SYNTAX_ERROR);
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
          // _Exit (not exit): this raw exit point bypasses the rest of the
          // call stack, so the SLMCleanup guard in runCompilerMain() never
          // runs. That's fine for our own state, but it means llama.cpp's
          // ggml Metal backend never gets its explicit teardown either --
          // skip the whole static-destructor phase (ggml's own internal,
          // TU-static device registry included) rather than let it run
          // uncleanly. See runCompilerMain -> main's own _Exit for the full
          // explanation.
          std::cout.flush();
          std::cerr.flush();
          std::_Exit(1);
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
  } // OVH-011: end of "if (!ClaimEntryFileImportSlot(...))"

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

  // RT-001: wrap the user's compiled main with the turf_rt_init/
  // turf_rt_shutdown process bootstrap (see include/Codegen.h's doc comment
  // on WrapMainWithRuntimeBootstrap() for the full contract). Must run
  // AFTER every function is codegenned (true here -- this is already past
  // both the pre-pass and the mandatory main pass) and BEFORE
  // runOptimizationPipeline() below, whose Internalize must-preserve set is
  // exactly {"main"} (that function's own comment) -- this call is what
  // makes that symbol the wrapper rather than the user's own body.
  WrapMainWithRuntimeBootstrap();

  // TOOL-001: resolve any still-outstanding forward-declared debug-info
  // metadata (getDebugType()'s struct branch, CodegenCore.cpp) before
  // verifyModule() below -- a temporary MDNode left unresolved is invalid
  // IR, and this is the one point every function's codegen (pre-pass AND
  // main pass) is guaranteed to have already run. A no-op when `-g` wasn't
  // passed (DBuilder is null).
  if (DBuilder)
    DBuilder->finalize();

  // TYPE-002 (S15/D2, secondary): a monomorphized struct instantiation whose
  // request was never recorded/drained leaves getLLVMType()'s GenericType
  // branch's forward-declared opaque stub (CodegenCore.cpp) permanently
  // unfilled -- nothing ever calls setBody() on it, because that only
  // happens when drainStructRequests() (Monomorphize.cpp) appends the real
  // clone to PrePassASTs and main.cpp's sub-pass 2c codegens it. D2 closed
  // the one known gap that produced this (a GenericType used only as a
  // struct FIELD, never a variable/method-call/field-access site), but if
  // erasure's old silent-stub tolerance were ever reintroduced or a future
  // recording site is missed the same way, the previous behaviour was a
  // raw, low-level "Cannot allocate unsized type"/"GEP into unsized type"
  // dump from verifyModule() below -- technically correct, but it names an
  // LLVM struct, not a TurfLang instantiation, and gives no hint that a
  // monomorphization request went missing. Scanned BEFORE verifyModule()
  // so this diagnostic (naming the actual instantiation) fires instead of
  // -- not in addition to -- the generic LLVM-verifier dump. A struct type
  // is a monomorphized instantiation's if its name contains the "_gen<N>_"
  // marker Monomorphize::instantiatedDeclName() always inserts right after
  // the base name -- no ordinary, hand-written Turf struct name can contain
  // that (identifiers can't start a segment with a digit after "_gen").
  for (llvm::StructType *ST : TheModule->getIdentifiedStructTypes()) {
    if (!ST->isOpaque())
      continue;
    const std::string Name = ST->getName().str();
    size_t GenPos = Name.find("_gen");
    if (GenPos == std::string::npos ||
        GenPos + 4 >= Name.size() || !isdigit((unsigned char)Name[GenPos + 4]))
      continue; // Not a monomorphized-instantiation name -- an ordinary
                // forward-declared struct not yet codegenned is expected
                // and harmless here (ordinary main.cpp iteration order).
    std::cerr << "Internal error: monomorphized struct instantiation '"
              << Name
              << "' was never given a real layout (its instantiation "
                 "request was recorded but never drained/codegenned). "
                 "This is a compiler bug, not a mistake in your Turf "
                 "code -- please report it.\n";
    return 1;
  }

  // RT-001: TurfTypeDesc emission (see EmitAllTurfTypeDescs()'s own doc
  // comment, CodegenStruct.cpp, for why this must run here -- after every
  // struct in the module is guaranteed fully built, INCLUDING the
  // just-finished lingering-opaque-stub scan above -- rather than inline,
  // per struct, during StructDeclExprAST::codegen()). Still before
  // verifyModule(): a descriptor is an ordinary constant global like any
  // other, so there's no reason to build it after IR verification, and
  // every reason (this exact call site already exists as "the point every
  // struct is done") to build it before.
  EmitAllTurfTypeDescs();

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

  // Phase 5: OPT-001 optimization pipeline (runOptimizationPipeline() above).
  //
  // The TargetMachine is built here, BEFORE optimization runs, and shared
  // with object emission below -- previously it was built entirely inside
  // emitObjectFile(), strictly AFTER this phase, so the optimizer ran with
  // no target triple/data layout set on the module at all. Building it
  // first (when it's needed) means SROA/vectorization-adjacent passes now
  // see the real target's pointer size/alignment instead of LLVM's
  // triple-less defaults -- a correctness improvement, not an observable
  // behavior change (F-OPT-1): the host target this module already linked
  // against unconditionally is the same host emitObjectFile() always used.
  //
  // `--emit-llvm` never had a real TargetMachine before this change either
  // (M.setTargetTriple()/setDataLayout() were only ever reached from inside
  // emitObjectFile(), which that flag skips entirely) -- preserved as-is:
  // PassBuilder(nullptr) is well-defined and simply runs no target-aware
  // transform, which is what "just show me the IR" has always meant here.
  TargetMachine *TM = nullptr;
  if (!opts.EmitLLVM) {
    TM = createTurfTargetMachine(opts, *TheModule);
    if (!TM) {
      std::cerr << "Error: could not create a target machine for '"
                << (opts.TargetTriple.empty() ? sys::getDefaultTargetTriple()
                                               : opts.TargetTriple)
                << "'.\n";
      return 1;
    }
  }
  runOptimizationPipeline(*TheModule, opts.OptLevel, TM);

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

    if (emitObjectFile(*TheModule, *TM, ObjFile) != 0)
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

/**
 * @brief Real process entry point.
 *
 * Delegates to runCompilerMain() for all compiler work, then terminates via
 * _Exit() instead of a normal return. This deliberately skips the C++
 * static-destructor phase of exit() -- by this point every real output
 * (object/IR files, the linked binary, stdout/stderr) has already been
 * flushed by ordinary automatic-storage unwinding inside runCompilerMain(),
 * so nothing is lost. The only things being skipped are process-wide
 * static/global destructors, which is required here because llama.cpp's
 * ggml Metal backend keeps its own internal, TU-static device registry
 * (ggml-metal-device.m) that is torn down independently of -- and after --
 * SLM::SLMEngine's own explicit, already-correct cleanup (see the
 * SLMCleanup guard above). That independent teardown can hit
 * `GGML_ASSERT([rsets->data count] == 0)` during normal exit() on macOS,
 * a vendored-library issue outside this compiler's control.
 */
int main(int argc, char **argv) {
  int ExitCode = runCompilerMain(argc, argv);
  std::cout.flush();
  std::cerr.flush();
  std::_Exit(ExitCode);
}
