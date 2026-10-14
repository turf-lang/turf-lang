#!/bin/bash
#
# The compiler's own regression suite: every .tr file under tests/.
#
# This is a thin wrapper over `turfc --test` (REL-002). How a test is judged --
# plain files must run and exit 0, fail_* must be rejected, RUNTIME_ABORT_EXPECTED
# must compile and then abort, the EXPECT-DIAG / EXPECT-RUNTIME / EXPECT-EXIT
# markers pin why (LEC-005), the diagnostic register (issue #263) -- is
# implemented once, in src/tooling/TestRunner.cpp and src/tooling/TestExpectations.cpp, and is
# the same code `turf test` runs for a project. Only what is particular to this
# repository lives here:
#
#   - tests/custom and tests/99_known_bugs are not live regressions and are
#     skipped (AGENTS.md);
#   - tests/DIAGNOSTIC_GAPS.txt is enforced: a project's own tests need no
#     register, the compiler's must keep theirs honest;
#   - the compiler is build/turfc, or $TURF_COMPILER (FDN-005 stage 3: an
#     ASan-instrumented turfc lives in its own build directory). Never
#     build/turf, the Go front end, so the suite needs no Go toolchain
#     (LEC-001, FREEZE.md 6.16).
#
# Usage: scripts/test_runner.sh [-O0|-O1|-O2|-O3|-Os] [-g]
#   The optional level is forwarded to every compile (OPT-001, INV-OPT-1); absent,
#   it is whatever the compiler's own default is. CI runs the whole suite once
#   per level. -g compiles every test with debug info (REL-004 stage 5: a debug
#   build must run every program exactly as a plain one does); it combines with
#   a level.
#
# Environment (read by turfc --test): TURF_TEST_JOBS caps how many tests run at
# once (default: every processor); TURF_TEST_TIMEOUT_SEC bounds one test
# program's run (default 5; the GC-stress CI steps raise it, see AGENTS.md).
# Run from the repository root.

if [ -n "${TURF_COMPILER:-}" ]; then
    COMPILER="$TURF_COMPILER"
else
    COMPILER="$(pwd)/build/turfc"
fi

RED="\033[0;31m"
RESET="\033[0m"

OPT_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -O0|-O1|-O2|-O3|-Os|-g) OPT_ARGS+=("$1"); shift ;;
        -O*)
            echo -e "${RED}Unknown optimization level '$1' (expected one of -O0, -O1, -O2, -O3, -Os).${RESET}"
            exit 1
            ;;
        *)
            echo -e "${RED}Unknown argument '$1' (expected -O0|-O1|-O2|-O3|-Os and/or -g).${RESET}"
            exit 1
            ;;
    esac
done

if [ ! -f "$COMPILER" ]; then
    echo -e "${RED}Compiler not found at $COMPILER. Please build Turf first.${RESET}"
    exit 1
fi

# TURF_TEST_EXTRA_EXCLUDE: space-separated paths under tests/ to skip as well,
# for a run whose build cannot pass them by design. The one user is the ASan CI
# job, which skips the stack-overflow test because an ASan runtime installs no
# SIGSEGV handler of its own (runtime/turfrt_signal.c, INV-RT-5).
EXTRA_EXCLUDE=()
for x in ${TURF_TEST_EXTRA_EXCLUDE:-}; do EXTRA_EXCLUDE+=(--exclude "$x"); done

exec "$COMPILER" --test tests \
    --diag-register tests/DIAGNOSTIC_GAPS.txt \
    --exclude custom --exclude 99_known_bugs \
    ${EXTRA_EXCLUDE[@]+"${EXTRA_EXCLUDE[@]}"} \
    "${OPT_ARGS[@]}"
