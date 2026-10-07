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
 * LLVM struct layout ({i64, ptr, i64, ptr}) must match this C struct's
 * layout field-for-field.
 *
 * OVH-001 (docs/planning/OVERHAUL_REMAINING_WORK.md): the original RT-001
 * version of this struct held the bitmap inline as a single `uint64_t`,
 * covering only the first 64 eight-byte words (512 bytes) of a type's
 * layout -- any pointer field past that point was silently untraced, which
 * MEM-002's collector then depended on for correctness, turning a
 * documented limitation into a live use-after-free (a `string` field past
 * byte 512 of a GC-heap struct got freed out from under a still-live
 * object). `word_count`/`pointer_words` replace it with a variable-length
 * bitmap covering the type's ENTIRE layout, at any size: `word_count` is
 * `ceil(size / 8)` (every 8-byte word of the allocation, including a
 * trailing partial one), and `pointer_words` points at a
 * `ceil(word_count / 64)`-element array of `uint64_t`, static storage,
 * never freed -- bit `i % 64` of `pointer_words[i / 64]` set means "byte
 * range [8i, 8i+8) may hold a pointer" (conservative: it marks every
 * pointer-SHAPED word, not only ones proven to point into the GC heap
 * specifically, since nothing before MEM-001/MEM-002 tags a pointer field
 * with its heap of origin). See `turf_rt_typedesc_is_pointer_word()`'s own
 * doc comment for the accessor that hides the two-level indexing.
 */
typedef struct TurfTypeDesc {
  uint64_t size;                    /* allocation size in bytes (DataLayout::getTypeAllocSize) */
  const char *name;                 /* the struct's Turf-level name; static storage, never freed */
  uint64_t word_count;              /* number of traced 8-byte words = ceil(size / 8) */
  const uint64_t *pointer_words;    /* ceil(word_count / 64)-element bitmap array; static storage */
} TurfTypeDesc;

/*
 * MEM-002's reuse of this exact struct for an ARRAY of elements (a
 * collection's backing buffer), rather than widening the C layout with a
 * new field: the collector's mark phase (turfrt_gc.c) walks a traced
 * block's bytes in `desc->size`-byte STRIDES, re-applying
 * `pointer_words` at every stride, until the stride count reaches the
 * block's own actual allocated length (tracked by the block table, not by
 * this struct). A single-instance struct allocation is the special case
 * where the block's length exactly equals `desc->size` -- one stride, no
 * repetition -- so RT-001's original struct descriptors need no change at
 * all to keep meaning exactly what they always meant; only a genuinely
 * multi-element block (built by passing an element-sized `desc` to
 * turf_rt_alloc/turf_rt_realloc for a block many times that size) exercises
 * the repeating behavior. See `docs/development/changes/MEM-002.md` for why
 * this reuse was chosen over adding an `is_array` field: this codebase's own
 * "verify claims live" rule caught that adding a field would silently
 * require re-deriving `getOrCreateTurfTypeDescType()`'s LLVM layout AND
 * every existing consumer, for a distinction the block's own recorded
 * length already lets the mark phase infer without being told.
 */

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
 * like sys::panic()'s own abort() in lib/std/core/sys.tr).
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
 * turf_rt_alloc -- the typed allocation entrypoint (F-MEM-4). As of MEM-002
 * this is a real, collected allocation: the returned block is registered
 * with the collector's block table (turfrt_gc.c) and reclaimed by
 * turf_rt_collect() once nothing reachable from a root still points into
 * it. `desc` may be NULL, which MEM-002 defines as "opaque -- this block
 * holds no outgoing GC-heap pointers anywhere in its bytes" (correct, not
 * merely permitted, for a raw byte run like a string buffer: nothing in
 * TurfLang ever stores a pointer INSIDE a string's own bytes). A non-NULL
 * `desc` is now an ARRAY-ELEMENT descriptor, not only a single-instance one
 * -- see this header's own TurfTypeDesc doc comment below for the repeating
 * semantics MEM-002 gives `size`/`pointer_word_bitmap` when the block is
 * longer than one `size`-sized stride. `desc` is also still used for a
 * panic message on OOM.
 *
 * As of MEM-001/MEM-002 (docs/planning/ARCHITECTURE_OVERHAUL.md §8), this is
 * called from Codegen for the Turf-level `gc_alloc(n)` bare intrinsic
 * (FuncCallExprAST::codegen(), src/CodegenFunc.cpp). Every GC-heap-bound
 * allocation classified in `docs/development/changes/MEM-001.md` (string
 * results, collection backing buffers) reaches this function; a raw byte
 * buffer (strings.tr/io.tr/net.tr) still passes `desc == NULL`, but a
 * collection's backing-buffer growth (list/stack/queue/set/map.tr, each
 * writing `gc_alloc(n) to T*`) now has a REAL per-instantiation array
 * descriptor injected by Codegen (`CastExprAST::codegen()`'s MEM-002
 * special case, src/CodegenExpr.cpp) -- see `docs/development/changes/
 * MEM-002.md`'s *Removed assumptions* for why MEM-001 could not have done
 * this itself (no array-shaped descriptor existed yet).
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
 * `io::read_to_string()` (lib/std/core/io.tr) and
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
 * turf_rt_collect -- MEM-002. Runs one full stop-the-world mark-sweep cycle
 * over the GC heap: mark (conservative over the stack and callee-saved
 * registers, precise over registered global roots and every already-marked
 * block's own declared pointer words) then sweep (free() every unmarked
 * block, compact the block table). Safe to call at any point after
 * turf_rt_init() -- in particular, safe to call from INSIDE
 * turf_rt_alloc()/turf_rt_alloc_zeroed()/turf_rt_realloc() themselves (this
 * is exactly what "also called on allocation pressure" means): the block
 * those functions are about to return is already registered before the
 * pressure check runs, and its address is still live on the C stack/in a
 * register at that point, so conservative scanning retains it correctly --
 * see turfrt_gc.c's own top comment for the full ordering argument. Manual
 * (`mem::alloc`) memory is never touched -- the collector has no block-table
 * entry for it at all (F-MEM-3).
 */
void turf_rt_collect(void);

/*
 * turf_rt_add_global_root / turf_rt_remove_global_root -- MEM-002, §14.6
 * point 2. Registers/unregisters a `void**` STORAGE LOCATION (not a
 * pointer value) as a precise root: the collector reads `*slot` fresh at
 * the start of every mark phase, so a slot's contents may change between
 * collections and the collector always sees the current value. As of this
 * group, verified live: TurfLang has no mutable global/static storage of
 * any kind (`export immut` constants are inlined literal values with no
 * storage -- FREEZE.md §6.5/§14.6 point 2's own text already says so), so
 * Codegen has no real call site that needs this today -- it exists as
 * genuine, tested runtime API (`scripts/gc_smoke.sh` exercises it directly
 * against `lib/libturfrt.a`, the same direct-C-linkage precedent
 * `scripts/mem_smoke.sh`/`scripts/runtime_smoke.sh` already use) so that a
 * FUTURE feature adding global/static storage (closures' captured
 * environments, say) needs no second runtime change to plug into it. See
 * `docs/development/changes/MEM-002.md`'s *Known pitfalls* before assuming
 * a Codegen-side caller is "missing" -- it is deliberately absent, not
 * forgotten.
 */
void turf_rt_add_global_root(void **slot);
void turf_rt_remove_global_root(void **slot);

/*
 * turf_rt_pin / turf_rt_unpin -- OVH-010
 * (docs/planning/OVERHAUL_REMAINING_WORK.md), §14.3/§14.6's own documented
 * answer to F-MEM-3's disclosed unsoundness: a GC pointer stored ONLY
 * inside manually-managed (malloc/mem::alloc) memory is invisible to
 * every root-finding path above it (stack, registers, global roots,
 * another GC-heap block's own declared pointer words) and gets reclaimed
 * out from under it. `turf_rt_pin(p)` marks the GC object CONTAINING `p`
 * (an interior pointer is fine -- `List<T>.data + k` is the documented,
 * common case, exactly like a conservative stack/register root) as a
 * precise, always-retained root until a matching `turf_rt_unpin(p)`;
 * pins NEST (an internal per-object count, not a boolean), so two
 * independent callers pinning the same object don't race to unpin it out
 * from under each other. `p == NULL`, or a `p` that is not a GC-heap
 * pointer at all (no containing block found), is a documented NO-OP for
 * BOTH functions -- C memory is legitimately passed around, and pinning
 * or unpinning it would be meaningless, not an error. `turf_rt_unpin(p)`
 * on a GC object that IS real but currently has no outstanding pin (never
 * pinned, or already fully unpinned) panics ("unpin of unpinned object")
 * -- an unbalanced unroot() is a programmer error worth surfacing, unlike
 * the deliberately-silent non-GC-pointer case above. A pin survives a
 * `turf_rt_realloc()` that moves the pinned block (`turf_gc_reregister_block`,
 * turfrt_gc.c, carries it to the new address) -- correct for a growing
 * collection buffer, though nothing in the standard library pins one
 * today (strings, the motivating case, are immutable and never move).
 * Reached from Turf source via the `gc_pin(p)`/`gc_unpin(p)` bare
 * intrinsics (`include/Intrinsics.h` category 1); `lib/std/mem/mem.tr`'s
 * `mem::root()`/`mem::unroot()` are their one caller each, the same
 * one-bare-intrinsic-one-stdlib-wrapper shape `gc_checked_free`/
 * `mem::free` already use.
 */
void turf_rt_pin(void *p);
void turf_rt_unpin(void *p);

/*
 * turf_rt_is_gc_pointer -- MEM-002. Exact-match block-table lookup: true iff
 * `p` is precisely the address turf_rt_alloc/turf_rt_alloc_zeroed/
 * turf_rt_realloc returned for some still-live GC-heap block (an interior
 * address, one that merely points INTO a block, returns false -- this is
 * deliberately narrower than the mark phase's own containment check, which
 * treats an interior pointer as retaining its object; this function answers
 * "is this exactly a block start", the question `mem::free`'s cross-heap
 * diagnostic needs). Reached from Turf source via the `gc_is_managed(p)`
 * bare intrinsic (`include/Intrinsics.h` category 1) -- `lib/std/mem/mem.tr`
 * `free()`'s own new cross-heap check is its one caller (MEM-002's
 * "Breaking changes": freeing a GC-heap pointer through `mem::free` must be
 * a diagnosable runtime error, not undefined behaviour, per MEM-001's own
 * "Must NOT reinterpret" list). O(1) amortized against the pending-alloc
 * buffer plus O(log n) against the indexed block table -- deliberately does
 * NOT require a full index rebuild/collection, so calling it on every
 * `mem::free()` in a hot path stays cheap.
 */
int turf_rt_is_gc_pointer(const void *p);

/*
 * turf_rt_checked_free -- MEM-002. The actual `free(3)` call for the manual
 * heap, reached from Turf source via the `gc_checked_free(p)` bare
 * intrinsic -- `lib/std/mem/mem.tr`'s own `free()` is the one caller,
 * wrapping this to keep `mem::free(p)`'s public name/signature unchanged.
 * Panics (turf_rt_panic, "cross-heap free") instead of calling libc's
 * free() when `p` is a live GC-heap block's own start address -- the
 * cross-heap mistake F-MEM-3 forbids, previously undiagnosed (MEM-001's own
 * "Must NOT reinterpret": "the two heaps do not merge", but nothing checked
 * it until this block table existed). `p == NULL` is the ordinary,
 * already-safe C free(NULL) no-op, unchanged.
 *
 * Named distinctly from turf_rt_is_gc_pointer's own bare-intrinsic
 * surface (gc_is_managed) rather than composed from it in Turf source --
 * `lib/std/mem/mem.tr` has no `extern fn free` to call any more (Turf's
 * `extern fn` binds a declared name directly to an identically-named C
 * symbol, with no alias syntax, so a checked wrapper could not itself be
 * named `free` in the same file without colliding with that extern
 * declaration -- see this group's own change record for the full reasoning
 * once this comment is landed).
 */
void turf_rt_checked_free(void *p);

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
 *
 * MEM-002 adds the four `collect_count`/`live_count`/`live_bytes`/
 * `freed_count` fields below -- exactly the "before/after a collection"
 * comparison MEM-001's own doc comment (this struct, pre-MEM-002 version)
 * predicted this hook would need. Unlike the three original fields, these
 * are POINT-IN-TIME as of the most recent turf_rt_collect() (0 for all four
 * before the first collection ever runs), not cumulative-since-start --
 * `live_count`/`live_bytes` answer "how much survived the last sweep",
 * which a monotonically-increasing counter cannot express.
 */
typedef struct TurfRtStats {
  uint64_t alloc_count;      /* turf_rt_alloc + turf_rt_alloc_zeroed calls */
  uint64_t realloc_count;    /* turf_rt_realloc calls */
  uint64_t bytes_requested;  /* cumulative bytes across all of the above */
  uint64_t collect_count;    /* MEM-002: turf_rt_collect() cycles run so far */
  uint64_t live_count;       /* MEM-002: blocks alive after the most recent sweep */
  uint64_t live_bytes;       /* MEM-002: bytes alive after the most recent sweep */
  uint64_t freed_count;      /* MEM-002: blocks reclaimed, cumulative across all cycles */
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
 * ordinary Turf source -- lib/std/core/sys.tr's own panic()/abort() is a
 * completely separate, pre-existing path this function does not touch or
 * replace.
 */
void turf_rt_panic(const char *kind, const char *msg, const char *file,
                    int line);

/*
 * turf_rt_typedesc_is_pointer_word -- reads one bit back out of a
 * TurfTypeDesc's `pointer_words` array, hiding the two-level
 * word_index/64, word_index%64 indexing (OVH-001) from every caller --
 * turfrt_gc.c's mark phase is the real, intended caller (the collector's
 * only consumer of a descriptor's bitmap). Returns 0 (never a diagnostic)
 * for a NULL desc or a `word_index >= desc->word_count` -- unlike the old
 * fixed 64-word cap this replaced, `word_count` is the type's REAL word
 * count at any size, so this is a genuine bounds check, not a silent
 * truncation.
 */
int turf_rt_typedesc_is_pointer_word(const TurfTypeDesc *desc,
                                      uint64_t word_index);

/*
 * turf_rt_abi_2_0 -- FDN-006 stage 3, the compiler/runtime ABI stamp
 * (runtime/turfrt_abi.c has the full contract). Does nothing; every
 * compiled Turf binary's `main` wrapper calls it
 * (WrapMainWithRuntimeBootstrap(), src/CodegenFunc.cpp) purely so a stale
 * or mismatched `libturfrt.a` fails at LINK time ("undefined reference to
 * `turf_rt_abi_2_0`") instead of silently linking and misbehaving at run
 * time. Never reachable from Turf source under any name (Rule B2/B3, like
 * every other symbol in this header).
 */
void turf_rt_abi_2_0(void);

#ifdef __cplusplus
}
#endif

#endif /* TURFRT_H */
