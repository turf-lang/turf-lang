#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include <memory>

// Expose all variables and functions to other files
extern int CurTok;
int getNextToken();
void InitializePrecedence();

// Both InitializePrecedence() (this file, defined ParserCore.cpp -- populates
// the file-local BinopPrecedence table ParseBinOpRHS() depends on for every
// binary expression) and InitializeSymbolTable() (SymbolTable.h, defined
// SymbolTable.cpp -- allocates GlobalSymbolTable, which ParseStructDecl()
// registers into and TypeChecker::visitMethodCall() reads back from) are
// load-bearing for parse/typecheck CORRECTNESS, not just nice-to-haves, and
// carry no built-in enforcement: skipping either doesn't crash, it silently
// mis-parses the first binary expression anywhere in the source (precedence)
// or fails every instance method call with a misleading "only struct
// instances have methods" diagnostic (symbol table) -- see issue #231. Any
// embedder driving the parser/resolver/typechecker outside main.cpp's own
// pipeline (an LSP, a REPL, a unit test) MUST call this once before parsing
// anything. Both underlying calls are individually idempotent (safe to call
// more than once per process -- see their own definitions), so this is too.
void InitializeCompilerGlobals();

// A token that legitimately starts a new top-level declaration (or ends the
// previous statement). TOK_AT is deliberately excluded: '@' can never
// successfully start anything any more (the decorator syntax it used to
// introduce is gone, see ParsePrimaryBase()'s TOK_AT case), so a failed
// construct is guaranteed to have made ZERO progress past its own leading
// '@' -- treating '@' as a safe place to stop would make the "already at a
// boundary, don't skip further" guard in SkipToNextTopLevelBoundary() fire
// before any token is ever consumed, looping forever on the very same '@'.
bool IsTopLevelBoundaryToken(int Tok);

// Panic-mode recovery: after a top-level statement's parse fails, skip
// tokens forward (tracking '{'/'('/'[' nesting via g_braceDepth so a nested
// boundary-shaped token doesn't stop the skip early) until back at depth 0
// AND sitting on a real top-level boundary. Used by main.cpp's own
// per-statement recovery loops and by Sandbox.cpp's incremental parse passes
// (see issue #228) so both share one resync implementation instead of two
// that can drift apart.
void SkipToNextTopLevelBoundary();

std::unique_ptr<ExprAST> ParseFile(const std::string &Path,
                                   const std::string &ModuleName = "",
                                   SourceLocation ImportLoc = SourceLocation{});
std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<ExprAST> Parse();

// Synthesizes an `import <name>;` AST node for each of the fixed prelude
// module names (int/double/bool/char/strings/array -- see
// STRING_UFCS_OPERATOR_CHAR_RFC.md §5.3/§7 step 6), as if the user had
// written them as the literal first lines of the top-level file. Called by
// main.cpp at the front of BOTH parse passes -- NOT used for library
// modules importing each other, which still explicitly import what they
// need. Aborts with an internal compiler error if a prelude module fails
// to load (these are compiler-shipped files expected to always exist).
std::vector<std::unique_ptr<ExprAST>> ParsePreludeImports(SourceLocation Loc);

// True if @p Path (canonicalized) falls under this install's `lib/std`
// tree -- the `unsafe { }` gate's trust boundary (issue #238). See its
// definition in ParserModule.cpp for the full rationale; exported here
// (rather than kept file-local like TurfInstallRoot()) specifically so
// TypeChecker.cpp's gate checks can call it without linking against
// llvm::sys::fs directly.
bool IsPathUnderLibStd(const std::string &Path);

// Resolves a bare module name (e.g. "net") to the physical `.tr` file a real
// `import net;` would load -- the same stdlib -> usr-package ->
// ImportingFile's-directory -> cwd search order ParseModuleImportByName()
// uses, but as a pure lookup with no parsing and no ImportedFiles/diagnostic
// side effects. See its definition in ParserModule.cpp for the full
// rationale (built for tooling -- an LSP's go-to-definition -- that needs
// "where does this module live" without compiling it). Returns "" if the
// module can't be found anywhere in the search order.
std::string ResolveModuleFile(const std::string &ModuleName,
                              const std::string &ImportingFile = "");

// Lists every module name resolvable via the same search order
// ResolveModuleFile()/ParseModuleImportByName() use: stdlib (lib/std/**),
// then installed user packages (lib/usr/**), then any sibling ".tr" file
// next to @p ImportingFile (skipping ImportingFile itself). Built for
// tooling -- an LSP's `import <cursor>` completion needs the full list of
// names a bare `import X;` could legally name, which nothing before this
// exposed (ResolveModuleFile() only answers "does this ONE name resolve").
// Shares IndexModuleDir()/TurfInstallRoot() with ResolveModuleFile(), so the
// two can never disagree about what a real import would find. Deduplicated
// and sorted; pass "" for ImportingFile to skip the sibling-file step (e.g.
// when the importing file isn't a real on-disk path).
std::vector<std::string> ListAvailableModules(const std::string &ImportingFile = "");

// Top-level AST capture for sandbox diffing
void ClearParsedTopLevelASTs();
std::vector<std::unique_ptr<ExprAST>> SwapParsedTopLevelASTs(std::vector<std::unique_ptr<ExprAST>> New);

// Save (push) / restore (pop) the module-import cache (ImportedFiles) and
// in-progress import stack, so an isolated second parse pass in the same
// process (Sandbox.cpp's temp-file validation; a future buffer-compile
// entry point) doesn't see -- or leave behind -- another compile's import
// state. Call PushImportCycleState() before an isolated pass and
// PopImportCycleState() after, bracketing it the same way
// SwapParsedTopLevelASTs({})/SwapParsedTopLevelASTs(std::move(saved)) do.
void PushImportCycleState();
void PopImportCycleState();
void RecordParsedTopLevelAST(const ExprAST *AST);
std::unique_ptr<ExprAST> BuildProgramASTFromCache();

// True if @p Node was spliced out of a top-level `unsafe { }` block by
// AppendFlattenedTopLevelDecl() below (issue #238) -- see that function's
// own comment for why top-level unsafe-wrapping works this way instead of
// a real nested UnsafeBlockExprAST node. Consulted only by the
// TypeChecker's unsafe-gate checks.
bool WasDeclaredInsideTopLevelUnsafe(const ExprAST *Node);

// Appends a just-parsed top-level declaration to @p Target, transparently
// flattening it first if it's a top-level `unsafe { }` block (issue #238).
//
// Every consumer of a file's top-level declaration list --
// Resolver::hoistTopLevel(), main.cpp's multi-pass codegen driver,
// TypeChecker::checkAll()'s resumable-per-declaration loop -- walks a FLAT
// vector of decl-shaped nodes, found by dynamic_cast, not a recursive AST
// walk. Pushing a real UnsafeBlockExprAST wrapper node onto that flat list
// (the way `unsafe { }` behaves everywhere else -- inside a function body,
// inside an ordinary block) would make a struct/fn declared inside it
// invisible to all of them. Instead: if @p AST is an UnsafeBlockExprAST,
// its children are moved out (UnsafeBlockExprAST::takeChildren()) and
// pushed onto @p Target individually, each one registered in the
// WasDeclaredInsideTopLevelUnsafe() side-set so the TypeChecker's
// unsafe-gate checks can still tell it was authorized; the wrapper node
// itself is discarded. Otherwise @p AST is pushed onto @p Target
// unchanged. If @p RecordForSandbox is true, RecordParsedTopLevelAST() is
// called for each node actually pushed (mirrors the existing call sites
// this replaces -- some call it, some don't, depending on whether that
// particular top-level loop's nodes feed the sandbox/SLM diff cache).
//
// Used identically by three call sites that each used to duplicate this
// same "if UnsafeBlockExprAST, flatten and mark; else push as-is" logic:
// ParserModule.cpp's ParseFile() (imported modules) and main.cpp's two
// top-level parse loops (the pre-pass and the resumable main pass).
void AppendFlattenedTopLevelDecl(std::unique_ptr<ExprAST> AST,
                                 std::vector<std::unique_ptr<ExprAST>> &Target,
                                 bool RecordForSandbox);

#endif
