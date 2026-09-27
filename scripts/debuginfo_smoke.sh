#!/bin/bash
#
# debuginfo_smoke.sh -- TOOL-001's own regression coverage for `-g`.
# (docs/planning/ARCHITECTURE_OVERHAUL.md §8 TOOL-001.) Not a `.tr` test:
# `test_runner.sh` has no way to assert "gdb can set a breakpoint by
# TurfLang line, show a backtrace with TurfLang function names, and print a
# local" -- that needs an actual debugger driving an actual binary, which is
# exactly what this script does. Gated on `gdb` being present (see below)
# so CI without a debugger installed SKIPS this rather than failing the
# whole suite over a missing, unrelated tool.
#
# What this proves, against a small fixture compiled at -O0 -g:
#   1. `llvm-dwarfdump --verify` finds no errors in the emitted DWARF.
#   2. gdb can set a breakpoint by TurfLang source line (not an address).
#   3. gdb prints a local's correct VALUE at that breakpoint.
#   4. gdb's backtrace shows real TurfLang function names and the correct
#      caller line, across a real call (main -> add), not just frame 0.
#
# Usage: scripts/debuginfo_smoke.sh
# Exit:  0 = every check passed (or gdb/llvm-dwarfdump is absent -- skip,
#            not fail); 1 = at least one check failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

TURF="./build/turf"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turf missing -- build before running this smoke test.${RESET}"
  exit 1
fi

if ! command -v gdb > /dev/null 2>&1; then
  echo -e "${YELLOW}gdb not found -- skipping debuginfo_smoke (not a failure).${RESET}"
  exit 0
fi

FAILED=0
pass() { echo -e "   ${GREEN}✓${RESET} $1"; }
fail() { echo -e "   ${RED}✗${RESET} $1"; FAILED=$((FAILED + 1)); }
note() { echo -e "   ${YELLOW}!${RESET} $1"; }

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

# A small fixture exercising exactly what the completion criteria name: a
# real call (so a backtrace has two real TurfLang frames to show), a local
# with a known value at the breakpoint line, and a parameter (gdb prints
# both automatically the moment it stops).
FIXTURE="$WORKDIR/fixture.tr"
cat > "$FIXTURE" << 'EOF'
fn add(a: int, b: int): int {
    mutable sum: int = a + b;
    return sum;
}

fn main(): int {
    mutable x: int = 5;
    mutable y: int = add(x, 10);
    return y - 15;
}
EOF

echo -e "${BOLD}1. Compile at -O0 -g${RESET}"
BIN="$WORKDIR/fixture"
if ! "$TURF" "$FIXTURE" -O0 -g -o "$BIN" > "$WORKDIR/compile.log" 2>&1; then
  fail "fixture does not compile at -O0 -g:"; sed 's/^/       /' "$WORKDIR/compile.log"
  echo -e "\n${RED}${BOLD}debuginfo_smoke found $FAILED problem(s).${RESET}"
  exit 1
fi
pass "compiled to $BIN"

echo -e "\n${BOLD}2. llvm-dwarfdump --verify${RESET}"
DWARFDUMP=""
for cand in llvm-dwarfdump llvm-dwarfdump-22; do
  if command -v "$cand" > /dev/null 2>&1; then DWARFDUMP="$cand"; break; fi
done
if [ -z "$DWARFDUMP" ]; then
  note "no llvm-dwarfdump on PATH -- skipping DWARF verification, not a failure"
else
  VERIFY_OUT=$("$DWARFDUMP" --verify "$BIN" 2>&1)
  if echo "$VERIFY_OUT" | grep -q "No errors."; then
    pass "$DWARFDUMP --verify: no errors"
  else
    fail "$DWARFDUMP --verify found problems:"
    echo "$VERIFY_OUT" | grep -i error | head -10 | sed 's/^/       /'
  fi
fi

echo -e "\n${BOLD}3. gdb: breakpoint by TurfLang line, print a local, backtrace${RESET}"
GDB_OUT=$(gdb --batch \
  -ex "break fixture.tr:3" \
  -ex "run" \
  -ex "print sum" \
  -ex "backtrace" \
  "$BIN" 2>&1)

if echo "$GDB_OUT" | grep -qE "Breakpoint 1,.*at fixture\.tr:3"; then
  pass "breakpoint set and hit by TurfLang source line (fixture.tr:3)"
else
  fail "breakpoint did not stop at fixture.tr:3 -- gdb output:"
  echo "$GDB_OUT" | sed 's/^/       /'
fi

if echo "$GDB_OUT" | grep -qE '\$1 = 15'; then
  pass "printed local 'sum' with the correct value (15)"
else
  fail "'print sum' did not show the expected value (15)"
fi

if echo "$GDB_OUT" | grep -q "add (a=5, b=10)" || echo "$GDB_OUT" | grep -qE "add\b.*a=5.*b=10"; then
  pass "backtrace shows 'add' with correct parameter values"
else
  fail "backtrace did not show 'add(a=5, b=10)' -- gdb output:"
  echo "$GDB_OUT" | sed 's/^/       /'
fi

if echo "$GDB_OUT" | grep -qE "#1 .*main \(\) at fixture\.tr:8"; then
  pass "backtrace shows the real caller (main, fixture.tr:8)"
else
  fail "backtrace did not show main() as frame #1 at fixture.tr:8 -- gdb output:"
  echo "$GDB_OUT" | sed 's/^/       /'
fi

# FDN-003 (docs/development/changes/FDN-003.md): a type's identity is
# (module, name), and its DWARF name is `module.Name` -- the same spelling
# as its LLVM struct and GC descriptor (TypeIdentity::llvmName()). A
# module-owned `Point` and a main-file `Point` must reach the debugger as two
# differently-named types, or `ptype`/`print` would show one type's layout
# for the other's values. `mine` is initialized directly by `Point::default()`
# on purpose: that path used to skip the DW_TAG_variable entirely, and a
# local typed by struct NAME used to get an unspecified debug type (no
# fields) -- the last check here prints it through gdb.
echo -e "\n${BOLD}4. DWARF names of module-owned types (FDN-003)${RESET}"
if [ -z "$DWARFDUMP" ]; then
  note "no llvm-dwarfdump on PATH -- skipping, not a failure"
else
  cat > "$WORKDIR/shapes_module.tr" << 'EOF2'
struct Point { x: int }
enum Kind { A, B }
fn origin(): Point { return Point::default(); }
fn kind(): Kind { return Kind::B; }
EOF2
  cat > "$WORKDIR/names.tr" << 'EOF2'
import shapes_module;
struct Point { y: double }
fn main(): int {
    mutable theirs: shapes_module::Point = shapes_module::origin();
    mutable mine: Point = Point::default();
    mutable k: shapes_module::Kind = shapes_module::kind();
    theirs.x = 1;
    mine.y = 2.0;
    mutable result: int = theirs.x;
    return result - 1;
}
EOF2
  NAMES_BIN="$WORKDIR/names"
  if ! "$TURF" "$WORKDIR/names.tr" -O0 -g -o "$NAMES_BIN" > "$WORKDIR/names.log" 2>&1; then
    fail "FDN-003 fixture does not compile at -O0 -g:"; sed 's/^/       /' "$WORKDIR/names.log"
  else
    NAMES_OUT=$("$DWARFDUMP" --debug-info "$NAMES_BIN" 2>&1)
    for want in 'shapes_module.Point' 'shapes_module.Kind' 'Point'; do
      if echo "$NAMES_OUT" | grep -qF "DW_AT_name	(\"$want\")"; then
        pass "DWARF names the type '$want'"
      else
        fail "no DWARF type named '$want'"
      fi
    done
    if "$DWARFDUMP" --verify "$NAMES_BIN" 2>&1 | grep -q "No errors."; then
      pass "$DWARFDUMP --verify: no errors with same-named types in two modules"
    else
      fail "$DWARFDUMP --verify found problems in the FDN-003 fixture"
    fi
    NAMES_GDB=$(gdb --batch -ex "break names.tr:10" -ex "run" \
      -ex "print mine" -ex "print theirs" "$NAMES_BIN" 2>&1)
    if echo "$NAMES_GDB" | grep -qE '\$1 = \{y = 2\}' &&
       echo "$NAMES_GDB" | grep -qE '\$2 = \{x = 1\}'; then
      pass "gdb prints a T::default()-initialized local and a module struct field-by-field"
    else
      fail "gdb could not print 'mine'/'theirs' with their fields -- gdb output:"
      echo "$NAMES_GDB" | sed 's/^/       /'
    fi
  fi
fi

echo
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}${BOLD}debuginfo_smoke: clean.${RESET} -g output verifies and gdb can debug it."
  exit 0
fi
echo -e "${RED}${BOLD}debuginfo_smoke found $FAILED problem(s).${RESET}"
exit 1
