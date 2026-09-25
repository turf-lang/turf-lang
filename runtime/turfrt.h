/*
 * runtime/turfrt.h -- libturfrt's public C ABI.
 *
 * RT-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8, F-MEM-4, F-STD-2). This
 * is the ONE header both the compiler (which emits calls to these symbols
 * and TurfTypeDesc constants, never includes this file directly -- Codegen
 * builds the matching LLVM types by hand, see src/CodegenStruct.cpp's
 * getOrCreateTurfTypeDescType()) and libturfrt's own .c files agree on.
 *
 * F-STD-2 (FREEZE.md §6.8): the intrinsic/builtin list is closed and
 * enumerated in one place -- include/Intrinsics.h names every symbol
 * declared here as part of that list. Adding a function to this header
 * without also adding it there is incomplete.
 *
 * Per RT-001's "Must NOT reinterpret": this is C, not TurfLang, and it is
 * not a place to move stdlib functionality into (F-STD-3, Rule B3 --
 * ARCHITECTURE_OVERHAUL.md §3.3/§15). lib/std/mem/mem.tr's malloc/free
 * bindings are untouched by this file and stay on the manual heap
 * (F-MEM-3) -- none of these functions are reachable from ordinary Turf
 * source AS extern fn bindings (Rule B2/B3); MEM-001 (the group that added
 * turf_rt_alloc_zeroed/turf_rt_realloc/turf_rt_stats alongside RT-001's
 * original turf_rt_alloc) instead reaches this file's alloc functions via
 * three new bare LANGUAGE intrinsics -- gc_alloc/gc_alloc_zeroed/
 * gc_realloc, category 1 in include/Intrinsics.h, same mechanism as
 * `sizeof`/`typeof` -- never a module-qualified call.
 */

#ifndef TURFRT_H
#define TURFRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TurfTypeDesc -- one per monomorphized aggregate (TYPE-002 made every
 * struct instantiation a real, distinct LLVM type; this is its runtime
 * shadow). Emitted as a constant global by CodegenStruct.cpp's
 * StructDeclExprAST::codegen(), immediately after the LLVM StructType's
 * setBody() call -- see that function's own comment for why the emitted
 * LLVM struct layout ({i64, ptr, i64}) must match this C struct's layout
 * field-for-field.
 *
 * In THIS group nothing ever reads `pointer_word_bitmap` -- MEM-002's
 * collector is what traces it. Kept here now (rather than deferred to
 * MEM-002) because emitting it while TYPE-002's per-instantiation layouts
 * are still fresh is far cheaper than retrofitting it once code depends on
 * struct layout elsewhere (ARCHITECTURE_OVERHAUL.md §8 RT-001, point 3).
 *
 * `pointer_word_bitmap` covers only the first 64 eight-byte words (512
 * bytes) of a type's layout -- bit i set means "byte range [8i, 8i+8) may
 * hold a pointer" (conservative: it marks every pointer-SHAPED word, not
 * only ones proven to point into the GC heap specifically, since nothing
 * before MEM-001/MEM-002 tags a pointer field with its heap of origin).
 * A struct whose alloc size exceeds 512 bytes has bits 64+ silently
 * unrepresented -- a known, documented limitation for MEM-002 to widen if
 * it ever becomes a real constraint (see the RT-001 change record's Known
 * pitfalls). No TurfLang struct in tests/, lib/std/, or showcase/ is
 * anywhere near this size as of this writing.
 */
typedef struct TurfTypeDesc {
  uint64_t size;                /* allocation size in bytes (DataLayout::getTypeAllocSize) */
  const char *name;             /* the struct's Turf-level name; static storage, never freed */
  uint64_t pointer_word_bitmap; /* bit i set => word i (bytes [8i, 8i+8)) may hold a pointer */
} TurfTypeDesc;

/*
 * turf_rt_init / turf_rt_shutdown -- process bootstrap, called from the
 * compiler-emitted wrapper Codegen builds around the user's `main` (see
 * src/CodegenFunc.cpp's WrapMainWithRuntimeBootstrap(), invoked from
 * main.cpp right after the mandatory pass confirms a real entry point
 * exists). Every linked Turf binary calls turf_rt_init() before a single
 * line of Turf-level `main` body runs, and turf_rt_shutdown() after it
 * returns -- including on every path (RT-001 does not special-case abrupt
 * termination via turf_rt_panic()'s abort(): a panic is not something a
 * program is expected to recover from and shut down cleanly after, exactly
 * like sys::panic()'s own abort() in lib/std/io/sys.tr).
 *
 * In this group both are effectively no-ops (there is no heap to stand up
 * yet -- MEM-002 gives turf_rt_init() a real block table to initialize).
 * They exist now so every compiled binary already calls through the real
 * bootstrap hook, and so MEM-002 never needs a second compiler-side change
 * to start calling into it.
 */
void turf_rt_init(void);
void turf_rt_shutdown(void);

/*
 * turf_rt_alloc -- the typed allocation entrypoint (F-MEM-4). In this group
 * it is a thin, fail-fast malloc wrapper that records nothing about `desc`
 * beyond using it for a panic message on OOM -- MEM-002 gives it a real
 * heap. `desc` may be NULL (an untyped/raw allocation has no descriptor);
 * callers that always have one should still pass it so OOM diagnostics
 * name the type being allocated.
 *
 * As of MEM-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8), this is called
 * from Codegen for the Turf-level `gc_alloc(n)` bare intrinsic
 * (FuncCallExprAST::codegen(), src/CodegenFunc.cpp) -- every GC-heap-bound
 * allocation classified in `docs/development/changes/MEM-001.md` (string
 * results, collection backing buffers) reaches this function with `desc`
 * always NULL: none of them are single instances of a monomorphized
 * aggregate the way EmitAllTurfTypeDescs()'s per-struct descriptors are
 * (a string is a raw byte run; a `List<T>` backing buffer is N elements,
 * not one), so there is no existing TurfTypeDesc that correctly describes
 * "N u8s" or "N Ts" -- an array-shaped descriptor is explicitly MEM-002
 * territory (RT-001's own `pointer_word_bitmap` doc comment), not
 * invented speculatively here. `desc == NULL` degrades OOM diagnostics to
 * "<raw>" (see turf_rt_alloc's own body) but is otherwise fully correct.
 */
void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc);

/*
 * turf_rt_alloc_zeroed -- like turf_rt_alloc, but the returned block is
 * zero-filled (a calloc()-equivalent, not malloc()+memset() bolted on
 * separately, for the same "no separate write needed" reason
 * lib/std/mem/mem.tr's alloc_zeroed() wraps calloc() instead of alloc()).
 *
 * MEM-001 needs this because two of the GC-heap call sites it reroutes
 * genuinely rely on zero-initialization, not just convenience:
 * `io::read_to_string()` (lib/std/io/io.tr) and
 * `net::TcpConnection.read()` (lib/std/io/net.tr) both allocate a buffer
 * sized one byte larger than the maximum they will read into, and rely on
 * that trailing byte (and, on a short read, everything past what was
 * actually written) already being 0 as the buffer's NUL terminator -- see
 * each function's own doc comment for why. Using turf_rt_alloc() (malloc,
 * uninitialized) for either would read that terminator byte as garbage.
 */
void *turf_rt_alloc_zeroed(size_t bytes, const TurfTypeDesc *desc);

/*
 * turf_rt_realloc -- resizes an existing turf_rt_alloc/turf_rt_alloc_zeroed
 * block to `new_size` bytes, exactly like C's realloc() (may extend in
 * place or move; `p` must be a pointer this API returned, or NULL). Exists
 * so MEM-001's collection-growth call sites (List<T>/Stack<T>/Queue<T>/
 * Set<T>/Map<K,V>, all of which grow their backing buffer with a plain
 * realloc() today) keep IDENTICAL growth behavior after rerouting --
 * without this, growth would have to become "always allocate fresh +
 * memcpy", which never reuses an allocator's in-place-extend fast path and
 * is a real (if currently unobservable) peak-memory regression relative to
 * today. Fail-fast on OOM via turf_rt_panic(), matching turf_rt_alloc().
 */
void *turf_rt_realloc(void *p, size_t new_size, const TurfTypeDesc *desc);

/*
 * TurfRtStats / turf_rt_stats -- MEM-001's allocation-accounting hook
 * (ARCHITECTURE_OVERHAUL.md §8 MEM-001, stage 3: "turf_rt_stats()"),
 * explicitly built to become MEM-002's own regression harness: once a real
 * collector exists, a test can compare turf_rt_stats() before/after a
 * collection to prove it actually reclaimed something. In THIS group
 * nothing collects, so these counters are monotonically increasing for
 * the life of the process -- "total ever requested through this API", NOT
 * "currently live" (there is no way to know what's still live without a
 * collector). `bytes_requested` sums every `bytes`/`new_size` argument
 * across all three alloc functions above, including every intermediate
 * size passed to turf_rt_realloc as a buffer grows repeatedly -- it is a
 * cumulative request total, not a high-water mark or a live-byte count.
 * Not reachable from Turf source under any name (Rule B2/B3) -- read
 * directly from C (scripts/mem_smoke.sh) or via turf_rt_shutdown()'s
 * opt-in stderr dump, gated on the TURF_RT_STATS_DUMP environment variable
 * being set to anything non-empty, specifically so it stays silent (no
 * output/behavior change) for every one of the hundreds of existing
 * compiled Turf programs that do not set it.
 */
typedef struct TurfRtStats {
  uint64_t alloc_count;      /* turf_rt_alloc + turf_rt_alloc_zeroed calls */
  uint64_t realloc_count;    /* turf_rt_realloc calls */
  uint64_t bytes_requested;  /* cumulative bytes across all of the above */
} TurfRtStats;

TurfRtStats turf_rt_stats(void);

/*
 * turf_rt_panic -- replaces every inline fprintf+abort CodegenPanic() used
 * to emit directly (src/Builtins.cpp). `kind` is a short, fixed
 * classification ("division by zero", "array bounds", "integer overflow",
 * ...) baked in at the call site as a string literal; `msg` is the
 * pre-existing human-readable message; `file`/`line` are the Turf source
 * location, not a C source location. Formats a single consistent line to
 * stderr, then abort()s -- same terminal signal (SIGABRT, matching
 * `scripts/test_runner.sh`'s RUNTIME_ABORT_EXPECTED contract, which checks
 * only "non-zero, and not a timeout/OOM-kill status") every one of these
 * checks has always produced; the destination is C's own runtime safety
 * guards ONLY (bounds/div-by-zero/overflow), never a call reachable from
 * ordinary Turf source -- lib/std/io/sys.tr's own panic()/abort() is a
 * completely separate, pre-existing path this function does not touch or
 * replace.
 */
void turf_rt_panic(const char *kind, const char *msg, const char *file,
                    int line);

/*
 * turf_rt_typedesc_is_pointer_word -- reads one bit back out of a
 * TurfTypeDesc's pointer_word_bitmap. See runtime/turfrt_types.c's own doc
 * comment: nothing but scripts/runtime_smoke.sh calls this in RT-001;
 * MEM-002's mark phase is the real, intended long-term caller. Returns 0
 * (never a diagnostic) for a NULL desc or an out-of-range word_index --
 * `pointer_word_bitmap` only ever covers words [0, 64).
 */
int turf_rt_typedesc_is_pointer_word(const TurfTypeDesc *desc,
                                      uint64_t word_index);

#ifdef __cplusplus
}
#endif

#endif /* TURFRT_H */
