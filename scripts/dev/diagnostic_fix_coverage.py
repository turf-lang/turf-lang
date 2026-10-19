#!/usr/bin/env python3
"""What the compiler's diagnostics print, by error code (TDP-2-001).

Checks every tests/**/fail_*.tr (not tests/99_known_bugs, tests/custom or tests/compat, a frozen copy of the 2.0.0 tests) with
`turfc <file> --check`, run from the file's own directory, and counts, for
each error code the suite exercises, whether its diagnostics carry a Hint: and
a Fix:. This is the table TDP-2-001's Motivation starts from, re-run so the
number of codes that print a fix can be reproduced.

Usage: scripts/dev/diagnostic_fix_coverage.py [--compiler build/turfc]
                                              [--jobs N] [--list] [--fixes]
  --list   one row per code
  --fixes  every printed fix, with its file
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEADER = re.compile(r"^── (error|warning)\b.*?\[(E\d{3})")


def check(compiler, path):
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    try:
        out = subprocess.run(
            [compiler, path.name, "--check"], cwd=path.parent, env=env,
            capture_output=True, text=True, timeout=120)
        text = out.stdout + out.stderr
    except subprocess.TimeoutExpired:
        text = ""
    return path, ANSI.sub("", text)


def diagnostics(text):
    """(code, has_hint, fix_text) for each error in the output."""
    found, cur = [], None
    for line in text.splitlines():
        m = HEADER.match(line)
        if m:
            cur = None
            if m.group(1) == "error":
                cur = [m.group(2), False, None]
                found.append(cur)
            continue
        if cur is None:
            continue
        s = line.strip()
        if s.startswith("Hint:"):
            cur[1] = True
        elif s.startswith("Fix:"):
            cur[2] = s[len("Fix:"):].strip()
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(ROOT / "build" / "turfc"))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--fixes", action="store_true")
    args = ap.parse_args()

    tests = sorted(
        p for p in (ROOT / "tests").rglob("fail_*.tr")
        if p.relative_to(ROOT / "tests").parts[0] not in ("99_known_bugs", "custom", "compat"))

    files = defaultdict(set)
    hinted = defaultdict(int)
    fixed = defaultdict(int)
    seen = defaultdict(int)
    fixes = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for path, text in pool.map(lambda p: check(args.compiler, p), tests):
            for code, hint, fix in diagnostics(text):
                files[code].add(path)
                seen[code] += 1
                hinted[code] += hint
                if fix is not None:
                    fixed[code] += 1
                    fixes.append((path.relative_to(ROOT), code, fix))

    registry = re.findall(r'\{"(E\d{3})"|\{ErrCode::(\w+)',
                          (ROOT / "src/diagnostics/ErrorCodes.h").read_text())
    total = len(registry)

    print(f"fail_*.tr files checked:      {len(tests)}")
    print(f"error codes in the registry:  {total}")
    print(f"codes the suite exercises:    {len(seen)}")
    print(f"  with a Hint: every time     {sum(1 for c in seen if hinted[c] == seen[c])}")
    print(f"  with a Hint: sometimes      {sum(1 for c in seen if 0 < hinted[c] < seen[c])}")
    print(f"  never with a Hint:          {sum(1 for c in seen if hinted[c] == 0)}")
    print(f"  printing a Fix: at least once {sum(1 for c in seen if fixed[c])}")
    print(f"errors reported:              {sum(seen.values())}")
    print(f"  with a Fix:                 {sum(fixed.values())}")

    if args.list:
        print("\ncode  errors  hint  fix  files")
        for code in sorted(seen):
            print(f"{code}  {seen[code]:6}  {hinted[code]:4}  {fixed[code]:3}  {len(files[code])}")
    if args.fixes:
        print()
        for path, code, fix in sorted(fixes):
            print(f"{code}  {path}  Fix: {fix}")


if __name__ == "__main__":
    main()
