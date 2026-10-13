#!/usr/bin/env python3
"""run_benchmark.py -- SLM-001 §16.4's measurable success criteria, computed
against the fixed corpus mutate.py builds (scripts/slm/slm_benchmark/corpus.json).

For each corpus case, compiles the mutated source WITH --smart and classifies
the outcome from the compiler's own stdout, then reports the metrics table
§16.4 defines:

    Deterministic-fix rate       | % fixed with no model invocation
    Sandbox-validation rate      | % of model suggestions that compile and
                                    clear the original error
    Semantic-correctness rate    | of validated fixes, % where the mutated
                                    test's own asserts still pass
    No-suggestion rate           | % where nothing is shown
    False-confidence rate        | % of *displayed* suggestions that were
                                    actually wrong
    p50 / p95 latency            | wall clock per diagnostic
    Cache hit rate               | among model-path attempts only

How semantic-correctness/false-confidence are measured (the SLM-001 follow-up
this script originally shipped without -- see docs/development/changes/
SLM-001.md's own history): every corpus case is a MECHANICALLY MUTATED,
otherwise-passing test that calls test::assert(...) to check its own
behavior. `--smart-write-fix=<path>` (a small compiler addition made
specifically for this) writes the FULL patched source -- not just a
description of the fix -- to a real file once a suggestion clears the
display gate. This script then does what `--smart` itself deliberately never
does on its own: takes that written file, compiles it FOR REAL (through
Codegen and the linker, not just the Sandbox's Lex/Parse/Resolve/TypeCheck/
Lint-only recheck), runs the resulting binary, and treats its exit code as
the oracle -- 0 means every test::assert() in the ORIGINAL program still
held after the fix (semantically correct, not just compilable); nonzero
means the fix compiled and cleared the diagnosed error but broke what the
program actually does (semantically wrong despite validating) or didn't even
survive Codegen/linking (a distinct, more surprising failure -- the sandbox's
own recheck is semantic-only and has never covered Codegen at all).

Usage:
    python3 scripts/slm/slm_benchmark/run_benchmark.py

Expect this to take a while: every non-deterministic-fix case loads the
~1GB GGUF model and runs real inference (confirmed live: tens of seconds per
case on the machine this was built on), same cost `scripts/slm/test_ai.sh`
already has for the same reason. Deterministic-fix cases are fast (no model
load at all -- see slm_deterministic_fix_smoke.sh's own timing-ceiling
assertion for that same property, checked mechanically there).
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
TURF = REPO_ROOT / "build" / "turfc"
CORPUS_PATH = Path(__file__).resolve().parent / "corpus.json"
SCRATCH = Path("/tmp/turf_slm_benchmark_run.tr")
FIXED_OUT = Path("/tmp/turf_slm_benchmark_fixed.tr")
FIXED_BIN = Path("/tmp/turf_slm_benchmark_fixed_bin")


def run_one(mutated_source: str) -> tuple[str, float]:
    SCRATCH.write_text(mutated_source)
    FIXED_OUT.unlink(missing_ok=True)
    env = dict(os.environ)
    env["TURF_SLM_DEBUG"] = "1"  # needed to observe "[SLM] Cache hit" lines
    start = time.monotonic()
    proc = subprocess.run(
        [str(TURF), "--smart", f"--smart-write-fix={FIXED_OUT}", str(SCRATCH)],
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


def check_semantic_outcome() -> str:
    """Compiles and runs FIXED_OUT (already written by run_one() for a
    validated fix). Returns "semantic_correct" (compiled, ran, exit 0 --
    every test::assert() in the original program still holds),
    "semantic_incorrect" (compiled and ran, but a nonzero exit -- an assert
    the pre-mutation original satisfied now fails), or "recompile_failed"
    (didn't even survive a real compile+link -- the Sandbox's own recheck
    never covers Codegen, only Lex/Parse/Resolve/TypeCheck/Lint, so this is
    a genuinely distinct, real gap the semantic check surfaces)."""
    if not FIXED_OUT.exists() or not FIXED_OUT.read_text().strip():
        return "recompile_failed"
    compile_proc = subprocess.run(
        [str(TURF), str(FIXED_OUT), "-o", str(FIXED_BIN)],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    if compile_proc.returncode != 0 or not FIXED_BIN.exists():
        return "recompile_failed"
    run_proc = subprocess.run([str(FIXED_BIN)], capture_output=True, text=True)
    FIXED_BIN.unlink(missing_ok=True)
    return "semantic_correct" if run_proc.returncode == 0 else "semantic_incorrect"


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
    semantic_outcomes: dict[str, int] = {
        "semantic_correct": 0, "semantic_incorrect": 0, "recompile_failed": 0,
    }
    latencies: list[float] = []
    cache_hits = 0
    model_path_attempts = 0

    for case in corpus:
        output, elapsed = run_one(case["mutated_source"])
        outcome = classify(output)
        outcomes[outcome] += 1
        latencies.append(elapsed)

        semantic = None
        if outcome in ("deterministic_fix", "model_fix"):
            semantic = check_semantic_outcome()
            semantic_outcomes[semantic] += 1

        is_cache_hit = "[SLM] Cache hit" in output
        if outcome != "deterministic_fix":
            model_path_attempts += 1
            if is_cache_hit:
                cache_hits += 1

        print(f"{case['id']:32s} {outcome:18s} {elapsed:7.2f}s"
              f"{'  (cache hit)' if is_cache_hit else ''}"
              f"{'  [' + semantic + ']' if semantic else ''}")

    total = len(corpus)
    validated = outcomes["deterministic_fix"] + outcomes["model_fix"]
    print()
    print("=== SLM-001 §16.4 benchmark report ===")
    print(f"Corpus size: {total}")
    print()
    print(f"{'Metric':30s} {'Value':>10s}")
    print(f"{'Deterministic-fix rate':30s} "
          f"{100 * outcomes['deterministic_fix'] / total:9.1f}%")
    print(f"{'Sandbox-validation rate':30s} {100 * validated / total:9.1f}%")
    if validated:
        semrate = 100 * semantic_outcomes["semantic_correct"] / validated
        falserate = 100 * (semantic_outcomes["semantic_incorrect"]
                            + semantic_outcomes["recompile_failed"]) / validated
        print(f"{'Semantic-correctness rate':30s} {semrate:9.1f}%  "
              f"({semantic_outcomes['semantic_correct']}/{validated} validated fixes)")
    else:
        print(f"{'Semantic-correctness rate':30s} {'N/A':>10s}  (no fix was validated)")
        falserate = None
    print(f"{'No-suggestion rate':30s} "
          f"{100 * outcomes['no_suggestion'] / total:9.1f}%")
    if falserate is not None:
        print(f"{'False-confidence rate':30s} {falserate:9.1f}%  "
              f"({semantic_outcomes['semantic_incorrect']} wrong + "
              f"{semantic_outcomes['recompile_failed']} didn't even recompile, "
              f"of {validated} displayed)")
    else:
        print(f"{'False-confidence rate':30s} {'N/A':>10s}  (no fix was validated)")
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
