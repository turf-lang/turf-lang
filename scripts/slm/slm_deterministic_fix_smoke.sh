#!/bin/bash
#
# slm_deterministic_fix_smoke.sh -- the compiler's own fixes (TDP-2-001) end to
# end, with and without --smart.
#
# `turfc --test` already pins every printed fix (`// EXPECT-FIX:`) and checks
# that the program with its fixes applied has none of the errors they fix. What
# it cannot see is the rest of the promise, so this script checks it:
#
#   1. Without --smart, each fixture prints the `Fix:` its EXPECT-FIX marker
#      names, and the program --smart-write-fix writes compiles AND runs to
#      exit 0 (the test runner analyses the fixed program; it never builds it).
#   2. With --smart, the same fix is printed and the model is never asked: no
#      `[Model Suggestion]`, and the compile finishes well under the time a
#      model takes just to load.
#   3. A clean compile never enters the fix path (TURF_FIX_DEBUG=1 prints a
#      `[fix]` line for every fix it checks).
#
# Usage: scripts/slm/slm_deterministic_fix_smoke.sh
# Exit:  0 = every check held; 1 = at least one did not.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

TURF="$PWD/build/turfc"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi
unset TURF_HOME

# Far below the seconds a model needs just to load; a fixture that fell through
# to the model could not finish under it.
CEILING_SECONDS=5

# Each fixture's fixed program must compile and run to exit 0, and must not
# import a sibling file (the fixed copy is written to a temporary directory).
FIXTURES=(
  tests/44_slm_diagnostics/fail_deterministic_typo_variable.tr
  tests/44_slm_diagnostics/fail_deterministic_typo_keyword.tr
  tests/15_enums/fail_enum_wrong_variant.tr
  tests/06_structs/fail_misspelled_method_struct.tr
  tests/38_monomorphization/fail_method_on_generic_struct_without_type_params.tr
  tests/01_syntax/fail_missing_semicolon_end_of_line.tr
  tests/04_control_flow/fail_do_while_missing_semicolon.tr
  tests/03_typechecker/fail_immutable_compound_assignment.tr
  tests/07_functions_modules/visibility/fail_pub_on_operator.tr
  tests/07_functions_modules/visibility/fail_pub_enum_variant.tr
  tests/07_functions_modules/visibility/fail_pub_before_decorator.tr
  tests/07_functions_modules/fail_dot_call_on_module_ref.tr
  tests/07_functions_modules/fail_dot_call_on_struct_static.tr
  tests/07_functions_modules/fail_scope_operator_on_instance_call.tr
  tests/07_functions_modules/fail_static_method_via_instance.tr
  tests/06_structs/fail_constructor_cannot_be_static.tr
)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0
strip() { sed 's/\x1b\[[0-9;]*m//g'; }

for file in "${FIXTURES[@]}"; do
  if [ ! -f "$file" ]; then
    echo -e "${RED}✗${RESET} $file -- fixture missing"
    FAILED=$((FAILED + 1))
    continue
  fi
  dir=$(dirname "$file"); name=$(basename "$file")
  fixed="$WORK/fixed.tr"; bin="$WORK/fixed_bin"
  rm -f "$fixed" "$bin"
  ok=1

  mapfile -t want < <(sed -n 's|^[[:space:]]*//[[:space:]]*EXPECT-FIX:[[:space:]]*||p' "$file")
  if [ "${#want[@]}" -eq 0 ]; then
    echo -e "${RED}✗${RESET} $file -- has no // EXPECT-FIX: marker"
    FAILED=$((FAILED + 1))
    continue
  fi

  plain=$(cd "$dir" && "$TURF" "$name" --check "--smart-write-fix=$fixed" 2>&1 | strip)
  start=$(date +%s.%N)
  smart=$(cd "$dir" && "$TURF" "$name" --check --smart 2>&1 | strip)
  end=$(date +%s.%N)
  elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

  for fix in "${want[@]}"; do
    grep -qxF "  Fix: $fix" <<<"$plain" || { echo -e "${RED}✗${RESET} $file -- no 'Fix: $fix' without --smart"; ok=0; }
    grep -qxF "  Fix: $fix" <<<"$smart" || { echo -e "${RED}✗${RESET} $file -- no 'Fix: $fix' with --smart"; ok=0; }
  done
  if grep -qF "[Model Suggestion]" <<<"$smart"; then
    echo -e "${RED}✗${RESET} $file -- --smart asked the model although the compiler had a fix"
    ok=0
  fi
  if awk -v e="$elapsed" -v c="$CEILING_SECONDS" 'BEGIN { exit !(e > c) }'; then
    echo -e "${RED}✗${RESET} $file -- --smart took ${elapsed}s (> ${CEILING_SECONDS}s); looks like it reached the model"
    ok=0
  fi
  if [ ! -s "$fixed" ]; then
    echo -e "${RED}✗${RESET} $file -- --smart-write-fix wrote no fixed program"
    ok=0
  elif ! "$TURF" "$fixed" -o "$bin" > /dev/null 2>&1 || [ ! -x "$bin" ]; then
    echo -e "${RED}✗${RESET} $file -- the fixed program does not compile"
    ok=0
  elif ! "$bin" > /dev/null 2>&1; then
    echo -e "${RED}✗${RESET} $file -- the fixed program compiled but exited non-zero"
    ok=0
  fi

  if [ "$ok" -eq 1 ]; then
    echo -e "${GREEN}✓${RESET} $file -- ${want[*]}; --smart in ${elapsed}s without the model; the fixed program runs"
  else
    FAILED=$((FAILED + 1))
    echo "$plain" | sed 's/^/    /'
  fi
done

# 3. A clean compile never checks a fix.
clean="$WORK/clean.tr"
printf 'fn main(): int {\n    mut x: int = 1;\n    x += 1;\n    return x - 2;\n}\n' > "$clean"
if TURF_FIX_DEBUG=1 "$TURF" "$clean" --check 2>&1 | grep -q '^\[fix\]'; then
  echo -e "${RED}✗${RESET} a clean compile entered the fix path"
  FAILED=$((FAILED + 1))
else
  echo -e "${GREEN}✓${RESET} a clean compile never enters the fix path"
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}Every compiler fix printed, checked and ran, with and without --smart.${RESET}"
  exit 0
fi
echo -e "${RED}${BOLD}$FAILED check(s) failed.${RESET}"
exit 1
