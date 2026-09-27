/*
 * runtime/turfrt_panic.c -- turf_rt_panic(), replacing CodegenPanic()'s old
 * inline fprintf(stderr, ...) + abort() (src/Builtins.cpp). See turfrt.h's
 * doc comment on turf_rt_panic() for the full contract.
 */

#include "turfrt.h"

#include <stdio.h>
#include <stdlib.h>

void turf_rt_panic(const char *kind, const char *msg, const char *file,
                    int line) {
  fprintf(stderr, "panic: %s: %s (%s:%d)\n", kind ? kind : "panic",
          msg ? msg : "", file ? file : "<unknown>", line);
  fflush(stderr);
  abort();
}
