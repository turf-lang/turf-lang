#ifndef BUILTINS_H
#define BUILTINS_H

#include "Lexer.h"
#include "llvm/IR/Value.h"
#include <functional>
#include <string>
#include <vector>

// CodegenIntrinsic
// Invoked by FuncDefExprAST when an @intrinsic decorator is present.
// It maps the intrinsic name string (e.g. "pow", "malloc") to LLVM IR
// generation.
llvm::Value *CodegenIntrinsic(const std::string &IntrinsicName,
                              const std::vector<llvm::Value *> &Args,
                              SourceLocation Loc);

// Metadata for a builtin function. Keep minimal for now; extend as needed.
struct BuiltinInfo {
    int Arity = -1; // -1 = variadic/unknown
    std::string Description;
};

// Runtime registration API for builtins. Use these to register or query
// builtin functions. The registry is lazily initialized inside the
// implementation so callers don't need to explicitly initialize it.
void RegisterBuiltin(const std::string &Name, const BuiltinInfo &Info);
const BuiltinInfo *LookupBuiltin(const std::string &Name);

// Returns a list of builtin/intrinsic function names available to the
// compiler (used for suggestion and diagnostics).
std::vector<std::string> GetBuiltinNames();

#endif
