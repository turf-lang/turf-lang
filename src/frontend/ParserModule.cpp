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

#include "frontend/Parser.h"
#include "frontend/AST.h"
#include "frontend/ASTChildWalker.h"
#include "analysis/Algorithms.h"
#include "sema/Builtins.h"
#include "codegen/Codegen.h"
#include "diagnostics/Errors.h"
#include "frontend/Lexer.h"
#include "diagnostics/SuggestionIndex.h"
#include "resolve/SymbolTable.h"
#include "frontend/ParserInternal.h"
#include "tooling/PackageGraph.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static std::unordered_set<std::string> ImportedFiles;

// LEC-001 stage 4 (FREEZE.md 6.16, F-PKG-2): the file currently being
// parsed's `import` spellings whose module identity differs from the
// spelling -- a dependency package's modules, keyed `<package>/<path>` (see
// ModuleCandidate::Key). The file writes `util::f()`; every later pass must
// see the identity, because two packages may each have a `util`.
//
// Rewriting at parse time, in the three places a module qualifier is read
// (ModuleKeyFor()), is what keeps the Resolver, TypeChecker, Monomorphizer
// and Codegen unchanged: they already treat a module name as an identity, so
// giving them an identity is the whole fix. The alternative -- every
// module-name lookup downstream consulting a per-file alias table -- touches
// every `hasModule()`/type-identity site and is exactly the kind of change
// that misses one.
//
// Per file: ParseFile() saves the importer's map and starts the imported
// file with an empty one (LexerStateGuard), and ParsePreludeImports() clears
// it at the start of each top-level parse pass.
static std::map<std::string, std::string> CurrentFileModuleAliases;

const std::string &ModuleKeyFor(const std::string &Spelled) {
  auto It = CurrentFileModuleAliases.find(Spelled);
  return It == CurrentFileModuleAliases.end() ? Spelled : It->second;
}

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

// FDN-003: the module whose file is being parsed right now ("" for the main
// program file). ImportStack's top frame IS that module for the whole of its
// own ParseFile() call -- the frame is pushed before the file's first token
// is read and popped only after its last -- so this needs no state of its own.
std::string CurrentParseModule() {
  return ImportStack.empty() ? std::string() : ImportStack.back().ModuleName;
}

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

// SMP-001: the source text of files an embedder has open but not saved,
// keyed by CanonicalOrOriginal() path. Consulted by ParseFile() before
// disk, so an import of an edited module sees the editor's text. Set only
// by the query facade (src/tooling/AnalysisFacade.cpp), inside its own
// PushParserCompileState()/PopParserCompileState() bracket -- empty for
// every CLI compile.
static std::map<std::string, std::string> SourceOverlays;

// SMP-001: AnalysisOptions::InstallRoot. Empty means TurfInstallRoot()'s
// ordinary TURF_HOME / executable-relative lookup.
static std::string InstallRootOverride;

static std::string CanonicalOrOriginal(const std::string &Path);

// Everything in this file that one compile accumulates and a second,
// isolated compile in the same process must neither see nor disturb. A
// stack, not one saved slot, so an isolated compile can itself run inside
// another (the SLM Sandbox validates a fix while the real compile that
// raised the diagnostic is still mid-flight).
//
// UnsafeTopLevelDecls and UnsafeModuleFiles were once left out of the older
// Push/PopImportCycleState() pair this replaces, and nothing else reset
// them between compiles either: a file analysed once with `unsafe module;`
// kept the raw-pointer capability in every later analysis after the pragma
// was deleted, and UnsafeTopLevelDecls kept raw pointers to freed AST
// nodes, which a later compile's nodes could be allocated on top of.
struct ParserCompileState {
  std::vector<std::unique_ptr<ExprAST>> ParsedTopLevelASTs;
  std::unordered_set<std::string> ImportedFiles;
  std::vector<ImportStackFrame> ImportStack;
  std::unordered_set<const ExprAST *> UnsafeTopLevelDecls;
  std::set<std::string> UnsafeModuleFiles;
  std::map<std::string, std::string> SourceOverlays;
  std::string InstallRootOverride;
  std::unordered_set<std::string> UnparsedFunctionNames;
};
static std::vector<ParserCompileState> SavedParserCompileStates;

// See NoteFunctionParseStarted() (Parser.h). Holds a name from the moment
// ParseFuncDef() reads it until the definition parses, so a definition that
// fails anywhere after its name -- a reported error returning nullptr, or a
// longjmp out of the body -- leaves its name here.
static std::unordered_set<std::string> UnparsedFunctionNames;

void NoteFunctionParseStarted(const std::string &Name) {
  UnparsedFunctionNames.insert(Name);
}

void NoteFunctionParsed(const std::string &Name) {
  UnparsedFunctionNames.erase(Name);
}

bool FunctionFailedToParse(const std::string &Name) {
  return UnparsedFunctionNames.count(Name) != 0;
}

static void ClearPackageSourceIndex(); // defined beside the cache it empties

void PushParserCompileState() {
  ClearPackageSourceIndex();
  SavedParserCompileStates.push_back(
      {std::move(ParsedTopLevelASTs), std::move(ImportedFiles),
       std::move(ImportStack), std::move(UnsafeTopLevelDecls),
       std::move(UnsafeModuleFiles), std::move(SourceOverlays),
       std::move(InstallRootOverride), std::move(UnparsedFunctionNames)});
  ParsedTopLevelASTs.clear();
  ImportedFiles.clear();
  ImportStack.clear();
  UnsafeTopLevelDecls.clear();
  UnsafeModuleFiles.clear();
  SourceOverlays.clear();
  InstallRootOverride.clear();
  UnparsedFunctionNames.clear();
}

void PopParserCompileState() {
  ClearPackageSourceIndex();
  auto &Saved = SavedParserCompileStates.back();
  ParsedTopLevelASTs = std::move(Saved.ParsedTopLevelASTs);
  ImportedFiles = std::move(Saved.ImportedFiles);
  ImportStack = std::move(Saved.ImportStack);
  UnsafeTopLevelDecls = std::move(Saved.UnsafeTopLevelDecls);
  UnsafeModuleFiles = std::move(Saved.UnsafeModuleFiles);
  SourceOverlays = std::move(Saved.SourceOverlays);
  InstallRootOverride = std::move(Saved.InstallRootOverride);
  UnparsedFunctionNames = std::move(Saved.UnparsedFunctionNames);
  SavedParserCompileStates.pop_back();
}

void SetSourceOverlay(const std::string &Path, const std::string &Text) {
  SourceOverlays[CanonicalOrOriginal(Path)] = Text;
}

void SetInstallRootOverride(const std::string &Root) {
  InstallRootOverride = Root;
}

// The overlay text for Path, or null when the file should be read from disk.
static const std::string *FindSourceOverlay(const std::string &Path) {
  if (SourceOverlays.empty())
    return nullptr;
  auto It = SourceOverlays.find(CanonicalOrOriginal(Path));
  return It == SourceOverlays.end() ? nullptr : &It->second;
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
  // Every top-level declaration passes through here before any pass recurses
  // over it (the sandbox clone just below is the first). A tree deeper than
  // the parser's own nesting limit was built from pieces that were each within
  // it -- see AstDepthProbe -- and would overflow the stack in a later pass.
  // Reject it as the nesting error it is. The tree is released, not destroyed:
  // freeing it recurses exactly as deep as the passes would have.
  auto TooDeep = [](ExprAST *Node) {
    return AstDepthExceeds(Node, MaxParseRecursionDepth);
  };
  // Point at the declaration's own line when it has one; the parser has
  // already moved past its end, so CurLoc is the fallback.
  auto WhereIs = [](ExprAST *Node) {
    if (auto *F = dynamic_cast<FuncDefExprAST *>(Node))
      return F->getLoc();
    return CurLoc;
  };
  if (TooDeep(AST.get())) {
    LogErrorAt(WhereIs(AST.get()), ErrCode::NestingTooDeep,
               "Expression nested too deeply to parse."
               "\n  Hint: split this declaration into smaller parts.",
               SLM::ErrorCategory::SYNTAX_ERROR);
    (void)AST.release();
    return;
  }
  if (auto *UB = dynamic_cast<UnsafeBlockExprAST *>(AST.get())) {
    const SourceLocation BlockLoc = UB->getLoc();
    for (auto &Child : UB->takeChildren()) {
      // A top-level `unsafe { }` only marks the declarations it wraps. A
      // statement in it was spliced into the file's declarations like one,
      // and Codegen then ran it with no enclosing function: `unsafe { mut p:
      // int* = null; }` at the top level was a segmentation fault in
      // VarDeclExprAST::codegen() (turf-fuzz #15). Every other statement at
      // the top level is E118 already.
      if (dynamic_cast<UnsafeBlockExprAST *>(Child.get())) {
        AppendFlattenedTopLevelDecl(std::move(Child), Target, RecordForSandbox);
        continue;
      }
      auto *Vd = dynamic_cast<VarDeclExprAST *>(Child.get());
      const bool IsDecl =
          dynamic_cast<FuncDefExprAST *>(Child.get()) ||
          dynamic_cast<StructDeclExprAST *>(Child.get()) ||
          dynamic_cast<EnumDeclExprAST *>(Child.get()) ||
          dynamic_cast<MachineDeclExprAST *>(Child.get()) ||
          dynamic_cast<ErrorDeclExprAST *>(Child.get()) ||
          dynamic_cast<ExternFuncDeclExprAST *>(Child.get()) ||
          dynamic_cast<ModuleImportExprAST *>(Child.get()) ||
          (Vd && Vd->isExported());
      if (!IsDecl) {
        LogErrorAt(Vd ? Vd->getLoc() : BlockLoc, ErrCode::StatementOutsideFunction,
                   "Statements are not allowed outside of a function, and a "
                   "top-level 'unsafe { }' block holds only declarations. Move "
                   "this code into a function, inside its own 'unsafe { }'.",
                   SLM::ErrorCategory::SYNTAX_ERROR);
        continue;
      }
      if (TooDeep(Child.get())) {
        LogErrorAt(WhereIs(Child.get()), ErrCode::NestingTooDeep,
                   "Expression nested too deeply to parse."
                   "\n  Hint: split this declaration into smaller parts.",
                   SLM::ErrorCategory::SYNTAX_ERROR);
        (void)Child.release();
        continue;
      }
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

static std::unique_ptr<ExprAST> ParseFileDeclarations(const std::string &Path);

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
  //
  // (Recovery is a RecoverUnwind exception now, not a longjmp, so the
  // guards would unwind even without the local boundary; the boundary still
  // decides that a broken import fails alone, not the importing declaration.)
  struct RecoveryBoundaryGuard {
    bool PrevActive;
    RecoveryBoundaryGuard() : PrevActive(g_recoverActive) {}
    ~RecoveryBoundaryGuard() { g_recoverActive = PrevActive; }
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

  // An embedder's unsaved buffer for this file wins over the disk copy
  // (SetSourceOverlay(), SMP-001). Both streams live for the rest of this
  // function, which is as long as LGuard keeps SourceFile pointed at one.
  const std::string *Overlay = FindSourceOverlay(Path);
  std::istringstream OverlayFile(Overlay ? *Overlay : std::string());
  std::ifstream ImportFile;
  if (!Overlay) {
    ImportFile.open(Path);
    if (!ImportFile.is_open())
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
    int SavedCurlyDepth; // LEC-005: g_curlyDepth, saved for the same reason
    // LEC-001 stage 4: the importing file's module aliases. An imported file
    // has its own imports; it must neither see the importer's aliases nor
    // leave its own behind for the importer's remaining tokens.
    std::map<std::string, std::string> SavedModuleAliases;
    // LG-1: tokens the importer lexed ahead of its current one.
    std::vector<LexedToken> SavedLookahead;

    LexerStateGuard() {
      SavedLookahead = takeLookahead();
      SavedModuleAliases = std::move(CurrentFileModuleAliases);
      CurrentFileModuleAliases.clear();
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
      SavedCurlyDepth = g_curlyDepth;
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
      g_curlyDepth = SavedCurlyDepth;
      CurrentFileModuleAliases = std::move(SavedModuleAliases);
      restoreLookahead(std::move(SavedLookahead));
    }
  } LGuard;

  // Set new lexer state
  SourceFile = Overlay ? static_cast<std::istream *>(&OverlayFile) : &ImportFile;
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
    std::istringstream OverlayLines(Overlay ? *Overlay : std::string());
    std::ifstream DiskLines;
    if (!Overlay)
      DiskLines.open(Path);
    std::istream &LineReader =
        Overlay ? static_cast<std::istream &>(OverlayLines) : DiskLines;
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
  // or bubbling up through a transitively nested `import` -- is caught here,
  // and this one import fails.
  g_recoverActive = true;
  try {
    return ParseFileDeclarations(Path);
  } catch (const RecoverUnwind &) {
    return nullptr;
  }
}

// ParseFile()'s parse loop, run inside its recovery point with the lexer
// pointed at Path.
static std::unique_ptr<ExprAST> ParseFileDeclarations(const std::string &Path) {
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
        CurTok == TOK_ERROR || CurTok == TOK_UNSAFE || CurTok == TOK_PUB) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok == ';') {
          LogErrorAt(CurLoc, ErrCode::UnexpectedToken, "Unexpected ';'", SLM::ErrorCategory::SYNTAX_ERROR);
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
      auto Expr = ParseImport();
      if (Expr) {
        if (CurTok != ';') {
          LogErrorAt(CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR, MissingSemicolonHints());
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
        LogErrorAt(CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR, MissingSemicolonHints());
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
// `build/turfc` still finds the sibling `lib/` directory with zero setup
// (this is the previous, only, behavior -- kept as a fallback rather than
// removed so CI/local dev builds don't all need TURF_HOME exported just to
// run the test suite). See docs/architecture/modules_and_scope_resolution.md
// Part 2.4 for the full rationale.
// RT-001: exported (no longer file-local) so main.cpp's linkObject() can
// anchor `-lturfrt`'s search path at the exact same root `import` already
// resolves lib/std against, without duplicating this env-var/fallback logic
// a second time -- same reasoning HasRawPointerCapability() further down
// this file gives for its own export (src/frontend/Parser.h's declaration site
// has the full note).
// Set by the embedding program's main() (the CLI's runCompilerMain(); the
// query facade fills it in for any embedder that left it empty). Defined in
// the library rather than in main.cpp, where it used to be: every other
// program linking turfcore then had to define it itself or fail to link.
std::string CompilerExecutablePath;

std::string TurfInstallRoot() {
  if (!InstallRootOverride.empty())
    return InstallRootOverride;
  if (const char *Env = std::getenv("TURF_HOME")) {
    if (*Env)
      return std::string(Env);
  }
  llvm::SmallString<128> Root(CompilerExecutablePath);
  llvm::sys::path::remove_filename(Root); // strip the "turfc"/"turfc.exe" itself
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

// OVH-012: exported (Parser.h) so Lexer.cpp's DiagnosticEngine::flushAll()
// can decide whether a warning's location is inside the standard library
// itself, without linking against llvm::sys::fs directly -- same reasoning
// HasRawPointerCapability()'s own export note gives. Deliberately narrower
// than IsPathUnderInstallRoot() (this file, static): a `lib/usr` user
// package lives under the same install root but is the USER's own code, not
// the standard library, so it must never have its warnings hidden by
// default the way a `lib/std` warning is. Named IsLibStdWarningLocation(),
// NOT IsPathUnderLibStd() -- see that name's own history two functions
// above (HasRawPointerCapability()'s doc comment) and Parser.h's doc
// comment on this function for why reusing it would be a mistake:
// scripts/check/check_boundary.sh greps for that exact identifier as a dedicated
// regression guard, and this function is not the capability check it
// guards against.
bool IsLibStdWarningLocation(const std::string &Path) {
  llvm::SmallString<160> LibStd(TurfInstallRoot());
  llvm::sys::path::append(LibStd, "lib", "std");
  return IsPathUnderDir(Path, LibStd.str().str());
}


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
  if (IsPathUnderInstallRoot(Path))
    return true;
  // LEC-001 stage 4 (FREEZE.md 6.16, F-PKG-4): in a package build the
  // capability is per package -- the graph's allow bit, which the driver sets
  // for the root package and for each dependency the root manifest lists
  // under [unsafe] allow. The global --allow-unsafe-modules would otherwise
  // grant it to every dependency at once, which is exactly the consent the
  // per-package rule exists to make explicit. So in a package build that
  // flag is not consulted at all.
  if (g_PackageGraph.Active) {
    const PackageInfo *P = g_PackageGraph.packageOf(Path);
    return P && P->AllowUnsafe;
  }
  return g_AllowUnsafeModules;
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
      SyntaxError(CurLoc, ErrCode::MissingSemicolon, "Expected ';' after 'unsafe module'").raise();
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

namespace {

// Where a module candidate was found. Single-file mode uses the first four,
// in the search order the resolver used before LEC-001 stage 3, kept so the
// candidate list reads the same way the old precedence did. A package-graph
// build (stage 4) uses Stdlib, OwnModule and Dependency only.
enum class ModuleRootKind { Stdlib, Installed, Sibling, Cwd, OwnModule, Dependency };

struct ModuleCandidate {
  ModuleRootKind Kind;
  // Spelled exactly as the pre-stage-3 first-hit search spelled it (an index
  // path, `<importer dir>/<m>.tr`, or a bare `<m>.tr` for the cwd). This
  // string becomes ModuleImportExprAST::ResolvedPath, the identity key of
  // ImportedFiles/ImportStack, so it must not change spelling just because
  // the search stopped short-circuiting.
  std::string Path;
  // The module's identity (FREEZE.md 6.16 F-PKG-2: a module is (package,
  // path under src/)). Empty means "the spelled name", which is right for the
  // stdlib, for single-file mode and for the root package's own modules --
  // none of them can collide, because a collision among them is already an
  // E090 at the import. A dependency package's module gets
  // `<package>/<path under src/ without .tr>` instead: two packages may each
  // have an internal `util`, and the root may have its own, and those must be
  // three modules with three sets of types. `/` cannot appear in a spelled
  // module name, so no key can ever equal one.
  std::string Key;
  // For a Dependency / OwnModule candidate: the package it belongs to.
  std::string Package;
};

} // namespace

bool IsStdlibModuleName(const std::string &Name) {
  return IndexModuleDir(TurfInstallRoot(), "std").count(Name) > 0;
}

// Every `.tr` file under a package's src/, by stem, keeping EVERY path for a
// stem: within a package, `import util;` means `src/**/util.tr`, and two such
// files are an ambiguity (E090), not "the first one the directory walk
// happened to return". Cached per root for the compiler invocation, like
// IndexModuleDir(), since every import in the package consults it -- and
// dropped at the start and end of every isolated compile
// (Push/PopParserCompileState), because a language server's process outlives
// the files it analyses: a module created after the first analysis must be
// importable by the next one. (IndexModuleDir() is not dropped: lib/std does
// not change under a running tool.)
static std::map<std::string, std::map<std::string, std::vector<std::string>>>
    PackageSourceIndexCache;

static void ClearPackageSourceIndex() { PackageSourceIndexCache.clear(); }

static const std::map<std::string, std::vector<std::string>> &
IndexPackageSources(const std::string &SrcRoot) {
  auto &Cache = PackageSourceIndexCache;
  auto Cached = Cache.find(SrcRoot);
  if (Cached != Cache.end())
    return Cached->second;
  std::map<std::string, std::vector<std::string>> Index;
  std::error_code EC;
  for (llvm::sys::fs::recursive_directory_iterator I(SrcRoot, EC), E; !EC && I != E;
       I.increment(EC)) {
    if (I->type() == llvm::sys::fs::file_type::regular_file &&
        llvm::sys::path::extension(I->path()) == ".tr")
      Index[llvm::sys::path::stem(I->path()).str()].push_back(I->path());
  }
  for (auto &KV : Index)
    std::sort(KV.second.begin(), KV.second.end()); // deterministic candidate order
  return Cache.emplace(SrcRoot, std::move(Index)).first->second;
}

// `<package>/<path under src/ without .tr>`, the identity of a dependency
// package's module (see ModuleCandidate::Key).
static std::string DependencyModuleKey(const PackageInfo &Pkg, const std::string &Path) {
  llvm::SmallString<256> Canonical;
  std::string P = llvm::sys::fs::real_path(Path, Canonical) ? Path : Canonical.str().str();
  std::string Rel = P.substr(std::min(P.size(), Pkg.SrcRoot.size() + 1));
  if (Rel.size() > 3 && Rel.compare(Rel.size() - 3, 3, ".tr") == 0)
    Rel.resize(Rel.size() - 3);
  std::replace(Rel.begin(), Rel.end(), '\\', '/');
  return Pkg.Name + "/" + Rel;
}

// A package-graph build (LEC-001 stage 4, F-PKG-2): from a module of package
// P, `import m;` may name (a) P's own `src/**/m.tr`, (b) the root module of a
// DIRECT dependency of P named m, or (c) the stdlib module m -- and nothing
// else. lib/usr, the importing file's directory and the cwd are not searched:
// a package build depends on the graph and nothing ambient.
//
// REL-002: a file under the root package's tests/ (only a `turfc --test` graph
// names that directory) also sees the helper modules beside it in tests/. The
// reverse is not true -- a src/ module never imports a test helper -- and a
// helper whose name matches a src/ module is ambiguous like any other pair.
// A helper is a `*_module.tr` file (the runner never runs one); every other
// file in tests/ is a test, never imported, so tests/route.tr does not compete
// with src/route.tr for `import route;`.
static bool IsTestHelperName(llvm::StringRef ModuleName) {
  return ModuleName.ends_with("_module");
}

static std::vector<ModuleCandidate>
CollectPackageModuleCandidates(const std::string &ModuleName, const PackageInfo &P,
                               const std::string &ImportingFile) {
  std::vector<ModuleCandidate> Found;
  const bool IsRoot = P.Name == g_PackageGraph.Root;

  const auto &StdIndex = IndexModuleDir(TurfInstallRoot(), "std");
  if (auto It = StdIndex.find(ModuleName); It != StdIndex.end())
    Found.push_back({ModuleRootKind::Stdlib, It->second, "", ""});

  const auto &Own = IndexPackageSources(P.SrcRoot);
  if (auto It = Own.find(ModuleName); It != Own.end())
    for (const auto &Path : It->second)
      Found.push_back({ModuleRootKind::OwnModule, Path,
                       IsRoot ? "" : DependencyModuleKey(P, Path), P.Name});

  if (IsRoot && g_PackageGraph.inTests(ImportingFile) && IsTestHelperName(ModuleName)) {
    const auto &Helpers = IndexPackageSources(P.TestsRoot);
    if (auto It = Helpers.find(ModuleName); It != Helpers.end())
      for (const auto &Path : It->second)
        Found.push_back({ModuleRootKind::OwnModule, Path, "", P.Name});
  }

  if (std::find(P.Deps.begin(), P.Deps.end(), ModuleName) != P.Deps.end()) {
    const PackageInfo *D = g_PackageGraph.find(ModuleName);
    llvm::SmallString<256> RootModule(D->SrcRoot);
    llvm::sys::path::append(RootModule, ModuleName + ".tr");
    Found.push_back({ModuleRootKind::Dependency, RootModule.str().str(),
                     DependencyModuleKey(*D, RootModule.str().str()), D->Name});
  }
  return Found;
}

// Why `import m;` found nothing in a package build although a file for `m`
// exists somewhere in the graph -- the two rules that make a module
// unreachable rather than missing. Returns false when m is simply not there.
static bool ExplainUnreachableModule(const std::string &ModuleName, const PackageInfo &P,
                                     SourceLocation Loc) {
  // A package in the build, but not one P may import: a transitive
  // dependency, or a package only some other package depends on.
  if (const PackageInfo *Q = g_PackageGraph.find(ModuleName)) {
    std::vector<std::string> Requirers;
    for (const auto &Other : g_PackageGraph.Packages)
      if (std::find(Other.Deps.begin(), Other.Deps.end(), ModuleName) != Other.Deps.end())
        Requirers.push_back(Other.Name);
    ModuleNotImportableError::notADirectDependency(Loc, P.Name, Q->Name, Requirers).raise();
    return true;
  }
  // An internal module of some other package: only a package's root module
  // is importable from outside it (FREEZE.md 6.16, F-PKG-2).
  for (const auto &Q : g_PackageGraph.Packages) {
    if (Q.Name == P.Name)
      continue;
    const auto &Idx = IndexPackageSources(Q.SrcRoot);
    auto It = Idx.find(ModuleName);
    if (It == Idx.end())
      continue;
    llvm::SmallString<256> Canonical;
    std::string Shown = llvm::sys::fs::real_path(It->second.front(), Canonical)
                            ? It->second.front()
                            : Canonical.str().str();
    ModuleNotImportableError::internalModule(Loc, ModuleName, P.Name, Q.Name, Shown).raise();
    return true;
  }
  return false;
}

// An import written inside the standard library resolves in the standard
// library only. The stdlib is one package (FREEZE.md 6.16, F-PKG-2: a package
// imports its own modules, its dependencies and the stdlib), and its own
// modules ARE stdlib modules -- so `strings.tr`'s `import list;` must mean
// lib/std's list even when the user's program sits next to a `list.tr` of its
// own. Without this, examining every root (below) would turn the user's
// harmless local list.tr into an ambiguity error raised from inside the
// stdlib, in a file the user never wrote and cannot change.
static bool ImportsResolveInStdlibOnly(const std::string &ImportingFile) {
  return IsLibStdWarningLocation(ImportingFile);
}

// LEC-001 stage 3 (FREEZE.md 6.16, F-PKG-2): EVERY place `import ModuleName;`
// could name, not just the first. Before this, the four roots were tried in
// order and the first hit won, so a local json.tr beside main.tr was silently
// never loaded while the stdlib had a `json` (audit F-19). More than one
// entry in the result is an error for the caller to raise, never a
// precedence to apply.
//
// Deduplicated by canonical path, keeping the first spelling: the test runner
// (and anyone running `turfc foo.tr` from foo.tr's own directory) has the
// importing file's directory EQUAL to the cwd, so steps 3 and 4 find the same
// file twice. Counting spellings instead of files would make every local
// import in that setup a false ambiguity.
//
// `StdlibOnly` restricts the search to lib/std: the prelude (a synthetic
// import of stdlib modules by definition -- there is no import statement the
// user could disambiguate) and imports written inside lib/std itself (see
// ImportsResolveInStdlibOnly()).
static std::vector<ModuleCandidate>
CollectModuleCandidates(const std::string &ModuleName,
                        const std::string &ImportingFile, bool StdlibOnly) {
  std::vector<ModuleCandidate> Found;
  std::vector<std::string> SeenCanonical;
  auto Add = [&](ModuleRootKind Kind, const std::string &Path) {
    // A file never imports itself: tests/route.tr's `import route;` means a
    // module called route, not the file doing the importing.
    if (!ImportingFile.empty() && llvm::sys::fs::equivalent(Path, ImportingFile))
      return;
    llvm::SmallString<256> Canonical;
    std::string Key =
        llvm::sys::fs::real_path(Path, Canonical) ? Path : Canonical.str().str();
    for (const auto &S : SeenCanonical)
      if (S == Key)
        return;
    SeenCanonical.push_back(Key);
    Found.push_back({Kind, Path, "", ""});
  };

  // A package-graph build resolves against the graph only (stage 4). An
  // importer in no package -- only reachable through a path the graph does
  // not describe -- gets the stdlib and nothing ambient.
  if (g_PackageGraph.Active && !StdlibOnly) {
    if (const PackageInfo *P = g_PackageGraph.packageOf(ImportingFile))
      return CollectPackageModuleCandidates(ModuleName, *P, ImportingFile);
    StdlibOnly = true;
  }

  std::string Root = TurfInstallRoot();
  const auto &StdIndex = IndexModuleDir(Root, "std");
  if (auto It = StdIndex.find(ModuleName); It != StdIndex.end())
    Add(ModuleRootKind::Stdlib, It->second);
  if (StdlibOnly)
    return Found;

  const auto &UsrIndex = IndexModuleDir(Root, "usr");
  if (auto It = UsrIndex.find(ModuleName); It != UsrIndex.end())
    Add(ModuleRootKind::Installed, It->second);

  // Issue #19: relative to the importing file's directory, so that
  // `turfc path/to/file.tr` finds sibling modules without a cd first.
  if (!ImportingFile.empty()) {
    llvm::SmallString<256> SrcRelPath(ImportingFile);
    llvm::sys::path::remove_filename(SrcRelPath);
    llvm::sys::path::append(SrcRelPath, ModuleName + ".tr");
    if (llvm::sys::fs::is_regular_file(SrcRelPath))
      Add(ModuleRootKind::Sibling, SrcRelPath.str().str());
  }

  std::string CwdCandidate = ModuleName + ".tr";
  if (llvm::sys::fs::is_regular_file(CwdCandidate))
    Add(ModuleRootKind::Cwd, CwdCandidate);

  return Found;
}

// The human-facing half of a candidate, for AmbiguousModuleError. The path is
// canonicalized (the stdlib index spells paths through the install root,
// which for a dev build is `<repo>/build/../lib/std/...`), then shown as
// `./rel/path` when it is under the cwd: the user's own files are almost
// always there, and the hint repeats the path, so an absolute temp-directory
// path twice per error buries the one part that matters.
static ModuleCandidateView
DescribeModuleCandidate(const ModuleCandidate &C, const std::string &ImportingFile) {
  llvm::SmallString<256> Canonical;
  std::string Shown =
      llvm::sys::fs::real_path(C.Path, Canonical) ? C.Path : Canonical.str().str();
  llvm::SmallString<256> Cwd;
  if (!llvm::sys::fs::current_path(Cwd) && !llvm::sys::fs::real_path(Cwd, Canonical)) {
    std::string Base = Canonical.str().str() + "/";
    if (Shown.compare(0, Base.size(), Base) == 0)
      Shown = "./" + Shown.substr(Base.size());
  }
  switch (C.Kind) {
  case ModuleRootKind::Stdlib:
    return {"standard library", Shown, true};
  case ModuleRootKind::Installed:
    return {"installed package (lib/usr)", Shown, false};
  case ModuleRootKind::Sibling:
    return {"next to " + llvm::sys::path::filename(ImportingFile).str(), Shown, false};
  case ModuleRootKind::Cwd:
    return {"current directory", Shown, false};
  case ModuleRootKind::OwnModule:
    return {"module of package '" + C.Package + "'", Shown, false};
  case ModuleRootKind::Dependency:
    return {"dependency '" + C.Package + "'", Shown, false, true};
  }
  return {"", Shown, false};
}

// Pure "where would this module resolve to on disk" lookup -- the same
// candidate collection ParseModuleImportByName() below uses (stdlib ->
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
// Returns "" if no `<name>.tr` exists at any search location, and also "" if
// more than one does: a real import of an ambiguous name is an error
// (AmbiguousModuleError), not a file, and this must never disagree with what
// a real import resolves to. Existence is checked on disk, not by attempting a
// parse -- a module that exists but has a syntax error still resolves to a
// real path here (this function answers "where", not "is it valid").
std::string ResolveModuleFile(const std::string &ModuleName,
                              const std::string &ImportingFile) {
  auto Candidates = CollectModuleCandidates(
      ModuleName, ImportingFile, ImportsResolveInStdlibOnly(ImportingFile));
  return Candidates.size() == 1 ? Candidates.front().Path : "";
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

  // A package build (the importing file is a file of a package in the active
  // graph) lists what CollectPackageModuleCandidates() would find, and
  // nothing else: the standard library, the package's own modules anywhere
  // under its src/ (and the helpers beside a test file), and its direct
  // dependencies. lib/usr, the importing file's directory and the cwd are not
  // searched there, so they must not be offered either.
  if (g_PackageGraph.Active && !ImportingFile.empty() &&
      !ImportsResolveInStdlibOnly(ImportingFile)) {
    if (const PackageInfo *P = g_PackageGraph.packageOf(ImportingFile)) {
      auto AddSources = [&](const std::string &Dir, bool HelpersOnly) {
        for (const auto &Entry : IndexPackageSources(Dir)) {
          if (HelpersOnly && !IsTestHelperName(Entry.first))
            continue;
          for (const auto &Path : Entry.second) {
            // The importing file cannot import itself.
            if (!llvm::sys::fs::equivalent(Path, ImportingFile)) {
              Names.emplace(Entry.first, true);
              break;
            }
          }
        }
      };
      for (const auto &Entry : IndexModuleDir(Root, "std"))
        Names.emplace(Entry.first, true);
      AddSources(P->SrcRoot, false);
      if (P->Name == g_PackageGraph.Root && g_PackageGraph.inTests(ImportingFile))
        AddSources(P->TestsRoot, true);
      for (const auto &Dep : P->Deps)
        Names.emplace(Dep, true);

      std::vector<std::string> Result;
      Result.reserve(Names.size());
      for (const auto &Entry : Names)
        Result.push_back(Entry.first);
      return Result;
    }
  }

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

// LEC-001 stage 5 (FREEZE.md 6.16, "the one exception to `turfc never looks for
// a manifest`"): when a single-file import fails and the importing file sits
// inside a turf.toml project, name `turf build` in the hint. A project's
// dependencies are visible only to a package build, so the plain "not found"
// message is misleading: the module exists, just not for this invocation.
//
// Existence only: the manifest is never opened, read or parsed here -- what it
// says is turf's business, and a compiler that parsed it would be a second
// manifest reader to keep in step with the CLI's. Returns "" outside a project.
static std::string ProjectManifestHint(const std::string &ImportingFile) {
  llvm::SmallString<256> Dir;
  if (llvm::sys::fs::real_path(ImportingFile, Dir))
    return ""; // unresolvable (e.g. a synthesized node): no location to search from
  llvm::sys::path::remove_filename(Dir);
  while (true) {
    llvm::SmallString<256> Manifest(Dir);
    llvm::sys::path::append(Manifest, "turf.toml");
    llvm::sys::fs::file_status Status;
    if (!llvm::sys::fs::status(Manifest, Status) &&
        Status.type() != llvm::sys::fs::file_type::directory_file) {
      // A test is run by `turf test`, which gives it the project's modules.
      llvm::SmallString<256> Tests(Dir);
      llvm::sys::path::append(Tests, "tests");
      llvm::SmallString<256> File;
      llvm::sys::fs::real_path(ImportingFile, File);
      llvm::StringRef Rel(File);
      if (Rel.consume_front(Tests.str()) && llvm::sys::path::is_separator(Rel.front()))
        return "This file is a test of the TurfLang project at " + Manifest.str().str() +
               ". Run it with `turf test tests" + Rel.str() +
               "`, which gives it the project's modules and dependencies.";
      return "This file is inside a TurfLang project (" + Manifest.str().str() +
             "). Its dependencies are only visible to `turf build`, which "
             "compiles the project with its package graph.";
    }
    llvm::StringRef Parent = llvm::sys::path::parent_path(Dir);
    if (Parent.empty() || Parent == Dir.str())
      return "";
    // Copy first: `Parent` points INTO `Dir`, and assigning a SmallString from
    // a view of itself trips LLVM's aliasing assertion (a crash in assert builds).
    Dir = Parent.str();
  }
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
                        std::vector<ImportedSymbolRef> RequestedSymbols = {},
                        bool StdlibOnly = false) {
  // Every search root is examined (CollectModuleCandidates()): stdlib,
  // installed user package, the importing file's directory, the cwd. Exactly
  // one must hold the module. More than one is E090, raised at the module
  // name -- never "the first root wins", which is how a local json.tr used to
  // be silently shadowed by the stdlib's (LEC-001 stage 3, F-PKG-2).
  //
  // ResolvedPath is the chosen candidate's path, spelled as before -- the
  // identity of "this physical module file" for both circular-import
  // detection (Parser-level) and per-file import scoping (Resolver-level).
  const std::string ImportingFile = SourceFilename;
  auto Candidates = CollectModuleCandidates(
      ModuleName, ImportingFile,
      StdlibOnly || ImportsResolveInStdlibOnly(ImportingFile));

  if (Candidates.empty()) {
    // In a package build a module that exists somewhere in the graph but is
    // out of this package's reach is E091, with the rule that forbids it --
    // "could not find module" would send the user looking for a file that
    // is already there.
    if (g_PackageGraph.Active && !StdlibOnly) {
      if (const PackageInfo *P = g_PackageGraph.packageOf(ImportingFile)) {
        if (ExplainUnreachableModule(ModuleName, *P, NameLoc.Line > 0 ? NameLoc : Loc))
          return nullptr;
        // Genuinely absent: the hint names the three places a package build
        // looks, not single-file mode's "your current directory".
        ModuleNotFoundError::inPackage(Loc, ModuleName, P->Name).raise();
        return nullptr;
      }
    }
    // Single-file mode only: in a package build the graph already decided what
    // is reachable, so a manifest somewhere above is beside the point.
    ModuleNotFoundError(Loc, ModuleName,
                        g_PackageGraph.Active ? "" : ProjectManifestHint(ImportingFile))
        .raise();
    return nullptr;
  }
  if (Candidates.size() > 1) {
    std::vector<ModuleCandidateView> Views;
    for (const auto &C : Candidates)
      Views.push_back(DescribeModuleCandidate(C, ImportingFile));
    AmbiguousModuleError(NameLoc.Line > 0 ? NameLoc : Loc, ModuleName, Views)
        .raise();
    return nullptr;
  }

  const std::string ResolvedPath = Candidates.front().Path;
  // The module's identity: the spelling, except for a dependency package's
  // modules (ModuleCandidate::Key). From here on only the identity is used --
  // it is what the ModuleImportExprAST carries, and what this file's later
  // `util::` qualifiers are rewritten to (ModuleKeyFor()).
  const std::string Identity =
      Candidates.front().Key.empty() ? ModuleName : Candidates.front().Key;
  if (Identity != ModuleName)
    CurrentFileModuleAliases[ModuleName] = Identity;
  size_t DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
  std::unique_ptr<ExprAST> Block = ParseFile(ResolvedPath, Identity, Loc);
  if (!Block) {
    // Found on disk but not parsed. Usually a syntax error inside it, already
    // reported there -- adding "module not found" on top would contradict the
    // real diagnostic. Only an unreadable file (no diagnostic recorded) is
    // reported as not found.
    if (DiagnosticEngine::getDiagnosticCount() == DiagCountBefore)
      ModuleNotFoundError(Loc, ModuleName).raise();
    return nullptr;
  }

  return std::make_unique<ModuleImportExprAST>(Loc, Identity, NameLoc, ResolvedPath,
                                                std::move(Block), Mode,
                                                std::move(RequestedSymbols));
}


// The fixed set of stdlib modules auto-imported into every top-level
// compile (STRING_UFCS_OPERATOR_CHAR_RFC.md §5.3/§7 step 6) -- this list
// IS "prelude.tr" as far as the compiler is concerned; there is no actual
// lib/std/prelude.tr file the compiler reads. Scoped to the top-level file
// only (see main.cpp's two call sites) -- library modules importing each
// other still explicitly import what they need, exactly like today.
//
// STD-001: this is a SPECIFIED list, not an arbitrary hardcode -- it must
// name exactly the `.tr` files under lib/std/core/, no more, no fewer (see
// src/sema/Intrinsics.h §4 and docs/architecture/boundary.md for why that
// correspondence is deliberate). scripts/check/check_boundary.sh's check 2
// enforces this mechanically; update both this array and lib/std/core/
// together, in the same change, or that script fails.
//
// LEC-008 follow-up (owner decision, 2026-10-07): the console, math and
// process-exit modules are part of the prelude too, and their functions are
// callable BARE (`printline("x")`, `abs(-3)`, `exit(1)`), not only as
// `io::printline`. They moved into lib/std/core/ to keep the rule above true.
static const char *const PreludeModuleNames[] = {
    "int", "double", "bool", "char", "strings", "array", "io", "math", "sys"};

// The subset whose public free functions a bare, unqualified call may reach
// when nothing the file declares or explicitly imports has that name. The
// other six are reached through UFCS, operators and `m::` only: their
// function names (`length`, `max` over arrays, `is_even`) read as methods and
// would be wrong as free names.
static const char *const PreludeBareModuleNames[] = {"io", "math", "sys"};

bool IsPreludeModuleName(const std::string &Name) {
  for (const char *Prelude : PreludeModuleNames)
    if (Name == Prelude)
      return true;
  return false;
}

bool IsPreludeBareModule(const std::string &Name) {
  for (const char *Prelude : PreludeBareModuleNames)
    if (Name == Prelude)
      return true;
  return false;
}

const std::vector<std::string> &PreludeBareModuleList() {
  static const std::vector<std::string> All(std::begin(PreludeBareModuleNames),
                                            std::end(PreludeBareModuleNames));
  return All;
}

// OVH-011 (docs/development/changes/OVH-011.md): the entry file's own
// top-level parse (main.cpp's own token-consuming loop, further down this
// same file's call chain -- NOT ParseFile()) never participated in the
// ImportedFiles cache/ImportStack machinery every OTHER file's content goes
// through. So a transitive import that happens to resolve to the exact
// physical file currently being compiled AS the entry point gets a SECOND,
// completely independent full parse of identical content, registering
// every struct/function a second time -- E034 "already declared". The
// concrete, verified case: `strings` (an always-injected prelude module)
// does `import list::List;`; compiling lib/std/collections/list.tr
// directly means that transitive import resolves to the entry file's own
// path.
//
// Called once per parse pass (main.cpp's pre-pass AND its separate
// mandatory pass -- ImportedFiles is cleared between them by
// ClearParsedTopLevelASTs(), so each pass needs its own claim), immediately
// after that pass's own ParsePreludeImports() call and BEFORE the entry
// file's own top-level declaration loop starts. Two outcomes:
//
//   - The entry path is ALREADY in ImportedFiles: a transitive import
//     during THIS PASS's prelude injection already reached it and parsed
//     it for real, exactly like any ordinary first-time import (nothing
//     about that parse changes). Returns true -- the caller must SKIP its
//     own top-level loop entirely: every declaration this file has is
//     already present, correctly registered under whatever module name
//     reached it (e.g. "list"); re-parsing here would only duplicate them.
//   - The entry path is NOT yet in ImportedFiles (the ordinary case: an
//     arbitrary user program, or a lib/std module whose prelude chain
//     never happens to reach it). Registers it now, before the caller's
//     own top-level loop runs, so a LATER self-reference introduced by one
//     of the entry file's OWN import statements (a companion module
//     importing the entry back -- see tests/22_prelude/
//     entry_file_reimported_transitively.tr) becomes an ordinary
//     already-imported cache hit too, instead of a second, independent
//     re-parse from within that companion's own ParseFile() call. Returns
//     false -- the caller proceeds normally.
//
// Also registers the same physical file's stdlib/usr-index alias (e.g.
// StdIndex["list"]) when one exists and matches by real_path -- a
// transitive import reaches this file through the MODULE SEARCH PATH, not
// by repeating the entry's own command-line spelling, so the two strings
// essentially never match as plain text without this (mirrors
// ParsePreludeImports()'s own self-check just below, which needs the
// identical canonicalization for the identical reason).
bool ClaimEntryFileImportSlot(const std::string &EntryPath) {
  if (EntryPath.empty())
    return false;

  std::string StdlibAlias;
  llvm::SmallString<256> CanonicalEntry;
  if (!llvm::sys::fs::real_path(EntryPath, CanonicalEntry)) {
    std::string ModuleGuess = llvm::sys::path::stem(EntryPath).str();
    std::string Root = TurfInstallRoot();
    for (const char *Subdir : {"std", "usr"}) {
      const auto &Index = IndexModuleDir(Root, Subdir);
      auto It = Index.find(ModuleGuess);
      if (It == Index.end())
        continue;
      llvm::SmallString<256> CanonicalCandidate;
      if (!llvm::sys::fs::real_path(It->second, CanonicalCandidate) &&
          CanonicalCandidate == CanonicalEntry) {
        StdlibAlias = It->second;
        break;
      }
    }
  }

  if (ImportedFiles.count(EntryPath) ||
      (!StdlibAlias.empty() && ImportedFiles.count(StdlibAlias)))
    return true;

  ImportedFiles.insert(EntryPath);
  if (!StdlibAlias.empty())
    ImportedFiles.insert(StdlibAlias);
  return false;
}

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

  // LEC-001 stage 4: this runs once at the start of every top-level parse
  // pass, in both drivers (main.cpp's two passes, the analysis facade), so it
  // is where the entry file starts with no module aliases -- the previous
  // pass's, or a previous analysis's program's, must not rewrite this one's
  // qualifiers.
  CurrentFileModuleAliases.clear();

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

    // StdlibOnly: the prelude is stdlib modules by definition, and there is
    // no import statement for the user to disambiguate -- a local strings.tr
    // must not break every program in its directory.
    auto AST = ParseModuleImportByName(Name, Loc, /*NameLoc=*/Loc, ImportMode::Bare,
                                       {}, /*StdlibOnly=*/true);
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
    LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected module name after 'import'", SLM::ErrorCategory::DECLARATION);
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
          LogErrorAt(CurLoc, ErrCode::UnclosedDelimiter, "Expected ']' after '*' in import list", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
        getNextToken(); // eat ']'
        Mode = ImportMode::Wildcard;
      } else {
        Mode = ImportMode::SymbolList;
        while (CurTok != ']' && CurTok != TOK_EOF) {
          if (CurTok != TOK_IDENTIFIER) {
            LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected a symbol name in import list", SLM::ErrorCategory::DECLARATION);
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
          LogErrorAt(CurLoc, ErrCode::UnclosedDelimiter, "Expected ']' to close import list", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
        getNextToken(); // eat ']'
        if (RequestedSymbols.empty()) {
          LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected at least one symbol name in import list", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
      }
    } else if (CurTok == TOK_IDENTIFIER) {
      Mode = ImportMode::Symbol;
      RequestedSymbols.push_back({IdentifierStr, CurLoc});
      getNextToken();
    } else {
      LogErrorAt(CurLoc, ErrCode::ExpectedName,
                 "Expected a symbol name or '[' after '::' in import", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  }

  return ParseModuleImportByName(ModuleName, Loc, NameLoc, Mode, std::move(RequestedSymbols));
}
