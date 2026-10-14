#!/bin/bash
#
# slm_replay_smoke.sh -- regression coverage for the half of `--smart` that
# runs AFTER the model: parsing its JSON, choosing where the replacement goes,
# the sandbox re-check, and the retry loop's feedback (issue #267).
#
# The model's output is not reproducible, so this path had no regression test
# at all, and it was where most correct suggestions were being lost: a right
# fix spliced at the wrong line, then reported back to the model as "your fix
# did not compile". TURF_SLM_REPLAY=<file> (src/slm/SLMIntegration.cpp,
# nextReplayResponse()) answers each model call with the next recorded
# response instead, so every case below is a real response shape, replayed
# deterministically, with no model and no cache involved. It also runs in a
# -DTURF_SMART=OFF build.
#
# Layout: scripts/slm/slm_replay/<program>.tr is the broken program, and
# scripts/slm/slm_replay/<program>.<case>.responses holds the model's answers,
# one per attempt, separated by a line that is exactly `%%`. Each CASES entry
# says whether the suggestion must validate (and the written fix must then
# compile and run to exit 0) or must be rejected.
#
# Usage: scripts/slm/slm_replay_smoke.sh
# Exit:  0 = every case behaved as expected; 1 = at least one did not.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

TURF="./build/turfc"
if [ ! -x "$TURF" ]; then
  echo -e "${RED}build/turfc missing -- build before running this smoke test.${RESET}"
  exit 1
fi

DIR="scripts/slm/slm_replay"

# program.case:expectation[|check]...
#
# The model gives no line numbers (original_code / new_code), so the cases are
# about the harness finding the copy. Each `check` is text the TURF_SLM_DEBUG
# log must contain; `!text` means it must NOT; `fixed:text` means the written
# fix file must contain it. (Checks are split on `|`, so they cannot contain
# one.)
CASES=(
  # The issue #267 shape: attempt 1 is genuinely wrong (no field `m`), and
  # attempt 2 is the right block. The feedback after attempt 1 must describe
  # attempt 1's own mistake, not a misplacement, and attempt 2 is what wins.
  "method_block.wrong_line_after_retry:validated|no field named 'm'|(attempt 2/3)"
  # A `:` sent as \u003a, which the old decoder left as literal text.
  "method_block.unicode_escape:validated"
  # The `>> 12 |` gutter echoed on every line of original_code and new_code.
  "method_block.echoed_gutter:validated|(exact copy): validated"
  # A change that does not fix the error must not validate anywhere, and the
  # model is told why instead of "it did not compile".
  "method_block.unchanged:rejected|new_code is the same code as original_code"

  # Locating original_code.
  # Exact copy, indentation and all: the error line itself.
  "method_block.exact_copy:validated|candidate [12, 12] (exact copy): validated|fixed:    return self.n to string;"
  # The same copy with its indentation stripped still matches, and the fix is
  # put back at the indentation the source has.
  "method_block.different_indentation:validated|candidate [12, 12] (exact copy): validated|fixed:    return self.n to string;"
  # One typo in the copy (`slef`): fuzzy-matched to the same line.
  "method_block.fuzzy_copy:validated|candidate [12, 12] (fuzzy copy): validated"
  # A copy that is nowhere in the program: no guessing a line for new_code
  # (which would have validated, being right for line 12). The attempt fails
  # with the copying message, which reaches the model, and attempt 2 wins. The
  # message quotes the error line instead of numbering it: the model is shown no
  # line numbers (C5), so a number would only invite it to count again.
  "method_block.not_in_program:validated|original_code does not appear in the program; copy the lines exactly|The line the error is on reads: return self.n;|!The error is on line|(attempt 2/3)"
  # The same line twice: the copy nearest the error wins, in either order.
  "dup_second.nearest_wins:validated|candidate [11, 11] (exact copy): validated|!candidate [7, 7]"
  "dup_first.nearest_wins:validated|candidate [7, 7] (exact copy): validated|!candidate [11, 11]"
  # An empty original_code is an insertion right after the error line (the
  # function header, line 1).
  "insert_line.empty_original:validated|candidate [1, 1] (insertion after the error line): validated"
  # An empty new_code is never a fix (it would delete the line), and says so.
  "method_block.empty_new_code:rejected|new_code was empty"

  # What the model is shown (workstream C). The logs carry both prompts
  # (TURF_SLM_DEBUG prints the user turn, TURF_SLM_DUMP_PROMPT the system turn).
  #
  # C5: the code is the file's own text, verbatim: no `>> 12 |` / `   11 |`
  # gutter, no "Code Context" header, no marker in any line. The error line is
  # its own field, so a copy of any line of "code" is a valid original_code. The
  # system prompt's rule 2 no longer talks about numbers or markers to leave out.
  "method_block.exact_copy:validated|\"error_line\": \"    return self.n;\"|fn Box.count(): string {\\n    return self.n;\\n}|!>> 12 |!Code Context|!   11 | |!WITHOUT the line numbers|line(s) of \"code\" that your fix changes"
  # C2: the syntax card is always in the system prompt, ahead of the docs.
  "method_block.exact_copy:validated|TURF SYNTAX (only this; not C, Python or Rust):|fn<T> Box.get(): T { return self.v; }|Syntax card: docs/slm/syntax_card.txt"
  # C4: the declaration an error sits in, when the code window does not reach
  # its header (describe() is longer than the +-8 lines shown) ...
  "long_function.facts:validated|\"inside\": \"fn describe(n: int): string\"|\"known_types\": [\"sum: int\"]|\"error_line\": \"    return sum;\""
  # ... the struct a field error is about, whole, from the compiler's own type ...
  "unknown_field.receiver:validated|\"receiver\": \"struct Box { v: int, n: int }\""
  # ... and the candidate overloads of an IMPORTED function, with the lines
  # read from the module's own file (these used to be lines of the program).
  "imported_callee.related:validated|\"related\": [{\"code\": \"pub fn abs(val: double): double {\", \"note\": \"candidate\"}|math.tr, line"
)

FAILED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for entry in "${CASES[@]}"; do
  head="${entry%%|*}"
  checks="${entry#"$head"}" # empty, or "|check|check..."
  name="${head%%:*}"
  expect="${head#*:}"
  program="$DIR/${name%%.*}.tr"
  responses="$DIR/$name.responses"
  fixed="$TMP/$name.fixed.tr"

  output=$(TURF_SLM_DEBUG=1 TURF_SLM_DUMP_PROMPT=1 TURF_SLM_REPLAY="$responses" \
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
  IFS='|' read -r -a check_list <<<"${checks#|}"
  # ${a[@]+...}: bash 3.2 (macOS) calls an empty array unbound under `set -u`.
  for check in ${check_list[@]+"${check_list[@]}"}; do
    [ -z "$check" ] && continue
    case "$check" in
    "!"*)
      if grep -qF -- "${check#!}" <<<"$output"; then
        echo -e "${RED}✗${RESET} $name -- log has: ${check#!}"
        ok=0
      fi
      ;;
    "fixed:"*)
      if ! grep -qF -- "${check#fixed:}" "$fixed" 2>/dev/null; then
        echo -e "${RED}✗${RESET} $name -- written fix lacks: ${check#fixed:}"
        ok=0
      fi
      ;;
    *)
      if ! grep -qF -- "$check" <<<"$output"; then
        echo -e "${RED}✗${RESET} $name -- log lacks: $check"
        ok=0
      fi
      ;;
    esac
  done

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
