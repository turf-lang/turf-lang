#!/usr/bin/env python3
"""fit_fix_score.py -- SL-1: fit the Fix Score's one threshold on the train split.

Minimality (src/diagnostics/FixScore.cpp): a fix is "wide" when its token edit
distance is more than kWideRatio times the number of tokens on the error's
line, and at least kWideFloor. The right fix of a train case is known: undo
the mutation (original_line for mutated_line). kWideRatio is the 95th
percentile of distance / error-line tokens over the single-mutation train
cases, so 1 in 20 right fixes would be flagged wide; kWideFloor keeps a short
line from flagging a two-token fix.

Reads corpus/train.jsonl only. Prints the constants to freeze in FixScore.cpp;
rerun after the corpus or the tokenizer changes, and record the result.

Usage: python3 scripts/slm/slm_benchmark/fit_fix_score.py
"""
import json
import math
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The tokenizer of FixScore.cpp: comments dropped, a string or char literal
# is one token, identifiers, numbers, the two-character operators, else one char.
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"?|\'(?:\\.|[^\'\\])*\'?|[A-Za-z_][A-Za-z0-9_]*|[0-9][0-9A-Za-z_.]*'
                   r'|::|->|==|!=|<=|>=|&&|\|\||\+=|-=|\*=|/=|%=|\S')


def tokens(text: str):
    out = []
    for line in text.split("\n"):
        i = line.find("//")
        # a // inside a string literal is not a comment
        while i >= 0 and line[:i].count('"') % 2 == 1:
            i = line.find("//", i + 2)
        out += TOKEN.findall(line if i < 0 else line[:i])
    return out


def distance(a, b):
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def main():
    ratios, dists = [], []
    for line in (HERE / "corpus" / "train.jsonl").read_text().splitlines():
        c = json.loads(line)
        if c["oracle"] != "asserts" or c["kind"].startswith("multi:"):
            continue
        d = distance(tokens(c["mutated_line"]), tokens(c["original_line"]))
        span = max(1, len(tokens(c["mutated_line"])))
        ratios.append(d / span)
        dists.append(d)
    ratios.sort()
    dists.sort()
    p95 = ratios[min(len(ratios) - 1, math.ceil(0.95 * len(ratios)) - 1)]
    floor = dists[min(len(dists) - 1, math.ceil(0.95 * len(dists)) - 1)]
    print(f"cases {len(ratios)}")
    print(f"distance / error-line tokens: median {ratios[len(ratios) // 2]:.3f}, p95 {p95:.3f}, max {ratios[-1]:.3f}")
    print(f"distance: median {dists[len(dists) // 2]}, p95 {floor}, max {dists[-1]}")
    print(f"freeze: kWideRatio = {math.ceil(p95 * 100) / 100:.2f}f; kWideFloor = {floor};")


if __name__ == "__main__":
    main()
