/*
 * runtime/turfrt_types.c -- small TurfTypeDesc accessors.
 *
 * The descriptors themselves are emitted as LLVM constant globals by the
 * compiler (src/CodegenStruct.cpp), never constructed at runtime -- this
 * file exists only to give the runtime (and its own smoke test,
 * scripts/runtime_smoke.sh) a single, shared place to read one back out,
 * rather than every reader re-deriving the bit-shift arithmetic.
 * turfrt_gc.c's mark phase is this function's real, load-bearing caller
 * (OVH-001) -- runtime_smoke.sh also exercises it directly.
 */

#include "turfrt.h"

int turf_rt_typedesc_is_pointer_word(const TurfTypeDesc *desc,
                                      uint64_t word_index) {
  if (!desc || word_index >= desc->word_count)
    return 0;
  uint64_t bitmap_word = desc->pointer_words[word_index / 64];
  return (bitmap_word >> (word_index % 64)) & 1u;
}
