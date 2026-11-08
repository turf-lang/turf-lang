#!/usr/bin/env python3
"""run_benchmark.py -- score `--smart` on the SL-3 corpus (SL-2, TDP-2-008).

Every case of one split (corpus/<split>.jsonl, written by build_corpus.py) is
compiled once per path:

  rules   the compiler alone: its checked `Fix:` (DX-1, SL-7), no model;
  model   the same compile with `--smart`: a rule fix where there is one,
          otherwise the model, or a cached answer of the model.

What was shown is read from `--diagnostics=json` (each error's `fix` edits)
and from `--smart-write-fix=<path>` (the model's validated program); stdout
text is never parsed. A shown fix is then judged by the case's oracle:

  asserts    the fixed program compiles, runs, exits 0 and prints what the
             original printed (the original's own asserts still hold);
  recompile  the fixed program compiles;
  abstain    a hard negative: any shown fix is wrong.

Utility per case: +1 a correct shown fix, 0 nothing shown, -K a wrong fix or
one that does not compile (K = 3, D-12). The headline is the mean utility
with a bootstrap 95% interval; with both paths, model - rules is reported
with a paired interval. Also: per-code and per-category utility, reliability
by provenance (rule, model, cache) and the Brier score of the confidence the
compiler printed, the rate of silence on hard negatives, and p50/p95 latency
and peak RSS by provenance. One JSON record per run goes to
bench/results/smart/.

Tuning reads the train split only (README.md); heldout is for a final score.

Usage:
    python3 scripts/slm/slm_benchmark/run_benchmark.py [--split train|heldout]
        [--paths rules,model] [--jobs 2] [--limit N] [--ids a,b] [--no-record]

The model path loads the one .gguf in models/ (TURF_SLM_REPLAY=<file>
replays recorded answers instead, for tests) and runs one case at a time.
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures as cf
import datetime
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_corpus as bc  # noqa: E402  (package graphs, shared with the corpus)

REPO = HERE.parents[2]
TURFC = REPO / "build" / "turfc"
CORPUS = HERE / "corpus"
RESULTS = REPO / "bench" / "results" / "smart"
RUN_TIMEOUT = 10
COMPILE_TIMEOUT = {"rules": 60, "model": 900}
_tls = threading.local()
WORK: Path


def workdir() -> Path:
    if not hasattr(_tls, "d"):
        _tls.d = Path(tempfile.mkdtemp(prefix="w", dir=WORK))
    return _tls.d


def run_measured(cmd, cwd, env, timeout):
    """-> (exit code or None on timeout, stdout, stderr, seconds, peak RSS in MB)."""
    out_f, err_f = cwd / "stdout.txt", cwd / "stderr.txt"
    with open(out_f, "w") as o, open(err_f, "w") as e:
        start = time.monotonic()
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=o, stderr=e, stdin=subprocess.DEVNULL)
        timer = threading.Timer(timeout, p.kill)
        timer.start()
        _, status, ru = os.wait4(p.pid, 0)
        timer.cancel()
        elapsed = time.monotonic() - start
    p.returncode = os.waitstatus_to_exitcode(status)
    code = None if p.returncode < 0 else p.returncode
    return (code, out_f.read_text(errors="replace"), err_f.read_text(errors="replace"),
            elapsed, ru.ru_maxrss / 1024)


def setup(case, text, d: Path):
    """Write `text` as the case's file. -> (turfc arguments naming it, the file)."""
    rel = case["source_test"]
    if case.get("package"):
        graph, root_main, copy = bc.package_tree(rel, text, d / "pkg")
        return ["--package-graph", str(graph), str(root_main)], copy
    f = d / "case.tr"
    f.write_text(text)
    return [str(f)], f


def diagnostics(stderr: str):
    out = []
    for line in stderr.splitlines():
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def apply_edits(chunk: str, first_line: int, edits) -> str:
    """Apply edits (spans 1-based, columns in bytes, end exclusive) to the
    text of lines first_line.., back to front."""
    data = chunk.encode()
    starts = [0] + [i + 1 for i, ch in enumerate(data) if ch == 0x0A]

    def off(pos):
        return starts[pos["line"] - first_line] + pos["column"] - 1

    for e in sorted(edits, key=lambda e: (e["span"]["start"]["line"], e["span"]["start"]["column"]), reverse=True):
        data = data[:off(e["span"]["start"])] + e["replacement"].encode() + data[off(e["span"]["end"]):]
    return data.decode(errors="replace")


def apply_fixes(text: str, diags, target: Path, cwd: Path):
    """Every error's fix whose edits are all in `target`, applied the way
    --smart-write-fix applies them: in order, and a fix touching a line an
    earlier fix took is skipped. The compiler checked each fix on its own.
    -> the fixed text, or None when no fix was shown."""
    lines = text.split("\n")
    fixes = []
    taken = set()
    for d in diags:
        fx = d.get("fix")
        if not fx or not fx.get("edits"):
            continue
        if any((cwd / e["file"]).resolve() != target.resolve() for e in fx["edits"]):
            continue
        lo = min(e["span"]["start"]["line"] for e in fx["edits"])
        hi = max(e["span"]["end"]["line"] for e in fx["edits"])
        if set(range(lo, hi + 1)) & taken:
            continue
        taken |= set(range(lo, hi + 1))
        fixes.append((lo, hi, fx["edits"]))
    if not fixes:
        return None
    for lo, hi, edits in sorted(fixes, reverse=True):  # bottom up: earlier line numbers stay right
        lines[lo - 1:hi] = apply_edits("\n".join(lines[lo - 1:hi]), lo, edits).split("\n")
    return "\n".join(lines)


def judge(case, fixed_text: str):
    """-> 'correct', 'wrong' or 'recompile_failed'."""
    if case["oracle"] == "abstain":
        return "wrong"
    d = workdir() / "judge"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir()
    args, _ = setup(case, fixed_text, d)
    exe = d / "fixed.bin"
    code, _, _, _, _ = run_measured([str(TURFC), *args, "-O0", "-o", str(exe)], d, bc.clean_env(), 120)
    if code != 0 or not exe.exists():
        return "recompile_failed"
    if case["oracle"] == "recompile":
        return "correct"
    code, out, _, _, _ = run_measured([str(exe)], d, bc.clean_env(), RUN_TIMEOUT)
    return "correct" if code == 0 and out == case["expected_stdout"] else "wrong"


CONF = re.compile(r"Final Confidence \(post-sandbox(?:, capped)?\): ([0-9.]+)")


def run_case(case, path, k):
    d = workdir() / "run"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir()
    args, target = setup(case, case["mutated_source"], d)
    written = d / "fixed.tr"
    cmd = [str(TURFC), *args, "--diagnostics=json", f"--smart-write-fix={written}", "-O0", "-o", str(d / "out.bin")]
    env = bc.clean_env()
    if path == "model":
        cmd.insert(1, "--smart")
        env["TURF_SLM_DEBUG"] = "1"
    code, _, err, secs, rss = run_measured(cmd, d, env, COMPILE_TIMEOUT[path])
    diags = diagnostics(err)
    errors = [x for x in diags if x.get("severity") == "error"]
    first = errors[0] if errors else {}
    rule_text = apply_fixes(case["mutated_source"], errors, target, d)
    model_text = written.read_text() if written.exists() and written.read_text().strip() else None
    if model_text is not None and rule_text is not None and model_text.rstrip("\n") == rule_text.rstrip("\n"):
        model_text = None  # the compiler's own fixes, written by --smart-write-fix
    if model_text is not None:
        prov = "cache" if "[SLM] Cache hit" in err else "model"
        shown = model_text
        m = CONF.findall(err)
        conf = float(m[-1]) if m else None
    elif rule_text is not None:
        prov, shown, conf = "rule", rule_text, 1.0
    else:
        prov, shown, conf = "none", None, None
    if code is None:
        outcome = "timeout"
    elif code == 0:
        outcome = "compiled"  # the corpus is stale: this case no longer fails
    elif shown is None:
        outcome = "none"
    else:
        outcome = judge(case, shown)
    utility = {"correct": 1, "none": 0}.get(outcome, -k)
    if outcome in ("timeout", "compiled"):
        utility = 0
    return dict(id=case["id"], path=path, code=case["error_code"], category=first.get("category") or "(none)",
                kind=case["kind"], oracle=case["oracle"], provenance=prov, outcome=outcome, utility=utility,
                confidence=conf, seconds=round(secs, 3), rss_mb=round(rss, 1))


# ----------------------------------------------------------------------------
# statistics
# ----------------------------------------------------------------------------

def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


def bootstrap(xs, boots, rng):
    """Percentile 95% interval of the mean."""
    if not xs:
        return (0.0, 0.0)
    n = len(xs)
    ms = sorted(mean([xs[rng.randrange(n)] for _ in range(n)]) for _ in range(boots))
    return (ms[int(0.025 * boots)], ms[int(0.975 * boots) - 1])


def pct(xs, p):
    if not xs:
        return 0.0
    s = sorted(xs)
    i = (len(s) - 1) * p
    f = int(i)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (i - f)


def summarize(rows, boots, seed):
    rng = random.Random(seed)
    us = [r["utility"] for r in rows]
    shown = [r for r in rows if r["provenance"] != "none" and r["outcome"] in ("correct", "wrong", "recompile_failed")]
    hard = [r for r in rows if r["oracle"] == "abstain"]
    outcomes = collections.Counter(r["outcome"] for r in rows)
    lo, hi = bootstrap(us, boots, rng)
    by_prov = {}
    for p in ("rule", "model", "cache", "none"):
        g = [r for r in rows if r["provenance"] == p]
        if not g:
            continue
        s = [r for r in g if r in shown]
        by_prov[p] = dict(cases=len(g), shown=len(s),
                          correct_rate=round(mean([r["outcome"] == "correct" for r in s]), 4) if s else None,
                          p50_s=round(pct([r["seconds"] for r in g], .5), 3),
                          p95_s=round(pct([r["seconds"] for r in g], .95), 3),
                          peak_rss_mb=max(r["rss_mb"] for r in g))
    scored = [r for r in shown if r["confidence"] is not None]
    brier = mean([(r["confidence"] - (r["outcome"] == "correct")) ** 2 for r in scored]) if scored else None

    def table(key):
        t = collections.defaultdict(list)
        for r in rows:
            t[r[key]].append(r["utility"])
        return {k: dict(cases=len(v), utility=round(mean(v), 3)) for k, v in sorted(t.items())}

    return dict(cases=len(rows), utility=round(mean(us), 4), ci95=[round(lo, 4), round(hi, 4)],
                outcomes=dict(sorted(outcomes.items())), shown=len(shown),
                false_confidence=round(mean([r["outcome"] != "correct" for r in shown]), 4) if shown else None,
                hard_negative_silence=round(mean([r["provenance"] == "none" for r in hard]), 4) if hard else None,
                hard_negatives=len(hard), brier=round(brier, 4) if brier is not None else None,
                brier_cases=len(scored), by_provenance=by_prov, by_code=table("code"),
                by_category=table("category"), by_kind=table("kind"))


def paired_diff(a_rows, b_rows, boots, seed):
    """model - rules over the cases both scored, with a paired bootstrap."""
    a = {r["id"]: r["utility"] for r in a_rows}
    d = [r["utility"] - a[r["id"]] for r in b_rows if r["id"] in a]
    lo, hi = bootstrap(d, boots, random.Random(seed + 1))
    return dict(cases=len(d), mean=round(mean(d), 4), ci95=[round(lo, 4), round(hi, 4)])


# ----------------------------------------------------------------------------
# report
# ----------------------------------------------------------------------------

def report(split, k, summaries, diff):
    print(f"\n=== --smart benchmark: {split}, K = {k} ===\n")
    print(f"{'path':6s} {'cases':>5s} {'utility':>8s} {'95% interval':>18s} {'shown':>6s} "
          f"{'false conf.':>11s} {'silence on HN':>13s} {'Brier':>6s}")
    for path, s in summaries.items():
        fc = "-" if s["false_confidence"] is None else f"{100 * s['false_confidence']:.1f}%"
        hn = "-" if s["hard_negative_silence"] is None else f"{100 * s['hard_negative_silence']:.0f}% of {s['hard_negatives']}"
        br = "-" if s["brier"] is None else f"{s['brier']:.3f}"
        print(f"{path:6s} {s['cases']:5d} {s['utility']:+8.3f} [{s['ci95'][0]:+.3f}, {s['ci95'][1]:+.3f}] "
              f"{s['shown']:6d} {fc:>11s} {hn:>13s} {br:>6s}")
    if diff:
        sep = "the intervals separate" if diff["separated"] else "the intervals overlap: no measured difference"
        print(f"\nmodel - rules: {diff['mean']:+.3f} [{diff['ci95'][0]:+.3f}, {diff['ci95'][1]:+.3f}] "
              f"over {diff['cases']} cases; {sep}.")
    for path, s in summaries.items():
        print(f"\n--- {path}: outcomes {s['outcomes']}")
        print(f"{'provenance':10s} {'cases':>5s} {'shown':>5s} {'correct':>8s} {'p50 s':>7s} {'p95 s':>7s} {'peak MB':>8s}")
        for p, v in s["by_provenance"].items():
            cr = "-" if v["correct_rate"] is None else f"{100 * v['correct_rate']:.1f}%"
            print(f"{p:10s} {v['cases']:5d} {v['shown']:5d} {cr:>8s} {v['p50_s']:7.2f} {v['p95_s']:7.2f} "
                  f"{v['peak_rss_mb']:8.0f}")
        for key, head in (("by_category", "category"), ("by_code", "code")):
            print(f"\n{head:28s} {'cases':>5s} {'utility':>8s}")
            for name, v in sorted(s[key].items(), key=lambda kv: (kv[1]["utility"], kv[0])):
                print(f"{name[:28]:28s} {v['cases']:5d} {v['utility']:+8.3f}")


def git_commit():
    try:
        sha = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=REPO, capture_output=True,
                             text=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=REPO,
                               capture_output=True, text=True).stdout.strip()
        return sha + ("+dirty" if dirty else "")
    except OSError:
        return "unknown"


def model_identity():
    if os.environ.get("TURF_SLM_REPLAY"):
        return {"replay": os.environ["TURF_SLM_REPLAY"]}
    ggufs = sorted((REPO / "models").glob("*.gguf"))
    if len(ggufs) != 1:
        sys.exit(f"the model path needs exactly one .gguf in {REPO / 'models'} (found {len(ggufs)})")
    h = hashlib.sha256()
    with open(ggufs[0], "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return {"file": ggufs[0].name, "sha256": h.hexdigest()}


def main():
    global WORK
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--split", choices=("train", "heldout"), default="train")
    ap.add_argument("--paths", default="rules,model", help="comma list of rules, model")
    ap.add_argument("--k", type=float, default=3.0, help="the cost of a wrong fix (D-12)")
    ap.add_argument("--jobs", type=int, default=2, help="parallel compiles on the rules path (model: 1)")
    ap.add_argument("--boots", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=20260)
    ap.add_argument("--limit", type=int, default=0, help="score only the first N cases")
    ap.add_argument("--ids", default="", help="comma list of case ids")
    ap.add_argument("--cases", default="", help="score this .jsonl instead of the split's (smoke tests)")
    ap.add_argument("--out", default="", help="the record's path (default bench/results/smart/<date>-<commit>-<split>.json)")
    ap.add_argument("--no-record", action="store_true")
    args = ap.parse_args()
    paths = [p for p in args.paths.split(",") if p]
    if not set(paths) <= {"rules", "model"}:
        sys.exit("--paths takes rules, model")
    if not TURFC.exists():
        sys.exit(f"missing {TURFC}: build the compiler first")
    corpus_file = Path(args.cases).resolve() if args.cases else CORPUS / f"{args.split}.jsonl"
    cases = [json.loads(l) for l in corpus_file.read_text().splitlines() if l.strip()]
    if args.ids:
        want = set(args.ids.split(","))
        cases = [c for c in cases if c["id"] in want]
    if args.limit:
        cases = cases[: args.limit]
    model = model_identity() if "model" in paths else None
    bc.PACKAGES = bc.example_packages()
    WORK = Path(tempfile.mkdtemp(prefix="smart_bench_"))
    bc.WORK = WORK
    rows = {}
    try:
        for path in paths:
            jobs = 1 if path == "model" else max(1, args.jobs)
            got = []
            with cf.ThreadPoolExecutor(jobs) as ex:
                for i, r in enumerate(ex.map(lambda c: run_case(c, path, args.k), cases), 1):
                    got.append(r)
                    print(f"[{path}] {i}/{len(cases)} {r['id']:28s} {r['provenance']:6s} {r['outcome']:16s} "
                          f"{r['utility']:+.0f} {r['seconds']:6.2f}s", file=sys.stderr)
            rows[path] = got
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
    summaries = {p: summarize(rows[p], args.boots, args.seed) for p in paths}
    diff = None
    if len(paths) == 2:
        diff = paired_diff(rows["rules"], rows["model"], args.boots, args.seed)
        r, m = summaries["rules"]["ci95"], summaries["model"]["ci95"]
        diff["separated"] = m[0] > r[1] or m[1] < r[0]
    label = f"{corpus_file.name} (--cases)" if args.cases else f"{args.split} split"
    report(label, args.k, summaries, diff)
    if args.no_record:
        return 0
    try:
        version = json.loads(subprocess.run([str(TURFC), "--version", "--json"], capture_output=True,
                                            text=True).stdout)
    except (OSError, json.JSONDecodeError):
        version = None
    record = dict(
        meta=dict(date=datetime.date.today().isoformat(), commit=git_commit(), turfc=version, model=model,
                  split=None if args.cases else args.split, cases_file=args.cases or None, corpus_sha256=hashlib.sha256(corpus_file.read_bytes()).hexdigest(),
                  k=args.k, boots=args.boots, seed=args.seed, paths=paths,
                  subset=bool(args.ids or args.limit)),
        summary=summaries, model_minus_rules=diff, cases=rows)
    out = Path(args.out) if args.out else RESULTS / f"{record['meta']['date']}-{record['meta']['commit']}-{args.split}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(record, indent=1) + "\n")
    print(f"\nrecord: {out.relative_to(REPO) if out.is_relative_to(REPO) else out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
