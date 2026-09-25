/**
 * @file ParserModule.cpp
 * @brief Module/import resolution: ParseFile, ParseImport, prelude auto-imports, stdlib search paths.
 * @details Part of the recursive-descent parser for TurfLang, split from the
 *          original monolithic Parser.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
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
#include <cstring>
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

// Nodes spliced out of a top-level `unsafe { }` block by ParseFile()'s
// dedicated TOK_UNSAFE branch below (issue #238). Every other consumer of
// a file's top-level declaration list (Resolver::hoistTopLevel(),
// main.cpp's multi-pass codegen driver, TypeChecker::checkAll()'s
// resumable-per-declaration loop) walks a FLAT vector of decl-shaped
// nodes, found by dynamic_cast, not a recursive AST walk -- wrapping a
// top-level struct/fn declaration in a real nested UnsafeBlockExprAST node
// would make it invisible to all of them. Flattening at parse time instead
// (splice the unsafe block's own children directly into the file's
// top-level list, tag each one here) means none of those consumers need
// to change at all; only the TypeChecker's unsafe-gate checks consult this
// set. Keyed by raw ExprAST* identity, the same idiom TypeChecker's own
// NodeTypes_ map already uses for per-node side-state.
static std::unordered_set<const ExprAST *> UnsafeTopLevelDecls;

void ClearParsedTopLevelASTs() {
  ParsedTopLevelASTs.clear();
  ImportedFiles.clear();
  // ImportStack should always be empty already (the RAII guard in ParseFile()
  // pops every frame it pushes, cycle or not) -- cleared defensively anyway,
  // mirroring ImportedFiles' own reset-between-compiles behavior above.
  ImportStack.clear();
  // Never cleared mid-compile -- reset here alongside the other per-compile
  // parser caches so a stale entry can't survive into the next compile in
  // the same process (a long-lived embedder, e.g. an LSP).
  UnsafeTopLevelDecls.clear();
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

bool WasDeclaredInsideTopLevelUnsafe(const ExprAST *Node) {
  return Node && UnsafeTopLevelDecls.count(Node) != 0;
}

void AppendFlattenedTopLevelDecl(std::unique_ptr<ExprAST> AST,
                                 std::vector<std::unique_ptr<ExprAST>> &Target,
                                 bool RecordForSandbox) {
  if (!AST)
    return;
  if (auto *UB = dynamic_cast<UnsafeBlockExprAST *>(AST.get())) {
    for (auto &Child : UB->takeChildren()) {
      UnsafeTopLevelDecls.insert(Child.get());
      if (RecordForSandbox)
        RecordParsedTopLevelAST(Child.get());
      Target.push_back(std::move(Child));
    }
    return;
  }
  if (RecordForSandbox)
    RecordParsedTopLevelAST(AST.get());
  Target.push_back(std::move(AST));
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
  // NOTE (issue #242, history): this function used to force
  // g_recoverActive = false unconditionally here, making any TurfError
  // anywhere inside an imported module (at any nesting depth) call
  // std::exit() directly instead of the normal longjmp-based recovery --
  // because longjmp() never runs C++ destructors for stack objects between
  // the jump and its setjmp() point, and a longjmp reaching all the way
  // back out to some CALLER's distant setjmp (main.cpp's pre-pass loop,
  // or any other embedder's own per-statement loop) would skip straight
  // past THIS function's own LexerStateGuard/ImportStackGuard below,
  // permanently leaving the global `SourceFile` pointer aimed at an
  // already-destroyed local std::ifstream (this function's own
  // `ImportFile`) -- confirmed at the time as a heap-corruption crash
  // (`munmap_chunk(): invalid pointer`) the next time anything touched
  // that dangling pointer. That was a real, necessary fix for the crash,
  // but the exit() tradeoff meant a broken import always killed the whole
  // process -- fatal for any tool embedding this compiler in-process
  // (an LSP) rather than shelling out per compile.
  //
  // Fixed properly below by giving THIS ParseFile() invocation its own
  // LOCAL recovery boundary (its own setjmp() point, armed right before
  // the parse loop) instead of relying on some distant caller's. Since the
  // jump target and LexerStateGuard/ImportStackGuard now live in the exact
  // same function scope, a longjmp() triggered by anything during this
  // file's own parsing -- or by a TurfError bubbling up through a
  // TRANSITIVELY nested `import` (that nested ParseFile() call installs
  // ITS OWN local recovery boundary the same way, so the jump never
  // travels further than the innermost currently-parsing ParseFile()) --
  // never needs to leave this function at all, so the guards' destructors
  // run normally regardless of whether parsing this file succeeds,
  // returns nullptr via an ordinary early return, or is caught by this
  // function's own setjmp(). See the setjmp() call further down for where
  // this pays off, and RecoveryBoundaryGuard just below for the g_recover*
  // save/restore (so this doesn't corrupt whatever OUTER recovery scope --
  // main.cpp's own loop, or any embedder's -- is already active around
  // the `import` statement that triggered this call).
  struct RecoveryBoundaryGuard {
    bool PrevActive;
    jmp_buf PrevJmpBuf;
    RecoveryBoundaryGuard() : PrevActive(g_recoverActive) {
      std::memcpy(PrevJmpBuf, g_recoverJmp, sizeof(jmp_buf));
    }
    ~RecoveryBoundaryGuard() {
      g_recoverActive = PrevActive;
      std::memcpy(g_recoverJmp, PrevJmpBuf, sizeof(jmp_buf));
    }
  } RecovBoundary;

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

  // NOTE (issue #242 follow-up, found via code review): NOT inserted into
  // ImportedFiles here, before parsing is even attempted -- that used to
  // mean a module that fails to parse was still marked "fully imported,"
  // so a SECOND, unrelated `import` of the same broken path (e.g. two
  // different files both importing it) hit the ImportedFiles cache check
  // above and silently returned an empty-but-"successful" block instead of
  // re-attempting the parse and re-reporting the failure. Unreachable
  // before #242 (any import failure called std::exit() immediately, so a
  // second import attempt never got a chance to run); newly reachable now
  // that a broken import returns cleanly instead. Not needed here for
  // cycle-safety either -- a genuine self-referencing import is already
  // caught by the ImportStack check above (IGuard below pushes onto it
  // BEFORE the parse loop starts), which is a completely separate
  // structure from this "fully parsed" cache -- see ImportStack's own doc
  // comment. Inserted instead at the bottom of this function, only once
  // parsing has genuinely succeeded.

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
    std::string SavedNumLitSpelling, SavedNumLitClean;
    int SavedNumLitRadix;
    bool SavedBoolVal;
    // SourceLines is what printDiag()'s caret snippet is snapshotted from
    // (see DiagnosticEngine::add() in Lexer.cpp) -- without saving/restoring
    // it here alongside SourceFilename, it never got populated for the
    // imported file at all, so any diagnostic raised while parsing an import
    // would snapshot the IMPORTING file's line text (whatever line happened
    // to share the same line number) instead of the module's real line.
    std::vector<std::string> SavedSourceLines;
    // g_braceDepth (issue #242): previously never needed saving/restoring
    // here because a failed import always called std::exit() immediately --
    // whatever this file's own parsing left g_braceDepth at, the process
    // was already dead. Now that a failed import returns cleanly instead
    // (see RecoveryBoundaryGuard above), the IMPORTING file's own
    // subsequent parsing must not inherit a bracket-depth counter left
    // mid-count by this file's abandoned parse.
    int SavedBraceDepth;

    LexerStateGuard() {
      SavedSourceFile = SourceFile;
      SavedLastChar = GetLastChar();
      SavedCurTok = CurTok;
      SavedCurLoc = CurLoc;
      SavedCurLine = CurLine;
      SavedCurCol = CurCol;
      SavedFilename = SourceFilename;
      SavedNumLitSpelling = NumLitSpelling;
      SavedNumLitClean = NumLitClean;
      SavedNumLitRadix = NumLitRadix;
      SavedBoolVal = BoolVal;
      SavedIdentifierStr = IdentifierStr;
      SavedStringVal = StringVal;
      SavedSourceLines = SourceLines;
      SavedBraceDepth = g_braceDepth;
    }
    ~LexerStateGuard() {
      SourceFile = SavedSourceFile;
      SetLastChar(SavedLastChar);
      CurTok = SavedCurTok;
      CurLoc = SavedCurLoc;
      CurLine = SavedCurLine;
      CurCol = SavedCurCol;
      SourceFilename = SavedFilename;
      NumLitSpelling = SavedNumLitSpelling;
      NumLitClean = SavedNumLitClean;
      NumLitRadix = SavedNumLitRadix;
      BoolVal = SavedBoolVal;
      IdentifierStr = SavedIdentifierStr;
      StringVal = SavedStringVal;
      SourceLines = SavedSourceLines;
      g_braceDepth = SavedBraceDepth;
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

  // Arm this function's own local recovery boundary (see
  // RecoveryBoundaryGuard's doc comment above for the full rationale): any
  // TurfError raised from here on -- directly in this file's own parsing,
  // or bubbling up through a transitively nested `import` -- longjmps back
  // to this exact setjmp() call, still well within RecovBoundary/IGuard/
  // LGuard's scope, so their destructors run normally once this function
  // returns (either the ordinary success path below, an ordinary early
  // `return nullptr;` inside the loop, or the `else` branch here).
  g_recoverActive = true;
  if (setjmp(g_recoverJmp) != 0) {
    return nullptr;
  }

  getNextToken();

  std::vector<std::unique_ptr<ExprAST>> Exprs;

  // LANG-002: a file's very first top-level statement may be the
  // `unsafe module;` capability pragma -- must be checked here, before the
  // ordinary loop below, since it's the ONLY point in this function that
  // sees this file's first real token (see ParseLeadingUnsafeModuleDecl()'s
  // own doc comment).
  {
    std::unique_ptr<ExprAST> LeadingUnsafeBlock;
    if (!ParseLeadingUnsafeModuleDecl(LeadingUnsafeBlock) && LeadingUnsafeBlock)
      AppendFlattenedTopLevelDecl(std::move(LeadingUnsafeBlock), Exprs, false);
  }

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
        CurTok == TOK_ERROR || CurTok == TOK_UNSAFE) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok == ';') {
          LogErrorAt(CurLoc, "Unexpected ';'", SLM::ErrorCategory::SYNTAX_ERROR);
          return nullptr;
        }
        // Flattens Expr first if it's a top-level `unsafe { }` block -- see
        // AppendFlattenedTopLevelDecl()'s doc comment (Parser.h) for why.
        AppendFlattenedTopLevelDecl(std::move(Expr), Exprs, false);
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
          LogErrorAt(CurLoc, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR);
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
        LogErrorAt(CurLoc, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR);
        return nullptr;
      }
      getNextToken(); // eat ';'
      Exprs.push_back(std::move(Expr));
    } else {
      // See the comment on the identical change above.
      return nullptr;
    }
  }

  // Only marked "fully imported" on this definite success path -- see the
  // NOTE above (issue #242 follow-up) for why this moved here instead of
  // being inserted eagerly before the parse was even attempted.
  ImportedFiles.insert(Path);

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
// RT-001: exported (no longer file-local) so main.cpp's linkObject() can
// anchor `-lturfrt`'s search path at the exact same root `import` already
// resolves lib/std against, without duplicating this env-var/fallback logic
// a second time -- same reasoning HasRawPointerCapability() further down
// this file gives for its own export (include/Parser.h's declaration site
// has the full note).
std::string TurfInstallRoot() {
  if (const char *Env = std::getenv("TURF_HOME")) {
    if (*Env)
      return std::string(Env);
  }
  llvm::SmallString<128> Root(CompilerExecutablePath);
  llvm::sys::path::remove_filename(Root); // strip the "turf"/"turf.exe" itself
  llvm::sys::path::append(Root, "..");
  return Root.str().str();
}

// True if canonicalized `Path` falls under canonicalized `Root` -- shared
// prefix-match helper behind both IsPathUnderInstallRoot() below and (until
// LANG-002 replaced it) the old lib/std-specific IsPathUnderLibStd(). A path
// that can't be resolved (e.g. a synthesized AST node with an empty
// SourceLocation.Filename) is conservatively NOT considered a match. The
// prefix check requires a path-separator boundary right after `Root` so a
// sibling directory that merely STARTS WITH the same characters (e.g. a
// hypothetical "lib/std2") can never false-match.
static bool IsPathUnderDir(const std::string &Path, const std::string &Root) {
  if (Path.empty())
    return false;
  llvm::SmallString<256> CanonicalPath;
  if (llvm::sys::fs::real_path(Path, CanonicalPath))
    return false;
  llvm::SmallString<256> CanonicalRoot;
  if (llvm::sys::fs::real_path(Root, CanonicalRoot))
    return false;
  std::string P = CanonicalPath.str().str();
  std::string R = CanonicalRoot.str().str();
  if (P.size() < R.size() || P.compare(0, R.size(), R) != 0)
    return false;
  if (P.size() == R.size())
    return true;
  char Sep = P[R.size()];
  return Sep == '/' || Sep == '\\';
}

// LANG-002 (STD-001): true if `Path` (canonicalized) falls under this
// install's resolved root (`TurfInstallRoot()` -- the same anchor `import`
// already uses). Generalizes the old, lib/std-SPECIFIC IsPathUnderLibStd():
// the raw-pointer capability is no longer a `lib/std`-only privilege (any
// file under the install root, e.g. a future `lib/usr` package, may declare
// it too), and -- unlike the mechanism it replaces -- being under the
// install root is no longer SUFFICIENT on its own; see
// HasRawPointerCapability() below for the actual gate.
static bool IsPathUnderInstallRoot(const std::string &Path) {
  return IsPathUnderDir(Path, TurfInstallRoot());
}

// LANG-002/STD-001: files that parsed a leading `unsafe module;` capability
// pragma (ParseLeadingUnsafeModuleDecl() below), keyed by the SAME
// real_path()-canonicalized form HasRawPointerCapability() queries with, so
// registration and lookup can never disagree over path spelling
// (relative-vs-absolute, symlinks, "./"). Re-registering the same file
// across the pre-pass/main-pass/import re-parses this compiler does is
// harmless (std::set::insert on an existing key is a no-op) -- see
// ParseFile()'s own comment on why a file's content can be parsed more than
// once in a single compile.
static std::set<std::string> UnsafeModuleFiles;

static std::string CanonicalOrOriginal(const std::string &Path) {
  llvm::SmallString<256> Canonical;
  if (llvm::sys::fs::real_path(Path, Canonical))
    return Path; // unresolvable (e.g. a synthesized node) -- fall back as-is
  return Canonical.str().str();
}

// LANG-002 (STD-001): the raw-pointer capability's trust boundary, replacing
// the old, purely path-based IsPathUnderLibStd() (issue #238). A privilege
// that used to be invisible in the file that used it, ungreppable, and
// unavailable to a user writing legitimate FFI bindings is now: DECLARED IN
// SOURCE (a first-line `unsafe module;`, ParseLeadingUnsafeModuleDecl()
// below) and, for that declaration to actually be honored, EITHER the file
// is under the resolved install root (lib/std/**, unconditionally -- a
// plain dev build needs no extra flag) OR the `--allow-unsafe-modules` CLI
// flag was passed (g_AllowUnsafeModules, Lexer.h) -- a user's own FFI
// bindings get the identical capability lib/std has, just opt-in rather
// than ambient. Consulted by TypeChecker::isUnsafeGateExempt() and
// checkNoRawPointersInSignature() (TypeCheckerCore.cpp) via this exported
// wrapper so the TypeChecker doesn't need to depend on llvm::sys::fs itself
// (its header is deliberately kept LLVM-free).
bool HasRawPointerCapability(const std::string &Path) {
  if (Path.empty())
    return false;
  if (!UnsafeModuleFiles.count(CanonicalOrOriginal(Path)))
    return false;
  return IsPathUnderInstallRoot(Path) || g_AllowUnsafeModules;
}

// LANG-002: called as the very first thing after a top-level parse loop's
// initial getNextToken() (ParseFile() below; main.cpp's two mirrored
// loops), i.e. only ever sees a file's FIRST real token. Checks for and
// consumes a leading `unsafe module;` capability pragma.
//
// This parser has exactly one token of lookahead, so telling `unsafe
// module;` apart from an ordinary top-level `unsafe { ... }` block requires
// consuming 'unsafe' FIRST and looking at what follows -- there's no way to
// peek past it non-destructively. Two outcomes follow:
//   - It WAS `unsafe module;`: consumes 'module' and ';', registers the
//     capability for the CURRENT SourceFilename, and returns true. The
//     caller's own top-level loop should just proceed -- there's no AST
//     node to append for a pragma.
//   - CurTok was TOK_UNSAFE but this ISN'T that shape (a genuine top-level
//     `unsafe { struct/fn/... }`): 'unsafe' has already been consumed, so
//     this finishes parsing the block itself via the shared
//     ParseUnsafeBlockBody() helper (see its own doc comment,
//     ParserStmt.cpp) and hands the result back through `OutBlock`, exactly
//     as if the caller had called ParseExpression() itself. Returns false.
//   - CurTok is anything else: no-op. Returns false, `OutBlock` untouched.
bool ParseLeadingUnsafeModuleDecl(std::unique_ptr<ExprAST> &OutBlock) {
  if (CurTok != TOK_UNSAFE)
    return false;
  SourceLocation KeywordLoc = CurLoc;
  getNextToken(); // consume 'unsafe'
  if (CurTok == TOK_IDENTIFIER && IdentifierStr == "module") {
    getNextToken(); // consume 'module'
    if (CurTok != ';') {
      SyntaxError(CurLoc, "Expected ';' after 'unsafe module'").raise();
      return true; // still: not a real unsafe-block AST for the caller
    }
    getNextToken(); // consume ';'
    UnsafeModuleFiles.insert(CanonicalOrOriginal(SourceFilename));
    return true;
  }
  OutBlock = ParseUnsafeBlockBody(KeywordLoc);
  return false;
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

// Pure "where would this module resolve to on disk" lookup -- the same
// four-step search order ParseModuleImportByName() below uses (stdlib ->
// installed user package -> ImportingFile's own directory -> cwd), but
// without ever calling ParseFile(): no ImportedFiles/ImportStack mutation, no
// diagnostics, no circular-import bookkeeping. For tooling that needs to
// answer "which file does module X live in" without compiling it (the LSP's
// go-to-definition, which used to hardcode a flat `lib/std/<name>.tr` path
// and broke the moment stdlib modules were reorganized into subfolders --
// see docs/architecture/modules_and_scope_resolution.md Part 2.4). Sharing
// TurfInstallRoot()/IndexModuleDir() with the real import path means this
// can never disagree with what a real `import ModuleName;` would actually
// resolve to, the same guarantee ParsePreludeImports()'s self-import check
// above already depends on.
//
// `ImportingFile`, if non-empty, is the path of the file that would contain
// the `import` -- used for step 3 (same-directory relative lookup), mirroring
// ParseModuleImportByName()'s use of the live SourceFilename for that same
// step. Pass "" to skip step 3 (e.g. when the importing file isn't a real
// on-disk path, or is unknown to the caller).
//
// Returns "" if no `<name>.tr` exists at any search location. Existence is
// checked with llvm::sys::fs::exists(), not by attempting a parse -- a
// module that exists but has a syntax error still resolves to a real path
// here (this function answers "where", not "is it valid").
std::string ResolveModuleFile(const std::string &ModuleName,
                              const std::string &ImportingFile) {
  std::string Root = TurfInstallRoot();

  const auto &StdIndex = IndexModuleDir(Root, "std");
  if (auto It = StdIndex.find(ModuleName); It != StdIndex.end())
    return It->second;

  const auto &UsrIndex = IndexModuleDir(Root, "usr");
  if (auto It = UsrIndex.find(ModuleName); It != UsrIndex.end())
    return It->second;

  if (!ImportingFile.empty()) {
    llvm::SmallString<256> SrcRelPath(ImportingFile);
    llvm::sys::path::remove_filename(SrcRelPath);
    llvm::sys::path::append(SrcRelPath, ModuleName + ".tr");
    if (llvm::sys::fs::exists(SrcRelPath))
      return SrcRelPath.str().str();
  }

  std::string CwdCandidate = ModuleName + ".tr";
  if (llvm::sys::fs::exists(CwdCandidate))
    return CwdCandidate;

  return "";
}

// See declaration in Parser.h. Sibling of ResolveModuleFile(): same search
// order, but enumerating every name instead of resolving one.
std::vector<std::string> ListAvailableModules(const std::string &ImportingFile) {
  std::string Root = TurfInstallRoot();

  // std::map (not unordered_map) so insertion here is naturally sorted and
  // de-duplicated by name -- a user module shadowing a stdlib name (step 3)
  // collapses to one entry rather than listing "net" twice.
  std::map<std::string, bool> Names; // value unused, std::set would do but
                                     // keeps this symmetric with IndexModuleDir's map style

  for (const auto &Entry : IndexModuleDir(Root, "std"))
    Names.emplace(Entry.first, true);
  for (const auto &Entry : IndexModuleDir(Root, "usr"))
    Names.emplace(Entry.first, true);

  if (!ImportingFile.empty()) {
    llvm::SmallString<256> Dir(ImportingFile);
    llvm::sys::path::remove_filename(Dir);
    if (Dir.empty())
      Dir = ".";
    std::error_code EC;
    for (llvm::sys::fs::directory_iterator I(Dir.str(), EC), E; !EC && I != E;
         I.increment(EC)) {
      if (I->type() != llvm::sys::fs::file_type::regular_file)
        continue;
      if (llvm::sys::path::extension(I->path()) != ".tr")
        continue;
      // Skip the importing file itself -- it can't meaningfully import
      // itself, and ParseModuleImportByName()'s step 3 would only ever
      // find it as a same-directory sibling of some OTHER file anyway.
      if (llvm::sys::fs::equivalent(I->path(), ImportingFile))
        continue;
      Names.emplace(llvm::sys::path::stem(I->path()).str(), true);
    }
  }

  std::vector<std::string> Result;
  Result.reserve(Names.size());
  for (const auto &Entry : Names)
    Result.push_back(Entry.first);
  return Result;
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
    LogErrorAt(CurLoc, "Expected module name after 'import'", SLM::ErrorCategory::DECLARATION);
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
          LogErrorAt(CurLoc, "Expected ']' after '*' in import list", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
        getNextToken(); // eat ']'
        Mode = ImportMode::Wildcard;
      } else {
        Mode = ImportMode::SymbolList;
        while (CurTok != ']' && CurTok != TOK_EOF) {
          if (CurTok != TOK_IDENTIFIER) {
            LogErrorAt(CurLoc, "Expected a symbol name in import list", SLM::ErrorCategory::DECLARATION);
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
          LogErrorAt(CurLoc, "Expected ']' to close import list", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
        getNextToken(); // eat ']'
        if (RequestedSymbols.empty()) {
          LogErrorAt(CurLoc, "Expected at least one symbol name in import list", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
      }
    } else if (CurTok == TOK_IDENTIFIER) {
      Mode = ImportMode::Symbol;
      RequestedSymbols.push_back({IdentifierStr, CurLoc});
      getNextToken();
    } else {
      LogErrorAt(CurLoc,
                 "Expected a symbol name or '[' after '::' in import", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  }

  return ParseModuleImportByName(ModuleName, Loc, NameLoc, Mode, std::move(RequestedSymbols));
}
