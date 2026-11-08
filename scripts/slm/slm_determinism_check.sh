#!/bin/bash
#
# slm_determinism_check.sh -- SL-4: the same input gives the same answer twice.
#
# --smart samples greedily (temperature 0), so a model and runtime that are
# deterministic answer a prompt the same way every time. The suggestion cache
# would hide one that is not, so both runs bypass it (TURF_SLM_NO_CACHE), and
# the model's raw answers (TURF_SLM_DEBUG prints each) and the written fix are
# compared. Needs a model: TURF_SLM_MODEL=<file.gguf>, or the one .gguf in
# <root>/models; without one, or in a build without the runtime, it SKIPs.
#
# Usage: scripts/slm/slm_determinism_check.sh [program.tr ...]
#        (default: three of the replay programs)
# Exit:  0 = the same both times (or SKIP); 1 = an answer differed.

set -u
cd "$(dirname "$0")/../.." || exit 1
TURF="./build/turfc"
[ -x "$TURF" ] || { echo "build/turfc missing -- build first"; exit 1; }
if ! "$TURF" --version 2>&1 | grep -q "smart built-in"; then
  echo "slm_determinism_check: SKIP -- this turfc has no model runtime"
  exit 0
fi
if [ -z "${TURF_SLM_MODEL:-}" ] && [ "$(ls models/*.gguf 2>/dev/null | wc -l)" -ne 1 ]; then
  echo "slm_determinism_check: SKIP -- no model (set TURF_SLM_MODEL or put one .gguf in models/)"
  exit 0
fi

PROGRAMS=("$@")
[ ${#PROGRAMS[@]} -eq 0 ] && PROGRAMS=(scripts/slm/slm_replay/method_block.tr
  scripts/slm/slm_replay/unknown_field.tr scripts/slm/slm_replay/insert_line.tr)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0

answers() { # the raw answers in a TURF_SLM_DEBUG log, in order
  awk '/^--- SLM RAW RESPONSE DEBUG ---$/{f=1;next} /^------------------------------$/{f=0} f' "$1"
}

for p in "${PROGRAMS[@]}"; do
  name=$(basename "$p" .tr)
  for run in 1 2; do
    env -u TURF_HOME TURF_SLM_NO_CACHE=1 TURF_SLM_DEBUG=1 "$TURF" --smart \
      "--smart-write-fix=$WORK/$name.$run.fix" "$p" -o "$WORK/bin" >"$WORK/$name.$run.log" 2>&1
    answers "$WORK/$name.$run.log" >"$WORK/$name.$run.answers"
    [ -f "$WORK/$name.$run.fix" ] || : >"$WORK/$name.$run.fix"
  done
  if ! [ -s "$WORK/$name.1.answers" ]; then
    echo "FAIL $name -- the model gave no answer (see the log below)"
    tail -20 "$WORK/$name.1.log"
    FAILED=$((FAILED + 1))
  elif cmp -s "$WORK/$name.1.answers" "$WORK/$name.2.answers" &&
     cmp -s "$WORK/$name.1.fix" "$WORK/$name.2.fix"; then
    echo "PASS $name -- the same answers and fix twice"
  else
    echo "FAIL $name -- the two runs differ:"
    diff "$WORK/$name.1.answers" "$WORK/$name.2.answers" | head -20
    FAILED=$((FAILED + 1))
  fi
done

[ "$FAILED" -eq 0 ] && { echo "slm_determinism_check: deterministic"; exit 0; }
echo "slm_determinism_check: $FAILED program(s) answered differently"
exit 1
