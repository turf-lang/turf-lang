#!/bin/bash
#
# gc_stress_corpus_smoke.sh -- FDN-005 stage 1 (docs/planning/ROADMAP_v2.0.0.md,
# "Stress corpus"): the three long-running programs the roadmap names,
# each compiled and run directly (not through test_runner.sh -- these live
# under tests/custom/, which test_runner.sh excludes by convention,
# AGENTS.md), asserting both correctness (the program's own test::assert()
# calls / PASS line) and bounded RSS the same way scripts/gc_rss_smoke.sh
# already does for the smaller MEM-002-era stress tests.
#
# Why these three files are NOT under tests/08_e2e_stress/ alongside
# 06/07 (which test_runner.sh DOES sweep, at every -O level AND under
# FDN-005 stage 2's TURF_GC_STRESS=1 runs): a 1,000,000-node graph build
# or a 10^7-operation Map churn, run under TURF_GC_STRESS=1 (a full
# mark-sweep on every single allocation instead of the ordinary
# heap-doubling schedule), turns an O(n)-allocation workload into an
# O(n^2 log n)-shaped one -- measured live, the graph file alone did not
# finish in 5 minutes under that mode. That is a property of what stress
# mode DOES, not a bug in either the mode or these files; the two concerns
# (allocation-frequency stress, stage 2; scale/duration stress, stage 1)
# are deliberately orthogonal and never combined.
#
# Usage: scripts/gc_stress_corpus_smoke.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

TURF="./build/turfc"
CORPUS_DIR="tests/custom/gc_stress_corpus"
FAIL=0
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

pass() { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; FAIL=1; }

if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi
if ! command -v /usr/bin/time > /dev/null 2>&1; then
  echo -e "${RED}/usr/bin/time not found -- this script needs GNU time's -v for RSS.${RESET}"
  exit 1
fi

# Shared by the two single-process checks below: compile $1, run it under
# /usr/bin/time -v, and assert both a clean exit (the program's own
# test::assert()s / PASS line already checked correctness) and peak RSS
# within $2 KB.
check_bounded_rss() {
  TEST_FILE="$1"; BOUND_KB="$2"; LABEL="$3"

  echo -e "${BOLD}$LABEL${RESET}"
  if "$TURF" "$TEST_FILE" -o "$WORKDIR/prog" > "$WORKDIR/compile.log" 2>&1; then
    pass "compiled"
  else
    fail "failed to compile"; cat "$WORKDIR/compile.log"
    return 1
  fi

  /usr/bin/time -v "$WORKDIR/prog" > "$WORKDIR/run.log" 2> "$WORKDIR/time.log"
  RC=$?
  if [ $RC -ne 0 ]; then
    fail "the program itself failed (exit $RC):"
    cat "$WORKDIR/run.log"
    return 1
  fi
  pass "ran, exit 0 -- $(tail -1 "$WORKDIR/run.log")"

  PEAK_KB=$(grep "Maximum resident set size" "$WORKDIR/time.log" | awk '{print $NF}')
  if [ -z "$PEAK_KB" ]; then
    fail "could not parse peak RSS from /usr/bin/time -v output"
    return 1
  fi
  echo "   Peak RSS: ${PEAK_KB} KB (bound: ${BOUND_KB} KB)"
  if [ "$PEAK_KB" -le "$BOUND_KB" ]; then
    pass "${PEAK_KB}KB is within the ${BOUND_KB}KB bound"
  else
    fail "${PEAK_KB}KB EXCEEDS the ${BOUND_KB}KB bound"
  fi
  echo
}

# 1. ~1,000,000-node heap()-allocated graph, rewired across 4 passes, each
# followed by a forced collection. Measured live at landing time:
# ~112MB peak RSS (10^6 live GraphNode blocks + their List<&GraphNode>/
# List<int> backing buffers + the block table itself -- all real LIVE
# data, not garbage, so this bound is deliberately looser relative to its
# measurement than gc_rss_smoke.sh's own "should stay near zero" checks;
# what it actually guards against is a leak MULTIPLYING that live total
# across the 4 rewiring passes, which would show up as several hundred MB,
# not a 2-3x margin.
check_bounded_rss "$CORPUS_DIR/million_node_graph_churn.tr" 300000 \
  "1. A 1,000,000-node graph, built and rewired 4 times"

# 2. Map<string, List<string>> churned for 10^7 put() operations against a
# bounded 500-key working set. Measured live: ~7.7MB peak RSS -- every
# overwritten value (a List<string> plus its 2 string elements) becomes
# garbage instantly, so unlike check 1 this bound IS meant to stay close
# to its measurement (a regression here means the collector stopped
# reclaiming Map/List-nested garbage, not that the live set grew).
check_bounded_rss "$CORPUS_DIR/map_of_lists_churn.tr" 40000 \
  "2. Map<string, List<string>> churned for 10,000,000 operations"

# 3. The http_server showcase's own shape (a near-identical copy, see
# http_load_server.tr's own header for why it isn't literally
# showcase/http_server.tr), driven by a real scripted Turf client
# (http_load_client.tr) for 10^5 sequential requests -- the one stress
# shape a short-lived CLI program cannot exercise: a long-lived SERVER
# process whose GC-heap allocations (request/response strings, `net.tr`'s
# own buffers) must be reclaimed via the ordinary heap-doubling schedule
# across its whole run, not a single forced gc_collect() at the end.
echo -e "${BOLD}3. http_server-shaped load: 100,000 sequential requests${RESET}"
if "$TURF" "$CORPUS_DIR/http_load_server.tr" -o "$WORKDIR/http_load_server" > "$WORKDIR/server_compile.log" 2>&1; then
  pass "server compiled"
else
  fail "server failed to compile"; cat "$WORKDIR/server_compile.log"
  FAIL=1
fi
if "$TURF" "$CORPUS_DIR/http_load_client.tr" -o "$WORKDIR/http_load_client" > "$WORKDIR/client_compile.log" 2>&1; then
  pass "client compiled"
else
  fail "client failed to compile"; cat "$WORKDIR/client_compile.log"
  FAIL=1
fi

if [ -x "$WORKDIR/http_load_server" ] && [ -x "$WORKDIR/http_load_client" ]; then
  /usr/bin/time -v "$WORKDIR/http_load_server" > "$WORKDIR/server_run.log" 2> "$WORKDIR/server_time.log" &
  SERVER_PID=$!
  sleep 0.3

  if timeout 120 "$WORKDIR/http_load_client" > "$WORKDIR/client_run.log" 2>&1; then
    pass "client: $(tail -1 "$WORKDIR/client_run.log")"
  else
    fail "client failed or timed out:"
    cat "$WORKDIR/client_run.log"
  fi

  # The server exits on its own once it has handled REQUEST_COUNT
  # requests (its own loop bound, matching the client's) -- wait for it
  # rather than killing it, so /usr/bin/time -v's RSS line is real.
  if wait "$SERVER_PID"; then
    pass "server: $(tail -1 "$WORKDIR/server_run.log")"
  else
    fail "server exited non-zero:"
    cat "$WORKDIR/server_run.log"
  fi

  PEAK_KB=$(grep "Maximum resident set size" "$WORKDIR/server_time.log" | awk '{print $NF}')
  BOUND_KB=30000
  if [ -n "$PEAK_KB" ]; then
    echo "   Server peak RSS: ${PEAK_KB} KB (bound: ${BOUND_KB} KB)"
    if [ "$PEAK_KB" -le "$BOUND_KB" ]; then
      pass "${PEAK_KB}KB is within the ${BOUND_KB}KB bound"
    else
      fail "${PEAK_KB}KB EXCEEDS the ${BOUND_KB}KB bound"
    fi
  else
    fail "could not parse server peak RSS"
  fi
else
  fail "skipping run: compile failure above"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}gc_stress_corpus_smoke: clean.${RESET}"
  exit 0
else
  echo -e "${RED}${BOLD}gc_stress_corpus_smoke: FAILED.${RESET}"
  exit 1
fi
