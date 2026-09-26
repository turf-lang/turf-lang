#!/usr/bin/env python3
"""run_benchmark.py -- SLM-001 §16.4's measurable success criteria, computed
against the fixed corpus mutate.py builds (scripts/slm_benchmark/corpus.json).

For each corpus case, compiles the mutated source WITH --smart and classifies
the outcome from the compiler's own stdout, then reports the metrics table
§16.4 defines:

    Deterministic-fix rate       | % fixed with no model invocation
    Sandbox-validation rate      | % of model suggestions that compile and
                                    clear the original error
    Semantic-correctness rate    | NOT computed by this tool -- see below
    No-suggestion rate           | % where nothing is shown
    False-confidence rate        | NOT computed by this tool -- see below
    p50 / p95 latency            | wall clock per diagnostic
    Cache hit rate               | among model-path attempts only

Deliberately NOT computed here, and why:
  - Semantic-correctness rate needs the accepted patch re-applied to a real
    copy of the file and that file's OWN test::assert() calls re-run, to
    confirm the fix restored MEANING, not just compilability. `--smart`
    itself never writes a fix back to disk (confirmed live: exit code is 1
    either way, even when a suggestion displays) -- the corrected line only
    ever exists as the in-memory Sandbox-committed text this script would
    have to scrape back out of the rendered diagnostic's pinned-context
    display. That scraper is a real, separate piece of tooling, not a
    natural extension of this script, and was deliberately left unbuilt
    this session -- see docs/development/changes/SLM-001.md's Known
    pitfalls.
  - False-confidence rate needs an ORACLE (a human, or the semantic-
    correctness check above) to judge whether a *displayed* (i.e. already
    sandbox-validated, confidence >= 0.8) suggestion was actually wrong.
    Sandbox validation proves "compiles and the original error is gone", not
    "is correct" -- the same gap semantic-correctness exists to close.

Usage:
    python3 scripts/slm_benchmark/run_benchmark.py

Expect this to take a while: every non-deterministic-fix case loads the
~1GB GGUF model and runs real inference (confirmed live: tens of seconds per
case on the machine this was built on), same cost `scripts/test_ai.sh`
already has for the same reason. Deterministic-fix cases are fast (no model
load at all -- see slm_deterministic_fix_smoke.sh's own timing-ceiling
assertion for that same property, checked mechanically there).
"""
from __future__ import annotations

import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TURF = REPO_ROOT / "build" / "turf"
CORPUS_PATH = Path(__file__).resolve().parent / "corpus.json"
SCRATCH = Path("/tmp/turf_slm_benchmark_run.tr")


def run_one(mutated_source: str) -> tuple[str, float]:
    SCRATCH.write_text(mutated_source)
    env = dict(os.environ)
    env["TURF_SLM_DEBUG"] = "1"  # needed to observe "[SLM] Cache hit" lines
    start = time.monotonic()
    proc = subprocess.run(
        [str(TURF), "--smart", str(SCRATCH)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        env=env,
    )
    elapsed = time.monotonic() - start
    return proc.stdout + proc.stderr, elapsed


def classify(output: str) -> str:
    if "[Deterministic Fix]" in output:
        return "deterministic_fix"
    if "[AI Suggestion]" in output:
        return "model_fix"
    return "no_suggestion"


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    k = (len(ordered) - 1) * pct
    f, c = int(k), min(int(k) + 1, len(ordered) - 1)
    if f == c:
        return ordered[f]
    return ordered[f] + (ordered[c] - ordered[f]) * (k - f)


def main() -> int:
    if not TURF.exists():
        print(f"error: {TURF} missing -- build the compiler first", file=sys.stderr)
        return 1
    if not CORPUS_PATH.exists():
        print(f"error: {CORPUS_PATH} missing -- run mutate.py first", file=sys.stderr)
        return 1

    corpus = json.loads(CORPUS_PATH.read_text())
    if not corpus:
        print("error: corpus.json is empty", file=sys.stderr)
        return 1

    outcomes: dict[str, int] = {"deterministic_fix": 0, "model_fix": 0, "no_suggestion": 0}
    latencies: list[float] = []
    cache_hits = 0
    model_path_attempts = 0

    for case in corpus:
        output, elapsed = run_one(case["mutated_source"])
        outcome = classify(output)
        outcomes[outcome] += 1
        latencies.append(elapsed)

        is_cache_hit = "[SLM] Cache hit" in output
        if outcome != "deterministic_fix":
            model_path_attempts += 1
            if is_cache_hit:
                cache_hits += 1

        print(f"{case['id']:32s} {outcome:18s} {elapsed:7.2f}s"
              f"{'  (cache hit)' if is_cache_hit else ''}")

    total = len(corpus)
    print()
    print("=== SLM-001 §16.4 benchmark report ===")
    print(f"Corpus size: {total}")
    print()
    print(f"{'Metric':30s} {'Value':>10s}")
    print(f"{'Deterministic-fix rate':30s} "
          f"{100 * outcomes['deterministic_fix'] / total:9.1f}%")
    validated = outcomes["deterministic_fix"] + outcomes["model_fix"]
    print(f"{'Sandbox-validation rate':30s} {100 * validated / total:9.1f}%")
    print(f"{'Semantic-correctness rate':30s} {'N/A':>10s}  (not computed -- see this "
          f"script's own doc comment)")
    print(f"{'No-suggestion rate':30s} "
          f"{100 * outcomes['no_suggestion'] / total:9.1f}%")
    print(f"{'False-confidence rate':30s} {'N/A':>10s}  (not computed -- needs an "
          f"oracle, see doc comment)")
    print(f"{'p50 latency':30s} {percentile(latencies, 0.50):9.2f}s")
    print(f"{'p95 latency':30s} {percentile(latencies, 0.95):9.2f}s")
    if model_path_attempts:
        print(f"{'Cache hit rate (model-path)':30s} "
              f"{100 * cache_hits / model_path_attempts:9.1f}%  "
              f"({cache_hits}/{model_path_attempts})")
    else:
        print(f"{'Cache hit rate (model-path)':30s} {'N/A':>10s}  "
              f"(every case resolved deterministically)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
