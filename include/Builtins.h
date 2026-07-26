#ifndef BUILTINS_H
#define BUILTINS_H

#include "Lexer.h"
#include "llvm/IR/Value.h"
#include <functional>
#include <string>
#include <vector>

// CodegenIntrinsic
// Generates LLVM IR for a true bare compiler intrinsic (typeof/lengthof/
// fail/assert -- see the BareIntrinsics set in Resolver::visitFuncCall) or
// for a compiler-emitted runtime check that needs to invoke one of them
// (e.g. a division-by-zero guard calling "fail" internally). Everything
// that used to be a name-based stdlib intrinsic (print/printline/input,
// malloc/free, abs/power, ...) is now ordinary Turf code in lib/std/*.tr
// delegating to `extern fn` FFI bindings instead.
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
