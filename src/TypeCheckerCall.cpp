/**
 * @file TypeCheckerCall.cpp
 * @brief Call & member/module access type checking: function calls (incl. UFCS/generics), method calls, field access, recover, scope access, array access.
 * @details Part of the TypeChecker pass (Issue #7), split from the original
 *          monolithic TypeChecker.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "TypeChecker.h"

// AST.h is needed for the full definition of every node type.
// It drags in LLVM headers, but TypeChecker itself does not use any LLVM API.
#include "AST.h"

#include "SymbolTable.h"

#include "Errors.h"
#include "SuggestionIndex.h"

#include "Builtins.h"

#include "TypeCheckerInternal.h"

#include "Monomorphize.h" // TYPE-002 (S11b): report-only instantiation collector

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>


// ─── Expression visitors ──────────────────────────────────────────────────────

// Exact-name-match only, no numeric coercion (int->double widening etc).
// Deliberately NOT typesCompatible() (used by ordinary `fn` overload
// resolution, which DOES permit that widening) -- RFC §4.2/§0 mandate
// exact-match-only for operator resolution specifically, to avoid a `char`
// or `int` value silently binding to an overload written for a different
// type. See STRING_UFCS_OPERATOR_CHAR_RFC.md §4.2.
bool sameTypeExact(const TurfType *A, const TurfType *B) {
  return A && B && A->getName() == B->getName();
}


/**
 * @brief LANG-001: does @p ParamTy (operator[]/operator[]='s declared SELF
 *        parameter type) match @p BaseTy (the actual base expression's
 *        type), for the purpose of `arr[i]`/`arr[i] = v`?
 *
 * Unlike every OTHER operand of every other overloadable operator, the self
 * operand of `operator[]`/`operator[]=` is allowed to match either BY VALUE
 * (sameTypeExact()) or BY REFERENCE (ParamTy == "&" + BaseTy's own name) --
 * the ordinary binary/unary operator rule (byte-for-byte exact match, no
 * implicit &) is deliberately relaxed here, ONLY for the self position, for
 * a concrete reason: `operator[]=` is USELESS as a mutation mechanism if its
 * self parameter is passed by value (writing through a copy that's
 * discarded the moment the call returns), and requiring every CALL SITE to
 * spell `(&box)[i] = v` instead of the natural `box[i] = v` would make the
 * feature unusable for its own stated purpose (closing `LIMITATIONS.md`
 * §4's `List<T>`/`Map`/`Set` `get`/`set` gap). A `.`-method's own implicit
 * `self` gets exactly this treatment for free (auto-address-of at the call
 * site, §6.6's documented exemption) -- `operator[]`/`operator[]=` aren't
 * `.`-methods (they're plain top-level `operator` declarations, see
 * ParseOperatorDecl()'s own doc comment on why), so they don't inherit that
 * mechanism automatically; this function is what extends the same
 * convenience to them specifically for the self position. The index and
 * value operands get NO such relaxation -- they still go through plain
 * sameTypeExact(), same as every other operator.
 */
bool operatorSelfOperandMatches(const TurfType *ParamTy,
                                const TurfType *BaseTy) {
  if (sameTypeExact(ParamTy, BaseTy))
    return true;
  if (ParamTy && ParamTy->isReference()) {
    const auto *RefTy = dynamic_cast<const TurfReferenceType *>(ParamTy);
    if (RefTy && sameTypeExact(RefTy->getPointee().get(), BaseTy))
      return true;
  }
  return false;
}


/**
 * @brief Like sameTypeExact(), but for arrays, mirrors typesCompatible()'s
 *        existing size-erasure rule: a declared "unsized" array parameter
 *        (`int[]`, Size == -1) matches a receiver of ANY concrete size of
 *        the same element type. Plain sameTypeExact() can't do this because
 *        ArrayType::getName() now includes the size ("int[5]" != "int[]"),
 *        which is correct for ordinary type equality but would make a
 *        UFCS-callable `fn sum(arr: int[]): int` unreachable via `arr.sum()`
 *        for every concretely-sized array variable -- i.e. always, since
 *        variables are declared with a concrete size. This is not new
 *        coercion (RFC §5.4's receiver rule is about rejecting numeric
 *        promotion, e.g. int-to-double); it's the same size-genericity
 *        array parameters already have for ordinary calls, extended to the
 *        UFCS receiver position specifically.
 */
static bool ufcsReceiverTypeMatches(const TurfType *ParamTy,
                                     const TurfType *ActualTy) {
  if (ParamTy && ActualTy && ParamTy->isArray() && ActualTy->isArray()) {
    const auto *PArr = dynamic_cast<const ArrayType *>(ParamTy);
    const auto *AArr = dynamic_cast<const ArrayType *>(ActualTy);
    if (!sameTypeExact(PArr->getElementType(), AArr->getElementType()))
      return false;
    return PArr->getSize() == -1 || AArr->getSize() == -1 ||
           PArr->getSize() == AArr->getSize();
  }
  return sameTypeExact(ParamTy, ActualTy);
}


/**
 * @brief LANG-004 item 2: does @p ArgTy (a `string`) coerce to @p ParamTy
 *        (a raw-pointer parameter) at an `extern fn` call site?
 *
 * `string` and `ptr`/`T*`/`void*` already share the identical LLVM
 * representation (`isPointerLike()`, CodegenCore.cpp), so this is purely a
 * missing rule in argument matching, not a representation gap. Deliberately
 * a standalone predicate, checked only where the candidate is a real
 * `extern fn` (`Cand->IsExtern`) -- NOT folded into typesCompatible()
 * (Types.cpp), which stays exact on this pair for ordinary Turf code.
 * Widening typesCompatible() itself would let `string` silently bind to any
 * raw-pointer parameter everywhere, undermining §6.2's frozen conversion
 * matrix for the sake of one FFI convenience. See
 * docs/planning/ARCHITECTURE_OVERHAUL.md §8 LANG-004, item 2 ("Must NOT
 * reinterpret").
 */
static bool isExternStringToPtrCoercion(const TurfType *ArgTy,
                                        const TurfType *ParamTy) {
  if (!ArgTy || !ParamTy || !ArgTy->isString())
    return false;
  return (ParamTy->isPointer() && !ParamTy->isReference()) ||
         ParamTy->isNamed("ptr") || ParamTy->isNamed("void*");
}


// ─── Functions & calls ────────────────────────────────────────────────────────

/**
 * @brief Walk a candidate's declared parameter type against the actual
 *        argument type passed at a call site, recording any bare
 *        TurfGenericParamType encountered into @p Subst (keyed by its name).
 *
 * Recurses through pointer/array wrapping on both sides in lockstep (T* vs
 * concrete*, T[] vs concrete[]) so a generic parameter used underneath a
 * wrapper still gets unified correctly. The first argument position that
 * mentions a given generic parameter name wins; later positions mentioning
 * the same name are not re-unified (good enough for this compiler's actual
 * generic surface, where a type parameter typically appears in exactly one
 * "shape" per signature — see Resolver::substituteTypeParams()'s doc
 * comment for the parallel, opposite-direction transform this mirrors).
 */
static void collectGenericSubstitution(
    const TurfTypePtr &ParamTy, const TurfTypePtr &ArgTy,
    std::unordered_map<std::string, TurfTypePtr> &Subst) {
  if (!ParamTy || !ArgTy)
    return;

  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(ParamTy.get())) {
    Subst.emplace(GP->getName(), ArgTy);
    return;
  }
  if (const auto *PPtr = dynamic_cast<const TurfPointerType *>(ParamTy.get())) {
    if (const auto *APtr = dynamic_cast<const TurfPointerType *>(ArgTy.get()))
      collectGenericSubstitution(PPtr->getPointee(), APtr->getPointee(), Subst);
    return;
  }
  if (isArrayType(ParamTy.get()) && isArrayType(ArgTy.get())) {
    const auto *PArr = dynamic_cast<const ArrayType *>(ParamTy.get());
    const auto *AArr = dynamic_cast<const ArrayType *>(ArgTy.get());
    if (PArr && AArr)
      collectGenericSubstitution(PArr->getElementTypePtr(),
                                 AArr->getElementTypePtr(), Subst);
    return;
  }
  if (const auto *PGen = dynamic_cast<const GenericType *>(ParamTy.get())) {
    if (const auto *AGen = dynamic_cast<const GenericType *>(ArgTy.get())) {
      const auto &PArgs = PGen->getTypeArgs();
      const auto &AArgs = AGen->getTypeArgs();
      for (size_t i = 0; i < PArgs.size() && i < AArgs.size(); ++i)
        collectGenericSubstitution(PArgs[i], AArgs[i], Subst);
    }
    return;
  }
  // STD-003: a callback parameter's own signature (`cmp: fn(T, T): int`)
  // never recursed into here either -- see Types.cpp's identical FuncType
  // addition to typesCompatible() (this function's sibling: that one
  // decides IF a candidate matches, this one decides what T ACTUALLY WAS at
  // this call site once a candidate is accepted). Each parameter position
  // and the return type recurse the same way pointer/array/generic-argument
  // wrapping already does above.
  if (const auto *PFunc = dynamic_cast<const FuncType *>(ParamTy.get())) {
    if (const auto *AFunc = dynamic_cast<const FuncType *>(ArgTy.get())) {
      const auto &PParams = PFunc->getParamTypes();
      const auto &AParams = AFunc->getParamTypes();
      for (size_t i = 0; i < PParams.size() && i < AParams.size(); ++i)
        collectGenericSubstitution(PParams[i], AParams[i], Subst);
      collectGenericSubstitution(PFunc->getReturnType(), AFunc->getReturnType(), Subst);
    }
    return;
  }
}

/**
 * @brief TYPE-002 (S11b): recover a CANONICAL, declaration-order list of
 *        generic type-parameter names from a declared parameter type,
 *        mirroring collectGenericSubstitution()'s own recursive walk.
 *
 * Subst (built by collectGenericSubstitution(), above) is an
 * unordered_map -- its iteration order is not the declaration order and is
 * not even guaranteed stable across equivalent runs. An instantiation
 * request needs a canonical, ORDERED type-argument tuple (two requests for
 * the same instantiation must produce index-comparable vectors, not just
 * the same set of pairs), so this walks the same param-type shape a
 * second time purely to recover the order names were first seen in --
 * exactly how collectGenericSubstitution() itself decided insertion order,
 * just captured explicitly instead of thrown away.
 */
static void collectGenericParamNameOrder(const TurfTypePtr &ParamTy,
                                         std::vector<std::string> &Order) {
  if (!ParamTy)
    return;
  if (const auto *GP = dynamic_cast<const TurfGenericParamType *>(ParamTy.get())) {
    if (std::find(Order.begin(), Order.end(), GP->getName()) == Order.end())
      Order.push_back(GP->getName());
    return;
  }
  if (const auto *PPtr = dynamic_cast<const TurfPointerType *>(ParamTy.get())) {
    collectGenericParamNameOrder(PPtr->getPointee(), Order);
    return;
  }
  if (isArrayType(ParamTy.get())) {
    if (const auto *Arr = dynamic_cast<const ArrayType *>(ParamTy.get()))
      collectGenericParamNameOrder(Arr->getElementTypePtr(), Order);
    return;
  }
  if (const auto *PGen = dynamic_cast<const GenericType *>(ParamTy.get())) {
    for (const auto &A : PGen->getTypeArgs())
      collectGenericParamNameOrder(A, Order);
    return;
  }
  if (const auto *PFunc = dynamic_cast<const FuncType *>(ParamTy.get())) {
    for (const auto &P : PFunc->getParamTypes())
      collectGenericParamNameOrder(P, Order);
    collectGenericParamNameOrder(PFunc->getReturnType(), Order);
    return;
  }
}

/// TYPE-002 (S11b): turn a (possibly partial) Subst map into the ordered
/// TypeArgs vector an InstantiationRequest needs, using CanonicalOrder
/// (from collectGenericParamNameOrder() above) to fix the index order. A
/// name in CanonicalOrder with no Subst entry (a return-only phantom type
/// parameter that never appeared in any argument position) is skipped --
/// the request is still meaningful over the parameters that DID resolve.
static std::vector<TurfTypePtr> orderedTypeArgs(
    const std::vector<std::string> &CanonicalOrder,
    const std::unordered_map<std::string, TurfTypePtr> &Subst) {
  std::vector<TurfTypePtr> Args;
  Args.reserve(CanonicalOrder.size());
  for (const auto &Name : CanonicalOrder) {
    auto It = Subst.find(Name);
    if (It != Subst.end())
      Args.push_back(It->second);
  }
  return Args;
}


/**
 * @brief Rewrite @p Ty, replacing any bare TurfGenericParamType occurrence
 *        (recursively through pointer/array/generic wrapping) with its
 *        concrete type from @p Subst, if one was recorded.
 *
 * This is what actually closes the call-site type-inference gap: without
 * it, TypeChecker::getType() for a call to `fn<T> firstElem(arr: T[]): T`
 * returned the callee's literal declared return type -- TurfGenericParamType
 * ("T") itself -- regardless of what concrete type T was instantiated to at
 * this call site, so `firstElem(intArray) == 10` failed type-checking with
 * "Cannot compare 'T' with 'int'" even though the call was perfectly valid.
 * A type parameter with no corresponding entry in Subst (never appeared in
 * any parameter position, e.g. a return-only phantom type) is left as-is.
 *
 * TYPE-002 (S12a): this is now a thin wrapper. The real logic moved to
 * Monomorphize::substituteConcreteTypes() -- it turned out to already BE
 * the "concrete-map variant" the monomorphizer needs (S11a's plan named
 * the wrong function to generalize; see the change record's *Removed
 * assumptions*). Delegating keeps every existing call site in this file
 * (and its behaviour) unchanged, verbatim.
 */
static TurfTypePtr substituteReturnType(
    const TurfTypePtr &Ty,
    const std::unordered_map<std::string, TurfTypePtr> &Subst) {
  return Monomorphize::substituteConcreteTypes(Ty, Subst);
}


void TypeChecker::visitFuncCall(FuncCallExprAST &Node) {
  // 1. Visit callee expression so it gets a type annotation too.
  if (Node.getCallee())
    const_cast<ExprAST *>(Node.getCallee())->accept(*this);

  // 2. Visit all argument expressions bottom-up; collect inferred types.
  std::vector<TurfTypePtr> ArgTypes;
  for (const auto &Arg : Node.getArgs()) {
    Arg->accept(*this);
    ArgTypes.push_back(getType(Arg.get()));
  }
  const int ArgCount = static_cast<int>(Node.getArgs().size());

  // 3. Determine the callee name for diagnostics and builtin lookup. Also
  // covers a `Left::Right(...)` callee (module call, static method,
  // StructName::new/default) -- previously only VariableExprAST was
  // handled here, leaving CalleeName empty for every qualified call's own
  // diagnostics (arity/type-mismatch messages below would print `''`
  // instead of e.g. `'new'`); Resolver::visitFuncCall and
  // Codegen::FuncCallExprAST::codegen() already derive it the same way.
  std::string CalleeName;
  const auto *ScopeCallee =
      dynamic_cast<const ScopeAccessExprAST *>(Node.getCallee());
  if (const auto *VE =
          dynamic_cast<const VariableExprAST *>(Node.getCallee())) {
    CalleeName = VE->getName();
  } else if (ScopeCallee) {
    CalleeName = ScopeCallee->getRightName();
  }

  // `StructName::default()` -- compiler-synthesized, no ResolverFunctionSymbol
  // at all (Resolver::visitFuncCall never populates ResolvedCandidates for
  // this kind), so it must be short-circuited here before the ordinary
  // builtin/candidate paths below, which would otherwise treat it as an
  // unresolved call and leave its type null. `StructName::new(...)` needs
  // NO equivalent branch here -- Resolver already synthesizes a self-
  // stripped candidate with ReturnType overridden to the struct's own type,
  // so it flows through the ordinary "User-defined / Resolver-registered
  // call path" below completely unmodified.
  if (ScopeCallee && ScopeCallee->ResolvedKind == ScopeAccessKind::DefaultConstruct) {
    setType(&Node, std::make_shared<NamedType>(ScopeCallee->getLeftName()));
    return;
  }

  // STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(c)): `ErrorType::Variant(
  // messageExpr)` -- exactly one `string` argument, the message attached to
  // this specific error occurrence. Short-circuited here for the identical
  // reason DefaultConstruct is just above: Resolver::visitFuncCall never
  // populates ResolvedCandidates for this kind either (ResolverCall.cpp's
  // isErrorTypeName() branch), so the ordinary candidate-matching path
  // below would treat it as unresolved. Result type is the SAME
  // TurfUserErrorType a bare `ErrorType::Variant` reference already gets
  // (visitScopeAccess()'s ErrorVariantRef branch) -- constructing WITH a
  // message doesn't change what kind of value this is, only what runtime
  // data it carries (Codegen's ErrorVariantConstructWithMessage branch,
  // CodegenFunc.cpp).
  if (ScopeCallee &&
      ScopeCallee->ResolvedKind == ScopeAccessKind::ErrorVariantConstructWithMessage) {
    if (ArgCount == 1 && ArgTypes[0] && !ArgTypes[0]->isNamed("string")) {
      reportError(Node.getLoc(),
                  "'" + ScopeCallee->getLeftName() + "::" + CalleeName +
                      "(...)' expects a 'string' message, got '" +
                      ArgTypes[0]->getName() + "'.",
                  {}, "Pass a 'string' expression as the error's message.",
                  "Wrong message type!");
    }
    setType(&Node, std::make_shared<NamedType>(ScopeCallee->getLeftName()));
    return;
  }

  // ── Bare language intrinsic path ─────────────────────────────────────────
  // Only 'typeof' is a true bare intrinsic (callable without a module
  // qualifier and not registered as user functions).
  // Everything else that used to live here (abs, power, print, input,
  // malloc, lengthof…) now lives in lib/std and is resolved via the
  // Resolver's module registry (lengthof's replacement is
  // strings::length(s), part of the prelude).
  // LookupBuiltin() returns null for all stdlib functions since Issue #9.
  if (Node.ResolvedCandidates.empty() && !CalleeName.empty()) {
    const BuiltinInfo *BI = LookupBuiltin(CalleeName);
    if (BI) {
      // Arity check using the canonical arity from Builtins.h.
      if (BI->Arity >= 0 && ArgCount != BI->Arity) {
        reportError(Node.getLoc(),
                    "'" + CalleeName + "' expects " +
                        std::to_string(BI->Arity) + " argument" +
                        (BI->Arity == 1 ? "" : "s") + ", got " +
                        std::to_string(ArgCount) + ".",
                    {}, "Check the number of arguments.",
                    "Wrong argument count!");
        setType(&Node, nullptr);
        return;
      }

      // ── typeof(x) → string ───────────────────────────────────────────────
      if (CalleeName == "typeof") {
        setType(&Node, TurfString());
        return;
      }

      // ── gc_alloc(n) / gc_alloc_zeroed(n) / gc_realloc(p, n) → void* ────────
      // MEM-001. All three always evaluate to a raw `void*` -- exactly the
      // same "untyped result, caller casts" contract mem::alloc()/
      // mem::realloc() already have (see e.g. list.tr's
      // `mem::malloc(byteSize) to T*`, the pattern every rerouted call site
      // keeps using with gc_alloc/gc_realloc in its place). No argument-type
      // validation beyond the arity check above -- same minimal-validation
      // convention typeof already uses; a non-int byte-count argument would
      // simply codegen with whatever LLVM width Args[0]->codegen() produces
      // (both runtime parameters are already-64-bit size_t, matching Turf's
      // 64-bit `int`).
      if (CalleeName == "gc_alloc" || CalleeName == "gc_alloc_zeroed" ||
          CalleeName == "gc_realloc") {
        setType(&Node, std::make_shared<TurfPointerType>(TurfVoid()));
        return;
      }

      // ── gc_collect() → void / gc_is_managed(p) → bool ──────────────────────
      // MEM-002. gc_collect() forces one mark-sweep cycle (turf_rt_collect);
      // gc_is_managed(p) is the exact-block-start query lib/std/mem/mem.tr's
      // checked free() uses to diagnose a cross-heap mem::free() call. Same
      // minimal-validation convention as gc_alloc's own branch above.
      if (CalleeName == "gc_collect") {
        setType(&Node, TurfVoid());
        return;
      }
      if (CalleeName == "gc_is_managed") {
        setType(&Node, TurfBool());
        return;
      }
      if (CalleeName == "gc_checked_free") {
        setType(&Node, TurfVoid());
        return;
      }

      // assert/fail used to have bespoke branches here (assert needed a
      // special Arity == -1 sentinel to express its 1-or-2-arg signature,
      // since it isn't a single fixed count the generic check above can
      // express). Both are now ordinary, real overloaded Turf functions in
      // lib/std/test.tr -- LookupBuiltin() returns null for them (they're
      // not in g_builtins at all anymore), so they never reach this branch;
      // they resolve through the exact same "User-defined / Resolver-
      // registered call path" below as any other function, including
      // proper per-overload arity/type checking for free.

      // Any other name that hits LookupBuiltin() is unexpected — fall through.
    }
  }

  // ── User-defined / Resolver-registered call path ──────────────────────────
  if (!Node.ResolvedCandidates.empty()) {
    // Step a: filter to candidates whose arity matches.
    // Vararg functions accept any ArgCount >= their declared param count.
    std::vector<std::shared_ptr<ResolverFunctionSymbol>> ArityMatches;
    for (const auto &Cand : Node.ResolvedCandidates) {
      if (Cand && (Cand->arity() == ArgCount ||
                   (Cand->isVarArg() && ArgCount >= Cand->arity())))
        ArityMatches.push_back(Cand);
    }

    if (ArityMatches.empty()) {
      // Build a human-readable list of known overloads for the error message.
      std::string Overloads;
      std::vector<RelatedLocation> Related;
      for (const auto &Cand : Node.ResolvedCandidates) {
        if (Cand) {
          if (!Overloads.empty()) Overloads += ", ";
          Overloads += Cand->signature();
          Related.push_back({toSourceLocation(Cand->DeclLoc),
                             "overload: " + Cand->signature()});
        }
      }
      reportError(Node.getLoc(),
                  "'" + CalleeName + "' called with " +
                      std::to_string(ArgCount) + " argument" +
                      (ArgCount == 1 ? "" : "s") +
                      "; known overloads: " + Overloads + ".",
                  Related, "Check the number of arguments.",
                  "Wrong argument count!");
      setType(&Node, nullptr);
      return;
    }

    // Step b: filter arity-matching candidates by type compatibility.
    // For vararg functions, only check the declared (named) params.
    // Extra args beyond Cand->arity() are passed raw per C ABI.
    std::vector<std::shared_ptr<ResolverFunctionSymbol>> TypeMatches;
    for (const auto &Cand : ArityMatches) {
      bool AllOk = true;
      int CheckCount = std::min(ArgCount, Cand->arity());
      for (int i = 0; i < CheckCount; ++i) {
        const TurfType *ParamTy = Cand->Params[i].Type.get();
        const TurfType *ArgTy   = ArgTypes[i].get();
        // OVH-005: an untyped literal argument acquires 'int32' directly --
        // see untypedLiteralAcquiresInt32()'s own doc comment. Matters most
        // here since int32 exists specifically for `extern fn` ABI
        // declarations (F-NUM-5), the overwhelmingly common place a
        // parameter is actually declared int32.
        if (ParamTy && ArgTy && !typesCompatible(ArgTy, ParamTy) &&
            !(Cand->IsExtern && isExternStringToPtrCoercion(ArgTy, ParamTy)) &&
            !untypedLiteralAcquiresInt32(Node.getArgs()[i].get(), ParamTy)) {
          AllOk = false;
          break;
        }
      }
      if (AllOk)
        TypeMatches.push_back(Cand);
    }

    if (TypeMatches.empty()) {
      // Build argument-type string for the error.
      std::string ArgTypeStr;
      std::vector<RelatedLocation> Related;
      bool AnyNullArg = false;
      for (int i = 0; i < ArgCount; ++i) {
        if (i) ArgTypeStr += ", ";
        ArgTypeStr += (ArgTypes[i] ? ArgTypes[i]->getName() : "unknown");
        if (ArgTypes[i] && ArgTypes[i]->isNull())
          AnyNullArg = true;
      }
      for (const auto &Cand : ArityMatches)
        if (Cand)
          Related.push_back({toSourceLocation(Cand->DeclLoc),
                             "candidate: " + Cand->signature()});
      reportError(Node.getLoc(),
                  "No overload of '" + CalleeName +
                      "' matches argument types (" + ArgTypeStr + ").",
                  Related,
                  AnyNullArg
                      ? "'null' is only valid for pointer-typed parameters -- "
                        "none of the overloads above take a pointer here."
                      : "",
                  "No matching overload!");
      setType(&Node, nullptr);
      return;
    }

    // Step c: narrow to the best match (if multiple, pick first — exact
    // matches will naturally appear before promotion matches because the
    // Resolver registers them in declaration order).
    // A future refinement can count implicit casts and pick fewest.
    auto Best = TypeMatches[0];
    if (TypeMatches.size() > 1) {
      // Prefer an exact-match candidate over a numeric-promotion one.
      for (const auto &Cand : TypeMatches) {
        bool ExactAll = true;
        int CheckCount = std::min(ArgCount, Cand->arity());
        for (int i = 0; i < CheckCount; ++i) {
          const TurfType *P = Cand->Params[i].Type.get();
          const TurfType *A = ArgTypes[i].get();
          if (P && A && P->getName() != A->getName()) {
            ExactAll = false;
            break;
          }
        }
        if (ExactAll) { Best = Cand; break; }
      }
    }

    Node.ResolvedCallTarget = Best;

    // SEM-002 -- see visitVarDecl()'s identical comment (TypeCheckerDecl.cpp).
    checkCallArgsRepresentable(Node.getArgs(), Best->Params, Node.getLoc());

    // If Best's return type mentions one of its own generic type parameters
    // (e.g. `fn<T> firstElem(arr: T[]): T`), report the CONCRETE type it was
    // instantiated to at this call site instead of the literal declared
    // TurfGenericParamType -- see collectGenericSubstitution()'s doc comment
    // for the gap this closes. A no-op (empty substitution map) for the
    // overwhelming majority of calls, which aren't generic at all.
    std::unordered_map<std::string, TurfTypePtr> Subst;
    int SubstCheckCount = std::min(ArgCount, Best->arity());
    for (int i = 0; i < SubstCheckCount; ++i)
      collectGenericSubstitution(Best->Params[i].Type, ArgTypes[i], Subst);

    // TYPE-002 (S11b): persist onto the node instead of letting Subst go
    // out of scope unread -- see FuncCallExprAST::GenericSubstitution's
    // doc comment (ASTAccess.h). Only when non-empty: an empty map means
    // this call isn't generic at all, and is not an instantiation request.
    if (!Subst.empty()) {
      Node.GenericSubstitution = Subst;
      // Report-only: feed the (opt-in, --dump-instantiations) collector.
      // A no-op whenever MonoCollector_ is null -- every ordinary compile.
      if (MonoCollector_) {
        std::vector<std::string> Order;
        for (int i = 0; i < SubstCheckCount; ++i)
          collectGenericParamNameOrder(Best->Params[i].Type, Order);
        std::vector<TurfTypePtr> TypeArgs = orderedTypeArgs(Order, Subst);
        // TYPE-002 (S14a): only a FULLY CONCRETE substitution is a real
        // instantiation request -- found live, not anticipated: a generic
        // function calling ANOTHER generic function through its own STILL-
        // OPEN type parameter (`fn<T> outer(x: T): T { return inner(x); }`,
        // checked as part of `outer`'s own un-substituted TEMPLATE body,
        // before outer is ever cloned) unifies inner's T against outer's
        // OWN T -- a non-empty but NOT concrete "substitution" (one open
        // parameter mapped to another). Harmless under erasure (the
        // resulting bogus "inner<T>" clone was dead code nothing called,
        // and getLLVMType(TurfGenericParamType) silently returned ptr) --
        // but a hard INV-GEN-1 assertion failure once erasure is deleted,
        // since that bogus clone's own literal TurfGenericParamType
        // parameter now reaches Codegen for real. See
        // isFullyConcreteType()'s own doc comment (TypeCheckerInternal.h).
        bool AllConcrete = true;
        for (const auto &A : TypeArgs)
          if (!isFullyConcreteType(A.get())) { AllConcrete = false; break; }
        if (AllConcrete) {
          // Best->BaseName, not CalleeName: for an ordinary free function
          // call the two always agree, but for `Box::new(...)` (a
          // constructor call routed through this same free-function path --
          // see the Resolver's "self-stripped candidate" comment above)
          // CalleeName is the user-facing spelled name "new" while the real
          // declaration Best resolved to is named "Box.new" -- the ONLY name
          // the monomorphizer's declaration registry (built from
          // FuncDefExprAST::getName(), Monomorphize.cpp) can ever find it
          // under. Found live: `Box::new(42)` failed with "no generic
          // declaration found for 'new'" before this fix.
          MonoCollector_->record({Monomorphize::InstantiationKind::FreeFunction,
                                 Best->BaseName, Best->SourceModule,
                                 std::move(TypeArgs), Node.getLoc()},
                                &Node);
        }
      }
    }

    setType(&Node, substituteReturnType(Best->ReturnType, Subst));
    return;
  }

  // ── Indirect call through a function-typed variable/parameter ───────────
  // `f(3)` where `f: fn(int): int` -- Resolver deliberately leaves
  // ResolvedCandidates/ResolvedCallTarget empty for this shape (see
  // Resolver::visitFuncCall's function-value branch) and instead attaches an
  // ordinary ResolvedSymbol to the VariableExprAST callee, exactly like any
  // other variable read -- so step 1 above (visiting the callee) already
  // gave it the right type via the ordinary visitVariable path, and that IS
  // the FuncType to check this call's arguments against.
  {
    const auto *VE = dynamic_cast<const VariableExprAST *>(Node.getCallee());
    if (VE && VE->getResolvedSymbol()) {
      TurfTypePtr CalleeTy = getType(Node.getCallee());
      if (CalleeTy && CalleeTy->isFunc()) {
        const auto *FT = static_cast<const FuncType *>(CalleeTy.get());
        const auto &ParamTypes = FT->getParamTypes();
        if (static_cast<int>(ParamTypes.size()) != ArgCount) {
          reportError(Node.getLoc(),
                      "'" + CalleeName + "' expects " +
                          std::to_string(ParamTypes.size()) + " argument" +
                          (ParamTypes.size() == 1 ? "" : "s") + ", got " +
                          std::to_string(ArgCount) + ".",
                      {}, "Check the number of arguments.",
                      "Wrong argument count!");
          setType(&Node, nullptr);
          return;
        }
        for (int i = 0; i < ArgCount; ++i) {
          if (ParamTypes[i] && ArgTypes[i] &&
              !typesCompatible(ArgTypes[i].get(), ParamTypes[i].get())) {
            reportError(Node.getLoc(),
                        "Argument " + std::to_string(i + 1) + " to '" +
                            CalleeName + "' expects '" +
                            ParamTypes[i]->getName() + "', got '" +
                            ArgTypes[i]->getName() + "'.");
            setType(&Node, nullptr);
            return;
          }
        }
        setType(&Node, FT->getReturnType());
        return;
      }
    }
  }

  // ── Unknown call (no builtin, no candidates) — be permissive. ─────────────
  // The Resolver should have caught truly unknown functions already.  Leave
  // the type as null so we don't cascade spurious errors.
  setType(&Node, nullptr);
}


bool TypeChecker::tryUFCSFallback(MethodCallExprAST &Node,
                                  const TurfType *BaseTy,
                                  const std::vector<TurfTypePtr> &ArgTypes,
                                  std::vector<const ResolverFunctionSymbol *> *OutNamedCandidates) {
  if (!BaseTy)
    return false;
  const std::string &MethodName = Node.getMethod();
  const int ArgCount = static_cast<int>(ArgTypes.size());

  // Gather candidates: self-file free functions + every module known/hoisted
  // anywhere in the compile (Option A visibility, same rule already applied
  // to `operator` resolution -- RFC §5.3 explicitly says "same rule as
  // operators").
  std::vector<const ResolverFunctionSymbol *> Candidates;
  for (const auto *S : RST_.lookupCurrentFileFunctions(MethodName))
    Candidates.push_back(S);
  for (const auto &M : RST_.findInModules(MethodName))
    if (M.Symbol)
      Candidates.push_back(M.Symbol);

  // Populated regardless of whether this call ultimately resolves -- a
  // caller with no other fallback left (the primitive-receiver path) needs
  // this even on failure, to tell "no such function anywhere" apart from
  // "it exists, just not for this receiver/arity" (see this parameter's
  // doc comment in TypeChecker.h).
  if (OutNamedCandidates)
    *OutNamedCandidates = Candidates;

  // Receiver (implicit self-argument) matching is EXACT-name-match only --
  // no numeric coercion -- RFC §5.4 (arrays get the same unsized-matches-
  // any-size leniency ordinary calls already have, see
  // ufcsReceiverTypeMatches()). Arity must also account for the receiver
  // occupying the first declared parameter slot.
  std::vector<const ResolverFunctionSymbol *> ReceiverMatches;
  for (const auto *C : Candidates) {
    if (C->arity() == ArgCount + 1 && !C->Params.empty() &&
        ufcsReceiverTypeMatches(C->Params[0].Type.get(), BaseTy))
      ReceiverMatches.push_back(C);
  }
  if (ReceiverMatches.empty())
    return false;

  // The REMAINING, explicitly-written call arguments are not an implicit
  // binding -- they use the ordinary typesCompatible() coercion rules, same
  // as any other function call (RFC §5.4's "no coercion" language is scoped
  // to the receiver specifically, not the whole call).
  std::vector<const ResolverFunctionSymbol *> TypeMatches;
  for (const auto *C : ReceiverMatches) {
    bool AllOk = true;
    for (int i = 0; i < ArgCount; ++i) {
      const TurfType *ParamTy = C->Params[i + 1].Type.get();
      const TurfType *ArgTy = ArgTypes[i].get();
      // OVH-005: see the identical exception in visitFuncCall() above.
      if (ParamTy && ArgTy && !typesCompatible(ArgTy, ParamTy) &&
          !untypedLiteralAcquiresInt32(Node.getArgs()[i].get(), ParamTy)) {
        AllOk = false;
        break;
      }
    }
    if (AllOk)
      TypeMatches.push_back(C);
  }
  if (TypeMatches.empty())
    return false;

  // Prefer an exact-match-on-remaining-args candidate over a numeric-
  // promotion one, matching visitFuncCall's identical tie-break -- but if
  // MORE THAN ONE candidate is exact on every remaining arg too, that's a
  // genuine identical-signature conflict (RFC §5.4's universal conflict
  // rule / §4.3), not a preference to silently resolve.
  const ResolverFunctionSymbol *Best = TypeMatches[0];
  const ResolverFunctionSymbol *ExactMatch = nullptr;
  for (const auto *C : TypeMatches) {
    bool ExactAll = true;
    for (int i = 0; i < ArgCount; ++i) {
      const TurfType *P = C->Params[i + 1].Type.get();
      const TurfType *A = ArgTypes[i].get();
      if (P && A && P->getName() != A->getName()) {
        ExactAll = false;
        break;
      }
    }
    if (ExactAll) {
      if (ExactMatch) {
        reportError(Node.getLoc(),
                    "Ambiguous UFCS call to '" + MethodName +
                        "': more than one visible free-function overload "
                        "matches this call site exactly.",
                    {}, "", "Ambiguous call!");
        setType(&Node, nullptr);
        return true; // handled (as an error) -- caller must not also report
      }
      ExactMatch = C;
    }
  }
  if (ExactMatch)
    Best = ExactMatch;

  Node.ResolvedCallTarget = std::make_shared<ResolverFunctionSymbol>(*Best);
  Node.ResolvedViaUFCS = true;
  // SEM-002 -- see visitVarDecl()'s identical comment (TypeCheckerDecl.cpp).
  // ParamOffset=1 skips the receiver slot (Params[0]), which has no
  // corresponding entry in Node.getArgs() -- the explicit args start at
  // Params[1], per this function's own receiver-binding comments above.
  checkCallArgsRepresentable(Node.getArgs(), Best->Params, Node.getLoc(), 1);
  setType(&Node, Best->ReturnType);
  return true;
}


void TypeChecker::visitMethodCall(MethodCallExprAST &Node) {
  // 1. Visit the base expression (e.g. `io` in io.printline(...)).
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);

  // 2. Visit all argument expressions bottom-up; collect inferred types.
  std::vector<TurfTypePtr> ArgTypes;
  for (const auto &Arg : Node.getArgs()) {
    Arg->accept(*this);
    ArgTypes.push_back(getType(Arg.get()));
  }
  const int ArgCount = static_cast<int>(Node.getArgs().size());

  const std::string &MethodName = Node.getMethod();

  // 3. Detect whether the base is a module qualifier.
  //    The Resolver marks `io` in `io.printline(...)` by setting
  //    ResolvedModuleName on the VariableExprAST.
  std::string ModuleName;
  if (const auto *BaseVar =
          dynamic_cast<const VariableExprAST *>(Node.getBase())) {
    ModuleName = BaseVar->ResolvedModuleName;
  }

  if (!ModuleName.empty()) {
    // 4. Module-qualified call: look up all matching overloads in that module.
    auto Matches = RST_.findInModules(MethodName);

    // Collect just the candidates from the right module.
    std::vector<const ResolverFunctionSymbol *> Candidates;
    for (const auto &M : Matches) {
      if (M.ModuleName == ModuleName && M.Symbol)
        Candidates.push_back(M.Symbol);
    }

    if (Candidates.empty()) {
      // Unknown module function — be permissive (Resolver should have caught).
      setType(&Node, nullptr);
      return;
    }

    // 5. Apply same arity → type-match → narrowing logic as visitFuncCall.
    //    a. Arity filter.
    std::vector<const ResolverFunctionSymbol *> ArityMatches;
    for (const auto *Cand : Candidates) {
      if (Cand->arity() == ArgCount)
        ArityMatches.push_back(Cand);
    }

    if (ArityMatches.empty()) {
      std::string Overloads;
      std::vector<RelatedLocation> Related;
      for (const auto *Cand : Candidates) {
        if (!Overloads.empty()) Overloads += ", ";
        Overloads += ModuleName + "." + Cand->signature();
        Related.push_back({toSourceLocation(Cand->DeclLoc),
                           "overload: " + ModuleName + "." + Cand->signature()});
      }
      reportError(Node.getLoc(),
                  "'" + ModuleName + "." + MethodName + "' called with " +
                      std::to_string(ArgCount) + " argument" +
                      (ArgCount == 1 ? "" : "s") +
                      "; known overloads: " + Overloads + ".",
                  Related, "Check the number of arguments.",
                  "Wrong argument count!");
      setType(&Node, nullptr);
      return;
    }

    //    b. Type-compatibility filter.
    std::vector<const ResolverFunctionSymbol *> TypeMatches;
    for (const auto *Cand : ArityMatches) {
      bool AllOk = true;
      for (int i = 0; i < ArgCount; ++i) {
        const TurfType *ParamTy = Cand->Params[i].Type.get();
        const TurfType *ArgTy   = ArgTypes[i].get();
        // OVH-005: see the identical exception in visitFuncCall() above.
        if (ParamTy && ArgTy && !typesCompatible(ArgTy, ParamTy) &&
            !(Cand->IsExtern && isExternStringToPtrCoercion(ArgTy, ParamTy)) &&
            !untypedLiteralAcquiresInt32(Node.getArgs()[i].get(), ParamTy)) {
          AllOk = false;
          break;
        }
      }
      if (AllOk)
        TypeMatches.push_back(Cand);
    }

    if (TypeMatches.empty()) {
      std::string ArgTypeStr;
      std::vector<RelatedLocation> Related;
      for (int i = 0; i < ArgCount; ++i) {
        if (i) ArgTypeStr += ", ";
        ArgTypeStr += (ArgTypes[i] ? ArgTypes[i]->getName() : "unknown");
      }
      for (const auto *Cand : ArityMatches)
        Related.push_back({toSourceLocation(Cand->DeclLoc),
                           "candidate: " + ModuleName + "." + Cand->signature()});
      reportError(Node.getLoc(),
                  "No overload of '" + ModuleName + "." + MethodName +
                      "' matches argument types (" + ArgTypeStr + ").",
                  Related, "", "No matching overload!");
      setType(&Node, nullptr);
      return;
    }

    //    c. Pick best candidate.
    const ResolverFunctionSymbol *Best = TypeMatches[0];
    if (TypeMatches.size() > 1) {
      for (const auto *Cand : TypeMatches) {
        bool ExactAll = true;
        for (int i = 0; i < ArgCount; ++i) {
          const TurfType *P = Cand->Params[i].Type.get();
          const TurfType *A = ArgTypes[i].get();
          if (P && A && P->getName() != A->getName()) {
            ExactAll = false;
            break;
          }
        }
        if (ExactAll) { Best = Cand; break; }
      }
    }

    // Set ResolvedCallTarget so Codegen can read mangledName() directly
    // without any overload resolution.  We use a shared_ptr with a no-op
    // deleter because the symbol is owned by the ResolverSymbolTable; we
    // just need an owning handle that Codegen can call ->mangledName() on.
    Node.ResolvedCallTarget = std::make_shared<ResolverFunctionSymbol>(*Best);
    // SEM-002 -- see visitVarDecl()'s identical comment (TypeCheckerDecl.cpp).
    checkCallArgsRepresentable(Node.getArgs(), Best->Params, Node.getLoc());
    setType(&Node, Best->ReturnType);
    return;
  }

  // ── Struct method call (e.g. u.grow(), a.inc()) ──────────────────────────
  // The base is a variable whose type is a struct. Struct methods are
  // registered in the Resolver's CurrentFileFunctions_ with BaseName
  // "StructName.methodName" (e.g. "User.grow").
  {
    TurfTypePtr BaseTy = getType(Node.getBase());
    bool IsStaticMethod = false;
    if (auto *Var = dynamic_cast<const VariableExprAST *>(Node.getBase())) {
      if (Var->IsStructName && GlobalSymbolTable) {
        IsStaticMethod = true;
        BaseTy = GlobalSymbolTable->LookupType(Var->getName());
      }
    }

    // `arr.length()` on a fixed-size array (int[3], not a struct at all) is
    // a builtin Codegen already special-cases (MethodCallExprAST::codegen(),
    // `if (Method == "length") { if (isArrayType(BaseTy)) ... }`) -- there
    // was never a TypeChecker-side check for it at all; it only ever worked
    // by accident via the old "be permissive" fallback leaving
    // ResolvedCallTarget null and Codegen's special-case bypassing it
    // entirely. Once that fallback started raising a real error for
    // non-struct method calls (see StructMethodNotFoundError's sibling
    // check just below), this legitimate case needed its own explicit,
    // correct handling instead of continuing to rely on silence.
    if (BaseTy && isArrayType(BaseTy.get()) && MethodName == "length" &&
        ArgCount == 0) {
      setType(&Node, TurfInt());
      return;
    }

    // Unwrap pointer-to-struct (self inside methods)
    if (BaseTy && !IsStaticMethod) {
      if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy.get()))
        BaseTy = PtrTy->getPointee();
    }
    // Resolve NamedType to TurfStructType
    if (BaseTy) {
      if (auto *NT = dynamic_cast<const NamedType *>(BaseTy.get())) {
        if (GlobalSymbolTable)
          BaseTy = GlobalSymbolTable->LookupType(NT->getName());
      }
    }
    // Snapshot BaseTy here, BEFORE the bare-name GenericType->TurfStructType
    // lookup just below replaces it with the un-parameterized template type
    // -- this is the last point where BaseTy can still be a genuine
    // `GenericType` carrying real type arguments. The struct-generic-
    // substitution recovery further down (`Subst`) needs exactly this value
    // for a `self`-based call: `self`'s OWN declared/registered type is
    // always `TurfPointerType(GenericType(StructName, Args))` (see the
    // pointer-unwrap just above), never a bare GenericType, so re-deriving
    // it there via a second, independent `getType(Node.getBase())` call (as
    // this used to do) dynamic_casts a POINTER type to GenericType and
    // always fails silently -- `Subst` came back permanently empty for
    // every `self.method(...)` call specifically (an ordinary variable like
    // `b: Box<int>` was never affected: its own type already IS a bare
    // GenericType with no pointer wrapper to strip). This stayed invisible
    // whenever the CALLEE's own parameters happened to independently
    // mention the struct's type parameter (`MethodOwnSubst`, built below
    // from matching the callee's declared param types against the caller's
    // already-concrete arguments, filled the exact same map entries through
    // an unrelated path) -- it only surfaced as a real, user-reachable bug
    // for a generic-struct method whose parameters DON'T mention the
    // struct's type parameter at all (e.g. `fn<T> Box.bar(x: int): int`)
    // called via `self.bar(...)` from a sibling method: both substitution
    // sources came back empty, `CombinedSubst` was empty, the Method-kind
    // instantiation request was never recorded, no clone was ever
    // generated, and Codegen failed with "Internal error: LLVM function
    // '...' not found after successful name resolution" trying to call a
    // function that was never built. Minimal repro:
    // `struct Box<T> { v: T } fn<T> Box.bar(x: int): int { return 0; }
    // fn<T> Box.foo(x: int): int { return self.bar(x); }` -- confirmed to
    // reproduce with a non-generic-struct comparison
    // (`lib/std/json/json.tr`'s `Parser.skip_ws()`, called the identical
    // way from a sibling method, unaffected) ruling out every other
    // explanation first.
    TurfTypePtr UnwrappedGenericBaseTy = BaseTy;
    // Resolve GenericType (e.g. a `Box<int>`-typed variable) to its
    // underlying TurfStructType via the base name -- mirrors the NamedType
    // resolution just above. Without this, calling a method on a generic
    // struct instance (`b.get()` where `b: Box<int>`) fell all the way
    // through to "Non-module, non-struct method call — be permissive" at
    // the bottom of this function, leaving Node.ResolvedCallTarget null.
    // Codegen's MethodCallExprAST::codegen() then silently returned nullptr
    // with NO diagnostic (MangledName stays empty, CalleeF stays null, and
    // the struct-method-call branch just `return nullptr;`), which meant
    // any variable declared from that call's result was never registered
    // in Codegen's symbol table -- surfacing only as a confusing
    // "you're trying to use 'p' before telling me what it is" error the
    // next time that variable was actually referenced somewhere codegen
    // could reach (e.g. inside assert()), nowhere near the real cause.
    if (BaseTy) {
      if (auto *GT = dynamic_cast<const GenericType *>(BaseTy.get())) {
        if (GlobalSymbolTable)
          BaseTy = GlobalSymbolTable->LookupType(GT->getBaseName());
      }
    }
    // ── Machine transition call (e.g. conn.transition(ConnectionState::X)) ──
    // Checked before the struct-method dynamic_cast just below so a machine
    // receiver never falls through to the permissive "no such method" UFCS
    // fallback with zero diagnostic -- same reasoning as the arr.length()
    // special case above. `transition` is deliberately NOT a reserved word
    // (see TurfMachineType's doc comment, include/Types.h): a machine
    // receiver whose method name ISN'T "transition" falls straight through
    // to ordinary UFCS/struct dispatch below unchanged, so a user's own
    // unrelated `fn transition(...)` still works via UFCS on a non-machine
    // receiver.
    //
    // If an enclosing compare arm narrowed this call's receiver to one
    // specific state (TypeChecker::visitCompare()'s MachineNarrowStack_),
    // the edge is checked statically here instead of deferring to Codegen's
    // runtime switch+panic guard -- see MachineNarrowStack_'s doc comment
    // (include/TypeChecker.h) for exactly which arm shapes qualify.
    if (BaseTy) {
      if (auto *MT = dynamic_cast<const TurfMachineType *>(BaseTy.get())) {
        if (MethodName == "transition") {
          if (ArgCount != 1) {
            reportError(Node.getLoc(),
                        "'transition' expects exactly 1 argument (the target "
                        "state), got " + std::to_string(ArgCount) + ".",
                        {}, "", "Wrong argument count!");
            setType(&Node, TurfVoid());
            return;
          }

          // The target must resolve to a state of THIS SAME machine type
          // (Color::Red-style ScopeAccess -- ArgTypes[0] is already the
          // TypeChecker-resolved type of that access, see visitScopeAccess()).
          const TurfType *TargetTy = ArgTypes[0].get();
          const auto *TargetMT =
              TargetTy ? dynamic_cast<const TurfMachineType *>(TargetTy) : nullptr;
          if (!TargetMT || TargetMT->getName() != MT->getName()) {
            InvalidTransitionTargetError(
                Node.getLoc(), MT->getName(),
                TargetTy ? TargetTy->getName() : "unknown")
                .raise();
            setType(&Node, TurfVoid());
            return;
          }

          const std::string *NarrowedState = nullptr;
          if (const auto *BaseVar =
                  dynamic_cast<const VariableExprAST *>(Node.getBase())) {
            for (auto It = MachineNarrowStack_.rbegin();
                 It != MachineNarrowStack_.rend(); ++It) {
              if (It->VarName == BaseVar->getName() &&
                  It->MachineName == MT->getName()) {
                NarrowedState = &It->StateName;
                break;
              }
            }
          }

          if (NarrowedState) {
            const auto *TargetAccess =
                dynamic_cast<const ScopeAccessExprAST *>(Node.getArgs()[0].get());
            std::string TargetState =
                TargetAccess ? TargetAccess->getVariant() : "";
            if (!MT->hasEdge(*NarrowedState, TargetState)) {
              InvalidTransitionError(Node.getLoc(), MT->getName(),
                                     *NarrowedState, TargetState)
                  .raise();
              setType(&Node, TurfVoid());
              return;
            }
            Node.TransitionCompileTimeProven = true;
          }

          Node.IsMachineTransition = true;
          setType(&Node, TurfVoid());
          return;
        }
      }
    }

    if (BaseTy) {
      if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy.get())) {
        const std::string &StructName = STy->getName();
        // Struct methods declared in the SAME file are stored in
        // CurrentFileFunctions_ as "User.grow".
        auto Candidates = RST_.lookupCurrentFileFunctions(StructName + "." + MethodName);
        // Struct methods declared in an IMPORTED module (e.g. lib/std/list.tr's
        // `fn<T> List.push(...)`, found via `import list;`) are hoisted into
        // the MODULE registry instead (Resolver::hoistModuleBody() ->
        // registerFunctionSignature() -> registerModuleFunction(), since the
        // module name is non-empty there) -- lookupCurrentFileFunctions()
        // alone can never find them, regardless of struct name/arity/
        // anything else. Without this, EVERY method call on an instance of a
        // struct declared in an imported module (not just generic ones --
        // this is unrelated to the GenericType fix above) silently failed to
        // resolve: Node.ResolvedCallTarget stayed null, and Codegen's
        // MethodCallExprAST::codegen() then silently returned nullptr with
        // no diagnostic. Confirmed via `--emit-llvm`: a whole sequence of
        // `l.push(...)`/`l.pop()`/assert() calls on a `List<int>` imported
        // from lib/std/list.tr vanished ENTIRELY from the generated
        // `main()` body -- not a crash, not a compile error, just silently
        // absent -- while an unconditional `io.printline("list ok")` after
        // them still ran, making the whole test look like it passed.
        for (const auto &Match : RST_.findInModules(StructName + "." + MethodName))
          if (Match.Symbol)
            Candidates.push_back(Match.Symbol);
        // Recover the struct's own generic-parameter substitution (e.g.
        // `l: List<int>` => T=int) BEFORE picking a candidate, not just
        // after, so it can be used to check ARGUMENT type compatibility
        // too, not only the return type. See the (moved-down, unchanged)
        // doc comment below Subst's use for the full story on why this
        // scan-the-fields recovery technique is needed at all.
        std::unordered_map<std::string, TurfTypePtr> Subst;
        if (auto *GT = dynamic_cast<const GenericType *>(UnwrappedGenericBaseTy.get())) {
          const auto &TypeArgs = GT->getTypeArgs();
          std::function<void(const TurfTypePtr &)> Scan = [&](const TurfTypePtr &FTy) {
            if (!FTy || Subst.size() >= TypeArgs.size()) return;
            if (auto *GP = dynamic_cast<const TurfGenericParamType *>(FTy.get())) {
              if (!Subst.count(GP->getName()))
                Subst.emplace(GP->getName(), TypeArgs[Subst.size()]);
            } else if (auto *NT = dynamic_cast<const NamedType *>(FTy.get())) {
              bool KnownElsewhere = GlobalSymbolTable &&
                                   GlobalSymbolTable->LookupType(NT->getName()) != nullptr;
              if (!KnownElsewhere && !Subst.count(NT->getName()))
                Subst.emplace(NT->getName(), TypeArgs[Subst.size()]);
            } else if (auto *PT = dynamic_cast<const TurfPointerType *>(FTy.get())) {
              Scan(PT->getPointee());
            } else if (isArrayType(FTy.get())) {
              Scan(dynamic_cast<const ArrayType *>(FTy.get())->getElementTypePtr());
            } else if (auto *NestedGT = dynamic_cast<const GenericType *>(FTy.get())) {
              // TYPE-002 (S15/D4): a field whose OWN declared type is
              // ANOTHER generic instantiation at the same type parameter
              // (`struct Outer<T> { inner: Inner<T> }`) was invisible to
              // this scan -- none of the branches above match a bare
              // GenericType, so `self.inner`'s own T never got recovered
              // and Inner's genuinely-substituted type argument was lost.
              // Recurse into its own type-argument list the same way a
              // pointer/array wrapper already does.
              for (const auto &NestedArg : NestedGT->getTypeArgs())
                Scan(NestedArg);
            }
          };
          for (const auto &F : STy->getFields())
            Scan(F.Type);
        }

        // Arity: struct methods have `self` as first param, user args = arity - 1 (unless static)
        int ExpectedArity = IsStaticMethod ? ArgCount : (ArgCount + 1);
        std::vector<const ResolverFunctionSymbol *> ArityMatches;
        for (const auto *C : Candidates)
          if (C->arity() == ExpectedArity) ArityMatches.push_back(C);

        // Argument type compatibility -- previously MISSING entirely for
        // struct methods (unlike the module-call path above, which already
        // filters candidates by typesCompatible()). Concretely confirmed:
        // `l.push("hello")` on a `List<int>` compiled clean and, at
        // runtime, read the string's raw pointer bits back as an `int` on
        // the next `pop()` -- a genuine type-safety hole, not just a
        // missing diagnostic, because `push(item: T)`'s declared param type
        // is the generic placeholder T (type-erased to a bare 64-bit slot
        // at the struct-method level), which trivially "matched" any
        // argument type with no check at all. Each candidate's param types
        // are substituted through Subst (T -> int, from the base's own
        // `List<int>` instantiation) before comparing, exactly mirroring
        // how the return type is substituted below.
        std::vector<const ResolverFunctionSymbol *> TypeMatches;
        for (const auto *C : ArityMatches) {
          bool AllOk = true;
          int ParamOffset = IsStaticMethod ? 0 : 1; // skip 'self'
          for (int i = 0; i < ArgCount; ++i) {
            TurfTypePtr ParamTy = C->Params[i + ParamOffset].Type;
            if (!Subst.empty())
              ParamTy = substituteReturnType(ParamTy, Subst);
            // OVH-005: see the identical exception in visitFuncCall() above.
            if (ParamTy && ArgTypes[i] && !typesCompatible(ArgTypes[i].get(), ParamTy.get()) &&
                !untypedLiteralAcquiresInt32(Node.getArgs()[i].get(), ParamTy.get())) {
              AllOk = false;
              break;
            }
          }
          if (AllOk)
            TypeMatches.push_back(C);
        }

        if (!ArityMatches.empty() && TypeMatches.empty()) {
          // Tier 1 (bound methods) is exhausted with zero matches for this
          // exact call -- right types don't exist for this arity. Try tier
          // 2 (UFCS) before reporting this as an error (RFC §5.2's tiering:
          // "this tier is exhausted in full before falling through").
          if (tryUFCSFallback(Node, BaseTy.get(), ArgTypes))
            return;

          // Method name and argument COUNT are both right, but no overload's
          // parameter types accept what was actually passed -- e.g.
          // List<int>.push("hello"). Distinct from "wrong number of
          // arguments" (WrongArityError, used below when Candidates aren't
          // even arity-compatible) and from "no method by this name"
          // (StructMethodNotFoundError).
          std::string ArgTypeStr;
          std::vector<RelatedLocation> Related;
          for (int i = 0; i < ArgCount; ++i) {
            if (i) ArgTypeStr += ", ";
            ArgTypeStr += (ArgTypes[i] ? ArgTypes[i]->getName() : "unknown");
          }
          for (const auto *Cand : ArityMatches)
            Related.push_back({toSourceLocation(Cand->DeclLoc),
                               "candidate: " + Cand->signature()});
          reportError(Node.getLoc(),
                      "No overload of '" + StructName + "." + MethodName +
                          "' matches argument types (" + ArgTypeStr + ").",
                      Related, "", "No matching overload!");
          setType(&Node, nullptr);
          return;
        }

        if (!TypeMatches.empty()) {
          const ResolverFunctionSymbol *Best = TypeMatches[0];
          Node.ResolvedCallTarget = std::make_shared<ResolverFunctionSymbol>(*Best);
          // SEM-002 -- see visitVarDecl()'s identical comment
          // (TypeCheckerDecl.cpp). Same ParamOffset (skip 'self' for an
          // instance method) as the type-compatibility filter just above.
          checkCallArgsRepresentable(Node.getArgs(), Best->Params, Node.getLoc(),
                                     IsStaticMethod ? 0 : 1);

          // Calling a method that mutates self through an immutable
          // variable was previously silently allowed -- direct field
          // assignment (`c.val = 5;`) already correctly raises
          // ImmutableAssignmentError, but `c.bump()` (where `bump()`
          // internally does `self.val += 1`) bypassed it entirely, since
          // this check simply didn't exist for method calls. See
          // ResolverFunctionSymbol::MutatesSelf's doc comment in
          // ResolverSymbolTable.h for how that's computed and why it's
          // conservative in the direction of never producing a false
          // positive here (a genuinely read-only method can never be
          // wrongly rejected).
          if (!IsStaticMethod && Best->MutatesSelf) {
            if (auto *BaseVarForImmut =
                    dynamic_cast<const VariableExprAST *>(Node.getBase())) {
              if (BaseVarForImmut->ResolvedSymbol &&
                  BaseVarForImmut->ResolvedSymbol->IsImmutable) {
#ifndef TYPECHECKER_NO_ERRORS
                ImmutableAssignmentError(Node.getLoc(), BaseVarForImmut->getName())
                    .withRelated(
                        toSourceLocation(BaseVarForImmut->ResolvedSymbol->DeclLoc),
                        "declared immutable here")
                    .raise();
#else
                throw std::runtime_error("ImmutableAssignmentError: " +
                                         BaseVarForImmut->getName());
#endif
              }
            }
          }

          // If the ORIGINAL (pre-struct-resolution) base type was a
          // GenericType (e.g. `l: List<int>`, T=int), substitute that
          // concrete type into the method's declared return type wherever
          // it mentions the struct's own type parameter -- e.g.
          // `fn<T> List.pop(): T` must report `int`, not the literal
          // TurfGenericParamType("T"), the same call-site-inference gap
          // fixed for free functions in visitFuncCall() (see
          // collectGenericSubstitution()/substituteReturnType() above), but
          // via a different substitution SOURCE: a struct method's `self`
          // parameter is declared as a bare `StructName*` with no type
          // arguments of its own (see ParseFuncDef's self-parameter
          // construction), so there is nothing to unify against in
          // Best->Params the way a free function's own parameter list would
          // give us -- the concrete type argument only exists on the BASE
          // EXPRESSION's type. Since TurfStructType doesn't record its own
          // declared type-parameter NAMES, they're recovered above (Subst)
          // by scanning the struct's fields for NamedType occurrences that
          // don't resolve to any known struct/enum (through pointer/array
          // wrapping) in declaration order and pairing each distinct name
          // found with GT's type args positionally.
          //
          // TurfGenericParamType, as of OVH-006 -- Resolver::visitStructDecl()
          // now re-registers GlobalSymbolTable's TurfStructType for this
          // struct with the same substituted field list it writes onto the
          // AST node, so the Scan below hits the `GP` branch directly for an
          // ordinary generic struct field. (Before that fix, the registered
          // type was a SEPARATE object, built once at PARSE time directly
          // from the as-parsed Fields and never updated, so its fields were
          // permanently stuck as NamedType("T") -- the `NT` branch below,
          // which survives as a defensive fallback, used to be the one that
          // actually fired; see OVH-006's change record for the live bug
          // that produced and the fix.) Positional pairing (declaration
          // order, first-occurrence-per-name) is exact for every generic
          // struct in this language's stdlib today (all single-type-
          // parameter), though not fully general for a hypothetical
          // multi-parameter struct whose fields don't happen to mention
          // every parameter in declaration order.
          // TYPE-002 (S14a): a method generic IN ITS OWN RIGHT
          // (`fn<T> Box.echo(x: T): T`) needs a SEPARATE substitution from
          // Subst above -- Subst only ever recovers the STRUCT's own
          // generic parameter(s) from the receiver's GenericType (empty
          // for a non-generic struct like Box). This unifies the METHOD's
          // OWN parameter types (already TurfGenericParamType-typed by the
          // SAME substituteTypeParams() pass every fn<T> declaration goes
          // through, struct-bound or not -- ResolverCore.cpp) against the
          // ACTUAL call arguments, exactly mirroring how visitFuncCall()
          // does this for an ordinary free function. Disclosed live in
          // S12b as a gap this session closes: `b.echo(42)` where
          // `b: Box` (not `Box<...>`) previously fell straight through to
          // the plain `setType(&Node, Best->ReturnType)` path below with
          // NO substitution at all, reporting the call's type as the
          // literal, un-substituted TurfGenericParamType("T") instead of
          // `int` -- a real type-checking gap, not just a monomorphization
          // one.
          std::unordered_map<std::string, TurfTypePtr> MethodOwnSubst;
          {
            int ParamOffset = IsStaticMethod ? 0 : 1;
            for (int i = 0; i < ArgCount; ++i)
              collectGenericSubstitution(Best->Params[i + ParamOffset].Type,
                                         ArgTypes[i], MethodOwnSubst);
          }

          std::unordered_map<std::string, TurfTypePtr> CombinedSubst = Subst;
          for (const auto &KV : MethodOwnSubst)
            CombinedSubst.insert(KV);

          if (!CombinedSubst.empty()) {
            // TYPE-002 (S11b): persist onto the node -- see
            // MethodCallExprAST::GenericSubstitution's doc comment
            // (ASTAccess.h).
            Node.GenericSubstitution = CombinedSubst;
            // Report-only: feed the (opt-in) collector, a no-op when null.
            if (MonoCollector_) {
              // UnwrappedGenericBaseTy, not a fresh getType(Node.getBase())
              // -- see that variable's own doc comment above (where BaseTy
              // is unwrapped from self's pointer type) for why a second,
              // independent re-fetch here used to silently miss a `self`-
              // based call's real GenericType too, which -- for a callee
              // whose own parameters don't separately reintroduce the
              // struct's type parameter -- meant this whole request never
              // got recorded even once CombinedSubst above was fixed to be
              // non-empty.
              if (auto *BaseGT = dynamic_cast<const GenericType *>(
                      UnwrappedGenericBaseTy.get())) {
                // Existing case (S12b/S13a): the RECEIVER's struct is
                // itself generic. Two requests, not one: the RECEIVER's
                // struct instantiation (e.g. List<int> itself must exist)
                // and the specific method being called on it (e.g.
                // List.push<int>) -- methods land before struct layout
                // does, so both are worth recording now. TypeArgs is
                // re-derived from the base expression's GenericType rather
                // than threaded out of the `if (auto *GT = ...)` scope
                // above it went out of scope in -- cheap, and keeps this
                // addition from having to restructure already-working code.
                //
                // TYPE-002 (S14a): only recorded when FULLY CONCRETE --
                // found live: a generic function's own body calling a
                // method on a `List<T>`-typed value (T the FUNCTION's own,
                // still-open type parameter) hits this same code path
                // during the TEMPLATE's own un-substituted type-check,
                // recording a bogus "List<T>"/"List.push<T>" request
                // otherwise -- see isFullyConcreteType()'s own doc comment
                // (TypeCheckerInternal.h) for the identical free-function
                // bug this mirrors.
                std::vector<TurfTypePtr> TypeArgsCopy = BaseGT->getTypeArgs();
                bool AllConcrete = true;
                for (const auto &A : TypeArgsCopy)
                  if (!isFullyConcreteType(A.get())) { AllConcrete = false; break; }
                if (AllConcrete) {
                  MonoCollector_->record({Monomorphize::InstantiationKind::Struct,
                                         BaseGT->getBaseName(), BaseGT->getModule(),
                                         TypeArgsCopy, Node.getLoc()},
                                        &Node);

                  // TYPE-002 (S15/D6): the METHOD-kind request's TypeArgs
                  // must be positioned to match Best->TypeParams -- exactly
                  // how Monomorphize.cpp's drainMethodRequests()/
                  // cloneAndSubstituteSelfBound() pairs them against
                  // Template->getTypeParams() at drain time -- not just the
                  // RECEIVER's own struct type arguments. A method that
                  // declares type parameters of its own IN ADDITION to
                  // redeclaring the struct's (`fn<T, U> Wrap.pick(other: U):
                  // U`) has MORE TypeParams than the struct alone provides;
                  // using TypeArgsCopy verbatim there silently dropped every
                  // extra one (U stayed an un-substituted
                  // TurfGenericParamType, tripping INV-GEN-1's assertion at
                  // drain time -- see the change record's D6 entry). Falls
                  // back to the struct-only TypeArgsCopy when TypeParams
                  // doesn't name anything beyond what BaseGT already
                  // provides (the common case, unchanged behaviour) or if
                  // CombinedSubst is somehow missing a name (defensive; the
                  // struct/method Subst maps built above are the ONLY
                  // sources CombinedSubst is assembled from, so every name
                  // in Best->TypeParams is expected to be present).
                  std::vector<TurfTypePtr> MethodTypeArgs = TypeArgsCopy;
                  if (Best->TypeParams.size() > TypeArgsCopy.size()) {
                    std::vector<TurfTypePtr> Extended;
                    Extended.reserve(Best->TypeParams.size());
                    bool AllFound = true;
                    for (const auto &Name : Best->TypeParams) {
                      auto It = CombinedSubst.find(Name);
                      if (It == CombinedSubst.end()) { AllFound = false; break; }
                      Extended.push_back(It->second);
                    }
                    if (AllFound)
                      MethodTypeArgs = std::move(Extended);
                  }
                  bool MethodArgsConcrete = true;
                  for (const auto &A : MethodTypeArgs)
                    if (!isFullyConcreteType(A.get())) { MethodArgsConcrete = false; break; }
                  if (MethodArgsConcrete)
                    MonoCollector_->record({Monomorphize::InstantiationKind::Method,
                                           StructName + "." + MethodName,
                                           Best->SourceModule, std::move(MethodTypeArgs),
                                           Node.getLoc()},
                                          &Node);
                }
              } else if (!MethodOwnSubst.empty()) {
                // New case (S14a): the receiver ISN'T a GenericType (Box
                // isn't generic), but the METHOD has its own type
                // parameters -- record using the METHOD's OWN ordered
                // TypeArgs (collectGenericParamNameOrder() recovers
                // declaration order the same way visitFuncCall() does for
                // an ordinary free function). No Struct-kind request: Box
                // itself needs no per-instantiation layout, only its
                // method does. Same concreteness gate as above, same reason.
                std::vector<std::string> Order;
                int ParamOffset = IsStaticMethod ? 0 : 1;
                for (int i = 0; i < ArgCount; ++i)
                  collectGenericParamNameOrder(Best->Params[i + ParamOffset].Type, Order);
                std::vector<TurfTypePtr> TypeArgs = orderedTypeArgs(Order, MethodOwnSubst);
                bool AllConcrete = true;
                for (const auto &A : TypeArgs)
                  if (!isFullyConcreteType(A.get())) { AllConcrete = false; break; }
                if (AllConcrete) {
                  MonoCollector_->record({Monomorphize::InstantiationKind::Method,
                                         StructName + "." + MethodName,
                                         Best->SourceModule, std::move(TypeArgs),
                                         Node.getLoc()},
                                        &Node);
                }
              }
            }
            setType(&Node, substituteReturnType(Best->ReturnType, CombinedSubst));
            return;
          }

          setType(&Node, Best->ReturnType);
          return;
        }

        // No overload matched -- tier 1 (bound methods) is exhausted with
        // zero matches (whether because the method name doesn't exist at
        // all, or exists but not at this arity). Try tier 2 (UFCS) before
        // reporting either error.
        if (tryUFCSFallback(Node, BaseTy.get(), ArgTypes))
          return;

        // Distinguish "this method doesn't exist on
        // this struct at all" from "it exists, but not with this many
        // arguments" -- see StructMethodNotFoundError's doc comment in
        // Errors.h for why this used to be silently permissive (both
        // TypeChecker AND Codegen), letting a call like
        // `l.nonexistentMethod(5)` vanish from the compiled program with
        // zero diagnostic anywhere.
        if (Candidates.empty()) {
          RankedSuggestions Suggestions;
          std::string Prefix = StructName + ".";
          std::vector<std::string> AllNames = RST_.getAllCurrentFileFunctionNames();
          std::vector<std::string> ModuleNames = RST_.getAllModuleFunctionNames();
          AllNames.insert(AllNames.end(), ModuleNames.begin(), ModuleNames.end());
          std::vector<std::string> BareMethodNames;
          for (const auto &N : AllNames)
            if (N.size() > Prefix.size() && N.compare(0, Prefix.size(), Prefix) == 0)
              BareMethodNames.push_back(N.substr(Prefix.size()));
          Suggestions.Local =
              SuggestionIndex::suggestVisible(MethodName, BareMethodNames);
          StructMethodNotFoundError(Node.getNameLoc(), StructName, MethodName,
                                    Suggestions)
              .raise();
        } else if (!IsStaticMethod && [&] {
          // This '.' call expected every candidate's arity to be
          // ArgCount + 1 (an implicit self, see ExpectedArity above), and
          // NONE matched -- check whether every candidate's REAL declared
          // arity instead equals the raw argument count exactly, i.e. each
          // one would have matched perfectly if self weren't being assumed
          // at all. That's not "wrong number of arguments", it's a true
          // static method (no self parameter) reached through an instance
          // with '.' -- see StaticMethodCalledOnInstanceError's doc
          // comment for why the generic WrongArityError below is actively
          // self-contradictory for this specific case.
          for (const auto *C : Candidates)
            if (C->arity() != ArgCount) return false;
          return true;
        }()) {
          StaticMethodCalledOnInstanceError(Node.getLoc(), StructName, MethodName)
              .raise();
        } else {
          // Unlike module functions (whose BaseName is the bare method
          // name, with the module qualifier added by the import), a struct
          // method's own BaseName is already "StructName.methodName" (see
          // registerFunctionSignature() -- FuncDefExprAST::getName() for
          // `fn<T> List.push(...)` IS "List.push"), so signature() already
          // includes it. Prepending StructName again here previously
          // produced "List.List.push(...)".
          std::vector<std::string> KnownSigs;
          std::vector<RelatedLocation> Related;
          for (const auto *Cand : Candidates) {
            KnownSigs.push_back(Cand->signature());
            Related.push_back({toSourceLocation(Cand->DeclLoc),
                               "candidate: " + Cand->signature()});
          }
          auto Err = WrongArityError(Node.getLoc(), StructName + "." + MethodName,
                                     ArgCount, KnownSigs);
          for (const auto &R : Related)
            Err.withRelated(R.Loc, R.Description);
          Err.raise();
        }
        setType(&Node, nullptr);
        return;
      }
      // BaseTy resolved to something real, but it's not a struct at all
      // (e.g. `x.foo()` where x: int, or a string, or a pointer to a
      // non-struct). Primitives have NO bound-method mechanism whatsoever
      // (STRING_UFCS_OPERATOR_CHAR_RFC.md §5.1) -- UFCS is the ONLY way
      // dot-call syntax can ever work on them, so this is the primary UFCS
      // entry point for primitive receivers, not just a struct-side
      // fallback. Previously fell all the way through to Codegen's
      // `MethodCallExprAST::codegen()`, which calls
      // `llvm::report_fatal_error("Internal error: method call on
      // unsupported type. This is a compiler bug.")` -- an abrupt process
      // abort (SIGABRT) reachable from ordinary, plausible user code (a
      // typo'd method call on a non-struct value), not an actual internal
      // invariant violation. Confirmed via `x.foo()` on an int, a string,
      // and an `int*`.
      std::vector<const ResolverFunctionSymbol *> NamedCandidates;
      if (tryUFCSFallback(Node, BaseTy.get(), ArgTypes, &NamedCandidates))
        return;
      if (NamedCandidates.empty()) {
        reportError(Node.getLoc(),
                    "Cannot call method '" + MethodName + "' on type '" +
                        BaseTy->getName() +
                        "' -- only struct instances have methods.",
                    {}, "", "No such method!");
      } else {
        // A function named MethodName exists and IS visible for UFCS --
        // it just doesn't accept THIS receiver (wrong type -- UFCS's
        // receiver match is exact, no coercion, see tryUFCSFallback()'s
        // doc comment -- or wrong arity). Previously this fell into the
        // exact same "only struct instances have methods" message as
        // "no function by this name exists anywhere", which reads as
        // "primitives can never use UFCS at all" -- false, and the
        // opposite of what a near-miss like this actually means.
        std::vector<RelatedLocation> Related;
        for (const auto *Cand : NamedCandidates)
          Related.push_back({toSourceLocation(Cand->DeclLoc),
                             "candidate: " + Cand->signature()});
        reportError(Node.getLoc(),
                    "No visible '" + MethodName + "' overload accepts a '" +
                        BaseTy->getName() + "' receiver via UFCS.",
                    Related,
                    "UFCS matches the receiver's type exactly -- it never "
                    "coerces or widens it (e.g. char -> int). Either change "
                    "the receiver's declared parameter type, or convert '" +
                        MethodName + "'s receiver explicitly before calling.",
                    "No matching overload!");
      }
      setType(&Node, nullptr);
      return;
    }
  }
  // BaseTy itself is null here -- the base expression already failed to
  // resolve/type-check on its own (e.g. an unknown variable), which raised
  // its own diagnostic earlier in this same visit. DiagnosticEngine::add()'s
  // one-error-per-line cascade suppression means calling reportError() again
  // here is harmless (silently dropped) if that's what happened; it's a
  // cheap safety net against ever reaching Codegen's fatal-error fallback
  // with zero diagnostic printed at all, in case some future code path
  // reaches here with a null BaseTy that ISN'T already covered elsewhere.
  reportError(Node.getLoc(),
              "Cannot determine the type of this method call's target -- "
              "the expression before '." + MethodName + "' has no known type.",
              {});
  setType(&Node, nullptr);
}


// ─── Structs & field access ───────────────────────────────────────────────────

void TypeChecker::visitFieldAccess(FieldAccessExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);

#ifndef TYPECHECKER_NO_ERRORS
  // Resolve the base type and look up the field.
  // GlobalSymbolTable is only available in the full build (not unit tests).
  TurfTypePtr BaseTy = getType(Node.getBase());

  // `unsafe { }` gate (issue #238): field/member access through a raw
  // ptr/T* base is the closest proxy this language has for a "dereference"
  // -- there's no explicit deref operator (see AddressOfExprAST's history)
  // -- so this is where that third gated "use" (declaration, cast,
  // dereference) is enforced. A `&T` base is exempt (isRawPointer()
  // already excludes it) -- auto-deref through a safe reference is always
  // allowed, the whole point of `&T`. `self` is ALSO exempt here even
  // though its synthesized type is still a raw TurfPointerType (ParserDecl.cpp's
  // implicit-self-param construction, unchanged by this issue -- `self` is
  // always non-null by construction, but retyping it to `&StructName`
  // wholesale risks disturbing method-mangled-name/dynamic_cast assumptions
  // across a huge amount of already-tested code for no behavioral gain).
  // `self` is a reserved keyword token (TOK_SELF, Lexer.h) -- a user can
  // never declare their own variable literally named "self" -- so a
  // VariableExprAST base named "self" unambiguously IS the method
  // receiver, not a name collision to worry about.
  bool IsSelfBase = false;
  if (const auto *BaseVar = dynamic_cast<const VariableExprAST *>(Node.getBase()))
    IsSelfBase = BaseVar->getName() == "self";
  if (!IsSelfBase)
    checkUnsafeGate(BaseTy.get(), Node.getLoc(), &Node);

  // STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(c)): `err.message` --
  // `err`'s type (AnyErrorType) is never a struct, so this must be checked
  // BEFORE the struct-field machinery below, which would otherwise fall
  // through to "Cannot access field on non-struct type". Deliberately
  // scoped to AnyErrorType ONLY (not TurfUserErrorType) -- `err` inside
  // `recover as err run { ... }` is the sole way a user ever gets a value
  // to inspect (errors_and_recover.md §1.4.3); reading `.message` directly
  // off a bare `ErrorType::Variant("msg")` construction, with no `err`
  // binding involved, is not part of this scope.
  if (BaseTy && BaseTy->isAnyError()) {
    if (Node.getFieldName() == "message") {
      setType(&Node, TurfString());
    } else {
      reportError(Node.getNameLoc(),
                  "An error value has no field '" + Node.getFieldName() +
                      "' -- only 'message' is readable.",
                  {}, "Use '.message' to read the string attached at "
                      "'ErrorType::Variant(\"...\")' construction.",
                  "No such field on an error value!");
      setType(&Node, TurfString());
    }
    return;
  }

  // Unwrap pointer-to-struct (e.g. self in method bodies)
  if (BaseTy) {
    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy.get()))
      BaseTy = PtrTy->getPointee();
  }

  // Resolve NamedType to TurfStructType via symbol table
  if (BaseTy) {
    if (auto *NT = dynamic_cast<const NamedType *>(BaseTy.get())) {
      if (GlobalSymbolTable)
        BaseTy = GlobalSymbolTable->LookupType(NT->getName());
    }
  }
  // Resolve GenericType (e.g. a `Box<int>`-typed variable) to its underlying
  // TurfStructType via the base name -- mirrors the NamedType resolution
  // just above, and the identical fix in visitMethodCall()'s struct-method
  // path. Without this, TypeChecker::getType() for `b.val` (b: Box<int>)
  // came back null, silently skipping any further type checking that
  // depends on it (e.g. comparing `b.val` against another value).
  //
  // Captured BEFORE BaseTy is overwritten below, same reason
  // visitMethodCall() captures its own copy early: needed afterward to
  // substitute this struct's OWN generic parameter(s) into the field's
  // declared type (T -> Entry for a List<Entry>-typed base, say) -- see
  // the Subst-building block below.
  std::vector<TurfTypePtr> GenericTypeArgs;
  if (BaseTy) {
    if (auto *GT0 = dynamic_cast<const GenericType *>(BaseTy.get()))
      GenericTypeArgs = GT0->getTypeArgs();
  }
  // TYPE-002 (S13b): a direct field access on a GenericType-typed base is
  // ALSO a struct instantiation request, not just a VarDeclExprAST's own
  // declared type (TypeCheckerDecl.cpp's visitVarDecl) or a method call's
  // receiver (visitMethodCall, just above) -- e.g. `struct Holder { items:
  // List<int> }` then `h.items.data` with NO method ever called on `items`
  // and no separate `List<int>`-typed variable declared anywhere else.
  // Without this, drainStructRequests() (S13a) never drains this specific
  // instantiation, so CodegenStruct.cpp's now-per-instantiation field
  // access (this session, below) finds nothing registered under its
  // mangled name and falls through to the "Cannot access field on
  // non-struct type" diagnostic on a program that used to compile fine.
  // Mirrors visitMethodCall()'s identical recording just above -- same
  // absorb-a-duplicate-into-the-existing-entry behaviour via
  // InstantiationCollector::record(), so this costs nothing when the SAME
  // instantiation was already requested elsewhere.
  if (MonoCollector_ && BaseTy) {
    if (auto *GT0 = dynamic_cast<const GenericType *>(BaseTy.get())) {
      // TYPE-002 (S14a): only recorded when FULLY CONCRETE -- see
      // isFullyConcreteType()'s own doc comment (TypeCheckerInternal.h) for
      // why a non-concrete "substitution" (a still-open type parameter,
      // reached while type-checking a generic template's own body before
      // it's ever cloned) must never be recorded as a real instantiation.
      bool AllConcrete = true;
      for (const auto &A : GT0->getTypeArgs())
        if (!isFullyConcreteType(A.get())) { AllConcrete = false; break; }
      if (AllConcrete)
        MonoCollector_->record({Monomorphize::InstantiationKind::Struct,
                               GT0->getBaseName(), GT0->getModule(),
                               GT0->getTypeArgs(), Node.getLoc()},
                            &Node);
    }
  }
  if (BaseTy) {
    if (auto *GT = dynamic_cast<const GenericType *>(BaseTy.get())) {
      if (GlobalSymbolTable)
        BaseTy = GlobalSymbolTable->LookupType(GT->getBaseName());
    }
  }

  if (BaseTy) {
    if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy.get())) {
      TurfTypePtr FieldTy = STy->getFieldType(Node.getFieldName());

      // No field by this name on the struct at all -- issue #241 Track D's
      // "Did You Mean?" extension to struct fields. Reuses the same
      // SuggestionIndex trie every other unknown-name diagnostic already
      // does, fed this struct's own field names as candidates. See
      // UnknownStructFieldError's own doc comment (ErrorsStruct.h) for why
      // this used to be a silent no-op here instead.
      if (!FieldTy) {
        std::vector<std::string> FieldNames;
        for (const auto &F : STy->getFields())
          FieldNames.push_back(F.Name);
        RankedSuggestions Suggestions;
        Suggestions.Local =
            SuggestionIndex::suggestVisible(Node.getFieldName(), FieldNames);
        UnknownStructFieldError(Node.getNameLoc(), STy->getName(),
                                Node.getFieldName(), Suggestions)
            .raise();
        return;
      }

      // Substitute this struct's own generic type parameter(s) into the
      // field's declared type -- e.g. `data: T*` on a List<Entry>-typed
      // base must report `Entry*`, not the bare, unsubstituted `T*`.
      // Mirrors visitMethodCall()'s identical Subst-recovery technique
      // (scan every field for a TurfGenericParamType/NamedType name --
      // see that function's own doc comment for why the NamedType branch
      // is a defensive fallback, not the common case, since OVH-006).
      // Without this, `someList.data[0].value`'s type came back as the generic
      // placeholder instead of the real field type, and something
      // depending on the real type downstream (e.g. `printline`'s
      // overload pick) silently chose the wrong one instead of erroring
      // -- confirmed live: printed a raw pointer bit pattern as a number
      // instead of the actual string. Reuses the same substituteReturnType()
      // helper visitMethodCall() already uses for return types.
      if (FieldTy && !GenericTypeArgs.empty()) {
        std::unordered_map<std::string, TurfTypePtr> Subst;
        std::function<void(const TurfTypePtr &)> Scan = [&](const TurfTypePtr &FTy) {
          if (!FTy || Subst.size() >= GenericTypeArgs.size()) return;
          if (auto *GP = dynamic_cast<const TurfGenericParamType *>(FTy.get())) {
            if (!Subst.count(GP->getName()))
              Subst.emplace(GP->getName(), GenericTypeArgs[Subst.size()]);
          } else if (auto *NT = dynamic_cast<const NamedType *>(FTy.get())) {
            bool KnownElsewhere = GlobalSymbolTable &&
                                 GlobalSymbolTable->LookupType(NT->getName()) != nullptr;
            if (!KnownElsewhere && !Subst.count(NT->getName()))
              Subst.emplace(NT->getName(), GenericTypeArgs[Subst.size()]);
          } else if (auto *PT = dynamic_cast<const TurfPointerType *>(FTy.get())) {
            Scan(PT->getPointee());
          } else if (isArrayType(FTy.get())) {
            Scan(dynamic_cast<const ArrayType *>(FTy.get())->getElementTypePtr());
          } else if (auto *NestedGT = dynamic_cast<const GenericType *>(FTy.get())) {
            // TYPE-002 (S15/D4): see visitMethodCall()'s identical fix --
            // a field declared as ANOTHER generic instantiation
            // (`struct Outer<T> { inner: Inner<T> }`) was invisible to this
            // scan, so `self.inner`'s own T never got recovered here either.
            for (const auto &NestedArg : NestedGT->getTypeArgs())
              Scan(NestedArg);
          }
        };
        for (const auto &F : STy->getFields())
          Scan(F.Type);
        if (!Subst.empty())
          FieldTy = substituteReturnType(FieldTy, Subst);
      }

      // Resolve NamedType field types so chained access (a.b.c) propagates
      if (FieldTy) {
        if (auto *NT = dynamic_cast<const NamedType *>(FieldTy.get())) {
          if (GlobalSymbolTable) {
            if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
              FieldTy = R;
          }
        }
      }
      setType(&Node, FieldTy);
      return;
    }
  }
#endif
  // Field type unknown or TypeChecker running without struct registry.
  setType(&Node, nullptr);
}


// True if every path through `E` diverges (ends in `return`/`break`/
// `continue`) -- used to enforce errors_and_recover.md's "recover run { ... }`
// must not silently fall through" rule. Deliberately a direct, non-interprocedural
// structural check (same spirit/limitations as Resolver's MutatesSelfField
// -- see its doc comment) rather than a full CFG reachability analysis:
// only these three are recognized as diverging, so a block that only
// diverges via e.g. `sys::exit(1)` or `fail(...)` (ordinary function calls,
// not something the compiler can prove never returns) is conservatively
// treated as NOT diverging. This errs toward rejecting some valid code
// rather than silently accepting invalid code, matching this codebase's
// usual posture for a hard-error gate.
//
// `break`/`continue` count alongside `return`: a genuinely common recovery
// shape is `recover run { logError(); continue; }` inside a server's accept
// loop (retry on a bad connection instead of aborting the whole loop) --
// confirmed live while converting net.tr's TcpListener::accept() to `!T`.
// Both are only legal syntax inside a loop in the first place (an earlier,
// unrelated check already rejects a stray break/continue outside one), so
// by the time this function ever sees one, it's already known to genuinely
// divert control away from the rest of this block, same as `return`.
static bool AllPathsDiverge(const ExprAST *E) {
  if (!E)
    return false;
  if (dynamic_cast<const ReturnExprAST *>(E) ||
      dynamic_cast<const BreakExprAST *>(E) ||
      dynamic_cast<const ContinueExprAST *>(E))
    return true;
  if (auto *Blk = dynamic_cast<const BlockExprAST *>(E)) {
    for (const auto &Child : Blk->getExpressions())
      if (AllPathsDiverge(Child.get()))
        return true; // a diverging statement makes everything after it
                      // unreachable, so the block as a whole diverges too
    return false;
  }
  if (auto *If = dynamic_cast<const IfExprAST *>(E)) {
    if (!If->getElseBody())
      return false; // no 'else' -- the "no branch taken" path falls through
    for (const auto &B : If->getBranches())
      if (!AllPathsDiverge(B.Body.get()))
        return false;
    return AllPathsDiverge(If->getElseBody());
  }
  return false; // while/for/anything else: conservatively assume it can fall through
}


void TypeChecker::visitRecoverExpr(RecoverExprAST &Node) {
  if (Node.getWrapped())
    const_cast<ExprAST *>(Node.getWrapped())->accept(*this);
  TurfTypePtr WrappedTy = getType(Node.getWrapped());

  if (!WrappedTy || !WrappedTy->isErrorUnion()) {
    reportError(Node.getLoc(),
               "'recover' can only be used on a fallible ('!T') expression.",
               {},
               "Only a call whose declared return type is '!T' can be "
               "followed by 'recover' -- an ordinary, non-fallible call has "
               "nothing to recover from.",
               "Invalid 'recover' use!");
    setType(&Node, nullptr);
    return;
  }
  const auto *EU = static_cast<const TurfErrorUnionType *>(WrappedTy.get());
  TurfTypePtr SuccessTy = EU->getSuccessTypePtr();
  // Codegen can't derive T from the runtime FallibleValue (every `!T` lowers
  // to the same struct regardless of T) -- stash it directly on the node,
  // same cross-pass pattern as ResolvedKind/ResolvedCandidates elsewhere.
  Node.ResolvedSuccessType = SuccessTy;

  if (Node.getKind() == RecoverKind::Produce) {
    if (Node.getProduceExpr())
      const_cast<ExprAST *>(Node.getProduceExpr())->accept(*this);
    TurfTypePtr ProduceTy = getType(Node.getProduceExpr());
    // "A recover block must evaluate to the function's success type"
    // (errors_and_recover.md) -- e.g. `writeFile(...) recover produce 42;` is
    // invalid when writeFile's success type is void, exactly like the doc's own
    // example: typesCompatible() already rejects int-vs-void generically,
    // no special-casing needed here.
    if (ProduceTy && SuccessTy && !typesCompatible(ProduceTy.get(), SuccessTy.get())) {
      reportError(Node.getLoc(),
                  "'recover produce " + ProduceTy->getName() +
                      "' doesn't match this call's success type '" +
                      SuccessTy->getName() + "'.");
    }
  } else {
    // Run / AsRun: per docs/architecture/errors_and_recover.md, a block that
    // doesn't `produce` a value must diverge on every path instead -- otherwise
    // there's no value for the recovered expression to have. EXCEPT when the
    // success type is `void`: errors_and_recover.md's own worked example shows
    // a plain `writeFile(...) recover run { io::printline("Write failed."); };`
    // (no 'return' at all) as explicitly VALID for a `!void` function -- there's
    // no value needed in that case, so simply running the block and falling
    // through is fine; only a non-void success type actually needs a value
    // to come from somewhere.
    if (Node.getBlock())
      const_cast<ExprAST *>(Node.getBlock())->accept(*this);
    if (!(SuccessTy && SuccessTy->isVoid()) && !AllPathsDiverge(Node.getBlock())) {
      reportError(Node.getLoc(),
                  "'recover " +
                      std::string(Node.getKind() == RecoverKind::AsRun
                                      ? "as " + Node.getBoundName() + " run"
                                      : "run") +
                      "' must end every path with 'return' -- it doesn't "
                      "produce a value, so falling through leaves this "
                      "expression with nothing to evaluate to.",
                  {},
                  "Either add 'return <value>;' on every path through the "
                  "block, or use 'recover produce <value>;' instead.",
                  "Non-diverging 'recover run'!");
    }
  }

  // All three forms recover to the success type T, not `!T` (confirmed).
  setType(&Node, SuccessTy);
}


void TypeChecker::visitScopeAccess(ScopeAccessExprAST &Node) {
  // A module-qualified call's callee (`io::printline` inside
  // `io::printline(x)`) reaches here too -- visitFuncCall() visits every
  // callee expression to give it SOME type annotation, but a module-
  // function reference has no meaningful standalone type of its own; the
  // CALL's type comes from ResolvedCandidates/ResolvedCallTarget instead
  // (computed by visitFuncCall itself, independent of the callee node).
  // Only the enum-variant meaning (Node.ResolvedKind set by the Resolver)
  // has a real type to assign here.
  if (Node.ResolvedKind == ScopeAccessKind::ModuleConstantRef) {
    if (Node.ResolvedConstant)
      setType(&Node, Node.ResolvedConstant->Type);
    return;
  }

  // `ErrorType::Variant` (docs/architecture/errors_and_recover.md) -- covers
  // both the bare, payload-less reference (this node visited directly as an
  // expression) and this node as the callee of a FuncCallExprAST construction
  // (the CALL's own type comes from ResolvedCandidates/ResolvedCallTarget
  // instead, same as any other call -- this just gives the callee itself
  // SOME type too, harmlessly, exactly like the comment above already
  // describes for ModuleFunctionRef/StaticMethodRef).
  if (Node.ResolvedKind == ScopeAccessKind::ErrorVariantRef) {
    if (GlobalSymbolTable) {
      if (auto ET = GlobalSymbolTable->LookupType(Node.getLeftName()))
        setType(&Node, ET);
    }
    return;
  }

  if (Node.ResolvedKind != ScopeAccessKind::EnumVariant)
    return;

  auto ET = GlobalSymbolTable ? GlobalSymbolTable->LookupType(Node.getLeftName()) : nullptr;
  if (!ET) return; // Should not happen, Resolver catches this

  if (Node.getRightName() == "COUNT") {
    setType(&Node, TurfInt());
  } else if (Node.getRightName() == "VALUES") {
    setType(&Node, std::make_shared<TurfPointerType>(ET));
  } else if (Node.getRightName() == "NAMES") {
    setType(&Node, std::make_shared<TurfPointerType>(TurfString()));
  } else {
    setType(&Node, ET);
  }
}


void TypeChecker::visitArrayAccess(ArrayAccessExprAST &Node) {
  if (Node.getBase())
    const_cast<ExprAST *>(Node.getBase())->accept(*this);
  if (Node.getIndex())
    const_cast<ExprAST *>(Node.getIndex())->accept(*this);

  // Result is the element type of the base array
  TurfTypePtr BaseTy = getType(Node.getBase());
  TurfTypePtr IndexTy = getType(Node.getIndex());

  // `unsafe { }` gate (OVH-003): indexing into a raw ptr/T* base is a
  // "dereference" exactly like visitFieldAccess()'s `p.field` gate below it
  // -- there was no equivalent check here at all before this fix (2.7's own
  // "Known gaps" list named it explicitly), so `someList.data[i] = v;`
  // compiled with no `unsafe { }` and no capability, silently reaching an
  // out-of-bounds write from ordinary safe code the moment `i` was bad.
  // Mirrors visitFieldAccess()'s self-exemption verbatim: `self` is always
  // non-null by construction and a reserved token, so a VariableExprAST
  // base literally named "self" unambiguously is the method receiver, not
  // a name collision to worry about. A struct/string/array base never
  // `isRawPointer()`, so this is a no-op for every non-pointer case above
  // and below (operator[] overloads, T[N] arrays, string indexing).
  bool IsSelfBase = false;
  if (const auto *BaseVar = dynamic_cast<const VariableExprAST *>(Node.getBase()))
    IsSelfBase = BaseVar->getName() == "self";
  if (!IsSelfBase)
    checkUnsafeGate(BaseTy.get(), Node.getLoc(), &Node);

  // ── operator[] overload resolution (LANG-001) ────────────────────────────
  // Checked BEFORE the builtin array/string/pointer dispatch below, mirrors
  // visitBinary()'s identical ordering rationale: a struct base can never
  // ALSO be a real array/pointer/string, so there is no ambiguity to
  // resolve between the two paths -- exactly one of them can ever apply to
  // a given BaseTy. Exact-type-match only (F-OP-1).
  if (!Node.ResolvedIndexGetCandidates.empty() && BaseTy && IndexTy) {
    for (const auto &C : Node.ResolvedIndexGetCandidates) {
      if (!C || C->arity() != 2)
        continue;
      if (operatorSelfOperandMatches(C->Params[0].Type.get(), BaseTy.get()) &&
          sameTypeExact(C->Params[1].Type.get(), IndexTy.get())) {
        if (Node.ResolvedIndexGetTarget) {
          reportError(Node.getLoc(),
                      "Ambiguous 'operator[](" + BaseTy->getName() + ", " +
                          IndexTy->getName() +
                          ")' call: more than one visible overload matches "
                          "at this call site.");
          break;
        }
        Node.ResolvedIndexGetTarget = C;
      }
    }
    if (Node.ResolvedIndexGetTarget) {
      setType(&Node, Node.ResolvedIndexGetTarget->ReturnType);
      return;
    }
  }

  if (BaseTy && BaseTy->isArray()) {
    const auto *ArrTy = dynamic_cast<const ArrayType *>(BaseTy.get());
    TurfTypePtr ElemTy = ArrTy ? ArrTy->getElementTypePtr() : nullptr;

#ifndef TYPECHECKER_NO_ERRORS
    // Issue #17 fix: if the element type is a NamedType (e.g. "User"), resolve
    // it to the actual TurfStructType via the symbol table. Without this,
    // arr[0].field fails because the type never becomes a struct.
    // GlobalSymbolTable is only available in the full build (not unit tests).
    if (ElemTy) {
      if (const auto *NT = dynamic_cast<const NamedType *>(ElemTy.get())) {
        if (GlobalSymbolTable) {
          if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
            ElemTy = Resolved;
        }
      }
    }
#endif

    setType(&Node, ElemTy);
  } else if (BaseTy && BaseTy->isString()) {
    setType(&Node, TurfInt());
  } else if (BaseTy) {
    if (const auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy.get())) {
      // Pointer-style indexing (`p[i]` on a `T*`, as opposed to a fixed-
      // size Turf array `T[N]`) -- the exact idiom list.tr/stack.tr/
      // queue.tr all use internally for their own `data[i]` field, and any
      // caller reading a generic collection's public `data` field directly
      // (`someList.data[0]`) hits it too. This branch never existed at
      // all before -- `p[i]` always fell through to the final `else`
      // below and got type `nullptr`, so anything depending on its type
      // (an overload pick like `printline(p[i])`, a further `.field`
      // access) silently got the wrong answer instead of a diagnostic.
      // Confirmed live: `printline(someList.data[0].value)` (value: string)
      // printed the raw pointer bit pattern as a number -- the `int`
      // print() overload got picked because `.value`'s type came back
      // null here, not because anything about the VALUE itself was wrong
      // (Codegen's independent getExprType() already resolved it
      // correctly; this is a completely separate type-inference pass that
      // never consults Codegen's results, same relationship as every
      // other getExprType()/TypeChecker parity gap this session found).
      TurfTypePtr ElemTy = PtrTy->getPointee();
      if (ElemTy) {
        if (const auto *NT = dynamic_cast<const NamedType *>(ElemTy.get())) {
          if (GlobalSymbolTable) {
            if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
              ElemTy = Resolved;
          }
        }
      }
      setType(&Node, ElemTy);
    } else {
      setType(&Node, nullptr);
    }
  } else {
    setType(&Node, nullptr);
  }
}
