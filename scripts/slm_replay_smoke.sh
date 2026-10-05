#!/bin/bash
#
# slm_replay_smoke.sh -- regression coverage for the half of `--smart` that
# runs AFTER the model: parsing its JSON, choosing where the replacement goes,
# the sandbox re-check, and the retry loop's feedback (issue #267).
#
# The model's output is not reproducible, so this path had no regression test
# at all, and it was where most correct suggestions were being lost: a right
# fix spliced at the wrong line, then reported back to the model as "your fix
# did not compile". TURF_SLM_REPLAY=<file> (src/SLMIntegration.cpp,
# nextReplayResponse()) answers each model call with the next recorded
# response instead, so every case below is a real response shape, replayed
# deterministically, with no model and no cache involved. It also runs in a
# -DTURF_SMART=OFF build.
#
# Layout: scripts/slm_replay/<program>.tr is the broken program, and
# scripts/slm_replay/<program>.<case>.responses holds the model's answers,
# one per attempt, separated by a line that is exactly `%%`. Each CASES entry
# says whether the suggestion must validate (and the written fix must then
# compile and run to exit 0) or must be rejected.
#
# Usage: scripts/slm_replay_smoke.sh
# Exit:  0 = every case behaved as expected; 1 = at least one did not.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

TURF="./build/turfc"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi

DIR="scripts/slm_replay"

# program.case:expectation[:required text in the debug log]
CASES=(
  # The issue #267 shape: attempt 1 is genuinely wrong (no field `m`), and
  # attempt 2 is the right block but says target_line=10, the blank line
  # above it. The feedback after attempt 1 must describe attempt 1's own
  # mistake, not a misplacement.
  "method_block.wrong_line_after_retry:validated:no field named 'm'"
  # A `:` sent as \u003a, which the old decoder left as literal text.
  "method_block.unicode_escape:validated"
  # The `>> 11 |` gutter echoed on every line, not just the first.
  "method_block.echoed_gutter:validated"
  # A multi-line range shifted one line up: [10, 12] for [11, 13].
  "method_block.shifted_range:validated"
  # A change that does not fix the error must not validate anywhere.
  "method_block.unchanged:rejected"
)

FAILED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for entry in "${CASES[@]}"; do
  name="${entry%%:*}"
  rest="${entry#*:}"
  expect="${rest%%:*}"
  needle=""
  [ "$rest" != "$expect" ] && needle="${rest#*:}"
  program="$DIR/${name%%.*}.tr"
  responses="$DIR/$name.responses"
  fixed="$TMP/$name.fixed.tr"

  output=$(TURF_SLM_DEBUG=1 TURF_SLM_REPLAY="$responses" \
    "$TURF" --smart "--smart-write-fix=$fixed" "$program" 2>&1)

  ok=1
  if [ "$expect" = "validated" ]; then
    if ! grep -q "Suggestion validated" <<<"$output"; then
      echo -e "${RED}✗${RESET} $name -- suggestion did not validate"
      ok=0
    elif [ ! -s "$fixed" ]; then
      echo -e "${RED}✗${RESET} $name -- no fixed file written"
      ok=0
    elif ! "$TURF" "$fixed" -o "$TMP/bin" > /dev/null 2>&1; then
      echo -e "${RED}✗${RESET} $name -- written fix does not compile"
      ok=0
    elif ! "$TMP/bin" > /dev/null 2>&1; then
      echo -e "${RED}✗${RESET} $name -- written fix exited non-zero"
      ok=0
    fi
  else
    if grep -q "Suggestion validated" <<<"$output"; then
      echo -e "${RED}✗${RESET} $name -- a non-fix validated"
      ok=0
    fi
  fi
  if [ -n "$needle" ] && ! grep -qF "$needle" <<<"$output"; then
    echo -e "${RED}✗${RESET} $name -- log lacks: $needle"
    ok=0
  fi

  if [ "$ok" -eq 1 ]; then
    echo -e "${GREEN}✓${RESET} $name -- $expect"
  else
    FAILED=$((FAILED + 1))
    echo "$output" | grep -vE "^llama_|^~llama|^ggml" | sed 's/^/    /' | tail -40
  fi
done

echo
if [ "$FAILED" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All SLM replay cases behaved as expected.${RESET}"
  exit 0
fi
echo -e "${RED}${BOLD}$FAILED replay case(s) failed.${RESET}"
exit 1
