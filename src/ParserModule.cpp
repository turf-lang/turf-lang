/**
 * @file ParserModule.cpp
 * @brief Module/import resolution: ParseFile, ParseImport, prelude auto-imports, stdlib search paths.
 * @details Part of the recursive-descent parser for TurfLang, split from the
 *          original monolithic Parser.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/planning/COMPILER_MODULE_BREAKDOWN.md).
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

#include "Parser.h"
#include "AST.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "Codegen.h"
#include "Errors.h"
#include "Lexer.h"
#include "SuggestionIndex.h"
#include "SymbolTable.h"
#include "ParserInternal.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static std::unordered_set<std::string> ImportedFiles;

// Tracks the chain of modules currently being parsed (not yet finished) so a
// circular `import` (A -> B -> ... -> A) can be reported as a real
// diagnostic instead of silently truncating. This is deliberately a SEPARATE
// structure from ImportedFiles above: ImportedFiles is a "fully parsed,
// cacheable" set (a module already finished parsing returns an empty body on
// a second import, by design -- see ParseFile()), while ImportStack is an
// "in progress right now" stack that must be empty of a path the moment that
// path's own ParseFile() call returns, cycle or not.
struct ImportStackFrame {
  std::string ModuleName;
  std::string ResolvedPath;
  SourceLocation ImportLoc; // location of the `import` statement that pulled this in
};
static std::vector<ImportStackFrame> ImportStack;

// Cache of every top-level AST node codegenned so far in this compile
// (used by the sandbox to rebuild a full-program AST for diffing/retrying
// -- see BuildProgramASTFromCache() below). Lives here alongside
// ClearParsedTopLevelASTs(), which resets it together with the import
// caches above on every fresh compile.
static std::vector<std::unique_ptr<ExprAST>> ParsedTopLevelASTs;

void ClearParsedTopLevelASTs() {
  ParsedTopLevelASTs.clear();
  ImportedFiles.clear();
  // ImportStack should always be empty already (the RAII guard in ParseFile()
  // pops every frame it pushes, cycle or not) -- cleared defensively anyway,
  // mirroring ImportedFiles' own reset-between-compiles behavior above.
  ImportStack.clear();
}

std::vector<std::unique_ptr<ExprAST>> SwapParsedTopLevelASTs(std::vector<std::unique_ptr<ExprAST>> New) {
  std::vector<std::unique_ptr<ExprAST>> Old = std::move(ParsedTopLevelASTs);
  ParsedTopLevelASTs = std::move(New);
  return Old;
}

// Saved (ImportedFiles, ImportStack) pairs, pushed/popped alongside
// SwapParsedTopLevelASTs() by anything that runs a SECOND, isolated parse
// pass in the same process as an already-in-progress or already-finished
// compile (Sandbox.cpp's temp-file validation passes today; a future
// buffer-compile library entry point tomorrow -- see issue #233). Without
// this, a sandboxed pass ran with the REAL compile's ImportedFiles cache
// still warm: re-`import`ing a module the real compile had already fully
// parsed silently returned an empty block for the SANDBOXED buffer too
// (ParseFile()'s cache-hit fast path, `include/Parser.h`), even though that
// import is genuinely new work for this isolated pass. ImportStack is
// included for symmetry/safety even though it should always be empty at
// push/pop time in practice -- see ClearParsedTopLevelASTs()'s doc comment.
// A vector-of-pairs (not a single saved slot) so nested save/restore is
// safe, matching SwapParsedTopLevelASTs' own move-in/move-out shape.
static std::vector<std::pair<std::unordered_set<std::string>, std::vector<ImportStackFrame>>>
    SavedImportCycleStates;

void PushImportCycleState() {
  SavedImportCycleStates.emplace_back(std::move(ImportedFiles), std::move(ImportStack));
  ImportedFiles.clear();
  ImportStack.clear();
}

void PopImportCycleState() {
  auto &Saved = SavedImportCycleStates.back();
  ImportedFiles = std::move(Saved.first);
  ImportStack = std::move(Saved.second);
  SavedImportCycleStates.pop_back();
}

void RecordParsedTopLevelAST(const ExprAST *AST) {
  if (AST) {
    ParsedTopLevelASTs.push_back(CloneExprAST(AST));
  }
}

std::unique_ptr<ExprAST> BuildProgramASTFromCache() {
  return std::make_unique<BlockExprAST>(CloneExprList(ParsedTopLevelASTs));
}


// Builds the reportable chain for a detected import cycle: the sub-sequence
// of ImportStack starting at the frame that re-enters Path (the module
// currently being closed back on) through to the top of the stack (whoever's
// `import ModuleName;` triggered this specific ParseFile() call). The caller
// appends ModuleName itself to close the loop when rendering.
static std::vector<ImportChainEntry>
BuildCircularImportChain(const std::vector<ImportStackFrame> &Stack,
                         const std::string &Path) {
  std::vector<ImportChainEntry> Chain;
  bool Started = false;
  for (const auto &Frame : Stack) {
    if (Frame.ResolvedPath == Path)
      Started = true;
    if (Started)
      Chain.push_back({Frame.ModuleName, Frame.ImportLoc});
  }
  return Chain;
}

std::unique_ptr<ExprAST> ParseFile(const std::string &Path,
                                   const std::string &ModuleName,
                                   SourceLocation ImportLoc) {
  // Force TurfError::raise() to take its plain "print and exit" branch,
  // never the longjmp-based recovery one, for the ENTIRE duration of
  // parsing this file (including anything it transitively imports).
  //
  // main.cpp's pre-pass "parse all top-level declarations" loop wraps each
  // top-level statement's parse in `g_recoverActive = true; if (setjmp(
  // g_recoverJmp) == 0) { ParseExpression(); } else { SkipToNextTopLevel
  // Boundary(); }` -- a deliberate mechanism to skip one broken top-level
  // statement in the file actively being compiled and keep collecting
  // errors from the rest of it. That's safe for an error raised directly
  // in the main file's own statement (nothing but trivial scope guards sit
  // between the raise() and the setjmp point), but NOT safe once a nested
  // `import` is involved: longjmp() never runs C++ destructors for stack
  // objects between the jump and its setjmp point, and ParseFile()'s own
  // LexerStateGuard/ImportStackGuard below live exactly there. A
  // CircularImportError (or ModuleNotFoundError, or any other TurfError)
  // firing two or more import-levels deep would longjmp straight past
  // those guards, permanently leaving the global `SourceFile` pointer
  // aimed at an already-destroyed local std::ifstream (this function's own
  // `ImportFile`, still "open" on the stack, never restored to whatever it
  // pointed at before) -- confirmed live as a heap-corruption crash
  // (`munmap_chunk(): invalid pointer`) far downstream, the next time
  // ANYTHING touched that dangling pointer (main()'s own unrelated
  // `SourceFile->close()` call during pre-pass cleanup). Forcing
  // g_recoverActive false here means any hard error anywhere inside an
  // imported module (at any nesting depth) instead calls std::exit()
  // directly -- always safe, since exit() terminates immediately and never
  // depends on the intermediate stack state a longjmp would corrupt. The
  // tradeoff -- a broken import ends the whole compile instead of being
  // "skipped" -- is the right one anyway: there's no useful way to skip
  // past a broken dependency and keep compiling against it.
  struct RecoveryGuard {
    bool Previous;
    RecoveryGuard() : Previous(g_recoverActive) { g_recoverActive = false; }
    ~RecoveryGuard() { g_recoverActive = Previous; }
  } RecovGuard;

  // Circular-import check FIRST, before the ImportedFiles cache check below:
  // a module still being parsed (on ImportStack) is a genuine cycle, not a
  // cache hit. Checking ImportedFiles first would be wrong here -- that set
  // isn't populated for the CURRENT file until after this check anyway, but
  // conceptually the two checks answer different questions ("is this a
  // cycle" vs "did we already fully parse this") and cycle detection must
  // win when both could apply.
  for (const auto &Frame : ImportStack) {
    if (Frame.ResolvedPath == Path) {
      CircularImportError(ImportLoc, ModuleName,
                          BuildCircularImportChain(ImportStack, Path))
          .raise();
      return nullptr;
    }
  }

  if (ImportedFiles.count(Path)) {
    return std::make_unique<BlockExprAST>(
        std::vector<std::unique_ptr<ExprAST>>{});
  }

  std::ifstream ImportFile(Path);
  if (!ImportFile.is_open()) {
    return nullptr;
  }

  ImportedFiles.insert(Path);

  // Push this module onto the in-progress stack for the duration of parsing
  // its body; popped via RAII (including on every early-return path below,
  // e.g. the "nested ParseExpression already reported a real error" bailouts
  // already documented further down in this function) so a missed pop can
  // never silently poison every subsequent import in the same compile.
  struct ImportStackGuard {
    ImportStackGuard(std::string MN, std::string RP, SourceLocation Loc) {
      ImportStack.push_back({std::move(MN), std::move(RP), Loc});
    }
    ~ImportStackGuard() { ImportStack.pop_back(); }
  } IGuard(ModuleName, Path, ImportLoc);

  // Save current lexer state
  struct LexerStateGuard {
    std::istream *SavedSourceFile;
    int SavedLastChar, SavedCurTok, SavedCurLine, SavedCurCol;
    SourceLocation SavedCurLoc;
    std::string SavedFilename, SavedIdentifierStr, SavedStringVal;
    double SavedNumVal; long long SavedIntVal; bool SavedBoolVal;
    // SourceLines is what printDiag()'s caret snippet is snapshotted from
    // (see DiagnosticEngine::add() in Lexer.cpp) -- without saving/restoring
    // it here alongside SourceFilename, it never got populated for the
    // imported file at all, so any diagnostic raised while parsing an import
    // would snapshot the IMPORTING file's line text (whatever line happened
    // to share the same line number) instead of the module's real line.
    std::vector<std::string> SavedSourceLines;

    LexerStateGuard() {
      SavedSourceFile = SourceFile;
      SavedLastChar = GetLastChar();
      SavedCurTok = CurTok;
      SavedCurLoc = CurLoc;
      SavedCurLine = CurLine;
      SavedCurCol = CurCol;
      SavedFilename = SourceFilename;
      SavedNumVal = NumVal;
      SavedIntVal = IntVal;
      SavedBoolVal = BoolVal;
      SavedIdentifierStr = IdentifierStr;
      SavedStringVal = StringVal;
      SavedSourceLines = SourceLines;
    }
    ~LexerStateGuard() {
      SourceFile = SavedSourceFile;
      SetLastChar(SavedLastChar);
      CurTok = SavedCurTok;
      CurLoc = SavedCurLoc;
      CurLine = SavedCurLine;
      CurCol = SavedCurCol;
      SourceFilename = SavedFilename;
      NumVal = SavedNumVal;
      IntVal = SavedIntVal;
      BoolVal = SavedBoolVal;
      IdentifierStr = SavedIdentifierStr;
      StringVal = SavedStringVal;
      SourceLines = SavedSourceLines;
    }
  } LGuard;

  // Set new lexer state
  SourceFile = &ImportFile;
  SourceFilename = Path;
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 1};
  SetLastChar(' ');

  // Populate SourceLines for THIS file so any diagnostic raised while
  // parsing it snapshots the module's own line text, not the importer's
  // (see LexerStateGuard's doc comment above). Mirrors the read loop
  // main.cpp uses to populate SourceLines for the top-level input file.
  SourceLines.clear();
  {
    std::ifstream LineReader(Path);
    std::string Line;
    while (std::getline(LineReader, Line))
      SourceLines.push_back(Line);
  }
  // Also seed the persistent cross-pass cache -- see AllSourceLines' doc
  // comment in Lexer.h. Unlike SourceLines, this entry survives past the
  // LexerStateGuard's restore below, so Resolver/TypeChecker/Codegen
  // diagnostics about content inside THIS module can still find their line.
  AllSourceLines[Path] = SourceLines;

  getNextToken();

  std::vector<std::unique_ptr<ExprAST>> Exprs;

  // Parse the file
  while (true) {
    if (CurTok == TOK_EOF)
      break;
    if (CurTok == ';') {
      getNextToken();
      continue;
    }

    if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM ||
        CurTok == TOK_MACHINE || CurTok == TOK_AT ||
        CurTok == TOK_EXTERN || CurTok == TOK_EXPORT || CurTok == TOK_OPERATOR ||
        CurTok == TOK_ERROR) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok == ';') {
          LogErrorAt(CurLoc, "Unexpected ';'");
          return nullptr;
        }
        Exprs.push_back(std::move(Expr));
      } else {
        // A nested ParseExpression() call already reported a real error
        // (via LogErrorAt) and returned nullptr -- bail out of this file
        // immediately rather than "skip one token and keep trying" (this
        // used to just call getNextToken() and continue the loop). Unlike
        // the top-level program's own parse loop, which deliberately keeps
        // going after an error to surface multiple problems in one compile,
        // there is no equivalent multi-error-collection UX for an imported
        // module -- and limping forward token-by-token through an already-
        // malformed declaration is actively dangerous here: confirmed to
        // eventually run the underlying ifstream's read position past EOF
        // repeatedly, which crashes the ENTIRE compiler with an unhandled
        // `std::__ios_failure` (not just this import failing cleanly) once
        // enough repeated `.get()` calls on an already-exhausted stream
        // trip libstdc++'s exception state. Reproduced via
        // `extern fn f(ptr: void*): void;` inside an imported module --
        // `ptr` collides with the `ptr` type keyword, which is itself a
        // separate, real stdlib bug (lib/std/mem.tr's own extern
        // declarations use `ptr` as a parameter name) -- but no single
        // syntax error inside an imported module should ever be able to
        // crash the compiler, regardless of what triggers it.
        return nullptr;
      }
      continue;
    }

    if (CurTok == TOK_IMPORT) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok != ';') {
          LogErrorAt(CurLoc, "Expected ';' after statement.");
          return nullptr;
        }
        getNextToken(); // eat ';'
        Exprs.push_back(std::move(Expr));
      } else {
        // See the comment on the identical change above.
        return nullptr;
      }
      continue;
    }

    // Everything else: must end with ';'
    auto Expr = ParseExpression();
    if (Expr) {
      if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after statement.");
        return nullptr;
      }
      getNextToken(); // eat ';'
      Exprs.push_back(std::move(Expr));
    } else {
      // See the comment on the identical change above.
      return nullptr;
    }
  }

  return std::make_unique<BlockExprAST>(std::move(Exprs));
}


// The Turf installation root: everything module resolution needs (lib/std,
// and lib/usr once a package manager exists) hangs off this one directory.
// `TURF_HOME`, if set, IS the installation -- a real install unpacked
// somewhere, or a dev checkout of this repo pointed at by hand -- and is
// authoritative. Without it, fall back to deriving the root from the
// compiler binary's own location (<binary>/..), so a freshly-built
// `build/turf` still finds the sibling `lib/` directory with zero setup
// (this is the previous, only, behavior -- kept as a fallback rather than
// removed so CI/local dev builds don't all need TURF_HOME exported just to
// run the test suite). See docs/architecture/modules_and_scope_resolution.md
// Part 2.4 for the full rationale.
static std::string TurfInstallRoot() {
  if (const char *Env = std::getenv("TURF_HOME")) {
    if (*Env)
      return std::string(Env);
  }
  llvm::SmallString<128> Root(CompilerExecutablePath);
  llvm::sys::path::remove_filename(Root); // strip the "turf"/"turf.exe" itself
  llvm::sys::path::append(Root, "..");
  return Root.str().str();
}

// Recursively indexes every "<name>.tr" file under <Root>/lib/<SubdirName>
// ONCE per (Root, SubdirName) pair, keyed by module name (filename minus
// ".tr"), and caches it for the rest of this compiler invocation. `lib/std`
// is organized into logical subfolders on disk (core/collections/io/mem/
// math/testing) purely for humans browsing the repo -- `import list;` names
// a MODULE, never a path, so this index is what lets a module move between
// subfolders later without a single import statement anywhere changing.
// `lib/usr` (third-party/user-installed packages, one subfolder per module
// name -- no installer exists yet, this is the search side of that story
// only) is indexed the same way, just under a different subdirectory.
static const std::unordered_map<std::string, std::string> &
IndexModuleDir(const std::string &Root, const char *SubdirName) {
  static std::map<std::string, std::unordered_map<std::string, std::string>>
      Cache;
  std::string Key = Root + "|" + SubdirName;
  auto Cached = Cache.find(Key);
  if (Cached != Cache.end())
    return Cached->second;

  std::unordered_map<std::string, std::string> Index;
  llvm::SmallString<256> Dir(Root);
  llvm::sys::path::append(Dir, "lib", SubdirName);

  std::error_code EC;
  for (llvm::sys::fs::recursive_directory_iterator I(Dir.str(), EC), E;
       !EC && I != E; I.increment(EC)) {
    if (I->type() == llvm::sys::fs::file_type::regular_file &&
        llvm::sys::path::extension(I->path()) == ".tr") {
      std::string Name = llvm::sys::path::stem(I->path()).str();
      // First hit wins. A genuine same-name collision can't happen in
      // lib/std today (curated, one file per module name) and lib/usr has
      // no installer yet to ever produce one -- not worth a diagnostic for
      // a case that isn't reachable yet.
      Index.emplace(std::move(Name), I->path());
    }
  }
  return Cache.emplace(std::move(Key), std::move(Index)).first->second;
}

// Module-name -> file-search -> ParseFile() -> ModuleImportExprAST tail,
// shared by ParseImport() (real, source-text `import X;`/`import X::[...];`
// statements) and ParsePreludeImports() (synthetic prelude imports injected
// by main.cpp with no source text at all -- see its own doc comment). This
// function doesn't care how ModuleName/Loc/Mode/RequestedSymbols were
// obtained, only that they're valid; the two callers just supply them
// differently.
std::unique_ptr<ExprAST>
ParseModuleImportByName(const std::string &ModuleName, SourceLocation Loc,
                        SourceLocation NameLoc,
                        ImportMode Mode = ImportMode::Bare,
                        std::vector<ImportedSymbolRef> RequestedSymbols = {}) {
  // Search order:
  //   1. Installation stdlib   -- $TURF_HOME (or <binary>/..)/lib/std/**/<module>.tr
  //   2. Installed user package -- $TURF_HOME (or <binary>/..)/lib/usr/**/<module>.tr
  //   3. Same directory as the source file being compiled
  //   4. Current working directory
  //
  // A ParseFile() call can return nullptr for two very different reasons:
  // the file simply doesn't exist at that path (fine, keep trying the next
  // one), or the file DOES exist but has a genuine syntax error inside it
  // (LogErrorAt already recorded a real diagnostic -- trying the NEXT
  // fallback path at that point would be wrong: it can only ever produce a
  // confusing, unrelated "module not found" error stacked on top of the
  // real one, since the module WAS found, just broken). Distinguish the two
  // by snapshotting the diagnostic count before each attempt and checking
  // whether it grew.
  // ResolvedPath tracks whichever candidate actually got parsed -- it's the
  // canonical identity of "this physical module file", used by both
  // circular-import detection (Parser-level) and per-file import scoping
  // (Resolver-level) so neither has to re-derive or guess a path string
  // independently of the other.
  std::string Root = TurfInstallRoot();
  std::string ResolvedPath;
  std::unique_ptr<ExprAST> Block;
  size_t DiagCountBefore;
  bool FoundButBroken = false;

  const auto &StdIndex = IndexModuleDir(Root, "std");
  if (auto It = StdIndex.find(ModuleName); It != StdIndex.end()) {
    ResolvedPath = It->second;
    DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
    Block = ParseFile(ResolvedPath, ModuleName, Loc);
    FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
  }

  if (!Block && !FoundButBroken) {
    const auto &UsrIndex = IndexModuleDir(Root, "usr");
    if (auto It = UsrIndex.find(ModuleName); It != UsrIndex.end()) {
      ResolvedPath = It->second;
      DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
      Block = ParseFile(ResolvedPath, ModuleName, Loc);
      FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
    }
  }

  if (!Block && !FoundButBroken) {
    // Issue #19 fix: resolve relative to the source file's directory so that
    // `turf path/to/file.tr` can find sibling modules without a cd first.
    if (!SourceFilename.empty()) {
      llvm::SmallString<256> SrcRelPath(SourceFilename);
      llvm::sys::path::remove_filename(SrcRelPath);
      llvm::sys::path::append(SrcRelPath, ModuleName + ".tr");
      ResolvedPath = SrcRelPath.str().str();
      DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
      Block = ParseFile(ResolvedPath, ModuleName, Loc);
      FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
    }
  }
  if (!Block && !FoundButBroken) {
    ResolvedPath = ModuleName + ".tr";
    DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
    Block = ParseFile(ResolvedPath, ModuleName, Loc);
    FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
    if (!Block && !FoundButBroken) {
      ModuleNotFoundError(Loc, ModuleName).raise();
      return nullptr;
    }
  }

  // The module WAS found at some path but failed to parse -- a real
  // diagnostic was already recorded there (see FoundButBroken's doc comment
  // above); don't construct a ModuleImportExprAST wrapping a null Block.
  if (!Block)
    return nullptr;

  return std::make_unique<ModuleImportExprAST>(Loc, ModuleName, NameLoc, ResolvedPath,
                                                std::move(Block), Mode,
                                                std::move(RequestedSymbols));
}


// The fixed set of stdlib modules auto-imported into every top-level
// compile (STRING_UFCS_OPERATOR_CHAR_RFC.md §5.3/§7 step 6) -- this list
// IS "prelude.tr" as far as the compiler is concerned; there is no actual
// lib/std/prelude.tr file the compiler reads. Scoped to the top-level file
// only (see main.cpp's two call sites) -- library modules importing each
// other still explicitly import what they need, exactly like today.
static const char *const PreludeModuleNames[] = {
    "int", "double", "bool", "char", "strings", "array"};

// Synthesizes a `ModuleImportExprAST` for each prelude module, as if the
// user had written `import int; import double; ...` as the literal first
// lines of their file -- reuses ParseModuleImportByName() (the exact same
// search-path/ParseFile()/AST-construction logic a real import goes
// through), so this is indistinguishable from a real import as far as the
// Resolver is concerned. NOT a new, separate always-on registry -- that
// shortcut is exactly what caused prelude.tr's original two bugs (see
// main.cpp's doc comment on why prelude.tr was removed, and the fixes
// referenced there that are already in place independent of this).
// These are compiler-shipped files that must always exist; a missing one
// is an internal compiler error, not a user-facing diagnostic.
std::vector<std::unique_ptr<ExprAST>> ParsePreludeImports(SourceLocation Loc) {
  std::vector<std::unique_ptr<ExprAST>> Result;

  // A top-level file that IS one of these prelude modules itself (e.g.
  // compiling lib/std/char.tr directly -- `turf lib/std/char.tr`, or the
  // `--resolve`-only compile-check AGENTS.md mandates for every lib/std/*.tr
  // file since the test runner doesn't otherwise exercise them) must NOT
  // auto-import itself: doing so re-parses the same file a second time as an
  // imported module, registering every one of its own functions TWICE --
  // once as an ordinary top-level "current file" function
  // (Resolver::visitFuncDef's normal registration) and once again under the
  // module's own namespace. Any function in that file that then calls one of
  // its own siblings via UFCS or an operator overload (e.g. char.tr's
  // isAlpha() calling c.isUpper()/c.isLower(), int.tr's gcd() calling
  // a.mod(b), strings.tr's `+` operator concatenating in a loop) finds TWO
  // exact-match candidates for the same call site and reports a false
  // "Ambiguous ... call" -- confirmed live before this check existed. This
  // is a pure self-import artifact of compiling a prelude module AS the
  // entry point: the identical file compiled as a real user program that
  // `import char;`s it normally has no duplicate and always worked correctly
  // (bare unqualified UFCS/operator calls inside the module itself are the
  // only thing this ever affected).
  llvm::SmallString<256> CanonicalSource;
  bool HaveCanonicalSource = false;
  if (!SourceFilename.empty() &&
      !llvm::sys::fs::real_path(SourceFilename, CanonicalSource))
    HaveCanonicalSource = true;

  for (const char *Name : PreludeModuleNames) {
    if (HaveCanonicalSource) {
      // Same TURF_HOME/binary-relative root + recursive index the real
      // lookup uses (TurfInstallRoot()/IndexModuleDir()) -- this MUST agree
      // with ParseModuleImportByName()'s own resolution, since a prelude
      // name found at a different physical path than the one about to be
      // compiled as the entry point would defeat this self-import check
      // entirely (see the doc comment above for what that breaks).
      std::string Root = TurfInstallRoot();
      const auto &StdIndex = IndexModuleDir(Root, "std");
      auto It = StdIndex.find(Name);
      if (It != StdIndex.end()) {
        llvm::SmallString<256> CanonicalCandidate;
        if (!llvm::sys::fs::real_path(It->second, CanonicalCandidate) &&
            CanonicalCandidate == CanonicalSource)
          continue; // compiling this prelude module as the top-level file itself
      }
    }

    auto AST = ParseModuleImportByName(Name, Loc, /*NameLoc=*/Loc);
    if (!AST) {
      llvm::report_fatal_error(
          llvm::StringRef(std::string("Internal error: prelude module '") +
                          Name + "' failed to load. This is a compiler bug."));
    }
    Result.push_back(std::move(AST));
  }
  return Result;
}


std::unique_ptr<ExprAST> ParseImport() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat import
  std::string ModuleName;
  // int/double/bool/char are prelude module names (STRING_UFCS_OPERATOR_
  // CHAR_RFC.md §7 step 6) that are ALSO primitive-type keywords, so they
  // lex as TOK_TYPE_INT/TOK_TYPE_DOUBLE/TOK_TYPE_BOOL/TOK_TYPE_CHAR, not
  // TOK_IDENTIFIER -- without this, a user literally writing `import char;`
  // hit "Expected module name after 'import'", even though the prelude
  // mechanism itself loads char.tr internally just fine (it never goes
  // through this token-based path at all, see ParsePreludeImports()).
  // `string` (singular, the type keyword) doesn't need an entry here: the
  // actual module is `strings` (plural), a real TOK_IDENTIFIER, no collision.
  SourceLocation NameLoc = CurLoc; // location of the module-name token itself -- see #230
  if (CurTok == TOK_IDENTIFIER) {
    ModuleName = IdentifierStr;
  } else if (CurTok == TOK_TYPE_INT) {
    ModuleName = "int";
  } else if (CurTok == TOK_TYPE_DOUBLE) {
    ModuleName = "double";
  } else if (CurTok == TOK_TYPE_BOOL) {
    ModuleName = "bool";
  } else if (CurTok == TOK_TYPE_CHAR) {
    ModuleName = "char";
  } else {
    LogErrorAt(CurLoc, "Expected module name after 'import'");
    return nullptr;
  }
  getNextToken();

  // On-demand qualified forms: import Path::Symbol; / import Path::[a, b];
  // / import Path::[*]; -- each pulls SPECIFIC symbols into the importing
  // file's own flat (unqualified) namespace, instead of the bare form's
  // qualified-access-only behavior (see ImportMode's doc comment). The
  // module file itself is still always fully parsed below regardless of
  // mode -- even a single targeted symbol import needs the whole file
  // searched, since there's no lazy/partial parsing.
  ImportMode Mode = ImportMode::Bare;
  std::vector<ImportedSymbolRef> RequestedSymbols;
  if (CurTok == TOK_SCOPE) {
    getNextToken(); // eat '::'
    if (CurTok == '[') {
      getNextToken(); // eat '['
      if (CurTok == '*') {
        getNextToken(); // eat '*'
        if (CurTok != ']') {
          LogErrorAt(CurLoc, "Expected ']' after '*' in import list");
          return nullptr;
        }
        getNextToken(); // eat ']'
        Mode = ImportMode::Wildcard;
      } else {
        Mode = ImportMode::SymbolList;
        while (CurTok != ']' && CurTok != TOK_EOF) {
          if (CurTok != TOK_IDENTIFIER) {
            LogErrorAt(CurLoc, "Expected a symbol name in import list");
            return nullptr;
          }
          RequestedSymbols.push_back({IdentifierStr, CurLoc});
          getNextToken();
          if (CurTok == ',') {
            getNextToken();
          } else {
            break;
          }
        }
        if (CurTok != ']') {
          LogErrorAt(CurLoc, "Expected ']' to close import list");
          return nullptr;
        }
        getNextToken(); // eat ']'
        if (RequestedSymbols.empty()) {
          LogErrorAt(CurLoc, "Expected at least one symbol name in import list");
          return nullptr;
        }
      }
    } else if (CurTok == TOK_IDENTIFIER) {
      Mode = ImportMode::Symbol;
      RequestedSymbols.push_back({IdentifierStr, CurLoc});
      getNextToken();
    } else {
      LogErrorAt(CurLoc,
                 "Expected a symbol name or '[' after '::' in import");
      return nullptr;
    }
  }

  return ParseModuleImportByName(ModuleName, Loc, NameLoc, Mode, std::move(RequestedSymbols));
}
