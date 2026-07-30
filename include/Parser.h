#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include <memory>

// Expose all variables and functions to other files
extern int CurTok;
int getNextToken();
void InitializePrecedence();

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
void RecordParsedTopLevelAST(const ExprAST *AST);
std::unique_ptr<ExprAST> BuildProgramASTFromCache();

#endif
