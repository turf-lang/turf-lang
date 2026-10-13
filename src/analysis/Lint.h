#ifndef LINT_H
#define LINT_H

#include "frontend/AST.h"
#include "frontend/Lexer.h"
#include <cstdint>
#include <set>

// Run all heuristic lint checks on a single AST node (and its children).
// Emits warnings to stderr; never halts compilation.
void LintExpression(ExprAST *E);

// Semantic pruning: Only lint nodes that have changed (by node ID).
// Used by incremental validation to skip re-checking unchanged subtrees.
// If ChangedNodeIDs is empty, behaves like LintExpression (lint everything).
void LintChangedNodesOnly(ExprAST *E, const std::set<uint64_t> &ChangedNodeIDs);

#endif
