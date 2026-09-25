/*
 * runtime/turfrt_alloc.c -- process bootstrap, the allocation entrypoints,
 * and MEM-001's allocation-accounting hook.
 *
 * turf_rt_init() is still trivial (RT-001) -- there is no heap STRUCTURE to
 * stand up yet (MEM-002's block table/free lists). turf_rt_shutdown() now
 * does exactly one optional thing beyond RT-001's no-op: dump g_stats to
 * stderr, gated on the TURF_RT_STATS_DUMP environment variable, so
 * scripts/mem_smoke.sh can observe allocation counts from outside without
 * any compiled Turf program's normal stdout/stderr output changing (see
 * turfrt.h's TurfRtStats doc comment for why this has to be opt-in).
 *
 * turf_rt_alloc()/turf_rt_alloc_zeroed()/turf_rt_realloc() share the same
 * fail-fast-through-turf_rt_panic() OOM policy "in one place"
 * (FREEZE.md §6.8/ARCHITECTURE_OVERHAUL.md §8 RT-001) that
 * lib/std/mem/mem.tr's mem::alloc()/alloc_zeroed()/grow() already use via
 * sys::panic() for the manual heap (F-MEM-3 keeps the two heaps' OOM
 * policies independently implemented, but both fail the same way: loudly,
 * never a silent NULL a caller might forget to check).
 */

#include "turfrt.h"

#include <stdio.h>
#include <stdlib.h>

static TurfRtStats g_stats = {0, 0, 0};

void turf_rt_init(void) {
  /* No global state to initialize yet -- MEM-002's heap/block-table setup
   * lands here. */
}

void turf_rt_shutdown(void) {
  /* MEM-002's final-collection/stats hook lands here too. For now: an
   * opt-in diagnostic dump only, so it never changes a compiled program's
   * default observable output (see this file's own top comment). */
  if (getenv("TURF_RT_STATS_DUMP") != NULL) {
    fprintf(stderr,
            "TURF_RT_STATS alloc_count=%llu realloc_count=%llu "
            "bytes_requested=%llu\n",
            (unsigned long long)g_stats.alloc_count,
            (unsigned long long)g_stats.realloc_count,
            (unsigned long long)g_stats.bytes_requested);
  }
}

void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc) {
  void *p = malloc(bytes);
  if (!p) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_alloc>", 0);
  }
  g_stats.alloc_count++;
  g_stats.bytes_requested += bytes;
  return p;
}

void *turf_rt_alloc_zeroed(size_t bytes, const TurfTypeDesc *desc) {
  void *p = calloc(1, bytes);
  if (!p) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_alloc_zeroed>", 0);
  }
  g_stats.alloc_count++;
  g_stats.bytes_requested += bytes;
  return p;
}

void *turf_rt_realloc(void *p, size_t new_size, const TurfTypeDesc *desc) {
  void *result = realloc(p, new_size);
  if (!result) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_realloc>", 0);
  }
  g_stats.realloc_count++;
  g_stats.bytes_requested += new_size;
  return result;
}

TurfRtStats turf_rt_stats(void) {
  return g_stats;
}
