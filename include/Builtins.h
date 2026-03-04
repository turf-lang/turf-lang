#ifndef BUILTINS_H
#define BUILTINS_H

#include "Lexer.h"
#include "llvm/IR/Value.h"
#include <functional>
#include <string>
#include <vector>

// Token range for builtin functions: -100 to -199, might be changed later
enum BuiltinToken {
  TOK_BUILTIN_PRINT = -100,
  TOK_BUILTIN_PRINTLINE = -101,
  TOK_BUILTIN_INPUT = -102,
  TOK_BUILTIN_LENGTHOF = -103,
  TOK_BUILTIN_TYPEOF = -104,
};

// BuiltinDef
// Everything needed to support one builtin function lives here.
// To add a new builtin, simply push_back a new BuiltinDef in Builtins.cpp, no other file needs to change.
struct BuiltinDef {
  std::string Name;     // Keyword the user types, e.g. "print"
  int Token;            // Unique token value from BuiltinToken enum above
  int ArgCount;         // How many arguments the function accepts

  // The codegen callback: receives already-evaluated LLVM Value* arguments
  // and the source location for error reporting. Returns the result Value*.
  std::function<llvm::Value *(std::vector<llvm::Value *> &, SourceLocation)>
      Codegen;
};

// Registry
extern std::vector<BuiltinDef> Builtins;

void RegisterBuiltins();

// Lookup helpers
const BuiltinDef *FindBuiltin(const std::string &Name);
const BuiltinDef *FindBuiltinByToken(int Tok);

#endif
