#!/bin/bash
#
# mem_smoke.sh -- MEM-001's own regression coverage
# (docs/planning/ARCHITECTURE_OVERHAUL.md §8 MEM-001, stage 3:
# "turf_rt_stats()" -- the allocation-accounting hook this group added
# specifically to become MEM-002's own regression harness). Mirrors
# runtime_smoke.sh's shape (RT-001): `test_runner.sh` cannot exercise any of
# this, since none of it is something a `.tr` program can observe about
# itself without the TURF_RT_STATS_DUMP opt-in this script relies on.
#
# What this proves:
#   1. libturfrt's turf_rt_stats()/turf_rt_alloc()/turf_rt_alloc_zeroed()/
#      turf_rt_realloc(), called directly from a small C program linked
#      straight against lib/libturfrt.a -- no Turf compiler involved.
#   2. A compiled Turf program that allocates on the GC heap (via the
#      gc_alloc/gc_alloc_zeroed/gc_realloc intrinsics, reached indirectly
#      here through ordinary List<T>.push()/string concatenation) produces
#      the exact allocation count MEM-001's own reroute should produce,
#      read back out via the TURF_RT_STATS_DUMP-gated stderr dump.
#   3. That dump is genuinely OPT-IN: the identical program run WITHOUT the
#      environment variable set produces no TURF_RT_STATS line at all --
#      the completion criterion this whole group is judged on ("full suite
#      green with identical behaviour") requires that no existing compiled
#      Turf program's default output changes.
#
# Usage: scripts/mem_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

TURF="./build/turf"
LIBTURFRT="lib/libturfrt.a"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turf missing -- build before running this smoke test.${RESET}"
  exit 1
fi
if [ ! -f "$LIBTURFRT" ]; then
  echo -e "${RED}$LIBTURFRT missing -- build before running this smoke test.${RESET}"
  exit 1
fi

echo -e "${BOLD}1. libturfrt's own C API: turf_rt_stats() accounts for every call${RESET}"
cat > "$WORKDIR/stats_test.c" <<'EOF'
#include "turfrt.h"
#include <assert.h>

int main(void) {
    turf_rt_init();

    TurfRtStats before = turf_rt_stats();

    void *a = turf_rt_alloc(64, NULL);
    void *b = turf_rt_alloc_zeroed(64, NULL);
    void *c = turf_rt_realloc(a, 128, NULL);
    (void)a; (void)b; (void)c;

    TurfRtStats after = turf_rt_stats();

    assert(after.alloc_count == before.alloc_count + 2);
    assert(after.realloc_count == before.realloc_count + 1);
    assert(after.bytes_requested == before.bytes_requested + 64 + 64 + 128);

    turf_rt_shutdown();
    return 0;
}
EOF
if cc -std=c11 -I runtime "$WORKDIR/stats_test.c" "$LIBTURFRT" -o "$WORKDIR/stats_test" > "$WORKDIR/stats_test.compile.log" 2>&1; then
  pass "compiled against libturfrt.a directly"
  if "$WORKDIR/stats_test"; then
    pass "ran, all assertions held (2 allocs, 1 realloc, 256 bytes requested)"
  else
    fail "assertion failed at runtime"
  fi
else
  fail "failed to compile against libturfrt.a"; cat "$WORKDIR/stats_test.compile.log"
fi

echo -e "${BOLD}2. A compiled Turf program's GC-heap allocations are counted${RESET}"
cat > "$WORKDIR/alloc_count.tr" <<'EOF'
import list::List;

fn main(): int {
    mut l: List<int> = List::default();
    mut i: int = 0;
    while i < 20 {
        l.push(i);
        i = i + 1;
    }
    return l.length();
}
EOF
if "$TURF" "$WORKDIR/alloc_count.tr" -o "$WORKDIR/alloc_count" > "$WORKDIR/alloc_count.compile.log" 2>&1; then
  pass "compiled"
  # List<int>.push() doubles capacity from 0 -> 8 -> 16 -> 32: one fresh
  # gc_alloc (cap 0 -> 8) then two gc_reallocs (8 -> 16, 16 -> 32) to cover
  # 20 pushes. Exactly one alloc_count, two realloc_count -- if this number
  # ever changes it means either the growth policy changed (list.tr) or the
  # reroute regressed (a call site silently reverted to mem::malloc/raw
  # malloc, which turf_rt_stats() cannot see at all).
  OUT=$(TURF_RT_STATS_DUMP=1 "$WORKDIR/alloc_count" 2>&1 >/dev/null)
  # MEM-002 extends the dump line with collect_count/live_count/live_bytes/
  # freed_count -- this check only cares about the three MEM-001 fields it
  # already named, so the pattern is left open-ended after bytes_requested
  # rather than re-anchored to an exact MEM-002 collection count (which
  # legitimately varies -- whether this specific 20-push program crosses
  # the 1 MiB collection threshold is an implementation detail of the
  # threshold constant, not something this test should pin down).
  if echo "$OUT" | grep -qE '^TURF_RT_STATS alloc_count=1 realloc_count=2 bytes_requested=[0-9]+ '; then
    pass "TURF_RT_STATS_DUMP shows exactly 1 alloc + 2 reallocs for 20 pushes"
  else
    fail "unexpected stats line: '$OUT'"
  fi
else
  fail "alloc_count.tr failed to compile"; cat "$WORKDIR/alloc_count.compile.log"
fi

echo -e "${BOLD}3. The dump is opt-in: no TURF_RT_STATS_DUMP means no output${RESET}"
OUT=$("$WORKDIR/alloc_count" 2>&1 >/dev/null)
if [ -z "$OUT" ]; then
  pass "silent by default -- no stderr output without the env var"
else
  fail "unexpected stderr output with no env var set: '$OUT'"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All mem smoke checks passed.${RESET}"
  exit 0
else
  echo -e "${RED}${BOLD}At least one mem smoke check failed.${RESET}"
  exit 1
fi
