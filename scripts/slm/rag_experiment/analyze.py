#!/usr/bin/env python3
"""Summarise results/<run>.jsonl: per-arm rates with Wilson 95% intervals,
paired exact McNemar tests on `correct`, and `correct` by kind. Only cases
that have a result for every arm present are used, so arms are compared on
the same cases."""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARMS = ["A0_none", "A1_category", "A1p_production", "A2_bm25_docs", "A3_examples",
        "A4_examples_docs",
        "A5_random_examples", "A6_production_examples"]
# `exact_text`: new_code is the original line, wherever the model put it.
# `exact`: the same text on the mutated line. The gap between them is a
# localisation miss (the right fix on the wrong line).
RATES = ["parsed", "abstained", "right_line", "compiles", "correct", "exact_text", "exact"]
FOREIGN_HINT = ("foreign", "c_", "java", "python", "rust", "js", "go_", "cpp")


def wilson(k: int, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return 0.0, 0.0
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


def mcnemar_exact(b: int, c: int) -> float:
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    p = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def pct(k, n):
    lo, hi = wilson(k, n)
    return f"{100 * k / n:.1f}% [{100 * lo:.1f}, {100 * hi:.1f}]" if n else "n/a"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_name")
    ap.add_argument("--results-dir", default=str(HERE / "results"))
    ap.add_argument("--corpus-dir", default=str(HERE / "corpus"))
    a = ap.parse_args()
    rdir = Path(a.results_dir)
    rows = [json.loads(x) for x in (rdir / f"{a.run_name}.jsonl").read_text().splitlines() if x.strip()]
    arms = [x for x in ARMS if any(r["arm"] == x for r in rows)]
    # text-matching runs (--patch-format text) carry extra fields
    rates = list(RATES)
    if any("located" in r for r in rows):
        rates[rates.index("right_line"):rates.index("right_line")] = ["located", "unchanged"]
        rates.append("correct_top")
    for r in rows:
        for k in rates:
            r.setdefault(k, False)
    by: dict[str, dict[str, dict]] = {}
    for r in rows:
        r["exact_text"] = r["exact"]
        r["exact"] = r["exact"] and r["right_line"]
        by.setdefault(r["id"], {})[r["arm"]] = r
    ids = sorted(i for i, d in by.items() if all(x in d for x in arms))
    n = len(ids)
    out = [f"# RAG experiment: {a.run_name}", ""]
    mp = rdir / f"{a.run_name}.meta.json"
    if mp.exists():
        m = json.loads(mp.read_text())
        out += [f"Platform: {m.get('platform')}  |  model: {m.get('model')} (sha256 {str(m.get('model_sha256'))[:12]})"
                f"  |  llama-server: {m.get('llama_server_version')}", ""]
    out += [f"Cases with every arm complete: {n} (of {len(by)} started). Wilson 95% intervals in brackets.", ""]
    out += ["## Per arm", "",
            "| arm | n | " + " | ".join(rates) + " | mean latency s | mean prompt tokens | mean context tokens |",
            "|---|---|" + "---|" * (len(rates) + 3)]
    for arm in arms:
        rs = [by[i][arm] for i in ids]
        cells = [pct(sum(1 for r in rs if r[k]), n) for k in rates]
        lat = sum(r["latency_s"] for r in rs) / max(n, 1)
        pt = sum(r["prompt_tokens"] for r in rs) / max(n, 1)
        ct = sum(r.get("ctx_tokens", 0) for r in rs) / max(n, 1)
        out.append(f"| {arm} | {n} | " + " | ".join(cells) + f" | {lat:.1f} | {pt:.0f} | {ct:.0f} |")
    out += ["", "## Paired comparison of `correct` (exact McNemar)", "",
            "b = arm correct and baseline wrong, c = baseline correct and arm wrong.", "",
            "| arm | vs | arm correct | base correct | b | c | p |", "|---|---|---|---|---|---|---|"]
    for base in ("A1p_production", "A1_category", "A0_none"):
        if base not in arms:
            continue
        for arm in arms:
            if arm == base:
                continue
            b = sum(1 for i in ids if by[i][arm]["correct"] and not by[i][base]["correct"])
            c = sum(1 for i in ids if by[i][base]["correct"] and not by[i][arm]["correct"])
            ka = sum(1 for i in ids if by[i][arm]["correct"])
            kb = sum(1 for i in ids if by[i][base]["correct"])
            out.append(f"| {arm} | {base} | {ka} | {kb} | {b} | {c} | {mcnemar_exact(b, c):.4f} |")
    # A test case whose (broken line, fixed line) pair also occurs in the train
    # bank can be answered by copying a retrieved example; "novel" cases cannot.
    cdir = Path(a.corpus_dir)
    train = [json.loads(x) for x in (cdir / "train.jsonl").read_text().splitlines() if x.strip()]
    test = {e["id"]: e for e in (json.loads(x) for x in (cdir / "test.jsonl").read_text().splitlines() if x.strip())}
    pairs = {(e["mutated_line"].strip(), e["original_line"].strip()) for e in train}
    seen = lambda i: i in test and (test[i]["mutated_line"].strip(), test[i]["original_line"].strip()) in pairs
    family = lambda i: "foreign" if (by[i][arms[0]].get("kind") or "").startswith("foreign") else "classic"
    subsets = [("foreign syntax", lambda i: family(i) == "foreign"),
               ("classic", lambda i: family(i) == "classic"),
               ("pair seen in train", seen),
               ("novel pair", lambda i: not seen(i))]
    out += ["", "## `correct` by subset", "",
            "| subset | n | " + " | ".join(arms) + " |", "|---|---|" + "---|" * len(arms)]
    for name, f in subsets:
        si = [i for i in ids if f(i)]
        out.append(f"| {name} | {len(si)} | " + " | ".join(
            f"{sum(1 for i in si if by[i][x]['correct'])}/{len(si)}" for x in arms) + " |")
    kinds = sorted({by[i][arms[0]].get("kind") or "?" for i in ids}) if arms else []
    out += ["", "## `correct` by kind", "",
            "| kind | n | " + " | ".join(arms) + " |", "|---|---|" + "---|" * len(arms)]
    for k in kinds:
        ki = [i for i in ids if (by[i][arms[0]].get("kind") or "?") == k]
        tag = " (foreign syntax)" if any(h in k for h in FOREIGN_HINT) else ""
        out.append(f"| {k}{tag} | {len(ki)} | " + " | ".join(
            f"{sum(1 for i in ki if by[i][x]['correct'])}/{len(ki)}" for x in arms) + " |")
    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    (rdir / f"{a.run_name}.md").write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
