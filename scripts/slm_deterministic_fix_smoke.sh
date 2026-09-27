#!/bin/bash
#
# slm_deterministic_fix_smoke.sh -- SLM-001's own regression coverage for
# §16.3 item 1 (the deterministic-fix layer): "Where a diagnostic already
# carries a unique, high-confidence suggestion ... synthesize the patch
# directly, sandbox-verify it, and never invoke the model."
# (docs/planning/ARCHITECTURE_OVERHAUL.md §16.3, §8's SLM-001 section.)
#
# Why this can't be a `tests/*.tr` file: `test_runner.sh` never passes
# `--smart` to any compile it runs (per DIAG-001's own testing-scope note,
# SLM classification/routing isn't `.tr`-testable), so a `--smart`-only
# behaviour needs its own harness -- same reasoning `opt_levels_smoke.sh`
# already documents for `-O` levels.
#
# Why this ISN'T the same as `scripts/test_ai.sh` (the existing, slower,
# genuinely model-backed harness under tests/custom/aggressive/): the whole
# point of the deterministic-fix layer is that it resolves these diagnostics
# WITHOUT ever loading the SLM -- so this script also proves that by timing
# every run against a generous ceiling no real model load could ever clear
# (loading the vendored qwen2.5-coder GGUF into RAM takes several seconds
# minimum; a model-free sandbox-only fix completes in well under a second on
# any machine that can build this compiler at all). A regression that
# silently made a deterministic-fix case fall through to the model would
# still "pass" on message content alone but blow through the timing ceiling
# here, which is exactly the failure mode this script exists to catch.
#
# Also covers the SLM-001 follow-up: `--smart-write-fix=<path>` writes the
# full, validated, patched source to a real file (not just a rendered
# diagnostic) -- this script confirms that written file actually compiles
# AND runs to exit 0 for both fixtures, i.e. is a genuine, working program,
# not merely something that satisfied the Sandbox's own (Codegen-free)
# recheck.
#
# Usage: scripts/slm_deterministic_fix_smoke.sh
# Exit:  0 = every fixture resolved deterministically, with no model call,
#            and the written fix compiles and runs correctly;
#        1 = at least one fixture didn't validate, fell through to the
#            model, or its written fix didn't compile/run.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

TURF="./build/turf"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turf missing -- build before running this smoke test.${RESET}"
  exit 1
fi

# A generous ceiling, not a tight latency assertion: this is here purely to
# distinguish "resolved via sandboxed search/replace" (milliseconds) from
# "silently fell through and actually called the model" (several seconds
# just to load the GGUF, before a single token is even generated). Anything
# under this is unambiguous proof the model path was never entered.
CEILING_SECONDS=5

# Each fixture's OLD and NEW identifier, so a change to either fixture file
# doesn't silently desync from what this script asserts happened.
FIXTURES=(
  "tests/44_slm_diagnostics/fail_deterministic_typo_variable.tr:cuont:count"
  "tests/44_slm_diagnostics/fail_deterministic_typo_keyword.tr:retrun:return"
)

FAILED=0

# A stale/adversarial cache entry from a previous run must never be able to
# fake a "no model called" result -- clear it first so every run of this
# script genuinely exercises tryDeterministicFix() fresh, not a cache hit
# that happens to look the same.
rm -rf "${HOME:-/nonexistent}/.cache/turf/slm"

for fixture in "${FIXTURES[@]}"; do
  file="${fixture%%:*}"
  rest="${fixture#*:}"
  old="${rest%%:*}"
  new="${rest#*:}"

  if [ ! -f "$file" ]; then
    echo -e "${RED}✗${RESET} $file -- fixture missing"
    FAILED=$((FAILED + 1))
    continue
  fi

  fixed_out="/tmp/turf_slm_smoke_fixed_$$.tr"
  fixed_bin="/tmp/turf_slm_smoke_fixed_bin_$$"
  rm -f "$fixed_out" "$fixed_bin"

  start=$(date +%s.%N)
  output=$("$TURF" --smart "--smart-write-fix=$fixed_out" "$file" 2>&1)
  end=$(date +%s.%N)
  elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

  ok=1
  if ! grep -q "\[Deterministic Fix\]" <<<"$output"; then
    echo -e "${RED}✗${RESET} $file -- no [Deterministic Fix] line in output"
    ok=0
  fi
  if ! grep -q "the model was never called" <<<"$output"; then
    echo -e "${RED}✗${RESET} $file -- output didn't confirm the model was skipped"
    ok=0
  fi
  if ! grep -q "'$old' -> '$new'" <<<"$output"; then
    echo -e "${RED}✗${RESET} $file -- expected rename '$old' -> '$new' not reported"
    ok=0
  fi
  if awk -v e="$elapsed" -v c="$CEILING_SECONDS" 'BEGIN { exit !(e > c) }'; then
    echo -e "${RED}✗${RESET} $file -- took ${elapsed}s (> ${CEILING_SECONDS}s ceiling); looks like it fell through to the model"
    ok=0
  fi

  # Deliberately NOT grepping the written file's text for "$old"/"$new":
  # this fixture's own doc comment names both words in prose (describing
  # what it tests), so a naive substring check false-positives on the
  # COMMENT, not the code -- found live while writing this check. Whether
  # the fix is real is instead proven the strong way: the written file
  # must actually compile and run to completion, which a stray uncorrected
  # '$old' token in the live code could never do.
  if [ ! -s "$fixed_out" ]; then
    echo -e "${RED}✗${RESET} $file -- --smart-write-fix didn't write a (non-empty) fixed file"
    ok=0
  elif ! "$TURF" "$fixed_out" -o "$fixed_bin" > /dev/null 2>&1 || [ ! -x "$fixed_bin" ]; then
    echo -e "${RED}✗${RESET} $file -- written fix does not compile"
    ok=0
  elif ! "$fixed_bin" > /dev/null 2>&1; then
    echo -e "${RED}✗${RESET} $file -- written fix compiled but exited non-zero when run"
    ok=0
  fi
  rm -f "$fixed_out" "$fixed_bin"

  if [ "$ok" -eq 1 ]; then
    echo -e "${GREEN}✓${RESET} $file -- '$old' -> '$new' in ${elapsed}s, model never called, written fix compiles and runs"
  else
    FAILED=$((FAILED + 1))
    echo "$output" | sed 's/^/    /'
  fi
done

echo
if [ "$FAILED" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All deterministic-fix fixtures resolved without the model.${RESET}"
  exit 0
fi
echo -e "${RED}${BOLD}$FAILED fixture(s) failed.${RESET}"
exit 1
