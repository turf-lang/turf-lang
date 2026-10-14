/**
 * @file TypeWalk.h
 * @brief SMP-002: the one shared walker for "does this type contain an open
 *        generic parameter, and if so substitute/unify it" -- the question
 *        this compiler used to answer independently, by hand, in at least
 *        seven separate recursive functions across four files (see
 *        `docs/planning/ARCHITECTURE_SIMPLIFICATION.md` §1 and
 *        `docs/planning/ROADMAP_v2.0.0.md`'s SMP-002 entry for the full
 *        list and the bugs that incompleteness caused -- three of the seven
 *        were independently, silently incomplete, discovered only by
 *        hitting them in practice).
 *
 * Three primitives, each exhaustive over every TurfType subclass that can
 * carry or wrap a `TurfGenericParamType`/an unsubstituted `NamedType`
 * standing in for one (pointer, reference, nullable reference, array,
 * generic-argument list, error-union success type, func-type parameter/
 * return list):
 *
 *   - `containsOpenParam()` -- a pure predicate, parameterized by how far to
 *     look (see `ParamScope` below, since the seven original functions
 *     deliberately did NOT all look equally far -- `isOrWrapsGenericParam()`
 *     never peeked into a `GenericType`'s own argument list,
 *     `isFullyConcreteType()` did, and `typesCompatible()`/`visitCast`'s
 *     legality bypass never unwrapped anything at all, only checking the
 *     exact node in front of them).
 *   - `substitute()` -- rewrite every open-parameter occurrence found via a
 *     name -> replacement map. Used both DECLARATION-direction (a generic
 *     function's own `NamedType("T")` becoming a real `TurfGenericParamType`,
 *     `Resolver::substituteTypeParams`'s job) and INSTANTIATION-direction (a
 *     concrete type replacing an open parameter at a monomorphization site,
 *     `Monomorphize::substituteConcreteTypes`'s job) -- both are literally
 *     the same walk over the same type shapes with a different map, so one
 *     function serves both call sites without a mode flag.
 *   - `unify()` -- walk a declared (possibly still-generic) type and an
 *     actual (concrete) type in lockstep, recording every open-parameter
 *     name it hits into a substitution map. NOT a compatibility check (it
 *     never fails loudly on a structural mismatch -- see its own doc
 *     comment) -- `typesCompatible()` remains the place that decides
 *     whether two types are allowed to unify at all; this only decides,
 *     given that a candidate was ALREADY chosen, what its open parameters
 *     actually resolved to.
 *
 * Plus one small companion, `collectParamOrder()`, needed only because an
 * `InstantiationRequest`'s `TypeArgs` must be a canonical, ORDERED tuple
 * (two requests for the same instantiation must produce index-comparable
 * vectors), while `unify()`'s substitution map is unordered by construction.
 *
 * What stays OUTSIDE this file, deliberately: `typesCompatible()`'s numeric
 * widening, null/pointer coercion, enum isolation, and exact-name fallback
 * rules. Those are TypeChecker-specific compatibility semantics, not
 * "does this type contain/match an open generic parameter" -- moving them
 * here would make this a second type-compatibility oracle instead of the
 * one shared primitive layer SMP-002 exists to create. `typesCompatible()`
 * calls into `containsOpenParam(Ty, ParamScope::Bare)` at each of its own
 * per-position generic-parameter-leniency checks and nowhere else.
 */
#ifndef TYPE_WALK_H
#define TYPE_WALK_H

#include "sema/Types.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace TypeWalk {

/// A name -> replacement-type map, shared shape for both `substitute()`'s
/// input and `unify()`'s output.
using SubstMap = std::unordered_map<std::string, TurfTypePtr>;

/// Classifies a `NamedType` whose name is neither a known compiler
/// primitive nor found by this callback as "must be an open generic
/// parameter, spelled the pre-substitution way" -- the exact heuristic
/// `isOrWrapsGenericParam()`'s own doc comment (TypeCheckerDecl.cpp)
/// documents and OVH-006 traces to its root cause. Left unset (an empty
/// `std::function`) at a call site that has no such fallback to offer,
/// which is the correct choice for `ParamScope::Bare` callers -- they never
/// consult it (see `ParamScope`'s own doc comment).
using NamedTypeIsKnownFn = std::function<bool(const std::string &)>;

/// How far `containsOpenParam()` looks past the type it's given. Not all of
/// the seven original functions this file replaces looked equally far --
/// this is by design, not an oversight (`ROADMAP_v2.0.0.md`'s SMP-002
/// entry, *Risks*), so each caller states which scope it needs rather than
/// this file picking one "most correct" answer for everyone.
enum class ParamScope {
  /// Exactly the type handed in, no unwrapping at all, no `NamedType`
  /// fallback. Matches every call site that used to write a single bare
  /// `dynamic_cast<const TurfGenericParamType *>(Ty)` inline
  /// (`typesCompatible()`'s Rule 1 and its array/pointer/generic-argument
  /// per-position leniency checks, `visitCast`'s legality bypass).
  Bare,
  /// `Bare`, plus recursing through a pointer/reference/nullable-reference
  /// pointee and an array's element type, plus the `NamedType` fallback
  /// (when `IsKnownNamedType` is provided). Does NOT look inside a
  /// `GenericType`'s own type-argument list -- `isOrWrapsGenericParam()`'s
  /// exact original scope; its one caller never needed to see through
  /// `Box<T>`'s own `T`.
  ThroughWrappers,
  /// `ThroughWrappers`, plus a `GenericType`'s own type-argument list too
  /// (so `Box<T>` is correctly "still open" even though `Box` itself,
  /// unwrapped, isn't a generic-parameter shape). `isFullyConcreteType()`'s
  /// exact original scope -- needed to tell `Box<int>` apart from `Box<T>`
  /// inside a still-uninstantiated template.
  ThroughGenericArgs,
};

/// Does `Ty` contain an open (unsubstituted) generic type parameter,
/// looking as far as `Scope` allows? See `ParamScope`'s own doc comment for
/// exactly what each scope does and does not unwrap, and each call site's
/// own comment for why it needs the scope it asks for.
bool containsOpenParam(const TurfType *Ty, ParamScope Scope,
                       const NamedTypeIsKnownFn &IsKnownNamedType = nullptr);

/// Rewrite `Ty`, replacing any `TurfGenericParamType(name)` OR bare
/// `NamedType(name)` occurrence found in `Subst` with its mapped
/// replacement, recursing through every wrapper kind this compiler's type
/// hierarchy has: pointer, reference (`&T`), nullable reference (`&?T`,
/// checked before the plain-pointer case in both -- `TurfReferenceType`/
/// `TurfNullableReferenceType` ARE-A `TurfPointerType`, Types.h), array,
/// a `GenericType`'s own argument list, a fallible type's success type
/// (`!T`), and a first-class function type's parameter/return list. A name
/// absent from `Subst`, or a shape this function doesn't recognize at all,
/// is returned unchanged (structurally the same `shared_ptr`, so a caller
/// comparing before/after by pointer identity to detect "did anything
/// change" keeps working -- both `Resolver::substituteTypeParams` and
/// `Monomorphize::substituteConcreteTypes` relied on exactly that before
/// this unification and still do).
///
/// Serves BOTH substitution directions this compiler needs with the same
/// walk: build `Subst` as `{name -> TurfGenericParamType(name)}` for every
/// one of a declaration's own type-parameter names to get
/// `Resolver::substituteTypeParams`'s declaration-direction behaviour;
/// build it as `{name -> concreteType}` from a resolved instantiation to get
/// `Monomorphize::substituteConcreteTypes`'s instantiation-direction
/// behaviour. Matching BOTH the `TurfGenericParamType` and `NamedType`
/// representations of a substitutable name in one pass (rather than one
/// function per representation) is what closes the gap OVH-006 diagnosed:
/// a generic struct's own field types can carry either representation
/// depending on which snapshot of the AST a consumer reads.
TurfTypePtr substitute(const TurfTypePtr &Ty, const SubstMap &Subst);

/// One-directional structural pattern match: walk `Pattern` (a declared,
/// possibly still-generic type) and `Concrete` (an actual type, e.g. a call
/// site's argument) in lockstep. Whenever `Pattern` reaches a bare
/// `TurfGenericParamType`, records `Concrete`'s corresponding type into
/// `Subst` under that name -- `Subst.emplace()`, so the FIRST position a
/// given name is seen in wins; a later position mentioning the same name is
/// not re-unified (this compiler's generic surface only ever needs one
/// "shape" per type parameter per signature, same as the function this
/// replaces always assumed).
///
/// A structural mismatch (`Pattern` is a pointer but `Concrete` isn't, an
/// arity difference between two `GenericType`s or two `FuncType`s, ...) is
/// silently skipped, not reported -- this is a best-effort binder for a
/// candidate the caller has ALREADY chosen (typically via
/// `typesCompatible()`), not a second compatibility oracle. It does not
/// return a success/failure result for exactly that reason: nothing this
/// compiler does with its output branches on whether it "matched
/// completely."
void unify(const TurfTypePtr &Pattern, const TurfTypePtr &Concrete,
          SubstMap &Subst);

/// Recover a CANONICAL, declaration-order list of generic type-parameter
/// names from `Pattern`, walking the identical shape `unify()` does (a name
/// already present in `Order` is not duplicated). `SubstMap` is an
/// `unordered_map` -- its iteration order is not the declaration order and
/// is not even guaranteed stable across equivalent runs -- while an
/// `InstantiationRequest` needs an ORDERED type-argument tuple so two
/// requests for the same instantiation produce index-comparable vectors,
/// not just the same set of name/type pairs. Call this on the same
/// `Pattern` a `unify()` call already walked to recover the order its names
/// were first seen in.
///
/// SMP-002 REAL BUG FIX, not just a rename: the function this replaces
/// (`collectGenericParamNameOrder()`, TypeCheckerCall.cpp) had NO
/// `GenericType` case at all -- an asymmetry with its sibling
/// `collectGenericSubstitution()` (now `unify()` above), which always did.
/// Confirmed live: a generic free function taking a generic-struct-typed
/// parameter (`fn<T> unwrap(b: Box<T>): T`) called at two different type
/// arguments (`unwrap(boxOfInt)`, `unwrap(boxOfDouble)`) recovered an EMPTY
/// `Order` for that parameter either way, so both calls' instantiation
/// requests collapsed to the same bogus `unwrap<>` (empty type-argument
/// list) -- one request merging two call sites that need two DIFFERENT
/// clones, corrupting the monomorphizer's per-instantiation cache key
/// (`Cannot access field on non-struct type` reproduced live against the
/// pre-unification code). Restoring symmetry with `unify()` here is what
/// fixes it -- see the change record's *Removed assumptions* and
/// `tests/38_monomorphization/`'s SMP-002 regression test for the full
/// repro.
void collectParamOrder(const TurfTypePtr &Pattern,
                       std::vector<std::string> &Order);

/// Like `unify()`, but for the shape a generic STRUCT's own already-resolved
/// VALUE needs when reading a field or return type through it (e.g.
/// `someListOfInt.data`, `TypeChecker::visitFieldAccess`/`visitMethodCall`):
/// there is no parallel "Concrete" type to walk `Pattern` alongside, only the
/// struct's own declared field type and its ALREADY-KNOWN, already-ordered
/// concrete type-argument list (`GenericTypeArgs`, e.g. `[int]` for a
/// `List<int>` value). Finds every open-parameter occurrence within
/// `Pattern` in FIRST-SEEN order (deduped, exactly like `collectParamOrder()`
/// walks) and binds each one, in that order, to the next unclaimed entry of
/// `TypeArgs`. A structural mismatch (more open-parameter occurrences than
/// `TypeArgs` has entries) simply stops consuming -- silently, not an error,
/// same best-effort spirit as `unify()`.
///
/// `IsKnownNamedType` plays the same role as `containsOpenParam()`'s
/// parameter of the same name: a generic struct's globally-registered
/// field types can still carry the pre-substitution `NamedType`
/// representation rather than `TurfGenericParamType` in code paths OVH-006
/// didn't reach, and this is the exact recovery technique that reads
/// through that -- unset means no fallback is offered (see
/// `ParamScope::Bare`'s own doc comment for why that's the correct default
/// for a caller with nothing to resolve a `NamedType` against).
void collectOrderedFieldSubstitution(
    const TurfTypePtr &Pattern, const std::vector<TurfTypePtr> &TypeArgs,
    SubstMap &Subst, const NamedTypeIsKnownFn &IsKnownNamedType = nullptr);

} // namespace TypeWalk

#endif
