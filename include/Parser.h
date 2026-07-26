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

// Top-level AST capture for sandbox diffing
void ClearParsedTopLevelASTs();
std::vector<std::unique_ptr<ExprAST>> SwapParsedTopLevelASTs(std::vector<std::unique_ptr<ExprAST>> New);
void RecordParsedTopLevelAST(const ExprAST *AST);
std::unique_ptr<ExprAST> BuildProgramASTFromCache();

#endif
