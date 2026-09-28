/*
 * runtime/turfrt_abi.c -- FDN-006 stage 3, the compiler/runtime ABI stamp
 * (docs/planning/ROADMAP_v2.0.0.md FDN-006).
 *
 * `turf_rt_abi_2_0` is a versioned symbol with no behaviour of its own --
 * every compiled Turf binary's `main` wrapper references it
 * (WrapMainWithRuntimeBootstrap(), src/CodegenFunc.cpp), purely so that
 * linking a freshly compiled object against a STALE `libturfrt.a` (one
 * built before this symbol existed, or a hypothetical future ABI-breaking
 * runtime that renames it) fails at LINK TIME with "undefined reference to
 * `turf_rt_abi_2_0`" -- a symbol name that says exactly what's wrong --
 * instead of silently linking and producing undefined behaviour at run
 * time against a runtime whose struct layouts/calling convention the
 * compiler no longer agrees with. Bump the suffix (`_2_0` -> `_2_1`, ...)
 * the next time a change to this file's ABI (TurfTypeDesc's layout,
 * turf_rt_alloc's signature, ...) would make an old libturfrt.a and a new
 * compiler (or vice versa) silently incompatible -- both the compiler's own
 * reference (CodegenFunc.cpp) and this definition must be renamed together,
 * which is the whole point: a mismatched pair no longer link, they don't
 * quietly run.
 */

#include "turfrt.h"

void turf_rt_abi_2_0(void) {}
