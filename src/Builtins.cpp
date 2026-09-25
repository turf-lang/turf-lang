/**
 * @file Builtins.cpp
 * @brief Implementation of TurfLang intrinsic and built-in functions.
 * @details Handles built-in operations like memory management (malloc, free),
 *          input/output (print, input), mathematical operations, and direct
 * LLVM IR mapping.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Builtins.h"
#include "Codegen.h" // TheModule, Builder, TheContext
#include "Errors.h"  // SemanticError
#include "Lexer.h"   // SourceLocation

#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"

using namespace llvm;

// Runtime registry for builtins
#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

static std::unordered_map<std::string, BuiltinInfo> g_builtins;
static std::once_flag g_builtins_init_flag;



static void ensure_builtins_registered();

void RegisterBuiltin(const std::string &Name, const BuiltinInfo &Info) {
  g_builtins[Name] = Info;
}

const BuiltinInfo *LookupBuiltin(const std::string &Name) {
  std::call_once(g_builtins_init_flag, ensure_builtins_registered);
  auto It = g_builtins.find(Name);
  if (It == g_builtins.end()) return nullptr;
  return &It->second;
}

std::vector<std::string> GetBuiltinNames() {
  std::call_once(g_builtins_init_flag, ensure_builtins_registered);
  std::vector<std::string> Names;
  Names.reserve(g_builtins.size());
  for (const auto &P : g_builtins) Names.push_back(P.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

static void ensure_builtins_registered() {
  // Only register the one true bare language-level intrinsic that is
  // callable without a module qualifier. This is the only name that
  // appears in g_builtins for suggestion quality (suggestBuiltins()) and
  // for TypeChecker's own arity check (LookupBuiltin()).
  //
  // stdlib functions (print, printline, input, malloc, abs, power, sqrt,
  // assert, fail, length, …) live in lib/std modules and are resolved via
  // the Resolver's module registry — they must NOT be listed here.
  // assert/fail specifically used to be registered here (with `assert`
  // needing a bespoke Arity == -1 sentinel to express its 1-or-2-arg
  // signature, bypassing the generic arity check entirely) until they
  // moved to lib/std/test.tr as ordinary, real overloaded Turf functions.
  // `lengthof` was the other bare intrinsic here until
  // STRING_UFCS_OPERATOR_CHAR_RFC.md §7 step 7 deleted it in favor of
  // strings::length(s).
  RegisterBuiltin("typeof",   {1, "Compile-time type name of an expression"});
}

// CodegenIntrinsic() used to live here -- the Codegen-side implementation
// for the two true bare intrinsics (typeof, lengthof). It's gone now:
// lengthof was deleted entirely (STRING_UFCS_OPERATOR_CHAR_RFC.md §7 step
// 7 -- strings::length(s) is the real replacement), and typeof's own
// handling was inlined directly into its one call site in Codegen.cpp
// (FuncCallExprAST::codegen()) rather than routed through a
// single-remaining-case dispatcher. BuiltinInfo/LookupBuiltin/g_builtins
// below are still very much alive -- TypeChecker still uses them for
// typeof's arity check -- only the Codegen-side generator function is gone.

/**
 * @brief Emits the compiler's own internal panic sequence: call
 *        libturfrt's turf_rt_panic(kind, msg, file, line), which prints a
 *        consistently-formatted message to stderr and abort()s. SOLELY for
 *        the compiler's own runtime safety guards (division/modulo by
 *        zero, bad string->int/enum casts, array bounds checks, integer
 *        overflow) -- not reachable from Turf source under any name.
 *        `fail`/`assert` used to share this exact IR shape as compiler
 *        intrinsics; now that they're ordinary Turf functions in
 *        lib/std/test.tr (built on lib/std/sys.tr's own, separate
 *        write(2,...)-based sys::panic()), this is purely an internal C++
 *        helper with no user-facing name at all.
 *
 * @details RT-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): this used to
 *          build the fprintf(stderr, "%s\n", MsgVal) + abort() sequence
 *          directly, inline, per call site. Every one of the 9 call sites
 *          (grep CodegenPanic() across src/Codegen*.cpp) still passes just
 *          one pre-built message string, so `kind` here is a single fixed
 *          literal, not a distinct value per call site -- the specific
 *          classification ("Division by zero", "Integer overflow", ...)
 *          stays exactly where it already was, inside MsgVal. Widening
 *          CodegenPanic()'s own signature to carry a distinct kind per call
 *          site would touch all 9 sites for no behavioral change (the
 *          printed message is unchanged either way) -- out of RT-001's
 *          scope (§25: no drive-by refactors of code this group only
 *          needed to reroute, not redesign).
 */
llvm::Value *CodegenPanic(llvm::Value *MsgVal, SourceLocation Loc) {
  // turf_rt_panic(const char *kind, const char *msg, const char *file, int line)
  Function *PanicFunc = TheModule->getFunction("turf_rt_panic");
  if (!PanicFunc) {
    Type *PtrTy = PointerType::getUnqual(*TheContext);
    FunctionType *FT = FunctionType::get(
        Type::getVoidTy(*TheContext),
        {PtrTy, PtrTy, PtrTy, Type::getInt32Ty(*TheContext)},
        /*isVarArg=*/false);
    PanicFunc = Function::Create(FT, Function::ExternalLinkage,
                                 "turf_rt_panic", TheModule.get());
  }

  Value *KindVal = Builder->CreateGlobalString("runtime check", "panickind");
  Value *FileVal =
      Builder->CreateGlobalString(Loc.Filename.empty() ? "<unknown>" : Loc.Filename,
                                  "panicfile");
  Value *LineVal = ConstantInt::get(Type::getInt32Ty(*TheContext), Loc.Line);

  Builder->CreateCall(PanicFunc, {KindVal, MsgVal, FileVal, LineVal});
  Builder->CreateUnreachable();

  // Mirrors ReturnExprAST::codegen(): CreateUnreachable() is a terminator,
  // so if this panic is not the last statement in its block, the next
  // statement's codegen would try to add IR to an already-terminated block
  // ("Terminator found in the middle of a basic block"). Give the builder a
  // fresh dead block to keep emitting into; it's either erased as dead code
  // (the panic was the function's last statement) or picked up as an
  // unreachable predecessor-less block wired into the live CFG by whatever
  // nested construct (if/while) owns this statement list.
  if (CurrentFunction) {
    BasicBlock *DeadBB =
        BasicBlock::Create(*TheContext, "after_panic", CurrentFunction);
    Builder->SetInsertPoint(DeadBB);
  }

  // Doesn't return, but we need to return something to satisfy the type
  // signature. Any value will do since it's unreachable.
  return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
}

