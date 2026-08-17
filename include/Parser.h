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

#endif
