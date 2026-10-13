#!/bin/bash
#
# opt_levels_smoke.sh -- OPT-001's own regression coverage for F-OPT-1:
# "Optimization levels change compile time, run time, binary size and
# debuggability. They never change observable behaviour."
# (docs/development/FREEZE.md §6.10, docs/planning/ARCHITECTURE_OVERHAUL.md
# §17.3.) `test_runner.sh` cannot exercise this at all -- it only ever
# compiles at the compiler's default level, so a level-specific regression
# (e.g. an `-O3` transform racing ahead of a runtime check) would be
# invisible to the whole rest of the test suite.
#
# What this proves: for every program in the DETERMINISTIC set below,
# compile it at -O0/-O1/-O2/-O3/-Os and assert byte-identical stdout AND an
# identical exit code at every level, against -O0 as the reference. For the
# NONDETERMINISTIC set (a program whose own output legitimately varies from
# run to run for reasons that have nothing to do with the optimizer -- an
# unseeded random roll, a printed elapsed-time measurement), a literal
# stdout diff would produce a FALSE failure on every single run regardless
# of optimizer correctness, so those are compiled at every level (still
# proving they build) and run once per level with only the EXIT CODE
# checked. For the SERVER/CLIENT set (a program that either blocks forever
# listening for connections, or requires a live peer already running), only
# compilation is checked -- there is no way to "run to completion and check
# output" for a process that never completes on its own.
#
# Usage: scripts/smoke/opt_levels_smoke.sh
# Exit:  0 = every level agrees; 1 = at least one program disagrees or fails
#            to compile/run at some level.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

TURF="./build/turfc"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi

LEVELS=(-O0 -O1 -O2 -O3 -Os)
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

FAILED=0
declare -A SIZES   # "program level" -> byte size, printed in the final table

pass() { echo -e "   ${GREEN}✓${RESET} $1"; }
fail() { echo -e "   ${RED}✗${RESET} $1"; FAILED=$((FAILED + 1)); }
note() { echo -e "   ${YELLOW}!${RESET} $1"; }

# Compiles $1 at level $2 into $WORKDIR/<basename>_<level>. Prints the
# compiler's stderr and returns non-zero on a compile failure; on success,
# records the binary's size in SIZES.
compile_one() {
  local src="$1" level="$2" base out
  base=$(basename "$src" .tr)
  out="$WORKDIR/${base}_${level}"
  if ! "$TURF" "$src" "$level" -o "$out" > "$WORKDIR/${base}_${level}.compile.log" 2>&1; then
    return 1
  fi
  SIZES["$base $level"]=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out" 2>/dev/null)
  return 0
}

# ── Deterministic set: exact stdout + exit-code equality across all levels ──
# tests/08_e2e_stress/*.tr -- pure computation, no I/O source of variance.
# showcase/llvm_ffi_selfhost_demo.tr -- §8 OPT-001's own named risk case:
# "the most FFI-dense program in the tree", the one Internalize/GlobalDCE
# could most plausibly break by internalizing something an FFI callback
# still needed.
DETERMINISTIC=()
while IFS= read -r f; do DETERMINISTIC+=("$f"); done < <(find tests/08_e2e_stress -name "*.tr" | sort)
DETERMINISTIC+=("showcase/llvm_ffi_selfhost_demo.tr")

echo -e "${BOLD}1. Deterministic set -- exact output + exit code must match -O0${RESET}"
for src in "${DETERMINISTIC[@]}"; do
  base=$(basename "$src" .tr)
  if ! compile_one "$src" "-O0"; then
    fail "$src does not compile at -O0:"; head -5 "$WORKDIR/${base}_-O0.compile.log" | sed 's/^/       /'
    continue
  fi
  "$WORKDIR/${base}_-O0" > "$WORKDIR/${base}_-O0.out" 2>&1
  REF_EXIT=$?
  ONE_BAD=0
  for level in "${LEVELS[@]:1}"; do
    if ! compile_one "$src" "$level"; then
      fail "$src does not compile at $level:"; head -5 "$WORKDIR/${base}_${level}.compile.log" | sed 's/^/       /'
      ONE_BAD=1
      continue
    fi
    "$WORKDIR/${base}_${level}" > "$WORKDIR/${base}_${level}.out" 2>&1
    THIS_EXIT=$?
    if [ "$THIS_EXIT" -ne "$REF_EXIT" ]; then
      fail "$src: exit code differs at $level (got $THIS_EXIT, -O0 was $REF_EXIT)"
      ONE_BAD=1
    elif ! diff -q "$WORKDIR/${base}_-O0.out" "$WORKDIR/${base}_${level}.out" > /dev/null; then
      fail "$src: stdout differs at $level vs -O0:"
      diff "$WORKDIR/${base}_-O0.out" "$WORKDIR/${base}_${level}.out" | head -6 | sed 's/^/       /'
      ONE_BAD=1
    fi
  done
  [ $ONE_BAD -eq 0 ] && pass "$src identical at every level (exit $REF_EXIT)"
done

# ── Nondeterministic set: build at every level, exit code only ─────────────
# showcase/config_and_dice.tr -- random::random_int() is unseeded by design
# (a dice roller that always rolled the same numbers wouldn't demonstrate
# anything); showcase/text_toolkit.tr -- prints a real time::elapsed_ms()
# measurement. Neither's stdout is optimizer-invariant BY DESIGN -- a
# faster -O3 binary is legitimately expected to report a smaller elapsed_ms
# than -O0 -- so a byte diff here would be testing the machine's clock
# jitter, not the compiler.
NONDETERMINISTIC=(showcase/config_and_dice.tr showcase/text_toolkit.tr)

echo -e "\n${BOLD}2. Nondeterministic set -- builds + exits 0 at every level (no stdout diff)${RESET}"
for src in "${NONDETERMINISTIC[@]}"; do
  base=$(basename "$src" .tr)
  ONE_BAD=0
  for level in "${LEVELS[@]}"; do
    if ! compile_one "$src" "$level"; then
      fail "$src does not compile at $level:"; head -5 "$WORKDIR/${base}_${level}.compile.log" | sed 's/^/       /'
      ONE_BAD=1
      continue
    fi
    if ! timeout 10 "$WORKDIR/${base}_${level}" > /dev/null 2>&1; then
      fail "$src exits non-zero (or times out) at $level"
      ONE_BAD=1
    fi
  done
  [ $ONE_BAD -eq 0 ] && pass "$src builds and exits 0 at every level"
done

# ── Server/client set: compile-only ─────────────────────────────────────────
# These either block forever (a server) or require a live peer already
# listening (a client) -- there is no "run to completion" for either, so
# only compilation is checked. Their genuinely-interesting cross-level risk
# (does the socket/accept-loop code miscompile?) is exercised manually, the
# same way the rest of the showcase/ directory's server programs always
# have been -- see AGENTS.md on why showcase/ has no automated runner.
SERVER_CLIENT=(showcase/http_server.tr showcase/http_client.tr showcase/kv_store_server.tr showcase/kv_store_client.tr)

echo -e "\n${BOLD}3. Server/client set -- compiles at every level (not run)${RESET}"
for src in "${SERVER_CLIENT[@]}"; do
  base=$(basename "$src" .tr)
  ONE_BAD=0
  for level in "${LEVELS[@]}"; do
    if ! compile_one "$src" "$level"; then
      fail "$src does not compile at $level:"; head -5 "$WORKDIR/${base}_${level}.compile.log" | sed 's/^/       /'
      ONE_BAD=1
    fi
  done
  [ $ONE_BAD -eq 0 ] && pass "$src compiles at every level"
done

# ── Binary-size table (change-record material, §8 OPT-001 stage 6) ─────────
echo -e "\n${BOLD}4. Binary sizes by level (bytes)${RESET}"
ALL_PROGRAMS=()
for src in "${DETERMINISTIC[@]}" "${NONDETERMINISTIC[@]}" "${SERVER_CLIENT[@]}"; do
  ALL_PROGRAMS+=("$(basename "$src" .tr)")
done
printf "   %-28s" "program"
for level in "${LEVELS[@]}"; do printf "%10s" "$level"; done
printf "\n"
for prog in "${ALL_PROGRAMS[@]}"; do
  printf "   %-28s" "$prog"
  for level in "${LEVELS[@]}"; do
    printf "%10s" "${SIZES["$prog $level"]:-"-"}"
  done
  printf "\n"
done

echo
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}${BOLD}opt_levels_smoke: clean.${RESET} Every program agrees across -O0..-O3/-Os."
  exit 0
fi
echo -e "${RED}${BOLD}opt_levels_smoke found $FAILED problem(s).${RESET}"
exit 1
