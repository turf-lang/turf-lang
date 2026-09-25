/*
 * runtime/turfrt_types.c -- small TurfTypeDesc accessors.
 *
 * The descriptors themselves are emitted as LLVM constant globals by the
 * compiler (src/CodegenStruct.cpp), never constructed at runtime -- this
 * file exists only to give the runtime (and its own smoke test,
 * scripts/runtime_smoke.sh) a single, shared place to read one back out,
 * rather than every reader re-deriving the bit-shift arithmetic. MEM-002's
 * collector will call turf_rt_typedesc_is_pointer_word() from its mark
 * phase; RT-001's own runtime_smoke.sh is this function's only caller today.
 */

#include "turfrt.h"

int turf_rt_typedesc_is_pointer_word(const TurfTypeDesc *desc,
                                      uint64_t word_index) {
  if (!desc || word_index >= 64)
    return 0;
  return (desc->pointer_word_bitmap >> word_index) & 1u;
}
