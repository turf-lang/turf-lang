/*
 * runtime/turfrt_gc.c -- MEM-002: the collector
 * (docs/planning/ARCHITECTURE_OVERHAUL.md §8/§14, FREEZE.md §6.5).
 *
 * Non-moving, mark-sweep, precise on the heap (per-type/per-element pointer
 * bitmaps -- TurfTypeDesc, turfrt.h), conservative on the stack and
 * callee-saved registers, stop-the-world, single-threaded, no finalizers.
 * The manual heap (mem::alloc/malloc) is never touched: this file has no
 * block-table entry for anything it didn't itself hand out via
 * turf_rt_alloc/turf_rt_alloc_zeroed/turf_rt_realloc (F-MEM-3).
 *
 * ─── The block table ────────────────────────────────────────────────────
 * Every live GC-heap block is one `GcBlock {start, size, desc, marked}`.
 * Two arrays hold them:
 *   - `g_pending` -- unsorted, append-only. Every turf_rt_alloc/realloc
 *     call appends here in O(1) amortized time; this is the hot path and
 *     must not pay a sort/shift cost per allocation.
 *   - `g_blocks` -- sorted by `start`, rebuilt (pending merged in, whole
 *     array re-sorted) exactly once at the top of every collection. Kept
 *     sorted between collections too (`turf_gc_reregister_block`'s removal
 *     path uses memmove, not swap-with-last) so realloc's exact-match
 *     lookup and turf_rt_is_gc_pointer's exact-match lookup can binary
 *     search it without triggering a full rebuild.
 * A block-CONTAINING-address query (`gc_find_containing`, what the mark
 * phase needs for both conservative roots and interior pointers --
 * `List<T>.data + k` is the documented, common case, LIMITATIONS.md §4)
 * only ever runs against `g_blocks`, and only during a collection, so it is
 * always searching an already-sorted, already-merged array.
 *
 * ─── Root discovery ─────────────────────────────────────────────────────
 * 1. Callee-saved registers (rbx/rbp/r12-r15, x86-64 System V) -- spilled
 *    to memory by `gc_capture_callee_saved`'s inline asm, NOT via
 *    setjmp(): glibc's setjmp/longjmp XOR-obfuscate saved pointer values
 *    with a per-process secret (PTR_MANGLE) since glibc 2.4, which would
 *    make every register's saved bit pattern fail to resemble a real heap
 *    address -- verified against glibc's own jmp_buf implementation before
 *    choosing inline asm over the more obvious setjmp()-based approach the
 *    roadmap's own text describes generically as "the standard Boehm
 *    technique". Boehm's own GC_push_regs avoids the identical trap the
 *    same way, for the same reason, on this same platform.
 * 2. The stack range [current SP, stack base) -- conservative, word-by-word.
 *    `g_stack_base` is captured once, in `turf_gc_record_stack_base()`
 *    (called from turf_rt_init(), before a single line of Turf-level `main`
 *    body runs -- INV-RT-1). "Current SP" is approximated by the address of
 *    a local variable in the innermost scanning function, the same
 *    from-scratch-collector idiom that lets stack scanning need zero
 *    compiler/Codegen cooperation: everything BELOW that address belongs to
 *    this file's own C call frames, never to Turf-level data, so excluding
 *    it is correct, not merely convenient.
 * 3. Registered global roots (`turf_rt_add_global_root`) -- precise:
 *    dereferenced fresh at the start of every mark phase.
 * 4. Once conservative/precise roots mark a block, ITS OWN contents are
 *    traced PRECISELY via its TurfTypeDesc (an explicit worklist, not
 *    recursion -- a long `List<Node*>` chain must not blow the C stack).
 *
 * ─── Why a value found in a register/stack word during the FIRST pass
 * (the just-allocated block still only live in turf_rt_alloc's own local,
 * see turf_rt_collect()'s allocation-pressure ordering, turfrt.h) survives
 * correctly: by the x86-64 SysV ABI, calling a function (turf_rt_collect())
 * obligates the CALLER to have already spilled to the stack, or kept in a
 * callee-saved register, anything it needs to survive that call -- either
 * way this file's own scan (stack range + callee-saved registers) finds it.
 */

#include "turfrt.h"
#include "turfrt_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__x86_64__)
#error "turfrt_gc.c's register-scanning path (gc_capture_callee_saved) is \
implemented for x86-64 System V only -- porting notes in \
docs/development/changes/MEM-002.md's Known pitfalls."
#endif

/* ─── Block table ────────────────────────────────────────────────────── */

typedef struct GcBlock {
  uintptr_t start;
  size_t size;
  const TurfTypeDesc *desc; /* NULL = opaque, no traced pointer words */
  unsigned char marked;
} GcBlock;

static GcBlock *g_blocks = NULL;
static size_t g_block_count = 0, g_block_cap = 0;

static GcBlock *g_pending = NULL;
static size_t g_pending_count = 0, g_pending_cap = 0;

static void **g_roots = NULL;
static size_t g_root_count = 0, g_root_cap = 0;

static uintptr_t g_stack_base = 0;

static uint64_t g_bytes_since_collect = 0;
#define TURF_GC_INITIAL_THRESHOLD ((uint64_t)(1u << 20)) /* 1 MiB */
static uint64_t g_gc_threshold = TURF_GC_INITIAL_THRESHOLD;

/* Cumulative/point-in-time counters surfaced through turf_rt_stats() --
 * MEM-001's own text named this hook as "MEM-002's own regression harness";
 * these three fields are what that promise cashes out to. */
static uint64_t g_collect_count = 0;
static uint64_t g_freed_count_total = 0;
static uint64_t g_live_count = 0;
static uint64_t g_live_bytes = 0;

/* A tiny generic growable-array grower shared by every array above (blocks,
 * pending, roots, and the mark worklist below) -- one realloc-doubling
 * implementation instead of four hand-copies of the same 6 lines. */
static void gc_arr_grow(void **arr, size_t *cap, size_t elem_size,
                         size_t need) {
  if (need <= *cap)
    return;
  size_t new_cap = (*cap == 0) ? 16 : *cap;
  while (new_cap < need)
    new_cap *= 2;
  void *p = realloc(*arr, new_cap * elem_size);
  if (!p) {
    turf_rt_panic("out of memory", "<gc block table>", "<turfrt_gc.c>", 0);
  }
  *arr = p;
  *cap = new_cap;
}

/*
 * Deliberately NOT "the address of a local variable inside
 * turf_rt_init()'s own frame", despite that being the more obvious
 * from-scratch-collector idiom (and this file's top comment's own
 * description of "current SP" a few lines below, which IS captured that
 * way safely -- see why this case differs, next paragraph). Found live,
 * via a genuine heap-use-after-free ASan repro
 * (tests/13_string_stdlib/17_stress_10k_concat.tr): turf_rt_init() is a
 * real function CALL that RETURNS before the user's real `main()` body
 * starts running (WrapMainWithRuntimeBootstrap(), CodegenFunc.cpp calls it,
 * gets its return, THEN calls the renamed user main -- a SIBLING call at
 * the wrapper's own depth, not a nested one). Once turf_rt_init() returns,
 * its own frame is popped, and the user main() call that follows reuses
 * that exact address range (or deeper) for ITS OWN frame -- meaning a
 * local variable's address captured INSIDE turf_rt_init() is NOT
 * guaranteed to sit above every frame the scanned program will ever push;
 * `main`'s own locals can end up at addresses the naive capture excluded
 * from [sp, stack_base), silently dropping them from every subsequent
 * conservative scan. (The "current SP" half of scanning, by contrast, is
 * captured fresh inside gc_mark() itself on EVERY collection -- there is
 * no stale-capture hazard there, only here, where the value is captured
 * exactly ONCE for the whole process.)
 *
 * The fix: query the OS for the thread's REAL stack region (Linux-
 * specific -- this codebase already targets glibc/Linux primarily, e.g.
 * net.tr's Linux-numbered socket constants, mem.tr's
 * malloc_usable_size()), which is fixed at thread-creation time and stays
 * correct regardless of current call depth.
 */
void turf_gc_record_stack_base(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (f) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
      if (strstr(line, "[stack]")) {
        unsigned long long lo = 0, hi = 0;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2) {
          g_stack_base = (uintptr_t)hi;
        }
        break;
      }
    }
    fclose(f);
  }
  if (g_stack_base == 0) {
    /* /proc unavailable (a minimal container/sandbox without procfs) --
     * degrade to the call-depth-dependent local-variable capture rather
     * than scanning nothing at all. Known pitfall, not silently accepted
     * as equivalent: see docs/development/changes/MEM-002.md. */
    int marker;
    g_stack_base = (uintptr_t)&marker;
  }
}

static long find_exact_in_pending(uintptr_t addr) {
  for (size_t i = 0; i < g_pending_count; i++)
    if (g_pending[i].start == addr)
      return (long)i;
  return -1;
}

/* g_blocks is kept sorted by `start` at all times between rebuilds (the
 * only mutators -- gc_rebuild_index's qsort, gc_sweep's order-preserving
 * compaction, and this file's own memmove-based removal below -- all
 * preserve that invariant), so an exact match can binary search. */
static long find_exact_in_blocks(uintptr_t addr) {
  size_t lo = 0, hi = g_block_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (g_blocks[mid].start == addr)
      return (long)mid;
    if (g_blocks[mid].start < addr)
      lo = mid + 1;
    else
      hi = mid;
  }
  return -1;
}

void turf_gc_register_block(void *p, size_t size, const TurfTypeDesc *desc) {
  if (!p)
    return;
  gc_arr_grow((void **)&g_pending, &g_pending_cap, sizeof(GcBlock),
              g_pending_count + 1);
  g_pending[g_pending_count].start = (uintptr_t)p;
  g_pending[g_pending_count].size = size;
  g_pending[g_pending_count].desc = desc;
  g_pending[g_pending_count].marked = 0;
  g_pending_count++;
}

void turf_gc_reregister_block(uintptr_t old_addr, void *new_p,
                               size_t new_size, const TurfTypeDesc *desc) {
  long idx;
  if (old_addr != 0) {
    if ((idx = find_exact_in_pending(old_addr)) >= 0) {
      /* Unsorted array -- swap-with-last removal is fine. */
      g_pending[idx] = g_pending[--g_pending_count];
    } else if ((idx = find_exact_in_blocks(old_addr)) >= 0) {
      /* Sorted array -- must shift, not swap, to preserve order. */
      memmove(&g_blocks[idx], &g_blocks[idx + 1],
              (g_block_count - (size_t)idx - 1) * sizeof(GcBlock));
      g_block_count--;
    }
    /* Not found: see this function's own doc comment in
     * turfrt_internal.h -- degrade to a fresh registration. */
  }
  turf_gc_register_block(new_p, new_size, desc);
}

int turf_gc_is_registered(const void *p) {
  if (!p)
    return 0;
  uintptr_t addr = (uintptr_t)p;
  if (find_exact_in_pending(addr) >= 0)
    return 1;
  if (find_exact_in_blocks(addr) >= 0)
    return 1;
  return 0;
}

int turf_rt_is_gc_pointer(const void *p) { return turf_gc_is_registered(p); }

/* ─── Global roots ───────────────────────────────────────────────────── */

void turf_rt_add_global_root(void **slot) {
  if (!slot)
    return;
  gc_arr_grow((void **)&g_roots, &g_root_cap, sizeof(void *),
              g_root_count + 1);
  g_roots[g_root_count++] = (void *)slot;
}

void turf_rt_remove_global_root(void **slot) {
  for (size_t i = 0; i < g_root_count; i++) {
    if (g_roots[i] == (void *)slot) {
      g_roots[i] = g_roots[--g_root_count];
      return;
    }
  }
}

/* ─── Collection ─────────────────────────────────────────────────────── */

static int gcblock_cmp(const void *a, const void *b) {
  const GcBlock *ba = (const GcBlock *)a;
  const GcBlock *bb = (const GcBlock *)b;
  if (ba->start < bb->start)
    return -1;
  if (ba->start > bb->start)
    return 1;
  return 0;
}

static void gc_rebuild_index(void) {
  if (g_pending_count == 0)
    return;
  gc_arr_grow((void **)&g_blocks, &g_block_cap, sizeof(GcBlock),
              g_block_count + g_pending_count);
  memcpy(&g_blocks[g_block_count], g_pending,
         g_pending_count * sizeof(GcBlock));
  g_block_count += g_pending_count;
  g_pending_count = 0;
  qsort(g_blocks, g_block_count, sizeof(GcBlock), gcblock_cmp);
}

/* Containment query -- last block whose start <= addr, then a bounds
 * check. Only ever called during a collection, against the just-rebuilt,
 * fully-sorted g_blocks. */
static GcBlock *gc_find_containing(uintptr_t addr) {
  size_t lo = 0, hi = g_block_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (g_blocks[mid].start <= addr)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0)
    return NULL;
  GcBlock *cand = &g_blocks[lo - 1];
  if (addr >= cand->start && addr < cand->start + cand->size)
    return cand;
  return NULL;
}

/* See this file's top comment: NOT setjmp-based, because glibc's jmp_buf
 * pointer-mangles saved registers on this platform. */
__attribute__((noinline)) static void
gc_capture_callee_saved(uintptr_t regs[6]) {
  __asm__ volatile("movq %%rbx, %0\n\t"
                    "movq %%rbp, %1\n\t"
                    "movq %%r12, %2\n\t"
                    "movq %%r13, %3\n\t"
                    "movq %%r14, %4\n\t"
                    "movq %%r15, %5\n\t"
                    : "=m"(regs[0]), "=m"(regs[1]), "=m"(regs[2]),
                      "=m"(regs[3]), "=m"(regs[4]), "=m"(regs[5])
                    :
                    : "memory");
}

typedef struct Worklist {
  GcBlock **items;
  size_t count, cap;
} Worklist;

static void wl_push(Worklist *wl, GcBlock *b) {
  gc_arr_grow((void **)&wl->items, &wl->cap, sizeof(GcBlock *), wl->count + 1);
  wl->items[wl->count++] = b;
}

static void gc_consider_root(uintptr_t candidate, Worklist *wl) {
  GcBlock *b = gc_find_containing(candidate);
  if (b && !b->marked) {
    b->marked = 1;
    wl_push(wl, b);
  }
}

/* Word-by-word conservative scan of [lo, hi) -- every aligned word is a
 * POSSIBLE pointer; gc_consider_root's own block-table lookup is what
 * actually decides, so over-scanning here costs false-retention risk, not
 * correctness (§14's own accepted tradeoff). */
static void gc_scan_range_conservative(uintptr_t lo, uintptr_t hi,
                                        Worklist *wl) {
  lo &= ~((uintptr_t)(sizeof(uintptr_t) - 1));
  for (uintptr_t a = lo; a + sizeof(uintptr_t) <= hi;
       a += sizeof(uintptr_t)) {
    uintptr_t word = *(uintptr_t *)a;
    gc_consider_root(word, wl);
  }
}

/* Precise trace of one already-marked block's own declared pointer words.
 * `desc->size` is this block's ELEMENT stride (turfrt.h's TurfTypeDesc doc
 * comment) -- repeated across the block's real allocated length, which
 * degrades to exactly one repetition for a single-instance struct
 * allocation (block size == desc->size) with no separate code path needed. */
static void gc_scan_block_precise(GcBlock *b, Worklist *wl) {
  if (!b->desc || b->desc->size == 0)
    return; /* opaque: no outgoing pointer word anywhere in this block */
  uint64_t stride = b->desc->size;
  for (uint64_t off = 0; off + stride <= b->size; off += stride) {
    /* OVH-001: word_count is the descriptor's REAL word count, at any
     * size -- no fixed cap here any more (the old 64-word cap silently
     * dropped every pointer field past byte 512, a live use-after-free). */
    uint64_t max_word = b->desc->word_count;
    for (uint64_t w = 0; w < max_word; w++) {
      if (!turf_rt_typedesc_is_pointer_word(b->desc, w))
        continue;
      uintptr_t addr = b->start + off + w * 8;
      if (addr + sizeof(uintptr_t) > b->start + b->size)
        continue; /* defensive: a malformed/truncated final stride */
      uintptr_t val = *(uintptr_t *)addr;
      gc_consider_root(val, wl);
    }
  }
}

static void gc_mark(void) {
  gc_rebuild_index();
  for (size_t i = 0; i < g_block_count; i++)
    g_blocks[i].marked = 0;

  Worklist wl = {0};

  uintptr_t regs[6];
  gc_capture_callee_saved(regs);
  for (int i = 0; i < 6; i++)
    gc_consider_root(regs[i], &wl);

  int marker;
  uintptr_t sp = (uintptr_t)&marker;
  if (g_stack_base > sp)
    gc_scan_range_conservative(sp, g_stack_base, &wl);

  for (size_t i = 0; i < g_root_count; i++) {
    void **slot = (void **)g_roots[i];
    gc_consider_root((uintptr_t)*slot, &wl);
  }

  while (wl.count > 0) {
    GcBlock *b = wl.items[--wl.count];
    gc_scan_block_precise(b, &wl);
  }

  free(wl.items);
}

static void gc_sweep(void) {
  size_t w = 0;
  uint64_t live_bytes = 0;
  uint64_t freed_this_cycle = 0;
  for (size_t i = 0; i < g_block_count; i++) {
    if (g_blocks[i].marked) {
      live_bytes += g_blocks[i].size;
      g_blocks[w++] = g_blocks[i];
    } else {
      free((void *)g_blocks[i].start);
      freed_this_cycle++;
    }
  }
  g_block_count = w;
  g_live_count = w;
  g_live_bytes = live_bytes;
  g_freed_count_total += freed_this_cycle;

  /* Heap-doubling: the next collection triggers once roughly this cycle's
   * live-set worth of NEW allocation has happened again, with a floor so a
   * near-empty heap doesn't collect on every single small allocation. */
  g_gc_threshold = live_bytes * 2;
  if (g_gc_threshold < TURF_GC_INITIAL_THRESHOLD)
    g_gc_threshold = TURF_GC_INITIAL_THRESHOLD;
}

void turf_rt_collect(void) {
  gc_mark();
  gc_sweep();
  g_bytes_since_collect = 0;
  g_collect_count++;
}

void turf_gc_note_alloc(size_t bytes) {
  g_bytes_since_collect += bytes;
  if (g_bytes_since_collect >= g_gc_threshold) {
    turf_rt_collect();
  }
}

/* Exposed to turfrt_alloc.c's turf_rt_stats() -- kept as a function rather
 * than extern globals so this file stays the only one that knows the block
 * table's real shape. */
void turf_gc_snapshot_stats(uint64_t *collect_count, uint64_t *live_count,
                             uint64_t *live_bytes, uint64_t *freed_count) {
  *collect_count = g_collect_count;
  *live_count = g_live_count;
  *live_bytes = g_live_bytes;
  *freed_count = g_freed_count_total;
}
