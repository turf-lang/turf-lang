#ifndef BUILTINS_H
#define BUILTINS_H

#include "Lexer.h"
#include "llvm/IR/Value.h"
#include <functional>
#include <string>
#include <vector>

// CodegenIntrinsic
// Invoked by FuncDefExprAST when an @intrinsic decorator is present.
// It maps the intrinsic name string (e.g. "pow", "malloc") to LLVM IR generation.
llvm::Value *CodegenIntrinsic(const std::string &IntrinsicName,
                              std::vector<llvm::Value *> &Args,
                              SourceLocation Loc);

#endif
