#ifndef BUILTINS_H
#define BUILTINS_H

#include "Lexer.h"
#include "llvm/IR/Value.h"
#include <functional>
#include <string>
#include <vector>

// CodegenIntrinsic
// Generates LLVM IR for a true bare compiler intrinsic (typeof/lengthof --
// see the BareIntrinsics set in Resolver::visitFuncCall). Everything that
// used to be a name-based stdlib intrinsic (print/printline/input,
// malloc/free, abs/power, assert/fail, ...) is now ordinary Turf code in
// lib/std/*.tr -- assert/fail specifically live in lib/std/test.tr and
// lib/std/sys.tr, delegating to `extern fn` FFI bindings, resolved through
// the exact same overload machinery as any other user function.
llvm::Value *CodegenIntrinsic(const std::string &IntrinsicName,
                              const std::vector<llvm::Value *> &Args,
                              SourceLocation Loc);

// CodegenPanic
// Emits the compiler's own internal "print MsgVal to stderr, then abort"
// sequence -- fprintf(stderr, "%s\n", MsgVal); abort(); unreachable -- used
// SOLELY by the compiler's own runtime safety guards (division/modulo by
// zero, bad string->int/enum casts, array bounds checks; see Codegen.cpp's
// call sites). This is deliberately NOT reachable from Turf source at all
// (no name in g_builtins routes here) -- it used to be shared with the
// user-facing `fail` builtin via CodegenIntrinsic("fail", ...), back when
// `fail` was a compiler intrinsic. Now that `fail` is an ordinary Turf
// function in lib/std/test.tr, this is purely an internal C++ helper the
// compiler uses for panics it generates itself.
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
