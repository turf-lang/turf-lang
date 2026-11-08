#!/usr/bin/env python3
"""check_heldout.py -- SL-3: prompt and weight tuning never read the held-out split.

The benchmark corpus is split by source file into corpus/train.jsonl and
corpus/heldout.jsonl (build_corpus.py). Anything that tunes the `--smart`
path -- prompts, few-shot examples, Fix Score weights, thresholds -- reads
train only. This check fails when a file under scripts/slm/, src/slm/ or
docs/slm/ names the held-out split, except:

  - build_corpus.py, which writes it;
  - run_benchmark.py, which only scores against it;
  - this file and the corpus README.

Usage: python3 scripts/slm/slm_benchmark/check_heldout.py   (exit 1 on a leak)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ALLOWED = {
    "scripts/slm/slm_benchmark/build_corpus.py",
    "scripts/slm/slm_benchmark/run_benchmark.py",
    "scripts/slm/slm_benchmark/check_heldout.py",
    "scripts/slm/slm_benchmark/README.md",
    "scripts/slm/slm_benchmark/corpus/STATS.md",
    "scripts/slm/slm_benchmark/corpus/heldout.jsonl",
}
leaks = []
for base in ("scripts/slm", "src/slm", "docs/slm"):
    for p in sorted((ROOT / base).rglob("*")):
        rel = p.relative_to(ROOT).as_posix()
        if not p.is_file() or rel in ALLOWED or "__pycache__" in rel:
            continue
        try:
            text = p.read_text(errors="replace")
        except OSError:
            continue
        if "heldout" in text:
            leaks.append(rel)
if leaks:
    print("check_heldout: these files name the held-out split; tuning must read train only:")
    for rel in leaks:
        print("  " + rel)
    sys.exit(1)
print("check_heldout: OK (only build_corpus.py and run_benchmark.py name the held-out split)")
