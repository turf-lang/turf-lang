#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include <memory>

// Expose all variables and functions to other files
extern int CurTok;
int getNextToken();
void InitializePrecedence();

std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<ExprAST> Parse();

#endif
