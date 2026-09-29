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

/*
 * FDN-005 stage 3 (docs/planning/ROADMAP_v2.0.0.md): must be defined before
 * ANY system header is included in this translation unit -- glibc's
 * <features.h> latches __USE_GNU (and everything gated on it, including
 * <ucontext.h>'s REG_RBX-style register-index macros gc_cross_validate_
 * marks() below needs) the FIRST time it is pulled in, via its own header
 * guard; a later #define _GNU_SOURCE right before #include <ucontext.h>
 * is too late once any earlier header in this file has already dragged
 * <features.h> in without it. Confirmed live: REG_RBX is undeclared under
 * plain -std=gnu11 without this. Guarded to Linux only (the only platform
 * this specific validation mode supports -- see TURF_GC_VALIDATE_SUPPORTED
 * below) so it never affects the macOS/Windows builds of this same file.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "turfrt.h"
#include "turfrt_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * FDN-005 stage 3 (docs/planning/ROADMAP_v2.0.0.md): only defined when this
 * translation unit is itself compiled with -fsanitize=address (CMakeLists.txt's
 * `turfrt_asan` target). See gc_scan_range_conservative()'s own comment,
 * below, for why the conservative stack scan specifically needs to know
 * this -- found live the first time this collector ever ran under ASan
 * (the whole point of this stage): a bare, unguarded stack-buffer-overflow
 * abort on every single collection, not a real bug.
 */
#if defined(__SANITIZE_ADDRESS__)
#define TURF_GC_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TURF_GC_ASAN 1
#endif
#endif
#ifdef TURF_GC_ASAN
#include <sanitizer/asan_interface.h>
#endif

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

/*
 * FDN-005 stage 3 (docs/planning/ROADMAP_v2.0.0.md): MEM-002's own
 * disclosed open question #2 (docs/development/changes/MEM-002.md,
 * *Unresolved questions*) -- "the register-scanning technique (inline
 * asm, not setjmp) has not been cross-validated against an independent
 * conservative-GC implementation." gc_cross_check_extra_registers() (below)
 * answers this with a SECOND, independently-implemented root capture:
 * glibc's own getcontext() (<ucontext.h>), which snapshots the FULL
 * general-purpose register file via the kernel's own signal-frame
 * machinery -- a completely different code path than this file's
 * hand-written inline asm, covering every GPR (caller-saved included),
 * not just the callee-saved subset the primary scan relies on. Comparing
 * the two passes' mark sets turns "the primary technique is modeled on
 * Boehm GC's own reasoning" into "the primary technique's own output was
 * checked against an independently-obtained register snapshot, on real
 * runs, not just argued about."
 *
 * Scoped to Linux/glibc/x86-64 ONLY -- __GLIBC__ is not yet defined at
 * this point in the file (it comes from <features.h>, pulled in by the
 * very #include <ucontext.h> this guard controls), so it is checked
 * again, after the include, before any of gc_cross_check_extra_registers()'s own
 * code is compiled. musl (a common non-glibc Linux libc) does not shape
 * ucontext_t/mcontext_t the same way at all; macOS's own ucontext.h is
 * formally deprecated and shapes registers differently again; Win64 has
 * no getcontext() at all. Each is a real, separate port, not a
 * configuration flag -- exactly the same honesty this file's own
 * TURF_GC_REGCAP_.../TURF_GC_STACKBASE_... dispatch already applies (a target
 * with no branch here simply does not get this EXTRA validation, unlike
 * the PRIMARY scan, which hard-#errors on an unported target -- this
 * stage is additional, opt-in scrutiny, never a load-bearing part of an
 * ordinary collection, so degrading to "unavailable" on an unported
 * platform is correct, not a silent under-scan the way it would be for
 * the primary path).
 */
#if defined(TURF_GC_REGCAP_SYSV64) && defined(__linux__)
#include <ucontext.h>
#if defined(__GLIBC__)
#define TURF_GC_VALIDATE_SUPPORTED 1
#include <pthread.h>
/* Weak so libturfrt adds no link-time dependency for a testing-only path:
 * before glibc 2.34 these lived in libpthread, which linkObject() does not
 * pass. Unresolved, they are NULL and the mark-set comparison reports
 * itself unavailable instead of failing every Turf program's link. */
#pragma weak pthread_getattr_np
#pragma weak pthread_attr_getstack
#pragma weak pthread_attr_destroy
#endif
#endif

/*
 * FDN-005 stage 3 (docs/planning/ROADMAP_v2.0.0.md): "the address of a
 * local variable in the current function" (this file's own top comment
 * calls it "current SP") is, under ASan's default
 * `detect_stack_use_after_return=1`, NOT guaranteed to be a real machine
 * stack address at all -- ASan's fake-stack instrumentation may relocate
 * an ordinary local's storage onto a HEAP-allocated "fake frame"
 * specifically so it can detect a pointer to it outliving its function
 * (the exact feature the name describes). Found live, the first time
 * gc_mark()'s own SP approximation ran under ASan: `gc_scan_range_
 * conservative()` was handed a genuine heap address as its LOWER bound
 * and a real stack address as its upper bound, producing a scan range
 * that walked mostly-unmapped memory and SEGV'd almost immediately --
 * not the redzone false positive `__asan_region_is_poisoned` (below)
 * already handles, a completely different failure mode.
 *
 * The fix is this tiny, `noinline`, ASan-EXEMPTED helper -- exempted from
 * instrumentation ONLY here, in a function whose entire body is "take a
 * local's address and return it", specifically so the compiler's
 * fake-stack pass never gets the chance to relocate `marker`. Scoping the
 * exemption to this one trivial function (rather than exempting
 * `gc_mark()` itself, which was the first fix attempted) matters: `gc_
 * mark()` also does real GC-heap block-table reads/writes that should
 * stay fully ASan-checked, and a plain "return &local" body has no memory
 * access worth checking anyway. gc_mark()'s "current SP" is the only
 * caller: turf_gc_record_stack_base()'s /proc-unavailable fallback used to
 * be the second, and was removed because a local's address there is not an
 * ancestor of the user's main() (see that function).
 */
__attribute__((noinline))
#ifdef TURF_GC_ASAN
__attribute__((no_sanitize("address")))
#endif
static uintptr_t
gc_approx_sp(void) {
  int marker;
  return (uintptr_t)&marker;
}

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
#if defined(TURF_GC_STACKBASE_PROCFS)
#if defined(__GLIBC__)
/* Not in any public header, but a stable exported glibc symbol
 * (GLIBC_2.1): the stack pointer at process entry, recorded by the
 * dynamic loader / static startup code before `main` -- or anything that
 * calls it -- exists. It is glibc's own answer to this question: its
 * pthread_getattr_np() uses it to find the main thread's stack mapping. */
extern void *__libc_stack_end;
#else
#include <pthread.h>
#endif

/*
 * turf_gc_record_stack_base()'s answer when /proc/self/maps is unavailable
 * (a container or sandbox without procfs). Every source here is an ANCESTOR
 * of every Turf frame -- the property the old local-address fallback lacked.
 * Returns 0 if the query fails; the caller panics then.
 */
static uintptr_t gc_stack_base_without_procfs(void) {
#if defined(__GLIBC__)
  return (uintptr_t)__libc_stack_end;
#else
  /* musl (and bionic) answer this for the main thread without procfs (it probes the
   * mapping with mremap), unlike glibc, which reads /proc/self/maps --
   * hence glibc takes the branch above instead. */
  pthread_attr_t attr;
  void *addr = NULL;
  size_t size = 0;
  if (pthread_getattr_np(pthread_self(), &attr) != 0)
    return 0;
  int ok = pthread_attr_getstack(&attr, &addr, &size) == 0;
  pthread_attr_destroy(&attr);
  return ok ? (uintptr_t)addr + size : 0;
#endif
}
#endif

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
  /* TURF_GC_NO_PROCFS=1 (a testing aid, like TURF_GC_STRESS) skips this
   * file's own /proc read so the fallback chain below can be exercised on a
   * machine that does have procfs -- ROADMAP_v2.0.0.md REL-003 requires the
   * no-/proc path be tested, and a minimal container is the only place it
   * runs for real. */
  const char *no_procfs = getenv("TURF_GC_NO_PROCFS");
  FILE *f = (no_procfs != NULL && no_procfs[0] != '\0' &&
             strcmp(no_procfs, "0") != 0)
                ? NULL
                : fopen("/proc/self/maps", "r");
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
  if (g_stack_base == 0)
    g_stack_base = gc_stack_base_without_procfs();
#endif
  if (g_stack_base == 0) {
    /* FDN-005: this used to degrade to gc_approx_sp() -- a local's address
     * in THIS frame, which is exactly the sibling-frame capture the doc
     * comment above explains is wrong. Measured with TURF_GC_NO_PROCFS=1
     * under TURF_GC_STRESS=1: 11 of 12 tests/41_gc programs freed live
     * objects (asserts, SIGSEGV, glibc heap-corruption aborts) at both -O0
     * and -O2. A collector that silently frees live data is worse than a
     * program that refuses to start, so there is no under-scanning
     * fallback any more. */
    turf_rt_panic("cannot locate the stack",
                  "the garbage collector could not determine this thread's "
                  "stack bounds on this platform",
                  "<runtime>", 0);
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
 * correctness (§14's own accepted tradeoff).
 *
 * FDN-005 stage 3: under ASan, [lo, hi) legitimately contains bytes ASan
 * has poisoned -- the redzones it inserts around EVERY stack-allocated
 * local/array in every frame between the true stack top and `hi` (a
 * conservative scan walks frames that have already returned, on purpose;
 * that's the entire technique). Found live the first time this collector
 * ran under ASan at all: an unconditional read of every word in range
 * aborted immediately on the very first collection, on a redzone byte
 * belonging to `gc_mark()`'s own `marker` local, not a real bug. A
 * poisoned byte is, by construction, never part of any Turf-visible
 * value (it's padding ASan itself inserted, not memory this collector
 * would ever need to trace a real pointer through), so skipping a
 * poisoned word changes nothing about correctness -- it only stops this
 * one, specific, well-known conservative-GC/ASan interaction from firing.
 * Deliberately scoped to JUST this function: gc_scan_block_precise()'s
 * own heap reads are NOT given this treatment, so a genuine
 * freed-block-reuse bug reached through PRECISE tracing (the actual bug
 * class this stage's ASan build exists to catch) still aborts normally. */
static void gc_scan_range_conservative(uintptr_t lo, uintptr_t hi,
                                        Worklist *wl) {
  lo &= ~((uintptr_t)(sizeof(uintptr_t) - 1));
  for (uintptr_t a = lo; a + sizeof(uintptr_t) <= hi;
       a += sizeof(uintptr_t)) {
#ifdef TURF_GC_ASAN
    if (__asan_region_is_poisoned((void *)a, sizeof(uintptr_t)))
      continue;
#endif
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

#ifdef TURF_GC_VALIDATE_SUPPORTED
/*
 * FDN-005 stage 3: independent register capture via glibc's getcontext()
 * -- a completely different mechanism than gc_capture_callee_saved()'s
 * hand-written inline asm (the kernel fills a ucontext_t's mcontext_t
 * during signal delivery; getcontext() is glibc's own wrapper around that
 * same machinery, not code this file wrote). Deliberately captures the
 * FULL general-purpose register file, not just the SysV64 callee-saved
 * subset gc_capture_callee_saved() reads; RSP is omitted (the stack-range
 * scan already covers it by definition) and RIP is omitted (a code
 * address, never a heap pointer).
 *
 * Must be called from the SAME instant as gc_capture_callee_saved() --
 * gc_mark() calls the two back-to-back with nothing in between (after
 * gc_rebuild_index(), which is fine: only the adjacency matters). This was NOT the first version of this function: an
 * earlier draft called this from a SEPARATE helper invoked AFTER the
 * primary mark/trace pass had already completed, and treated any block
 * the primary pass marked but this one didn't as a hard error. That
 * version false-positived on almost every test in tests/41_gc/, for a
 * reason that took a live repro to see: by the time the primary pass
 * finishes (gc_rebuild_index(), the full worklist trace, several mallocs
 * for the worklist array), MANY function calls have run since
 * gc_capture_callee_saved()'s own read -- every caller-saved register
 * gc_capture_callee_saved() never even looks at could have been (and
 * generally was) overwritten many times over by that unrelated
 * intervening work, so a LATER getcontext() call is not reading "the same
 * moment" at all, just whatever scratch data the most recent internal
 * computation happened to leave behind. Comparing against literally
 * unrelated register garbage is not a validation of anything -- hence
 * capturing both at the same instant, before either pass has run.
 */
#define TURF_GC_VALIDATE_WORDS 15
static size_t
gc_capture_independent_regs(uintptr_t out[TURF_GC_VALIDATE_WORDS]) {
  ucontext_t uc;
  if (getcontext(&uc) != 0)
    return 0;
  const int regnums[TURF_GC_VALIDATE_WORDS] = {
      REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RDI, REG_RSI, REG_RBP,
      REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14,
      REG_R15,
  };
  for (size_t i = 0; i < TURF_GC_VALIDATE_WORDS; i++)
    out[i] = (uintptr_t)uc.uc_mcontext.gregs[regnums[i]];
  return TURF_GC_VALIDATE_WORDS;
}

/*
 * Indices into gc_capture_independent_regs()'s own regnums[] table (just
 * above) that duplicate gc_capture_callee_saved()'s SysV64 set (rbx, rbp,
 * r12-r15) -- skipped here since the primary pass already traces them;
 * checking them again would only ever produce a guaranteed, uninformative
 * match. What's left (rax, rcx, rdx, rdi, rsi, r8-r11 -- 9 registers) is
 * the genuinely NEW information this independent capture adds: every one
 * of them is caller-saved under SysV64, meaning nothing in this file's
 * own design ever claims they survive a call, and the primary scan never
 * looks at them at all.
 */
static const int gc_validate_extra_idx[] = {0, 2, 3, 4, 5, 7, 8, 9, 10};
#define TURF_GC_VALIDATE_EXTRA_COUNT \
  (sizeof(gc_validate_extra_idx) / sizeof(gc_validate_extra_idx[0]))

static uint64_t g_gc_validate_extra_findings = 0;

/*
 * Called AFTER the production trace has finished (so `marked` reflects
 * everything the real collection considers reachable) with the register
 * snapshot captured at the SAME instant as the primary capture. Reports
 * (does not panic, does not mark, does not affect what gets swept) any
 * caller-saved register that happens to point inside a block the
 * production scan did NOT otherwise retain.
 *
 * Deliberately non-fatal, unlike the ASan build's own detections: a
 * caller-saved register's content, at any given instant, is not
 * guaranteed to be a live Turf-level value at all -- the C ABI places no
 * obligation on what a caller-saved register holds across a call, so it
 * may easily contain unrelated leftover computation that HAPPENS to
 * alias a live block's address range (the exact same "over-scanning
 * costs false-retention risk, not correctness" tradeoff this file's own
 * top comment already accepts for the primary scan, just applied to a
 * register range the primary scan intentionally excludes). A finding
 * here is evidence worth collecting across many runs, not, on its own,
 * proof that the primary scan missed a real root -- turning it into a
 * hard abort would make TURF_GC_VALIDATE flaky in CI over pure coincidence,
 * exactly the failure mode this stage exists to avoid introducing.
 */
static void
gc_cross_check_extra_registers(const uintptr_t indep_regs[TURF_GC_VALIDATE_WORDS]) {
  for (size_t k = 0; k < TURF_GC_VALIDATE_EXTRA_COUNT; k++) {
    uintptr_t candidate = indep_regs[gc_validate_extra_idx[k]];
    GcBlock *b = gc_find_containing(candidate);
    if (b && !b->marked) {
      g_gc_validate_extra_findings++;
      fprintf(stderr,
              "TURF_GC_VALIDATE: a caller-saved register (outside "
              "gc_capture_callee_saved()'s own set) pointed at a block "
              "(start=%p size=%zu) the production scan did not otherwise "
              "mark live -- likely coincidental stale register content "
              "(an accepted conservative-scanning risk), not necessarily "
              "a missed root; see docs/development/changes/FDN-005.md. "
              "(finding #%llu this process)\n",
              (void *)b->start, b->size,
              (unsigned long long)g_gc_validate_extra_findings);
    }
  }
}

/*
 * FDN-005 stage 3, second half -- ROADMAP_v2.0.0.md asks for a mode that
 * "scans the full stack range conservatively *and* compares mark sets".
 * The register check above cannot catch a wrong STACK BOUND, which is the
 * failure mode that actually shipped: the pre-FDN-005 no-procfs fallback
 * in turf_gc_record_stack_base() under-scanned and freed live objects in
 * nearly every tests/41_gc program. This pass derives every conservative
 * root from sources the primary pass never touches:
 *   - registers: getcontext() (not gc_capture_callee_saved()'s inline asm),
 *     taken in turf_rt_collect(), an ANCESTOR frame of gc_mark();
 *   - stack low end: that snapshot's own RSP (not gc_approx_sp());
 *   - stack high end: glibc's pthread_getattr_np() (not g_stack_base),
 *     i.e. the whole [stack] mapping.
 *
 * Why a block marked here but not by the primary pass is a real missed
 * root, never noise -- so it panics rather than reports:
 *   - [snapshot RSP, top) is a subset of the primary's [approx SP, base)
 *     whenever the primary's base is right (approx SP lies in a frame
 *     below gc_mark(), the snapshot RSP in turf_rt_collect(), above it),
 *     and nothing writes to turf_rt_collect()'s frame or its ancestors'
 *     between the two scans -- both see the same words;
 *   - only the CALLEE-saved registers of the snapshot are used; any value
 *     they held was either still in the register at the primary capture or
 *     spilled by an intervening prologue into a frame the primary scans.
 * The caller-saved registers are deliberately left to the report-only
 * check above: their contents at a call boundary are not roots at all.
 */
static ucontext_t g_validate_ctx;
static int g_validate_ctx_valid = 0;
static uintptr_t g_validate_stack_top = 0;

/* Called once, the first time TURF_GC_VALIDATE is seen set. */
static void gc_validate_init_stack_top(void) {
  if (!pthread_getattr_np || !pthread_attr_getstack || !pthread_attr_destroy)
    return;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0)
    return;
  void *addr = NULL;
  size_t size = 0;
  if (pthread_attr_getstack(&attr, &addr, &size) == 0)
    g_validate_stack_top = (uintptr_t)addr + size;
  pthread_attr_destroy(&attr);
}

/*
 * Runs the whole independent mark (roots + precise trace) using the
 * blocks' own `marked` bits, copies the result out, and clears the bits
 * again so the primary pass starts from the same all-clear state it
 * always does. Returns a malloc'd byte per g_blocks entry, or NULL if no
 * snapshot/stack top is available for this collection.
 */
static unsigned char *gc_independent_mark(void) {
  if (!g_validate_ctx_valid || g_validate_stack_top == 0)
    return NULL;
  const greg_t *g = g_validate_ctx.uc_mcontext.gregs;
  static const int callee_saved[] = {REG_RBX, REG_RBP, REG_R12,
                                     REG_R13, REG_R14, REG_R15};
  Worklist wl = {0};
  for (size_t i = 0; i < sizeof(callee_saved) / sizeof(callee_saved[0]); i++)
    gc_consider_root((uintptr_t)g[callee_saved[i]], &wl);
  uintptr_t lo = (uintptr_t)g[REG_RSP];
  if (g_validate_stack_top > lo)
    gc_scan_range_conservative(lo, g_validate_stack_top, &wl);
  for (size_t i = 0; i < g_root_count; i++) {
    void **slot = (void **)g_roots[i];
    gc_consider_root((uintptr_t)*slot, &wl);
  }
  for (size_t i = 0; i < g_pin_count; i++)
    gc_consider_root(g_pins[i].block_start, &wl);
  while (wl.count > 0)
    gc_scan_block_precise(wl.items[--wl.count], &wl);
  free(wl.items);

  unsigned char *marks = malloc(g_block_count ? g_block_count : 1);
  if (!marks)
    turf_rt_panic("out of memory", "<gc validate mark set>", "<turfrt_gc.c>",
                  0);
  for (size_t i = 0; i < g_block_count; i++) {
    marks[i] = g_blocks[i].marked;
    g_blocks[i].marked = 0;
  }
  return marks;
}

/* After the primary trace: every block the independent pass reached must
 * also be marked by the primary one. Lists the first 16 misses and the
 * total, then panics -- sweeping would free a live object. */
static void gc_compare_mark_sets(const unsigned char *indep) {
  size_t missed = 0;
  for (size_t i = 0; i < g_block_count; i++) {
    if (indep[i] && !g_blocks[i].marked) {
      if (missed < 16)
        fprintf(stderr,
                "TURF_GC_VALIDATE: mark-set mismatch -- block start=%p "
                "size=%zu is reachable from the independent full-stack "
                "scan but the production scan did not mark it\n",
                (void *)g_blocks[i].start, g_blocks[i].size);
      missed++;
    }
  }
  if (missed) {
    fprintf(stderr,
            "TURF_GC_VALIDATE: %zu missed block(s) at collection #%llu "
            "(production stack base %p, independent stack top %p)\n",
            missed, (unsigned long long)g_collect_count + 1,
            (void *)g_stack_base, (void *)g_validate_stack_top);
    turf_rt_panic("GC root missed",
                  "TURF_GC_VALIDATE: the production root scan missed a "
                  "block the independent scan reaches -- sweeping would "
                  "free a live object",
                  "<runtime>", 0);
  }
}
#endif /* TURF_GC_VALIDATE_SUPPORTED */

/*
 * FDN-005 stage 3: a testing aid in exactly the same shape as
 * TURF_GC_STRESS (turf_gc_note_alloc(), below) -- opt-in, read once and
 * cached. On a platform this file has no independent-capture
 * implementation for, setting this prints a one-time notice and is
 * otherwise a no-op (never a silent "looked like it worked" -- the same
 * honesty TURF_GC_REGCAP_*'s hard #error gives the PRIMARY scan; this is
 * the OPTIONAL extra-scrutiny path, so degrading to "unavailable here"
 * rather than failing to build is the correct choice for it specifically,
 * not a double standard).
 */
static int g_gc_validate = -1;

static int gc_validate_enabled(void) {
  if (g_gc_validate < 0) {
    const char *e = getenv("TURF_GC_VALIDATE");
    g_gc_validate = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
#ifndef TURF_GC_VALIDATE_SUPPORTED
    if (g_gc_validate) {
      fprintf(stderr,
              "TURF_GC_VALIDATE=1 was set, but this build has no "
              "independent register-capture cross-validation for this "
              "platform (Linux/glibc/x86-64 only, FDN-005 stage 3) -- "
              "ignoring.\n");
      g_gc_validate = 0;
    }
#else
    if (g_gc_validate) {
      gc_validate_init_stack_top();
      if (g_validate_stack_top == 0)
        fprintf(stderr,
                "TURF_GC_VALIDATE=1: pthread_getattr_np() is unavailable, "
                "so the full-stack mark-set comparison is skipped; only the "
                "register cross-check runs.\n");
    }
#endif
  }
  return g_gc_validate;
}

/* noinline: gc_independent_mark()'s no-false-positive argument needs
 * gc_mark()'s frame to sit strictly BELOW turf_rt_collect()'s getcontext()
 * snapshot RSP -- inlined, gc_mark()'s own locals would share that frame
 * and change between the independent scan and the primary one. */
__attribute__((noinline)) static void gc_mark(void) {
  gc_rebuild_index();
  for (size_t i = 0; i < g_block_count; i++)
    g_blocks[i].marked = 0;

#ifdef TURF_GC_VALIDATE_SUPPORTED
  unsigned char *indep_marks =
      gc_validate_enabled() ? gc_independent_mark() : NULL;
#endif

  Worklist wl = {0};

  uintptr_t regs[TURF_GC_REGCAP_WORDS];
  gc_capture_callee_saved(regs);

  /* FDN-005 stage 3: the report-only caller-saved check's snapshot, taken
   * immediately after the primary register read with nothing in between
   * (gc_rebuild_index() and the independent mark above both run BEFORE
   * the pair, never between them) -- see gc_capture_independent_regs()'s
   * own comment for why the two must be adjacent. `do_validate`/
   * `indep_regs` do not exist when this platform has no implementation. */
#ifdef TURF_GC_VALIDATE_SUPPORTED
  int do_validate = gc_validate_enabled();
  uintptr_t indep_regs[TURF_GC_VALIDATE_WORDS];
  if (do_validate)
    (void)gc_capture_independent_regs(indep_regs);
#else
  (void)gc_validate_enabled(); /* still surfaces the one-time notice */
#endif

  for (int i = 0; i < TURF_GC_REGCAP_WORDS; i++)
    gc_consider_root(regs[i], &wl);

  uintptr_t sp = gc_approx_sp();
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

#ifdef TURF_GC_VALIDATE_SUPPORTED
  /* Checked AFTER the trace above completes, so `marked` reflects full
   * production reachability -- see gc_cross_check_extra_registers()'s own
   * comment for why this reports rather than aborts. */
  if (do_validate)
    gc_cross_check_extra_registers(indep_regs);
  if (indep_marks) {
    gc_compare_mark_sets(indep_marks);
    free(indep_marks);
  }
#endif
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
#ifdef TURF_GC_VALIDATE_SUPPORTED
  /* The independent mark's snapshot -- taken here, in gc_mark()'s caller,
   * so its RSP bounds a range that excludes every collector frame below
   * (gc_independent_mark()'s own comment). */
  g_validate_ctx_valid =
      gc_validate_enabled() && getcontext(&g_validate_ctx) == 0;
#endif
  gc_mark();
  gc_sweep();
  g_bytes_since_collect = 0;
  g_collect_count++;
}

/*
 * FDN-005 stage 2 (docs/planning/ROADMAP_v2.0.0.md): a testing aid, exactly
 * like TURF_RT_STATS_DUMP (runtime/turfrt_alloc.c) -- opt-in, read once and
 * cached (this is checked on every single allocation, unlike the
 * shutdown-only stats dump, so a fresh getenv() per call is not acceptable
 * overhead even for the common case where it's unset), never changes a
 * program's observable OUTPUT, only how often it collects. Forcing a
 * collection on every allocation is the single most effective way to
 * surface a "would-be-collected-too-early" bug: the heap-doubling
 * heuristic's own threshold means most real programs only collect a
 * handful of times over their whole run, so a root the primary scan barely
 * misses (a narrow window between an allocation and the value being
 * spilled somewhere a conservative scan would find it) can go unnoticed
 * for a very long time under ordinary allocation pressure and get caught
 * immediately once every single allocation is itself a collection point.
 */
static int g_gc_stress = -1; /* -1 = not yet queried */

static int gc_stress_enabled(void) {
  if (g_gc_stress < 0) {
    const char *e = getenv("TURF_GC_STRESS");
    g_gc_stress = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return g_gc_stress;
}

void turf_gc_note_alloc(size_t bytes) {
  g_bytes_since_collect += bytes;
  if (gc_stress_enabled() || g_bytes_since_collect >= g_gc_threshold) {
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
