#!/bin/bash

# Configuration
TEST_DIR="tests"
# Get absolute path for compiler so we can run it from any directory.
# Overridable via TURF_COMPILER -- FDN-005 stage 3 (docs/planning/
# ROADMAP_v2.0.0.md): an ASan-instrumented `turfc` (CMakeLists.txt's
# TURF_RT_SANITIZE option) lives in its own build directory, never
# build/turfc, so CI/a local session can point this whole suite at it
# without touching the ordinary dev build. Runs the compiler directly,
# never build/turf (the Go front end), so the suite needs no Go toolchain
# (LEC-001, FREEZE.md 6.16).
if [ -n "${TURF_COMPILER:-}" ]; then
    COMPILER="$TURF_COMPILER"
else
    COMPILER="$(pwd)/build/turfc"
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

# Waits for background process $1, killing it (-9) if it is still running
# after $2 seconds; returns its exit status (137 if killed). Polls rather
# than spawning a `(sleep N && kill -9 $pid) &` watchdog: that watchdog was
# never cleaned up, so every test left a `sleep $RUN_TIMEOUT_SEC` behind
# that held any pipe reading this script's output open (CI's own step
# capture, `| tee`) for the full timeout after the suite had finished --
# up to 700s per TURF_GC_STRESS step -- and later ran `kill -9` on a PID
# that may by then have been reused by an unrelated process. Killing the
# watchdog afterwards races its own fork of `sleep` under a fully loaded
# parallel run (measured: 494 of 838 left behind), and `wait -n` needs a
# newer bash than macOS ships. Bash reaps a finished background child on
# its own, so `kill -0` stops succeeding as soon as the test exits.
wait_with_timeout() {
    local pid=$1 ticks=0 limit=$(( $2 * 10 ))
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$ticks" -ge "$limit" ]; then
            kill -9 "$pid" 2>/dev/null
            break
        fi
        sleep 0.1
        ticks=$(( ticks + 1 ))
    done
    wait "$pid" 2>/dev/null
}
export -f wait_with_timeout

# EXPECT-DIAG (LEC-005): a test file may pin what the COMPILER SAID, not just
# whether it succeeded. Without this a `fail_*.tr` proves only that the file
# failed to compile -- not that it failed for the reason it names (the
# "wrong-reason pass" AGENTS.md records twice from LEC-002: a reserved word
# used as a name, a test that started failing on E088 instead of the gate it
# was written for) -- and a warning, which never fails a compile, could not be
# asserted at all.
#
#   // EXPECT-DIAG: E011          the output contains the code `[E011` (header: `[E011 · control flow]`)
#   // EXPECT-DIAG: dead store    the output contains this text (use this for a
#                                 warning: warnings carry no code)
#   // EXPECT-NO-DIAG: dead store the output does NOT contain this text (the
#                                 shape of a false-positive or cascade fix)
#   // EXPECT-RUNTIME: Integer overflow
#                                 (RUNTIME_ABORT_EXPECTED tests only; see
#                                 check_runtime_expectations() below)
#
# Any number of lines, all must hold. The text is matched literally against the
# compiler's output with ANSI colour stripped, so match on prose, never on
# escape codes or column numbers. Applies to the COMPILE output of every kind of
# test -- fail_*, plain and RUNTIME_ABORT_EXPECTED -- never the program's own
# output. Prints nothing and returns 0 when every expectation holds; otherwise
# prints one line per miss and returns 1.
check_diag_expectations() {
    local file="$1" output="$2" esc plain line kind arg missed=0
    grep -qE '^[[:space:]]*//[[:space:]]*EXPECT(-NO)?-DIAG:' "$file" || return 0
    esc=$(printf '\033')
    plain=$(printf '%s' "$output" | sed "s/${esc}\[[0-9;]*m//g")
    while IFS= read -r line; do
        kind="${line%%:*}"
        arg="${line#*:}"
        arg="${arg#"${arg%%[![:space:]]*}"}"   # trim leading blanks
        arg="${arg%"${arg##*[![:space:]]}"}"   # trim trailing blanks
        [ -n "$arg" ] || continue
        if [[ "$kind" == *EXPECT-NO-DIAG ]]; then
            if grep -qF -- "$arg" <<< "$plain"; then
                echo "EXPECT-NO-DIAG violated: the compiler output contains '$arg'"
                missed=1
            fi
        elif [[ "$arg" =~ ^E[0-9]{3}$ ]]; then
            # `[E011 · control flow]` -- the code is followed by ` ·` or `]`.
            if ! grep -qE -- "\\[$arg( |\\])" <<< "$plain"; then
                echo "EXPECT-DIAG missed: no [$arg] in the compiler output"
                missed=1
            fi
        else
            if ! grep -qF -- "$arg" <<< "$plain"; then
                echo "EXPECT-DIAG missed: the compiler output does not contain '$arg'"
                missed=1
            fi
        fi
    done < <(grep -E '^[[:space:]]*//[[:space:]]*EXPECT(-NO)?-DIAG:' "$file" \
                 | sed -E 's|^[[:space:]]*//[[:space:]]*||')
    return $missed
}
export -f check_diag_expectations

# EXPECT-RUNTIME (LEC-005): the runtime half of the same idea, for a
# `// RUNTIME_ABORT_EXPECTED` test -- whose compile output has nothing to say
# about WHY the program aborted. `// EXPECT-RUNTIME: Integer overflow` requires
# the program's own output (stdout and stderr together) to contain that text,
# so a test written for one trap cannot keep passing because some other abort
# fired first. Same contract as check_diag_expectations(): nothing printed and
# 0 when every line holds, else one line per miss and 1.
check_runtime_expectations() {
    local file="$1" output="$2" line arg missed=0
    grep -qE '^[[:space:]]*//[[:space:]]*EXPECT-RUNTIME:' "$file" || return 0
    while IFS= read -r line; do
        arg="${line#*:}"
        arg="${arg#"${arg%%[![:space:]]*}"}"
        arg="${arg%"${arg##*[![:space:]]}"}"
        [ -n "$arg" ] || continue
        if ! grep -qF -- "$arg" <<< "$output"; then
            echo "EXPECT-RUNTIME missed: the program's output does not contain '$arg'"
            missed=1
        fi
    done < <(grep -E '^[[:space:]]*//[[:space:]]*EXPECT-RUNTIME:' "$file")
    return $missed
}
export -f check_runtime_expectations

# EXPECT-EXIT (issue #263): for a runtime-abort test whose reason has no text to
# pin because it is silent by design -- `sys::abort()` (SIGABRT, 134) prints
# nothing, and a `recover run { return 1; }` test proves its failure path was
# reached only by its exit code. `// EXPECT-EXIT: 134` requires exactly that
# exit status. $1 test file, $2 the program's exit status.
check_exit_expectation() {
    local file="$1" status="$2" want
    want=$(grep -E '^[[:space:]]*//[[:space:]]*EXPECT-EXIT:' "$file" | head -1 \
               | sed -E 's/^[[:space:]]*\/\/[[:space:]]*EXPECT-EXIT:[[:space:]]*//; s/[[:space:]]+$//')
    [ -n "$want" ] || return 0
    [ "$status" = "$want" ] && return 0
    echo "EXPECT-EXIT missed: the program exited with $status, not $want"
    return 1
}
export -f check_exit_expectation

# Diagnostic policy (issue #263, INV-TEST-2): EXPECT-DIAG only protects the
# tests that opt in, and ~400 `fail_*.tr` did not, so each of them could still
# be rejected for a reason other than the one it was written for. Closing that
# hole by review alone does not stay closed, so this makes it a rule: a
# compile-time `fail_*.tr` either carries `// EXPECT-DIAG: Exxx` or has exactly
# one entry in tests/DIAGNOSTIC_GAPS.txt, which doubles as the work list for
# the two things a beginner-friendly compiler must not do -- reject a program
# for the wrong reason (WRONG-REASON) or reject it with no error code
# (NO-CODE). See that file's header for the entry format. Entries are checked
# against the real output so the register cannot rot: a line that stopped
# being true fails the suite until it is deleted.
#
# $1 test file (cwd-relative), $2 its path as `find` printed it (the register's
# key), $3 the compile output. Prints one line per problem; returns 1 on any.
check_diag_policy() {
    local file="$1" key="$2" output="$3" esc plain emitted codeless catchall actual_errors
    local entries kinds line codepin=0 bad=0 want code title n field
    esc=$(printf '\033')
    plain=$(printf '%s' "$output" | sed "s/${esc}\[[0-9;]*m//g")
    # The header reads `[E138 · declaration]`: match on the `[E138` prefix only.
    emitted=$(grep -oE '\[E[0-9]{3}' <<< "$plain" | tr -d '[' | sort -u | paste -sd, -)
    # NO-CODE: a `── error` header with no `[Exxx]` is a plain error -- the
    # beginner has no `--explain` page for it. Warnings carry no codes by design.
    codeless=$(grep -E '^── error' <<< "$plain" | grep -vcE '\[E[0-9]{3}')
    # GENERIC-CODE: E066 is `TypeError`, the one class every TypeChecker
    # diagnostic funnels through, so it also carries "No matching overload!",
    # "Missing initializer!", ... whose --explain page it does not describe.
    # The banner is the line under the header; anything but the real
    # "Type mismatch!" under E066 is a more specific mistake wearing the
    # catch-all's code. Found by banner, not by test, so a NEW one cannot slip in.
    catchall=$(awk '/^── error.*\[E066[] ]/ { f = 1; next } f && NF { print; f = 0 }' <<< "$plain" \
                   | sed -E 's/^[[:space:]]+//' | grep -vxF 'Type mismatch!' | head -1)
    # CASCADE: the "(N errors)" count the compiler prints under the file name.
    # Summed over every header: an error in an imported module prints under its
    # OWN file header ("(1 error)" for the main file, "(1 error)" for the module),
    # and a cascade across files (a syntax error in a module reported as "module
    # not found" in the importer) is the sum of them.
    actual_errors=$(grep -oE '^[^ ]+ \(([0-9]+) errors?\)' <<< "$plain" | grep -oE '[0-9]+' | awk '{ s += $1 } END { if (NR) print s }')
    entries=$(awk -v k="$key" '$1 !~ /^#/ && $2 == k' "$REGISTER")
    kinds=" $(awk '{ print $1 }' <<< "$entries" | tr '\n' ' ')"
    grep -qE '^[[:space:]]*//[[:space:]]*EXPECT-DIAG:[[:space:]]*E[0-9]{3}[[:space:]]*$' "$file" && codepin=1

    # Every entry must be a known kind.
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        case "${line%% *}" in
            UNPINNED|NO-CODE|WRONG-REASON|GENERIC-CODE|CASCADE) ;;
            *) echo "DIAG-POLICY: $key has an entry of unknown kind '${line%% *}' in tests/DIAGNOSTIC_GAPS.txt (expected UNPINNED, NO-CODE, WRONG-REASON, GENERIC-CODE or CASCADE)"
               bad=1 ;;
        esac
    done <<< "$entries"
    [ "$bad" = 0 ] || return 1

    if [[ "$kinds" == *" UNPINNED "* ]]; then
        if [ "$codepin" = 1 ]; then
            echo "DIAG-POLICY stale: $key pins an error code but is still listed UNPINNED in tests/DIAGNOSTIC_GAPS.txt -- delete that line"
            return 1
        fi
        return 0   # baseline: nothing else is checked until the test is pinned
    fi

    if [ "$codepin" = 1 ] && [[ "$kinds" == *" WRONG-REASON "* ]]; then
        echo "DIAG-POLICY stale: $key pins an error code but is still listed WRONG-REASON -- if it now fires the diagnostic it was written for, delete that line"
        bad=1
    fi
    if [ "$codepin" = 0 ] && [[ "$kinds" == " " ]]; then
        echo "DIAG-POLICY: $key has no '// EXPECT-DIAG: Exxx' line and is not in tests/DIAGNOSTIC_GAPS.txt."
        echo "  The compiler emitted: ${emitted:-no error code at all}."
        echo "  Pin the code the test is MEANT to produce. If the compiler emits a different one, list it as WRONG-REASON; if none, as NO-CODE."
        bad=1
    fi

    # --- NO-CODE ---------------------------------------------------------
    if [ "$codeless" -gt 0 ] && [[ "$kinds" != *" NO-CODE "* ]] && [[ "$kinds" != *" WRONG-REASON "* ]]; then
        echo "DIAG-POLICY: $key makes the compiler print $codeless error(s) with no [Exxx] code -- a beginner cannot 'turf --explain' those."
        echo "  List it as NO-CODE in tests/DIAGNOSTIC_GAPS.txt (and pin its message text with '// EXPECT-DIAG: <text>'), or give the error a code."
        bad=1
    fi
    if [[ "$kinds" == *" NO-CODE "* ]]; then
        if [ "$codeless" -eq 0 ]; then
            echo "DIAG-POLICY stale: $key is listed NO-CODE but every error it prints now has a code (${emitted:-none}) -- pin '// EXPECT-DIAG: ${emitted%%,*}' and delete the line"
            bad=1
        elif ! grep -E '^[[:space:]]*//[[:space:]]*EXPECT-DIAG:' "$file" | grep -qvE 'EXPECT-DIAG:[[:space:]]*E[0-9]{3}[[:space:]]*$'; then
            echo "DIAG-POLICY: $key is NO-CODE, so it must pin its reason by text: add '// EXPECT-DIAG: <fragment of the message>'"
            bad=1
        fi
    fi

    # --- GENERIC-CODE ----------------------------------------------------
    if [ -n "$catchall" ] && [[ "$kinds" != *" GENERIC-CODE "* ]] && [[ "$kinds" != *" WRONG-REASON "* ]]; then
        echo "DIAG-POLICY: $key reports '$catchall' under E066, the type-mismatch catch-all, so a beginner's 'turf --explain E066' will not describe it."
        echo "  List it as GENERIC-CODE (code=E066 title=\"$catchall\") in tests/DIAGNOSTIC_GAPS.txt, or give that mistake its own code."
        bad=1
    fi
    if [[ "$kinds" == *" GENERIC-CODE "* ]]; then
        line=$(grep -E '^GENERIC-CODE ' <<< "$entries" | head -1)
        code=$(grep -oE 'code=E[0-9]{3}' <<< "$line" | head -1); code="${code#code=}"
        title=$(sed -nE 's/.*title="([^"]*)".*/\1/p' <<< "$line")
        if [ -z "$code" ] || [ -z "$title" ]; then
            echo "DIAG-POLICY: $key's GENERIC-CODE entry needs 'code=Exxx' and 'title=\"...\"'"
            bad=1
        elif ! grep -qF "[$code]" <<< "$plain" || ! grep -qF -- "$title" <<< "$plain"; then
            echo "DIAG-POLICY stale: $key is listed as '$title' under $code but the output no longer has both (${emitted:-no code}) -- update or delete the entry"
            bad=1
        fi
    fi

    # --- CASCADE ---------------------------------------------------------
    if [[ "$kinds" == *" CASCADE "* ]]; then
        line=$(grep -E '^CASCADE ' <<< "$entries" | head -1)
        field=$(grep -oE 'errors=[0-9]+' <<< "$line" | head -1); want="${field#errors=}"
        if [ -z "$want" ]; then
            echo "DIAG-POLICY: $key's CASCADE entry needs 'errors=N' (how many errors the compiler prints today)"
            bad=1
        elif [ "$actual_errors" != "$want" ]; then
            echo "DIAG-POLICY stale: $key is listed as printing $want errors but now prints ${actual_errors:-?} -- update the entry, or delete it if the cascade is fixed (and then pin '// EXPECT-NO-DIAG: <the bogus message>')"
            bad=1
        fi
    fi

    # --- WRONG-REASON ----------------------------------------------------
    if [[ "$kinds" == *" WRONG-REASON "* ]]; then
        line=$(grep -E '^WRONG-REASON ' <<< "$entries" | head -1)
        field=$(grep -oE 'emits=[A-Za-z0-9,]+' <<< "$line" | head -1)
        if [ -z "$field" ] || ! grep -q 'intended=' <<< "$line"; then
            echo "DIAG-POLICY: $key's WRONG-REASON entry needs 'emits=Exxx' (or 'emits=none') and 'intended=...'"
            bad=1
        elif [ "${field#emits=}" = "none" ]; then
            if [ -n "$emitted" ]; then
                echo "DIAG-POLICY stale: $key is listed as emitting no code but the output has $emitted -- update or delete the entry"
                bad=1
            fi
        else
            while IFS= read -r n; do
                if ! grep -qF "[$n]" <<< "$plain"; then
                    echo "DIAG-POLICY stale: $key is listed as emitting $n but the output has ${emitted:-no code} -- update or delete the entry"
                    bad=1
                fi
            done < <(tr ',' '\n' <<< "${field#emits=}")
        fi
    fi
    return $bad
}
export -f check_diag_policy

# The runtime half of the same policy: a `// RUNTIME_ABORT_EXPECTED` test must
# say what it aborts WITH (`// EXPECT-RUNTIME: <text>`), else it could keep
# passing because some other trap fired first. Runtime aborts carry no codes, so
# this is about the reason, and the register line is just UNPINNED.
# $1 test file, $2 its register key. Returns 1 and prints why on a violation.
check_runtime_policy() {
    local file="$1" key="$2" kind pinned=0
    kind=$(awk -v k="$key" '$1 !~ /^#/ && $2 == k { print $1; exit }' "$REGISTER")
    grep -qE '^[[:space:]]*//[[:space:]]*EXPECT-(RUNTIME|EXIT):[[:space:]]*[^[:space:]]' "$file" && pinned=1
    if [ "$pinned" = 1 ]; then
        [ -z "$kind" ] && return 0
        echo "DIAG-POLICY stale: $key has an EXPECT-RUNTIME pin but is still listed $kind in tests/DIAGNOSTIC_GAPS.txt -- delete that line"
        return 1
    fi
    case "$kind" in
        UNPINNED) return 0 ;;
        "")
            echo "DIAG-POLICY: $key aborts at runtime but has no '// EXPECT-RUNTIME: <text>' line and is not in tests/DIAGNOSTIC_GAPS.txt."
            echo "  Pin the message it is MEANT to abort with, so another trap firing first cannot keep it green."
            return 1 ;;
        *)
            echo "DIAG-POLICY: $key is a runtime-abort test; only UNPINNED is a valid register kind for it, not '$kind'"
            return 1 ;;
    esac
}
export -f check_runtime_policy

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
        if ! DIAG_MISS=$(check_diag_expectations "$FILE_NAME" "$COMPILE_OUTPUT"); then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (EXPECT-DIAG)${RESET}"
            { echo "STATUS=FAIL"; echo "DETAIL=$test_file (EXPECT-DIAG):"; echo "$DIAG_MISS"; echo "$COMPILE_OUTPUT"; } > "$RESULT_FILE"
            rm -f "$PROGRAM" "$LOGFILE"
            return
        fi
        "$PROGRAM" > "$LOGFILE" 2>&1 &
        CMD_PID=$!
        wait_with_timeout $CMD_PID 5
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
        elif ! DIAG_MISS=$(check_runtime_expectations "$FILE_NAME" "$RUN_OUTPUT"); then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Aborted, but EXPECT-RUNTIME not met)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Aborted at runtime, but EXPECT-RUNTIME not met):"
                echo "$DIAG_MISS"
                echo "$RUN_OUTPUT"
            } > "$RESULT_FILE"
        elif ! DIAG_MISS=$(check_exit_expectation "$FILE_NAME" "$RUN_STATUS"); then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Aborted, but EXPECT-EXIT not met)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Aborted at runtime, but EXPECT-EXIT not met):"
                echo "$DIAG_MISS"
                echo "$RUN_OUTPUT"
            } > "$RESULT_FILE"
        elif ! DIAG_MISS=$(check_runtime_policy "$FILE_NAME" "$test_file"); then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Aborted, but its reason is not pinned: see DIAGNOSTIC_GAPS.txt)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Diagnostic policy, tests/DIAGNOSTIC_GAPS.txt):"
                echo "$DIAG_MISS"
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
        elif ! DIAG_MISS=$(check_diag_expectations "$FILE_NAME" "$COMPILE_OUTPUT"); then
            # Rejected -- but not (only) for the reason the file says it should be.
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Rejected, but EXPECT-DIAG not met)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Rejected, but EXPECT-DIAG not met):"
                echo "$DIAG_MISS"
                echo "$COMPILE_OUTPUT"
            } > "$RESULT_FILE"
        elif ! DIAG_MISS=$(check_diag_policy "$FILE_NAME" "$test_file" "$COMPILE_OUTPUT"); then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (Rejected, but its reason is not pinned: see DIAGNOSTIC_GAPS.txt)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Diagnostic policy, tests/DIAGNOSTIC_GAPS.txt):"
                echo "$DIAG_MISS"
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
            # The exit code is the only clue when the compiler dies from a
            # signal (>= 128) with no output at all.
            echo "DETAIL=$test_file (Compile Error, exit $COMPILE_STATUS):"
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

    # A plain test compiles, so what it can pin is the WARNINGS (or their
    # absence) -- checked before the program runs, on the compile output.
    if ! DIAG_MISS=$(check_diag_expectations "$FILE_NAME" "$COMPILE_OUTPUT"); then
        echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${RED}FAILED (EXPECT-DIAG)${RESET}"
        { echo "STATUS=FAIL"; echo "DETAIL=$test_file (EXPECT-DIAG):"; echo "$DIAG_MISS"; echo "$COMPILE_OUTPUT"; } > "$RESULT_FILE"
        rm -f "$PROGRAM" "$LOGFILE"
        return
    fi

    # Special handling for I/O tests that expect input
    if [[ "$SUITE" == "io" ]]; then
        # Pipe dummy values (e.g., name and age) to avoid blocking
        echo -e "TestUser\n25" | "$PROGRAM" > "$LOGFILE" 2>&1 &
        CMD_PID=$!
        wait_with_timeout $CMD_PID "$RUN_TIMEOUT_SEC"
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat "$LOGFILE")
    else
        # Regular tests
        "$PROGRAM" > "$LOGFILE" 2>&1 &
        CMD_PID=$!
        wait_with_timeout $CMD_PID "$RUN_TIMEOUT_SEC"
        RUN_STATUS=$?
        RUN_OUTPUT=$(cat "$LOGFILE")
    fi

    if [ $RUN_STATUS -eq 137 ] || [ $RUN_STATUS -eq 143 ] || [ $RUN_STATUS -eq 211 ]; then
        # "Likely infinite loop" is the right guess at the default 5s bound and
        # the wrong one under TURF_GC_STRESS, where an allocation-heavy test
        # (07_heap_bounded_allocation.tr: ~200s on a developer machine) is
        # merely slow and a loaded CI runner can exceed a bound that is only
        # ~1.5x its own measurement. Say which it probably is, and what to try.
        if [ -n "${TURF_GC_STRESS:-}" ] && [ "${TURF_GC_STRESS}" != "0" ]; then
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${YELLOW}TIMEOUT (TURF_GC_STRESS is on: possibly just slow, see TURF_TEST_TIMEOUT_SEC)${RESET}"
            {
                echo "STATUS=FAIL"
                echo "DETAIL=$test_file (Timed out after ${RUN_TIMEOUT_SEC}s under TURF_GC_STRESS -- a collection per allocation makes allocation-heavy tests slow, not hung: time it with a larger TURF_TEST_TIMEOUT_SEC before assuming a loop)"
            } > "$RESULT_FILE"
        else
            echo -e "[${CYAN}${SUITE}${RESET}] ${FILE_NAME} ... ${YELLOW}TIMEOUT (Likely infinite loop)${RESET}"
            { echo "STATUS=FAIL"; echo "DETAIL=$test_file (Timed out after ${RUN_TIMEOUT_SEC}s)"; } > "$RESULT_FILE"
        fi
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
        # A passing test's output is otherwise discarded -- which made
        # TURF_GC_VALIDATE's report-only findings (runtime/turfrt_gc.c) and
        # CI's grep for them invisible. Nothing else prints this prefix.
        grep "^TURF_GC_VALIDATE:" "$LOGFILE" | sed "s|^|[${SUITE}/${FILE_NAME}] |"
    fi

    # Cleanup compiled program
    rm -f "$PROGRAM" "$LOGFILE"
}
export -f run_one_test
REGISTER="$(pwd)/$TEST_DIR/DIAGNOSTIC_GAPS.txt"
if [ ! -f "$REGISTER" ]; then
    echo -e "${RED}Missing $REGISTER (the register of unpinned diagnostics, see check_diag_policy).${RESET}"
    exit 1
fi
export REGISTER
export RESULTS_DIR COMPILER TURF_OPT_FLAG RUN_TIMEOUT_SEC # + check_*_expectations, above
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

# The register is owed work, never "expected": say how much, every run, so the
# number is visible and can only be seen to go down.
echo -e "Diagnostic register (tests/DIAGNOSTIC_GAPS.txt): $(awk '$1=="UNPINNED"' "$REGISTER" | wc -l) unpinned, $(awk '$1=="NO-CODE"' "$REGISTER" | wc -l) no-code, $(awk '$1=="WRONG-REASON"' "$REGISTER" | wc -l) wrong-reason, $(awk '$1=="GENERIC-CODE"' "$REGISTER" | wc -l) generic-code, $(awk '$1=="CASCADE"' "$REGISTER" | wc -l) cascade"

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
