#!/bin/bash
#
# tree_programs_smoke.sh -- REL-004 stage 2: the Turf files `test_runner.sh`
# never touches. The runner covers tests/ only, so a stdlib module or a
# showcase program left on removed syntax sat broken with the suite green
# (AGENTS.md records a confirmed instance). Until now the only check was
# session_postflight.sh, which a CI run never executes; this is the same
# coverage as a CI step, plus the debug-info property of Part VII.
#
#   1. every lib/std module passes `--check --module` (type-checks as a
#      library: a pure library file has no main(), so it cannot be compiled
#      as a program);
#   2. the nine prelude modules compiled *directly* with `--resolve`: the only
#      regression coverage for the prelude-self-import bug class (a module's
#      own UFCS self-call reported "Ambiguous ... call"). Run from a scratch
#      directory because `--resolve` writes its binary into the cwd;
#   3. every showcase/*.tr compiles to an executable at -O0 and -O2;
#   4. every showcase/*.tr compiled with -g passes `llvm-dwarfdump --verify`
#      (the Part VII "Debug info" row). Skipped, loudly, when the tool is not
#      on PATH -- unless CI is set, where a missing tool is a failure;
#   5. every examples/ project is built by examples_smoke.sh, not here.
#
# Usage: scripts/smoke/tree_programs_smoke.sh        (after building build/turfc)
# Exit:  0 = every check passed; 1 = at least one failed (each is listed).

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO="$(pwd)"

TURF="${TURF_COMPILER:-$REPO/build/turfc}"
[ -x "$TURF" ] || { echo "FAIL: $TURF not built"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
strip() { sed 's/\x1b\[[0-9;]*m//g'; }

FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }
firsterrors() { strip | grep -E '^── error|^Error:|ambiguous|Ambiguous' | head -3 | sed 's/^/       /'; }

# ── 1. lib/std, as libraries ────────────────────────────────────────────────
STD_COUNT=0
while IFS= read -r f; do
  STD_COUNT=$((STD_COUNT + 1))
  out=$("$TURF" "$f" --check --module 2>&1); rc=$?
  if [ $rc -ne 0 ]; then
    fail "$f does not pass --check --module (exit $rc)"
    echo "$out" | firsterrors
  fi
done < <(find lib/std -name '*.tr' | sort)
echo "lib/std: $STD_COUNT modules checked as libraries"

# ── 2. the prelude modules, compiled directly ───────────────────────────────
# The list is read from the compiler's own PreludeModuleNames rather than
# restated, so a tenth prelude module is covered the day it is added.
PRELUDE=$(grep -A1 'PreludeModuleNames\[\] = {' src/frontend/ParserModule.cpp | grep -oE '"[a-z0-9_]+"' | tr -d '"')
[ -n "$PRELUDE" ] || fail "could not read PreludeModuleNames from src/frontend/ParserModule.cpp"
PRELUDE_COUNT=0
mkdir "$WORK/resolve"
for m in $PRELUDE; do
  f=$(find "$REPO/lib/std/core" -name "$m.tr" | head -1)
  if [ -z "$f" ]; then fail "prelude module '$m' has no lib/std/core/$m.tr"; continue; fi
  PRELUDE_COUNT=$((PRELUDE_COUNT + 1))
  out=$(cd "$WORK/resolve" && "$TURF" --resolve "$f" 2>&1 | strip)
  # E119 (no entry point) is the one expected complaint of a library file.
  errs=$(echo "$out" | grep -E '^── error|^Error:' | grep -v -e '\[E119[] ·]' -e 'No entry point found')
  if [ -n "$errs" ] || echo "$out" | grep -qi 'ambiguous'; then
    fail "prelude module $f does not resolve cleanly when compiled directly"
    echo "$out" | firsterrors
  fi
done
echo "prelude: $PRELUDE_COUNT modules compiled directly"

# ── 3 + 4. showcase programs ────────────────────────────────────────────────
DWARF=$(command -v llvm-dwarfdump || true)
if [ -z "$DWARF" ]; then
  if [ -n "${CI:-}" ]; then fail "llvm-dwarfdump is not on PATH (required when CI is set)"
  else echo "SKIP: llvm-dwarfdump not on PATH; showcase DWARF is not verified"; fi
fi

SHOW_COUNT=0
while IFS= read -r f; do
  SHOW_COUNT=$((SHOW_COUNT + 1))
  name=$(basename "$f" .tr)
  for lvl in -O0 -O2; do
    if ! out=$("$TURF" "$f" "$lvl" -o "$WORK/$name$lvl" 2>&1); then
      fail "$f does not compile at $lvl"
      echo "$out" | firsterrors
    fi
  done
  if ! out=$("$TURF" "$f" -g -O0 -o "$WORK/$name.g" 2>&1); then
    fail "$f does not compile with -g"
    echo "$out" | firsterrors
  elif [ -n "$DWARF" ]; then
    if ! vout=$("$DWARF" --verify "$WORK/$name.g" 2>&1); then
      fail "$f: llvm-dwarfdump --verify reports errors in the -g output"
      echo "$vout" | head -5 | sed 's/^/       /'
    fi
  fi
  rm -f "$WORK/$name"-O0 "$WORK/$name"-O2 "$WORK/$name.g"
done < <(find showcase -name '*.tr' | sort)
echo "showcase: $SHOW_COUNT programs compiled at -O0, -O2 and -g"

[ "$STD_COUNT" -gt 0 ] && [ "$SHOW_COUNT" -gt 0 ] || fail "a tree was empty -- the checks above proved nothing"

if [ $FAILED -ne 0 ]; then echo "$FAILED check(s) failed"; exit 1; fi
echo "PASS: tree_programs_smoke"
