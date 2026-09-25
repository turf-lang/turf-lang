/*
 * runtime/turfrt_alloc.c -- process bootstrap, the allocation entrypoints,
 * and the allocation-accounting hook (MEM-001's turf_rt_stats(), extended
 * by MEM-002 with the collector's own counters).
 *
 * turf_rt_init() now does the one piece of MEM-002 setup that has to run
 * before ANY Turf-level code does: recording the stack base conservative
 * scanning needs (turf_gc_record_stack_base(), turfrt_gc.c -- see that
 * file's own top comment for why "a local variable's address inside this
 * function" is the right thing to capture here). turf_rt_shutdown() is
 * unchanged from RT-001/MEM-001 -- still just the opt-in TURF_RT_STATS_DUMP
 * dump; MEM-002 deliberately does NOT run a final collection at shutdown
 * (nothing in §14/§22.2 asks for one, and process exit reclaims everything
 * anyway -- see the MEM-002 change record's Known pitfalls for why this is
 * a considered omission, not an oversight).
 *
 * turf_rt_alloc()/turf_rt_alloc_zeroed()/turf_rt_realloc() share the same
 * fail-fast-through-turf_rt_panic() OOM policy "in one place"
 * (FREEZE.md §6.8/ARCHITECTURE_OVERHAUL.md §8 RT-001) that
 * lib/std/mem/mem.tr's mem::alloc()/alloc_zeroed()/grow() already use via
 * sys::panic() for the manual heap (F-MEM-3 keeps the two heaps' OOM
 * policies independently implemented, but both fail the same way: loudly,
 * never a silent NULL a caller might forget to check). As of MEM-002, each
 * additionally REGISTERS the block with the collector (turf_gc_register_block/
 * turf_gc_reregister_block, turfrt_gc.c) and runs the allocation-pressure
 * check (turf_gc_note_alloc) that may trigger a collection before
 * returning -- the block is always registered FIRST, so it is a legitimate
 * root candidate (found on the C stack/in a register, still live inside
 * this very function) by the time that check can run, never swept out from
 * under its own about-to-be-returned pointer.
 */

#include "turfrt.h"
#include "turfrt_internal.h"

#include <stdio.h>
#include <stdlib.h>

static TurfRtStats g_stats = {0, 0, 0, 0, 0, 0, 0};

void turf_rt_init(void) {
  turf_gc_record_stack_base();
}

void turf_rt_shutdown(void) {
  /* Opt-in diagnostic dump only, so it never changes a compiled program's
   * default observable output (see this file's own top comment). */
  if (getenv("TURF_RT_STATS_DUMP") != NULL) {
    TurfRtStats s = turf_rt_stats();
    fprintf(stderr,
            "TURF_RT_STATS alloc_count=%llu realloc_count=%llu "
            "bytes_requested=%llu collect_count=%llu live_count=%llu "
            "live_bytes=%llu freed_count=%llu\n",
            (unsigned long long)s.alloc_count,
            (unsigned long long)s.realloc_count,
            (unsigned long long)s.bytes_requested,
            (unsigned long long)s.collect_count,
            (unsigned long long)s.live_count,
            (unsigned long long)s.live_bytes,
            (unsigned long long)s.freed_count);
  }
}

void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc) {
  void *p = malloc(bytes);
  if (!p) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_alloc>", 0);
  }
  turf_gc_register_block(p, bytes, desc);
  g_stats.alloc_count++;
  g_stats.bytes_requested += bytes;
  turf_gc_note_alloc(bytes);
  return p;
}

void *turf_rt_alloc_zeroed(size_t bytes, const TurfTypeDesc *desc) {
  void *p = calloc(1, bytes);
  if (!p) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_alloc_zeroed>", 0);
  }
  turf_gc_register_block(p, bytes, desc);
  g_stats.alloc_count++;
  g_stats.bytes_requested += bytes;
  turf_gc_note_alloc(bytes);
  return p;
}

void *turf_rt_realloc(void *p, size_t new_size, const TurfTypeDesc *desc) {
  uintptr_t old_addr = (uintptr_t)p;
  void *result = realloc(p, new_size);
  if (!result) {
    turf_rt_panic("out of memory", desc && desc->name ? desc->name : "<raw>",
                  "<turf_rt_realloc>", 0);
  }
  turf_gc_reregister_block(old_addr, result, new_size, desc);
  g_stats.realloc_count++;
  g_stats.bytes_requested += new_size;
  turf_gc_note_alloc(new_size);
  return result;
}

TurfRtStats turf_rt_stats(void) {
  turf_gc_snapshot_stats(&g_stats.collect_count, &g_stats.live_count,
                          &g_stats.live_bytes, &g_stats.freed_count);
  return g_stats;
}

void turf_rt_checked_free(void *p) {
  if (p != NULL && turf_gc_is_registered(p)) {
    turf_rt_panic("cross-heap free",
                  "mem::free() was called on a pointer that belongs to the "
                  "GC heap (a string or collection buffer), not the manual "
                  "heap -- the two heaps do not merge",
                  "<mem::free>", 0);
  }
  free(p);
}
