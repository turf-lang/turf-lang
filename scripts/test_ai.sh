#!/bin/bash

set -u

TEST_DIR="tests/custom/aggressive"
COMPILER="./turfc"
RUN_ROOT="ai_outputs/runs"
STAMP=$(date +%Y%m%d_%H%M%S)
RUN_DIR="$RUN_ROOT/$STAMP"

if [ ! -x "$COMPILER" ]; then
  echo "Compiler not found at $COMPILER. Please build Turf first."
  exit 1
fi

mkdir -p "$RUN_DIR"

TOTAL=0
PASSED=0
FAILED=0

for test_file in "$TEST_DIR"/*.tr; do
  if [ ! -f "$test_file" ]; then
    continue
  fi

  TOTAL=$((TOTAL + 1))
  test_name=$(basename "$test_file" .tr)
  log_file="$RUN_DIR/$test_name.log"

  TURF_SANDBOX_DEBUG=1 "$COMPILER" "$test_file" --smart >"$log_file" 2>&1

  if grep -q "✓ Fix validated and committed" "$log_file"; then
    echo "PASS $test_name"
    PASSED=$((PASSED + 1))
  else
    echo "FAIL $test_name"
    FAILED=$((FAILED + 1))
  fi
done

echo "Total:$TOTAL Passed:$PASSED Failed:$FAILED"

if [ "$FAILED" -ne 0 ]; then
  exit 1
fi

exit 0
