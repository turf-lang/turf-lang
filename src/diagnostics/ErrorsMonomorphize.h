#ifndef ERRORS_MONOMORPHIZE_H
#define ERRORS_MONOMORPHIZE_H

#include "diagnostics/Errors.h"

#include <string>
#include <vector>

/**
 * @brief A chain of generic instantiations requesting each other never
 *        bottoms out -- e.g. `fn<T> wrap(x: T) { wrap(Box{val:x}); }`,
 *        where every instantiation of `wrap` requests a new, strictly
 *        larger instantiation of itself, forever.
 *
 * §9.5 step 4 (ARCHITECTURE_OVERHAUL.md): the monomorphizer caps the length
 * of a request chain at Monomorphize::kMaxInstantiationDepth (64) rather than
 * run to real exhaustion, exactly the way a C++ template-recursion-depth
 * limit works, and for the identical reason -- an unbounded chain is
 * almost always a mistake, not a real program, and running out of memory
 * mid-compile is a much worse failure mode than a clear diagnostic naming
 * the chain. A second, much larger cap on the TOTAL number of instantiations
 * (Monomorphize::kMaxTotalInstantiations) backstops a generic that FANS OUT
 * instead of recursing in a line; it reports through this same class, with
 * @p TotalCount set.
 *
 * LEC-005 (D25): the chain is the REAL parent chain of the request that hit
 * the limit (InstantiationRequest::Parent), not a dump of every request ever
 * made, and the diagnostic is anchored at its outermost user-code hop.
 */
class GenericInstantiationDepthError : public TurfError {
public:
  /**
   * @param Loc        The outermost user-code request in the chain.
   * @param Chain      The chain in request order, e.g.
   *                   {"wrap<int>", "wrap<Box<int>>", "wrap<Box<Box<int>>>", ...},
   *                   possibly elided in the middle (describeChain()) -- rendered
   *                   the same way StructLayoutCycleError renders a cycle path,
   *                   since both are "here is the whole chain, not just where it
   *                   broke" diagnostics.
   * @param Depth      The chain's true length, which @p Chain's entry count
   *                   understates once it has been elided.
   * @param TotalCount 0 for the depth cap; otherwise the instantiation count
   *                   that tripped the total cap instead.
   */
  GenericInstantiationDepthError(SourceLocation Loc,
                                  const std::vector<std::string> &Chain,
                                  size_t Depth, size_t TotalCount = 0)
      : TurfError(Loc, Colors::BRIGHT_RED +
                            (TotalCount
                                 ? "too many generic instantiations (" +
                                       std::to_string(TotalCount) +
                                       " distinct requests)."
                                 : "generic instantiation depth exceeded (" +
                                       std::to_string(Depth) + " deep).") +
                            Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    std::string Path;
    for (size_t i = 0; i < Chain.size(); ++i) {
      if (i) Path += Colors::BRIGHT_GREEN + " -> " + Colors::CYAN;
      Path += Chain[i];
    }
    Message += "\n  " + Colors::BRIGHT_GREEN + "chain: " + Colors::CYAN +
               Path + Colors::RESET;
    Hint_ = TotalCount
        ? "Some generic keeps requesting new instantiations faster than it "
          "can finish them -- typically one that grows its own type "
          "argument in more than one way at once. If this is intentional, "
          "it's past what this compiler will monomorphize; restructure it "
          "to stop growing the type at some concrete depth."
        : "A generic function or struct that requests a new instantiation "
          "of itself every time it runs never bottoms out. If this is "
          "intentional (e.g. a type-level Peano count), it's past what "
          "this compiler will monomorphize -- restructure it to stop "
          "growing the type at some concrete depth.";
  }
};

#endif
