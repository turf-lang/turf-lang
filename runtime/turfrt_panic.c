/*
 * runtime/turfrt_panic.c -- turf_rt_panic(), replacing CodegenPanic()'s old
 * inline fprintf(stderr, ...) + abort() (src/sema/Builtins.cpp). See turfrt.h's
 * doc comment on turf_rt_panic() for the full contract.
 */

#include "turfrt.h"

#include <stdio.h>
#include <stdlib.h>

void turf_rt_panic(const char *kind, const char *msg, const char *file,
                    int line) {
  /* What the program printed before the panic is part of its output; abort()
   * does not flush stdio, so a piped stdout lost it. Flushed before the
   * message, so a terminal shows the two in order. */
  fflush(stdout);
  fprintf(stderr, "panic: %s: %s (%s:%d)\n", kind ? kind : "panic",
          msg ? msg : "", file ? file : "<unknown>", line);
  fflush(stderr);
  abort();
}

void turfrt_flush_stdout(void) { fflush(stdout); }

void turfrt_abort(void) {
  fflush(stdout);
  abort();
}
