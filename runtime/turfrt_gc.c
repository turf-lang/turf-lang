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
 * 1. Callee-saved registers -- spilled to memory by
 *    `gc_capture_callee_saved`'s inline asm, NOT via setjmp(): glibc's
 *    setjmp/longjmp XOR-obfuscate saved pointer values with a per-process
 *    secret (PTR_MANGLE) since glibc 2.4, which would make every register's
 *    saved bit pattern fail to resemble a real heap address -- verified
 *    against glibc's own jmp_buf implementation before choosing inline asm
 *    over the more obvious setjmp()-based approach the roadmap's own text
 *    describes generically as "the standard Boehm technique". Boehm's own
 *    GC_push_regs avoids the identical trap the same way, for the same
 *    reason, on the platforms it targets. OVH-013
 *    (docs/planning/OVERHAUL_REMAINING_WORK.md) widened this from
 *    "x86-64 System V only" to three concrete ABIs, selected at compile
 *    time (see the `TURF_GC_REGCAP_*` dispatch below the includes):
 *      - x86-64 System V (Linux, and any other non-Windows x86-64 target):
 *        rbx/rbp/r12-r15 -- the original RT-001/MEM-002 implementation.
 *      - x86-64 Win64 (Windows, MinGW-w64): the SysV set PLUS rdi/rsi
 *        (callee-saved under Win64, volatile under SysV) and xmm6-xmm15
 *        (Win64's calling convention makes the full 128 bits of each
 *        nonvolatile; SysV has no callee-saved XMM registers at all).
 *      - AArch64 AAPCS64 (macOS arm64, and any other AAPCS64 target such
 *        as Linux arm64): x19-x28, the frame-pointer register x29, and the
 *        low 64 bits of d8-d15 (AAPCS64 only requires the bottom half of
 *        v8-v15 preserved, not the full 128-bit q-register).
 *    Every set follows the identical technique this file always used: a
 *    `noinline` leaf function reads the CURRENT contents of exactly those
 *    registers, on the strength of the calling convention's own callee-saved
 *    contract (any frame between the true root and this read that needed one
 *    of these registers for itself must have already spilled the original
 *    value to ITS OWN stack frame before reusing it -- and that frame is
 *    covered by root 2 below). Porting to a fourth ABI means adding a fourth
 *    `TURF_GC_REGCAP_*` branch with THAT ABI's own callee-saved register
 *    list, not a configuration flag.
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
 * 4. Pinned objects (`turf_rt_pin`/`turf_rt_unpin`, OVH-010) -- precise:
 *    every block with an outstanding pin is marked directly, independent
 *    of whether anything above still points at it. This is the answer to
 *    F-MEM-3's own disclosed unsoundness (a GC pointer stored ONLY inside
 *    manual-heap memory is invisible to 1-3 above); see turfrt.h's own doc
 *    comment on turf_rt_pin for the full contract.
 * 5. Once conservative/precise roots mark a block, ITS OWN contents are
 *    traced PRECISELY via its TurfTypeDesc (an explicit worklist, not
 *    recursion -- a long `List<Node*>` chain must not blow the C stack).
 *
 * ─── Why a value found in a register/stack word during the FIRST pass
 * (the just-allocated block still only live in turf_rt_alloc's own local,
 * see turf_rt_collect()'s allocation-pressure ordering, turfrt.h) survives
 * correctly: by the target's own calling convention, calling a function
 * (turf_rt_collect()) obligates the CALLER to have already spilled to the
 * stack, or kept in a callee-saved register, anything it needs to survive
 * that call -- either way this file's own scan (stack range + callee-saved
 * registers) finds it. True of every ABI this file implements, not only
 * x86-64 SysV -- the argument only depends on "callee-saved register
 * contents survive an intervening call", which every one of them promises.
 */

#include "turfrt.h"
#include "turfrt_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ─── Platform dispatch (OVH-013) ───────────────────────────────────────
 * Two independent axes, each resolved once here and used by name (never by
 * re-testing the underlying macro) for the rest of the file:
 *   - TURF_GC_REGCAP_* picks gc_capture_callee_saved()'s register set/asm
 *     and TURF_GC_REGCAP_WORDS, its output size in `uintptr_t` words.
 *   - TURF_GC_STACKBASE_* picks turf_gc_record_stack_base()'s OS query.
 * A target this file has no branch for is a hard #error, not a silent
 * fallback -- an unported architecture must fail to BUILD, never build and
 * silently under-scan (a use-after-free, exactly OVH-001's failure mode).
 */
#if defined(_WIN32)
#define TURF_GC_STACKBASE_WIN32 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 /* Windows 8+: GetCurrentThreadStackLimits */
#endif
#include <windows.h>
#if defined(__x86_64__) || defined(_M_X64)
#define TURF_GC_REGCAP_WIN64 1
#define TURF_GC_REGCAP_WORDS 28 /* 8 GPRs + 10 xmm x 2 words each */
#else
#error "turfrt_gc.c's register-scanning path has no Windows implementation \
for this architecture (Win64/x86-64 only) -- see \
docs/planning/OVERHAUL_REMAINING_WORK.md OVH-013."
#endif
#elif defined(__APPLE__)
#define TURF_GC_STACKBASE_APPLE 1
#include <pthread.h>
#if defined(__aarch64__)
#define TURF_GC_REGCAP_AAPCS64 1
#define TURF_GC_REGCAP_WORDS 19 /* x19-x28, x29, d8-d15 */
#else
#error "turfrt_gc.c's register-scanning path has no macOS implementation \
for this architecture (arm64/AAPCS64 only) -- see \
docs/planning/OVERHAUL_REMAINING_WORK.md OVH-013."
#endif
#else
#define TURF_GC_STACKBASE_PROCFS 1
#if defined(__x86_64__)
#define TURF_GC_REGCAP_SYSV64 1
#define TURF_GC_REGCAP_WORDS 6 /* rbx, rbp, r12-r15 */
#elif defined(__aarch64__)
#define TURF_GC_REGCAP_AAPCS64 1
#define TURF_GC_REGCAP_WORDS 19 /* x19-x28, x29, d8-d15 */
#else
#error "turfrt_gc.c's register-scanning path (gc_capture_callee_saved) has \
no implementation for this architecture/OS combination -- porting notes in \
docs/planning/OVERHAUL_REMAINING_WORK.md's OVH-013."
#endif
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

/* OVH-010: pins, keyed by the CONTAINING BLOCK's start address (stable for
 * life -- the collector is non-moving, F-MEM-1), not by whatever pointer
 * value was actually passed to turf_rt_pin() -- an interior pointer pins
 * the whole block, matching how a stack/register root already treats one
 * (gc_find_containing/gc_consider_root below). `count` lets pins nest. */
typedef struct GcPin {
  uintptr_t block_start;
  size_t count;
} GcPin;

static GcPin *g_pins = NULL;
static size_t g_pin_count = 0, g_pin_cap = 0;

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
 * The fix: query the OS for the thread's REAL stack region, which is fixed
 * at thread-creation time and stays correct regardless of current call
 * depth. OVH-013 (docs/planning/OVERHAUL_REMAINING_WORK.md) added the
 * `TURF_GC_STACKBASE_APPLE`/`TURF_GC_STACKBASE_WIN32` branches below
 * alongside the original Linux `/proc/self/maps` query -- each queries the
 * one OS API that answers this same question on its own platform:
 * `pthread_get_stackaddr_np` (macOS) and `GetCurrentThreadStackLimits`
 * (Windows) are that platform's own stated way to get a thread's stack
 * bounds, the same relationship `/proc/self/maps`'s "[stack]" mapping has
 * to Linux.
 */
void turf_gc_record_stack_base(void) {
#if defined(TURF_GC_STACKBASE_APPLE)
  /* Highest address of the CURRENT thread's stack -- the stack grows down
   * from here, exactly the semantics `g_stack_base` needs (same convention
   * as the Linux "[stack]" mapping's high end, below). */
  g_stack_base = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#elif defined(TURF_GC_STACKBASE_WIN32)
  ULONG_PTR low_limit = 0, high_limit = 0;
  GetCurrentThreadStackLimits(&low_limit, &high_limit);
  /* HighLimit is the stack's base (its highest address, since the stack
   * grows down toward LowLimit) -- MSDN's own naming for exactly the value
   * this function returns everywhere else. */
  g_stack_base = (uintptr_t)high_limit;
#else
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
#endif
  if (g_stack_base == 0) {
    /* The platform query above found nothing (e.g. /proc unavailable in a
     * minimal container/sandbox without procfs) -- degrade to the
     * call-depth-dependent local-variable capture rather than scanning
     * nothing at all. Known pitfall, not silently accepted as equivalent:
     * see docs/development/changes/MEM-002.md. */
    int marker;
    g_stack_base = (uintptr_t)&marker;
  }
}

/*
 * FDN-006 stage 1 (docs/planning/ROADMAP_v2.0.0.md): turfrt_signal.c's
 * SIGSEGV handler needs `g_stack_base` (the high end of the stack this
 * function just computed) to tell a guard-page fault near the stack's LOW
 * end apart from an unrelated wild-pointer SIGSEGV elsewhere in the address
 * space. Exposed as an accessor, not an `extern` global, for the same
 * reason `turf_gc_snapshot_stats` is one (turfrt_internal.h's own doc
 * comment on it) -- this file stays the only one that knows the real
 * storage. Returns 0 if `turf_gc_record_stack_base()` has not run yet
 * (should be unreachable: turf_rt_init() calls both, in that order).
 */
uintptr_t turf_gc_get_stack_base(void) { return g_stack_base; }

static long find_pin(uintptr_t block_start) {
  for (size_t i = 0; i < g_pin_count; i++)
    if (g_pins[i].block_start == block_start)
      return (long)i;
  return -1;
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
  long pin_idx = -1;
  if (old_addr != 0) {
    /* OVH-010: a pin is keyed by the OLD block's start address -- if
     * realloc() moved it, carry the pin forward to the new address below,
     * rather than silently stranding it on a now-freed block (which would
     * make gc_consider_root's later lookup find nothing and the object
     * collect out from under an outstanding root(), or -- if the address
     * happened to be reused for something else entirely -- accidentally
     * pin the wrong object under the old key). */
    pin_idx = find_pin(old_addr);
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
  if (pin_idx >= 0) {
    g_pins[pin_idx].block_start = (uintptr_t)new_p;
  }
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

/* Containment lookup usable OUTSIDE a collection -- gc_find_containing()
 * above only searches the sorted g_blocks, and its own comment says it is
 * "only ever called during a collection, against the just-rebuilt, fully-
 * sorted array." turf_rt_pin()/turf_rt_unpin() can run at any time, so this
 * also linearly scans g_pending (small in practice: a block sits there only
 * until the next collection's gc_rebuild_index() merges it in). */
static GcBlock *gc_find_containing_any(uintptr_t addr) {
  GcBlock *b = gc_find_containing(addr);
  if (b)
    return b;
  for (size_t i = 0; i < g_pending_count; i++) {
    GcBlock *cand = &g_pending[i];
    if (addr >= cand->start && addr < cand->start + cand->size)
      return cand;
  }
  return NULL;
}

/* OVH-010: see turfrt.h's own doc comment on turf_rt_pin/turf_rt_unpin for
 * the full contract (interior pointers, nesting, the no-op/panic split). */
void turf_rt_pin(void *p) {
  if (!p)
    return;
  GcBlock *b = gc_find_containing_any((uintptr_t)p);
  if (!b)
    return; /* not a GC pointer at all -- documented no-op (F-MEM-3: C
              * memory is legitimately passed around). */
  long idx = find_pin(b->start);
  if (idx >= 0) {
    g_pins[idx].count++;
    return;
  }
  gc_arr_grow((void **)&g_pins, &g_pin_cap, sizeof(GcPin), g_pin_count + 1);
  g_pins[g_pin_count].block_start = b->start;
  g_pins[g_pin_count].count = 1;
  g_pin_count++;
}

void turf_rt_unpin(void *p) {
  if (!p)
    return;
  GcBlock *b = gc_find_containing_any((uintptr_t)p);
  if (!b)
    return; /* symmetric with turf_rt_pin's own no-op on non-GC memory */
  long idx = find_pin(b->start);
  if (idx < 0) {
    turf_rt_panic("unpin of unpinned object",
                  "mem::unroot() was called on an object with no matching "
                  "mem::root() pin outstanding",
                  "<mem::unroot>", 0);
  }
  if (--g_pins[idx].count == 0) {
    g_pins[idx] = g_pins[--g_pin_count];
  }
}

/* See this file's top comment: NOT setjmp-based, because glibc's jmp_buf
 * (and the Microsoft/Apple libc equivalents) pointer-mangle or otherwise
 * transform saved register values in ways that defeat treating them as
 * plain addresses. One branch per `TURF_GC_REGCAP_*` selected above --
 * each writes exactly that ABI's own callee-saved register set into
 * `regs[0 .. TURF_GC_REGCAP_WORDS)`, nothing more. */
__attribute__((noinline)) static void
gc_capture_callee_saved(uintptr_t regs[TURF_GC_REGCAP_WORDS]) {
#if defined(TURF_GC_REGCAP_SYSV64)
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
#elif defined(TURF_GC_REGCAP_WIN64)
  /* Win64's callee-saved set is SysV's PLUS rdi/rsi (volatile under SysV,
   * nonvolatile here) PLUS the full 128 bits of xmm6-xmm15 (SysV has no
   * callee-saved XMM registers at all) -- see this file's top comment. */
  __asm__ volatile("movq %%rbx, %0\n\t"
                    "movq %%rbp, %1\n\t"
                    "movq %%rdi, %2\n\t"
                    "movq %%rsi, %3\n\t"
                    "movq %%r12, %4\n\t"
                    "movq %%r13, %5\n\t"
                    "movq %%r14, %6\n\t"
                    "movq %%r15, %7\n\t"
                    : "=m"(regs[0]), "=m"(regs[1]), "=m"(regs[2]),
                      "=m"(regs[3]), "=m"(regs[4]), "=m"(regs[5]),
                      "=m"(regs[6]), "=m"(regs[7])
                    :
                    : "memory");
  /* `movq`/`movhps` extract an xmm register's low/high 64 bits -- both
   * baseline SSE1/SSE2 instructions guaranteed on any x86-64 CPU, chosen
   * over SSE4.1's `pextrq` specifically so this does not silently require
   * a newer CPU feature than Win64 itself already mandates.
   *
   * One `asm` statement PER register, not one statement for all ten --
   * found live via a MinGW cross-compile (`x86_64-w64-mingw32-gcc -O0`):
   * a single statement with all 20 memory outputs failed to assemble at
   * all ("asm operand has impossible constraints or there are not enough
   * registers") because GCC could not simultaneously solve the address
   * computation for 20 output operands at -O0's reduced register-allocator
   * freedom -- it built fine at -O2 (more freedom to share address
   * arithmetic), which would have shipped a debug-build-only compile
   * failure on Windows undetected by this project's own Release-only CI.
   * Splitting into ten independent two-operand statements removes the
   * register pressure entirely and compiles clean at every `-O` level. */
  __asm__ volatile("movq %%xmm6, %0\n\t" "movhps %%xmm6, %1\n\t"
                    : "=m"(regs[8]), "=m"(regs[9]) :: "memory");
  __asm__ volatile("movq %%xmm7, %0\n\t" "movhps %%xmm7, %1\n\t"
                    : "=m"(regs[10]), "=m"(regs[11]) :: "memory");
  __asm__ volatile("movq %%xmm8, %0\n\t" "movhps %%xmm8, %1\n\t"
                    : "=m"(regs[12]), "=m"(regs[13]) :: "memory");
  __asm__ volatile("movq %%xmm9, %0\n\t" "movhps %%xmm9, %1\n\t"
                    : "=m"(regs[14]), "=m"(regs[15]) :: "memory");
  __asm__ volatile("movq %%xmm10, %0\n\t" "movhps %%xmm10, %1\n\t"
                    : "=m"(regs[16]), "=m"(regs[17]) :: "memory");
  __asm__ volatile("movq %%xmm11, %0\n\t" "movhps %%xmm11, %1\n\t"
                    : "=m"(regs[18]), "=m"(regs[19]) :: "memory");
  __asm__ volatile("movq %%xmm12, %0\n\t" "movhps %%xmm12, %1\n\t"
                    : "=m"(regs[20]), "=m"(regs[21]) :: "memory");
  __asm__ volatile("movq %%xmm13, %0\n\t" "movhps %%xmm13, %1\n\t"
                    : "=m"(regs[22]), "=m"(regs[23]) :: "memory");
  __asm__ volatile("movq %%xmm14, %0\n\t" "movhps %%xmm14, %1\n\t"
                    : "=m"(regs[24]), "=m"(regs[25]) :: "memory");
  __asm__ volatile("movq %%xmm15, %0\n\t" "movhps %%xmm15, %1\n\t"
                    : "=m"(regs[26]), "=m"(regs[27]) :: "memory");
#elif defined(TURF_GC_REGCAP_AAPCS64)
  __asm__ volatile("str x19, %0\n\t"
                    "str x20, %1\n\t"
                    "str x21, %2\n\t"
                    "str x22, %3\n\t"
                    "str x23, %4\n\t"
                    "str x24, %5\n\t"
                    "str x25, %6\n\t"
                    "str x26, %7\n\t"
                    "str x27, %8\n\t"
                    "str x28, %9\n\t"
                    "str x29, %10\n\t"
                    : "=m"(regs[0]), "=m"(regs[1]), "=m"(regs[2]),
                      "=m"(regs[3]), "=m"(regs[4]), "=m"(regs[5]),
                      "=m"(regs[6]), "=m"(regs[7]), "=m"(regs[8]),
                      "=m"(regs[9]), "=m"(regs[10])
                    :
                    : "memory");
  /* AAPCS64 only requires the LOW 64 bits of v8-v15 preserved (the "d"
   * register view), not the full 128-bit "q"/"v" view -- storing more
   * would read bits the callee is free to have clobbered. */
  __asm__ volatile("str d8,  %0\n\t"
                    "str d9,  %1\n\t"
                    "str d10, %2\n\t"
                    "str d11, %3\n\t"
                    "str d12, %4\n\t"
                    "str d13, %5\n\t"
                    "str d14, %6\n\t"
                    "str d15, %7\n\t"
                    : "=m"(regs[11]), "=m"(regs[12]), "=m"(regs[13]),
                      "=m"(regs[14]), "=m"(regs[15]), "=m"(regs[16]),
                      "=m"(regs[17]), "=m"(regs[18])
                    :
                    : "memory");
#endif
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

  uintptr_t regs[TURF_GC_REGCAP_WORDS];
  gc_capture_callee_saved(regs);
  for (int i = 0; i < TURF_GC_REGCAP_WORDS; i++)
    gc_consider_root(regs[i], &wl);

  int marker;
  uintptr_t sp = (uintptr_t)&marker;
  if (g_stack_base > sp)
    gc_scan_range_conservative(sp, g_stack_base, &wl);

  for (size_t i = 0; i < g_root_count; i++) {
    void **slot = (void **)g_roots[i];
    gc_consider_root((uintptr_t)*slot, &wl);
  }

  /* OVH-010: every pinned object is an always-retained root, regardless of
   * whether anything on the stack/in a register/in global storage still
   * points at it -- that is the entire point of mem::root(), a GC object
   * reachable only through manual-heap memory (invisible to every scan
   * above) staying alive anyway. g_blocks is already rebuilt at this point
   * (gc_rebuild_index() above), so every pin's block_start is guaranteed to
   * resolve here even if the pin was placed before this cycle's merge. */
  for (size_t i = 0; i < g_pin_count; i++) {
    gc_consider_root(g_pins[i].block_start, &wl);
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
