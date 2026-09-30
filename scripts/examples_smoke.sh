#!/bin/bash
#
# examples_smoke.sh -- LEC-001 stage 8: the projects under examples/ build and
# run with the real `turf`, and their committed turf.lock files are current.
#
# Nothing else builds examples/: scripts/test_runner.sh only runs tests/, so
# without this script an example can rot against the compiler with the whole
# suite green (AGENTS.md says the same of lib/std and showcase/). It also holds
# the docs to what works: examples/README.md and docs/reference/
# projects_and_packages.md quote these projects' output.
#
# The examples are copied into a temporary directory first, so no build/ is
# written into the checkout. Every case checks the exit status and the text it
# is about, never just "it failed" (AGENTS.md: a failure for the wrong reason
# is not a pass).
#
# Usage: scripts/examples_smoke.sh   (after building build/turf and build/turfc)
# Exit:  0 = all checks pass, or build/turf is absent and CI is not set (Go is
#            optional for a local build: a SKIP, never a silent pass in CI);
#        1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
TURF="$ROOT/build/turf"
TURFC="$ROOT/build/turfc"

if [ ! -x "$TURF" ]; then
  if [ -n "${CI:-}" ]; then
    echo "FAIL: build/turf is missing in CI (was Go installed before the CMake build?)"
    exit 1
  fi
  echo "SKIP (no build/turf: Go not installed?)"
  exit 0
fi
[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }
# Which compiler and which stdlib run must follow from the binaries' own
# location, not from a developer's shell (TURF_HOME often points at another
# checkout).
unset TURF_HOME TURF_COMPILER

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# The examples have no git dependency, but the cache is ours regardless.
export TURF_CACHE_DIR="$WORK/cache"
FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

t() { # t <name> <dir> <turf args...>: output and status under $WORK/_<name>.{out,rc}
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURF" "$@") > "$WORK/_$n.out" 2>&1
  echo $? > "$WORK/_$n.rc"
}
strip() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }
expect_rc() { [ "$(cat "$WORK/_$2.rc")" = "$3" ] || fail "$1: exit $(cat "$WORK/_$2.rc"), expected $3 (got: $(strip "$WORK/_$2.out" | head -c 400))"; }
expect_out() {
  strip "$WORK/_$2.out" | grep -qF -- "$3" ||
    fail "$1: output lacks '$3' (got: $(strip "$WORK/_$2.out" | head -c 500))"
}
same() { cmp -s "$2" "$3" || fail "$1: $2 and $3 differ"; }

# The copy keeps the sibling layout (../geometry) that plotter's manifest names.
# A build/ left in the checkout by hand is not copied.
mkdir -p "$WORK/examples"
for p in geometry plotter; do
  [ -d "examples/$p" ] || { fail "examples/$p does not exist"; continue; }
  cp -R "examples/$p" "$WORK/examples/$p"
  rm -rf "$WORK/examples/$p/build"
done
EX=$WORK/examples

# --- the committed lock files are current ------------------------------------------------
# This comes first on purpose: `turf run` and a plain `turf build` rewrite a stale
# turf.lock, so any earlier command would repair the copy and hide the drift.
# --locked fails if turf.lock would change, and --offline that nothing may touch
# the network, so together they prove the committed file is complete and that
# building needs nothing but the checkout (--locked alone may still clone).
for p in geometry plotter; do
  t "locked_$p" "$EX/$p" build --locked --offline
  expect_rc "turf build --locked --offline in $p" "locked_$p" 0
  same "$p: a locked build leaves turf.lock as committed" "examples/$p/turf.lock" "$EX/$p/turf.lock"
done

# --- the program -------------------------------------------------------------------
t run_plotter "$EX/plotter" run
expect_rc "turf run in plotter" run_plotter 0
# The lines main.tr prints: each one is what a step of the example proves.
expect_out "plotter: a package's own internal module" run_plotter "distance squared: 25"
expect_out "plotter: a second function through the internal module" run_plotter "manhattan: 7"
expect_out "plotter: a generic function called with plotter's own struct" run_plotter "picked the sprite drawn as *"
expect_out "plotter: sizeof(T) is the caller's type, not a stand-in" run_plotter "a Sprite is 32 bytes"
expect_out "plotter: the same function with int" run_plotter "an int is 8 bytes"
[ -x "$EX/plotter/build/plotter" ] || fail "turf run in plotter: build/plotter was not left behind"

# --- the library ---------------------------------------------------------------------
t check_geometry "$EX/geometry" check
expect_rc "turf check in geometry" check_geometry 0
# examples/geometry has no src/main.tr (it is only a library), so `turf run`
# says what to create rather than just that nothing can run.
t run_geometry "$EX/geometry" run
expect_rc "turf run in a library with no main.tr" run_geometry 1
expect_out "turf run in a library with no main.tr" run_geometry "has no src/main.tr, so there is nothing to run"
expect_out "turf run in a library with no main.tr" run_geometry "fn main(): int"

if [ "$FAILED" -ne 0 ]; then
  echo "examples_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "examples_smoke: all checks passed"
