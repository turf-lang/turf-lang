#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include <memory>
#include <vector>

// Expose all variables and functions to other files
extern int CurTok;
int getNextToken();
void InitializePrecedence();

std::unique_ptr<ExprAST> ParseFile(const std::string &Path);
std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<ExprAST> Parse();
bool RequiresSemicolon(const ExprAST* AST);

// Top-level AST capture for sandbox diffing
void ClearParsedTopLevelASTs();
void RecordParsedTopLevelAST(const ExprAST *AST);
std::unique_ptr<ExprAST> BuildProgramASTFromCache();

#endif
