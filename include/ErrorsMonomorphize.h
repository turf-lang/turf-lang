#ifndef ERRORS_MONOMORPHIZE_H
#define ERRORS_MONOMORPHIZE_H

#include "Errors.h"

#include <string>
#include <vector>

/**
 * @brief A chain of generic instantiations requesting each other never
 *        bottoms out -- e.g. `fn<T> wrap(x: T) { wrap(Box{val:x}); }`,
 *        where every instantiation of `wrap` requests a new, strictly
 *        larger instantiation of itself, forever.
 *
 * §9.5 step 4 (ARCHITECTURE_OVERHAUL.md): the monomorphizer's worklist is
 * capped at Monomorphize::kMaxInstantiationDepth (default 64) rather than
 * run to real exhaustion, exactly the way a C++ template-recursion-depth
 * limit works, and for the identical reason -- an unbounded chain is
 * almost always a mistake, not a real program, and running out of memory
 * mid-compile is a much worse failure mode than a clear diagnostic naming
 * the chain.
 *
 * Not yet raised anywhere (S11b) -- the worklist this would guard isn't
 * drained until S12a. The class and its E079 registration exist now so
 * S12a wires the depth check against an already-agreed diagnostic rather
 * than inventing one under time pressure.
 */
class GenericInstantiationDepthError : public TurfError {
public:
  /**
   * @param Loc   Where the depth cap was hit -- the instantiation request
   *              that would have been the (kMaxInstantiationDepth+1)th.
   * @param Chain The instantiation chain in request order, e.g.
   *              {"wrap<int>", "wrap<Box<int>>", "wrap<Box<Box<int>>>", ...}
   *              -- rendered the same way StructLayoutCycleError renders a
   *              cycle path, since both are "here is the whole chain, not
   *              just where it broke" diagnostics.
   */
  GenericInstantiationDepthError(SourceLocation Loc,
                                  const std::vector<std::string> &Chain)
      : TurfError(Loc, Colors::BRIGHT_RED +
                            "generic instantiation depth exceeded (" +
                            std::to_string(Chain.size()) + " deep)." +
                            Colors::RESET) {
    Category_ = SLM::ErrorCategory::DECLARATION;
    std::string Path;
    for (size_t i = 0; i < Chain.size(); ++i) {
      if (i) Path += Colors::BRIGHT_GREEN + " -> " + Colors::CYAN;
      Path += Chain[i];
    }
    Message += "\n  " + Colors::BRIGHT_GREEN + "chain: " + Colors::CYAN +
               Path + Colors::RESET;
    Hint_ = "A generic function or struct that requests a new instantiation "
            "of itself every time it runs never bottoms out. If this is "
            "intentional (e.g. a type-level Peano count), it's past what "
            "this compiler will monomorphize -- restructure it to stop "
            "growing the type at some concrete depth.";
  }
};

#endif
