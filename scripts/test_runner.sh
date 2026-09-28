#!/bin/bash

# Configuration
TEST_DIR="tests"
# Get absolute path for compiler so we can run it from any directory.
# Overridable via TURF_COMPILER -- FDN-005 stage 3 (docs/planning/
# ROADMAP_v2.0.0.md): an ASan-instrumented `turf` (CMakeLists.txt's
# TURF_RT_SANITIZE option) lives in its own build directory, never
# build/turf, so CI/a local session can point this whole suite at it
# without touching the ordinary dev build.
if [ -n "${TURF_COMPILER:-}" ]; then
    COMPILER="$TURF_COMPILER"
else
    COMPILER="$(pwd)/build/turf"
fi

# Colors for output
BOLD="\033[1m"
GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
CYAN="\033[0;36m"
RESET="\033[0m"

# OPT-001: optional `-O0`/`-O1`/`-O2`/`-O3`/`-Os` as $1, forwarded to every
# compile invocation below. Absent (the default, and what every existing
# caller/CLAUDE.md/AGENTS.md invocation still does) means "whatever the
# compiler's own default is" -- this script deliberately does not hardcode
# `-O2` itself, so it never drifts out of sync if the compiler's own
# default ever changes. This is INV-OPT-1's actual whole-suite regression
# coverage: `opt_levels_smoke.sh` only exercises a small representative
# program set (its own doc comment explains why -- server/client showcase
# programs and nondeterministic ones can't be byte-diffed across levels),
# while this flag lets CI run the FULL 624-test suite, unmodified, at each
# level individually.
TURF_OPT_FLAG=""
if [ $# -gt 0 ]; then
    case "$1" in
        -O0|-O1|-O2|-O3|-Os) TURF_OPT_FLAG="$1"; shift ;;
        -O*)
            echo -e "${RED}Unknown optimization level '$1' (expected one of -O0, -O1, -O2, -O3, -Os).${RESET}"
            exit 1
            ;;
    esac
fi

# Ensure compiler exists
if [ ! -f "$COMPILER" ]; then
    echo -e "${RED}Compiler not found at $COMPILER. Please build Turf first.${RESET}"
    exit 1
fi

if [ -n "$TURF_OPT_FLAG" ]; then
    echo -e "${BOLD}Running Turf Test Suites... (${TURF_OPT_FLAG})${RESET}\n"
else
    echo -e "${BOLD}Running Turf Test Suites...${RESET}\n"
fi

# How many tests to run at once. Defaults to every available processor (same
# idea as the parallel `-j$(nproc)` build), with a portable fallback chain
# for environments where `nproc` isn't installed (e.g. macOS) -- overridable
# via TURF_TEST_JOBS for a CI runner that deliberately wants to cap it.
#
# On macOS specifically, every test execs a brand-new uniquely-named binary
# (see PROGRAM below), and macOS's on-demand Gatekeeper/AMFI signature check
# for a never-before-seen executable (~0.3-0.5s) appears to serialize across
# concurrent processes -- confirmed by measurement: 8 freshly-compiled
# binaries launched concurrently took 3.2s total wall time, vs ~10ms once
# each had already been run once ("warmed"). At full core-count parallelism
# that queuing alone was enough to push individual tests past the run
# timeout below, producing mass false "Timed out" failures with nothing
# actually wrong with the compiler or the test. Halving default parallelism
# on macOS cuts how many of these checks queue up at once; it doesn't
# eliminate the per-test tax (see the timeout bump below for that).
# Per-test wall-clock timeout in seconds, overridable via
# TURF_TEST_TIMEOUT_SEC -- FDN-005 (docs/planning/ROADMAP_v2.0.0.md): a run
# with TURF_GC_STRESS=1 set (forces a full mark-sweep collection on every
# single allocation, runtime/turfrt_gc.c) makes an allocation-heavy program
# like tests/08_e2e_stress/06_gc_bounded_allocation.tr / 07_heap_bounded_
# allocation.tr take far longer than the same program collecting only on
# the ordinary heap-doubling schedule -- confirmed live: 07 alone exceeds
# the default 5s bound under stress mode at -O0. The default (5) is
# unchanged for every existing caller that doesn't set this.
if [ -n "$TURF_TEST_TIMEOUT_SEC" ]; then
    RUN_TIMEOUT_SEC="$TURF_TEST_TIMEOUT_SEC"
else
    RUN_TIMEOUT_SEC=5
fi

if [ -n "$TURF_TEST_JOBS" ]; then
    JOBS="$TURF_TEST_JOBS"
elif command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    JOBS=$(( $(sysctl -n hw.ncpu 2>/dev/null || echo 8) / 2 ))
    [ "$JOBS" -lt 1 ] && JOBS=1
else
    JOBS=4
fi

# Each test's outcome is written to its own file here (test files run truly
# concurrently -- in separate `bash -c` subprocesses spawned by xargs -- so
# there's no shared, lockable in-memory counter to update the way the old
# sequential version just incremented PASSED/FAILED directly). Aggregated
# back into the exact same summary format after every test finishes.
RESULTS_DIR=$(mktemp -d)
trap 'rm -rf "$RESULTS_DIR"' EXIT

# Find all test files, exclude custom directory, and ignore module files.
# Rewritten from the old `find | grep -v ... | grep -v ...` pipeline to pure
# `find` predicates with -print0 (NUL-delimited) specifically so this can
# feed `xargs -0` safely -- a `grep -v` stage would see one gigantic
# NUL-separated line instead of one match per file.
mapfile -d '' -t TEST_FILES < <(
    find "$TEST_DIR" -type f -name "*.tr" \
        -not -path "$TEST_DIR/custom/*" \
        -not -path "$TEST_DIR/99_known_bugs/*" \
        -not -name "*_module.tr" \
        -print0
)

# Runs exactly one test file end to end (compile, classify, execute, judge)
# and writes its outcome to $RESULTS_DIR. This is the SAME logic the old
# sequential loop body had, unchanged test-classification-wise (fail_*
# rejection, RUNTIME_ABORT_EXPECTED, signal-crash detection, timeout
# handling, io-suite stdin piping) -- only the "where results go" plumbing
# changed, since this now runs as its own subprocess under `xargs -P` rather
# than as one iteration of a shared-state sequential loop.
run_one_test() {
    local test_file="$1"
    local SUITE FILE_NAME TEST_DIR_PATH PROGRAM RESULT_FILE
    SUITE=$(basename "$(dirname "$test_file")")
    FILE_NAME=$(basename "$test_file")
    TEST_DIR_PATH=$(dirname "$test_file")
    # Unique per-invocation program/log name ($$ = this worker's own PID):
    # several tests in the SAME directory run concurrently under -P, so the
    # old shared "program"/"output.log" names would let two parallel workers
    # overwrite each other's binary or output mid-run.
    PROGRAM="$(pwd)/${TEST_DIR_PATH}/.testrun_${FILE_NAME%.tr}_$$"
    LOGFILE="${PROGRAM}.log"
    RESULT_FILE="$RESULTS_DIR/$(echo "$test_file" | tr '/' '_').result"

    # Enter the test directory so imports resolve correctly (Issue 19)
    cd "$TEST_DIR_PATH" || { echo "PASS_OR_FAIL=FAIL" > "$RESULT_FILE"; return; }

    # 1. Compile the test
    if [ -n "$TURF_OPT_FLAG" ]; then
        COMPILE_OUTPUT=$("$COMPILER" "$FILE_NAME" -o "$PROGRAM" "$TURF_OPT_FLAG" 2>&1)
    else
        COMPILE_OUTPUT=$("$COMPILER" "$FILE_NAME" -o "$PROGRAM" 2>&1)
    fi
    COMPILE_STATUS=$?

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
    if [[ "$FILE_NAME" == fail_* ]] && grep -q "RUNTIME_ABORT_EXPECTED" "$FILE_NAME"; then
        if [ $COMPILE_STATUS -ne 0 ]; then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Expected to compile successfully, but got a compile error)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Unexpected compile error):"
                echo "$COMPILE_OUTPUT"
            } > "$RESULT_FILE"
            rm -f "$PROGRAM" "$LOGFILE"
            return
        fi
        if [ ! -f "$PROGRAM" ]; then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}NO EXECUTABLE PRODUCED${RESET}"
            { echo "STATUS=FAIL"; echo "DETAIL=$test_file (No executable)"; } > "$RESULT_FILE"
            return
        fi
        "$PROGRAM" > "$LOGFILE" 2>&1 &
        CMD_PID=$!
        (sleep 5 && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat "$LOGFILE")
        rm -f "$PROGRAM" "$LOGFILE"

        if [ $RUN_STATUS -eq 0 ] || [ $RUN_STATUS -eq 137 ] || [ $RUN_STATUS -eq 143 ]; then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Expected a runtime abort, got exit $RUN_STATUS)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Expected runtime abort, got exit $RUN_STATUS):"
                echo "$RUN_OUTPUT"
            } > "$RESULT_FILE"
        else
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${GREEN}PASS (Aborted at runtime as expected)${RESET}"
            echo "STATUS=PASS" > "$RESULT_FILE"
        fi
        return
    fi

    # Check if this is a test designed to fail compilation
    if [[ "$FILE_NAME" == fail_* ]]; then
        if [ $COMPILE_STATUS -eq 0 ]; then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Should have failed compilation)${RESET}"
            { echo "STATUS=FAIL"; echo "DETAIL=$test_file (Compiled successfully but expected to fail)"; } > "$RESULT_FILE"
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
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Compiler crashed, signal $((COMPILE_STATUS - 128)), instead of rejecting cleanly)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Compiler crashed with exit $COMPILE_STATUS instead of a clean diagnostic):"
                echo "$COMPILE_OUTPUT"
            } > "$RESULT_FILE"
        else
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${GREEN}PASS (Rejected properly)${RESET}"
            echo "STATUS=PASS" > "$RESULT_FILE"
        fi
        rm -f "$PROGRAM" "$LOGFILE"
        return
    fi

    if [ $COMPILE_STATUS -ne 0 ]; then
        echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}COMPILE FAILED${RESET}"
        {
            echo "STATUS=FAIL"
            echo "DETAIL=$test_file (Compile Error):"
            echo "$COMPILE_OUTPUT"
        } > "$RESULT_FILE"
        rm -f "$PROGRAM" "$LOGFILE"
        return
    fi

    # 2. Run the compiled program
    if [ ! -f "$PROGRAM" ]; then
        echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}NO EXECUTABLE PRODUCED${RESET}"
        { echo "STATUS=FAIL"; echo "DETAIL=$test_file (No executable)"; } > "$RESULT_FILE"
        return
    fi

    # Special handling for I/O tests that expect input
    if [[ "$SUITE" == "io" ]]; then
        # Pipe dummy values (e.g., name and age) to avoid blocking
        echo -e "TestUser\n25" | "$PROGRAM" > "$LOGFILE" 2>&1 &
        CMD_PID=$!
        (sleep "$RUN_TIMEOUT_SEC" && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat "$LOGFILE")
    else
        # Regular tests
        "$PROGRAM" > "$LOGFILE" 2>&1 &
        CMD_PID=$!
        (sleep "$RUN_TIMEOUT_SEC" && kill -9 $CMD_PID 2>/dev/null) &
        wait $CMD_PID 2>/dev/null
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat "$LOGFILE")
    fi

    if [ $RUN_STATUS -eq 137 ] || [ $RUN_STATUS -eq 143 ] || [ $RUN_STATUS -eq 211 ]; then
        echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${YELLOW}TIMEOUT (Likely infinite loop)${RESET}"
        { echo "STATUS=FAIL"; echo "DETAIL=$test_file (Timed out after ${RUN_TIMEOUT_SEC}s)"; } > "$RESULT_FILE"
    elif [ $RUN_STATUS -ne 0 ]; then
        echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}RUNTIME FAILED${RESET} (Exit code $RUN_STATUS)"
        {
            echo "STATUS=FAIL"
            echo "DETAIL=$test_file (Runtime Error exit $RUN_STATUS):"
            echo "$RUN_OUTPUT"
        } > "$RESULT_FILE"
    else
        echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${GREEN}PASS${RESET}"
        echo "STATUS=PASS" > "$RESULT_FILE"
    fi

    # Cleanup compiled program
    rm -f "$PROGRAM" "$LOGFILE"
}
export -f run_one_test
export RESULTS_DIR COMPILER TURF_OPT_FLAG RUN_TIMEOUT_SEC
export BOLD GREEN RED YELLOW CYAN RESET

printf '%s\0' "${TEST_FILES[@]}" | xargs -0 -P "$JOBS" -I{} bash -c 'run_one_test "$@"' _ {}

# Aggregate every test's result file back into the exact same summary format
# the old sequential version printed.
TOTAL_TESTS=0
PASSED=0
FAILED=0
declare -a FAILED_TESTS

for result_file in "$RESULTS_DIR"/*.result; do
    [ -e "$result_file" ] || continue
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    STATUS=""
    DETAIL=""
    IN_DETAIL=0
    while IFS= read -r line; do
        if [ $IN_DETAIL -eq 1 ]; then
            DETAIL+=$'\n'"$line"
        elif [[ "$line" == STATUS=* ]]; then
            STATUS="${line#STATUS=}"
        elif [[ "$line" == DETAIL=* ]]; then
            DETAIL="${line#DETAIL=}"
            IN_DETAIL=1
        fi
    done < "$result_file"

    if [ "$STATUS" == "PASS" ]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$DETAIL")
    fi
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
