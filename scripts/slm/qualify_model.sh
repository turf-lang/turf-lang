#!/bin/bash
#
# qualify_model.sh -- SL-4: measure one model for --smart and say whether it
# meets the published thresholds (docs/slm/qualified_models.tsv's header).
#
# Runs, with TURF_SLM_MODEL=<file.gguf> (the model is not moved anywhere):
#   1. scripts/slm/slm_determinism_check.sh;
#   2. the benchmark's train split on both paths, rules and model
#      (scripts/slm/slm_benchmark/run_benchmark.py), recorded under
#      bench/results/smart/qualify/<date>-<sha256 first 12>.json;
# and prints the model's SHA-256, utility, false-confidence rate, latency and
# peak RSS. With --record, a model that meets every threshold is added to
# docs/slm/qualified_models.tsv (commit it with the record).
#
# Hours on a CPU: the model path compiles every train case with --smart.
#
# Usage: scripts/slm/qualify_model.sh <file.gguf> [--record] [--jobs N]
# Exit:  0 = qualified; 1 = measured, not qualified; 2 = could not measure.

set -u
cd "$(dirname "$0")/../.." || exit 2
MODEL=""
RECORD=0
JOBS=2
while [ $# -gt 0 ]; do
  case "$1" in
  --record) RECORD=1 ;;
  --jobs) JOBS=$2; shift ;;
  *) MODEL=$1 ;;
  esac
  shift
done
[ -f "$MODEL" ] || { echo "usage: $0 <file.gguf> [--record] [--jobs N]"; exit 2; }
MODEL=$(cd "$(dirname "$MODEL")" && pwd)/$(basename "$MODEL")
./build/turfc --version 2>&1 | grep -q "smart built-in" ||
  { echo "qualify_model: build/turfc has no model runtime; build without --no-smart"; exit 2; }

if command -v sha256sum >/dev/null; then SHA=$(sha256sum "$MODEL" | cut -d' ' -f1)
else SHA=$(shasum -a 256 "$MODEL" | cut -d' ' -f1); fi
DATE=$(date +%F)
OUT=bench/results/smart/qualify/$DATE-${SHA:0:12}.json
mkdir -p "$(dirname "$OUT")"
echo "model: $(basename "$MODEL")  sha256: $SHA"

export TURF_SLM_MODEL="$MODEL"
DET=pass
env -u TURF_HOME scripts/slm/slm_determinism_check.sh || DET=fail
env -u TURF_HOME python3 scripts/slm/slm_benchmark/run_benchmark.py --split train --paths rules,model \
  --jobs "$JOBS" --out "$OUT" || { echo "qualify_model: the benchmark failed"; exit 2; }

python3 - "$OUT" "$SHA" "$(basename "$MODEL")" "$DATE" "$DET" "$RECORD" <<'EOF'
import json, sys
out, sha, name, date, det, record = sys.argv[1:]
rec = json.load(open(out))
m, diff = rec["summary"]["model"], rec["model_minus_rules"]
prov = m["by_provenance"].get("model", {})
fc = m["false_confidence"]
checks = [
    ("utility: model - rules >= 0", diff["mean"] >= 0,
     f"{diff['mean']:+.3f} [{diff['ci95'][0]:+.3f}, {diff['ci95'][1]:+.3f}]"),
    ("false confidence <= 20%", fc is not None and fc <= 0.20, "-" if fc is None else f"{100 * fc:.1f}%"),
    ("determinism", det == "pass", det),
]
print(f"\nmodel utility {m['utility']:+.3f} [{m['ci95'][0]:+.3f}, {m['ci95'][1]:+.3f}], "
      f"{m['shown']} shown; latency p50 {prov.get('p50_s', '-')} s, p95 {prov.get('p95_s', '-')} s; "
      f"peak RSS {prov.get('peak_rss_mb', '-')} MB")
print("conformance: " + " ".join(f"{k}={v}" for k, v in m["conformance"].items()))
for what, ok, got in checks:
    print(f"{'PASS' if ok else 'FAIL'} {what}: {got}")
qualified = all(ok for _, ok, _ in checks)
print(f"\n{name}: {'qualified' if qualified else 'not qualified'} (record: {out})")
if qualified and record == "1":
    with open("docs/slm/qualified_models.tsv", "a") as f:
        f.write(f"{sha}\t{name}\t{date}\t{out}\n")
    print("added to docs/slm/qualified_models.tsv")
sys.exit(0 if qualified else 1)
EOF
