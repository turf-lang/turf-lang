/*
 * runtime/turfrt_alloc.c -- process bootstrap and the allocation entrypoint.
 *
 * turf_rt_init()/turf_rt_shutdown() are deliberately trivial in this group
 * (RT-001) -- there is no heap to stand up yet (MEM-002). They exist now
 * purely so the compiler-emitted wrapper around `main`
 * (src/CodegenFunc.cpp's WrapMainWithRuntimeBootstrap()) has a real symbol
 * to call on every code path, so MEM-002 never needs a second
 * compiler-side change to start calling into a real bootstrap.
 *
 * turf_rt_alloc() is a thin malloc() wrapper with the out-of-memory policy
 * "in one place" (FREEZE.md §6.8/ARCHITECTURE_OVERHAUL.md §8 RT-001) --
 * fail fast through turf_rt_panic() rather than returning NULL, the same
 * fail-fast convention lib/std/mem/mem.tr's mem::alloc() already uses via
 * sys::panic() for the manual heap (F-MEM-3 keeps the two heaps' OOM
 * policies independently implemented, but both fail the same way: loudly,
 * never a silent NULL a caller might forget to check).
 */

#include "turfrt.h"

#include <stdlib.h>

void turf_rt_init(void) {
  /* No global state to initialize yet -- MEM-002's heap/block-table setup
   * lands here. */
}

void turf_rt_shutdown(void) {
  /* No global state to tear down yet -- MEM-002's final-collection/stats
   * hook lands here. */
}

void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc) {
  void *p = malloc(bytes);
  if (!p) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_alloc>", 0);
  }
  return p;
}
