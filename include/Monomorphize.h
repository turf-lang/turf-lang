/**
 * @file Monomorphize.h
 * @brief TYPE-002 monomorphizer.
 *
 * S11b (docs/development/changes/TYPE-002.md) stood up the skeleton: a
 * request/cache/worklist shape that RECORDS every generic instantiation the
 * type-checked AST implies, wired report-only behind --dump-instantiations.
 *
 * S12a makes free-function instantiation real: draining the worklist now
 * clones the template declaration, substitutes concrete types throughout
 * (signature AND body), re-resolves and re-typechecks the clone, rewrites
 * every requesting call site to point at it, and appends it to the AST list
 * main.cpp's pre-pass will codegen normally. See drainFreeFunctionRequests()
 * below and its doc comment for the full algorithm and every risk it
 * addresses. Method and struct requests are still only recorded, not
 * drained -- S12b and S13a.
 *
 * Pipeline position (§9.5 of ARCHITECTURE_OVERHAUL.md, confirmed against
 * source in S11a -- see TYPE-002.md's U-4' answer): this collector is fed
 * DURING the pre-pass's own TypeChecker run (sub-pass 2b), from inside
 * TypeChecker::visitFuncCall/visitMethodCall/visitVarDecl, at the exact
 * points those already compute a generic substitution map today. It is
 * never fed from the main pass -- U-4' proved the pre-pass sees every
 * instantiation request in the program, since a bare top-level statement
 * outside any function is a hard compile error (main.cpp:1145-1164).
 * Draining runs immediately afterward, as sub-pass 2b.5.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Apache License.
 */

#ifndef MONOMORPHIZE_H
#define MONOMORPHIZE_H

#include "Lexer.h" // SourceLocation
#include "Types.h"

#include <deque>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

class ExprAST;
class Resolver;

namespace Monomorphize {

/// What kind of declaration an instantiation request names. A free
/// function and a struct METHOD are tracked separately (not lumped into
/// one "function" kind) because S12b's method-call handling recovers its
/// substitution map from a fundamentally different source than S12a's
/// free-function handling does (the base expression's own GenericType,
/// not argument/parameter unification -- see TypeCheckerCall.cpp's
/// visitMethodCall, and TYPE-002.md's *Removed assumptions*).
enum class InstantiationKind {
  FreeFunction,
  Method,
  Struct,
};

/// One concrete instantiation request: "compile FreeFunction/Method/Struct
/// `DeclKey` with these exact type arguments." `DeclKey` is the bare
/// (never module-qualified) name candidate lookup already uses (e.g.
/// "identity", "List.push", "List") -- NOT a mangled name; `SourceModule`
/// disambiguates two different modules declaring the same bare name
/// (S11b's DeclKey alone couldn't -- corrected in S12a; see the change
/// record's *Removed assumptions*). Mangling only happens once a request
/// is actually drained (S12a+, for FreeFunction).
struct InstantiationRequest {
  InstantiationKind Kind;
  std::string DeclKey;
  std::string SourceModule; ///< "" = current file, matches ResolverFunctionSymbol::SourceModule / GenericType::getModule().
  std::vector<TurfTypePtr> TypeArgs;
  SourceLocation Site; ///< where this request was FIRST discovered (for
                       ///< reporting; the full site LIST lives on the
                       ///< collector's entry, see below).

  /// Structural equality, deliberately never by getName() string
  /// (INV-TYPE-1) -- two requests are the same instantiation only if every
  /// type argument is pairwise isEquivalent(), not merely same-spelled.
  bool sameInstantiationAs(const InstantiationRequest &Other) const {
    if (Kind != Other.Kind || DeclKey != Other.DeclKey ||
        SourceModule != Other.SourceModule)
      return false;
    if (TypeArgs.size() != Other.TypeArgs.size())
      return false;
    for (size_t i = 0; i < TypeArgs.size(); ++i) {
      if (!TypeArgs[i] || !Other.TypeArgs[i])
        return false;
      if (!TypeArgs[i]->isEquivalent(Other.TypeArgs[i].get()))
        return false;
    }
    return true;
  }
};

/// Total instantiations this compile will process before giving up and
/// reporting GenericInstantiationDepthError (E079). §9.5 step 4 frames
/// this as a CHAIN-depth cap (like a C++ template recursion limit); S12a's
/// actual implementation caps total distinct instantiations drained
/// instead of the true parent-chain length, since the worklist doesn't
/// track per-request parents. A total-count cap catches the same failure
/// mode (a generic that requests a new, ever-growing instantiation of
/// itself, forever) -- U-2's census (~18-20 real instantiations in the
/// whole corpus) means this never fires for a legitimate program either
/// way. Documented as a simplification, not silently passed off as the
/// literal chain-depth measurement §9.5 describes.
inline constexpr int kMaxInstantiationDepth = 64;

/// One already-seen (or in-flight) instantiation request, plus every call
/// site that asked for it -- S12a needs the full site list, not just a
/// seen/unseen bit, because more than one call site can request the exact
/// same instantiation (e.g. `identity(1); identity(2);`), and every one of
/// them must be rewritten to the same clone once it exists.
struct InstantiationEntry {
  InstantiationRequest Request;
  std::vector<ExprAST *> Sites;
  /// TYPE-002 (S12b): set once a drain function has actually processed
  /// this entry (cloned/substituted/registered it, or deliberately left it
  /// for a LATER drain function to own -- see Monomorphize.cpp's
  /// per-Kind drain functions for what "processed" means for each kind).
  /// See InstantiationCollector's own doc comment for why this replaced a
  /// destructive worklist queue.
  bool Drained = false;
};

/// Deliberately linear-scan dedup, not a hash map: S11a's U-2 census found
/// ~18-20 concrete instantiations in the entire corpus today, so an
/// O(n^2) worst case over `insert` calls is not a real cost, and a linear
/// scan lets every comparison go through `sameInstantiationAs()`
/// (pairwise isEquivalent()) with no risk of a hand-rolled hash function
/// silently falling back to identity-by-getName() (INV-TYPE-1) the way a
/// naive `std::hash` specialization over TurfTypePtr would invite. Revisit
/// if S11b/S12a's real-corpus numbers ever look different from S11a's
/// census.
///
/// `std::deque`, not `std::vector` (S12a correction -- see the change
/// record's *Removed assumptions*): drainFreeFunctionRequests() holds an
/// `InstantiationEntry&` across a call that can itself insert MORE entries
/// (re-typechecking one clone's body discovers a nested generic call,
/// which records a new request on this SAME cache) -- a `vector` would
/// reallocate and silently dangle that reference the moment a nested
/// discovery grew it past capacity. `deque` never invalidates a reference
/// to an existing element on `push_back`, only iterators, which nothing
/// here holds across an insert.
class InstantiationCache {
  std::deque<InstantiationEntry> Entries_;

public:
  /// Returns the existing entry for an equivalent request, or nullptr.
  InstantiationEntry *find(const InstantiationRequest &Req) {
    for (auto &E : Entries_)
      if (E.Request.sameInstantiationAs(Req))
        return &E;
    return nullptr;
  }
  InstantiationEntry &insert(InstantiationRequest Req) {
    Entries_.push_back({std::move(Req), {}});
    return Entries_.back();
  }
  size_t size() const { return Entries_.size(); }
  const std::deque<InstantiationEntry> &all() const { return Entries_; }
  InstantiationEntry &at(size_t Idx) { return Entries_[Idx]; }
};

/// Collector: TypeChecker feeds it via record() during sub-pass 2b (report-
/// only there -- the actual clone/resolve/typecheck work never happens
/// from inside TypeChecker's own traversal, only from a drain function
/// afterward, sub-pass 2b.5).
///
/// TYPE-002 (S12b correction -- see the change record's *Removed
/// assumptions*): S11b/S12a used a destructive FIFO `Worklist_` (pop an
/// index, act on it or drop it forever). That broke the moment a SECOND
/// drain function existed (S12b's `drainMethodRequests()`, alongside
/// S12a's `drainFreeFunctionRequests()`): each Kind a function doesn't
/// handle gets `continue`-d past, but `popWorklist()` already REMOVED it
/// from the queue -- so whichever drain function runs first silently
/// discards every OTHER kind's entries before the function actually
/// responsible for them ever gets a chance to look at the queue. Replaced
/// with a non-destructive design: entries live in `Cache_` (append-only,
/// see InstantiationCache) forever, each with its own `Drained` flag; a
/// drain function does its own full index sweep via `cache().size()`
/// (re-read each iteration, so it naturally picks up entries a NESTED
/// discovery appends mid-sweep) and sets `Drained` on exactly the entries
/// it acted on. A NESTED discovery of a DIFFERENT kind (a method calling a
/// generic free function, or vice versa) is what `main.cpp`'s alternating
/// drain loop is for -- see its own comment for why one function's own
/// internal fixed point isn't the whole story.
class InstantiationCollector {
  InstantiationCache Cache_;

public:
  /// Record one instantiation request from one call/decl site. A
  /// duplicate (same sameInstantiationAs()) is absorbed into the existing
  /// entry's Sites list -- this IS §9.5 step 2's "rewrite the requesting
  /// site to point at the cached instantiation" (the rewrite itself
  /// happens once, in a drain function, once the cached instantiation
  /// exists).
  void record(InstantiationRequest Req, ExprAST *Site) {
    if (auto *Existing = Cache_.find(Req)) {
      if (Site) Existing->Sites.push_back(Site);
      return;
    }
    auto &NewEntry = Cache_.insert(std::move(Req));
    if (Site) NewEntry.Sites.push_back(Site);
  }

  /// The returned reference stays valid across further record() calls
  /// (Cache_ is a deque -- see its own doc comment) -- safe to hold across
  /// the re-resolve/re-typecheck of the entry this index names, even
  /// though that can itself call record() and grow Cache_.
  InstantiationEntry &entryAt(size_t Idx) { return Cache_.at(Idx); }

  const InstantiationCache &cache() const { return Cache_; }

  /// Human-readable dump for `--dump-instantiations`.
  void report(std::ostream &OS) const;
};

/// TYPE-002 (S12a): rewrite @p Ty, replacing any TurfGenericParamType OR
/// bare NamedType occurrence found in @p Subst (recursively through
/// pointer/reference/array/generic-argument wrapping) with its concrete
/// substitution. Relocated from TypeCheckerCall.cpp's file-local
/// substituteReturnType() (S11a wrongly named ResolverCore.cpp's
/// substituteTypeParams() -- a different-direction transform -- as the
/// thing to generalize; substituteReturnType() already WAS the concrete-
/// map walk needed here, complete with the TurfReferenceType-before-
/// TurfPointerType ordering fix -- see the change record's *Removed
/// assumptions*). TypeCheckerCall.cpp's own substituteReturnType() now
/// delegates to this so its existing behaviour is unchanged, verbatim.
TurfTypePtr substituteConcreteTypes(
    const TurfTypePtr &Ty,
    const std::unordered_map<std::string, TurfTypePtr> &Subst);

/// TYPE-002 (S12a): the clone's new declared name, e.g.
/// "identity" + [int] -> "identity_gen1_int". Used as BOTH the clone's
/// FuncDefExprAST::Name and its ResolverFunctionSymbol::BaseName, so the
/// EXISTING, unmodified mangledName() naturally produces a collision-free
/// LLVM symbol from it -- no separate mangling entry point needed at
/// Codegen call sites. (S11b's mangledNameForInstantiation() is retired;
/// see the change record.)
std::string instantiatedDeclName(const std::string &BaseName,
                                 const std::vector<TurfTypePtr> &TypeArgs);

/// TYPE-002 (S12a): drain every un-drained FreeFunction-kind entry in
/// @p Collector to a fixed point (re-sweeping on growth -- see
/// InstantiationCollector's own doc comment), cloning, substituting,
/// re-resolving, re-typechecking, and registering each one, appending the
/// resulting concrete FuncDefExprAST clones onto @p PrePassASTs so
/// main.cpp's ordinary sub-pass 2c/3a/3b codegen loops pick them up with no
/// Codegen changes at all. Rewrites every requesting call site's
/// ResolvedCallTarget to the clone. A struct-bound declaration (Params[0]
/// is "self") is excluded even though it arrives as a FreeFunction-kind
/// request (`Box::new(...)`'s "self-stripped candidate") -- see the .cpp
/// doc comment. Method/Struct-kind entries are left un-drained (S12b's
/// drainMethodRequests()/S13a act on them). @p PreR is the SAME Resolver
/// instance that already ran resolveAll() over the whole program -- reused
/// so the clone resolves against the real, already-hoisted program symbol
/// table. Returns true if it drained at least one entry (main.cpp's
/// alternating drain loop uses this to detect a fixed point across BOTH
/// this function and drainMethodRequests()).
bool drainFreeFunctionRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR);

/// TYPE-002 (S12b): the Method-kind counterpart to
/// drainFreeFunctionRequests() -- a generic method's OWN Params (except
/// `self`, index 0) and ReturnType are substituted to concrete types the
/// same way; `self`'s OWN declared type is ALSO rewritten, from
/// `StructName*` to `StructName<TypeArgs>*`, so TypeChecker's ALREADY-
/// EXISTING field-substitution mechanism (visitFieldAccess's
/// GenericTypeArgs scan, TypeCheckerCall.cpp) correctly resolves
/// `self.<field>`'s type for this specific instantiation during
/// re-typecheck -- see the .cpp doc comment for why this is safe even
/// though struct LAYOUT stays fully erased/shared this session (S13a's
/// job): `self`'s LLVM representation is completely unchanged either way
/// (a GenericType-typed struct pointer erases identically to a bare one),
/// and CastToType's existing erasure/un-erasure branches are driven by the
/// VALUE's actual LLVM shape and the DESTINATION type, never by whether
/// the AST source-side annotation says "generic" -- confirmed by reading,
/// not assumed; see the change record. Returns true if it drained at
/// least one entry.
bool drainMethodRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR);

/// TYPE-002 (S14a): the constructor counterpart of drainMethodRequests() --
/// `fn<T> StructName.new(...)` called as `StructName::new(...)`. Still a
/// FreeFunction-kind request (TypeCheckerCall.cpp's visitFuncCall records
/// it that way, unchanged since S11b/S12a), so this shares
/// drainFreeFunctionRequests()'s registry/Sites shape (FuncCallExprAST),
/// not drainMethodRequests()'s (MethodCallExprAST) -- only the CLONE-
/// BUILDING step (self-type-rewrite) is shared with methods. S12a
/// deliberately excluded every struct-bound FreeFunction-kind entry from
/// drainFreeFunctionRequests() because no per-instantiation struct layout
/// existed yet for a substituted `self.<field>` reference to resolve
/// against; S13a built that layout, and this function is what actually
/// drains the requests S12a left recorded but permanently untouched. See
/// the .cpp doc comment for the full mechanism and why it's safe. Returns
/// true if it drained at least one entry.
bool drainConstructorRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR);

/// TYPE-002 (S13a): the Struct-kind counterpart to
/// drainFreeFunctionRequests()/drainMethodRequests() -- gives each generic
/// struct instantiation (e.g. `List<int>`, `List<string>`) its own real
/// `TurfStructType` (registered in GlobalSymbolTable under its mangled
/// instantiation name) and, once the clone is appended to @p PrePassASTs,
/// its own real, distinct `llvm::StructType` (main.cpp's ordinary sub-pass
/// 2c struct-codegen loop already codegens every StructDeclExprAST it finds
/// unconditionally -- no template-skip guard exists or is needed for
/// structs, unlike functions -- see this function's .cpp doc comment).
///
/// Deliberately does NOT rewrite anything at the requesting call sites
/// (Entry.Sites): unlike a FreeFunction/Method instantiation, nothing reads
/// a "which struct type did this site resolve to" field yet -- Codegen's
/// two existing GenericType->base-name collapse points
/// (FieldAccessExprAST::codegenLValue()/::codegen(), CodegenStruct.cpp) and
/// getLLVMType()'s own GenericType branch (CodegenCore.cpp) are ALL left
/// exactly as they are this session, on purpose: this is S13b's job. The
/// per-instantiation type this function creates EXISTS and is independently
/// verifiable (its own registration, its own acyclicity check, its own LLVM
/// layout) but is not yet CONSUMED by anything -- purely additive, zero risk
/// of changing any existing program's compiled behaviour, matching this
/// session's "same program, same output" mandate (TYPE-002.md's own "Is
/// this breaking?" section). Returns true if it drained at least one entry.
bool drainStructRequests(
    std::vector<std::unique_ptr<ExprAST>> &PrePassASTs,
    InstantiationCollector &Collector, Resolver &PreR);

} // namespace Monomorphize

#endif // MONOMORPHIZE_H
