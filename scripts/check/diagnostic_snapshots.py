#!/usr/bin/env python3
"""DX-10: a snapshot of every diagnostic code.

Every code in the V-5 registry (src/diagnostics/ErrorCodes.h), retired codes
and E121 (an internal compiler error) excepted, has a snapshot under
tests/snapshots/diagnostics/:

  INDEX.txt     one line per code: `Exxx <program> [flag ...]`, the program
                (a path under tests/) whose compile reports the code, and the
                flags it is compiled with (`--warn Exxx` for a warning that is
                `allow` by default, `--package-graph <json>` for a package rule)
  programs/     the programs no ordinary test is a good snapshot of (a package
                graph); the test runner leaves this directory out
  Exxx.txt      the full rendered text of that compile, colour stripped
  Exxx.json     the same compile with --diagnostics=json, one object a line

The check compiles each INDEX program both ways and fails when

  - a registered, non-retired code has no INDEX line (or no snapshot file),
  - an INDEX line names a retired or unknown code, or a program missing,
  - the compile no longer reports the code it is the snapshot of,
  - the text or the JSON differs from the snapshot (a stale snapshot).

So a reworded message, a moved span or a changed hint shows here as a diff,
and is accepted on purpose with `--write`, which regenerates every snapshot
from the INDEX. `--write --pick` also fills in an INDEX line for a code that
has none, choosing the shortest-output tests/**/*.tr that reports it (warnings
are looked for with every lint at `warn`).

The programs are compiled from the repository root with `--check`, so paths
in the output read `tests/...` everywhere, and the root is cut from an absolute
path (a standard-library location reads `lib/std/...`); the `.tr` files stay where they
are and keep their own `EXPECT-*` markers.

Usage: scripts/check/diagnostic_snapshots.py [--compiler build/turfc]
                                            [--jobs N] [--write [--pick]]
                                            [--only Exxx ...]
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "src/diagnostics/ErrorCodes.h"
SNAPDIR = ROOT / "tests/snapshots/diagnostics"
INDEX = SNAPDIR / "INDEX.txt"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEADER_CODE = re.compile(r"^── (?:error|warning)\b[^\[\n]*\[(E\d{3})", re.M)
# Codes no correct compile produces, so no program can be their snapshot:
# E121 is an internal compiler error, which a test exists to make disappear.
EXEMPT = {"E121"}
EXCLUDE = ("tests/compat/", "tests/custom/", "tests/99_known_bugs/",
           "tests/snapshots/")


def registry():
    """{code: (class name, retired?)} from the registry rows."""
    text = REGISTRY.read_text()
    consts = dict(re.findall(r'inline constexpr const char \*(\w+) = "(E\d{3})"', text))
    rows = {}
    for first, cls, desc in re.findall(
            r'^\s*\{(ErrCode::\w+|"E\d{3}"), "(\w+)", "([^"]*)"', text, re.M):
        code = consts[first[len("ErrCode::"):]] if first.startswith("ErrCode::") \
            else first.strip('"')
        rows[code] = (cls, "(retired" in desc)
    return rows


def is_warning(cls):
    return cls.endswith("Warning")


def read_index():
    entries = {}
    if INDEX.exists():
        for n, line in enumerate(INDEX.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            entries[parts[0]] = (parts[1], parts[2:], n)
    return entries


def write_index(entries):
    lines = ["# DX-10: the program each diagnostic code's snapshot is taken from.",
             "# `Exxx <program under tests/> [flags]`; regenerate the snapshots with",
             "# scripts/check/diagnostic_snapshots.py --write (--pick fills in new codes).",
             ""]
    for code in sorted(entries):
        prog, flags, _ = entries[code]
        lines.append(" ".join([code, prog] + flags))
    INDEX.write_text("\n".join(lines) + "\n")


def compile_once(compiler, prog, flags, json_mode):
    cmd = [compiler, prog, "--check"] + flags
    if json_mode:
        cmd.append("--diagnostics=json")
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    env["NO_COLOR"] = "1"
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           timeout=120, env=env)
    except subprocess.TimeoutExpired:
        return None, "timed out"
    out = ANSI.sub("", p.stdout + p.stderr)
    # A related location in the standard library is an absolute path (an editor
    # opens it as given); the checkout's own root is cut from it so a snapshot
    # reads `lib/std/...` on every machine.
    out = out.replace(str(ROOT) + "/", "")
    return p.returncode, out


def snapshot(compiler, prog, flags):
    """(text, json, error) for one program."""
    rc, text = compile_once(compiler, prog, flags, False)
    if rc is None:
        return None, None, text
    rc2, js = compile_once(compiler, prog, flags, True)
    if rc2 is None:
        return None, None, js
    js = "\n".join(l for l in js.splitlines() if l.startswith("{")) + "\n"
    return text, js, None


def candidates():
    for p in sorted(ROOT.glob("tests/**/*.tr")):
        rel = p.relative_to(ROOT).as_posix()
        if rel.startswith(EXCLUDE) or p.name.endswith("_module.tr"):
            continue
        yield rel


def pick(compiler, missing, rows, jobs):
    """{code: (program, flags)} for each missing code a test reports."""
    warn_flags = []
    for code in missing:
        if is_warning(rows[code][0]):
            warn_flags += ["--warn", code]
    found = {}

    def scan(rel):
        rc, out = compile_once(compiler, rel, warn_flags, False)
        if rc is None:
            return rel, set(), 0
        return rel, set(HEADER_CODE.findall(out)) & set(missing), len(out)

    best = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for rel, codes, size in ex.map(scan, candidates()):
            for code in codes:
                if code not in best or size < best[code][1]:
                    best[code] = (rel, size)
    for code, (rel, _) in best.items():
        flags = ["--warn", code] if is_warning(rows[code][0]) else []
        found[code] = (rel, flags)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(ROOT / "build/turfc"))
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("TURF_TEST_JOBS", "2")))
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--pick", action="store_true")
    ap.add_argument("--only", nargs="*", default=[])
    a = ap.parse_args()

    rows = registry()
    entries = read_index()
    problems = []

    live = {c for c, (_, retired) in rows.items() if not retired} - EXEMPT
    for code, (prog, _, n) in entries.items():
        if code not in rows:
            problems.append(f"INDEX.txt:{n}: {code} is not a registered code")
        elif rows[code][1]:
            problems.append(f"INDEX.txt:{n}: {code} is retired; remove its line")
        elif not (ROOT / prog).exists():
            problems.append(f"INDEX.txt:{n}: {prog} does not exist")
    missing = sorted(live - set(entries))

    if a.write and a.pick and missing:
        SNAPDIR.mkdir(parents=True, exist_ok=True)
        for code, (prog, flags) in pick(a.compiler, missing, rows, a.jobs).items():
            entries[code] = (prog, flags, 0)
            print(f"picked {code}: {prog} {' '.join(flags)}".rstrip())
        write_index(entries)
        missing = sorted(live - set(entries))

    for code in missing:
        problems.append(f"{code} ({rows[code][0]}) has no snapshot: add a "
                        "program that reports it and an INDEX.txt line "
                        "(--write --pick finds one in tests/)")

    todo = [(c, e) for c, e in sorted(entries.items())
            if c in live and (not a.only or c in a.only)]

    def run(item):
        code, (prog, flags, _) = item
        if not (ROOT / prog).exists():
            return code, None
        text, js, err = snapshot(a.compiler, prog, flags)
        if err:
            return code, f"{code}: {prog}: {err}"
        if code not in HEADER_CODE.findall(text):
            return code, (f"{code}: {prog} {' '.join(flags)} no longer reports "
                          f"{code}; pick another program (--write --pick after "
                          "removing its INDEX line)")
        tpath, jpath = SNAPDIR / f"{code}.txt", SNAPDIR / f"{code}.json"
        if a.write:
            tpath.write_text(text)
            jpath.write_text(js)
            return code, None
        for path, got in ((tpath, text), (jpath, js)):
            if not path.exists():
                return code, f"{code}: {path.relative_to(ROOT)} is missing (--write)"
            want = path.read_text()
            if want != got:
                return code, (f"{code}: {path.relative_to(ROOT)} is stale:\n" +
                              diff(want, got))
        return code, None

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for _, problem in ex.map(run, todo):
            if problem:
                problems.append(problem)

    if a.write and not a.only:
        keep = {f"{c}.txt" for c in entries} | {f"{c}.json" for c in entries} | {"INDEX.txt"}
        for f in SNAPDIR.iterdir():
            if f.is_file() and f.name not in keep:
                f.unlink()

    for p in problems:
        print("diagnostic_snapshots: " + p)
    if problems:
        return 1
    print(f"diagnostic_snapshots: OK ({len(todo)} codes, text and JSON)")
    return 0


def diff(want, got):
    import difflib
    return "".join(difflib.unified_diff(want.splitlines(True), got.splitlines(True),
                                         "snapshot", "compiler", n=1))[:4000]


if __name__ == "__main__":
    sys.exit(main())
