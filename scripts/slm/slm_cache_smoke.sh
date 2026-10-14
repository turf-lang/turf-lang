#!/bin/bash
#
# slm_cache_smoke.sh -- the on-disk `--smart` cache (src/slm/SLMIntegration.cpp,
# readSlmCache()/writeSlmNegativeCache()), without the model.
#
# A VALIDATED suggestion is cached as `<key>.json`; "the model could not fix
# this" (it answered NONE, or all three attempts failed) is cached separately
# as `<key>.neg` for 24 hours (kSlmNegativeTtl), so a repeat run skips the
# model. This runs the replay harness (scripts/slm/slm_replay/) with
# TURF_SLM_REPLAY_CACHE=1, the opt-in that makes a replay run use the cache,
# and HOME pointed at a temp directory, so it never touches ~/.cache/turf/slm.
# "The model was skipped" means the log has neither the user prompt nor a raw
# response: a hit returns before either is reached.
#
# Usage: scripts/slm/slm_cache_smoke.sh
# Exit:  0 = every check held; 1 = at least one did not.

set -u
cd "$(dirname "$0")/../.." || exit 1

GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"
TURF="./build/turfc"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi

DIR="scripts/slm/slm_replay"
PROGRAM="$DIR/method_block.tr"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export HOME="$TMP/home"
CACHE="$HOME/.cache/turf/slm"
mkdir -p "$HOME"

FAILED=0
pass() { echo -e "${GREEN}✓${RESET} $1"; }
fail() { echo -e "${RED}✗${RESET} $1"; FAILED=$((FAILED + 1)); }

# run <responses-name> [extra env assignments...] -> $OUT
run() {
  local responses="$DIR/$1.responses"
  shift
  OUT=$(env TURF_SLM_DEBUG=1 TURF_SLM_REPLAY="$responses" "$@" \
    "$TURF" --smart "$PROGRAM" 2>&1)
}
has() { grep -qF -- "$1" <<<"$OUT"; }
# The model's whole path: the prompt is built and a response consumed.
model_ran() { has "--- USER PROMPT ---" && has "SLM RAW RESPONSE DEBUG"; }
count() { command ls "$CACHE"/*."$1" 2>/dev/null | wc -l; }

# 0. Plain replay bypasses the cache in both directions.
run method_block.none
if model_ran && [ ! -e "$CACHE" ]; then
  pass "replay without TURF_SLM_REPLAY_CACHE writes no cache"
else
  fail "replay without TURF_SLM_REPLAY_CACHE touched the cache"
fi

# 1. A NONE answer is cached negatively, and the second run skips the model.
run method_block.none TURF_SLM_REPLAY_CACHE=1
if model_ran && has "Fix mode is NONE" && [ "$(count neg)" = 1 ] &&
   [ "$(count json)" = 0 ]; then
  pass "NONE: first run asks the model and writes one .neg, no .json"
else
  fail "NONE: first run"
  echo "$OUT" | tail -20
fi
run method_block.none TURF_SLM_REPLAY_CACHE=1
if has "Negative cache hit (none" && ! model_ran; then
  pass "NONE: second run is a negative hit and never reaches the model"
else
  fail "NONE: second run did not skip the model"
fi
# The plain diagnostic is still shown, never a suggestion.
if has "Suggestion validated" || has "[AI Suggestion]"; then
  fail "NONE: a negative hit produced a suggestion"
else
  pass "NONE: a negative hit shows no suggestion"
fi

# 2. A stale negative (older than 24h) is ignored and deleted; the model runs
# again, and a validated fix then replaces it: a .json, no .neg, and the
# following run is a positive hit that skips the model.
touch -d '25 hours ago' "$CACHE"/*.neg
run method_block.exact_copy TURF_SLM_REPLAY_CACHE=1
if model_ran && has "Suggestion validated" && [ "$(count json)" = 1 ] &&
   [ "$(count neg)" = 0 ] && ! has "Negative cache hit"; then
  pass "expired negative: model runs again; the fix is cached and the .neg gone"
else
  fail "expired negative"
  echo "$OUT" | tail -20
fi
run method_block.none TURF_SLM_REPLAY_CACHE=1
if has "[SLM] Cache hit" && has "Suggestion validated" && ! model_ran &&
   ! has "Negative cache hit"; then
  pass "positive entry is served as the fix (a negative never shadows it)"
else
  fail "positive hit"
fi

# 3. Three failed attempts are cached negatively too.
rm -rf "$CACHE"
run method_block.three_failures TURF_SLM_REPLAY_CACHE=1
if has "failed after 3 attempts" && [ "$(count neg)" = 1 ] &&
   [ "$(count json)" = 0 ]; then
  pass "3 failed attempts: one .neg, no .json"
else
  fail "3 failed attempts: first run"
fi
run method_block.three_failures TURF_SLM_REPLAY_CACHE=1
if has "Negative cache hit (failed" && ! model_ran; then
  pass "3 failed attempts: second run skips the model"
else
  fail "3 failed attempts: second run did not skip the model"
fi

# 4. A retry is never blocked, and a run that ends in a fix leaves no negative:
# attempt 1 is wrong, attempt 2 is right.
rm -rf "$CACHE"
run method_block.wrong_line_after_retry TURF_SLM_REPLAY_CACHE=1
if has "(attempt 2/3)" && [ "$(count neg)" = 0 ] && [ "$(count json)" = 1 ]; then
  pass "retry reaches the model and its fix is cached, no negative"
else
  fail "retry run"
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo -e "${GREEN}SLM cache smoke test passed.${RESET}"
  exit 0
fi
echo -e "${RED}$FAILED SLM cache check(s) failed.${RESET}"
exit 1
