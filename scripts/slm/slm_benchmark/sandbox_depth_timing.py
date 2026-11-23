#!/usr/bin/env python3
"""What the --smart sandbox's depth costs per candidate (#363).

For every single-mutation case of the train split, the right fix (the line
the mutation replaced) is replayed as the model's answer (TURF_SLM_REPLAY), so
no model runs, and the case is compiled with --smart once per sandbox depth
(TURF_SLM_SANDBOX_DEPTH=frontend, analysis, codegen). TURF_SLM_DEBUG prints each
candidate's time; this collects them. A case the compiler's own Fix: covers
never reaches the sandbox and has no time.

Also reported: right fixes a deeper depth turns down that the one before it
accepts (they would be fixes lost to the deeper check).

    python3 scripts/slm/slm_benchmark/sandbox_depth_timing.py --jobs 2

Writes bench/results/smart/<date>-<commit>-sandbox-depth.json and prints a
summary table.
"""

import argparse
import concurrent.futures as cf
import datetime
import json
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_corpus as bc  # noqa: E402
import run_benchmark as rb  # noqa: E402

DEPTHS = {"frontend": "front end", "analysis": "analysis", "codegen": "codegen"}
CANDIDATE = re.compile(r"candidate \[\d+, \d+\] \([^)]*\): (validated|rejected) \(depth ([a-z ]+), ([0-9.]+) ms\)")


def run(case, depth, work: Path):
    d = Path(tempfile.mkdtemp(prefix="c", dir=work))
    try:
        args, _ = rb.setup(case, case["mutated_source"], d)
        answer = {"reasoning": "replayed", "fix_mode": "MODIFY",
                  "original_code": case["mutated_line"], "new_code": case["original_line"],
                  "category": "GENERAL"}
        replay = d / "answer.responses"
        replay.write_text(json.dumps(answer) + "\n")
        env = bc.clean_env()
        env.update(TURF_SLM_DEBUG="1", TURF_SLM_REPLAY=str(replay), TURF_SLM_SANDBOX_DEPTH=depth)
        try:
            p = subprocess.run([str(rb.TURFC), "--smart", *args, "-O0", "-o", str(d / "out.bin")], cwd=d, env=env,
                               capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL, errors="replace")
            err = p.stderr
        except subprocess.TimeoutExpired:
            err = ""
        cands = [(m.group(1), float(m.group(3))) for m in CANDIDATE.finditer(err)]
        return dict(id=case["id"], depth=depth, candidates=cands, validated="Suggestion validated" in err)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))] if xs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--limit", type=int, default=0, help="first N cases only (0 = all)")
    a = ap.parse_args()

    cases = [json.loads(line) for line in (rb.CORPUS / "train.jsonl").read_text().splitlines() if line.strip()]
    cases = [c for c in cases if c.get("original_line") is not None and c.get("mutated_line") is not None
             and not c["kind"].startswith("multi:")]
    if a.limit:
        cases = cases[: a.limit]
    work = Path(tempfile.mkdtemp(prefix="sandbox_depth_"))
    bc.PACKAGES = bc.example_packages()
    bc.WORK = work
    rows = []
    try:
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = [ex.submit(run, c, depth, work) for depth in DEPTHS for c in cases]
            for f in cf.as_completed(futs):
                rows.append(f.result())
    finally:
        shutil.rmtree(work, ignore_errors=True)

    summary = {}
    for depth in DEPTHS:
        times = [ms for r in rows if r["depth"] == depth for _, ms in r["candidates"]]
        summary[depth] = dict(cases_sandboxed=sum(1 for r in rows if r["depth"] == depth and r["candidates"]),
                              candidates=len(times), validated=sum(1 for r in rows if r["depth"] == depth and r["validated"]),
                              mean_ms=round(statistics.mean(times), 1) if times else None,
                              p50_ms=pct(times, 50), p95_ms=pct(times, 95))
    by_id = {}
    for r in rows:
        by_id.setdefault(r["id"], {})[r["depth"]] = r["validated"]
    lost = sorted(i for i, v in by_id.items() if v.get("frontend") and not v.get("analysis"))
    lost_cg = sorted(i for i, v in by_id.items() if v.get("analysis") and not v.get("codegen"))
    gained = sorted(i for i, v in by_id.items() if v.get("analysis") and not v.get("frontend"))

    record = dict(date=datetime.date.today().isoformat(), commit=rb.git_commit(), split="train",
                  cases=len(cases), summary=summary, lost_at_analysis=lost, lost_at_codegen=lost_cg, gained_at_analysis=gained)
    out = rb.RESULTS / f"{record['date']}-{record['commit']}-sandbox-depth.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(record, indent=1) + "\n")

    print(f"{len(cases)} single-mutation train cases, right fix replayed\n")
    print("| depth | cases sandboxed | candidates | validated | mean ms | p50 ms | p95 ms |")
    print("|---|---|---|---|---|---|---|")
    for depth, s in summary.items():
        print(f"| {DEPTHS[depth]} | {s['cases_sandboxed']} | {s['candidates']} | {s['validated']} | "
              f"{s['mean_ms']} | {s['p50_ms']} | {s['p95_ms']} |")
    print(f"\nright fixes the analysis depth turns down: {len(lost)} {lost[:10]}")
    print(f"right fixes the codegen depth turns down: {len(lost_cg)} {lost_cg[:10]}")
    print(f"written: {out.relative_to(rb.REPO)}")


if __name__ == "__main__":
    main()
