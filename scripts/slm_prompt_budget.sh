#!/bin/bash
# SLM_IMPROVEMENT_PLAN workstream C3: the system prompt must fit the model's
# batch, for every category doc, with the biggest per-code page on top.
#
# SLMEngine::generateSuggestion() decodes the system prompt in ONE llama_decode
# call, so it is capped at n_batch (4096 tokens, src/SLMEngine.cpp). Past the
# cap it keeps the head and the last 1024 tokens and drops the MIDDLE -- which
# keeps the rules but silently loses the end of the documentation (issue #267:
# general.txt alone was ~3500 tokens, so every GENERAL prompt lost its rules
# until the middle-drop was added). Going over no longer breaks the rules, but
# the model still never sees what was dropped, and nothing said so.
#
# The prompt is measured, never estimated, and it is the REAL prompt:
# fetchSuggestion() prints the system prompt it builds when TURF_SLM_DUMP_PROMPT
# is set, so the scaffold (preamble, the syntax card, the rules) is whatever the
# compiler really sends, not a second copy of it that can drift. It is then
# re-assembled for every category doc with the largest docs/slm/codes page, the
# worst case a diagnostic of that category can produce, and tokenized with
# llama.cpp's own llama-tokenize. The vocab-only qwen2 GGUF that ships in the
# llama.cpp submodule gives the same counts as the 1 GB model (checked on the
# docs), so no model download is needed -- this runs wherever llama.cpp is built.
#
# Usage: scripts/slm_prompt_budget.sh [--table]   (--table prints every row)
# Exit:  0 = every prompt fits, or the tokenizer is not built (SKIP)
#        1 = some prompt is over the cap, 2 = build/turfc is missing
set -u
cd "$(dirname "$0")/.." || exit 2

LIMIT=4096 # n_batch, src/SLMEngine.cpp
TURFC="${TURF_COMPILER:-./build/turfc}"
TOKENIZE=external/llama.cpp/build/bin/llama-tokenize
VOCAB=external/llama.cpp/models/ggml-vocab-qwen2.gguf
REAL_MODEL=models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf
FIXTURE=scripts/slm_replay/method_block.tr # E064: TYPE_MISMATCH, has a code page

[ -x "$TURFC" ] || { echo "slm_prompt_budget: $TURFC is missing -- build first"; exit 2; }
if [ ! -x "$TOKENIZE" ]; then
    echo "slm_prompt_budget: SKIP -- $TOKENIZE is not built (llama.cpp's tools); nothing was measured"
    exit 0
fi
MODEL="$VOCAB"
[ -f "$MODEL" ] || MODEL="$REAL_MODEL"
if [ ! -f "$MODEL" ]; then
    echo "slm_prompt_budget: SKIP -- neither $VOCAB nor $REAL_MODEL exists, so nothing can be tokenized"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

tokens() { # tokens of the text in file $1
    "$TOKENIZE" -m "$MODEL" -f "$1" --ids --log-disable 2>/dev/null | tr ',' '\n' | wc -l
}

# The prompt the compiler really builds for the fixture's E064, with no model.
TURF_SLM_DUMP_PROMPT=1 TURF_SLM_REPLAY=/dev/null "$TURFC" --smart "$FIXTURE" 2>&1 |
    sed -E 's/\x1b\[[0-9;]*m//g' |
    awk '/^--- SYSTEM PROMPT ---$/{f=1;next} /^--- END SYSTEM PROMPT ---$/{f=0} f' >"$TMP/captured"
[ -s "$TMP/captured" ] || { echo "slm_prompt_budget: the compiler printed no system prompt (is TURF_SLM_DUMP_PROMPT still honoured?)"; exit 2; }

# Split it at the markers fetchSuggestion() puts between the parts:
#   <scaffold + card> THIS ERROR (E064):\n <page> \nGENERAL REFERENCE:\n <doc> \nRULES: <rules>
captured=$(cat "$TMP/captured"; printf x)
captured="${captured%x}"
page_marker="THIS ERROR (E064):"$'\n'
sep=$'\n'"GENERAL REFERENCE:"$'\n'
rules=$'\n'"RULES:"$'\n'
case "$captured" in
*"$page_marker"*"$sep"*"$rules"*) ;;
*) echo "slm_prompt_budget: the captured prompt lacks the THIS ERROR / GENERAL REFERENCE / RULES markers"; exit 2 ;;
esac
head_part="${captured%%"$page_marker"*}"
after_head="${captured#*"$page_marker"}"
tail_part="${after_head#*"$rules"}"
tail_part="$rules$tail_part"

# The card is inside head_part, after "Error category: X\n\n"; separate it only
# to report it. (If there is no card the prompt just has none.)
cat_marker=$'\n\n'"TURF SYNTAX"
scaffold_only="${head_part%%"$cat_marker"*}"
printf '%s' "$head_part" >"$TMP/head"
printf '%s' "$scaffold_only" >"$TMP/scaffold"
printf '%s' "$tail_part" >"$TMP/tail"
n_head=$(tokens "$TMP/head")
n_scaffold=$(tokens "$TMP/scaffold")
n_tail=$(tokens "$TMP/tail")

# The category docs the compiler can load: every docs/slm/*.txt path named in
# getCategoryDocPath() (include/ErrorCategory.h), so a new one is covered
# without touching this script.
DOCS=$(grep -oE 'docs/slm/[a-z_]+\.txt' include/ErrorCategory.h | sort -u)

# The largest per-code page, by tokens (the biggest few by size are tokenized).
biggest=""
biggest_n=0
for f in $(ls -S docs/slm/codes/E*.txt | head -6); do
    n=$(tokens "$f")
    if [ "$n" -gt "$biggest_n" ]; then biggest_n=$n; biggest=$f; fi
done
n_card=$((n_head - n_scaffold))

fail=0
rows=""
worst_total=0
worst_doc=""
for doc in $DOCS; do
    [ -f "$doc" ] || { echo "FAIL $doc is named in include/ErrorCategory.h but does not exist"; fail=1; continue; }
    {
        cat "$TMP/head"
        printf '%s' "$page_marker"
        cat "$biggest"
        printf '%s' "$sep"
        cat "$doc"
        cat "$TMP/tail"
    } >"$TMP/full"
    total=$(tokens "$TMP/full")
    n_doc=$(tokens "$doc")
    verdict="ok"
    if [ "$total" -gt "$LIMIT" ]; then verdict="OVER by $((total - LIMIT))"; fail=1; fi
    rows+=$(printf '%-34s %6d %6d  %s' "$doc" "$n_doc" "$total" "$verdict")$'\n'
    if [ "$total" -gt "$worst_total" ]; then worst_total=$total; worst_doc=$doc; fi
done

echo "system prompt budget: n_batch cap = $LIMIT tokens (tokenizer vocab: $(basename "$MODEL"))"
echo "  scaffold (preamble + rules, no docs)  $((n_scaffold + n_tail)) tokens"
echo "  syntax card                           $n_card tokens"
echo "  largest code page                     $biggest_n tokens ($(basename "$biggest"))"
printf '%-34s %6s %6s  %s\n' "category doc" "doc" "total" "(total = scaffold + card + largest page + doc)"
printf '%s' "$rows"

if [ $fail -ne 0 ]; then
    echo "slm_prompt_budget: FAILED -- over the $LIMIT-token cap, the model would lose the middle of its documentation; trim the doc"
    exit 1
fi
echo "slm_prompt_budget: OK (worst case $worst_total tokens, $worst_doc, cap $LIMIT)"
exit 0
