#!/usr/bin/env python3
"""SL-4 drift: has a qualified model's --smart utility dropped?

Compares a benchmark record of the model path (run_benchmark.py --paths
rules,model --split train) with the qualification record of the same model
(bench/results/smart/qualify/*-<sha256 first 12>.json, the newest). Fails when
the new run's model utility is below the qualification's 95% interval, or its
conformance counts (empty answers, parse failures, grammar escapes, splice
failures) rose per answer: the first is the model or the prompt drifting, the
second the harness regressing.

    python3 scripts/slm/slm_benchmark/check_drift.py <record.json>
"""

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def rates(conf):
    n = max(1, conf.get("answers", 0))
    return {k: v / n for k, v in conf.items() if k != "answers"}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    new = json.load(open(sys.argv[1]))
    sha = (new["meta"].get("model") or {}).get("sha256")
    if not sha:
        sys.exit("check_drift: the record names no model SHA-256 (was the model path run?)")
    quals = sorted((REPO / "bench/results/smart/qualify").glob(f"*-{sha[:12]}.json"))
    if not quals:
        sys.exit(f"check_drift: no qualification record for {sha[:12]} (scripts/slm/qualify_model.sh)")
    old = json.load(open(quals[-1]))
    m_new, m_old = new["summary"]["model"], old["summary"]["model"]
    bad = 0
    lo = m_old["ci95"][0]
    ok = m_new["utility"] >= lo
    bad += not ok
    print(f"{'PASS' if ok else 'FAIL'} utility {m_new['utility']:+.3f} (qualified {m_old['utility']:+.3f}, "
          f"interval from {lo:+.3f}; {quals[-1].name})")
    r_new, r_old = rates(m_new.get("conformance", {})), rates(m_old.get("conformance", {}))
    for k, v in r_new.items():
        ok = v <= r_old.get(k, 0) + 1e-9
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'} {k} per answer {v:.3f} (qualified {r_old.get(k, 0):.3f})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
