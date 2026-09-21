#ifndef TYPE_CHECKER_INTERNAL_H
#define TYPE_CHECKER_INTERNAL_H

// Private, implementation-only forward declarations shared across the 5
// TypeCheckerCore/Expr/Stmt/Decl/Call.cpp files (the split of the original
// monolithic TypeChecker.cpp -- see docs/architecture/compiler_pipeline.md).
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

// TYPE-002: defined in TypeCheckerDecl.cpp, next to their only ORIGINAL
// caller (visitVarDecl's `guess`-inference gate). isFullyConcreteType() is
// ALSO used by TypeCheckerCall.cpp's visitFuncCall/visitMethodCall as of
// S14a, to gate their own MonoCollector_->record() calls on the computed
// substitution being genuinely concrete, not just non-empty -- see
// isFullyConcreteType()'s own doc comment for the real bug this fixes
// (a generic function calling another generic function through its own
// still-open type parameter used to record a bogus, non-concrete
// instantiation request, harmless under erasure but a hard INV-GEN-1
// assertion failure once erasure is gone).
bool isOrWrapsGenericParam(const TurfType *Ty);
bool isFullyConcreteType(const TurfType *Ty);

#endif
