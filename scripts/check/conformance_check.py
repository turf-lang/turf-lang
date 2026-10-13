#!/usr/bin/env python3
"""conformance_check.py -- REL-004 stage 1: keep tests/CONFORMANCE.md honest.

The map is a table, one row per Part VII area, whose Positive and Negative
cells list repository paths in backticks (a file, a directory, or a glob).
This fails when:

  * a path matches nothing (a test was renamed or deleted under the map),
  * an area's Positive or Negative cell is empty or has no usable evidence,
  * a Negative cell names a `.tr` file that is not a `fail_*` test (a positive
    test filed as a negative one proves nothing),
  * a Positive cell names only `fail_*` tests,
  * an area appears twice.

Second check (tests/CONFORMANCE_ITEMS.md): every frozen decision in
docs/development/FREEZE.md's appendix (`F-GEN-1` ... `F-FMT-3`) has exactly one
row, and the row's evidence matches what the decision needs:

  PN  a passing test or script AND a fail_* test or script
  P   a passing test or script            N  a fail_* test or script
  S   a script (a rule only a script can observe: exit statuses, archive layout)
  -   no evidence, and a written reason (a process rule, not program behaviour)

A script named as evidence must be run by a workflow under .github/workflows/,
or the evidence would never be exercised. A new frozen decision with no row, a
row with no such decision, and a path that matches nothing all fail the check.

"Usable evidence" is a `.tr` test (negative: a `fail_*` one) or a script under
scripts/. It cannot judge whether the evidence is ENOUGH; that stays a
reviewer's call, and tests/CONFORMANCE.md has a section to write holes down in.

Usage: scripts/check/conformance_check.py    (from anywhere; paths are repo-relative)
Exit:  0 = consistent; 1 = at least one problem (each is listed).
"""
from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAP = REPO / "tests" / "CONFORMANCE.md"


def expand(path: str) -> list[Path]:
    """Files a cell entry stands for: a directory is every file below it."""
    out: list[Path] = []
    for hit in sorted(glob.glob(str(REPO / path), recursive=True)):
        p = Path(hit)
        if p.is_dir():
            out += sorted(f for f in p.rglob("*") if f.is_file())
        else:
            out.append(p)
    return out


def classify(files: list[Path], negative: bool) -> tuple[int, list[str]]:
    """(evidence count, complaints) for one cell's already-expanded files."""
    evidence, complaints = 0, []
    for f in files:
        rel = f.relative_to(REPO).as_posix()
        if f.suffix == ".tr":
            if f.name.endswith("_module.tr"):
                continue  # a companion, not a test
            is_fail = f.name.startswith("fail_")
            if negative and not is_fail:
                continue  # a directory entry also holds positives; only fail_* counts
            if not negative and is_fail:
                continue
            evidence += 1
        elif f.suffix in (".sh", ".ps1") and rel.startswith("scripts/"):
            evidence += 1
    return evidence, complaints


ITEMS = REPO / "tests" / "CONFORMANCE_ITEMS.md"
FREEZE = REPO / "docs" / "development" / "FREEZE.md"
WORKFLOWS = REPO / ".github" / "workflows"
FID = re.compile(r"F-[A-Z]+-\d+")


def frozen_ids() -> list[str]:
    """The decision IDs from FREEZE.md's appendix table, in order."""
    text = FREEZE.read_text()
    appendix = text[text.index("## Appendix"):]
    ids = []
    for line in appendix.splitlines():
        m = re.match(r"\|\s*\*\*(F-[A-Z]+-\d+)\*\*\s*\|", line)
        if m:
            ids.append(m[1])
    return ids


def workflow_text() -> str:
    return "\n".join(f.read_text() for f in sorted(WORKFLOWS.glob("*.yml")))


def check_items(problems: list[str]) -> int:
    """Returns the number of rows read."""
    if not ITEMS.exists():
        problems.append(f"{ITEMS.relative_to(REPO)} is missing")
        return 0
    wanted = frozen_ids()
    if not wanted:
        problems.append("could not read any F-* decision from FREEZE.md's appendix")
    wf = workflow_text()
    seen: dict[str, int] = {}
    for n, line in enumerate(ITEMS.read_text().splitlines(), 1):
        m = re.match(r"\|\s*\*\*(F-[A-Z]+-\d+)\*\*\s*\|", line)
        if not m:
            continue
        fid = m[1]
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 5:
            problems.append(f"{fid} (line {n}): expected 5 columns, found {len(cells)}")
            continue
        _, needs, pos, neg, note = cells
        needs = needs.strip("`")
        if fid in seen:
            problems.append(f"{fid}: appears twice (lines {seen[fid]} and {n})")
        seen[fid] = n
        if needs not in ("PN", "P", "N", "S", "-"):
            problems.append(f"{fid}: Needs must be PN, P, N, S or -, not '{needs}'")
            continue
        if needs == "-":
            if len(note) < 20:
                problems.append(f"{fid}: Needs '-' requires a written reason in Note")
            continue
        if not note:
            problems.append(f"{fid}: Note is empty (say what the evidence shows)")

        def gather(cell: str, label: str) -> list[Path]:
            files: list[Path] = []
            for p in re.findall(r"`([^`]+)`", cell):
                hit = expand(p)
                if not hit:
                    problems.append(f"{fid}: {label} path '{p}' matches nothing")
                files += hit
                # A named test file must be filed on the side it belongs to;
                # a directory or glob legitimately holds both kinds.
                if p.endswith(".tr") and "*" not in p and (REPO / p).is_file():
                    is_fail = Path(p).name.startswith("fail_")
                    if label == "Negative" and not is_fail:
                        problems.append(f"{fid}: Negative path '{p}' is not a fail_* test")
                    if label == "Positive" and is_fail:
                        problems.append(f"{fid}: Positive path '{p}' is a fail_* test")
                for f in hit:
                    rel = f.relative_to(REPO).as_posix()
                    if rel.startswith("scripts/") and f.suffix in (".sh", ".ps1", ".py") \
                            and f.name not in wf and f.name != "conformance_check.py":
                        problems.append(f"{fid}: script '{rel}' is not run by any workflow, so it is not evidence")
            return files

        pfiles, nfiles = gather(pos, "Positive"), gather(neg, "Negative")
        has_script = any(f.relative_to(REPO).as_posix().startswith("scripts/") for f in pfiles + nfiles)
        p_count, _ = classify(pfiles, negative=False)
        n_count, _ = classify(nfiles, negative=True)
        if needs == "PN" and (p_count == 0 or n_count == 0):
            problems.append(f"{fid}: needs PN but has {p_count} positive and {n_count} negative evidence")
        elif needs == "P" and p_count == 0:
            problems.append(f"{fid}: needs P but has no positive evidence")
        elif needs == "N" and n_count == 0:
            problems.append(f"{fid}: needs N but has no negative evidence")
        elif needs == "S" and not has_script:
            problems.append(f"{fid}: needs S but names no script")

    for fid in wanted:
        if fid not in seen:
            problems.append(f"{fid}: frozen in FREEZE.md but has no row in {ITEMS.name}")
    for fid, n in seen.items():
        if fid not in wanted:
            problems.append(f"{fid} (line {n}): no such decision in FREEZE.md's appendix")
    return len(seen)


def main() -> int:
    if not MAP.exists():
        print(f"conformance_check: {MAP.relative_to(REPO)} is missing", file=sys.stderr)
        return 1
    problems: list[str] = []
    areas: dict[str, int] = {}
    in_table = False
    for n, line in enumerate(MAP.read_text().splitlines(), 1):
        if line.startswith("| Area |"):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            if areas:
                break  # the first table after the header is the map
            continue
        if re.match(r"^\|[-| ]+\|$", line):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3:
            problems.append(f"line {n}: expected 3 columns, found {len(cells)}")
            continue
        area, pos, neg = cells
        if area in areas:
            problems.append(f"line {n}: area '{area}' appears twice (first on line {areas[area]})")
        areas[area] = n

        for label, cell in (("Positive", pos), ("Negative", neg)):
            paths = re.findall(r"`([^`]+)`", cell)
            if not paths:
                problems.append(f"{area}: the {label} cell lists no paths")
                continue
            files: list[Path] = []
            for p in paths:
                hit = expand(p)
                if not hit:
                    problems.append(f"{area}: {label} path '{p}' matches nothing")
                files += hit
                if label == "Negative":
                    for f in hit:
                        if f.suffix == ".tr" and not f.name.startswith("fail_") and "*" not in p \
                                and not p.endswith("/") and f.is_file() and Path(REPO / p).is_file():
                            problems.append(f"{area}: Negative path '{p}' is not a fail_* test")
            count, _ = classify(files, negative=(label == "Negative"))
            if count == 0:
                what = "a fail_* test or a script" if label == "Negative" else "a passing test or a script"
                problems.append(f"{area}: the {label} cell has no evidence ({what})")

    if not areas:
        problems.append("tests/CONFORMANCE.md has no area table (a row starting '| Area |' is expected)")

    n_items = check_items(problems)

    if problems:
        print("conformance_check: FAIL")
        for p in problems:
            print("  " + p)
        return 1
    print(f"conformance_check: OK ({len(areas)} areas with positive and negative evidence; "
          f"{n_items} frozen decisions, each with its evidence)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
