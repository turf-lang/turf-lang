/*
 * runtime/turfrt_internal.h -- private glue between turfrt_alloc.c and
 * turfrt_gc.c (MEM-002, docs/planning/ARCHITECTURE_OVERHAUL.md §8/§14).
 *
 * NOT part of turfrt.h's public C ABI: nothing here is ever referenced by
 * Codegen (F-STD-2's closed intrinsic list is about what a COMPILED TURF
 * PROGRAM can reach; this header is internal to libturfrt's own .c files
 * calling each other) and this file is never installed alongside turfrt.h.
 */

#ifndef TURFRT_INTERNAL_H
#define TURFRT_INTERNAL_H

#include "turfrt.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Registers a freshly-obtained block (already actually allocated via
 * malloc/calloc) with the collector's block table. `desc` may be NULL
 * (opaque -- no outgoing pointer word inside this block is ever traced;
 * correct for a raw byte run such as a string buffer, wrong for a buffer
 * that holds pointer-shaped elements -- see turfrt.h's TurfTypeDesc doc
 * comment). Must be called before the pointer is handed anywhere else: a
 * GC-heap block is invisible to the collector until registered.
 */
void turf_gc_register_block(void *p, size_t size, const TurfTypeDesc *desc);

/*
 * Moves an existing registration from `old_p` to `new_p` with a new
 * size/desc -- realloc() may relocate the underlying memory. If `old_p`
 * is not a currently-registered block, this degrades to registering
 * `new_p` as a brand-new block rather than aborting: every real
 * turf_rt_realloc call site passes back a pointer this API itself
 * previously returned, so this path should be unreachable for a
 * well-formed caller, but a defensive degrade (rather than a crash) means
 * a latent bug surfaces as an oddly early collection, not a runtime abort
 * in unrelated code.
 */
/*
 * `old_addr` is the OLD block's address taken as a plain integer (not
 * `void *`) deliberately: the caller obtains it from a pointer that
 * realloc() may already have freed by the time this runs (a legal,
 * ordinary C comparison-by-value, but one that trips `-Wuse-after-free` if
 * spelled as a `void *` parameter here) -- this function only ever uses it
 * as an equality-comparison key against the block table, never
 * dereferences it.
 */
void turf_gc_reregister_block(uintptr_t old_addr, void *new_p,
                               size_t new_size, const TurfTypeDesc *desc);

/*
 * Called after every successful alloc/realloc with the number of bytes
 * newly requested. Runs the allocation-pressure heuristic (heap-doubling:
 * collect once cumulative requests since the last collection cross a
 * threshold, then set the next threshold from the post-sweep live total)
 * and triggers turf_rt_collect() when it decides to.
 */
void turf_gc_note_alloc(size_t bytes);

/*
 * Exact-match lookup: is `p` the exact start address of a block the
 * collector currently knows about (whether or not a collection has run
 * since it was allocated)? Cheap and always available -- does not require
 * an index rebuild. turf_rt_is_gc_pointer() (turfrt.h) is this function
 * under its public name; kept distinct so turfrt_alloc.c can also use it
 * internally without pulling in the public-API naming.
 */
int turf_gc_is_registered(const void *p);

/*
 * Captures the process's approximate stack base -- the high-address end of
 * the range conservative scanning walks down from. Called exactly once,
 * from turf_rt_init(), before a single line of Turf-level code has run.
 * See turfrt_gc.c's own top comment for why "the address of a local
 * variable inside turf_rt_init()'s own frame" is a correct (if slightly
 * loose) choice.
 */
void turf_gc_record_stack_base(void);

/*
 * Reads the collector's own counters back out for turf_rt_stats() (turfrt.h)
 * to fold into its public TurfRtStats snapshot -- kept as an accessor,
 * rather than extern globals turfrt_alloc.c reads directly, so turfrt_gc.c
 * stays the only file that knows the block table's real shape.
 */
void turf_gc_snapshot_stats(uint64_t *collect_count, uint64_t *live_count,
                             uint64_t *live_bytes, uint64_t *freed_count);

#endif /* TURFRT_INTERNAL_H */
