#!/bin/bash

# Configuration
TEST_DIR="tests"
COMPILER="./turf"
PROGRAM="./program"

# Colors for output
BOLD="\033[1m"
GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
CYAN="\033[0;36m"
RESET="\033[0m"

# Track results
TOTAL_TESTS=0
PASSED=0
FAILED=0
declare -a FAILED_TESTS

# Ensure compiler exists
if [ ! -f "$COMPILER" ]; then
    echo -e "${RED}Compiler not found at $COMPILER. Please build Turf first.${RESET}"
    exit 1
fi

echo -e "${BOLD}Running Turf Test Suites...${RESET}\n"

# Find all test files, exclude the custom directory
TEST_FILES=$(find "$TEST_DIR" -type f -name "*.tr" | grep -v "$TEST_DIR/custom/")

for test_file in $TEST_FILES; do
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    
    # Extract suite name (e.g. cfg, io, loops)
    SUITE=$(basename $(dirname "$test_file"))
    FILE_NAME=$(basename "$test_file")
    
    echo -n -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... "
    
    # 1. Compile the test
    COMPILE_OUTPUT=$("$COMPILER" "$test_file" -o "$PROGRAM" 2>&1)
    COMPILE_STATUS=$?
    
    if [ $COMPILE_STATUS -ne 0 ]; then
        echo -e "${RED}COMPILE FAILED${RESET}"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test_file (Compile Error):\n$COMPILE_OUTPUT")
        continue
    fi
    
    # 2. Run the compiled program
    if [ ! -f "$PROGRAM" ]; then
        echo -e "${RED}NO EXECUTABLE PRODUCED${RESET}"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test_file (No executable)")
        continue
    fi
    
    # Special handling for I/O tests that expect input
    if [[ "$SUITE" == "io" ]]; then
        # Pipe dummy values (e.g., name and age) to avoid blocking
        echo -e "TestUser\n25" | "$PROGRAM" > output.log 2>&1 &
        CMD_PID=$!
        (sleep 2 && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat output.log)
        RUN_OUTPUT=$(cat output.log)
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat output.log)
    else
        # Regular tests
        "$PROGRAM" > output.log 2>&1 &
        CMD_PID=$!
        (sleep 2 && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat output.log)
        RUN_OUTPUT=$(cat output.log)
    fi

    if [ $RUN_STATUS -eq 137 ] || [ $RUN_STATUS -eq 143 ] || [ $RUN_STATUS -eq 211 ]; then
        echo -e "${YELLOW}TIMEOUT (Likely infinite loop)${RESET}"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test_file (Timed out after 2s)")
    elif [ $RUN_STATUS -ne 0 ]; then
        echo -e "${RED}RUNTIME FAILED${RESET} (Exit code $RUN_STATUS)"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test_file (Runtime Error exit $RUN_STATUS):\n$RUN_OUTPUT")
    else
        echo -e "${GREEN}PASS${RESET}"
        PASSED=$((PASSED + 1))
    fi
    
    # Cleanup compiled program
    rm -f "$PROGRAM"
done

echo ""
echo -e "${BOLD}=== Test Summary ===${RESET}"
echo -e "Total Tests: ${TOTAL_TESTS}"
echo -e "Passed:      ${GREEN}${PASSED}${RESET}"
echo -e "Failed:      ${RED}${FAILED}${RESET}"

if [ $FAILED -gt 0 ]; then
    echo -e "\n${BOLD}${RED}Failed Tests Details:${RESET}"
    for fail_detail in "${FAILED_TESTS[@]}"; do
        echo -e "\n${YELLOW}--- ${fail_detail} ---${RESET}"
    done
    exit 1
else
    echo -e "\n${GREEN}${BOLD}All tests passed successfully!${RESET}"
    exit 0
fi
