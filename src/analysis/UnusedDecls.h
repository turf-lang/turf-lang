#ifndef UNUSED_DECLS_H
#define UNUSED_DECLS_H

// AN-7 (TDP-2-007, issue #334): dead-code warnings over a whole resolved and
// type-checked program.
//
//   E178  a private function or method nothing calls or names
//   E179  an import nothing in its file uses (with a Fix that removes it)
//   E180  a `pub` function in a binary's root file that nothing calls
//
// Uses are read from what the Resolver and TypeChecker recorded on each call
// and name (ResolvedCandidates, ResolvedCallTarget, ResolvedFunctionRef), so a
// call through UFCS, a generic instantiation, a function value or a `@test`
// counts. Matching is by declaration and by name: an overload or a same-named
// method elsewhere being used keeps a declaration quiet, which can only miss a
// dead function, never report a live one. `main`, operators, `extern fn`,
// `@test` functions and anything in lib/std are never reported, nor is a
// library module's `pub` item.

#include <memory>
#include <vector>

class ExprAST;

namespace UnusedDecls {

// @p Program is the main file's top-level declarations, its imports carrying
// their module bodies. @p IsBinaryRoot is false for a module checked on its
// own (--module), whose `pub` items are its interface.
void report(const std::vector<const ExprAST *> &Program, bool IsBinaryRoot);

} // namespace UnusedDecls

#endif
