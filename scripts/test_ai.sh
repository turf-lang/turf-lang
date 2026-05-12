#!/bin/bash

TEST_DIR="tests/custom/aggressive"
COMPILER="./turf"
OUTPUT_DIR="ai_outputs"
RUN_DIR="$OUTPUT_DIR/runs"

if [ ! -f "$COMPILER" ]; then
    echo "Compiler not found at $COMPILER. Build Turf first."
    exit 1
fi

mkdir -p "$RUN_DIR"

STAMP=$(date +%Y%m%d_%H%M%S)
SESSION_DIR="$RUN_DIR/$STAMP"
mkdir -p "$SESSION_DIR"

TOTAL=0
PASSED=0
FAILED=0

echo "Running smart AI regression tests..."
echo "Output directory: $SESSION_DIR"

while IFS= read -r test_file; do
    TOTAL=$((TOTAL + 1))
    test_name=$(basename "$test_file" .tr)
    log_file="$SESSION_DIR/$test_name.log"

    (
        echo "============================================================"
        echo "Test: $test_file"
        echo "Command: $COMPILER --smart $test_file -o program"
        echo "Started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "============================================================"
        TURF_SANDBOX_DEBUG=1 "$COMPILER" --smart "$test_file" -o program
        exit_code=$?
        echo "Exit code: $exit_code"
        echo "Finished: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        rm -f program
    ) > "$log_file" 2>&1

    if grep -q "✓ Fix validated and committed" "$log_file"; then
        echo "PASS $test_name (sandbox validation succeeded)"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL $test_name (see $log_file)"
        FAILED=$((FAILED + 1))
    fi
done < <(find "$TEST_DIR" -type f -name "*.tr" | sort)

echo ""
echo "AI regression summary"
echo "Total:  $TOTAL"
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ $FAILED -gt 0 ]; then
    exit 1
fi