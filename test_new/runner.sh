#!/bin/bash

# Turflang Automated Test Runner
# Usage: ./runner.sh

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Ensure we are in test_new directory
cd "$(dirname "$0")" || exit 1

echo -e "${YELLOW}Starting Turflang Test Suite...${NC}\n"

PASS_COUNT=0
FAIL_COUNT=0

# Recursively find all .tr files
while IFS= read -r test_file; do
    echo -n "Running $test_file ... "

    # 1. Compile the file
    out_binary="${test_file%.tr}.out"
    compile_output=$(../build/turf "$test_file" -o "$out_binary" 2>&1)
    compile_status=$?

    # Check if this is a negative test (file name starts with 'fail_')
    basename_test=$(basename "$test_file")
    if [[ "$basename_test" == fail_* ]]; then
        if [ $compile_status -ne 0 ]; then
            echo -e "${GREEN}[PASS] (Failed as expected)${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            echo -e "${RED}[FAIL]${NC}"
            echo "Expected compilation to fail, but it succeeded!"
            FAIL_COUNT=$((FAIL_COUNT + 1))
            rm -f "$out_binary"
        fi
        continue
    fi

    # For positive tests, compilation must succeed
    if [ $compile_status -ne 0 ]; then
        echo -e "${RED}[FAIL] (Compilation Error)${NC}"
        echo "$compile_output" | sed 's/^/    /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
        continue
    fi

    # 2. Run the compiled binary
    run_output=$(./"$out_binary" 2>&1)
    run_status=$?

    if [ $run_status -eq 0 ]; then
        echo -e "${GREEN}[PASS]${NC}"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo -e "${RED}[FAIL] (Exit Code: $run_status)${NC}"
        echo "$run_output" | sed 's/^/    /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    # 3. Cleanup
    rm -f "$out_binary" "$out_binary.o"

done < <(find . -name "*.tr" | sort)

echo -e "\n${YELLOW}=== Test Summary ===${NC}"
if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}All $PASS_COUNT tests passed!${NC}"
    exit 0
else
    echo -e "${RED}$FAIL_COUNT tests failed${NC} out of $((PASS_COUNT + FAIL_COUNT))."
    exit 1
fi
