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
 * (F-MEM-3) -- nothing here is reachable from ordinary Turf source yet;
 * MEM-001 is what routes real allocations through turf_rt_alloc.
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
 * Nothing calls this yet as of RT-001 -- lib/std/mem/mem.tr's malloc/free
 * bindings are deliberately untouched (F-MEM-3); MEM-001 is what reroutes
 * GC-heap-bound allocations here.
 */
void *turf_rt_alloc(size_t bytes, const TurfTypeDesc *desc);

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
