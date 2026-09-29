#!/bin/bash
#
# gc_no_procfs_smoke.sh -- FDN-005's regression coverage for the collector's
# stack-base fallback, the path a Turf program takes when /proc/self/maps
# cannot be read (a minimal container or sandbox without procfs --
# ROADMAP_v2.0.0.md REL-003: "FDN-005 must test the fallback path too").
#
# The bug this guards against: before FDN-005 the fallback captured the
# address of a local inside turf_rt_init()'s frame. That frame is a SIBLING
# of the user's main(), not an ancestor, so main()'s own locals fell outside
# the conservatively scanned range and the collector freed live objects --
# 11 of 12 tests/41_gc programs failed under TURF_GC_STRESS with /proc
# hidden. It never showed on a normal Linux box because /proc is always
# there; TURF_GC_NO_PROCFS=1 (runtime/turfrt_gc.c) hides it on demand.
#
#   1. The knob really selects the fallback, and the fallback is an
#      ancestor of every frame (glibc's __libc_stack_end) -- checked from C,
#      since the recorded base is not observable from Turf source. Without
#      this, check 2 would pass vacuously if the knob were ever ignored.
#   2. Every tests/41_gc program plus the string-concat stress test, at -O0
#      and -O2, with the fallback forced, a collection on every allocation
#      (TURF_GC_STRESS) and the full-stack mark-set comparison on
#      (TURF_GC_VALIDATE) -- which turns a missed root into a deterministic
#      panic at the first collection instead of luck-dependent corruption.
#
# Usage: scripts/gc_no_procfs_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

COMPILER="${TURF_COMPILER:-$(pwd)/build/turf}"
LIBTURFRT="lib/libturfrt.a"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

for f in "$COMPILER" "$LIBTURFRT"; do
  if [ ! -f "$f" ]; then
    echo -e "${RED}$f missing -- build before running this smoke test.${RESET}"
    exit 1
  fi
done

echo -e "${BOLD}1. TURF_GC_NO_PROCFS=1 selects a stack base that is an ancestor of main()${RESET}"
cat > "$WORKDIR/base_test.c" <<'EOF'
#include "turfrt.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uintptr_t turf_gc_get_stack_base(void); /* turfrt_internal.h */
extern void *__libc_stack_end;

int main(int argc, char **argv) {
  turf_rt_init();
  uintptr_t base = turf_gc_get_stack_base();
  uintptr_t here = (uintptr_t)&argc;
  uintptr_t proc_hi = 0;
  FILE *f = fopen("/proc/self/maps", "r");
  char line[512];
  while (f && fgets(line, sizeof line, f))
    if (strstr(line, "[stack]")) {
      unsigned long long lo, hi;
      if (sscanf(line, "%llx-%llx", &lo, &hi) == 2) proc_hi = (uintptr_t)hi;
    }
  if (f) fclose(f);
  if (base <= here) { printf("base %#lx not above main's frame %#lx\n",
                             (unsigned long)base, (unsigned long)here); return 1; }
  if (strcmp(argv[1], "fallback") == 0) {
    if (base != (uintptr_t)__libc_stack_end) {
      printf("fallback base %#lx != __libc_stack_end %#lx\n",
             (unsigned long)base, (unsigned long)__libc_stack_end); return 1; }
  } else if (base != proc_hi) {
    printf("procfs base %#lx != [stack] top %#lx\n",
           (unsigned long)base, (unsigned long)proc_hi); return 1;
  }
  return 0;
}
EOF
if ! ldd --version 2>&1 | grep -qi "gnu libc\|glibc"; then
  pass "skipped: not glibc (the fallback is __libc_stack_end on glibc only)"
elif cc -std=gnu11 -I runtime "$WORKDIR/base_test.c" "$LIBTURFRT" -o "$WORKDIR/base_test" > "$WORKDIR/base.log" 2>&1; then
  if out=$("$WORKDIR/base_test" procfs 2>&1); then
    pass "with /proc: base is the [stack] mapping's top"
  else
    fail "with /proc: $out"
  fi
  if out=$(TURF_GC_NO_PROCFS=1 "$WORKDIR/base_test" fallback 2>&1); then
    pass "without /proc: base is __libc_stack_end, above main()'s frame"
  else
    fail "without /proc: $out"
  fi
else
  fail "base_test.c failed to compile:"; cat "$WORKDIR/base.log"
fi

echo -e "${BOLD}2. GC regression programs under the fallback, TURF_GC_STRESS and TURF_GC_VALIDATE${RESET}"
PROGRAMS=$(ls tests/41_gc/[0-9]*.tr; echo tests/13_string_stdlib/17_stress_10k_concat.tr)
for O in -O0 -O2; do
  for src in $PROGRAMS; do
    name=$(basename "$src" .tr)
    bin="$WORKDIR/$name$O"
    if ! "$COMPILER" $O "$src" -o "$bin" > "$WORKDIR/$name.compile.log" 2>&1; then
      fail "$name ($O) failed to compile"; cat "$WORKDIR/$name.compile.log"; continue
    fi
    if out=$(TURF_GC_NO_PROCFS=1 TURF_GC_STRESS=1 TURF_GC_VALIDATE=1 timeout 120 "$bin" 2>&1); then
      pass "$name ($O)"
    else
      fail "$name ($O): $(echo "$out" | grep -m1 -E 'TURF_GC_VALIDATE|panic|Assertion|assert' || echo "$out" | tail -1)"
    fi
  done
done

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All no-procfs GC checks passed.${RESET}"
else
  echo -e "${RED}${BOLD}At least one no-procfs GC check failed.${RESET}"
fi
exit "$FAIL"
