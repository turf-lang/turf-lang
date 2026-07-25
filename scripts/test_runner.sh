#!/bin/bash

# Configuration
TEST_DIR="tests"
# Get absolute path for compiler so we can run it from any directory
COMPILER="$(pwd)/build/turf"

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

# Find all test files, exclude custom directory, and ignore module files
TEST_FILES=$(find "$TEST_DIR" -type f -name "*.tr" | grep -v "$TEST_DIR/custom/" | grep -v "$TEST_DIR/10_known_bugs/" | grep -v "_module.tr")

for test_file in $TEST_FILES; do
    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    # Extract suite name (e.g. cfg, io, loops)
    SUITE=$(basename $(dirname "$test_file"))
    FILE_NAME=$(basename "$test_file")
    TEST_DIR_PATH=$(dirname "$test_file")
    PROGRAM="$(pwd)/program"

    echo -n -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... "

    # Enter the test directory so imports resolve correctly (Issue 19)
    cd "$TEST_DIR_PATH"

    # 1. Compile the test
    COMPILE_OUTPUT=$("$COMPILER" "$FILE_NAME" -o "$PROGRAM" 2>&1)
    COMPILE_STATUS=$?

    # Return to project root
    cd - > /dev/null

    # A handful of fail_*.tr tests aren't testing "the compiler rejects this
    # input" at all -- they're testing that a stdlib guard (e.g.
    # str.substring's bounds check) correctly calls fail() at RUNTIME on
    # otherwise valid, compiling code. Those opt in via a
    # `// RUNTIME_ABORT_EXPECTED` marker comment so they get the (compile
    # must succeed, then runtime exit must be non-zero) contract instead of
    # the default (compile must fail) one below. Without this, such a test
    # can silently "pass" for the wrong reason forever if an unrelated bug
    # (e.g. a typo'd import name) makes compilation fail before the actual
    # behavior under test is ever reached -- confirmed to have happened to
    # fail_string_substr_oob.tr, which was failing on "module 'str' not
    # found" (the module is really named 'strings') instead of ever
    # exercising the substring-OOB abort it claims to verify.
    if [[ "$FILE_NAME" == fail_* ]] && grep -q "RUNTIME_ABORT_EXPECTED" "$test_file"; then
        if [ $COMPILE_STATUS -ne 0 ]; then
            echo -e "${RED}FAILED (Expected to compile successfully, but got a compile error)${RESET}"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_file (Unexpected compile error):\n$COMPILE_OUTPUT")
            continue
        fi
        if [ ! -f "$PROGRAM" ]; then
            echo -e "${RED}NO EXECUTABLE PRODUCED${RESET}"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_file (No executable)")
            continue
        fi
        cd "$TEST_DIR_PATH"
        "$PROGRAM" > output.log 2>&1 &
        CMD_PID=$!
        (sleep 2 && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat output.log)
        cd - > /dev/null
        rm -f "$PROGRAM" "$TEST_DIR_PATH/output.log"

        if [ $RUN_STATUS -eq 0 ] || [ $RUN_STATUS -eq 137 ] || [ $RUN_STATUS -eq 143 ]; then
            echo -e "${RED}FAILED (Expected a runtime abort, got exit $RUN_STATUS)${RESET}"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_file (Expected runtime abort, got exit $RUN_STATUS):\n$RUN_OUTPUT")
        else
            echo -e "${GREEN}PASS (Aborted at runtime as expected)${RESET}"
            PASSED=$((PASSED + 1))
        fi
        continue
    fi

    # Check if this is a test designed to fail compilation
    if [[ "$FILE_NAME" == fail_* ]]; then
        if [ $COMPILE_STATUS -eq 0 ]; then
            echo -e "${RED}FAILED (Should have failed compilation)${RESET}"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_file (Compiled successfully but expected to fail)")
        elif [ $COMPILE_STATUS -ge 128 ]; then
            # A shell exit status >= 128 means the compiler died from a
            # signal (e.g. 134 = SIGABRT, 139 = SIGSEGV) rather than exiting
            # cleanly via a reported diagnostic. A fail_*.tr test's whole
            # point is "the compiler correctly REJECTS this input" -- a
            # crash is not a rejection, it's a compiler bug that happens to
            # also produce a non-zero exit code, and this branch used to
            # silently count it as a pass. Added after a real crash (an
            # unhandled iostream exception from a syntax error inside an
            # imported module) was confirmed to satisfy the old
            # `$COMPILE_STATUS -ne 0` check and would have masked a
            # regression back to that crash forever.
            echo -e "${RED}FAILED (Compiler crashed, signal $((COMPILE_STATUS - 128)), instead of rejecting cleanly)${RESET}"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_file (Compiler crashed with exit $COMPILE_STATUS instead of a clean diagnostic):\n$COMPILE_OUTPUT")
        else
            echo -e "${GREEN}PASS (Rejected properly)${RESET}"
            PASSED=$((PASSED + 1))
        fi
        continue
    fi

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
    else
        # Regular tests
        "$PROGRAM" > output.log 2>&1 &
        CMD_PID=$!
        (sleep 2 && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
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
    rm -f "$PROGRAM" output.log
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
