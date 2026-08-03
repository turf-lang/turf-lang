#ifndef RESOLVER_INTERNAL_H
#define RESOLVER_INTERNAL_H

// Private, implementation-only forward declarations shared across the 5
// ResolverCore/Expr/Stmt/Decl/Call.cpp files (the split of the original
// monolithic Resolver.cpp -- see docs/planning/COMPILER_MODULE_BREAKDOWN.md).
// NOT part of the compiler's public interface (that's Resolver.h) -- these
// are free functions (not Resolver:: methods, which are already all visible
// across files via the class declaration in Resolver.h) that used to be
// `static` in the monolith but are called from more than one of the new
// files, so they need real external linkage now.

#include "Lexer.h"           // SourceLocation
#include "ResolverSymbolBase.h" // ResolverSourceLocation
#include "Types.h"           // TurfTypePtr

#include <string>
#include <vector>

// toSourceLocation() used to be declared here too, but it's now a single
// `inline` function shared with the TypeChecker split -- see
// ResolverSymbolBase.h's doc comment (already pulled in transitively via
// ResolverSymbolBase.h below).

// Defined in ResolverCore.cpp. Replaces any TurfGenericParamType-shaped bare
// name in @p Ty that matches a name in @p TypeParams with... itself, no-op
// otherwise (see the definition's own doc comment for the full rationale).
// Needed by ResolverCore.cpp (buildFunctionSymbol) and ResolverDecl.cpp
// (visitFuncDef, visitStructDecl).
TurfTypePtr substituteTypeParams(const TurfTypePtr &Ty,
                                 const std::vector<std::string> &TypeParams);

#endif
