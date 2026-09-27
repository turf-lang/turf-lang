#!/bin/bash
#
# std002_caller_location_smoke.sh -- STD-002's own regression coverage
# (docs/planning/ARCHITECTURE_OVERHAUL.md §8 STD-002), for exactly the
# concrete testing criterion that section names: "a test asserting that
# test::assert's failure message names the caller's file and line, not
# lib/std/testing/test.tr's". `test_runner.sh`'s RUNTIME_ABORT_EXPECTED
# convention only checks exit code -- it cannot express "and the stderr
# text says X, not Y" -- so this is a dedicated smoke script, the same
# precedent RT-001's runtime_smoke.sh set for a panic's exact message shape.
#
# tests/42_caller_location/ covers the underlying `@caller_location`/
# `caller_location` LANGUAGE mechanism generically (direct call + transitive
# forwarding through an intermediate `@caller_location` function, via
# ordinary test::assert calls on the returned string) -- this script is
# specifically about the STDLIB APPLICATION: a real failing test::assert()
# call, compiled and run for real, with its actual stderr captured.
#
# Usage: scripts/std002_caller_location_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

TURF="./build/turf"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turf missing -- build before running this smoke test.${RESET}"
  exit 1
fi

echo -e "${BOLD}std002_caller_location_smoke${RESET}\n"

cat > "$WORKDIR/user_test.tr" <<'EOF'
import test;

fn main(): int {
    test::assert(1 == 2, "should never happen");
    return 0;
}
EOF

if ! "$TURF" "$WORKDIR/user_test.tr" -o "$WORKDIR/user_test" > "$WORKDIR/compile.log" 2>&1; then
  fail "the probe program failed to compile:"
  cat "$WORKDIR/compile.log"
  echo
  echo -e "${RED}${BOLD}std002_caller_location_smoke: FAILED.${RESET}"
  exit 1
fi

"$WORKDIR/user_test" > "$WORKDIR/run.out" 2>&1
EXIT_CODE=$?
OUT=$(cat "$WORKDIR/run.out")

echo -e "${BOLD}1. A failing test::assert() aborts${RESET}"
if [ "$EXIT_CODE" -eq 134 ]; then
  pass "exit 134 (SIGABRT), as expected"
else
  fail "expected exit 134, got $EXIT_CODE"
fi

echo -e "\n${BOLD}2. The failure message names the CALLER's file and line${RESET}"
if echo "$OUT" | grep -q "user_test.tr:4"; then
  pass "message contains 'user_test.tr:4' (the probe's own test::assert call site)"
else
  fail "message does not name the caller's file:line. Got:"
  echo "$OUT" | sed 's/^/       /'
fi

echo -e "\n${BOLD}3. The failure message does NOT name lib/std/testing/test.tr${RESET}"
if echo "$OUT" | grep -q "testing/test.tr"; then
  fail "message still names lib/std/testing/test.tr -- the bug STD-002 fixes. Got:"
  echo "$OUT" | sed 's/^/       /'
else
  pass "message does not leak test.tr's own internal file/line"
fi

echo
if [ $FAIL -eq 0 ]; then
  echo -e "${GREEN}${BOLD}std002_caller_location_smoke: clean.${RESET}"
  exit 0
fi
echo -e "${RED}${BOLD}std002_caller_location_smoke: FAILED.${RESET}"
exit 1
