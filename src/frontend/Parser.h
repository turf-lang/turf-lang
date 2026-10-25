#ifndef PARSER_H
#define PARSER_H

#include "frontend/AST.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

// ─── ParserState (F-S1, issue #302) ───────────────────────────────────────
// Everything the parser reads and writes while it parses one file, owned by
// whoever parses. These were process-wide globals (CurTok, the bracket and
// recursion depths, the flags a construct sets for the ones nested in it,
// the file's module aliases, and LexerScope's pointer to the current lexer),
// so ParseFile() saved and restored them around every import, and the query
// facade, the formatter and --migrate=pub each did it again with their own
// list. A nested parse is now a second ParserState, made current by a
// ParserScope, and nothing of the outer one is touched.
//
// Not here: what one compile accumulates across every file it parses (the
// import caches, the files that declared `unsafe module;`, the parsed
// top-level declarations: ParserModule.cpp's ParserCompileState, behind
// Push/PopParserCompileState()), error recovery (g_recoverActive, which the
// passes after the parser arm too), and the compile's file bookkeeping
// (SourceFilename, SourceLines, AllSourceLines -- see LexerState).
struct ParserState {
  // A parser with no lexer reads EOF (Lex()'s idle lexer).
  ParserState() = default;
  explicit ParserState(LexerState &L) : Lexer(&L) {}
  ParserState(const ParserState &) = delete;
  ParserState &operator=(const ParserState &) = delete;

  // The lexer this parser reads its tokens from (Lex()).
  LexerState *Lexer = nullptr;
  // The current token the parser is looking at.
  int CurTok = 0;

  // Current '{'/'('/'[' nesting depth relative to the top level (0), kept by
  // getNextToken() as each token is consumed, so a parse failure at any
  // nesting depth can recover: SkipToNextTopLevelBoundary() skips until it
  // is back at 0. A naive "assume depth 1" recovery is not enough -- see that
  // function. Reset to 0 by resetLexer() so a fresh parse pass (pre-pass vs.
  // main pass, or a Sandbox.cpp re-parse) never inherits a stale value.
  int BraceDepth = 0;
  // LEC-005: the '{'/'}' share of BraceDepth alone. A parse error inside a
  // function body leaves the body's own `{` open; resynchronising on that
  // count (not on the all-bracket one, which an unclosed `(` makes useless --
  // issue #245) is what lets recovery skip to the function's closing brace
  // instead of stopping at the next `;` inside the body and parsing the rest
  // of the body as top-level statements. Kept by getNextToken() beside
  // BraceDepth.
  int CurlyDepth = 0;
  // How many ParseRecursionGuards are live (below). A recovered error
  // unwinds through every one, which decrements it.
  int RecursionDepth = 0;

  // True only while the local-variable declaration parse reads its type
  // annotation: the one place `guess` may appear (F-GUESS-3). See ParseType().
  bool AllowGuessType = false;
  // The size parameters (`N` in `fn<T, N: size>`) of the function being
  // parsed: inside it, `[N]` in a type is an array whose size is N. Set by
  // ParseFuncDef() for its whole declaration and body. See ParseType().
  std::vector<std::string> SizeParamsInScope;
  // LEC-003: true only while ParseCompareExpr() parses a `matches` header's
  // pattern. There `Name {` is the START of a struct pattern (`matches Point
  // { x, y }:`), not the removed struct brace-literal, so ParsePrimary()'s
  // "'Name { ... }' is no longer valid syntax" hint -- which fires whenever a
  // known struct name is followed by '{' -- must stand down. Scoped by an
  // RAII guard around the one ParseExpression() call that reads the header,
  // never left set across a statement.
  bool InPatternHeader = false;
  // LEC-004: set by ParsePattern() just before it reads a header, taken (and
  // cleared) by the first ParsePostfixExpression() call. See there.
  bool PatternHeadCallPending = false;
  // LEC-004: true while the innermost function body being parsed has had a
  // `try` in it. ParseUnary() sets it; FuncBodyTryScope (ParserInternal.h)
  // saves, clears and restores it around each function body, so a `try` in
  // a nested function marks that function and not its enclosing one.
  bool FuncUsesTry = false;

  // LEC-001 stage 4 (FREEZE.md 6.16, F-PKG-2): this file's `import` spellings
  // whose module identity differs from the spelling -- see ModuleKeyFor()
  // (ParserModule.cpp). Per file: an import parses with a ParserState of its
  // own, so it neither sees the importer's aliases nor leaves its own behind,
  // and ParsePreludeImports() clears it at the start of each top-level pass.
  std::map<std::string, std::string> ModuleAliases;
};

// The parser that is parsing: the innermost live ParserScope's, or, with
// none, an idle one with no lexer.
ParserState &Parser();

// Makes `P` the current parser (and so its lexer the current lexer, Lex())
// for this scope's lifetime, and puts the previous one back after -- an
// import's parse, the query facade's isolated analysis, the formatter and
// --migrate=pub all parse or lex a second buffer this way without touching
// the first.
class ParserScope {
public:
  explicit ParserScope(ParserState &P);
  ~ParserScope();
  ParserScope(const ParserScope &) = delete;
  ParserScope &operator=(const ParserScope &) = delete;

private:
  ParserState *Saved;
};

// The recursive-descent parser has no built-in recursion-depth limit --
// ParseExpression()/ParseUnary()/ParsePrimary()/ParseBlock()/ParseType()
// etc. are ordinary unbounded C++ recursion. Deeply nested malformed input
// (a long run of unmatched '(' or '{', exactly what a half-typed
// expression in an LSP's live edit buffer produces constantly while the
// user is still typing) risks a genuine C++ stack overflow -- a hard
// process crash, not something a diagnostic or setjmp/longjmp recovery can
// catch. Guarded at four entry points: ParseExpression() and ParseBlock()
// (the two top-level recursive-descent entry points reached from
// statement/declaration parsing), plus ParseUnary() and ParseType()
// (each recurses directly into ITSELF for chained prefix operators
// (`!!!!x`) and nested generic/error-union types (`list<list<...>>`,
// `!!T`) respectively, WITHOUT going back through ParseExpression() in
// between -- confirmed via code review to be completely unbounded even
// after the ParseExpression()/ParseBlock() guards existed, since neither
// of those is re-entered by this kind of direct self-recursion). Every
// OTHER recursive parse function either goes through one of these four on
// its way to recursing, or is bounded by the token stream itself (e.g. a
// parameter list can only be as long as the tokens the user actually
// typed) rather than by unbounded self-recursion.
constexpr int MaxParseRecursionDepth = 2000;

// RAII guard: increments the current parser's RecursionDepth on
// construction, decrements the same parser's on destruction, so every one
// of ParseExpression()/ParseBlock()'s many early-return paths is covered
// automatically with no manual bookkeeping at each one. Construct one as the
// very first statement in the guarded function and check tooDeep() before
// doing any real work.
struct ParseRecursionGuard {
  ParserState &P;
  ParseRecursionGuard() : P(Parser()) { ++P.RecursionDepth; }
  ~ParseRecursionGuard() { --P.RecursionDepth; }
  bool tooDeep() const { return P.RecursionDepth > MaxParseRecursionDepth; }
};

int getNextToken();

// The fix-it for an "Expected ';'" raised while CurTok is the token AFTER the
// statement: insert ';' immediately after the statement's last token (which
// may be on an earlier line than the token the error points at). Empty unless
// the missing ';' is unambiguous -- the next token starts a later line or is a
// '}' -- because `x = 5 6;` or `f(a b)` on one line are not a missing ';'
// (inserting one there would "validate" a different program than the one the
// writer meant). Also empty when the source line does not show the previous
// token ending where the lexer says it did. Pass it as LogErrorAt()'s last
// argument at every site that reports ErrCode::MissingSemicolon after parsing
// a complete statement. Defined in ParserCore.cpp.
SLM::DiagnosticHints MissingSemicolonHints();

// InitializeSymbolTable() (SymbolTable.h, defined SymbolTable.cpp --
// allocates GlobalSymbolTable, which ParseStructDecl() registers into and
// TypeChecker::visitMethodCall() reads back from) is load-bearing for
// typecheck CORRECTNESS and carries no built-in enforcement: skipping it
// doesn't crash, it fails every instance method call with a misleading "only
// struct instances have methods" diagnostic -- see issue #231. Any embedder
// driving the parser/resolver/typechecker outside main.cpp's own pipeline (an
// LSP, a REPL, a unit test) MUST call this once before parsing anything. It
// is idempotent (safe to call more than once per process). The operator
// precedence table it used to fill too is a constant (ParserCore.cpp, F-S1).
void InitializeCompilerGlobals();

// A token that legitimately starts a new top-level declaration (or ends the
// previous statement). TOK_AT is deliberately excluded: STD-002 added back
// exactly one legal use of '@' (`@caller_location fn ...`,
// ParsePrimaryBase()'s TOK_AT case), but every FAILURE path through that
// case still consumes at least the '@' token itself before returning
// nullptr (either immediately, for any other '@word', or after also
// consuming 'caller_location' when the token after it isn't 'fn') -- so a
// failed construct starting with '@' is still guaranteed to make forward
// progress, just not necessarily "zero tokens consumed" any more. Treating
// '@' as a safe place to stop would make the "already at a boundary, don't
// skip further" guard in SkipToNextTopLevelBoundary() fire before that
// progress happens on the SUCCESS path (a legal `@caller_location fn ...`
// itself), which is the actual reason '@' stays excluded from this set.
bool IsTopLevelBoundaryToken(int Tok);

// Panic-mode recovery: after a top-level statement's parse fails, skip
// tokens forward (tracking '{'/'('/'[' nesting via g_braceDepth so a nested
// boundary-shaped token doesn't stop the skip early) until back at depth 0
// AND sitting on a real top-level boundary. Used by main.cpp's own
// per-statement recovery loops and by Sandbox.cpp's incremental parse passes
// (see issue #228) so both share one resync implementation instead of two
// that can drift apart.
void SkipToNextTopLevelBoundary();

// FDN-003: the module whose file is currently being parsed ("" for the main
// program file) -- the owning module every struct/enum/machine/error
// declaration's TypeIdentity is built from. Defined in ParserModule.cpp.
std::string CurrentParseModule();

std::unique_ptr<ExprAST> ParseFile(const std::string &Path,
                                   const std::string &ModuleName = "",
                                   SourceLocation ImportLoc = SourceLocation{});
std::unique_ptr<ExprAST> ParseExpression();
/// Parses one `import ...` declaration, and nothing after it. A top-level
/// `import` must not go through ParseExpression(): that would let a binary
/// operator typed after the module name (`import test %`) swallow the next
/// declaration as its right-hand side, hiding both the import and the
/// declaration from the Resolver's hoisting pass -- which then reported a
/// bogus error from inside lib/std (found by the REL-004 fuzzer).
std::unique_ptr<ExprAST> ParseImport();
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

// LEC-008 (FREEZE.md 6.24, F-IMP-1): true for a prelude module name. The
// Resolver asks it so `strings::length(s)` stays spellable in EVERY file, not
// only in the main program that ParsePreludeImports() injects them into.
// Reads the same PreludeModuleNames array, so it cannot drift from it.
bool IsPreludeModuleName(const std::string &Name);

// True for the prelude modules whose public free functions are callable bare
// (`io`, `math`, `sys`): the Resolver's last-resort tier for an unqualified
// call that nothing in the file declares or explicitly imports.
bool IsPreludeBareModule(const std::string &Name);

// The same names as a list, for a tool that has to enumerate them
// (turf::preludeBareModules(), include/turf/Analysis.h). One array behind both,
// so the predicate and the list cannot disagree.
const std::vector<std::string> &PreludeBareModuleList();

// OVH-011 (docs/development/changes/OVH-011.md): called by main.cpp
// once per parse pass, right after that pass's own ParsePreludeImports()
// call and before the entry file's own top-level declaration loop starts.
// Returns true if @p EntryPath was already fully parsed via a transitive
// import reached during prelude injection (the caller must SKIP its own
// top-level loop entirely -- re-parsing would duplicate every declaration);
// false otherwise, having registered @p EntryPath so a LATER self-reference
// (one of the entry file's own imports importing it back) becomes an
// ordinary cache hit too. See its definition in ParserModule.cpp for the
// full rationale and the concrete bug this closes (lib/std/collections/
// list.tr, transitively re-imported by the always-injected `strings`
// prelude module's own `import list::List;`).
bool ClaimEntryFileImportSlot(const std::string &EntryPath);

// RT-001: this install's resolved root -- TURF_HOME if set and non-empty,
// else <compiler-binary-dir>/.. (so a plain `build/turfc` dev build finds
// the sibling lib/ directory, lib/std included, with zero setup). Exported
// (was file-local in ParserModule.cpp until RT-001) specifically so
// main.cpp's linkObject() can anchor `-lturfrt`'s search path at the same
// root `import` already resolves lib/std against, without a second,
// independently-maintained copy of this logic. See its definition in
// ParserModule.cpp for the full rationale.
std::string TurfInstallRoot();

// LANG-002/STD-001: true if @p Path declared the raw-pointer capability
// (`unsafe module;`) AND that declaration is actually honored -- the
// `unsafe { }` gate's trust boundary, replacing the old, purely path-based
// IsPathUnderLibStd() (issue #238). See its definition in ParserModule.cpp
// for the full rationale; exported here (rather than kept file-local like
// TurfInstallRoot()) specifically so TypeChecker.cpp's gate checks can call
// it without linking against llvm::sys::fs directly.
bool HasRawPointerCapability(const std::string &Path);

// OVH-012: true if @p Path (canonicalized) falls under THIS install's
// `lib/std` specifically -- a strict subset of TurfInstallRoot() (excludes
// a sibling `lib/usr` user package, which is not "stdlib" for this check's
// purpose even though it lives under the same install root). Purely
// path-based, unlike HasRawPointerCapability() just above -- and
// DELIBERATELY named differently from the old, removed `IsPathUnderLibStd()`
// (issue #238) that function replaced: `scripts/check/check_boundary.sh` greps
// for that exact identifier as a regression guard against a path-based
// CAPABILITY check ever reappearing (F-STD-1, FREEZE.md §6.8 -- privileges
// must be declared in source, never inferred from a file path). This
// function grants no capability at all; it only answers "is this
// diagnostic's location code the user didn't write," backing
// DiagnosticEngine::flushAll()'s "system-module warning" print-time gate
// (see Lexer.cpp) -- reusing the old name here would be confusing at best
// and would trip that guard at worst. See its definition in ParserModule.cpp
// for the full rationale.
bool IsLibStdWarningLocation(const std::string &Path);

// LANG-002: checks for and consumes a leading `unsafe module;` capability
// pragma at the current token position. See its definition in
// ParserModule.cpp for the full contract (when to call it, and what to do
// with @p OutBlock when it returns false).
bool ParseLeadingUnsafeModuleDecl(std::unique_ptr<ExprAST> &OutBlock);

// Resolves a bare module name (e.g. "net") to the physical `.tr` file a real
// `import net;` would load -- the same stdlib -> usr-package ->
// ImportingFile's-directory -> cwd search order ParseModuleImportByName()
// uses, but as a pure lookup with no parsing and no ImportedFiles/diagnostic
// side effects. See its definition in ParserModule.cpp for the full
// rationale (built for tooling -- an LSP's go-to-definition -- that needs
// "where does this module live" without compiling it). Returns "" if the
// module can't be found anywhere in the search order, and also "" if more
// than one file answers to the name (LEC-001 stage 3): a real import of an
// ambiguous name is E090, not a file.
std::string ResolveModuleFile(const std::string &ModuleName,
                              const std::string &ImportingFile = "");

// LEC-001 stage 4: the module identity the file being parsed means by the
// module qualifier @p Spelled (`util` in `util::f()`, `util::T`). Equal to
// @p Spelled except for a dependency package's modules, whose identity is
// `<package>/<path>`. Called wherever the parser reads a qualifier, so every
// later pass sees identities only (see ParserModule.cpp).
const std::string &ModuleKeyFor(const std::string &Spelled);

// True if @p Name is a module of this installation's standard library
// (lib/std/**). Used to reject a dependency package named like a stdlib
// module, which `import <name>;` could never reach (LEC-001 stage 4).
bool IsStdlibModuleName(const std::string &Name);

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

// Save (push) / restore (pop) every piece of per-compile parser state: the
// sandbox's top-level AST cache, the module-import cache and in-progress
// import stack, the `unsafe` capability registrations, and the SMP-001
// overlay/install-root settings below. Push leaves all of it empty, so the
// compile that runs between the two calls starts exactly like a fresh
// process would and leaves nothing behind. Only the query facade
// (src/tooling/AnalysisFacade.cpp) calls these -- see its GlobalStateScope.
void PushParserCompileState();
void PopParserCompileState();

// SMP-001: make ParseFile() read `Text` instead of the disk file at `Path`
// (an embedder's unsaved buffer), and make TurfInstallRoot() return `Root`.
// Both reset on PushParserCompileState(), so set them after pushing.
void SetSourceOverlay(const std::string &Path, const std::string &Text);
void SetInstallRootOverride(const std::string &Root);

void RecordParsedTopLevelAST(const ExprAST *AST);

// A function whose definition failed to parse after its name was read: its
// own error is already reported, so a call to the name is not reported again
// as an unknown function (E041). ParseFuncDef() notes the name when it reads
// it and forgets it when the definition parses. Reset by
// PushParserCompileState().
void NoteFunctionParseStarted(const std::string &Name);
void NoteFunctionParsed(const std::string &Name);
bool FunctionFailedToParse(const std::string &Name);
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
