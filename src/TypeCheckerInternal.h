#ifndef TYPE_CHECKER_INTERNAL_H
#define TYPE_CHECKER_INTERNAL_H

// Private, implementation-only forward declarations shared across the 5
// TypeCheckerCore/Expr/Stmt/Decl/Call.cpp files (the split of the original
// monolithic TypeChecker.cpp -- see docs/planning/COMPILER_MODULE_BREAKDOWN.md).
// NOT part of the compiler's public interface (that's TypeChecker.h) -- these
// are free functions (not TypeChecker:: methods, which are already all
// visible across files via the class declaration in TypeChecker.h) that used
// to be `static`/anonymous-namespace-local in the monolith but are called
// from more than one of the new files, so they need real external linkage
// now.

#include "Lexer.h"                 // SourceLocation
#include "ResolverSymbolBase.h"    // ResolverSourceLocation
#include "Types.h"                 // TurfType

// toSourceLocation() is a single `inline` function shared with the Resolver
// split -- see ResolverSymbolBase.h's doc comment (already pulled in
// transitively via ResolverSymbolBase.h below).

// Defined in TypeCheckerCall.cpp (next to its main partner,
// ufcsReceiverTypeMatches). Exact (no numeric coercion) type-equality check,
// used by TypeCheckerExpr.cpp's visitBinary (operator-overload candidate
// matching) as well as TypeCheckerCall.cpp's own UFCS receiver matching.
bool sameTypeExact(const TurfType *A, const TurfType *B);

#endif
