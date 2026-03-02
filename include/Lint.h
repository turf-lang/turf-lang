#ifndef LINT_H
#define LINT_H

#include "AST.h"
#include "Lexer.h"

// Run all heuristic lint checks on a single AST node (and its children).
// Emits warnings to stderr; never halts compilation.
void LintExpression(ExprAST *E);

#endif
