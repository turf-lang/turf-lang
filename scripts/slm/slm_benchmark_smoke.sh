#!/bin/bash
#
# slm_benchmark_smoke.sh -- SL-2: the `--smart` scorer (run_benchmark.py)
# classifies and judges outcomes the way it says it does. No model: the model
# path answers from a recorded response (TURF_SLM_REPLAY), so this runs in CI
# and in a -DTURF_SMART=OFF build.
#
# Cases (written here, so they never go stale with the corpus):
#   typo        a misspelt name: the compiler's own fix, judged by running the
#               fixed program (oracle asserts)              -> rules: correct
#   hard        two equally good renames (oracle abstain)   -> rules: silent
#   replay_ok   the replayed answer fixes the program       -> model: correct
#   replay_bad  the same answer, but the program must print
#               something it never prints                    -> model: wrong, -K
#
# Usage: scripts/slm/slm_benchmark_smoke.sh      Exit: 0 pass, 1 fail.

set -u
cd "$(dirname "$0")/../.." || exit 1
[ -x build/turfc ] || { echo "FAIL: build/turfc missing"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK/cases.jsonl" <<'EOF'
import json, sys
def case(id, src, oracle, out="", code="E040", rel="scripts/slm/slm_benchmark_smoke.sh"):
    return dict(id=id, split="train", source_test=rel, package=None, kind="smoke", oracle=oracle, line=1,
                original_line="", mutated_line="", mutated_source=src, diagnostic="", error_code=code,
                error_message="", diag_line=1, expected_stdout=out, expected_exit=0)
typo = 'fn main(): int {\n    immut name: string = "ok";\n    printline(nmae);\n    return 0;\n}\n'
hard = open("scripts/slm/slm_benchmark/hard_negatives/two_equal_renames.tr").read()
rep = open("scripts/slm/slm_replay/insert_line.tr").read()
rows = [case("typo", typo, "asserts", "ok\n"), case("hard", hard, "abstain"),
        case("replay_ok", rep, "asserts", "", "E106"), case("replay_bad", rep, "asserts", "never printed\n", "E106")]
open(sys.argv[1], "w").write("".join(json.dumps(r) + "\n" for r in rows))
EOF

env -u TURF_HOME TURF_SLM_REPLAY="$PWD/scripts/slm/slm_replay/insert_line.empty_original.responses" \
  python3 scripts/slm/slm_benchmark/run_benchmark.py --cases "$WORK/cases.jsonl" --paths rules,model \
  --boots 200 --jobs 1 --out "$WORK/record.json" > "$WORK/log" 2>&1 || { cat "$WORK/log"; echo "FAIL: run_benchmark.py exited nonzero"; exit 1; }

python3 - "$WORK/record.json" <<'EOF'
import json, sys
rec = json.load(open(sys.argv[1]))
got = {(r["path"], r["id"]): r for p in rec["cases"].values() for r in p}
want = [("rules", "typo", "rule", "correct", 1), ("rules", "hard", "none", "none", 0),
        ("model", "replay_ok", "model", "correct", 1), ("model", "replay_bad", "model", "wrong", -3)]
bad = 0
for path, id, prov, outcome, util in want:
    r = got[(path, id)]
    ok = (r["provenance"], r["outcome"], r["utility"]) == (prov, outcome, util)
    print(("PASS" if ok else "FAIL") + f": {path}/{id}: {r['provenance']} {r['outcome']} {r['utility']:+g}"
          + ("" if ok else f" (want {prov} {outcome} {util:+d})"))
    bad += not ok
s = rec["summary"]["rules"]
if s["hard_negative_silence"] != 1.0:
    print("FAIL: rules path not silent on the hard negative"); bad += 1
if rec["model_minus_rules"]["cases"] != 4:
    print("FAIL: model - rules is not paired over all 4 cases"); bad += 1
sys.exit(1 if bad else 0)
EOF
rc=$?
[ $rc -eq 0 ] && echo "slm_benchmark_smoke: all checks passed" || { cat "$WORK/log"; }
exit $rc
