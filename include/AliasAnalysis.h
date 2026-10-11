#ifndef ALIAS_ANALYSIS_H
#define ALIAS_ANALYSIS_H

#include "Annotation.h"
#include "CFG.h"
#include "SymbolTable.h"
#include "Types.h" // TurfTypePtr
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ExprAST;
class FuncCallExprAST;
struct ResolverFunctionSymbol;

// Simple intra-procedural alias/lifetime analysis.
// Tracks allocation sites and freed state to detect use-after-free and
// double-free within a function.
//
// Which functions allocate and which release is never decided by a function's
// NAME: a function says so itself with an `@ownership` annotation, and this
// pass reads that off the call's resolved target. A user's own arena or pool
// functions are understood exactly as the standard library's are.
class AliasAnalysisPass {
public:
  void runOnCFG(CFG &Cfg, const std::vector<SymbolID> &InitialAssigned);
};

/// The meaning of `@ownership(...)`, owned by the pass that reads it (see
/// include/Annotation.h: the front end records an annotation, the one pass that
/// consumes its name decides what it means).
///
///   @ownership(acquires)        the return value is a fresh allocation
///   @ownership(releases: p)     the call frees whatever its parameter `p` holds
///
/// Several may be stacked on one declaration.
namespace OwnershipAnnotation {

inline constexpr const char *Name = "ownership";

/// True if `Callee` is declared `@ownership(acquires)`.
bool acquires(const ResolverFunctionSymbol &Callee);

/// The function `Call` runs, if it is a call of one: a FuncCallExprAST, or a
/// MethodCallExprAST resolved through UFCS (`p.free()` is `free(p)`). Null for
/// anything else, including a bound method call.
const ResolverFunctionSymbol *calledFunction(const ExprAST &Call);

/// True if `Call`'s resolved target declares `@ownership(releases: <param>)`.
bool releases(const ExprAST &Call);

/// The argument expression `Call` passes for the parameter its target
/// releases (a UFCS call's receiver is its first argument), or null if it
/// releases nothing or the argument is not written.
const ExprAST *releasedArgument(const ExprAST &Call);

/// Checks one `@ownership` annotation against the declaration it sits on and
/// returns the problem as a sentence, or "" if it is well formed. Run by the
/// Resolver at the declaration, so a misspelt argument is an error there
/// instead of silently disabling the check it was written for.
std::string validate(const Annotation &Ann,
                     const std::vector<std::pair<std::string, TurfTypePtr>> &Params,
                     const TurfTypePtr &ReturnType, bool IsExtern,
                     const std::vector<Annotation> &All);

} // namespace OwnershipAnnotation

#endif
