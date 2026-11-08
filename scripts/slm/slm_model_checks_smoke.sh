#!/bin/bash
#
# slm_model_checks_smoke.sh -- SL-4's checks on the model that need no model:
#
#   - a model not in docs/slm/qualified_models.tsv gets one note and still runs
#     (TURF_SLM_MODEL names a file of random bytes; it is never loaded, since
#     the memory check refuses it first);
#   - the memory check fails closed, with a note, before loading
#     (TURF_SLM_ASSUME_FREE_MB);
#   - TURF_SLM_MODEL naming no file says so;
#   - the run's wall-clock budget fails closed (TURF_SLM_BUDGET_SECONDS, with
#     TURF_SLM_REPLAY_DELAY_MS making each replayed answer slow);
#   - the suggestion cache's fingerprint names the schema and the model.
#
# The determinism check and qualification need a model:
# scripts/slm/slm_determinism_check.sh, scripts/slm/qualify_model.sh.
#
# Usage: scripts/slm/slm_model_checks_smoke.sh
# Exit:  0 = every check passed; 1 = one did not.

set -u
cd "$(dirname "$0")/../.." || exit 1
TURF="./build/turfc"
[ -x "$TURF" ] || { echo "build/turfc missing -- build first"; exit 1; }
DIR=scripts/slm/slm_replay
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0

check() { # name, output, text that must be in it ('!' prefix: must not)
  local name=$1 out=$2 want=$3
  if [ "${want#!}" != "$want" ]; then
    if grep -qF -- "${want#!}" <<<"$out"; then echo "FAIL $name -- has: ${want#!}"; FAILED=$((FAILED + 1)); return; fi
  elif ! grep -qF -- "$want" <<<"$out"; then
    echo "FAIL $name -- lacks: $want"; FAILED=$((FAILED + 1)); return
  fi
  echo "PASS $name -- ${want}"
}

if "$TURF" --version 2>&1 | grep -q "smart built-in"; then
  head -c 4096 /dev/urandom >"$WORK/stranger.gguf"
  out=$(HOME="$WORK" TURF_SLM_DEBUG=1 TURF_SLM_MODEL="$WORK/stranger.gguf" TURF_SLM_ASSUME_FREE_MB=1 \
    "$TURF" --smart "$DIR/method_block.tr" -o "$WORK/bin" 2>&1)
  check memory "$out" "note: --smart needs about 512 MB free to load stranger.gguf and 1 MB is; showing the compiler's own diagnostics."
  check memory "$out" "[SLM] model: $WORK/stranger.gguf (llama="
  check memory "$out" "[E064"
  # With room to spare it is past the memory check: the note on qualification
  # comes next, then llama.cpp refuses the random bytes.
  out=$(HOME="$WORK" TURF_SLM_MODEL="$WORK/stranger.gguf" TURF_SLM_ASSUME_FREE_MB=1000000 \
    "$TURF" --smart "$DIR/method_block.tr" -o "$WORK/bin" 2>&1)
  check unqualified "$out" "note: --smart is running stranger.gguf, which is not a qualified model (scripts/slm/qualify_model.sh); its suggestions are checked the same way."
  check unqualified "$out" "[E064"
  # The SHA-256 is computed once per model file and kept beside the cache.
  if ls "$WORK/.cache/turf/slm/"model-*.sha256 >/dev/null 2>&1; then
    echo "PASS sha-memo -- the model's SHA-256 is kept"
  else
    echo "FAIL sha-memo -- no model-*.sha256 under the cache"; FAILED=$((FAILED + 1))
  fi
else
  echo "SKIP memory/unqualified -- this turfc has no model runtime"
fi

out=$(TURF_SLM_MODEL=/no/such/model.gguf "$TURF" --smart "$DIR/method_block.tr" -o "$WORK/bin" 2>&1)
check missing-model "$out" "note: TURF_SLM_MODEL names /no/such/model.gguf, which is not a file; showing the compiler's own diagnostics."

# Three failing answers, 700 ms each, against a 1-second budget: the second is
# cut off, the third never asked for, and the compiler's diagnostic stands.
out=$(TURF_SLM_DEBUG=1 TURF_SLM_BUDGET_SECONDS=1 TURF_SLM_REPLAY_DELAY_MS=700 \
  TURF_SLM_REPLAY="$DIR/method_block.three_failures.responses" \
  "$TURF" --smart "$DIR/method_block.tr" -o "$WORK/bin" 2>&1)
check budget "$out" "note: --smart stopped at its 1-second budget; showing the compiler's own diagnostics."
check budget "$out" "!(attempt 3/3)"
check budget "$out" "!Suggestion validated"
check budget "$out" "[SLM] conformance: answers=2 empty=1"

# The cache fingerprint: schema 4, and the model identity ("replay" here).
out=$(HOME="$WORK/c" TURF_SLM_REPLAY_CACHE=1 TURF_SLM_REPLAY="$DIR/method_block.exact_copy.responses" \
  "$TURF" --smart "$DIR/method_block.tr" -o "$WORK/bin" 2>&1)
first=$(head -1 "$(ls "$WORK"/c/.cache/turf/slm/*.json 2>/dev/null | head -1)" 2>/dev/null)
check fingerprint "$first" "schema4:"
check fingerprint "$first" ":replay"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "slm_model_checks_smoke: all checks passed"
  exit 0
fi
echo "slm_model_checks_smoke: $FAILED check(s) failed"
exit 1
