#!/bin/bash
#
# consumer_fixtures.sh -- IF-5 (docs/planning/ROADMAP_v3.0.0.md): the compiler's
# CI runs the fixture tests of its three consumers, so a compiler change that
# would break one fails in this repository.
#
# This is the compiler-side half of IF-5. The consumer-side half (each consumer
# tests against the latest compiler release and against a nightly build) is the
# consumers' own CI. The consumers are checked out at their default branch,
# which is what a compiler change must keep working.
#
#   turf       (turf-cli)   go vet ./... && go test ./...; then the `turf` built
#                           from that checkout is run against this build/turfc
#                           (driver-interface handshake, --version, a program
#                           run through `turf run`).
#   turf-lsp                CMake builds the compiler's turfcore from THIS
#                           checkout (-DTURF_LANG_DIR), so the server is linked
#                           against include/turf/Analysis.h as it is in this
#                           tree. Then its unit tests, scripts/check_grammar.py
#                           and scripts/smoke.py (the real server over stdio,
#                           staged beside this tree's lib/std).
#   turf-fuzz               oracles/soundness.py against build/turfc: pinned
#                           verdicts (the F-MEM-5 escape boundary), so it
#                           is deterministic and runs in seconds. The random
#                           oracles (run.py, overflow.py, tier0) are the
#                           fuzzer's nightly, not a fixture.
#
# Usage: scripts/check/consumer_fixtures.sh   (after building build/turfc)
# Env:   TURF_CLI_DIR  TURF_LSP_DIR  TURF_FUZZ_DIR   the consumer checkouts
#                      (default: turf-cli, turf-lsp, turf-fuzz beside this repo)
#        CONSUMERS     which to run, space separated (default: "cli lsp fuzz")
#        JOBS          parallelism for the turf-lsp build (default: nproc/2)
# Exit:  0 = every consumer passed or was skipped; 1 = one failed. A consumer
#        whose checkout is missing is a FAIL when CI is set and a printed SKIP
#        otherwise. Each consumer's own commands are kept: nothing here
#        reimplements their tests.
#
# Linux/macOS shell only (the job is in the Linux workflow); there is no
# PowerShell twin, unlike scripts/smoke/, which also runs on Windows.

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
TURFC="$ROOT/build/turfc"
CLI_DIR=${TURF_CLI_DIR:-$ROOT/../turf-cli}
LSP_DIR=${TURF_LSP_DIR:-$ROOT/../turf-lsp}
FUZZ_DIR=${TURF_FUZZ_DIR:-$ROOT/../turf-fuzz}
CONSUMERS=${CONSUMERS:-cli lsp fuzz}
if [ -z "${JOBS:-}" ]; then
  JOBS=$(( $(nproc 2>/dev/null || echo 2) / 2 ))
  [ "$JOBS" -ge 1 ] || JOBS=1
fi

# An ambient install root or compiler override must not decide the result.
unset TURF_COMPILER TURF_HOME

[ -x "$TURFC" ] || { echo "FAIL: $TURFC is not built"; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/consumer-fixtures.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT
LOGDIR=${CONSUMER_LOGS:-$WORK}
mkdir -p "$LOGDIR"

RESULTS=()
FAILED=0

record() { # name status detail
  RESULTS+=("$1 $2 $3")
  if [ "$2" = FAIL ]; then FAILED=1; fi
  return 0
}

# run_step LOG DESCRIPTION COMMAND... : runs it in the current directory,
# appending to LOG. On failure the tail of the log is printed.
run_step() {
  local log=$1 what=$2
  shift 2
  echo "-- $what"
  if "$@" >>"$log" 2>&1; then
    return 0
  fi
  echo "   FAILED: $what"
  tail -n 40 "$log" | sed 's/^/   | /'
  STEP_FAILED=$what
  return 1
}

missing() { # name dir : prints and records SKIP/FAIL
  if [ -n "${CI:-}" ]; then
    echo "FAIL: $1 checkout not found at $2 (CI is set)"
    record "$1" FAIL "checkout not found at $2"
  else
    echo "SKIP: $1 checkout not found at $2 (set the *_DIR variable; a FAIL when CI is set)"
    record "$1" SKIP "checkout not found at $2"
  fi
}

# A `turf` built from the turf-cli checkout, for the turf-lsp smoke test and the
# handshake check. Built once; empty if there is no Go or the build fails.
TURF_BIN=""
build_turf() {
  [ -n "$TURF_BIN" ] && return 0
  command -v go >/dev/null 2>&1 || return 1
  (cd "$CLI_DIR" && CGO_ENABLED=0 go build -trimpath -o "$WORK/bin/turf" ./cmd/turf) \
    >>"$LOGDIR/turf-cli.log" 2>&1 || return 1
  TURF_BIN="$WORK/bin/turf"
}

check_cli() {
  local log="$LOGDIR/turf-cli.log"
  : >"$log"
  STEP_FAILED=""
  if [ ! -f "$CLI_DIR/go.mod" ]; then missing turf "$CLI_DIR"; return; fi
  if ! command -v go >/dev/null 2>&1; then
    if [ -n "${CI:-}" ]; then record turf FAIL "go is not on PATH"
    else echo "SKIP: go is not on PATH"; record turf SKIP "go is not on PATH"; fi
    return
  fi
  (
    cd "$CLI_DIR" || exit 1
    run_step "$log" "turf-cli: go vet ./..." go vet ./... &&
      run_step "$log" "turf-cli: go test ./..." go test ./... &&
      run_step "$log" "turf-cli: go build ./cmd/turf" build_turf
  ) || { record turf FAIL "Go checks failed (log: $log)"; return; }
  build_turf || { record turf FAIL "go build ./cmd/turf failed (log: $log)"; return; }
  # The built `turf` against this turfc: the handshake accepts it, and a program
  # is compiled and run through `turf` the way a user runs it.
  mkdir -p "$WORK/cli-prog"
  printf 'import io;\nfn main(): int {\n    io::printline("consumer-fixtures-ok");\n    return 0;\n}\n' >"$WORK/cli-prog/hello.tr"
  local out
  if ! out=$(cd "$WORK/cli-prog" && TURF_COMPILER="$TURFC" "$TURF_BIN" --version 2>&1) \
     || ! printf '%s' "$out" | grep -q 'turfc'; then
    echo "$out" >>"$log"
    echo "   FAILED: turf --version against build/turfc"; echo "$out" | sed 's/^/   | /'
    record turf FAIL "turf --version against build/turfc (log: $log)"; return
  fi
  if ! out=$(cd "$WORK/cli-prog" && TURF_COMPILER="$TURFC" "$TURF_BIN" hello.tr 2>&1) \
     || ! printf '%s' "$out" | grep -q 'Successfully compiled' \
     || [ "$(cd "$WORK/cli-prog" && ./hello)" != "consumer-fixtures-ok" ]; then
    echo "$out" >>"$log"
    echo "   FAILED: turf hello.tr against build/turfc"; echo "$out" | sed 's/^/   | /'
    record turf FAIL "turf hello.tr against build/turfc (log: $log)"; return
  fi
  record turf PASS "go vet, go test, handshake and run against build/turfc"
}

check_lsp() {
  local log="$LOGDIR/turf-lsp.log" bdir="$ROOT/build/consumers/turf-lsp"
  : >"$log"
  if [ ! -f "$LSP_DIR/CMakeLists.txt" ]; then missing turf-lsp "$LSP_DIR"; return; fi
  if ! build_turf; then
    record turf-lsp FAIL "no turf to stage for the smoke test (needs turf-cli and go; log: $LOGDIR/turf-cli.log)"
    return
  fi
  (
    cd "$LSP_DIR" || exit 1
    run_step "$log" "turf-lsp: cmake configure against $ROOT" \
      cmake -S . -B "$bdir" -DCMAKE_BUILD_TYPE=Release -DTURF_LANG_DIR="$ROOT" -DTURF_SMART=OFF &&
      run_step "$log" "turf-lsp: build turf-lsp and turf-lsp-tests (-j$JOBS)" \
        cmake --build "$bdir" --target turf-lsp turf-lsp-tests -j"$JOBS" &&
      run_step "$log" "turf-lsp: unit tests" "$bdir/turf-lsp-tests" &&
      run_step "$log" "turf-lsp: grammar check" python3 scripts/check_grammar.py --server "$bdir/turf-lsp" &&
      run_step "$log" "turf-lsp: smoke test over stdio" \
        python3 scripts/smoke.py --server "$bdir/turf-lsp" --stdlib-from "$ROOT" --turf "$TURF_BIN"
  ) && record turf-lsp PASS "built against this tree; unit, grammar, smoke" \
    || record turf-lsp FAIL "a step failed (log: $log)"
}

check_fuzz() {
  local log="$LOGDIR/turf-fuzz.log"
  : >"$log"
  if [ ! -f "$FUZZ_DIR/oracles/soundness.py" ]; then missing turf-fuzz "$FUZZ_DIR"; return; fi
  (
    cd "$FUZZ_DIR" || exit 1
    # The oracles read TURF_ROOT/build/turfc. They run at -O0 and -O2 and use
    # the ASan runtime only if build_asan/turfc exists.
    export TURF_ROOT="$ROOT"
    run_step "$log" "turf-fuzz: soundness oracle (pinned verdicts)" \
      python3 -u oracles/soundness.py --jobs "$JOBS"
  ) && record turf-fuzz PASS "soundness oracle against build/turfc" \
    || record turf-fuzz FAIL "the soundness oracle failed (log: $log)"
}

for c in $CONSUMERS; do
  case $c in
    cli)  check_cli ;;
    lsp)  check_lsp ;;
    fuzz) check_fuzz ;;
    *) echo "unknown consumer '$c' (cli, lsp, fuzz)"; exit 2 ;;
  esac
done

echo
echo "== consumer fixtures (IF-5), compiler: $("$TURFC" --version | head -n 1)"
for r in "${RESULTS[@]}"; do
  name=${r%% *}; rest=${r#* }; status=${rest%% *}; detail=${rest#* }
  printf '%-4s %-9s %s\n' "$status" "$name" "$detail"
done
[ "$FAILED" -eq 0 ] || exit 1
exit 0
