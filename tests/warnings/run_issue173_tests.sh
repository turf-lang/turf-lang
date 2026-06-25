#!/usr/bin/env bash
# run_issue173_tests.sh
# Tests for Issue #173: TurfWarning - non-interrupting warning infrastructure.
#
# Each test verifies three acceptance criteria:
#   1. Compilation succeeds (turf exits 0 and emits "Successfully compiled")
#   2. At least one "warning" diagnostic appears in compiler output
#   3. The compiled binary runs and produces the expected output

set -uo pipefail
# Navigate to project root (two levels up from tests/warnings/)
cd "$(dirname "$0")/../.."

TURF="./build/turf"
PASS=0
FAIL=0

run_test() {
  local name="$1"
  local file="$2"
  local expected_program_output="$3"
  local expected_warning_fragment="$4"

  echo "--- $name ---"

  local compiler_output compile_exit
  compiler_output=$("$TURF" "$file" 2>&1) && compile_exit=0 || compile_exit=$?

  local ok=true

  # 1. Compiler must exit 0
  if [ "$compile_exit" -ne 0 ]; then
    echo "  FAIL: compiler exited with code $compile_exit (expected 0)"
    echo "  compiler output: $compiler_output"
    ok=false
  fi

  # 2. Compiler output must contain a "warning" diagnostic
  if ! echo "$compiler_output" | grep -qi "$expected_warning_fragment"; then
    echo "  FAIL: compiler output does not contain '$expected_warning_fragment'"
    echo "  compiler output was:"
    echo "$compiler_output" | sed 's/^/    /'
    ok=false
  fi

  # 3. If compilation succeeded, the binary must run and produce expected output
  if [ "$compile_exit" -eq 0 ]; then
    local program_output program_exit
    program_output=$(./program 2>&1) && program_exit=0 || program_exit=$?

    if [ "$program_exit" -ne 0 ]; then
      echo "  FAIL: compiled binary exited with code $program_exit"
      ok=false
    fi

    if ! echo "$program_output" | grep -qF "$expected_program_output"; then
      echo "  FAIL: binary stdout does not contain '$expected_program_output'"
      echo "  binary output was: $program_output"
      ok=false
    fi
  fi

  if $ok; then
    echo "  PASS"
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
  fi
  echo
}

# Test 1: Shadowing warning — three nested variable shadows, must still print "30"
run_test \
  "Issue #173 T1: Shadowing warning does not halt compilation" \
  "tests/warnings/issue173_shadowing_warn.tr" \
  "30" \
  "warning"

# Test 2: Shadowing in conditional — inner 'y' shadows outer 'y', must still print "inner"
run_test \
  "Issue #173 T2: Shadowing warning in conditional does not halt compilation" \
  "tests/warnings/issue173_redundant_cmp_warn.tr" \
  "inner" \
  "warning"

# Test 3: Self-assignment warning — x = x, must still print "42"
run_test \
  "Issue #173 T3: Self-assignment warning does not halt compilation" \
  "tests/warnings/issue173_self_assign_warn.tr" \
  "42" \
  "warning"

echo "================================"
echo "Results: $PASS passed, $FAIL failed"
echo "================================"

exit $FAIL
