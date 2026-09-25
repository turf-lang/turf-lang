#!/bin/bash
#
# gc_rss_smoke.sh -- MEM-002's own completion criterion, made observable
# (docs/planning/ARCHITECTURE_OVERHAUL.md §8: "The stress test's RSS is
# bounded ... the observable proof that leaks are gone").
#
# tests/08_e2e_stress/06_gc_bounded_allocation.tr is only ever checked by
# `test_runner.sh` for "compiles and exits 0" -- it has no way to read its
# own memory usage, and RSS is an OS-level property `assert()` inside the
# program could never observe. This script is what actually measures it:
# compiles that file, runs it under `/usr/bin/time -v`, and asserts peak
# RSS stays well under what the workload would need if nothing collected.
#
# Bound derivation, stated so a future session doesn't have to re-derive
# it: the test program allocates roughly 5000 * ~16KB =~ 80MB of CUMULATIVE
# GC-heap allocation REQUESTS (5000 independent List<int> growth series,
# each summing its doubling-capacity buffers up to 1000 elements). Measured
# live at time of writing this script: ~2.7MB peak RSS. 20MB is a bound
# with generous headroom above the measured figure (interpreter/libc/loader
# baseline plus real allocator fragmentation, both platform-dependent) while
# staying FAR below the ~80MB an unbounded leak (MEM-001-without-MEM-002's
# own "identical to today, still leaks" behaviour) would produce -- a
# regression that silently stopped collecting would still be caught long
# before this bound, not just barely.
#
# Usage: scripts/gc_rss_smoke.sh
# Exit:  0 = peak RSS within bound; 1 = compile/run failure or bound exceeded.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

TURF="./build/turf"
TEST_FILE="tests/08_e2e_stress/06_gc_bounded_allocation.tr"
BOUND_KB=20000
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turf missing -- build before running this smoke test.${RESET}"
  exit 1
fi
if ! command -v /usr/bin/time > /dev/null 2>&1; then
  echo -e "${RED}/usr/bin/time not found -- this script needs GNU time's -v for RSS.${RESET}"
  exit 1
fi

echo -e "${BOLD}1. Compile the bounded-allocation stress test${RESET}"
if "$TURF" "$TEST_FILE" -o "$WORKDIR/stress" > "$WORKDIR/compile.log" 2>&1; then
  pass "compiled"
else
  fail "failed to compile"; cat "$WORKDIR/compile.log"
  exit 1
fi

echo -e "${BOLD}2. Run under /usr/bin/time -v and measure peak RSS${RESET}"
/usr/bin/time -v "$WORKDIR/stress" > "$WORKDIR/run.log" 2> "$WORKDIR/time.log"
RC=$?
if [ $RC -ne 0 ]; then
  fail "the program itself failed (exit $RC) -- not a memory question, see its own output"
  cat "$WORKDIR/run.log"
  exit 1
fi
pass "ran, exit 0 (its own test::assert()s on the total count held)"

PEAK_KB=$(grep "Maximum resident set size" "$WORKDIR/time.log" | awk '{print $NF}')
if [ -z "$PEAK_KB" ]; then
  fail "could not parse peak RSS from /usr/bin/time -v output"
  cat "$WORKDIR/time.log"
  exit 1
fi

echo -e "${BOLD}3. Peak RSS is bounded, not proportional to cumulative allocation${RESET}"
echo "   Peak RSS: ${PEAK_KB} KB  (bound: ${BOUND_KB} KB; cumulative requests: ~80000 KB if nothing collected)"
if [ "$PEAK_KB" -le "$BOUND_KB" ]; then
  pass "${PEAK_KB}KB is within the ${BOUND_KB}KB bound"
else
  fail "${PEAK_KB}KB EXCEEDS the ${BOUND_KB}KB bound -- looks like the collector stopped reclaiming"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}gc_rss_smoke: clean.${RESET}"
  exit 0
else
  echo -e "${RED}${BOLD}gc_rss_smoke: FAILED.${RESET}"
  exit 1
fi
