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

#include <algorithm>
#include <deque>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

class ExprAST;
class FuncDefExprAST;
class Resolver;
struct ResolverFunctionSymbol;

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

  /// LEC-006: the exact template declaration, when the requester knows it
  /// (ResolverFunctionSymbol::Decl -- see its doc comment for why a name is
  /// not enough: overloads share one). Two requests for different overloads
  /// are different instantiations even at identical type arguments, and the
  /// drain instantiates THIS declaration instead of whichever overload the
  /// name registry happens to hold. Null falls back to the name registry,
  /// the behaviour of every request kind that does not set it.
  const FuncDefExprAST *Decl = nullptr;

  /// LEC-005 (D25): the cache index of the instantiation whose BODY was being
  /// re-typechecked when this request was discovered, or -1 when it came from
  /// ordinary (non-instantiated) code. Following it to -1 reconstructs the
  /// real chain -- user call -> `Set<Big>.add` -> `Set<Big>.contains` -> ... --
  /// that a diagnostic about the innermost request needs and that the cache
  /// itself (one flat dump of everything ever requested) cannot give. Only the
  /// FIRST discoverer is kept (see InstantiationCollector::record()); not part
  /// of sameInstantiationAs(), since who asked never changes what is built.
  int Parent = -1;

  /// Structural equality, deliberately never by getName() string
  /// (INV-TYPE-1) -- two requests are the same instantiation only if every
  /// type argument is pairwise isEquivalent(), not merely same-spelled.
  bool sameInstantiationAs(const InstantiationRequest &Other) const {
    if (Kind != Other.Kind || DeclKey != Other.DeclKey ||
        SourceModule != Other.SourceModule || Decl != Other.Decl)
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

/// The longest chain of instantiations requesting each other
/// (InstantiationRequest::Parent) this compile will follow before reporting
/// GenericInstantiationDepthError (E079) -- a C++-template-recursion-limit
/// analogue, and what §9.5 step 4 always said it was.
///
/// History: S12a could not measure this (requests had no parent), so it capped
/// the TOTAL number of instantiations drained at 64 instead and documented
/// that as a simplification. That was wrong in both directions: a perfectly
/// ordinary program with 70 independent `ident<S1> .. ident<S70>` calls was
/// rejected with E079, and the "chain" it printed was a dump of the whole
/// cache. LEC-005 gives requests parents, so the cap is now the real thing.
inline constexpr int kMaxInstantiationDepth = 64;

/// A runaway that FANS OUT (`f<T>` requests both `f<Box<T>>` and
/// `f<Pair<T>>`) reaches kMaxInstantiationDepth only after enumerating
/// breadth-first every instantiation above it -- exponentially many -- so the
/// depth cap alone would be an effective hang. This bounds the TOTAL, set far
/// above anything a real program instantiates (the whole stdlib plus a large
/// application is hundreds), purely as the backstop that keeps a pathological
/// generic a diagnostic instead of an out-of-memory.
inline constexpr size_t kMaxTotalInstantiations = 4096;

/// One already-seen (or in-flight) instantiation request, plus every call
/// site that asked for it -- S12a needs the full site list, not just a
/// seen/unseen bit, because more than one call site can request the exact
/// same instantiation (e.g. `identity(1); identity(2);`), and every one of
/// them must be rewritten to the same clone once it exists.
struct InstantiationEntry {
  InstantiationRequest Request;
  /// This entry's own position in the cache -- what a child request stores as
  /// its `Parent`. Set by InstantiationCache::insert().
  size_t Index = 0;
  std::vector<ExprAST *> Sites;
  /// TYPE-002 (S12b): set once a drain function has actually processed
  /// this entry (cloned/substituted/registered it, or deliberately left it
  /// for a LATER drain function to own -- see Monomorphize.cpp's
  /// per-Kind drain functions for what "processed" means for each kind).
  /// See InstantiationCollector's own doc comment for why this replaced a
  /// destructive worklist queue.
  bool Drained = false;

  /// TYPE-002 (S15/D1): the clone's registered symbol, once Drained is set
  /// for a FreeFunction/Method-kind entry (null for Struct-kind, which
  /// never rewrites a call site at all -- see drainStructRequests()). Kept
  /// on the entry, not just handed to the immediate caller, so a call site
  /// discovered AFTER this entry was drained (record()'s existing-entry
  /// branch keeps appending to Sites forever, with no Drained check of its
  /// own) can still be rewritten later instead of silently left pointing at
  /// the un-codegenned template. See SitesRewritten below and the change
  /// record's D1 entry for why Drained alone used to mean BOTH "don't clone
  /// again" and "don't rewrite new sites again" -- two different questions
  /// that need two different answers once instantiation order isn't
  /// guaranteed to discover every call site before the entry drains.
  std::shared_ptr<ResolverFunctionSymbol> ClonedSymbol;

  /// TYPE-002 (S15/D1): how many of Sites have already had their
  /// ResolvedCallTarget rewritten to ClonedSymbol. A drain function that
  /// finds Drained already true for an entry of the Kind it owns rewrites
  /// only Sites[SitesRewritten..) instead of skipping the entry outright.
  size_t SitesRewritten = 0;
};

/// Dedup is a hash lookup followed by `sameInstantiationAs()` on the bucket's
/// few candidates -- never by the hash alone. The hash is built from each type
/// argument's getName(), and that is sound only because every isEquivalent()
/// that exists (the default, ArrayType, FuncType) implies equal getName(): a
/// hash may be coarser than equality but must never separate two equivalent
/// requests, so a NEW isEquivalent() override that is more lenient than the
/// spelling must also change instHash() below. Equality itself still goes
/// through `sameInstantiationAs()` (pairwise isEquivalent(), INV-TYPE-1).
///
/// This was a deliberate linear scan while S11a's census found ~20 concrete
/// instantiations in the whole corpus. LEC-005's fan-out backstop
/// (kMaxTotalInstantiations = 4096) made the worst case real: scanning all
/// entries for every request, each comparison rebuilding two deeply nested
/// type names, took ~20 s to reach the cap (tests/38_monomorphization/
/// fail_generic_fanout_instantiation_limit.tr, measured by sampling: all of
/// it in find()).
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
  /// instHash() -> index into Entries_. A multimap: unequal requests may share
  /// a hash, and find() checks each candidate with sameInstantiationAs().
  std::unordered_multimap<size_t, size_t> ByHash_;

  static size_t instHash(const InstantiationRequest &R) {
    size_t H = std::hash<std::string>()(R.DeclKey);
    auto Mix = [&H](size_t V) { H ^= V + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2); };
    Mix(static_cast<size_t>(R.Kind));
    Mix(std::hash<std::string>()(R.SourceModule));
    Mix(std::hash<const void *>()(R.Decl));
    Mix(R.TypeArgs.size());
    for (const auto &T : R.TypeArgs)
      Mix(T ? std::hash<std::string>()(T->getName()) : 0);
    return H;
  }

public:
  /// Returns the existing entry for an equivalent request, or nullptr.
  InstantiationEntry *find(const InstantiationRequest &Req) {
    auto Range = ByHash_.equal_range(instHash(Req));
    for (auto It = Range.first; It != Range.second; ++It) {
      auto &E = Entries_[It->second];
      if (E.Request.sameInstantiationAs(Req))
        return &E;
    }
    return nullptr;
  }
  InstantiationEntry &insert(InstantiationRequest Req) {
    InstantiationEntry NewEntry;
    ByHash_.emplace(instHash(Req), Entries_.size());
    NewEntry.Request = std::move(Req);
    NewEntry.Index = Entries_.size();
    Entries_.push_back(std::move(NewEntry));
    return Entries_.back();
  }
  size_t size() const { return Entries_.size(); }
  const std::deque<InstantiationEntry> &all() const { return Entries_; }
  InstantiationEntry &at(size_t Idx) { return Entries_[Idx]; }
  const InstantiationEntry &at(size_t Idx) const { return Entries_[Idx]; }
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
  /// Cache index of the entry whose clone is being re-typechecked right now,
  /// or -1. See ParentScope.
  int CurrentParent_ = -1;

public:
  /// Record one instantiation request from one call/decl site. A
  /// duplicate (same sameInstantiationAs()) is absorbed into the existing
  /// entry's Sites list -- this IS §9.5 step 2's "rewrite the requesting
  /// site to point at the cached instantiation" (the rewrite itself
  /// happens once, in a drain function, once the cached instantiation
  /// exists).
  ///
  /// LEC-005 (D25): a NEW entry is stamped with the instantiation currently
  /// being re-typechecked (setCurrentParent()), or -1 outside one. A repeat
  /// request keeps its first parent, with one correction: if ordinary code
  /// asks for something an instantiation had asked for first, the entry is
  /// re-rooted -- the shortest chain, and the one that ends at a call the user
  /// actually wrote, is the useful one to report.
  void record(InstantiationRequest Req, ExprAST *Site) {
    if (auto *Existing = Cache_.find(Req)) {
      if (Site) Existing->Sites.push_back(Site);
      if (CurrentParent_ < 0 && Existing->Request.Parent >= 0) {
        Existing->Request.Parent = -1;
        Existing->Request.Site = Req.Site;
      }
      return;
    }
    Req.Parent = CurrentParent_;
    auto &NewEntry = Cache_.insert(std::move(Req));
    if (Site) NewEntry.Sites.push_back(Site);
  }

  /// From now on every NEW request recorded is a child of @p Parent (the
  /// cache index of the entry whose clone is being re-typechecked), or a root
  /// for -1. Returns the previous value so the caller can put it back.
  ///
  /// A set/restore pair, written when errors were reported by `longjmp`
  /// (which skipped destructors, so a guard would never have restored); the
  /// caller restores after its catch. The nested TypeChecker shares this
  /// collector, which is how "the current parent" reaches it.
  int setCurrentParent(int Parent) {
    const int Previous = CurrentParent_;
    CurrentParent_ = Parent;
    return Previous;
  }

  /// The chain of requests ending at @p Entry, outermost first (`[0]` is the
  /// one ordinary code asked for; `.back()` is @p Entry itself).
  std::vector<const InstantiationEntry *>
  chainTo(const InstantiationEntry &Entry) const {
    std::vector<const InstantiationEntry *> Chain{&Entry};
    // A parent is always an earlier entry (it exists before its children), so
    // this strictly descends in index and cannot loop.
    for (int P = Entry.Request.Parent; P >= 0;
         P = Cache_.at(static_cast<size_t>(P)).Request.Parent)
      Chain.push_back(&Cache_.at(static_cast<size_t>(P)));
    std::reverse(Chain.begin(), Chain.end());
    return Chain;
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

/// How a request reads in a diagnostic: `Set<Big>.add` for a method,
/// `identity<int>` for a free function, `Set<Big>` for a struct -- the form a
/// user would recognise from their own source, not a mangled or module-
/// qualified internal name.
std::string describeRequest(const InstantiationRequest &Req);

/// The chain @p Chain (InstantiationCollector::chainTo()) as
/// `a -> b -> c`, eliding the middle of a long one (`first, ..., last`) so a
/// 65-deep runaway stays readable. Used by E079.
std::vector<std::string>
describeChain(const std::vector<const InstantiationEntry *> &Chain);

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
///
/// TYPE-002 (S15/D5): @p Module, when non-empty, is prepended
/// ("<module>_<name>_gen<N>_..."). Only finishInstantiation() (Monomorphize.cpp)
/// passes it, using the request's own SourceModule -- a FreeFunction/Method-
/// kind InstantiationRequest's SourceModule comes from the already-resolved
/// symbol (Best->SourceModule), reliable regardless of how the call site
/// spelled the reference (buildDeclRegistry()'s own doc comment). Without
/// this, two DIFFERENT modules declaring the same-named generic free
/// function (`fn<T> tag(x: T): int` in both `amod.tr` and `bmod.tr`) both
/// produced the identically-named clone "tag_gen1_int" -- correctly
/// distinct INSTANTIATION-CACHE entries (InstantiationRequest::
/// sameInstantiationAs() already keys on SourceModule), but ONE shared,
/// colliding declared NAME once cloned -- "Duplicate function definition:
/// 'tag_gen1_int'", a regression against the pre-TYPE-002 compiler, which
/// mangled each module's own "tag" independently. Every OTHER call site
/// (struct instantiation naming, all in CodegenCore.cpp/CodegenStruct.cpp/
/// CodegenFunc.cpp) leaves this unset -- a struct's BaseName is already its
/// TypeIdentity (`list::List`, FDN-003), which carries the owning module, so
/// folding it in again would only add noise, not fix anything real.
std::string instantiatedDeclName(const std::string &BaseName,
                                 const std::vector<TurfTypePtr> &TypeArgs,
                                 const std::string &Module = "");

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
