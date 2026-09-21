/**
 * @file Monomorphize.h
 * @brief TYPE-002 monomorphizer -- instantiation request tracking.
 *
 * S11b (docs/development/changes/TYPE-002.md) stands up the skeleton only:
 * a request/cache/worklist shape that can RECORD every generic
 * instantiation the type-checked AST implies, and report it. Nothing here
 * clones an AST, substitutes a type, or registers a real symbol yet --
 * that is S12a onward. Wiring this collector into TypeChecker (report-only
 * mode, gated on `--dump-instantiations`) is therefore a zero-behaviour-
 * change addition: nothing consumes the worklist this session.
 *
 * Pipeline position (§9.5 of ARCHITECTURE_OVERHAUL.md, confirmed against
 * source in S11a -- see TYPE-002.md's U-4' answer): this collector is fed
 * DURING the pre-pass's own TypeChecker run (sub-pass 2b), from inside
 * TypeChecker::visitFuncCall/visitMethodCall/visitVarDecl, at the exact
 * points those already compute a generic substitution map today. It is
 * never fed from the main pass -- U-4' proved the pre-pass sees every
 * instantiation request in the program, since a bare top-level statement
 * outside any function is a hard compile error (main.cpp:1145-1164).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Apache License.
 */

#ifndef MONOMORPHIZE_H
#define MONOMORPHIZE_H

#include "Lexer.h" // SourceLocation
#include "Types.h"

#include <deque>
#include <ostream>
#include <string>
#include <vector>

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
/// `DeclKey` with these exact type arguments." `DeclKey` is the same
/// module-qualified base name candidate lookup already uses (e.g.
/// "identity", "List.push", "List") -- NOT a mangled name; mangling only
/// happens once a request is actually acted on (S12a+).
struct InstantiationRequest {
  InstantiationKind Kind;
  std::string DeclKey;
  std::vector<TurfTypePtr> TypeArgs;
  SourceLocation Site; ///< where this request was discovered (for a future
                       ///< instantiation-chain diagnostic, S12a).

  /// Structural equality, deliberately never by getName() string
  /// (INV-TYPE-1) -- two requests are the same instantiation only if every
  /// type argument is pairwise isEquivalent(), not merely same-spelled.
  bool sameInstantiationAs(const InstantiationRequest &Other) const {
    if (Kind != Other.Kind || DeclKey != Other.DeclKey)
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

/// Depth cap for the instantiation worklist once S12a starts actually
/// draining it (a recursive generic, e.g. `fn<T> wrap(x: T) {
/// wrap(Box{val:x}); }`, would otherwise recurse forever). Inert this
/// session -- nothing pops the worklist yet, so nothing can exceed it --
/// defined here now so S12a wires the check against an already-agreed
/// constant rather than picking a number under time pressure.
inline constexpr int kMaxInstantiationDepth = 64;

/// Deliberately linear-scan dedup, not a hash map: S11a's U-2 census found
/// ~18-20 concrete instantiations in the entire corpus today, so an
/// O(n^2) worst case over `insert` calls is not a real cost, and a linear
/// scan lets every comparison go through `sameInstantiationAs()`
/// (pairwise isEquivalent()) with no risk of a hand-rolled hash function
/// silently falling back to identity-by-getName() (INV-TYPE-1) the way a
/// naive `std::hash` specialization over TurfTypePtr would invite. Revisit
/// if S11b/S12a's real-corpus numbers ever look different from S11a's
/// census.
class InstantiationCache {
  std::vector<InstantiationRequest> Seen_;

public:
  bool contains(const InstantiationRequest &Req) const {
    for (const auto &S : Seen_)
      if (S.sameInstantiationAs(Req))
        return true;
    return false;
  }
  void insert(InstantiationRequest Req) { Seen_.push_back(std::move(Req)); }
  size_t size() const { return Seen_.size(); }
  const std::vector<InstantiationRequest> &all() const { return Seen_; }
};

/// Report-only collector: TypeChecker feeds it via record(); nothing
/// drains Worklist_ yet (S12a's job). Kept as a real, separate deque
/// rather than folding it into InstantiationCache now, so S12a's
/// "pop a request, clone-and-specialize, push any new requests the clone's
/// body produces" loop (§9.5 step 2-3) has the right shape already, not a
/// shape S11b's own skeleton has to be reworked to support.
class InstantiationCollector {
  InstantiationCache Cache_;
  std::deque<InstantiationRequest> Worklist_;

public:
  /// Record one instantiation request. A duplicate (same
  /// sameInstantiationAs()) is silently absorbed -- this is exactly the
  /// cache-hit path §9.5 step 2 describes ("rewrite the requesting site to
  /// point at the cached instantiation"), just with no rewrite target to
  /// point at yet.
  void record(InstantiationRequest Req) {
    if (Cache_.contains(Req))
      return;
    Worklist_.push_back(Req); // unread this session; shape for S12a.
    Cache_.insert(std::move(Req));
  }

  const InstantiationCache &cache() const { return Cache_; }
  size_t pendingInWorklist() const { return Worklist_.size(); }

  /// Human-readable dump for `--dump-instantiations` -- the live
  /// instrumentation S11a's U-2 static census stood in for.
  void report(std::ostream &OS) const;
};

} // namespace Monomorphize

#endif // MONOMORPHIZE_H
