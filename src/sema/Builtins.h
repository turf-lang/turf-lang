#ifndef BUILTINS_H
#define BUILTINS_H

#include "frontend/Lexer.h"
#include "llvm/IR/Value.h"
#include <functional>
#include <string>
#include <vector>

// CodegenPanic
// Emits the compiler's own internal "print MsgVal to stderr, then abort"
// sequence -- fprintf(stderr, "%s\n", MsgVal); abort(); unreachable -- used
// SOLELY by the compiler's own runtime safety guards (division/modulo by
// zero, bad string->int/enum casts, array bounds checks; see Codegen.cpp's
// call sites). This is deliberately NOT reachable from Turf source at all
// (no name in g_builtins routes here) -- it used to be shared with the
// user-facing `fail` builtin, back when `fail` was a compiler intrinsic.
// Now that `fail` is an ordinary Turf function in lib/std/test.tr, this is
// purely an internal C++ helper the compiler uses for panics it generates
// itself.
llvm::Value *CodegenPanic(llvm::Value *MsgVal, SourceLocation Loc);

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
